/**************************************************************************/
/*  physx_river_3d.cpp                                                    */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "physx_river_3d.h"

#include "core/config/engine.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/templates/hash_set.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/resources/3d/world_3d.h"
#include "scene/resources/mesh.h"
#include "scene/resources/shader.h"
#include "servers/physics_3d/physics_server_3d.h"

namespace {

// The river surface. Per vertex (built by PhysXRiver3D): UV = (arc length,
// distance across) in m, UV2 = flow (local xz, m/s), COLOR.r = water depth
// encoded as (d + 1) / 4, COLOR.g = white water, COLOR.b = fade past the banks.
const char *RIVER_SHADER_SRC = R"(
shader_type spatial;
#if defined(RT)
// The path tracer's copy is a medium boundary: it bends light, tints with
// depth and casts caustics (the same as PhysXWaterSurface3D's).
render_mode cull_disabled;
#else
render_mode blend_mix, cull_disabled, unshaded;
#endif

// Animation clock, set by PhysXRiver3D while the game runs (0 in the editor,
// so the editor doesn't redraw every frame).
uniform float water_time = 0.0;
// Two-phase flow (Vlachos 2010): each copy of the ripples rides the current for
// one flow_period, then restarts while the other one is showing.
uniform float flow_period = 1.5;
uniform vec4 water_color : source_color = vec4(0.10, 0.25, 0.24, 0.72);
uniform vec4 shallow_color : source_color = vec4(0.32, 0.42, 0.34, 0.35);
uniform vec4 foam_color : source_color = vec4(0.92, 0.95, 0.97, 0.95);
// Path tracing only: river water is murkier than the sea (more red and
// green taken out per metre, more fine silt scattering).
uniform vec3 pathtraced_absorption = vec3(0.6, 0.14, 0.1);
uniform vec3 pathtraced_scattering = vec3(0.09, 0.1, 0.09);
uniform float pathtraced_scattering_anisotropy : hint_range(-0.9, 0.95) = 0.85;
uniform float pathtraced_roughness : hint_range(0.0, 1.0) = 0.03;
// Ripples: cells per metre, and their slope in still water and per m/s of current
// (faster water is rougher).
uniform float ripple_scale = 1.0;
uniform float ripple_strength = 0.06;
uniform float ripple_speed_strength = 0.12;
uniform float foam_detail_scale = 1.5;
uniform float foam_breakup : hint_range(0.0, 1.0) = 0.6;
// The surface fades out over this much water depth (m) at the waterline.
uniform float edge_softness : hint_range(0.0, 0.5, 0.005) = 0.06;

varying vec3 v_world;
varying vec2 v_flow;
varying vec4 v_info;
varying vec3 v_normal;

float r_hash(vec2 p) {
	vec3 p3 = fract(vec3(p.xyx) * 0.1031);
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.x + p3.y) * p3.z);
}

float r_noise(vec2 p) {
	vec2 i = floor(p);
	vec2 f = fract(p);
	vec2 u = f * f * (3.0 - 2.0 * f);
	return mix(mix(r_hash(i), r_hash(i + vec2(1.0, 0.0)), u.x),
			mix(r_hash(i + vec2(0.0, 1.0)), r_hash(i + vec2(1.0, 1.0)), u.x), u.y);
}

// Value noise's gradient (Quilez): the slope of a ripple octave.
vec2 r_noise_grad(vec2 p) {
	vec2 i = floor(p);
	vec2 f = fract(p);
	vec2 du = 6.0 * f * (1.0 - f);
	vec2 u = f * f * (3.0 - 2.0 * f);
	float a = r_hash(i);
	float b = r_hash(i + vec2(1.0, 0.0));
	float c = r_hash(i + vec2(0.0, 1.0));
	float d = r_hash(i + vec2(1.0, 1.0));
	return du * (vec2(b - a, c - a) + (a - b - c + d) * u.yx);
}

// Slope of the ripples at p (three octaves, each half the height of the last
// per doubling of frequency -- roughly a capillary-gravity spectrum).
vec2 ripple_slope(vec2 p) {
	vec2 s = r_noise_grad(p * 1.7) * 0.5;
	s += r_noise_grad(p * 3.9 + 11.0) * 0.3;
	s += r_noise_grad(p * 8.6 - 7.0) * 0.2;
	return s;
}

// The two phases' offsets and the first one's weight.
float flow_phases(vec2 p, vec2 flow, out vec2 o_a, out vec2 o_b) {
	float t = water_time / max(flow_period, 0.05) + r_noise(p * 0.05);
	float p_a = fract(t);
	float p_b = fract(t + 0.5);
	o_a = flow * (p_a - 0.5) * flow_period;
	o_b = flow * (p_b - 0.5) * flow_period;
	return 1.0 - abs(1.0 - 2.0 * p_a);
}

void vertex() {
	v_world = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	v_flow = (MODEL_MATRIX * vec4(UV2.x, 0.0, UV2.y, 0.0)).xz;
	v_info = COLOR;
	v_normal = normalize((MODEL_MATRIX * vec4(NORMAL, 0.0)).xyz);
}

void fragment() {
	vec2 p = v_world.xz * ripple_scale;
	vec2 o_a;
	vec2 o_b;
	float w = flow_phases(v_world.xz, v_flow, o_a, o_b);
	// Surface-gradient blend of the two phases, renormalised so the crossover
	// doesn't flatten the ripples (phase B is shifted half a tile, so the two are
	// close to independent).
	vec2 slope = (ripple_slope(p - o_a * ripple_scale) * w + ripple_slope(p - o_b * ripple_scale + vec2(13.1, 7.7)) * (1.0 - w))
			/ sqrt(w * w + (1.0 - w) * (1.0 - w));
	float speed = length(v_flow);
	float depth = v_info.r * 4.0 - 1.0;
	// Calm at the waterline: ripples die out over the last 15 cm.
	slope *= (ripple_strength + ripple_speed_strength * speed) * smoothstep(0.0, 0.15, depth);
	vec3 n_mesh = normalize(v_normal);
	if (n_mesh.y < 0.0) {
		n_mesh = -n_mesh;
	}
	vec3 world_normal = normalize(n_mesh - vec3(slope.x, 0.0, slope.y));

	// White water, broken into lace that rides the current.
	vec2 q_a = (v_world.xz - o_a) * foam_detail_scale;
	vec2 q_b = (v_world.xz - o_b) * foam_detail_scale + vec2(5.3, 2.1);
	float lace_a = 0.5 * r_noise(q_a) + 0.3 * r_noise(q_a * 2.7 + 17.0) + 0.2 * r_noise(q_a * 7.3 - 5.0);
	float lace_b = 0.5 * r_noise(q_b) + 0.3 * r_noise(q_b * 2.7 + 17.0) + 0.2 * r_noise(q_b * 7.3 - 5.0);
	float lace = lace_a * w + lace_b * (1.0 - w);
	float foam = clamp(v_info.g * (1.0 + foam_breakup) - lace * foam_breakup, 0.0, 1.0);
	foam = foam * foam * (3.0 - 2.0 * foam);

#if defined(RT)
	NORMAL = normalize((VIEW_MATRIX * vec4(world_normal, 0.0)).xyz);
	ALBEDO = mix(vec3(1.0), foam_color.rgb, foam);
	ROUGHNESS = mix(pathtraced_roughness, 0.8, foam);
	SPECULAR = 0.5;
	METALLIC = 0.0;
	TRANSMISSION = 1.0 - foam;
	IOR = 1.33;
	ABSORPTION = pathtraced_absorption;
	SCATTERING = pathtraced_scattering;
	SCATTERING_ANISOTROPY = pathtraced_scattering_anisotropy;
	// Gone past the waterline and the banks' fade, as in raster.
	ALPHA = smoothstep(0.0, max(edge_softness, 1e-3), depth) * v_info.b;
	ALPHA_SCISSOR_THRESHOLD = 0.5;
#else
	// Unshaded with a small bounded shading term, as PhysXWaterSurface3D's raster look.
	vec3 n = normalize((VIEW_MATRIX * vec4(world_normal, 0.0)).xyz);
	float ndotl = clamp(dot(n, vec3(0.3, 0.85, 0.4)), 0.0, 1.0);
	float deep = smoothstep(0.0, 1.2, depth);
	vec3 base_rgb = mix(shallow_color.rgb, water_color.rgb, deep) * mix(0.75, 1.15, ndotl);
	float base_a = mix(shallow_color.a, water_color.a, deep);
	ALBEDO = mix(base_rgb, foam_color.rgb, foam);
	float edge = smoothstep(0.0, max(edge_softness, 1e-3), depth) * v_info.b;
	ALPHA = mix(base_a, foam_color.a, foam) * edge;
#endif
}
)";

constexpr float GRAVITY = 9.81f;
constexpr float MAX_SPEED = 12.0f; // m/s cap (a fall's sheet, not a sustained river)
constexpr float CASCADE_MIN_DROP = 0.15f; // m

} // namespace

void PhysXRiver3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_widths", "widths"), &PhysXRiver3D::set_widths);
	ClassDB::bind_method(D_METHOD("get_widths"), &PhysXRiver3D::get_widths);
	ClassDB::bind_method(D_METHOD("set_depths", "depths"), &PhysXRiver3D::set_depths);
	ClassDB::bind_method(D_METHOD("get_depths"), &PhysXRiver3D::get_depths);
	ClassDB::bind_method(D_METHOD("set_default_width", "width"), &PhysXRiver3D::set_default_width);
	ClassDB::bind_method(D_METHOD("get_default_width"), &PhysXRiver3D::get_default_width);
	ClassDB::bind_method(D_METHOD("set_discharge", "discharge"), &PhysXRiver3D::set_discharge);
	ClassDB::bind_method(D_METHOD("get_discharge"), &PhysXRiver3D::get_discharge);
	ClassDB::bind_method(D_METHOD("set_roughness", "roughness"), &PhysXRiver3D::set_roughness);
	ClassDB::bind_method(D_METHOD("get_roughness"), &PhysXRiver3D::get_roughness);
	ClassDB::bind_method(D_METHOD("set_max_slope", "slope"), &PhysXRiver3D::set_max_slope);
	ClassDB::bind_method(D_METHOD("get_max_slope"), &PhysXRiver3D::get_max_slope);
	ClassDB::bind_method(D_METHOD("set_snap_to_ground", "snap"), &PhysXRiver3D::set_snap_to_ground);
	ClassDB::bind_method(D_METHOD("get_snap_to_ground"), &PhysXRiver3D::get_snap_to_ground);
	ClassDB::bind_method(D_METHOD("set_ground_collision_mask", "mask"), &PhysXRiver3D::set_ground_collision_mask);
	ClassDB::bind_method(D_METHOD("get_ground_collision_mask"), &PhysXRiver3D::get_ground_collision_mask);
	ClassDB::bind_method(D_METHOD("set_obstacle_collision_mask", "mask"), &PhysXRiver3D::set_obstacle_collision_mask);
	ClassDB::bind_method(D_METHOD("get_obstacle_collision_mask"), &PhysXRiver3D::get_obstacle_collision_mask);
	ClassDB::bind_method(D_METHOD("set_step_length", "length"), &PhysXRiver3D::set_step_length);
	ClassDB::bind_method(D_METHOD("get_step_length"), &PhysXRiver3D::get_step_length);
	ClassDB::bind_method(D_METHOD("set_cross_segments", "segments"), &PhysXRiver3D::set_cross_segments);
	ClassDB::bind_method(D_METHOD("get_cross_segments"), &PhysXRiver3D::get_cross_segments);
	ClassDB::bind_method(D_METHOD("set_bank_margin", "margin"), &PhysXRiver3D::set_bank_margin);
	ClassDB::bind_method(D_METHOD("get_bank_margin"), &PhysXRiver3D::get_bank_margin);
	ClassDB::bind_method(D_METHOD("set_foam_persistence", "seconds"), &PhysXRiver3D::set_foam_persistence);
	ClassDB::bind_method(D_METHOD("get_foam_persistence"), &PhysXRiver3D::get_foam_persistence);
	ClassDB::bind_method(D_METHOD("set_water_material", "material"), &PhysXRiver3D::set_water_material);
	ClassDB::bind_method(D_METHOD("get_water_material"), &PhysXRiver3D::get_water_material);
	ClassDB::bind_method(D_METHOD("set_carve", "carve"), &PhysXRiver3D::set_carve);
	ClassDB::bind_method(D_METHOD("get_carve"), &PhysXRiver3D::get_carve);
	ClassDB::bind_method(D_METHOD("set_carve_depth", "depth"), &PhysXRiver3D::set_carve_depth);
	ClassDB::bind_method(D_METHOD("get_carve_depth"), &PhysXRiver3D::get_carve_depth);
	ClassDB::bind_method(D_METHOD("set_carve_bank_slope", "slope"), &PhysXRiver3D::set_carve_bank_slope);
	ClassDB::bind_method(D_METHOD("get_carve_bank_slope"), &PhysXRiver3D::get_carve_bank_slope);
	ClassDB::bind_method(D_METHOD("set_carve_falloff", "falloff"), &PhysXRiver3D::set_carve_falloff);
	ClassDB::bind_method(D_METHOD("get_carve_falloff"), &PhysXRiver3D::get_carve_falloff);
	ClassDB::bind_method(D_METHOD("set_carve_cell_size", "size"), &PhysXRiver3D::set_carve_cell_size);
	ClassDB::bind_method(D_METHOD("get_carve_cell_size"), &PhysXRiver3D::get_carve_cell_size);
	ClassDB::bind_method(D_METHOD("set_base_height_source", "path"), &PhysXRiver3D::set_base_height_source);
	ClassDB::bind_method(D_METHOD("get_base_height_source"), &PhysXRiver3D::get_base_height_source);
	ClassDB::bind_method(D_METHOD("get_carve_brushes", "tile_size"), &PhysXRiver3D::get_carve_brushes, DEFVAL(64.0f));
	ClassDB::bind_method(D_METHOD("rebuild"), &PhysXRiver3D::rebuild);
	ClassDB::bind_method(D_METHOD("sample_height", "world_pos"), &PhysXRiver3D::sample_height);
	ClassDB::bind_method(D_METHOD("sample_flow", "world_pos"), &PhysXRiver3D::sample_flow);
	ClassDB::bind_method(D_METHOD("sample_depth", "world_pos"), &PhysXRiver3D::sample_depth);
	ClassDB::bind_method(D_METHOD("sample_foam", "world_pos"), &PhysXRiver3D::sample_foam);
	ClassDB::bind_method(D_METHOD("get_length"), &PhysXRiver3D::get_length);
	ClassDB::bind_method(D_METHOD("get_section_count"), &PhysXRiver3D::get_section_count);
	ClassDB::bind_method(D_METHOD("get_section", "index"), &PhysXRiver3D::get_section);

	ADD_GROUP("Shape", "");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_FLOAT32_ARRAY, "widths"), "set_widths", "get_widths");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_FLOAT32_ARRAY, "depths"), "set_depths", "get_depths");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "default_width", PROPERTY_HINT_RANGE, "0.1,100,0.05,or_greater,suffix:m"), "set_default_width", "get_default_width");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "snap_to_ground"), "set_snap_to_ground", "get_snap_to_ground");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "ground_collision_mask", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_ground_collision_mask", "get_ground_collision_mask");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "obstacle_collision_mask", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_obstacle_collision_mask", "get_obstacle_collision_mask");
	ADD_GROUP("Hydraulics", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "discharge", PROPERTY_HINT_RANGE, "0.01,1000,0.01,or_greater,suffix:m³/s"), "set_discharge", "get_discharge");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "roughness", PROPERTY_HINT_RANGE, "0.01,0.15,0.001"), "set_roughness", "get_roughness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "max_slope", PROPERTY_HINT_RANGE, "0.005,1,0.005,or_greater"), "set_max_slope", "get_max_slope");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "foam_persistence", PROPERTY_HINT_RANGE, "0.1,30,0.1,or_greater,suffix:s"), "set_foam_persistence", "get_foam_persistence");
	ADD_GROUP("Carve", "carve_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "carve"), "set_carve", "get_carve");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "carve_depth", PROPERTY_HINT_RANGE, "0,20,0.05,or_greater,suffix:m"), "set_carve_depth", "get_carve_depth");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "carve_bank_slope", PROPERTY_HINT_RANGE, "0.05,5,0.05,or_greater"), "set_carve_bank_slope", "get_carve_bank_slope");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "carve_falloff", PROPERTY_HINT_RANGE, "0,20,0.05,or_greater,suffix:m"), "set_carve_falloff", "get_carve_falloff");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "carve_cell_size", PROPERTY_HINT_RANGE, "0.05,4,0.05,or_greater,suffix:m"), "set_carve_cell_size", "get_carve_cell_size");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "base_height_source"), "set_base_height_source", "get_base_height_source");

	ADD_SIGNAL(MethodInfo("carve_changed"));

	ADD_GROUP("Mesh", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "step_length", PROPERTY_HINT_RANGE, "0.1,10,0.05,or_greater,suffix:m"), "set_step_length", "get_step_length");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "cross_segments", PROPERTY_HINT_RANGE, "2,64,1"), "set_cross_segments", "get_cross_segments");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "bank_margin", PROPERTY_HINT_RANGE, "0,10,0.05,or_greater,suffix:m"), "set_bank_margin", "get_bank_margin");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "water_material", PROPERTY_HINT_RESOURCE_TYPE, "ShaderMaterial"), "set_water_material", "get_water_material");
}

#define RIVER_SETTER(m_name, m_type, m_member, m_expr) \
	void PhysXRiver3D::set_##m_name(m_type p_value) { \
		m_member = (m_expr); \
		_queue_build(); \
	}

RIVER_SETTER(widths, const PackedFloat32Array &, widths, p_value)
RIVER_SETTER(depths, const PackedFloat32Array &, depths, p_value)
RIVER_SETTER(default_width, float, default_width, MAX(p_value, 0.05f))
RIVER_SETTER(discharge, float, discharge, MAX(p_value, 0.001f))
RIVER_SETTER(roughness, float, roughness, CLAMP(p_value, 0.005f, 0.5f))
RIVER_SETTER(max_slope, float, max_slope, MAX(p_value, 0.001f))
RIVER_SETTER(snap_to_ground, bool, snap_to_ground, p_value)
RIVER_SETTER(ground_collision_mask, uint32_t, ground_collision_mask, p_value)
RIVER_SETTER(obstacle_collision_mask, uint32_t, obstacle_collision_mask, p_value)
RIVER_SETTER(step_length, float, step_length, MAX(p_value, 0.05f))
RIVER_SETTER(cross_segments, int, cross_segments, CLAMP(p_value, 2, 256))
RIVER_SETTER(bank_margin, float, bank_margin, MAX(p_value, 0.0f))
RIVER_SETTER(foam_persistence, float, foam_persistence, MAX(p_value, 0.05f))
RIVER_SETTER(carve, bool, carve, p_value)
RIVER_SETTER(carve_depth, float, carve_depth, MAX(p_value, 0.0f))
RIVER_SETTER(carve_bank_slope, float, carve_bank_slope, MAX(p_value, 0.01f))
RIVER_SETTER(carve_falloff, float, carve_falloff, MAX(p_value, 0.0f))
RIVER_SETTER(carve_cell_size, float, carve_cell_size, MAX(p_value, 0.05f))
RIVER_SETTER(base_height_source, const NodePath &, base_height_source, p_value)

#undef RIVER_SETTER

void PhysXRiver3D::set_water_material(const Ref<ShaderMaterial> &p_material) {
	water_material = p_material;
	if (mesh_instance != nullptr) {
		mesh_instance->set_material_override(water_material);
	}
}

void PhysXRiver3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			if (!is_connected(SNAME("curve_changed"), callable_mp(this, &PhysXRiver3D::_on_curve_changed))) {
				connect(SNAME("curve_changed"), callable_mp(this, &PhysXRiver3D::_on_curve_changed));
			}
			_queue_build();
			set_physics_process_internal(true);
			set_process_internal(!Engine::get_singleton()->is_editor_hint());
		} break;
		case NOTIFICATION_EXIT_TREE: {
			set_physics_process_internal(false);
			set_process_internal(false);
		} break;
		case NOTIFICATION_INTERNAL_PHYSICS_PROCESS: {
			// Ground rays need the ground in the physics space, which it is by the first tick.
			if (build_pending) {
				_build();
			}
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			water_time += get_process_delta_time();
			if (water_material.is_valid()) {
				water_material->set_shader_parameter(SNAME("water_time"), (float)Math::fmod(water_time, 3600.0));
			}
		} break;
	}
}

void PhysXRiver3D::_on_curve_changed() {
	_queue_build();
}

void PhysXRiver3D::_queue_build() {
	build_pending = true;
}

void PhysXRiver3D::rebuild() {
	_build();
}

float PhysXRiver3D::_point_value(const PackedFloat32Array &p_values, int p_index, float p_default) const {
	if (p_values.is_empty()) {
		return p_default;
	}
	const float v = p_values[MIN(p_index, p_values.size() - 1)];
	return v > 0.0f ? v : p_default;
}

bool PhysXRiver3D::_ground_at(PhysicsDirectSpaceState3D *p_space, const Vector3 &p_local, float &r_y, uint32_t p_mask) const {
	if (p_space == nullptr) {
		return false;
	}
	constexpr real_t PROBE_ABOVE = 50.0;
	constexpr real_t PROBE_BELOW = 200.0;
	constexpr int MAX_SKIPS = 8;
	const Transform3D xf = get_global_transform();
	PhysicsDirectSpaceState3D::RayParameters ray;
	ray.from = xf.xform(p_local + Vector3(0, PROBE_ABOVE, 0));
	ray.to = xf.xform(p_local - Vector3(0, PROBE_BELOW, 0));
	ray.collision_mask = p_mask;
	ray.collide_with_areas = false;
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	for (int skip = 0; skip < MAX_SKIPS; skip++) {
		PhysicsDirectSpaceState3D::RayResult result;
		if (!p_space->intersect_ray(ray, result)) {
			return false;
		}
		// Only static ground: anything floating or moving is skipped.
		if (ps->body_get_mode(result.rid) == PhysicsServer3D::BODY_MODE_STATIC) {
			r_y = xf.affine_inverse().xform(result.position).y;
			return true;
		}
		ray.exclude.insert(result.rid);
	}
	return false;
}

// The ground as it was before any carving: from base_height_source's
// base_height_at(world_xz) when set (a terrain deformer keeps the uncarved
// heights; measuring the carved ground would carve deeper on every rebuild),
// else measured.
bool PhysXRiver3D::_base_ground_at(PhysicsDirectSpaceState3D *p_space, const Vector3 &p_local, float &r_y) const {
	Node *source = base_height_source.is_empty() || !is_inside_tree() ? nullptr : get_node_or_null(base_height_source);
	if (source != nullptr && source->has_method(SNAME("base_height_at"))) {
		const Transform3D xf = get_global_transform();
		const Vector3 w = xf.xform(p_local);
		const Variant v = source->call(SNAME("base_height_at"), Vector2(w.x, w.z));
		if (v.get_type() != Variant::FLOAT && v.get_type() != Variant::INT) {
			return false;
		}
		const float h = v;
		if (Math::is_nan(h)) {
			return false;
		}
		r_y = xf.affine_inverse().xform(Vector3(w.x, h, w.z)).y;
		return true;
	}
	return _ground_at(p_space, p_local, r_y, ground_collision_mask);
}

// The carved channel's height (local y) at p_n across a section, and how much
// the brush applies there (1 inside the banks, fading to 0 past them). A
// trapezoid: a flat floor 40% of the width, banks sloping up to the
// uncarved ground; at a bend the floor moves towards the outer bank (deep
// outside, a shallow bar inside -- Peytavie et al. 2019's asymmetric sections).
float PhysXRiver3D::_carve_target(const Section &p_sec, float p_n, float &r_mask) const {
	const float half = 0.5f * p_sec.width;
	const float offset = CLAMP(-p_sec.curvature * p_sec.width * p_sec.width * 0.25f, -0.25f * half, 0.25f * half);
	const float floor_half = 0.4f * half;
	const float lo = offset - floor_half;
	const float hi = offset + floor_half;
	const float an = Math::abs(p_n);
	r_mask = 1.0f;
	if (an > half) {
		const float f = carve_falloff > 1e-4f ? CLAMP((an - half) / carve_falloff, 0.0f, 1.0f) : 1.0f;
		r_mask = 1.0f - f * f * (3.0f - 2.0f * f);
		return p_sec.carve_top + (an - half) * carve_bank_slope;
	}
	float shape;
	if (p_n < lo) {
		shape = (p_n + half) / MAX(lo + half, 1e-3f);
	} else if (p_n > hi) {
		shape = (half - p_n) / MAX(half - hi, 1e-3f);
	} else {
		shape = 1.0f;
	}
	shape = CLAMP(shape, 0.0f, 1.0f);
	shape = shape * shape * (3.0f - 2.0f * shape);
	return p_sec.carve_top - (p_sec.carve_top - p_sec.carve_bed) * shape;
}

void PhysXRiver3D::_build() {
	build_pending = false;
	sections.clear();
	const Ref<Curve3D> c = get_curve();
	if (!is_inside_tree() || c.is_null() || c->get_point_count() < 2 || c->get_baked_length() < 0.05f) {
		_build_mesh();
		return;
	}
	const float length = c->get_baked_length();
	const int count = MAX(2, (int)Math::ceil(length / step_length) + 1);
	const float ds = length / (count - 1);

	// Arc length of each control point, for interpolating its width and depth.
	const int pc = c->get_point_count();
	LocalVector<float> point_s;
	point_s.resize(pc);
	for (int k = 0; k < pc; k++) {
		point_s[k] = c->get_closest_offset(c->get_point_position(k));
		if (k > 0) {
			point_s[k] = MAX(point_s[k], point_s[k - 1]);
		}
	}
	auto interp = [&](const PackedFloat32Array &p_values, float p_default, float p_s) -> float {
		if (p_s <= point_s[0]) {
			return _point_value(p_values, 0, p_default);
		}
		for (int k = 1; k < pc; k++) {
			if (p_s <= point_s[k]) {
				const float span = point_s[k] - point_s[k - 1];
				const float t = span > 1e-4f ? (p_s - point_s[k - 1]) / span : 1.0f;
				return Math::lerp(_point_value(p_values, k - 1, p_default), _point_value(p_values, k, p_default), t);
			}
		}
		return _point_value(p_values, pc - 1, p_default);
	};

	PhysicsDirectSpaceState3D *space = nullptr;
	if (snap_to_ground && is_inside_world() && get_world_3d().is_valid()) {
		space = get_world_3d()->get_direct_space_state();
	}

	// Centre line: position, heading, width, given depth (0 = from Manning), bed.
	sections.resize(count);
	LocalVector<float> given_depth;
	LocalVector<float> bed;
	LocalVector<uint8_t> bed_found;
	given_depth.resize(count);
	bed.resize(count);
	bed_found.resize(count);
	Vector3 last_tangent(1, 0, 0);
	for (int i = 0; i < count; i++) {
		Section &sec = sections[i];
		sec.s = i * ds;
		sec.centre = c->sample_baked(sec.s, true);
		Vector3 t = c->sample_baked(MIN(sec.s + 0.5f * ds, length), true) - c->sample_baked(MAX(sec.s - 0.5f * ds, 0.0f), true);
		t.y = 0.0f;
		if (t.length_squared() < 1e-8f) {
			t = last_tangent;
		}
		t.normalize();
		sec.curvature = i > 0 ? last_tangent.cross(t).y / ds : 0.0f;
		last_tangent = t;
		sec.side = Vector3(t.z, 0.0f, -t.x);
		sec.width = interp(widths, default_width, sec.s);
		given_depth[i] = depths.is_empty() ? 0.0f : interp(depths, 0.0f, sec.s);
		float y = 0.0f;
		bed_found[i] = _base_ground_at(space, sec.centre, y) ? 1 : 0;
		bed[i] = bed_found[i] ? y : sec.centre.y - (given_depth[i] > 0.0f ? given_depth[i] : 0.0f);
	}

	// Ground across each section, measured once (independent of the water).
	const int m = cross_segments;
	LocalVector<float> ground;
	LocalVector<uint8_t> ground_found;
	ground.resize(count * (m + 1));
	ground_found.resize(count * (m + 1));
	for (int i = 0; i < count; i++) {
		const Section &sec = sections[i];
		const float half = 0.5f * sec.width + bank_margin;
		for (int j = 0; j <= m; j++) {
			const float n = -half + j * (2.0f * half / m);
			float y = 0.0f;
			ground_found[i * (m + 1) + j] = _base_ground_at(space, sec.centre + sec.side * n, y) ? 1 : 0;
			ground[i * (m + 1) + j] = y;
		}
	}
	{
		// Curvature smoothed over about 6 m (a bend, not the polyline's kinks).
		const int cw = MAX(1, (int)Math::round(3.0f / ds));
		LocalVector<float> raw;
		raw.resize(count);
		for (int i = 0; i < count; i++) {
			raw[i] = sections[i].curvature;
		}
		for (int i = 0; i < count; i++) {
			float sum = 0.0f;
			int used = 0;
			for (int k = MAX(0, i - cw); k <= MIN(count - 1, i + cw); k++) {
				sum += raw[k];
				used++;
			}
			sections[i].curvature = sum / used;
		}
	}
	if (carve) {
		// The channel floor: carve_depth under the uncarved ground, never
		// climbing downstream -- through a rise it cuts (Horizon's river tool).
		for (int i = 0; i < count; i++) {
			Section &sec = sections[i];
			sec.carve_top = bed[i];
			sec.carve_bed = bed[i] - carve_depth;
			if (i > 0) {
				sec.carve_bed = MIN(sec.carve_bed, sections[i - 1].carve_bed);
			}
			bed[i] = sec.carve_bed;
			bed_found[i] = 1;
			// The water sees the ground as the brush leaves it ("lower to").
			const float half = 0.5f * sec.width + bank_margin;
			for (int j = 0; j <= m; j++) {
				const float n = -half + j * (2.0f * half / m);
				float mask = 0.0f;
				const float target = _carve_target(sec, n, mask);
				const int k = i * (m + 1) + j;
				const float base = ground_found[k] ? ground[k] : sec.carve_top;
				ground[k] = Math::lerp(base, MIN(base, target), mask);
				ground_found[k] = 1;
			}
		}
	}
	// Rocks and other obstacles on their own layers stand on the (base or
	// carved) ground: a carved river measures the uncarved terrain, which has
	// no rocks in it.
	PhysicsDirectSpaceState3D *obstacle_space = space;
	if (obstacle_space == nullptr && obstacle_collision_mask != 0 && is_inside_world() && get_world_3d().is_valid()) {
		obstacle_space = get_world_3d()->get_direct_space_state();
	}
	if (obstacle_collision_mask != 0 && obstacle_space != nullptr) {
		for (int i = 0; i < count; i++) {
			const Section &sec = sections[i];
			const float half = 0.5f * sec.width + bank_margin;
			for (int j = 0; j <= m; j++) {
				const float n = -half + j * (2.0f * half / m);
				float y = 0.0f;
				const int k = i * (m + 1) + j;
				if (_ground_at(obstacle_space, sec.centre + sec.side * n, y, obstacle_collision_mask) && (!ground_found[k] || y > ground[k])) {
					ground[k] = y;
					ground_found[k] = 1;
				}
			}
		}
	}
	// Water depth across section i for a surface height: measured ground, or
	// with none, a parabolic bed inside the banks rising past them.
	auto depth_at = [&](int p_i, int p_j, float p_surface, float p_centre_depth) -> float {
		const Section &sec = sections[p_i];
		if (ground_found[p_i * (m + 1) + p_j]) {
			return p_surface - ground[p_i * (m + 1) + p_j];
		}
		const float half = 0.5f * sec.width + bank_margin;
		const float n = -half + p_j * (2.0f * half / m);
		const float u = 2.0f * n / sec.width;
		return Math::abs(u) <= 1.0f ? p_centre_depth * (1.0f - u * u) : -(Math::abs(n) - 0.5f * sec.width);
	};
	// Manning conveyance of section i at a surface height, divided channel:
	// K = integral(d^(5/3) dn) / n, so Q = K S^(1/2).
	auto conveyance_at = [&](int p_i, float p_surface, float p_centre_depth) -> float {
		const Section &sec = sections[p_i];
		const float dn = (sec.width + 2.0f * bank_margin) / m;
		float k = 0.0f;
		for (int j = 0; j <= m; j++) {
			const float d = depth_at(p_i, j, p_surface, p_centre_depth);
			if (d > 0.0f) {
				k += ((j == 0 || j == m) ? 0.5f : 1.0f) * dn * Math::pow(d, 5.0f / 3.0f);
			}
		}
		return k / roughness;
	};

	// Bed slope for Manning's normal depth, averaged over about 10 m, leaving
	// out steps steeper than max_slope: those are cascades the water falls
	// down, not a reach it flows along (counting them would drain the water
	// either side of a fall to a film).
	const int half_window = MAX(1, (int)Math::round(5.0f / ds));
	LocalVector<float> bed_slope;
	bed_slope.resize(count);
	for (int i = 0; i < count; i++) {
		const int a = MAX(0, i - half_window);
		const int b = MIN(count - 1, i + half_window);
		float drop = 0.0f;
		int used = 0;
		for (int k = a; k < b; k++) {
			const float g = (bed[k] - bed[k + 1]) / ds;
			if (g <= max_slope) {
				drop += g;
				used++;
			}
		}
		bed_slope[i] = used > 0 ? MAX(drop / used, 1e-4f) : max_slope;
	}

	// Surface: the normal depth over the real cross-section where the river
	// follows the ground (or the given depth), the path itself otherwise; then
	// never uphill.
	LocalVector<float> centre_depth;
	centre_depth.resize(count);
	for (int i = 0; i < count; i++) {
		Section &sec = sections[i];
		float surface;
		if (given_depth[i] > 0.0f) {
			surface = bed_found[i] ? bed[i] + given_depth[i] : sec.centre.y;
			centre_depth[i] = given_depth[i];
		} else if (bed_found[i] || snap_to_ground) {
			// Bisect for the surface whose conveyance carries the discharge:
			// Q = K(surface) S^(1/2). Without measured ground, the parabola's
			// centre depth is the unknown instead.
			const float target = discharge / Math::sqrt(bed_slope[i]);
			float lo = 0.0f;
			float hi = 20.0f;
			for (int it = 0; it < 30; it++) {
				const float mid = 0.5f * (lo + hi);
				if (conveyance_at(i, bed[i] + mid, mid) < target) {
					lo = mid;
				} else {
					hi = mid;
				}
			}
			const float d = MAX(0.5f * (lo + hi), 0.02f);
			surface = bed[i] + d;
			centre_depth[i] = d;
		} else {
			surface = sec.centre.y;
			centre_depth[i] = Math::pow(discharge * roughness / (sec.width * Math::sqrt(bed_slope[i])), 0.6f);
			bed[i] = surface - centre_depth[i];
		}
		sec.surface = surface;
		if (i > 0) {
			sec.surface = MIN(sec.surface, sections[i - 1].surface);
		}
		// A cascade is a real fall: steeper than max_slope and more than a few
		// centimetres (the running minimum leaves small steps where a rock's
		// backwater meets the reach below it; those are not falls).
		const float drop = i > 0 ? sections[i - 1].surface - sec.surface : 0.0f;
		sec.cascade = drop / ds > max_slope && drop > CASCADE_MIN_DROP;
		sec.centre.y = sec.surface;
	}
	for (int i = 0; i < count; i++) {
		const int a = MAX(0, i - half_window);
		const int b = MIN(count - 1, i + half_window);
		sections[i].slope = b > a ? (sections[a].surface - sections[b].surface) / ((b - a) * ds) : 0.0f;
	}

	// Across: depth per sample, then the discharge shared as depth^(5/3).
	float carried_foam = 0.0f;
	for (int i = 0; i < count; i++) {
		Section &sec = sections[i];
		const float half = 0.5f * sec.width + bank_margin;
		sec.depth.resize(m + 1);
		sec.speed_at.resize(m + 1);
		const float dn = 2.0f * half / m;
		float conveyance = 0.0f; // integral of d^(5/3) dn
		float area = 0.0f;
		float wet_width = 0.0f;
		for (int j = 0; j <= m; j++) {
			const float d = depth_at(i, j, sec.surface, sec.surface - bed[i]);
			sec.depth[j] = d;
			const float w = (j == 0 || j == m) ? 0.5f : 1.0f;
			if (d > 0.0f) {
				conveyance += w * dn * Math::pow(d, 5.0f / 3.0f);
				area += w * dn * d;
				wet_width += w * dn;
			}
		}
		for (int j = 0; j <= m; j++) {
			const float d = sec.depth[j];
			// v = q / d = Q d^(2/3) / integral(d^(5/3)).
			sec.speed_at[j] = (d > 0.0f && conveyance > 1e-6f) ? MIN(discharge * Math::pow(d, 2.0f / 3.0f) / conveyance, MAX_SPEED) : 0.0f;
		}
		sec.speed = area > 1e-6f ? MIN(discharge / area, MAX_SPEED) : 0.0f;
		// White water: supercritical flow (Froude above ~1) and the foot of every
		// cascade, then carried downstream at the current's speed, fading.
		const float mean_depth = wet_width > 1e-4f ? area / wet_width : 0.0f;
		const float froude = mean_depth > 1e-3f ? sec.speed / Math::sqrt(GRAVITY * mean_depth) : 0.0f;
		float source = CLAMP((froude - 0.8f) / 0.8f, 0.0f, 1.0f);
		source = source * source * (3.0f - 2.0f * source);
		if (sec.cascade) {
			source = 1.0f;
		}
		carried_foam *= Math::exp(-ds / (MAX(sec.speed, 0.1f) * foam_persistence));
		carried_foam = MAX(carried_foam, source);
		sec.foam = carried_foam;
	}
	_solve_flow(ds);
	_build_mesh();
	update_configuration_warnings();
	if (carve) {
		emit_signal(SNAME("carve_changed"));
	}
}

// Flow round rocks. Steady river flow is friction dominated (Manning), so the
// unit discharge q follows the surface gradient: q = -K grad(eta) with
// K = d^(5/3) / (n sqrt(S)) (the diffusive-wave model of flood codes such as
// LISFLOOD-FP, Bates and De Roo 2000). Continuity, div q = 0, then makes q the
// curl of a stream function psi with div(grad(psi) / K) = 0. In a straight
// reach that gives exactly the divided channel's d^(5/3) profile, and round a
// rock (dry cells inside the river) the flow parts, speeds up past its sides
// and slows in front and behind. Solved in the river's own strip coordinates
// (s along, n across) with the bend's metric h = 1 - curvature n, psi held at
// 0 on the right bank and the discharge on the left, ends held at the
// section's own profile; an island's psi is chosen so no net flow goes round
// it (as PhysXWaterSurface3D's flow bake). Then white water per sample:
// Froude, cascades, and where the current strikes or grazes a rock, carried
// downstream along the flow.
void PhysXRiver3D::_solve_flow(float p_ds) {
	const int count = (int)sections.size();
	const int m = cross_segments;
	const int row = m + 1;
	const int cells = count * row;
	constexpr float DRY = 0.02f; // m: shallower is a wall
	constexpr float MIN_D = 0.05f; // m: depth floor for weights and speeds
	auto is_wet = [&](int p_i, int p_j) -> bool {
		return sections[p_i].depth[p_j] > DRY;
	};
	auto dn_of = [&](int p_i) -> float {
		return (sections[p_i].width + 2.0f * bank_margin) / m;
	};
	auto n_of = [&](int p_i, int p_j) -> float {
		const float half = 0.5f * sections[p_i].width + bank_margin;
		return -half + p_j * (2.0f * half / m);
	};
	auto metric = [&](int p_i, int p_j) -> float {
		return MAX(0.2f, 1.0f - sections[p_i].curvature * n_of(p_i, p_j));
	};
	LocalVector<float> inv_k; // 1 / d^(5/3), 0 where dry
	inv_k.resize(cells);
	for (int i = 0; i < count; i++) {
		for (int j = 0; j <= m; j++) {
			inv_k[i * row + j] = is_wet(i, j) ? Math::pow(MAX(sections[i].depth[j], MIN_D), -5.0f / 3.0f) : 0.0f;
		}
	}

	// Start from each section's own d^(5/3) profile, integrated from the right bank.
	LocalVector<float> psi;
	psi.resize(cells);
	for (int i = 0; i < count; i++) {
		const float dn = dn_of(i);
		float acc = 0.0f;
		psi[i * row] = 0.0f;
		for (int j = 1; j <= m; j++) {
			const float a = is_wet(i, j - 1) ? Math::pow(sections[i].depth[j - 1], 5.0f / 3.0f) : 0.0f;
			const float b = is_wet(i, j) ? Math::pow(sections[i].depth[j], 5.0f / 3.0f) : 0.0f;
			acc += 0.5f * (a + b) * dn;
			psi[i * row + j] = acc;
		}
		const float scale = acc > 1e-9f ? discharge / acc : 0.0f;
		for (int j = 0; j <= m; j++) {
			psi[i * row + j] *= scale;
		}
	}

	// Dry patches: the banks keep their start values (0 and the discharge);
	// a patch that touches neither bank edge nor an end is an island, solved for.
	LocalVector<int> comp;
	comp.resize(cells);
	for (int c = 0; c < cells; c++) {
		comp[c] = -1;
	}
	LocalVector<uint8_t> comp_island;
	LocalVector<int> stack;
	int comps = 0;
	for (int c = 0; c < cells; c++) {
		if (comp[c] >= 0 || inv_k[c] > 0.0f) {
			continue;
		}
		bool island = true;
		stack.clear();
		stack.push_back(c);
		comp[c] = comps;
		while (!stack.is_empty()) {
			const int k = stack[stack.size() - 1];
			stack.remove_at(stack.size() - 1);
			const int i = k / row;
			const int j = k % row;
			if (i == 0 || i == count - 1 || j == 0 || j == m) {
				island = false;
			}
			const int nb[4] = { i > 0 ? k - row : -1, i < count - 1 ? k + row : -1, j > 0 ? k - 1 : -1, j < m ? k + 1 : -1 };
			for (int q = 0; q < 4; q++) {
				if (nb[q] >= 0 && comp[nb[q]] < 0 && inv_k[nb[q]] == 0.0f) {
					comp[nb[q]] = comps;
					stack.push_back(nb[q]);
				}
			}
		}
		comp_island.push_back(island ? 1 : 0);
		comps++;
	}
	bool any_island = false;
	for (int q = 0; q < comps; q++) {
		any_island = any_island || comp_island[q];
	}
	// An island's start value: the mean of its cells' start values.
	LocalVector<float> comp_value;
	comp_value.resize(comps);
	if (any_island) {
		LocalVector<int> comp_cells;
		comp_cells.resize(comps);
		for (int q = 0; q < comps; q++) {
			comp_value[q] = 0.0f;
			comp_cells[q] = 0;
		}
		for (int c = 0; c < cells; c++) {
			if (comp[c] >= 0 && comp_island[comp[c]]) {
				comp_value[comp[c]] += psi[c];
				comp_cells[comp[c]]++;
			}
		}
		for (int q = 0; q < comps; q++) {
			if (comp_island[q]) {
				comp_value[q] /= MAX(comp_cells[q], 1);
			}
		}
		for (int c = 0; c < cells; c++) {
			if (comp[c] >= 0 && comp_island[comp[c]]) {
				psi[c] = comp_value[comp[c]];
			}
		}

		// SOR on div(grad(psi) / K) = 0 with the bend metric. Face weights:
		// along s 1 / (h K ds^2), across h / (K dn^2); a face onto a wall
		// takes the wet cell's own K.
		const float ds2 = p_ds * p_ds;
		auto face = [&](int p_a, int p_b, bool p_along, float p_h, float p_dn2) -> float {
			const float ka = inv_k[p_a];
			const float kb = inv_k[p_b];
			const float k = (ka > 0.0f && kb > 0.0f) ? 0.5f * (ka + kb) : MAX(ka, kb);
			return p_along ? k / (p_h * ds2) : k * p_h / p_dn2;
		};
		LocalVector<float> comp_num;
		LocalVector<float> comp_den;
		comp_num.resize(comps);
		comp_den.resize(comps);
		const float omega = 1.85f;
		const float tolerance = 1e-5f * discharge;
		for (int iter = 0; iter < 4000; iter++) {
			float max_change = 0.0f;
			for (int i = 1; i < count - 1; i++) {
				const float dn2 = dn_of(i) * dn_of(i);
				for (int j = 0; j <= m; j++) {
					const int k = i * row + j;
					if (inv_k[k] == 0.0f) {
						continue;
					}
					const float h = metric(i, j);
					float num = 0.0f;
					float den = 0.0f;
					const int nb[4] = { k - row, k + row, j > 0 ? k - 1 : -1, j < m ? k + 1 : -1 };
					for (int q = 0; q < 4; q++) {
						if (nb[q] < 0) {
							continue; // the strip's side edge: no flow across it
						}
						const float w = face(k, nb[q], q < 2, h, dn2);
						num += w * psi[nb[q]];
						den += w;
					}
					if (den > 0.0f) {
						const float change = omega * (num / den - psi[k]);
						psi[k] += change;
						max_change = MAX(max_change, Math::abs(change));
					}
				}
			}
			// Islands: psi where the flow round them sums to zero.
			for (int q = 0; q < comps; q++) {
				comp_num[q] = 0.0f;
				comp_den[q] = 0.0f;
			}
			for (int i = 1; i < count - 1; i++) {
				const float dn2 = dn_of(i) * dn_of(i);
				for (int j = 0; j <= m; j++) {
					const int k = i * row + j;
					if (inv_k[k] == 0.0f) {
						continue;
					}
					const int nb[4] = { k - row, k + row, j > 0 ? k - 1 : -1, j < m ? k + 1 : -1 };
					for (int q = 0; q < 4; q++) {
						if (nb[q] < 0 || comp[nb[q]] < 0 || !comp_island[comp[nb[q]]]) {
							continue;
						}
						const float w = face(k, nb[q], q < 2, metric(i, j), dn2);
						comp_num[comp[nb[q]]] += w * psi[k];
						comp_den[comp[nb[q]]] += w;
					}
				}
			}
			for (int c = 0; c < cells; c++) {
				const int q = comp[c];
				if (q >= 0 && comp_island[q] && comp_den[q] > 0.0f) {
					psi[c] = comp_num[q] / comp_den[q];
				}
			}
			if (max_change < tolerance && iter > 4) {
				break;
			}
		}
	}

	// Velocity from psi: along = dpsi/dn / d, across = -dpsi/ds / (h d).
	for (int i = 0; i < count; i++) {
		Section &sec = sections[i];
		sec.across_at.resize(row);
		sec.foam_at.resize(row);
		const float dn = dn_of(i);
		for (int j = 0; j <= m; j++) {
			const int k = i * row + j;
			if (inv_k[k] == 0.0f) {
				sec.speed_at[j] = 0.0f;
				sec.across_at[j] = 0.0f;
				continue;
			}
			const int jl = MAX(j - 1, 0);
			const int jr = MIN(j + 1, m);
			const int il = MAX(i - 1, 0);
			const int ir = MIN(i + 1, count - 1);
			const float d = MAX(sec.depth[j], MIN_D);
			const float dpsi_dn = (psi[i * row + jr] - psi[i * row + jl]) / ((jr - jl) * dn);
			const float dpsi_ds = ir > il ? (psi[ir * row + j] - psi[il * row + j]) / ((ir - il) * p_ds) : 0.0f;
			sec.speed_at[j] = CLAMP(dpsi_dn / d, -MAX_SPEED, MAX_SPEED);
			sec.across_at[j] = CLAMP(-dpsi_ds / (metric(i, j) * d), -MAX_SPEED, MAX_SPEED);
		}
	}

	// White water per sample. Sources: the local Froude number (so it gathers in
	// the fast core), cascades across the whole river, and the current meeting
	// a rock -- the pillow on its upstream face and the shear lines past its
	// sides (an island's wet neighbours, faster water foaming more). Carried
	// downstream along the local flow, fading at the mean current's pace, so
	// two trails close in behind each rock.
	LocalVector<float> carried;
	carried.resize(row);
	for (int i = 0; i < count; i++) {
		Section &sec = sections[i];
		const float decay = Math::exp(-p_ds / (MAX(sec.speed, 0.1f) * foam_persistence));
		const float dn = dn_of(i);
		for (int j = 0; j <= m; j++) {
			const int k = i * row + j;
			float src = 0.0f;
			if (inv_k[k] > 0.0f) {
				// Supercritical water and cascades.
				const float d = MAX(sec.depth[j], MIN_D);
				const float speed = Math::sqrt(sec.speed_at[j] * sec.speed_at[j] + sec.across_at[j] * sec.across_at[j]);
				const float fr = speed / Math::sqrt(GRAVITY * d);
				float f = CLAMP((fr - 0.8f) / 0.8f, 0.0f, 1.0f);
				src = f * f * (3.0f - 2.0f * f);
				if (sec.cascade) {
					src = 1.0f;
				}
				// A rock next to this sample.
				bool by_rock = false;
				for (int di = -1; di <= 1 && !by_rock; di++) {
					for (int dj = -1; dj <= 1; dj++) {
						const int ii = i + di;
						const int jj = j + dj;
						if (ii < 0 || ii >= count || jj < 0 || jj > m) {
							continue;
						}
						const int c = comp[ii * row + jj];
						if (c >= 0 && comp_island[c]) {
							by_rock = true;
							break;
						}
					}
				}
				if (by_rock) {
					// Lace rather than a sheet: up to 0.75, from 0.5 m/s, full at 2.5 m/s.
					float r = CLAMP((speed - 0.5f) / 2.0f, 0.0f, 1.0f);
					src = MAX(src, 0.75f * r * r * (3.0f - 2.0f * r));
				}
			}
			float up = 0.0f;
			if (i > 0 && inv_k[k] > 0.0f) {
				// Where this sample's water was one step upstream.
				const float along = MAX(sec.speed_at[j], 0.05f);
				const float shift = CLAMP(sec.across_at[j] / along * p_ds / dn, -1.0f, 1.0f);
				const float u = CLAMP((float)j - shift, 0.0f, (float)m);
				const int j0 = MIN((int)u, m - 1);
				up = Math::lerp(carried[j0], carried[j0 + 1], u - j0) * decay;
			}
			sec.foam_at[j] = inv_k[k] > 0.0f ? MAX(src, up) : 0.0f;
		}
		for (int j = 0; j <= m; j++) {
			carried[j] = sec.foam_at[j];
		}
	}
}

void PhysXRiver3D::_build_mesh() {
	if (mesh_instance == nullptr) {
		mesh_instance = memnew(MeshInstance3D);
		mesh_instance->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
		add_child(mesh_instance, false, INTERNAL_MODE_BACK);
	}
	if (water_material.is_null()) {
		Ref<Shader> shader;
		shader.instantiate();
		shader->set_code(RIVER_SHADER_SRC);
		water_material.instantiate();
		water_material->set_shader(shader);
	}
	mesh_instance->set_material_override(water_material);
	const int count = (int)sections.size();
	if (count < 2) {
		mesh_instance->set_mesh(Ref<Mesh>());
		return;
	}
	const int m = cross_segments;
	const int row = m + 1;
	PackedVector3Array verts;
	PackedVector3Array normals;
	PackedVector2Array uvs;
	PackedVector2Array uv2s;
	PackedColorArray colors;
	PackedInt32Array indices;
	verts.resize(count * row);
	normals.resize(count * row);
	uvs.resize(count * row);
	uv2s.resize(count * row);
	colors.resize(count * row);
	for (int i = 0; i < count; i++) {
		const Section &sec = sections[i];
		const float half = 0.5f * sec.width + bank_margin;
		const Vector3 tangent(-sec.side.z, 0.0f, sec.side.x);
		for (int j = 0; j <= m; j++) {
			const int k = i * row + j;
			const float n = -half + 2.0f * half * j / m;
			verts.set(k, sec.centre + sec.side * n);
			uvs.set(k, Vector2(sec.s, n));
			const Vector3 v = tangent * sec.speed_at[j] + sec.side * sec.across_at[j];
			uv2s.set(k, Vector2(v.x, v.z));
			const float foam = sec.foam_at[j];
			const float width_fade = 1.0f - CLAMP((Math::abs(n) - 0.5f * sec.width) / MAX(bank_margin, 1e-3f), 0.0f, 1.0f);
			colors.set(k, Color(CLAMP((sec.depth[j] + 1.0f) * 0.25f, 0.0f, 1.0f), foam, width_fade, 1.0f));
		}
	}
	for (int i = 0; i < count; i++) {
		for (int j = 0; j <= m; j++) {
			const Vector3 a = verts[i * row + MIN(j + 1, m)] - verts[i * row + MAX(j - 1, 0)];
			const Vector3 b = verts[MIN(i + 1, count - 1) * row + j] - verts[MAX(i - 1, 0) * row + j];
			Vector3 nrm = b.cross(a);
			if (nrm.y < 0.0f) {
				nrm = -nrm;
			}
			normals.set(i * row + j, nrm.length_squared() > 1e-12f ? nrm.normalized() : Vector3(0, 1, 0));
		}
	}
	for (int i = 0; i + 1 < count; i++) {
		for (int j = 0; j < m; j++) {
			const int k00 = i * row + j;
			const int k01 = k00 + 1;
			const int k10 = k00 + row;
			const int k11 = k10 + 1;
			// Skip quads that are dry all round (ground well above the water).
			const float dmax = MAX(MAX(sections[i].depth[j], sections[i].depth[j + 1]), MAX(sections[i + 1].depth[j], sections[i + 1].depth[j + 1]));
			if (dmax < -0.05f) {
				continue;
			}
			// Clockwise seen from above: the top is the front face, which the path
			// tracer takes as entering the water (the other way round, every ray
			// from above counted as leaving it and grazing ones were totally reflected).
			indices.push_back(k00);
			indices.push_back(k01);
			indices.push_back(k10);
			indices.push_back(k01);
			indices.push_back(k11);
			indices.push_back(k10);
		}
	}
	if (indices.is_empty()) {
		mesh_instance->set_mesh(Ref<Mesh>());
		return;
	}
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = verts;
	arrays[Mesh::ARRAY_NORMAL] = normals;
	arrays[Mesh::ARRAY_TEX_UV] = uvs;
	arrays[Mesh::ARRAY_TEX_UV2] = uv2s;
	arrays[Mesh::ARRAY_COLOR] = colors;
	arrays[Mesh::ARRAY_INDEX] = indices;
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	mesh_instance->set_mesh(mesh);
}

bool PhysXRiver3D::_locate(const Vector3 &p_world, int &r_section, float &r_t, float &r_n) const {
	const int count = (int)sections.size();
	const Ref<Curve3D> c = get_curve();
	if (count < 2 || c.is_null() || !is_inside_tree()) {
		return false;
	}
	Vector3 local = get_global_transform().affine_inverse().xform(p_world);
	const float ds = sections[1].s - sections[0].s;
	// The nearest point on the river's centre line, in plan.
	const Vector3 flat_query(local.x, 0.0f, local.z);
	float s = c->get_closest_offset(local);
	const float x = s / ds;
	int i = CLAMP((int)Math::floor(x), 0, count - 2);
	float t = CLAMP(x - i, 0.0f, 1.0f);
	const Section &a = sections[i];
	const Section &b = sections[i + 1];
	const Vector3 centre = a.centre.lerp(b.centre, t);
	const Vector3 side = a.side.lerp(b.side, t).normalized();
	const Vector3 rel = flat_query - Vector3(centre.x, 0.0f, centre.z);
	const Vector3 tangent(-side.z, 0.0f, side.x);
	const float along = rel.dot(tangent);
	// Past either end of the river.
	if ((i == 0 && t <= 0.0f && along < -0.5f * ds) || (i == count - 2 && t >= 1.0f && along > 0.5f * ds)) {
		return false;
	}
	const float n = rel.dot(side);
	const float half = 0.5f * Math::lerp(a.width, b.width, t) + bank_margin;
	if (Math::abs(n) > half) {
		return false;
	}
	r_section = i;
	r_t = t;
	r_n = n;
	return true;
}

namespace {
float cross_value(const PhysXRiver3D::Section &p_sec, const LocalVector<float> &p_values, float p_n, float p_margin, int p_m) {
	const float half = 0.5f * p_sec.width + p_margin;
	const float u = CLAMP((p_n + half) / (2.0f * half) * p_m, 0.0f, (float)p_m);
	const int j = MIN((int)u, p_m - 1);
	return Math::lerp(p_values[j], p_values[j + 1], u - j);
}
} // namespace

float PhysXRiver3D::sample_depth(const Vector3 &p_world_pos) const {
	int i;
	float t;
	float n;
	if (!_locate(p_world_pos, i, t, n)) {
		return 0.0f;
	}
	return Math::lerp(cross_value(sections[i], sections[i].depth, n, bank_margin, cross_segments),
			cross_value(sections[i + 1], sections[i + 1].depth, n, bank_margin, cross_segments), t);
}

float PhysXRiver3D::sample_height(const Vector3 &p_world_pos) const {
	int i;
	float t;
	float n;
	if (!_locate(p_world_pos, i, t, n)) {
		return -Math::INF;
	}
	const float d = Math::lerp(cross_value(sections[i], sections[i].depth, n, bank_margin, cross_segments),
			cross_value(sections[i + 1], sections[i + 1].depth, n, bank_margin, cross_segments), t);
	if (d <= 0.0f) {
		return -Math::INF;
	}
	const float local_y = Math::lerp(sections[i].surface, sections[i + 1].surface, t);
	const Transform3D xf = get_global_transform();
	const Vector3 local = xf.affine_inverse().xform(p_world_pos);
	return xf.xform(Vector3(local.x, local_y, local.z)).y;
}

Vector3 PhysXRiver3D::sample_flow(const Vector3 &p_world_pos) const {
	int i;
	float t;
	float n;
	if (!_locate(p_world_pos, i, t, n)) {
		return Vector3();
	}
	const float v = Math::lerp(cross_value(sections[i], sections[i].speed_at, n, bank_margin, cross_segments),
			cross_value(sections[i + 1], sections[i + 1].speed_at, n, bank_margin, cross_segments), t);
	const float a = Math::lerp(cross_value(sections[i], sections[i].across_at, n, bank_margin, cross_segments),
			cross_value(sections[i + 1], sections[i + 1].across_at, n, bank_margin, cross_segments), t);
	const Vector3 side = sections[i].side.lerp(sections[i + 1].side, t).normalized();
	const Vector3 tangent(-side.z, 0.0f, side.x);
	return get_global_transform().basis.xform(tangent * v + side * a);
}

float PhysXRiver3D::sample_foam(const Vector3 &p_world_pos) const {
	int i;
	float t;
	float n;
	if (!_locate(p_world_pos, i, t, n)) {
		return 0.0f;
	}
	return Math::lerp(cross_value(sections[i], sections[i].foam_at, n, bank_margin, cross_segments),
			cross_value(sections[i + 1], sections[i + 1].foam_at, n, bank_margin, cross_segments), t);
}

float PhysXRiver3D::get_length() const {
	return sections.is_empty() ? 0.0f : sections[sections.size() - 1].s;
}

Dictionary PhysXRiver3D::get_section(int p_index) const {
	Dictionary d;
	ERR_FAIL_INDEX_V(p_index, (int)sections.size(), d);
	const Section &sec = sections[p_index];
	const Transform3D xf = is_inside_tree() ? get_global_transform() : Transform3D();
	d["centre"] = xf.xform(sec.centre);
	d["s"] = sec.s;
	d["width"] = sec.width;
	d["surface"] = xf.xform(sec.centre).y;
	d["slope"] = sec.slope;
	d["speed"] = sec.speed;
	d["foam"] = sec.foam;
	d["cascade"] = sec.cascade;
	PackedFloat32Array depth;
	PackedFloat32Array speed;
	PackedFloat32Array across;
	PackedFloat32Array foam;
	for (uint32_t j = 0; j < sec.depth.size(); j++) {
		depth.push_back(sec.depth[j]);
		speed.push_back(sec.speed_at[j]);
		across.push_back(sec.across_at[j]);
		foam.push_back(sec.foam_at[j]);
	}
	d["depths"] = depth;
	d["speeds"] = speed;
	d["across"] = across;
	d["foams"] = foam;
	return d;
}

Array PhysXRiver3D::get_carve_brushes(float p_tile_size) const {
	Array brushes;
	const int count = (int)sections.size();
	const Ref<Curve3D> c = get_curve();
	if (!carve || count < 2 || c.is_null() || !is_inside_tree()) {
		return brushes;
	}
	const float tile = MAX(p_tile_size, 1.0f);
	const float cell = carve_cell_size;
	const Transform3D xf = get_global_transform();
	const Transform3D inv = xf.affine_inverse();
	const float ds = sections[1].s - sections[0].s;
	// Tiles the channel and its fade touch.
	HashSet<Vector2i> tiles;
	for (int i = 0; i < count; i++) {
		const Section &sec = sections[i];
		const float reach = 0.5f * sec.width + carve_falloff;
		for (int e = -2; e <= 2; e++) {
			const Vector3 w = xf.xform(sec.centre + sec.side * (reach * e * 0.5f));
			tiles.insert(Vector2i((int)Math::floor(w.x / tile), (int)Math::floor(w.z / tile)));
		}
	}
	const int res = (int)Math::ceil(tile / cell);
	for (const Vector2i &t : tiles) {
		const Vector2 origin(t.x * tile, t.y * tile);
		PackedFloat32Array heights;
		PackedFloat32Array mask;
		heights.resize(res * res);
		mask.resize(res * res);
		float *hw = heights.ptrw();
		float *mw = mask.ptrw();
		float max_mask = 0.0f;
		for (int z = 0; z < res; z++) {
			for (int x = 0; x < res; x++) {
				const int k = z * res + x;
				hw[k] = 0.0f;
				mw[k] = 0.0f;
				const Vector3 world(origin.x + (x + 0.5f) * cell, xf.origin.y, origin.y + (z + 0.5f) * cell);
				Vector3 local = inv.xform(world);
				const float sq = c->get_closest_offset(local) / ds;
				const int i = CLAMP((int)Math::floor(sq), 0, count - 2);
				const float tt = CLAMP(sq - i, 0.0f, 1.0f);
				const Section &a = sections[i];
				const Section &b = sections[i + 1];
				const Vector3 centre = a.centre.lerp(b.centre, tt);
				const Vector3 side = a.side.lerp(b.side, tt).normalized();
				const Vector3 rel(local.x - centre.x, 0.0f, local.z - centre.z);
				const Vector3 tangent(-side.z, 0.0f, side.x);
				const float along = rel.dot(tangent);
				if ((i == 0 && tt <= 0.0f && along < -0.5f * ds) || (i == count - 2 && tt >= 1.0f && along > 0.5f * ds)) {
					continue; // past either end
				}
				const float n = rel.dot(side);
				float ma;
				float mb;
				const float ya = _carve_target(a, n, ma);
				const float yb = _carve_target(b, n, mb);
				const float m_here = Math::lerp(ma, mb, tt);
				if (m_here <= 0.0f) {
					continue;
				}
				local.y = Math::lerp(ya, yb, tt);
				hw[k] = xf.xform(local).y;
				mw[k] = m_here;
				max_mask = MAX(max_mask, m_here);
			}
		}
		if (max_mask <= 0.0f) {
			continue;
		}
		Dictionary brush;
		brush["rect"] = Rect2(origin, Vector2(res * cell, res * cell));
		brush["cell_size"] = cell;
		brush["size"] = Vector2i(res, res);
		brush["heights"] = heights;
		brush["mask"] = mask;
		brush["mode"] = "lower";
		brushes.push_back(brush);
	}
	return brushes;
}

PackedStringArray PhysXRiver3D::get_configuration_warnings() const {
	PackedStringArray warnings = Path3D::get_configuration_warnings();
	if (get_curve().is_null() || get_curve()->get_point_count() < 2) {
		warnings.push_back(RTR("Draw the river's path: it needs at least two curve points, from source to mouth."));
	}
	return warnings;
}

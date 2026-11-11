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

#include "flow_poisson.h"
#include "physx_water_mist_3d.h"
#include "physx_water_surface_3d.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/templates/hash_set.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/gpu_particles_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/main/viewport.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/3d/world_3d.h"
#include "scene/resources/mesh.h"
#include "scene/resources/particle_process_material.h"
#include "scene/resources/shader.h"
#include "scene/resources/texture_rd.h"
#include "servers/physics_3d/physics_server_3d.h"

#ifdef GODOT_PHYSX_FLOW
#include "../flow/physx_flow_emitter_3d.h"
#endif

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
render_mode blend_mix, cull_disabled;
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
// A living waterline: the edge laps in and out by up to this much water depth
// (m) -- small wavelets and the current's swirls arriving at the bank, so on a
// gentle bank the line wanders by decimetres and is never straight -- with a
// lace of foam caught along it.
uniform float edge_lap_height : hint_range(0.0, 0.2, 0.005) = 0.035;
uniform float edge_lap_scale = 0.8;
uniform float edge_foam : hint_range(0.0, 1.0, 0.01) = 0.35;
// The bank's own small relief the ground mesh is too coarse to carry (pebbles,
// ruts, tufts): up to this much depth (m), fixed to the ground, so the line
// runs in and out of little bays and points instead of following the mesh.
uniform float edge_relief : hint_range(0.0, 0.3, 0.005) = 0.05;
// The live window (set by PhysXRiver3D while live_flow runs): a square of
// shallow-water simulation round the camera. Height is the disturbance on the
// river's own surface; flow is the total velocity (rg, world xz, m/s) with the
// white water it made in b.
uniform bool live_enabled = false;
uniform vec2 live_center = vec2(0.0);
uniform vec2 live_size = vec2(48.0);
uniform vec2 live_texel = vec2(0.25);
// The window's mean plane (h0, dh/dx, dh/dz about live_center), taken out.
uniform vec3 live_tilt = vec3(0.0);
uniform sampler2D live_height_tex : hint_default_black, filter_linear, repeat_disable;
uniform sampler2D live_flow_tex : hint_default_black, filter_linear, repeat_disable;
// Raster reflections from above (water_raster_reflections.h).
)" WATER_RASTER_REFLECTIONS_GLSL R"(
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

// How far inside the live window a world point is: 1 inside, easing to 0 over
// its outer edge; and its texture coordinate.
float live_weight(vec2 world_xz, out vec2 cuv) {
	vec2 rel = (world_xz - live_center) / live_size;
	cuv = clamp(rel + 0.5, vec2(0.0), vec2(1.0));
	vec2 a = abs(rel);
	return live_enabled ? 1.0 - smoothstep(0.44, 0.5, max(a.x, a.y)) : 0.0;
}

void vertex() {
	v_world = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	v_flow = (MODEL_MATRIX * vec4(UV2.x, 0.0, UV2.y, 0.0)).xz;
	v_info = COLOR;
	v_normal = normalize((MODEL_MATRIX * vec4(NORMAL, 0.0)).xyz);
	vec2 cuv;
	float live = live_weight(v_world.xz, cuv);
	if (live > 0.0) {
		// The live water's rise and fall on the river's surface.
		vec2 off = v_world.xz - live_center;
		float h = (textureLod(live_height_tex, cuv, 0.0).r - (live_tilt.x + dot(live_tilt.yz, off))) * live * step(0.0, v_info.r * 4.0 - 1.0);
		VERTEX.y += h;
		v_world.y += h;
	}
}

void fragment() {
	vec2 flow = v_flow;
	float live_foam = 0.0;
	vec2 live_slope = vec2(0.0);
	vec2 cuv;
	float live = live_weight(v_world.xz, cuv);
	if (live > 0.0) {
		vec4 lf = texture(live_flow_tex, cuv);
		flow = mix(flow, lf.rg, live);
		live_foam = lf.b * live;
		vec2 du = live_texel / live_size;
		float hx = texture(live_height_tex, cuv + vec2(du.x, 0.0)).r - texture(live_height_tex, cuv - vec2(du.x, 0.0)).r;
		float hz = texture(live_height_tex, cuv + vec2(0.0, du.y)).r - texture(live_height_tex, cuv - vec2(0.0, du.y)).r;
		live_slope = (vec2(hx, hz) / (2.0 * live_texel) - live_tilt.yz) * live;
	}
	vec2 p = v_world.xz * ripple_scale;
	vec2 o_a;
	vec2 o_b;
	float w = flow_phases(v_world.xz, flow, o_a, o_b);
	// Surface-gradient blend of the two phases, renormalised so the crossover
	// doesn't flatten the ripples (phase B is shifted half a tile, so the two are
	// close to independent).
	vec2 slope = (ripple_slope(p - o_a * ripple_scale) * w + ripple_slope(p - o_b * ripple_scale + vec2(13.1, 7.7)) * (1.0 - w))
			/ sqrt(w * w + (1.0 - w) * (1.0 - w));
	float speed = length(flow);
	float depth = v_info.r * 4.0 - 1.0;
	// The waterline laps: carried by the current (the same two phases as the
	// ripples) and breathing in and out over a few seconds.
	vec2 e_a = (v_world.xz - o_a) * edge_lap_scale;
	vec2 e_b = (v_world.xz - o_b) * edge_lap_scale + vec2(3.7, 9.1);
	float lap_n = (0.6 * r_noise(e_a) + 0.4 * r_noise(e_a * 2.9 + 4.0)) * w + (0.6 * r_noise(e_b) + 0.4 * r_noise(e_b * 2.9 + 4.0)) * (1.0 - w);
	float breathe = 0.5 + 0.5 * sin(water_time * 1.3 + r_noise(v_world.xz * 0.15) * 6.28318);
	depth += edge_lap_height * ((lap_n - 0.5) * 2.0 + (breathe - 0.5));
	vec2 rq = v_world.xz * 0.35;
	float relief = 0.5 * r_noise(rq) + 0.3 * r_noise(rq * 3.1 + 17.0) + 0.2 * r_noise(rq * 8.3 - 5.0);
	depth += edge_relief * (relief - 0.5) * 2.0;
	// Calm at the waterline: ripples die out over the last 15 cm.
	slope *= (ripple_strength + ripple_speed_strength * speed) * smoothstep(0.0, 0.15, depth);
	vec3 n_mesh = normalize(v_normal);
	if (n_mesh.y < 0.0) {
		n_mesh = -n_mesh;
	}
	slope += live_slope * smoothstep(0.0, 0.15, depth);
	vec3 world_normal = normalize(n_mesh - vec3(slope.x, 0.0, slope.y));

	// White water, broken into lace that rides the current.
	vec2 q_a = (v_world.xz - o_a) * foam_detail_scale;
	vec2 q_b = (v_world.xz - o_b) * foam_detail_scale + vec2(5.3, 2.1);
	float lace_a = 0.5 * r_noise(q_a) + 0.3 * r_noise(q_a * 2.7 + 17.0) + 0.2 * r_noise(q_a * 7.3 - 5.0);
	float lace_b = 0.5 * r_noise(q_b) + 0.3 * r_noise(q_b * 2.7 + 17.0) + 0.2 * r_noise(q_b * 7.3 - 5.0);
	float lace = lace_a * w + lace_b * (1.0 - w);
	// Foam caught along the waterline, broken into lace.
	float edge_band = edge_foam * (1.0 - smoothstep(0.0, 0.12, depth)) * smoothstep(-0.01, 0.02, depth);
	float foam = clamp(max(max(v_info.g, live_foam), edge_band) * (1.0 + foam_breakup) - lace * foam_breakup, 0.0, 1.0);
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
	// Open water keeps PhysXWaterSurface3D's raster look: its own colour with a
	// small bounded shading term, plus the sky and the sun's glitter, written to
	// EMISSION (no ALBEDO, no SPECULAR, so the lights add nothing to it). White
	// water is a rough diffuse surface: it goes to ALBEDO and takes the scene's
	// lights and shadows, so a cascade in a shaded valley no longer glows (the
	// path tracer already lit it; docs/ideas/water.md "Step 4f").
	vec3 n = normalize((VIEW_MATRIX * vec4(world_normal, 0.0)).xyz);
	float ndotl = clamp(dot(n, vec3(0.3, 0.85, 0.4)), 0.0, 1.0);
	float deep = smoothstep(0.0, 1.2, depth);
	vec3 base_rgb = mix(shallow_color.rgb, water_color.rgb, deep) * mix(0.75, 1.15, ndotl) * water_raster_body_light();
	float base_a = mix(shallow_color.a, water_color.a, deep);
	float edge = smoothstep(0.0, max(edge_softness, 1e-3), depth) * v_info.b;
	float a0 = mix(base_a, foam_color.a, foam) * edge;
	// The reflection term is linear in the colour, so the water's part and the
	// foam's part split exactly: what was one unshaded colour is now emission
	// plus a lit albedo, the same where the light is the old fixed one.
	vec4 refl = water_raster_reflect(base_rgb * (1.0 - foam), a0, world_normal, v_world, CAMERA_POSITION_WORLD, edge * (1.0 - foam), pathtraced_roughness);
	float a_new = max(refl.a, 1e-4);
	// Light the surface let through (1 - F), recovered from the alpha it returned.
	float through = a0 < 0.9999 ? clamp((1.0 - refl.a) / (1.0 - a0), 0.0, 1.0) : 1.0;
	float keep = a0 * through / a_new;
	NORMAL = n;
	EMISSION = refl.rgb;
	ALBEDO = foam_color.rgb * foam * keep;
	ROUGHNESS = 0.85;
	SPECULAR = 0.25 * foam;
	METALLIC = 0.0;
	ALPHA = refl.a;
#endif
}
)";

// A waterfall's falling sheet: a thin slab of water (two faces q/|v| apart, so
// the path tracer refracts through a real sheet). Per vertex (built by
// PhysXRiver3D): UV = (travel time since the lip in s, distance across in m),
// UV2.x = speed (m/s), COLOR.r = fade at the sheet's edges, COLOR.g =
// aeration (white water), COLOR.b = how far past its break-up length the
// sheet has fallen (0 whole, 1 broken into clumps).
const char *FALL_SHADER_SRC = R"(
shader_type spatial;
#if defined(RT)
render_mode cull_back;
#else
render_mode blend_mix, cull_back;
#endif

uniform float water_time = 0.0;
uniform vec4 water_color : source_color = vec4(0.30, 0.42, 0.42, 0.45);
uniform vec4 foam_color : source_color = vec4(0.92, 0.95, 0.97, 0.95);
uniform vec3 pathtraced_absorption = vec3(0.6, 0.14, 0.1);
uniform float pathtraced_roughness : hint_range(0.0, 1.0) = 0.05;
// Streaks: per metre across, and per second of travel along (the pattern rides
// the water, so it stretches as the sheet speeds up).
uniform float streak_scale = 3.0;
uniform float streak_rate = 5.0;

float f_hash(vec2 p) {
	vec3 p3 = fract(vec3(p.xyx) * 0.1031);
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.x + p3.y) * p3.z);
}

float f_noise(vec2 p) {
	vec2 i = floor(p);
	vec2 f = fract(p);
	vec2 u = f * f * (3.0 - 2.0 * f);
	return mix(mix(f_hash(i), f_hash(i + vec2(1.0, 0.0)), u.x),
			mix(f_hash(i + vec2(0.0, 1.0)), f_hash(i + vec2(1.0, 1.0)), u.x), u.y);
}

void fragment() {
	float edge = COLOR.r;
	float aeration = COLOR.g;
	float broken = COLOR.b;
	vec2 p = vec2(UV.y * streak_scale, (UV.x - water_time) * streak_rate);
	float streak = 0.55 * f_noise(p * vec2(1.0, 0.35)) + 0.3 * f_noise(p * vec2(2.3, 0.9) + 7.0) + 0.15 * f_noise(p * vec2(5.1, 2.4) - 3.0);
	float foam = smoothstep(0.15, 0.85, aeration + (streak - 0.5) * 0.9);
	// Past break-up the sheet is clumps and streaks with air between them; the
	// thin edges go first.
	float holes = broken * 0.8 + (1.0 - edge) * 0.6;
	float cover = smoothstep(-0.04, 0.04, streak - holes + 0.12);
#if defined(RT)
	ALBEDO = mix(vec3(1.0), foam_color.rgb, foam);
	ROUGHNESS = mix(pathtraced_roughness, 0.8, foam);
	SPECULAR = 0.5;
	METALLIC = 0.0;
	TRANSMISSION = 1.0 - foam;
	IOR = 1.33;
	ABSORPTION = pathtraced_absorption;
	ALPHA = cover;
	ALPHA_SCISSOR_THRESHOLD = 0.5;
#else
	// Lit by the scene's lights, shadows and sky like any surface (it was unshaded with a fixed light,
	// so a fall in a shaded valley glowed flat white): white water scatters diffusely, the clear sheet
	// is a smooth dielectric showing the sky in its specular.
	ALBEDO = mix(water_color.rgb, foam_color.rgb, foam);
	ROUGHNESS = mix(pathtraced_roughness + 0.1, 0.85, foam);
	SPECULAR = 0.5;
	METALLIC = 0.0;
	ALPHA = mix(water_color.a, foam_color.a, foam) * cover;
#endif
}
)";

constexpr float GRAVITY = 9.81f;
constexpr float MAX_SPEED = 12.0f; // m/s cap (a fall's sheet, not a sustained river)
constexpr float CASCADE_MIN_DROP = 0.15f; // m

} // namespace

String PhysXRiver3D::get_fall_shader_code() {
	return String(FALL_SHADER_SRC);
}

namespace {
float cross_value(const PhysXRiver3D::Section &p_sec, const LocalVector<float> &p_values, float p_n, float p_margin, int p_m) {
	const float half = 0.5f * p_sec.width + p_margin;
	const float u = CLAMP((p_n + half) / (2.0f * half) * p_m, 0.0f, (float)p_m);
	const int j = MIN((int)u, p_m - 1);
	return Math::lerp(p_values[j], p_values[j + 1], u - j);
}
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
	ClassDB::bind_method(D_METHOD("set_mouth_target", "path"), &PhysXRiver3D::set_mouth_target);
	ClassDB::bind_method(D_METHOD("get_mouth_target"), &PhysXRiver3D::get_mouth_target);
	ClassDB::bind_method(D_METHOD("set_fall_min_height", "height"), &PhysXRiver3D::set_fall_min_height);
	ClassDB::bind_method(D_METHOD("get_fall_min_height"), &PhysXRiver3D::get_fall_min_height);
	ClassDB::bind_method(D_METHOD("set_fall_spray", "enabled"), &PhysXRiver3D::set_fall_spray);
	ClassDB::bind_method(D_METHOD("get_fall_spray"), &PhysXRiver3D::get_fall_spray);
	ClassDB::bind_method(D_METHOD("set_fall_mist", "enabled"), &PhysXRiver3D::set_fall_mist);
	ClassDB::bind_method(D_METHOD("get_fall_mist"), &PhysXRiver3D::get_fall_mist);
	ClassDB::bind_method(D_METHOD("set_fall_mist_amount", "amount"), &PhysXRiver3D::set_fall_mist_amount);
	ClassDB::bind_method(D_METHOD("get_fall_mist_amount"), &PhysXRiver3D::get_fall_mist_amount);
	ClassDB::bind_method(D_METHOD("set_fall_material", "material"), &PhysXRiver3D::set_fall_material);
	ClassDB::bind_method(D_METHOD("get_fall_material"), &PhysXRiver3D::get_fall_material);
	ClassDB::bind_method(D_METHOD("get_waterfalls"), &PhysXRiver3D::get_waterfalls);
	ClassDB::bind_method(D_METHOD("set_mouth_blend", "blend"), &PhysXRiver3D::set_mouth_blend);
	ClassDB::bind_method(D_METHOD("get_mouth_blend"), &PhysXRiver3D::get_mouth_blend);
	ClassDB::bind_method(D_METHOD("get_discharge_at", "s"), &PhysXRiver3D::get_discharge_at);
	ClassDB::bind_method(D_METHOD("set_raster_reflections", "enabled"), &PhysXRiver3D::set_raster_reflections);
	ClassDB::bind_method(D_METHOD("get_raster_reflections"), &PhysXRiver3D::get_raster_reflections);
	ClassDB::bind_method(D_METHOD("set_live_flow", "enabled"), &PhysXRiver3D::set_live_flow);
	ClassDB::bind_method(D_METHOD("get_live_flow"), &PhysXRiver3D::get_live_flow);
	ClassDB::bind_method(D_METHOD("set_live_domain_size", "size"), &PhysXRiver3D::set_live_domain_size);
	ClassDB::bind_method(D_METHOD("get_live_domain_size"), &PhysXRiver3D::get_live_domain_size);
	ClassDB::bind_method(D_METHOD("set_live_grid_resolution", "resolution"), &PhysXRiver3D::set_live_grid_resolution);
	ClassDB::bind_method(D_METHOD("get_live_grid_resolution"), &PhysXRiver3D::get_live_grid_resolution);
	ClassDB::bind_method(D_METHOD("set_wall_friction", "friction"), &PhysXRiver3D::set_wall_friction);
	ClassDB::bind_method(D_METHOD("get_wall_friction"), &PhysXRiver3D::get_wall_friction);
	ClassDB::bind_method(D_METHOD("set_flow_foam", "strength"), &PhysXRiver3D::set_flow_foam);
	ClassDB::bind_method(D_METHOD("get_flow_foam"), &PhysXRiver3D::get_flow_foam);
	ClassDB::bind_method(D_METHOD("set_flow_foam_persistence", "seconds"), &PhysXRiver3D::set_flow_foam_persistence);
	ClassDB::bind_method(D_METHOD("get_flow_foam_persistence"), &PhysXRiver3D::get_flow_foam_persistence);
	ClassDB::bind_method(D_METHOD("set_live_damping", "damping"), &PhysXRiver3D::set_live_damping);
	ClassDB::bind_method(D_METHOD("get_live_damping"), &PhysXRiver3D::get_live_damping);
	ClassDB::bind_method(D_METHOD("set_live_follow", "path"), &PhysXRiver3D::set_live_follow);
	ClassDB::bind_method(D_METHOD("get_live_follow"), &PhysXRiver3D::get_live_follow);
	ClassDB::bind_method(D_METHOD("get_live_center"), &PhysXRiver3D::get_live_center);
	ClassDB::bind_method(D_METHOD("is_live_running"), &PhysXRiver3D::is_live_running);
	ClassDB::bind_method(D_METHOD("submit_sphere", "owner", "world_pos", "radius", "strength"), &PhysXRiver3D::submit_sphere, DEFVAL(1.0f));
	ClassDB::bind_method(D_METHOD("clear_sphere", "owner"), &PhysXRiver3D::clear_sphere);
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
	ADD_GROUP("Mouth", "mouth_");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "mouth_target", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "PhysXWaterSurface3D,PhysXRiver3D"), "set_mouth_target", "get_mouth_target");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "mouth_blend", PROPERTY_HINT_RANGE, "0,50,0.1,or_greater,suffix:m"), "set_mouth_blend", "get_mouth_blend");
	ADD_GROUP("Waterfalls", "fall_");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "fall_min_height", PROPERTY_HINT_RANGE, "0,20,0.1,or_greater,suffix:m"), "set_fall_min_height", "get_fall_min_height");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "fall_spray"), "set_fall_spray", "get_fall_spray");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "fall_mist"), "set_fall_mist", "get_fall_mist");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "fall_mist_amount", PROPERTY_HINT_RANGE, "0,10,0.05,or_greater"), "set_fall_mist_amount", "get_fall_mist_amount");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "fall_material", PROPERTY_HINT_RESOURCE_TYPE, "ShaderMaterial"), "set_fall_material", "get_fall_material");
	ADD_SIGNAL(MethodInfo("rebuilt"));
	ADD_GROUP("Live flow", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "live_flow"), "set_live_flow", "get_live_flow");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "raster_reflections"), "set_raster_reflections", "get_raster_reflections");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "live_domain_size", PROPERTY_HINT_RANGE, "8,256,1,or_greater,suffix:m"), "set_live_domain_size", "get_live_domain_size");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "live_grid_resolution", PROPERTY_HINT_RANGE, "32,512,1"), "set_live_grid_resolution", "get_live_grid_resolution");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wall_friction", PROPERTY_HINT_RANGE, "0,10,0.05,suffix:1/s"), "set_wall_friction", "get_wall_friction");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "flow_foam", PROPERTY_HINT_RANGE, "0,4,0.05"), "set_flow_foam", "get_flow_foam");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "flow_foam_persistence", PROPERTY_HINT_RANGE, "0.1,60,0.1,suffix:s"), "set_flow_foam_persistence", "get_flow_foam_persistence");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "live_damping", PROPERTY_HINT_RANGE, "0,1,0.005"), "set_live_damping", "get_live_damping");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "live_follow", PROPERTY_HINT_NODE_TYPE, "Node3D"), "set_live_follow", "get_live_follow");
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

Node *PhysXRiver3D::_mouth_node() const {
	if (mouth_target.is_empty() || !is_inside_tree()) {
		return nullptr;
	}
	Node *n = get_node_or_null(mouth_target);
	return n == this ? nullptr : n;
}

void PhysXRiver3D::set_mouth_target(const NodePath &p_path) {
	_unregister_mouth();
	mouth_target = p_path;
	_queue_build();
}

void PhysXRiver3D::set_mouth_blend(float p_blend) {
	mouth_blend = MAX(p_blend, 0.0f);
	_queue_build();
}

float PhysXRiver3D::get_discharge_at(float p_s) const {
	float q = discharge;
	for (const Tributary &t : tributaries) {
		if (p_s >= t.s) {
			q += t.discharge;
		}
	}
	return q;
}

void PhysXRiver3D::set_tributary(ObjectID p_river, const Vector3 &p_world, float p_discharge) {
	for (Tributary &e : tributaries) {
		if (e.river == p_river) {
			if (e.at.distance_to(p_world) < 0.5f && Math::abs(e.discharge - p_discharge) < 1e-3f) {
				return; // unchanged: no rebuild (the two rivers would keep waking each other)
			}
			e.at = p_world;
			e.discharge = p_discharge;
			_queue_build();
			return;
		}
	}
	Tributary trib;
	trib.river = p_river;
	trib.at = p_world;
	trib.discharge = p_discharge;
	tributaries.push_back(trib);
	_queue_build();
}

void PhysXRiver3D::clear_tributary(ObjectID p_river) {
	for (uint32_t k = 0; k < tributaries.size(); k++) {
		if (tributaries[k].river == p_river) {
			tributaries.remove_at(k);
			_queue_build();
			return;
		}
	}
}

// Tell the water this river flows into: a river adds the discharge downstream
// of the join; a lake or the sea gets the outflow as a plume (a turbulent
// jet spreading from the mouth).
void PhysXRiver3D::_register_mouth() {
	Node *target = _mouth_node();
	if (target == nullptr || sections.size() < 2) {
		_unregister_mouth();
		return;
	}
	if (mouth_id.is_valid() && mouth_id != target->get_instance_id()) {
		_unregister_mouth();
	}
	const Transform3D xf = get_global_transform();
	int mouth = -1;
	for (uint32_t i = 1; i < sections.size(); i++) {
		if (sections[i].mouth_fade < 1.0f) {
			mouth = (int)i - 1;
			break;
		}
	}
	if (mouth < 0) {
		mouth = (int)sections.size() - 1;
	}
	const Section &sec = sections[mouth];
	const Vector3 at = xf.xform(sec.centre);
	const Vector3 dir = xf.basis.xform(Vector3(-sec.side.z, 0.0f, sec.side.x)).normalized();
	if (PhysXRiver3D *main = Object::cast_to<PhysXRiver3D>(target)) {
		if (!main->is_connected(SNAME("rebuilt"), callable_mp(this, &PhysXRiver3D::_on_target_rebuilt))) {
			main->connect(SNAME("rebuilt"), callable_mp(this, &PhysXRiver3D::_on_target_rebuilt));
		}
		main->set_tributary(get_instance_id(), at, sec.discharge);
	} else if (PhysXWaterSurface3D *sea = Object::cast_to<PhysXWaterSurface3D>(target)) {
		sea->set_inflow((int64_t)(uint64_t)get_instance_id(), at, dir, sec.width, sec.discharge);
	}
	mouth_id = target->get_instance_id();
}

void PhysXRiver3D::_unregister_mouth() {
	if (!mouth_id.is_valid()) {
		return;
	}
	Object *o = ObjectDB::get_instance(mouth_id);
	if (PhysXRiver3D *main = Object::cast_to<PhysXRiver3D>(o)) {
		if (main->is_connected(SNAME("rebuilt"), callable_mp(this, &PhysXRiver3D::_on_target_rebuilt))) {
			main->disconnect(SNAME("rebuilt"), callable_mp(this, &PhysXRiver3D::_on_target_rebuilt));
		}
		main->clear_tributary(get_instance_id());
	} else if (PhysXWaterSurface3D *sea = Object::cast_to<PhysXWaterSurface3D>(o)) {
		sea->clear_inflow((int64_t)(uint64_t)get_instance_id());
	}
	mouth_id = ObjectID();
}

// The river this one flows into was rebuilt: follow its surface at the join
// if it moved (more than a centimetre, so the two settle instead of waking
// each other for ever).
void PhysXRiver3D::_on_target_rebuilt() {
	PhysXRiver3D *main = Object::cast_to<PhysXRiver3D>(_mouth_node());
	if (main == nullptr || sections.size() < 2) {
		return;
	}
	const Transform3D xf = get_global_transform();
	for (uint32_t i = 0; i < sections.size(); i++) {
		const Vector3 w = xf.xform(sections[i].centre);
		if (main->sample_depth(w) > 0.0f) {
			const float level = main->sample_height(w);
			if (!Math::is_finite(mouth_level_seen) || Math::abs(level - mouth_level_seen) > 0.01f) {
				_queue_build();
			}
			return;
		}
	}
}

void PhysXRiver3D::set_live_flow(bool p_enabled) {
	live_flow = p_enabled;
	if (!live_flow) {
		_live_stop();
	}
}

void PhysXRiver3D::set_live_domain_size(float p_size) {
	live_domain_size = MAX(p_size, 4.0f);
	_live_stop();
}

void PhysXRiver3D::set_live_grid_resolution(int p_n) {
	live_grid_resolution = CLAMP(p_n, 16, 1024);
	_live_stop();
}

void PhysXRiver3D::set_live_damping(float p_damping) {
	live_damping = MAX(p_damping, 0.0f);
	_live_stop();
}

void PhysXRiver3D::set_wall_friction(float p_friction) {
	wall_friction = MAX(p_friction, 0.0f);
	live_fields_dirty = true;
}

void PhysXRiver3D::set_flow_foam(float p_strength) {
	flow_foam = MAX(p_strength, 0.0f);
	if (live_configured) {
		live_solver.set_flow_foam(flow_foam, flow_foam_persistence);
	}
}

void PhysXRiver3D::set_flow_foam_persistence(float p_seconds) {
	flow_foam_persistence = MAX(p_seconds, 0.05f);
	if (live_configured) {
		live_solver.set_flow_foam(flow_foam, flow_foam_persistence);
	}
}

void PhysXRiver3D::set_live_follow(const NodePath &p_path) {
	live_follow = p_path;
}

void PhysXRiver3D::submit_sphere(int p_owner, const Vector3 &p_world_pos, float p_radius, float p_strength) {
	if (live_configured) {
		live_solver.submit_sphere((uint64_t)p_owner, p_world_pos, p_radius, p_strength);
	}
}

void PhysXRiver3D::clear_sphere(int p_owner) {
	if (live_configured) {
		live_solver.clear_sphere((uint64_t)p_owner);
	}
}

// Strip coordinates of a world point: section, fraction to the next, distance
// across (local m, + towards the left bank). Unlike _locate(), never refuses a
// point past the banks or the ends (it clamps along the river); returns
// whether the point is on the river.
bool PhysXRiver3D::_strip_at(const Vector3 &p_world, int &r_section, float &r_t, float &r_n) const {
	const int count = (int)sections.size();
	const Ref<Curve3D> c = get_curve();
	if (count < 2 || c.is_null() || !is_inside_tree()) {
		return false;
	}
	const Vector3 local = get_global_transform().affine_inverse().xform(p_world);
	const float ds = sections[1].s - sections[0].s;
	const float s = c->get_closest_offset(local);
	const float x = s / ds;
	const int i = CLAMP((int)Math::floor(x), 0, count - 2);
	const float t = CLAMP(x - i, 0.0f, 1.0f);
	const Section &a = sections[i];
	const Section &b = sections[i + 1];
	const Vector3 centre = a.centre.lerp(b.centre, t);
	const Vector3 side = a.side.lerp(b.side, t).normalized();
	const Vector3 rel = Vector3(local.x - centre.x, 0.0f, local.z - centre.z);
	const Vector3 tangent(-side.z, 0.0f, side.x);
	const float along = rel.dot(tangent);
	r_section = i;
	r_t = t;
	r_n = rel.dot(side);
	const float half = 0.5f * Math::lerp(a.width, b.width, t) + bank_margin;
	const bool past_end = (i == 0 && t <= 0.0f && along < -0.5f * ds) || (i == count - 2 && t >= 1.0f && along > 0.5f * ds);
	return !past_end && Math::abs(r_n) <= half;
}

// The river's stream function at a world point, for the live window's face
// fluxes (the bank's value past the banks), and the water depth there
// (WaterSolver::WALL_DEPTH off the river).
float PhysXRiver3D::_psi_at(const Vector3 &p_world, float &r_depth) const {
	int i;
	float t;
	float n;
	r_depth = WaterSolver::WALL_DEPTH;
	if (sections.size() < 2) {
		return 0.0f;
	}
	const bool on = _strip_at(p_world, i, t, n);
	const int m = cross_segments;
	auto value = [&](const Section &p_sec, const LocalVector<float> &p_values) -> float {
		return cross_value(p_sec, p_values, n, bank_margin, m);
	};
	if (sections[i].psi_at.size() != (uint32_t)(m + 1) || sections[i + 1].psi_at.size() != (uint32_t)(m + 1)) {
		return 0.0f;
	}
	if (on) {
		const float d = Math::lerp(value(sections[i], sections[i].depth), value(sections[i + 1], sections[i + 1].depth), t);
		r_depth = d > 0.0f ? d : 0.0f;
	}
	return Math::lerp(value(sections[i], sections[i].psi_at), value(sections[i + 1], sections[i + 1].psi_at), t);
}

// The live window's still-water depth per cell and baked face fluxes (m^2/s:
// +x, +z, -x, -z per cell), from the river's stream function at the cell
// corners, as PhysXWaterSurface3D does from its own bake. The ocean's
// convention is u = (dpsi/dz, -dpsi/dx) / h; the river's psi grows towards the
// left bank, which is the opposite turn, so it goes in negated.
void PhysXRiver3D::_live_fields(PackedFloat32Array &r_depth, PackedFloat32Array &r_flux) const {
	const int n = live_grid_resolution;
	const float cell = live_domain_size / n;
	const float y = get_global_position().y;
	r_depth.resize(n * n);
	LocalVector<float> psi;
	psi.resize(n * n);
	float *dw = r_depth.ptrw();
	for (int z = 0; z < n; z++) {
		for (int x = 0; x < n; x++) {
			const Vector3 w(live_center.x + (x + 0.5f - 0.5f * n) * cell, y, live_center.y + (z + 0.5f - 0.5f * n) * cell);
			float d = 0.0f;
			psi[z * n + x] = -_psi_at(w, d);
			dw[z * n + x] = d;
		}
	}
	LocalVector<float> corner;
	corner.resize((n + 1) * (n + 1));
	for (int j = 0; j <= n; j++) {
		for (int i = 0; i <= n; i++) {
			float sum = 0.0f;
			float dry_psi = 0.0f;
			bool dry = false;
			for (int k = 0; k < 4; k++) {
				const int cx = CLAMP(i - 1 + (k & 1), 0, n - 1);
				const int cz = CLAMP(j - 1 + (k >> 1), 0, n - 1);
				const int c = cz * n + cx;
				if (dw[c] <= 0.0f && !dry) {
					dry = true;
					dry_psi = psi[c];
				}
				sum += psi[c];
			}
			corner[j * (n + 1) + i] = dry ? dry_psi : 0.25f * sum;
		}
	}
	auto at = [&](int i, int j) { return corner[j * (n + 1) + i]; };
	r_flux.resize(n * n * 4);
	float *q = r_flux.ptrw();
	for (int z = 0; z < n; z++) {
		for (int x = 0; x < n; x++) {
			const int i = z * n + x;
			q[i * 4 + 0] = (at(x + 1, z + 1) - at(x + 1, z)) / cell;
			q[i * 4 + 1] = -(at(x + 1, z + 1) - at(x, z + 1)) / cell;
			q[i * 4 + 2] = (at(x, z + 1) - at(x, z)) / cell;
			q[i * 4 + 3] = -(at(x + 1, z) - at(x, z)) / cell;
		}
	}
	for (int k = 0; k < (int)r_flux.size(); k++) {
		if (!Math::is_finite(q[k])) {
			q[k] = 0.0f;
		}
	}
}

void PhysXRiver3D::_live_stop() {
	if (live_height_tex.is_valid()) {
		live_height_tex->set_texture_rd_rid(RID());
	}
	if (live_flow_tex.is_valid()) {
		live_flow_tex->set_texture_rd_rid(RID());
	}
	live_bound = false;
	live_configured = false;
	live_height_cache.clear();
	live_velocity_cache.clear();
	live_tilt = Vector3();
	_live_material();
}

void PhysXRiver3D::_live_material() {
	if (water_material.is_null()) {
		return;
	}
	const bool on = live_bound && live_configured;
	water_material->set_shader_parameter(SNAME("live_enabled"), on);
	if (on) {
		water_material->set_shader_parameter(SNAME("live_center"), live_center);
		water_material->set_shader_parameter(SNAME("live_size"), Vector2(live_domain_size, live_domain_size));
		water_material->set_shader_parameter(SNAME("live_texel"), Vector2(live_domain_size, live_domain_size) / live_grid_resolution);
		water_material->set_shader_parameter(SNAME("live_height_tex"), live_height_tex);
		water_material->set_shader_parameter(SNAME("live_flow_tex"), live_flow_tex);
		water_material->set_shader_parameter(SNAME("live_tilt"), live_tilt);
	}
}

// The live window: the shallow-water solver of PhysXWaterSurface3D's live flow
// (Chentanez and Mueller 2010, water_ripple.glsl) on a square round the camera,
// fed the river's depth and baked fluxes. The river's surface slopes, but the
// solver only carries the disturbance on it (gravity and bed friction balance in
// the baked flow), so its heights are added to the river's own. Moving more
// than a quarter of the square re-centres it and restarts it from the baked flow.
void PhysXRiver3D::_live_update(double p_delta) {
	if (!live_flow || Engine::get_singleton()->is_editor_hint() || sections.size() < 2 || build_pending) {
		return;
	}
	Vector3 follow;
	Node3D *target = live_follow.is_empty() ? nullptr : Object::cast_to<Node3D>(get_node_or_null(live_follow));
	if (target != nullptr) {
		follow = target->get_global_position();
	} else if (get_viewport() != nullptr && get_viewport()->get_camera_3d() != nullptr) {
		const Camera3D *cam = get_viewport()->get_camera_3d();
		// The water the camera looks at: where its view meets the river's height.
		const Vector3 o = cam->get_global_position();
		const Vector3 dir = -cam->get_global_transform().basis.get_column(2);
		const float y = get_global_position().y;
		const float t = dir.y < -0.05f ? (y - o.y) / dir.y : 0.0f;
		follow = o + dir * CLAMP(t, 0.0f, 0.4f * live_domain_size + Math::abs(o.y - y));
	} else {
		return;
	}
	const float cell = live_domain_size / live_grid_resolution;
	const Vector2 want(Math::snapped(follow.x, cell), Math::snapped(follow.z, cell));
	if (!live_configured) {
		live_center = want;
		PackedFloat32Array depth;
		PackedFloat32Array flux;
		_live_fields(depth, flux);
		float mean = 0.0f;
		int wet = 0;
		for (int k = 0; k < depth.size(); k++) {
			if (depth[k] > 0.0f) {
				mean += depth[k];
				wet++;
			}
		}
		WaterSolver::Settings st;
		st.grid_resolution = live_grid_resolution;
		st.domain_size = Vector2(live_domain_size, live_domain_size);
		st.grid_center = live_center;
		st.cell_depth = depth;
		st.depth = wet > 0 ? mean / wet : 1.0f;
		st.damping = live_damping;
		st.gravity = GRAVITY;
		st.water_level = 0.0f; // the solver's heights are the disturbance on the river's surface
		st.ocean_grid_resolution = 16; // no waves on a river: the smallest ocean layer, amplitude 0
		st.ocean_domain_size = Vector2(live_domain_size, live_domain_size);
		st.wave_amplitude = 0.0f;
		st.caustics_enabled = false;
		live_depth = depth;
		live_tilt = Vector3();
		live_solver.configure(st);
		live_solver.set_flow_foam(flow_foam, flow_foam_persistence);
		live_solver.set_base_flux(flux, wall_friction);
		live_configured = true;
		live_fields_dirty = false;
		live_bound = false;
	} else if (MAX(Math::abs(want.x - live_center.x), Math::abs(want.y - live_center.y)) > 0.25f * live_domain_size || live_fields_dirty) {
		live_center = want;
		live_solver.recenter(live_center);
		PackedFloat32Array depth;
		PackedFloat32Array flux;
		_live_fields(depth, flux);
		live_solver.update_seabed(depth);
		live_depth = depth;
		live_tilt = Vector3();
		live_solver.set_base_flux(flux, wall_friction);
		live_fields_dirty = false;
		live_height_cache.clear();
		live_velocity_cache.clear();
		_live_material();
	}
	live_solver.step(p_delta);
	if (!live_bound && live_solver.is_available()) {
		if (live_height_tex.is_null()) {
			live_height_tex.instantiate();
		}
		if (live_flow_tex.is_null()) {
			live_flow_tex.instantiate();
		}
		live_height_tex->set_texture_rd_rid(live_solver.get_ripple_height_texture_rd_rid());
		live_flow_tex->set_texture_rd_rid(live_solver.get_velocity_texture_rd_rid());
		live_bound = true;
		_live_material();
	}
	if (++live_refresh >= 2) {
		live_refresh = 0;
		int n = 0;
		Vector2 domain;
		live_solver.get_height_grid(live_height_cache, n, domain);
		live_solver.get_velocity_grid(live_velocity_cache);
		_live_fit_tilt();
	}
}

// The live water settles into a gentle tilt across the window: the velocity
// head (v^2 / 2g) of a river speeding up or slowing down, which the baked
// surface (Manning's normal depth) leaves out. Kept, it would step the surface
// at the window's edge and move with the window; so the window's mean plane
// over its wet cells is taken out and only local disturbances (eddies, wakes,
// waves round rocks) stay. A least-squares plane, refitted at each readback.
void PhysXRiver3D::_live_fit_tilt() {
	const int n = live_grid_resolution;
	if (live_height_cache.size() != n * n || live_depth.size() != n * n) {
		return;
	}
	const float cell = live_domain_size / n;
	double s1 = 0, sx = 0, sz = 0, sxx = 0, szz = 0, sxz = 0, sh = 0, sxh = 0, szh = 0;
	const float *h = live_height_cache.ptr();
	const float *d = live_depth.ptr();
	for (int z = 0; z < n; z++) {
		for (int x = 0; x < n; x++) {
			const int i = z * n + x;
			if (!(d[i] > 0.05f) || !Math::is_finite(h[i])) {
				continue;
			}
			const double px = (x + 0.5 - 0.5 * n) * cell;
			const double pz = (z + 0.5 - 0.5 * n) * cell;
			s1 += 1;
			sx += px;
			sz += pz;
			sxx += px * px;
			szz += pz * pz;
			sxz += px * pz;
			sh += h[i];
			sxh += px * h[i];
			szh += pz * h[i];
		}
	}
	if (s1 < 16) {
		return;
	}
	// Normal equations for h = a + b x + c z (3 x 3, Cramer).
	const double m[3][3] = { { s1, sx, sz }, { sx, sxx, sxz }, { sz, sxz, szz } };
	const double r[3] = { sh, sxh, szh };
	auto det3 = [](const double p[3][3]) {
		return p[0][0] * (p[1][1] * p[2][2] - p[1][2] * p[2][1]) - p[0][1] * (p[1][0] * p[2][2] - p[1][2] * p[2][0]) + p[0][2] * (p[1][0] * p[2][1] - p[1][1] * p[2][0]);
	};
	const double det = det3(m);
	if (Math::abs(det) < 1e-9) {
		return;
	}
	double out[3];
	for (int k = 0; k < 3; k++) {
		double mk[3][3];
		for (int a = 0; a < 3; a++) {
			for (int b = 0; b < 3; b++) {
				mk[a][b] = b == k ? r[a] : m[a][b];
			}
		}
		out[k] = det3(mk) / det;
	}
	live_tilt = Vector3((float)out[0], (float)out[1], (float)out[2]);
	if (water_material.is_valid() && live_bound) {
		water_material->set_shader_parameter(SNAME("live_tilt"), live_tilt);
	}
}

// The live window's disturbance height at a world point, 0 outside it (eased
// over its edge, as the shader).
float PhysXRiver3D::_live_height_at(const Vector3 &p_world) const {
	const int n = live_grid_resolution;
	if (!live_configured || live_height_cache.size() != n * n) {
		return 0.0f;
	}
	const Vector2 rel = (Vector2(p_world.x, p_world.z) - live_center) / live_domain_size;
	const float inside = 1.0f - Math::smoothstep(0.44f, 0.5f, MAX(Math::abs(rel.x), Math::abs(rel.y)));
	if (inside <= 0.0f) {
		return 0.0f;
	}
	const float gx = CLAMP((rel.x + 0.5f) * n - 0.5f, 0.0f, (float)(n - 1));
	const float gz = CLAMP((rel.y + 0.5f) * n - 0.5f, 0.0f, (float)(n - 1));
	const int x0 = MIN((int)gx, n - 2);
	const int z0 = MIN((int)gz, n - 2);
	const float fx = gx - x0;
	const float fz = gz - z0;
	const float *h = live_height_cache.ptr();
	const float a = Math::lerp(h[z0 * n + x0], h[z0 * n + x0 + 1], fx);
	const float b = Math::lerp(h[(z0 + 1) * n + x0], h[(z0 + 1) * n + x0 + 1], fx);
	const Vector2 off = Vector2(p_world.x, p_world.z) - live_center;
	return (Math::lerp(a, b, fz) - (live_tilt.x + live_tilt.y * off.x + live_tilt.z * off.y)) * inside;
}

// The live part of the flow (the solver's read-back face velocities, averaged to
// cell centres), world xz m/s, 0 outside the window.
Vector2 PhysXRiver3D::_live_velocity_at(const Vector3 &p_world) const {
	const int n = live_grid_resolution;
	if (!live_configured || live_velocity_cache.size() != n * n * 2) {
		return Vector2();
	}
	const Vector2 rel = (Vector2(p_world.x, p_world.z) - live_center) / live_domain_size;
	const float inside = 1.0f - Math::smoothstep(0.44f, 0.5f, MAX(Math::abs(rel.x), Math::abs(rel.y)));
	if (inside <= 0.0f) {
		return Vector2();
	}
	const float *v = live_velocity_cache.ptr();
	auto centre = [&](int x, int z) {
		const int i = z * n + x;
		const float u = 0.5f * (v[i * 2] + (x > 0 ? v[(i - 1) * 2] : 0.0f));
		const float w = 0.5f * (v[i * 2 + 1] + (z > 0 ? v[(i - n) * 2 + 1] : 0.0f));
		return Vector2(u, w);
	};
	const float gx = CLAMP((rel.x + 0.5f) * n - 0.5f, 0.0f, (float)(n - 1));
	const float gz = CLAMP((rel.y + 0.5f) * n - 0.5f, 0.0f, (float)(n - 1));
	const int x0 = MIN((int)gx, n - 2);
	const int z0 = MIN((int)gz, n - 2);
	const float fx = gx - x0;
	const float fz = gz - z0;
	return centre(x0, z0).lerp(centre(x0 + 1, z0), fx).lerp(centre(x0, z0 + 1).lerp(centre(x0 + 1, z0 + 1), fx), fz) * inside;
}

void PhysXRiver3D::set_water_material(const Ref<ShaderMaterial> &p_material) {
	water_material = p_material;
	if (mesh_instance != nullptr) {
		mesh_instance->set_material_override(water_material);
	}
	_live_material();
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
			_live_stop();
			_unregister_mouth();
		} break;
		case NOTIFICATION_INTERNAL_PHYSICS_PROCESS: {
			// Ground rays need the ground in the physics space, which it is by the first tick.
			if (build_pending) {
				_build();
			}
			_live_update(get_physics_process_delta_time());
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			water_time += get_process_delta_time();
			if (water_material.is_valid()) {
				water_material->set_shader_parameter(SNAME("water_time"), (float)Math::fmod(water_time, 3600.0));
				raster_reflection_state.update(this, water_material, raster_reflections);
			}
			if (fall_material.is_valid() && !falls.is_empty()) {
				fall_material->set_shader_parameter(SNAME("water_time"), (float)Math::fmod(water_time, 3600.0));
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
	falls.clear();
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

	// Where each tributary joins: its mouth's place along this river, and which bank.
	{
		const Transform3D inv = get_global_transform().affine_inverse();
		for (Tributary &t : tributaries) {
			const Vector3 local = inv.xform(t.at);
			t.s = c->get_closest_offset(local);
			const Vector3 on = c->sample_baked(t.s, true);
			const Vector3 ahead = c->sample_baked(MIN(t.s + 0.5f, length), true) - c->sample_baked(MAX(t.s - 0.5f, 0.0f), true);
			const Vector3 side(ahead.z, 0.0f, -ahead.x); // towards the left bank
			t.from_right = (local - on).dot(side) < 0.0f;
		}
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
		sec.discharge = get_discharge_at(sec.s);
		sec.psi_right = 0.0f;
		for (const Tributary &t : tributaries) {
			if (t.from_right && sec.s >= t.s) {
				sec.psi_right -= t.discharge;
			}
		}
		sec.mouth_fade = 1.0f;
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
			const float target = sec.discharge / Math::sqrt(bed_slope[i]);
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
			centre_depth[i] = Math::pow(sec.discharge * roughness / (sec.width * Math::sqrt(bed_slope[i])), 0.6f);
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
	// The mouth: the river ends in a lake, the sea or another river. Its surface
	// can't fall below the water it flows into (the level pool: the reach
	// ponds back until its own normal depth rises above that level), and past
	// the shoreline the ribbon fades into that water over mouth_blend.
	{
		Node *target = _mouth_node();
		PhysXWaterSurface3D *sea = Object::cast_to<PhysXWaterSurface3D>(target);
		PhysXRiver3D *main = Object::cast_to<PhysXRiver3D>(target);
		if (sea != nullptr || main != nullptr) {
			const Transform3D xf = get_global_transform();
			const Transform3D inv = xf.affine_inverse();
			int mouth = -1;
			float level_world = 0.0f;
			// The mouth: the first section over that water whose own surface has
			// come down to its level (upstream of it the river may run over
			// water that only floods its bed, still above it).
			for (int i = 0; i < count; i++) {
				const Vector3 w = xf.xform(sections[i].centre);
				if (sea != nullptr ? !sea->is_wet(w) : !(main->sample_depth(w) > 0.0f)) {
					continue;
				}
				const float level = sea != nullptr ? sea->get_global_position().y + sea->get_water_level() : main->sample_height(w);
				if (Math::is_finite(level) && w.y <= level + 0.02f) {
					mouth = i;
					level_world = level;
					break;
				}
			}
			if (main != nullptr) {
				mouth_level_seen = mouth >= 0 && Math::is_finite(level_world) ? level_world : -Math::INF;
			}
			if (mouth >= 0 && Math::is_finite(level_world)) {
				for (int i = 0; i < count; i++) {
					Section &sec = sections[i];
					const float level = inv.xform(Vector3(xf.xform(sec.centre).x, level_world, xf.xform(sec.centre).z)).y;
					if (sec.surface < level) {
						sec.surface = level;
						sec.centre.y = level;
						sec.cascade = false;
					}
					if (i > mouth) {
						const float past = sec.s - sections[mouth].s;
						const float f = mouth_blend > 1e-3f ? CLAMP(past / mouth_blend, 0.0f, 1.0f) : 1.0f;
						sec.mouth_fade = 1.0f - f * f * (3.0f - 2.0f * f);
					}
				}
			}
		}
	}
	_find_falls(space, bed, ds);
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
			sec.speed_at[j] = (d > 0.0f && conveyance > 1e-6f) ? MIN(sec.discharge * Math::pow(d, 2.0f / 3.0f) / conveyance, MAX_SPEED) : 0.0f;
		}
		sec.speed = area > 1e-6f ? MIN(sec.discharge / area, MAX_SPEED) : 0.0f;
		// White water: supercritical flow (Froude above ~1) and the foot of every
		// cascade, then carried downstream at the current's speed, fading.
		const float mean_depth = wet_width > 1e-4f ? area / wet_width : 0.0f;
		const float froude = mean_depth > 1e-3f ? sec.speed / Math::sqrt(GRAVITY * mean_depth) : 0.0f;
		float source = CLAMP((froude - 0.8f) / 0.8f, 0.0f, 1.0f);
		source = source * source * (3.0f - 2.0f * source);
		if (sec.cascade) {
			source = 1.0f;
		}
		source = MAX(source, sec.plunge);
		carried_foam *= Math::exp(-ds / (MAX(sec.speed, 0.1f) * foam_persistence));
		carried_foam = MAX(carried_foam, source);
		sec.foam = carried_foam;
	}
	_solve_flow(ds);
	_build_mesh();
	_build_fall_effects();
	live_fields_dirty = true;
	_register_mouth();
	emit_signal(SNAME("rebuilt"));
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
		const float scale = acc > 1e-9f ? sections[i].discharge / acc : 0.0f;
		for (int j = 0; j <= m; j++) {
			psi[i * row + j] = sections[i].psi_right + psi[i * row + j] * scale;
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

		if (bool(GLOBAL_GET("physics/physx_3d/water/multigrid_flow"))) {
			// The same equation in its symmetric finite-volume form (cell area h ds dn): along s a face
			// conducts K_f dn_f / (h_f ds), across K_f h_f ds / dn, with K_f the mean of a wet pair or the wet
			// cell's own onto a wall, and the metric h at the face; then multigrid-preconditioned CG.
			LocalVector<int> kind;
			LocalVector<int> island_of;
			LocalVector<float> fcx;
			LocalVector<float> fcz;
			kind.resize(cells);
			fcx.resize(cells);
			fcz.resize(cells);
			island_of.resize(comps);
			int islands = 0;
			for (int q = 0; q < comps; q++) {
				island_of[q] = comp_island[q] ? islands++ : -1;
			}
			auto k_face = [&](int p_a, int p_b) -> float {
				const float ka = inv_k[p_a];
				const float kb = inv_k[p_b];
				return (ka > 0.0f && kb > 0.0f) ? 0.5f * (ka + kb) : MAX(ka, kb);
			};
			for (int i = 0; i < count; i++) {
				const float dn = dn_of(i);
				for (int j = 0; j <= m; j++) {
					const int k = i * row + j;
					if (i == 0 || i == count - 1) {
						kind[k] = FlowPoisson::FIXED;
					} else if (comp[k] >= 0) {
						kind[k] = island_of[comp[k]] >= 0 ? FlowPoisson::ISLAND + island_of[comp[k]] : FlowPoisson::FIXED;
					} else {
						kind[k] = FlowPoisson::UNKNOWN;
					}
					fcx[k] = j < m ? k_face(k, k + 1) * 0.5f * (metric(i, j) + metric(i, j + 1)) * p_ds / dn : 0.0f;
					fcz[k] = i < count - 1 ? k_face(k, k + row) * 0.5f * (dn + dn_of(i + 1)) / (0.5f * (metric(i, j) + metric(i + 1, j)) * p_ds) : 0.0f;
				}
			}
			FlowPoisson::solve(row, count, fcx, fcz, kind, islands, psi);
		} else {
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
			const float tolerance = 1e-5f * sections[count - 1].discharge;
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
	}

	// Velocity from psi: along = dpsi/dn / d, across = -dpsi/ds / (h d).
	for (int i = 0; i < count; i++) {
		Section &sec = sections[i];
		sec.across_at.resize(row);
		sec.foam_at.resize(row);
		sec.psi_at.resize(row);
		for (int j = 0; j <= m; j++) {
			sec.psi_at[j] = psi[i * row + j];
		}
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
				src = MAX(src, sec.plunge);
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
		_live_material();
	}
	mesh_instance->set_material_override(water_material);
	const int count = (int)sections.size();
	if (count < 2) {
		mesh_instance->set_mesh(Ref<Mesh>());
		_build_fall_mesh();
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
			const float foam = sec.foam_at[j] * sec.mouth_fade;
			const float width_fade = (1.0f - CLAMP((Math::abs(n) - 0.5f * sec.width) / MAX(bank_margin, 1e-3f), 0.0f, 1.0f)) * sec.mouth_fade;
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
			if (dmax < -0.05f || MAX(sections[i].mouth_fade, sections[i + 1].mouth_fade) <= 0.0f || sections[i].fall_drop) {
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
	_build_fall_mesh();
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

// Waterfalls (docs/ideas/water.md "Step 4f"). A run of cascade sections that
// drops at least fall_min_height, where a free jet from the lip stays clear
// of the ground, is a free overfall: the surface draws down to the brink depth
// 0.715 y_c (Rouse 1936) over about 4 y_c, the water leaves the lip at
// v0 = q / y_b and falls as a projectile, the ribbon under the jet is hidden
// and the water where it lands boils white.
void PhysXRiver3D::_find_falls(PhysicsDirectSpaceState3D *p_space, const LocalVector<float> &p_bed, float p_ds) {
	falls.clear();
	const int count = (int)sections.size();
	for (Section &sec : sections) {
		sec.fall_drop = false;
		sec.plunge = 0.0f;
	}
	if (fall_min_height <= 0.0f || count < 3) {
		return;
	}
	auto bed_at_s = [&](float p_s) -> float {
		const float f = CLAMP(p_s / p_ds, 0.0f, (float)(count - 1));
		const int i = MIN((int)f, count - 2);
		return Math::lerp(p_bed[i], p_bed[i + 1], f - i);
	};
	int i = 1;
	while (i < count) {
		if (!sections[i].cascade) {
			i++;
			continue;
		}
		const int a = i - 1;
		int b = i;
		while (b + 1 < count && sections[b + 1].cascade) {
			b++;
		}
		i = b + 1;
		const Section &sa = sections[a];
		const float height = sa.surface - sections[b].surface;
		if (height < fall_min_height) {
			continue;
		}
		Fall fall;
		fall.brink = a;
		fall.foot = b;
		fall.side = sa.side;
		fall.tangent = Vector3(-sa.side.z, 0.0f, sa.side.x);
		fall.width = sa.width;
		fall.q = sa.discharge / MAX(sa.width, 0.5f);
		const float yc = Math::pow(fall.q * fall.q / GRAVITY, 1.0f / 3.0f);
		const float yb = MAX(0.715f * yc, 0.01f);
		fall.lip_speed = MIN(fall.q / yb, MAX_SPEED);
		// The lip: where the ground between the last high section and the next
		// one drops away (bisected), or halfway with no ground to measure.
		float t_lip = 0.5f;
		if (p_space != nullptr) {
			const Vector3 from = sa.centre;
			const Vector3 to = sections[a + 1].centre;
			const float edge = p_bed[a] - 0.5f * height;
			float lo = 0.0f;
			float hi = 1.0f;
			for (int it = 0; it < 10; it++) {
				const float mid = 0.5f * (lo + hi);
				float y = 0.0f;
				if (_base_ground_at(p_space, from.lerp(to, mid), y) && y > edge) {
					lo = mid;
				} else {
					hi = mid;
				}
			}
			t_lip = lo;
		}
		fall.lip_s = sa.s + t_lip * p_ds;
		fall.lip = sa.centre.lerp(sections[a + 1].centre, t_lip);
		fall.lip.y = p_bed[a];
		fall.pool = sections[b].surface;
		// The free jet must stay clear of the ground (the underside of the sheet
		// against the bed under it); a steep chute it would run down stays a cascade.
		const float drop = fall.lip.y + 0.5f * yb - fall.pool;
		if (drop <= 0.0f) {
			continue;
		}
		const float tau_land = Math::sqrt(2.0f * drop / GRAVITY);
		bool clear = true;
		for (int k = 1; k <= 16 && clear; k++) {
			const float tau = tau_land * k / 16.0f;
			const float s = fall.lip_s + fall.lip_speed * tau;
			const float under = fall.lip.y - 0.5f * GRAVITY * tau * tau;
			if (s < sections[count - 1].s && under < bed_at_s(s) - 0.05f && under > fall.pool + 0.1f) {
				clear = false;
			}
		}
		if (!clear) {
			continue;
		}
		fall.height = sa.surface - fall.pool;
		fall.land_s = fall.lip_s + fall.lip_speed * tau_land;
		fall.impact_speed = Math::sqrt(fall.lip_speed * fall.lip_speed + 2.0f * GRAVITY * drop);
		// Break-up length of a nappe, L_b = 6 q^0.32 (Horeni 1956).
		fall.breakup = 6.0f * Math::pow(MAX(fall.q, 1e-3f), 0.32f);
		// Draw-down to the brink over about 4 y_c upstream of the lip.
		const float brink = fall.lip.y + yb;
		for (int k = a; k >= 0; k--) {
			Section &sec = sections[k];
			const float f = 1.0f - (fall.lip_s - sec.s) / MAX(4.0f * yc, 1e-3f);
			if (f <= 0.0f) {
				break;
			}
			const float y = Math::lerp(sec.surface, MAX(brink, p_bed[k] + 0.02f), f * f);
			sec.surface = MIN(sec.surface, y);
			sec.centre.y = sec.surface;
		}
		// Under the jet: hide the ribbon draped down the cliff.
		for (int k = a; k < b; k++) {
			sections[k].fall_drop = true;
		}
		// Where it lands, and the pool round it, boil white: from the foot of the
		// cliff to a few metres past the landing.
		const float pool_end = fall.land_s + MAX(2.0f, 0.3f * fall.height);
		for (int k = b; k < count; k++) {
			Section &sec = sections[k];
			if (sec.s > pool_end + 2.0f) {
				break;
			}
			const float f = CLAMP((pool_end + 2.0f - sec.s) / 2.0f, 0.0f, 1.0f);
			// Boiling, not a solid sheet: the lace still shows through.
			sec.plunge = MAX(sec.plunge, 0.8f * f);
			sec.cascade = false;
		}
		falls.push_back(fall);
	}
}

// The falling sheet of each waterfall: one column per wet cross sample at the
// brink, each following the projectile from its own unit discharge; a slab
// with front and back faces q/|v| apart (aerated water bulks up to three
// times). Rows are spaced evenly in travel time.
void PhysXRiver3D::_build_fall_mesh() {
	if (falls.is_empty() || sections.size() < 2) {
		if (fall_instance != nullptr) {
			fall_instance->set_mesh(Ref<Mesh>());
		}
		return;
	}
	if (fall_instance == nullptr) {
		fall_instance = memnew(MeshInstance3D);
		fall_instance->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
		add_child(fall_instance, false, INTERNAL_MODE_BACK);
	}
	if (fall_material.is_null()) {
		Ref<Shader> shader;
		shader.instantiate();
		shader->set_code(FALL_SHADER_SRC);
		fall_material.instantiate();
		fall_material->set_shader(shader);
	}
	fall_instance->set_material_override(fall_material);
	const int m = cross_segments;
	PackedVector3Array verts;
	PackedVector3Array normals;
	PackedVector2Array uvs;
	PackedVector2Array uv2s;
	PackedColorArray colors;
	PackedInt32Array indices;
	for (const Fall &fall : falls) {
		const Section &sa = sections[fall.brink];
		const float half = 0.5f * sa.width + bank_margin;
		// Wet span at the brink.
		int j0 = -1;
		int j1 = -1;
		for (int j = 0; j <= m; j++) {
			if (sa.depth[j] > 0.02f) {
				j0 = j0 < 0 ? j : j0;
				j1 = j;
			}
		}
		if (j0 < 0 || j1 <= j0) {
			continue;
		}
		const int cols = j1 - j0 + 1;
		const int rows = CLAMP((int)Math::ceil(fall.height * 3.0f), 8, 64);
		const int per_face = cols * (rows + 2);
		for (int face = 0; face < 2; face++) {
			const float sign = face == 0 ? 1.0f : -1.0f;
			const int base = verts.size();
			for (int jj = 0; jj < cols; jj++) {
				const int j = j0 + jj;
				const float n = -half + 2.0f * half * j / m;
				const float q = MAX(sa.speed_at[j] * sa.depth[j], 0.0f);
				const float yc = Math::pow(q * q / GRAVITY, 1.0f / 3.0f);
				const float yb = MAX(0.715f * yc, 0.005f);
				const float v0 = CLAMP(q / yb, 0.3f, MAX_SPEED);
				const float breakup = 6.0f * Math::pow(MAX(q, 1e-3f), 0.32f);
				const float edge = MIN(1.0f, MIN((float)jj, (float)(cols - 1 - jj)) / 2.0f) * CLAMP(q / MAX(fall.q, 1e-3f), 0.0f, 1.0f);
				const Vector3 lip = fall.lip + fall.side * n;
				const float mid_y = fall.lip.y + 0.5f * yb;
				const float tau_land = Math::sqrt(2.0f * MAX(mid_y - fall.pool, 0.01f) / GRAVITY) + 0.05f;
				// Row 0: the brink section's surface, joining the ribbon.
				{
					const Vector3 p = sa.centre + sa.side * n;
					verts.push_back(face == 0 ? p : Vector3(p.x, fall.lip.y - 0.05f, p.z));
					normals.push_back(Vector3(0, sign, 0));
					uvs.push_back(Vector2(-(fall.lip_s - sa.s) / MAX(v0, 0.3f), n));
					uv2s.push_back(Vector2(v0, 0.0f));
					colors.push_back(Color(edge, 0.0f, 0.0f, 1.0f));
				}
				for (int k = 0; k <= rows; k++) {
					const float tau = tau_land * k / rows;
					const float z = 0.5f * GRAVITY * tau * tau; // fallen so far
					const Vector3 vel = fall.tangent * v0 - Vector3(0, GRAVITY * tau, 0);
					const float speed = vel.length();
					const float aeration = CLAMP(0.15f + 0.85f * z / breakup, 0.0f, 1.0f);
					const float broken = CLAMP((z - breakup) / (0.5f * breakup), 0.0f, 1.0f);
					const float thick = q / MAX(speed, 0.1f) * (1.0f + 2.0f * aeration);
					// Outward: away from the cliff and up at the lip.
					Vector3 out = fall.side.cross(vel).normalized();
					if (out.dot(fall.tangent) < -1e-4f || (Math::abs(out.dot(fall.tangent)) <= 1e-4f && out.y < 0.0f)) {
						out = -out;
					}
					// The sheet spreads sideways by a few per cent of the fall (Ervine and Falvey 1987).
					const Vector3 spread = fall.side * (n * 0.04f * z / MAX(half, 0.5f));
					const Vector3 centre = Vector3(lip.x, mid_y, lip.z) + fall.tangent * (v0 * tau) - Vector3(0, z, 0) + spread;
					verts.push_back(centre + out * (0.5f * thick * sign));
					normals.push_back(out * sign);
					uvs.push_back(Vector2(tau, n));
					uv2s.push_back(Vector2(speed, aeration));
					colors.push_back(Color(edge, aeration, broken, 1.0f));
				}
			}
			const int rr = rows + 2;
			for (int jj = 0; jj + 1 < cols; jj++) {
				for (int k = 0; k + 1 < rr; k++) {
					const int k00 = base + jj * rr + k;
					const int k01 = k00 + 1;
					const int k10 = k00 + rr;
					const int k11 = k10 + 1;
					// Front faces wound so the outward side is the front (as the ribbon:
					// the path tracer takes the front as entering the water).
					const Vector3 e1 = verts[k10] - verts[k00];
					const Vector3 e2 = verts[k01] - verts[k00];
					const bool flip = e1.cross(e2).dot(normals[k00]) > 0.0f;
					if (!flip) {
						indices.push_back(k00);
						indices.push_back(k10);
						indices.push_back(k01);
						indices.push_back(k10);
						indices.push_back(k11);
						indices.push_back(k01);
					} else {
						indices.push_back(k00);
						indices.push_back(k01);
						indices.push_back(k10);
						indices.push_back(k10);
						indices.push_back(k01);
						indices.push_back(k11);
					}
				}
			}
			(void)per_face;
		}
	}
	if (indices.is_empty()) {
		fall_instance->set_mesh(Ref<Mesh>());
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
	fall_instance->set_mesh(mesh);
}

// Spray and mist where each jet lands, scaled with the fall's power
// rho g Q H. Spray: droplets thrown up from the impact line at a share of the
// impact speed (GPUParticles3D, small spheres, lit, so they show under path
// tracing). Mist: the finest drops, which fall at a few cm/s and drift out on
// the air the fall drags down -- a PhysXWaterMist3D grid per fall, or a cool
// PhysXFlowEmitter3D feeding the scene's PhysXFlow3D when it has one.
void PhysXRiver3D::_build_fall_effects() {
	for (Node3D *n : fall_effects) {
		if (n != nullptr) {
			remove_child(n);
			memdelete(n);
		}
	}
	fall_effects.clear();
	if (Engine::get_singleton()->is_editor_hint() || (!fall_spray && !fall_mist)) {
		return;
	}
	const Ref<Curve3D> c = get_curve();
	for (const Fall &fall : falls) {
		const float power = 1000.0f * GRAVITY * fall.q * fall.width * fall.height; // W
		Vector3 land = fall.lip + fall.tangent * (fall.land_s - fall.lip_s);
		land.y = fall.pool;
		Transform3D xf;
		xf.basis = Basis(fall.side, Vector3(0, 1, 0), -fall.tangent).orthonormalized();
		xf.origin = land;
		if (fall_spray) {
			GPUParticles3D *spray = memnew(GPUParticles3D);
			spray->set_name("FallSpray");
			spray->set_transform(xf);
			spray->set_amount(CLAMP((int)(power / 200.0f), 64, 4096));
			const float rise = 0.35f * fall.impact_speed;
			spray->set_lifetime(MAX(0.6f, 2.0f * rise / GRAVITY));
			spray->set_pre_process_time(2.0);
			spray->set_visibility_aabb(AABB(Vector3(-fall.width - 4.0f, -1.0f, -6.0f), Vector3(2.0f * fall.width + 8.0f, rise * rise / GRAVITY + 3.0f, 12.0f)));
			Ref<ParticleProcessMaterial> pm;
			pm.instantiate();
			pm->set_emission_shape(ParticleProcessMaterial::EMISSION_SHAPE_BOX);
			pm->set_emission_box_extents(Vector3(0.5f * fall.width, 0.1f, 0.3f));
			pm->set_direction(Vector3(0, 1, 0.35f));
			pm->set_spread(40.0f);
			pm->set_param_min(ParticleProcessMaterial::PARAM_INITIAL_LINEAR_VELOCITY, 0.4f * rise);
			pm->set_param_max(ParticleProcessMaterial::PARAM_INITIAL_LINEAR_VELOCITY, rise);
			pm->set_gravity(Vector3(0, -GRAVITY, 0));
			pm->set_param_min(ParticleProcessMaterial::PARAM_DAMPING, 0.5f);
			pm->set_param_max(ParticleProcessMaterial::PARAM_DAMPING, 1.5f);
			pm->set_param_min(ParticleProcessMaterial::PARAM_SCALE, 0.04f);
			pm->set_param_max(ParticleProcessMaterial::PARAM_SCALE, 0.12f);
			spray->set_process_material(pm);
			Ref<SphereMesh> drop;
			drop.instantiate();
			drop->set_radius(0.5f);
			drop->set_height(1.0f);
			drop->set_radial_segments(6);
			drop->set_rings(3);
			Ref<StandardMaterial3D> dm;
			dm.instantiate();
			dm->set_albedo(Color(0.95f, 0.97f, 1.0f));
			dm->set_roughness(0.25f);
			drop->set_material(dm);
			spray->set_draw_pass_mesh(0, drop);
			add_child(spray, false, INTERNAL_MODE_BACK);
			fall_effects.push_back(spray);
		}
		if (fall_mist) {
			bool to_flow = false;
#ifdef GODOT_PHYSX_FLOW
			// A scene with a PhysXFlow3D simulates the mist in its own gas grid (a full fluid solve).
			to_flow = is_inside_tree() && !get_tree()->get_root()->find_children("*", "PhysXFlow3D", true, false).is_empty();
			if (to_flow) {
				PhysXFlowEmitter3D *mist = memnew(PhysXFlowEmitter3D);
				mist->set_name("FallMist");
				Transform3D mxf = xf;
				mxf.origin += Vector3(0, 0.6f, 0);
				mist->set_transform(mxf);
				mist->set_shape(PhysXFlowEmitter3D::SHAPE_BOX);
				mist->set_size(Vector3(fall.width + 1.0f, 1.2f, 2.0f));
				// The fall's wind: out from the foot at a fraction of the impact speed.
				mist->set_velocity(Vector3(0, 0.4f, -0.15f * fall.impact_speed));
				mist->set_temperature(0.0f);
				mist->set_smoke(CLAMP(power / 100000.0f, 0.2f, 2.0f));
				add_child(mist, false, INTERNAL_MODE_BACK);
				fall_effects.push_back(mist);
			}
#endif
			if (!to_flow) {
				// Otherwise its own mist grid (PhysXWaterMist3D): fed with a share of the fall's water that
				// grows with the square of the impact speed (atomisation by the jet), blown out along the
				// pool by the air the fall drags down (about 0.3 of the impact speed).
				const float reach = CLAMP(4.0f + 0.6f * fall.height, 6.0f, 24.0f);
				const float side = MAX(fall.width + 2.0f * reach, 2.0f * reach);
				const float shift = MAX(reach - 3.0f, 0.0f);
				PhysXWaterMist3D *mist = memnew(PhysXWaterMist3D);
				mist->set_name("FallMist");
				Transform3D mxf = xf;
				mxf.origin += fall.tangent * shift;
				mist->set_transform(mxf);
				mist->set_size(Vector3(side, CLAMP(fall.height + 4.0f, 6.0f, 30.0f), side));
				mist->set_cell_size(0.5f);
				mist->set_source_position(Vector3(0.0f, 0.5f, shift));
				mist->set_source_extents(Vector3(0.5f * fall.width + 0.5f, 0.75f, 1.0f));
				mist->set_jet_direction(Vector3(0.0f, 0.0f, -1.0f));
				mist->set_jet_speed(MIN(0.3f * fall.impact_speed, 12.0f));
				const float q_total = fall.q * fall.width; // m^3/s
				const float v = fall.impact_speed / 10.0f;
				mist->set_source_rate(fall_mist_amount * 1e-5f * v * v * 1000.0f * q_total * 1000.0f); // g/s
				add_child(mist, false, INTERNAL_MODE_BACK);
				fall_effects.push_back(mist);
			}
		}
	}
	(void)c;
}

void PhysXRiver3D::set_fall_min_height(float p_height) {
	fall_min_height = MAX(p_height, 0.0f);
	_queue_build();
}

void PhysXRiver3D::set_fall_spray(bool p_enabled) {
	fall_spray = p_enabled;
	_queue_build();
}

void PhysXRiver3D::set_fall_mist(bool p_enabled) {
	fall_mist = p_enabled;
	_queue_build();
}

void PhysXRiver3D::set_fall_mist_amount(float p_amount) {
	fall_mist_amount = MAX(p_amount, 0.0f);
	_queue_build();
}

void PhysXRiver3D::set_fall_material(const Ref<ShaderMaterial> &p_material) {
	fall_material = p_material;
	if (fall_instance != nullptr) {
		fall_instance->set_material_override(fall_material);
	}
}

Array PhysXRiver3D::get_waterfalls() const {
	Array out;
	const Transform3D xf = is_inside_tree() ? get_global_transform() : get_transform();
	for (const Fall &fall : falls) {
		Vector3 land = fall.lip + fall.tangent * (fall.land_s - fall.lip_s);
		land.y = fall.pool;
		Dictionary d;
		d["lip"] = xf.xform(fall.lip);
		d["impact"] = xf.xform(land);
		d["height"] = fall.height;
		d["discharge"] = fall.q * fall.width;
		d["width"] = fall.width;
		d["lip_speed"] = fall.lip_speed;
		d["impact_speed"] = fall.impact_speed;
		d["breakup_length"] = fall.breakup;
		d["throw"] = fall.land_s - fall.lip_s;
		out.push_back(d);
	}
	return out;
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
	return xf.xform(Vector3(local.x, local_y, local.z)).y + _live_height_at(p_world_pos);
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
	const Vector2 live = _live_velocity_at(p_world_pos);
	return get_global_transform().basis.xform(tangent * v + side * a) + Vector3(live.x, 0.0f, live.y);
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
	d["under_fall"] = sec.fall_drop;
	d["plunge"] = sec.plunge;
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

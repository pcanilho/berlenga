/**************************************************************************/
/*  oasis_springs_3d.cpp                                                  */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2026-present Paulo Canilho and Berlenga contributors.    */
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

#include "oasis_springs_3d.h"

#include "physx_water_surface_3d.h"

#include "core/config/engine.h"
#include "core/math/math_funcs.h"
#include "core/math/random_pcg.h"
#include "core/object/class_db.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/resources/3d/world_3d.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"
#include "scene/resources/shader.h"
#include "servers/physics_3d/physics_server_3d.h"

namespace {
constexpr float SECONDS_PER_YEAR = 31557600.0f;
constexpr float DEFAULT_EVAPORATION = 2.5f; // m a year, Western Desert open water (6-8 mm a day in summer)
constexpr int OUTLINE_SEGMENTS = 128;
constexpr int BOIL_RINGS = 22;
constexpr int BOIL_SEGMENTS = 48;
constexpr float BOIL_EXTENT = 2.0f; // the boil disc reaches this many boil radii, fading into the bed
constexpr float BOIL_LIFT = 0.01f; // m above the measured bed at the vent
constexpr float BOIL_EDGE_LIFT = -0.01f; // and at the disc's edge (into the bed)

// The sand boil: a ring of sand thrown out round the vent and a fluidised floor that heaves as the flow pulses,
// grains dancing on it (the look of every sand-boil film: Ginnie Springs, the Siwa "ain"). The heights are the
// same function in raster and path tracing (TIME is captured for both). The disc sits on the measured bed, 1 cm
// over it at the vent and sinking 1 cm into it at its edge, so the apron meets the bed along an uneven line (no
// cut-out edge: under path tracing a sheet over the bed shades the bed through its holes).
const char *BOIL_SHADER = R"(
shader_type spatial;
render_mode cull_back;

uniform vec3 sand_color : source_color = vec3(0.78, 0.69, 0.52);
uniform float boil_radius = 0.45;
uniform float strength = 1.0;
uniform float phase = 0.0;

varying float v_r;
varying vec3 v_world;
varying float v_pulse;

float hash12(vec2 p) {
	vec3 p3 = fract(vec3(p.xyx) * 0.1031);
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.x + p3.y) * p3.z);
}

float vnoise(vec2 p) {
	vec2 i = floor(p);
	vec2 f = fract(p);
	vec2 u = f * f * (3.0 - 2.0 * f);
	return mix(mix(hash12(i), hash12(i + vec2(1.0, 0.0)), u.x), mix(hash12(i + vec2(0.0, 1.0)), hash12(i + vec2(1.0, 1.0)), u.x), u.y);
}

// The flow through the vent: slow surges, uneven in time (a sine bent by a slower one), sharpened so the floor
// spends most of its time settled and heaves briefly.
float boil_pulse(float t) {
	return pow(0.5 + 0.5 * sin(t * 1.7 + phase + 1.3 * sin(t * 0.37 + phase * 1.7)), 3.0);
}

float boil_height(vec2 q, float t, float pulse) {
	float r = length(q) / boil_radius;
	// The cone: sand the jet threw out, its crest just outside the throat and broken up round the ring, a steep
	// inner wall and a long outer apron.
	float side = r < 1.0 ? (r - 1.0) / 0.3 : (r - 1.0) / 0.55;
	float lip = 0.07 * (0.7 + 0.6 * vnoise(q * 6.0 + phase)) * exp(-side * side);
	// The throat inside: lifted as a fluidised mound by each surge, with grains dancing on it.
	float inner = clamp(1.0 - r / 0.8, 0.0, 1.0);
	float heave = 0.035 * strength * pulse * inner * inner;
	float dance = 0.008 * strength * inner * (vnoise(q * 16.0 + vec2(t * 1.1, -t * 0.8)) - 0.5) * (0.35 + pulse);
	// The apron's edge: grain-scale relief, so where it dips under the bed the line wanders.
	float apron = 0.005 * (vnoise(q * 11.0 + phase * 3.1) - 0.5) * smoothstep(1.2, 2.0, r);
	return lip + heave + dance + apron;
}

void vertex() {
	vec2 q = VERTEX.xz;
	float t = TIME;
	float pulse = boil_pulse(t);
	float e = 0.015;
	float h = boil_height(q, t, pulse);
	float hx = boil_height(q + vec2(e, 0.0), t, pulse);
	float hz = boil_height(q + vec2(0.0, e), t, pulse);
	VERTEX.y += h;
	NORMAL = normalize(NORMAL + vec3(h - hx, 0.0, h - hz) / e);
	v_r = length(q) / boil_radius;
	v_pulse = pulse;
	v_world = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
}

void fragment() {
	// Grains: two scales of value noise on the world position (no tile to repeat). The cone is the coarse sand the
	// jet sorts out (a little darker, grainier); the throat a dark pit of sand in suspension that the surges fill
	// with pale, clean sand. sand_color is the bed's own (under water, as the ground round it draws it), so the
	// apron fades into the bed with no patch.
	float g = 0.87 + 0.26 * vnoise(v_world.xz * 7.0) + 0.22 * (hash12(floor(v_world.xz * 220.0)) - 0.5);
	float throat = 1.0 - smoothstep(0.15, 0.7, v_r);
	float lip = exp(-pow((v_r - 1.0) / 0.4, 2.0));
	vec3 c = sand_color * g * (1.0 - 0.1 * lip) * mix(1.0, mix(0.45, 1.1, v_pulse), throat);
	ALBEDO = c;
	ROUGHNESS = 0.9;
}
)";

uint32_t _hash_u32(uint32_t p_x) {
	p_x ^= p_x >> 16;
	p_x *= 0x7feb352dU;
	p_x ^= p_x >> 15;
	p_x *= 0x846ca68bU;
	p_x ^= p_x >> 16;
	return p_x;
}

// One draw per spring from its position (stable as springs are added or removed elsewhere) and the node's seed.
uint32_t _spring_seed(const Vector3 &p_position, int p_seed) {
	uint32_t h = _hash_u32((uint32_t)p_seed * 0x9e3779b9U + 0x632be5abU);
	h = _hash_u32(h ^ (uint32_t)(int32_t)Math::floor(p_position.x * 4.0f));
	h = _hash_u32(h ^ (uint32_t)(int32_t)Math::floor(p_position.z * 4.0f));
	return h;
}
} // namespace

float OasisSprings3D::pool_area(float p_discharge, float p_evaporation, float p_seepage) {
	const float loss = MAX(p_evaporation + p_seepage, 0.01f); // m a year
	return MAX(p_discharge, 0.0f) * 0.001f * SECONDS_PER_YEAR / loss;
}

// Hargreaves & Samani (1985): ET0 = 0.0023 * 0.408 Ra (T + 17.8) sqrt(Tmax - Tmin) mm a day, Ra the
// extraterrestrial radiation (MJ/m2/day, FAO-56 eq. 21) at the latitude, T a sinusoid from the January and July
// means lapsed to the height. Open water over a shallow pool is Kc 1.05 of it (FAO-56 table 12). Siwa (29.2 N,
// 13 to 29 C, 14 K days) gives about 1.7 m a year, inside the 1.5 to 2.5 m measured for its lakes; 46 N with
// 3 to 22 C and 9 K days about 0.9 m, as for lakes in central Europe.
float OasisSprings3D::climate_evaporation(const WeatherSeason::Climate &p_climate, float p_height, float p_diurnal_range) {
	const float phi = Math::deg_to_rad(CLAMP(p_climate.latitude, -66.0f, 66.0f));
	const float dz = (p_height - p_climate.reference_height) * 0.001f;
	const float range_sqrt = Math::sqrt(MAX(p_diurnal_range, 0.0f));
	double total_mm = 0.0;
	for (int day = 1; day <= 365; day++) {
		const float a = Math::TAU * float(day) / 365.0f;
		const float season = 0.5f - 0.5f * Math::cos(Math::TAU * float(day - 15) / 365.0f); // 0 mid-January, 1 mid-July
		const float lapse = Math::lerp(p_climate.lapse_rate_january, p_climate.lapse_rate_july, season);
		const float t = Math::lerp(p_climate.temperature_january, p_climate.temperature_july, season) - lapse * dz;
		const float dr = 1.0f + 0.033f * Math::cos(a);
		const float decl = 0.409f * Math::sin(a - 1.39f);
		const float ws = Math::acos(CLAMP(-Math::tan(phi) * Math::tan(decl), -1.0f, 1.0f));
		const float ra = 24.0f * 60.0f / Math::PI * 0.0820f * dr * (ws * Math::sin(phi) * Math::sin(decl) + Math::cos(phi) * Math::cos(decl) * Math::sin(ws));
		total_mm += MAX(0.0023f * 0.408f * ra * MAX(t + 17.8f, 0.0f) * range_sqrt, 0.0f);
	}
	return float(total_mm * 0.001 * 1.05);
}

float OasisSprings3D::evaporation_at(float p_height) const {
	if (evaporation > 0.0f) {
		return evaporation;
	}
	if (WorldContracts *wc = WorldContracts::get_singleton()) {
		const Ref<WorldWeather> ww = wc->get_weather();
		WeatherSeason::Climate climate;
		if (ww.is_valid() && ww->season_climate(climate)) {
			return climate_evaporation(climate, p_height, diurnal_range);
		}
	}
	return DEFAULT_EVAPORATION;
}

PackedVector4Array OasisSprings3D::_read_springs() const {
	PackedVector4Array out = springs;
	if (spring_source.is_empty() || !is_inside_tree()) {
		return out;
	}
	Node *source = get_node_or_null(spring_source);
	if (source == nullptr || !source->has_method(SNAME("get_springs"))) {
		return out;
	}
	const Variant v = source->call(SNAME("get_springs"));
	switch (v.get_type()) {
		case Variant::PACKED_VECTOR4_ARRAY: {
			out.append_array(PackedVector4Array(v));
		} break;
		case Variant::PACKED_VECTOR3_ARRAY: {
			for (const Vector3 &p : PackedVector3Array(v)) {
				out.push_back(Vector4(p.x, p.y, p.z, 0.0f));
			}
		} break;
		case Variant::ARRAY: {
			for (const Variant &e : Array(v)) {
				if (e.get_type() == Variant::VECTOR4) {
					out.push_back(e);
				} else if (e.get_type() == Variant::VECTOR3) {
					const Vector3 p = e;
					out.push_back(Vector4(p.x, p.y, p.z, 0.0f));
				}
			}
		} break;
		default:
			break;
	}
	return out;
}

// The outline: a disc broken by harmonics 2..7, each a random share (falling as k^-1.5, the spectrum of natural
// shorelines' low end) and phase, scaled so its area is the water balance's exactly. Never pinched below 0.55 of
// the radius, so the vent (at most vent_offset of it from the centre) is always inside.
Ref<ArrayMesh> OasisSprings3D::_outline_mesh(Spring &r_spring) const {
	RandomPCG rng(r_spring.seed);
	float amp[8] = {};
	float ph[8] = {};
	float sum = 0.0f;
	for (int k = 2; k < 8; k++) {
		amp[k] = Math::lerp(0.4f, 1.0f, rng.randf()) * Math::pow(float(k), -1.5f);
		ph[k] = rng.randf() * Math::TAU;
		sum += amp[k];
	}
	float rad[OUTLINE_SEGMENTS];
	double area = 0.0;
	for (int i = 0; i < OUTLINE_SEGMENTS; i++) {
		const float th = Math::TAU * float(i) / float(OUTLINE_SEGMENTS);
		float n = 0.0f;
		for (int k = 2; k < 8; k++) {
			n += amp[k] / sum * Math::cos(float(k) * th + ph[k]);
		}
		rad[i] = MAX(1.0f + outline_noise * n, 0.55f);
	}
	for (int i = 0; i < OUTLINE_SEGMENTS; i++) {
		area += 0.5 * double(rad[i]) * double(rad[(i + 1) % OUTLINE_SEGMENTS]) * Math::sin(Math::TAU / OUTLINE_SEGMENTS);
	}
	const float scale = Math::sqrt(r_spring.area / MAX(float(area), 1e-4f));

	r_spring.outline.resize(OUTLINE_SEGMENTS);
	for (int i = 0; i < OUTLINE_SEGMENTS; i++) {
		const float th = Math::TAU * float(i) / float(OUTLINE_SEGMENTS);
		r_spring.outline.set(i, Vector2(r_spring.pool_centre.x, r_spring.pool_centre.z) + Vector2(Math::cos(th), Math::sin(th)) * rad[i] * scale);
	}

	PackedVector3Array verts;
	PackedVector3Array normals;
	verts.resize(OUTLINE_SEGMENTS * 3);
	normals.resize(OUTLINE_SEGMENTS * 3);
	Vector3 *w = verts.ptrw();
	Vector3 *nw = normals.ptrw();
	for (int i = 0; i < OUTLINE_SEGMENTS; i++) {
		const int j = (i + 1) % OUTLINE_SEGMENTS;
		const float a0 = Math::TAU * float(i) / float(OUTLINE_SEGMENTS);
		const float a1 = Math::TAU * float(j) / float(OUTLINE_SEGMENTS);
		w[i * 3 + 0] = Vector3();
		w[i * 3 + 1] = Vector3(Math::cos(a1), 0.0f, Math::sin(a1)) * rad[j] * scale;
		w[i * 3 + 2] = Vector3(Math::cos(a0), 0.0f, Math::sin(a0)) * rad[i] * scale;
		nw[i * 3 + 0] = nw[i * 3 + 1] = nw[i * 3 + 2] = Vector3(0, 1, 0);
	}
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = verts;
	arrays[Mesh::ARRAY_NORMAL] = normals;
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	return mesh;
}

Ref<ShaderMaterial> OasisSprings3D::_boil_shader_material() {
	Ref<Shader> shader;
	shader.instantiate();
	shader->set_code(BOIL_SHADER);
	Ref<ShaderMaterial> m;
	m.instantiate();
	m->set_shader(shader);
	return m;
}

void OasisSprings3D::_clear() {
	for (Spring &s : built) {
		for (MeshInstance3D *b : s.boils) {
			if (b != nullptr) {
				b->queue_free();
			}
		}
		if (s.pool != nullptr) {
			s.pool->queue_free();
		}
	}
	built.clear();
	vents_placed = false;
}

void OasisSprings3D::_rebuild() {
	dirty = false;
	_clear();
	if (!is_inside_tree()) {
		return;
	}
	const PackedVector4Array list = _read_springs();
	for (const Vector4 &v : list) {
		Spring s;
		s.position = Vector3(v.x, v.y, v.z);
		s.discharge = v.w > 0.0f ? v.w : default_discharge;
		s.seed = _spring_seed(s.position, seed);
		const float e = evaporation_at(s.position.y);
		s.area = pool_area(s.discharge, e, seepage);
		s.radius = Math::sqrt(s.area / Math::PI);
		if (s.radius < min_radius || s.radius > max_radius) {
			s.radius = CLAMP(s.radius, min_radius, max_radius);
			s.area = Math::PI * s.radius * s.radius;
		}
		// The vent off the pool's centre: the pool spreads to the lower side of a spring as often as not.
		RandomPCG rng(s.seed ^ 0x5bd1e995U);
		const float va = rng.randf() * Math::TAU;
		const float vr = Math::lerp(0.3f, 1.0f, rng.randf()) * vent_offset * s.radius;
		const Vector2 vent_off(Math::cos(va) * vr, Math::sin(va) * vr);
		s.pool_centre = s.position - Vector3(vent_off.x, 0.0f, vent_off.y);

		PhysXWaterSurface3D *pool = memnew(PhysXWaterSurface3D);
		pool->set_name(vformat("Pool%d", (int)built.size()));
		pool->set_surface_mesh(_outline_mesh(s));
		pool->set_depth(fallback_depth);
		pool->set_seabed_from_floor(seabed_from_floor);
		pool->set_seabed_collision_mask(seabed_collision_mask);
		// Still water: no swell, surf or spray. Wind ripples come from the weather over the pool's own fetch.
		pool->set_shore_waves(false);
		pool->set_shore_spray(false);
		pool->set_crest_spray(false);
		add_child(pool, false, INTERNAL_MODE_FRONT);
		pool->set_global_position(s.pool_centre);
		s.pool = pool;

		// One to three vents a pace or two apart (their discs never overlap): springs seldom rise through one eye.
		const int count = sand_boils ? 1 + int(rng.rand() % 3) : 0;
		const float a0 = rng.randf() * Math::TAU;
		for (int i = 0; i < count; i++) {
			Vector3 at = s.position;
			if (i > 0) {
				// The second to one side of the first, the third across from it (at least 120 degrees apart).
				const float a = a0 + (i == 1 ? 0.0f : Math::PI + Math::lerp(-1.0f, 1.0f, rng.randf()));
				const float d = Math::lerp(1.05f, 1.6f, rng.randf()) * 2.0f * boil_radius * BOIL_EXTENT;
				at += Vector3(Math::cos(a) * d, 0.0f, Math::sin(a) * d);
			}
			s.vents.push_back(at);
			s.vent_phase.push_back(rng.randf() * Math::TAU);
		}
		built.push_back(s);
	}
}

// The boils' discs follow the measured bed: each vertex is a ray down onto the static ground under the pool.
void OasisSprings3D::_place_vents() {
	vents_placed = true;
	PhysicsDirectSpaceState3D *space = get_world_3d().is_valid() ? get_world_3d()->get_direct_space_state() : nullptr;
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	if (boil_material.is_null()) {
		boil_material = _boil_shader_material();
	}
	const float rmax = boil_radius * BOIL_EXTENT;
	for (Spring &s : built) {
		for (uint32_t vi = 0; vi < s.vents.size(); vi++) {
			Vector3 &vent = s.vents[vi];
			auto bed_at = [&](const Vector3 &p) -> float {
				const float fallback = s.position.y - fallback_depth;
				if (space == nullptr || !seabed_from_floor) {
					return fallback;
				}
				PhysicsDirectSpaceState3D::RayParameters ray;
				ray.from = Vector3(p.x, s.position.y + 2.0f, p.z);
				ray.to = Vector3(p.x, s.position.y - 60.0f, p.z);
				ray.collision_mask = seabed_collision_mask;
				ray.collide_with_areas = false;
				for (int skip = 0; skip < 4; skip++) {
					PhysicsDirectSpaceState3D::RayResult result;
					if (!space->intersect_ray(ray, result)) {
						break;
					}
					if (ps->body_get_mode(result.rid) == PhysicsServer3D::BODY_MODE_STATIC) {
						return result.position.y;
					}
					ray.exclude.insert(result.rid);
				}
				return fallback;
			};
			vent.y = bed_at(vent);
			if (vent.y >= s.position.y - 0.05f) {
				continue; // dry here: the bed rises above the water
			}
			// A polar disc, rings closer near the vent where the floor moves.
			PackedVector3Array verts;
			PackedVector3Array normals;
			PackedInt32Array idx;
			verts.push_back(Vector3(0.0f, 0.0f, 0.0f));
			for (int r = 1; r <= BOIL_RINGS; r++) {
				const float rr = rmax * Math::pow(float(r) / BOIL_RINGS, 1.4f);
				for (int a = 0; a < BOIL_SEGMENTS; a++) {
					const float th = Math::TAU * float(a) / BOIL_SEGMENTS;
					verts.push_back(Vector3(Math::cos(th) * rr, 0.0f, Math::sin(th) * rr));
				}
			}
			// Each vertex on the bed (relative to the vent), a few mm above it.
			Vector3 *vw = verts.ptrw();
			for (int i = 0; i < verts.size(); i++) {
				const Vector3 p = vent + Vector3(vw[i].x, 0.0f, vw[i].z);
				const float lift = Math::lerp(BOIL_LIFT, BOIL_EDGE_LIFT, Vector2(vw[i].x, vw[i].z).length() / rmax);
				vw[i].y = MIN(bed_at(p), s.position.y - 0.02f) - vent.y + lift;
			}
			normals.resize(verts.size());
			for (int a = 0; a < BOIL_SEGMENTS; a++) {
				const int b = (a + 1) % BOIL_SEGMENTS;
				idx.push_back(0);
				idx.push_back(1 + a);
				idx.push_back(1 + b);
			}
			for (int r = 1; r < BOIL_RINGS; r++) {
				const int i0 = 1 + (r - 1) * BOIL_SEGMENTS;
				const int i1 = 1 + r * BOIL_SEGMENTS;
				for (int a = 0; a < BOIL_SEGMENTS; a++) {
					const int b = (a + 1) % BOIL_SEGMENTS;
					idx.push_back(i0 + a);
					idx.push_back(i1 + a);
					idx.push_back(i0 + b);
					idx.push_back(i0 + b);
					idx.push_back(i1 + a);
					idx.push_back(i1 + b);
				}
			}
			// The bed's normals, from the triangles.
			Vector3 *nw = normals.ptrw();
			for (int i = 0; i < idx.size(); i += 3) {
				const Vector3 n = (vw[idx[i + 2]] - vw[idx[i]]).cross(vw[idx[i + 1]] - vw[idx[i]]);
				nw[idx[i]] += n;
				nw[idx[i + 1]] += n;
				nw[idx[i + 2]] += n;
			}
			for (int i = 0; i < normals.size(); i++) {
				nw[i] = nw[i].length_squared() > 0.0f ? nw[i].normalized() : Vector3(0, 1, 0);
				if (nw[i].y < 0.0f) {
					nw[i] = -nw[i];
				}
			}
			Array arrays;
			arrays.resize(Mesh::ARRAY_MAX);
			arrays[Mesh::ARRAY_VERTEX] = verts;
			arrays[Mesh::ARRAY_NORMAL] = normals;
			arrays[Mesh::ARRAY_INDEX] = idx;
			Ref<ArrayMesh> mesh;
			mesh.instantiate();
			mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
			// Each boil its own copy: its phase and size.
			Ref<ShaderMaterial> m = boil_material->duplicate();
			m->set_shader_parameter(SNAME("sand_color"), sand_color);
			m->set_shader_parameter(SNAME("boil_radius"), boil_radius);
			m->set_shader_parameter(SNAME("strength"), boil_strength);
			m->set_shader_parameter(SNAME("phase"), s.vent_phase[vi]);
			mesh->surface_set_material(0, m);
			MeshInstance3D *mi = memnew(MeshInstance3D);
			mi->set_name(vformat("Boil%d", (int)s.boils.size()));
			mi->set_mesh(mesh);
			mi->set_extra_cull_margin(0.1f); // the most the shader lifts the disc
			add_child(mi, false, INTERNAL_MODE_FRONT);
			mi->set_global_position(vent);
			s.boils.push_back(mi);
		}
	}
}

// The surface over each vent: the upwelling lifts a faint dome (a few mm) that rises with each surge and sheds
// rings as it settles. The same pulse as the boil's shader.
void OasisSprings3D::_pulse(double p_delta) {
	clock += p_delta;
	const float t = float(clock);
	for (Spring &s : built) {
		if (s.pool == nullptr) {
			continue;
		}
		for (uint32_t vi = 0; vi < s.vents.size(); vi++) {
			const float ph = s.vent_phase[vi];
			const float pulse = Math::pow(0.5f + 0.5f * Math::sin(t * 1.7f + ph + 1.3f * Math::sin(t * 0.37f + ph * 1.7f)), 3.0f);
			const float radius = boil_radius * 2.5f;
			const Vector3 at(s.vents[vi].x, s.position.y, s.vents[vi].z);
			s.pool->submit_impulse(at, radius, 0.0025f * boil_strength * (0.3f + pulse));
		}
	}
}

void OasisSprings3D::rebuild() {
	dirty = true;
	if (is_inside_tree()) {
		_rebuild();
	}
}

void OasisSprings3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			set_physics_process_internal(true);
			if (dirty) {
				_rebuild();
			}
		} break;
		case NOTIFICATION_EXIT_TREE: {
			dirty = true;
		} break;
		case NOTIFICATION_INTERNAL_PHYSICS_PROCESS: {
			if (dirty) {
				_rebuild();
			}
			if (!vents_placed) {
				_place_vents();
			}
			if (!Engine::get_singleton()->is_editor_hint() && sand_boils) {
				_pulse(get_physics_process_delta_time());
			}
		} break;
	}
}

PhysXWaterSurface3D *OasisSprings3D::get_pool(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, (int)built.size(), nullptr);
	return built[p_index].pool;
}

float OasisSprings3D::get_pool_area(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, (int)built.size(), 0.0f);
	return built[p_index].area;
}

float OasisSprings3D::get_pool_radius(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, (int)built.size(), 0.0f);
	return built[p_index].radius;
}

float OasisSprings3D::get_pool_discharge(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, (int)built.size(), 0.0f);
	return built[p_index].discharge;
}

PackedVector2Array OasisSprings3D::get_outline(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, (int)built.size(), PackedVector2Array());
	return built[p_index].outline;
}

PackedVector3Array OasisSprings3D::get_vents(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, (int)built.size(), PackedVector3Array());
	PackedVector3Array out;
	for (const Vector3 &v : built[p_index].vents) {
		out.push_back(v);
	}
	return out;
}

#define OASIS_SETTER(m_name, m_type, m_member) \
	void OasisSprings3D::set_##m_name(m_type p_value) { \
		m_member = p_value; \
		rebuild(); \
	}

OASIS_SETTER(springs, const PackedVector4Array &, springs)
OASIS_SETTER(spring_source, const NodePath &, spring_source)
OASIS_SETTER(default_discharge, float, default_discharge)
OASIS_SETTER(evaporation, float, evaporation)
OASIS_SETTER(diurnal_range, float, diurnal_range)
OASIS_SETTER(seepage, float, seepage)
OASIS_SETTER(min_radius, float, min_radius)
OASIS_SETTER(max_radius, float, max_radius)
OASIS_SETTER(outline_noise, float, outline_noise)
OASIS_SETTER(vent_offset, float, vent_offset)
OASIS_SETTER(fallback_depth, float, fallback_depth)
OASIS_SETTER(seabed_from_floor, bool, seabed_from_floor)
OASIS_SETTER(seabed_collision_mask, uint32_t, seabed_collision_mask)
OASIS_SETTER(sand_boils, bool, sand_boils)
OASIS_SETTER(boil_radius, float, boil_radius)
OASIS_SETTER(boil_strength, float, boil_strength)
OASIS_SETTER(sand_color, const Color &, sand_color)
OASIS_SETTER(seed, int, seed)

#undef OASIS_SETTER

void OasisSprings3D::_bind_methods() {
#define OASIS_BIND(m_name, m_arg) \
	ClassDB::bind_method(D_METHOD("set_" #m_name, m_arg), &OasisSprings3D::set_##m_name); \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &OasisSprings3D::get_##m_name);
	OASIS_BIND(springs, "springs")
	OASIS_BIND(spring_source, "path")
	OASIS_BIND(default_discharge, "discharge")
	OASIS_BIND(evaporation, "metres_a_year")
	OASIS_BIND(diurnal_range, "kelvin")
	OASIS_BIND(seepage, "metres_a_year")
	OASIS_BIND(min_radius, "radius")
	OASIS_BIND(max_radius, "radius")
	OASIS_BIND(outline_noise, "amount")
	OASIS_BIND(vent_offset, "share")
	OASIS_BIND(fallback_depth, "depth")
	OASIS_BIND(seabed_from_floor, "enabled")
	OASIS_BIND(seabed_collision_mask, "mask")
	OASIS_BIND(sand_boils, "enabled")
	OASIS_BIND(boil_radius, "radius")
	OASIS_BIND(boil_strength, "strength")
	OASIS_BIND(sand_color, "color")
	OASIS_BIND(seed, "seed")
#undef OASIS_BIND
	ClassDB::bind_method(D_METHOD("rebuild"), &OasisSprings3D::rebuild);
	ClassDB::bind_method(D_METHOD("get_pool_count"), &OasisSprings3D::get_pool_count);
	ClassDB::bind_method(D_METHOD("get_pool", "index"), &OasisSprings3D::get_pool);
	ClassDB::bind_method(D_METHOD("get_pool_area", "index"), &OasisSprings3D::get_pool_area);
	ClassDB::bind_method(D_METHOD("get_pool_radius", "index"), &OasisSprings3D::get_pool_radius);
	ClassDB::bind_method(D_METHOD("get_pool_discharge", "index"), &OasisSprings3D::get_pool_discharge);
	ClassDB::bind_method(D_METHOD("get_vents", "index"), &OasisSprings3D::get_vents);
	ClassDB::bind_method(D_METHOD("get_outline", "index"), &OasisSprings3D::get_outline);
	ClassDB::bind_method(D_METHOD("evaporation_at", "height"), &OasisSprings3D::evaporation_at);
	ClassDB::bind_static_method("OasisSprings3D", D_METHOD("pool_area", "discharge", "evaporation", "seepage"), &OasisSprings3D::pool_area);

	ADD_PROPERTY(PropertyInfo(Variant::PACKED_VECTOR4_ARRAY, "springs"), "set_springs", "get_springs");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "spring_source"), "set_spring_source", "get_spring_source");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "default_discharge", PROPERTY_HINT_RANGE, "0.001,5,0.001,or_greater,suffix:L/s"), "set_default_discharge", "get_default_discharge");
	ADD_GROUP("Water Balance", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "evaporation", PROPERTY_HINT_RANGE, "0,6,0.05,suffix:m/yr"), "set_evaporation", "get_evaporation");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "diurnal_range", PROPERTY_HINT_RANGE, "0,30,0.5,suffix:K"), "set_diurnal_range", "get_diurnal_range");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "seepage", PROPERTY_HINT_RANGE, "0,10,0.05,suffix:m/yr"), "set_seepage", "get_seepage");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "min_radius", PROPERTY_HINT_RANGE, "0.5,50,0.1,suffix:m"), "set_min_radius", "get_min_radius");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "max_radius", PROPERTY_HINT_RANGE, "1,200,0.5,suffix:m"), "set_max_radius", "get_max_radius");
	ADD_GROUP("Shape", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "outline_noise", PROPERTY_HINT_RANGE, "0,0.45,0.01"), "set_outline_noise", "get_outline_noise");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "vent_offset", PROPERTY_HINT_RANGE, "0,0.45,0.01"), "set_vent_offset", "get_vent_offset");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "fallback_depth", PROPERTY_HINT_RANGE, "0.1,20,0.1,suffix:m"), "set_fallback_depth", "get_fallback_depth");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "seabed_from_floor"), "set_seabed_from_floor", "get_seabed_from_floor");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "seabed_collision_mask", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_seabed_collision_mask", "get_seabed_collision_mask");
	ADD_GROUP("Sand Boils", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "sand_boils"), "set_sand_boils", "get_sand_boils");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "boil_radius", PROPERTY_HINT_RANGE, "0.1,2,0.01,suffix:m"), "set_boil_radius", "get_boil_radius");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "boil_strength", PROPERTY_HINT_RANGE, "0,3,0.05"), "set_boil_strength", "get_boil_strength");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "sand_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_sand_color", "get_sand_color");
	ADD_GROUP("", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "seed"), "set_seed", "get_seed");
}

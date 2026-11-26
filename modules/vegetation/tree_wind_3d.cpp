/**************************************************************************/
/*  tree_wind_3d.cpp                                                      */
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

#include "tree_wind_3d.h"

#include "tree_generator.h"
#include "vegetation_fire.h"
#include "vegetation_field_3d.h"

#include "core/config/engine.h"
#include "core/math/random_pcg.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/os.h"
#include "core/templates/hash_set.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/multimesh_instance_3d.h"
#include "scene/3d/physics/collision_object_3d.h"
#include "scene/3d/physics/collision_shape_3d.h"
#include "scene/3d/physics/rigid_body_3d.h"
#include "scene/3d/physics/static_body_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "scene/resources/3d/capsule_shape_3d.h"
#include "scene/resources/3d/cylinder_shape_3d.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/image_texture.h"
#include "servers/physics_3d/physics_server_3d.h"
#include "servers/rendering/rendering_server.h"

#include "modules/weather/weather_map_3d.h"
#include "modules/world/world_contracts.h"

namespace {
constexpr float AIR_DENSITY = 1.2f; // kg/m3
constexpr float DRAG_COEFFICIENT = 0.6f; // Of the crown silhouette, below the reconfiguration speed.
constexpr float RECONFIGURATION_SPEED = 4.0f; // m/s; above it foliage streamlines (Vogel 1989).
constexpr int RECORD_FLOATS = 20; // Transform 12, colour 4, custom 4.
// Kaimal turbulence lengths (m) and relative sigmas for the along-wind, cross-wind and vertical
// parts, as WeatherMap3D uses them (IEC 61400-1 with Lambda = 7 m).
constexpr float KAIMAL_L[3] = { 8.1f * 7.0f, 2.7f * 7.0f, 0.66f * 7.0f };
constexpr float KAIMAL_SIGMA[3] = { 1.0f, 0.8f, 0.5f };
constexpr int WAVES_PER_AXIS = 8;
constexpr float BLAST_CHUNK_RADIUS = 0.03f; // m: thinner bones ride the Blast chunk below them.
constexpr float BLAST_NODE_HEALTH = 1.0e6f; // Chunks never wear out; bonds carry the wood's strength.

uint32_t hash_u32(uint32_t p_x) {
	p_x ^= p_x >> 16;
	p_x *= 0x7feb352du;
	p_x ^= p_x >> 15;
	p_x *= 0x846ca68bu;
	p_x ^= p_x >> 16;
	return p_x;
}

float hash01(uint32_t p_seed, uint32_t p_salt) {
	return float(hash_u32(p_seed * 0x9e3779b9u + p_salt * 0x85ebca6bu) >> 8) / 16777216.0f;
}

float hash_phase(uint32_t p_seed, uint32_t p_salt) {
	return float(hash_u32(p_seed * 0x9e3779b9u + p_salt) >> 8) * (float(Math::TAU) / 16777216.0f);
}

// Kaimal spectral density of a velocity component at frequency p_f (Hz): f S(f) / sigma^2 =
// 4 n / (1 + 6 n)^(5/3), n = f L / U.
float kaimal_density(float p_f, float p_sigma, float p_length, float p_speed) {
	const float u = MAX(p_speed, 0.5f);
	const float n = p_f * p_length / u;
	return p_sigma * p_sigma * 4.0f * (p_length / u) / Math::pow(1.0f + 6.0f * n, 5.0f / 3.0f);
}
} // namespace

TreeWind3D::Rig TreeWind3D::parse_rig(const PackedFloat32Array &p_data, float p_vogel) {
	Rig rig;
	rig.vogel = p_vogel;
	const int stride = TreeGenerator::RIG_STRIDE;
	const int count = p_data.size() / stride;
	rig.bones.resize(count);
	const float *d = p_data.ptr();
	LocalVector<int> groups;
	groups.resize(count);
	for (int b = 0; b < count; b++) {
		const float *o = d + b * stride;
		Bone &bone = rig.bones[b];
		bone.parent = MAX(int(o[TreeGenerator::RIG_PARENT]), 0);
		bone.pivot = Vector3(o[TreeGenerator::RIG_PIVOT_X], o[TreeGenerator::RIG_PIVOT_Y], o[TreeGenerator::RIG_PIVOT_Z]);
		bone.centroid = Vector3(o[TreeGenerator::RIG_CENTROID_X], o[TreeGenerator::RIG_CENTROID_Y], o[TreeGenerator::RIG_CENTROID_Z]);
		bone.dir = Vector3(o[TreeGenerator::RIG_DIR_X], o[TreeGenerator::RIG_DIR_Y], o[TreeGenerator::RIG_DIR_Z]);
		bone.area = o[TreeGenerator::RIG_AREA];
		bone.stiffness = o[TreeGenerator::RIG_STIFFNESS];
		bone.snow_stiffness = bone.stiffness;
		bone.zeta = CLAMP(o[TreeGenerator::RIG_ZETA], 0.02f, 1.0f);
		const float inertia = MAX(o[TreeGenerator::RIG_INERTIA], 1e-6f);
		bone.omega = b == 0 ? 0.0f : Math::sqrt(MAX(bone.stiffness, 0.0f) / inertia);
		groups[b] = CLAMP(int(o[TreeGenerator::RIG_GROUP]), 0, count - 1);
		bone.radius = o[TreeGenerator::RIG_RADIUS];
		bone.mass = o[TreeGenerator::RIG_MASS];
		bone.length = o[TreeGenerator::RIG_LENGTH];
		// Foliage around the bone reaches about as far from its axis as its load centroid sits.
		const Vector3 side = bone.centroid - bone.dir * bone.centroid.dot(bone.dir);
		bone.reach = CLAMP(side.length() * 1.2f + bone.radius, 0.2f, 3.0f);
		rig.extent = MAX(rig.extent, (bone.pivot + bone.dir * bone.length).length() + bone.reach);
		if (b > 0 && bone.parent == 0) {
			rig.leaf_area += bone.area;
		}
	}
	rig.crown_radius = _crown_radius(rig);
	// Trees build their wood to the loads they meet (Mattheck's axiom of uniform stress, "Trees:
	// the mechanical design", 1991; thigmomorphogenesis, Telewski 2006): a branch holds its own
	// weight, and a stem or limb thickens until a strong breeze bends each part only a little.
	// The growth model sizes wood by the pipe model alone, and its thinnest leaders (a spent tip
	// the crown overtook) would bend like string. So each segment is at least stiff enough to
	// sag no more than 0.25 rad under its carried load turned sideways, and to turn no more than
	// 0.03 rad in a 15 m/s wind.
	const float gravity = 9.81f;
	const float max_sag = 0.25f;
	const float design_wind = 15.0f;
	const float design_turn = 0.03f;
	const float design_cd = DRAG_COEFFICIENT * Math::pow(design_wind / RECONFIGURATION_SPEED, p_vogel);
	LocalVector<float> mass;
	mass.resize(count);
	for (int b = 0; b < count; b++) {
		mass[b] = d[b * stride + TreeGenerator::RIG_MASS];
		Bone &bone = rig.bones[b];
		if (b > 0) {
			const float lever = bone.centroid.length();
			const float wind_moment = 0.5f * AIR_DENSITY * design_cd * bone.area * design_wind * design_wind * lever;
			bone.stiffness = MAX(bone.stiffness, MAX(mass[b] * gravity * lever / max_sag, wind_moment / design_turn));
			// Under a slow, held load (snow) the wood bends by its own stiffness: the wind's floor stands in for
			// the gust-stiffened, reconfiguring crown, not for the wood. Kept from sagging over 0.5 rad under its
			// own weight turned sideways (the pipe model's thin spent leaders).
			bone.snow_stiffness = MIN(MAX(bone.snow_stiffness, mass[b] * gravity * lever / 0.5f), bone.stiffness);
		}
	}
	// Each axis (a stem or a limb, split in segments) sways as one mode. Its frequency by
	// Rayleigh's quotient on the static shape under a sideways load of one g: segment c turns by
	// theta_c = M_c / k_c, its own load moves by u_c (the turns of it and the segments below), and
	// omega^2 = sum k theta^2 / sum m_own u^2.
	for (int b = 1; b < count; b++) {
		if (groups[b] != b) {
			continue;
		}
		double num = 0.0, den = 0.0;
		for (int c = b; c < count; c++) {
			if (groups[c] != b) {
				continue;
			}
			const Bone &bc = rig.bones[c];
			const double theta_c = double(mass[c]) * gravity * bc.centroid.length() / double(bc.stiffness);
			num += double(bc.stiffness) * theta_c * theta_c;
			// Own mass: carried mass less what the next segment of this axis carries.
			float own = mass[c];
			for (int e = c + 1; e < count; e++) {
				if (groups[e] == b && rig.bones[e].parent == c) {
					own -= mass[e];
				}
			}
			const Vector3 at = bc.pivot + bc.centroid;
			double u = 0.0;
			for (int a2 = c; a2 >= b; a2 = rig.bones[a2].parent) {
				const Bone &ba = rig.bones[a2];
				const double ta = double(mass[a2]) * gravity * ba.centroid.length() / double(ba.stiffness);
				u += ta * double(at.distance_to(ba.pivot));
				if (a2 == b) {
					break;
				}
			}
			den += double(MAX(own, 0.0f)) * u * u;
		}
		// Kept to what trees and shrubs show: 0.1 Hz (tall conifers) to 8 Hz (twigs).
		const float omega = CLAMP(den > 1e-12 ? float(Math::sqrt(num / den)) : 0.0f, float(Math::TAU) * 0.1f, float(Math::TAU) * 8.0f);
		for (int c = b; c < count; c++) {
			if (groups[c] == b) {
				rig.bones[c].omega = omega;
			}
		}
	}
	// Bones of one stem share their resonant phase (solve_pose), so the stem moves as one mode.
	rig.groups = groups;
	return rig;
}

int TreeWind3D::get_mesh_bone_count(const Ref<Mesh> &p_mesh) {
	if (p_mesh.is_null() || !p_mesh->has_meta(SNAME("tree_rig"))) {
		return 0;
	}
	const PackedFloat32Array d = p_mesh->get_meta(SNAME("tree_rig"));
	return d.size() / TreeGenerator::RIG_STRIDE;
}

const TreeWind3D::Rig *TreeWind3D::_rig_for(const Ref<Mesh> &p_mesh) {
	if (p_mesh.is_null() || !p_mesh->has_meta(SNAME("tree_rig"))) {
		return nullptr;
	}
	const uint64_t id = p_mesh->get_instance_id();
	if (!rigs.has(id)) {
		const float vogel = p_mesh->has_meta(SNAME("tree_vogel")) ? float(p_mesh->get_meta(SNAME("tree_vogel"))) : -0.7f;
		Rig rig = parse_rig(p_mesh->get_meta(SNAME("tree_rig")), vogel);
		if (p_mesh->has_meta(SNAME("tree_snow_caps"))) {
			const Array caps = p_mesh->get_meta(SNAME("tree_snow_caps"));
			for (int k = 0; k < MIN(caps.size(), 3); k++) {
				rig.snow_caps[k] = caps[k];
			}
			if (p_mesh->has_meta(SNAME("tree_snow_cap_sites"))) {
				rig.cap_sites = p_mesh->get_meta(SNAME("tree_snow_cap_sites"));
			}
		}
		if (p_mesh->has_meta(SNAME("tree_foliage"))) {
			// Leafless broadleaves hold some 10-20% of what conifers do, on twigs and forks (Hedstrom and Pomeroy
			// 1998 on leafless aspen; Pomeroy and Gray 1995); in leaf, a wet early or late snow loads them fully
			// (the October snows that break broadleaf crowns).
			rig.snow_catch = Math::lerp(0.15f, 1.0f, CLAMP(float(p_mesh->get_meta(SNAME("tree_foliage"))), 0.0f, 1.0f));
		}
		if (p_mesh->has_meta(SNAME("tree_glaze_surface"))) {
			rig.glaze_surface = p_mesh->get_meta(SNAME("tree_glaze_surface"));
		}
		if (p_mesh->has_meta(SNAME("tree_crown_radius"))) {
			rig.crown_radius = MAX(float(p_mesh->get_meta(SNAME("tree_crown_radius"))), 0.3f);
		}
		if (p_mesh->has_meta(SNAME("tree_colliders"))) {
			rig.colliders = p_mesh->get_meta(SNAME("tree_colliders"));
		}
		if (p_mesh->has_meta(SNAME("tree_static_materials"))) {
			rig.static_materials = p_mesh->get_meta(SNAME("tree_static_materials"));
		}
		if (p_mesh->has_meta(SNAME("tree_storm"))) {
			// Storm breaks and windthrow (docs/ideas/trees.md section 5t). A part breaks when the
			// wind's bending moment at its base passes its strength, the tree is thrown when the
			// overturning moment at its foot passes its anchorage. Strength from the wood: MOR pi r^3
			// / 4, less 15% for knots (GALES, Gardiner et al. 2000); anchorage from the roots: C_reg
			// times the stem's mass (Nicoll et al. 2006; the stem some three quarters of the mass).
			// Trees build both to the loads they meet (Mattheck's uniform stress; thigmomorphogenesis):
			// across sizes and species they fail at much the same wind, about 42 m/s in gusts (Virot
			// et al. 2016). The growth model sizes wood by the pipe model alone (its thin leaders far
			// too weak, its old stems far too heavy), so each part's strength is held within 1-1.6
			// times a design load (the roots' within 1-1.2), scaled by the species' wood (MOR over
			// 50 MPa) or roots (C_reg over 150 N m/kg).
			const PackedFloat32Array sd = p_mesh->get_meta(SNAME("tree_storm"));
			if (sd.size() >= 9) {
				const float mor = sd[0];
				const float creg = sd[1];
				// The everyday wind each part grew in: 15 m/s at 10 m on the log law over short
				// vegetation (z0 3 cm) with turbulence 0.2, as the solver's fallback blows, at its peak:
				// three sigmas of gust and the crest of its own resonant sway (solve_pose's terms).
				// Trees hold about twice that peak load before failing; calibrated so the median
				// tree's weakest part goes at gusts near 42 m/s in a storm of turbulence 0.25 (Virot).
				const float safety = 2.1f;
				auto q_ref = [&](const Bone &p_bone) {
					const float z0 = 0.03f;
					const float h = (p_bone.pivot + p_bone.centroid).y;
					const float u = 15.0f * Math::log(MAX(h, 1.5f * z0) / z0) / Math::log(10.0f / z0);
					const float sigma = 0.2f * u;
					const float f0 = MAX(p_bone.omega, 0.01f) / float(Math::TAU);
					const float band = float(Math::PI) * f0 / (4.0f * MAX(p_bone.zeta, 0.02f));
					const float res = Math::sqrt(kaimal_density(f0, sigma * KAIMAL_SIGMA[0], KAIMAL_L[0], u) * band);
					const float v = u + 3.0f * sigma + 2.0f * res;
					return 0.5f * AIR_DENSITY * DRAG_COEFFICIENT * Math::pow(MAX(v, RECONFIGURATION_SPEED) / RECONFIGURATION_SPEED, rig.vogel) * v * v;
				};
				float stem_mass = 0.0f;
				float base_ref = 0.0f;
				// Trees in snowy climates carry what their crowns catch: conifer limbs and tops break from about 20-50 kg
				// per m2 of crown of wet snow (Peltola et al. 1999, Scots pine; Nykanen et al. 1997), so each part holds
				// at least its share of 35 kg/m2 (section 5al), the defects below taking the weakest from about 23.
				// Broadleaves meet snow bare and are built for little of it (12): one in leaf breaks under an early or
				// late wet snow, as in the October snows of central Europe.
				const float snow_design = (rig.vogel > -0.6f ? 35.0f : 12.0f) * 9.81f * float(Math::PI) * rig.crown_radius * rig.crown_radius / MAX(rig.leaf_area, 1e-6f);
				for (uint32_t b = 1; b < rig.bones.size(); b++) {
					Bone &bone = rig.bones[b];
					const float wood = 0.85f * mor * float(Math::PI) * bone.radius * bone.radius * bone.radius * 0.25f;
					const float snow_lo = snow_design * bone.area * Vector2(bone.centroid.x, bone.centroid.z).length() * (mor / 50.0e6f);
					const float lo = MAX(safety * q_ref(bone) * bone.area * bone.centroid.length() * (mor / 50.0e6f), snow_lo);
					bone.strength = CLAMP(wood, lo, 1.6f * lo);
					if (bone.parent == 0) {
						stem_mass += bone.mass;
						base_ref += q_ref(bone) * bone.area * (bone.pivot + bone.centroid).length();
					}
				}
				const float lo_root = safety * 1.15f * base_ref * (creg / 150.0f);
				rig.anchorage = CLAMP(creg * 0.75f * stem_mass, lo_root, 1.2f * lo_root);
				rig.plate_radius = sd[2];
				rig.plate_thick = sd[3];
				rig.storm_data = sd;
				rig.storm = rig.bones.size() > 1;
				if (p_mesh->has_meta(SNAME("tree_fracture_cap"))) {
					rig.cap_kit = p_mesh->get_meta(SNAME("tree_fracture_cap"));
				}
			}
		}
		rig.mesh = p_mesh;
		rigs[id] = rig;
	}
	return &rigs[id];
}

TreeWind3D::WindSample TreeWind3D::sample_wind(const Vector3 &p_position, float p_height) const {
	WindSample w;
	if (frame_weather) {
		frame_weather->get_wind_parts(p_position, p_height, w.mean, w.along, w.sigma);
		return w;
	}
	// No weather map: the fallback wind at 10 m on the log law over short vegetation (z0 3 cm).
	const float z0 = 0.03f;
	const float z = MAX(p_height, 1.5f * z0);
	const float profile = Math::log(z / z0) / Math::log(10.0f / z0);
	w.mean = Vector3(fallback_wind.x, 0.0f, fallback_wind.y) * profile;
	const float speed = fallback_wind.length();
	w.along = speed > 1e-3f ? fallback_wind / speed : Vector2(1, 0);
	w.sigma = fallback_turbulence * speed;
	return w;
}

void TreeWind3D::_gather_waves() {
	frame_weather = Object::cast_to<WeatherMap3D>(get_node_or_null(weather_map_path));
	if (frame_weather) {
		frame_weather->get_gust_waves(waves);
		base_direction = frame_weather->get_base_wind_direction();
		base_speed = frame_weather->get_base_wind_speed();
		return;
	}
	// Our own Kaimal waves, built as WeatherMap3D builds its own, drifting with the fallback wind.
	base_speed = fallback_wind.length();
	base_direction = base_speed > 1e-3f ? fallback_wind / base_speed : Vector2(1, 0);
	waves.resize(WAVES_PER_AXIS * 3);
	RandomPCG rng(0x7ee5u);
	const float n_min = 0.004f;
	const float n_max = 25.0f;
	for (int axis = 0; axis < 3; axis++) {
		for (int i = 0; i < WAVES_PER_AXIS; i++) {
			const float t0 = float(i) / WAVES_PER_AXIS;
			const float t1 = float(i + 1) / WAVES_PER_AXIS;
			const float n0 = i == 0 ? 0.0f : n_min * Math::pow(n_max / n_min, t0);
			const float n1 = n_min * Math::pow(n_max / n_min, t1);
			const float var = Math::pow(1.0f + 6.0f * n0, -2.0f / 3.0f) - Math::pow(1.0f + 6.0f * n1, -2.0f / 3.0f);
			const float nc = i == 0 ? 0.5f * n1 : Math::sqrt(n0 * n1);
			const float k = float(Math::TAU) * nc / KAIMAL_L[axis];
			const float turn = (rng.randf() * 2.0f - 1.0f) * 0.6f;
			const float phase0 = rng.randf() * float(Math::TAU);
			const float phase = float(Math::fmod(double(phase0) - double(k) * base_speed * time, Math::TAU));
			waves[axis * WAVES_PER_AXIS + i] = Vector4(k, k * turn, KAIMAL_SIGMA[axis] * Math::sqrt(2.0f * MAX(var, 0.0f)), phase);
		}
	}
}

void TreeWind3D::solve_pose(const Rig &p_rig, const Transform3D &p_world, uint32_t p_seed, Transform3D *r_bones, const Touch *p_touch, float p_min_length, StormLoad *r_storm, float p_snow_kg) const {
	const uint32_t count = p_rig.bones.size();
	if (count == 0) {
		return;
	}
	const Basis &wb = p_world.basis;
	const float scale = MAX(Math::pow(float(Math::abs(wb.determinant())), 1.0f / 3.0f), 1e-3f);
	const Basis rot = wb.orthonormalized();
	const Basis to_local = rot.transposed();
	const Vector2 ea = base_direction;
	const Vector2 ec(-ea.y, ea.x);
	const float t = float(Math::fmod(time, 3600.0 * 24.0));
	r_bones[0] = Transform3D();
	const uint32_t wave_count = waves.size();
	const uint32_t per_axis = MAX(wave_count / 3, 1u);
	const bool storm = r_storm && p_rig.storm;
	// A fire-killed tree's rot (docs/ideas/physx.md, "Standing snags"): its fine limbs dry, check and drop within a few
	// years, its roots and butt hold about a year, then rot (snags fall at half-lives of about 5 to 10 years: Everett et al.
	// 1999, Russell et al. 2006, Dunn and Bailey 2016). Strength left: limbs 0.2 + 0.8 e^(-t/3), roots and butt
	// 0.15 + 0.85 e^(-(t-1)/6), t in years dead.
	const float snag_t = r_storm ? r_storm->snag_age : -1.0f;
	const float snag_wood = snag_t < 0.0f ? 1.0f : 0.2f + 0.8f * Math::exp(-snag_t / 3.0f);
	const float snag_roots = snag_t < 0.0f ? 1.0f : 0.15f + 0.85f * Math::exp(-MAX(snag_t - 1.0f, 0.0f) / 6.0f);
	const float scale3 = scale * scale * scale;
	Vector3 base_moment;
	Vector3 base_wind;
	const bool has_broken = p_touch && p_touch->broken.size() == count;
	const bool has_flat = p_touch && p_touch->flat.size() == count;
	// Snow on the crown (section 5al): its weight per unit of carried leaf area, in tree space (N per m2 at scale 1).
	// Each bone carries its area's share, the same cantilever as the wind's drag: theta = r x F / k.
	const Vector3 snow_per_area = p_snow_kg > 0.0f && p_rig.leaf_area > 1e-6f ? to_local.xform(Vector3(0.0f, -9.81f * p_snow_kg / (p_rig.leaf_area * scale * scale), 0.0f)) : Vector3();
	for (uint32_t b = 1; b < count; b++) {
		const Bone &bone = p_rig.bones[b];
		const Vector3 load_local = bone.pivot + bone.centroid;
		const Vector3 at = p_world.xform(load_local);
		const WindSample ws = sample_wind(at, MAX(load_local.y * scale, 0.5f));
		// Too small on screen to see its own sway (its length and foliage reach under the LOD's
		// bound): it keeps the lean the mean wind gives it and drops the gusts and resonance (the
		// costly part). Riding its parent unbent, a twig lost its whole lean, up to the bend cap,
		// where the bound assumed 0.15 rad: the bench measured 1.4 px against 1 px.
		const bool lean_only = bone.length + bone.reach < p_min_length && !(p_touch && p_touch->active);
		if (lean_only) {
			const Vector3 v = to_local.xform(ws.mean);
			const float vs = v.length();
			Transform3D local;
			if (vs > 1e-4f || p_snow_kg > 0.0f) {
				const float cd = DRAG_COEFFICIENT * (vs > RECONFIGURATION_SPEED ? Math::pow(vs / RECONFIGURATION_SPEED, p_rig.vogel) : 1.0f);
				const Vector3 force = v * (0.5f * AIR_DENSITY * cd * bone.area * scale * scale * vs * drag_scale);
				Vector3 theta = bone.centroid.cross(force) / MAX(bone.stiffness * scale * scale * stiffness_scale, 1e-3f);
				theta += bone.centroid.cross(snow_per_area * (bone.area * scale * scale)) / MAX(bone.snow_stiffness * scale * scale * stiffness_scale, 1e-3f);
				theta -= bone.dir * theta.dot(bone.dir);
				const float angle = theta.length();
				if (angle > 1e-6f) {
					const float bent = max_bend * Math::tanh(angle / max_bend);
					const Basis turn(theta / angle, bent);
					local = Transform3D(turn, bone.pivot - turn.xform(bone.pivot));
				}
			}
			r_bones[b] = r_bones[MIN(uint32_t(bone.parent), b - 1)] * local;
			continue;
		}
		const float omega0 = MAX(bone.omega / scale * Math::sqrt(stiffness_scale), 1e-3f);
		const float zeta = bone.zeta;

		// Background: the gust waves through the oscillator, gain capped at the quasi-static 1 so
		// the resonance is counted once, below.
		const float pa = at.x * ea.x + at.z * ea.y;
		const float pc = at.x * ec.x + at.z * ec.y;
		float g[3] = { 0.0f, 0.0f, 0.0f };
		for (uint32_t i = 0; i < wave_count; i++) {
			const Vector4 &w = waves[i];
			const float r = Math::abs(w.x) * base_speed / omega0;
			const float re = 1.0f - r * r;
			const float im = 2.0f * zeta * r;
			// gain * cos(phase - lag), with cos and sin of the lag from re and im (no atan2).
			const float mag = MAX(Math::sqrt(re * re + im * im), 1e-4f);
			const float gain = MIN(1.0f / mag, 1.0f) / mag;
			const float ph = w.x * pa + w.y * pc + w.w;
			g[MIN(i / per_axis, 2u)] += w.z * gain * (Math::cos(ph) * re + Math::sin(ph) * im);
		}
		const Vector3 along(ws.along.x, 0.0f, ws.along.y);
		const Vector3 across(-ws.along.y, 0.0f, ws.along.x);
		const Vector3 gust = (along * g[0] + across * g[1] + Vector3(0, g[2], 0)) * ws.sigma;

		// Resonance: the broadband load near omega0 makes a narrow-band sway of variance
		// S(f0) pi f0 / (4 zeta) (Davenport's resonant part), drawn as two waves at
		// omega0 (1 -+ zeta) for its beating envelope. Stems share one phase set.
		const float speed = ws.mean.length();
		const float f0 = omega0 / float(Math::TAU);
		const float band = float(Math::PI) * f0 / (4.0f * zeta);
		const float amp_u = Math::sqrt(kaimal_density(f0, ws.sigma * KAIMAL_SIGMA[0], KAIMAL_L[0], speed) * band);
		const float amp_v = Math::sqrt(kaimal_density(f0, ws.sigma * KAIMAL_SIGMA[1], KAIMAL_L[1], speed) * band);
		const uint32_t phase_seed = p_seed * 977u + uint32_t(p_rig.groups.size() > b ? p_rig.groups[b] : int(b));
		const float wa = omega0 * (1.0f - zeta);
		const float wbb = omega0 * (1.0f + zeta);
		const float res_u = amp_u * (Math::cos(wa * t + hash_phase(phase_seed, 1)) + Math::cos(wbb * t + hash_phase(phase_seed, 2)));
		const float res_v = amp_v * (Math::cos(wa * t + hash_phase(phase_seed, 3)) + Math::cos(wbb * t + hash_phase(phase_seed, 4)));

		const Vector3 v_world = ws.mean + gust + along * res_u + across * res_v;
		const Vector3 v = to_local.xform(v_world);
		const float vs = v.length();
		const float cd = DRAG_COEFFICIENT * (vs > RECONFIGURATION_SPEED ? Math::pow(vs / RECONFIGURATION_SPEED, p_rig.vogel) : 1.0f);
		// Force in tree space at world scale: area scales with scale^2.
		const Vector3 snow_force = snow_per_area * (bone.area * scale * scale);
		const Vector3 force = v * (0.5f * AIR_DENSITY * cd * bone.area * scale * scale * vs * drag_scale) + snow_force;
		if (storm && !(has_broken && p_touch->broken[b])) {
			// The bending moment at the bone's base (N m): its lever in tree space times scale.
			const Vector3 moment = bone.centroid.cross(force) * scale;
			if (bone.parent == 0) {
				base_moment += (bone.pivot + bone.centroid).cross(force) * scale;
				base_wind = v_world;
			}
			// A stem's first segment fails at the foot: the roots' test below covers it.
			if (bone.parent != 0 && bone.strength > 0.0f && bone.radius * scale >= storm_min_radius) {
				// Knots, old wounds and rot: each part of each tree is 65-115% as strong as clear wood.
				const float defect = (0.65f + 0.5f * hash01(p_seed, b)) * snag_wood;
				const float ratio = moment.length() / MAX(bone.strength * scale3 * storm_strength * defect, 1e-3f);
				if (ratio > r_storm->ratio) {
					r_storm->ratio = ratio;
					r_storm->bone = int(b);
					r_storm->wind = v_world;
					// Bowed by its snow more than pushed by the wind: it falls the way it leans.
					const float drag = 0.5f * AIR_DENSITY * cd * bone.area * scale * scale * vs * vs * drag_scale;
					if (snow_force.length() > drag) {
						const Vector3 out = rot.xform(bone.dir);
						r_storm->wind = Vector3(out.x, 0.0f, out.z) + v_world * 0.1f;
						r_storm->snow = true;
					}
				}
			}
		}
		// theta = moment / stiffness; the lever scales with scale and the stiffness with scale^3.
		Vector3 theta = bone.centroid.cross(force - snow_force) / MAX(bone.stiffness * scale * scale * stiffness_scale, 1e-3f);
		theta += bone.centroid.cross(snow_force) / MAX(bone.snow_stiffness * scale * scale * stiffness_scale, 1e-3f);
		theta -= bone.dir * theta.dot(bone.dir); // Bending only, no twist.
		if (p_touch && p_touch->active && b < p_touch->angle.size()) {
			theta += p_touch->angle[b];
		}
		const float angle = theta.length();
		Transform3D local;
		if (angle > 1e-6f) {
			const float bent = max_bend * Math::tanh(angle / max_bend);
			const Basis turn(theta / angle, bent);
			local = Transform3D(turn, bone.pivot - turn.xform(bone.pivot));
		}
		r_bones[b] = r_bones[MIN(uint32_t(bone.parent), b - 1)] * local;
		if (has_flat && p_touch->flat[b]) {
			// A stem segment snapped at its joint by a storm: its own wood is pressed flat onto the
			// joint's plane (under the fracture cap), so the stem below keeps its full girth up to the
			// break where its skin blends into this bone; collapsed to a point, it pinched to a cone.
			const Vector3 d = bone.dir.normalized();
			const Basis flat(Vector3(1.0f - d.x * d.x, -d.x * d.y, -d.x * d.z), Vector3(-d.y * d.x, 1.0f - d.y * d.y, -d.y * d.z), Vector3(-d.z * d.x, -d.z * d.y, 1.0f - d.z * d.z));
			r_bones[b] = r_bones[MIN(uint32_t(bone.parent), b - 1)] * Transform3D(flat, d * d.dot(bone.pivot));
		} else if (has_broken && p_touch->broken[b]) {
			// Snapped off: the bone's geometry collapses to its pivot (the piece is a rigid body now).
			r_bones[b] = Transform3D(Basis(Vector3(), Vector3(), Vector3()), r_bones[b].xform(bone.pivot));
		}
	}
	if (storm && p_rig.anchorage > 0.0f && !(has_broken && p_touch->broken[1])) {
		// The overturning moment at the foot, 15% more for the crown's weight carried out over the
		// lean (HWIND, Peltola et al. 1999); each tree's roots and soil 70-130% of the mean.
		const float defect = (0.7f + 0.6f * hash01(p_seed, 0x51u)) * snag_roots;
		r_storm->root_ratio = base_moment.length() * 1.15f / MAX(p_rig.anchorage * scale3 * storm_anchorage * defect, 1e-3f);
		// A fire-charred foot (docs/ideas/physx.md, "Burnt trees fall later"): the char eats the stem's section from
		// outside; under bark of about a tenth of its radius (5 mm to 3 cm), the wood lost leaves a section modulus of (r' / r)^3,
		// the char taken as half its depth round the stem (a catface on the side the flames leaned on, little on the
		// other). Hazard trees after fires fail this way, at the burnt base (Dickinson and Johnson 2001; Hood et al.).
		if (r_storm->base_char > 0.0f && p_rig.bones.size() > 1 && p_rig.bones[1].strength > 0.0f) {
			const Bone &foot = p_rig.bones[1];
			const float r = foot.radius * scale;
			const float wood = MAX(r_storm->base_char - CLAMP(0.1f * r, 0.005f, 0.03f), 0.0f) * 0.5f;
			if (wood > 0.001f && r > 0.0f) {
				const float left = MAX(1.0f - wood / r, 0.05f);
				const float wood_defect = (0.65f + 0.5f * hash01(p_seed, 1u)) * snag_roots;
				const float foot_ratio = base_moment.length() / MAX(foot.strength * scale3 * storm_strength * wood_defect * left * left * left, 1e-3f);
				if (foot_ratio > r_storm->root_ratio) {
					r_storm->root_ratio = foot_ratio;
					r_storm->base_snap = true;
				}
			}
		}
		// A snag dead over half a year fails at its rotten butt rather than lifting a root plate (snags break at the
		// base: Everett et al. 1999; Russell et al. 2006).
		if (r_storm->snag_age > 0.5f) {
			r_storm->base_snap = true;
		}
		if (r_storm->root_ratio > r_storm->ratio) {
			r_storm->wind = base_wind;
		}
	}
}

void TreeWind3D::_solve_job(uint32_t p_index, int p_unused) {
	Job &job = jobs[p_index];
	Target &t = *job.target;
	const Transform3D &placement = t.placements[job.instance];
	Transform3D bones[256];
	const Vector3 o = placement.origin;
	const uint32_t seed = hash_u32(uint32_t(int(o.x * 16.0f)) * 73856093u ^ uint32_t(int(o.z * 16.0f)) * 19349663u);
	const Touch *tc = t.touch.getptr(job.instance);
	float *w = t.write + size_t(job.instance) * t.bones * RECORD_FLOATS;
	if (tc && tc->uprooted) {
		// Thrown by a storm: the tree is a body now; the instance draws nothing.
		for (int b = 0; b < t.bones; b++) {
			float *f = w + b * RECORD_FLOATS;
			for (int k = 0; k < 12; k++) {
				f[k] = 0.0f;
			}
			f[3] = placement.origin.x;
			f[7] = placement.origin.y;
			f[11] = placement.origin.z;
		}
		return;
	}
	solve_pose(*t.rig, job.world * placement, seed, bones, tc, job.min_length, job.storm ? &job.load : nullptr, job.snow_kg);
	for (int b = 0; b < t.bones; b++) {
		const Transform3D r = placement * bones[b];
		float *f = w + b * RECORD_FLOATS;
		f[0] = r.basis.rows[0][0];
		f[1] = r.basis.rows[0][1];
		f[2] = r.basis.rows[0][2];
		f[3] = r.origin.x;
		f[4] = r.basis.rows[1][0];
		f[5] = r.basis.rows[1][1];
		f[6] = r.basis.rows[1][2];
		f[7] = r.origin.y;
		f[8] = r.basis.rows[2][0];
		f[9] = r.basis.rows[2][1];
		f[10] = r.basis.rows[2][2];
		f[11] = r.origin.z;
	}
}

void TreeWind3D::fill_rest(const Ref<MultiMesh> &p_multimesh, const Ref<Mesh> &p_mesh, const LocalVector<Transform3D> &p_transforms, const LocalVector<Color> &p_custom_data) {
	ERR_FAIL_COND(p_multimesh.is_null());
	const int k = MAX(get_mesh_bone_count(p_mesh), 1);
	p_multimesh->set_instance_count(0);
	p_multimesh->set_transform_format(MultiMesh::TRANSFORM_3D);
	p_multimesh->set_use_colors(true);
	p_multimesh->set_use_custom_data(true);
	p_multimesh->set_mesh(p_mesh);
	p_multimesh->set_bones_per_instance(k);
	const int n = p_transforms.size();
	p_multimesh->set_instance_count(n * k);
	if (n == 0) {
		return;
	}
	Vector<float> buffer;
	buffer.resize(n * k * RECORD_FLOATS);
	float *w = buffer.ptrw();
	for (int i = 0; i < n; i++) {
		const Transform3D &r = p_transforms[i];
		const Color c = i < int(p_custom_data.size()) ? p_custom_data[i] : Color(0, 0, 0, 0);
		for (int b = 0; b < k; b++) {
			float *f = w + (size_t(i) * k + b) * RECORD_FLOATS;
			f[0] = r.basis.rows[0][0];
			f[1] = r.basis.rows[0][1];
			f[2] = r.basis.rows[0][2];
			f[3] = r.origin.x;
			f[4] = r.basis.rows[1][0];
			f[5] = r.basis.rows[1][1];
			f[6] = r.basis.rows[1][2];
			f[7] = r.origin.y;
			f[8] = r.basis.rows[2][0];
			f[9] = r.basis.rows[2][1];
			f[10] = r.basis.rows[2][2];
			f[11] = r.origin.z;
			f[12] = 1.0f;
			f[13] = 1.0f;
			f[14] = 1.0f;
			f[15] = 1.0f;
			f[16] = c.r;
			f[17] = c.g;
			f[18] = c.b;
			f[19] = c.a;
		}
	}
	RenderingServer::get_singleton()->multimesh_set_buffer(p_multimesh->get_rid(), buffer);
}

Ref<MultiMesh> TreeWind3D::make_multimesh(const Ref<Mesh> &p_mesh, const TypedArray<Transform3D> &p_transforms, const PackedColorArray &p_custom_data) {
	Ref<MultiMesh> mm;
	mm.instantiate();
	LocalVector<Transform3D> xf;
	xf.resize(p_transforms.size());
	for (int i = 0; i < p_transforms.size(); i++) {
		xf[i] = p_transforms[i];
	}
	LocalVector<Color> cd;
	cd.resize(p_custom_data.size());
	for (int i = 0; i < p_custom_data.size(); i++) {
		cd[i] = p_custom_data[i];
	}
	fill_rest(mm, p_mesh, xf, cd);
	return mm;
}

void TreeWind3D::_scan_node(Node *p_node) {
	MultiMeshInstance3D *mmi = Object::cast_to<MultiMeshInstance3D>(p_node);
	if (mmi && mmi->get_multimesh().is_valid()) {
		Ref<MultiMesh> mm = mmi->get_multimesh();
		const Rig *rig = _rig_for(mm->get_mesh());
		const int k = mm->get_bones_per_instance();
		if (rig && k > 1 && int(rig->bones.size()) == k && mm->is_using_colors() && mm->is_using_custom_data() && mm->get_transform_format() == MultiMesh::TRANSFORM_3D) {
			const int n = mm->get_instance_count() / k;
			bool known = false;
			for (const Target &t : targets) {
				if (t.node == mmi->get_instance_id() && t.multimesh == mm && int(t.placements.size()) == n) {
					known = true;
					break;
				}
			}
			if (!known && n > 0) {
				Target t;
				t.node = mmi->get_instance_id();
				t.multimesh = mm;
				t.rig = rig;
				t.bones = k;
				t.buffer = RenderingServer::get_singleton()->multimesh_get_buffer(mm->get_rid());
				t.placements.resize(n);
				t.customs.resize(n);
				t.moving.resize(n);
				t.slot.resize(n);
				t.base_char.resize(n);
				t.snag_age.resize(n);
				t.avalanche_hit.resize(n);
				t.snow_kg.resize(n);
				t.snow_creep.resize(n);
				t.cap_level.resize(n);
				t.snow_seen.resize(n);
				t.glaze_kg.resize(n);
				t.glaze_shed.resize(n);
				for (int c = 0; c < n; c++) {
					t.glaze_kg[c] = 0.0f;
					t.glaze_shed[c] = 0.0f;
					t.cap_level[c] = 0;
					t.snow_seen[c] = 0.0f;
					t.snow_kg[c] = 0.0f;
					t.snow_creep[c] = 0.0f;
					t.base_char[c] = 0.0f;
					t.snag_age[c] = -1.0f;
					t.avalanche_hit[c] = 0;
				}
				const float *r = t.buffer.ptr();
				for (int i = 0; i < n; i++) {
					const float *f = r + size_t(i) * k * RECORD_FLOATS;
					Transform3D x;
					x.basis.rows[0] = Vector3(f[0], f[1], f[2]);
					x.basis.rows[1] = Vector3(f[4], f[5], f[6]);
					x.basis.rows[2] = Vector3(f[8], f[9], f[10]);
					x.origin = Vector3(f[3], f[7], f[11]);
					t.placements[i] = x;
					t.customs[i] = Color(f[16], f[17], f[18], f[19]);
					t.moving[i] = 0;
					t.slot[i] = uint8_t(hash_u32(uint32_t(i) * 2654435761u ^ uint32_t(n)) & 0xffu);
				}
				targets.push_back(t);
			}
		}
	}
	for (int i = 0; i < p_node->get_child_count(true); i++) {
		_scan_node(p_node->get_child(i, true));
	}
}

void TreeWind3D::_scan() {
	// Drop targets whose node or MultiMesh went away or changed.
	for (int i = int(targets.size()) - 1; i >= 0; i--) {
		MultiMeshInstance3D *mmi = ObjectDB::get_instance<MultiMeshInstance3D>(targets[i].node);
		if (!mmi || mmi->get_multimesh() != targets[i].multimesh || targets[i].multimesh->get_instance_count() != int(targets[i].placements.size()) * targets[i].bones) {
			_free_colliders(targets[i]);
			_free_caps(targets[i]);
			targets.remove_at(i);
		}
	}
	Node *root = get_node_or_null(target_path);
	if (root) {
		// A streaming VegetationField3D adds and frees cells as the camera moves and says so with
		// "populated": scan on the next update rather than the next once-a-second sweep, so new
		// cells never show a frame of rest pose.
		const Callable on_populated = callable_mp(this, &TreeWind3D::_on_target_populated);
		if (root->has_signal(SNAME("populated")) && !root->is_connected(SNAME("populated"), on_populated)) {
			root->connect(SNAME("populated"), on_populated);
		}
		// Near cells built on demand (PlantSpecies.near_mesh_source) appear one at a time as
		// children of the field, without "populated".
		const Callable on_child = callable_mp(this, &TreeWind3D::_on_target_child);
		if (!root->is_connected(SNAME("child_entered_tree"), on_child)) {
			root->connect(SNAME("child_entered_tree"), on_child);
		}
		_scan_node(root);
	}
}

void TreeWind3D::_on_target_populated() {
	rescan_timer = 0.0f;
}

void TreeWind3D::_on_target_child(Node *p_child) {
	if (Object::cast_to<MultiMeshInstance3D>(p_child)) {
		rescan_timer = 0.0f;
	}
}

void TreeWind3D::rescan() {
	for (Target &t : targets) {
		_free_colliders(t);
		_free_caps(t);
	}
	targets.clear();
	rigs.clear();
	_scan();
}

void TreeWind3D::update(double p_delta) {
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	time += p_delta;
	rescan_timer -= float(p_delta);
	if (rescan_timer <= 0.0f) {
		_scan();
		rescan_timer = 1.0f;
	}
	_gather_waves();
	Vector3 eye = is_inside_tree() ? get_global_position() : Vector3();
	// Level of detail (the significance scheme of Unreal's Animation Budget Allocator and the
	// wind qualities per LOD of SpeedTree; docs/ideas/trees.md section 3c): a tree's pose is solved
	// at a rate and with the bones its size on screen can show, so the pose error stays under
	// lod_pixel_error pixels. Without a camera every tree is solved in full.
	Camera3D *cam = nullptr;
	if (is_inside_tree() && get_viewport() && get_viewport()->get_camera_3d()) {
		cam = get_viewport()->get_camera_3d();
		eye = cam->get_global_position();
	}
	last_eye = eye;
	bool lod = lod_enabled && cam && cam->get_projection() == Camera3D::PROJECTION_PERSPECTIVE;
	Vector<Plane> frustum;
	float px_per_rad = 0.0f; // Pixels per metre at 1 m.
	if (lod) {
		frustum = cam->get_frustum();
		const float h_px = MAX(get_viewport()->get_visible_rect().size.height, 1.0f);
		const float half = Math::deg_to_rad(cam->get_fov() * 0.5f);
		// fov is vertical unless the camera keeps the width.
		const Size2 vs = get_viewport()->get_visible_rect().size;
		const float span = cam->get_keep_aspect_mode() == Camera3D::KEEP_WIDTH ? MAX(vs.width, 1.0f) : h_px;
		px_per_rad = 0.5f * span / MAX(Math::tan(half), 1e-3f);
	}
	frame_index++;
	const float dt = CLAMP(float(p_delta), 1.0f / 240.0f, 0.1f);
	// Storm loads are tested only while gusts (about 1.6 times the mean) can pass storm_min_wind.
	const bool storm_frame = storm_enabled && base_speed * 1.6f >= storm_min_wind && is_inside_tree();
	int skipped = 0;
	jobs.clear();
	// Fire-charred stem feet (WorldFire.stem_char_at), looked up about once a second while a fire is live.
	char_timer -= float(p_delta);
	if (char_timer <= 0.0f) {
		char_timer = 1.0f;
		const WorldFire *fire = WorldContracts::fire();
		if (fire && fire->has_provider()) {
			for (Target &t : targets) {
				MultiMeshInstance3D *cm = ObjectDB::get_instance<MultiMeshInstance3D>(t.node);
				if (!cm || !cm->is_inside_tree() || t.base_char.size() != t.placements.size()) {
					continue;
				}
				const Transform3D cw = cm->get_global_transform();
				for (uint32_t i = 0; i < t.placements.size(); i++) {
					const Vector3 at = cw.xform(t.placements[i].origin);
					t.base_char[i] = MAX(t.base_char[i], fire->stem_char_at(at));
					_update_snag(t, i, cw * t.placements[i], at, fire);
				}
			}
		}
		_avalanche_check();
		_snow_check(1.0f);
		_glaze_check();
		_cut_canopy();
		_report_windthrow();
	} else {
		snow_frame = false;
	}
	_gather_contacts(p_delta);
	LocalVector<Target *> changed;
	for (Target &t : targets) {
		MultiMeshInstance3D *mmi = ObjectDB::get_instance<MultiMeshInstance3D>(t.node);
		if (!mmi || !t.rig) {
			continue;
		}
		const Transform3D world = mmi->is_inside_tree() ? mmi->get_global_transform() : Transform3D();
		t.write = t.buffer.ptrw();
		bool any = false;
		if (collision_enabled) {
			_update_colliders(t, world, eye);
		}
		for (uint32_t i = 0; i < t.placements.size(); i++) {
			const Vector3 at = world.xform(t.placements[i].origin);
			const bool touched = touch_enabled && _touch_instance(t, int(i), world * t.placements[i], p_delta);
			const float dist = at.distance_to(eye);
			if (touched || dist <= wind_range) {
				float min_length = 0.0f;
				if (lod && !touched) {
					const float scale = MAX(Math::pow(float(Math::abs(world.basis.determinant() * t.placements[i].basis.determinant())), 1.0f / 3.0f), 1e-3f);
					const float extent = t.rig->extent * scale;
					const Vector3 mid = at + Vector3(0.0f, 0.5f * extent, 0.0f);
					// Out of view, with room for the shadow it casts into view: held at a low rate,
					// so reflections and a turn of the camera find it near its pose.
					bool seen = true;
					for (const Plane &pl : frustum) {
						if (pl.distance_to(mid) > extent * 2.5f) {
							seen = false;
							break;
						}
					}
					const float px_per_m = px_per_rad / MAX(dist, 0.1f);
					const float err = lod_pixel_error * lod_bias;
					// A crown sways its height times about 0.04 per second at its tip (Moore and
					// Maguire 2004: 0.2-0.6 Hz, tips a few per cent of the height); a pose held
					// for n frames lags by n times that per frame.
					const float drift_px = 0.04f * extent * dt * px_per_m * MAX(base_speed / 8.0f, 0.5f);
					// Past 200 m (the wind reaches 1 km) a tree a few pixels tall may hold its
					// pose up to 32 frames; nearer, 8 keeps the sway's curve inside the bound.
					int period = CLAMP(int(err / MAX(drift_px, 1e-6f)), 1, dist > 200.0f ? 32 : 8);
					if (!seen) {
						period = MAX(period, 8);
					}
					if (period > 1 && (frame_index + t.slot[i]) % uint64_t(period) != 0) {
						skipped++;
						continue;
					}
					// A bone bends some 0.15 rad at most in a strong wind; shorter than this, its
					// own turn moves its tip less than the error on screen.
					min_length = err / (0.15f * px_per_m * scale);
				}
				Job job;
				job.target = &t;
				job.instance = int(i);
				job.world = world;
				job.min_length = min_length;
				job.snow_kg = (i < t.snow_kg.size() ? t.snow_kg[i] : 0.0f) + (i < t.glaze_kg.size() ? t.glaze_kg[i] : 0.0f);
				job.storm = (storm_frame || (snow_frame && job.snow_kg > 1.0f)) && t.rig->storm;
				if (i < t.base_char.size()) {
					job.load.base_char = t.base_char[i];
				}
				if (i < t.snag_age.size()) {
					job.load.snag_age = t.snag_age[i];
				}
				jobs.push_back(job);
				t.moving[i] = 1;
				any = true;
			} else if (t.moving[i]) {
				// Leaving the range: back to rest.
				const Transform3D &r = t.placements[i];
				for (int b = 0; b < t.bones; b++) {
					float *f = t.write + (size_t(i) * t.bones + b) * RECORD_FLOATS;
					f[0] = r.basis.rows[0][0];
					f[1] = r.basis.rows[0][1];
					f[2] = r.basis.rows[0][2];
					f[3] = r.origin.x;
					f[4] = r.basis.rows[1][0];
					f[5] = r.basis.rows[1][1];
					f[6] = r.basis.rows[1][2];
					f[7] = r.origin.y;
					f[8] = r.basis.rows[2][0];
					f[9] = r.basis.rows[2][1];
					f[10] = r.basis.rows[2][2];
					f[11] = r.origin.z;
				}
				t.moving[i] = 0;
				any = true;
			}
		}
		if (any) {
			changed.push_back(&t);
		}
	}
	if (!jobs.is_empty()) {
		WorkerThreadPool::GroupID g = WorkerThreadPool::get_singleton()->add_template_group_task(this, &TreeWind3D::_solve_job, 0, jobs.size(), -1, true, String("TreeWind3D"));
		WorkerThreadPool::get_singleton()->wait_for_group_task_completion(g);
	}
	if (storm_frame || snow_frame) {
		for (uint32_t j = 0; j < jobs.size(); j++) {
			const Job &job = jobs[j];
			if (job.storm && (job.load.ratio > 1.0f || job.load.root_ratio > 1.0f)) {
				_storm_fail(*job.target, job.instance, job.world, job.load);
			}
		}
	}
	if (!avalanche_pending.is_empty()) {
		_avalanche_fail(changed);
	}
	if (!storm_parts.is_empty() || !caps.is_empty()) {
		_storm_parts(p_delta);
	}
	for (Target *t : changed) {
		RenderingServer::get_singleton()->multimesh_set_buffer(t->multimesh->get_rid(), t->buffer);
	}
	caps_drawn = 0;
	for (Target &t : targets) {
		bool moved = false;
		for (Target *c : changed) {
			moved = moved || c == &t;
		}
		_update_caps(t, moved);
	}
	_step_clumps(p_delta);
	solved_instances = jobs.size();
	skipped_instances = skipped;
	solve_usec = OS::get_singleton()->get_ticks_usec() - t0;
	if (budget_msec > 0.0f && lod) {
		// Over the budget the LOD coarsens; well under it, it relaxes back to the set error.
		const float ms = solve_usec / 1000.0f;
		if (ms > budget_msec) {
			lod_bias = MIN(lod_bias * 1.15f, 8.0f);
		} else if (ms < 0.7f * budget_msec) {
			lod_bias = MAX(lod_bias / 1.05f, 1.0f);
		}
	} else {
		lod_bias = 1.0f;
	}
}

void TreeWind3D::_gather_contacts(double p_delta) {
	contacts.clear();
	if (!is_inside_tree() || !touch_enabled) {
		return;
	}
	// The touch group and world_contact, every body any system should feel (docs/ideas/world-links.md).
	Vector<Node *> nodes = get_tree()->get_nodes_in_group(touch_group);
	nodes.append_array(get_tree()->get_nodes_in_group(SNAME("world_contact")));
	HashMap<ObjectID, Vector3> seen;
	for (Node *n : nodes) {
		Node3D *n3 = Object::cast_to<Node3D>(n);
		if (!n3 || !n3->is_inside_tree() || seen.has(n3->get_instance_id())) {
			continue;
		}
		const Vector3 pos = n3->get_global_position();
		const Vector3 *last = contact_last.getptr(n3->get_instance_id());
		Vector3 vel = last && p_delta > 1e-5 ? (pos - *last) / float(p_delta) : Vector3();
		if (vel.length_squared() > 30.0f * 30.0f) {
			vel = Vector3(); // Teleported, not moving.
		}
		seen[n3->get_instance_id()] = pos;
		// A touch_radius meta: one sphere of that radius at the origin, as before (an override).
		if (n3->has_meta(SNAME("touch_radius"))) {
			Contact c;
			c.position = pos;
			c.velocity = vel;
			c.radius = n3->get_meta(SNAME("touch_radius"));
			contacts.push_back(c);
			continue;
		}
		// Else its real collision geometry as spheres, each with its own velocity (WorldContracts::contact_spheres, the
		// grass's contact; docs/ideas/trees.md 7a): a ragdoll's bones, a vehicle's body and tyres, reported contact
		// points. A node with no shape at all keeps touch_radius at its origin.
		WorldContracts::contact_spheres(n3, contact_sp, contact_vel, vel);
		if (contact_sp.size() == 1 && WorldContracts::body_extent(n3, -1.0f).x < 0.0f) {
			contact_sp[0].w = touch_radius;
		}
		for (uint32_t k = 0; k < contact_sp.size(); k++) {
			Contact c;
			c.position = Vector3(contact_sp[k].x, contact_sp[k].y, contact_sp[k].z);
			c.velocity = contact_vel[k];
			c.radius = contact_sp[k].w;
			contacts.push_back(c);
		}
	}
	contact_last = seen;
}

// Touch bending (docs/ideas/trees.md section 7a): bodies in touch_group push the bones they reach
// out of their sphere, each bone yielding as its stiffness allows (a twig all the way, a trunk not
// at all), and spring back on the bone's own frequency and damping. Fast bodies that bend a thin
// branch far snap it off.
bool TreeWind3D::_touch_instance(Target &r_target, int p_instance, const Transform3D &p_tree, double p_delta) {
	Touch *tc = r_target.touch.getptr(p_instance);
	if (contacts.is_empty() && !tc) {
		return false;
	}
	const Rig &rig = *r_target.rig;
	const uint32_t n = rig.bones.size();
	const float scale = MAX(Math::pow(float(Math::abs(p_tree.basis.determinant())), 1.0f / 3.0f), 1e-3f);
	const Transform3D inv = p_tree.affine_inverse();
	bool contact = false;
	LocalVector<Vector3> want;
	for (const Contact &c : contacts) {
		const Vector3 cl = inv.xform(c.position);
		const float r = c.radius / scale;
		if (cl.length() > rig.extent + r) {
			continue;
		}
		if (want.is_empty()) {
			want.resize(n);
			for (Vector3 &w : want) {
				w = Vector3();
			}
		}
		for (uint32_t b = 1; b < n; b++) {
			const Bone &bone = rig.bones[b];
			const Vector3 p = bone.pivot;
			const float t = CLAMP((cl - p).dot(bone.dir), 0.0f, bone.length);
			const Vector3 x = p + bone.dir * t;
			const float lever = t;
			if (lever < 0.2f) {
				continue;
			}
			const Vector3 away = x - cl;
			const float d = away.length();
			const float pen = r + bone.reach - d;
			if (pen <= 0.0f) {
				continue;
			}
			Vector3 axis = (x - p).cross(away);
			if (axis.length_squared() < 1e-10f) {
				axis = bone.dir.cross(Vector3(0, 1, 0));
				if (axis.length_squared() < 1e-10f) {
					continue;
				}
			}
			const float bend = pen / lever;
			const float yield = MIN(1.0f, touch_stiffness / MAX(bone.stiffness * scale * scale * scale, 1e-3f));
			const Vector3 turn = axis.normalized() * MIN(bend * yield, 0.6f);
			if (turn.length_squared() > want[b].length_squared()) {
				want[b] = turn;
			}
			contact = true;
			// A fast body bending a thin branch past about 35 degrees snaps it.
			if (bend * yield > 0.6f && bone.radius * scale < break_radius && c.velocity.length() > break_speed) {
				if (!tc) {
					tc = &r_target.touch[p_instance];
				}
				if (tc->broken.size() != n) {
					tc->broken.resize(n);
					memset(tc->broken.ptr(), 0, n);
				}
				if (!tc->broken[b]) {
					_snap(r_target, p_instance, p_tree, int(b), c.velocity);
				}
			}
		}
	}
	if (!contact && !tc) {
		return false;
	}
	if (!tc) {
		tc = &r_target.touch[p_instance];
	}
	if (tc->angle.size() != n) {
		tc->angle.resize(n);
		tc->rate.resize(n);
		tc->target.resize(n);
		for (uint32_t b = 0; b < n; b++) {
			tc->angle[b] = Vector3();
			tc->rate[b] = Vector3();
		}
	}
	if (tc->broken.size() != n) {
		tc->broken.resize(n);
		memset(tc->broken.ptr(), 0, n);
	}
	// Each bone's touch turn is a damped spring toward the pushed pose, at the bone's own
	// frequency, stepped finely enough to stay stable for twigs.
	float largest = 0.0f;
	bool broken = false;
	for (uint32_t b = 1; b < n; b++) {
		const Bone &bone = rig.bones[b];
		tc->target[b] = want.is_empty() ? Vector3() : want[b];
		const float omega = MAX(bone.omega / scale, float(Math::TAU) * 0.5f);
		const float zeta = CLAMP(bone.zeta * 3.0f, 0.5f, 1.0f); // Leaves and twigs rub: more damping than in free sway.
		const int steps = CLAMP(int(Math::ceil(omega * float(p_delta) / 0.25f)), 1, 64);
		const float h = float(p_delta) / steps;
		for (int k = 0; k < steps; k++) {
			const Vector3 acc = (tc->target[b] - tc->angle[b]) * (omega * omega) - tc->rate[b] * (2.0f * zeta * omega);
			tc->rate[b] += acc * h;
			tc->angle[b] += tc->rate[b] * h;
		}
		largest = MAX(largest, MAX(tc->angle[b].length(), tc->rate[b].length() * 0.1f));
		broken = broken || tc->broken[b];
	}
	tc->active = contact || largest > 2e-3f || broken;
	if (!tc->active) {
		r_target.touch.erase(p_instance);
		return false;
	}
	return true;
}

// The bones on a snapped branch, as a mesh of their own (cut from the tree's surfaces once).
Ref<ArrayMesh> TreeWind3D::_piece_mesh(const Rig &p_rig, int p_bone, const LocalVector<uint8_t> *p_exclude, uint64_t p_key, float p_cut, int p_cut_side) {
	const uint64_t key = p_key ? p_key : (p_rig.mesh.is_valid() ? uint64_t(p_rig.mesh->get_instance_id()) : 0u) * 1315423911u + uint64_t(p_bone);
	if (piece_meshes.has(key)) {
		return piece_meshes[key];
	}
	Ref<ArrayMesh> piece;
	piece.instantiate();
	if (p_rig.mesh.is_null()) {
		return piece;
	}
	const uint32_t n = p_rig.bones.size();
	LocalVector<uint8_t> inside;
	inside.resize(n);
	for (uint32_t b = 0; b < n; b++) {
		inside[b] = b == uint32_t(p_bone) || (b > uint32_t(p_bone) && b > 0 && inside[p_rig.bones[b].parent]);
		if (b > 0 && p_exclude && p_exclude->size() == n && (*p_exclude)[b]) {
			inside[b] = 0; // Broken off before, its own part.
		}
	}
	for (int s = 0; s < p_rig.mesh->get_surface_count(); s++) {
		const Ref<ArrayMesh> named = p_rig.mesh;
		const bool bark = named.is_valid() && named->surface_get_name(s) == "bark";
		if (p_cut_side < 0 && !bark) {
			continue; // A stump is bare wood and bark.
		}
		const Array arr = p_rig.mesh->surface_get_arrays(s);
		const PackedVector3Array verts = arr[Mesh::ARRAY_VERTEX];
		const PackedVector3Array normals = arr[Mesh::ARRAY_NORMAL];
		const PackedVector2Array uvs = arr[Mesh::ARRAY_TEX_UV];
		const PackedColorArray colors = arr[Mesh::ARRAY_COLOR];
		const PackedInt32Array bones = arr[Mesh::ARRAY_BONES];
		const PackedInt32Array index = arr[Mesh::ARRAY_INDEX];
		const PackedVector2Array uv2s = arr[Mesh::ARRAY_TEX_UV2];
		const PackedFloat32Array tangents = arr[Mesh::ARRAY_TANGENT];
		const bool has_uv2 = uv2s.size() == verts.size();
		const bool has_tan = tangents.size() == verts.size() * 4;
		PackedVector2Array ou2;
		PackedFloat32Array ot;
		if (bones.size() != verts.size() * 4 || index.is_empty()) {
			continue;
		}
		HashMap<int, int> remap;
		PackedVector3Array ov, on;
		PackedVector2Array ou;
		PackedColorArray oc;
		PackedInt32Array oi;
		// A vertex where a triangle crosses the cut, interpolated between two of its own.
		auto emit_mix = [&](int p_a, int p_b, float p_t) {
			ov.push_back(verts[p_a].lerp(verts[p_b], p_t));
			const Vector3 na = p_a < normals.size() ? normals[p_a] : Vector3(0, 1, 0);
			const Vector3 nb = p_b < normals.size() ? normals[p_b] : Vector3(0, 1, 0);
			on.push_back(na.lerp(nb, p_t).normalized());
			ou.push_back((p_a < uvs.size() ? uvs[p_a] : Vector2()).lerp(p_b < uvs.size() ? uvs[p_b] : Vector2(), p_t));
			oc.push_back((p_a < colors.size() ? colors[p_a] : Color(1, 1, 1)).lerp(p_b < colors.size() ? colors[p_b] : Color(1, 1, 1), p_t));
			if (has_uv2) {
				ou2.push_back(uv2s[p_a].lerp(uv2s[p_b], p_t));
			}
			if (has_tan) {
				for (int q = 0; q < 4; q++) {
					ot.push_back(q < 3 ? Math::lerp(tangents[p_a * 4 + q], tangents[p_b * 4 + q], p_t) : tangents[p_a * 4 + 3]);
				}
			}
			return ov.size() - 1;
		};
		auto emit_vertex = [&](int v) {
			const int *got = remap.getptr(v);
			if (got) {
				return *got;
			}
			const int ni = ov.size();
			remap[v] = ni;
			ov.push_back(verts[v]);
			on.push_back(v < normals.size() ? normals[v] : Vector3(0, 1, 0));
			ou.push_back(v < uvs.size() ? uvs[v] : Vector2());
			oc.push_back(v < colors.size() ? colors[v] : Color(1, 1, 1));
			if (has_uv2) {
				ou2.push_back(uv2s[v]);
			}
			if (has_tan) {
				for (int q = 0; q < 4; q++) {
					ot.push_back(tangents[v * 4 + q]);
				}
			}
			return ni;
		};
		for (int i = 0; i + 2 < index.size(); i += 3) {
			const int bb = bones[index[i] * 4];
			if (bb < 0 || bb >= int(n) || !inside[bb]) {
				continue;
			}
			if (p_cut_side < 0 && bb > 1) {
				continue;
			}
			if (p_cut_side != 0 && bb <= 1) {
				// Clipped at the cut (Sutherland-Hodgman on one plane): the kept side has d >= 0.
				float d[3];
				int keep = 0;
				for (int k = 0; k < 3; k++) {
					d[k] = (verts[index[i + k]].y - p_cut) * float(p_cut_side);
					keep += d[k] >= 0.0f ? 1 : 0;
				}
				if (keep == 0) {
					continue;
				}
				if (keep < 3) {
					int poly[4];
					int count = 0;
					for (int k = 0; k < 3; k++) {
						const int a = index[i + k], b2 = index[i + (k + 1) % 3];
						const float da = d[k], db = d[(k + 1) % 3];
						if (da >= 0.0f) {
							poly[count++] = emit_vertex(a);
						}
						if ((da >= 0.0f) != (db >= 0.0f)) {
							poly[count++] = emit_mix(a, b2, da / (da - db));
						}
					}
					for (int k = 1; k + 1 < count; k++) {
						oi.push_back(poly[0]);
						oi.push_back(poly[k]);
						oi.push_back(poly[k + 1]);
					}
					continue;
				}
			}
			for (int k = 0; k < 3; k++) {
				const int v = index[i + k];
				const int *got = remap.getptr(v);
				if (got) {
					oi.push_back(*got);
					continue;
				}
				const int ni = ov.size();
				remap[v] = ni;
				ov.push_back(verts[v]);
				on.push_back(v < normals.size() ? normals[v] : Vector3(0, 1, 0));
				ou.push_back(v < uvs.size() ? uvs[v] : Vector2());
				oc.push_back(v < colors.size() ? colors[v] : Color(1, 1, 1));
				if (has_uv2) {
					ou2.push_back(uv2s[v]);
				}
				if (has_tan) {
					for (int q = 0; q < 4; q++) {
						ot.push_back(tangents[v * 4 + q]);
					}
				}
				oi.push_back(ni);
			}
		}
		if (oi.is_empty()) {
			continue;
		}
		Array out;
		out.resize(Mesh::ARRAY_MAX);
		out[Mesh::ARRAY_VERTEX] = ov;
		out[Mesh::ARRAY_NORMAL] = on;
		out[Mesh::ARRAY_TEX_UV] = ou;
		out[Mesh::ARRAY_COLOR] = oc;
		out[Mesh::ARRAY_INDEX] = oi;
		if (has_uv2) {
			out[Mesh::ARRAY_TEX_UV2] = ou2;
		}
		if (has_tan) {
			out[Mesh::ARRAY_TANGENT] = ot;
		}
		piece->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, out);
		if (s < p_rig.static_materials.size() && Ref<Material>(p_rig.static_materials[s]).is_valid()) {
			// The generator's unskinned twin of the surface's material (lichen and flutter shaders too).
			piece->surface_set_material(piece->get_surface_count() - 1, p_rig.static_materials[s]);
			const Ref<ArrayMesh> am = p_rig.mesh;
			if (am.is_valid()) {
				piece->surface_set_name(piece->get_surface_count() - 1, am->surface_get_name(s));
			}
			continue;
		}
		Ref<BaseMaterial3D> m = p_rig.mesh->surface_get_material(s);
		if (m.is_valid()) {
			m = m->duplicate();
			m->set_flag(BaseMaterial3D::FLAG_PARTICLE_TRAILS_MODE, false);
			piece->surface_set_material(piece->get_surface_count() - 1, m);
		}
	}
	piece_meshes[key] = piece;
	return piece;
}

// A snapped branch becomes a rigid body (PhysX when the physics server is PhysX), thrown with part
// of the body's speed; its bones collapse on the tree.
void TreeWind3D::_snap(Target &r_target, int p_instance, const Transform3D &p_tree, int p_bone, const Vector3 &p_velocity) {
	Touch &tc = r_target.touch[p_instance];
	const Rig &rig = *r_target.rig;
	const uint32_t n = rig.bones.size();
	if (tc.broken.size() != n) {
		tc.broken.resize(n);
		memset(tc.broken.ptr(), 0, n);
	}
	for (uint32_t b = uint32_t(p_bone); b < n; b++) {
		if (b == uint32_t(p_bone) || (b > 0 && tc.broken[rig.bones[b].parent])) {
			tc.broken[b] = 1;
		}
	}
	if (!is_inside_tree()) {
		return;
	}
	const Bone &bone = rig.bones[p_bone];
	const float scale = MAX(Math::pow(float(Math::abs(p_tree.basis.determinant())), 1.0f / 3.0f), 1e-3f);
	const Basis rot = p_tree.basis.orthonormalized();
	RigidBody3D *body = memnew(RigidBody3D);
	body->set_name("TreePiece");
	body->set_as_top_level(true);
	MeshInstance3D *mi = memnew(MeshInstance3D);
	mi->set_mesh(_piece_mesh(rig, p_bone));
	mi->set_scale(Vector3(scale, scale, scale));
	body->add_child(mi);
	CollisionShape3D *cs = memnew(CollisionShape3D);
	Ref<CapsuleShape3D> cap;
	cap.instantiate();
	const float r = MAX(bone.reach * 0.5f, 0.05f) * scale;
	cap->set_radius(r);
	cap->set_height(MAX(bone.length * scale, 2.0f * r + 0.01f));
	cs->set_shape(cap);
	const Vector3 mid = (bone.pivot + bone.dir * (bone.length * 0.5f)) * scale;
	cs->set_transform(Transform3D(Basis(Quaternion(Vector3(0, 1, 0), bone.dir.normalized())), mid));
	body->add_child(cs);
	body->set_center_of_mass_mode(RigidBody3D::CENTER_OF_MASS_MODE_CUSTOM);
	body->set_center_of_mass(mid);
	body->set_mass(MAX(bone.mass * scale * scale * scale, 0.5f));
	add_child(body);
	body->set_global_transform(Transform3D(rot, p_tree.origin));
	body->set_linear_velocity(p_velocity * 0.5f);
	pieces.push_back(body->get_instance_id());
	while (int(pieces.size()) > max_pieces) {
		Node *old = ObjectDB::get_instance<Node>(pieces[0]);
		if (old) {
			old->queue_free();
		}
		pieces.remove_at(0);
	}
}

static Transform3D read_record(const float *p_f) {
	Transform3D r;
	r.basis.rows[0] = Vector3(p_f[0], p_f[1], p_f[2]);
	r.basis.rows[1] = Vector3(p_f[4], p_f[5], p_f[6]);
	r.basis.rows[2] = Vector3(p_f[8], p_f[9], p_f[10]);
	r.origin = Vector3(p_f[3], p_f[7], p_f[11]);
	return r;
}

static void write_record(float *p_f, const Transform3D &p_r) {
	p_f[0] = p_r.basis.rows[0][0];
	p_f[1] = p_r.basis.rows[0][1];
	p_f[2] = p_r.basis.rows[0][2];
	p_f[3] = p_r.origin.x;
	p_f[4] = p_r.basis.rows[1][0];
	p_f[5] = p_r.basis.rows[1][1];
	p_f[6] = p_r.basis.rows[1][2];
	p_f[7] = p_r.origin.y;
	p_f[8] = p_r.basis.rows[2][0];
	p_f[9] = p_r.basis.rows[2][1];
	p_f[10] = p_r.basis.rows[2][2];
	p_f[11] = p_r.origin.z;
}

// A storm kit's [name, arrays, material] triples (TreeGenerator storm_kit) as a mesh, made once.
Ref<ArrayMesh> TreeWind3D::_kit_mesh(const Rig &p_rig, bool p_plate) {
	const uint64_t key = (p_rig.mesh.is_valid() ? uint64_t(p_rig.mesh->get_instance_id()) : 0u) * 2u + (p_plate ? 1u : 0u);
	if (kit_meshes.has(key)) {
		return kit_meshes[key];
	}
	Ref<ArrayMesh> m;
	m.instantiate();
	// The root plate is built only when a tree is thrown (some 80-100k triangles, a few hundred ms).
	const Array kit = p_plate ? TreeGenerator::build_storm_plate(p_rig.storm_data) : p_rig.cap_kit;
	Ref<Material> bark;
	const Ref<ArrayMesh> am = p_rig.mesh;
	for (int s = 0; am.is_valid() && s < am->get_surface_count() && s < p_rig.static_materials.size(); s++) {
		if (am->surface_get_name(s) == "bark") {
			bark = p_rig.static_materials[s];
		}
	}
	for (int i = 0; i < kit.size(); i++) {
		const Array e = kit[i];
		if (e.size() < 3) {
			continue;
		}
		m->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, e[1]);
		m->surface_set_name(m->get_surface_count() - 1, e[0]);
		const Ref<Material> mat = e[2];
		m->surface_set_material(m->get_surface_count() - 1, mat.is_valid() ? mat : bark);
	}
	kit_meshes[key] = m;
	return m;
}

// Writes an instance's pose now (after a break), so the frame never shows the part twice.
void TreeWind3D::_collapse(Target &r_target, int p_instance, const Transform3D &p_world) {
	const Transform3D &placement = r_target.placements[p_instance];
	const Touch *tc = r_target.touch.getptr(p_instance);
	float *w = r_target.write + size_t(p_instance) * r_target.bones * RECORD_FLOATS;
	if (tc && tc->uprooted) {
		for (int b = 0; b < r_target.bones; b++) {
			write_record(w + b * RECORD_FLOATS, Transform3D(Basis(Vector3(), Vector3(), Vector3()), placement.origin));
		}
		return;
	}
	Transform3D bones[256];
	const Vector3 o = placement.origin;
	const uint32_t seed = hash_u32(uint32_t(int(o.x * 16.0f)) * 73856093u ^ uint32_t(int(o.z * 16.0f)) * 19349663u);
	solve_pose(*r_target.rig, p_world * placement, seed, bones, tc, 0.0f);
	for (int b = 0; b < r_target.bones; b++) {
		write_record(w + b * RECORD_FLOATS, placement * bones[b]);
	}
}

// The part of a tree above bone p_bone (the whole tree for 0) as a rigid body at p_at, with all its
// surfaces, the capsules of its wood, and its mass and centre of mass from the rig.
RigidBody3D *TreeWind3D::_make_part(Target &r_target, int p_instance, const Transform3D &p_tree, int p_bone, const Transform3D &p_at, const LocalVector<uint8_t> &p_exclude, uint64_t p_key) {
	const Rig &rig = *r_target.rig;
	const uint32_t n = rig.bones.size();
	const float scale = MAX(Math::pow(float(Math::abs(p_tree.basis.determinant())), 1.0f / 3.0f), 1e-3f);
	LocalVector<uint8_t> inside;
	inside.resize(n);
	for (uint32_t b = 0; b < n; b++) {
		inside[b] = b == uint32_t(p_bone) || (b > uint32_t(p_bone) && b > 0 && inside[rig.bones[b].parent]);
		if (b > 0 && p_exclude.size() == n && p_exclude[b]) {
			inside[b] = 0;
		}
	}
	RigidBody3D *body = memnew(RigidBody3D);
	body->set_name("TreePart");
	body->set_as_top_level(true);
	// Its contacts, for the Blast swap at its first hard impact.
	body->set_contact_monitor(true);
	body->set_max_contacts_reported(8);
	MeshInstance3D *mi = memnew(MeshInstance3D);
	mi->set_mesh(_piece_mesh(rig, p_bone, &p_exclude, p_key, making_snap ? snap_cut : 0.0f, making_snap ? 1 : 0));
	mi->set_scale(Vector3(scale, scale, scale));
	body->add_child(mi);
	// Its wood's capsules (TreeGenerator collider_radius and up).
	int shapes = 0;
	const float *c = rig.colliders.ptr();
	for (int k = 0; k < rig.colliders.size() / 8; k++) {
		const float *o = c + k * 8;
		const int bone = int(o[7]);
		if (bone < 0 || bone >= int(n) || !inside[bone] || ((p_bone > 0 || making_snap) && bone == 0)) {
			continue;
		}
		Vector3 a = Vector3(o[0], o[1], o[2]) * scale;
		Vector3 bb = Vector3(o[3], o[4], o[5]) * scale;
		const float r = o[6] * scale;
		if (making_snap && bone <= 1) {
			// The stem's wood below the cut stays on the stump.
			const float cut = snap_cut * scale;
			if (a.y < cut && bb.y < cut) {
				continue;
			}
			if (a.y < cut) {
				a = a.lerp(bb, (cut - a.y) / (bb.y - a.y));
			} else if (bb.y < cut) {
				bb = bb.lerp(a, (cut - bb.y) / (a.y - bb.y));
			}
		}
		if (a.distance_to(bb) < 1e-3f) {
			continue;
		}
		// A tree burnt through at its foot stood in a fire that scorched its low limbs brittle
		// (crown scorch, Van Wagner 1973): they snap under it as it lands instead of propping it
		// (a branch's capsules ride the stem bone it grows on: the ones leaning out).
		if (making_snap && Math::abs((bb - a).normalized().y) < 0.7f && MIN(a.y, bb.y) < 0.35f * rig.extent * scale) {
			continue;
		}
		Ref<CapsuleShape3D> cap;
		cap.instantiate();
		cap->set_radius(r);
		cap->set_height(a.distance_to(bb) + 2.0f * r);
		CollisionShape3D *cs = memnew(CollisionShape3D);
		cs->set_shape(cap);
		cs->set_transform(Transform3D(Basis(Quaternion(Vector3(0, 1, 0), (bb - a).normalized())), (a + bb) * 0.5f));
		body->add_child(cs);
		shapes++;
	}
	const Bone &bone = rig.bones[p_bone];
	if (shapes == 0) {
		Ref<CapsuleShape3D> cap;
		cap.instantiate();
		const float r = MAX(bone.reach * 0.5f, 0.05f) * scale;
		cap->set_radius(r);
		cap->set_height(MAX(bone.length * scale, 2.0f * r + 0.01f));
		CollisionShape3D *cs = memnew(CollisionShape3D);
		cs->set_shape(cap);
		cs->set_transform(Transform3D(Basis(Quaternion(Vector3(0, 1, 0), bone.dir.normalized())), (bone.pivot + bone.dir * (bone.length * 0.5f)) * scale));
		body->add_child(cs);
	}
	// Mass and its centre from each bone's own mass (carried less what its children carry), at
	// the middle of its wood.
	float total = 0.0f;
	Vector3 moment;
	for (uint32_t b = MAX(uint32_t(p_bone), 1u); b < n; b++) {
		if (!inside[b]) {
			continue;
		}
		float own = rig.bones[b].mass;
		for (uint32_t e = b + 1; e < n; e++) {
			if (rig.bones[e].parent == int(b)) {
				own -= rig.bones[e].mass;
			}
		}
		own = MAX(own, 0.0f);
		total += own;
		moment += (rig.bones[b].pivot + rig.bones[b].dir * (rig.bones[b].length * 0.5f)) * own;
	}
	Vector3 com = total > 1e-6f ? moment / total : bone.pivot + bone.dir * (bone.length * 0.5f);
	float mass = total * scale * scale * scale;
	if (p_bone == 0 && rig.storm_data.size() >= 9 && !making_snap) {
		// The root plate: a lens thinning to its rim, about a quarter of its disc in soil (Krisans
		// et al. 2020: 0.5-1.5 m3 for spruce plates of 1.4-2 m radius), at 1,400 kg/m3 moist.
		MeshInstance3D *pm = memnew(MeshInstance3D);
		pm->set_mesh(_kit_mesh(rig, true));
		pm->set_scale(Vector3(scale, scale, scale));
		body->add_child(pm);
		const float plate = 0.25f * float(Math::PI) * rig.plate_radius * rig.plate_radius * rig.plate_thick * 1400.0f * scale * scale * scale;
		com = (com * mass + Vector3(0.0f, -0.5f * rig.plate_thick, 0.0f) * plate) / MAX(mass + plate, 1e-3f);
		mass += plate;
	}
	body->set_center_of_mass_mode(RigidBody3D::CENTER_OF_MASS_MODE_CUSTOM);
	body->set_center_of_mass(com * scale);
	body->set_mass(MAX(mass, 0.5f));
	add_child(body);
	body->set_global_transform(p_at);
	// Parts of one tree start touching where they broke apart.
	Touch &tc = r_target.touch[p_instance];
	for (const ObjectID &id : tc.parts) {
		CollisionObject3D *other = ObjectDB::get_instance<CollisionObject3D>(id);
		if (other) {
			PhysicsServer3D::get_singleton()->body_add_collision_exception(body->get_rid(), other->get_rid());
			PhysicsServer3D::get_singleton()->body_add_collision_exception(other->get_rid(), body->get_rid());
		}
	}
	tc.parts.push_back(body->get_instance_id());
	return body;
}

// Whether the fire killed the tree (docs/ideas/physx.md, "Standing snags"): Ryan and Reinhardt's (1988) mortality from
// the crown volume scorched (CVS, %, the share of its foliage below the lethal scorch height) and its bark thickness
// (BT, inches; about 4% of the stem's diameter, FOFEM's middle), P = 1 / (1 + exp(-1.941 + 6.316 (1 - e^-BT) -
// 0.000535 CVS^2)), against a hash steady per tree. A killed tree's age dead follows the fire's burn age.
void TreeWind3D::_update_snag(Target &r_target, uint32_t p_instance, const Transform3D &p_tree, const Vector3 &p_at, const WorldFire *p_fire) {
	if (p_instance >= r_target.snag_age.size() || !r_target.rig) {
		return;
	}
	float &age = r_target.snag_age[p_instance];
	if (age >= 0.0f) {
		age = MAX(age, p_fire->burn_age_at(p_at));
		return;
	}
	const float scorch = p_fire->crown_scorch_at(p_at);
	if (scorch <= 0.0f) {
		return;
	}
	const Rig &rig = *r_target.rig;
	const float scale = MAX(Math::pow(float(Math::abs(p_tree.basis.determinant())), 1.0f / 3.0f), 1e-3f);
	// A tree with a foliage profile follows the rule its leaf shader follows (VegetationFire::tree_killed, trees.md
	// 5ag), so a snag's crown browns and is cast exactly where its wood weakens; the bone areas are the fallback.
	const PackedFloat32Array fc = rig.mesh.is_valid() ? PackedFloat32Array(rig.mesh->get_meta(SNAME("tree_fire_crown"), PackedFloat32Array())) : PackedFloat32Array();
	if (fc.size() == 6 && fc[5] > 0.0f) {
		if (VegetationFire::tree_killed(p_at.x, p_at.z, scale, scorch, fc.ptr())) {
			age = MAX(p_fire->burn_age_at(p_at), 0.0f);
			storm_snags++;
		}
		return;
	}
	float below = 0.0f, total = 0.0f;
	for (uint32_t b = 1; b < rig.bones.size(); b++) {
		const Bone &bone = rig.bones[b];
		total += bone.area;
		below += (bone.pivot + bone.centroid).y * scale < scorch ? bone.area : 0.0f;
	}
	const float cvs = total > 0.0f ? 100.0f * below / total : 0.0f;
	const float dbh_in = (rig.bones.size() > 1 ? 2.0f * rig.bones[1].radius * scale : 0.2f) * 39.37f;
	const float bt = 0.04f * dbh_in;
	const float p = 1.0f / (1.0f + Math::exp(-1.941f + 6.316f * (1.0f - Math::exp(-bt)) - 0.000535f * cvs * cvs));
	const Vector3 o = r_target.placements[p_instance].origin;
	const uint32_t seed = hash_u32(uint32_t(int(o.x * 16.0f)) * 73856093u ^ uint32_t(int(o.z * 16.0f)) * 19349663u);
	if (hash01(seed, 0x5a9u) < p) {
		age = MAX(p_fire->burn_age_at(p_at), 0.0f);
		storm_snags++;
	}
}

// A storm broke a part of a tree or threw it (docs/ideas/trees.md section 5t): the part becomes a
// rigid body turning on a hinge (the last fibres on the break's compression side, or the root
// plate's leeward edge) while the wind still pushes it; a fracture cap closes the stub.
void TreeWind3D::_storm_fail(Target &r_target, int p_instance, const Transform3D &p_world, const StormLoad &p_load) {
	const Rig &rig = *r_target.rig;
	const uint32_t n = rig.bones.size();
	Touch &tc = r_target.touch[p_instance];
	if (tc.uprooted) {
		return;
	}
	if (tc.broken.size() != n) {
		tc.broken.resize(n);
		memset(tc.broken.ptr(), 0, n);
	}
	if (tc.flat.size() != n) {
		tc.flat.resize(n);
		memset(tc.flat.ptr(), 0, n);
	}
	const bool thrown = p_load.root_ratio > 1.0f && p_load.root_ratio >= p_load.ratio;
	const bool snap = thrown && p_load.base_snap; // a charred foot gave way: no root plate, no pit
	const int b = thrown ? 0 : p_load.bone;
	if (b < 0 || (b > 0 && tc.broken[b])) {
		return;
	}
	const Transform3D tree = p_world * r_target.placements[p_instance];
	const float scale = MAX(Math::pow(float(Math::abs(tree.basis.determinant())), 1.0f / 3.0f), 1e-3f);
	const Basis rot = tree.basis.orthonormalized();
	// The part's pose now, from this frame's solve (the record holds placement times the bone).
	const Transform3D posed = b == 0 ? tree : p_world * read_record(r_target.write + (size_t(p_instance) * r_target.bones + b) * RECORD_FLOATS);
	const Transform3D at(posed.basis.orthonormalized(), posed.origin);
	// Downwind in the tree's own space.
	Vector3 dw = rot.transposed().xform(p_load.wind);
	dw.y = 0.0f;
	dw = dw.length_squared() > 1e-8f ? dw.normalized() : Vector3(1, 0, 0);
	RandomPCG rr;
	rr.seed(uint64_t(hash_u32(uint32_t(p_instance) * 2654435761u + uint32_t(b))) + frame_index);

	// Parts broken off before stay their own; the key names this part's mesh and Blast asset.
	LocalVector<uint8_t> exclude;
	exclude.resize(n);
	for (uint32_t c = 0; c < n; c++) {
		exclude[c] = tc.broken[c];
	}
	uint64_t key = hash_u32(uint32_t(rig.mesh.is_valid() ? uint64_t(rig.mesh->get_instance_id()) : 0u) ^ 0x5bd1e995u) * 2654435761ull + uint64_t(b) * 40503u;
	for (uint32_t c = 0; c < n; c++) {
		if (exclude[c]) {
			key = key * 1099511628211ull + c;
		}
	}
	for (uint32_t c = MAX(uint32_t(b), 1u); c < n; c++) {
		if (c == uint32_t(b) || b == 0 || tc.broken[rig.bones[c].parent]) {
			tc.broken[c] = 1;
		}
	}
	const Bone &bone = rig.bones[b];
	const Vector3 dir = b == 0 ? Vector3(0, 1, 0) : bone.dir.normalized();
	// A stem segment (not the first of its axis) snaps at its joint and is drawn flat there; a
	// limb tears out at its collar and goes whole.
	const bool stem_joint = b > 0 && rig.groups.size() == n && rig.groups[b] != b;
	if (stem_joint) {
		tc.flat[b] = 1;
	}
	if (thrown) {
		tc.uprooted = true;
	}
	tc.active = true;
	// Parts hung on their fibres above this break (or on a tree now thrown) hang from nothing now.
	for (StormPart &other : storm_parts) {
		if (!other.joint.is_valid() || tc.parts.find(other.body) < 0) {
			continue;
		}
		int up = other.bone;
		while (up > b && up > 0) {
			up = rig.bones[up].parent;
		}
		if (up == b) {
			PhysicsServer3D::get_singleton()->free_rid(other.joint);
			other.joint = RID();
		}
	}

	// A charred stem breaks a little above the ground, in its charred band (the catface; burnt-out trees
	// leave stumps of 0.3 to 1.5 m), at a height steady per tree; the stem's radius there.
	float cut = 0.0f, cut_r = rig.bones.size() > 1 ? rig.bones[1].radius : 0.1f;
	if (snap) {
		cut = (0.3f + 0.6f * hash01(uint32_t(p_instance) * 2654435761u + 0x7f4a7c15u, 3u)) / scale;
		const float *co = rig.colliders.ptr();
		for (int k = 0; k < rig.colliders.size() / 8; k++) {
			const float *o = co + k * 8;
			if (int(o[7]) == 1 && MIN(o[1], o[4]) <= cut && MAX(o[1], o[4]) >= cut) {
				cut_r = o[6];
				break;
			}
		}
		key ^= 0x9e3779b97f4a7c15ull; // Its own asset: the same tree without its plate, cut over its stump.
		key = key * 1099511628211ull + uint64_t(Math::round(cut * 100.0f));
	}
	snap_cut = cut;
	making_snap = snap;
	RigidBody3D *body = _make_part(r_target, p_instance, tree, b, at, exclude, key);
	making_snap = false;
	// The torn side faces the wind (tension), the crumpled side away from it; its bearing in 16
	// steps, so parts broken alike share a Blast asset.
	Vector3 tens = -dw - dir * dir.dot(-dw);
	if (tens.length_squared() < 1e-6f) {
		tens = dir.cross(Vector3(0, 0, 1));
	}
	tens.normalize();
	{
		const float az = Math::round(Math::atan2(tens.z, tens.x) / (float(Math::TAU) / 16.0f));
		key = key * 31u + uint64_t(int(az) + 16);
		Vector3 q(Math::cos(az * float(Math::TAU) / 16.0f), 0.0f, Math::sin(az * float(Math::TAU) / 16.0f));
		q = q - dir * dir.dot(q);
		if (q.length_squared() > 1e-4f) {
			tens = q.normalized();
		}
	}
	const float parent_r = b > 0 ? rig.bones[bone.parent].radius : 0.0f;
	const float cap_r = stem_joint ? bone.radius * 1.05f : bone.radius;
	const Vector3 cap_at = stem_joint ? bone.pivot : bone.pivot + dir * (0.9f * parent_r);
	const Array &cap_kit = rig.cap_kit;
	Transform3D butt_xf = Transform3D(Basis(Vector3(), Vector3(), Vector3()), Vector3());
	if (b > 0 && cap_kit.size() > 0) {
		Basis cb;
		cb.set_column(0, tens * cap_r);
		cb.set_column(1, dir * cap_r);
		cb.set_column(2, tens.cross(dir) * cap_r);
		MeshInstance3D *cap = memnew(MeshInstance3D);
		cap->set_name("TreeStub");
		cap->set_mesh(_kit_mesh(rig, false));
		cap->set_as_top_level(true);
		add_child(cap);
		Cap cp;
		cp.node = cap->get_instance_id();
		cp.target = r_target.node;
		cp.instance = p_instance;
		cp.bone = bone.parent;
		cp.local = Transform3D(cb, cap_at);
		cap->set_global_transform(p_world * read_record(r_target.write + (size_t(p_instance) * r_target.bones + cp.bone) * RECORD_FLOATS) * cp.local);
		caps.push_back(cp);
		// The part's butt: the matching face, turned the other way along the wood.
		Basis pb;
		pb.set_column(0, tens * cap_r);
		pb.set_column(1, -dir * cap_r);
		pb.set_column(2, tens.cross(-dir) * cap_r);
		MeshInstance3D *butt = memnew(MeshInstance3D);
		butt->set_mesh(_kit_mesh(rig, false));
		butt->set_transform(Transform3D(pb * scale, cap_at * scale));
		body->add_child(butt);
		butt_xf = Transform3D(pb, cap_at);
	} else if (snap && cap_kit.size() > 0) {
		// The stump: the stem's bark below the cut, closed by a fracture cap, a static body where the tree stood.
		const Vector3 up(0, 1, 0);
		Vector3 side = tens - up * tens.dot(up);
		side = side.length_squared() > 1e-6f ? side.normalized() : Vector3(1, 0, 0);
		const float cr = cut_r * 1.05f;
		Basis cb;
		cb.set_column(0, side * cr);
		// Charred wood breaks short and brash, not in the long splinters of green wood: the cap a third as tall.
		cb.set_column(1, up * (0.3f * cr));
		cb.set_column(2, side.cross(up) * cr);
		StaticBody3D *stump = memnew(StaticBody3D);
		stump->set_name("TreeStump");
		stump->set_as_top_level(true);
		stump->set_collision_layer(collision_layer | 1u);
		MeshInstance3D *sm = memnew(MeshInstance3D);
		sm->set_mesh(_piece_mesh(rig, 0, nullptr, key ^ 0x51ed270b27f6c3a1ull, cut, -1));
		sm->set_scale(Vector3(scale, scale, scale));
		stump->add_child(sm);
		MeshInstance3D *top = memnew(MeshInstance3D);
		top->set_mesh(_kit_mesh(rig, false));
		top->set_transform(Transform3D(cb * scale, Vector3(0, cut, 0) * scale));
		stump->add_child(top);
		Ref<CylinderShape3D> cyl;
		cyl.instantiate();
		cyl->set_radius(cut_r * scale);
		cyl->set_height(cut * scale);
		CollisionShape3D *cs = memnew(CollisionShape3D);
		cs->set_shape(cyl);
		cs->set_position(Vector3(0, 0.5f * cut * scale, 0));
		stump->add_child(cs);
		add_child(stump);
		stump->set_global_transform(Transform3D(rot, tree.origin));
		PhysicsServer3D::get_singleton()->body_add_collision_exception(body->get_rid(), stump->get_rid());
		PhysicsServer3D::get_singleton()->body_add_collision_exception(stump->get_rid(), body->get_rid());
		tc.parts.push_back(stump->get_instance_id());
		// The trunk's butt: the matching face, turned down.
		Basis pb;
		pb.set_column(0, side * cr);
		pb.set_column(1, -up * (0.3f * cr));
		pb.set_column(2, side.cross(-up) * cr);
		MeshInstance3D *butt = memnew(MeshInstance3D);
		butt->set_mesh(_kit_mesh(rig, false));
		butt->set_transform(Transform3D(pb * scale, Vector3(0, cut, 0) * scale));
		body->add_child(butt);
		butt_xf = Transform3D(pb, Vector3(0, cut, 0));
	}

	// The hinge: on the leeward edge of the break (its compression side holds longest), or the root
	// plate's leeward edge, three quarters of its radius out and a little under the old surface.
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	StormPart sp;
	sp.body = body->get_instance_id();
	sp.vogel = rig.vogel;
	sp.start = at.basis;
	sp.mesh = rig.mesh;
	sp.bone = b;
	sp.scale = scale;
	sp.key = key;
	sp.snap_cut = snap ? cut : 0.0f;
	sp.exclude.resize(n);
	for (uint32_t c = 0; c < n; c++) {
		sp.exclude[c] = exclude[c];
	}
	sp.butt = butt_xf;
	sp.fall = dw;
	float area = 0.0f;
	Vector3 load;
	for (uint32_t c = 1; c < n; c++) {
		if ((b == 0 && rig.bones[c].parent == 0) || int(c) == b) {
			area += rig.bones[c].area;
			load += (rig.bones[c].pivot + rig.bones[c].centroid) * rig.bones[c].area;
		}
	}
	sp.area = area * scale * scale;
	sp.load = (area > 1e-6f ? load / area : bone.pivot) * scale;
	Vector3 hinge_at, hinge_axis;
	if (thrown) {
		// Over the root plate's lee rim; a charred stem folds over the lee edge of its stump.
		hinge_at = snap ? dw * cut_r + Vector3(0, cut, 0) : Vector3(0, 0, 0) + dw * (0.75f * rig.plate_radius) - Vector3(0, 0.1f, 0);
		hinge_axis = Vector3(0, 1, 0).cross(dw);
		sp.release = 0.0f;
	} else {
		Vector3 lee = dw - dir * dir.dot(dw);
		lee = lee.length_squared() > 1e-6f ? lee.normalized() : -tens;
		hinge_at = cap_at + lee * cap_r;
		hinge_axis = dir.cross(lee);
		// Green wood holds a while as it folds (a greenstick break, van Casteren et al. 2012): the
		// part tears free after turning 30-75 degrees; a light one may stay hung on its fibres
		// (a hanger, about a third of limbs under 300 kg).
		const float kg = rig.bones[b].mass * scale * scale * scale;
		sp.release = (kg < 300.0f && rr.randf() < 0.35f) ? 0.0f : Math::lerp(0.5f, 1.3f, rr.randf());
	}
	if (hinge_axis.length_squared() > 1e-8f) {
		const Vector3 pa = hinge_at * scale;
		const Vector3 aa = hinge_axis.normalized();
		sp.joint = ps->joint_create();
		ps->joint_make_hinge_simple(sp.joint, body->get_rid(), pa, aa, RID(), at.xform(pa), at.basis.xform(aa));
	}
	_blast_request(sp);
	storm_parts.push_back(sp);

	// The stub's colliders are rebuilt without the broken wood (and none for a thrown tree).
	Target::Body *have = r_target.bodies.getptr(p_instance);
	if (have) {
		ps->free_rid(have->body);
		for (const RID &sh : have->shapes) {
			ps->free_rid(sh);
		}
		r_target.bodies.erase(p_instance);
	}
	_collapse(r_target, p_instance, p_world);
	if (thrown) {
		storm_throws++;
		storm_snaps += snap ? 1 : 0;
		if (report_windthrow) {
			ThrowCell &cell = throw_cells[Vector2i(Math::floor(tree.origin.x / WINDTHROW_CELL), Math::floor(tree.origin.z / WINDTHROW_CELL))];
			cell.thrown++;
			cell.sum += Vector2(tree.origin.x, tree.origin.z);
			throw_cells_dirty = true;
		}
		// A MultiMesh whose every tree is thrown draws nothing but still costs its skinned copy under
		// path tracing (the fallen trees are drawn by their destructibles): hide it.
		bool all = true;
		for (uint32_t i = 0; i < r_target.placements.size() && all; i++) {
			const Touch *t = r_target.touch.getptr(int(i));
			all = t && t->uprooted;
		}
		MultiMeshInstance3D *mmi = ObjectDB::get_instance<MultiMeshInstance3D>(r_target.node);
		if (all && mmi) {
			mmi->set_visible(false);
		}
	} else {
		storm_breaks++;
	}
	const bool iced = p_instance < int(r_target.glaze_kg.size()) && r_target.glaze_kg[p_instance] > 0.0f;
	const bool by_glaze = p_load.snow && iced && r_target.glaze_kg[p_instance] >= (p_instance < int(r_target.snow_kg.size()) ? r_target.snow_kg[p_instance] : 0.0f);
	snow_breaks += p_load.snow && !by_glaze ? 1 : 0;
	glaze_breaks += by_glaze ? 1 : 0;
	if (iced) {
		// The jolt shatters the ice off what broke (all of it off a thrown tree): it stays off until the glaze melts.
		const float part = thrown ? 1.0f : (rig.leaf_area > 1e-6f ? CLAMP(rig.bones[b].area / rig.leaf_area, 0.1f, 1.0f) : 0.5f);
		r_target.glaze_shed[p_instance] = MIN(1.0f, r_target.glaze_shed[p_instance] + part * (1.0f - r_target.glaze_shed[p_instance]));
		r_target.glaze_kg[p_instance] *= 1.0f - part;
	}
	// The jolt throws the crown's snow onto the pack (WorldSnow.shake_canopy): all of it from a felled tree,
	// what the broken part held from a snapped limb or top (section 5al).
	if (snow_load_enabled && p_instance < int(r_target.snow_kg.size()) && r_target.snow_kg[p_instance] > 0.0f) {
		WorldSnow *snow = WorldContracts::snow();
		if (snow && snow->has_provider()) {
			const float part = thrown ? 1.0f : (rig.leaf_area > 1e-6f ? CLAMP(rig.bones[b].area / rig.leaf_area, 0.1f, 1.0f) : 0.5f);
			snow_shaken_kg += snow->shake_canopy(tree.origin, rig.crown_radius * scale, part);
			if (p_instance < int(r_target.snow_seen.size())) {
				_drop_clumps(r_target, p_instance, p_world, r_target.snow_seen[p_instance] * part);
				r_target.snow_seen[p_instance] *= 1.0f - part;
			}
			r_target.snow_kg[p_instance] *= 1.0f - part;
			r_target.snow_creep[p_instance] *= 1.0f - part;
		}
	}
	// A felled tree or snapped stem takes its leaves out of the weather map's canopy, so snow, rain and wind
	// under it see the gap (a cleared avalanche lane stops anchoring and braking the next one).
	if (thrown || stem_joint) {
		CanopyHole hole;
		hole.xz = Vector2(tree.origin.x, tree.origin.z);
		hole.radius = _crown_radius(rig) * scale;
		// A snapped stem keeps the foliage below its break: about half the share of the height under it.
		hole.keep = thrown ? 0.0f : CLAMP(0.5f * bone.pivot.y / MAX(rig.extent, 0.1f), 0.0f, 0.6f);
		canopy_holes.push_back(hole);
	}
	Dictionary pit;
	if (thrown && !snap) {
		// The pit the plate leaves (5k, tree_root_pit): its centre, radius and depth, world space.
		pit["center"] = tree.origin;
		pit["radius"] = 0.9f * rig.plate_radius * scale;
		pit["depth"] = CLAMP(1.6f * rig.plate_thick * scale, 0.35f, 0.95f);
	}
	emit_signal(SNAME("storm_damage"), tree.xform(cap_at), (thrown ? rig.bones[1].radius : bone.radius) * scale, thrown && !snap, pit);
}

// Avalanche impact on trees (docs/ideas/trees.md 5ak). The stem's breaking pressure as a power of its
// diameter, fitted to Bartelt and Stoeckli 2001 (Annals of Glaciology 32): trunks of 30-50 cm break from
// about 10-20 kPa of dense flow, so P = 15 kPa (D / 0.4 m)^1.36, which gives 1-4 kPa for stems of 5-12 cm,
// the range a powder cloud reaches (Sovilla et al. 2015). A crown on a stem that holds snaps from about
// 3 kPa: the powder cloud's front takes the tops of mature trees (RAMMS::EXTENDED's suspension layer).
float TreeWind3D::avalanche_strength(float p_stem_diameter, bool p_crown) {
	if (p_crown) {
		return 3000.0f;
	}
	return MAX(15000.0f * Math::pow(MAX(p_stem_diameter, 0.01f) / 0.4f, 1.36f), 500.0f);
}

// Once a second: the snow each crown holds (section 5al). Snow keeps the mass, as kg per m2 of ground over the
// canopy (WorldSnow.canopy_load_at, Hedstrom and Pomeroy 1998); a tree's crown, where the canopy closes to C
// (WorldWeather.canopy_closure_at), holds L / C per m2 of its own spread, pi R^2. Under a held load the wood creeps on:
// softwood branches add about 34% of their instant bend over a few hours (Kelvin retardation, spectra peaking at
// 1-10 h; Ray and Bret-Harte 2019), taken as one first-order step of snow_creep_hours.
void TreeWind3D::_snow_check(float p_dt) {
	snow_frame = false;
	if (!snow_load_enabled || !is_inside_tree()) {
		return;
	}
	const WorldSnow *snow = WorldContracts::snow();
	if (!snow || !snow->has_provider()) {
		for (Target &t : targets) {
			for (uint32_t i = 0; i < t.snow_kg.size(); i++) {
				t.snow_kg[i] = 0.0f;
				t.snow_creep[i] = 0.0f;
				if (i < t.cap_level.size() && t.cap_level[i]) {
					t.cap_level[i] = 0;
					t.caps_dirty = true;
				}
			}
		}
		return;
	}
	const WorldWeather *weather = WorldContracts::weather();
	const bool has_weather = weather && weather->has_provider();
	// snow_creep_hours 0: no creep (tests that want the instant bend).
	const float creep_share = snow_creep_hours > 0.0f ? 0.34f : 0.0f;
	const float creep_step = snow_creep_hours > 0.0f ? 1.0f - Math::exp(-p_dt / (snow_creep_hours * 3600.0f)) : 1.0f;
	for (Target &t : targets) {
		MultiMeshInstance3D *mmi = ObjectDB::get_instance<MultiMeshInstance3D>(t.node);
		if (!mmi || !mmi->is_inside_tree() || !t.rig || t.snow_kg.size() != t.placements.size()) {
			continue;
		}
		const Transform3D cw = mmi->get_global_transform();
		for (uint32_t i = 0; i < t.placements.size(); i++) {
			const Transform3D tree = cw * t.placements[i];
			const float load = snow->canopy_load_at(tree.origin);
			// Caps by the canopy's load over its capacity, the ratio the crown whitening reads (snow's catch map):
			// a light dusting from 8%, full from 35%, heavy from 70%. Near the camera only.
			if (i < t.cap_level.size()) {
				const float cap = snow->canopy_capacity_at(tree.origin);
				const float ratio = cap > 0.0f ? load / cap : load / 20.0f;
				uint8_t lv = 0;
				if (t.rig->snow_caps[0].is_valid() && load > 0.0f && tree.origin.distance_to(last_eye) <= snow_cap_range) {
					lv = ratio < 0.08f ? 0 : (ratio < 0.35f ? 1 : (ratio < 0.7f ? 2 : 3));
				}
				const Touch *tc = t.touch.getptr(int(i));
				if (tc && tc->uprooted) {
					lv = 0;
				}
				if (t.cap_level[i] != lv) {
					t.cap_level[i] = lv;
					t.caps_dirty = true;
				}
			}
			float kg = 0.0f;
			if (load > 0.0f) {
				const float scale = MAX(Math::pow(float(Math::abs(tree.basis.determinant())), 1.0f / 3.0f), 1e-3f);
				const float closure = has_weather ? weather->canopy_closure_at(tree.origin) : 0.7f;
				const float r = t.rig->crown_radius * scale;
				kg = load / MAX(closure, 0.3f) * float(Math::PI) * r * r * t.rig->snow_catch;
			}
			const float inst = kg;
			// Snow leaving the crown (the cover's unloading in wind and warmth) drops as clumps near the camera.
			if (i < t.snow_seen.size()) {
				const float lost = t.snow_seen[i] - inst;
				if (lost > MAX(5.0f, 0.1f * t.snow_seen[i])) {
					_drop_clumps(t, int(i), cw, lost);
					t.snow_seen[i] = inst;
				} else if (inst > t.snow_seen[i]) {
					t.snow_seen[i] = inst;
				}
			}
			t.snow_creep[i] += (creep_share * inst - t.snow_creep[i]) * creep_step;
			t.snow_kg[i] = inst + t.snow_creep[i];
			snow_frame = snow_frame || t.snow_kg[i] > 1.0f;
		}
	}
}

// The snow caps (section 5al): one MultiMesh per load level under the target, drawing that level's cap pads on the
// instances at it, with the same bone records as the trees (so they ride the wind and the path tracer sees them as the
// trees' own geometry); every other instance's record collapses to a point.
void TreeWind3D::_update_caps(Target &r_target, bool p_moved) {
	if (!r_target.rig || r_target.rig->snow_caps[0].is_null() || r_target.cap_level.size() != r_target.placements.size()) {
		return;
	}
	MultiMeshInstance3D *mmi = ObjectDB::get_instance<MultiMeshInstance3D>(r_target.node);
	if (!mmi) {
		return;
	}
	int count[3] = { 0, 0, 0 };
	for (uint32_t i = 0; i < r_target.cap_level.size(); i++) {
		if (r_target.cap_level[i]) {
			count[r_target.cap_level[i] - 1]++;
		}
	}
	const bool any = count[0] + count[1] + count[2] > 0;
	if (!any && !r_target.caps_dirty) {
		return;
	}
	if (!p_moved && !r_target.caps_dirty) {
		caps_drawn += count[0] + count[1] + count[2];
		return;
	}
	r_target.caps_dirty = false;
	const int n = r_target.placements.size();
	const int k = r_target.bones;
	for (int lv = 0; lv < 3; lv++) {
		MultiMeshInstance3D *cap = ObjectDB::get_instance<MultiMeshInstance3D>(r_target.cap_nodes[lv]);
		if (count[lv] == 0 || r_target.rig->snow_caps[lv].is_null()) {
			if (cap) {
				cap->set_visible(false);
			}
			continue;
		}
		if (!cap) {
			cap = memnew(MultiMeshInstance3D);
			cap->set_name(vformat("SnowCaps%d", lv + 1));
			Ref<MultiMesh> mm;
			mm.instantiate();
			mm->set_transform_format(MultiMesh::TRANSFORM_3D);
			mm->set_use_colors(true);
			mm->set_use_custom_data(true);
			mm->set_mesh(r_target.rig->snow_caps[lv]);
			mm->set_bones_per_instance(k);
			mm->set_instance_count(n * k);
			cap->set_multimesh(mm);
			mmi->add_child(cap, false, Node::INTERNAL_MODE_BACK);
			r_target.cap_nodes[lv] = cap->get_instance_id();
		}
		cap->set_visible(true);
		r_target.cap_buffer = r_target.buffer;
		float *w = r_target.cap_buffer.ptrw();
		for (int i = 0; i < n; i++) {
			if (r_target.cap_level[i] == lv + 1) {
				continue;
			}
			const Vector3 o = r_target.placements[i].origin;
			for (int b = 0; b < k; b++) {
				float *f = w + (size_t(i) * k + b) * RECORD_FLOATS;
				for (int q = 0; q < 12; q++) {
					f[q] = 0.0f;
				}
				f[3] = o.x;
				f[7] = o.y;
				f[11] = o.z;
			}
		}
		RenderingServer::get_singleton()->multimesh_set_buffer(cap->get_multimesh()->get_rid(), r_target.cap_buffer);
		caps_drawn += count[lv];
	}
}

// Clumps of snow leaving a crown (section 5al): a load drop of p_kg falls from its cap sites as lumps of about 1.5 kg
// (sintered crown snow breaks into lumps of 10-30 cm; Hedstrom and Pomeroy 1998 saw unloading as mass release), at
// most 40 a tree, near the camera only. They fall with air drag to some 5 m/s (snow lumps' terminal speed), tumble,
// crumble as they go and are gone on the ground: where they land is the snow cover's (it deepens there).
void TreeWind3D::_drop_clumps(Target &r_target, int p_instance, const Transform3D &p_world, float p_kg) {
	if (!snow_clumps_enabled || p_kg <= 0.0f || !r_target.rig || r_target.rig->cap_sites.size() < 4 || clumps.size() >= 3000) {
		return;
	}
	const Transform3D tree = p_world * r_target.placements[p_instance];
	if (tree.origin.distance_to(last_eye) > clump_range) {
		return;
	}
	const PackedFloat32Array &sites = r_target.rig->cap_sites;
	const int count = sites.size() / 4;
	const int n = CLAMP(int(p_kg / 1.5f), 1, 40);
	RandomPCG r;
	r.seed(uint64_t(frame_index) * 2654435761ull + uint64_t(p_instance) * 40503ull + uint64_t(clumps.size()));
	float ground = tree.origin.y;
	const WorldGround *wg = WorldContracts::ground();
	if (wg && wg->has_provider()) {
		ground = wg->height_at(tree.origin);
	}
	const float scale = MAX(Math::pow(float(Math::abs(tree.basis.determinant())), 1.0f / 3.0f), 1e-3f);
	for (int k = 0; k < n; k++) {
		const int s = int(r.rand() % uint32_t(count));
		const int bone = CLAMP(int(sites[s * 4]), 0, r_target.bones - 1);
		const Vector3 local(sites[s * 4 + 1], sites[s * 4 + 2], sites[s * 4 + 3]);
		const Transform3D posed = p_world * read_record(r_target.buffer.ptr() + (size_t(p_instance) * r_target.bones + bone) * RECORD_FLOATS);
		Clump c;
		c.position = posed.xform(local);
		c.velocity = Vector3(Math::lerp(-0.3f, 0.3f, r.randf()), Math::lerp(-0.5f, 0.2f, r.randf()), Math::lerp(-0.3f, 0.3f, r.randf()));
		c.rotation = Basis(Vector3(r.randf() - 0.5f, r.randf() - 0.5f, r.randf() - 0.5f).normalized(), r.randf() * float(Math::TAU));
		c.spin = Vector3(r.randf() - 0.5f, r.randf() - 0.5f, r.randf() - 0.5f) * 4.0f;
		const float d = Math::lerp(0.08f, 0.28f, r.randf()) * MIN(scale, 1.5f);
		c.size = Vector3(d * Math::lerp(0.8f, 1.3f, r.randf()), d * Math::lerp(0.6f, 1.0f, r.randf()), d * Math::lerp(0.8f, 1.3f, r.randf()));
		c.ground = ground;
		clumps.push_back(c);
	}
	clumps_dropped += n;
}

void TreeWind3D::_step_clumps(double p_delta) {
	MultiMeshInstance3D *node = ObjectDB::get_instance<MultiMeshInstance3D>(clump_node);
	if (clumps.is_empty()) {
		if (node && node->is_visible()) {
			node->set_visible(false);
		}
		return;
	}
	const float dt = float(MIN(p_delta, 0.1));
	const float drag = 9.81f / 25.0f; // terminal 5 m/s: g = k v^2
	for (int i = int(clumps.size()) - 1; i >= 0; i--) {
		Clump &c = clumps[i];
		const float v = c.velocity.length();
		c.velocity += (Vector3(0.0f, -9.81f, 0.0f) - c.velocity * (drag * v)) * dt;
		c.position += c.velocity * dt;
		const float spin = c.spin.length();
		if (spin > 1e-4f) {
			c.rotation = Basis(c.spin / spin, spin * dt) * c.rotation;
		}
		c.size *= 1.0f - 0.08f * dt; // crumbling as it falls
		if (c.position.y <= c.ground + c.size.y * 0.3f) {
			clumps.remove_at_unordered(i);
		}
	}
	if (!node) {
		if (clump_mesh.is_null()) {
			// A lumpy ball: a coarse sphere, each clump scaled unevenly and turned (no two alike).
			Ref<SphereMesh> sphere;
			sphere.instantiate();
			sphere->set_radius(0.5f);
			sphere->set_height(0.85f);
			sphere->set_radial_segments(8);
			sphere->set_rings(4);
			Ref<StandardMaterial3D> mat;
			mat.instantiate();
			mat->set_albedo(Color(0.9f, 0.91f, 0.93f));
			mat->set_roughness(0.8f);
			mat->set_specular(0.3f);
			sphere->set_material(mat);
			clump_mesh = sphere;
		}
		node = memnew(MultiMeshInstance3D);
		node->set_name("SnowClumps");
		Ref<MultiMesh> mm;
		mm.instantiate();
		mm->set_transform_format(MultiMesh::TRANSFORM_3D);
		mm->set_mesh(clump_mesh);
		node->set_multimesh(mm);
		add_child(node, false, Node::INTERNAL_MODE_BACK);
		node->set_as_top_level(true);
		node->set_global_transform(Transform3D());
		clump_node = node->get_instance_id();
		clump_capacity = 0;
	}
	Ref<MultiMesh> mm = node->get_multimesh();
	const int n = clumps.size();
	if (n > clump_capacity) {
		clump_capacity = MAX(n, clump_capacity * 2);
		clump_capacity = MAX(clump_capacity, 64);
		mm->set_instance_count(clump_capacity);
	}
	Vector<float> buf;
	buf.resize(clump_capacity * 12);
	float *w = buf.ptrw();
	memset(w, 0, buf.size() * sizeof(float));
	for (int i = 0; i < n; i++) {
		const Clump &c = clumps[i];
		const Basis b = c.rotation * Basis::from_scale(c.size);
		float *f = w + i * 12;
		f[0] = b.rows[0][0];
		f[1] = b.rows[0][1];
		f[2] = b.rows[0][2];
		f[3] = c.position.x;
		f[4] = b.rows[1][0];
		f[5] = b.rows[1][1];
		f[6] = b.rows[1][2];
		f[7] = c.position.y;
		f[8] = b.rows[2][0];
		f[9] = b.rows[2][1];
		f[10] = b.rows[2][2];
		f[11] = c.position.z;
	}
	RenderingServer::get_singleton()->multimesh_set_buffer(mm->get_rid(), buf);
	mm->set_visible_instance_count(n);
	node->set_visible(true);
}

void TreeWind3D::_free_caps(Target &r_target) {
	for (int lv = 0; lv < 3; lv++) {
		MultiMeshInstance3D *cap = ObjectDB::get_instance<MultiMeshInstance3D>(r_target.cap_nodes[lv]);
		if (cap) {
			cap->queue_free();
		}
		r_target.cap_nodes[lv] = ObjectID();
	}
}

int TreeWind3D::get_instance_cap_level(Node *p_target, int p_instance) const {
	for (const Target &t : targets) {
		if (p_target && t.node == p_target->get_instance_id() && p_instance >= 0 && p_instance < int(t.cap_level.size())) {
			return t.cap_level[p_instance];
		}
	}
	return 0;
}

float TreeWind3D::glaze_kg(const Vector2 &p_surface, float p_glaze_mm, float p_scale) {
	// Radial ice R on a cylinder of radius r: rho_ice pi R (2r + R) a metre (Jones 1998); shoot lengths scale with the
	// tree, radii too, the ice does not.
	const float R = MAX(p_glaze_mm, 0.0f) * 0.001f;
	return 900.0f * float(Math::PI) * (2.0f * R * p_surface.y * p_scale * p_scale + R * R * p_surface.x * p_scale);
}

void TreeWind3D::_glaze_check() {
	const WorldWeather *weather = WorldContracts::weather();
	const bool on = glaze_load_enabled && is_inside_tree() && weather && weather->has_provider();
	for (Target &t : targets) {
		MultiMeshInstance3D *mmi = ObjectDB::get_instance<MultiMeshInstance3D>(t.node);
		if (!on || !mmi || !mmi->is_inside_tree() || !t.rig || t.glaze_kg.size() != t.placements.size()) {
			for (uint32_t i = 0; i < t.glaze_kg.size(); i++) {
				t.glaze_kg[i] = 0.0f;
				t.glaze_shed[i] = 0.0f;
			}
			continue;
		}
		const Transform3D cw = mmi->get_global_transform();
		for (uint32_t i = 0; i < t.placements.size(); i++) {
			const Transform3D tree = cw * t.placements[i];
			const float mm = t.rig->glaze_surface.x > 0.0f ? weather->glaze_at(tree.origin) : 0.0f;
			if (mm <= 0.0f) {
				t.glaze_kg[i] = 0.0f;
				t.glaze_shed[i] = 0.0f; // Melted: the next storm glazes it whole again.
				continue;
			}
			const Touch *tc = t.touch.getptr(int(i));
			if (tc && tc->uprooted) {
				t.glaze_kg[i] = 0.0f;
				continue;
			}
			const float scale = MAX(Math::pow(float(Math::abs(tree.basis.determinant())), 1.0f / 3.0f), 1e-3f);
			// No more ice than the rain the crown intercepts: radial ice R on thin twigs takes about pi rho_ice / rho_w R
			// = 2.83 R of freezing rain (Jones 1998, calm), caught over the crown's plan area, half again for wind-driven
			// rain on its sides. The upper twigs shelter the lower: a broad crown fills to its twigs' share, a narrow
			// conifer's cone sheds the rest (conical conifers resist ice storms; Hauer et al. 2006).
			const float cr = t.rig->crown_radius * scale;
			const float caught = 1000.0f * 2.83f * mm * 0.001f * float(Math::PI) * cr * cr * 1.5f;
			t.glaze_kg[i] = MIN(glaze_kg(t.rig->glaze_surface, mm, scale), caught) * (1.0f - t.glaze_shed[i]);
			snow_frame = snow_frame || t.glaze_kg[i] > 1.0f; // The static load's storm check, as the snow's.
		}
	}
}

float TreeWind3D::get_instance_glaze_kg(Node *p_target, int p_instance) const {
	for (const Target &t : targets) {
		if (p_target && t.node == p_target->get_instance_id() && p_instance >= 0 && p_instance < int(t.glaze_kg.size())) {
			return t.glaze_kg[p_instance];
		}
	}
	return 0.0f;
}

float TreeWind3D::get_instance_snow_kg(Node *p_target, int p_instance) const {
	for (const Target &t : targets) {
		if (p_target && t.node == p_target->get_instance_id() && p_instance >= 0 && p_instance < int(t.snow_kg.size())) {
			return t.snow_kg[p_instance];
		}
	}
	return 0.0f;
}

float TreeWind3D::_crown_radius(const Rig &p_rig) {
	float r = 0.0f;
	for (uint32_t c = 1; c < p_rig.bones.size(); c++) {
		const Bone &b = p_rig.bones[c];
		const Vector3 tip = b.pivot + b.dir * b.length;
		r = MAX(r, Vector2(tip.x, tip.z).length() + b.reach);
	}
	return MAX(r, 0.5f);
}

// Once a second: the highest pressure an avalanche has put at each tree's foot (WorldSnow, Pa) against its
// strength. A tree that fails waits for the frame's solve (its pose records) in avalanche_pending.
void TreeWind3D::_avalanche_check() {
	if (!avalanche_enabled || !is_inside_tree()) {
		return;
	}
	const WorldSnow *snow = WorldContracts::snow();
	if (!snow || !snow->has_provider()) {
		return;
	}
	const WorldGround *ground = WorldContracts::ground();
	const bool has_ground = ground && ground->has_provider();
	for (uint32_t ti = 0; ti < targets.size(); ti++) {
		Target &t = targets[ti];
		MultiMeshInstance3D *mmi = ObjectDB::get_instance<MultiMeshInstance3D>(t.node);
		if (!mmi || !mmi->is_inside_tree() || !t.rig || !t.rig->storm || t.rig->bones.size() < 2 || t.avalanche_hit.size() != t.placements.size()) {
			continue;
		}
		const Rig &rig = *t.rig;
		const uint32_t n = rig.bones.size();
		const Transform3D cw = mmi->get_global_transform();
		for (uint32_t i = 0; i < t.placements.size(); i++) {
			if (t.avalanche_hit[i] >= 2) {
				continue;
			}
			const Touch *tc = t.touch.getptr(int(i));
			if (tc && tc->uprooted) {
				continue;
			}
			const Transform3D tree = cw * t.placements[i];
			const Vector3 at = tree.origin;
			const float p = snow->avalanche_peak_pressure_at(at);
			if (p < 300.0f) {
				continue;
			}
			const float scale = MAX(Math::pow(float(Math::abs(tree.basis.determinant())), 1.0f / 3.0f), 1e-3f);
			// The stem's diameter above its flare (the collider spanning 2.5 m; the first one holds the flare).
			float r = rig.bones[1].radius;
			const float bh = 2.5f / scale;
			const float *co = rig.colliders.ptr();
			for (int k = 0; k < rig.colliders.size() / 8; k++) {
				const float *o = co + k * 8;
				if (int(o[7]) == 1 && MIN(o[1], o[4]) <= bh && MAX(o[1], o[4]) >= bh) {
					r = o[6];
					break;
				}
			}
			const float d = 2.0f * r * scale;
			// Wood defects, rot and rooting: each tree's own strength, steady per tree.
			const uint32_t seed = uint32_t(i) * 2654435761u + ti * 40503u;
			const bool whole = p >= avalanche_strength(d, false) * (0.75f + 0.5f * hash01(seed, 11u));
			int bone = 0;
			if (!whole) {
				if (t.avalanche_hit[i] != 0 || d < 0.15f || p < avalanche_strength(d, true) * (0.6f + 0.8f * hash01(seed, 12u))) {
					continue;
				}
				// The stem joint nearest 0.6 of the tree's reach: the top of the crown goes.
				bone = -1;
				float best = 1e9f;
				if (rig.groups.size() == n) {
					for (uint32_t c = 2; c < n; c++) {
						if (rig.groups[c] != rig.groups[1] || rig.groups[c] == int(c) || (tc && tc->broken.size() == n && tc->broken[c])) {
							continue;
						}
						const float off = Math::abs(rig.bones[c].pivot.y - 0.6f * rig.extent);
						if (off < best) {
							best = off;
							bone = int(c);
						}
					}
				}
				if (bone < 0) {
					continue;
				}
			}
			// The flow runs downslope; the tree falls with it.
			Vector3 down;
			if (has_ground) {
				const float gx = ground->height_at(at + Vector3(3, 0, 0)) - ground->height_at(at - Vector3(3, 0, 0));
				const float gz = ground->height_at(at + Vector3(0, 0, 3)) - ground->height_at(at - Vector3(0, 0, 3));
				down = Vector3(-gx, 0.0f, -gz);
			}
			if (down.length_squared() < 1e-4f) {
				down = Vector3(base_direction.x, 0.0f, base_direction.y);
			}
			down = down.length_squared() > 1e-8f ? down.normalized() : Vector3(1, 0, 0);
			AvalancheFail f;
			f.target = int(ti);
			f.instance = int(i);
			// The flow speed from its pressure, 0.5 rho u^2 with dense flow at 300 kg/m3.
			f.load.wind = down * Math::sqrt(2.0f * p / 300.0f);
			f.load.bone = bone;
			if (whole) {
				// Frozen soil holds the roots: most stems in an avalanche path break near the foot, some go with
				// their root plate (Bartelt and Stoeckli 2001).
				f.load.root_ratio = 2.0f;
				f.load.base_snap = hash01(seed, 13u) < 0.7f;
				t.avalanche_hit[i] = 2;
			} else {
				f.load.ratio = 2.0f;
				t.avalanche_hit[i] = 1;
			}
			avalanche_pending.push_back(f);
		}
	}
}

void TreeWind3D::_avalanche_fail(LocalVector<Target *> &r_changed) {
	for (const AvalancheFail &f : avalanche_pending) {
		if (f.target < 0 || f.target >= int(targets.size())) {
			continue;
		}
		Target &t = targets[f.target];
		MultiMeshInstance3D *mmi = ObjectDB::get_instance<MultiMeshInstance3D>(t.node);
		if (!mmi || !t.rig || !t.write || f.instance >= int(t.placements.size())) {
			continue;
		}
		const uint32_t had = storm_parts.size();
		_storm_fail(t, f.instance, mmi->is_inside_tree() ? mmi->get_global_transform() : Transform3D(), f.load);
		// The flow shoves what it broke downslope: the part leaves its hinge at about 1.5 m/s, and gravity
		// takes it from there (a storm's wind does this push for windthrow; on a still day nothing would).
		if (storm_parts.size() > had) {
			RigidBody3D *body = ObjectDB::get_instance<RigidBody3D>(storm_parts[storm_parts.size() - 1].body);
			if (body) {
				const Vector3 down = f.load.wind.length_squared() > 1e-8f ? f.load.wind.normalized() : Vector3(1, 0, 0);
				body->apply_central_impulse(down * body->get_mass() * 1.5f);
			}
		}
		if (f.load.bone == 0) {
			avalanche_throws++;
		} else {
			avalanche_breaks++;
		}
		if (r_changed.find(&t) < 0) {
			r_changed.push_back(&t);
		}
	}
	avalanche_pending.clear();
}

// Windthrow to the stand-age map (WorldStand, docs/ideas/world-contracts.md Stand; trees.md 5aq). Severity by the
// share of stems thrown, as disturbance maps grade it on 30 m cells (Senf and Seidl 2021, Landsat pixels): a cell
// where at least WINDTHROW_SHARE of the stems went down is stand-replacing (high severity, Frelich 2002) and is
// reported once, as a disc over the cell centred on its thrown stems; lighter losses are gaps in a stand that goes on.
void TreeWind3D::_report_windthrow() {
	if (!throw_cells_dirty || !report_windthrow) {
		return;
	}
	throw_cells_dirty = false;
	WorldContracts *wc = WorldContracts::get_singleton();
	Ref<WorldStand> stand = wc ? wc->get_stand() : Ref<WorldStand>();
	if (stand.is_null()) {
		return;
	}
	// Stems standing or down per cell that has throws.
	HashMap<Vector2i, int> stems;
	for (const Target &t : targets) {
		Node3D *node = ObjectDB::get_instance<Node3D>(t.node);
		if (!node || !node->is_inside_tree()) {
			continue;
		}
		const Transform3D cw = node->get_global_transform();
		for (uint32_t i = 0; i < t.placements.size(); i++) {
			const Vector3 at = cw.xform(t.placements[i].origin);
			const Vector2i c(Math::floor(at.x / WINDTHROW_CELL), Math::floor(at.z / WINDTHROW_CELL));
			const ThrowCell *cell = throw_cells.getptr(c);
			if (cell && cell->disturbance == 0) {
				stems[c] = stems.has(c) ? stems[c] + 1 : 1;
			}
		}
	}
	for (KeyValue<Vector2i, ThrowCell> &kv : throw_cells) {
		ThrowCell &cell = kv.value;
		const int *n = stems.getptr(kv.key);
		if (cell.disturbance != 0 || !n || cell.thrown < 3 || float(cell.thrown) < WINDTHROW_SHARE * float(*n)) {
			continue;
		}
		const Vector2 c = cell.sum / float(cell.thrown);
		cell.disturbance = stand->add_disturbance(Vector3(c.x, 0.0f, c.y), WINDTHROW_CELL * 0.6f, WorldStand::DISTURBANCE_WIND, 0.0f);
		windthrow_patches++;
	}
}

// The felled crowns' leaf area out of the weather map's canopy_map (the map VegetationField3D paints from its
// crowns, LAI / 4 in green): inside a crown's core the leaves go, fading out to 1.4 crown radii as the
// field's own crown term does. Once a second at most, on our own copy of the map.
void TreeWind3D::_cut_canopy() {
	if (canopy_holes.is_empty()) {
		return;
	}
	// The field that placed the crowns keeps the losses (VegetationField3D.add_canopy_loss), so its next repaint
	// of the canopy, or a streamed cell, does not bring them back; it does not repaint on the call, so the live map
	// is cut here too.
	Node *root = get_node_or_null(target_path);
	VegetationField3D *field = Object::cast_to<VegetationField3D>(root);
	if (field) {
		for (const CanopyHole &hole : canopy_holes) {
			// It takes every crown centred within the radius: a metre catches this stem's crown, not its neighbours'.
			field->add_canopy_loss(hole.xz, 1.0f, hole.keep);
		}
	}
	WeatherMap3D *wm = Object::cast_to<WeatherMap3D>(get_node_or_null(weather_map_path));
	if (!wm) {
		if (root) {
			const Variant path = root->get(SNAME("weather_map"));
			if (path.get_type() == Variant::NODE_PATH && !NodePath(path).is_empty()) {
				wm = Object::cast_to<WeatherMap3D>(root->get_node_or_null(path));
			}
		}
	}
	const Ref<Texture2D> tex = wm ? wm->get_canopy_map() : Ref<Texture2D>();
	const Rect2 rect = wm ? wm->get_canopy_map_rect() : Rect2();
	if (tex.is_null() || !rect.has_area()) {
		canopy_holes.clear();
		return;
	}
	if (canopy_edit.is_null() || tex->get_instance_id() != canopy_texture) {
		const Ref<Image> img = tex->get_image();
		if (img.is_null() || img->is_empty()) {
			canopy_holes.clear();
			return;
		}
		canopy_edit = img->duplicate();
		if (canopy_edit->is_compressed()) {
			canopy_edit->decompress();
		}
	}
	const int w = canopy_edit->get_width();
	const int h = canopy_edit->get_height();
	const Vector2 cell = rect.size / Vector2(w, h);
	for (const CanopyHole &hole : canopy_holes) {
		const float reach = hole.radius * 1.4f;
		const int x0 = MAX(int(Math::floor((hole.xz.x - reach - rect.position.x) / cell.x)), 0);
		const int x1 = MIN(int(Math::ceil((hole.xz.x + reach - rect.position.x) / cell.x)), w - 1);
		const int y0 = MAX(int(Math::floor((hole.xz.y - reach - rect.position.y) / cell.y)), 0);
		const int y1 = MIN(int(Math::ceil((hole.xz.y + reach - rect.position.y) / cell.y)), h - 1);
		for (int y = y0; y <= y1; y++) {
			for (int x = x0; x <= x1; x++) {
				const Vector2 p = rect.position + Vector2(x + 0.5f, y + 0.5f) * cell;
				const float d = p.distance_to(hole.xz) / MAX(hole.radius, 0.1f);
				if (d >= 1.4f) {
					continue;
				}
				Color c = canopy_edit->get_pixel(x, y);
				c.g *= Math::lerp(hole.keep, 1.0f, Math::smoothstep(0.6f, 1.4f, d));
				if (c.g < 1e-3f) {
					c.g = 0.0f;
					c.r = 0.0f;
				}
				canopy_edit->set_pixel(x, y, c);
			}
		}
	}
	canopy_holes_cut += canopy_holes.size();
	canopy_holes.clear();
	const Ref<ImageTexture> nt = ImageTexture::create_from_image(canopy_edit);
	wm->set_canopy_map(nt);
	canopy_texture = nt->get_instance_id();
}

// Wind on the parts a storm broke off, while they fall (the drag of the crown they carry, with
// its Vogel streamlining, on the wind relative to the part), the hinges that let go, and the
// stubs' caps following the stems they close.
void TreeWind3D::_storm_parts(double p_delta) {
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	for (int k = int(storm_parts.size()) - 1; k >= 0; k--) {
		StormPart &sp = storm_parts[k];
		RigidBody3D *body = ObjectDB::get_instance<RigidBody3D>(sp.body);
		if (!body || !body->is_inside_tree()) {
			if (sp.joint.is_valid()) {
				ps->free_rid(sp.joint);
			}
			storm_parts.remove_at(k);
			continue;
		}
		sp.age += float(p_delta);
		const Transform3D g = body->get_global_transform();
		if (sp.joint.is_valid() && sp.release > 0.0f) {
			const float angle = Quaternion((sp.start.inverse() * g.basis).orthonormalized()).get_angle();
			if (angle > sp.release) {
				ps->free_rid(sp.joint);
				sp.joint = RID();
			}
		}
		if (sp.age < 8.0f) {
			const Vector3 at = g.xform(sp.load);
			const WindSample ws = sample_wind(at, MAX(sp.load.y, 2.0f));
			// Storm gusts run about a quarter over the mean through a fall of a few seconds.
			Vector3 v = ws.mean * 1.25f - body->get_linear_velocity();
			const float vs = v.length();
			const float cd = DRAG_COEFFICIENT * (vs > RECONFIGURATION_SPEED ? Math::pow(vs / RECONFIGURATION_SPEED, sp.vogel) : 1.0f);
			const Vector3 f = v * (0.5f * AIR_DENSITY * cd * sp.area * vs * drag_scale);
			body->set_constant_force(f);
			body->set_constant_torque((at - g.xform(body->get_center_of_mass())).cross(f));
		} else {
			body->set_constant_force(Vector3());
			body->set_constant_torque(Vector3());
			if (!sp.joint.is_valid() && body->is_sleeping()) {
				// At rest: frozen, so a fallen tree costs a static body.
				body->set_freeze_enabled(true);
				storm_parts.remove_at(k);
			}
		}
	}
	for (int k = int(caps.size()) - 1; k >= 0; k--) {
		const Cap &cp = caps[k];
		Node3D *node = ObjectDB::get_instance<Node3D>(cp.node);
		const Target *found = nullptr;
		for (const Target &t : targets) {
			if (t.node == cp.target) {
				found = &t;
				break;
			}
		}
		MultiMeshInstance3D *mmi = found ? ObjectDB::get_instance<MultiMeshInstance3D>(found->node) : nullptr;
		if (!node || !mmi || cp.instance >= int(found->placements.size())) {
			if (node) {
				node->queue_free();
			}
			caps.remove_at(k);
			continue;
		}
		const float *rec = found->buffer.ptr() + (size_t(cp.instance) * found->bones + cp.bone) * RECORD_FLOATS;
		node->set_global_transform(mmi->get_global_transform() * read_record(rec) * cp.local);
	}
}

Dictionary TreeWind3D::get_storm_load(const Ref<Mesh> &p_mesh, const Transform3D &p_placement, int p_seed) {
	Dictionary d;
	const Rig *rig = _rig_for(p_mesh);
	if (!rig) {
		return d;
	}
	_gather_waves();
	Transform3D bones[256];
	StormLoad sl;
	solve_pose(*rig, p_placement, uint32_t(p_seed), bones, nullptr, 0.0f, &sl);
	d["bone"] = sl.bone;
	d["ratio"] = sl.ratio;
	d["root_ratio"] = sl.root_ratio;
	d["radius"] = sl.bone >= 0 ? rig->bones[sl.bone].radius : 0.0f;
	d["height"] = sl.bone >= 0 ? rig->bones[sl.bone].pivot.y : 0.0f;
	d["storm"] = rig->storm;
	return d;
}

// The chunks of a broken part: one per bone of 3 cm and up (and the part's own first bone), thinner
// bones riding the chunk below them; a thrown tree's foot rides its stem's first segment.
void TreeWind3D::_blast_chunks(const Rig &p_rig, int p_bone, const LocalVector<uint8_t> &p_exclude, LocalVector<int> &r_chunk_of, LocalVector<int> &r_chunk_bone) {
	const uint32_t n = p_rig.bones.size();
	LocalVector<uint8_t> inside;
	inside.resize(n);
	for (uint32_t b = 0; b < n; b++) {
		inside[b] = b == uint32_t(p_bone) || (b > uint32_t(p_bone) && b > 0 && inside[p_rig.bones[b].parent]);
		if (b > 0 && p_exclude.size() == n && p_exclude[b]) {
			inside[b] = 0;
		}
	}
	r_chunk_of.resize(n);
	r_chunk_bone.clear();
	for (uint32_t b = 0; b < n; b++) {
		r_chunk_of[b] = -1;
	}
	for (uint32_t b = 0; b < n; b++) {
		if (!inside[b] || b == 0) {
			continue;
		}
		const int parent = p_rig.bones[b].parent;
		if (b == uint32_t(p_bone) || p_rig.bones[b].radius >= BLAST_CHUNK_RADIUS || r_chunk_of[parent] < 0) {
			r_chunk_of[b] = r_chunk_bone.size();
			r_chunk_bone.push_back(int(b));
			if (parent == 0 && r_chunk_of[0] < 0 && p_bone == 0) {
				r_chunk_of[0] = r_chunk_of[b];
			}
		} else {
			r_chunk_of[b] = r_chunk_of[parent];
		}
	}
}

// A broken part as a Blast asset (docs/ideas/trees.md section 5t), for its fall's impact: one chunk
// per bone of 3 cm and up (thinner bones ride the chunk below them), each chunk carrying every
// surface of its wood and foliage, a closed hull of its wood (cylinders at 0.85 of the bone's radius,
// inside the bark: the chunk's collision, volume and, where it breaks, its wood face) and a bond to
// the chunk it grows from with the health of the wood there (its breaking moment in kN m, over the
// node's health). The whole tree's root plate rides the stem's foot.
// Pure (no node, server or cache state): it runs on a worker thread, from the break on, so the asset is
// ready by the time the part lands.
Ref<Resource> TreeWind3D::_blast_asset(const Rig &p_rig, const Array &p_surface_arrays, int p_bone, const LocalVector<uint8_t> &p_exclude, const Transform3D &p_butt, const Vector3 &p_fall, float p_snap_cut, Array &r_materials) {
	const bool p_snap = p_snap_cut > 0.0f; // Snapped at a charred foot: no plate, cut at p_snap_cut over its stump.
	const uint64_t t_start = OS::get_singleton()->get_ticks_usec();
	Object *authoring = ClassDB::instantiate("PhysXBlastAuthoring");
	if (!authoring) {
		return Ref<Resource>();
	}
	Ref<RefCounted> holder(Object::cast_to<RefCounted>(authoring));
	const uint32_t n = p_rig.bones.size();
	LocalVector<int> chunk_of;
	LocalVector<int> chunk_bone; // First bone of each chunk.
	_blast_chunks(p_rig, p_bone, p_exclude, chunk_of, chunk_bone);
	const int chunks = chunk_bone.size();
	if (chunks < 2 || p_rig.mesh.is_null()) {
		return Ref<Resource>(); // One chunk: nothing to break; it stays the body it is.
	}
	// Materials: the tree's unskinned surfaces, then the broken wood (the fracture cap's).
	const int surfaces = p_rig.mesh->get_surface_count();
	Array mats;
	for (int s = 0; s < surfaces; s++) {
		mats.push_back(s < p_rig.static_materials.size() ? p_rig.static_materials[s] : Variant(p_rig.mesh->surface_get_material(s)));
	}
	Ref<Material> wood;
	if (p_rig.cap_kit.size() > 0) {
		const Array e = p_rig.cap_kit[0];
		wood = e[2];
	}
	const int wood_slot = mats.size();
	mats.push_back(wood);
	// Per chunk, per surface: the triangles whose first bone is in the chunk.
	struct Soup {
		LocalVector<Vector3> v, nrm;
		LocalVector<Vector2> uv, uv2;
		LocalVector<Color> col;
		LocalVector<float> tan;
		LocalVector<int32_t> idx;
		bool has_uv2 = false, has_tan = false;
	};
	LocalVector<LocalVector<Soup>> soups;
	soups.resize(chunks);
	for (int c = 0; c < chunks; c++) {
		soups[c].resize(surfaces);
	}
	const uint64_t t_soup = OS::get_singleton()->get_ticks_usec();
	for (int s = 0; s < surfaces && s < p_surface_arrays.size(); s++) {
		const Array arr = p_surface_arrays[s];
		if (arr.size() != Mesh::ARRAY_MAX) {
			continue;
		}
		const PackedVector3Array verts = arr[Mesh::ARRAY_VERTEX];
		const PackedVector3Array normals = arr[Mesh::ARRAY_NORMAL];
		const PackedVector2Array uvs = arr[Mesh::ARRAY_TEX_UV];
		const PackedVector2Array uv2s = arr[Mesh::ARRAY_TEX_UV2];
		const PackedColorArray colors = arr[Mesh::ARRAY_COLOR];
		const PackedFloat32Array tangents = arr[Mesh::ARRAY_TANGENT];
		const PackedInt32Array bones = arr[Mesh::ARRAY_BONES];
		const PackedInt32Array index = arr[Mesh::ARRAY_INDEX];
		if (bones.size() != verts.size() * 4 || index.is_empty()) {
			continue;
		}
		const int vn = verts.size();
		const bool has_uv2 = uv2s.size() == vn;
		const bool has_tan = tangents.size() == vn * 4;
		const bool has_n = normals.size() == vn, has_uv = uvs.size() == vn, has_col = colors.size() == vn;
		const Vector3 *vp = verts.ptr();
		const int32_t *bp = bones.ptr();
		const int32_t *ip = index.ptr();
		// Each vertex's copy in the chunk that took it last (a vertex on a chunk's border is copied
		// once per chunk it serves).
		LocalVector<int> remap_chunk, remap_index;
		remap_chunk.resize(vn);
		remap_index.resize(vn);
		for (int v = 0; v < vn; v++) {
			remap_chunk[v] = -1;
		}
		for (int i = 0; i + 2 < index.size(); i += 3) {
			const int bb = bp[ip[i] * 4];
			if (bb < 0 || bb >= int(n) || chunk_of[bb] < 0) {
				continue;
			}
			// A snapped stem's wood below the cut stays on the stump.
			if (p_snap && bb <= 1 && (vp[ip[i]].y + vp[ip[i + 1]].y + vp[ip[i + 2]].y) < 3.0f * p_snap_cut) {
				continue;
			}
			const int c = chunk_of[bb];
			Soup &so = soups[c][s];
			so.has_uv2 = has_uv2;
			so.has_tan = has_tan;
			for (int k = 0; k < 3; k++) {
				const int v = ip[i + k];
				if (remap_chunk[v] == c) {
					so.idx.push_back(remap_index[v]);
					continue;
				}
				const int ni = so.v.size();
				remap_chunk[v] = c;
				remap_index[v] = ni;
				so.v.push_back(vp[v]);
				so.nrm.push_back(has_n ? normals[v] : Vector3(0, 1, 0));
				so.uv.push_back(has_uv ? uvs[v] : Vector2());
				so.col.push_back(has_col ? colors[v] : Color(1, 1, 1));
				if (has_uv2) {
					so.uv2.push_back(uv2s[v]);
				}
				if (has_tan) {
					for (int q = 0; q < 4; q++) {
						so.tan.push_back(tangents[v * 4 + q]);
					}
				}
				so.idx.push_back(ni);
			}
		}
	}
	auto soup_arrays = [](const Soup &p_s) {
		Array a;
		a.resize(Mesh::ARRAY_MAX);
		PackedVector3Array v, nrm;
		PackedVector2Array uv, uv2;
		PackedColorArray col;
		PackedFloat32Array tan;
		PackedInt32Array idx;
		v.resize(p_s.v.size());
		memcpy(v.ptrw(), p_s.v.ptr(), sizeof(Vector3) * p_s.v.size());
		nrm.resize(p_s.nrm.size());
		memcpy(nrm.ptrw(), p_s.nrm.ptr(), sizeof(Vector3) * p_s.nrm.size());
		uv.resize(p_s.uv.size());
		memcpy(uv.ptrw(), p_s.uv.ptr(), sizeof(Vector2) * p_s.uv.size());
		col.resize(p_s.col.size());
		memcpy(col.ptrw(), p_s.col.ptr(), sizeof(Color) * p_s.col.size());
		idx.resize(p_s.idx.size());
		memcpy(idx.ptrw(), p_s.idx.ptr(), sizeof(int32_t) * p_s.idx.size());
		a[Mesh::ARRAY_VERTEX] = v;
		a[Mesh::ARRAY_NORMAL] = nrm;
		a[Mesh::ARRAY_TEX_UV] = uv;
		a[Mesh::ARRAY_COLOR] = col;
		if (p_s.has_uv2) {
			uv2.resize(p_s.uv2.size());
			memcpy(uv2.ptrw(), p_s.uv2.ptr(), sizeof(Vector2) * p_s.uv2.size());
			a[Mesh::ARRAY_TEX_UV2] = uv2;
		}
		if (p_s.has_tan) {
			tan.resize(p_s.tan.size());
			memcpy(tan.ptrw(), p_s.tan.ptr(), sizeof(float) * p_s.tan.size());
			a[Mesh::ARRAY_TANGENT] = tan;
		}
		a[Mesh::ARRAY_INDEX] = idx;
		return a;
	};
	// The hull: a closed eight-sided cylinder along each bone of the chunk, with end caps.
	auto cylinder = [](Soup &r_h, const Vector3 &a0, const Vector3 &a1, float r) {
		const Color wc(0.6f, 0.5f, 0.38f);
		{
			const Vector3 d = (a1 - a0).normalized();
			Vector3 u = d.cross(Vector3(0, 1, 0));
			if (u.length_squared() < 1e-4f) {
				u = d.cross(Vector3(1, 0, 0));
			}
			u.normalize();
			const Vector3 w = d.cross(u);
			const int base = r_h.v.size();
			for (int end = 0; end < 2; end++) {
				const Vector3 c = end == 0 ? a0 : a1;
				for (int k = 0; k < 8; k++) {
					const float t = float(Math::TAU) * k / 8.0f;
					const Vector3 rd = u * Math::cos(t) + w * Math::sin(t);
					r_h.v.push_back(c + rd * r);
					r_h.nrm.push_back(rd);
					r_h.uv.push_back(Vector2(k / 8.0f, end));
					r_h.col.push_back(wc);
				}
				r_h.v.push_back(c);
				r_h.nrm.push_back(end == 0 ? -d : d);
				r_h.uv.push_back(Vector2(0.5f, end));
				r_h.col.push_back(wc);
			}
			// Godot front faces wind clockwise seen from outside.
			for (int k = 0; k < 8; k++) {
				const int k1 = (k + 1) % 8;
				const int s0 = base + k, s1 = base + k1, t0 = base + 9 + k, t1 = base + 9 + k1;
				r_h.idx.push_back(s0);
				r_h.idx.push_back(t0);
				r_h.idx.push_back(s1);
				r_h.idx.push_back(s1);
				r_h.idx.push_back(t0);
				r_h.idx.push_back(t1);
				r_h.idx.push_back(base + 8);
				r_h.idx.push_back(s0);
				r_h.idx.push_back(s1);
				r_h.idx.push_back(base + 17);
				r_h.idx.push_back(t1);
				r_h.idx.push_back(t0);
			}
		}
	};
	auto hull = [&](int p_chunk, Soup &r_h) {
		for (uint32_t b = 1; b < n; b++) {
			if (chunk_of[b] != p_chunk) {
				continue;
			}
			// Wood of chunk size only: twigs fold on landing rather than prop the limb up, and
			// thousands of them would make a hull slow to cook.
			const Bone &bone = p_rig.bones[b];
			if (bone.radius < BLAST_CHUNK_RADIUS && chunk_bone[p_chunk] != int(b)) {
				continue;
			}
			// Starting clear of the wood it grows from, and a little short of its tip: hulls that
			// overlap push apart when they come to be in different pieces.
			const Vector3 d = bone.dir.normalized();
			float start = 0.005f;
			if (bone.parent > 0) {
				const Bone &pb = p_rig.bones[bone.parent];
				const float sine = d.cross(pb.dir.normalized()).length();
				if (sine > 0.3f) {
					start = pb.radius * 0.85f / sine + bone.radius * 0.85f * Math::sqrt(MAX(1.0f - sine * sine, 0.0f)) / sine;
				}
			}
			start = MIN(start, 0.4f * bone.length);
			if (p_snap && b == 1) {
				start = MAX(start, (p_snap_cut - bone.pivot.y) / MAX(d.y, 0.3f)); // Above the stump.
			}
			cylinder(r_h, bone.pivot + d * start, bone.pivot + d * MAX(bone.length - 0.005f, start + 0.01f), MAX(bone.radius * 0.85f, 0.005f));
		}
	};
	// The thrown tree's root plate (5k), drawn on the stem's foot.
	Array plate;
	int bark_slot = -1;
	for (int s = 0; s < surfaces; s++) {
		const Ref<ArrayMesh> am = p_rig.mesh;
		if (am.is_valid() && am->surface_get_name(s) == "bark") {
			bark_slot = s;
		}
	}
	const uint64_t t_plate = OS::get_singleton()->get_ticks_usec();
	if (p_bone == 0 && chunk_of[0] >= 0 && !p_snap) {
		plate = TreeGenerator::build_storm_plate(p_rig.storm_data);
	}
	const uint64_t t_plate_end = OS::get_singleton()->get_ticks_usec();
	// The part's butt: its fracture cap (unit size, placed by p_butt), on the part's first chunk.
	Array butt;
	if ((p_bone > 0 || p_snap) && p_butt.basis.determinant() != 0.0f && p_rig.cap_kit.size() > 0) {
		const Array e = p_rig.cap_kit[0];
		Array a = Array(e[1]).duplicate(true);
		PackedVector3Array v = a[Mesh::ARRAY_VERTEX];
		PackedVector3Array nn = a[Mesh::ARRAY_NORMAL];
		const Basis nb = p_butt.basis.inverse().transposed();
		for (int i = 0; i < v.size(); i++) {
			v.write[i] = p_butt.xform(v[i]);
			if (i < nn.size()) {
				nn.write[i] = nb.xform(nn[i]).normalized();
			}
		}
		a[Mesh::ARRAY_VERTEX] = v;
		a[Mesh::ARRAY_NORMAL] = nn;
		a[Mesh::ARRAY_TANGENT] = Variant();
		butt = a;
	}
	Array pieces;
	Array root;
	Soup root_hull;
	for (int c = 0; c < chunks; c++) {
		Soup h;
		hull(c, h);
		const bool foot = p_bone == 0 && c == chunk_of[0] && !p_snap;
		// The root plate's hull: a thin disc nearly its full width, so a thrown tree lies propped on
		// its plate standing on edge, the butt held off the ground (Mitchell 2013). Thin, because the
		// pieces weigh by hull volume: a full-thickness disc outweighs the stem and swings the tree
		// back upright on it.
		if (foot) {
			cylinder(h, Vector3(0, -0.25f * p_rig.plate_thick, 0), Vector3(), 0.8f * p_rig.plate_radius);
		}
		Array piece;
		Dictionary hs;
		hs["arrays"] = soup_arrays(h);
		hs["slot"] = wood_slot;
		piece.push_back(hs);
		for (int s = 0; s < surfaces; s++) {
			if (soups[c][s].idx.is_empty()) {
				continue;
			}
			Dictionary sd;
			sd["arrays"] = soup_arrays(soups[c][s]);
			sd["slot"] = s;
			piece.push_back(sd);
			Dictionary rd;
			rd["arrays"] = sd["arrays"];
			rd["slot"] = s;
			root.push_back(rd);
		}
		auto extra = [&](const Array &p_arrays, int p_slot) {
			Dictionary d;
			d["arrays"] = p_arrays;
			d["slot"] = p_slot;
			piece.push_back(d);
			root.push_back(d);
		};
		if (foot) {
			for (int i = 0; i < plate.size(); i++) {
				const Array e = plate[i];
				const String name = e[0];
				int slot = wood_slot;
				if (name == "bark" && bark_slot >= 0) {
					slot = bark_slot;
				} else if (name == "soil") {
					slot = mats.size();
					mats.push_back(e[2]);
				}
				extra(e[1], slot);
			}
		}
		if (c == chunk_of[p_bone] && butt.size() > 0) {
			extra(butt, wood_slot);
		}
		pieces.push_back(piece);
		const int off = root_hull.v.size();
		for (uint32_t k = 0; k < h.v.size(); k++) {
			root_hull.v.push_back(h.v[k]);
			root_hull.nrm.push_back(h.nrm[k]);
			root_hull.uv.push_back(h.uv[k]);
			root_hull.col.push_back(h.col[k]);
		}
		for (uint32_t k = 0; k < h.idx.size(); k++) {
			root_hull.idx.push_back(h.idx[k] + off);
		}
	}
	Dictionary rh;
	rh["arrays"] = soup_arrays(root_hull);
	rh["slot"] = wood_slot;
	root.insert(0, rh);
	// Bonds: each chunk to the one it grows from, at its first bone's pivot.
	Array bonds;
	for (int c = 0; c < chunks; c++) {
		const int fb = chunk_bone[c];
		if (fb == p_bone || p_rig.bones[fb].parent == 0) {
			continue;
		}
		const int pc = chunk_of[p_rig.bones[fb].parent];
		if (pc < 0 || pc == c) {
			continue;
		}
		const Bone &bone = p_rig.bones[fb];
		Dictionary bd;
		bd["chunks"] = Vector2i(c, pc);
		bd["centroid"] = bone.pivot;
		bd["normal"] = -bone.dir.normalized();
		bd["area"] = float(Math::PI) * bone.radius * bone.radius;
		bd["health"] = MAX(bone.strength / 1000.0f, 0.01f) / BLAST_NODE_HEALTH;
		bonds.push_back(bd);
	}
	const uint64_t t_cook = OS::get_singleton()->get_ticks_usec();
	const Variant res = authoring->call("build_prefractured", root, pieces, bonds, mats);
	print_verbose(vformat("TreeWind3D: Blast asset bone %d, %d chunks: soups %d ms, plate %d ms, arrays %d ms, authoring %d ms", p_bone, chunks, (t_plate - t_soup) / 1000, (t_plate_end - t_plate) / 1000, (t_cook - t_start) / 1000, (OS::get_singleton()->get_ticks_usec() - t_cook) / 1000));
	Ref<Resource> asset = res;
	if (asset.is_null()) {
		WARN_PRINT(vformat("TreeWind3D: Blast asset for bone %d failed: %s", p_bone, String(authoring->call("get_last_error"))));
	}
	r_materials = mats;
	return asset;
}

void TreeWind3D::_blast_job(BlastJob *p_job) {
	p_job->asset = _blast_asset(p_job->rig, p_job->surface_arrays, p_job->bone, p_job->exclude, p_job->butt, p_job->fall, p_job->snap_cut, p_job->materials);
	if (p_job->asset.is_valid() && p_job->asset->has_method("prepare_render")) {
		// Its chunks' render arrays in GPU format now, so the swap only uploads them.
		const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
		p_job->asset->call("prepare_render");
		print_verbose(vformat("TreeWind3D: Blast asset bone %d: prepare_render %d ms", p_job->bone, (OS::get_singleton()->get_ticks_usec() - t0) / 1000));
	}
}

// At the break: start building the part's Blast asset (cached by its key) on a worker thread.
void TreeWind3D::_blast_request(const StormPart &p_part) {
	if (blast_assets.has(p_part.key) || blast_jobs.has(p_part.key) || p_part.mesh.is_null() || !ClassDB::class_exists("PhysXBlastAuthoring")) {
		return;
	}
	const Rig *rig = _rig_for(p_part.mesh);
	if (!rig) {
		return;
	}
	// The mesh's arrays come from the rendering server: read them here, once per mesh.
	const uint64_t mid = p_part.mesh->get_instance_id();
	if (!mesh_arrays.has(mid)) {
		Array arrays;
		for (int s = 0; s < p_part.mesh->get_surface_count(); s++) {
			arrays.push_back(p_part.mesh->surface_get_arrays(s));
		}
		mesh_arrays[mid] = arrays;
	}
	BlastJob *job = memnew(BlastJob);
	job->rig = *rig;
	job->surface_arrays = mesh_arrays[mid];
	job->bone = p_part.bone;
	job->exclude = p_part.exclude;
	job->butt = p_part.butt;
	job->fall = p_part.fall;
	job->snap_cut = p_part.snap_cut;
	job->task = WorkerThreadPool::get_singleton()->add_template_task(this, &TreeWind3D::_blast_job, job, false, String("TreeWind3D Blast"));
	blast_jobs[p_part.key] = job;
}

// Whether the key's asset is built (or failed): collects a finished job.
bool TreeWind3D::_blast_ready(uint64_t p_key) {
	if (blast_assets.has(p_key)) {
		return true;
	}
	BlastJob **job = blast_jobs.getptr(p_key);
	if (!job) {
		return true; // Never requested (no Blast): the swap finds no asset and leaves the body be.
	}
	if (!WorkerThreadPool::get_singleton()->is_task_completed((*job)->task)) {
		return false;
	}
	WorkerThreadPool::get_singleton()->wait_for_task_completion((*job)->task);
	if ((*job)->asset.is_valid()) {
		blast_assets[p_key] = (*job)->asset;
		blast_materials[p_key] = (*job)->materials;
	}
	memdelete(*job);
	blast_jobs.erase(p_key);
	return true;
}

void TreeWind3D::_blast_wait_all() {
	for (KeyValue<uint64_t, BlastJob *> &kv : blast_jobs) {
		WorkerThreadPool::get_singleton()->wait_for_task_completion(kv.value->task);
		memdelete(kv.value);
	}
	blast_jobs.clear();
}

// A falling part that hits hard becomes a dynamic Blast destructible of its chunks in the same pose,
// and the impact breaks the bonds near the contact by how hard it was: limbs snap where the crown
// lands, the stem goes on down.
void TreeWind3D::_blast_swap(StormPart &r_part, RigidBody3D *p_body, const Vector3 &p_contact, float p_impulse) {
	if (!ClassDB::class_exists("PhysXDestructible3D")) {
		WARN_PRINT_ONCE("TreeWind3D: no PhysXDestructible3D (or rig) for a storm part's impact.");
		return;
	}
	const Ref<Resource> *found = blast_assets.getptr(r_part.key);
	if (!found || found->is_null()) {
		return;
	}
	const Ref<Resource> asset = *found;
	Object *obj = ClassDB::instantiate("PhysXDestructible3D");
	Node3D *node = Object::cast_to<Node3D>(obj);
	if (!node) {
		if (obj) {
			memdelete(obj);
		}
		return;
	}
	const Transform3D g = p_body->get_global_transform();
	node->set_name("TreeFall");
	node->set("blast_asset", asset);
	node->set("health", BLAST_NODE_HEALTH);
	node->set("auto_mass", false);
	node->set("mass", p_body->get_mass());
	node->set("dynamic", true);
	node->set("anchor", 2); // ANCHOR_NONE
	// Only the impacts below break it (each scaled to the wood); its own landings do not.
	node->set("impact_strength", 1.0e12);
	node->set("shatter_speed", 0.5);
	node->set("freeze_when_asleep", true);
	// It goes on falling as the part was.
	node->set("initial_linear_velocity", p_body->get_linear_velocity());
	node->set("initial_angular_velocity", p_body->get_angular_velocity());
	node->set_as_top_level(true);
	node->set_transform(Transform3D(g.basis.scaled_local(Vector3(r_part.scale, r_part.scale, r_part.scale)), g.origin));
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	add_child(node, false, INTERNAL_MODE_BACK);
	const uint64_t t1 = OS::get_singleton()->get_ticks_usec();
	// The impact: the momentum the contact took, over a lever of a metre, against the wood's
	// breaking moment (kN m), spread over a few metres around the contact.
	const float damage = p_impulse * 0.05f;
	node->call("apply_radial_damage", p_contact, damage, 1.5f, 4.0f); // Radii in tree space.
	print_verbose(vformat("TreeWind3D: Blast swap bone %d: enter tree %d ms, damage %d ms", r_part.bone, (t1 - t0) / 1000, (OS::get_singleton()->get_ticks_usec() - t1) / 1000));
	if (r_part.joint.is_valid()) {
		PhysicsServer3D::get_singleton()->free_rid(r_part.joint);
		r_part.joint = RID();
	}
	r_part.body = ObjectID();
	p_body->queue_free();
	storm_falls++;
	// For its settling: each chunk's mass, centre and wood (the segment it can rest on).
	const Rig *rig = _rig_for(r_part.mesh);
	if (rig) {
		StormFall fall;
		fall.node = node->get_instance_id();
		fall.scale = r_part.scale;
		LocalVector<int> chunk_of, chunk_bone;
		_blast_chunks(*rig, r_part.bone, r_part.exclude, chunk_of, chunk_bone);
		const uint32_t n = rig->bones.size();
		const uint32_t chunks = chunk_bone.size();
		fall.mass.resize(chunks);
		fall.center.resize(chunks);
		fall.low_a.resize(chunks);
		fall.low_b.resize(chunks);
		for (uint32_t c = 0; c < chunks; c++) {
			fall.mass[c] = 0.0f;
			fall.center[c] = Vector3();
			const Bone &fb = rig->bones[chunk_bone[c]];
			fall.low_a[c] = fb.pivot;
			fall.low_b[c] = fb.pivot + fb.dir.normalized() * fb.length;
		}
		fall.butt = chunk_of[r_part.bone];
		fall.parent.resize(chunks);
		fall.strength.resize(chunks);
		for (uint32_t c = 0; c < chunks; c++) {
			const Bone &fb = rig->bones[chunk_bone[c]];
			fall.parent[c] = (int(c) == fall.butt || fb.parent < 0) ? -1 : chunk_of[fb.parent];
			fall.strength[c] = MAX(fb.strength / 1000.0f, 0.01f);
		}
		for (uint32_t b = 1; b < n; b++) {
			const int c = chunk_of[b];
			if (c < 0) {
				continue;
			}
			float own = rig->bones[b].mass;
			for (uint32_t e = b + 1; e < n; e++) {
				if (rig->bones[e].parent == int(b)) {
					own -= rig->bones[e].mass;
				}
			}
			own = MAX(own, 0.0f);
			fall.mass[c] += own;
			fall.center[c] += (rig->bones[b].pivot + rig->bones[b].dir * (rig->bones[b].length * 0.5f)) * own;
		}
		const float s3 = r_part.scale * r_part.scale * r_part.scale;
		for (uint32_t c = 0; c < chunks; c++) {
			fall.center[c] = fall.mass[c] > 1e-6f ? fall.center[c] / fall.mass[c] : (fall.low_a[c] + fall.low_b[c]) * 0.5f;
			fall.mass[c] *= s3;
		}
		storm_settling.push_back(fall);
	}
}

// A fallen part at rest on its crown: the limb it rests on carries the share of its weight the
// foot does not, and breaks under it if that is more than its wood holds (a windthrown crown
// crushes into the ground until it lies on wood strong enough).
void TreeWind3D::_storm_settle(double p_delta) {
	const float g = 9.81f;
	for (int k = int(storm_settling.size()) - 1; k >= 0; k--) {
		StormFall &fall = storm_settling[k];
		Node3D *node = ObjectDB::get_instance<Node3D>(fall.node);
		fall.age += float(p_delta);
		if (!node || fall.age > 20.0f) {
			storm_settling.remove_at(k);
			continue;
		}
		if (fall.age < fall.next) {
			continue;
		}
		fall.next = fall.age + 0.25f;
		HashMap<int, Transform3D> seen;
		const int count = node->call("get_piece_count");
		for (int i = 0; i < count; i++) {
			const Transform3D t = node->call("get_piece_transform", i);
			const PackedInt32Array chunks = node->call("get_piece_chunks", i);
			if (chunks.size() < 2) {
				continue;
			}
			const int key = chunks[0];
			seen[key] = t;
			// Only a piece at rest (moved under 2 cm and half a degree in the last quarter second).
			const Transform3D *was = fall.last.getptr(key);
			const bool rest = was && was->origin.distance_to(t.origin) < 0.02f && Quaternion((was->basis.inverse() * t.basis).orthonormalized()).get_angle() < 0.01f;
			if (!rest) {
				continue;
			}
			auto world = [&](const Vector3 &p) { return t.origin + t.basis.xform(p * fall.scale); };
			float mass = 0.0f;
			Vector3 com;
			Vector3 foot;
			bool has_foot = false;
			int low_chunk = -1;
			Vector3 low;
			// Each chunk's lowest end, world space.
			LocalVector<int> ids;
			LocalVector<Vector3> ends;
			for (int ci = 0; ci < chunks.size(); ci++) {
				const int c = chunks[ci] - 1; // Asset chunk 0 is the whole.
				if (c < 0 || c >= int(fall.mass.size())) {
					continue;
				}
				mass += fall.mass[c];
				com += world(fall.center[c]) * fall.mass[c];
				if (c == fall.butt) {
					foot = world(fall.low_a[c]);
					has_foot = true;
					continue;
				}
				const Vector3 pa = world(fall.low_a[c]), pb = world(fall.low_b[c]);
				const Vector3 p = pa.y < pb.y ? pa : pb;
				ids.push_back(c);
				ends.push_back(p);
				if (low_chunk < 0 || p.y < low.y) {
					low = p;
					low_chunk = c;
				}
			}
			if (mass <= 0.0f || low_chunk < 0) {
				continue;
			}
			com /= mass;
			// Propped: the centre of mass well above what it rests on (a stem lying on the ground
			// has it a radius up).
			if (com.y - low.y < 1.0f) {
				continue;
			}
			// What it rests on: every limb end within 25 cm of the lowest (the ground there).
			LocalVector<int> rests;
			Vector3 rest_mid;
			for (uint32_t k2 = 0; k2 < ids.size(); k2++) {
				if (ends[k2].y < low.y + 0.25f) {
					rests.push_back(int(k2));
					rest_mid += ends[k2];
				}
			}
			rest_mid /= float(rests.size());
			// The rests' share of the weight: the foot takes the rest, by the levers about the centre.
			float share = 1.0f;
			if (has_foot) {
				const Vector2 c2(com.x, com.z), f2(foot.x, foot.z), l2(rest_mid.x, rest_mid.z);
				const float lf = f2.distance_to(c2), ll = l2.distance_to(c2);
				share = lf + ll > 1e-3f ? CLAMP(lf / (lf + ll), 0.0f, 1.0f) : 0.5f;
			}
			const float load = mass * g * share / float(rests.size()); // N, upward at each rest.
			HashSet<int> mine;
			for (int ci = 0; ci < chunks.size(); ci++) {
				mine.insert(chunks[ci] - 1);
			}
			// Up each limb from its rest toward the foot, the first joint whose wood cannot carry
			// that load's moment about it (its lever across the ground) breaks.
			const float s3 = fall.scale * fall.scale * fall.scale;
			int broken = 0;
			for (int r : rests) {
				const Vector3 at_rest = ends[r];
				for (int c = ids[r]; c >= 0 && c != fall.butt && mine.has(c) && fall.parent[c] >= 0 && mine.has(fall.parent[c]); c = fall.parent[c]) {
					const Vector3 at = world(fall.low_a[c]);
					const float moment = load * Vector2(at.x - at_rest.x, at.z - at_rest.z).length() / 1000.0f / s3; // kN m at scale 1
					if (moment > fall.strength[c]) {
						node->call("apply_radial_damage", at, moment, 0.05f, 0.3f);
						broken++;
						break;
					}
				}
			}
			print_verbose(vformat("TreeWind3D: settle piece %d (%.0f kg, centre %.2f m up): %d rests, %.1f kN each, %d joints break", i, mass, com.y - low.y, rests.size(), load / 1000.0f, broken));
			if (broken > 0) {
				break; // The pieces changed: the next check sees them anew.
			}
		}
		fall.last = seen;
	}
}

// Contacts of the falling parts, in the physics step: the first hard hit swaps a part for Blast.
void TreeWind3D::_storm_impacts() {
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	for (StormPart &sp : storm_parts) {
		if (sp.swapped || sp.age < 0.2f) {
			continue;
		}
		RigidBody3D *body = ObjectDB::get_instance<RigidBody3D>(sp.body);
		if (!body || !body->is_inside_tree()) {
			continue;
		}
		PhysicsDirectBodyState3D *state = ps->body_get_direct_state(body->get_rid());
		if (!state) {
			continue;
		}
		float best = 0.0f;
		Vector3 at;
		for (int c = 0; c < state->get_contact_count(); c++) {
			const float j = state->get_contact_impulse(c).length();
			if (j > best) {
				best = j;
				at = state->get_contact_local_position(c);
			}
		}
		// A thrown tree swaps only once it has turned 30 degrees on its plate's hinge: the plate's
		// hull (nearly its full disc) is then clear of the ground but at its hinge edge, and the
		// tree cannot be stood back up on it as on a pedestal. It swaps on a hit past that, or once
		// it comes to rest (propped on its crown, which the crush then breaks).
		float turned = 2.0f;
		if (sp.bone == 0) {
			turned = Quaternion((sp.start.inverse() * body->get_global_transform().basis).orthonormalized()).get_angle();
		}
		// A hit that changes the part's whole speed by 1.5 m/s or more (a crown landing, not
		// rolling or sliding).
		if (!sp.hit && turned > 0.5f && best > 1.5f * float(body->get_mass())) {
			sp.hit = true;
			sp.hit_local = body->get_global_transform().affine_inverse().xform(at);
			sp.hit_impulse = best;
		} else if (!sp.hit && turned > 0.5f && sp.age > 1.0f && body->get_angular_velocity().length() < 0.15f) {
			sp.hit = true;
			sp.hit_local = body->get_center_of_mass();
			sp.hit_impulse = float(body->get_mass()) * 1.5f;
		}
		// Swapped once its asset is built (it almost always is by the landing).
		if (sp.hit && _blast_ready(sp.key)) {
			sp.swapped = true;
			_blast_swap(sp, body, body->get_global_transform().xform(sp.hit_local), sp.hit_impulse);
		}
	}
}

// Static colliders of trunks and limbs (capsules from TreeGenerator.get_collision_capsules()),
// only for instances inside collision_range, on their own collision layer.
void TreeWind3D::_update_colliders(Target &r_target, const Transform3D &p_world, const Vector3 &p_eye) {
	const Rig &rig = *r_target.rig;
	const int caps = rig.colliders.size() / 8;
	if (caps == 0 || !is_inside_tree() || get_world_3d().is_null()) {
		return;
	}
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	const RID space = get_world_3d()->get_space();
	const float *c = rig.colliders.ptr();
	for (uint32_t i = 0; i < r_target.placements.size(); i++) {
		const Transform3D tree = p_world * r_target.placements[i];
		const Touch *tcu = r_target.touch.getptr(int(i));
		const bool near = !(tcu && tcu->uprooted) && tree.origin.distance_to(p_eye) <= collision_range;
		Target::Body *have = r_target.bodies.getptr(int(i));
		if (near == (have != nullptr)) {
			continue;
		}
		if (!near) {
			ps->free_rid(have->body);
			for (const RID &sh : have->shapes) {
				ps->free_rid(sh);
			}
			r_target.bodies.erase(int(i));
			continue;
		}
		Target::Body nb;
		nb.body = ps->body_create();
		ps->body_set_mode(nb.body, PhysicsServer3D::BODY_MODE_STATIC);
		ps->body_set_collision_layer(nb.body, collision_layer);
		ps->body_set_collision_mask(nb.body, collision_mask);
		ps->body_attach_object_instance_id(nb.body, get_instance_id());
		const float scale = MAX(Math::pow(float(Math::abs(tree.basis.determinant())), 1.0f / 3.0f), 1e-3f);
		for (int k = 0; k < caps; k++) {
			const float *o = c + k * 8;
			const int bone = int(o[7]);
			if (tcu && bone >= 0 && bone < int(tcu->broken.size()) && tcu->broken[bone]) {
				continue; // Broken off by a storm or a body: the part has its own.
			}
			const Vector3 a = Vector3(o[0], o[1], o[2]) * scale;
			const Vector3 b = Vector3(o[3], o[4], o[5]) * scale;
			const float r = o[6] * scale;
			const Vector3 axis = b - a;
			if (axis.length() < 1e-3f) {
				continue;
			}
			const RID sh = ps->capsule_shape_create();
			Dictionary d;
			d["radius"] = r;
			d["height"] = axis.length() + 2.0f * r;
			ps->shape_set_data(sh, d);
			ps->body_add_shape(nb.body, sh, Transform3D(Basis(Quaternion(Vector3(0, 1, 0), axis.normalized())), (a + b) * 0.5f));
			nb.shapes.push_back(sh);
		}
		ps->body_set_state(nb.body, PhysicsServer3D::BODY_STATE_TRANSFORM, Transform3D(tree.basis.orthonormalized(), tree.origin));
		ps->body_set_space(nb.body, space);
		if (tcu) {
			// The stub and the parts broken off it start touching at the break.
			for (const ObjectID &id : tcu->parts) {
				const CollisionObject3D *part = ObjectDB::get_instance<CollisionObject3D>(id);
				if (part) {
					ps->body_add_collision_exception(nb.body, part->get_rid());
					ps->body_add_collision_exception(part->get_rid(), nb.body);
				}
			}
		}
		r_target.bodies[int(i)] = nb;
	}
}

void TreeWind3D::_free_colliders(Target &r_target) {
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	for (KeyValue<int, Target::Body> &kv : r_target.bodies) {
		ps->free_rid(kv.value.body);
		for (const RID &sh : kv.value.shapes) {
			ps->free_rid(sh);
		}
	}
	r_target.bodies.clear();
}

void TreeWind3D::set_collision_enabled(bool p_enabled) {
	collision_enabled = p_enabled;
	if (!p_enabled) {
		for (Target &t : targets) {
			_free_colliders(t);
		}
	}
}

int TreeWind3D::get_collider_count() const {
	int count = 0;
	for (const Target &t : targets) {
		count += t.bodies.size();
	}
	return count;
}

PackedVector3Array TreeWind3D::get_touch_bends(int p_target, int p_instance) const {
	PackedVector3Array out;
	ERR_FAIL_INDEX_V(p_target, int(targets.size()), out);
	const Touch *tc = targets[p_target].touch.getptr(p_instance);
	if (!tc) {
		return out;
	}
	for (const Vector3 &a : tc->angle) {
		out.push_back(a);
	}
	return out;
}

PackedFloat32Array TreeWind3D::get_rig_frequencies(const Ref<Mesh> &p_mesh) {
	PackedFloat32Array f;
	if (p_mesh.is_null() || !p_mesh->has_meta(SNAME("tree_rig"))) {
		return f;
	}
	const Rig rig = parse_rig(p_mesh->get_meta(SNAME("tree_rig")), -0.7f);
	f.resize(rig.bones.size());
	for (uint32_t b = 0; b < rig.bones.size(); b++) {
		f.set(b, rig.bones[b].omega / float(Math::TAU));
	}
	return f;
}

PackedVector3Array TreeWind3D::get_bone_bends(const Ref<Mesh> &p_mesh, const Transform3D &p_placement, int p_seed) {
	PackedVector3Array out;
	const Rig *rig = _rig_for(p_mesh);
	if (!rig) {
		return out;
	}
	_gather_waves();
	Transform3D bones[256];
	solve_pose(*rig, p_placement, uint32_t(p_seed), bones);
	out.resize(rig->bones.size());
	for (uint32_t b = 0; b < rig->bones.size(); b++) {
		// Rotation of the bone relative to its parent, as axis * angle.
		const Transform3D rel = bones[MIN(uint32_t(rig->bones[b].parent), b)].affine_inverse() * bones[b];
		Vector3 axis;
		real_t angle = 0.0f;
		rel.basis.orthonormalized().get_axis_angle(axis, angle);
		out.set(b, b == 0 ? Vector3() : axis * angle);
	}
	return out;
}

void TreeWind3D::set_target(const NodePath &p_path) {
	target_path = p_path;
	targets.clear();
	rescan_timer = 0.0f;
}

void TreeWind3D::set_weather_map(const NodePath &p_path) {
	weather_map_path = p_path;
}

void TreeWind3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			set_process_internal(true);
			set_physics_process_internal(true);
		} break;
		case NOTIFICATION_EXIT_TREE: {
			for (Target &t : targets) {
				_free_colliders(t);
			}
			for (StormPart &sp : storm_parts) {
				if (sp.joint.is_valid()) {
					PhysicsServer3D::get_singleton()->free_rid(sp.joint);
				}
			}
			storm_parts.clear();
			_blast_wait_all();
		} break;
		case NOTIFICATION_INTERNAL_PHYSICS_PROCESS: {
			if (!storm_parts.is_empty()) {
				_storm_impacts();
			}
			if (!storm_settling.is_empty()) {
				_storm_settle(get_physics_process_delta_time());
			}
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			if (active && !Engine::get_singleton()->is_editor_hint()) {
				update(get_process_delta_time());
			}
		} break;
	}
}

void TreeWind3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_target", "path"), &TreeWind3D::set_target);
	ClassDB::bind_method(D_METHOD("get_target"), &TreeWind3D::get_target);
	ClassDB::bind_method(D_METHOD("set_weather_map", "path"), &TreeWind3D::set_weather_map);
	ClassDB::bind_method(D_METHOD("get_weather_map"), &TreeWind3D::get_weather_map);
	ClassDB::bind_method(D_METHOD("set_lod_enabled", "enabled"), &TreeWind3D::set_lod_enabled);
	ClassDB::bind_method(D_METHOD("is_lod_enabled"), &TreeWind3D::is_lod_enabled);
	ClassDB::bind_method(D_METHOD("set_lod_pixel_error", "pixels"), &TreeWind3D::set_lod_pixel_error);
	ClassDB::bind_method(D_METHOD("get_lod_pixel_error"), &TreeWind3D::get_lod_pixel_error);
	ClassDB::bind_method(D_METHOD("set_budget_msec", "msec"), &TreeWind3D::set_budget_msec);
	ClassDB::bind_method(D_METHOD("get_budget_msec"), &TreeWind3D::get_budget_msec);
	ClassDB::bind_method(D_METHOD("get_lod_bias"), &TreeWind3D::get_lod_bias);
	ClassDB::bind_method(D_METHOD("get_skipped_instances"), &TreeWind3D::get_skipped_instances);
	ClassDB::bind_method(D_METHOD("set_wind_range", "range"), &TreeWind3D::set_wind_range);
	ClassDB::bind_method(D_METHOD("get_wind_range"), &TreeWind3D::get_wind_range);
	ClassDB::bind_method(D_METHOD("set_drag_scale", "scale"), &TreeWind3D::set_drag_scale);
	ClassDB::bind_method(D_METHOD("get_drag_scale"), &TreeWind3D::get_drag_scale);
	ClassDB::bind_method(D_METHOD("set_stiffness_scale", "scale"), &TreeWind3D::set_stiffness_scale);
	ClassDB::bind_method(D_METHOD("get_stiffness_scale"), &TreeWind3D::get_stiffness_scale);
	ClassDB::bind_method(D_METHOD("set_max_bend", "angle"), &TreeWind3D::set_max_bend);
	ClassDB::bind_method(D_METHOD("get_max_bend"), &TreeWind3D::get_max_bend);
	ClassDB::bind_method(D_METHOD("set_fallback_wind", "wind"), &TreeWind3D::set_fallback_wind);
	ClassDB::bind_method(D_METHOD("get_fallback_wind"), &TreeWind3D::get_fallback_wind);
	ClassDB::bind_method(D_METHOD("set_fallback_turbulence", "intensity"), &TreeWind3D::set_fallback_turbulence);
	ClassDB::bind_method(D_METHOD("get_fallback_turbulence"), &TreeWind3D::get_fallback_turbulence);
	ClassDB::bind_method(D_METHOD("set_active", "active"), &TreeWind3D::set_active);
	ClassDB::bind_method(D_METHOD("is_active"), &TreeWind3D::is_active);
	ClassDB::bind_method(D_METHOD("set_time", "time"), &TreeWind3D::set_time);
	ClassDB::bind_method(D_METHOD("get_time"), &TreeWind3D::get_time);
	ClassDB::bind_method(D_METHOD("set_collision_enabled", "enabled"), &TreeWind3D::set_collision_enabled);
	ClassDB::bind_method(D_METHOD("is_collision_enabled"), &TreeWind3D::is_collision_enabled);
	ClassDB::bind_method(D_METHOD("set_collision_range", "range"), &TreeWind3D::set_collision_range);
	ClassDB::bind_method(D_METHOD("get_collision_range"), &TreeWind3D::get_collision_range);
	ClassDB::bind_method(D_METHOD("set_collision_layer", "layer"), &TreeWind3D::set_collision_layer);
	ClassDB::bind_method(D_METHOD("get_collision_layer"), &TreeWind3D::get_collision_layer);
	ClassDB::bind_method(D_METHOD("set_collision_mask", "mask"), &TreeWind3D::set_collision_mask);
	ClassDB::bind_method(D_METHOD("get_collision_mask"), &TreeWind3D::get_collision_mask);
	ClassDB::bind_method(D_METHOD("set_touch_enabled", "enabled"), &TreeWind3D::set_touch_enabled);
	ClassDB::bind_method(D_METHOD("is_touch_enabled"), &TreeWind3D::is_touch_enabled);
	ClassDB::bind_method(D_METHOD("set_touch_group", "group"), &TreeWind3D::set_touch_group);
	ClassDB::bind_method(D_METHOD("get_touch_group"), &TreeWind3D::get_touch_group);
	ClassDB::bind_method(D_METHOD("set_touch_radius", "radius"), &TreeWind3D::set_touch_radius);
	ClassDB::bind_method(D_METHOD("get_touch_radius"), &TreeWind3D::get_touch_radius);
	ClassDB::bind_method(D_METHOD("set_touch_stiffness", "stiffness"), &TreeWind3D::set_touch_stiffness);
	ClassDB::bind_method(D_METHOD("get_touch_stiffness"), &TreeWind3D::get_touch_stiffness);
	ClassDB::bind_method(D_METHOD("set_break_speed", "speed"), &TreeWind3D::set_break_speed);
	ClassDB::bind_method(D_METHOD("get_break_speed"), &TreeWind3D::get_break_speed);
	ClassDB::bind_method(D_METHOD("set_break_radius", "radius"), &TreeWind3D::set_break_radius);
	ClassDB::bind_method(D_METHOD("get_break_radius"), &TreeWind3D::get_break_radius);
	ClassDB::bind_method(D_METHOD("get_collider_count"), &TreeWind3D::get_collider_count);
	ClassDB::bind_method(D_METHOD("set_storm_enabled", "enabled"), &TreeWind3D::set_storm_enabled);
	ClassDB::bind_method(D_METHOD("is_storm_enabled"), &TreeWind3D::is_storm_enabled);
	ClassDB::bind_method(D_METHOD("set_storm_strength", "scale"), &TreeWind3D::set_storm_strength);
	ClassDB::bind_method(D_METHOD("get_storm_strength"), &TreeWind3D::get_storm_strength);
	ClassDB::bind_method(D_METHOD("set_storm_anchorage", "scale"), &TreeWind3D::set_storm_anchorage);
	ClassDB::bind_method(D_METHOD("get_storm_anchorage"), &TreeWind3D::get_storm_anchorage);
	ClassDB::bind_method(D_METHOD("set_storm_min_radius", "radius"), &TreeWind3D::set_storm_min_radius);
	ClassDB::bind_method(D_METHOD("get_storm_min_radius"), &TreeWind3D::get_storm_min_radius);
	ClassDB::bind_method(D_METHOD("set_storm_min_wind", "speed"), &TreeWind3D::set_storm_min_wind);
	ClassDB::bind_method(D_METHOD("get_storm_min_wind"), &TreeWind3D::get_storm_min_wind);
	ClassDB::bind_method(D_METHOD("get_storm_break_count"), &TreeWind3D::get_storm_break_count);
	ClassDB::bind_method(D_METHOD("get_storm_throw_count"), &TreeWind3D::get_storm_throw_count);
	ClassDB::bind_method(D_METHOD("set_snow_load_enabled", "enabled"), &TreeWind3D::set_snow_load_enabled);
	ClassDB::bind_method(D_METHOD("is_snow_load_enabled"), &TreeWind3D::is_snow_load_enabled);
	ClassDB::bind_method(D_METHOD("set_snow_creep_hours", "hours"), &TreeWind3D::set_snow_creep_hours);
	ClassDB::bind_method(D_METHOD("get_snow_creep_hours"), &TreeWind3D::get_snow_creep_hours);
	ClassDB::bind_method(D_METHOD("get_snow_break_count"), &TreeWind3D::get_snow_break_count);
	ClassDB::bind_method(D_METHOD("get_snow_shaken_kg"), &TreeWind3D::get_snow_shaken_kg);
	ClassDB::bind_method(D_METHOD("get_instance_snow_kg", "target", "instance"), &TreeWind3D::get_instance_snow_kg);
	ClassDB::bind_method(D_METHOD("set_glaze_load_enabled", "enabled"), &TreeWind3D::set_glaze_load_enabled);
	ClassDB::bind_method(D_METHOD("is_glaze_load_enabled"), &TreeWind3D::is_glaze_load_enabled);
	ClassDB::bind_method(D_METHOD("get_glaze_break_count"), &TreeWind3D::get_glaze_break_count);
	ClassDB::bind_method(D_METHOD("get_instance_glaze_kg", "target", "instance"), &TreeWind3D::get_instance_glaze_kg);
	ClassDB::bind_static_method("TreeWind3D", D_METHOD("glaze_kg", "surface", "glaze_mm", "scale"), &TreeWind3D::glaze_kg);
	ClassDB::bind_method(D_METHOD("get_instance_cap_level", "target", "instance"), &TreeWind3D::get_instance_cap_level);
	ClassDB::bind_method(D_METHOD("set_snow_cap_range", "range"), &TreeWind3D::set_snow_cap_range);
	ClassDB::bind_method(D_METHOD("get_snow_cap_range"), &TreeWind3D::get_snow_cap_range);
	ClassDB::bind_method(D_METHOD("get_snow_caps_drawn"), &TreeWind3D::get_snow_caps_drawn);
	ClassDB::bind_method(D_METHOD("set_snow_clumps_enabled", "enabled"), &TreeWind3D::set_snow_clumps_enabled);
	ClassDB::bind_method(D_METHOD("is_snow_clumps_enabled"), &TreeWind3D::is_snow_clumps_enabled);
	ClassDB::bind_method(D_METHOD("get_snow_clumps_dropped"), &TreeWind3D::get_snow_clumps_dropped);
	ClassDB::bind_method(D_METHOD("get_snow_clumps_falling"), &TreeWind3D::get_snow_clumps_falling);
	ClassDB::bind_method(D_METHOD("set_report_windthrow", "enabled"), &TreeWind3D::set_report_windthrow);
	ClassDB::bind_method(D_METHOD("is_report_windthrow"), &TreeWind3D::is_report_windthrow);
	ClassDB::bind_method(D_METHOD("get_windthrow_patch_count"), &TreeWind3D::get_windthrow_patch_count);
	ClassDB::bind_method(D_METHOD("set_avalanche_enabled", "enabled"), &TreeWind3D::set_avalanche_enabled);
	ClassDB::bind_method(D_METHOD("is_avalanche_enabled"), &TreeWind3D::is_avalanche_enabled);
	ClassDB::bind_method(D_METHOD("get_avalanche_break_count"), &TreeWind3D::get_avalanche_break_count);
	ClassDB::bind_method(D_METHOD("get_avalanche_throw_count"), &TreeWind3D::get_avalanche_throw_count);
	ClassDB::bind_method(D_METHOD("get_canopy_holes_cut"), &TreeWind3D::get_canopy_holes_cut);
	ClassDB::bind_static_method("TreeWind3D", D_METHOD("avalanche_strength", "stem_diameter", "crown"), &TreeWind3D::avalanche_strength);
	ClassDB::bind_method(D_METHOD("get_storm_fall_count"), &TreeWind3D::get_storm_fall_count);
	ClassDB::bind_method(D_METHOD("get_storm_snap_count"), &TreeWind3D::get_storm_snap_count);
	ClassDB::bind_method(D_METHOD("get_snag_count"), &TreeWind3D::get_snag_count);
	ClassDB::bind_method(D_METHOD("get_storm_load", "mesh", "placement", "seed"), &TreeWind3D::get_storm_load);
	ClassDB::bind_method(D_METHOD("get_piece_count"), &TreeWind3D::get_piece_count);
	ClassDB::bind_method(D_METHOD("get_touch_bends", "target", "instance"), &TreeWind3D::get_touch_bends);
	ClassDB::bind_method(D_METHOD("rescan"), &TreeWind3D::rescan);
	ClassDB::bind_method(D_METHOD("update", "delta"), &TreeWind3D::update);
	ClassDB::bind_method(D_METHOD("get_target_count"), &TreeWind3D::get_target_count);
	ClassDB::bind_method(D_METHOD("get_solved_instances"), &TreeWind3D::get_solved_instances);
	ClassDB::bind_method(D_METHOD("get_solve_msec"), &TreeWind3D::get_solve_msec);
	ClassDB::bind_method(D_METHOD("get_bone_bends", "mesh", "placement", "seed"), &TreeWind3D::get_bone_bends);
	ClassDB::bind_static_method("TreeWind3D", D_METHOD("get_rig_frequencies", "mesh"), &TreeWind3D::get_rig_frequencies);
	ClassDB::bind_static_method("TreeWind3D", D_METHOD("make_multimesh", "mesh", "transforms", "custom_data"), &TreeWind3D::make_multimesh, DEFVAL(PackedColorArray()));
	ClassDB::bind_static_method("TreeWind3D", D_METHOD("get_mesh_bone_count", "mesh"), &TreeWind3D::get_mesh_bone_count);

	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "target"), "set_target", "get_target");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "weather_map", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "WeatherMap3D"), "set_weather_map", "get_weather_map");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wind_range", PROPERTY_HINT_RANGE, "0,2000,1,or_greater,suffix:m"), "set_wind_range", "get_wind_range");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "lod_enabled"), "set_lod_enabled", "is_lod_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lod_pixel_error", PROPERTY_HINT_RANGE, "0.05,8,0.05,suffix:px"), "set_lod_pixel_error", "get_lod_pixel_error");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "budget_msec", PROPERTY_HINT_RANGE, "0,20,0.1,suffix:ms"), "set_budget_msec", "get_budget_msec");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "drag_scale", PROPERTY_HINT_RANGE, "0,4,0.01"), "set_drag_scale", "get_drag_scale");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "stiffness_scale", PROPERTY_HINT_RANGE, "0.1,4,0.01"), "set_stiffness_scale", "get_stiffness_scale");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "max_bend", PROPERTY_HINT_RANGE, "0.05,1.5,0.01,radians_as_degrees"), "set_max_bend", "get_max_bend");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "fallback_wind", PROPERTY_HINT_NONE, "suffix:m/s"), "set_fallback_wind", "get_fallback_wind");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "fallback_turbulence", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_fallback_turbulence", "get_fallback_turbulence");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "active"), "set_active", "is_active");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "time", PROPERTY_HINT_NONE, "suffix:s", PROPERTY_USAGE_NONE), "set_time", "get_time");
	ADD_GROUP("Collision", "collision_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "collision_enabled"), "set_collision_enabled", "is_collision_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "collision_range", PROPERTY_HINT_RANGE, "0,500,1,or_greater,suffix:m"), "set_collision_range", "get_collision_range");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_layer", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_collision_layer", "get_collision_layer");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_mask", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_collision_mask", "get_collision_mask");
	ADD_GROUP("Touch", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "touch_enabled"), "set_touch_enabled", "is_touch_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "touch_group"), "set_touch_group", "get_touch_group");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "touch_radius", PROPERTY_HINT_RANGE, "0.05,5,0.01,suffix:m"), "set_touch_radius", "get_touch_radius");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "touch_stiffness", PROPERTY_HINT_RANGE, "10,100000,1,exp,suffix:N m/rad"), "set_touch_stiffness", "get_touch_stiffness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "break_speed", PROPERTY_HINT_RANGE, "0,30,0.1,suffix:m/s"), "set_break_speed", "get_break_speed");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "break_radius", PROPERTY_HINT_RANGE, "0,0.3,0.005,suffix:m"), "set_break_radius", "get_break_radius");
	ADD_GROUP("Storm", "storm_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "storm_enabled"), "set_storm_enabled", "is_storm_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "storm_strength", PROPERTY_HINT_RANGE, "0.1,4,0.01"), "set_storm_strength", "get_storm_strength");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "storm_anchorage", PROPERTY_HINT_RANGE, "0.1,4,0.01"), "set_storm_anchorage", "get_storm_anchorage");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "storm_min_radius", PROPERTY_HINT_RANGE, "0,0.3,0.005,suffix:m"), "set_storm_min_radius", "get_storm_min_radius");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "storm_min_wind", PROPERTY_HINT_RANGE, "0,60,0.5,suffix:m/s"), "set_storm_min_wind", "get_storm_min_wind");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "avalanche_enabled"), "set_avalanche_enabled", "is_avalanche_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "report_windthrow"), "set_report_windthrow", "is_report_windthrow");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "snow_load_enabled"), "set_snow_load_enabled", "is_snow_load_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "glaze_load_enabled"), "set_glaze_load_enabled", "is_glaze_load_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "snow_clumps_enabled"), "set_snow_clumps_enabled", "is_snow_clumps_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "snow_cap_range", PROPERTY_HINT_RANGE, "0,1000,1,suffix:m"), "set_snow_cap_range", "get_snow_cap_range");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "snow_creep_hours", PROPERTY_HINT_RANGE, "0,48,0.1,suffix:h"), "set_snow_creep_hours", "get_snow_creep_hours");
	ADD_SIGNAL(MethodInfo("storm_damage", PropertyInfo(Variant::VECTOR3, "position"), PropertyInfo(Variant::FLOAT, "radius"), PropertyInfo(Variant::BOOL, "uprooted"), PropertyInfo(Variant::DICTIONARY, "pit")));
}

TreeWind3D::TreeWind3D() {
}

TreeWind3D::~TreeWind3D() {
	_blast_wait_all();
}

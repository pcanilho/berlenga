/**************************************************************************/
/*  tree_wind_3d.cpp                                                      */
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


#include "tree_wind_3d.h"

#include "tree_generator.h"

#include "core/config/engine.h"
#include "core/math/random_pcg.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/os.h"
#include "modules/weather/weather_map_3d.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/physics/collision_shape_3d.h"
#include "scene/3d/physics/rigid_body_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/resources/3d/capsule_shape_3d.h"
#include "servers/physics_3d/physics_server_3d.h"
#include "scene/3d/multimesh_instance_3d.h"
#include "scene/main/viewport.h"
#include "servers/rendering/rendering_server.h"

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

uint32_t hash_u32(uint32_t p_x) {
	p_x ^= p_x >> 16;
	p_x *= 0x7feb352du;
	p_x ^= p_x >> 15;
	p_x *= 0x846ca68bu;
	p_x ^= p_x >> 16;
	return p_x;
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
	}
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
		if (p_mesh->has_meta(SNAME("tree_colliders"))) {
			rig.colliders = p_mesh->get_meta(SNAME("tree_colliders"));
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

void TreeWind3D::solve_pose(const Rig &p_rig, const Transform3D &p_world, uint32_t p_seed, Transform3D *r_bones, const Touch *p_touch) const {
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
	for (uint32_t b = 1; b < count; b++) {
		const Bone &bone = p_rig.bones[b];
		const Vector3 load_local = bone.pivot + bone.centroid;
		const Vector3 at = p_world.xform(load_local);
		const WindSample ws = sample_wind(at, MAX(load_local.y * scale, 0.5f));
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
			const float gain = MIN(1.0f / MAX(Math::sqrt(re * re + im * im), 1e-4f), 1.0f);
			const float lag = Math::atan2(im, re);
			g[MIN(i / per_axis, 2u)] += w.z * gain * Math::cos(w.x * pa + w.y * pc + w.w - lag);
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
		const Vector3 force = v * (0.5f * AIR_DENSITY * cd * bone.area * scale * scale * vs * drag_scale);
		// theta = moment / stiffness; the lever scales with scale and the stiffness with scale^3.
		Vector3 theta = bone.centroid.cross(force) / MAX(bone.stiffness * scale * scale * stiffness_scale, 1e-3f);
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
		if (p_touch && b < p_touch->broken.size() && p_touch->broken[b]) {
			// Snapped off: the bone's geometry collapses to its pivot (the piece is a rigid body now).
			r_bones[b] = Transform3D(Basis(Vector3(), Vector3(), Vector3()), r_bones[b].xform(bone.pivot));
		}
	}
}

void TreeWind3D::_solve_job(uint32_t p_index, int p_unused) {
	const Job &job = jobs[p_index];
	Target &t = *job.target;
	const Transform3D &placement = t.placements[job.instance];
	Transform3D bones[256];
	const Vector3 o = placement.origin;
	const uint32_t seed = hash_u32(uint32_t(int(o.x * 16.0f)) * 73856093u ^ uint32_t(int(o.z * 16.0f)) * 19349663u);
	solve_pose(*t.rig, job.world * placement, seed, bones, t.touch.getptr(job.instance));
	float *w = t.write + size_t(job.instance) * t.bones * RECORD_FLOATS;
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
		_scan_node(root);
	}
}

void TreeWind3D::_on_target_populated() {
	rescan_timer = 0.0f;
}

void TreeWind3D::rescan() {
	for (Target &t : targets) {
		_free_colliders(t);
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
	if (is_inside_tree() && get_viewport() && get_viewport()->get_camera_3d()) {
		eye = get_viewport()->get_camera_3d()->get_global_position();
	}
	jobs.clear();
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
			if (touched || at.distance_to(eye) <= wind_range) {
				jobs.push_back({ &t, int(i), world });
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
	for (Target *t : changed) {
		RenderingServer::get_singleton()->multimesh_set_buffer(t->multimesh->get_rid(), t->buffer);
	}
	solved_instances = jobs.size();
	solve_usec = OS::get_singleton()->get_ticks_usec() - t0;
}

void TreeWind3D::_gather_contacts(double p_delta) {
	contacts.clear();
	if (!is_inside_tree() || !touch_enabled) {
		return;
	}
	const Vector<Node *> nodes = get_tree()->get_nodes_in_group(touch_group);
	HashMap<ObjectID, Vector3> seen;
	for (Node *n : nodes) {
		Node3D *n3 = Object::cast_to<Node3D>(n);
		if (!n3 || !n3->is_inside_tree()) {
			continue;
		}
		Contact c;
		c.position = n3->get_global_position();
		const Vector3 *last = contact_last.getptr(n3->get_instance_id());
		c.velocity = last && p_delta > 1e-5 ? (c.position - *last) / float(p_delta) : Vector3();
		c.radius = n3->has_meta(SNAME("touch_radius")) ? float(n3->get_meta(SNAME("touch_radius"))) : touch_radius;
		contacts.push_back(c);
		seen[n3->get_instance_id()] = c.position;
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
Ref<ArrayMesh> TreeWind3D::_piece_mesh(const Rig &p_rig, int p_bone) {
	const uint64_t key = (p_rig.mesh.is_valid() ? uint64_t(p_rig.mesh->get_instance_id()) : 0u) * 1315423911u + uint64_t(p_bone);
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
	}
	for (int s = 0; s < p_rig.mesh->get_surface_count(); s++) {
		const Array arr = p_rig.mesh->surface_get_arrays(s);
		const PackedVector3Array verts = arr[Mesh::ARRAY_VERTEX];
		const PackedVector3Array normals = arr[Mesh::ARRAY_NORMAL];
		const PackedVector2Array uvs = arr[Mesh::ARRAY_TEX_UV];
		const PackedColorArray colors = arr[Mesh::ARRAY_COLOR];
		const PackedInt32Array bones = arr[Mesh::ARRAY_BONES];
		const PackedInt32Array index = arr[Mesh::ARRAY_INDEX];
		if (bones.size() != verts.size() * 4 || index.is_empty()) {
			continue;
		}
		HashMap<int, int> remap;
		PackedVector3Array ov, on;
		PackedVector2Array ou;
		PackedColorArray oc;
		PackedInt32Array oi;
		for (int i = 0; i + 2 < index.size(); i += 3) {
			const int bb = bones[index[i] * 4];
			if (bb < 0 || bb >= int(n) || !inside[bb]) {
				continue;
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
		piece->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, out);
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
		const bool near = tree.origin.distance_to(p_eye) <= collision_range;
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
		} break;
		case NOTIFICATION_EXIT_TREE: {
			for (Target &t : targets) {
				_free_colliders(t);
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
}

TreeWind3D::TreeWind3D() {
}

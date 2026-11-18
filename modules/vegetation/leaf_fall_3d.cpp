/**************************************************************************/
/*  leaf_fall_3d.cpp                                                      */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/* Copyright (c) 2026-present Paulo Canilho and Berlenga contributors.    */
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

#include "leaf_fall_3d.h"

#include "tree_generator.h"
#include "vegetation_field_3d.h"

#include "core/config/engine.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/multimesh_instance_3d.h"
#include "scene/main/viewport.h"
#include "scene/main/window.h"
#include "servers/rendering/rendering_server.h"

#include "modules/weather/weather_map_3d.h"

namespace {

// Andersen, Pesavento and Wang (2005): drag A - B cos 2a over a plate at angle of attack a, circulation lift
// C_T sin 2a, rotational lift C_R. Their fits to falling plates: A 1.4, B 1.0, C_T 1.2, C_R pi.
constexpr float PLATE_A = 1.4f;
constexpr float PLATE_B = 1.0f;
constexpr float PLATE_CT = 1.2f;
// C_R is half APW's pi: a leaf is far lighter than their plates, and at pi the rotational lift held a tumbling
// leaf up (it hovered at 5 cm/s); at pi / 2 it flutters down below its broadside terminal speed.
constexpr float PLATE_CR = float(Math::PI) * 0.5f;
// Rotational damping, APW's pi mu2 rho l^4 |w| w per span (mu2 0.2), with a small linear part.
constexpr float DAMP_LINEAR = 0.05f;
constexpr float DAMP_QUADRATIC = 0.2f * float(Math::PI);
constexpr float GRAVITY = 9.81f;

struct PlateState {
	Vector3 p;
	Vector3 v;
	Basis b;
	Vector3 w;
};

// One step of a thin plate (chord p_l, area p_s, mass p_m) in air moving at p_air.
void plate_step(PlateState &r_s, const Vector3 &p_air, float p_l, float p_s, float p_m, float p_flutter, float p_rho, float p_dt) {
	const Vector3 n = r_s.b.get_column(1).normalized();
	const Vector3 vrel = r_s.v - p_air;
	const float s = vrel.length();
	// Added mass of a plate moving along its normal (a disc of the chord's width per span): rho pi l S / 4.
	const float ma = p_rho * float(Math::PI) * p_l * p_s * 0.25f;
	Vector3 f(0, -p_m * GRAVITY, 0);
	Vector3 torque;
	if (s > 1e-4f) {
		const Vector3 vh = vrel / s;
		const float vn = vrel.dot(n);
		const float sina = MIN(Math::abs(vn) / s, 1.0f);
		const float cosa = Math::sqrt(MAX(1.0f - sina * sina, 0.0f));
		const float cos2a = 1.0f - 2.0f * sina * sina;
		const float q = 0.5f * p_rho * p_s;
		// Drag along the relative velocity.
		f += -q * (PLATE_A - PLATE_B * cos2a) * s * vrel;
		// Circulation lift, across the relative velocity toward the plate's low edge: the glide.
		Vector3 nperp = n - vh * vh.dot(n);
		const float np = nperp.length();
		if (np > 1e-5f) {
			f += -SIGN(vn) * (nperp / np) * (q * PLATE_CT * s * s * 2.0f * sina * cosa * p_flutter);
		}
		// Rotational lift: a tumbling plate carries a circulation of its own (the Magnus-like term).
		const Vector3 w_tumble = r_s.w - n * r_s.w.dot(n);
		f += 0.5f * p_rho * PLATE_CR * p_l * p_s * w_tumble.cross(vrel) * p_flutter;
		// Munk's moment from the added masses turns the plate broadside to the flow.
		const Vector3 vt = vrel - n * vn;
		torque += ma * vn * n.cross(vt);
	}
	// Translation: the normal part moves the added mass too.
	const float fn = f.dot(n);
	const Vector3 acc = (f - n * fn) / p_m + n * (fn / (p_m + ma));
	r_s.v += acc * p_dt;
	r_s.p += r_s.v * p_dt;
	// Rotation: a thin plate's moment of inertia about a chord-wise axis, with its added inertia.
	const float inertia = MAX(p_m * p_l * p_l / 12.0f + ma * p_l * p_l / 32.0f, 1e-12f);
	r_s.w += torque / inertia * p_dt;
	// The rotational damping, implicitly: a light leaf's damping rate is far beyond the step's.
	const float damp = (DAMP_LINEAR * s + DAMP_QUADRATIC * p_l * r_s.w.length()) * p_rho * p_s * p_l * p_l / inertia;
	r_s.w /= 1.0f + damp * p_dt;
	if (!r_s.w.is_finite() || !r_s.v.is_finite()) {
		r_s.w = Vector3();
		r_s.v = Vector3();
	}
	const float wl = r_s.w.length();
	if (wl > 1e-6f) {
		r_s.b = Basis(r_s.w / wl, wl * p_dt) * r_s.b;
	}
}

// Steps for one frame: at most 1/240 s, and a fifth of the period of the leaf's swing under Munk's moment, whose
// angular frequency is about |V| sqrt(m_a / I) (stiff for light leaves in a strong relative wind).
int plate_substeps(float p_dt, float p_speed, float p_l, float p_s, float p_m, float p_rho) {
	const float ma = p_rho * float(Math::PI) * p_l * p_s * 0.25f;
	const float inertia = MAX(p_m * p_l * p_l / 12.0f + ma * p_l * p_l / 32.0f, 1e-12f);
	const float omega = (p_speed + 0.5f) * Math::sqrt(ma / inertia);
	const float h = MIN(1.0f / 240.0f, float(Math::TAU) / MAX(omega, 1e-3f) / 5.0f);
	return CLAMP(int(Math::ceil(p_dt / h)), 1, 64);
}

} // namespace

PackedVector4Array LeafFall3D::simulate_plate(float p_length, float p_area, float p_mass, float p_flutter, const Vector3 &p_wind, float p_tilt_degrees, float p_seconds, float p_air_density) {
	PackedVector4Array out;
	PlateState s;
	s.p = Vector3();
	s.v = Vector3();
	s.b = Basis(Vector3(0, 0, 1), Math::deg_to_rad(p_tilt_degrees));
	const float dt = 1.0f / 240.0f;
	const int steps = int(p_seconds / dt);
	for (int i = 0; i < steps; i++) {
		plate_step(s, p_wind, p_length, p_area, p_mass, p_flutter, p_air_density, dt);
		if (i % 6 == 0) {
			const Vector3 n = s.b.get_column(1).normalized();
			out.push_back(Vector4(s.p.x, s.p.y, s.p.z, Math::rad_to_deg(Math::acos(CLAMP(Math::abs(n.y), 0.0f, 1.0f)))));
		}
		if ((i & 255) == 0) {
			s.b.orthonormalize();
		}
	}
	return out;
}

Vector3 LeafFall3D::_camera_position() const {
	if (is_inside_tree() && get_viewport() && get_viewport()->get_camera_3d()) {
		return get_viewport()->get_camera_3d()->get_global_position();
	}
	return get_global_position();
}

float LeafFall3D::_ground_at(const Vector3 &p_p) const {
	const WeatherMap3D *wm = WeatherMap3D::get_active();
	if (wm) {
		const float g = wm->get_terrain_ground_at(p_p);
		if (Math::is_finite(g)) {
			return g;
		}
		return wm->get_ground_height();
	}
	return get_global_position().y;
}

void LeafFall3D::_spawn(const Vector3 &p_p, const Vector3 &p_v, int p_kind, bool p_lying) {
	if (kinds.is_empty()) {
		return;
	}
	uint32_t slot;
	if (!free_slots.is_empty()) {
		slot = free_slots[free_slots.size() - 1];
		free_slots.resize(free_slots.size() - 1);
	} else if (int(leaves.size()) < max_leaves) {
		slot = leaves.size();
		leaves.push_back(Leaf());
	} else {
		// Full: take the leaf that has lain longest.
		int best = -1;
		float longest = -1.0f;
		for (uint32_t i = 0; i < leaves.size(); i++) {
			if (leaves[i].lying > longest) {
				longest = leaves[i].lying;
				best = i;
			}
		}
		if (best < 0) {
			return;
		}
		slot = best;
		alive_count--;
	}
	Leaf &l = leaves[slot];
	l = Leaf();
	l.alive = true;
	int kind = CLAMP(p_kind, 0, int(kinds.size()) - 1);
	if (kinds[kind].group_first >= 0 && kinds[kind].group_count > 1) {
		kind = kinds[kind].group_first + int(rng.rand() % uint32_t(kinds[kind].group_count));
	}
	l.kind = uint16_t(kind);
	l.p = p_p;
	l.v = p_v;
	// Any attitude: a detached leaf hangs at whatever angle its petiole held it.
	const Vector3 axis = Vector3(rng.randf() - 0.5f, rng.randf() - 0.5f, rng.randf() - 0.5f).normalized();
	l.b = Basis(axis.is_normalized() ? axis : Vector3(0, 1, 0), rng.randf() * float(Math::TAU));
	l.w = Vector3(rng.randf() - 0.5f, rng.randf() - 0.5f, rng.randf() - 0.5f) * 4.0f;
	l.scale = 0.8f + 0.4f * rng.randf();
	l.lying = p_lying ? 0.0f : -1.0f;
	if (p_lying) {
		l.b = Basis(Vector3(0, 1, 0), rng.randf() * float(Math::TAU));
		l.w = Vector3();
		l.v = Vector3();
	}
	alive_count++;
}

void LeafFall3D::drop_leaf(const Vector3 &p_position, int p_kind) {
	_spawn(p_position, Vector3(), p_kind, false);
}

void LeafFall3D::_step_leaf(Leaf &p_leaf, float p_delta, const WeatherMap3D *p_weather) {
	const Kind &k = kinds[p_leaf.kind];
	const float sc = p_leaf.scale;
	const float ground = _ground_at(p_leaf.p);
	if (p_leaf.lying >= 0.0f) {
		p_leaf.lying += p_delta;
		// Loose dry leaves lift when the wind 10 cm over the ground passes the threshold (gusts do it).
		if (p_weather) {
			const Vector3 a = p_weather->get_wind_at(p_leaf.p, 0.1f);
			const float u = Vector2(a.x, a.z).length();
			if (u > lift_speed && rng.randf() < (u / lift_speed - 1.0f) * 4.0f * p_delta) {
				p_leaf.lying = -1.0f;
				p_leaf.v = a * 0.4f + Vector3(0, 0.4f + 0.6f * rng.randf(), 0);
				p_leaf.w = Vector3(rng.randf() - 0.5f, rng.randf() - 0.5f, rng.randf() - 0.5f) * 10.0f;
				p_leaf.p.y = ground + 0.02f;
				lifted_count++;
			}
		}
		return;
	}
	p_leaf.age += p_delta;
	const Vector3 air = p_weather ? p_weather->get_wind_at(p_leaf.p) : Vector3();
	PlateState s{ p_leaf.p, p_leaf.v, p_leaf.b, p_leaf.w };
	const int n = plate_substeps(p_delta, (p_leaf.v - air).length(), k.length * sc, k.area * sc * sc, k.mass * sc * sc, air_density);
	const float dt = p_delta / n;
	for (int i = 0; i < n; i++) {
		plate_step(s, air, k.length * sc, k.area * sc * sc, k.mass * sc * sc, k.flutter, air_density, dt);
	}
	s.b.orthonormalize();
	p_leaf.p = s.p;
	p_leaf.v = s.v;
	p_leaf.b = s.b;
	p_leaf.w = s.w;
	if (p_leaf.p.y <= ground + 0.005f) {
		// Settled: flat on the ground, keeping its heading.
		p_leaf.p.y = ground + 0.005f;
		Vector3 x = p_leaf.b.get_column(0);
		x.y = 0.0f;
		x = x.length_squared() > 1e-6f ? x.normalized() : Vector3(1, 0, 0);
		const float flip = p_leaf.b.get_column(1).y < 0.0f && rng.randf() < 0.5f ? -1.0f : 1.0f;
		p_leaf.b = Basis(x, Vector3(0, flip, 0), x.cross(Vector3(0, flip, 0)));
		p_leaf.v = Vector3();
		p_leaf.w = Vector3();
		p_leaf.lying = 0.0f;
	}
}

void LeafFall3D::_step(float p_delta) {
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	const WeatherMap3D *wm = weather_map.is_empty() ? nullptr : Object::cast_to<WeatherMap3D>(get_node_or_null(weather_map));
	if (wm == nullptr) {
		wm = WeatherMap3D::get_active();
	}
	const Vector3 cam = _camera_position();
	const float time_scale = wm ? wm->get_time_scale() : 1.0f;
	if (auto_crowns) {
		auto_timer -= p_delta;
		if (auto_timer <= 0.0f) {
			auto_timer = 2.0f;
			_refresh_auto_crowns(wm, cam);
		}
	}

	// Shedding: the season's share a day, more in strong wind (the gusts break the ripe abscission layers).
	if (shed_accum.size() != crowns.size()) {
		shed_accum.resize(crowns.size());
		for (uint32_t i = 0; i < shed_accum.size(); i++) {
			shed_accum[i] = rng.randf();
		}
	}
	for (uint32_t i = 0; i < crowns.size(); i++) {
		const Crown &c = crowns[i];
		if (c.center.distance_to(cam) > range + c.radii.length()) {
			continue;
		}
		const Vector3 a = wm ? wm->get_wind_at(c.center) : Vector3();
		const float u = a.length();
		const float gust = 1.0f + wind_shed * Math::pow(MAX(u - 3.0f, 0.0f) / 7.0f, 3.0f);
		shed_accum[i] += c.leaves * (c.shed_daily >= 0.0f ? c.shed_daily : shed_rate) / 86400.0f * time_scale * gust * p_delta;
		while (shed_accum[i] >= 1.0f) {
			shed_accum[i] -= 1.0f;
			Vector3 d;
			do {
				d = Vector3(rng.randf() * 2.0f - 1.0f, rng.randf() * 2.0f - 1.0f, rng.randf() * 2.0f - 1.0f);
			} while (d.length_squared() > 1.0f);
			const Vector3 p = c.center + d * c.radii;
			_spawn(p, wm ? wm->get_wind_at(p) * 0.5f : Vector3(), c.kind, false);
		}
	}
	// Loose litter round the camera taken up by the wind.
	if (litter_density > 0.0f && wm && !kinds.is_empty()) {
		const float ga = _ground_at(cam);
		const Vector3 a = wm->get_wind_at(Vector3(cam.x, ga, cam.z), 0.1f);
		const float u = Vector2(a.x, a.z).length();
		if (u > lift_speed) {
			const float area = float(Math::PI) * range * range * 0.25f;
			litter_accum += litter_density * area * 0.002f * (u / lift_speed - 1.0f) * p_delta;
			while (litter_accum >= 1.0f) {
				litter_accum -= 1.0f;
				const float r = range * 0.5f * Math::sqrt(rng.randf());
				const float t = rng.randf() * float(Math::TAU);
				Vector3 p(cam.x + r * Math::cos(t), 0.0f, cam.z + r * Math::sin(t));
				p.y = _ground_at(p) + 0.02f;
				_spawn(p, a * 0.4f + Vector3(0, 0.5f + 0.5f * rng.randf(), 0), int(rng.rand() % kinds.size()), false);
				lifted_count++;
			}
		}
	}

	for (uint32_t i = 0; i < leaves.size(); i++) {
		Leaf &l = leaves[i];
		if (!l.alive) {
			continue;
		}
		_step_leaf(l, p_delta, wm);
		const bool far = l.p.distance_to(cam) > range * 1.5f;
		const bool old = l.lying >= 0.0f && l.lying > lie_time;
		if (far || old || l.age > 600.0f) {
			l.alive = false;
			free_slots.push_back(i);
			alive_count--;
		}
	}
	step_ms = (OS::get_singleton()->get_ticks_usec() - t0) / 1000.0f;
}

void LeafFall3D::_rebuild_draws() {
	for (MultiMeshInstance3D *d : draws) {
		if (d) {
			d->queue_free();
		}
	}
	draws.clear();
	multimeshes.clear();
	for (uint32_t k = 0; k < kinds.size(); k++) {
		Ref<MultiMesh> mm;
		mm.instantiate();
		mm->set_transform_format(MultiMesh::TRANSFORM_3D);
		mm->set_mesh(kinds[k].mesh);
		mm->set_instance_count(max_leaves);
		mm->set_visible_instance_count(0);
		MultiMeshInstance3D *mi = memnew(MultiMeshInstance3D);
		mi->set_multimesh(mm);
		mi->set_as_top_level(true);
		add_child(mi, false, INTERNAL_MODE_FRONT);
		mi->set_global_transform(Transform3D());
		// The leaves cover the whole range round the camera.
		mi->set_custom_aabb(AABB(Vector3(-1e5, -1e4, -1e5), Vector3(2e5, 2e4, 2e5)));
		draws.push_back(mi);
		multimeshes.push_back(mm);
	}
}

void LeafFall3D::_upload() {
	if (multimeshes.size() != kinds.size()) {
		_rebuild_draws();
	}
	for (uint32_t k = 0; k < kinds.size(); k++) {
		Vector<float> buf;
		buf.resize(max_leaves * 12);
		float *w = buf.ptrw();
		int count = 0;
		for (const Leaf &l : leaves) {
			if (!l.alive || l.kind != k || count >= max_leaves) {
				continue;
			}
			const Basis b = l.b.scaled(Vector3(l.scale, l.scale, l.scale));
			float *o = w + count * 12;
			o[0] = b.rows[0][0];
			o[1] = b.rows[0][1];
			o[2] = b.rows[0][2];
			o[3] = l.p.x;
			o[4] = b.rows[1][0];
			o[5] = b.rows[1][1];
			o[6] = b.rows[1][2];
			o[7] = l.p.y;
			o[8] = b.rows[2][0];
			o[9] = b.rows[2][1];
			o[10] = b.rows[2][2];
			o[11] = l.p.z;
			count++;
		}
		RenderingServer::get_singleton()->multimesh_set_buffer(multimeshes[k]->get_rid(), buf);
		multimeshes[k]->set_visible_instance_count(count);
	}
}

// Crowns from the scene's vegetation (VegetationField3D.get_crowns_near) and their shedding from the trees'
// phenology (TreeGenerator.get_leaf_season): each day a crown sheds the rise in its species' fallen_fraction
// from today to tomorrow, so the leaf fall follows the same autumn the crowns show.
void LeafFall3D::_refresh_auto_crowns(const WeatherMap3D *p_weather, const Vector3 &p_camera) {
	// Seasons for today and tomorrow.
	if (p_weather) {
		const int day = p_weather->get_day_of_year();
		if (day != auto_day) {
			auto_day = day;
			auto_season = p_weather->get_season_on_day(day);
			auto_season_next = p_weather->get_season_on_day(day % 365 + 1);
			preset_shed.clear();
		}
	} else if (auto_day != 0) {
		auto_day = 0;
		auto_season = Dictionary();
		auto_season_next = Dictionary();
		preset_shed.clear();
	}
	// The fields.
	LocalVector<VegetationField3D *> fields;
	if (!vegetation_field.is_empty()) {
		VegetationField3D *f = Object::cast_to<VegetationField3D>(get_node_or_null(vegetation_field));
		if (f) {
			fields.push_back(f);
		}
	} else if (is_inside_tree()) {
		const TypedArray<Node> found = get_tree()->get_root()->find_children("*", "VegetationField3D", true, false);
		for (int i = 0; i < found.size(); i++) {
			VegetationField3D *f = Object::cast_to<VegetationField3D>(found[i].operator Object *());
			if (f) {
				fields.push_back(f);
			}
		}
	}
	// Drop the old automatic crowns, keep the ones added by hand.
	LocalVector<Crown> kept;
	for (const Crown &c : crowns) {
		if (!c.automatic) {
			kept.push_back(c);
		}
	}
	crowns = kept;
	for (VegetationField3D *f : fields) {
		const Array near = f->get_crowns_near(p_camera, range);
		for (int i = 0; i < near.size(); i++) {
			const Dictionary d = near[i];
			const int preset = d.get("preset", -1);
			if (preset < 0 || preset >= TreeGenerator::PRESET_MAX) {
				continue;
			}
			// Leaf kinds, made again a week on so the colours follow the autumn.
			int kind = -1;
			const int *k = preset_kind.getptr(preset);
			const int *kd = preset_kind_day.getptr(preset);
			if (k && kd && Math::abs(*kd - auto_day) < 7) {
				kind = *k;
			} else if (kinds.size() < 512) {
				const float fallen = TreeGenerator::get_leaf_season(TreeGenerator::Preset(preset), auto_season).get("fallen_fraction", 0.0f);
				kind = add_tree_kinds(preset, auto_season, 4, fallen > 0.02f);
				preset_kind[preset] = kind;
				preset_kind_day[preset] = auto_day;
			} else if (k) {
				kind = *k;
			}
			if (kind < 0) {
				continue;
			}
			// The day's share: the rise in fallen_fraction; storms still tear a few green leaves off in summer.
			float *sh = preset_shed.getptr(preset);
			if (!sh) {
				const Dictionary a = TreeGenerator::get_leaf_season(TreeGenerator::Preset(preset), auto_season);
				const Dictionary b = TreeGenerator::get_leaf_season(TreeGenerator::Preset(preset), auto_season_next);
				const float fa = a.get("fallen_fraction", 0.0f), fb = b.get("fallen_fraction", 0.0f);
				const float out = a.get("leaf_out", 1.0f);
				float share = 0.0f;
				if (fa < 1.0f && out > 0.0f) {
					// A share of the leaves still on the crown.
					share = MAX((fb - fa) / MAX(1.0f - fa, 0.05f), shed_rate * 0.05f) * out;
				}
				sh = &preset_shed.insert(preset, share)->value;
			}
			const Vector3 pos = d.get("position", Vector3());
			const float h = d.get("height", 10.0f);
			const float base = d.get("crown_base", h * 0.35f);
			const float r = MAX(float(d.get("crown_radius", 2.0f)), 0.2f);
			Crown c;
			c.center = pos + Vector3(0, (base + h) * 0.5f, 0);
			c.radii = Vector3(r, MAX((h - base) * 0.5f, 0.3f), r);
			c.kind = kind;
			const float fa = TreeGenerator::get_leaf_season(TreeGenerator::Preset(preset), auto_season).get("fallen_fraction", 0.0f);
			c.leaves = leaf_area_index * Math::PI * r * r / MAX(kinds[kind].area, 1e-5f) * (1.0f - fa);
			c.shed_daily = *sh;
			c.automatic = true;
			crowns.push_back(c);
		}
	}
	shed_accum.clear(); // resized with fresh random phases in _step
}

float LeafFall3D::get_preset_shed_daily(int p_preset) const {
	const float *sh = preset_shed.getptr(p_preset);
	return sh ? *sh : -1.0f;
}

void LeafFall3D::refresh_crowns_near(const Vector3 &p_position) {
	const WeatherMap3D *wm = weather_map.is_empty() ? nullptr : Object::cast_to<WeatherMap3D>(get_node_or_null(weather_map));
	_refresh_auto_crowns(wm ? wm : WeatherMap3D::get_active(), p_position);
	auto_timer = 2.0f;
}

void LeafFall3D::set_auto_crowns(bool p_v) {
	auto_crowns = p_v;
	auto_timer = 0.0f;
	if (!p_v) {
		LocalVector<Crown> kept;
		for (const Crown &c : crowns) {
			if (!c.automatic) {
				kept.push_back(c);
			}
		}
		crowns = kept;
		shed_accum.clear();
	}
}

void LeafFall3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			if (!Engine::get_singleton()->is_editor_hint()) {
				rng.seed(uint64_t(get_instance_id()));
				set_physics_process_internal(true);
			}
		} break;
		case NOTIFICATION_INTERNAL_PHYSICS_PROCESS: {
			if (!enabled) {
				return;
			}
			_step(float(get_physics_process_delta_time()));
			_upload();
		} break;
	}
}

void LeafFall3D::set_max_leaves(int p_v) {
	max_leaves = CLAMP(p_v, 1, 65536);
	leaves.clear();
	free_slots.clear();
	alive_count = 0;
	multimeshes.clear();
	for (MultiMeshInstance3D *d : draws) {
		if (d) {
			d->queue_free();
		}
	}
	draws.clear();
}

int LeafFall3D::add_kind(const Ref<Mesh> &p_mesh, float p_length, float p_area, float p_mass, float p_flutter) {
	Kind k;
	k.mesh = p_mesh;
	k.length = MAX(p_length, 0.001f);
	k.area = MAX(p_area, 1e-6f);
	k.mass = MAX(p_mass, 1e-7f);
	k.flutter = CLAMP(p_flutter, 0.0f, 1.0f);
	kinds.push_back(k);
	multimeshes.clear();
	return kinds.size() - 1;
}

int LeafFall3D::add_tree_kinds(int p_preset, const Dictionary &p_season, int p_variants, bool p_dry) {
	const TreeGenerator::Preset preset = TreeGenerator::Preset(p_preset);
	const Dictionary ph = TreeGenerator::get_leaf_physics(preset);
	const float length = ph.get("length", 0.08f);
	const float area = ph.get("area", 0.003f);
	const float mass = p_dry ? float(ph.get("mass_dry", 0.0002f)) : float(ph.get("mass_fresh", 0.0005f));
	const float flutter = ph.get("flutter", 1.0f);
	const int first = kinds.size();
	const int n = CLAMP(p_variants, 1, 16);
	for (int v = 0; v < n; v++) {
		add_kind(TreeGenerator::make_leaf_mesh(preset, p_season, v), length, area, mass, flutter);
	}
	for (int i = first; i < first + n; i++) {
		kinds[i].group_first = first;
		kinds[i].group_count = n;
	}
	return first;
}

void LeafFall3D::clear_kinds() {
	kinds.clear();
	leaves.clear();
	free_slots.clear();
	alive_count = 0;
	_rebuild_draws();
}

int LeafFall3D::add_crown(const Vector3 &p_center, const Vector3 &p_radii, int p_kind, float p_leaves) {
	Crown c;
	c.center = p_center;
	c.radii = p_radii.abs();
	c.kind = MAX(p_kind, 0);
	c.leaves = MAX(p_leaves, 0.0f);
	crowns.push_back(c);
	return crowns.size() - 1;
}

void LeafFall3D::clear_crowns() {
	crowns.clear();
	shed_accum.clear();
}

PackedVector4Array LeafFall3D::get_leaf_states() const {
	PackedVector4Array out;
	for (const Leaf &l : leaves) {
		if (l.alive) {
			out.push_back(Vector4(l.p.x, l.p.y, l.p.z, l.lying >= 0.0f ? 1.0f : 0.0f));
		}
	}
	return out;
}

PackedVector3Array LeafFall3D::get_leaf_velocities() const {
	PackedVector3Array out;
	for (const Leaf &l : leaves) {
		if (l.alive) {
			out.push_back(l.v);
		}
	}
	return out;
}

PackedVector3Array LeafFall3D::get_leaf_normals() const {
	PackedVector3Array out;
	for (const Leaf &l : leaves) {
		if (l.alive) {
			out.push_back(l.b.get_column(1));
		}
	}
	return out;
}

void LeafFall3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_enabled", "enabled"), &LeafFall3D::set_enabled);
	ClassDB::bind_method(D_METHOD("is_enabled"), &LeafFall3D::is_enabled);
	ClassDB::bind_method(D_METHOD("set_max_leaves", "count"), &LeafFall3D::set_max_leaves);
	ClassDB::bind_method(D_METHOD("get_max_leaves"), &LeafFall3D::get_max_leaves);
	ClassDB::bind_method(D_METHOD("set_range", "range"), &LeafFall3D::set_range);
	ClassDB::bind_method(D_METHOD("get_range"), &LeafFall3D::get_range);
	ClassDB::bind_method(D_METHOD("set_auto_crowns", "enabled"), &LeafFall3D::set_auto_crowns);
	ClassDB::bind_method(D_METHOD("is_auto_crowns"), &LeafFall3D::is_auto_crowns);
	ClassDB::bind_method(D_METHOD("set_vegetation_field", "path"), &LeafFall3D::set_vegetation_field);
	ClassDB::bind_method(D_METHOD("get_vegetation_field"), &LeafFall3D::get_vegetation_field);
	ClassDB::bind_method(D_METHOD("set_leaf_area_index", "lai"), &LeafFall3D::set_leaf_area_index);
	ClassDB::bind_method(D_METHOD("get_leaf_area_index"), &LeafFall3D::get_leaf_area_index);
	ClassDB::bind_method(D_METHOD("get_preset_shed_daily", "preset"), &LeafFall3D::get_preset_shed_daily);
	ClassDB::bind_method(D_METHOD("refresh_crowns_near", "position"), &LeafFall3D::refresh_crowns_near);
	ClassDB::bind_method(D_METHOD("set_shed_rate", "rate"), &LeafFall3D::set_shed_rate);
	ClassDB::bind_method(D_METHOD("get_shed_rate"), &LeafFall3D::get_shed_rate);
	ClassDB::bind_method(D_METHOD("set_wind_shed", "factor"), &LeafFall3D::set_wind_shed);
	ClassDB::bind_method(D_METHOD("get_wind_shed"), &LeafFall3D::get_wind_shed);
	ClassDB::bind_method(D_METHOD("set_lift_speed", "speed"), &LeafFall3D::set_lift_speed);
	ClassDB::bind_method(D_METHOD("get_lift_speed"), &LeafFall3D::get_lift_speed);
	ClassDB::bind_method(D_METHOD("set_litter_density", "density"), &LeafFall3D::set_litter_density);
	ClassDB::bind_method(D_METHOD("get_litter_density"), &LeafFall3D::get_litter_density);
	ClassDB::bind_method(D_METHOD("set_lie_time", "seconds"), &LeafFall3D::set_lie_time);
	ClassDB::bind_method(D_METHOD("get_lie_time"), &LeafFall3D::get_lie_time);
	ClassDB::bind_method(D_METHOD("set_weather_map", "path"), &LeafFall3D::set_weather_map);
	ClassDB::bind_method(D_METHOD("get_weather_map"), &LeafFall3D::get_weather_map);
	ClassDB::bind_method(D_METHOD("add_kind", "mesh", "length", "area", "mass", "flutter"), &LeafFall3D::add_kind);
	ClassDB::bind_method(D_METHOD("add_tree_kinds", "preset", "season", "variants", "dry"), &LeafFall3D::add_tree_kinds, DEFVAL(4), DEFVAL(true));
	ClassDB::bind_method(D_METHOD("clear_kinds"), &LeafFall3D::clear_kinds);
	ClassDB::bind_method(D_METHOD("get_kind_count"), &LeafFall3D::get_kind_count);
	ClassDB::bind_method(D_METHOD("add_crown", "center", "radii", "kind", "leaves"), &LeafFall3D::add_crown);
	ClassDB::bind_method(D_METHOD("clear_crowns"), &LeafFall3D::clear_crowns);
	ClassDB::bind_method(D_METHOD("get_crown_count"), &LeafFall3D::get_crown_count);
	ClassDB::bind_method(D_METHOD("drop_leaf", "position", "kind"), &LeafFall3D::drop_leaf);
	ClassDB::bind_method(D_METHOD("get_leaf_count"), &LeafFall3D::get_leaf_count);
	ClassDB::bind_method(D_METHOD("get_lifted_count"), &LeafFall3D::get_lifted_count);
	ClassDB::bind_method(D_METHOD("get_step_time"), &LeafFall3D::get_step_time);
	ClassDB::bind_method(D_METHOD("get_leaf_states"), &LeafFall3D::get_leaf_states);
	ClassDB::bind_method(D_METHOD("get_leaf_velocities"), &LeafFall3D::get_leaf_velocities);
	ClassDB::bind_method(D_METHOD("get_leaf_normals"), &LeafFall3D::get_leaf_normals);
	ClassDB::bind_static_method("LeafFall3D", D_METHOD("simulate_plate", "length", "area", "mass", "flutter", "wind", "tilt_degrees", "seconds", "air_density"), &LeafFall3D::simulate_plate, DEFVAL(1.2f));

	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enabled"), "set_enabled", "is_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_leaves", PROPERTY_HINT_RANGE, "1,65536,1"), "set_max_leaves", "get_max_leaves");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "range", PROPERTY_HINT_RANGE, "1,500,0.1,suffix:m"), "set_range", "get_range");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "auto_crowns"), "set_auto_crowns", "is_auto_crowns");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "vegetation_field", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "VegetationField3D"), "set_vegetation_field", "get_vegetation_field");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "leaf_area_index", PROPERTY_HINT_RANGE, "0,12,0.1"), "set_leaf_area_index", "get_leaf_area_index");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "shed_rate", PROPERTY_HINT_RANGE, "0,1,0.0001"), "set_shed_rate", "get_shed_rate");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wind_shed", PROPERTY_HINT_RANGE, "0,200,0.1"), "set_wind_shed", "get_wind_shed");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lift_speed", PROPERTY_HINT_RANGE, "0.1,20,0.1,suffix:m/s"), "set_lift_speed", "get_lift_speed");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "litter_density", PROPERTY_HINT_RANGE, "0,500,0.1"), "set_litter_density", "get_litter_density");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lie_time", PROPERTY_HINT_RANGE, "0,3600,1,suffix:s"), "set_lie_time", "get_lie_time");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "weather_map", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "WeatherMap3D"), "set_weather_map", "get_weather_map");
}

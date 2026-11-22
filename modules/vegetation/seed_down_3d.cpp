/**************************************************************************/
/*  seed_down_3d.cpp                                                        */
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

#include "seed_down_3d.h"

#include "plant_generator.h"
#include "plant_species.h"
#include "vegetation_field_3d.h"

#include "core/config/engine.h"
#include "core/object/class_db.h"
#include "modules/world/world_contracts.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/multimesh_instance_3d.h"
#include "scene/main/viewport.h"
#include "scene/main/window.h"
#include "scene/resources/material.h"
#include "servers/rendering/rendering_server.h"

// Falling speeds in still air, dry, pappus open. The dandelion's is measured: 0.391 m/s for a 0.63 mg diaspore
// with a 13.8 mm pappus of about 100 bristles (Cummins et al. 2018, Nature 562, drop tests). Thistledown and
// willowherb fluff are scaled from it by their larger, lighter-loaded plumes: estimates, no measured value found.
float SeedDown3D::_fall_speed(int p_kind) {
	switch (p_kind) {
		case KIND_THISTLE:
			return 0.3f;
		case KIND_WILLOWHERB:
			return 0.25f;
		default:
			return 0.391f;
	}
}

int SeedDown3D::_kind_of(int p_preset) {
	switch (p_preset) {
		case PlantGenerator::PRESET_DANDELION:
			return KIND_DANDELION;
		case PlantGenerator::PRESET_THISTLE:
			return KIND_THISTLE;
		case PlantGenerator::PRESET_FIREWEED:
			return KIND_WILLOWHERB;
		default:
			return -1;
	}
}

SeedDown3D::SeedDown3D() {
	seeds.resize(max_seeds);
}

Vector3 SeedDown3D::_camera_position() const {
	if (is_inside_tree() && get_viewport() && get_viewport()->get_camera_3d()) {
		return get_viewport()->get_camera_3d()->get_global_position();
	}
	return is_inside_tree() ? get_global_position() : Vector3();
}

float SeedDown3D::_ground_at(const Vector3 &p_p) const {
	const WorldWeather *wm = WorldContracts::weather();
	if (wm->has_provider()) {
		return wm->ground_height_at(p_p);
	}
	return is_inside_tree() ? float(get_global_position().y) : 0.0f;
}

// The heads near the camera that can be in seed: the scene's fields' species of the plumed presets (meta
// plant_preset), each placement a head at its plant's top. Read again every 2 s.
void SeedDown3D::_refresh_sources(const Vector3 &p_camera) {
	sources.clear();
	if (!is_inside_tree()) {
		return;
	}
	const TypedArray<Node> found = get_tree()->get_root()->find_children("*", "VegetationField3D", true, false);
	const float r2 = range * range;
	for (int f = 0; f < found.size(); f++) {
		VegetationField3D *vf = Object::cast_to<VegetationField3D>(found[f].operator Object *());
		if (!vf || vf->get_biome().is_null()) {
			continue;
		}
		const TypedArray<PlantSpecies> species = vf->get_biome()->get_species();
		for (int i = 0; i < species.size(); i++) {
			const Ref<PlantSpecies> sp = species[i];
			if (sp.is_null() || !sp->has_meta("plant_preset")) {
				continue;
			}
			const int preset = sp->get_meta("plant_preset");
			const int kind = _kind_of(preset);
			if (kind < 0) {
				continue;
			}
			float top = 0.3f;
			const TypedArray<Mesh> m = sp->get_meshes();
			if (m.size() > 0 && Ref<Mesh>(m[0]).is_valid()) {
				top = MAX(float(Ref<Mesh>(m[0])->get_aabb().get_end().y) * 0.9f, 0.05f);
			}
			const PackedVector3Array at = vf->get_placements(i);
			for (int k = 0; k < at.size(); k++) {
				if (at[k].distance_squared_to(p_camera) > r2) {
					continue;
				}
				Source s;
				s.p = at[k] + Vector3(0, top, 0);
				s.kind = uint8_t(kind);
				s.preset = preset;
				sources.push_back(s);
			}
		}
	}
}

// Release: a head in seed loses its seeds to the wind's drag on the open pappus. Dry diaspores come off more
// readily than wet ones (closed pappus: 24% of dry and 50% of wet ones stayed on the head at 9.8 m/s, Seale et al.
// 2022, eLife 11:e81962), and rain stops it. The rate rises from a light breeze to a strong one; release_rate
// (seeds a second a head at full) and the 2-10 m/s ramp are estimates.
void SeedDown3D::_release(const Vector3 &p_camera, float p_delta, const WorldWeather *p_weather) {
	if (sources.is_empty() || release_rate <= 0.0f) {
		return;
	}
	const Dictionary season = p_weather->season_at(p_camera);
	const int day = int(season.get("day_of_year", -1));
	if (day != share_day) {
		share_day = day;
		seed_share.clear();
	}
	// A sample of heads each step; each stands for its share of all of them.
	const int n = int(sources.size());
	const int tries = MIN(n, 48);
	const float per = float(n) / float(tries);
	for (int t = 0; t < tries; t++) {
		const Source &s = sources[rng.rand() % uint32_t(n)];
		float *share = seed_share.getptr(s.preset);
		if (!share) {
			seed_share.insert(s.preset, season.is_empty() ? 0.0f : float(PlantGenerator::get_bloom_shares(PlantGenerator::Preset(s.preset), season).z));
			share = seed_share.getptr(s.preset);
		}
		if (*share <= 0.0f) {
			continue;
		}
		const Vector3 w = p_weather->wind_at(s.p, MAX(float(s.p.y - _ground_at(s.p)), 0.1f));
		const float u = Vector2(w.x, w.z).length();
		const float pull = Math::smoothstep(2.0f, 10.0f, u);
		if (pull <= 0.0f) {
			continue;
		}
		const float wet = CLAMP(p_weather->surface_wetness_at(s.p), 0.0f, 1.0f);
		const float rain = p_weather->rainfall_at(s.p) > 0.2f ? 0.1f : 1.0f;
		const float rate = release_rate * *share * pull * Math::lerp(1.0f, 0.5f, wet) * rain * per;
		float expect = rate * p_delta;
		while (expect > 0.0f) {
			if (rng.randf() < MIN(expect, 1.0f)) {
				release_at(s.p + Vector3(rng.randf() - 0.5f, rng.randf() - 0.5f, rng.randf() - 0.5f) * 0.05f, s.kind, 1);
			}
			expect -= 1.0f;
		}
	}
}

void SeedDown3D::release_at(const Vector3 &p_position, int p_kind, int p_count) {
	if (max_seeds <= 0) {
		return;
	}
	for (int i = 0; i < p_count; i++) {
		Seed &s = seeds[next_slot];
		next_slot = (next_slot + 1) % max_seeds;
		s = Seed();
		s.p = p_position;
		s.kind = uint8_t(CLAMP(p_kind, 0, KIND_MAX - 1));
		s.scale = 0.85f + 0.3f * rng.randf();
		s.spin = rng.randf() * Math::TAU;
		s.alive = true;
		released++;
	}
}

// Aloft the seed moves with the wind at its height and falls at its falling speed; the closing pappus of a damp
// seed falls two to three times faster (Seale et al. 2022). Turbulence near the ground lifts and drops it: a
// vertical gust w' as an Ornstein-Uhlenbeck process (Langevin model of dispersal; Nathan et al. 2002, Nature 418)
// with sigma_w about 1.25 u* and u* about a tenth of the local wind (neutral surface layer), its Lagrangian time
// about 0.5 z / sigma_w (Wilson and Sawford 1996), so the gusts near the grass are short. No drift correction for
// the turbulence's change with height (Thomson 1987): a seed near the ground lingers a little too long.
void SeedDown3D::_step_seed(Seed &p_seed, float p_delta, const WorldWeather *p_weather) {
	if (p_seed.lying >= 0.0f) {
		p_seed.lying += p_delta;
		if (p_seed.lying > lie_time) {
			p_seed.alive = false;
			return;
		}
		if (p_seed.on_water) {
			const Vector3 flow = WorldContracts::water()->flow_at(p_seed.p);
			p_seed.p += Vector3(flow.x, 0.0f, flow.z) * p_delta;
			return;
		}
		// Lifted out of the grass again by a gust past the lift speed.
		const Vector3 w = p_weather->wind_at(p_seed.p, 0.1f);
		const float u = Vector2(w.x, w.z).length();
		if (u > lift_speed && rng.randf() < p_delta * (u - lift_speed) * 0.5f) {
			p_seed.lying = -1.0f;
			p_seed.w_turb = 0.3f * u;
		}
		return;
	}
	const float ground = _ground_at(p_seed.p);
	const float h = MAX(float(p_seed.p.y) - ground, 0.05f);
	const Vector3 wind = p_weather->wind_at(p_seed.p, h);
	const float u = Vector2(wind.x, wind.z).length();
	const float wet = CLAMP(p_weather->surface_wetness_at(p_seed.p), 0.0f, 1.0f);
	const float fall = _fall_speed(p_seed.kind) * (1.0f + 1.5f * wet);
	const float sigma = 0.125f * u;
	const float tl = CLAMP(0.5f * h / MAX(sigma, 0.05f), 0.05f, 2.0f); // Eddies grow with height: T_L about 0.5 z / sigma_w.
	p_seed.w_turb += -p_seed.w_turb * p_delta / tl + sigma * Math::sqrt(2.0f * p_delta / tl) * float(rng.randfn(0.0f, 1.0f));
	// Its inertia is small: it takes the air's speed within a few hundredths of a second.
	const Vector3 air(wind.x, wind.y + p_seed.w_turb - fall, wind.z);
	p_seed.v = p_seed.v.lerp(air, MIN(1.0f, p_delta / 0.04f));
	p_seed.p += p_seed.v * p_delta;
	p_seed.spin += p_delta * (0.5f + u * 0.3f);
	// Landing: on open water it floats; on land it is caught in the grass or lies on bare ground.
	float top = ground;
	const WorldWater *water = WorldContracts::water();
	bool on_water = false;
	if (water->has_provider()) {
		const float wh = water->height_at(Vector3(p_seed.p.x, ground, p_seed.p.z));
		if (Math::is_finite(wh) && wh > ground + 0.005f) {
			top = wh;
			on_water = true;
		}
	}
	if (p_seed.p.y <= top) {
		p_seed.p.y = top + (on_water ? 0.002f : 0.004f);
		p_seed.lying = 0.0f;
		p_seed.on_water = on_water;
		p_seed.v = Vector3();
		p_seed.w_turb = 0.0f;
	}
}

void SeedDown3D::advance(float p_seconds, bool p_release) {
	const WorldWeather *wm = WorldContracts::weather();
	const Vector3 cam = _camera_position();
	if (p_release && sources.is_empty()) {
		_refresh_sources(cam);
	}
	const float dt = 1.0f / 60.0f;
	for (float t = 0.0f; t < p_seconds; t += dt) {
		if (p_release) {
			_release(cam, dt, wm);
		}
		for (Seed &s : seeds) {
			if (s.alive) {
				_step_seed(s, dt, wm);
			}
		}
	}
}

void SeedDown3D::refresh_sources() {
	_refresh_sources(_camera_position());
}

int SeedDown3D::get_aloft_count() const {
	int n = 0;
	for (const Seed &s : seeds) {
		n += (s.alive && s.lying < 0.0f) ? 1 : 0;
	}
	return n;
}

int SeedDown3D::get_lying_count() const {
	int n = 0;
	for (const Seed &s : seeds) {
		n += (s.alive && s.lying >= 0.0f) ? 1 : 0;
	}
	return n;
}

int SeedDown3D::get_floating_count() const {
	int n = 0;
	for (const Seed &s : seeds) {
		n += (s.alive && s.lying >= 0.0f && s.on_water) ? 1 : 0;
	}
	return n;
}

PackedVector4Array SeedDown3D::get_seed_states() const {
	PackedVector4Array out;
	for (const Seed &s : seeds) {
		if (s.alive) {
			out.push_back(Vector4(s.p.x, s.p.y, s.p.z, s.lying));
		}
	}
	return out;
}

void SeedDown3D::clear_seeds() {
	for (Seed &s : seeds) {
		s.alive = false;
	}
	released = 0;
}

void SeedDown3D::set_max_seeds(int p_v) {
	max_seeds = CLAMP(p_v, 0, 20000);
	seeds.resize(max_seeds);
	for (Seed &s : seeds) {
		s.alive = false;
	}
	next_slot = 0;
	for (int k = 0; k < KIND_MAX; k++) {
		if (multimeshes[k].is_valid()) {
			multimeshes[k]->set_instance_count(max_seeds);
			multimeshes[k]->set_visible_instance_count(0);
		}
	}
}

// One diaspore as geometry (metres): the achene, the beak (dandelion) and the pappus bristles as thin two-sided
// strips, so the path tracer sees the same seeds. Colours by vertex: brown seed, white silk.
Ref<Mesh> SeedDown3D::_make_mesh(int p_kind) const {
	PackedVector3Array verts;
	PackedVector3Array normals;
	PackedColorArray colors;
	PackedInt32Array idx;
	auto quad = [&](const Vector3 &a, const Vector3 &b, const Vector3 &side, const Color &c) {
		const int i0 = verts.size();
		const Vector3 n = (b - a).cross(side).normalized();
		for (const Vector3 &v : { a - side, a + side, b + side, b - side }) {
			verts.push_back(v);
			normals.push_back(n);
			colors.push_back(c);
		}
		idx.append_array(PackedInt32Array({ i0, i0 + 1, i0 + 2, i0, i0 + 2, i0 + 3 }));
	};
	const Color brown(0.32f, 0.24f, 0.15f);
	const Color silk(0.93f, 0.93f, 0.9f);
	float radius = 0.007f, beak = 0.009f, seed_len = 0.003f, rise = 0.12f;
	int bristles = 24;
	if (p_kind == KIND_THISTLE) {
		radius = 0.014f;
		beak = 0.0f;
		seed_len = 0.004f;
		rise = 0.35f;
		bristles = 32;
	} else if (p_kind == KIND_WILLOWHERB) {
		radius = 0.009f;
		beak = 0.0f;
		seed_len = 0.0015f;
		rise = 0.8f;
		bristles = 20;
	}
	// The achene (two crossed strips), then the beak, then the bristles from its tip.
	const Vector3 seed_top(0, seed_len, 0);
	quad(Vector3(), seed_top, Vector3(0.0005f, 0, 0), brown);
	quad(Vector3(), seed_top, Vector3(0, 0, 0.0005f), brown);
	const Vector3 hub = seed_top + Vector3(0, beak, 0);
	if (beak > 0.0f) {
		quad(seed_top, hub, Vector3(0.00012f, 0, 0), Color(0.75f, 0.72f, 0.62f));
	}
	for (int i = 0; i < bristles; i++) {
		const float a = (i + 0.5f) * Math::TAU / bristles;
		const Vector3 out(Math::cos(a), rise, Math::sin(a));
		const Vector3 tip = hub + out.normalized() * radius;
		const Vector3 side = Vector3(-Math::sin(a), 0, Math::cos(a)) * 0.00015f;
		quad(hub, tip, side, silk);
	}
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = verts;
	arrays[Mesh::ARRAY_NORMAL] = normals;
	arrays[Mesh::ARRAY_COLOR] = colors;
	arrays[Mesh::ARRAY_INDEX] = idx;
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	Ref<StandardMaterial3D> mat;
	mat.instantiate();
	mat->set_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
	mat->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
	mat->set_roughness(0.75f);
	mat->set_feature(BaseMaterial3D::FEATURE_BACKLIGHT, true);
	mat->set_backlight(Color(0.55f, 0.55f, 0.52f)); // Thin silk glows against the sun.
	mesh->surface_set_material(0, mat);
	return mesh;
}

void SeedDown3D::_ensure_draws() {
	for (int k = 0; k < KIND_MAX; k++) {
		if (draws[k]) {
			continue;
		}
		meshes[k] = _make_mesh(k);
		Ref<MultiMesh> mm;
		mm.instantiate();
		mm->set_transform_format(MultiMesh::TRANSFORM_3D);
		mm->set_mesh(meshes[k]);
		mm->set_instance_count(max_seeds);
		mm->set_visible_instance_count(0);
		MultiMeshInstance3D *mi = memnew(MultiMeshInstance3D);
		mi->set_multimesh(mm);
		mi->set_as_top_level(true);
		add_child(mi, false, INTERNAL_MODE_FRONT);
		mi->set_global_transform(Transform3D());
		mi->set_custom_aabb(AABB(Vector3(-1e5, -1e4, -1e5), Vector3(2e5, 2e4, 2e5)));
		draws[k] = mi;
		multimeshes[k] = mm;
	}
}

void SeedDown3D::_upload() {
	_ensure_draws();
	for (int k = 0; k < KIND_MAX; k++) {
		Vector<float> buf;
		buf.resize(MAX(max_seeds, 1) * 12);
		float *w = buf.ptrw();
		int count = 0;
		for (const Seed &s : seeds) {
			if (!s.alive || s.kind != k || count >= max_seeds) {
				continue;
			}
			// Pappus up while aloft (the separated vortex ring keeps it so); tipped over lying in the grass.
			Basis b(Vector3(0, 1, 0), s.spin);
			if (s.lying >= 0.0f && !s.on_water) {
				b = Basis(Vector3(Math::cos(s.spin), 0, Math::sin(s.spin)), 1.2f) * b;
			}
			// Old seeds lying long fade by shrinking away in their last seconds (taken by the grass).
			const float fade = s.lying >= 0.0f ? CLAMP((lie_time - s.lying) / 5.0f, 0.0f, 1.0f) : 1.0f;
			b.scale(Vector3(1, 1, 1) * s.scale * fade);
			float *o = w + count * 12;
			o[0] = b.rows[0][0];
			o[1] = b.rows[0][1];
			o[2] = b.rows[0][2];
			o[3] = s.p.x;
			o[4] = b.rows[1][0];
			o[5] = b.rows[1][1];
			o[6] = b.rows[1][2];
			o[7] = s.p.y;
			o[8] = b.rows[2][0];
			o[9] = b.rows[2][1];
			o[10] = b.rows[2][2];
			o[11] = s.p.z;
			count++;
		}
		RenderingServer::get_singleton()->multimesh_set_buffer(multimeshes[k]->get_rid(), buf);
		multimeshes[k]->set_visible_instance_count(count);
	}
}

void SeedDown3D::_notification(int p_what) {
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
			if (still_shot) {
				_upload();
				return;
			}
			const float dt = float(get_physics_process_delta_time());
			const Vector3 cam = _camera_position();
			source_timer -= dt;
			if (source_timer <= 0.0f) {
				source_timer = 2.0f;
				_refresh_sources(cam);
			}
			const WorldWeather *wm = WorldContracts::weather();
			_release(cam, dt, wm);
			const float r2 = (range * 1.5f) * (range * 1.5f);
			for (Seed &s : seeds) {
				if (!s.alive) {
					continue;
				}
				_step_seed(s, dt, wm);
				if (s.p.distance_squared_to(cam) > r2) {
					s.alive = false; // Gone out of view: its slot for another.
				}
			}
			_upload();
		} break;
	}
}

void SeedDown3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_enabled", "enabled"), &SeedDown3D::set_enabled);
	ClassDB::bind_method(D_METHOD("is_enabled"), &SeedDown3D::is_enabled);
	ClassDB::bind_method(D_METHOD("set_still_shot", "enabled"), &SeedDown3D::set_still_shot);
	ClassDB::bind_method(D_METHOD("is_still_shot"), &SeedDown3D::is_still_shot);
	ClassDB::bind_method(D_METHOD("set_max_seeds", "count"), &SeedDown3D::set_max_seeds);
	ClassDB::bind_method(D_METHOD("get_max_seeds"), &SeedDown3D::get_max_seeds);
	ClassDB::bind_method(D_METHOD("set_range", "metres"), &SeedDown3D::set_range);
	ClassDB::bind_method(D_METHOD("get_range"), &SeedDown3D::get_range);
	ClassDB::bind_method(D_METHOD("set_release_rate", "per_second"), &SeedDown3D::set_release_rate);
	ClassDB::bind_method(D_METHOD("get_release_rate"), &SeedDown3D::get_release_rate);
	ClassDB::bind_method(D_METHOD("set_lie_time", "seconds"), &SeedDown3D::set_lie_time);
	ClassDB::bind_method(D_METHOD("get_lie_time"), &SeedDown3D::get_lie_time);
	ClassDB::bind_method(D_METHOD("set_lift_speed", "speed"), &SeedDown3D::set_lift_speed);
	ClassDB::bind_method(D_METHOD("get_lift_speed"), &SeedDown3D::get_lift_speed);
	ClassDB::bind_method(D_METHOD("release_at", "position", "kind", "count"), &SeedDown3D::release_at);
	ClassDB::bind_method(D_METHOD("advance", "seconds", "release"), &SeedDown3D::advance, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("refresh_sources"), &SeedDown3D::refresh_sources);
	ClassDB::bind_method(D_METHOD("get_source_count"), &SeedDown3D::get_source_count);
	ClassDB::bind_method(D_METHOD("get_released_count"), &SeedDown3D::get_released_count);
	ClassDB::bind_method(D_METHOD("get_aloft_count"), &SeedDown3D::get_aloft_count);
	ClassDB::bind_method(D_METHOD("get_lying_count"), &SeedDown3D::get_lying_count);
	ClassDB::bind_method(D_METHOD("get_floating_count"), &SeedDown3D::get_floating_count);
	ClassDB::bind_method(D_METHOD("get_seed_states"), &SeedDown3D::get_seed_states);
	ClassDB::bind_method(D_METHOD("clear_seeds"), &SeedDown3D::clear_seeds);
	ClassDB::bind_static_method("SeedDown3D", D_METHOD("get_fall_speed", "kind"), &SeedDown3D::get_fall_speed);
	ClassDB::bind_static_method("SeedDown3D", D_METHOD("get_kind_for_preset", "preset"), &SeedDown3D::get_kind_for_preset);

	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enabled"), "set_enabled", "is_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "still_shot"), "set_still_shot", "is_still_shot");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_seeds", PROPERTY_HINT_RANGE, "0,20000,1"), "set_max_seeds", "get_max_seeds");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "range", PROPERTY_HINT_RANGE, "1,500,0.5,suffix:m"), "set_range", "get_range");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "release_rate", PROPERTY_HINT_RANGE, "0,5,0.01,suffix:/s"), "set_release_rate", "get_release_rate");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lie_time", PROPERTY_HINT_RANGE, "1,1000,1,suffix:s"), "set_lie_time", "get_lie_time");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lift_speed", PROPERTY_HINT_RANGE, "0.1,20,0.1,suffix:m/s"), "set_lift_speed", "get_lift_speed");

	BIND_ENUM_CONSTANT(KIND_DANDELION);
	BIND_ENUM_CONSTANT(KIND_THISTLE);
	BIND_ENUM_CONSTANT(KIND_WILLOWHERB);
	BIND_ENUM_CONSTANT(KIND_MAX);
}

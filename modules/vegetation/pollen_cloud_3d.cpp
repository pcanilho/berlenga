/**************************************************************************/
/*  pollen_cloud_3d.cpp                                                   */
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


#include "pollen_cloud_3d.h"

#include "grass_field_3d.h"
#include "plant_generator.h"
#include "plant_species.h"
#include "vegetation_field_3d.h"

#include "core/config/engine.h"
#include "core/object/class_db.h"
#include "core/templates/sort_array.h"
#include "modules/terrain_deform/terrain_data_map_3d.h"
#include "modules/world/world_contracts.h"
#include "scene/3d/camera_3d.h"
#include "scene/main/viewport.h"
#include "scene/main/window.h"

float PollenCloud3D::release_factor(float p_humidity, float p_wetness, float p_rain_mmh, float p_wind) {
	// Anthers open as they dry and close again in damp air (Taylor et al. 2002, cited by Waudby et al. 2022,
	// Aerobiologia 38: grass pollen counts fall with relative humidity and rain); grass pollen counts are low in
	// 0-2 m/s of wind and moderate to extreme in about 4-10 m/s (Waudby et al. 2022). Thresholds here: estimates.
	const float dry_air = 1.0f - Math::smoothstep(0.55f, 0.85f, float(CLAMP(p_humidity, 0.0f, 1.0f)));
	const float dry_leaves = 1.0f - Math::smoothstep(0.1f, 0.5f, float(CLAMP(p_wetness, 0.0f, 1.0f)));
	const float no_rain = p_rain_mmh > 0.1f ? 0.0f : 1.0f;
	const float wind = Math::smoothstep(3.0f, 8.0f, p_wind);
	return dry_air * dry_leaves * no_rain * wind;
}

float PollenCloud3D::window_share(int p_day, const Vector2 &p_window) {
	if (p_day < 0) {
		return 0.0f;
	}
	const float d = float(p_day);
	return Math::smoothstep(float(p_window.x) - 7.0f, float(p_window.x), d) * (1.0f - Math::smoothstep(float(p_window.y), float(p_window.y) + 7.0f, d));
}

Vector3 PollenCloud3D::_camera_position() const {
	if (is_inside_tree() && get_viewport() && get_viewport()->get_camera_3d()) {
		return get_viewport()->get_camera_3d()->get_global_position();
	}
	return is_inside_tree() ? get_global_position() : Vector3();
}

void PollenCloud3D::_build() {
	if (tried) {
		return;
	}
	tried = true;
	if (!ClassDB::class_exists("PhysXFlow3D") || !ClassDB::class_exists("PhysXFlowEmitter3D")) {
		return;
	}
	flow = Object::cast_to<Node3D>(ClassDB::instantiate("PhysXFlow3D"));
	if (!flow) {
		return;
	}
	// A faint haze, not smoke: low density, slow to fade (pollen settles at a few cm/s, so the air holds it for
	// minutes; Flow's fade stands in for its spreading thin), a little heavier than air (it settles), curling in the
	// wind's turbulence. Pollen grains absorb more than ice: a yellow single-scattering albedo under 1 (estimate).
	flow->set_name("PollenFlow");
	flow->set("cell_size", cell_size);
	flow->set("max_blocks", max_blocks);
	flow->set("combustion", false);
	flow->set("buoyancy", 0.0f);
	flow->set("smoke_buoyancy", -0.03f);
	flow->set("smoke_fade", 0.05f);
	flow->set("vorticity", 3.0f);
	flow->set("emission_strength", 0.0f);
	flow->set("smoke_density", 0.35f);
	flow->set("scattering_strength", 1.0f); // raster only; PT never sees it (parity)
	flow->set("scattering_albedo", 0.85f);
	flow->set("smoke_color", color);
	flow->set("collide_with_bodies", true);
	flow->set("collision_range", 30.0f);
	flow->set("weather_wind", true);
	flow->set("terrain_floor", true);
	if (!terrain_path.is_empty()) {
		flow->set("terrain_path", terrain_path);
	}
	add_child(flow, false, INTERNAL_MODE_FRONT);
	flow->set_as_top_level(true);
	flow->set_global_transform(Transform3D());
	const int n = max_conifer_sources + max_grass_sources;
	for (int i = 0; i < n; i++) {
		Node3D *e = Object::cast_to<Node3D>(ClassDB::instantiate("PhysXFlowEmitter3D"));
		if (!e) {
			break;
		}
		e->set_name(vformat("Pollen%d", i));
		e->set("enabled", false);
		flow->add_child(e);
		e->set("flow", e->get_path_to(flow));
		emitters.push_back(e);
	}
}

void PollenCloud3D::update_sources() {
	sources.clear();
	conifer_count = 0;
	grass_count = 0;
	release = 0.0f;
	const WorldWeather *wm = WorldContracts::weather();
	if (!enabled || !is_inside_tree() || !wm) {
		_feed();
		return;
	}
	const Vector3 eye = _camera_position();
	const int day = wm->day_of_year();
	const float hum = wm->air_humidity_at(eye);
	const float wet = wm->surface_wetness_at(eye);
	const float rain = wm->rainfall_at(eye);
	const float wind10 = wm->wind_at(eye, 10.0f).length();
	// In flower: the conifers by their window; the sward by its grasses' anthesis for the season here
	// (PlantGenerator.get_pollen_release for cocksfoot and timothy, as VegetationPollen3D's puffs).
	const float cone_share = window_share(day, conifer_window);
	const Dictionary season = wm->season_at(eye);
	const float grass_share = season.is_empty() ? window_share(day, grass_window) : MAX(PlantGenerator::get_pollen_release(PlantGenerator::PRESET_COCKSFOOT, season), PlantGenerator::get_pollen_release(PlantGenerator::PRESET_TIMOTHY, season));
	release = release_factor(hum, wet, rain, wind10) * MAX(cone_share, grass_share);
	if (release <= 0.0f) {
		_feed();
		return;
	}
	struct Cand {
		Source s;
		float d2 = 0.0f;
		bool operator<(const Cand &p_o) const { return d2 < p_o.d2; }
	};
	// Conifer crowns: each sheds by the wind at its crown's middle (the gusts that shake it).
	LocalVector<Cand> cones;
	if (max_conifer_sources > 0 && cone_share > 0.0f) {
		const TypedArray<Node> found = get_tree()->get_root()->find_children("*", "VegetationField3D", true, false);
		for (int f = 0; f < found.size(); f++) {
			const VegetationField3D *vf = Object::cast_to<VegetationField3D>(found[f].operator Object *());
			if (!vf) {
				continue;
			}
			const Array crowns = vf->get_crowns_near(eye, range);
			for (int i = 0; i < crowns.size(); i++) {
				const Dictionary c = crowns[i];
				if (!bool(c.get("conifer", false))) {
					continue;
				}
				const Vector3 foot = c["position"];
				const float h = c["height"];
				const float base = c["crown_base"];
				const float r = c["crown_radius"];
				const float mid = 0.5f * (h + base);
				const Vector3 at = foot + Vector3(0, mid, 0);
				const Vector3 w = wm->wind_at(at, mid);
				const float k = cone_share * release_factor(wm->air_humidity_at(at), wm->surface_wetness_at(at), wm->rainfall_at(at), w.length());
				if (k <= 0.01f) {
					continue;
				}
				Cand cd;
				cd.s.position = at;
				cd.s.velocity = w;
				cd.s.radius = CLAMP(r * 0.8f, 0.5f, 5.0f);
				cd.s.rate = k;
				cd.s.conifer = true;
				cd.d2 = at.distance_squared_to(eye);
				cones.push_back(cd);
			}
		}
		cones.sort();
		for (uint32_t i = 0; i < cones.size() && int(i) < max_conifer_sources; i++) {
			sources.push_back(cones[i].s);
			conifer_count++;
		}
	}
	// Flowering sward: points on a 6 m lattice round the camera where a GrassField3D's rules grow grass, the best
	// covered nearest first; each sheds by the wind half a metre up (the panicles).
	if (max_grass_sources > 0 && grass_share > 0.0f) {
		LocalVector<Cand> sward;
		const TypedArray<Node> found = get_tree()->get_root()->find_children("*", "GrassField3D", true, false);
		for (int f = 0; f < found.size(); f++) {
			const GrassField3D *gf = Object::cast_to<GrassField3D>(found[f].operator Object *());
			if (!gf || !gf->is_visible_in_tree()) {
				continue;
			}
			const TerrainDataMap3D *map = gf->get_data_map().is_empty() ? nullptr : Object::cast_to<TerrainDataMap3D>(gf->get_node_or_null(gf->get_data_map()));
			if (!map) {
				continue;
			}
			const Ref<PlantSpecies> rules = gf->get_rules();
			const float reach = MIN(range, 30.0f);
			const float step = 6.0f;
			const int n = int(reach / step);
			for (int j = -n; j <= n; j++) {
				for (int i = -n; i <= n; i++) {
					Vector3 p(Math::floor(eye.x / step + i) * step, 0.0f, Math::floor(eye.z / step + j) * step);
					const float d2 = Vector2(p.x - eye.x, p.z - eye.z).length_squared();
					if (d2 > reach * reach) {
						continue;
					}
					p.y = map->get_height_at(p);
					if (!Math::is_finite(p.y)) {
						continue;
					}
					const float cover = rules.is_valid() ? rules->get_suitability(map, p) : 1.0f;
					if (cover < 0.3f) {
						continue;
					}
					const Vector3 at = p + Vector3(0, 0.5f, 0);
					const Vector3 w = wm->wind_at(at, 0.5f);
					// The sward's wind half a metre up is a fraction of the 10 m wind: grass sheds by the open wind.
					const float k = cover * grass_share * release_factor(wm->air_humidity_at(at), wm->surface_wetness_at(at), wm->rainfall_at(at), wm->wind_at(at, 10.0f).length());
					if (k <= 0.01f) {
						continue;
					}
					Cand cd;
					cd.s.position = at;
					cd.s.velocity = w;
					cd.s.radius = 2.5f;
					cd.s.rate = k * 0.5f; // a sward sheds less per emitter volume than a pine's crown (estimate)
					cd.d2 = d2;
					sward.push_back(cd);
				}
			}
		}
		sward.sort();
		for (uint32_t i = 0; i < sward.size() && int(i) < max_grass_sources; i++) {
			sources.push_back(sward[i].s);
			grass_count++;
		}
	}
	_feed();
}

void PollenCloud3D::_feed() {
	if (!sources.is_empty()) {
		_build();
	}
	for (uint32_t i = 0; i < emitters.size(); i++) {
		Node3D *e = emitters[i];
		if (i >= sources.size()) {
			e->set("enabled", false);
			continue;
		}
		const Source &s = sources[i];
		e->set("enabled", true);
		e->set("shape", 0); // sphere
		e->set("radius", s.radius);
		e->set_global_position(s.position);
		e->set("velocity", s.velocity);
		e->set("divergence", 0.5f); // shaken loose, not blown out
		e->set("smoke", s.rate * smoke_per_source);
		e->set("temperature", 0.0f);
		e->set("couple_rate", 2.0f);
	}
}

Array PollenCloud3D::get_sources() const {
	Array out;
	for (const Source &s : sources) {
		Dictionary d;
		d["position"] = s.position;
		d["velocity"] = s.velocity;
		d["radius"] = s.radius;
		d["rate"] = s.rate;
		d["conifer"] = s.conifer;
		out.push_back(d);
	}
	return out;
}

void PollenCloud3D::set_enabled(bool p_enable) {
	enabled = p_enable;
	if (!enabled) {
		sources.clear();
		_feed();
	}
}

void PollenCloud3D::set_color(const Color &p_color) {
	color = p_color;
	if (flow) {
		flow->set("smoke_color", p_color);
	}
}

void PollenCloud3D::set_terrain_path(const NodePath &p_path) {
	terrain_path = p_path;
	if (flow && !p_path.is_empty()) {
		flow->set("terrain_path", p_path);
	}
}

void PollenCloud3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			if (!Engine::get_singleton()->is_editor_hint()) {
				set_process_internal(true);
			}
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			update_left -= get_process_delta_time();
			if (update_left <= 0.0) {
				update_left = 0.25;
				update_sources();
			}
		} break;
	}
}

void PollenCloud3D::_bind_methods() {
	ClassDB::bind_static_method("PollenCloud3D", D_METHOD("release_factor", "humidity", "wetness", "rain_mmh", "wind"), &PollenCloud3D::release_factor);
	ClassDB::bind_static_method("PollenCloud3D", D_METHOD("window_share", "day", "window"), &PollenCloud3D::window_share);
	ClassDB::bind_method(D_METHOD("update_sources"), &PollenCloud3D::update_sources);
	ClassDB::bind_method(D_METHOD("has_flow"), &PollenCloud3D::has_flow);
	ClassDB::bind_method(D_METHOD("get_flow"), &PollenCloud3D::get_flow);
	ClassDB::bind_method(D_METHOD("get_release"), &PollenCloud3D::get_release);
	ClassDB::bind_method(D_METHOD("get_conifer_sources"), &PollenCloud3D::get_conifer_sources);
	ClassDB::bind_method(D_METHOD("get_grass_sources"), &PollenCloud3D::get_grass_sources);
	ClassDB::bind_method(D_METHOD("get_sources"), &PollenCloud3D::get_sources);
	ClassDB::bind_method(D_METHOD("set_enabled", "enable"), &PollenCloud3D::set_enabled);
	ClassDB::bind_method(D_METHOD("is_enabled"), &PollenCloud3D::is_enabled);
	ClassDB::bind_method(D_METHOD("set_range", "range"), &PollenCloud3D::set_range);
	ClassDB::bind_method(D_METHOD("get_range"), &PollenCloud3D::get_range);
	ClassDB::bind_method(D_METHOD("set_max_conifer_sources", "count"), &PollenCloud3D::set_max_conifer_sources);
	ClassDB::bind_method(D_METHOD("get_max_conifer_sources"), &PollenCloud3D::get_max_conifer_sources);
	ClassDB::bind_method(D_METHOD("set_max_grass_sources", "count"), &PollenCloud3D::set_max_grass_sources);
	ClassDB::bind_method(D_METHOD("get_max_grass_sources"), &PollenCloud3D::get_max_grass_sources);
	ClassDB::bind_method(D_METHOD("set_conifer_window", "days"), &PollenCloud3D::set_conifer_window);
	ClassDB::bind_method(D_METHOD("get_conifer_window"), &PollenCloud3D::get_conifer_window);
	ClassDB::bind_method(D_METHOD("set_grass_window", "days"), &PollenCloud3D::set_grass_window);
	ClassDB::bind_method(D_METHOD("get_grass_window"), &PollenCloud3D::get_grass_window);
	ClassDB::bind_method(D_METHOD("set_smoke_per_source", "value"), &PollenCloud3D::set_smoke_per_source);
	ClassDB::bind_method(D_METHOD("get_smoke_per_source"), &PollenCloud3D::get_smoke_per_source);
	ClassDB::bind_method(D_METHOD("set_color", "color"), &PollenCloud3D::set_color);
	ClassDB::bind_method(D_METHOD("get_color"), &PollenCloud3D::get_color);
	ClassDB::bind_method(D_METHOD("set_terrain_path", "path"), &PollenCloud3D::set_terrain_path);
	ClassDB::bind_method(D_METHOD("get_terrain_path"), &PollenCloud3D::get_terrain_path);

	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enabled"), "set_enabled", "is_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "range", PROPERTY_HINT_RANGE, "1,500,1,suffix:m"), "set_range", "get_range");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_conifer_sources", PROPERTY_HINT_RANGE, "0,24,1"), "set_max_conifer_sources", "get_max_conifer_sources");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_grass_sources", PROPERTY_HINT_RANGE, "0,24,1"), "set_max_grass_sources", "get_max_grass_sources");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "conifer_window"), "set_conifer_window", "get_conifer_window");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "grass_window"), "set_grass_window", "get_grass_window");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "smoke_per_source", PROPERTY_HINT_RANGE, "0,20,0.01"), "set_smoke_per_source", "get_smoke_per_source");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "color"), "set_color", "get_color");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "terrain_path"), "set_terrain_path", "get_terrain_path");
}

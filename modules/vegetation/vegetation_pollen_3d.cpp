/**************************************************************************/
/*  vegetation_pollen_3d.cpp                                              */
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

#include "vegetation_pollen_3d.h"

#include "plant_generator.h"
#include "plant_species.h"
#include "vegetation_field_3d.h"

#include "core/templates/hash_set.h"
#include "core/config/engine.h"
#include "core/object/class_db.h"
#include "scene/main/scene_tree.h"
#include "scene/resources/mesh.h"

#include "modules/weather/weather_map_3d.h"

static constexpr float POLLEN_GRID = 2.0f; // Metres per hash cell.
static constexpr float POLLEN_PULSE = 0.15f; // Seconds a puff's emitter runs.

static Vector2i pollen_key(const Vector3 &p_at) {
	return Vector2i(int(Math::floor(p_at.x / POLLEN_GRID)), int(Math::floor(p_at.z / POLLEN_GRID)));
}

bool VegetationPollen3D::is_flow_available() const {
	return ClassDB::class_exists("PhysXFlow3D") && ClassDB::class_exists("PhysXFlowEmitter3D");
}

PackedStringArray VegetationPollen3D::get_configuration_warnings() const {
	PackedStringArray w = Node3D::get_configuration_warnings();
	if (!is_flow_available()) {
		w.push_back(RTR("This engine has no PhysX Flow (build with flow_sdk=): no pollen puffs."));
	}
	if (vegetation_field.is_empty()) {
		w.push_back(RTR("Set vegetation_field: the plants that shed pollen come from its placements."));
	}
	return w;
}

Node *VegetationPollen3D::_field() const {
	return vegetation_field.is_empty() ? nullptr : get_node_or_null(vegetation_field);
}

void VegetationPollen3D::set_vegetation_field(const NodePath &p_v) {
	vegetation_field = p_v;
	indexed_count = -1;
}

void VegetationPollen3D::set_season(const Dictionary &p_v) {
	season = p_v;
	release.clear();
}

void VegetationPollen3D::reindex() {
	indexed_count = -1;
}

int VegetationPollen3D::get_active_puffs() const {
	int n = 0;
	for (const Puff &p : puffs) {
		n += p.left > 0.0f ? 1 : 0;
	}
	return n;
}

// The pollen species' placements, hashed on a 2 m grid; rebuilt when the field places again.
void VegetationPollen3D::_index() {
	VegetationField3D *vf = Object::cast_to<VegetationField3D>(_field());
	if (!vf) {
		plants.clear();
		grid.clear();
		indexed_count = -1;
		return;
	}
	if (vf->get_instance_id() == ObjectID(indexed_field) && vf->get_plant_count() == indexed_count) {
		return;
	}
	indexed_field = uint64_t(vf->get_instance_id());
	indexed_count = vf->get_plant_count();
	plants.clear();
	grid.clear();
	const Ref<Biome> biome = vf->get_biome();
	if (biome.is_null()) {
		return;
	}
	const TypedArray<PlantSpecies> species = biome->get_species();
	for (int i = 0; i < species.size(); i++) {
		const Ref<PlantSpecies> sp = species[i];
		if (sp.is_null() || !sp->has_meta("pollen")) {
			continue;
		}
		const float pollen = sp->get_meta("pollen");
		const int preset = sp->has_meta("plant_preset") ? int(sp->get_meta("plant_preset")) : -1;
		// Size from the first mesh: the flowers stand near the top, the reach is the tuft's radius.
		float top = 0.4f, reach = 0.2f;
		const TypedArray<Mesh> meshes = sp->get_meshes();
		if (meshes.size() > 0) {
			const Ref<Mesh> m = meshes[0];
			if (m.is_valid()) {
				const AABB box = m->get_aabb();
				top = MAX(box.get_end().y * 0.85f, 0.05f);
				reach = CLAMP(MAX(box.size.x, box.size.z) * 0.35f, 0.08f, 0.6f);
			}
		}
		const PackedVector3Array at = vf->get_placements(i);
		for (int k = 0; k < at.size(); k++) {
			Plant p;
			p.position = at[k];
			p.top = top;
			p.reach = reach;
			p.pollen = pollen;
			p.preset = preset;
			grid[pollen_key(p.position)].push_back(plants.size());
			plants.push_back(p);
		}
	}
}

// The share of each pollen preset shedding today; dew or rain on the anthers holds it back.
void VegetationPollen3D::_refresh_release() {
	Dictionary s = season;
	const WeatherMap3D *wm = weather_map.is_empty() ? nullptr : Object::cast_to<WeatherMap3D>(get_node_or_null(weather_map));
	if (s.is_empty() && wm) {
		s = const_cast<WeatherMap3D *>(wm)->get_season(); // Not const: it eases the season state.
	}
	release.clear();
	for (const Plant &p : plants) {
		if (p.preset >= 0 && !release.has(p.preset)) {
			release.insert(p.preset, PlantGenerator::get_pollen_release(PlantGenerator::Preset(p.preset), s));
		}
	}
}

void VegetationPollen3D::_fire(const Plant &p_plant, const Vector3 &p_velocity, const Vector3 &p_wind, float p_amount) {
	if (puffs.is_empty()) {
		return;
	}
	Puff &pf = puffs[next_puff];
	next_puff = (next_puff + 1) % puffs.size();
	pf.left = POLLEN_PULSE;
	Node3D *e = pf.emitter;
	e->set("enabled", true);
	e->set_global_position(p_plant.position + Vector3(0, p_plant.top, 0));
	e->set("radius", CLAMP(p_plant.reach * 0.6f, 0.08f, 0.3f));
	// The shaken heads fling it along the push and a little up; it swells as it leaves.
	// The ground wind carries it from the start: a wind box over the area would take hundreds of
	// blocks at these cells.
	e->set("velocity", p_velocity * 0.35f + p_wind + Vector3(0, 0.25f, 0));
	e->set("smoke", MIN(p_amount, 4.0f));
	e->set("temperature", 0.0f);
	e->set("divergence", 1.5f);
	e->set("couple_rate", 25.0f);
	fired++;
}

void VegetationPollen3D::_resize_pool() {
	if (!flow) {
		return;
	}
	while ((int)puffs.size() > max_puffs) {
		puffs[puffs.size() - 1].emitter->queue_free();
		puffs.resize(puffs.size() - 1);
	}
	while ((int)puffs.size() < max_puffs) {
		Node3D *e = Object::cast_to<Node3D>(ClassDB::instantiate("PhysXFlowEmitter3D"));
		if (!e) {
			return;
		}
		e->set_name(vformat("Puff%d", (int)puffs.size()));
		e->set("enabled", false);
		flow->add_child(e);
		e->set("flow", e->get_path_to(flow));
		Puff p;
		p.emitter = e;
		puffs.push_back(p);
	}
	next_puff = 0;
}

void VegetationPollen3D::set_max_puffs(int p_v) {
	max_puffs = CLAMP(p_v, 1, 64);
	_resize_pool();
}

void VegetationPollen3D::set_pollen_color(const Color &p_c) {
	pollen_color = p_c;
	if (flow) {
		flow->set("smoke_color", p_c);
	}
}

void VegetationPollen3D::_build() {
	if (flow || !is_flow_available()) {
		return;
	}
	flow = Object::cast_to<Node3D>(ClassDB::instantiate("PhysXFlow3D"));
	if (!flow) {
		return;
	}
	flow->set_name("Flow");
	// Pollen: fine cells for small puffs, no heat, grains settling at 2 to 3 cm/s (barely heavy), a
	// fade over several seconds as the puff spreads thin, and swirl from the walker's wake.
	flow->set("cell_size", 0.05f);
	flow->set("max_blocks", 384);
	flow->set("combustion", false);
	flow->set("buoyancy", 0.0f);
	flow->set("smoke_buoyancy", -0.03f);
	flow->set("smoke_fade", 0.35f);
	flow->set("vorticity", 1.5f);
	flow->set("emission_strength", 0.0f);
	flow->set("smoke_density", 6.0f);
	flow->set("smoke_color", pollen_color);
	flow->set("collide_with_bodies", true);
	flow->set("collision_range", 4.0f);
	flow->set("terrain_floor", true);
	add_child(flow, false, INTERNAL_MODE_FRONT);
	flow->set_as_top_level(true);
	flow->set_global_transform(Transform3D());
	_resize_pool();
}

void VegetationPollen3D::_step(float p_delta) {
	time += p_delta;
	for (Puff &p : puffs) {
		if (p.left > 0.0f) {
			p.left -= p_delta;
			if (p.left <= 0.0f) {
				p.emitter->set("enabled", false);
			}
		}
	}
	if (!enabled || !is_inside_tree()) {
		return;
	}
	_index();
	season_timer -= p_delta;
	if (season_timer <= 0.0f || release.is_empty()) {
		season_timer = 5.0f;
		_refresh_release();
	}
	const WeatherMap3D *wm = weather_map.is_empty() ? nullptr : Object::cast_to<WeatherMap3D>(get_node_or_null(weather_map));
	// The trample group and world_contact, every body any system should feel (docs/ideas/world-links.md).
	Vector<Node *> nodes = get_tree()->get_nodes_in_group(trample_group);
	nodes.append_array(get_tree()->get_nodes_in_group(SNAME("world_contact")));
	LocalVector<Mover> next;
	HashSet<ObjectID> listed;
	for (Node *n : nodes) {
		Node3D *b = Object::cast_to<Node3D>(n);
		if (!b || listed.has(b->get_instance_id())) {
			continue;
		}
		listed.insert(b->get_instance_id());
		Mover m;
		m.id = b->get_instance_id();
		m.last = b->get_global_position();
		m.seen = true;
		Vector3 prev = m.last;
		bool known = false;
		for (const Mover &o : movers) {
			if (o.id == m.id) {
				prev = o.last;
				known = true;
				break;
			}
		}
		next.push_back(m);
		if (!known || p_delta <= 0.0f || plants.is_empty()) {
			continue;
		}
		const Vector3 vel = (m.last - prev) / p_delta;
		const float speed = Vector2(vel.x, vel.z).length();
		if (speed < min_speed || speed > 30.0f) {
			continue;
		}
		Vector3 wind;
		if (wm) {
			wind = wm->get_wind_at(m.last, 1.0f);
			wind.y = 0.0f;
		}
		const float radius = b->has_meta("trample_radius") ? float(b->get_meta("trample_radius")) : 0.4f;
		const Vector2i k0 = pollen_key(m.last - Vector3(radius + 0.6f, 0, radius + 0.6f));
		const Vector2i k1 = pollen_key(m.last + Vector3(radius + 0.6f, 0, radius + 0.6f));
		// Every plant the body brushes this step sheds into one puff in front of it: the pool stays
		// small however dense the sward.
		Vector3 sum_at;
		float sum = 0.0f, reach = 0.0f;
		for (int z = k0.y; z <= k1.y; z++) {
			for (int x = k0.x; x <= k1.x; x++) {
				const LocalVector<uint32_t> *cell = grid.getptr(Vector2i(x, z));
				if (!cell) {
					continue;
				}
				for (uint32_t idx : *cell) {
					Plant &p = plants[idx];
					const Vector2 d(p.position.x - m.last.x, p.position.z - m.last.z);
					if (d.length() > radius + p.reach) {
						continue;
					}
					// A body below the heads (a rolling ball low down) still shakes the stems; one far
					// above them (a bird) does not.
					if (m.last.y - radius > p.position.y + p.top + 0.2f) {
						continue;
					}
					const float share = p.preset >= 0 && release.has(p.preset) ? release[p.preset] : 0.0f;
					if (share <= 0.0f) {
						continue;
					}
					// The anthers refill over the recharge time after a shake.
					const float charge = CLAMP((time - p.last) / recharge_time, 0.0f, 1.0f);
					if (charge < 0.25f) {
						continue;
					}
					float dry = 1.0f;
					if (wm) {
						dry = 1.0f - Math::smoothstep(0.15f, 0.5f, wm->get_wetness_at(p.position)); // Wet anthers hold it.
					}
					const float a = p.pollen * share * charge * dry;
					p.last = time;
					sum += a;
					sum_at += (p.position + Vector3(0, p.top, 0)) * a;
					reach = MAX(reach, p.reach);
				}
			}
		}
		const float amount = strength * sum * CLAMP(speed / 1.5f, 0.3f, 1.5f) * 0.6f;
		if (amount > 0.02f) {
			Plant puff;
			puff.position = sum_at / sum;
			puff.top = 0.0f;
			puff.reach = MAX(reach, radius);
			_fire(puff, vel, wind, amount);
		}
	}
	movers = next;
}

void VegetationPollen3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			if (Engine::get_singleton()->is_editor_hint()) {
				break;
			}
			_build();
			set_physics_process_internal(true);
		} break;
		case NOTIFICATION_INTERNAL_PHYSICS_PROCESS: {
			_step(get_physics_process_delta_time());
		} break;
	}
}

void VegetationPollen3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_enabled", "enabled"), &VegetationPollen3D::set_enabled);
	ClassDB::bind_method(D_METHOD("is_enabled"), &VegetationPollen3D::is_enabled);
	ClassDB::bind_method(D_METHOD("set_vegetation_field", "path"), &VegetationPollen3D::set_vegetation_field);
	ClassDB::bind_method(D_METHOD("get_vegetation_field"), &VegetationPollen3D::get_vegetation_field);
	ClassDB::bind_method(D_METHOD("set_weather_map", "path"), &VegetationPollen3D::set_weather_map);
	ClassDB::bind_method(D_METHOD("get_weather_map"), &VegetationPollen3D::get_weather_map);
	ClassDB::bind_method(D_METHOD("set_trample_group", "group"), &VegetationPollen3D::set_trample_group);
	ClassDB::bind_method(D_METHOD("get_trample_group"), &VegetationPollen3D::get_trample_group);
	ClassDB::bind_method(D_METHOD("set_season", "season"), &VegetationPollen3D::set_season);
	ClassDB::bind_method(D_METHOD("get_season"), &VegetationPollen3D::get_season);
	ClassDB::bind_method(D_METHOD("set_min_speed", "speed"), &VegetationPollen3D::set_min_speed);
	ClassDB::bind_method(D_METHOD("get_min_speed"), &VegetationPollen3D::get_min_speed);
	ClassDB::bind_method(D_METHOD("set_strength", "strength"), &VegetationPollen3D::set_strength);
	ClassDB::bind_method(D_METHOD("get_strength"), &VegetationPollen3D::get_strength);
	ClassDB::bind_method(D_METHOD("set_recharge_time", "seconds"), &VegetationPollen3D::set_recharge_time);
	ClassDB::bind_method(D_METHOD("get_recharge_time"), &VegetationPollen3D::get_recharge_time);
	ClassDB::bind_method(D_METHOD("set_max_puffs", "count"), &VegetationPollen3D::set_max_puffs);
	ClassDB::bind_method(D_METHOD("get_max_puffs"), &VegetationPollen3D::get_max_puffs);
	ClassDB::bind_method(D_METHOD("set_pollen_color", "color"), &VegetationPollen3D::set_pollen_color);
	ClassDB::bind_method(D_METHOD("get_pollen_color"), &VegetationPollen3D::get_pollen_color);
	ClassDB::bind_method(D_METHOD("get_flow"), &VegetationPollen3D::get_flow);
	ClassDB::bind_method(D_METHOD("is_flow_available"), &VegetationPollen3D::is_flow_available);
	ClassDB::bind_method(D_METHOD("get_pollen_plant_count"), &VegetationPollen3D::get_pollen_plant_count);
	ClassDB::bind_method(D_METHOD("get_puffs_fired"), &VegetationPollen3D::get_puffs_fired);
	ClassDB::bind_method(D_METHOD("get_active_puffs"), &VegetationPollen3D::get_active_puffs);
	ClassDB::bind_method(D_METHOD("reindex"), &VegetationPollen3D::reindex);

	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enabled"), "set_enabled", "is_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "vegetation_field", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "VegetationField3D"), "set_vegetation_field", "get_vegetation_field");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "weather_map", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "WeatherMap3D"), "set_weather_map", "get_weather_map");
	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "trample_group"), "set_trample_group", "get_trample_group");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "season"), "set_season", "get_season");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "min_speed", PROPERTY_HINT_RANGE, "0,5,0.01,suffix:m/s"), "set_min_speed", "get_min_speed");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "strength", PROPERTY_HINT_RANGE, "0,10,0.01"), "set_strength", "get_strength");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "recharge_time", PROPERTY_HINT_RANGE, "0.1,600,0.1,suffix:s"), "set_recharge_time", "get_recharge_time");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_puffs", PROPERTY_HINT_RANGE, "1,64,1"), "set_max_puffs", "get_max_puffs");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "pollen_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_pollen_color", "get_pollen_color");
}

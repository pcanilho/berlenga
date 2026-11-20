/**************************************************************************/
/*  vegetation_mist_3d.cpp                                                */
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

#include "vegetation_mist_3d.h"

#include "core/config/engine.h"
#include "core/object/class_db.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/light_3d.h"
#include "scene/main/viewport.h"
#include "servers/physics_3d/physics_server_3d.h"

#include "modules/weather/weather_map_3d.h"
#include "modules/world/world_contracts.h"

static constexpr float MIST_HOLLOW_RADIUS = 10.0f; // Ring for the hollow index, metres.
static constexpr float MIST_LEVEL_PERIOD = 0.5f; // Seconds between weather reads.

static Vector2i mist_key(const Vector3 &p_at, float p_cell) {
	return Vector2i(int(Math::floor(p_at.x / p_cell)), int(Math::floor(p_at.z / p_cell)));
}

bool VegetationMist3D::is_flow_available() const {
	return ClassDB::class_exists("PhysXFlow3D") && ClassDB::class_exists("PhysXFlowEmitter3D");
}

PackedStringArray VegetationMist3D::get_configuration_warnings() const {
	PackedStringArray w = Node3D::get_configuration_warnings();
	if (!is_flow_available()) {
		w.push_back(RTR("This engine has no PhysX Flow (build with flow_sdk=): the mist stays off."));
	}
	if (mist_amount < 0.0f && weather_map.is_empty()) {
		w.push_back(RTR("With mist_amount below 0 the mist follows the weather: set weather_map, or set mist_amount."));
	}
	return w;
}

Node3D *VegetationMist3D::_target() const {
	if (!follow_target.is_empty()) {
		return Object::cast_to<Node3D>(get_node_or_null(follow_target));
	}
	if (is_inside_tree() && get_viewport()) {
		return get_viewport()->get_camera_3d();
	}
	return nullptr;
}

float VegetationMist3D::_ground_height(const Vector3 &p_at) const {
	if (!terrain.is_empty()) {
		Node *t = get_node_or_null(terrain);
		if (t && t->has_method("get_height")) {
			const Variant h = t->call("get_height", Vector3(p_at.x, p_at.y + 500.0f, p_at.z));
			if (h.get_type() == Variant::FLOAT || h.get_type() == Variant::INT) {
				const float f = h;
				if (Math::is_finite(f)) {
					return f;
				}
			}
		}
	}
	if (!is_inside_tree() || get_world_3d().is_null()) {
		return p_at.y;
	}
	PhysicsDirectSpaceState3D *space = get_world_3d()->get_direct_space_state();
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	if (!space) {
		return p_at.y;
	}
	PhysicsDirectSpaceState3D::RayParameters rp;
	rp.from = Vector3(p_at.x, p_at.y + 500.0f, p_at.z);
	rp.to = Vector3(p_at.x, p_at.y - 3000.0f, p_at.z);
	rp.collide_with_areas = false;
	for (int tries = 0; tries < 6; tries++) {
		PhysicsDirectSpaceState3D::RayResult rr;
		if (!space->intersect_ray(rp, rr)) {
			break;
		}
		if (ps->body_get_mode(rr.rid) != PhysicsServer3D::BODY_MODE_STATIC) {
			rp.exclude.insert(rr.rid); // Walkers and props are not the ground.
			continue;
		}
		return rr.position.y;
	}
	return p_at.y;
}

// The cell's ground height and its propensity for mist: cooled air drains off crests into hollows
// (cold-air pooling), so a cell below the mean of a ring round it gathers more.
Vector2 VegetationMist3D::_ground_cell(const Vector2i &p_key) {
	if (const Vector2 *c = ground_cache.getptr(p_key)) {
		return *c;
	}
	const Vector3 centre((p_key.x + 0.5f) * mist_cell, get_global_position().y, (p_key.y + 0.5f) * mist_cell);
	const Node3D *t = _target();
	const float ref_y = t ? t->get_global_position().y : centre.y;
	const float h0 = _ground_height(Vector3(centre.x, ref_y, centre.z));
	float ring = 0.0f;
	for (int i = 0; i < 8; i++) {
		const float a = i * Math::TAU / 8.0f;
		ring += _ground_height(Vector3(centre.x + Math::cos(a) * MIST_HOLLOW_RADIUS, ref_y, centre.z + Math::sin(a) * MIST_HOLLOW_RADIUS));
	}
	const float hollow = ring / 8.0f - h0; // Metres below the ring: positive in a hollow.
	const float propensity = CLAMP(1.0f + hollow / 1.5f, 0.15f, 2.0f);
	const Vector2 out(h0, propensity);
	if (ground_cache.size() > 4096) {
		ground_cache.clear();
	}
	ground_cache.insert(p_key, out);
	return out;
}

float VegetationMist3D::get_propensity_at(const Vector3 &p_position) {
	return _ground_cell(mist_key(p_position, mist_cell)).y;
}

// Radiation fog needs air near saturation, calm (wind mixes the cooled layer away above about
// 3 m/s), a clear sky to cool under, and no sun to burn it off (Gultepe et al. 2007; Price 2011).
// Wet ground feeds the cooled layer its vapour (fog comes most often on the night after rain; Bergot
// and Guedalia 1994), and the layer saturates where the radiating surface cools below the air's dew
// point (the weather's surface temperature, with a sky clock).
float VegetationMist3D::weather_mist_level() const {
	float humid = 0.0f, calm = 1.0f, clear = 1.0f, dark = 1.0f;
	const Node3D *t = _target();
	const Vector3 at = t ? t->get_global_position() : get_global_position();
	const WorldWeather *wm = weather_map.is_empty() || !WorldContracts::weather()->has_provider() ? nullptr : WorldContracts::weather(); // weather_map set: follow the weather contract
	if (!wm) {
		return 0.0f;
	}
	const float wet = CLAMP(wm->surface_wetness_at(at), 0.0f, 1.0f);
	humid = Math::smoothstep(0.86f, 0.97f, wm->air_humidity_at(at) + 0.06f * wet);
	const float below_dew = wm->dew_point_at(at) - wm->surface_temperature_at(at);
	humid = MAX(humid, Math::smoothstep(0.0f, 2.0f, below_dew) * Math::smoothstep(0.8f, 0.92f, wm->air_humidity_at(at) + 0.06f * wet));
	calm = 1.0f - Math::smoothstep(1.5f, 4.0f, wm->mean_wind_at(at).length());
	clear = 1.0f - 0.7f * CLAMP(wm->cloud_cover_at(at), 0.0f, 1.0f);
	if (wm->air_temperature_at(at) < -12.0f) {
		humid *= 0.3f; // Ice fog is another thing; little liquid mist this cold.
	}
	const DirectionalLight3D *sun = sun_light.is_empty() ? nullptr : Object::cast_to<DirectionalLight3D>(get_node_or_null(sun_light));
	if (sun && sun->is_visible_in_tree()) {
		const float elevation = Math::rad_to_deg(Math::asin(CLAMP(sun->get_global_basis().get_column(2).normalized().y, -1.0f, 1.0f)));
		dark = 1.0f - Math::smoothstep(4.0f, 18.0f, elevation); // Burns off as the sun climbs.
	}
	// Hill fog: where the low cloud's base sits on the ground (WorldWeather.cloud_base_at, the day's condensation
	// level) the ground stands in the cloud. It is cloud, not a radiation layer: wind, a cloudy sky and the sun do not
	// clear it (Gultepe et al. 2007, orographic and cloud-base-lowering fog).
	float hill = 0.0f;
	const float cover = CLAMP(wm->cloud_cover_at(at), 0.0f, 1.0f);
	if (cover > 0.3f) {
		const float g = WorldContracts::ground()->has_provider() ? WorldContracts::ground()->height_at(at) : at.y;
		if (Math::is_finite(g)) {
			hill = Math::smoothstep(wm->cloud_base_at(at) - 40.0f, wm->cloud_base_at(at), g) * Math::smoothstep(0.3f, 0.7f, cover);
		}
	}
	return MAX(humid * calm * clear * dark, hill);
}

void VegetationMist3D::_apply_flow_settings() {
	if (!flow) {
		return;
	}
	flow->set("cell_size", flow_cell_size);
	const float span = 2.0f * mist_range + mist_cell;
	const float block_area = (32.0f * flow_cell_size) * (16.0f * flow_cell_size);
	// Blocks are 32x16x16 cells; the layer crosses two or three block rows over uneven ground,
	// and the gas drifts past the cells' edge.
	flow->set("max_blocks", CLAMP(int(Math::ceil(span * span / block_area * 6.0f)), 64, 1024));
	flow->set("combustion", false);
	flow->set("buoyancy", 0.0f);
	flow->set("smoke_buoyancy", -heaviness); // Air a few degrees colder: 1 to 2 % denser, it drains downhill.
	flow->set("smoke_fade", 0.03f); // Evaporates slowly; the cells hold the layer.
	flow->set("vorticity", 0.3f); // Calm air.
	flow->set("emission_strength", 0.0f);
	flow->set("smoke_density", extinction);
	flow->set("smoke_color", mist_color);
	flow->set("scattering_anisotropy", 0.75f); // 10 um droplets scatter forward (HG g about 0.8).
	flow->set("collide_with_bodies", true);
	flow->set("collision_range", mist_range + mist_cell);
	flow->set("terrain_floor", true);
	if (!terrain.is_empty()) {
		Node *t = get_node_or_null(terrain);
		if (t) {
			flow->set("terrain_path", flow->get_path_to(t));
		}
	}
}

void VegetationMist3D::_resize_pool() {
	if (!flow) {
		return;
	}
	const int r = int(Math::ceil(mist_range / mist_cell));
	const int want = (2 * r + 1) * (2 * r + 1);
	while ((int)cells.size() > want) {
		cells[cells.size() - 1].emitter->queue_free();
		cells.resize(cells.size() - 1);
	}
	while ((int)cells.size() < want) {
		Object *o = ClassDB::instantiate("PhysXFlowEmitter3D");
		Node3D *e = Object::cast_to<Node3D>(o);
		if (!e) {
			if (o) {
				memdelete(o);
			}
			return;
		}
		e->set_name(vformat("Mist%d", (int)cells.size()));
		e->set("shape", 1); // Box.
		e->set("enabled", false);
		flow->add_child(e);
		e->set("flow", e->get_path_to(flow));
		Cell c;
		c.key = Vector2i(INT32_MAX, INT32_MAX);
		c.emitter = e;
		cells.push_back(c);
	}
}

void VegetationMist3D::_build() {
	if (flow || !is_flow_available()) {
		return;
	}
	flow = Object::cast_to<Node3D>(ClassDB::instantiate("PhysXFlow3D"));
	if (!flow) {
		return;
	}
	flow->set_name("Flow");
	add_child(flow, false, INTERNAL_MODE_FRONT);
	flow->set_as_top_level(true);
	flow->set_global_transform(Transform3D());
	_apply_flow_settings();
	_resize_pool();
}

void VegetationMist3D::_step(float p_delta) {
	if (!flow) {
		return;
	}
	level_timer -= p_delta;
	if (level_timer <= 0.0f) {
		level_timer = MIST_LEVEL_PERIOD;
		level = mist_amount >= 0.0f ? MIN(mist_amount, 2.0f) : weather_mist_level();
	}
	const Node3D *t = _target();
	if (!enabled || !t) {
		for (Cell &c : cells) {
			c.emitter->set("enabled", false);
		}
		active_cells = 0;
		return;
	}
	const Vector3 centre = t->get_global_position();
	const Vector2i mid = mist_key(centre, mist_cell);
	const int r = int(Math::ceil(mist_range / mist_cell));
	Vector3 wind;
	const WorldWeather *wm = weather_map.is_empty() || !WorldContracts::weather()->has_provider() ? nullptr : WorldContracts::weather(); // weather_map set: follow the weather contract
	if (wm) {
		wind = wm->wind_at(centre, 1.0f); // The air the mist sits in, a metre up.
		wind.y = 0.0f;
	}
	int i = 0;
	active_cells = 0;
	for (int dz = -r; dz <= r; dz++) {
		for (int dx = -r; dx <= r; dx++) {
			if (i >= (int)cells.size()) {
				break;
			}
			Cell &c = cells[i++];
			const Vector2i key = mid + Vector2i(dx, dz);
			// A round patch: corner cells past the range stay off.
			const Vector2 off = Vector2((key.x + 0.5f) * mist_cell - centre.x, (key.y + 0.5f) * mist_cell - centre.z);
			if (off.length() > mist_range + mist_cell * 0.5f) {
				c.emitter->set("enabled", false);
				continue;
			}
			Vector2 g = _ground_cell(key);
			// Thinned over the outer third of the patch, so it melts into the environment's fog
			// beyond instead of ending at a line.
			const float edge = 1.0f - Math::smoothstep(mist_range * 0.6f, mist_range + mist_cell * 0.5f, off.length());
			const Vector3 cell_at((key.x + 0.5f) * mist_cell, g.x, (key.y + 0.5f) * mist_cell);
			float local = 1.0f;
			if (wm) {
				local += 0.5f * CLAMP(wm->surface_wetness_at(cell_at), 0.0f, 1.0f); // wet ground mists more
			}
			float smoke = level * g.y * local * edge;
			// Steam fog over open water (sea smoke): air much colder than the water takes up its vapour and
			// condenses it at once, in any light, while a breeze leaves it lying (Saunders 1964; ten or more
			// degrees for thick steam). The water lags the season (get_water_temperature).
			const float water = wm ? WorldContracts::water()->height_at(cell_at + Vector3(0, 2.0f, 0)) : -Math::INF;
			if (Math::is_finite(water) && water > g.x - 0.5f) {
				const float excess = wm->water_temperature() - wm->air_temperature_at(Vector3(cell_at.x, water, cell_at.z));
				const float steam = Math::smoothstep(3.0f, 10.0f, excess) * (1.0f - Math::smoothstep(6.0f, 12.0f, wind.length())) * Math::smoothstep(0.5f, 0.8f, wm->air_humidity_at(cell_at));
				g.x = water;
				smoke = MAX(smoke, steam * edge);
			}
			// The emitter couples the layer towards its level, so it holds the mist where the weather
			// wants it and refills what a walker stirred away. A tiny level keeps it coupled, so the
			// mist clears when the level drops instead of lingering.
			c.key = key;
			c.emitter->set("enabled", true);
			c.emitter->set_global_position(Vector3((key.x + 0.5f) * mist_cell, g.x + mist_depth * 0.3f, (key.y + 0.5f) * mist_cell));
			c.emitter->set("size", Vector3(mist_cell, mist_depth * 0.6f, mist_cell));
			c.emitter->set("smoke", MAX(smoke, 0.0005f));
			c.emitter->set("temperature", 0.0f);
			c.emitter->set("velocity", wind);
			c.emitter->set("couple_rate", 0.5f);
			active_cells += smoke > 0.01f ? 1 : 0;
		}
	}
}

void VegetationMist3D::set_enabled(bool p_v) {
	enabled = p_v;
}

void VegetationMist3D::set_terrain(const NodePath &p_v) {
	terrain = p_v;
	ground_cache.clear();
	_apply_flow_settings();
}

void VegetationMist3D::set_mist_range(float p_v) {
	mist_range = CLAMP(p_v, 2.0f, 200.0f);
	_apply_flow_settings();
	_resize_pool();
}

void VegetationMist3D::set_mist_cell(float p_v) {
	mist_cell = CLAMP(p_v, 1.0f, 64.0f);
	ground_cache.clear();
	_apply_flow_settings();
	_resize_pool();
}

void VegetationMist3D::set_extinction(float p_v) {
	extinction = MAX(p_v, 0.0f);
	if (flow) {
		flow->set("smoke_density", extinction);
	}
}

void VegetationMist3D::set_heaviness(float p_v) {
	heaviness = MAX(p_v, 0.0f);
	if (flow) {
		flow->set("smoke_buoyancy", -heaviness);
	}
}

void VegetationMist3D::set_flow_cell_size(float p_v) {
	flow_cell_size = CLAMP(p_v, 0.02f, 2.0f);
	_apply_flow_settings();
}

void VegetationMist3D::set_mist_color(const Color &p_c) {
	mist_color = p_c;
	if (flow) {
		flow->set("smoke_color", p_c);
	}
}

void VegetationMist3D::_notification(int p_what) {
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

void VegetationMist3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_enabled", "enabled"), &VegetationMist3D::set_enabled);
	ClassDB::bind_method(D_METHOD("is_enabled"), &VegetationMist3D::is_enabled);
	ClassDB::bind_method(D_METHOD("set_follow_target", "path"), &VegetationMist3D::set_follow_target);
	ClassDB::bind_method(D_METHOD("get_follow_target"), &VegetationMist3D::get_follow_target);
	ClassDB::bind_method(D_METHOD("set_terrain", "path"), &VegetationMist3D::set_terrain);
	ClassDB::bind_method(D_METHOD("get_terrain"), &VegetationMist3D::get_terrain);
	ClassDB::bind_method(D_METHOD("set_weather_map", "path"), &VegetationMist3D::set_weather_map);
	ClassDB::bind_method(D_METHOD("get_weather_map"), &VegetationMist3D::get_weather_map);
	ClassDB::bind_method(D_METHOD("set_sun_light", "path"), &VegetationMist3D::set_sun_light);
	ClassDB::bind_method(D_METHOD("get_sun_light"), &VegetationMist3D::get_sun_light);
	ClassDB::bind_method(D_METHOD("set_mist_amount", "amount"), &VegetationMist3D::set_mist_amount);
	ClassDB::bind_method(D_METHOD("get_mist_amount"), &VegetationMist3D::get_mist_amount);
	ClassDB::bind_method(D_METHOD("set_mist_range", "range"), &VegetationMist3D::set_mist_range);
	ClassDB::bind_method(D_METHOD("get_mist_range"), &VegetationMist3D::get_mist_range);
	ClassDB::bind_method(D_METHOD("set_mist_cell", "size"), &VegetationMist3D::set_mist_cell);
	ClassDB::bind_method(D_METHOD("get_mist_cell"), &VegetationMist3D::get_mist_cell);
	ClassDB::bind_method(D_METHOD("set_mist_depth", "depth"), &VegetationMist3D::set_mist_depth);
	ClassDB::bind_method(D_METHOD("get_mist_depth"), &VegetationMist3D::get_mist_depth);
	ClassDB::bind_method(D_METHOD("set_extinction", "extinction"), &VegetationMist3D::set_extinction);
	ClassDB::bind_method(D_METHOD("get_extinction"), &VegetationMist3D::get_extinction);
	ClassDB::bind_method(D_METHOD("set_heaviness", "heaviness"), &VegetationMist3D::set_heaviness);
	ClassDB::bind_method(D_METHOD("get_heaviness"), &VegetationMist3D::get_heaviness);
	ClassDB::bind_method(D_METHOD("set_flow_cell_size", "size"), &VegetationMist3D::set_flow_cell_size);
	ClassDB::bind_method(D_METHOD("get_flow_cell_size"), &VegetationMist3D::get_flow_cell_size);
	ClassDB::bind_method(D_METHOD("set_mist_color", "color"), &VegetationMist3D::set_mist_color);
	ClassDB::bind_method(D_METHOD("get_mist_color"), &VegetationMist3D::get_mist_color);
	ClassDB::bind_method(D_METHOD("get_flow"), &VegetationMist3D::get_flow);
	ClassDB::bind_method(D_METHOD("is_flow_available"), &VegetationMist3D::is_flow_available);
	ClassDB::bind_method(D_METHOD("get_mist_level"), &VegetationMist3D::get_mist_level);
	ClassDB::bind_method(D_METHOD("weather_mist_level"), &VegetationMist3D::weather_mist_level);
	ClassDB::bind_method(D_METHOD("get_propensity_at", "position"), &VegetationMist3D::get_propensity_at);
	ClassDB::bind_method(D_METHOD("get_active_cells"), &VegetationMist3D::get_active_cells);

	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enabled"), "set_enabled", "is_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "follow_target", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D"), "set_follow_target", "get_follow_target");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "terrain"), "set_terrain", "get_terrain");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "weather_map", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "WeatherMap3D"), "set_weather_map", "get_weather_map");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "sun_light", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "DirectionalLight3D"), "set_sun_light", "get_sun_light");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "mist_amount", PROPERTY_HINT_RANGE, "-1,2,0.01"), "set_mist_amount", "get_mist_amount");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "mist_range", PROPERTY_HINT_RANGE, "2,200,0.5,suffix:m"), "set_mist_range", "get_mist_range");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "mist_cell", PROPERTY_HINT_RANGE, "1,64,0.5,suffix:m"), "set_mist_cell", "get_mist_cell");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "mist_depth", PROPERTY_HINT_RANGE, "0.1,20,0.05,suffix:m"), "set_mist_depth", "get_mist_depth");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "extinction", PROPERTY_HINT_RANGE, "0,1,0.001,suffix:1/m"), "set_extinction", "get_extinction");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "heaviness", PROPERTY_HINT_RANGE, "0,2,0.01"), "set_heaviness", "get_heaviness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "flow_cell_size", PROPERTY_HINT_RANGE, "0.02,2,0.01,suffix:m"), "set_flow_cell_size", "get_flow_cell_size");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "mist_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_mist_color", "get_mist_color");
}

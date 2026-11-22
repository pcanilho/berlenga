/**************************************************************************/
/*  physx_haboob_front_3d.cpp                                               */
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

#include "physx_haboob_front_3d.h"

#include "physx_flow_3d.h"
#include "physx_flow_emitter_3d.h"

#include "core/object/class_db.h"
#include "modules/modules_enabled.gen.h" // MODULE_WEATHER_ENABLED
#include "modules/world/world_contracts.h"
#include "scene/3d/camera_3d.h"
#include "scene/main/viewport.h"

#ifdef MODULE_WEATHER_ENABLED
#include "modules/weather/weather_map_3d.h"
#endif

Node3D *PhysXHaboobFront3D::_target() const {
	if (!follow_target.is_empty()) {
		if (Node3D *n = Object::cast_to<Node3D>(get_node_or_null(follow_target))) {
			return n;
		}
	}
	return is_inside_tree() && get_viewport() ? get_viewport()->get_camera_3d() : nullptr;
}

void PhysXHaboobFront3D::set_terrain_path(const NodePath &p_path) {
	terrain_path = p_path;
	if (flow && is_inside_tree()) {
		Node *n = get_node_or_null(terrain_path);
		flow->set_terrain_path(n ? flow->get_path_to(n) : NodePath());
	}
}

void PhysXHaboobFront3D::set_dust_color(const Color &p_c) {
	dust_color = p_c;
	if (flow) {
		flow->set_smoke_color(dust_color);
	}
}

void PhysXHaboobFront3D::_build() {
	if (flow) {
		return;
	}
	flow = memnew(PhysXFlow3D);
	flow->set_name("Flow");
	// A cold current's dust: no heat (the outflow is denser than the air it undercuts; the emitters' own push lifts
	// the head), the dust riding the air, hanging for minutes, coarse cells over hundreds of metres, strong vorticity
	// confinement for the billows, the weather's wind carrying what is left behind.
	flow->set_cell_size(flow_cell_size);
	flow->set_max_blocks(512);
	flow->set_combustion(false);
	flow->set_buoyancy(0.0f);
	flow->set_smoke_buoyancy(0.0f);
	flow->set_smoke_fade(0.02f);
	flow->set_vorticity(2.0f);
	flow->set_emission_strength(0.0f);
	flow->set_temperature_range(1.0f);
	flow->set_fire_light(false);
	flow->set_embers(false);
	flow->set_heat_shimmer_kelvin(0.0f);
	flow->set_smoke_density(0.15f);
	flow->set_scattering_strength(2.0f);
	flow->set_smoke_color(dust_color);
	flow->set_terrain_floor(true);
	flow->set_weather_wind_coupling(0.5f);
	add_child(flow, false, INTERNAL_MODE_FRONT);
	flow->set_as_top_level(true);
	flow->set_global_transform(Transform3D());
	if (!terrain_path.is_empty()) {
		Node *n = get_node_or_null(terrain_path);
		flow->set_terrain_path(n ? flow->get_path_to(n) : NodePath());
	}
	for (int i = 0; i < MAX_EMITTERS; i++) {
		PhysXFlowEmitter3D *e = memnew(PhysXFlowEmitter3D);
		e->set_regional_smoke(false); // its smoke is reported by its owner, or is not smoke
		e->set_name(vformat("Foot%d", i));
		e->set_shape(PhysXFlowEmitter3D::SHAPE_BOX);
		e->set_smoke(0.0f);
		e->set_temperature(0.0f);
		e->set_fuel(0.0f);
		e->set_enabled(false);
		flow->add_child(e);
		e->set_flow_path(e->get_path_to(flow));
		emitters[i] = e;
	}
}

// Where the dust front runs near the target: the nearest point of the map's head share (WeatherMap3D
// get_dust_head_at) within near_range, the front's line across its travel there, and emitter_count points along it,
// each slid along the travel onto the head's crest.
void PhysXHaboobFront3D::_scan() {
	front.clear();
#ifdef MODULE_WEATHER_ENABLED
	const WeatherMap3D *wm = WeatherMap3D::get_active();
	Node3D *t = _target();
	if (!wm || !t || !enabled) {
		return;
	}
	const Vector3 c = t->get_global_position();
	const float s = MAX(near_range / 12.0f, 10.0f);
	Vector3 anchor;
	float best = 1e30f;
	for (float z = -near_range; z <= near_range; z += s) {
		for (float x = -near_range; x <= near_range; x += s) {
			if (x * x + z * z > near_range * near_range) {
				continue;
			}
			const Vector3 p(c.x + x, c.y, c.z + z);
			if (wm->get_dust_head_at(p) > 0.4f && wm->get_dust_at(p) > 0.5f && x * x + z * z < best) {
				best = x * x + z * z;
				anchor = p;
			}
		}
	}
	if (best >= 1e30f) {
		return;
	}
	const Vector3 wv = wm->get_wind_at(anchor, 10.0f);
	Vector2 fwd(wv.x, wv.z);
	const float speed = MIN(fwd.length(), 30.0f);
	if (speed < 1.0f) {
		return;
	}
	fwd /= fwd.length();
	const Vector2 tan(-fwd.y, fwd.x);
	const Vector2 a2(anchor.x, anchor.z);
	const Vector2 mid = a2 + tan * tan.dot(Vector2(c.x, c.z) - a2);
	const float span = 1.6f * near_range;
	const WorldGround *g = WorldContracts::ground();
	const WorldWeather *ww = WorldContracts::weather();
	for (int i = 0; i < emitter_count; i++) {
		const Vector2 q0 = mid + tan * span * ((i + 0.5f) / emitter_count - 0.5f);
		float hb = 0.0f;
		Vector2 q = q0;
		for (int k = -6; k <= 6; k++) {
			const Vector2 qq = q0 + fwd * (k * 0.5f * s);
			const float h = wm->get_dust_head_at(Vector3(qq.x, c.y, qq.y));
			if (h > hb) {
				hb = h;
				q = qq;
			}
		}
		if (hb < 0.3f) {
			continue;
		}
		Vector3 p(q.x, c.y, q.y);
		if (g && g->has_provider()) {
			const float h = g->height_at(p);
			p.y = Math::is_finite(h) ? h : p.y;
		} else if (ww && ww->has_provider()) {
			p.y = ww->ground_height_at(p);
		}
		FrontPoint fp;
		fp.position = p;
		fp.forward = fwd;
		fp.speed = speed;
		fp.load = wm->get_dust_at(p);
		front.push_back(fp);
	}
#endif
}

// Each emitter a box on the front's foot, as wide as its share of the row, pushing forward at the front's speed and
// up over the nose (the head's updraft about half the front's speed: Simpson 1997's laboratory heads), lifting dust
// in proportion to the column behind it.
void PhysXHaboobFront3D::_draw() {
	if (!flow) {
		return;
	}
	const float width = 1.6f * near_range / MAX(emitter_count, 1);
	for (int i = 0; i < MAX_EMITTERS; i++) {
		PhysXFlowEmitter3D *e = emitters[i];
		if (!e) {
			continue;
		}
		const bool on = i < int(front.size());
		e->set_enabled(on);
		if (!on) {
			continue;
		}
		const FrontPoint &f = front[i];
		const float h = 30.0f;
		const Vector3 fwd(f.forward.x, 0.0f, f.forward.y);
		Basis b;
		b.set_column(0, Vector3(-fwd.z, 0.0f, fwd.x));
		b.set_column(1, Vector3(0.0f, 1.0f, 0.0f));
		b.set_column(2, fwd);
		e->set_global_transform(Transform3D(b, f.position + Vector3(0.0f, 0.5f * h, 0.0f)));
		e->set_size(Vector3(width * 1.1f, h, 20.0f));
		e->set_velocity(fwd * (1.1f * f.speed) + Vector3(0.0f, 0.5f * f.speed, 0.0f));
		e->set_smoke(CLAMP(f.load / 4.0f, 0.0f, 1.5f));
		e->set_couple_rate(1.5f);
	}
}

Vector3 PhysXHaboobFront3D::get_front_point(int p_index) const {
	return p_index >= 0 && p_index < int(front.size()) ? front[p_index].position : Vector3();
}

Vector3 PhysXHaboobFront3D::get_front_velocity(int p_index) const {
	if (p_index < 0 || p_index >= int(front.size())) {
		return Vector3();
	}
	const FrontPoint &f = front[p_index];
	return Vector3(f.forward.x, 0.0f, f.forward.y) * f.speed;
}

void PhysXHaboobFront3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			_build();
			set_physics_process_internal(true);
		} break;
		case NOTIFICATION_INTERNAL_PHYSICS_PROCESS: {
			const float dt = float(get_physics_process_delta_time());
			scan_clock -= dt;
			if (scan_clock <= 0.0f) {
				scan_clock = scan_interval;
				_scan();
			} else {
				// Between scans the row rides the front.
				for (FrontPoint &f : front) {
					f.position += Vector3(f.forward.x, 0.0f, f.forward.y) * (f.speed * dt);
				}
			}
			_draw();
		} break;
	}
}

void PhysXHaboobFront3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_enabled", "enabled"), &PhysXHaboobFront3D::set_enabled);
	ClassDB::bind_method(D_METHOD("is_enabled"), &PhysXHaboobFront3D::is_enabled);
	ClassDB::bind_method(D_METHOD("set_follow_target", "path"), &PhysXHaboobFront3D::set_follow_target);
	ClassDB::bind_method(D_METHOD("get_follow_target"), &PhysXHaboobFront3D::get_follow_target);
	ClassDB::bind_method(D_METHOD("set_terrain_path", "path"), &PhysXHaboobFront3D::set_terrain_path);
	ClassDB::bind_method(D_METHOD("get_terrain_path"), &PhysXHaboobFront3D::get_terrain_path);
	ClassDB::bind_method(D_METHOD("set_near_range", "range"), &PhysXHaboobFront3D::set_near_range);
	ClassDB::bind_method(D_METHOD("get_near_range"), &PhysXHaboobFront3D::get_near_range);
	ClassDB::bind_method(D_METHOD("set_emitter_count", "count"), &PhysXHaboobFront3D::set_emitter_count);
	ClassDB::bind_method(D_METHOD("get_emitter_count"), &PhysXHaboobFront3D::get_emitter_count);
	ClassDB::bind_method(D_METHOD("set_flow_cell_size", "size"), &PhysXHaboobFront3D::set_flow_cell_size);
	ClassDB::bind_method(D_METHOD("get_flow_cell_size"), &PhysXHaboobFront3D::get_flow_cell_size);
	ClassDB::bind_method(D_METHOD("set_dust_color", "color"), &PhysXHaboobFront3D::set_dust_color);
	ClassDB::bind_method(D_METHOD("get_dust_color"), &PhysXHaboobFront3D::get_dust_color);
	ClassDB::bind_method(D_METHOD("scan_now"), &PhysXHaboobFront3D::scan_now);
	ClassDB::bind_method(D_METHOD("get_front_point_count"), &PhysXHaboobFront3D::get_front_point_count);
	ClassDB::bind_method(D_METHOD("get_front_point", "index"), &PhysXHaboobFront3D::get_front_point);
	ClassDB::bind_method(D_METHOD("get_front_velocity", "index"), &PhysXHaboobFront3D::get_front_velocity);
	ClassDB::bind_method(D_METHOD("get_flow"), &PhysXHaboobFront3D::get_flow);

	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enabled"), "set_enabled", "is_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "follow_target", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D"), "set_follow_target", "get_follow_target");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "terrain_path"), "set_terrain_path", "get_terrain_path");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "near_range", PROPERTY_HINT_RANGE, "50,2000,1,suffix:m"), "set_near_range", "get_near_range");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "emitter_count", PROPERTY_HINT_RANGE, "1,16,1"), "set_emitter_count", "get_emitter_count");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "flow_cell_size", PROPERTY_HINT_RANGE, "0.25,8,0.05,suffix:m"), "set_flow_cell_size", "get_flow_cell_size");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "dust_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_dust_color", "get_dust_color");
}

/**************************************************************************/
/*  physx_cave_mist_3d.cpp                                                */
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

#include "physx_cave_mist_3d.h"

#ifdef MODULE_ROCKS_ENABLED

#include "physx_flow_3d.h"
#include "physx_flow_emitter_3d.h"

#include "core/object/class_db.h"
#include "scene/3d/camera_3d.h"
#include "scene/main/viewport.h"

#include "modules/rocks/cave_network_3d.h"
#include "modules/terrain_deform/terrain_generator_3d.h"
#include "modules/world/world_contracts.h"

CaveNetwork3D *PhysXCaveMist3D::_network() const {
	if (!cave_network.is_empty()) {
		return Object::cast_to<CaveNetwork3D>(get_node_or_null(cave_network));
	}
	return Object::cast_to<CaveNetwork3D>(get_parent());
}

Node3D *PhysXCaveMist3D::_target() const {
	if (!follow_target.is_empty()) {
		return Object::cast_to<Node3D>(get_node_or_null(follow_target));
	}
	return is_inside_tree() && get_viewport() ? get_viewport()->get_camera_3d() : nullptr;
}

float PhysXCaveMist3D::_period() const {
	if (swell_period > 0.0f) {
		return swell_period;
	}
	const CaveNetwork3D *net = _network();
	const TerrainGenerator3D *gen = net ? Object::cast_to<TerrainGenerator3D>(net->get_node_or_null(net->get_generator())) : nullptr;
	if (Node *sea = gen ? gen->get_sea_node() : nullptr) {
		const Variant p = sea->get("shore_wave_period");
		if (p.get_type() == Variant::FLOAT && float(p) > 1.0f) {
			return float(p);
		}
	}
	return 10.0f;
}

void PhysXCaveMist3D::_build() {
	if (flow) {
		return;
	}
	flow = memnew(PhysXFlow3D);
	flow->set_name("Flow");
	flow->set_cell_size(0.25f);
	flow->set_max_blocks(1024);
	flow->set_combustion(false);
	flow->set_buoyancy(0.0f);
	flow->set_smoke_buoyancy(-heaviness);
	flow->set_smoke_fade(evaporation);
	flow->set_vorticity(3.0f);
	flow->set_emission_strength(0.0f);
	flow->set_smoke_density(0.35f);
	flow->set_scattering_strength(2.0f);
	flow->set_smoke_color(mist_color);
	flow->set_collide_with_bodies(true);
	flow->set_collision_range(12.0f);
	add_child(flow, false, INTERNAL_MODE_FRONT);
	flow->set_as_top_level(true);
	flow->set_global_transform(Transform3D());
	set_max_mouths(max_mouths);
}

void PhysXCaveMist3D::set_max_mouths(int p_v) {
	max_mouths = CLAMP(p_v, 1, 16);
	if (!flow) {
		return;
	}
	while ((int)mouths.size() > max_mouths) {
		mouths[mouths.size() - 1].emitter->queue_free();
		mouths.resize(mouths.size() - 1);
	}
	while ((int)mouths.size() < max_mouths) {
		Mouth m;
		m.emitter = memnew(PhysXFlowEmitter3D);
		m.emitter->set_regional_smoke(false); // its smoke is reported by its owner, or is not smoke
		m.emitter->set_name(vformat("Mouth%d", (int)mouths.size()));
		m.emitter->set_enabled(false);
		flow->add_child(m.emitter);
		m.emitter->set_flow_path(m.emitter->get_path_to(flow));
		mouths.push_back(m);
	}
}

void PhysXCaveMist3D::set_heaviness(float p_v) {
	heaviness = MAX(p_v, 0.0f);
	if (flow) {
		flow->set_smoke_buoyancy(-heaviness);
	}
}

void PhysXCaveMist3D::set_evaporation(float p_v) {
	evaporation = MAX(p_v, 0.0f);
	if (flow) {
		flow->set_smoke_fade(evaporation);
	}
}

void PhysXCaveMist3D::set_mist_color(const Color &p_c) {
	mist_color = p_c;
	if (flow) {
		flow->set_smoke_color(p_c);
	}
}

int PhysXCaveMist3D::get_active_mouths() const {
	int n = 0;
	for (const Mouth &m : mouths) {
		n += m.cave >= 0 ? 1 : 0;
	}
	return n;
}

void PhysXCaveMist3D::_step(float p_delta) {
	CaveNetwork3D *net = _network();
	for (Mouth &m : mouths) {
		m.emitter->set_enabled(false);
		m.cave = -1;
	}
	if (!net || !flow || !enabled) {
		return;
	}
	const TerrainGenerator3D *gen = Object::cast_to<TerrainGenerator3D>(net->get_node_or_null(net->get_generator()));
	if (!gen || !gen->has_sea_result()) {
		return;
	}
	if (flow->get_terrain_path().is_empty()) {
		flow->set_terrain_path(flow->get_path_to(gen));
		flow->set_terrain_floor(true);
	}
	clock += p_delta;
	const float period = _period();
	const float sea = float(gen->get_sea_level());
	Node3D *t = _target();
	const Vector3 c = t ? t->get_global_position() : get_global_position();
	// The nearest sea caves within range whose mouth floor is under the sea (the swell runs in).
	struct Near {
		int cave;
		float d;
		bool operator<(const Near &p_o) const { return d < p_o.d; }
	};
	LocalVector<Near> near;
	for (int i = 0; i < net->get_cave_count(); i++) {
		const CaveNetwork3D::Cave *cv = net->get_cave(i);
		if (!cv || !cv->sea || cv->entrance.y > sea + 0.3f) {
			continue; // the swell only breathes through a mouth whose floor it reaches
		}
		const float d = cv->entrance.distance_to(c);
		if (d < near_range) {
			near.push_back({ i, d });
		}
	}
	near.sort();
	const WorldWeather *wm = WorldContracts::weather();
	for (uint32_t k = 0; k < near.size() && k < mouths.size(); k++) {
		Mouth &m = mouths[k];
		const CaveNetwork3D::Cave *cv = net->get_cave(near[k].cave);
		m.cave = near[k].cave;
		const Vector3 out = Vector3(cv->out.x, 0.0f, cv->out.z).normalized();
		// Each cave on its own phase of the swell (the wave reaches the mouths at different times).
		const float ph = float(Math::fmod(clock / period + 0.37 * near[k].cave, 1.0));
		const float s = Math::sin(float(Math::TAU) * ph);
		const float surge = s > 0.0f ? s * s : 0.0f;
		const float back = s < 0.0f ? -s * 0.25f : 0.0f; // the trough draws a little air back in
		const Vector3 at = Vector3(cv->entrance.x, MAX(sea, cv->entrance.y) + 0.6f, cv->entrance.z) - out * 2.0f;
		Vector3 gust;
		for (int g = 0; g < 3; g++) {
			gust_seed = gust_seed * 1664525u + 1013904223u;
			gust[g] = ((gust_seed >> 8) * (1.0f / 16777216.0f) * 2.0f - 1.0f) * turbulence;
		}
		Vector3 v = out * (push_speed * (0.15f + surge) - push_speed * back) + gust;
		v.y = MIN(v.y, 0.0f); // laden air spills out level and sinks; it never jets up
		v += wm->wind_at(at, 2.0f) * 0.2f;
		m.emitter->set_enabled(true);
		// A slab across the mouth's lower part (the air leaves over the water, under the arch), world aligned.
		const float half_w = cv->segments.is_empty() ? 2.0f : CLAMP(float(cv->segments[0].ra) * 0.7f, 1.0f, 6.0f);
		const float depth = 0.8f;
		m.emitter->set_shape(PhysXFlowEmitter3D::SHAPE_BOX);
		m.emitter->set_size(Vector3(Math::abs(out.z) * 2.0f * half_w + Math::abs(out.x) * depth, 0.8f, Math::abs(out.x) * 2.0f * half_w + Math::abs(out.z) * depth));
		m.emitter->set_global_position(at);
		m.emitter->set_velocity(v);
		m.emitter->set_divergence(1.5f * surge);
		m.emitter->set_smoke(mist_rate * (0.15f + surge));
		m.emitter->set_temperature(0.0f);
		m.emitter->set_couple_rate(2.0f);
		m.phase = ph;
	}
}

void PhysXCaveMist3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			_build();
			set_physics_process_internal(true);
		} break;
		case NOTIFICATION_INTERNAL_PHYSICS_PROCESS: {
			_step(float(get_physics_process_delta_time()));
		} break;
	}
}

void PhysXCaveMist3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_cave_network", "path"), &PhysXCaveMist3D::set_cave_network);
	ClassDB::bind_method(D_METHOD("get_cave_network"), &PhysXCaveMist3D::get_cave_network);
	ClassDB::bind_method(D_METHOD("set_follow_target", "path"), &PhysXCaveMist3D::set_follow_target);
	ClassDB::bind_method(D_METHOD("get_follow_target"), &PhysXCaveMist3D::get_follow_target);
	ClassDB::bind_method(D_METHOD("set_enabled", "enabled"), &PhysXCaveMist3D::set_enabled);
	ClassDB::bind_method(D_METHOD("is_enabled"), &PhysXCaveMist3D::is_enabled);
	ClassDB::bind_method(D_METHOD("set_near_range", "metres"), &PhysXCaveMist3D::set_near_range);
	ClassDB::bind_method(D_METHOD("get_near_range"), &PhysXCaveMist3D::get_near_range);
	ClassDB::bind_method(D_METHOD("set_max_mouths", "count"), &PhysXCaveMist3D::set_max_mouths);
	ClassDB::bind_method(D_METHOD("get_max_mouths"), &PhysXCaveMist3D::get_max_mouths);
	ClassDB::bind_method(D_METHOD("set_swell_period", "seconds"), &PhysXCaveMist3D::set_swell_period);
	ClassDB::bind_method(D_METHOD("get_swell_period"), &PhysXCaveMist3D::get_swell_period);
	ClassDB::bind_method(D_METHOD("set_mist_rate", "rate"), &PhysXCaveMist3D::set_mist_rate);
	ClassDB::bind_method(D_METHOD("get_mist_rate"), &PhysXCaveMist3D::get_mist_rate);
	ClassDB::bind_method(D_METHOD("set_push_speed", "speed"), &PhysXCaveMist3D::set_push_speed);
	ClassDB::bind_method(D_METHOD("get_push_speed"), &PhysXCaveMist3D::get_push_speed);
	ClassDB::bind_method(D_METHOD("set_heaviness", "heaviness"), &PhysXCaveMist3D::set_heaviness);
	ClassDB::bind_method(D_METHOD("get_heaviness"), &PhysXCaveMist3D::get_heaviness);
	ClassDB::bind_method(D_METHOD("set_evaporation", "rate"), &PhysXCaveMist3D::set_evaporation);
	ClassDB::bind_method(D_METHOD("get_evaporation"), &PhysXCaveMist3D::get_evaporation);
	ClassDB::bind_method(D_METHOD("set_turbulence", "speed"), &PhysXCaveMist3D::set_turbulence);
	ClassDB::bind_method(D_METHOD("get_turbulence"), &PhysXCaveMist3D::get_turbulence);
	ClassDB::bind_method(D_METHOD("set_mist_color", "color"), &PhysXCaveMist3D::set_mist_color);
	ClassDB::bind_method(D_METHOD("get_mist_color"), &PhysXCaveMist3D::get_mist_color);
	ClassDB::bind_method(D_METHOD("get_flow"), &PhysXCaveMist3D::get_flow);
	ClassDB::bind_method(D_METHOD("get_active_mouths"), &PhysXCaveMist3D::get_active_mouths);

	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "cave_network", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "CaveNetwork3D"), "set_cave_network", "get_cave_network");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "follow_target", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D"), "set_follow_target", "get_follow_target");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enabled"), "set_enabled", "is_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "near_range", PROPERTY_HINT_RANGE, "0,1000,1,suffix:m"), "set_near_range", "get_near_range");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_mouths", PROPERTY_HINT_RANGE, "1,16,1"), "set_max_mouths", "get_max_mouths");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "swell_period", PROPERTY_HINT_RANGE, "0,30,0.1,suffix:s"), "set_swell_period", "get_swell_period");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "mist_rate", PROPERTY_HINT_RANGE, "0,20,0.01"), "set_mist_rate", "get_mist_rate");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "push_speed", PROPERTY_HINT_RANGE, "0,10,0.01,suffix:m/s"), "set_push_speed", "get_push_speed");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "heaviness", PROPERTY_HINT_RANGE, "0,2,0.01"), "set_heaviness", "get_heaviness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "evaporation", PROPERTY_HINT_RANGE, "0,2,0.01"), "set_evaporation", "get_evaporation");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "turbulence", PROPERTY_HINT_RANGE, "0,5,0.01,suffix:m/s"), "set_turbulence", "get_turbulence");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "mist_color"), "set_mist_color", "get_mist_color");
}

#endif // MODULE_ROCKS_ENABLED

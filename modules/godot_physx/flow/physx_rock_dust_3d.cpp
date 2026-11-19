/**************************************************************************/
/*  physx_rock_dust_3d.cpp                                                */
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

#include "physx_rock_dust_3d.h"

#ifdef MODULE_ROCKS_ENABLED

#include "physx_flow_3d.h"
#include "physx_flow_emitter_3d.h"

#include "core/object/class_db.h"
#include "scene/3d/camera_3d.h"
#include "scene/main/viewport.h"

#include "modules/rocks/rock_field_3d.h"
#include "modules/world/world_contracts.h"

RockField3D *PhysXRockDust3D::_field() const {
	if (!rock_field.is_empty()) {
		return Object::cast_to<RockField3D>(get_node_or_null(rock_field));
	}
	return Object::cast_to<RockField3D>(get_parent());
}

Node3D *PhysXRockDust3D::_target() const {
	if (!follow_target.is_empty()) {
		return Object::cast_to<Node3D>(get_node_or_null(follow_target));
	}
	return is_inside_tree() && get_viewport() ? get_viewport()->get_camera_3d() : nullptr;
}

void PhysXRockDust3D::_build() {
	if (flow) {
		return;
	}
	flow = memnew(PhysXFlow3D);
	flow->set_name("Flow");
	// Rock dust: no fire or heat, heavier than the air, a slow fade (the fines hang for tens of seconds), cells of
	// 25 cm (a cloud is metres across), the generator's ground as the floor, the falling pieces as solids.
	flow->set_cell_size(0.25f);
	flow->set_max_blocks(1024);
	flow->set_combustion(false);
	flow->set_buoyancy(0.0f);
	flow->set_smoke_buoyancy(-dust_heaviness);
	flow->set_smoke_fade(0.08f);
	flow->set_vorticity(5.0f);
	flow->set_emission_strength(0.0f);
	// Pale and translucent: mineral dust scatters most of the light it catches (single-scattering albedo above 0.9),
	// so it reads light grey in the sun and soft at its edges, not as a dark smoke.
	flow->set_smoke_density(1.5f);
	flow->set_scattering_strength(3.0f);
	flow->set_smoke_color(dust_color);
	flow->set_collide_with_bodies(true);
	flow->set_collision_range(10.0f);
	flow->set_terrain_floor(true);
	add_child(flow, false, INTERNAL_MODE_FRONT);
	flow->set_as_top_level(true);
	flow->set_global_transform(Transform3D());

	air = memnew(PhysXFlowEmitter3D);
	air->set_name("Wind");
	air->set_shape(PhysXFlowEmitter3D::SHAPE_BOX);
	air->set_size(Vector3(30, 8, 30));
	air->set_smoke(0.0f);
	air->set_temperature(0.0f);
	air->set_couple_rate(wind_coupling);
	air->set_enabled(false);
	flow->add_child(air);
	air->set_flow_path(air->get_path_to(flow));

	set_max_sources(max_sources);
}

void PhysXRockDust3D::set_max_sources(int p_v) {
	max_sources = CLAMP(p_v, 1, 32);
	if (!flow) {
		return;
	}
	while ((int)sources.size() > max_sources) {
		sources[sources.size() - 1].emitter->queue_free();
		sources.resize(sources.size() - 1);
	}
	while ((int)sources.size() < max_sources) {
		Source s;
		s.emitter = memnew(PhysXFlowEmitter3D);
		s.emitter->set_name(vformat("Dust%d", (int)sources.size()));
		s.emitter->set_enabled(false);
		flow->add_child(s.emitter);
		s.emitter->set_flow_path(s.emitter->get_path_to(flow));
		sources.push_back(s);
	}
}

void PhysXRockDust3D::set_dust_heaviness(float p_v) {
	dust_heaviness = MAX(p_v, 0.0f);
	if (flow) {
		flow->set_smoke_buoyancy(-dust_heaviness);
	}
}

void PhysXRockDust3D::set_dust_color(const Color &p_c) {
	dust_color = p_c;
	if (flow) {
		flow->set_smoke_color(p_c);
	}
}

int PhysXRockDust3D::get_active_sources() const {
	int n = 0;
	for (const Source &s : sources) {
		n += s.left > 0.0f ? 1 : 0;
	}
	return n;
}

void PhysXRockDust3D::_step(float p_delta) {
	RockField3D *field = _field();
	if (!field || !flow || !enabled) {
		for (Source &s : sources) {
			s.left = 0.0f;
			s.emitter->set_enabled(false);
		}
		if (air) {
			air->set_enabled(false);
		}
		return;
	}
	// The floor: the field's generator (its heights), found once it exists.
	if (flow->get_terrain_path().is_empty()) {
		if (Node *gen = field->get_node_or_null(field->get_generator())) {
			flow->set_terrain_path(flow->get_path_to(gen));
		}
	}
	Node3D *t = _target();
	const Vector3 c = t ? t->get_global_position() : get_global_position();
	LocalVector<RockField3D::Dust> events;
	field->take_rockfall_dust(events);
	// Each event joins a burst still running within a couple of metres of it (a piece's run of hits), else a free
	// source, else the weakest; mass-weighted position, jet and size.
	// Rockfall fines settling on snow darken it (dirty snow, WorldSnow.add_impurity): most of a rockfall's dust is
	// coarse and lands within metres, carried a little downwind; mineral dust counts 0.013 of its mass as black carbon.
	WorldSnow *snow = WorldContracts::snow();
	const WorldWeather *weather = WorldContracts::weather();
	for (const RockField3D::Dust &d : events) {
		if (snow && snow->has_provider() && d.mass > 0.0f) {
			const Vector3 w = weather && weather->has_provider() ? weather->wind_at(d.position, 1.0f) : Vector3();
			const float r = MAX(d.radius * 4.0f, 3.0f);
			snow->add_impurity(d.position + Vector3(w.x, 0.0f, w.z) * 2.0f, r, d.mass * 0.7f * 1.0e6f * 0.013f / (float(Math::PI) * r * r));
		}
		if (d.position.distance_to(c) > near_range) {
			continue;
		}
		total += d.mass;
		const float rate = d.mass / burst_time;
		int best = -1;
		float best_d = 1e30f;
		for (uint32_t i = 0; i < sources.size(); i++) {
			const Source &s = sources[i];
			if (s.left <= 0.0f) {
				continue;
			}
			const float dd = s.position.distance_to(d.position);
			if (dd < s.radius + d.radius + 2.0f && dd < best_d) {
				best_d = dd;
				best = i;
			}
		}
		if (best < 0) {
			float weakest = 1e30f;
			for (uint32_t i = 0; i < sources.size(); i++) {
				const float w = sources[i].left > 0.0f ? sources[i].rate : -1.0f;
				if (w < weakest) {
					weakest = w;
					best = i;
				}
			}
			if (best < 0) {
				continue;
			}
			Source &s = sources[best];
			s.position = d.position;
			s.velocity = d.velocity;
			s.rate = rate;
			s.radius = d.radius;
			s.left = burst_time;
			continue;
		}
		Source &s = sources[best];
		const float w = rate / (s.rate + rate);
		s.position = s.position.lerp(d.position, w);
		s.velocity = s.velocity.lerp(d.velocity, w);
		s.radius = MAX(s.radius, d.radius);
		s.rate += rate;
		s.left = burst_time;
	}
	Vector3 mid;
	float weight = 0.0f;
	for (Source &s : sources) {
		if (s.left <= 0.0f) {
			s.emitter->set_enabled(false);
			continue;
		}
		s.left -= p_delta;
		// A small core at the contact, loosely coupled: it pushes dust out and Flow makes the cloud (a sphere held at full
		// density reads as a ball).
		const float r = CLAMP(s.radius * 0.6f, 0.3f, 1.2f);
		s.emitter->set_enabled(true);
		s.emitter->set_radius(r);
		s.emitter->set_global_position(s.position);
		// Out of the impact: along the piece's path (a share of its speed, kept off the ground), up a little,
		// swelling, with a hashed gust so the cloud rolls.
		Vector3 gust;
		for (int k = 0; k < 3; k++) {
			gust_seed = gust_seed * 1664525u + 1013904223u;
			gust[k] = ((gust_seed >> 8) * (1.0f / 16777216.0f) * 2.0f - 1.0f) * turbulence;
		}
		gust.y = Math::abs(gust.y);
		Vector3 jet = s.velocity * carry;
		jet.y = MAX(jet.y, 0.0f) + 2.0f;
		s.emitter->set_velocity(jet + gust);
		s.emitter->set_divergence(expansion);
		s.emitter->set_smoke(MIN(s.rate * smoke_per_kg, 20.0f));
		s.emitter->set_temperature(0.0f);
		s.emitter->set_couple_rate(4.0f);
		mid += s.position * s.rate;
		weight += s.rate;
	}
	// The weather's wind over the dust, 2 m up.
	const Vector3 at = weight > 0.0f ? mid / weight : c;
	Vector3 wind = WorldContracts::weather()->wind_at(at, 2.0f);
	if (const WorldFire *fire = WorldContracts::fire(); fire->has_provider()) {
		wind += fire->wind_at(at); // a fire nearby draws the dust in
	}
	air->set_enabled(weight > 0.0f && wind.length_squared() > 1e-4f);
	air->set_global_position(at + Vector3(0, 2.0f, 0));
	air->set_velocity(wind);
	air->set_couple_rate(wind_coupling);
}

void PhysXRockDust3D::_notification(int p_what) {
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

void PhysXRockDust3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_rock_field", "path"), &PhysXRockDust3D::set_rock_field);
	ClassDB::bind_method(D_METHOD("get_rock_field"), &PhysXRockDust3D::get_rock_field);
	ClassDB::bind_method(D_METHOD("set_follow_target", "path"), &PhysXRockDust3D::set_follow_target);
	ClassDB::bind_method(D_METHOD("get_follow_target"), &PhysXRockDust3D::get_follow_target);
	ClassDB::bind_method(D_METHOD("set_enabled", "enabled"), &PhysXRockDust3D::set_enabled);
	ClassDB::bind_method(D_METHOD("is_enabled"), &PhysXRockDust3D::is_enabled);
	ClassDB::bind_method(D_METHOD("set_near_range", "range"), &PhysXRockDust3D::set_near_range);
	ClassDB::bind_method(D_METHOD("get_near_range"), &PhysXRockDust3D::get_near_range);
	ClassDB::bind_method(D_METHOD("set_max_sources", "count"), &PhysXRockDust3D::set_max_sources);
	ClassDB::bind_method(D_METHOD("get_max_sources"), &PhysXRockDust3D::get_max_sources);
	ClassDB::bind_method(D_METHOD("set_burst_time", "seconds"), &PhysXRockDust3D::set_burst_time);
	ClassDB::bind_method(D_METHOD("get_burst_time"), &PhysXRockDust3D::get_burst_time);
	ClassDB::bind_method(D_METHOD("set_smoke_per_kg", "smoke"), &PhysXRockDust3D::set_smoke_per_kg);
	ClassDB::bind_method(D_METHOD("get_smoke_per_kg"), &PhysXRockDust3D::get_smoke_per_kg);
	ClassDB::bind_method(D_METHOD("set_dust_heaviness", "heaviness"), &PhysXRockDust3D::set_dust_heaviness);
	ClassDB::bind_method(D_METHOD("get_dust_heaviness"), &PhysXRockDust3D::get_dust_heaviness);
	ClassDB::bind_method(D_METHOD("set_expansion", "rate"), &PhysXRockDust3D::set_expansion);
	ClassDB::bind_method(D_METHOD("get_expansion"), &PhysXRockDust3D::get_expansion);
	ClassDB::bind_method(D_METHOD("set_carry", "share"), &PhysXRockDust3D::set_carry);
	ClassDB::bind_method(D_METHOD("get_carry"), &PhysXRockDust3D::get_carry);
	ClassDB::bind_method(D_METHOD("set_turbulence", "speed"), &PhysXRockDust3D::set_turbulence);
	ClassDB::bind_method(D_METHOD("get_turbulence"), &PhysXRockDust3D::get_turbulence);
	ClassDB::bind_method(D_METHOD("set_wind_coupling", "rate"), &PhysXRockDust3D::set_wind_coupling);
	ClassDB::bind_method(D_METHOD("get_wind_coupling"), &PhysXRockDust3D::get_wind_coupling);
	ClassDB::bind_method(D_METHOD("set_dust_color", "color"), &PhysXRockDust3D::set_dust_color);
	ClassDB::bind_method(D_METHOD("get_dust_color"), &PhysXRockDust3D::get_dust_color);
	ClassDB::bind_method(D_METHOD("get_flow"), &PhysXRockDust3D::get_flow);
	ClassDB::bind_method(D_METHOD("get_active_sources"), &PhysXRockDust3D::get_active_sources);
	ClassDB::bind_method(D_METHOD("get_total_dust"), &PhysXRockDust3D::get_total_dust);

	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "rock_field", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "RockField3D"), "set_rock_field", "get_rock_field");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "follow_target", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D"), "set_follow_target", "get_follow_target");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enabled"), "set_enabled", "is_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "near_range", PROPERTY_HINT_RANGE, "0,500,0.5,suffix:m"), "set_near_range", "get_near_range");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_sources", PROPERTY_HINT_RANGE, "1,32,1"), "set_max_sources", "get_max_sources");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "burst_time", PROPERTY_HINT_RANGE, "0.05,3,0.01,suffix:s"), "set_burst_time", "get_burst_time");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "smoke_per_kg", PROPERTY_HINT_RANGE, "0,20,0.01"), "set_smoke_per_kg", "get_smoke_per_kg");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "dust_heaviness", PROPERTY_HINT_RANGE, "0,10,0.1"), "set_dust_heaviness", "get_dust_heaviness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "expansion", PROPERTY_HINT_RANGE, "0,10,0.1,suffix:1/s"), "set_expansion", "get_expansion");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "carry", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_carry", "get_carry");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "turbulence", PROPERTY_HINT_RANGE, "0,10,0.1,suffix:m/s"), "set_turbulence", "get_turbulence");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wind_coupling", PROPERTY_HINT_RANGE, "0,4,0.01"), "set_wind_coupling", "get_wind_coupling");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "dust_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_dust_color", "get_dust_color");
}

#endif // MODULE_ROCKS_ENABLED

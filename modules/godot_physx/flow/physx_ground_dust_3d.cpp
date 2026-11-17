/**************************************************************************/
/*  physx_ground_dust_3d.cpp                                              */
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

#include "physx_ground_dust_3d.h"

#ifdef MODULE_GROUND_ENABLED

#include "physx_flow_3d.h"
#include "physx_flow_emitter_3d.h"

#include "core/object/class_db.h"
#include "scene/3d/camera_3d.h"
#include "scene/main/viewport.h"

#include "modules/ground/ground_surface_3d.h"

GroundSurface3D *PhysXGroundDust3D::_ground() const {
	if (!ground_surface.is_empty()) {
		return Object::cast_to<GroundSurface3D>(get_node_or_null(ground_surface));
	}
	return GroundSurface3D::get_active();
}

Node3D *PhysXGroundDust3D::_target() const {
	if (!follow_target.is_empty()) {
		return Object::cast_to<Node3D>(get_node_or_null(follow_target));
	}
	return is_inside_tree() && get_viewport() ? get_viewport()->get_camera_3d() : nullptr;
}

void PhysXGroundDust3D::_build() {
	if (flow) {
		return;
	}
	flow = memnew(PhysXFlow3D);
	flow->set_name("Flow");
	// Dust, not smoke: no fire, no heat, heavy in itself (smoke_buoyancy below 0), a slow fade (fines linger
	// for tens of seconds), cells of 15 cm, the ground surface as the floor, and the bodies near the sources
	// (the car) as solids.
	flow->set_cell_size(0.15f);
	flow->set_max_blocks(512);
	flow->set_combustion(false);
	flow->set_buoyancy(0.0f);
	flow->set_smoke_buoyancy(-dust_heaviness);
	flow->set_smoke_fade(0.12f);
	flow->set_vorticity(2.0f);
	flow->set_emission_strength(0.0f);
	flow->set_smoke_density(3.0f);
	flow->set_smoke_color(dust_color);
	flow->set_collide_with_bodies(true);
	flow->set_collision_range(6.0f);
	flow->set_terrain_floor(true);
	add_child(flow, false, INTERNAL_MODE_FRONT);
	flow->set_as_top_level(true);
	flow->set_global_transform(Transform3D());
	if (GroundSurface3D *g = _ground()) {
		flow->set_terrain_path(flow->get_path_to(g));
	}

	air = memnew(PhysXFlowEmitter3D);
	air->set_name("Wind");
	air->set_shape(PhysXFlowEmitter3D::SHAPE_BOX);
	air->set_size(Vector3(16, 4, 16));
	air->set_smoke(0.0f);
	air->set_temperature(0.0f);
	air->set_couple_rate(wind_coupling);
	air->set_enabled(false);
	flow->add_child(air);
	air->set_flow_path(air->get_path_to(flow));
	set_max_sources(max_sources);
}

void PhysXGroundDust3D::set_max_sources(int p_v) {
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
		s.emitter->set_radius(source_radius);
		s.emitter->set_enabled(false);
		flow->add_child(s.emitter);
		s.emitter->set_flow_path(s.emitter->get_path_to(flow));
		sources.push_back(s);
	}
}

void PhysXGroundDust3D::set_dust_heaviness(float p_v) {
	dust_heaviness = MAX(p_v, 0.0f);
	if (flow) {
		flow->set_smoke_buoyancy(-dust_heaviness);
	}
}

void PhysXGroundDust3D::set_dust_color(const Color &p_c) {
	dust_color = p_c;
	if (flow) {
		flow->set_smoke_color(p_c);
	}
}

int PhysXGroundDust3D::get_active_sources() const {
	int n = 0;
	for (const Source &s : sources) {
		n += s.idle == 0 ? 1 : 0;
	}
	return n;
}

void PhysXGroundDust3D::_step() {
	GroundSurface3D *g = _ground();
	Node3D *t = _target();
	if (!g || !flow || !enabled) {
		for (Source &s : sources) {
			s.emitter->set_enabled(false);
		}
		if (air) {
			air->set_enabled(false);
		}
		return;
	}
	const Vector3 c = t ? t->get_global_position() : get_global_position();
	g->claim_dust(c, near_range);
	LocalVector<GroundSurface3D::Dust> lifted;
	g->take_near_dust(lifted);

	for (Source &s : sources) {
		s.rate = 0.0f;
		s.velocity = Vector3();
		s.idle++;
	}
	// Each lifted puff joins the source nearest it (a wheel's contact moves under a metre a frame), else a free
	// one, else the nearest; rate-weighted position and jet.
	Vector3 mid;
	float total = 0.0f;
	for (const GroundSurface3D::Dust &d : lifted) {
		int best = -1;
		float best_d2 = 1e30f;
		int free_slot = -1;
		for (uint32_t i = 0; i < sources.size(); i++) {
			const Source &s = sources[i];
			if (s.idle > 1) {
				if (free_slot < 0) {
					free_slot = i;
				}
				continue;
			}
			const float d2 = s.position.distance_squared_to(d.position);
			if (d2 < best_d2) {
				best_d2 = d2;
				best = i;
			}
		}
		if (best < 0 || (best_d2 > 1.0f && free_slot >= 0)) {
			best = free_slot >= 0 ? free_slot : best;
		}
		if (best < 0) {
			continue;
		}
		Source &s = sources[best];
		if (s.rate <= 0.0f) {
			s.position = d.position;
			s.velocity = d.velocity * d.rate;
			s.rate = d.rate;
		} else {
			const float w = d.rate / (s.rate + d.rate);
			s.position = s.position.lerp(d.position, w);
			s.velocity += d.velocity * d.rate;
			s.rate += d.rate;
		}
		s.idle = 0;
		mid += d.position * d.rate;
		total += d.rate;
	}
	for (Source &s : sources) {
		if (s.idle != 0 || s.rate <= 0.0f) {
			s.emitter->set_enabled(false);
			continue;
		}
		s.emitter->set_enabled(true);
		s.emitter->set_radius(source_radius);
		// The sphere sits on the sand, its centre a little above the contact.
		s.emitter->set_global_position(s.position + Vector3(0, source_radius * 0.6f, 0));
		// A gust on the jet each step (hashed, so runs repeat): the wake's eddies roll the plume.
		Vector3 gust;
		for (int c = 0; c < 3; c++) {
			gust_seed = gust_seed * 1664525u + 1013904223u;
			gust[c] = ((gust_seed >> 8) * (1.0f / 16777216.0f) * 2.0f - 1.0f) * turbulence;
		}
		gust.y = Math::abs(gust.y);
		s.emitter->set_velocity(s.velocity / s.rate + gust);
		s.emitter->set_divergence(expansion);
		s.emitter->set_smoke(MIN(s.rate * smoke_per_kg, 4.0f));
		s.emitter->set_temperature(0.0f);
		s.emitter->set_couple_rate(30.0f); // a wheel crosses its sphere in a tenth of a second: couple hard
	}

	// The ground wind at a metre over the sand (log profile, z0 = 1 mm: u(1 m) = 0.75 u(10 m)).
	const Vector3 at = total > 0.0f ? mid / total : c;
	const float floor_y = g->get_surface_height_at(at);
	const Vector2 w10 = g->get_wind_10m();
	air->set_enabled(w10.length_squared() > 1e-4f);
	air->set_global_position(Vector3(at.x, floor_y + 2.0f, at.z));
	air->set_velocity(Vector3(w10.x, 0, w10.y) * 0.75f);
	air->set_couple_rate(wind_coupling);
}

void PhysXGroundDust3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			_build();
			set_physics_process_internal(true);
		} break;
		case NOTIFICATION_INTERNAL_PHYSICS_PROCESS: {
			_step();
		} break;
	}
}

void PhysXGroundDust3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_ground_surface", "path"), &PhysXGroundDust3D::set_ground_surface);
	ClassDB::bind_method(D_METHOD("get_ground_surface"), &PhysXGroundDust3D::get_ground_surface);
	ClassDB::bind_method(D_METHOD("set_follow_target", "path"), &PhysXGroundDust3D::set_follow_target);
	ClassDB::bind_method(D_METHOD("get_follow_target"), &PhysXGroundDust3D::get_follow_target);
	ClassDB::bind_method(D_METHOD("set_enabled", "enabled"), &PhysXGroundDust3D::set_enabled);
	ClassDB::bind_method(D_METHOD("is_enabled"), &PhysXGroundDust3D::is_enabled);
	ClassDB::bind_method(D_METHOD("set_near_range", "range"), &PhysXGroundDust3D::set_near_range);
	ClassDB::bind_method(D_METHOD("get_near_range"), &PhysXGroundDust3D::get_near_range);
	ClassDB::bind_method(D_METHOD("set_max_sources", "count"), &PhysXGroundDust3D::set_max_sources);
	ClassDB::bind_method(D_METHOD("get_max_sources"), &PhysXGroundDust3D::get_max_sources);
	ClassDB::bind_method(D_METHOD("set_source_radius", "radius"), &PhysXGroundDust3D::set_source_radius);
	ClassDB::bind_method(D_METHOD("get_source_radius"), &PhysXGroundDust3D::get_source_radius);
	ClassDB::bind_method(D_METHOD("set_smoke_per_kg", "smoke"), &PhysXGroundDust3D::set_smoke_per_kg);
	ClassDB::bind_method(D_METHOD("get_smoke_per_kg"), &PhysXGroundDust3D::get_smoke_per_kg);
	ClassDB::bind_method(D_METHOD("set_dust_heaviness", "heaviness"), &PhysXGroundDust3D::set_dust_heaviness);
	ClassDB::bind_method(D_METHOD("get_dust_heaviness"), &PhysXGroundDust3D::get_dust_heaviness);
	ClassDB::bind_method(D_METHOD("set_wind_coupling", "rate"), &PhysXGroundDust3D::set_wind_coupling);
	ClassDB::bind_method(D_METHOD("get_wind_coupling"), &PhysXGroundDust3D::get_wind_coupling);
	ClassDB::bind_method(D_METHOD("set_expansion", "rate"), &PhysXGroundDust3D::set_expansion);
	ClassDB::bind_method(D_METHOD("get_expansion"), &PhysXGroundDust3D::get_expansion);
	ClassDB::bind_method(D_METHOD("set_turbulence", "speed"), &PhysXGroundDust3D::set_turbulence);
	ClassDB::bind_method(D_METHOD("get_turbulence"), &PhysXGroundDust3D::get_turbulence);
	ClassDB::bind_method(D_METHOD("set_dust_color", "color"), &PhysXGroundDust3D::set_dust_color);
	ClassDB::bind_method(D_METHOD("get_dust_color"), &PhysXGroundDust3D::get_dust_color);
	ClassDB::bind_method(D_METHOD("get_flow"), &PhysXGroundDust3D::get_flow);
	ClassDB::bind_method(D_METHOD("get_active_sources"), &PhysXGroundDust3D::get_active_sources);

	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "ground_surface", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "GroundSurface3D"), "set_ground_surface", "get_ground_surface");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "follow_target", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D"), "set_follow_target", "get_follow_target");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enabled"), "set_enabled", "is_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "near_range", PROPERTY_HINT_RANGE, "0,200,0.5,suffix:m"), "set_near_range", "get_near_range");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_sources", PROPERTY_HINT_RANGE, "1,32,1"), "set_max_sources", "get_max_sources");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "source_radius", PROPERTY_HINT_RANGE, "0.05,2,0.01,suffix:m"), "set_source_radius", "get_source_radius");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "smoke_per_kg", PROPERTY_HINT_RANGE, "0,40,0.1"), "set_smoke_per_kg", "get_smoke_per_kg");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "dust_heaviness", PROPERTY_HINT_RANGE, "0,10,0.1"), "set_dust_heaviness", "get_dust_heaviness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wind_coupling", PROPERTY_HINT_RANGE, "0,4,0.01"), "set_wind_coupling", "get_wind_coupling");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "expansion", PROPERTY_HINT_RANGE, "0,10,0.1,suffix:1/s"), "set_expansion", "get_expansion");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "turbulence", PROPERTY_HINT_RANGE, "0,10,0.1,suffix:m/s"), "set_turbulence", "get_turbulence");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "dust_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_dust_color", "get_dust_color");
}

#endif // MODULE_GROUND_ENABLED

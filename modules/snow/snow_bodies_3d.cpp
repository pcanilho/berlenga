/**************************************************************************/
/*  snow_bodies_3d.cpp                                                    */
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

#include "snow_bodies_3d.h"

#include "snow_clumps_3d.h"
#include "snow_cover_3d.h"
#include "snow_surface_3d.h"
#include "snow_fall_3d.h"

#include "core/config/engine.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "modules/world/world_contracts.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/physics/animatable_body_3d.h"
#include "scene/3d/physics/rigid_body_3d.h"
#include "scene/3d/visual_instance_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/resources/material.h"
#include "servers/physics_3d/physics_server_3d.h"

namespace {
const float GRAVITY = 9.81f;
const float WATER_DENSITY = 1000.0f;
const float FUSION = 334000.0f; // J/kg
const float MIN_DEPTH = 0.0005f; // m: less is gone
const double RAY_INTERVAL = 1.0; // s between sky checks
const double GEOMETRY_INTERVAL = 5.0; // s between looks for new meshes under a body
} // namespace

bool SnowBodies3D::is_carrier(const Node *p_node) {
	if (!p_node || p_node->is_in_group(SNAME("snow_ignore"))) {
		return false;
	}
	if (p_node->is_in_group(SNAME("snow_catch"))) {
		return true;
	}
	// Things that move by themselves or are moved: rigid bodies (vehicles too), animated bodies, the PhysX vehicles
	// and cloth. Characters only through the group, so a person's materials stay as authored unless asked.
	return Object::cast_to<RigidBody3D>(p_node) || Object::cast_to<AnimatableBody3D>(p_node) ||
			p_node->is_class("PhysXVehicle3D") || p_node->is_class("PhysXMotorcycle3D") || p_node->is_class("PhysXCloth3D");
}

void SnowBodies3D::_consider(Node *p_node) {
	if (auto_track && is_carrier(p_node) && Object::cast_to<Node3D>(p_node)) {
		track(p_node);
	}
}

void SnowBodies3D::_scan(Node *p_node) {
	_consider(p_node);
	for (int i = 0; i < p_node->get_child_count(); i++) {
		_scan(p_node->get_child(i));
	}
}

void SnowBodies3D::_on_node_added(Node *p_node) {
	_consider(p_node);
}

void SnowBodies3D::track(Node *p_node) {
	ERR_FAIL_NULL(Object::cast_to<Node3D>(p_node));
	const ObjectID id = p_node->get_instance_id();
	if (bodies.has(id)) {
		return;
	}
	Body b;
	b.id = id;
	bodies.insert(id, b);
}

void SnowBodies3D::untrack(Node *p_node) {
	ERR_FAIL_NULL(p_node);
	HashMap<ObjectID, Body>::Iterator it = bodies.find(p_node->get_instance_id());
	if (!it) {
		return;
	}
	for (const ObjectID &gid : it->value.geometry) {
		if (GeometryInstance3D *g = ObjectDB::get_instance<GeometryInstance3D>(gid)) {
			g->set_instance_shader_parameter(SNAME("snow_body"), Vector4());
		}
	}
	bodies.remove(it);
}

bool SnowBodies3D::is_tracked(Node *p_node) const {
	return p_node && bodies.has(p_node->get_instance_id());
}

TypedArray<Node> SnowBodies3D::get_tracked() const {
	TypedArray<Node> out;
	for (const KeyValue<ObjectID, Body> &kv : bodies) {
		if (Node *n = ObjectDB::get_instance<Node>(kv.key)) {
			out.push_back(n);
		}
	}
	return out;
}

float SnowBodies3D::get_body_depth(Node *p_node) const {
	ERR_FAIL_NULL_V(p_node, 0.0f);
	const Body *b = bodies.getptr(p_node->get_instance_id());
	return b ? b->depth : 0.0f;
}

float SnowBodies3D::get_body_density(Node *p_node) const {
	ERR_FAIL_NULL_V(p_node, 0.0f);
	const Body *b = bodies.getptr(p_node->get_instance_id());
	return b ? b->density : 0.0f;
}

float SnowBodies3D::get_body_wetness(Node *p_node) const {
	ERR_FAIL_NULL_V(p_node, 0.0f);
	const Body *b = bodies.getptr(p_node->get_instance_id());
	return b ? b->wet : 0.0f;
}

void SnowBodies3D::set_body_depth(Node *p_node, float p_depth, float p_density) {
	ERR_FAIL_NULL(p_node);
	track(p_node);
	Body *b = bodies.getptr(p_node->get_instance_id());
	b->depth = CLAMP(p_depth, 0.0f, max_depth);
	if (p_density > 0.0f) {
		b->density = p_density;
	}
	b->seen = true;
	if (Node3D *n = Object::cast_to<Node3D>(p_node)) {
		b->last_position = n->get_global_position();
	}
}

void SnowBodies3D::_prepare_geometry(GeometryInstance3D *p_geometry) {
	if (!auto_materials) {
		return;
	}
	// A body's BaseMaterial3D materials learn to show its load. One that had no snow gets snow from the body only
	// (snow_from_weather off), so other instances sharing it, untracked, stay bare as before.
	auto prepare = [](const Ref<Material> &p_material) {
		Ref<BaseMaterial3D> m = p_material;
		if (m.is_null() || m->is_snow_on_body()) {
			return;
		}
		if (!m->get_feature(BaseMaterial3D::FEATURE_SNOW)) {
			m->set_feature(BaseMaterial3D::FEATURE_SNOW, true);
			m->set_snow_from_weather(false);
		}
		m->set_snow_on_body(true);
	};
	prepare(p_geometry->get_material_override());
	if (MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(p_geometry)) {
		for (int s = 0; s < mi->get_surface_override_material_count(); s++) {
			prepare(mi->get_active_material(s));
		}
	} else {
		// Cloth and other generated geometry keep their material in a "material" property.
		bool valid = false;
		const Variant v = p_geometry->get(SNAME("material"), &valid);
		if (valid) {
			prepare(v);
		}
	}
}

void SnowBodies3D::_collect_geometry(Body &r_body, Node *p_node) {
	if (GeometryInstance3D *g = Object::cast_to<GeometryInstance3D>(p_node)) {
		if (!r_body.geometry.has(g->get_instance_id())) {
			r_body.geometry.push_back(g->get_instance_id());
			_prepare_geometry(g);
			r_body.sent = Vector4(-1.0f, -1.0f, -1.0f, -1.0f);
		}
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		Node *c = p_node->get_child(i);
		// A carrier inside another keeps its own load.
		if (c != p_node && is_carrier(c) && bodies.has(c->get_instance_id())) {
			continue;
		}
		_collect_geometry(r_body, c);
	}
}

float SnowBodies3D::_exposure(Node3D *p_node) {
	// Is the sky open above the body? A ray up from its top, past itself.
	Ref<World3D> w = get_world_3d();
	if (w.is_null()) {
		return 1.0f;
	}
	PhysicsDirectSpaceState3D *space = PhysicsServer3D::get_singleton()->space_get_direct_state(w->get_space());
	if (!space) {
		return 1.0f;
	}
	Vector3 top = p_node->get_global_position();
	if (VisualInstance3D *vi = Object::cast_to<VisualInstance3D>(p_node)) {
		const AABB box = p_node->get_global_transform().xform(vi->get_aabb());
		top = Vector3(box.get_center().x, box.position.y + box.size.y, box.get_center().z);
	}
	PhysicsDirectSpaceState3D::RayParameters rp;
	rp.from = top + Vector3(0.0f, 0.05f, 0.0f);
	rp.to = rp.from + Vector3(0.0f, 60.0f, 0.0f);
	if (CollisionObject3D *co = Object::cast_to<CollisionObject3D>(p_node)) {
		rp.exclude.insert(co->get_rid());
	}
	PhysicsDirectSpaceState3D::RayResult rr;
	return space->intersect_ray(rp, rr) ? 0.0f : 1.0f;
}

bool SnowBodies3D::slab_slides(float p_depth, float p_density, float p_wetness, float p_slope, float p_cohesion) {
	// Mohr-Coulomb on the bed (as the cornice's crack, follow-up 7): the load's shear rho g d sin(slope) against the
	// bond c plus friction rho g d cos(slope) tan(phi). Melt water at the bed breaks the bond and lubricates it
	// (wet snow slides off warm roofs at about 25 degrees); phi falls from 25 to 15 degrees with wetness.
	if (p_depth <= 0.0f || p_slope <= 0.0f) {
		return false;
	}
	const float w = CLAMP(p_wetness, 0.0f, 1.0f);
	const float load = p_density * GRAVITY * p_depth; // Pa
	const float c = p_cohesion * (1.0f - w) * (1.0f - w);
	const float phi = Math::deg_to_rad(25.0f - 10.0f * w);
	return load * Math::sin(p_slope) > c + load * Math::cos(p_slope) * Math::tan(phi);
}

void SnowBodies3D::_measure(Body &r_body, Node3D *p_node) {
	// The body's footprint from above: its meshes' bounds in world space.
	AABB box;
	bool any = false;
	for (const ObjectID &gid : r_body.geometry) {
		GeometryInstance3D *g = ObjectDB::get_instance<GeometryInstance3D>(gid);
		if (!g || !g->is_inside_tree()) {
			continue;
		}
		const AABB b = g->get_global_transform().xform(g->get_aabb());
		box = any ? box.merge(b) : b;
		any = true;
	}
	if (!any) {
		r_body.top_area = 0.0f;
		r_body.top_offset = Vector3();
		return;
	}
	r_body.top_area = box.size.x * box.size.z;
	r_body.top_radius = 0.5f * MAX(box.size.x, box.size.z);
	const Vector3 c = box.get_center();
	r_body.top_offset = Vector3(c.x, box.position.y + box.size.y, c.z) - p_node->get_global_position();
}

void SnowBodies3D::_shed(Body &r_body, Node3D *p_node, float p_depth_lost, const Vector3 &p_toward, float p_temperature) {
	// What falls off lands round the body (or past its low edge, for a slab) as clumps, as much as the snow holds
	// together (SnowClumps3D::cohesion); the rest is powder.
	if (!clumps_enabled || p_depth_lost <= 0.0f) {
		return;
	}
	SnowSurface3D *surface = SnowSurface3D::get_active();
	SnowClumps3D *clumps = surface ? surface->get_clumps_node() : nullptr;
	if (!clumps) {
		return;
	}
	const float share = SnowClumps3D::cohesion(r_body.density, r_body.wet, p_temperature);
	const float volume = p_depth_lost * r_body.top_area * share;
	const Vector3 top = p_node->get_global_position() + r_body.top_offset;
	Vector3 at = top;
	float radius = r_body.top_radius + 0.3f;
	if (p_toward != Vector3()) {
		at = top + p_toward * (r_body.top_radius + 0.25f);
		radius = 0.5f * r_body.top_radius;
	}
	clumps_dropped += clumps->drop(at, radius, volume, r_body.wet);
}

void SnowBodies3D::_apply_weight() {
	// The load's weight at the top of the body, every physics step (forces do not carry over).
	for (const KeyValue<ObjectID, Body> &kv : bodies) {
		const Body &b = kv.value;
		if (b.load_mass <= 0.0f) {
			continue;
		}
		RigidBody3D *rb = ObjectDB::get_instance<RigidBody3D>(b.id);
		if (rb && rb->is_inside_tree() && !rb->is_freeze_enabled()) {
			rb->apply_force(Vector3(0.0f, -b.load_mass * GRAVITY, 0.0f), b.top_offset);
		}
	}
}

float SnowBodies3D::get_body_load_mass(Node *p_node) const {
	ERR_FAIL_NULL_V(p_node, 0.0f);
	const Body *b = bodies.getptr(p_node->get_instance_id());
	return b ? b->load_mass : 0.0f;
}

float SnowBodies3D::get_body_top_area(Node *p_node) const {
	ERR_FAIL_NULL_V(p_node, 0.0f);
	const Body *b = bodies.getptr(p_node->get_instance_id());
	return b ? b->top_area : 0.0f;
}

void SnowBodies3D::_step(double p_dt) {
	if (p_dt <= 0.0) {
		return;
	}
	const WorldWeather *weather = WorldContracts::weather();
	const WorldSnow *snow = WorldContracts::snow();
	const WorldFire *fire = WorldContracts::fire();
	const SnowCover3D *cover = SnowCover3D::get_active();
	const double scale = weather->has_provider() ? weather->time_scale() : 1.0;
	const float dt = float(p_dt);
	const float dt_h = float(p_dt * scale / 3600.0);
	const float ddf = cover ? cover->get_melt_temperature_factor() : 0.05f; // mm water / (h C)

	LocalVector<ObjectID> gone;
	for (KeyValue<ObjectID, Body> &kv : bodies) {
		Body &b = kv.value;
		Node3D *n = ObjectDB::get_instance<Node3D>(b.id);
		if (!n || !n->is_inside_tree()) {
			gone.push_back(kv.key);
			continue;
		}
		const Vector3 p = n->get_global_position();
		b.geometry_accum += p_dt;
		if (b.geometry_accum >= GEOMETRY_INTERVAL) {
			b.geometry_accum = 0.0;
			_collect_geometry(b, n);
		}
		_measure(b, n);
		b.ray_accum += p_dt;
		if (b.ray_accum >= RAY_INTERVAL) {
			b.ray_accum = 0.0;
			b.exposure = _exposure(n);
		}
		const float t = weather->has_provider() ? weather->air_temperature_at(p) : -5.0f;
		if (!b.seen) {
			// First sight: a body standing out in the snow carries what the ground round it holds.
			b.seen = true;
			b.last_position = p;
			b.last_velocity = Vector3();
			if (snow->has_provider() && b.exposure > 0.5f) {
				b.depth = CLAMP(snow->depth_at(p), 0.0f, max_depth);
				const float rho = snow->density_at(p);
				b.density = rho > 0.0f ? rho : SnowCover3D::new_snow_density(t);
				b.wet = CLAMP(snow->wetness_at(p), 0.0f, 1.0f);
			}
		}
		const Vector3 v = (p - b.last_position) / dt;
		const Vector3 a = (v - b.last_velocity) / dt;
		b.last_position = p;
		b.last_velocity = v;

		// Falling snow builds the load where the sky is open; fresh snow is light and bright.
		if (weather->has_provider() && b.exposure > 0.0f) {
			const float swe = weather->snowfall_at(p) * dt_h * b.exposure; // mm of water
			if (swe > 0.0f) {
				const float rho_new = SnowCover3D::new_snow_density(t);
				const float add = swe / 1000.0f * WATER_DENSITY / rho_new;
				b.density = (b.depth * b.density + add * rho_new) / MAX(b.depth + add, 1.0e-6f);
				b.depth += add;
				b.albedo = MIN(b.albedo + add * 30.0f, 0.88f); // a centimetre freshens it
			}
		}
		// Wind over the blowing threshold strips it (Li and Pomeroy 1997); wet and settled snow holds on.
		if (weather->has_provider() && b.depth > 0.0f) {
			const Vector3 air = weather->wind_at(p) - v;
			const float u = Vector2(air.x, air.z).length();
			const float ut = SnowFall3D::blowing_threshold(t) * (1.0f + 4.0f * b.wet) * Math::sqrt(MAX(b.density, 50.0f) / 150.0f);
			if (u > ut) {
				b.depth *= Math::exp(-wind_shed_rate * (u - ut) * dt);
			}
		}
		// Jolts and shakes throw it off: braking, bumps, a door slammed, cloth flapping.
		const float g = a.length() / GRAVITY;
		if (g > shake_threshold && b.depth > 0.0f) {
			const float before = b.depth;
			b.depth *= Math::exp(-shake_shed_rate * (g - shake_threshold) * dt);
			_shed(b, n, before - b.depth, Vector3(), t);
		}
		// A tilted top lets the load go as one slab (not cloth: it has no one top).
		if (slide_enabled && b.depth > 0.0f && !n->is_class("PhysXCloth3D")) {
			const Vector3 up = n->get_global_transform().basis.get_column(1).normalized();
			const float slope = Math::acos(CLAMP(up.y, -1.0f, 1.0f));
			if (slab_slides(b.depth, b.density, b.wet, slope, slab_cohesion)) {
				const Vector3 down = Vector3(up.x, 0.0f, up.z).normalized();
				const float lost = b.depth;
				b.depth = 0.0f;
				slabs_slid++;
				_shed(b, n, lost, down, t);
			}
		}
		// Melt: the cover's degree-day factor in a thaw, and fire's heat; melt water wets what is left, which
		// refreezes in the cold.
		float melt = 0.0f; // m of snow
		if (t > 0.0f) {
			melt += ddf * t * dt_h / 1000.0f * WATER_DENSITY / MAX(b.density, 50.0f);
		}
		if (fire->has_provider()) {
			const float heat = MAX(fire->heat_at(p), 0.0f);
			melt += heat * dt / (FUSION * MAX(b.density, 50.0f));
		}
		const float rain = weather->has_provider() ? weather->rainfall_at(p) * dt_h : 0.0f; // mm
		if (b.depth > 0.0f) {
			const float before = b.depth;
			b.depth = MAX(b.depth - melt, 0.0f);
			const float wet_in = (before - b.depth) / MAX(before, 1.0e-4f) + rain / MAX(b.depth * b.density, 1.0f);
			b.wet = CLAMP(b.wet + wet_in, 0.0f, 1.0f);
			if (t < 0.0f) {
				b.wet = MAX(b.wet - 0.1f * -t * dt_h, 0.0f);
			}
			// Settling (towards about 300 kg/m3 over two days) and ageing (albedo falls, faster when wet).
			b.density += (300.0f - b.density) * MIN(dt_h / 48.0f, 1.0f);
			b.albedo = MAX(b.albedo - dt_h * (0.004f + 0.02f * b.wet), 0.55f);
		}
		b.depth = MIN(b.depth, max_depth);
		if (b.depth < MIN_DEPTH) {
			b.depth = 0.0f;
			b.wet = 0.0f;
		}
		b.load_mass = b.depth * b.density * b.top_area;
		// Cloth carries it as areal mass (kg/m2) on its solver, where the cloth offers a snow_load.
		if (n->is_class("PhysXCloth3D")) {
			bool has = false;
			n->get(SNAME("snow_load"), &has);
			if (has) {
				n->set(SNAME("snow_load"), b.depth * b.density);
			}
		}
		// Only real changes go to the GPU.
		const Vector4 out(b.depth, b.albedo, b.wet, 1.0f + b.density / 1000.0f);
		if (Math::abs(out.x - b.sent.x) > 0.001f || Math::abs(out.y - b.sent.y) > 0.01f || Math::abs(out.z - b.sent.z) > 0.02f ||
				Math::abs(out.w - b.sent.w) > 0.01f) {
			b.sent = out;
			for (const ObjectID &gid : b.geometry) {
				if (GeometryInstance3D *gi = ObjectDB::get_instance<GeometryInstance3D>(gid)) {
					gi->set_instance_shader_parameter(SNAME("snow_body"), out);
				}
			}
		}
	}
	for (const ObjectID &id : gone) {
		bodies.erase(id);
	}
}

void SnowBodies3D::step(double p_dt) {
	_step(p_dt);
}

void SnowBodies3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			if (Engine::get_singleton()->is_editor_hint()) {
				break;
			}
			if (auto_track) {
				_scan(get_tree()->get_root());
			}
			get_tree()->connect(SNAME("node_added"), callable_mp(this, &SnowBodies3D::_on_node_added));
			set_physics_process_internal(true);
		} break;
		case NOTIFICATION_EXIT_TREE: {
			if (get_tree()->is_connected(SNAME("node_added"), callable_mp(this, &SnowBodies3D::_on_node_added))) {
				get_tree()->disconnect(SNAME("node_added"), callable_mp(this, &SnowBodies3D::_on_node_added));
			}
			set_physics_process_internal(false);
		} break;
		case NOTIFICATION_INTERNAL_PHYSICS_PROCESS: {
			if (weight_enabled) {
				_apply_weight();
			}
			accum += get_physics_process_delta_time();
			if (accum >= 1.0 / MAX(update_rate, 0.1f)) {
				_step(accum);
				accum = 0.0;
			}
		} break;
	}
}

void SnowBodies3D::set_update_rate(float p_rate) {
	update_rate = CLAMP(p_rate, 0.1f, 120.0f);
}

void SnowBodies3D::set_shake_threshold(float p_g) {
	shake_threshold = MAX(p_g, 0.0f);
}

void SnowBodies3D::set_shake_shed_rate(float p_rate) {
	shake_shed_rate = MAX(p_rate, 0.0f);
}

void SnowBodies3D::set_wind_shed_rate(float p_rate) {
	wind_shed_rate = MAX(p_rate, 0.0f);
}

void SnowBodies3D::set_max_depth(float p_depth) {
	max_depth = MAX(p_depth, 0.0f);
}

void SnowBodies3D::_bind_methods() {
	ClassDB::bind_static_method("SnowBodies3D", D_METHOD("is_carrier", "node"), &SnowBodies3D::is_carrier);
	ClassDB::bind_method(D_METHOD("track", "node"), &SnowBodies3D::track);
	ClassDB::bind_method(D_METHOD("untrack", "node"), &SnowBodies3D::untrack);
	ClassDB::bind_method(D_METHOD("is_tracked", "node"), &SnowBodies3D::is_tracked);
	ClassDB::bind_method(D_METHOD("get_tracked_count"), &SnowBodies3D::get_tracked_count);
	ClassDB::bind_method(D_METHOD("get_tracked"), &SnowBodies3D::get_tracked);
	ClassDB::bind_method(D_METHOD("get_body_depth", "node"), &SnowBodies3D::get_body_depth);
	ClassDB::bind_method(D_METHOD("get_body_density", "node"), &SnowBodies3D::get_body_density);
	ClassDB::bind_method(D_METHOD("get_body_wetness", "node"), &SnowBodies3D::get_body_wetness);
	ClassDB::bind_method(D_METHOD("set_body_depth", "node", "depth", "density"), &SnowBodies3D::set_body_depth, DEFVAL(-1.0f));
	ClassDB::bind_method(D_METHOD("step", "delta"), &SnowBodies3D::step);
	ClassDB::bind_method(D_METHOD("get_body_load_mass", "node"), &SnowBodies3D::get_body_load_mass);
	ClassDB::bind_method(D_METHOD("get_body_top_area", "node"), &SnowBodies3D::get_body_top_area);
	ClassDB::bind_method(D_METHOD("get_clumps_dropped"), &SnowBodies3D::get_clumps_dropped);
	ClassDB::bind_method(D_METHOD("get_slabs_slid"), &SnowBodies3D::get_slabs_slid);
	ClassDB::bind_static_method("SnowBodies3D", D_METHOD("slab_slides", "depth", "density", "wetness", "slope", "cohesion"), &SnowBodies3D::slab_slides);
	ClassDB::bind_method(D_METHOD("set_weight_enabled", "enable"), &SnowBodies3D::set_weight_enabled);
	ClassDB::bind_method(D_METHOD("is_weight_enabled"), &SnowBodies3D::is_weight_enabled);
	ClassDB::bind_method(D_METHOD("set_slide_enabled", "enable"), &SnowBodies3D::set_slide_enabled);
	ClassDB::bind_method(D_METHOD("is_slide_enabled"), &SnowBodies3D::is_slide_enabled);
	ClassDB::bind_method(D_METHOD("set_clumps_enabled", "enable"), &SnowBodies3D::set_clumps_enabled);
	ClassDB::bind_method(D_METHOD("is_clumps_enabled"), &SnowBodies3D::is_clumps_enabled);
	ClassDB::bind_method(D_METHOD("set_slab_cohesion", "pascals"), &SnowBodies3D::set_slab_cohesion);
	ClassDB::bind_method(D_METHOD("get_slab_cohesion"), &SnowBodies3D::get_slab_cohesion);

	ClassDB::bind_method(D_METHOD("set_update_rate", "rate"), &SnowBodies3D::set_update_rate);
	ClassDB::bind_method(D_METHOD("get_update_rate"), &SnowBodies3D::get_update_rate);
	ClassDB::bind_method(D_METHOD("set_shake_threshold", "g"), &SnowBodies3D::set_shake_threshold);
	ClassDB::bind_method(D_METHOD("get_shake_threshold"), &SnowBodies3D::get_shake_threshold);
	ClassDB::bind_method(D_METHOD("set_shake_shed_rate", "rate"), &SnowBodies3D::set_shake_shed_rate);
	ClassDB::bind_method(D_METHOD("get_shake_shed_rate"), &SnowBodies3D::get_shake_shed_rate);
	ClassDB::bind_method(D_METHOD("set_wind_shed_rate", "rate"), &SnowBodies3D::set_wind_shed_rate);
	ClassDB::bind_method(D_METHOD("get_wind_shed_rate"), &SnowBodies3D::get_wind_shed_rate);
	ClassDB::bind_method(D_METHOD("set_max_depth", "depth"), &SnowBodies3D::set_max_depth);
	ClassDB::bind_method(D_METHOD("get_max_depth"), &SnowBodies3D::get_max_depth);
	ClassDB::bind_method(D_METHOD("set_auto_materials", "enable"), &SnowBodies3D::set_auto_materials);
	ClassDB::bind_method(D_METHOD("is_auto_materials"), &SnowBodies3D::is_auto_materials);
	ClassDB::bind_method(D_METHOD("set_auto_track", "enable"), &SnowBodies3D::set_auto_track);
	ClassDB::bind_method(D_METHOD("is_auto_track"), &SnowBodies3D::is_auto_track);

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "update_rate", PROPERTY_HINT_RANGE, "0.1,120,0.1,suffix:Hz"), "set_update_rate", "get_update_rate");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "shake_threshold", PROPERTY_HINT_RANGE, "0,5,0.01,suffix:g"), "set_shake_threshold", "get_shake_threshold");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "shake_shed_rate", PROPERTY_HINT_RANGE, "0,20,0.01"), "set_shake_shed_rate", "get_shake_shed_rate");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wind_shed_rate", PROPERTY_HINT_RANGE, "0,2,0.001"), "set_wind_shed_rate", "get_wind_shed_rate");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "max_depth", PROPERTY_HINT_RANGE, "0,2,0.01,suffix:m"), "set_max_depth", "get_max_depth");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "auto_materials"), "set_auto_materials", "is_auto_materials");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "auto_track"), "set_auto_track", "is_auto_track");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "weight_enabled"), "set_weight_enabled", "is_weight_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "slide_enabled"), "set_slide_enabled", "is_slide_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "clumps_enabled"), "set_clumps_enabled", "is_clumps_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "slab_cohesion", PROPERTY_HINT_RANGE, "0,2000,1,suffix:Pa"), "set_slab_cohesion", "get_slab_cohesion");
}

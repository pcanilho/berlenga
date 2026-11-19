/**************************************************************************/
/*  physx_debris_surge_3d.cpp                                             */
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

#include "physx_debris_surge_3d.h"

#include "core/config/engine.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/multimesh_instance_3d.h"
#include "scene/3d/physics/collision_shape_3d.h"
#include "scene/3d/physics/rigid_body_3d.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/3d/sphere_shape_3d.h"
#include "scene/resources/material.h"

#include "modules/world/world_contracts.h"

PhysXDebrisSurge3D::PhysXDebrisSurge3D() {
	rng = uint32_t(uint64_t(this) >> 4) | 1u;
}

float PhysXDebrisSurge3D::_rand() {
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return float(rng & 0xffffffu) / float(0xffffff);
}

// The ground contract carries the weather's debris_flow signal (WeatherRunoff3D emits it there too).
void PhysXDebrisSurge3D::_connect() {
	if (connected || !listen) {
		return;
	}
	Object *g = WorldContracts::ground();
	if (g && g->has_signal("debris_flow")) {
		g->connect("debris_flow", callable_mp(this, &PhysXDebrisSurge3D::_on_debris_flow));
		connected = true;
	}
}

void PhysXDebrisSurge3D::set_listen(bool p_v) {
	listen = p_v;
	if (!listen && connected) {
		Object *g = WorldContracts::ground();
		if (g && g->is_connected("debris_flow", callable_mp(this, &PhysXDebrisSurge3D::_on_debris_flow))) {
			g->disconnect("debris_flow", callable_mp(this, &PhysXDebrisSurge3D::_on_debris_flow));
		}
		connected = false;
	}
}

void PhysXDebrisSurge3D::_on_debris_flow(const Dictionary &p_event) {
	const int id = p_event.get("id", -1);
	if (id >= 0 && id <= last_event) {
		return;
	}
	start(p_event);
}

// A train of surges (Kean et al. 2011: post-fire debris flows arrive as a series of surges, the first the biggest):
// each takes a shrinking share of the volume.
void PhysXDebrisSurge3D::start(const Dictionary &p_event) {
	const PackedVector3Array path = p_event.get("path", PackedVector3Array());
	if (path.size() < 2) {
		return;
	}
	last_event = MAX(last_event, int(p_event.get("id", -1)));
	const float volume = MAX(float(p_event.get("volume", 500.0f)), 1.0f);
	// A storm on a burnt slope fires many small events at once (one per gully): the small ones are left to the mud
	// body, and only the biggest few get surges, so fronts and boulders do not multiply.
	if (volume < min_volume) {
		skipped++;
		return;
	}
	for (int64_t k = int64_t(surges.size()) - 1; k >= 0; k--) {
		if (surges[k].done) {
			_drop_surge(k);
		}
	}
	int events = 0;
	int smallest = -1;
	for (uint32_t k = 0; k < surges.size(); k++) {
		const bool first = k == 0 || surges[k].event_id != surges[k - 1].event_id;
		if (first) {
			events++;
			if (smallest < 0 || surges[k].event_volume < surges[smallest].event_volume) {
				smallest = k;
			}
		}
	}
	if (events >= max_events) {
		if (smallest < 0 || surges[smallest].event_volume >= volume) {
			skipped++;
			return;
		}
		const int drop_id = surges[smallest].event_id;
		for (int64_t k = int64_t(surges.size()) - 1; k >= 0; k--) {
			if (surges[k].event_id == drop_id) {
				_drop_surge(k);
			}
		}
		skipped++;
	}
	const int n = CLAMP(int(Math::ceil(volume / 400.0f)), 1, surge_count);
	float total = 0.0f;
	for (int k = 0; k < n; k++) {
		total += Math::pow(0.6f, float(k));
	}
	LocalVector<float> along;
	along.push_back(0.0f);
	for (int i = 1; i < path.size(); i++) {
		along.push_back(along[i - 1] + path[i].distance_to(path[i - 1]));
	}
	for (int k = 0; k < n; k++) {
		Surge s;
		s.event_id = p_event.get("id", -1);
		s.path = path;
		s.widths = p_event.get("widths", PackedFloat32Array());
		s.along = along;
		s.volume = volume * Math::pow(0.6f, float(k)) / total;
		s.event_volume = volume;
		s.delay = k * surge_interval * (0.7f + 0.6f * _rand());
		surges.push_back(s);
		_spawn_boulders(surges.size() - 1);
	}
}

// Removes a surge and its boulders (they stay where they lie only once its front has stopped).
void PhysXDebrisSurge3D::_drop_surge(int p_surge) {
	const bool settled = surges[p_surge].done;
	for (int64_t b = int64_t(boulders.size()) - 1; b >= 0; b--) {
		if (boulders[b].surge == p_surge) {
			if (!settled) {
				if (Node *n = Object::cast_to<Node>(ObjectDB::get_instance(boulders[b].body))) {
					n->queue_free();
				}
			}
			boulders.remove_at(b);
		}
	}
	for (Boulder &bo : boulders) {
		if (bo.surge > p_surge) {
			bo.surge--;
		}
	}
	surges.remove_at(p_surge);
}

void PhysXDebrisSurge3D::_point_at(const Surge &p_surge, float p_s, Vector3 &r_pos, Vector3 &r_dir, float &r_width, float &r_slope) const {
	const int n = p_surge.path.size();
	int i = 0;
	while (i < n - 2 && p_surge.along[i + 1] < p_s) {
		i++;
	}
	const float seg = MAX(p_surge.along[i + 1] - p_surge.along[i], 1e-3f);
	const float t = CLAMP((p_s - p_surge.along[i]) / seg, 0.0f, 1.0f);
	const Vector3 a = p_surge.path[i];
	const Vector3 b = p_surge.path[i + 1];
	r_pos = a.lerp(b, t);
	const Vector3 d = b - a;
	r_dir = Vector3(d.x, 0.0f, d.z).normalized();
	const float run = MAX(Vector2(d.x, d.z).length(), 1e-3f);
	r_slope = MAX(-d.y / run, 0.0f);
	const float w0 = i < p_surge.widths.size() ? p_surge.widths[i] : 3.0f;
	const float w1 = i + 1 < p_surge.widths.size() ? p_surge.widths[i + 1] : w0;
	r_width = MAX(Math::lerp(w0, w1, t), 0.5f);
}

// The coarse fraction gathered at the snout: sizes from a power law (number ~ D^-2.5) up to max_boulder and no
// bigger than the flow is deep, placed in the first few metres of the front.
void PhysXDebrisSurge3D::_spawn_boulders(int p_surge) {
	Surge &s = surges[p_surge];
	if (rock_mesh.is_null()) {
		Ref<SphereMesh> sm;
		sm.instantiate();
		sm->set_radius(0.5f);
		sm->set_height(1.0f);
		sm->set_radial_segments(7);
		sm->set_rings(4);
		Ref<StandardMaterial3D> m;
		m.instantiate();
		m->set_albedo(Color(0.22f, 0.2f, 0.18f));
		m->set_roughness(0.95f);
		sm->set_material(m);
		rock_mesh = sm;
	}
	const int want = MIN(int(s.volume * boulder_rate), max_boulders - int(boulders.size()));
	Vector3 pos, dir;
	float w, slope;
	_point_at(s, 0.0f, pos, dir, w, slope);
	const float q = get_peak_discharge(s.volume);
	const float v = 2.1f * Math::pow(q, 1.0f / 3.0f) * Math::pow(MAX(slope, 0.02f), 1.0f / 3.0f);
	const float depth = q / MAX(v * w, 0.1f);
	for (int k = 0; k < want; k++) {
		const float dmin = 0.15f;
		const float dmax = MAX(MIN(max_boulder, depth * 1.2f), dmin * 1.5f);
		// Inverse CDF of a power law with exponent -2.5 between dmin and dmax.
		const float u = _rand();
		const float a = Math::pow(dmin, -1.5f), b = Math::pow(dmax, -1.5f);
		const float d = Math::pow(a + u * (b - a), -1.0f / 1.5f);
		RigidBody3D *rb = memnew(RigidBody3D);
		CollisionShape3D *cs = memnew(CollisionShape3D);
		Ref<SphereShape3D> sh;
		sh.instantiate();
		sh->set_radius(d * 0.45f);
		cs->set_shape(sh);
		rb->add_child(cs);
		MeshInstance3D *mi = memnew(MeshInstance3D);
		mi->set_mesh(rock_mesh);
		mi->set_scale(Vector3(d * (0.9f + 0.3f * _rand()), d * (0.6f + 0.3f * _rand()), d * (0.8f + 0.3f * _rand())));
		mi->set_rotation(Vector3(_rand(), _rand(), _rand()) * Math::TAU);
		rb->add_child(mi);
		rb->set_mass(boulder_density * Math::PI / 6.0f * d * d * d);
		rb->set_angular_damp(3.0f); // an irregular block does not roll on like the sphere it collides as
		rb->set_visible(false);
		rb->set_freeze_enabled(true);
		add_child(rb, false, INTERNAL_MODE_FRONT);
		rb->set_as_top_level(true);
		const Vector3 side = Vector3(-dir.z, 0.0f, dir.x);
		rb->set_global_position(pos + side * (_rand() - 0.5f) * w * 0.6f - dir * _rand() * 3.0f + Vector3(0, d * 0.5f, 0));
		Boulder bo;
		bo.body = rb->get_instance_id();
		bo.surge = p_surge;
		bo.radius = d * 0.5f;
		boulders.push_back(bo);
	}
}

// The front: Rickenmann's speed for the surge's peak discharge and the local slope, its depth from Q = v w h; it
// slows and stops where the slope eases below stop_slope (a fan), or at the path's end. A mud system's front, where
// one reports it for this event, replaces it.
void PhysXDebrisSurge3D::_step_surge(Surge &p_surge, float p_delta) {
	if (p_surge.done) {
		return;
	}
	if (p_surge.delay > 0.0f) {
		p_surge.delay -= p_delta;
		return;
	}
	if (Object *src = front_source.is_empty() ? nullptr : get_node_or_null(front_source)) {
		if (src->has_method("get_debris_front")) {
			// GroundDebrisFlow3D (the mud system): an unknown or finished id comes back done with no position, so
			// only a front with a position is followed.
			const Dictionary f = src->call("get_debris_front", p_surge.event_id);
			if (f.has("position") && f["position"].get_type() == Variant::VECTOR3) {
				p_surge.speed = f.get("speed", 0.0f);
				p_surge.depth = f.get("depth", 0.0f);
				if (f.has("path_index")) {
					const float pi = f["path_index"];
					const int i = CLAMP(int(pi), 0, int(p_surge.along.size()) - 1);
					const int j = MIN(i + 1, int(p_surge.along.size()) - 1);
					p_surge.s = Math::lerp(p_surge.along[i], p_surge.along[j], pi - i);
				}
				p_surge.done = f.get("done", false);
				return;
			}
		}
	}
	Vector3 pos, dir;
	float w, slope;
	_point_at(p_surge, p_surge.s, pos, dir, w, slope);
	const float q = get_peak_discharge(p_surge.volume);
	const float target = slope < stop_slope ? 0.0f : 2.1f * Math::pow(q, 1.0f / 3.0f) * Math::pow(slope, 1.0f / 3.0f);
	// The front's inertia: it eases to the speed for the slope over a few seconds, and runs out on the fan.
	p_surge.speed = Math::lerp(p_surge.speed, target, 1.0f - Math::exp(-p_delta / (target < p_surge.speed ? 8.0f : 3.0f)));
	// Depth from Q = v w h at the speed it had in the channel (a stopping front spreads and thins on the fan rather
	// than piling up).
	p_surge.depth = q / MAX(MAX(p_surge.speed, 2.0f) * w, 0.1f);
	p_surge.s += p_surge.speed * p_delta;
	const float end = p_surge.along[p_surge.along.size() - 1];
	if (p_surge.s >= end || p_surge.speed < 0.05f && p_surge.s > 1.0f) {
		p_surge.s = MIN(p_surge.s, end);
		p_surge.stopped_time = p_surge.stopped_time < 0.0f ? 0.0f : p_surge.stopped_time + p_delta;
		if (p_surge.stopped_time > 20.0f) {
			p_surge.done = true;
		}
	}
}

// Boulders in the flow: drag toward the flow's speed (Cd 1, the slurry's density), the slurry's buoyancy, and an
// extra push from behind in the snout, where the coarsest ride the front (Iverson 1997). Ahead of the front or once
// it has stopped they are on their own: they roll on and settle.
void PhysXDebrisSurge3D::_push_boulders(float p_delta) {
	for (int64_t k = int64_t(boulders.size()) - 1; k >= 0; k--) {
		Boulder &bo = boulders[k];
		RigidBody3D *rb = Object::cast_to<RigidBody3D>(ObjectDB::get_instance(bo.body));
		if (!rb) {
			boulders.remove_at_unordered(k);
			continue;
		}
		const Surge &s = surges[bo.surge];
		if (s.delay > 0.0f) {
			continue;
		}
		if (rb->is_freeze_enabled()) {
			rb->set_freeze_enabled(false);
			rb->set_visible(true);
		}
		if (s.done) {
			continue;
		}
		// Where along the path it is: the nearest segment, searched near the last one.
		const Vector3 p = rb->get_global_position();
		int best = bo.seg;
		float bd = 1e30f;
		for (int i = MAX(bo.seg - 2, 0); i < MIN(bo.seg + 4, s.path.size() - 1); i++) {
			const Vector3 a = s.path[i], b = s.path[i + 1];
			const Vector3 ab = b - a;
			const float t = CLAMP((p - a).dot(ab) / MAX(ab.length_squared(), 1e-6f), 0.0f, 1.0f);
			const float d = p.distance_squared_to(a + ab * t);
			if (d < bd) {
				bd = d;
				best = i;
			}
		}
		bo.seg = best;
		const Vector3 a = s.path[best], b = s.path[best + 1];
		const Vector3 ab = b - a;
		const float t = CLAMP((p - a).dot(ab) / MAX(ab.length_squared(), 1e-6f), 0.0f, 1.0f);
		const float sp = Math::lerp(s.along[best], s.along[best + 1], t);
		const float behind = s.s - sp; // m behind the front
		const float body_len = 20.0f + s.volume / MAX(s.depth * 3.0f, 0.5f) * 0.1f;
		// Those a little ahead of a slowing front are in its lobe and stop with it.
		if (behind < -10.0f || behind > body_len) {
			continue;
		}
		const Vector3 bed = a + ab * t;
		const float submerged = CLAMP((bed.y + s.depth - (p.y - bo.radius)) / (2.0f * bo.radius), 0.0f, 1.0f);
		if (submerged <= 0.0f) {
			continue;
		}
		const Vector3 dir = Vector3(ab.x, 0.0f, ab.z).normalized();
		// The flow behind the snout runs faster than the front (its surface about 1.3 times), so the coarse
		// clasts drift forward until they gather at the snout, where they move with it.
		const Vector3 u = dir * s.speed * (behind > 2.0f ? 1.3f : 1.0f);
		const Vector3 rel = u - rb->get_linear_velocity();
		const float area = Math::PI * bo.radius * bo.radius * submerged;
		Vector3 f = 0.5f * slurry_density * 1.2f * area * rel * rel.length();
		// No more than takes it to the flow's speed this step (stable for small cobbles).
		const float fmax = float(rb->get_mass()) * rel.length() / MAX(p_delta, 1e-3f);
		if (f.length() > fmax) {
			f = f.normalized() * fmax;
		}
		const float vol = 4.0f / 3.0f * Math::PI * bo.radius * bo.radius * bo.radius * submerged;
		f += Vector3(0, slurry_density * 9.81f * vol, 0);
		rb->apply_central_force(f);
	}
}

// Mud thrown up at the front where boulders and the snout churn: drops of a few centimetres, ballistic, more the
// faster and wider the front.
void PhysXDebrisSurge3D::_step_spray(float p_delta) {
	for (const Surge &s : surges) {
		if (s.delay > 0.0f || s.done || s.speed < 0.3f || spray <= 0.0f) {
			continue;
		}
		Vector3 pos, dir;
		float w, slope;
		_point_at(s, s.s, pos, dir, w, slope);
		spray_carry += spray * 6.0f * s.speed * w * p_delta;
		const Vector3 side(-dir.z, 0.0f, dir.x);
		while (spray_carry >= 1.0f && drops.size() < 2000) {
			spray_carry -= 1.0f;
			Drop d;
			d.p = pos + side * (_rand() - 0.5f) * w + Vector3(0, s.depth * (0.5f + 0.5f * _rand()), 0);
			d.v = dir * s.speed * (0.8f + 0.6f * _rand()) + side * (_rand() - 0.5f) * 2.0f + Vector3(0, (1.0f + 3.0f * _rand()) * MIN(s.speed / 3.0f, 1.5f), 0);
			d.life = 0.6f + 0.8f * _rand();
			d.size = 0.02f + 0.06f * _rand() * _rand();
			drops.push_back(d);
		}
	}
	spray_carry = MIN(spray_carry, 1.0f);
	for (int64_t k = int64_t(drops.size()) - 1; k >= 0; k--) {
		Drop &d = drops[k];
		d.age += p_delta;
		d.v += Vector3(0, -9.81f, 0) * p_delta;
		d.v *= 1.0f - MIN(0.3f * p_delta, 1.0f);
		d.p += d.v * p_delta;
		if (d.age > d.life) {
			drops.remove_at_unordered(k);
		}
	}
	if (drops.is_empty()) {
		if (spray_mmi) {
			spray_mmi->set_visible(false);
		}
		return;
	}
	if (!spray_mmi) {
		spray_mm.instantiate();
		spray_mm->set_transform_format(MultiMesh::TRANSFORM_3D);
		Ref<SphereMesh> sm;
		sm.instantiate();
		sm->set_radius(0.5f);
		sm->set_height(1.0f);
		sm->set_radial_segments(6);
		sm->set_rings(3);
		Ref<StandardMaterial3D> m;
		m.instantiate();
		m->set_albedo(mud_color);
		m->set_roughness(0.4f); // wet
		sm->set_material(m);
		spray_mm->set_mesh(sm);
		spray_mmi = memnew(MultiMeshInstance3D);
		spray_mmi->set_multimesh(spray_mm);
		spray_mmi->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
		add_child(spray_mmi, false, INTERNAL_MODE_FRONT);
		spray_mmi->set_as_top_level(true);
		spray_mmi->set_global_transform(Transform3D());
	}
	spray_mmi->set_visible(true);
	spray_mm->set_instance_count(drops.size());
	for (uint32_t i = 0; i < drops.size(); i++) {
		const Drop &d = drops[i];
		// Stretched along their flight a little (motion), round at rest.
		const float st = 1.0f + MIN(d.v.length() * 0.05f, 1.0f);
		Basis bs = Basis::looking_at(d.v.length() > 0.01f ? d.v.normalized() : Vector3(0, 0, -1), Math::abs(d.v.normalized().y) > 0.99f ? Vector3(1, 0, 0) : Vector3(0, 1, 0));
		bs.scale_local(Vector3(d.size, d.size, d.size * st));
		spray_mm->set_instance_transform(i, Transform3D(bs, d.p));
	}
}

int PhysXDebrisSurge3D::get_active_surges() const {
	int n = 0;
	for (const Surge &s : surges) {
		n += (!s.done && s.delay <= 0.0f) ? 1 : 0;
	}
	return n;
}

Vector3 PhysXDebrisSurge3D::get_front_position(int p_surge) const {
	ERR_FAIL_INDEX_V(p_surge, (int)surges.size(), Vector3());
	Vector3 pos, dir;
	float w, slope;
	_point_at(surges[p_surge], surges[p_surge].s, pos, dir, w, slope);
	return pos;
}

float PhysXDebrisSurge3D::get_front_speed(int p_surge) const {
	ERR_FAIL_INDEX_V(p_surge, (int)surges.size(), 0.0f);
	return surges[p_surge].speed;
}

float PhysXDebrisSurge3D::get_front_depth(int p_surge) const {
	ERR_FAIL_INDEX_V(p_surge, (int)surges.size(), 0.0f);
	return surges[p_surge].depth;
}

void PhysXDebrisSurge3D::clear() {
	for (Boulder &bo : boulders) {
		if (Node *n = Object::cast_to<Node>(ObjectDB::get_instance(bo.body))) {
			n->queue_free();
		}
	}
	boulders.clear();
	surges.clear();
	drops.clear();
}

void PhysXDebrisSurge3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			if (!Engine::get_singleton()->is_editor_hint()) {
				set_physics_process_internal(true);
			}
		} break;
		case NOTIFICATION_INTERNAL_PHYSICS_PROCESS: {
			_connect();
			const float dt = float(get_physics_process_delta_time());
			for (Surge &s : surges) {
				_step_surge(s, dt);
			}
			_push_boulders(dt);
			_step_spray(dt);
		} break;
	}
}

void PhysXDebrisSurge3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("start", "event"), &PhysXDebrisSurge3D::start);
	ClassDB::bind_method(D_METHOD("clear"), &PhysXDebrisSurge3D::clear);
	ClassDB::bind_method(D_METHOD("get_surge_count"), &PhysXDebrisSurge3D::get_surge_count);
	ClassDB::bind_method(D_METHOD("get_active_surges"), &PhysXDebrisSurge3D::get_active_surges);
	ClassDB::bind_method(D_METHOD("get_boulder_count"), &PhysXDebrisSurge3D::get_boulder_count);
	ClassDB::bind_method(D_METHOD("get_spray_count"), &PhysXDebrisSurge3D::get_spray_count);
	ClassDB::bind_method(D_METHOD("get_front_position", "surge"), &PhysXDebrisSurge3D::get_front_position);
	ClassDB::bind_method(D_METHOD("get_front_speed", "surge"), &PhysXDebrisSurge3D::get_front_speed);
	ClassDB::bind_method(D_METHOD("get_front_depth", "surge"), &PhysXDebrisSurge3D::get_front_depth);
	ClassDB::bind_method(D_METHOD("get_peak_discharge", "volume"), &PhysXDebrisSurge3D::get_peak_discharge);
	ClassDB::bind_method(D_METHOD("set_front_source", "path"), &PhysXDebrisSurge3D::set_front_source);
	ClassDB::bind_method(D_METHOD("get_front_source"), &PhysXDebrisSurge3D::get_front_source);
	ClassDB::bind_method(D_METHOD("set_listen", "enabled"), &PhysXDebrisSurge3D::set_listen);
	ClassDB::bind_method(D_METHOD("is_listening"), &PhysXDebrisSurge3D::is_listening);
	ClassDB::bind_method(D_METHOD("set_surge_count_max", "count"), &PhysXDebrisSurge3D::set_surge_count);
	ClassDB::bind_method(D_METHOD("get_surge_count_max"), &PhysXDebrisSurge3D::get_surge_count_max);
	ClassDB::bind_method(D_METHOD("set_surge_interval", "seconds"), &PhysXDebrisSurge3D::set_surge_interval);
	ClassDB::bind_method(D_METHOD("get_surge_interval"), &PhysXDebrisSurge3D::get_surge_interval);
	ClassDB::bind_method(D_METHOD("set_boulder_density", "kg_per_m3"), &PhysXDebrisSurge3D::set_boulder_density);
	ClassDB::bind_method(D_METHOD("get_boulder_density"), &PhysXDebrisSurge3D::get_boulder_density);
	ClassDB::bind_method(D_METHOD("set_slurry_density", "kg_per_m3"), &PhysXDebrisSurge3D::set_slurry_density);
	ClassDB::bind_method(D_METHOD("get_slurry_density"), &PhysXDebrisSurge3D::get_slurry_density);
	ClassDB::bind_method(D_METHOD("set_max_boulder", "metres"), &PhysXDebrisSurge3D::set_max_boulder);
	ClassDB::bind_method(D_METHOD("get_max_boulder"), &PhysXDebrisSurge3D::get_max_boulder);
	ClassDB::bind_method(D_METHOD("set_max_boulders", "count"), &PhysXDebrisSurge3D::set_max_boulders);
	ClassDB::bind_method(D_METHOD("get_max_boulders"), &PhysXDebrisSurge3D::get_max_boulders);
	ClassDB::bind_method(D_METHOD("set_boulder_rate", "per_m3"), &PhysXDebrisSurge3D::set_boulder_rate);
	ClassDB::bind_method(D_METHOD("get_boulder_rate"), &PhysXDebrisSurge3D::get_boulder_rate);
	ClassDB::bind_method(D_METHOD("set_stop_slope", "slope"), &PhysXDebrisSurge3D::set_stop_slope);
	ClassDB::bind_method(D_METHOD("get_stop_slope"), &PhysXDebrisSurge3D::get_stop_slope);
	ClassDB::bind_method(D_METHOD("set_min_volume", "m3"), &PhysXDebrisSurge3D::set_min_volume);
	ClassDB::bind_method(D_METHOD("get_min_volume"), &PhysXDebrisSurge3D::get_min_volume);
	ClassDB::bind_method(D_METHOD("set_max_events", "count"), &PhysXDebrisSurge3D::set_max_events);
	ClassDB::bind_method(D_METHOD("get_max_events"), &PhysXDebrisSurge3D::get_max_events);
	ClassDB::bind_method(D_METHOD("get_skipped_events"), &PhysXDebrisSurge3D::get_skipped_events);
	ClassDB::bind_method(D_METHOD("set_spray", "amount"), &PhysXDebrisSurge3D::set_spray);
	ClassDB::bind_method(D_METHOD("get_spray"), &PhysXDebrisSurge3D::get_spray);
	ClassDB::bind_method(D_METHOD("set_mud_color", "color"), &PhysXDebrisSurge3D::set_mud_color);
	ClassDB::bind_method(D_METHOD("get_mud_color"), &PhysXDebrisSurge3D::get_mud_color);

	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "front_source"), "set_front_source", "get_front_source");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "listen"), "set_listen", "is_listening");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "surge_count_max", PROPERTY_HINT_RANGE, "1,16,1"), "set_surge_count_max", "get_surge_count_max");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "surge_interval", PROPERTY_HINT_RANGE, "1,600,1,suffix:s"), "set_surge_interval", "get_surge_interval");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "boulder_density", PROPERTY_HINT_RANGE, "500,4000,10,suffix:kg/m³"), "set_boulder_density", "get_boulder_density");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "slurry_density", PROPERTY_HINT_RANGE, "1000,2400,10,suffix:kg/m³"), "set_slurry_density", "get_slurry_density");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "max_boulder", PROPERTY_HINT_RANGE, "0.05,4,0.05,suffix:m"), "set_max_boulder", "get_max_boulder");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_boulders", PROPERTY_HINT_RANGE, "0,500,1"), "set_max_boulders", "get_max_boulders");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "boulder_rate", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_boulder_rate", "get_boulder_rate");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "stop_slope", PROPERTY_HINT_RANGE, "0,0.5,0.005"), "set_stop_slope", "get_stop_slope");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "min_volume", PROPERTY_HINT_RANGE, "0,10000,1,suffix:m³"), "set_min_volume", "get_min_volume");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_events", PROPERTY_HINT_RANGE, "1,64,1"), "set_max_events", "get_max_events");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "spray", PROPERTY_HINT_RANGE, "0,4,0.01"), "set_spray", "get_spray");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "mud_color"), "set_mud_color", "get_mud_color");
}

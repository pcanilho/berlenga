/**************************************************************************/
/*  physx_water_pusher_3d.cpp                                             */
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

#include "physx_water_pusher_3d.h"

#include "physx_live_water_3d.h"
#include "physx_river_3d.h"
#include "physx_water_surface_3d.h"

#include "core/config/engine.h"
#include "core/object/class_db.h"
#include "scene/3d/physics/collision_object_3d.h"
#include "scene/3d/physics/collision_shape_3d.h"
#include "scene/3d/skeleton_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/resources/3d/box_shape_3d.h"
#include "scene/resources/3d/capsule_shape_3d.h"
#include "scene/resources/3d/cylinder_shape_3d.h"
#include "scene/resources/3d/sphere_shape_3d.h"

void PhysXWaterPusher3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_source", "source"), &PhysXWaterPusher3D::set_source);
	ClassDB::bind_method(D_METHOD("get_source"), &PhysXWaterPusher3D::get_source);
	ClassDB::bind_method(D_METHOD("set_target", "path"), &PhysXWaterPusher3D::set_target);
	ClassDB::bind_method(D_METHOD("get_target"), &PhysXWaterPusher3D::get_target);
	ClassDB::bind_method(D_METHOD("set_bone_radius", "radius"), &PhysXWaterPusher3D::set_bone_radius);
	ClassDB::bind_method(D_METHOD("get_bone_radius"), &PhysXWaterPusher3D::get_bone_radius);
	ClassDB::bind_method(D_METHOD("set_max_spheres", "count"), &PhysXWaterPusher3D::set_max_spheres);
	ClassDB::bind_method(D_METHOD("get_max_spheres"), &PhysXWaterPusher3D::get_max_spheres);
	ClassDB::bind_method(D_METHOD("set_strength", "strength"), &PhysXWaterPusher3D::set_strength);
	ClassDB::bind_method(D_METHOD("get_strength"), &PhysXWaterPusher3D::get_strength);
	ClassDB::bind_method(D_METHOD("set_rest_strength", "strength"), &PhysXWaterPusher3D::set_rest_strength);
	ClassDB::bind_method(D_METHOD("get_rest_strength"), &PhysXWaterPusher3D::get_rest_strength);
	ClassDB::bind_method(D_METHOD("set_reference_speed", "speed"), &PhysXWaterPusher3D::set_reference_speed);
	ClassDB::bind_method(D_METHOD("get_reference_speed"), &PhysXWaterPusher3D::get_reference_speed);
	ClassDB::bind_method(D_METHOD("set_enabled", "enabled"), &PhysXWaterPusher3D::set_enabled);
	ClassDB::bind_method(D_METHOD("is_enabled"), &PhysXWaterPusher3D::is_enabled);
	ClassDB::bind_method(D_METHOD("get_spheres"), &PhysXWaterPusher3D::get_spheres);
	ClassDB::bind_method(D_METHOD("get_submitted_count"), &PhysXWaterPusher3D::get_submitted_count);

	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enabled"), "set_enabled", "is_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "source", PROPERTY_HINT_ENUM, "Auto,Skeleton,Shapes"), "set_source", "get_source");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "target"), "set_target", "get_target");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "bone_radius", PROPERTY_HINT_RANGE, "0.005,1,0.005,or_greater,suffix:m"), "set_bone_radius", "get_bone_radius");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_spheres", PROPERTY_HINT_RANGE, "1,48,1"), "set_max_spheres", "get_max_spheres");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "strength", PROPERTY_HINT_RANGE, "0,4,0.01"), "set_strength", "get_strength");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "rest_strength", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_rest_strength", "get_rest_strength");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "reference_speed", PROPERTY_HINT_RANGE, "0.01,20,0.01,suffix:m/s"), "set_reference_speed", "get_reference_speed");

	BIND_ENUM_CONSTANT(SOURCE_AUTO);
	BIND_ENUM_CONSTANT(SOURCE_SKELETON);
	BIND_ENUM_CONSTANT(SOURCE_SHAPES);
}

void PhysXWaterPusher3D::set_enabled(bool p_enabled) {
	enabled = p_enabled;
	if (!enabled) {
		_clear_all();
	}
}

int64_t PhysXWaterPusher3D::_owner(int p_index) const {
	// One slot per sphere: the node's id and the sphere's index (ids are spaced far apart).
	return (int64_t)(((uint64_t)get_instance_id() << 6) | (uint64_t)p_index);
}

PackedVector4Array PhysXWaterPusher3D::get_spheres() const {
	PackedVector4Array out;
	for (const Sphere &s : spheres) {
		out.push_back(Vector4(s.position.x, s.position.y, s.position.z, s.radius));
	}
	return out;
}

// Limbs: spheres of bone_radius strung along each bone, from its head to each child's head, spaced a radius apart.
void PhysXWaterPusher3D::_gather_skeleton(Node *p_node) {
	Skeleton3D *sk = Object::cast_to<Skeleton3D>(p_node);
	if (sk == nullptr) {
		return;
	}
	const Transform3D xf = sk->get_global_transform();
	const int count = sk->get_bone_count();
	for (int b = 0; b < count; b++) {
		const int parent = sk->get_bone_parent(b);
		if (parent < 0) {
			continue;
		}
		const Vector3 a = xf.xform(sk->get_bone_global_pose(parent).origin);
		const Vector3 c = xf.xform(sk->get_bone_global_pose(b).origin);
		const float len = a.distance_to(c);
		const int steps = MAX(1, (int)Math::ceil(len / bone_radius));
		for (int k = 0; k <= steps; k++) {
			spheres.push_back({ a.lerp(c, (float)k / steps), bone_radius });
		}
	}
}

// Shapes: a sphere for a sphere, a row along a capsule or cylinder, a grid through a box (others: their AABB).
void PhysXWaterPusher3D::_gather_shapes(Node *p_node) {
	for (int i = 0; i < p_node->get_child_count(); i++) {
		CollisionShape3D *cs = Object::cast_to<CollisionShape3D>(p_node->get_child(i));
		if (cs == nullptr || cs->is_disabled() || cs->get_shape().is_null()) {
			continue;
		}
		const Transform3D xf = cs->get_global_transform();
		const Ref<Shape3D> shape = cs->get_shape();
		if (Ref<SphereShape3D> s = shape; s.is_valid()) {
			spheres.push_back({ xf.origin, s->get_radius() * xf.basis.get_scale().x });
		} else if (Ref<CapsuleShape3D> c = shape; c.is_valid()) {
			const float r = c->get_radius();
			const float h = MAX(c->get_height() * 0.5f - r, 0.0f);
			const int steps = MAX(1, (int)Math::ceil(2.0f * h / r));
			for (int k = 0; k <= steps; k++) {
				spheres.push_back({ xf.xform(Vector3(0.0f, -h + 2.0f * h * k / steps, 0.0f)), r });
			}
		} else if (Ref<CylinderShape3D> y = shape; y.is_valid()) {
			const float r = y->get_radius();
			const float h = y->get_height() * 0.5f;
			const int steps = MAX(1, (int)Math::ceil(2.0f * h / r));
			for (int k = 0; k <= steps; k++) {
				spheres.push_back({ xf.xform(Vector3(0.0f, -h + 2.0f * h * k / steps, 0.0f)), r });
			}
		} else {
			Vector3 half;
			if (Ref<BoxShape3D> bx = shape; bx.is_valid()) {
				half = bx->get_size() * 0.5f;
			} else {
				half = shape->get_debug_mesh().is_valid() ? shape->get_debug_mesh()->get_aabb().size * 0.5f : Vector3(0.5f, 0.5f, 0.5f);
			}
			const float r = MAX(MIN(half.x, MIN(half.y, half.z)), 0.02f);
			const Vector3i cells(MAX(1, (int)Math::round(half.x / r)), MAX(1, (int)Math::round(half.y / r)), MAX(1, (int)Math::round(half.z / r)));
			for (int z = 0; z < cells.z; z++) {
				for (int y = 0; y < cells.y; y++) {
					for (int x = 0; x < cells.x; x++) {
						const Vector3 local(-half.x + (2.0f * x + 1.0f) * half.x / cells.x, -half.y + (2.0f * y + 1.0f) * half.y / cells.y, -half.z + (2.0f * z + 1.0f) * half.z / cells.z);
						spheres.push_back({ xf.xform(local), r });
					}
				}
			}
		}
	}
}

void PhysXWaterPusher3D::_gather() {
	spheres.clear();
	Node *t = target.is_empty() ? get_parent() : get_node_or_null(target);
	if (t == nullptr) {
		return;
	}
	const bool skeleton = source == SOURCE_SKELETON || (source == SOURCE_AUTO && Object::cast_to<Skeleton3D>(t) != nullptr);
	if (skeleton) {
		_gather_skeleton(t);
	} else {
		_gather_shapes(t);
	}
}

static void collect_waters(Node *p_node, LocalVector<ObjectID> &r_out) {
	if (Object::cast_to<PhysXLiveWater3D>(p_node) || Object::cast_to<PhysXWaterSurface3D>(p_node) || Object::cast_to<PhysXRiver3D>(p_node)) {
		r_out.push_back(p_node->get_instance_id());
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		collect_waters(p_node->get_child(i), r_out);
	}
}

void PhysXWaterPusher3D::_scan_waters() {
	waters.clear();
	if (is_inside_tree()) {
		collect_waters(get_tree()->get_root(), waters);
	}
}

void PhysXWaterPusher3D::_clear_all() {
	for (const ObjectID &id : waters) {
		Object *o = ObjectDB::get_instance(id);
		for (int i = 0; i < submitted; i++) {
			const int64_t owner = _owner(i);
			if (PhysXLiveWater3D *lw = Object::cast_to<PhysXLiveWater3D>(o)) {
				lw->clear_sphere(owner);
			} else if (PhysXWaterSurface3D *ws = Object::cast_to<PhysXWaterSurface3D>(o)) {
				ws->clear_sphere((int)owner);
			} else if (PhysXRiver3D *rv = Object::cast_to<PhysXRiver3D>(o)) {
				rv->clear_sphere((int)owner);
			}
		}
	}
	submitted = 0;
}

void PhysXWaterPusher3D::_push(double p_dt) {
	_gather();
	// Speeds against last frame (same sphere count: the same limbs).
	LocalVector<float> speed;
	speed.resize(spheres.size());
	for (uint32_t i = 0; i < spheres.size(); i++) {
		speed[i] = (previous.size() == spheres.size() && p_dt > 0.0) ? spheres[i].position.distance_to(previous[i]) / (float)p_dt : 0.0f;
	}
	previous.resize(spheres.size());
	for (uint32_t i = 0; i < spheres.size(); i++) {
		previous[i] = spheres[i].position;
	}
	const int old_submitted = submitted;
	int used = 0;
	for (const ObjectID &id : waters) {
		Object *o = ObjectDB::get_instance(id);
		PhysXLiveWater3D *lw = Object::cast_to<PhysXLiveWater3D>(o);
		PhysXWaterSurface3D *ws = Object::cast_to<PhysXWaterSurface3D>(o);
		PhysXRiver3D *rv = Object::cast_to<PhysXRiver3D>(o);
		if (lw == nullptr && ws == nullptr && rv == nullptr) {
			continue;
		}
		// The spheres that reach the water here, the deepest first in body order, up to max_spheres.
		int k = 0;
		for (uint32_t i = 0; i < spheres.size() && k < max_spheres; i++) {
			const Sphere &s = spheres[i];
			float surface;
			if (lw != nullptr) {
				surface = lw->sample_depth(s.position) > 0.0f ? lw->sample_height(s.position) : NAN;
			} else if (ws != nullptr) {
				surface = ws->sample_height(s.position);
			} else {
				surface = rv->sample_height(s.position);
			}
			if (!Math::is_finite(surface) || s.position.y - s.radius > surface) {
				continue; // out of the water
			}
			const int64_t owner = _owner(k);
			if (lw != nullptr) {
				lw->submit_sphere(owner, s.position, s.radius * strength);
			} else {
				// Near the surface only (a sphere deep under it moves the live layer little), harder when moving.
				const float proximity = CLAMP(1.0f - (surface - s.position.y) / (2.0f * s.radius), 0.0f, 1.0f);
				const float push = strength * proximity * CLAMP(rest_strength + (1.0f - rest_strength) * speed[i] / reference_speed, 0.0f, 1.0f);
				if (ws != nullptr) {
					ws->submit_sphere((int)owner, s.position, s.radius, push);
				} else {
					rv->submit_sphere((int)owner, s.position, s.radius, push);
				}
			}
			k++;
		}
		// Slots this water no longer gets are cleared.
		for (int j = k; j < old_submitted; j++) {
			const int64_t owner = _owner(j);
			if (lw != nullptr) {
				lw->clear_sphere(owner);
			} else if (ws != nullptr) {
				ws->clear_sphere((int)owner);
			} else {
				rv->clear_sphere((int)owner);
			}
		}
		used = MAX(used, k);
	}
	submitted = used;
}

void PhysXWaterPusher3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			if (!Engine::get_singleton()->is_editor_hint()) {
				set_physics_process_internal(true);
				water_scan_timer = 0.0;
			}
		} break;
		case NOTIFICATION_EXIT_TREE: {
			_clear_all();
			set_physics_process_internal(false);
		} break;
		case NOTIFICATION_INTERNAL_PHYSICS_PROCESS: {
			const double dt = get_physics_process_delta_time();
			water_scan_timer -= dt;
			if (water_scan_timer <= 0.0) {
				_scan_waters(); // (new water bodies; ones that left the tree are skipped by id)
				water_scan_timer = 2.0;
			}
			if (enabled) {
				_push(dt);
			} else if (submitted > 0) {
				_clear_all();
			}
		} break;
	}
}

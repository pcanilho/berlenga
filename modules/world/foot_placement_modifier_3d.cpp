/**************************************************************************/
/*  foot_placement_modifier_3d.cpp                                        */
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

#include "foot_placement_modifier_3d.h"

#include "world_contracts.h"
#include "world_walker_steps.h"

#include "core/object/class_db.h"
#include "scene/3d/skeleton_3d.h"
#include "scene/resources/3d/world_3d.h"
#ifndef PHYSICS_3D_DISABLED
#include "scene/3d/physics/collision_object_3d.h"
#include "servers/physics_3d/physics_server_3d.h"
#endif

void FootPlacementModifier3D::_configure_springs() {
	pelvis_spring.configure(pelvis_frequency, 1.0, 0.0);
	for (Leg &leg : legs) {
		leg.spring.configure(foot_frequency, 1.0, 0.0);
		leg.normal_spring.configure(foot_frequency * (real_t)0.5, 1.0, 0.0);
	}
}

Node3D *FootPlacementModifier3D::_walker() const {
	if (!walker_node.is_empty()) {
		return Object::cast_to<Node3D>(get_node_or_null(walker_node));
	}
	// The nearest character body above the skeleton: its capsule is the walker the deformers know.
	for (Node *n = get_parent(); n; n = n->get_parent()) {
		if (n->is_class("CharacterBody3D")) {
			return Object::cast_to<Node3D>(n);
		}
	}
	return nullptr;
}

// The ground height under a point: the world ground's deformed surface (and the snow on it) when a producer knows the
// point within the probe window, else a short physics ray through the window (never a roof above, never the walker).
bool FootPlacementModifier3D::_ground_at(const Vector3 &p_point, real_t &r_height) const {
	const real_t top = p_point.y + probe_up;
	const real_t bottom = p_point.y - probe_down;
	if (use_world_ground) {
		WorldGround *ground = WorldContracts::ground();
		const float h = ground ? ground->known_height_at(p_point) : NAN;
		if (Math::is_finite(h) && h <= top && h >= bottom) {
			WorldSnow *snow = WorldContracts::snow();
			const float depth = snow ? snow->depth_at(p_point) : 0.0f;
			r_height = h + MAX(depth, 0.0f);
			return true;
		}
	}
#ifndef PHYSICS_3D_DISABLED
	Skeleton3D *skeleton = get_skeleton();
	if (!skeleton || !skeleton->is_inside_tree()) {
		return false;
	}
	Ref<World3D> world = skeleton->get_world_3d();
	PhysicsDirectSpaceState3D *space = world.is_valid() ? world->get_direct_space_state() : nullptr;
	if (!space) {
		return false;
	}
	PhysicsDirectSpaceState3D::RayParameters ray;
	ray.from = Vector3(p_point.x, top, p_point.z);
	ray.to = Vector3(p_point.x, bottom, p_point.z);
	ray.collision_mask = collision_mask;
	ray.collide_with_areas = false;
	HashSet<RID> exclude;
	const CollisionObject3D *walker = Object::cast_to<CollisionObject3D>(_walker());
	if (walker) {
		exclude.insert(walker->get_rid());
		ray.exclude = exclude;
	}
	PhysicsDirectSpaceState3D::RayResult hit;
	if (space->intersect_ray(ray, hit)) {
		real_t h = hit.position.y;
		if (use_world_ground) {
			WorldSnow *snow = WorldContracts::snow();
			h += snow ? MAX(snow->depth_at(hit.position), 0.0f) : 0.0f;
		}
		r_height = h;
		return true;
	}
#endif
	return false;
}

// Height and slope under a foot: four samples a sole's half length round the point (central differences).
bool FootPlacementModifier3D::_sample_ground(const Vector3 &p_point, real_t &r_height, Vector3 &r_normal) const {
	if (!_ground_at(p_point, r_height)) {
		return false;
	}
	const real_t r = 0.12;
	real_t hx0 = r_height, hx1 = r_height, hz0 = r_height, hz1 = r_height;
	_ground_at(p_point + Vector3(-r, 0, 0), hx0);
	_ground_at(p_point + Vector3(r, 0, 0), hx1);
	_ground_at(p_point + Vector3(0, 0, -r), hz0);
	_ground_at(p_point + Vector3(0, 0, r), hz1);
	r_normal = Vector3(hx0 - hx1, 2 * r, hz0 - hz1).normalized();
	return true;
}

// Holden's analytic two-joint IK, in the skeleton's space: bend the middle joint to the root-to-target length,
// swing the limb onto the target keeping the clip's bend plane, give the end bone its basis.
void FootPlacementModifier3D::_solve_two_joint(Skeleton3D *p_skeleton, int p_upper, int p_lower, int p_end, const Vector3 &p_target, const Basis &p_end_basis) {
	const Transform3D gu = p_skeleton->get_bone_global_pose(p_upper);
	const Transform3D gl = p_skeleton->get_bone_global_pose(p_lower);
	const Transform3D gf = p_skeleton->get_bone_global_pose(p_end);
	const Vector3 a = gu.origin;
	const Vector3 b = gl.origin;
	const Vector3 c = gf.origin;
	const real_t lab = a.distance_to(b);
	const real_t lcb = b.distance_to(c);
	if (lab < (real_t)1e-5 || lcb < (real_t)1e-5) {
		return;
	}
	const real_t lat = CLAMP(a.distance_to(p_target), (real_t)0.01 * (lab + lcb), lab + lcb - (real_t)1e-4 * (lab + lcb));
	auto ang = [](const Vector3 &u, const Vector3 &v) {
		return Math::acos(CLAMP(u.normalized().dot(v.normalized()), (real_t)-1.0, (real_t)1.0));
	};
	const real_t ac_ab_0 = ang(c - a, b - a);
	const real_t ba_bc_0 = ang(a - b, c - b);
	const real_t ac_at_0 = ang(c - a, p_target - a);
	const real_t ac_ab_1 = Math::acos(CLAMP((lcb * lcb - lab * lab - lat * lat) / (-2 * lab * lat), (real_t)-1.0, (real_t)1.0));
	const real_t ba_bc_1 = Math::acos(CLAMP((lat * lat - lab * lab - lcb * lcb) / (-2 * lab * lcb), (real_t)-1.0, (real_t)1.0));
	Vector3 axis0 = (c - a).cross(b - a);
	if (axis0.length_squared() < (real_t)1e-14) {
		return; // a dead straight leg has no bend plane: leave it as the clip has it
	}
	axis0.normalize();
	const Basis r0(axis0, ac_ab_1 - ac_ab_0);
	const Basis r2(axis0, ba_bc_1 - ba_bc_0);
	const Vector3 axis1 = (c - a).cross(p_target - a);
	const Basis r1 = axis1.length_squared() > (real_t)1e-14 ? Basis(axis1.normalized(), ac_at_0) : Basis();
	const Basis ru = r1 * r0;
	const Vector3 nb = a + ru.xform(b - a);
	const Basis rl = ru * r2;
	p_skeleton->set_bone_global_pose(p_upper, Transform3D(ru * gu.basis, a));
	p_skeleton->set_bone_global_pose(p_lower, Transform3D(rl * gl.basis, nb));
	p_skeleton->set_bone_global_pose(p_end, Transform3D(p_end_basis, nb + rl.xform(c - b)));
}

// The ankle's height above the sole: set, or the rest pose's (rigs that stand on their origin at rest), but never
// more than the lowest the clip has held the ankle over its floor (rigs whose rest pose floats or crouches).
real_t FootPlacementModifier3D::_ankle_height(Skeleton3D *p_skeleton, int p_foot, real_t p_lowest) const {
	if (ankle_height >= 0) {
		return ankle_height;
	}
	const Transform3D g = p_skeleton->get_global_transform_interpolated();
	real_t h = MAX((g.xform(p_skeleton->get_bone_global_rest(p_foot).origin) - g.origin).y, (real_t)0.0);
	if (p_lowest >= 0) {
		h = MIN(h, p_lowest);
	}
	return h;
}

void FootPlacementModifier3D::_process_modification(double p_delta) {
	Skeleton3D *skeleton = get_skeleton();
	if (!skeleton) {
		return;
	}
	const int hips = skeleton->find_bone(hips_bone);
	int bones[2][3];
	for (int s = 0; s < 2; s++) {
		bones[s][0] = skeleton->find_bone(legs[s].thigh);
		bones[s][1] = skeleton->find_bone(legs[s].shin);
		bones[s][2] = skeleton->find_bone(legs[s].foot);
		if (bones[s][0] < 0 || bones[s][1] < 0 || bones[s][2] < 0) {
			return;
		}
	}
	const real_t dt = (real_t)MAX(p_delta, 0.0);
	const Transform3D sk_global = skeleton->get_global_transform_interpolated();
	const Transform3D sk_inverse = sk_global.affine_inverse();
	const Vector3 up(0, 1, 0);
	const real_t floor = sk_global.origin.y; // the clip stands on the model's origin
	const Node3D *reference = Object::cast_to<Node3D>(get_node_or_null(reference_node));
	const Basis frame = (reference && reference->is_inside_tree() ? const_cast<Node3D *>(reference)->get_global_transform_interpolated().basis : sk_global.basis).orthonormalized();
	Vector3 forward = frame.get_column(2);
	forward.y = 0;
	forward = forward.length_squared() > (real_t)1e-8 ? forward.normalized() : Vector3(0, 0, 1);

	Node3D *walker = publish_footfalls ? _walker() : nullptr;
	if (walker && walker->get_instance_id() != walker_id) {
		if (walker_id.is_valid()) {
			WorldFootfalls::remove(walker_id);
		}
		walker_id = walker->get_instance_id();
		walker->set_meta(SNAME("world_contact_feet"), true);
	}

	Vector3 clip[2];
	Basis clip_basis[2];
	real_t ankle[2];
	real_t needed[2];
	for (int s = 0; s < 2; s++) {
		Leg &leg = legs[s];
		const Transform3D pose = skeleton->get_bone_global_pose(bones[s][2]);
		clip[s] = sk_global.xform(pose.origin);
		clip_basis[s] = pose.basis;
		const real_t over = clip[s].y - floor;
		if (over >= 0 && (leg.lowest < 0 || over < leg.lowest)) {
			leg.lowest = over;
		}
		ankle[s] = _ankle_height(skeleton, bones[s][2], leg.lowest);
		const real_t lift = clip[s].y - floor - ankle[s];
		const real_t speed = (leg.clip_prev_valid && dt > 0) ? clip[s].distance_to(leg.clip_prev) / dt : 0;
		const bool still = dt > 0 ? speed < plant_speed : (!settled || leg.planted);
		leg.planted = lift < plant_height && still;
		if (dt > 0 || !leg.clip_prev_valid) {
			leg.clip_prev = clip[s];
			leg.clip_prev_valid = true;
		}

		real_t height = 0;
		Vector3 normal = up;
		leg.on_ground = _sample_ground(Vector3(clip[s].x, clip[s].y - ankle[s], clip[s].z), height, normal);
		Vector3 target = clip[s];
		if (leg.on_ground) {
			leg.ground = height;
			const Vector3 sole(clip[s].x, height, clip[s].z);
			if (leg.planted) {
				const Vector3 here = sole + normal * ankle[s];
				const Vector2 off(here.x - leg.lock.x, here.z - leg.lock.z);
				if (!leg.locked || off.length() > release_distance) {
					leg.lock = here;
					leg.locked = true;
					if (walker && settled) {
						WorldWalkerStep step;
						const Vector3 mid = sole + forward * sole_forward;
						step.position = Vector3(mid.x, height, mid.z);
						step.yaw = Math::atan2(-forward.z, forward.x);
						step.side = s;
						WorldFootfalls::publish(walker_id, step);
					}
				}
				target = leg.lock;
			} else {
				leg.locked = false;
				// Swinging: the clip's lift over this ground, never into it.
				target.y = MAX(clip[s].y + (height - floor), height + ankle[s]);
			}
		} else {
			leg.locked = false;
		}
		if (!settled) {
			leg.spring.reset(target);
			leg.normal_spring.reset(normal);
		}
		leg.target = leg.spring.update(dt, target);
		leg.normal = leg.normal_spring.update(dt, leg.on_ground ? normal : up).normalized();
		needed[s] = leg.target.y - clip[s].y;
	}

	// The pelvis comes down to reach the lower foot (the other leg bends), never up.
	const real_t drop = CLAMP(MIN(needed[0], needed[1]), -max_pelvis_drop, (real_t)0.0);
	if (!settled) {
		pelvis_spring.reset(drop);
		settled = true;
	}
	pelvis_drop = pelvis_spring.update(dt, drop);
	if (hips >= 0) {
		Transform3D hp = skeleton->get_bone_global_pose(hips);
		hp.origin += sk_inverse.basis.xform(up * pelvis_drop + frame.xform(pelvis_offset));
		skeleton->set_bone_global_pose(hips, hp);
	}

	const Vector3 up_sk = sk_inverse.basis.xform(up).normalized();
	for (int s = 0; s < 2; s++) {
		const Leg &leg = legs[s];
		Basis end_basis = clip_basis[s];
		if (leg.on_ground) {
			// Tilted to the slope, fully while planted, fading out as the clip lifts the foot.
			const real_t lift = clip[s].y - floor - ankle[s];
			const real_t w = (real_t)1.0 - CLAMP(lift / (4 * plant_height), (real_t)0.0, (real_t)1.0);
			const Vector3 n_sk = sk_inverse.basis.xform(leg.normal).normalized();
			const Vector3 axis = up_sk.cross(n_sk);
			const real_t angle = MIN(up_sk.angle_to(n_sk), max_foot_tilt) * w;
			if (axis.length_squared() > (real_t)1e-12 && angle > 0) {
				end_basis = Basis(axis.normalized(), angle) * end_basis;
			}
		}
		_solve_two_joint(skeleton, bones[s][0], bones[s][1], bones[s][2], sk_inverse.xform(leg.target), end_basis);
	}
}

void FootPlacementModifier3D::_notification(int p_what) {
	if (p_what == NOTIFICATION_EXIT_TREE && walker_id.is_valid()) {
		WorldFootfalls::remove(walker_id);
		Object *walker = ObjectDB::get_instance(walker_id);
		if (walker) {
			walker->remove_meta(SNAME("world_contact_feet"));
		}
		walker_id = ObjectID();
	}
}

void FootPlacementModifier3D::reset() {
	settled = false;
	for (Leg &leg : legs) {
		leg.locked = false;
		leg.planted = false;
		leg.clip_prev_valid = false;
		leg.lowest = -1.0;
	}
}

void FootPlacementModifier3D::set_hips_bone(const String &p_bone) {
	hips_bone = p_bone;
}
String FootPlacementModifier3D::get_hips_bone() const {
	return hips_bone;
}
void FootPlacementModifier3D::set_thigh_bone(Side p_side, const String &p_bone) {
	ERR_FAIL_INDEX((int)p_side, 2);
	legs[p_side].thigh = p_bone;
}
String FootPlacementModifier3D::get_thigh_bone(Side p_side) const {
	ERR_FAIL_INDEX_V((int)p_side, 2, String());
	return legs[p_side].thigh;
}
void FootPlacementModifier3D::set_shin_bone(Side p_side, const String &p_bone) {
	ERR_FAIL_INDEX((int)p_side, 2);
	legs[p_side].shin = p_bone;
}
String FootPlacementModifier3D::get_shin_bone(Side p_side) const {
	ERR_FAIL_INDEX_V((int)p_side, 2, String());
	return legs[p_side].shin;
}
void FootPlacementModifier3D::set_foot_bone(Side p_side, const String &p_bone) {
	ERR_FAIL_INDEX((int)p_side, 2);
	legs[p_side].foot = p_bone;
}
String FootPlacementModifier3D::get_foot_bone(Side p_side) const {
	ERR_FAIL_INDEX_V((int)p_side, 2, String());
	return legs[p_side].foot;
}

#define FOOT_ACCESSORS(m_type, m_get_type, m_name, m_getter, m_setter_body) \
	void FootPlacementModifier3D::set_##m_name(m_type p_value) {            \
		m_setter_body;                                                      \
	}                                                                       \
	m_get_type FootPlacementModifier3D::m_getter() const {                  \
		return m_name;                                                      \
	}

FOOT_ACCESSORS(const NodePath &, NodePath, reference_node, get_reference_node, reference_node = p_value)
FOOT_ACCESSORS(const NodePath &, NodePath, walker_node, get_walker_node, walker_node = p_value)
FOOT_ACCESSORS(const Vector3 &, Vector3, pelvis_offset, get_pelvis_offset, pelvis_offset = p_value)
FOOT_ACCESSORS(real_t, real_t, ankle_height, get_ankle_height, ankle_height = p_value)
FOOT_ACCESSORS(real_t, real_t, probe_up, get_probe_up, probe_up = MAX(p_value, (real_t)0.0))
FOOT_ACCESSORS(real_t, real_t, probe_down, get_probe_down, probe_down = MAX(p_value, (real_t)0.0))
FOOT_ACCESSORS(real_t, real_t, max_pelvis_drop, get_max_pelvis_drop, max_pelvis_drop = MAX(p_value, (real_t)0.0))
FOOT_ACCESSORS(real_t, real_t, max_foot_tilt, get_max_foot_tilt, max_foot_tilt = CLAMP(p_value, (real_t)0.0, (real_t)Math::PI * (real_t)0.5))
FOOT_ACCESSORS(real_t, real_t, plant_speed, get_plant_speed, plant_speed = MAX(p_value, (real_t)0.0))
FOOT_ACCESSORS(real_t, real_t, plant_height, get_plant_height, plant_height = MAX(p_value, (real_t)0.001))
FOOT_ACCESSORS(real_t, real_t, release_distance, get_release_distance, release_distance = MAX(p_value, (real_t)0.0))
FOOT_ACCESSORS(real_t, real_t, sole_forward, get_sole_forward, sole_forward = p_value)
FOOT_ACCESSORS(uint32_t, uint32_t, collision_mask, get_collision_mask, collision_mask = p_value)
FOOT_ACCESSORS(bool, bool, use_world_ground, is_using_world_ground, use_world_ground = p_value)
FOOT_ACCESSORS(bool, bool, publish_footfalls, is_publishing_footfalls, publish_footfalls = p_value)
FOOT_ACCESSORS(real_t, real_t, pelvis_frequency, get_pelvis_frequency, pelvis_frequency = MAX(p_value, (real_t)0.01); _configure_springs())
FOOT_ACCESSORS(real_t, real_t, foot_frequency, get_foot_frequency, foot_frequency = MAX(p_value, (real_t)0.01); _configure_springs())

#undef FOOT_ACCESSORS

bool FootPlacementModifier3D::is_foot_planted(Side p_side) const {
	ERR_FAIL_INDEX_V((int)p_side, 2, false);
	return legs[p_side].locked;
}
Vector3 FootPlacementModifier3D::get_foot_target(Side p_side) const {
	ERR_FAIL_INDEX_V((int)p_side, 2, Vector3());
	return legs[p_side].target;
}
real_t FootPlacementModifier3D::get_pelvis_drop() const {
	return pelvis_drop;
}

void FootPlacementModifier3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_hips_bone", "bone"), &FootPlacementModifier3D::set_hips_bone);
	ClassDB::bind_method(D_METHOD("get_hips_bone"), &FootPlacementModifier3D::get_hips_bone);
	ClassDB::bind_method(D_METHOD("set_thigh_bone", "side", "bone"), &FootPlacementModifier3D::set_thigh_bone);
	ClassDB::bind_method(D_METHOD("get_thigh_bone", "side"), &FootPlacementModifier3D::get_thigh_bone);
	ClassDB::bind_method(D_METHOD("set_shin_bone", "side", "bone"), &FootPlacementModifier3D::set_shin_bone);
	ClassDB::bind_method(D_METHOD("get_shin_bone", "side"), &FootPlacementModifier3D::get_shin_bone);
	ClassDB::bind_method(D_METHOD("set_foot_bone", "side", "bone"), &FootPlacementModifier3D::set_foot_bone);
	ClassDB::bind_method(D_METHOD("get_foot_bone", "side"), &FootPlacementModifier3D::get_foot_bone);
#define FOOT_BIND(m_name, m_getter)                                                                     \
	ClassDB::bind_method(D_METHOD("set_" #m_name, #m_name), &FootPlacementModifier3D::set_##m_name); \
	ClassDB::bind_method(D_METHOD(#m_getter), &FootPlacementModifier3D::m_getter);
	FOOT_BIND(left_thigh_bone, get_left_thigh_bone)
	FOOT_BIND(left_shin_bone, get_left_shin_bone)
	FOOT_BIND(left_foot_bone, get_left_foot_bone)
	FOOT_BIND(right_thigh_bone, get_right_thigh_bone)
	FOOT_BIND(right_shin_bone, get_right_shin_bone)
	FOOT_BIND(right_foot_bone, get_right_foot_bone)
	FOOT_BIND(reference_node, get_reference_node)
	FOOT_BIND(walker_node, get_walker_node)
	FOOT_BIND(pelvis_offset, get_pelvis_offset)
	FOOT_BIND(ankle_height, get_ankle_height)
	FOOT_BIND(probe_up, get_probe_up)
	FOOT_BIND(probe_down, get_probe_down)
	FOOT_BIND(max_pelvis_drop, get_max_pelvis_drop)
	FOOT_BIND(max_foot_tilt, get_max_foot_tilt)
	FOOT_BIND(plant_speed, get_plant_speed)
	FOOT_BIND(plant_height, get_plant_height)
	FOOT_BIND(release_distance, get_release_distance)
	FOOT_BIND(sole_forward, get_sole_forward)
	FOOT_BIND(collision_mask, get_collision_mask)
	FOOT_BIND(use_world_ground, is_using_world_ground)
	FOOT_BIND(publish_footfalls, is_publishing_footfalls)
	FOOT_BIND(pelvis_frequency, get_pelvis_frequency)
	FOOT_BIND(foot_frequency, get_foot_frequency)
#undef FOOT_BIND
	ClassDB::bind_method(D_METHOD("is_foot_planted", "side"), &FootPlacementModifier3D::is_foot_planted);
	ClassDB::bind_method(D_METHOD("get_foot_target", "side"), &FootPlacementModifier3D::get_foot_target);
	ClassDB::bind_method(D_METHOD("get_pelvis_drop"), &FootPlacementModifier3D::get_pelvis_drop);
	ClassDB::bind_method(D_METHOD("reset"), &FootPlacementModifier3D::reset);

	ADD_GROUP("Bones", "");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "hips_bone"), "set_hips_bone", "get_hips_bone");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "left_thigh_bone"), "set_left_thigh_bone", "get_left_thigh_bone");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "left_shin_bone"), "set_left_shin_bone", "get_left_shin_bone");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "left_foot_bone"), "set_left_foot_bone", "get_left_foot_bone");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "right_thigh_bone"), "set_right_thigh_bone", "get_right_thigh_bone");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "right_shin_bone"), "set_right_shin_bone", "get_right_shin_bone");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "right_foot_bone"), "set_right_foot_bone", "get_right_foot_bone");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ankle_height", PROPERTY_HINT_RANGE, "-1,0.5,0.001,suffix:m"), "set_ankle_height", "get_ankle_height");

	ADD_GROUP("Body", "");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "reference_node", PROPERTY_HINT_NODE_TYPE, "Node3D"), "set_reference_node", "get_reference_node");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "walker_node", PROPERTY_HINT_NODE_TYPE, "Node3D"), "set_walker_node", "get_walker_node");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "pelvis_offset", PROPERTY_HINT_NONE, "suffix:m"), "set_pelvis_offset", "get_pelvis_offset");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "max_pelvis_drop", PROPERTY_HINT_RANGE, "0,1,0.001,suffix:m"), "set_max_pelvis_drop", "get_max_pelvis_drop");

	ADD_GROUP("Ground", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "use_world_ground"), "set_use_world_ground", "is_using_world_ground");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_mask", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_collision_mask", "get_collision_mask");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "probe_up", PROPERTY_HINT_RANGE, "0,2,0.01,suffix:m"), "set_probe_up", "get_probe_up");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "probe_down", PROPERTY_HINT_RANGE, "0,2,0.01,suffix:m"), "set_probe_down", "get_probe_down");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "max_foot_tilt", PROPERTY_HINT_RANGE, "0,90,0.1,radians_as_degrees"), "set_max_foot_tilt", "get_max_foot_tilt");

	ADD_GROUP("Planting", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "plant_speed", PROPERTY_HINT_RANGE, "0,2,0.01,suffix:m/s"), "set_plant_speed", "get_plant_speed");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "plant_height", PROPERTY_HINT_RANGE, "0.001,0.5,0.001,suffix:m"), "set_plant_height", "get_plant_height");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "release_distance", PROPERTY_HINT_RANGE, "0,1,0.01,suffix:m"), "set_release_distance", "get_release_distance");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "sole_forward", PROPERTY_HINT_RANGE, "-0.2,0.2,0.001,suffix:m"), "set_sole_forward", "get_sole_forward");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "publish_footfalls"), "set_publish_footfalls", "is_publishing_footfalls");

	ADD_GROUP("Dynamics", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "pelvis_frequency", PROPERTY_HINT_RANGE, "0.01,20,0.01,suffix:Hz"), "set_pelvis_frequency", "get_pelvis_frequency");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "foot_frequency", PROPERTY_HINT_RANGE, "0.01,30,0.01,suffix:Hz"), "set_foot_frequency", "get_foot_frequency");

	BIND_ENUM_CONSTANT(SIDE_LEFT);
	BIND_ENUM_CONSTANT(SIDE_RIGHT);
}

FootPlacementModifier3D::FootPlacementModifier3D() {
	_configure_springs();
}

FootPlacementModifier3D::~FootPlacementModifier3D() {
	if (walker_id.is_valid()) {
		WorldFootfalls::remove(walker_id);
	}
}

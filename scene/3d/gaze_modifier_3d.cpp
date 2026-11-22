/**************************************************************************/
/*  gaze_modifier_3d.cpp                                                  */
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

#include "gaze_modifier_3d.h"

#include "core/object/class_db.h"
#include "scene/3d/skeleton_3d.h"

namespace {

// An orthonormal character frame from a basis: +Z forward, +Y up, +X the character's left.
Basis character_frame(const Basis &p_basis) {
	Vector3 fwd = p_basis.get_column(2).normalized();
	Vector3 up = p_basis.get_column(1);
	Vector3 left = up.cross(fwd);
	if (left.length_squared() < (real_t)1e-12 || fwd.length_squared() < (real_t)1e-12) {
		return Basis();
	}
	left.normalize();
	up = fwd.cross(left).normalized();
	return Basis(left, up, fwd);
}

Vector2 clamp_angles(const Vector2 &p_angles, real_t p_yaw, real_t p_up, real_t p_down) {
	return Vector2(CLAMP(p_angles.x, -p_yaw, p_yaw), CLAMP(p_angles.y, -p_up, p_down));
}

} // namespace

void GazeModifier3D::_configure_springs() {
	head_spring.configure(head_frequency, head_damping, head_response);
	eye_spring.configure(eye_frequency, eye_damping, eye_response);
	fade_spring.configure(fade_frequency, 1.0, 0.0);
}

bool GazeModifier3D::_target_global(Vector3 &r_target) const {
	switch (target_mode) {
		case TARGET_MODE_NODE: {
			const Node3D *node = Object::cast_to<Node3D>(get_node_or_null(target_node));
			if (!node || !node->is_inside_tree()) {
				return false;
			}
			r_target = const_cast<Node3D *>(node)->get_global_transform_interpolated().origin;
			return true;
		}
		case TARGET_MODE_POSITION:
			r_target = target_position;
			return true;
		default:
			return false;
	}
}

// The character frame in the skeleton's space: the reference node's (its +Z faces forward), or the
// skeleton's own when there is none. Rigs whose armature is turned under the character's node then
// aim right with no per-rig correction.
Basis GazeModifier3D::_character_basis(Skeleton3D *p_skeleton) const {
	const Node3D *reference = Object::cast_to<Node3D>(get_node_or_null(reference_node));
	if (!reference || reference == p_skeleton || !reference->is_inside_tree()) {
		return Basis();
	}
	const Basis skeleton_basis = p_skeleton->get_global_transform_interpolated().basis.orthonormalized();
	const Basis reference_basis = const_cast<Node3D *>(reference)->get_global_transform_interpolated().basis.orthonormalized();
	return character_frame(skeleton_basis.inverse() * reference_basis);
}

// Yaw (left positive) and pitch (down positive) of a direction in the character frame.
Vector2 GazeModifier3D::_angles_of(const Basis &p_character, const Vector3 &p_dir) {
	const Vector3 d = p_character.xform_inv(p_dir);
	return Vector2(Math::atan2(d.x, d.z), Math::atan2(-d.y, Vector2(d.x, d.z).length()));
}

// The bone's forward in its own frame: the character's forward as the rest pose faces it.
Vector3 GazeModifier3D::_rest_forward(Skeleton3D *p_skeleton, int p_bone, const Basis &p_character) const {
	return p_skeleton->get_bone_global_rest(p_bone).basis.inverse().xform(p_character.get_column(2)).normalized();
}

// Turns a bone about its own origin by `p_yaw` about the character's up, then `p_pitch` about the
// character's left as it lies at azimuth `p_facing_yaw` (where the head faces after the yaw), so the
// look's elevation changes without rolling it.
void GazeModifier3D::_turn_bone(Skeleton3D *p_skeleton, int p_bone, const Basis &p_character, real_t p_yaw, real_t p_pitch, real_t p_facing_yaw) {
	if (p_bone < 0 || (Math::is_zero_approx(p_yaw) && Math::is_zero_approx(p_pitch))) {
		return;
	}
	const Vector3 up = p_character.get_column(1);
	const Vector3 left = Basis(up, p_facing_yaw).xform(p_character.get_column(0));
	const Basis turn = Basis(left, p_pitch) * Basis(up, p_yaw);
	Transform3D pose = p_skeleton->get_bone_global_pose(p_bone);
	pose.basis = turn * pose.basis;
	p_skeleton->set_bone_global_pose(p_bone, pose);
}

Vector2 GazeModifier3D::_delayed_target() const {
	if (history.is_empty()) {
		return target_angles;
	}
	const double at = time - head_latency;
	Vector2 angles = history[0].angles;
	for (uint32_t i = 0; i < history.size(); i++) {
		if (history[i].time > at) {
			break;
		}
		angles = history[i].angles;
	}
	return angles;
}

Vector2 GazeModifier3D::angles_to(const Vector3 &p_global_position) const {
	Skeleton3D *skeleton = get_skeleton();
	if (!skeleton) {
		return Vector2();
	}
	const int head = skeleton->find_bone(head_bone);
	const Transform3D skeleton_global = skeleton->get_global_transform_interpolated();
	const Basis character = _character_basis(skeleton);
	const Basis character_global = character_frame(skeleton_global.basis.orthonormalized() * character);
	Vector3 eyes = skeleton_global.origin;
	if (head >= 0) {
		const int le = skeleton->find_bone(left_eye_bone);
		const int re = skeleton->find_bone(right_eye_bone);
		if (le >= 0 && re >= 0) {
			eyes = skeleton_global.xform((skeleton->get_bone_global_pose(le).origin + skeleton->get_bone_global_pose(re).origin) * (real_t)0.5);
		} else {
			eyes = skeleton_global.xform(skeleton->get_bone_global_pose(head).origin) + character_global.xform(eye_offset);
		}
	}
	return _angles_of(character_global, p_global_position - eyes);
}

void GazeModifier3D::_process_modification(double p_delta) {
	Skeleton3D *skeleton = get_skeleton();
	if (!skeleton) {
		return;
	}
	const int head = skeleton->find_bone(head_bone);
	if (head < 0) {
		return;
	}
	const int neck = skeleton->find_bone(neck_bone);
	const int left_eye = skeleton->find_bone(left_eye_bone);
	const int right_eye = skeleton->find_bone(right_eye_bone);
	const real_t dt = (real_t)MAX(p_delta, 0.0);

	const Basis character = _character_basis(skeleton);
	const Transform3D skeleton_global = skeleton->get_global_transform_interpolated();
	const Basis character_global = character_frame(skeleton_global.basis.orthonormalized() * character);

	// Where to look, from the eyes, in the character's frame.
	Vector3 target;
	bool look = _target_global(target);
	Vector2 want;
	if (look) {
		want = angles_to(target);
		if (Math::abs(want.x) > ignore_angle) {
			look = false; // far round behind them: not chased
			want = Vector2();
		} else {
			want = clamp_angles(want, head_yaw_limit + (spine_bones.is_empty() ? (real_t)0.0 : spine_yaw_limit), head_up_limit, head_down_limit);
		}
	}
	target_angles = want;

	time += dt;
	if (!settled) {
		// Already looking when first seen: start there, so a scene shown with the clock stopped (a
		// path-traced still) shows the look rather than the bare clip.
		settled = true;
		head_spring.reset(want);
		eye_spring.reset(want);
		fade_spring.reset(look ? 1.0 : 0.0);
		history.clear();
	}
	if (dt > 0) {
		history.push_back({ time, want });
		const double keep = time - head_latency - 0.5;
		while (history.size() > 2 && history[1].time < keep) {
			history.remove_at(0);
		}
	}

	Vector2 head_goal = head_spring.update(dt, _delayed_target());
	const Vector2 eye_goal = eye_spring.update(dt, want);
	const real_t k = CLAMP(fade_spring.update(dt, look ? (real_t)1.0 : (real_t)0.0), (real_t)0.0, (real_t)1.0);
	look_weight = k;
	if (k <= (real_t)1e-4) {
		head_angles = _angles_of(character, skeleton->get_bone_global_pose(head).basis.xform(_rest_forward(skeleton, head, character)));
		return;
	}
	if (drift_amplitude > 0) {
		// Quasi-periodic drift: incommensurate sines in the accumulated time only, so a stopped clock holds.
		const real_t w = (real_t)Math::TAU * drift_frequency;
		const real_t t = (real_t)time;
		const real_t s = (real_t)(drift_seed % 1000) * (real_t)0.731;
		head_goal += Vector2(
							 (real_t)0.6 * Math::sin(w * t + s) + (real_t)0.4 * Math::sin(w * (real_t)1.618 * t + s * (real_t)2.3),
							 (real_t)0.6 * Math::sin(w * (real_t)0.83 * t + s * (real_t)1.7) + (real_t)0.4 * Math::sin(w * (real_t)2.13 * t + s * (real_t)0.4)) *
				drift_amplitude;
	}

	// Where the clip has the head facing now.
	const Vector3 head_forward = _rest_forward(skeleton, head, character);
	const Vector2 clip = _angles_of(character, skeleton->get_bone_global_pose(head).basis.xform(head_forward));
	const real_t yaw_change = k * (real_t)Math::angle_difference(clip.x, head_goal.x);
	const real_t pitch_change = k * (head_goal.y - clip.y);

	// The upper back takes only the part of the turn past where the head is comfortable.
	real_t spine_yaw = 0;
	if (!spine_bones.is_empty()) {
		spine_yaw = k * SIGN(head_goal.x) * CLAMP(Math::abs(head_goal.x) - spine_start_angle, (real_t)0.0, spine_yaw_limit);
		real_t total = 0;
		for (int i = 0; i < spine_bones.size(); i++) {
			total += i < spine_shares.size() ? MAX(spine_shares[i], 0.0f) : 1.0f;
		}
		if (total > 0) {
			for (int i = 0; i < spine_bones.size(); i++) {
				const real_t share = (i < spine_shares.size() ? MAX(spine_shares[i], 0.0f) : 1.0f) / total;
				_turn_bone(skeleton, skeleton->find_bone(spine_bones[i]), character, spine_yaw * share, 0, 0);
			}
		}
	}

	// Neck and head carry the rest.
	const real_t head_yaw = yaw_change - spine_yaw;
	const real_t neck_part = neck >= 0 ? CLAMP(neck_share, (real_t)0.0, (real_t)1.0) : (real_t)0.0;
	_turn_bone(skeleton, neck, character, head_yaw * neck_part, pitch_change * neck_part, clip.x + spine_yaw + head_yaw * neck_part);
	_turn_bone(skeleton, head, character, head_yaw * (1 - neck_part), pitch_change * (1 - neck_part), clip.x + yaw_change);
	head_angles = _angles_of(character, skeleton->get_bone_global_pose(head).basis.xform(head_forward));

	// The eyes lead: what is left between the head and the fast eye goal, within their range, plus each
	// eye's own convergence on a near target.
	const int eyes[2] = { left_eye, right_eye };
	const Vector2 relative = clamp_angles(Vector2((real_t)Math::angle_difference(head_angles.x, eye_goal.x), eye_goal.y - head_angles.y), eye_yaw_limit, eye_up_limit, eye_down_limit);
	Vector3 mid;
	if (left_eye >= 0 && right_eye >= 0) {
		mid = skeleton_global.xform((skeleton->get_bone_global_pose(left_eye).origin + skeleton->get_bone_global_pose(right_eye).origin) * (real_t)0.5);
	}
	for (int e = 0; e < 2; e++) {
		const int bone = eyes[e];
		if (bone < 0) {
			continue;
		}
		const Vector3 forward = _rest_forward(skeleton, bone, character);
		const Vector2 now = _angles_of(character, skeleton->get_bone_global_pose(bone).basis.xform(forward));
		Vector2 converge;
		if (look && left_eye >= 0 && right_eye >= 0) {
			const Vector3 at = skeleton_global.xform(skeleton->get_bone_global_pose(bone).origin);
			converge.x = (real_t)Math::angle_difference(_angles_of(character_global, target - mid).x, _angles_of(character_global, target - at).x);
		}
		const Vector2 goal = head_angles + relative + converge;
		_turn_bone(skeleton, bone, character, k * (real_t)Math::angle_difference(now.x, goal.x), k * (goal.y - now.y), goal.x);
	}
}

void GazeModifier3D::reset() {
	settled = false;
	history.clear();
}

void GazeModifier3D::set_target_mode(TargetMode p_mode) {
	target_mode = p_mode;
}
GazeModifier3D::TargetMode GazeModifier3D::get_target_mode() const {
	return target_mode;
}
void GazeModifier3D::set_target_node(const NodePath &p_node) {
	target_node = p_node;
}
NodePath GazeModifier3D::get_target_node() const {
	return target_node;
}
void GazeModifier3D::set_target_position(const Vector3 &p_position) {
	target_position = p_position;
}
Vector3 GazeModifier3D::get_target_position() const {
	return target_position;
}
void GazeModifier3D::set_reference_node(const NodePath &p_node) {
	reference_node = p_node;
}
NodePath GazeModifier3D::get_reference_node() const {
	return reference_node;
}

#define GAZE_ACCESSORS(m_type, m_get_type, m_name, m_setter_body) \
	void GazeModifier3D::set_##m_name(m_type p_value) { \
		m_setter_body; \
	} \
	m_get_type GazeModifier3D::get_##m_name() const { \
		return m_name; \
	}

GAZE_ACCESSORS(const PackedStringArray &, PackedStringArray, spine_bones, spine_bones = p_value)
GAZE_ACCESSORS(const PackedFloat32Array &, PackedFloat32Array, spine_shares, spine_shares = p_value)
GAZE_ACCESSORS(const String &, String, neck_bone, neck_bone = p_value)
GAZE_ACCESSORS(const String &, String, head_bone, head_bone = p_value)
GAZE_ACCESSORS(const String &, String, left_eye_bone, left_eye_bone = p_value)
GAZE_ACCESSORS(const String &, String, right_eye_bone, right_eye_bone = p_value)
GAZE_ACCESSORS(real_t, real_t, neck_share, neck_share = CLAMP(p_value, (real_t)0.0, (real_t)1.0))
GAZE_ACCESSORS(const Vector3 &, Vector3, eye_offset, eye_offset = p_value)
GAZE_ACCESSORS(real_t, real_t, head_yaw_limit, head_yaw_limit = MAX(p_value, (real_t)0.0))
GAZE_ACCESSORS(real_t, real_t, head_up_limit, head_up_limit = MAX(p_value, (real_t)0.0))
GAZE_ACCESSORS(real_t, real_t, head_down_limit, head_down_limit = MAX(p_value, (real_t)0.0))
GAZE_ACCESSORS(real_t, real_t, spine_start_angle, spine_start_angle = MAX(p_value, (real_t)0.0))
GAZE_ACCESSORS(real_t, real_t, spine_yaw_limit, spine_yaw_limit = MAX(p_value, (real_t)0.0))
GAZE_ACCESSORS(real_t, real_t, eye_yaw_limit, eye_yaw_limit = MAX(p_value, (real_t)0.0))
GAZE_ACCESSORS(real_t, real_t, eye_up_limit, eye_up_limit = MAX(p_value, (real_t)0.0))
GAZE_ACCESSORS(real_t, real_t, eye_down_limit, eye_down_limit = MAX(p_value, (real_t)0.0))
GAZE_ACCESSORS(real_t, real_t, ignore_angle, ignore_angle = CLAMP(p_value, (real_t)0.0, (real_t)Math::PI))
GAZE_ACCESSORS(real_t, real_t, head_frequency, head_frequency = MAX(p_value, (real_t)0.01); _configure_springs())
GAZE_ACCESSORS(real_t, real_t, head_damping, head_damping = MAX(p_value, (real_t)0.0); _configure_springs())
GAZE_ACCESSORS(real_t, real_t, head_response, head_response = p_value; _configure_springs())
GAZE_ACCESSORS(real_t, real_t, head_latency, head_latency = MAX(p_value, (real_t)0.0))
GAZE_ACCESSORS(real_t, real_t, eye_frequency, eye_frequency = MAX(p_value, (real_t)0.01); _configure_springs())
GAZE_ACCESSORS(real_t, real_t, eye_damping, eye_damping = MAX(p_value, (real_t)0.0); _configure_springs())
GAZE_ACCESSORS(real_t, real_t, eye_response, eye_response = p_value; _configure_springs())
GAZE_ACCESSORS(real_t, real_t, fade_frequency, fade_frequency = MAX(p_value, (real_t)0.01); _configure_springs())
GAZE_ACCESSORS(real_t, real_t, drift_amplitude, drift_amplitude = MAX(p_value, (real_t)0.0))
GAZE_ACCESSORS(real_t, real_t, drift_frequency, drift_frequency = MAX(p_value, (real_t)0.0))
GAZE_ACCESSORS(int, int, drift_seed, drift_seed = p_value)

#undef GAZE_ACCESSORS

Vector2 GazeModifier3D::get_target_angles() const {
	return target_angles;
}
Vector2 GazeModifier3D::get_head_angles() const {
	return head_angles;
}
real_t GazeModifier3D::get_look_weight() const {
	return look_weight;
}

void GazeModifier3D::_bind_methods() {
#define GAZE_BIND(m_name)                                                                              \
	ClassDB::bind_method(D_METHOD("set_" #m_name, #m_name), &GazeModifier3D::set_##m_name); \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &GazeModifier3D::get_##m_name);
	GAZE_BIND(target_mode)
	GAZE_BIND(target_node)
	GAZE_BIND(target_position)
	GAZE_BIND(reference_node)
	GAZE_BIND(spine_bones)
	GAZE_BIND(spine_shares)
	GAZE_BIND(neck_bone)
	GAZE_BIND(head_bone)
	GAZE_BIND(left_eye_bone)
	GAZE_BIND(right_eye_bone)
	GAZE_BIND(neck_share)
	GAZE_BIND(eye_offset)
	GAZE_BIND(head_yaw_limit)
	GAZE_BIND(head_up_limit)
	GAZE_BIND(head_down_limit)
	GAZE_BIND(spine_start_angle)
	GAZE_BIND(spine_yaw_limit)
	GAZE_BIND(eye_yaw_limit)
	GAZE_BIND(eye_up_limit)
	GAZE_BIND(eye_down_limit)
	GAZE_BIND(ignore_angle)
	GAZE_BIND(head_frequency)
	GAZE_BIND(head_damping)
	GAZE_BIND(head_response)
	GAZE_BIND(head_latency)
	GAZE_BIND(eye_frequency)
	GAZE_BIND(eye_damping)
	GAZE_BIND(eye_response)
	GAZE_BIND(fade_frequency)
	GAZE_BIND(drift_amplitude)
	GAZE_BIND(drift_frequency)
	GAZE_BIND(drift_seed)
#undef GAZE_BIND
	ClassDB::bind_method(D_METHOD("get_target_angles"), &GazeModifier3D::get_target_angles);
	ClassDB::bind_method(D_METHOD("get_head_angles"), &GazeModifier3D::get_head_angles);
	ClassDB::bind_method(D_METHOD("get_look_weight"), &GazeModifier3D::get_look_weight);
	ClassDB::bind_method(D_METHOD("angles_to", "global_position"), &GazeModifier3D::angles_to);
	ClassDB::bind_method(D_METHOD("reset"), &GazeModifier3D::reset);

	ADD_GROUP("Target", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "target_mode", PROPERTY_HINT_ENUM, "None,Node,Position"), "set_target_mode", "get_target_mode");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "target_node", PROPERTY_HINT_NODE_TYPE, "Node3D"), "set_target_node", "get_target_node");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "target_position", PROPERTY_HINT_NONE, "suffix:m"), "set_target_position", "get_target_position");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "reference_node", PROPERTY_HINT_NODE_TYPE, "Node3D"), "set_reference_node", "get_reference_node");

	ADD_GROUP("Bones", "");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_STRING_ARRAY, "spine_bones"), "set_spine_bones", "get_spine_bones");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_FLOAT32_ARRAY, "spine_shares"), "set_spine_shares", "get_spine_shares");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "neck_bone"), "set_neck_bone", "get_neck_bone");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "head_bone"), "set_head_bone", "get_head_bone");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "left_eye_bone"), "set_left_eye_bone", "get_left_eye_bone");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "right_eye_bone"), "set_right_eye_bone", "get_right_eye_bone");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "neck_share", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_neck_share", "get_neck_share");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "eye_offset", PROPERTY_HINT_NONE, "suffix:m"), "set_eye_offset", "get_eye_offset");

	ADD_GROUP("Limits", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "head_yaw_limit", PROPERTY_HINT_RANGE, "0,180,0.1,radians_as_degrees"), "set_head_yaw_limit", "get_head_yaw_limit");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "head_up_limit", PROPERTY_HINT_RANGE, "0,90,0.1,radians_as_degrees"), "set_head_up_limit", "get_head_up_limit");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "head_down_limit", PROPERTY_HINT_RANGE, "0,90,0.1,radians_as_degrees"), "set_head_down_limit", "get_head_down_limit");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "spine_start_angle", PROPERTY_HINT_RANGE, "0,180,0.1,radians_as_degrees"), "set_spine_start_angle", "get_spine_start_angle");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "spine_yaw_limit", PROPERTY_HINT_RANGE, "0,90,0.1,radians_as_degrees"), "set_spine_yaw_limit", "get_spine_yaw_limit");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "eye_yaw_limit", PROPERTY_HINT_RANGE, "0,90,0.1,radians_as_degrees"), "set_eye_yaw_limit", "get_eye_yaw_limit");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "eye_up_limit", PROPERTY_HINT_RANGE, "0,90,0.1,radians_as_degrees"), "set_eye_up_limit", "get_eye_up_limit");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "eye_down_limit", PROPERTY_HINT_RANGE, "0,90,0.1,radians_as_degrees"), "set_eye_down_limit", "get_eye_down_limit");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ignore_angle", PROPERTY_HINT_RANGE, "0,180,0.1,radians_as_degrees"), "set_ignore_angle", "get_ignore_angle");

	ADD_GROUP("Dynamics", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "head_frequency", PROPERTY_HINT_RANGE, "0.01,10,0.01,or_greater,suffix:Hz"), "set_head_frequency", "get_head_frequency");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "head_damping", PROPERTY_HINT_RANGE, "0,2,0.01,or_greater"), "set_head_damping", "get_head_damping");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "head_response", PROPERTY_HINT_RANGE, "-2,2,0.01"), "set_head_response", "get_head_response");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "head_latency", PROPERTY_HINT_RANGE, "0,1,0.01,suffix:s"), "set_head_latency", "get_head_latency");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "eye_frequency", PROPERTY_HINT_RANGE, "0.01,20,0.01,or_greater,suffix:Hz"), "set_eye_frequency", "get_eye_frequency");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "eye_damping", PROPERTY_HINT_RANGE, "0,2,0.01,or_greater"), "set_eye_damping", "get_eye_damping");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "eye_response", PROPERTY_HINT_RANGE, "-2,2,0.01"), "set_eye_response", "get_eye_response");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "fade_frequency", PROPERTY_HINT_RANGE, "0.01,10,0.01,or_greater,suffix:Hz"), "set_fade_frequency", "get_fade_frequency");

	ADD_GROUP("Drift", "drift_");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "drift_amplitude", PROPERTY_HINT_RANGE, "0,10,0.01,radians_as_degrees"), "set_drift_amplitude", "get_drift_amplitude");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "drift_frequency", PROPERTY_HINT_RANGE, "0,5,0.01,suffix:Hz"), "set_drift_frequency", "get_drift_frequency");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "drift_seed"), "set_drift_seed", "get_drift_seed");

	BIND_ENUM_CONSTANT(TARGET_MODE_NONE);
	BIND_ENUM_CONSTANT(TARGET_MODE_NODE);
	BIND_ENUM_CONSTANT(TARGET_MODE_POSITION);
}

GazeModifier3D::GazeModifier3D() {
	_configure_springs();
}

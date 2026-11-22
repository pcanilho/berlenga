/**************************************************************************/
/*  gaze_modifier_3d.h                                                    */
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

#pragma once

#include "core/templates/local_vector.h"
#include "scene/3d/skeleton_modifier_3d.h"
#include "scene/animation/second_order_dynamics.h"

// A look at a target shared over the upper back, neck, head and eyes, on top of whatever the clip does.
//
// It follows the head-eye coordination model of Andrist, Pejsa, Mutlu and Gleicher ("A Head-Eye
// Coordination Model for Animating Gaze Shifts of Virtual Characters", 2012) and the torso alignment of
// Pejsa et al. ("Gaze and Attention Management for Embodied Conversational Agents", 2015): the eyes lead
// and carry what fits in their range, the head follows a little later and carries the rest up to its
// comfortable range, and the upper back joins only for the part of the turn past that. Each part moves
// with second-order dynamics (SecondOrderSystem), so shifts start quickly, overshoot slightly and settle,
// and bigger shifts move faster, as real gaze shifts do (the "main sequence", Bahill, Clark and Stark
// 1975). A faint quasi-periodic drift keeps a held look alive. Everything advances with the modifier's
// delta and nothing else, so a stopped clock holds the pose exactly (path-traced stills).
class GazeModifier3D : public SkeletonModifier3D {
	GDCLASS(GazeModifier3D, SkeletonModifier3D);

public:
	enum TargetMode {
		TARGET_MODE_NONE,
		TARGET_MODE_NODE,
		TARGET_MODE_POSITION,
	};

private:
	TargetMode target_mode = TARGET_MODE_NODE;
	NodePath target_node;
	Vector3 target_position;
	NodePath reference_node;

	PackedStringArray spine_bones;
	PackedFloat32Array spine_shares;
	String neck_bone;
	String head_bone;
	String left_eye_bone;
	String right_eye_bone;
	real_t neck_share = 0.4;
	Vector3 eye_offset = Vector3(0.0, 0.09, 0.08);

	real_t head_yaw_limit = Math::deg_to_rad((real_t)70.0);
	real_t head_up_limit = Math::deg_to_rad((real_t)30.0);
	real_t head_down_limit = Math::deg_to_rad((real_t)42.0);
	real_t spine_start_angle = Math::deg_to_rad((real_t)50.0);
	real_t spine_yaw_limit = Math::deg_to_rad((real_t)22.0);
	real_t eye_yaw_limit = Math::deg_to_rad((real_t)30.0);
	real_t eye_up_limit = Math::deg_to_rad((real_t)20.0);
	real_t eye_down_limit = Math::deg_to_rad((real_t)25.0);
	real_t ignore_angle = Math::deg_to_rad((real_t)120.0);

	real_t head_frequency = 1.3;
	real_t head_damping = 0.72;
	real_t head_response = -0.08;
	real_t head_latency = 0.15;
	real_t eye_frequency = 4.5;
	real_t eye_damping = 0.9;
	real_t eye_response = 0.0;
	real_t fade_frequency = 0.7;

	real_t drift_amplitude = Math::deg_to_rad((real_t)0.8);
	real_t drift_frequency = 0.35;
	int drift_seed = 0;

	SecondOrderSystem<Vector2> head_spring;
	SecondOrderSystem<Vector2> eye_spring;
	SecondOrderSystem<real_t> fade_spring;
	struct Sample {
		double time = 0.0;
		Vector2 angles;
	};
	LocalVector<Sample> history;
	double time = 0.0;
	bool settled = false;

	Vector2 target_angles;
	Vector2 head_angles;
	real_t look_weight = 0.0;
	real_t spine_yaw_applied = 0.0;

	void _configure_springs();
	bool _target_global(Vector3 &r_target) const;
	Basis _character_basis(Skeleton3D *p_skeleton) const;
	static Vector2 _angles_of(const Basis &p_character, const Vector3 &p_dir);
	Vector3 _rest_forward(Skeleton3D *p_skeleton, int p_bone, const Basis &p_character) const;
	void _turn_bone(Skeleton3D *p_skeleton, int p_bone, const Basis &p_character, real_t p_yaw, real_t p_pitch, real_t p_facing_yaw);
	Vector2 _delayed_target() const;

protected:
	static void _bind_methods();
	virtual void _process_modification(double p_delta) override;

public:
	void set_target_mode(TargetMode p_mode);
	TargetMode get_target_mode() const;
	void set_target_node(const NodePath &p_node);
	NodePath get_target_node() const;
	void set_target_position(const Vector3 &p_position);
	Vector3 get_target_position() const;
	void set_reference_node(const NodePath &p_node);
	NodePath get_reference_node() const;

	void set_spine_bones(const PackedStringArray &p_bones);
	PackedStringArray get_spine_bones() const;
	void set_spine_shares(const PackedFloat32Array &p_shares);
	PackedFloat32Array get_spine_shares() const;
	void set_neck_bone(const String &p_bone);
	String get_neck_bone() const;
	void set_head_bone(const String &p_bone);
	String get_head_bone() const;
	void set_left_eye_bone(const String &p_bone);
	String get_left_eye_bone() const;
	void set_right_eye_bone(const String &p_bone);
	String get_right_eye_bone() const;
	void set_neck_share(real_t p_share);
	real_t get_neck_share() const;
	void set_eye_offset(const Vector3 &p_offset);
	Vector3 get_eye_offset() const;

	void set_head_yaw_limit(real_t p_angle);
	real_t get_head_yaw_limit() const;
	void set_head_up_limit(real_t p_angle);
	real_t get_head_up_limit() const;
	void set_head_down_limit(real_t p_angle);
	real_t get_head_down_limit() const;
	void set_spine_start_angle(real_t p_angle);
	real_t get_spine_start_angle() const;
	void set_spine_yaw_limit(real_t p_angle);
	real_t get_spine_yaw_limit() const;
	void set_eye_yaw_limit(real_t p_angle);
	real_t get_eye_yaw_limit() const;
	void set_eye_up_limit(real_t p_angle);
	real_t get_eye_up_limit() const;
	void set_eye_down_limit(real_t p_angle);
	real_t get_eye_down_limit() const;
	void set_ignore_angle(real_t p_angle);
	real_t get_ignore_angle() const;

	void set_head_frequency(real_t p_value);
	real_t get_head_frequency() const;
	void set_head_damping(real_t p_value);
	real_t get_head_damping() const;
	void set_head_response(real_t p_value);
	real_t get_head_response() const;
	void set_head_latency(real_t p_value);
	real_t get_head_latency() const;
	void set_eye_frequency(real_t p_value);
	real_t get_eye_frequency() const;
	void set_eye_damping(real_t p_value);
	real_t get_eye_damping() const;
	void set_eye_response(real_t p_value);
	real_t get_eye_response() const;
	void set_fade_frequency(real_t p_value);
	real_t get_fade_frequency() const;

	void set_drift_amplitude(real_t p_angle);
	real_t get_drift_amplitude() const;
	void set_drift_frequency(real_t p_value);
	real_t get_drift_frequency() const;
	void set_drift_seed(int p_seed);
	int get_drift_seed() const;

	Vector2 get_target_angles() const;
	Vector2 get_head_angles() const;
	real_t get_look_weight() const;
	real_t get_spine_yaw() const;
	Vector2 angles_to(const Vector3 &p_global_position) const;
	void reset();

	GazeModifier3D();
};

VARIANT_ENUM_CAST(GazeModifier3D::TargetMode);

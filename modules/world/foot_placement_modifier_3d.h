/**************************************************************************/
/*  foot_placement_modifier_3d.h                                          */
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

#include "scene/3d/skeleton_modifier_3d.h"
#include "scene/animation/second_order_dynamics.h"

// Feet on the real ground under whatever the clip does (docs/ideas/character-motion.md, gap 3).
//
// The established technique (Unreal's Foot Placement node, Unity's foot IK, Johansen 2009, "Automated
// Semi-Procedural Animation for Character Locomotion"): per leg, find the ground under the clip's foot, plant the
// foot there while the clip has it still (foot lock, so it never slides), let it follow the clip with the ground's
// offset while it swings, tilt it to the ground's slope within a limit, lower the pelvis to reach the lower foot,
// and solve each leg with analytic two-joint IK (Holden, "Simple Two Joint IK", 2017), keeping the clip's knee plane.
// The pelvis and foot targets move through second-order dynamics, so nothing pops.
//
// The ground is the world's: the deformed surface of the ground's producer (WorldGround.known_height_at: prints,
// ruts, mud), the snow on it (WorldSnow.depth_at), else a short physics ray around the foot. Each plant is also a
// footfall (WorldFootfalls) for the walker, so the ground, snow, sand and mud print where the feet really landed.
class FootPlacementModifier3D : public SkeletonModifier3D {
	GDCLASS(FootPlacementModifier3D, SkeletonModifier3D);

public:
	enum Side {
		SIDE_LEFT,
		SIDE_RIGHT,
	};

private:
	struct Leg {
		String thigh;
		String shin;
		String foot;
		bool planted = false;
		bool locked = false;
		Vector3 lock; // world, the ankle where the foot was planted
		Vector3 clip_prev; // world, the clip's ankle last frame
		bool clip_prev_valid = false;
		Vector3 target; // world, the ankle the IK aims at this frame
		Vector3 normal = Vector3(0, 1, 0);
		real_t ground = 0.0; // the ground height under the foot
		bool on_ground = false;
		real_t lowest = -1.0; // the lowest the clip has held this ankle over its floor (m; < 0: not seen yet)
		SecondOrderSystem<Vector3> spring;
		SecondOrderSystem<Vector3> normal_spring;
	};

	String hips_bone;
	Leg legs[2];
	NodePath reference_node;
	NodePath walker_node;
	Vector3 pelvis_offset;
	real_t ankle_height = -1.0; // m above the sole; < 0: from the rest pose
	real_t probe_up = 0.5;
	real_t probe_down = 0.6;
	real_t max_pelvis_drop = 0.35;
	real_t max_foot_tilt = Math::deg_to_rad((real_t)30.0);
	real_t plant_speed = 0.15;
	real_t plant_height = 0.06;
	real_t release_distance = 0.2;
	real_t sole_forward = 0.05;
	uint32_t collision_mask = 1;
	bool use_world_ground = true;
	bool publish_footfalls = true;
	real_t pelvis_frequency = 4.0;
	real_t foot_frequency = 9.0;

	SecondOrderSystem<real_t> pelvis_spring;
	real_t pelvis_drop = 0.0;
	bool settled = false;
	ObjectID walker_id;

	void _configure_springs();
	Node3D *_walker() const;
	bool _ground_at(const Vector3 &p_point, real_t &r_height) const;
	bool _sample_ground(const Vector3 &p_point, real_t &r_height, Vector3 &r_normal) const;
	static void _solve_two_joint(Skeleton3D *p_skeleton, int p_upper, int p_lower, int p_end, const Vector3 &p_target, const Basis &p_end_basis);
	real_t _ankle_height(Skeleton3D *p_skeleton, int p_foot, real_t p_lowest) const;

protected:
	static void _bind_methods();
	void _notification(int p_what);
	virtual void _process_modification(double p_delta) override;

public:
	void set_hips_bone(const String &p_bone);
	String get_hips_bone() const;
	void set_thigh_bone(Side p_side, const String &p_bone);
	String get_thigh_bone(Side p_side) const;
	void set_shin_bone(Side p_side, const String &p_bone);
	String get_shin_bone(Side p_side) const;
	void set_foot_bone(Side p_side, const String &p_bone);
	String get_foot_bone(Side p_side) const;

	void set_left_thigh_bone(const String &p_bone) { set_thigh_bone(SIDE_LEFT, p_bone); }
	String get_left_thigh_bone() const { return get_thigh_bone(SIDE_LEFT); }
	void set_left_shin_bone(const String &p_bone) { set_shin_bone(SIDE_LEFT, p_bone); }
	String get_left_shin_bone() const { return get_shin_bone(SIDE_LEFT); }
	void set_left_foot_bone(const String &p_bone) { set_foot_bone(SIDE_LEFT, p_bone); }
	String get_left_foot_bone() const { return get_foot_bone(SIDE_LEFT); }
	void set_right_thigh_bone(const String &p_bone) { set_thigh_bone(SIDE_RIGHT, p_bone); }
	String get_right_thigh_bone() const { return get_thigh_bone(SIDE_RIGHT); }
	void set_right_shin_bone(const String &p_bone) { set_shin_bone(SIDE_RIGHT, p_bone); }
	String get_right_shin_bone() const { return get_shin_bone(SIDE_RIGHT); }
	void set_right_foot_bone(const String &p_bone) { set_foot_bone(SIDE_RIGHT, p_bone); }
	String get_right_foot_bone() const { return get_foot_bone(SIDE_RIGHT); }

	void set_reference_node(const NodePath &p_node);
	NodePath get_reference_node() const;
	void set_walker_node(const NodePath &p_node);
	NodePath get_walker_node() const;
	void set_pelvis_offset(const Vector3 &p_offset);
	Vector3 get_pelvis_offset() const;
	void set_ankle_height(real_t p_height);
	real_t get_ankle_height() const;
	void set_probe_up(real_t p_distance);
	real_t get_probe_up() const;
	void set_probe_down(real_t p_distance);
	real_t get_probe_down() const;
	void set_max_pelvis_drop(real_t p_distance);
	real_t get_max_pelvis_drop() const;
	void set_max_foot_tilt(real_t p_angle);
	real_t get_max_foot_tilt() const;
	void set_plant_speed(real_t p_speed);
	real_t get_plant_speed() const;
	void set_plant_height(real_t p_height);
	real_t get_plant_height() const;
	void set_release_distance(real_t p_distance);
	real_t get_release_distance() const;
	void set_sole_forward(real_t p_distance);
	real_t get_sole_forward() const;
	void set_collision_mask(uint32_t p_mask);
	uint32_t get_collision_mask() const;
	void set_use_world_ground(bool p_enable);
	bool is_using_world_ground() const;
	void set_publish_footfalls(bool p_enable);
	bool is_publishing_footfalls() const;
	void set_pelvis_frequency(real_t p_frequency);
	real_t get_pelvis_frequency() const;
	void set_foot_frequency(real_t p_frequency);
	real_t get_foot_frequency() const;

	bool is_foot_planted(Side p_side) const;
	Vector3 get_foot_target(Side p_side) const;
	real_t get_pelvis_drop() const;
	void reset();

	FootPlacementModifier3D();
	~FootPlacementModifier3D();
};

VARIANT_ENUM_CAST(FootPlacementModifier3D::Side);

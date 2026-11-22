/**************************************************************************/
/*  test_foot_placement_modifier_3d.cpp                                   */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_foot_placement_modifier_3d)

#include "modules/modules_enabled.gen.h"

#if !defined(_3D_DISABLED) && defined(MODULE_WORLD_ENABLED)

#include "modules/world/foot_placement_modifier_3d.h"
#include "modules/world/world_contracts.h"
#include "modules/world/world_walker_steps.h"
#include "scene/3d/bone_attachment_3d.h"
#include "scene/3d/skeleton_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"

namespace TestFootPlacementModifier3D {

// Ground for the tests: a step up of `step` for x > 0, else `base`.
class StepGround : public WorldGroundProvider {
public:
	float base = 0.0f;
	float step = 0.0f;
	virtual float contract_ground_height_at(const Vector3 &p_position) const override { return p_position.x > 0.0f ? base + step : base; }
	virtual WorldGroundSoil contract_ground_soil_at(const Vector3 &p_position) const override { return WorldGroundSoil(); }
};

static Skeleton3D *make_legs(SceneTree *p_tree) {
	Skeleton3D *sk = memnew(Skeleton3D);
	p_tree->get_root()->add_child(sk);
	sk->set_modifier_callback_mode_process(Skeleton3D::MODIFIER_CALLBACK_MODE_PROCESS_MANUAL);
	struct B {
		const char *name;
		const char *parent;
		Vector3 offset;
	};
	// Knees a touch forward so each leg has a bend plane; the ankles 0.08 m up at rest.
	const B bones[] = {
		{ "Hips", "", Vector3(0, 0.95, 0) },
		{ "LeftUpLeg", "Hips", Vector3(0.15, -0.05, 0) },
		{ "LeftLeg", "LeftUpLeg", Vector3(0, -0.42, 0.03) },
		{ "LeftFoot", "LeftLeg", Vector3(0, -0.40, -0.03) },
		{ "RightUpLeg", "Hips", Vector3(-0.15, -0.05, 0) },
		{ "RightLeg", "RightUpLeg", Vector3(0, -0.42, 0.03) },
		{ "RightFoot", "RightLeg", Vector3(0, -0.40, -0.03) },
	};
	for (const B &b : bones) {
		const int i = sk->add_bone(b.name);
		if (String(b.parent) != "") {
			sk->set_bone_parent(i, sk->find_bone(b.parent));
		}
		sk->set_bone_rest(i, Transform3D(Basis(), b.offset));
		sk->set_bone_pose(i, Transform3D(Basis(), b.offset));
	}
	return sk;
}

static FootPlacementModifier3D *make_feet(Skeleton3D *p_sk) {
	FootPlacementModifier3D *f = memnew(FootPlacementModifier3D);
	f->set_hips_bone("Hips");
	f->set_left_thigh_bone("LeftUpLeg");
	f->set_left_shin_bone("LeftLeg");
	f->set_left_foot_bone("LeftFoot");
	f->set_right_thigh_bone("RightUpLeg");
	f->set_right_shin_bone("RightLeg");
	f->set_right_foot_bone("RightFoot");
	p_sk->add_child(f);
	return f;
}

static BoneAttachment3D *attach(Skeleton3D *p_sk, const String &p_bone) {
	BoneAttachment3D *a = memnew(BoneAttachment3D);
	p_sk->add_child(a);
	a->set_bone_name(p_bone);
	return a;
}

static void step(Skeleton3D *p_sk, double p_dt) {
	p_sk->advance(p_dt);
	p_sk->notification(Skeleton3D::NOTIFICATION_UPDATE_SKELETON);
}

TEST_CASE("[SceneTree][FootPlacementModifier3D] Each foot stands on the world ground under it") {
	StepGround ground;
	ground.step = 0.15f;
	WorldContracts::register_ground(&ground);
	SceneTree *tree = SceneTree::get_singleton();
	Skeleton3D *sk = make_legs(tree);
	FootPlacementModifier3D *f = make_feet(sk);
	BoneAttachment3D *left = attach(sk, "LeftFoot");
	BoneAttachment3D *right = attach(sk, "RightFoot");
	for (int i = 0; i < 120; i++) {
		step(sk, 1.0 / 60.0);
	}
	CHECK_MESSAGE(f->is_foot_planted(FootPlacementModifier3D::SIDE_LEFT), "A still foot on the ground is planted.");
	CHECK_MESSAGE(Math::is_equal_approx(left->get_global_position().y, (real_t)0.23, (real_t)0.005), "The left foot is up on the step (0.15 m + the 0.08 m ankle).");
	CHECK_MESSAGE(Math::is_equal_approx(right->get_global_position().y, (real_t)0.08, (real_t)0.005), "The right foot stays on the lower ground.");
	CHECK_MESSAGE(Math::abs(f->get_pelvis_drop()) < 0.005, "No pelvis drop when one foot is on the clip's floor.");

	// Both feet 10 cm lower than the clip's floor: the pelvis comes down to reach them.
	ground.base = -0.1f;
	ground.step = 0.0f;
	f->reset();
	for (int i = 0; i < 240; i++) {
		step(sk, 1.0 / 60.0);
	}
	CHECK_MESSAGE(Math::is_equal_approx(f->get_pelvis_drop(), (real_t)-0.1, (real_t)0.005), "The pelvis drops to the lower ground.");
	CHECK_MESSAGE(Math::is_equal_approx(left->get_global_position().y, (real_t)-0.02, (real_t)0.005), "The feet reach it.");
	WorldContracts::unregister_ground(&ground);
	memdelete(sk);
}

TEST_CASE("[SceneTree][FootPlacementModifier3D] A rig whose rest pose floats still stands on the ground") {
	StepGround ground;
	ground.step = 0.15f;
	WorldContracts::register_ground(&ground);
	SceneTree *tree = SceneTree::get_singleton();
	Skeleton3D *sk = make_legs(tree);
	// The rest has the shins short (the ankles 0.28 m up); the clip stands them on the floor as before.
	for (const char *n : { "LeftFoot", "RightFoot" }) {
		const int b = sk->find_bone(n);
		sk->set_bone_rest(b, Transform3D(Basis(), Vector3(0, -0.20, -0.03)));
	}
	FootPlacementModifier3D *f = make_feet(sk);
	BoneAttachment3D *left = attach(sk, "LeftFoot");
	for (int i = 0; i < 120; i++) {
		step(sk, 1.0 / 60.0);
	}
	CHECK_MESSAGE(Math::is_equal_approx(left->get_global_position().y, (real_t)0.23, (real_t)0.005), "The ankle height comes from the clip, not the floating rest.");
	CHECK(f->is_foot_planted(FootPlacementModifier3D::SIDE_LEFT));
	WorldContracts::unregister_ground(&ground);
	memdelete(sk);
}

TEST_CASE("[SceneTree][FootPlacementModifier3D] A planted foot holds while the pelvis moves, and each plant is a footfall") {
	StepGround ground;
	WorldContracts::register_ground(&ground);
	SceneTree *tree = SceneTree::get_singleton();
	Node3D *walker = memnew(Node3D);
	tree->get_root()->add_child(walker);
	Skeleton3D *sk = make_legs(tree);
	tree->get_root()->remove_child(sk);
	walker->add_child(sk);
	FootPlacementModifier3D *f = make_feet(sk);
	f->set_walker_node(f->get_path_to(walker));
	BoneAttachment3D *left = attach(sk, "LeftFoot");
	for (int i = 0; i < 30; i++) {
		step(sk, 1.0 / 60.0);
	}
	const Vector3 planted = left->get_global_position();
	// An idle weight shift: hips 4 cm over to one side and dropped 4.5 degrees on the other; the feet stay put.
	f->set_pelvis_offset(Vector3(0.04, 0, 0));
	f->set_pelvis_rotation(Vector3(0, 0, Math::deg_to_rad((real_t)4.5)));
	for (int i = 0; i < 60; i++) {
		step(sk, 1.0 / 60.0);
	}
	CHECK_MESSAGE(left->get_global_position().distance_to(planted) < 0.003, "The planted foot does not slide.");
	CHECK(bool(walker->get_meta("world_contact_feet", false)));

	// A step: the clip lifts the left foot, moves it 30 cm forward and puts it down.
	uint64_t serial = 0;
	LocalVector<WorldWalkerStep> before;
	WorldFootfalls::read_since(walker->get_instance_id(), serial, before);
	const int foot = sk->find_bone("LeftFoot");
	const Vector3 rest = sk->get_bone_pose_position(foot);
	for (int i = 0; i < 20; i++) {
		sk->set_bone_pose_position(foot, rest + Vector3(0, 0.15, 0.3 * i / 19.0));
		step(sk, 1.0 / 60.0);
	}
	sk->set_bone_pose_position(foot, rest + Vector3(0, 0, 0.3));
	for (int i = 0; i < 30; i++) {
		step(sk, 1.0 / 60.0);
	}
	LocalVector<WorldWalkerStep> after;
	WorldFootfalls::read_since(walker->get_instance_id(), serial, after);
	REQUIRE_MESSAGE(after.size() >= 1, "The new plant is published as a footfall.");
	CHECK_MESSAGE(after[after.size() - 1].side == 0, "On the left.");
	CHECK_MESSAGE(Math::abs(after[after.size() - 1].position.z - (planted.z + 0.3 + f->get_sole_forward())) < 0.03, "Where the foot landed.");

	// A deformer reading this walker gets those footfalls instead of guessed strides.
	WorldWalkerSteps steps;
	WorldContactBody body;
	body.walker = true;
	body.feet = true;
	LocalVector<WorldWalkerStep> got;
	steps.begin();
	steps.step(walker, body, got);
	steps.end();
	CHECK_MESSAGE(got.is_empty(), "A new reader starts after what was already published.");

	WorldContracts::unregister_ground(&ground);
	memdelete(walker);
}

} // namespace TestFootPlacementModifier3D

#endif

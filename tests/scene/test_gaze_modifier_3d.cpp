/**************************************************************************/
/*  test_gaze_modifier_3d.cpp                                             */
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

TEST_FORCE_LINK(test_gaze_modifier_3d)

#ifndef _3D_DISABLED

#include "scene/3d/gaze_modifier_3d.h"
#include "scene/3d/skeleton_3d.h"
#include "scene/animation/second_order_dynamics.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"

namespace TestGazeModifier3D {

TEST_CASE("[SecondOrderDynamics] Step response settles, overshoots under critical damping and holds at zero time") {
	SecondOrderSystem<real_t> s;
	s.configure(2.0, 0.5, 0.0);
	s.reset(0.0);
	real_t peak = 0.0;
	for (int i = 0; i < 600; i++) {
		peak = MAX(peak, s.update(1.0 / 120.0, 1.0));
	}
	CHECK_MESSAGE(peak > 1.05, "An under-damped system overshoots.");
	CHECK_MESSAGE(Math::is_equal_approx(s.y, (real_t)1.0, (real_t)1e-3), "It settles on the target.");

	SecondOrderSystem<real_t> c;
	c.configure(2.0, 1.0, 0.0);
	c.reset(0.0);
	real_t cpeak = 0.0;
	for (int i = 0; i < 600; i++) {
		cpeak = MAX(cpeak, c.update(1.0 / 120.0, 1.0));
	}
	CHECK_MESSAGE(cpeak <= 1.0 + 1e-4, "A critically damped system does not overshoot.");

	SecondOrderSystem<real_t> a;
	a.configure(2.0, 0.7, -0.5);
	a.reset(0.0);
	const real_t first = a.update(1.0 / 120.0, 1.0);
	const real_t second = a.update(1.0 / 120.0, 1.0);
	CHECK_MESSAGE(MIN(first, second) < 0.0, "A negative response anticipates (first moves the other way).");

	const real_t held = a.y;
	a.update(0.0, 5.0);
	CHECK_MESSAGE(a.y == held, "A zero step changes nothing.");

	SecondOrderSystem<real_t> big;
	big.configure(8.0, 0.3, 1.0);
	big.reset(0.0);
	for (int i = 0; i < 50; i++) {
		big.update(0.5, 1.0);
	}
	CHECK_MESSAGE(Math::abs(big.y - 1.0) < 1.0, "Large frame times stay stable.");
}

TEST_CASE("[SecondOrderDynamics] Scripting wrapper keeps the value's type") {
	Ref<SecondOrderDynamics> d;
	d.instantiate();
	d->set_frequency(3.0);
	d->set_damping(1.0);
	d->reset(Vector3());
	Variant v;
	for (int i = 0; i < 300; i++) {
		v = d->update(1.0 / 60.0, Vector3(1, 2, 3));
	}
	CHECK(v.get_type() == Variant::VECTOR3);
	CHECK(((Vector3)v).is_equal_approx(Vector3(1, 2, 3)));
	d->reset(0.0);
	CHECK(d->update(1.0 / 60.0, 1.0).get_type() == Variant::FLOAT);
}

// A small upright rig facing +Z: hips, two spine bones, neck, head and two eyes.
static Skeleton3D *make_rig(SceneTree *p_tree) {
	Skeleton3D *sk = memnew(Skeleton3D);
	p_tree->get_root()->add_child(sk);
	sk->set_modifier_callback_mode_process(Skeleton3D::MODIFIER_CALLBACK_MODE_PROCESS_MANUAL);
	struct B {
		const char *name;
		const char *parent;
		Vector3 offset;
	};
	const B bones[] = {
		{ "Hips", "", Vector3(0, 1.0, 0) },
		{ "Spine", "Hips", Vector3(0, 0.15, 0) },
		{ "Chest", "Spine", Vector3(0, 0.2, 0) },
		{ "Neck", "Chest", Vector3(0, 0.2, 0) },
		{ "Head", "Neck", Vector3(0, 0.1, 0) },
		{ "LeftEye", "Head", Vector3(0.032, 0.09, 0.08) },
		{ "RightEye", "Head", Vector3(-0.032, 0.09, 0.08) },
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

static GazeModifier3D *make_gaze(Skeleton3D *p_sk) {
	GazeModifier3D *g = memnew(GazeModifier3D);
	PackedStringArray spine;
	spine.push_back("Spine");
	spine.push_back("Chest");
	g->set_spine_bones(spine);
	g->set_neck_bone("Neck");
	g->set_head_bone("Head");
	g->set_left_eye_bone("LeftEye");
	g->set_right_eye_bone("RightEye");
	g->set_drift_amplitude(0.0);
	g->set_target_mode(GazeModifier3D::TARGET_MODE_POSITION);
	p_sk->add_child(g);
	return g;
}

static void step(Skeleton3D *p_sk, double p_dt) {
	p_sk->advance(p_dt);
	p_sk->notification(Skeleton3D::NOTIFICATION_UPDATE_SKELETON);
}

TEST_CASE("[SceneTree][GazeModifier3D] The head reaches a target within its range and settles") {
	SceneTree *tree = SceneTree::get_singleton();
	Skeleton3D *sk = make_rig(tree);
	GazeModifier3D *g = make_gaze(sk);
	g->set_target_position(Vector3(0, 1.74, 2));
	step(sk, 1.0 / 60.0);
	// Straight ahead first, then 40 degrees to the left.
	const real_t yaw = Math::deg_to_rad((real_t)40.0);
	g->set_target_position(Vector3(Math::sin(yaw) * 2.0, 1.74, Math::cos(yaw) * 2.0));
	real_t peak = 0.0;
	for (int i = 0; i < 360; i++) {
		step(sk, 1.0 / 60.0);
		peak = MAX(peak, g->get_head_angles().x);
	}
	CHECK_MESSAGE(Math::abs(g->get_head_angles().x - g->get_target_angles().x) < Math::deg_to_rad((real_t)1.0), "The head ends facing the target.");
	CHECK_MESSAGE(peak > g->get_target_angles().x, "The shift overshoots slightly before settling.");
	CHECK_MESSAGE(peak < g->get_target_angles().x + Math::deg_to_rad((real_t)6.0), "The overshoot is small.");
	memdelete(sk);
}

TEST_CASE("[SceneTree][GazeModifier3D] Limits, the back's share and targets behind") {
	SceneTree *tree = SceneTree::get_singleton();
	Skeleton3D *sk = make_rig(tree);
	GazeModifier3D *g = make_gaze(sk);
	// 85 degrees round: past the head's 70, the back takes the rest up to its 22 (far off, so the eyes'
	// own movement as the head turns does not change the angle).
	const real_t yaw = Math::deg_to_rad((real_t)85.0);
	g->set_target_position(Vector3(Math::sin(yaw) * 50.0, 1.74, Math::cos(yaw) * 50.0));
	step(sk, 1.0 / 60.0);
	for (int i = 0; i < 360; i++) {
		step(sk, 1.0 / 60.0);
	}
	CHECK_MESSAGE(Math::abs(g->get_head_angles().x - Math::deg_to_rad((real_t)85.0)) < Math::deg_to_rad((real_t)1.5), "Head and back together reach 85 degrees.");
	CHECK_MESSAGE(Math::abs(g->get_spine_yaw() - g->get_spine_yaw_limit()) < Math::deg_to_rad((real_t)1.0), "The back takes its full 22 degrees.");

	// Far down: clamped to the head's down limit.
	g->set_target_position(Vector3(0, -5, 0.5));
	for (int i = 0; i < 360; i++) {
		step(sk, 1.0 / 60.0);
	}
	CHECK_MESSAGE(g->get_head_angles().y <= g->get_head_down_limit() + Math::deg_to_rad((real_t)1.0), "Pitch stays within the down limit.");

	// Behind: not chased; the look fades out.
	g->set_target_position(Vector3(0, 1.74, -2));
	for (int i = 0; i < 600; i++) {
		step(sk, 1.0 / 60.0);
	}
	CHECK_MESSAGE(g->get_look_weight() < 0.05, "A target behind is not looked for.");
	memdelete(sk);
}

TEST_CASE("[SceneTree][GazeModifier3D] A stopped clock holds the pose exactly") {
	SceneTree *tree = SceneTree::get_singleton();
	Skeleton3D *sk = make_rig(tree);
	GazeModifier3D *g = make_gaze(sk);
	g->set_drift_amplitude(Math::deg_to_rad((real_t)0.8));
	g->set_target_position(Vector3(1, 1.6, 2));
	for (int i = 0; i < 30; i++) {
		step(sk, 1.0 / 60.0);
	}
	const Vector2 before = g->get_head_angles();
	for (int i = 0; i < 10; i++) {
		step(sk, 0.0);
	}
	CHECK_MESSAGE(g->get_head_angles().is_equal_approx(before), "Zero-time frames do not move the head (path-traced stills accumulate cleanly).");
	memdelete(sk);
}

TEST_CASE("[SceneTree][GazeModifier3D] An unseen skeleton rests its modifiers and resumes without a glide") {
	SceneTree *tree = SceneTree::get_singleton();
	Skeleton3D *sk = make_rig(tree);
	GazeModifier3D *g = make_gaze(sk);
	sk->set_modifier_update_mode(Skeleton3D::MODIFIER_UPDATE_MODE_WHEN_VISIBLE);
	g->set_target_position(sk->get_global_position() + Vector3(-2, 1.6, 3));
	for (int i = 0; i < 240; i++) {
		step(sk, 1.0 / 60.0);
	}
	CHECK_MESSAGE(sk->are_modifiers_running(), "Shown, with no renderer to cull it: the modifiers run.");
	const Vector2 settled = g->get_head_angles();

	// Hidden: nothing runs, so the head keeps the angles it had while the target swings to the other side.
	sk->hide();
	CHECK_FALSE(sk->are_modifiers_running());
	g->set_target_position(sk->get_global_position() + Vector3(2, 1.6, 3));
	for (int i = 0; i < 60; i++) {
		step(sk, 1.0 / 60.0);
	}
	CHECK_MESSAGE(g->get_head_angles().is_equal_approx(settled), "Resting modifiers do not run.");

	// Shown again: the first frame already faces the new target, with no turn across from the old one.
	sk->show();
	CHECK(sk->are_modifiers_running());
	step(sk, 1.0 / 60.0);
	CHECK_MESSAGE(Math::abs(g->get_head_angles().x - g->get_target_angles().x) < Math::deg_to_rad((real_t)1.0), "Resumed at the target.");

	// The upstream default never rests.
	sk->set_modifier_update_mode(Skeleton3D::MODIFIER_UPDATE_MODE_ALWAYS);
	sk->hide();
	CHECK(sk->are_modifiers_running());
	memdelete(sk);
}

} // namespace TestGazeModifier3D

#endif // _3D_DISABLED

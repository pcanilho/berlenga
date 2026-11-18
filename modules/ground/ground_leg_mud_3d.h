/**************************************************************************/
/*  ground_leg_mud_3d.h                                                   */
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

#include "scene/3d/node_3d.h"

class Skeleton3D;

// Mud on boots and legs (docs/ideas/mud.md, stage 4e). Child of a character: each leg's mud line rises from the sole
// by how deep its foot sank in mud and flecks fly up the shin with the stride; it dries over minutes and standing
// water washes it. It turns BaseMaterial3D's Mud layer on (legs mode) in copies of the materials of the meshes under
// its parent and feeds them each physics tick: the shader lays the mud in each shin's own frame, from the sole and
// knee points this node writes, so it stays on the leg through the walk cycle.
class GroundLegMud3D : public Node3D {
	GDCLASS(GroundLegMud3D, Node3D);

public:
	enum Leg {
		LEG_LEFT,
		LEG_RIGHT,
	};

private:
	bool enabled = true;
	NodePath skeleton; // empty: the first Skeleton3D under the parent
	String foot_bones[2]; // empty: found by name ("LeftFoot", "foot.L", ...)
	String knee_bones[2]; // the lower leg's bone, starting at the knee ("LeftLeg", "calf_l", ...)
	NodePath sole_nodes[2]; // instead of bones: nodes at the soles and knees (rigs without a skeleton)
	NodePath knee_nodes[2];
	float ankle_height = 0.08f; // m from the foot bone down to the sole
	float shin_radius = 0.06f;
	float line[2] = {}; // m above the sole
	float flecks[2] = {}; // 0..1
	float wet = 0.0f;
	float in_water[2] = { 1e3f, 1e3f };
	bool planted[2] = { false, false }; // last step's, so a footfall prints once // s since each foot last stood in water (the read back can miss a frame as the field moves)
	Vector3 mud_rgb = Vector3(0.17f, 0.11f, 0.065f);
	Vector3 last_position;
	bool have_last = false;
	bool meshes_ready = false;
	bool written = false;
	LocalVector<ObjectID> meshes;
	int bone_foot[2] = { -1, -1 };
	int bone_knee[2] = { -1, -1 };
	ObjectID skeleton_id;

	Skeleton3D *_skeleton();
	void _find_bones(Skeleton3D *p_skeleton);
	void _find_meshes();
	bool _leg_points(int p_leg, Vector3 &r_sole, Vector3 &r_knee);
	void _step(double p_delta);

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	void set_enabled(bool p_enabled);
	bool is_enabled() const { return enabled; }
	void set_skeleton(const NodePath &p_path);
	NodePath get_skeleton() const { return skeleton; }
	void set_foot_bone(Leg p_leg, const String &p_name);
	String get_foot_bone(Leg p_leg) const { return foot_bones[p_leg]; }
	void set_knee_bone(Leg p_leg, const String &p_name);
	String get_knee_bone(Leg p_leg) const { return knee_bones[p_leg]; }
	void set_sole_node(Leg p_leg, const NodePath &p_path) { sole_nodes[p_leg] = p_path; }
	NodePath get_sole_node(Leg p_leg) const { return sole_nodes[p_leg]; }
	void set_knee_node(Leg p_leg, const NodePath &p_path) { knee_nodes[p_leg] = p_path; }
	NodePath get_knee_node(Leg p_leg) const { return knee_nodes[p_leg]; }
	void set_ankle_height(float p_m) { ankle_height = MAX(p_m, 0.0f); }
	float get_ankle_height() const { return ankle_height; }
	void set_shin_radius(float p_m) { shin_radius = MAX(p_m, 0.01f); }
	float get_shin_radius() const { return shin_radius; }
	// The mud line above leg p_leg's sole (m) and its flecks (0..1); the wetness (1 fresh, 0 dry).
	float get_mud_line(Leg p_leg) const { return line[p_leg]; }
	float get_mud_flecks(Leg p_leg) const { return flecks[p_leg]; }
	float get_mud_wetness() const { return wet; }
	void clear_mud();
};

VARIANT_ENUM_CAST(GroundLegMud3D::Leg);

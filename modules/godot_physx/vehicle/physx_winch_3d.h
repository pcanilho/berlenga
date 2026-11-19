/**************************************************************************/
/*  physx_winch_3d.h                                                      */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
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
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"

namespace physx {
class PxDistanceJoint;
class PxRigidActor;
} //namespace physx

class PhysXVehicle3D;
class MeshInstance3D;

// A vehicle's winch (mud round 11): a child of a PhysXVehicle3D at its fairlead, its cable to an anchor (a tree's or
// a rock's static body, another vehicle, any physics body, or a point in the world). The cable is a PhysX distance
// joint, tension only (its max distance the cable paid out, a spring at the rope's own axial stiffness EA / L past
// it), and the drum's motor winds the max distance in at the line speed its load allows: an electric winch's speed
// falls about linearly with the pull, from its no-load speed to a tenth of it at its rated pull, and stalls just past
// that (WARN's published 10,000 lb curves). Drawn as a catenary from its length and weight (sagging when slack, near
// straight under load), lying on the ground where it would pass under it.
class PhysXWinch3D : public Node3D {
	GDCLASS(PhysXWinch3D, Node3D);

public:
	enum Drum {
		DRUM_HOLD, // the brake holds the cable (it slips past brake_hold)
		DRUM_IN, // the motor winds in
		DRUM_FREE, // free spool: the cable pays out as it is pulled, and is wound in to take up slack
	};

private:
	NodePath anchor;
	Vector3 anchor_point;
	bool attached = true;
	Drum drum = DRUM_HOLD;
	real_t rated_pull = 44500.0; // N (10,000 lb)
	real_t line_speed = 0.14; // m/s at no load
	real_t brake_hold = 1.5; // times the rated pull
	real_t cable_length = 0.0; // m paid out; 0 = the distance when attached, plus a little slack
	real_t max_cable_length = 30.0;
	real_t cable_mass = 0.35; // kg/m (9.5 mm steel wire rope; synthetic about 0.06)
	real_t cable_radius = 0.0048;
	real_t cable_stiffness = 6.0e6; // N, the rope's EA
	Ref<Material> cable_material;

	physx::PxDistanceJoint *joint = nullptr;
	physx::PxRigidActor *joint_a = nullptr;
	physx::PxRigidActor *joint_b = nullptr;
	PhysXVehicle3D *vehicle = nullptr;
	MeshInstance3D *cable_mesh = nullptr;
	Ref<ArrayMesh> mesh;
	real_t tension = 0.0;
	real_t distance = 0.0;
	bool length_set = false;

	void _update_vehicle();
	physx::PxRigidActor *_anchor_actor(Transform3D &r_frame) const;
	Vector3 _anchor_global() const;
	void _release_joint();
	void _physics_step(double p_delta);
	void _draw_cable();

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	void set_anchor(const NodePath &p_anchor);
	NodePath get_anchor() const { return anchor; }
	void set_anchor_point(const Vector3 &p_point);
	Vector3 get_anchor_point() const { return anchor_point; }
	void set_attached(bool p_attached);
	bool is_attached() const { return attached; }
	void set_drum(Drum p_drum);
	Drum get_drum() const { return drum; }
	void set_rated_pull(real_t p_v);
	real_t get_rated_pull() const { return rated_pull; }
	void set_line_speed(real_t p_v);
	real_t get_line_speed() const { return line_speed; }
	void set_brake_hold(real_t p_v);
	real_t get_brake_hold() const { return brake_hold; }
	void set_cable_length(real_t p_v);
	real_t get_cable_length() const { return cable_length; }
	void set_max_cable_length(real_t p_v);
	real_t get_max_cable_length() const { return max_cable_length; }
	void set_cable_mass(real_t p_v);
	real_t get_cable_mass() const { return cable_mass; }
	void set_cable_radius(real_t p_v);
	real_t get_cable_radius() const { return cable_radius; }
	void set_cable_stiffness(real_t p_v);
	real_t get_cable_stiffness() const { return cable_stiffness; }
	void set_cable_material(const Ref<Material> &p_material);
	Ref<Material> get_cable_material() const { return cable_material; }

	// The cable's tension the last step (N), and the straight distance fairlead to anchor (m).
	real_t get_tension() const { return tension; }
	real_t get_anchor_distance() const { return distance; }
	// The line speed the motor manages at p_pull (N): the no-load speed falling to a tenth at the rated pull, none past
	// 1.1 times it.
	real_t line_speed_at(real_t p_pull) const;
	// Points along the drawn cable, fairlead to anchor, in global space.
	PackedVector3Array get_cable_points(int p_count = 24) const;

	PackedStringArray get_configuration_warnings() const override;

	PhysXWinch3D();
	~PhysXWinch3D();
};

VARIANT_ENUM_CAST(PhysXWinch3D::Drum);

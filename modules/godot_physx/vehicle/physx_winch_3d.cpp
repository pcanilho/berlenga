/**************************************************************************/
/*  physx_winch_3d.cpp                                                    */
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

#include "physx_winch_3d.h"

#include "../godot_physx_conversions.h"
#include "../godot_physx_server_3d.h"
#include "../objects/godot_physx_body_3d.h"
#include "../spaces/godot_physx_space_3d.h"
#include "physx_vehicle_3d.h"

#include "core/config/engine.h"
#include "core/object/class_db.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/physics/collision_object_3d.h"
#include "scene/resources/3d/world_3d.h"

#include "modules/modules_enabled.gen.h" // For ground.

#ifdef MODULE_GROUND_ENABLED
#include "modules/ground/ground_surface_3d.h"
#endif

#include <PxPhysicsAPI.h>
#include <extensions/PxDistanceJoint.h>

using namespace physx;

PhysXWinch3D::PhysXWinch3D() {
	set_notify_transform(false);
}

PhysXWinch3D::~PhysXWinch3D() {
	_release_joint();
}

void PhysXWinch3D::_update_vehicle() {
	vehicle = Object::cast_to<PhysXVehicle3D>(get_parent());
}

void PhysXWinch3D::_release_joint() {
	if (joint) {
		joint->release();
		joint = nullptr;
	}
	joint_a = nullptr;
	joint_b = nullptr;
}

PxRigidActor *PhysXWinch3D::_anchor_actor(Transform3D &r_frame) const {
	r_frame = Transform3D(Basis(), anchor_point);
	Node *n = anchor.is_empty() ? nullptr : get_node_or_null(anchor);
	if (!n) {
		return nullptr;
	}
	if (PhysXVehicle3D *v = Object::cast_to<PhysXVehicle3D>(n)) {
		return v->get_px_actor();
	}
	if (CollisionObject3D *co = Object::cast_to<CollisionObject3D>(n)) {
		GodotPhysXServer3D *server = GodotPhysXServer3D::get_singleton();
		GodotPhysXBody3D *body = server ? server->get_body(co->get_rid()) : nullptr;
		if (body && body->get_px_actor()) {
			return body->get_px_actor();
		}
	}
	// Any other node (a tree's mesh, a marker): a fixed point in the world.
	if (Node3D *n3 = Object::cast_to<Node3D>(n)) {
		r_frame = Transform3D(Basis(), n3->get_global_transform().xform(anchor_point));
	}
	return nullptr;
}

Vector3 PhysXWinch3D::_anchor_global() const {
	Node3D *n = anchor.is_empty() ? nullptr : Object::cast_to<Node3D>(get_node_or_null(anchor));
	return n ? n->get_global_transform().xform(anchor_point) : anchor_point;
}

real_t PhysXWinch3D::line_speed_at(real_t p_pull) const {
	if (rated_pull <= 0.0) {
		return 0.0;
	}
	return line_speed * MAX(1.0 - 0.9 * p_pull / rated_pull, 0.0);
}

void PhysXWinch3D::_physics_step(double p_delta) {
	if (Engine::get_singleton()->is_editor_hint() || !vehicle || !attached || !is_inside_tree()) {
		_release_joint();
		tension = 0.0;
		return;
	}
	PxRigidActor *a = vehicle->get_px_actor();
	if (!a) {
		_release_joint();
		tension = 0.0;
		return;
	}
	Transform3D bframe;
	PxRigidActor *b = _anchor_actor(bframe);
	const Vector3 fair = get_global_position();
	const Vector3 anc = _anchor_global();
	distance = fair.distance_to(anc);
	if (!length_set) {
		cable_length = cable_length > 0.0 ? cable_length : MIN(distance + 0.3, max_cable_length);
		length_set = true;
	}
	if (joint && (joint_a != a || joint_b != b)) {
		_release_joint();
	}
	if (!joint) {
		GodotPhysXServer3D *server = GodotPhysXServer3D::get_singleton();
		GodotPhysXSpace3D *space = server && vehicle->get_world_3d().is_valid() ? server->get_space(vehicle->get_world_3d()->get_space()) : nullptr;
		PxPhysics *physics = space ? space->get_px_physics() : nullptr;
		if (!physics) {
			return;
		}
		// The vehicle's actor frame is its node's.
		const Vector3 local_a = vehicle->get_global_transform().affine_inverse().xform(fair);
		joint = PxDistanceJointCreate(*physics, a, PxTransform(to_px(local_a)), b, PxTransform(to_px(bframe.origin)));
		if (!joint) {
			return;
		}
		joint->setDistanceJointFlags(PxDistanceJointFlag::eMAX_DISTANCE_ENABLED | PxDistanceJointFlag::eSPRING_ENABLED);
		joint->setMinDistance(0.0f);
		joint_a = a;
		joint_b = b;
	}
	// The drum, from the load it carried the last step.
	const real_t dt = real_t(p_delta);
	switch (drum) {
		case DRUM_IN:
			cable_length -= line_speed_at(tension) * dt;
			break;
		case DRUM_FREE:
			cable_length = MAX(cable_length, distance);
			break;
		case DRUM_HOLD:
			if (tension > brake_hold * rated_pull) {
				cable_length += line_speed * dt;
			}
			break;
	}
	cable_length = CLAMP(cable_length, real_t(0.3), max_cable_length);
	joint->setMaxDistance(PxReal(cable_length));
	// The rope's axial stiffness over its length, damped near critically against the vehicle's mass.
	const real_t k = cable_stiffness / MAX(cable_length, real_t(0.5));
	PxRigidDynamic *dyn = a->is<PxRigidDynamic>();
	const real_t m = dyn ? real_t(dyn->getMass()) : 1000.0;
	joint->setStiffness(PxReal(k));
	joint->setDamping(PxReal(Math::sqrt(k * m)));
	if (dyn && dyn->getScene() && !(dyn->getRigidBodyFlags() & PxRigidBodyFlag::eKINEMATIC) && dyn->isSleeping() && (drum == DRUM_IN || distance > cable_length)) {
		dyn->wakeUp();
	}
	PxVec3 f, t;
	joint->getConstraint()->getForce(f, t);
	tension = distance > cable_length - 0.005 ? real_t(f.magnitude()) : 0.0;
}

PackedVector3Array PhysXWinch3D::get_cable_points(int p_count) const {
	PackedVector3Array pts;
	if (!is_inside_tree()) {
		return pts;
	}
	const int n = MAX(p_count, 1);
	const Vector3 A = get_global_position();
	const Vector3 B = _anchor_global();
	const Vector3 d = B - A;
	const double h = Vector2(d.x, d.z).length();
	const double v = d.y;
	const double chord = d.length();
	const double L = MAX(double(cable_length), chord);
	const double w = MAX(double(cable_mass) * 9.81, 1e-4);
	double a;
	if (L > chord * 1.0005 && h > 1e-3) {
		// Slack: the catenary of length L through both ends, 2 a sinh(h / 2a) = sqrt(L^2 - v^2), by bisection on a.
		const double S = Math::sqrt(MAX(L * L - v * v, h * h));
		double lo = h / 600.0;
		double hi = 1e6 * h;
		for (int it = 0; it < 80; it++) {
			const double mid = Math::sqrt(lo * hi);
			if (2.0 * mid * Math::sinh(h / (2.0 * mid)) > S) {
				lo = mid;
			} else {
				hi = mid;
			}
		}
		a = Math::sqrt(lo * hi);
	} else {
		// Taut: the sag its own weight gives under the tension (a = T / w).
		a = MAX(double(tension), 1.0) / w;
	}
	const Vector3 dir = h > 1e-3 ? Vector3(d.x, 0.0, d.z) / h : Vector3();
	const double x0 = h > 1e-3 ? h * 0.5 - a * Math::asinh(v / (2.0 * a * Math::sinh(h / (2.0 * a)))) : 0.0;
#ifdef MODULE_GROUND_ENABLED
	GroundSurface3D *ground = GroundSurface3D::get_active();
#endif
	pts.resize(n + 1);
	for (int k = 0; k <= n; k++) {
		const double s = double(k) / n;
		Vector3 p;
		if (h <= 1e-3) {
			p = A + d * s;
		} else {
			const double x = s * h;
			// y(x) - y(0) = a (cosh((x - x0) / a) - cosh(x0 / a)), as a product of sinhs (no cancellation when a is large).
			const double y = 2.0 * a * Math::sinh((x - 2.0 * x0) / (2.0 * a)) * Math::sinh(x / (2.0 * a));
			p = A + dir * x + Vector3(0.0, y, 0.0);
		}
#ifdef MODULE_GROUND_ENABLED
		if (ground && k > 0 && k < n) {
			p.y = MAX(p.y, ground->get_surface_height_at(p) + cable_radius);
		}
#endif
		pts.write[k] = p;
	}
	return pts;
}

void PhysXWinch3D::_draw_cable() {
	if (!cable_mesh) {
		return;
	}
	if (!attached || !vehicle) {
		cable_mesh->hide();
		return;
	}
	cable_mesh->show();
	const PackedVector3Array pts = get_cable_points(32);
	if (pts.size() < 2) {
		return;
	}
	const int sides = 6;
	PackedVector3Array verts;
	PackedVector3Array normals;
	PackedVector2Array uvs;
	PackedInt32Array idx;
	Vector3 side = Vector3(0, 1, 0);
	real_t run = 0.0;
	for (int k = 0; k < pts.size(); k++) {
		const Vector3 tg = (pts[MIN(k + 1, pts.size() - 1)] - pts[MAX(k - 1, 0)]).normalized();
		if (Math::abs(tg.dot(side)) > 0.9) {
			side = Vector3(1, 0, 0);
		}
		const Vector3 u = tg.cross(side).normalized();
		const Vector3 vv = tg.cross(u);
		side = vv;
		if (k > 0) {
			run += pts[k].distance_to(pts[k - 1]);
		}
		for (int j = 0; j < sides; j++) {
			const real_t ang = Math::TAU * j / sides;
			const Vector3 nrm = u * Math::cos(ang) + vv * Math::sin(ang);
			verts.push_back(pts[k] + nrm * cable_radius);
			normals.push_back(nrm);
			// The strands' lay: a texture's v along the rope, u round it.
			uvs.push_back(Vector2(real_t(j) / sides, run / (8.0 * cable_radius)));
		}
		if (k > 0) {
			const int r0 = (k - 1) * sides;
			const int r1 = k * sides;
			for (int j = 0; j < sides; j++) {
				const int j1 = (j + 1) % sides;
				idx.push_back(r0 + j);
				idx.push_back(r1 + j);
				idx.push_back(r1 + j1);
				idx.push_back(r0 + j);
				idx.push_back(r1 + j1);
				idx.push_back(r0 + j1);
			}
		}
	}
	Array arr;
	arr.resize(Mesh::ARRAY_MAX);
	arr[Mesh::ARRAY_VERTEX] = verts;
	arr[Mesh::ARRAY_NORMAL] = normals;
	arr[Mesh::ARRAY_TEX_UV] = uvs;
	arr[Mesh::ARRAY_INDEX] = idx;
	mesh->clear_surfaces();
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arr);
	Ref<Material> mat = cable_material;
	if (mat.is_null()) {
		static Ref<StandardMaterial3D> steel;
		if (steel.is_null()) {
			steel.instantiate();
			steel->set_albedo(Color(0.32, 0.32, 0.33));
			steel->set_metallic(0.85);
			steel->set_roughness(0.45);
		}
		mat = steel;
	}
	mesh->surface_set_material(0, mat);
}

void PhysXWinch3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			_update_vehicle();
			if (!cable_mesh) {
				mesh.instantiate();
				cable_mesh = memnew(MeshInstance3D);
				cable_mesh->set_mesh(mesh);
				add_child(cable_mesh, false, INTERNAL_MODE_FRONT);
				cable_mesh->set_as_top_level(true);
				cable_mesh->set_global_transform(Transform3D());
			}
			set_physics_process_internal(true);
			set_process_internal(true);
		} break;
		case NOTIFICATION_EXIT_TREE: {
			_release_joint();
			vehicle = nullptr;
			set_physics_process_internal(false);
			set_process_internal(false);
		} break;
		case NOTIFICATION_INTERNAL_PHYSICS_PROCESS: {
			_physics_step(get_physics_process_delta_time());
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			_draw_cable();
		} break;
	}
}

void PhysXWinch3D::set_anchor(const NodePath &p_anchor) {
	anchor = p_anchor;
	_release_joint();
}

void PhysXWinch3D::set_anchor_point(const Vector3 &p_point) {
	anchor_point = p_point;
	_release_joint();
}

void PhysXWinch3D::set_attached(bool p_attached) {
	attached = p_attached;
	if (!attached) {
		_release_joint();
		length_set = false;
		cable_length = 0.0;
	}
}

void PhysXWinch3D::set_drum(Drum p_drum) {
	drum = p_drum;
}

void PhysXWinch3D::set_rated_pull(real_t p_v) {
	rated_pull = MAX(p_v, real_t(0.0));
}

void PhysXWinch3D::set_line_speed(real_t p_v) {
	line_speed = MAX(p_v, real_t(0.0));
}

void PhysXWinch3D::set_brake_hold(real_t p_v) {
	brake_hold = MAX(p_v, real_t(0.0));
}

void PhysXWinch3D::set_cable_length(real_t p_v) {
	cable_length = MAX(p_v, real_t(0.0));
	length_set = cable_length > 0.0;
}

void PhysXWinch3D::set_max_cable_length(real_t p_v) {
	max_cable_length = MAX(p_v, real_t(0.5));
}

void PhysXWinch3D::set_cable_mass(real_t p_v) {
	cable_mass = MAX(p_v, real_t(0.0));
}

void PhysXWinch3D::set_cable_radius(real_t p_v) {
	cable_radius = MAX(p_v, real_t(0.0005));
}

void PhysXWinch3D::set_cable_stiffness(real_t p_v) {
	cable_stiffness = MAX(p_v, real_t(1.0));
}

void PhysXWinch3D::set_cable_material(const Ref<Material> &p_material) {
	cable_material = p_material;
}

PackedStringArray PhysXWinch3D::get_configuration_warnings() const {
	PackedStringArray warnings = Node3D::get_configuration_warnings();
	if (!Object::cast_to<PhysXVehicle3D>(get_parent())) {
		warnings.push_back(RTR("PhysXWinch3D must be a direct child of a PhysXVehicle3D (placed at its fairlead)."));
	}
	return warnings;
}

void PhysXWinch3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_anchor", "anchor"), &PhysXWinch3D::set_anchor);
	ClassDB::bind_method(D_METHOD("get_anchor"), &PhysXWinch3D::get_anchor);
	ClassDB::bind_method(D_METHOD("set_anchor_point", "point"), &PhysXWinch3D::set_anchor_point);
	ClassDB::bind_method(D_METHOD("get_anchor_point"), &PhysXWinch3D::get_anchor_point);
	ClassDB::bind_method(D_METHOD("set_attached", "attached"), &PhysXWinch3D::set_attached);
	ClassDB::bind_method(D_METHOD("is_attached"), &PhysXWinch3D::is_attached);
	ClassDB::bind_method(D_METHOD("set_drum", "drum"), &PhysXWinch3D::set_drum);
	ClassDB::bind_method(D_METHOD("get_drum"), &PhysXWinch3D::get_drum);
	ClassDB::bind_method(D_METHOD("set_rated_pull", "newtons"), &PhysXWinch3D::set_rated_pull);
	ClassDB::bind_method(D_METHOD("get_rated_pull"), &PhysXWinch3D::get_rated_pull);
	ClassDB::bind_method(D_METHOD("set_line_speed", "speed"), &PhysXWinch3D::set_line_speed);
	ClassDB::bind_method(D_METHOD("get_line_speed"), &PhysXWinch3D::get_line_speed);
	ClassDB::bind_method(D_METHOD("set_brake_hold", "share"), &PhysXWinch3D::set_brake_hold);
	ClassDB::bind_method(D_METHOD("get_brake_hold"), &PhysXWinch3D::get_brake_hold);
	ClassDB::bind_method(D_METHOD("set_cable_length", "length"), &PhysXWinch3D::set_cable_length);
	ClassDB::bind_method(D_METHOD("get_cable_length"), &PhysXWinch3D::get_cable_length);
	ClassDB::bind_method(D_METHOD("set_max_cable_length", "length"), &PhysXWinch3D::set_max_cable_length);
	ClassDB::bind_method(D_METHOD("get_max_cable_length"), &PhysXWinch3D::get_max_cable_length);
	ClassDB::bind_method(D_METHOD("set_cable_mass", "kg_per_m"), &PhysXWinch3D::set_cable_mass);
	ClassDB::bind_method(D_METHOD("get_cable_mass"), &PhysXWinch3D::get_cable_mass);
	ClassDB::bind_method(D_METHOD("set_cable_radius", "radius"), &PhysXWinch3D::set_cable_radius);
	ClassDB::bind_method(D_METHOD("get_cable_radius"), &PhysXWinch3D::get_cable_radius);
	ClassDB::bind_method(D_METHOD("set_cable_stiffness", "ea"), &PhysXWinch3D::set_cable_stiffness);
	ClassDB::bind_method(D_METHOD("get_cable_stiffness"), &PhysXWinch3D::get_cable_stiffness);
	ClassDB::bind_method(D_METHOD("set_cable_material", "material"), &PhysXWinch3D::set_cable_material);
	ClassDB::bind_method(D_METHOD("get_cable_material"), &PhysXWinch3D::get_cable_material);
	ClassDB::bind_method(D_METHOD("get_tension"), &PhysXWinch3D::get_tension);
	ClassDB::bind_method(D_METHOD("get_anchor_distance"), &PhysXWinch3D::get_anchor_distance);
	ClassDB::bind_method(D_METHOD("line_speed_at", "pull"), &PhysXWinch3D::line_speed_at);
	ClassDB::bind_method(D_METHOD("get_cable_points", "count"), &PhysXWinch3D::get_cable_points, DEFVAL(24));

	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "anchor"), "set_anchor", "get_anchor");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "anchor_point", PROPERTY_HINT_NONE, "suffix:m"), "set_anchor_point", "get_anchor_point");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "attached"), "set_attached", "is_attached");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "drum", PROPERTY_HINT_ENUM, "Hold,In,Free"), "set_drum", "get_drum");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "rated_pull", PROPERTY_HINT_RANGE, "0,200000,100,or_greater,suffix:N"), "set_rated_pull", "get_rated_pull");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "line_speed", PROPERTY_HINT_RANGE, "0,2,0.01,or_greater,suffix:m/s"), "set_line_speed", "get_line_speed");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "brake_hold", PROPERTY_HINT_RANGE, "0,5,0.05"), "set_brake_hold", "get_brake_hold");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cable_length", PROPERTY_HINT_RANGE, "0,100,0.01,or_greater,suffix:m"), "set_cable_length", "get_cable_length");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "max_cable_length", PROPERTY_HINT_RANGE, "0.5,100,0.1,or_greater,suffix:m"), "set_max_cable_length", "get_max_cable_length");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cable_mass", PROPERTY_HINT_RANGE, "0,5,0.01,suffix:kg/m"), "set_cable_mass", "get_cable_mass");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cable_radius", PROPERTY_HINT_RANGE, "0.0005,0.05,0.0001,suffix:m"), "set_cable_radius", "get_cable_radius");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cable_stiffness", PROPERTY_HINT_RANGE, "1,1e8,1,or_greater,suffix:N"), "set_cable_stiffness", "get_cable_stiffness");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "cable_material", PROPERTY_HINT_RESOURCE_TYPE, "BaseMaterial3D,ShaderMaterial"), "set_cable_material", "get_cable_material");

	BIND_ENUM_CONSTANT(DRUM_HOLD);
	BIND_ENUM_CONSTANT(DRUM_IN);
	BIND_ENUM_CONSTANT(DRUM_FREE);
}

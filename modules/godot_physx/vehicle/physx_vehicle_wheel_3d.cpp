/**************************************************************************/
/*  physx_vehicle_wheel_3d.cpp                                            */
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

#include "physx_vehicle_wheel_3d.h"

#include "physx_motorcycle_3d.h"
#include "physx_tank_3d.h"
#include "physx_vehicle_3d.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"

PhysXVehicleWheel3D::PhysXVehicleWheel3D() {
	// Every local transform change outside _set_simulated_transform() is an
	// authored one (also before entering the tree: a scene load sets position first).
	set_notify_local_transform(true);
}

void PhysXVehicleWheel3D::_set_simulated_transform(const Transform3D &p_transform) {
	writing_simulated_pose = true;
	set_transform(p_transform);
	writing_simulated_pose = false;
}

void PhysXVehicleWheel3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_LOCAL_TRANSFORM_CHANGED: {
			if (!writing_simulated_pose) {
				rest_transform = get_transform();
			}
		} break;
		case NOTIFICATION_ENTER_TREE: {
			// Deferred: its sibling wheels, which share the weight, may not have entered yet.
			callable_mp(this, &PhysXVehicleWheel3D::_join_world_contact).call_deferred();
			if (PhysXVehicle3D *v = Object::cast_to<PhysXVehicle3D>(get_parent())) {
				vehicle = v;
				v->wheels.push_back(this);
				v->_rebuild_if_live();
				// Its own soil model ruts the ground (docs/ideas/sand.md); the generic body press stays off it.
				set_meta(SNAME("ground_self_pressing"), true);
			} else if (PhysXMotorcycle3D *m = Object::cast_to<PhysXMotorcycle3D>(get_parent())) {
				motorcycle = m;
				m->wheels.push_back(this);
				m->_rebuild_if_live();
			} else if (PhysXTank3D *t = Object::cast_to<PhysXTank3D>(get_parent())) {
				tank = t;
				t->wheels.push_back(this);
				t->_rebuild_if_live();
			}
		} break;
		case NOTIFICATION_EXIT_TREE: {
			if (vehicle) {
				vehicle->wheels.erase(this);
				vehicle->_rebuild_if_live();
				vehicle = nullptr;
			} else if (motorcycle) {
				motorcycle->wheels.erase(this);
				motorcycle->_rebuild_if_live();
				motorcycle = nullptr;
			} else if (tank) {
				tank->wheels.erase(this);
				tank->_rebuild_if_live();
				tank = nullptr;
			}
		} break;
	}
}

void PhysXVehicleWheel3D::_rebuild_parent_if_live() {
	if (vehicle) {
		vehicle->_rebuild_if_live();
	} else if (motorcycle) {
		motorcycle->_rebuild_if_live();
	} else if (tank) {
		tank->_rebuild_if_live();
	}
}

// Every wheel is a body the world feels (docs/ideas/world-links.md, contact 1): grass flattens under it, branches
// give way, snow ruts. The systems read its size from these metas, as they do for any body.
void PhysXVehicleWheel3D::_join_world_contact() {
	add_to_group(SNAME("world_contact"));
	const float r = float(radius);
	const float w = float(half_width);
	set_meta(SNAME("trample_radius"), MAX(w, 0.05f) + 0.05f);
	set_meta(SNAME("touch_radius"), r);
	set_meta(SNAME("snow_radius"), MAX(w, 0.05f));
	set_meta(SNAME("deform_wheel_radius"), r);
	set_meta(SNAME("deform_wheel_width"), 2.0f * w);
	// Its share of the vehicle's weight.
	Node *parent = get_parent();
	real_t mass = 0.0;
	if (PhysXVehicle3D *v = Object::cast_to<PhysXVehicle3D>(parent)) {
		mass = v->get_mass();
	} else if (PhysXMotorcycle3D *m = Object::cast_to<PhysXMotorcycle3D>(parent)) {
		mass = m->get_mass();
	} else if (PhysXTank3D *t = Object::cast_to<PhysXTank3D>(parent)) {
		mass = t->get_mass();
	}
	if (mass > 0.0 && parent) {
		int count = 0;
		for (int i = 0; i < parent->get_child_count(); i++) {
			count += Object::cast_to<PhysXVehicleWheel3D>(parent->get_child(i)) ? 1 : 0;
		}
		set_meta(SNAME("ground_mass"), float(mass) / MAX(count, 1));
	}
}

void PhysXVehicleWheel3D::set_radius(real_t p_v) {
	radius = p_v;
	if (is_inside_tree()) {
		_join_world_contact();
	}
	update_gizmos();
	_rebuild_parent_if_live();
}
void PhysXVehicleWheel3D::set_half_width(real_t p_v) {
	half_width = p_v;
	if (is_inside_tree()) {
		_join_world_contact();
	}
	_rebuild_parent_if_live();
}
void PhysXVehicleWheel3D::set_wheel_mass(real_t p_v) {
	wheel_mass = p_v;
	_rebuild_parent_if_live();
}
void PhysXVehicleWheel3D::set_wheel_moment_of_inertia(real_t p_v) {
	wheel_moment_of_inertia = p_v;
	_rebuild_parent_if_live();
}
void PhysXVehicleWheel3D::set_damping_rate(real_t p_v) {
	damping_rate = p_v;
	_rebuild_parent_if_live();
}
void PhysXVehicleWheel3D::set_suspension_travel(real_t p_v) {
	suspension_travel = p_v;
	update_gizmos();
	_rebuild_parent_if_live();
}
void PhysXVehicleWheel3D::set_suspension_stiffness(real_t p_v) {
	suspension_stiffness = p_v;
	update_gizmos();
	_rebuild_parent_if_live();
}
void PhysXVehicleWheel3D::set_suspension_damping(real_t p_v) {
	suspension_damping = p_v;
	_rebuild_parent_if_live();
}
void PhysXVehicleWheel3D::set_tire_lateral_stiffness(real_t p_v) {
	tire_lateral_stiffness = p_v;
	_rebuild_parent_if_live();
}
void PhysXVehicleWheel3D::set_tire_longitudinal_stiffness(real_t p_v) {
	tire_longitudinal_stiffness = p_v;
	_rebuild_parent_if_live();
}
void PhysXVehicleWheel3D::set_tire_camber_stiffness(real_t p_v) {
	tire_camber_stiffness = p_v;
	_rebuild_parent_if_live();
}
void PhysXVehicleWheel3D::set_tire_friction(real_t p_v) {
	tire_friction = p_v;
	_rebuild_parent_if_live();
}
void PhysXVehicleWheel3D::set_tire_rest_grip(real_t p_v) {
	tire_rest_grip = p_v;
	_rebuild_parent_if_live();
}
void PhysXVehicleWheel3D::set_tire_slide_grip(real_t p_v) {
	tire_slide_grip = p_v;
	_rebuild_parent_if_live();
}
void PhysXVehicleWheel3D::set_tire_pressure(real_t p_v) {
	tire_pressure = MAX(p_v, (real_t)10.0); // Read live by the soft-soil step: no rebuild.
}
PhysXVehicleWheel3D::TreadSoil PhysXVehicleWheel3D::tread_soil(Tread p_tread) {
	// Lugs and chain links shear the soil below the tread's face as well as at it (Wong 2008, sec. 2.6: grousers);
	// forestry chains add about a fifth to a wheel's pull on soft ground (Forest Research FCTN013; Sever et al.
	// 2018, CROJFE 39). Open voids throw clay out sooner and fill slower; a closed road tread packs and holds it.
	TreadSoil t;
	switch (p_tread) {
		case TREAD_ROAD:
			t.grip = 0.85f;
			t.fill = 1.4f;
			t.shed = 1.3f;
			t.depth = 0.009f;
			break;
		case TREAD_ALL_TERRAIN:
			break;
		case TREAD_MUD:
			t.grip = 1.12f;
			t.fill = 0.6f;
			t.shed = 0.7f;
			t.depth = 0.025f;
			break;
		case TREAD_CHAINS:
			t.grip = 1.2f;
			t.fill = 0.5f; // the links flex and crack the clay out
			t.shed = 0.75f;
			t.depth = 0.02f;
			t.clog_bite = 0.5f;
			break;
	}
	return t;
}

float PhysXVehicleWheel3D::tread_firm_grip(Tread p_tread, float p_ice, float p_snow) {
	// Dry: rubber on the road; chains ride on steel. Ice: chains about treble a tyre's 0.1; snow: about double
	// (CRREL tyre and chain tests, Shoop et al.).
	static const float dry[4] = { 1.0f, 0.97f, 0.9f, 0.6f };
	static const float ice[4] = { 1.0f, 1.0f, 1.0f, 3.0f };
	static const float snow[4] = { 0.9f, 1.0f, 1.3f, 2.0f };
	const int i = CLAMP((int)p_tread, 0, 3);
	const float a = CLAMP(p_ice, 0.0f, 1.0f);
	const float b = CLAMP(p_snow, 0.0f, 1.0f) * (1.0f - a);
	return dry[i] * (1.0f - a - b) + ice[i] * a + snow[i] * b;
}

void PhysXVehicleWheel3D::set_use_as_steering(bool p_v) {
	use_as_steering = p_v;
	_rebuild_parent_if_live();
}
void PhysXVehicleWheel3D::set_use_as_traction(bool p_v) {
	use_as_traction = p_v;
	_rebuild_parent_if_live();
}

PackedStringArray PhysXVehicleWheel3D::get_configuration_warnings() const {
	PackedStringArray warnings = Node3D::get_configuration_warnings();
	if (!Object::cast_to<PhysXVehicle3D>(get_parent()) && !Object::cast_to<PhysXMotorcycle3D>(get_parent()) && !Object::cast_to<PhysXTank3D>(get_parent())) {
		warnings.push_back(RTR("PhysXVehicleWheel3D serves to provide a wheel to a PhysXVehicle3D, PhysXMotorcycle3D, or PhysXTank3D. Please use it as a child of one of those."));
	}
	return warnings;
}

void PhysXVehicleWheel3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_radius", "value"), &PhysXVehicleWheel3D::set_radius);
	ClassDB::bind_method(D_METHOD("get_radius"), &PhysXVehicleWheel3D::get_radius);
	ClassDB::bind_method(D_METHOD("set_half_width", "value"), &PhysXVehicleWheel3D::set_half_width);
	ClassDB::bind_method(D_METHOD("get_half_width"), &PhysXVehicleWheel3D::get_half_width);
	ClassDB::bind_method(D_METHOD("set_wheel_mass", "value"), &PhysXVehicleWheel3D::set_wheel_mass);
	ClassDB::bind_method(D_METHOD("get_wheel_mass"), &PhysXVehicleWheel3D::get_wheel_mass);
	ClassDB::bind_method(D_METHOD("set_wheel_moment_of_inertia", "value"), &PhysXVehicleWheel3D::set_wheel_moment_of_inertia);
	ClassDB::bind_method(D_METHOD("get_wheel_moment_of_inertia"), &PhysXVehicleWheel3D::get_wheel_moment_of_inertia);
	ClassDB::bind_method(D_METHOD("set_damping_rate", "value"), &PhysXVehicleWheel3D::set_damping_rate);
	ClassDB::bind_method(D_METHOD("get_damping_rate"), &PhysXVehicleWheel3D::get_damping_rate);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "radius", PROPERTY_HINT_RANGE, "0.05,2,0.01,or_greater"), "set_radius", "get_radius");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "half_width", PROPERTY_HINT_RANGE, "0.01,1,0.01,or_greater"), "set_half_width", "get_half_width");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wheel_mass", PROPERTY_HINT_RANGE, "0.1,200,0.1,or_greater"), "set_wheel_mass", "get_wheel_mass");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wheel_moment_of_inertia", PROPERTY_HINT_RANGE, "0.01,50,0.01,or_greater"), "set_wheel_moment_of_inertia", "get_wheel_moment_of_inertia");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "damping_rate", PROPERTY_HINT_RANGE, "0,5,0.01,or_greater"), "set_damping_rate", "get_damping_rate");

	ClassDB::bind_method(D_METHOD("set_suspension_travel", "value"), &PhysXVehicleWheel3D::set_suspension_travel);
	ClassDB::bind_method(D_METHOD("get_suspension_travel"), &PhysXVehicleWheel3D::get_suspension_travel);
	ClassDB::bind_method(D_METHOD("set_suspension_stiffness", "value"), &PhysXVehicleWheel3D::set_suspension_stiffness);
	ClassDB::bind_method(D_METHOD("get_suspension_stiffness"), &PhysXVehicleWheel3D::get_suspension_stiffness);
	ClassDB::bind_method(D_METHOD("set_suspension_damping", "value"), &PhysXVehicleWheel3D::set_suspension_damping);
	ClassDB::bind_method(D_METHOD("get_suspension_damping"), &PhysXVehicleWheel3D::get_suspension_damping);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "suspension_travel", PROPERTY_HINT_RANGE, "0.01,1,0.01,or_greater"), "set_suspension_travel", "get_suspension_travel");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "suspension_stiffness", PROPERTY_HINT_RANGE, "1000,200000,100,or_greater"), "set_suspension_stiffness", "get_suspension_stiffness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "suspension_damping", PROPERTY_HINT_RANGE, "0,20000,10,or_greater"), "set_suspension_damping", "get_suspension_damping");

	ClassDB::bind_method(D_METHOD("set_tire_lateral_stiffness", "value"), &PhysXVehicleWheel3D::set_tire_lateral_stiffness);
	ClassDB::bind_method(D_METHOD("get_tire_lateral_stiffness"), &PhysXVehicleWheel3D::get_tire_lateral_stiffness);
	ClassDB::bind_method(D_METHOD("set_tire_longitudinal_stiffness", "value"), &PhysXVehicleWheel3D::set_tire_longitudinal_stiffness);
	ClassDB::bind_method(D_METHOD("get_tire_longitudinal_stiffness"), &PhysXVehicleWheel3D::get_tire_longitudinal_stiffness);
	ClassDB::bind_method(D_METHOD("set_tire_camber_stiffness", "value"), &PhysXVehicleWheel3D::set_tire_camber_stiffness);
	ClassDB::bind_method(D_METHOD("get_tire_camber_stiffness"), &PhysXVehicleWheel3D::get_tire_camber_stiffness);
	ClassDB::bind_method(D_METHOD("set_tire_friction", "value"), &PhysXVehicleWheel3D::set_tire_friction);
	ClassDB::bind_method(D_METHOD("get_tire_friction"), &PhysXVehicleWheel3D::get_tire_friction);
	ClassDB::bind_method(D_METHOD("set_tire_rest_grip", "value"), &PhysXVehicleWheel3D::set_tire_rest_grip);
	ClassDB::bind_method(D_METHOD("get_tire_rest_grip"), &PhysXVehicleWheel3D::get_tire_rest_grip);
	ClassDB::bind_method(D_METHOD("set_tire_slide_grip", "value"), &PhysXVehicleWheel3D::set_tire_slide_grip);
	ClassDB::bind_method(D_METHOD("get_tire_slide_grip"), &PhysXVehicleWheel3D::get_tire_slide_grip);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "tire_lateral_stiffness", PROPERTY_HINT_RANGE, "1000,100000,100,or_greater"), "set_tire_lateral_stiffness", "get_tire_lateral_stiffness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "tire_longitudinal_stiffness", PROPERTY_HINT_RANGE, "1000,100000,100,or_greater"), "set_tire_longitudinal_stiffness", "get_tire_longitudinal_stiffness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "tire_camber_stiffness", PROPERTY_HINT_RANGE, "0,100000,100,or_greater"), "set_tire_camber_stiffness", "get_tire_camber_stiffness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "tire_friction", PROPERTY_HINT_RANGE, "0.1,3,0.01,or_greater"), "set_tire_friction", "get_tire_friction");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "tire_rest_grip", PROPERTY_HINT_RANGE, "0,1.5,0.01"), "set_tire_rest_grip", "get_tire_rest_grip");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "tire_slide_grip", PROPERTY_HINT_RANGE, "0,1.5,0.01"), "set_tire_slide_grip", "get_tire_slide_grip");
	ClassDB::bind_method(D_METHOD("set_tire_pressure", "value"), &PhysXVehicleWheel3D::set_tire_pressure);
	ClassDB::bind_method(D_METHOD("get_tire_pressure"), &PhysXVehicleWheel3D::get_tire_pressure);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "tire_pressure", PROPERTY_HINT_RANGE, "10,1000,1,or_greater,suffix:kPa"), "set_tire_pressure", "get_tire_pressure");
	ClassDB::bind_method(D_METHOD("set_tread", "tread"), &PhysXVehicleWheel3D::set_tread);
	ClassDB::bind_method(D_METHOD("get_tread"), &PhysXVehicleWheel3D::get_tread);
	ClassDB::bind_static_method("PhysXVehicleWheel3D", D_METHOD("tread_firm_grip", "tread", "ice", "snow"), &PhysXVehicleWheel3D::tread_firm_grip);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "tread", PROPERTY_HINT_ENUM, "Road,All Terrain,Mud,Chains"), "set_tread", "get_tread");
	BIND_ENUM_CONSTANT(TREAD_ROAD);
	BIND_ENUM_CONSTANT(TREAD_ALL_TERRAIN);
	BIND_ENUM_CONSTANT(TREAD_MUD);
	BIND_ENUM_CONSTANT(TREAD_CHAINS);

	ClassDB::bind_method(D_METHOD("set_use_as_steering", "value"), &PhysXVehicleWheel3D::set_use_as_steering);
	ClassDB::bind_method(D_METHOD("is_used_as_steering"), &PhysXVehicleWheel3D::is_used_as_steering);
	ClassDB::bind_method(D_METHOD("set_use_as_traction", "value"), &PhysXVehicleWheel3D::set_use_as_traction);
	ClassDB::bind_method(D_METHOD("is_used_as_traction"), &PhysXVehicleWheel3D::is_used_as_traction);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "use_as_steering"), "set_use_as_steering", "is_used_as_steering");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "use_as_traction"), "set_use_as_traction", "is_used_as_traction");
}

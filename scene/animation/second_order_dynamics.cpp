/**************************************************************************/
/*  second_order_dynamics.cpp                                             */
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

#include "second_order_dynamics.h"

#include "core/object/class_db.h"

void SecondOrderDynamics::_configure() {
	system.configure(frequency, damping, response);
}

Vector4 SecondOrderDynamics::_to_vector4(const Variant &p_value) {
	switch (p_value.get_type()) {
		case Variant::INT:
		case Variant::FLOAT:
			return Vector4((real_t)p_value, 0, 0, 0);
		case Variant::VECTOR2: {
			const Vector2 v = p_value;
			return Vector4(v.x, v.y, 0, 0);
		}
		case Variant::VECTOR3: {
			const Vector3 v = p_value;
			return Vector4(v.x, v.y, v.z, 0);
		}
		case Variant::VECTOR4:
			return p_value;
		default:
			ERR_FAIL_V_MSG(Vector4(), "SecondOrderDynamics takes a float, Vector2, Vector3 or Vector4.");
	}
}

Variant SecondOrderDynamics::_from_vector4(const Vector4 &p_value) const {
	switch (value_type) {
		case Variant::VECTOR2:
			return Vector2(p_value.x, p_value.y);
		case Variant::VECTOR3:
			return Vector3(p_value.x, p_value.y, p_value.z);
		case Variant::VECTOR4:
			return p_value;
		default:
			return (double)p_value.x;
	}
}

void SecondOrderDynamics::set_frequency(real_t p_frequency) {
	frequency = MAX(p_frequency, (real_t)1e-3);
	_configure();
}

real_t SecondOrderDynamics::get_frequency() const {
	return frequency;
}

void SecondOrderDynamics::set_damping(real_t p_damping) {
	damping = MAX(p_damping, (real_t)0.0);
	_configure();
}

real_t SecondOrderDynamics::get_damping() const {
	return damping;
}

void SecondOrderDynamics::set_response(real_t p_response) {
	response = p_response;
	_configure();
}

real_t SecondOrderDynamics::get_response() const {
	return response;
}

void SecondOrderDynamics::reset(const Variant &p_value) {
	value_type = p_value.get_type() == Variant::INT ? Variant::FLOAT : p_value.get_type();
	system.reset(_to_vector4(p_value));
}

Variant SecondOrderDynamics::update(double p_delta, const Variant &p_target) {
	const Variant::Type type = p_target.get_type() == Variant::INT ? Variant::FLOAT : p_target.get_type();
	if (type != value_type) {
		// The first value of a new kind starts the system there, at rest.
		reset(p_target);
		return p_target;
	}
	return _from_vector4(system.update(p_delta, _to_vector4(p_target)));
}

Variant SecondOrderDynamics::get_value() const {
	return _from_vector4(system.y);
}

Variant SecondOrderDynamics::get_velocity() const {
	return _from_vector4(system.yd);
}

void SecondOrderDynamics::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_frequency", "frequency"), &SecondOrderDynamics::set_frequency);
	ClassDB::bind_method(D_METHOD("get_frequency"), &SecondOrderDynamics::get_frequency);
	ClassDB::bind_method(D_METHOD("set_damping", "damping"), &SecondOrderDynamics::set_damping);
	ClassDB::bind_method(D_METHOD("get_damping"), &SecondOrderDynamics::get_damping);
	ClassDB::bind_method(D_METHOD("set_response", "response"), &SecondOrderDynamics::set_response);
	ClassDB::bind_method(D_METHOD("get_response"), &SecondOrderDynamics::get_response);
	ClassDB::bind_method(D_METHOD("reset", "value"), &SecondOrderDynamics::reset);
	ClassDB::bind_method(D_METHOD("update", "delta", "target"), &SecondOrderDynamics::update);
	ClassDB::bind_method(D_METHOD("get_value"), &SecondOrderDynamics::get_value);
	ClassDB::bind_method(D_METHOD("get_velocity"), &SecondOrderDynamics::get_velocity);

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "frequency", PROPERTY_HINT_RANGE, "0.001,30,0.001,or_greater,suffix:Hz"), "set_frequency", "get_frequency");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "damping", PROPERTY_HINT_RANGE, "0,4,0.001,or_greater"), "set_damping", "get_damping");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "response", PROPERTY_HINT_RANGE, "-4,4,0.001,or_less,or_greater"), "set_response", "get_response");
}

SecondOrderDynamics::SecondOrderDynamics() {
	_configure();
	system.reset(Vector4());
}

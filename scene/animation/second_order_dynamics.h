/**************************************************************************/
/*  second_order_dynamics.h                                               */
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

#include "core/math/math_funcs.h"
#include "core/math/vector4.h"
#include "core/object/ref_counted.h"

// Second-order dynamics: an output that follows a target like a mass on a spring and damper, so
// procedural motion starts, overshoots and settles the way a body does instead of moving on a straight
// line or a fixed-length tween.
//
// The system is y + k1 y' + k2 y'' = x + k3 x' with k1 = zeta / (pi f), k2 = 1 / (2 pi f)^2 and
// k3 = r zeta / (2 pi f) (t3ssel8r, "Giving Personality to Procedural Animations using Math", 2022):
// `f` is the natural frequency in Hz (how fast it responds), `zeta` the damping ratio (below 1 it
// overshoots, 1 is critically damped, above 1 it creeps in), `r` the initial response (0 eases in, above 1
// overshoots at the start, below 0 first moves the other way: anticipation). It is stepped semi-implicitly
// with k2 raised to the stability bound max(k2, T^2/2 + T k1/2, T k1), so any frame time stays stable.
// A step of zero time changes nothing, so stills and paused frames hold exactly.
template <typename T>
struct SecondOrderSystem {
	real_t k1 = 0.0;
	real_t k2 = 1.0;
	real_t k3 = 0.0;
	T xp = T();
	T y = T();
	T yd = T();

	void configure(real_t p_frequency, real_t p_damping, real_t p_response) {
		const real_t w = Math::TAU * MAX(p_frequency, (real_t)1e-4);
		k1 = p_damping / (Math::PI * MAX(p_frequency, (real_t)1e-4));
		k2 = (real_t)1.0 / (w * w);
		k3 = p_response * p_damping / w;
	}

	void reset(const T &p_value) {
		xp = p_value;
		y = p_value;
		yd = T();
	}

	const T &update(real_t p_delta, const T &p_target) {
		if (p_delta <= (real_t)0.0) {
			return y;
		}
		const T xd = (p_target - xp) / p_delta;
		xp = p_target;
		const real_t k2_stable = MAX(k2, MAX(p_delta * p_delta * (real_t)0.5 + p_delta * k1 * (real_t)0.5, p_delta * k1));
		y = y + yd * p_delta;
		yd = yd + (p_target + xd * k3 - y - yd * k1) * (p_delta / k2_stable);
		return y;
	}
};

class SecondOrderDynamics : public RefCounted {
	GDCLASS(SecondOrderDynamics, RefCounted);

	real_t frequency = 1.0;
	real_t damping = 1.0;
	real_t response = 0.0;
	Variant::Type value_type = Variant::FLOAT;
	SecondOrderSystem<Vector4> system;

	void _configure();
	static Vector4 _to_vector4(const Variant &p_value);
	Variant _from_vector4(const Vector4 &p_value) const;

protected:
	static void _bind_methods();

public:
	void set_frequency(real_t p_frequency);
	real_t get_frequency() const;
	void set_damping(real_t p_damping);
	real_t get_damping() const;
	void set_response(real_t p_response);
	real_t get_response() const;

	void reset(const Variant &p_value);
	Variant update(double p_delta, const Variant &p_target);
	Variant get_value() const;
	Variant get_velocity() const;

	SecondOrderDynamics();
};

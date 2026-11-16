/**************************************************************************/
/*  ground_terramechanics.cpp                                             */
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

#include "ground_terramechanics.h"

#include "core/math/math_funcs.h"
#include "core/typedefs.h"

namespace GroundTerramechanics {

float bekker_k(const GroundSoil &p_soil, float p_width) {
	return p_soil.kc / MAX(p_width, 0.02f) + p_soil.kphi;
}

float wheel_static_sinkage(const GroundSoil &p_soil, float p_load, float p_radius, float p_width, float p_inflation) {
	if (!p_soil.soft || p_load <= 0.0f || p_radius <= 0.0f || p_width <= 0.0f) {
		return 0.0f;
	}
	const float n = CLAMP(p_soil.n, 0.1f, 2.9f);
	const float k = bekker_k(p_soil, p_width);
	const float w = p_load / 1000.0f; // kN
	const float d = 2.0f * p_radius;
	float z = Math::pow(3.0f * w / ((3.0f - n) * p_width * k * Math::sqrt(d)), 2.0f / (2.0f * n + 1.0f));
	// A tyre whose inflation is below the pressure the rigid wheel would put on the ground flattens: it
	// sinks only as deep as its own pressure presses (Wong 2008, sec. 2.4, the elastic mode).
	if (p_inflation > 0.0f) {
		const float rigid_pressure = k * Math::pow(z, n);
		if (p_inflation < rigid_pressure) {
			z = Math::pow(p_inflation / k, 1.0f / n);
		}
	}
	return MIN(z, p_soil.max_sinkage);
}

float slip_sinkage(float p_static, float p_slip) {
	const float i = CLAMP(Math::abs(p_slip), 0.0f, 0.95f);
	return p_static * (1.0f + i) / (1.0f - 0.5f * i);
}

float compaction_resistance(const GroundSoil &p_soil, float p_width, float p_sinkage, float p_already) {
	if (!p_soil.soft || p_sinkage <= 0.0f) {
		return 0.0f;
	}
	const float n = p_soil.n;
	const float k = bekker_k(p_soil, p_width);
	const float d = CLAMP(p_already, 0.0f, p_sinkage);
	return 1000.0f * p_width * k * (Math::pow(p_sinkage, n + 1.0f) - Math::pow(d, n + 1.0f)) / (n + 1.0f);
}

float bulldozing_resistance(const GroundSoil &p_soil, float p_width, float p_sinkage, float p_already) {
	const float z = p_sinkage - MAX(p_already, 0.0f);
	if (!p_soil.soft || z <= 0.0f) {
		return 0.0f;
	}
	const float tp = MAX(p_soil.tan_phi, 0.02f);
	const float phi = Math::atan(tp);
	const float cos2 = Math::cos(phi) * Math::cos(phi);
	// Bearing capacity factors: Nq (Prandtl, Reissner), Nc = (Nq - 1) cot phi, Ngamma (Vesic 1973).
	const float t = Math::tan((float)Math::PI * 0.25f + phi * 0.5f);
	const float nq = Math::exp((float)Math::PI * tp) * t * t;
	const float nc = (nq - 1.0f) / tp;
	const float ng = 2.0f * (nq + 1.0f) * tp;
	const float kpc = (nc - tp) * cos2;
	const float kpg = (2.0f * ng / tp + 1.0f) * cos2;
	return 1000.0f * p_width * (0.67f * p_soil.cohesion * z * kpc + 0.5f * z * z * p_soil.unit_weight * kpg);
}

float janosi_hanamoto(float p_j_end, float p_shear_k) {
	const float j = Math::abs(p_j_end);
	const float k = MAX(p_shear_k, 1e-4f);
	if (j < 1e-6f) {
		return 0.0f;
	}
	const float x = j / k;
	// The mean of 1 - exp(-j / K) over the contact, j growing linearly from 0 to p_j_end.
	return 1.0f - (1.0f - Math::exp(-x)) / x;
}

GroundWheelSoil wheel(const GroundSoil &p_soil, float p_load, float p_radius, float p_width, float p_inflation, float p_slip, float p_slip_angle, float p_already_pressed) {
	GroundWheelSoil r;
	if (!p_soil.soft || p_load <= 0.0f || p_radius <= 0.0f || p_width <= 0.0f) {
		return r;
	}
	r.soft = true;
	r.static_sinkage = wheel_static_sinkage(p_soil, p_load, p_radius, p_width, p_inflation);
	r.sinkage = MIN(slip_sinkage(r.static_sinkage, p_slip), p_soil.max_sinkage);
	// The contact: the arc a rigid wheel of radius r has under the surface at sinkage z, or the flat of a
	// flattened tyre (its load over its inflation pressure), whichever is longer.
	const float z = r.sinkage;
	float length = p_radius * Math::acos(CLAMP(1.0f - z / p_radius, -1.0f, 1.0f));
	if (p_inflation > 0.0f) {
		length = MAX(length, p_load / 1000.0f / (p_width * p_inflation));
	}
	r.contact_length = MAX(length, 0.01f);
	r.compaction = compaction_resistance(p_soil, p_width, z, p_already_pressed);
	r.bulldozing = bulldozing_resistance(p_soil, p_width, z, p_already_pressed);
	const float area = p_width * r.contact_length;
	r.shear_limit = 1000.0f * area * p_soil.cohesion + p_load * p_soil.tan_phi;
	r.traction = r.shear_limit * janosi_hanamoto(CLAMP(Math::abs(p_slip), 0.0f, 1.0f) * r.contact_length, p_soil.shear_k);
	r.lateral = r.shear_limit * janosi_hanamoto(Math::abs(Math::tan(CLAMP(p_slip_angle, -1.4f, 1.4f))) * r.contact_length, p_soil.shear_k);
	return r;
}

} // namespace GroundTerramechanics

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

GroundTyreFlat tyre_flat(float p_load, float p_radius, float p_width, float p_inflation) {
	GroundTyreFlat f;
	if (p_load <= 0.0f || p_radius <= 0.0f || p_width <= 0.0f || p_inflation <= 0.0f) {
		return f;
	}
	const float d = 2.0f * p_radius;
	// The load stands on the air and the carcass over the flat (Wong 2008, sec. 2.4: p_g = p_i + p_c, W = p_g b L).
	const float pg = p_inflation + TYRE_CARCASS_KPA;
	// The deflection from the tyre's vertical stiffness, which grows with the pressure and as sqrt(width x
	// diameter) (Rhyne 2005, "Development of a vertical stiffness relationship for belted radial tires", Tire Sci.
	// Technol. 33(3)): k = C p_g sqrt(b D), C set so a 225/65 R17 on 220 kPa is about 220 N/mm, as measured for
	// passenger tyres. Past about a third of its section height a tyre bottoms on its rim (the limit airing-down
	// guides and tyre makers give): it stops there and the pressure on the flat rises instead. Section height from
	// the width (a 75 series off-road tyre), kept within a sensible share of the radius.
	const float h = CLAMP(0.75f * p_width, 0.12f * p_radius, 0.5f * p_radius);
	const float d_max = 0.35f * h;
	const float k = 2329.0f * pg * Math::sqrt(p_width * d); // N/m
	f.deflection = MIN(p_load / k, d_max);
	// The flat: as long as the pressure needs, but no longer than the chord at the deflection allows.
	const float chord = 2.0f * Math::sqrt(MAX(d * f.deflection - f.deflection * f.deflection, 0.0f));
	f.length = MAX(MIN(p_load / 1000.0f / (p_width * pg), MAX(chord, 0.01f)), 0.01f);
	f.pressure = p_load / 1000.0f / (p_width * f.length);
	return f;
}

float layered_sinkage(const GroundSoil &p_soil, float p_sinkage, float p_width) {
	// A layer H deep over a rigid base (Salman 2022, after Yegorov; Bekker 1969 on shallow soil): while the layer
	// left under a load of width b is thicker than b the base is not felt and p = k z^n; past z0 = H - b the
	// stress bulb presses on the base through a compacted cone and the apparent modulus grows exponentially,
	// k_app = k (1 + B (exp(c (z - z0) / b) - 1)), B 1 and c 2 here (the published fits are for one sandy loam).
	// The same pressure then sinks less, and the load stops short of the base unless it is heavy enough.
	const float h = p_soil.max_sinkage;
	if (!p_soil.layered || h <= 0.01f || p_sinkage <= 0.0f) {
		return p_sinkage;
	}
	const float b = MAX(p_width, 0.02f);
	const float z0 = MAX(h - b, 0.0f);
	if (p_sinkage <= z0) {
		return p_sinkage;
	}
	const float n = CLAMP(p_soil.n, 0.1f, 2.9f);
	const float target = Math::pow(p_sinkage, n); // the pressure over k
	auto pressure = [&](float p_z) {
		return (1.0f + (Math::exp(2.0f * (p_z - z0) / b) - 1.0f)) * Math::pow(p_z, n);
	};
	if (pressure(h) <= target) {
		return h;
	}
	float lo = z0;
	float hi = MIN(p_sinkage, h);
	for (int i = 0; i < 16; i++) {
		const float mid = 0.5f * (lo + hi);
		if (pressure(mid) < target) {
			lo = mid;
		} else {
			hi = mid;
		}
	}
	return 0.5f * (lo + hi);
}

float pan_share(const GroundSoil &p_soil, float p_sinkage) {
	const float h = p_soil.max_sinkage;
	if (!p_soil.layered || h <= 0.01f) {
		return 0.0f;
	}
	// The tread bites the base once the film of soil left under it is thinner than its lugs (about 3 cm); through a
	// thicker film the soil shears first.
	return 1.0f - Math::smoothstep(0.0f, 0.03f, h - p_sinkage);
}

float pan_shear_limit(const GroundSoil &p_soil, float p_load, float p_sinkage, float p_shear_limit) {
	// On the base the tyre's load bears on firm ground (Coulomb friction, mu W); the soil above never lowers that.
	return Math::lerp(p_shear_limit, MAX(p_shear_limit, PAN_FRICTION * MAX(p_load, 0.0f)), pan_share(p_soil, p_sinkage));
}

float tyre_rolling_coefficient(float p_inflation, float p_speed) {
	// Gillespie, "Fundamentals of Vehicle Dynamics" (1992), eq. 4-2: f_r = 0.005 + (1 / p)(0.01 + 0.0095 (v / 100)^2),
	// p in bar, v in km/h: the carcass's hysteresis grows as the tyre flexes more.
	const float bar = MAX(p_inflation, 10.0f) / 100.0f;
	const float kmh = Math::abs(p_speed) * 3.6f / 100.0f;
	return 0.005f + (0.01f + 0.0095f * kmh * kmh) / bar;
}

// The sinkage with no base under the layer (the layer then caps it).
static float _free_sinkage(const GroundSoil &p_soil, float p_load, float p_radius, float p_width, float p_inflation) {
	if (!p_soil.soft || p_load <= 0.0f || p_radius <= 0.0f || p_width <= 0.0f) {
		return 0.0f;
	}
	const float n = CLAMP(p_soil.n, 0.1f, 2.9f);
	const float k = bekker_k(p_soil, p_width);
	const float w = p_load / 1000.0f; // kN
	const float d = 2.0f * p_radius;
	float z = Math::pow(3.0f * w / ((3.0f - n) * p_width * k * Math::sqrt(d)), 2.0f / (2.0f * n + 1.0f));
	// A tyre whose ground pressure (inflation plus carcass, more once it bottoms) is below the pressure the rigid
	// wheel would put on the ground flattens: it sinks only as deep as that pressure presses (Wong 2008, sec. 2.4,
	// the elastic mode).
	if (p_inflation > 0.0f) {
		const float rigid_pressure = k * Math::pow(z, n);
		const float pg = tyre_flat(p_load, p_radius, p_width, p_inflation).pressure;
		if (pg < rigid_pressure) {
			z = Math::pow(pg / k, 1.0f / n);
		}
	}
	return z;
}

float wheel_static_sinkage(const GroundSoil &p_soil, float p_load, float p_radius, float p_width, float p_inflation) {
	return MIN(layered_sinkage(p_soil, _free_sinkage(p_soil, p_load, p_radius, p_width, p_inflation), p_width), p_soil.max_sinkage);
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

float wheel_bulldozing_resistance(const GroundSoil &p_soil, float p_width, float p_radius, float p_sinkage, float p_already) {
	// Bekker's compaction resistance (the pressure on the wheel's front arc) holds for sinkage up to about a sixth
	// of the diameter (Bekker 1969; Wong, "Theory of Ground Vehicles", sec. 2.4.1). Within it the wheel presses
	// the soil down and the field moves what it displaces to the rims, as in Chrono's SCM, which has no separate
	// bulldozing force; bulldozing is marginal at small sinkage (NSF PAR 10304353, 2021). Past D/6 the soil ahead
	// fails in passive earth pressure and the wheel pushes a wedge of it: Bekker's bulldozing on that depth.
	const float onset = p_radius / 3.0f;
	return bulldozing_resistance(p_soil, p_width, p_sinkage, MAX(p_already, onset));
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

GroundSoil torn_mat(const GroundSoil &p_soil) {
	GroundSoil t = p_soil;
	if (p_soil.mat_tear <= 0.0f) {
		return t;
	}
	t.kc = p_soil.torn_kc;
	t.kphi = p_soil.torn_kphi;
	t.cohesion = p_soil.torn_cohesion;
	t.tan_phi = p_soil.torn_tan_phi;
	t.mat_tear = 0.0f;
	return t;
}

GroundWheelSoil wheel(const GroundSoil &p_soil, float p_load, float p_radius, float p_width, float p_inflation, float p_slip, float p_slip_angle, float p_already_pressed) {
	GroundWheelSoil r;
	if (!p_soil.soft || p_load <= 0.0f || p_radius <= 0.0f || p_width <= 0.0f) {
		return r;
	}
	if (p_soil.mat_tear > 0.0f) {
		// Muskeg (Wong, Garber, Radforth and Dowell 1979; Wong, Radforth and Preston-Thomas 1982): the root mat
		// bears as p = (k_p + 4 m_m / D_h) z up to a critical sinkage, where it ruptures and the bearing drops to
		// the weak peat's under it (bevameter tests put the break at 8 to 14 cm). The mat's term rides on Bekker's
		// kc / b (n 1, D_h about the tyre's width). Pressed past it, the load tears through. The mat is a web of
		// roots, not loose soil: a slipping tread does not dig it out (no slip sinkage) until it spins hard enough
		// to shred it, past a slip of about 0.7.
		GroundSoil intact = p_soil;
		intact.mat_tear = 0.0f;
		r = wheel(intact, p_load, p_radius, p_width, p_inflation, p_slip, p_slip_angle, p_already_pressed);
		if (r.static_sinkage <= p_soil.mat_tear && Math::abs(p_slip) < 0.7f) {
			r.sinkage = r.static_sinkage;
			return r;
		}
		r = wheel(torn_mat(p_soil), p_load, p_radius, p_width, p_inflation, p_slip, p_slip_angle, p_already_pressed);
		r.torn = true;
		return r;
	}
	r.soft = true;
	// The base stiffens what the load presses; what a spinning wheel digs out on top of that it throws out (Lyasko's slip sinkage).
	const float free_z = _free_sinkage(p_soil, p_load, p_radius, p_width, p_inflation);
	r.static_sinkage = MIN(layered_sinkage(p_soil, free_z, p_width), p_soil.max_sinkage);
	// The extra a spinning wheel digs is soil thrown out, not pressed, so the base does not stiffen it. A wheel
	// sliding sideways digs in too: the soil flows out from under it to the side, and the sinkage grows with the
	// slip angle as with the slip ratio (Ishigami et al. 2007, J. Field Robotics, "Terramechanics-based model for
	// steering maneuver of planetary exploration rovers on loose soil"; Wong 2008 on lateral slip sinkage). Here
	// on the combined slip, the slip ratio and tan of the slip angle (the side slip's share of the travel).
	const float side = Math::abs(Math::tan(CLAMP(p_slip_angle, -1.4f, 1.4f)));
	const float combined = MIN(Math::sqrt(p_slip * p_slip + side * side), 1.0f);
	r.sinkage = MIN(r.static_sinkage + MAX(slip_sinkage(free_z, combined) - free_z, 0.0f), p_soil.max_sinkage);
	// The contact: the arc a rigid wheel of radius r has under the surface at sinkage z, or the flat of a
	// flattened tyre (its load over its inflation pressure), whichever is longer.
	const float z = r.sinkage;
	float length = p_radius * Math::acos(CLAMP(1.0f - z / p_radius, -1.0f, 1.0f));
	if (p_inflation > 0.0f) {
		length = MAX(length, tyre_flat(p_load, p_radius, p_width, p_inflation).length);
	}
	r.contact_length = MAX(length, 0.01f);
	r.compaction = compaction_resistance(p_soil, p_width, z, p_already_pressed);
	r.bulldozing = wheel_bulldozing_resistance(p_soil, p_width, p_radius, z, p_already_pressed);
	const float area = p_width * r.contact_length;
	r.shear_limit = pan_shear_limit(p_soil, p_load, z, 1000.0f * area * p_soil.cohesion + p_load * p_soil.tan_phi);
	r.traction = r.shear_limit * janosi_hanamoto(CLAMP(Math::abs(p_slip), 0.0f, 1.0f) * r.contact_length, p_soil.shear_k);
	r.lateral = r.shear_limit * janosi_hanamoto(Math::abs(Math::tan(CLAMP(p_slip_angle, -1.4f, 1.4f))) * r.contact_length, p_soil.shear_k);
	return r;
}


float clogged_grip(const GroundSoil &p_soil, float p_load, float p_area) {
	const float clean = 1000.0f * p_area * p_soil.cohesion + p_load * p_soil.tan_phi;
	const float slick = 1000.0f * p_area * p_soil.adhesion + p_load * 0.6f * p_soil.tan_phi;
	return clean > 0.0f ? CLAMP(slick / clean, 0.05f, 1.0f) : 1.0f;
}

float adhesion_resistance(const GroundSoil &p_soil, float p_width, float p_sinkage) {
	return 1000.0f * p_soil.adhesion * p_width * MAX(p_sinkage, 0.0f);
}

float shedding_speed(const GroundSoil &p_soil, float p_radius) {
	// w^2 r = ca / (rho t): v = w r = sqrt(ca r / (rho t)), a 2 cm lug of clay at 1700 kg/m3.
	const float ca = 1000.0f * MAX(p_soil.adhesion, 0.05f);
	return Math::sqrt(ca * MAX(p_radius, 0.05f) / (1700.0f * 0.02f));
}

} // namespace GroundTerramechanics

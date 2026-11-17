/**************************************************************************/
/*  ground_terramechanics.h                                               */
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

// Soft soil under wheels and feet (docs/ideas/sand.md, stage 1): the terramechanics of Bekker and Wong, as
// Project Chrono's Soil Contact Model and off-road games (MudRunner) use it at run time.
//
// - Sinkage of a rigid wheel (Bekker 1969): z0 = (3 W / ((3 - n) b k sqrt(D)))^(2 / (2n + 1)), k = kc / b +
//   kphi. A tyre softer than the ground under it flattens instead (Wong, "Theory of Ground Vehicles", 4th
//   ed. 2008, sec. 2.4): the sinkage is that of its inflation pressure, z = (p / k)^(1 / n).
// - Slip sinkage (Lyasko 2010, "Slip sinkage effect in soil-vehicle mechanics", J. Terramechanics 47):
//   z = z0 (1 + i) / (1 - 0.5 i). A spinning wheel digs itself in.
// - Compaction resistance (Bekker): Rc = b k z^(n + 1) / (n + 1), only for the depth not already pressed
//   (a wheel in an old rut does less work: multi-pass).
// - Bulldozing resistance (Bekker 1969, as given in Wong 2008): Rb = b (0.67 c z Kpc + 0.5 z^2 gamma Kpgamma), Kpc =
//   (Nc - tan phi) cos^2 phi, Kpgamma = (2 Ngamma / tan phi + 1) cos^2 phi, bearing factors after Terzaghi
//   and Vesic, for the new depth only.
// - Traction (Janosi and Hanamoto 1961): the shear stress along the contact grows with the shear
//   displacement j as (c + sigma tan phi)(1 - exp(-j / K)); integrated over a contact of length L at slip
//   i (j = i x): H = (A c + W tan phi) (1 - K / (i L) (1 - exp(-i L / K))). Lateral the same with
//   j = x tan(alpha).
// Soil values per material come from Wong's table 2.2 (dry sand, sandy loam, clayey soil) and his shear
// deformation moduli K.

struct GroundSoil {
	float n = 1.1f;
	float kc = 0.99f; // kN/m^(n+1)
	float kphi = 1528.43f; // kN/m^(n+2)
	float cohesion = 1.04f; // kPa
	float tan_phi = 0.5317f; // internal friction
	float shear_k = 0.025f; // m, shear deformation modulus
	float unit_weight = 15.7f; // kN/m3
	float tan_repose = 0.65f;
	float max_sinkage = 0.3f; // m, the loose layer
	float stickiness = 0.0f; // 0..1, how it clings to rubber and steel (docs/ideas/mud.md, stage 3)
	float adhesion = 0.0f; // kPa, its tangential adhesion to rubber and steel (Wong's ca)
	bool soft = false; // false: firm ground, rock, no soil
};

struct GroundWheelSoil {
	float static_sinkage = 0.0f; // m, at no slip
	float sinkage = 0.0f; // m, with slip sinkage
	float contact_length = 0.0f; // m
	float compaction = 0.0f; // N
	float bulldozing = 0.0f; // N
	float shear_limit = 0.0f; // N, Mohr-Coulomb: A c + W tan phi
	float traction = 0.0f; // N, the longitudinal force the soil gives at this slip
	float lateral = 0.0f; // N, the lateral force at this slip angle
	bool soft = false;
};

namespace GroundTerramechanics {
float bekker_k(const GroundSoil &p_soil, float p_width);
// p_load N, p_radius and p_width m, p_inflation kPa (0: rigid).
float wheel_static_sinkage(const GroundSoil &p_soil, float p_load, float p_radius, float p_width, float p_inflation);
float slip_sinkage(float p_static, float p_slip);
float compaction_resistance(const GroundSoil &p_soil, float p_width, float p_sinkage, float p_already); // N
float bulldozing_resistance(const GroundSoil &p_soil, float p_width, float p_sinkage, float p_already); // N
// A wheel's bulldozing: only the depth past a sixth of its diameter (see the .cpp).
float wheel_bulldozing_resistance(const GroundSoil &p_soil, float p_width, float p_radius, float p_sinkage, float p_already); // N
// The Janosi-Hanamoto share of the shear limit at shear displacement p_j over the contact (m): 0 to 1.
float janosi_hanamoto(float p_j_end, float p_shear_k);
GroundWheelSoil wheel(const GroundSoil &p_soil, float p_load, float p_radius, float p_width, float p_inflation, float p_slip, float p_slip_angle, float p_already_pressed);
// Mud on wheels and bellies (docs/ideas/mud.md, stage 3). A tread full of clay grips as a slick: rubber on clay,
// A ca + W tan delta (delta about 0.6 phi), not soil on soil through the lugs; the share of the clean shear
// limit it keeps (0..1).
float clogged_grip(const GroundSoil &p_soil, float p_load, float p_area);
// The clay holding the tread as it leaves the rut: ca over the trailing face (width by sinkage), N.
float adhesion_resistance(const GroundSoil &p_soil, float p_width, float p_sinkage);
// The tread speed (m/s) past which a wheel of radius p_radius throws off a lug-deep layer of this clay: when
// w^2 r passes ca / (rho t).
float shedding_speed(const GroundSoil &p_soil, float p_radius);
} // namespace GroundTerramechanics

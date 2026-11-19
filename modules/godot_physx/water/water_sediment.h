/**************************************************************************/
/*  water_sediment.h                                                      */
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

#include "core/math/vector2.h"
#include "core/math/vector3.h"

// Suspended load in water (the water contract's sediment: x mineral silt and clay, y ash and char, kg/m3) and what it
// does to the light, shared by rivers and lakes so the same water looks alike in both (docs/ideas/world-links.md,
// sediment). Mass-specific coefficients, per metre per kg/m3:
// - Mineral: absorption rising toward the blue (iron oxides in the particles, a* about 0.01-0.04 m2/g red to
//   blue; Babin et al. 2003, Bowers and Binding 2006) plus the humic matter runoff washes off with it (river
//   floods carry a_CDOM(440) of several per metre in step with their silt): brown water, darker the more it carries and strong, nearly grey scattering (b* about 0.2 m2/g for silt; Babin et al.
//   2003): tan to brown water, opaque within a few decimetres at a few hundred mg/L.
// - Ash and char: black carbon absorbs about flat across the visible (Bond and Bergstrom 2006 give 7.5 m2/g for fresh
//   soot; settled ash particles are far larger, a* of a few tenths of m2/g) and scatters less: grey to black water.
// Lakes settle it out (well mixed, an e-folding time): mineral silt within about half a day, light ash and char in
// about two (they float and sink slowly).
struct WaterSediment {
	static Vector3 mineral_absorption() { return Vector3(12.0f, 26.0f, 75.0f); }
	static Vector3 mineral_scattering() { return Vector3(200.0f, 200.0f, 200.0f); }
	static Vector3 ash_absorption() { return Vector3(300.0f, 300.0f, 300.0f); }
	static Vector3 ash_scattering() { return Vector3(150.0f, 150.0f, 150.0f); }
	static constexpr float MINERAL_SETTLE_TIME = 0.5f * 86400.0f; // s, e-folding in still water
	static constexpr float ASH_SETTLE_TIME = 2.0f * 86400.0f; // s
	// The most water carries as a flood (kg/m3): hyperconcentrated flow reaches about 60% sediment by weight
	// (Beverage and Culbertson 1964; Pierson 2005); denser, the mixture is a debris flow, the ground's to move.
	static constexpr float MAX_LOAD = 1000.0f;

	static Vector3 absorption(const Vector2 &p_c) { return mineral_absorption() * p_c.x + ash_absorption() * p_c.y; }
	static Vector3 scattering(const Vector2 &p_c) { return mineral_scattering() * p_c.x + ash_scattering() * p_c.y; }
};

// The same coefficients in shader text (for a load read on the GPU, as a river's along its length), and how the load
// goes into the water's medium:
// - Similarity (van de Hulst 1980; Wyman, Patterson and Bigio 1989; Zhao, Wu, Ramamoorthi and Bala 2014): suspended
//   particles scatter strongly forward (g about 0.95: their backscattering ratio b_b / b is about 0.02, Petzold 1972,
//   Twardowski et al. 2001, and diffusion carries about twice that), so turbid water's light has scattered many times, and
//   what carries it is the reduced scattering b (1 - g). Kept as that, the medium takes a broader phase as the load
//   dominates (g toward 0.5 with the load's share of the scattering: broad enough to light evenly, still forward
//   enough that the bed blurs out with depth rather than staying sharp) and the scattering that keeps b (1 - g): the same
//   light in the diffusion regime, without a path tracer drawing its sun light through a phase peak a few degrees
//   wide on random forward bounces (gold glitter that took thousands of samples to settle). Clear water is left as
//   it is (no load: its own scattering and anisotropy).
// - White water in loaded water: the light a foam patch sends back has crossed millimetres to centimetres of the water
//   between its bubbles, so foam takes the water's colour (Dierssen 2019: whitecap spectra shaped by the water's
//   absorption; the light bounces through many films and lenses of water between bubbles, an effective path of a
//   few centimetres in loaded water). Beige in a silty flood, grey in ash.
inline constexpr const char *WATER_SEDIMENT_GLSL = R"(
vec3 sediment_absorption_of(vec2 c) {
	return vec3(12.0, 26.0, 75.0) * c.x + vec3(300.0) * c.y;
}
vec3 sediment_scattering_of(vec2 c) {
	return vec3(200.0) * c.x + vec3(150.0) * c.y;
}
float water_medium_g(vec3 b_w, vec3 b_s, float g_w) {
	vec3 share = b_s / max(b_w + b_s, vec3(1e-6));
	return mix(g_w, min(g_w, 0.5), max(share.r, max(share.g, share.b)));
}
vec3 water_medium_scattering(vec3 b_w, vec3 b_s, float g_w) {
	if (max(b_s.r, max(b_s.g, b_s.b)) <= 0.0) {
		return b_w;
	}
	float g = water_medium_g(b_w, b_s, g_w);
	return (max(b_w, vec3(0.0)) * (1.0 - g_w) + max(b_s, vec3(0.0)) * (1.0 - 0.95)) / max(1.0 - g, 0.05);
}
vec3 sediment_foam_tint(vec3 a_s) {
	return exp(-max(a_s, vec3(0.0)) * 0.05);
}
)";

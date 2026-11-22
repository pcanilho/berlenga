/**************************************************************************/
/*  fairy_rings.h                                                         */
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

// Meadow fairy rings (docs/ideas/vegetation.md, "Meadow fairy rings"). A grassland fungus (Marasmius oreades and
// others) grows out from a point as a ring of mycelium, 10 to 50 cm a year, so rings run from a couple of metres to
// tens of metres across. Seen from outside in (Shantz and Piemeisel 1917, J. Agric. Res. 11): a band of dark, lush
// grass where the advancing mycelium frees nitrogen; behind it a band where the grass is browned or dead (the
// mycelium leaves the soil water-repellent); then a weaker dark band, and ordinary sward in the middle. The fruit
// bodies come up along the dead band's outer edge after rain. Old rings are broken into arcs where the front met poor
// soil or another ring.
//
// One world layout, the same in C++ and in the grass shader (GLSL below): at most one ring per CELL-metre cell,
// present with DENSITY, its centre kept far enough inside the cell that the ring never crosses into the next one, so a
// point needs only its own cell.

#include "core/math/math_funcs.h"
#include "core/math/vector2.h"
#include "core/math/vector3.h"

namespace FairyRings {
static constexpr float CELL = 40.0f;
static constexpr float DENSITY = 0.3f; // Share of cells with a ring.
static constexpr float R_MIN = 2.0f;
static constexpr float R_MAX = 14.0f;
// Bands by the signed distance from the ring's line (m, + outside): the dark front out to DARK_OUT, the dead band in to
// DEAD_IN, the weaker inner dark band to INNER_IN; the fruit bodies along the line.
static constexpr float DARK_OUT = 0.55f;
static constexpr float DEAD_IN = -0.5f;
static constexpr float INNER_IN = -1.1f;

inline uint32_t mix32(uint32_t p_x) {
	p_x ^= p_x >> 16;
	p_x *= 0x7feb352du;
	p_x ^= p_x >> 15;
	p_x *= 0x846ca68bu;
	p_x ^= p_x >> 16;
	return p_x;
}
inline float unit(uint32_t p_x) {
	return float(p_x >> 8) / 16777216.0f;
}

struct Hit {
	float distance = 1000.0f; // Signed from the ring's line, + outside; 1000: no ring.
	float arc = 0.0f; // 1 on the ring, 0 in a gap of a broken one.
	float radius = 0.0f;
};

inline Hit at(const Vector2 &p_xz) {
	Hit r;
	const int cx = int(Math::floor(p_xz.x / CELL));
	const int cz = int(Math::floor(p_xz.y / CELL));
	const uint32_t h0 = mix32((uint32_t(cx) * 0x8da6b343u) ^ mix32((uint32_t(cz) * 0xd8163841u) ^ 0x5eedfa1u));
	if (unit(h0) >= DENSITY) {
		return r;
	}
	const float u1 = unit(mix32(h0 ^ 0x9e3779b9u));
	const float radius = R_MIN + (R_MAX - R_MIN) * u1 * u1; // More young, small rings than old, wide ones.
	const float room = CELL - 2.0f * radius - 2.0f;
	const Vector2 centre(cx * CELL + radius + 1.0f + room * unit(mix32(h0 ^ 0x85ebca6bu)), cz * CELL + radius + 1.0f + room * unit(mix32(h0 ^ 0xc2b2ae35u)));
	const Vector2 to = p_xz - centre;
	const float len = to.length();
	r.radius = radius;
	r.distance = len - radius;
	const float broken = unit(mix32(h0 ^ 0x27d4eb2fu));
	r.arc = 1.0f;
	if (broken > 0.4f) {
		const float th = Math::atan2(to.y, to.x);
		const float pa = unit(mix32(h0 ^ 0x165667b1u)) * Math::TAU;
		const float pb = unit(mix32(h0 ^ 0xd3a2646cu)) * Math::TAU;
		const float s = 0.6f * Math::sin(3.0f * th + pa) + 0.4f * Math::sin(5.0f * th + pb);
		r.arc = Math::smoothstep(-0.25f, 0.05f, s + 0.6f - 0.6f * broken);
	}
	return r;
}

// The three bands, 0 to 1 each: x the dark front, y the dead band, z the inner dark band.
inline Vector3 bands(const Hit &p_hit) {
	const float d = p_hit.distance;
	if (d > DARK_OUT + 0.01f || d < INNER_IN - 0.01f) {
		return Vector3();
	}
	const float dark = Math::smoothstep(-0.05f, 0.08f, d) * (1.0f - Math::smoothstep(DARK_OUT - 0.12f, DARK_OUT, d));
	const float dead = Math::smoothstep(DEAD_IN, DEAD_IN + 0.1f, d) * (1.0f - Math::smoothstep(-0.08f, 0.02f, d));
	const float inner = 0.5f * Math::smoothstep(INNER_IN, INNER_IN + 0.15f, d) * (1.0f - Math::smoothstep(DEAD_IN - 0.05f, DEAD_IN + 0.05f, d));
	return Vector3(dark, dead, inner) * p_hit.arc;
}

// The same in GLSL: fairy_ring_bands(xz) returns vec3(dark, dead, inner).
static constexpr const char *GLSL = R"(
uint fairy_mix32(uint x) {
	x ^= x >> 16u;
	x *= 0x7feb352du;
	x ^= x >> 15u;
	x *= 0x846ca68bu;
	x ^= x >> 16u;
	return x;
}
float fairy_unit(uint x) {
	return float(x >> 8u) / 16777216.0;
}
vec3 fairy_ring_bands(vec2 p) {
	const float CELL = 40.0;
	ivec2 c = ivec2(floor(p / CELL));
	uint h0 = fairy_mix32((uint(c.x) * 0x8da6b343u) ^ fairy_mix32((uint(c.y) * 0xd8163841u) ^ 0x5eedfa1u));
	if (fairy_unit(h0) >= 0.3) {
		return vec3(0.0);
	}
	float u1 = fairy_unit(fairy_mix32(h0 ^ 0x9e3779b9u));
	float radius = 2.0 + 12.0 * u1 * u1;
	float room = CELL - 2.0 * radius - 2.0;
	vec2 centre = vec2(float(c.x) * CELL + radius + 1.0 + room * fairy_unit(fairy_mix32(h0 ^ 0x85ebca6bu)), float(c.y) * CELL + radius + 1.0 + room * fairy_unit(fairy_mix32(h0 ^ 0xc2b2ae35u)));
	vec2 to = p - centre;
	float d = length(to) - radius;
	if (d > 0.56 || d < -1.11) {
		return vec3(0.0);
	}
	float arc = 1.0;
	float broken = fairy_unit(fairy_mix32(h0 ^ 0x27d4eb2fu));
	if (broken > 0.4) {
		float th = atan(to.y, to.x);
		float pa = fairy_unit(fairy_mix32(h0 ^ 0x165667b1u)) * 6.2831853;
		float pb = fairy_unit(fairy_mix32(h0 ^ 0xd3a2646cu)) * 6.2831853;
		float s = 0.6 * sin(3.0 * th + pa) + 0.4 * sin(5.0 * th + pb);
		arc = smoothstep(-0.25, 0.05, s + 0.6 - 0.6 * broken);
	}
	float dark = smoothstep(-0.05, 0.08, d) * (1.0 - smoothstep(0.43, 0.55, d));
	float dead = smoothstep(-0.5, -0.4, d) * (1.0 - smoothstep(-0.08, 0.02, d));
	float inner = 0.5 * smoothstep(-1.1, -0.95, d) * (1.0 - smoothstep(-0.55, -0.45, d));
	return vec3(dark, dead, inner) * arc;
}
)";
} // namespace FairyRings

/**************************************************************************/
/*  deer_beds.h                                                           */
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

// Deer beds (docs/ideas/vegetation.md, "Deer beds"): where deer lie up in tall grass they leave oval patches of
// flattened grass, about 1 m long for roe and 1.5 m for red deer, in hay meadows and at field edges, often several
// together (Discover Wildlife, "How to identify wildlife paths and beds"). A deer turns before lying, so the grass lies
// swirled round the bed; it stands up again over days once the bed is left (an estimate: about ten days), and deer come
// back to the same beds.
//
// One world layout, the same in C++ and in the grass shader (GLSL below): at most one bedding site per CELL-metre cell,
// present with DENSITY, of 1 to 3 beds within SPREAD metres of its centre, kept inside the cell. Each bed is lain in
// again every 9 to 17 days (its own period and phase), so on any day the site shows beds from fresh to nearly gone.

#include "core/math/math_funcs.h"
#include "core/math/vector2.h"

namespace DeerBeds {
static constexpr float CELL = 32.0f;
static constexpr float DENSITY = 0.4f; // Share of cells with a site: about 3.9 sites a hectare (an estimate).
static constexpr float SPREAD = 2.6f;
static constexpr float RECOVER_DAYS = 10.0f; // Fully up again (an estimate).

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
	float press = 0.0f; // 0 to 1: how flat the grass lies here (inside a bed, by its age).
	Vector2 lie; // The direction the grass lies (unit), round the bed.
	float length = 0.0f; // The bed's length (m), 0: none.
	float age = -1.0f; // Days since a deer last lay in it.
};

// p_day: days (the season's day of the year; any running count works).
inline Hit at(const Vector2 &p_xz, float p_day) {
	Hit r;
	const int cx = int(Math::floor(p_xz.x / CELL));
	const int cz = int(Math::floor(p_xz.y / CELL));
	const uint32_t h0 = mix32((uint32_t(cx) * 0x9e3779b1u) ^ mix32((uint32_t(cz) * 0x85ebca77u) ^ 0xdee4bedu));
	if (unit(h0) >= DENSITY) {
		return r;
	}
	const float m = SPREAD + 1.2f;
	const Vector2 site(cx * CELL + m + (CELL - 2.0f * m) * unit(mix32(h0 ^ 0x27d4eb2fu)), cz * CELL + m + (CELL - 2.0f * m) * unit(mix32(h0 ^ 0x165667b1u)));
	if ((p_xz - site).length_squared() > (SPREAD + 1.0f) * (SPREAD + 1.0f)) {
		return r;
	}
	const bool red = unit(mix32(h0 ^ 0xd3a2646cu)) < 0.3f; // Roe mostly, red deer at some sites.
	const int n = 1 + int(unit(mix32(h0 ^ 0xfd7046c5u)) * 2.999f);
	for (int i = 0; i < n; i++) {
		const uint32_t hb = mix32(h0 ^ (0x9e3779b9u * uint32_t(i + 1)));
		const float ang = unit(mix32(hb ^ 0x1u)) * Math::TAU;
		const float dist = i == 0 ? 0.0f : SPREAD * (0.45f + 0.55f * unit(mix32(hb ^ 0x2u)));
		const Vector2 c = site + Vector2(Math::cos(ang), Math::sin(ang)) * dist;
		const float len = red ? 1.3f + 0.4f * unit(mix32(hb ^ 0x3u)) : 0.9f + 0.3f * unit(mix32(hb ^ 0x3u));
		const float yaw = unit(mix32(hb ^ 0x4u)) * Math::TAU;
		const Vector2 ax(Math::cos(yaw), Math::sin(yaw));
		const Vector2 ay(-ax.y, ax.x);
		const Vector2 d = p_xz - c;
		const float u = d.dot(ax) / (0.5f * len);
		const float v = d.dot(ay) / (0.31f * len);
		const float q = Math::sqrt(u * u + v * v);
		if (q >= 1.0f) {
			continue;
		}
		const float period = 6.0f + 8.0f * unit(mix32(hb ^ 0x5u));
		const float age = Math::fposmod(p_day + period * unit(mix32(hb ^ 0x6u)), period);
		const float press = (1.0f - Math::smoothstep(0.7f, 1.0f, q)) * (1.0f - Math::smoothstep(0.5f, RECOVER_DAYS, age));
		if (press <= r.press) {
			continue;
		}
		// Round the bed, the way the deer turned (each bed its own way), a little outward: pressed out from the middle.
		const float turn = (hb & 0x80u) ? 1.0f : -1.0f;
		const Vector2 radial = d.length() > 1e-4f ? d.normalized() : ax;
		const Vector2 tang = Vector2(-radial.y, radial.x) * turn;
		r.press = press;
		r.lie = (tang + radial * 0.45f).normalized();
		r.length = len;
		r.age = age;
	}
	return r;
}

// The same in GLSL: deer_bed(xz, day) returns vec3(press, lie direction xz).
static constexpr const char *GLSL = R"(
uint deer_mix32(uint x) {
	x ^= x >> 16u;
	x *= 0x7feb352du;
	x ^= x >> 15u;
	x *= 0x846ca68bu;
	x ^= x >> 16u;
	return x;
}
float deer_unit(uint x) {
	return float(x >> 8u) / 16777216.0;
}
vec3 deer_bed(vec2 p, float day) {
	const float CELL = 32.0;
	const float SPREAD = 2.6;
	ivec2 c = ivec2(floor(p / CELL));
	uint h0 = deer_mix32((uint(c.x) * 0x9e3779b1u) ^ deer_mix32((uint(c.y) * 0x85ebca77u) ^ 0xdee4bedu));
	if (deer_unit(h0) >= 0.4) {
		return vec3(0.0);
	}
	float m = SPREAD + 1.2;
	vec2 site = vec2(float(c.x) * CELL + m + (CELL - 2.0 * m) * deer_unit(deer_mix32(h0 ^ 0x27d4eb2fu)), float(c.y) * CELL + m + (CELL - 2.0 * m) * deer_unit(deer_mix32(h0 ^ 0x165667b1u)));
	vec2 ds = p - site;
	if (dot(ds, ds) > (SPREAD + 1.0) * (SPREAD + 1.0)) {
		return vec3(0.0);
	}
	bool red = deer_unit(deer_mix32(h0 ^ 0xd3a2646cu)) < 0.3;
	int n = 1 + int(deer_unit(deer_mix32(h0 ^ 0xfd7046c5u)) * 2.999);
	vec3 best = vec3(0.0);
	for (int i = 0; i < n; i++) {
		uint hb = deer_mix32(h0 ^ (0x9e3779b9u * uint(i + 1)));
		float ang = deer_unit(deer_mix32(hb ^ 0x1u)) * 6.2831853;
		float dist = i == 0 ? 0.0 : SPREAD * (0.45 + 0.55 * deer_unit(deer_mix32(hb ^ 0x2u)));
		vec2 cc = site + vec2(cos(ang), sin(ang)) * dist;
		float len = red ? 1.3 + 0.4 * deer_unit(deer_mix32(hb ^ 0x3u)) : 0.9 + 0.3 * deer_unit(deer_mix32(hb ^ 0x3u));
		float yaw = deer_unit(deer_mix32(hb ^ 0x4u)) * 6.2831853;
		vec2 ax = vec2(cos(yaw), sin(yaw));
		vec2 ay = vec2(-ax.y, ax.x);
		vec2 d = p - cc;
		float u = dot(d, ax) / (0.5 * len);
		float v = dot(d, ay) / (0.31 * len);
		float q = sqrt(u * u + v * v);
		if (q >= 1.0) {
			continue;
		}
		float period = 6.0 + 8.0 * deer_unit(deer_mix32(hb ^ 0x5u));
		float age = mod(day + period * deer_unit(deer_mix32(hb ^ 0x6u)), period);
		float press = (1.0 - smoothstep(0.7, 1.0, q)) * (1.0 - smoothstep(0.5, 10.0, age));
		if (press <= best.x) {
			continue;
		}
		float turn = (hb & 0x80u) != 0u ? 1.0 : -1.0;
		vec2 radial = length(d) > 1e-4 ? normalize(d) : ax;
		vec2 tang = vec2(-radial.y, radial.x) * turn;
		best = vec3(press, normalize(tang + radial * 0.45));
	}
	return best;
}
)";
} // namespace DeerBeds

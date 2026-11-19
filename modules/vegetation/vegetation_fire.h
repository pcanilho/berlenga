/**************************************************************************/
/*  vegetation_fire.h                                                     */
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
#include "core/typedefs.h"

// Fire on plants (docs/ideas/physx.md, "Fire burns grass, plants and trees"): PhysXFireSpread3D
// publishes fire_state_map over fire_char_rect (r: share of the fuel burnt, g: glow of the burning
// band and embers, b: sqrt(peak flame length / 32 m), a: sqrt(peak crown scorch height / 48 m), square
// roots so a grass fire's half metre and a torching tree's twenty both keep their detail in 8 bits). Plant shaders
// read it with no state of their own; the vegetation module declares the globals (rect width 0:
// no fire), so they compile with or without PhysX.
namespace VegetationFire {

static constexpr const char *SHADER_CODE = R"(
global uniform sampler2D fire_state_map;
global uniform vec4 fire_char_rect;

// x: burnt 0..1, y: glow 0..1, z: flame length (m), w: crown scorch height (m).
vec4 fire_state_at(vec2 xz) {
	if (fire_char_rect.z <= 0.0) {
		return vec4(0.0);
	}
	vec2 uv = (xz - fire_char_rect.xy) / fire_char_rect.zw;
	if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
		return vec4(0.0);
	}
	vec4 s = textureLod(fire_state_map, uv, 0.0);
	return vec4(s.r, s.g, s.b * s.b * 32.0, s.a * s.a * 48.0);
}

// A blackbody of about 900-1,100 K in linear sRGB (as the ground's glow), g 0..1.
vec3 fire_glow_color(float g) {
	return vec3(1.0, 0.32 * g, 0.06 * g * g) * g;
}

// Leaves and herbs at height h above the ground under a fire: charred black below the flame,
// killed and browned up to the scorch height (Van Wagner 1973). n: a 0..1 hash steady per leaf,
// so the edges are ragged. Returns the share charred (for glow and roughness).
float fire_foliage_apply(vec4 fs, float h, float n, inout vec3 fire_albedo, inout float fire_rough) {
	if (fs.x <= 0.0) {
		return 0.0;
	}
	float burn = fs.x * (1.0 - smoothstep(0.7, 1.3, h / max(fs.z * (0.8 + 0.4 * n), 0.02)));
	float scorch = fs.x * (1.0 - smoothstep(0.85, 1.15, h / max(fs.w * (0.85 + 0.3 * n), 0.02)));
	float lum = dot(fire_albedo, vec3(0.2126, 0.7152, 0.0722));
	// Heat-killed foliage: red-brown, keeping the leaf's own pattern.
	vec3 brown = vec3(0.30, 0.15, 0.05) * clamp(lum * 5.0, 0.5, 1.4);
	fire_albedo = mix(fire_albedo, brown, smoothstep(0.0, 1.0, scorch));
	// Char: near black with a little grey ash where it burnt out.
	vec3 ch = mix(vec3(0.035, 0.03, 0.027), vec3(0.32, 0.31, 0.3), step(0.88, n) * fs.x * fs.x);
	fire_albedo = mix(fire_albedo, ch, smoothstep(0.0, 1.0, burn));
	fire_rough = mix(fire_rough, 0.95, burn);
	return burn;
}
)";

// The burnt ground's ash (PhysXFireSpread3D.grass_ash; fire_char_map over fire_char_rect, RGB from black char at
// 0.03 to white-grey ash at 0.35): 0..1, for stubble to carry the same ash flecks as the ground under it.
static constexpr const char *ASH_CODE = R"(
global uniform sampler2D fire_char_map : filter_linear;

float fire_ash_at(vec2 xz) {
	if (fire_char_rect.z <= 0.0) {
		return 0.0;
	}
	vec2 uv = (xz - fire_char_rect.xy) / fire_char_rect.zw;
	if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
		return 0.0;
	}
	vec3 c = textureLod(fire_char_map, uv, 0.0).rgb;
	return clamp((c.g - 0.03) / 0.32, 0.0, 1.0);
}
)";

// The fires' indraft near the ground (WorldFire.wind_at; PhysXFireSpread3D publishes fire_wind_map over fire_wind_rect,
// RG: world x and z in m/s; rect width 0: no fire), for plants to lean into.
static constexpr const char *WIND_CODE = R"(
global uniform sampler2D fire_wind_map : filter_linear;
global uniform vec4 fire_wind_rect;

vec3 fire_wind_at(vec2 xz) {
	if (fire_wind_rect.z <= 0.0) {
		return vec3(0.0);
	}
	vec2 uv = (xz - fire_wind_rect.xy) / fire_wind_rect.zw;
	if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
		return vec3(0.0);
	}
	vec2 w = textureLod(fire_wind_map, uv, 0.0).rg;
	return vec3(w.x, 0.0, w.y);
}
)";


// Fire mortality of a tree (docs/ideas/trees.md 5ag), one rule for TreeWind3D (snags: limbs weaken, the butt
// rots) and the leaf shader (a killed tree's whole crown browns and is cast), so the two always agree. Ryan and
// Reinhardt (1988), FOFEM: P = 1 / (1 + exp(-1.941 + 6.316 (1 - e^-BT) - 0.000535 CVS^2)), BT the bark thickness
// in inches (4% of the stem's diameter at breast height), CVS the % of the crown's foliage below the lethal scorch
// height; a hash of the tree's world position (16 cm cells) against P. The crown's foliage profile is TreeGenerator's
// "tree_fire_crown" mesh meta (and the leaf material's fire_crown_q, fire_crown_top, fire_dbh): heights over the
// tree's foot (mesh space, metres) below which 0, 25, 50, 75 and 100% of the leaves sit, then the stem's diameter at
// 1.3 m. The scorch height is read as the state map stores it (8 bits of sqrt(h / 48 m)), so CPU and GPU see the same
// number.
inline float scorch_as_stored(float p_height) {
	const float s = float(uint8_t(CLAMP(double(Math::sqrt(MIN(MAX(p_height, 0.0f) / 48.0f, 1.0f))) * 255.0, 0.0, 255.0))) / 255.0f; // As Image::set_pixel stores it.
	return s * s * 48.0f;
}

// Share (0-1) of the foliage below p_h, from the five quantile heights (already scaled to the tree).
inline float crown_below(const float *p_q, float p_h) {
	if (p_h <= p_q[0]) {
		return 0.0f;
	}
	for (int i = 0; i < 4; i++) {
		if (p_h < p_q[i + 1]) {
			return 0.25f * (i + (p_h - p_q[i]) / MAX(p_q[i + 1] - p_q[i], 1e-4f));
		}
	}
	return 1.0f;
}

inline float kill_chance(float p_cvs_percent, float p_dbh_m) {
	const float bt = 0.04f * p_dbh_m * 39.37f;
	return 1.0f / (1.0f + Math::exp(-1.941f + 6.316f * (1.0f - Math::exp(-bt)) - 0.000535f * p_cvs_percent * p_cvs_percent));
}

inline float tree_hash01(float p_x, float p_z) {
	auto mix = [](uint32_t x) {
		x ^= x >> 16;
		x *= 0x7feb352du;
		x ^= x >> 15;
		x *= 0x846ca68bu;
		x ^= x >> 16;
		return x;
	};
	const uint32_t seed = mix(uint32_t(int(p_x * 16.0f)) * 73856093u ^ uint32_t(int(p_z * 16.0f)) * 19349663u);
	return float(mix(seed * 0x9e3779b9u + 0x5a9u * 0x85ebca6bu) >> 8) / 16777216.0f;
}

// Whether the fire killed the tree whose foot is at world (p_x, p_z), scaled p_scale, under a crown scorch height
// p_scorch (metres, as WorldFire.crown_scorch_at gives it). p_crown: the six "tree_fire_crown" values.
inline bool tree_killed(float p_x, float p_z, float p_scale, float p_scorch, const float *p_crown) {
	const float h = scorch_as_stored(p_scorch);
	if (h <= 0.0f) {
		return false;
	}
	float q[5];
	for (int i = 0; i < 5; i++) {
		q[i] = p_crown[i] * p_scale;
	}
	return tree_hash01(p_x, p_z) < kill_chance(100.0f * crown_below(q, h), p_crown[5] * p_scale);
}

// The same in the leaf shader. Needs SHADER_CODE (fire_state_map, fire_char_rect) and the material's fire_crown_q (vec4),
// fire_crown_top and fire_dbh (0: no profile, never killed).
static constexpr const char *MORTALITY_CODE = R"(
uniform vec4 fire_crown_q = vec4(0.0);
uniform float fire_crown_top = 0.0;
uniform float fire_dbh = 0.0;

uint fire_mix(uint x) {
	x ^= x >> 16u;
	x *= 0x7feb352du;
	x ^= x >> 15u;
	x *= 0x846ca68bu;
	x ^= x >> 16u;
	return x;
}

bool fire_tree_killed(vec3 foot, float scale) {
	if (fire_dbh <= 0.0 || fire_char_rect.z <= 0.0) {
		return false;
	}
	// The foot's own cell, unfiltered: the CPU reads the cell's value.
	vec2 uv = (foot.xz - fire_char_rect.xy) / fire_char_rect.zw;
	if (any(lessThan(uv, vec2(0.0))) || any(greaterThanEqual(uv, vec2(1.0)))) {
		return false;
	}
	ivec2 ts = textureSize(fire_state_map, 0);
	ivec2 cell = clamp(ivec2(floor(uv * vec2(ts))), ivec2(0), ts - ivec2(1));
	float sa = texelFetch(fire_state_map, cell, 0).a;
	float h = sa * sa * 48.0;
	if (h <= 0.0) {
		return false;
	}
	float q[5] = float[5](fire_crown_q.x * scale, fire_crown_q.y * scale, fire_crown_q.z * scale, fire_crown_q.w * scale, fire_crown_top * scale);
	float below = 1.0;
	if (h <= q[0]) {
		below = 0.0;
	} else {
		for (int i = 0; i < 4; i++) {
			if (h < q[i + 1]) {
				below = 0.25 * (float(i) + (h - q[i]) / max(q[i + 1] - q[i], 1e-4));
				break;
			}
		}
	}
	float cvs = 100.0 * below;
	float bt = 0.04 * fire_dbh * scale * 39.37;
	float p = 1.0 / (1.0 + exp(-1.941 + 6.316 * (1.0 - exp(-bt)) - 0.000535 * cvs * cvs));
	uint seed = fire_mix(uint(int(foot.x * 16.0)) * 73856093u ^ uint(int(foot.z * 16.0)) * 19349663u);
	float r = float(fire_mix(seed * 0x9e3779b9u + 0x5a9u * 0x85ebca6bu) >> 8u) / 16777216.0;
	return r < p;
}
)";

} // namespace VegetationFire

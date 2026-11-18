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

} // namespace VegetationFire

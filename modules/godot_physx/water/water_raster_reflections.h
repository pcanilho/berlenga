/**************************************************************************/
/*  water_raster_reflections.h                                            */
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

#include "core/object/object_id.h"
#include "core/object/ref_counted.h"
#include "core/templates/rid.h"
#include "scene/resources/image_texture.h"

class Node3D;
class ShaderMaterial;
class DirectionalLight3D;

// Raster water seen from above (PhysXWaterSurface3D, PhysXRiver3D): the sky mirrored by the exact Fresnel
// term, from a small panorama of the environment's sky, and the sun's glitter: a GGX lobe (Bruneton,
// Neyret and Holzschuch 2010 light the sea with a microfacet sun) whose roughness widens with how fast the
// normal turns across the pixel (geometric specular antialiasing, Tokuyoshi and Kaplanyan 2019), so far
// water gathers its glints into a glitter path instead of flickering. The materials stay unshaded and
// blended: water_raster_reflect() adds what the surface reflects and lets through only the rest.
//
// WATER_RASTER_REFLECTIONS_GLSL goes in a shader's global scope (uniforms + the function). It is a plain
// constant, not a macro: a raw string spanning lines inside #define is not portable (GCC 11 rejects it).
// Call from
// fragment():
//   vec4 r = water_raster_reflect(ALBEDO, ALPHA, world_normal, world_position, CAMERA_POSITION_WORLD, surface, roughness);
//   ALBEDO = r.rgb; ALPHA = r.a;
// where surface (0..1) is how much of the pixel is open water (0 on foam, past the waterline).
inline constexpr const char *WATER_RASTER_REFLECTIONS_GLSL = R"(
uniform bool raster_reflections = true;
uniform sampler2D raster_sky_panorama : filter_linear, repeat_enable, hint_default_black;
uniform float raster_sky_energy = 0.0; // 0 = no sky baked (then no sky term)
uniform vec3 raster_sun_travel = vec3(0.0, -1.0, 0.0); // the direction sunlight travels
uniform vec3 raster_sun_irradiance = vec3(0.0); // pi x energy x colour, as a raster light
uniform vec3 raster_sky_irradiance = vec3(0.0); // from the baked sky on a level surface (before raster_sky_energy)

// The light on the water's body: what a level white diffuse surface would send back under the sun and
// the sky (irradiance / pi), so the open water's colour, which is drawn as emission, darkens with the day
// as the lit foam beside it does. Without a baked sky and sun (raster_reflections off), 1: the colour as set.
vec3 water_raster_body_light() {
	if (!raster_reflections || raster_sky_energy <= 0.0) {
		return vec3(1.0);
	}
	vec3 e = raster_sun_irradiance * max(-raster_sun_travel.y, 0.0) + raster_sky_irradiance * raster_sky_energy;
	return e / PI;
}

vec4 water_raster_reflect(vec3 p_albedo, float p_alpha, vec3 p_normal, vec3 p_world, vec3 p_camera, float p_surface, float p_roughness) {
	if (!raster_reflections) {
		return vec4(p_albedo, p_alpha);
	}
	vec3 v = normalize(p_camera - p_world);
	vec3 n_w = dot(p_normal, v) < 0.0 ? p_normal - 2.0 * dot(p_normal, v) * v : p_normal;
	n_w = normalize(n_w);
	float nv = clamp(dot(n_w, v), 1e-3, 1.0);
	// Exact unpolarized Fresnel, air into water.
	float st = sqrt(max(1.0 - nv * nv, 0.0)) / 1.33;
	float ct = sqrt(max(1.0 - st * st, 0.0));
	float rs = (nv - 1.33 * ct) / (nv + 1.33 * ct);
	float rp = (1.33 * nv - ct) / (1.33 * nv + ct);
	float f_eff = 0.5 * (rs * rs + rp * rp) * p_surface;
	vec3 add = vec3(0.0);
	vec3 r = reflect(-v, n_w);
	r.y = max(r.y, 0.01); // a ray turned down by a steep wave sees the horizon's sky
	r = normalize(r);
	if (raster_sky_energy > 0.0) {
		vec2 pc = vec2(atan(r.x, -r.z) + PI, acos(clamp(r.y, -1.0, 1.0))) / vec2(2.0 * PI, PI);
		add += f_eff * textureLod(raster_sky_panorama, pc, 0.0).rgb * raster_sky_energy;
	}
	vec3 l = -raster_sun_travel;
	if (dot(n_w, l) > 0.0 && dot(raster_sun_irradiance, vec3(1.0)) > 0.0) {
		vec3 hv = normalize(l + v);
		float nh = clamp(dot(n_w, hv), 0.0, 1.0);
		float a2 = p_roughness * p_roughness;
		a2 *= a2;
		vec3 dndx = dFdx(n_w);
		vec3 dndy = dFdy(n_w);
		a2 = clamp(a2 + min(0.5 * (dot(dndx, dndx) + dot(dndy, dndy)), 0.18), 1e-6, 1.0);
		float den = nh * nh * (a2 - 1.0) + 1.0;
		float d_ggx = a2 / (PI * den * den);
		float fh = 0.02 + 0.98 * pow(1.0 - clamp(dot(hv, v), 0.0, 1.0), 5.0);
		add += raster_sun_irradiance * (fh * d_ggx / (4.0 * nv)) * p_surface;
	}
	// What showed through before is now only what the surface lets through: out = old (1 - F) + add,
	// written for blend_mix.
	float a_new = 1.0 - (1.0 - p_alpha) * (1.0 - f_eff);
	return vec4((p_albedo * p_alpha * (1.0 - f_eff) + add) / max(a_new, 1e-4), a_new);
}
)";

// The CPU side: finds the sun and bakes the sky panorama, and sets the uniforms above on a material.
// Call update() once a frame.
class WaterRasterReflections {
	ObjectID sun;
	int sun_search = 0;
	RID sky_baked;
	int sky_rebake = 0;
	Ref<ImageTexture> sky_texture;
	float sky_energy = 0.0f;
	Vector3 sky_irradiance; // the baked sky's irradiance on a level surface

public:
	// The first visible directional light (not sky-only), looked up again now and then.
	DirectionalLight3D *find_sun(Node3D *p_node);
	void update(Node3D *p_node, const Ref<ShaderMaterial> &p_material, bool p_enabled);
};

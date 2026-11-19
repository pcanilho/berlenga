/**************************************************************************/
/*  water_raster_reflections.h                                            */
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

#include "core/object/object_id.h"
#include "core/object/ref_counted.h"
#include "core/templates/rid.h"
#include "core/object/object.h"
#include "core/templates/hash_map.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/shader.h"

class Node3D;
class ShaderMaterial;
class DirectionalLight3D;

// Raster water seen from above (PhysXWaterSurface3D, PhysXRiver3D): the sky mirrored by the exact Fresnel
// term, from a small panorama of the environment's sky, and the sun's glitter: a GGX lobe (Bruneton,
// Neyret and Holzschuch 2010 light the sea with a microfacet sun) whose roughness widens with how fast the
// normal turns across the pixel (geometric specular antialiasing, Tokuyoshi and Kaplanyan 2019), so far
// water gathers its glints into a glitter path instead of flickering. The materials are blended:
// water_raster_reflect() adds what the surface reflects and lets through only the rest.
//
// Shadows: the open water is drawn as emission (its own bounded look, not the PBR response that blew out
// when a lit surface was blended over the lit floor), so whatever the sun adds goes through light()
// instead, where the light's attenuation carries its shadow maps and cloud projector. The sun's glitter and
// the sun's share of the body colour come back from water_raster_reflect() as two out values; the fragment
// stores them in the wr_sun_* varyings and light() calls water_raster_sun_light(). Only the sky's share of the
// body light and the mirrored sky stay in the emission (a shadowed patch of water still mirrors the sky).
//
// WATER_RASTER_REFLECTIONS_GLSL goes in a shader's global scope (uniforms + the function). It is a plain
// constant, not a macro: a raw string spanning lines inside #define is not portable (GCC 11 rejects it).
// Call from
// fragment():
//   vec4 r = water_raster_reflect(rgb, a, world_normal, world_position, CAMERA_POSITION_WORLD, surface, roughness,
//           sun_rgb, wr_sun_body, wr_sun_glint, wr_sun_a2);
//   EMISSION = r.rgb; ALPHA = r.a;
// light():
//   SPECULAR_LIGHT += water_raster_sun_light(LIGHT, VIEW, LIGHT_COLOR * ATTENUATION, LIGHT_IS_DIRECTIONAL,
//           INV_VIEW_MATRIX, wr_sun_body, wr_sun_glint, wr_sun_a2);
// where surface (0..1) is how much of the pixel is open water (0 on foam, past the waterline).
inline constexpr const char *WATER_RASTER_REFLECTIONS_GLSL = R"(
uniform bool raster_reflections = true;
uniform sampler2D raster_sky_panorama : filter_linear, repeat_enable, hint_default_black;
uniform float raster_sky_energy = 0.0; // 0 = no sky baked (then no sky term)
uniform vec3 raster_sun_travel = vec3(0.0, -1.0, 0.0); // the direction sunlight travels
uniform vec3 raster_sun_irradiance = vec3(0.0); // pi x energy x colour, as a raster light
uniform vec3 raster_sky_irradiance = vec3(0.0); // from the baked sky on a level surface (before raster_sky_energy)

// The sun's part, handed from fragment() to light(): the body colour's weight per unit of irradiance on a
// level surface (premultiplied for the blend), the glitter's normal (world) with its weight (the open-water
// share over the blend's alpha) in w, and its GGX alpha squared.
varying vec3 wr_sun_body;
varying vec4 wr_sun_glint;
varying float wr_sun_a2;

// Whether the body takes the sun through light() (a sky and sun are baked); else its colour is drawn as set.
bool water_raster_lit() {
	return raster_reflections && raster_sky_energy > 0.0;
}

// The sky's light on the water's body: what a level white diffuse surface sends back under the sky
// (irradiance / pi), so the open water's colour, drawn as emission, darkens with the day as the lit foam
// beside it does; the sun's share comes through light() (water_raster_sun_light), shadowed. Without a
// baked sky and sun (raster_reflections off), 1: the colour as set.
vec3 water_raster_body_light() {
	if (!water_raster_lit()) {
		return vec3(1.0);
	}
	return raster_sky_irradiance * raster_sky_energy / PI;
}

// What one light adds to the open water, from light(): the body's share (as a level diffuse surface, the sun
// only: p_body is the colour the sun lights) and the glitter's GGX lobe, for the sun and for omni and spot lights
// (lamps, torches) alike, all under the light's attenuation, so the water takes shadows like the ground. A
// negative p_glint.w (raster_reflections off) is the glitter's weight for lamps only.
vec3 water_raster_sun_light(vec3 p_light, vec3 p_view, vec3 p_irradiance, bool p_directional, mat4 p_inv_view, vec3 p_body, vec4 p_glint, float p_a2) {
	vec3 l = normalize(mat3(p_inv_view) * p_light);
	vec3 add = p_directional ? p_body * p_irradiance * max(l.y, 0.0) : vec3(0.0);
	vec3 n_w = p_glint.xyz;
	float a2 = p_a2;
	float weight = p_glint.w < 0.0 ? (p_directional ? 0.0 : -p_glint.w) : p_glint.w;
	float nl = dot(n_w, l);
	if (weight > 0.0 && nl > 0.0) {
		vec3 v = normalize(mat3(p_inv_view) * p_view);
		float nv = clamp(dot(n_w, v), 1e-3, 1.0);
		vec3 hv = normalize(l + v);
		float nh = clamp(dot(n_w, hv), 0.0, 1.0);
		float den = nh * nh * (a2 - 1.0) + 1.0;
		float d_ggx = a2 / (PI * den * den);
		float fh = 0.02 + 0.98 * pow(1.0 - clamp(dot(hv, v), 0.0, 1.0), 5.0);
		// Height-correlated Smith masking and shadowing (Heitz 2014) times n.l: the glitter fades out as a facet
		// turns from the sun or the eye. Without it (G = 1, D F / 4 n.v) it peaked right where the normal turned
		// away from a low sun and then cut to nothing, as dark hard-edged holes in the sun's path.
		float vis = 0.5 / max(nl * sqrt(nv * nv * (1.0 - a2) + a2) + nv * sqrt(nl * nl * (1.0 - a2) + a2), 1e-5);
		add += p_irradiance * (fh * d_ggx * vis * nl) * weight;
	}
	return add;
}

// p_sun_albedo: the colour the sun lights, scaled as p_albedo is (p_albedo itself carries only the sky's
// light). r_sun_body and r_sun_glint go to the wr_sun_* varyings for light().
vec4 water_raster_reflect(vec3 p_albedo, float p_alpha, vec3 p_normal, vec3 p_world, vec3 p_camera, float p_surface, float p_roughness, vec3 p_sun_albedo, out vec3 r_sun_body, out vec4 r_sun_glint, out float r_sun_a2) {
	r_sun_body = vec3(0.0);
	r_sun_glint = vec4(0.0, 1.0, 0.0, 0.0);
	r_sun_a2 = 1.0;
	vec3 v = normalize(p_camera - p_world);
	vec3 n_w = dot(p_normal, v) < 0.0 ? p_normal - 2.0 * dot(p_normal, v) * v : p_normal;
	n_w = normalize(n_w);
	if (!raster_reflections) {
		// No mirrored sky or sun, but lamps (omni and spot lights) still glint on it: water lit only by a cave
		// lamp or a street lamp read as bare floor.
		float a2_off = p_roughness * p_roughness;
		a2_off *= a2_off;
		vec3 dndx_off = dFdx(n_w);
		vec3 dndy_off = dFdy(n_w);
		r_sun_a2 = clamp(a2_off + min(0.5 * (dot(dndx_off, dndx_off) + dot(dndy_off, dndy_off)), 0.18), 1e-6, 0.999);
		r_sun_glint = vec4(n_w, -p_surface / max(p_alpha, 1e-4)); // negative: lamps only
		return vec4(p_albedo, p_alpha);
	}
	float nv = clamp(dot(n_w, v), 1e-3, 1.0);
	// The mean Fresnel and mirrored sky over the slopes the pixel holds but its normal no longer shows
	// (Bruneton, Neyret and Holzschuch 2010): three Gauss-Hermite slopes along the view's tilt, each weighted by
	// the area it turns toward the eye. A grazing pixel's facets that face the eye mirror less and higher sky.
	float sig = sqrt(0.5 * pow(p_roughness, 4.0));
	vec3 tv = v - nv * n_w;
	tv = dot(tv, tv) > 1e-8 ? normalize(tv) : vec3(0.0);
	float f_eff = 0.0;
	vec3 add = vec3(0.0);
	float wsum = 0.0;
	for (int k = 0; k < 3; k++) {
		float s_k = k == 0 ? 0.0 : (k == 1 ? 1.7320508 : -1.7320508) * sig;
		float g_k = k == 0 ? 0.6666667 : 0.1666667;
		vec3 n_k = normalize(n_w + s_k * tv);
		float nv_k = dot(n_k, v);
		if (nv_k <= 0.0) {
			continue;
		}
		float w_k = g_k * nv_k / max(dot(n_k, n_w), 1e-3);
		nv_k = clamp(nv_k, 1e-3, 1.0);
		// Exact unpolarized Fresnel, air into water.
		float st = sqrt(max(1.0 - nv_k * nv_k, 0.0)) / 1.33;
		float ct = sqrt(max(1.0 - st * st, 0.0));
		float rs = (nv_k - 1.33 * ct) / (nv_k + 1.33 * ct);
		float rp = (1.33 * nv_k - ct) / (1.33 * nv_k + ct);
		float f_k = 0.5 * (rs * rs + rp * rp);
		// A mirror ray turned down into the sea by a steep facet (far wave backs at a low sun) does not see the
		// sky: it meets the next wave, taken as the level sea, and only that wave's Fresnel share of it goes on
		// up to the sky (mirrored again); the rest enters the water and comes back as the body. Continuous at the
		// horizon, where the second Fresnel is 1. Before, these rays read the horizon sky at full Fresnel and
		// showed as bright blobs on the far sea.
		vec3 r = reflect(-v, n_k);
		if (r.y < 0.0) {
			float c2 = clamp(-r.y, 1e-3, 1.0);
			float st2 = sqrt(max(1.0 - c2 * c2, 0.0)) / 1.33;
			float ct2 = sqrt(max(1.0 - st2 * st2, 0.0));
			float rs2 = (c2 - 1.33 * ct2) / (c2 + 1.33 * ct2);
			float rp2 = (1.33 * c2 - ct2) / (1.33 * c2 + ct2);
			f_k *= 0.5 * (rs2 * rs2 + rp2 * rp2);
			r.y = -r.y;
		}
		f_eff += w_k * f_k;
		wsum += w_k;
		if (raster_sky_energy > 0.0) {
			r.y = max(r.y, 0.01);
			r = normalize(r);
			vec2 pc = vec2(atan(r.x, -r.z) + PI, acos(clamp(r.y, -1.0, 1.0))) / vec2(2.0 * PI, PI);
			add += w_k * f_k * textureLod(raster_sky_panorama, pc, 0.0).rgb * raster_sky_energy;
		}
	}
	f_eff = f_eff / max(wsum, 1e-6) * p_surface;
	add *= p_surface / max(wsum, 1e-6);
	// The glitter's roughness, widened by how fast the normal turns across the pixel (screen derivatives are
	// only defined here, in fragment(), so light() gets the result).
	float a2 = p_roughness * p_roughness;
	a2 *= a2;
	vec3 dndx = dFdx(n_w);
	vec3 dndy = dFdy(n_w);
	a2 = clamp(a2 + min(0.5 * (dot(dndx, dndx) + dot(dndy, dndy)), 0.18), 1e-6, 0.999);
	// What showed through before is now only what the surface lets through: out = old (1 - F) + add,
	// written for blend_mix.
	float a_new = 1.0 - (1.0 - p_alpha) * (1.0 - f_eff);
	float inv = 1.0 / max(a_new, 1e-4);
	if (water_raster_lit()) {
		r_sun_body = p_sun_albedo * p_alpha * (1.0 - f_eff) * inv / PI;
	}
	r_sun_glint = vec4(n_w, p_surface * inv);
	r_sun_a2 = a2;
	return vec4((p_albedo * p_alpha * (1.0 - f_eff) + add) * inv, a_new);
}

// The light inside the water from this sky and sun: the sun's direction once refracted (Snell, 1.33), its
// irradiance past the surface (Fresnel transmission), and the sky's radiance from above. Zero without them.
void water_light_in_water(out vec3 r_sun_dir, out vec3 r_sun_irradiance, out vec3 r_sky_radiance) {
	r_sun_dir = vec3(0.0, -1.0, 0.0);
	r_sun_irradiance = vec3(0.0);
	r_sky_radiance = vec3(0.0);
	if (!water_raster_lit()) {
		return;
	}
	r_sky_radiance = raster_sky_irradiance * raster_sky_energy / PI;
	vec3 travel = normalize(raster_sun_travel);
	float cos_i = -travel.y;
	if (cos_i <= 0.0) {
		return;
	}
	float sin_t = sqrt(max(1.0 - cos_i * cos_i, 0.0)) / 1.33;
	float cos_t = sqrt(max(1.0 - sin_t * sin_t, 0.0));
	vec2 hz = travel.xz;
	hz = dot(hz, hz) > 1e-12 ? normalize(hz) : vec2(0.0);
	r_sun_dir = vec3(hz.x * sin_t, -cos_t, hz.y * sin_t);
	float rs = (cos_i - 1.33 * cos_t) / (cos_i + 1.33 * cos_t);
	float rp = (1.33 * cos_i - cos_t) / (1.33 * cos_i + cos_t);
	r_sun_irradiance = raster_sun_irradiance * (1.0 - 0.5 * (rs * rs + rp * rp));
}

// Interleaved gradient noise (Jimenez 2014), a new pattern each frame: per-pixel offsets for marches.
float water_jitter(vec2 p_frag, float p_time) {
	p_frag += 5.588238 * floor(p_time * 60.0);
	return fract(52.9829189 * fract(dot(p_frag, vec2(0.06711056, 0.00583715))));
}

#if !defined(RT)
// Clear water seen from above (docs/ideas/water.md "Clear water you can see into"): the light coming up
// through the surface at p_s toward the eye, before the surface's own Fresnel reflection. The view ray v
// refracts into the water along n (Snell). Where the straight ray meets the bed at depth z0 (z_bed, the
// water's own depth there when known, >= 0; else from the screen's depth), the refracted one meets it after
// z0 / -t.y (a level bed); that screen point is read for colour and depth. Where it shows something in
// front of the water (a rock standing out of it), the offset is halved, twice, then given up for the
// straight pixel (GPU Gems 2, ch. 19). The light seen there was dimmed on its way down to that depth and is
// dimmed again along the refracted path (Beer-Lambert, per channel); the water adds its own scattered sun
// (Henyey-Greenstein, as far as sun_visibility lets the sun reach the surface) and sky along the path. The
// medium is the path tracer's: absorption p_ab and scattering p_sc per metre, anisotropy p_g.
// r_seen is the bed's share of the result, r_bed the point seen there and r_sun_share the part of the bed's light
// that came from the sun (the rest from the sky), for caustics.
vec3 water_clear_through_ex(sampler2D p_screen, sampler2D p_depth, vec3 p_s, vec3 v, vec3 n, vec2 uv, float z_bed,
		vec3 p_ab, vec3 p_sc, float p_g, vec3 p_sun_dir, vec3 p_sun_irr, vec3 p_sky, float p_jitter, mat4 proj,
		mat4 view, mat4 inv_proj, mat4 inv_view, bool p_march, out vec3 r_seen, out vec3 r_bed, out float r_sun_share) {
	vec3 t = refract(v, n, 1.0 / 1.33);
	if (dot(t, t) < 1e-6) {
		t = v;
	}
	float d0 = texture(p_depth, uv).r;
	float len0 = 1e4;
	if (d0 > 0.0) {
		vec4 b = inv_proj * vec4(uv * 2.0 - 1.0, d0, 1.0);
		len0 = distance((inv_view * vec4(b.xyz / b.w, 1.0)).xyz, p_s);
	}
	// The known bed, but never deeper than the bed the screen shows straight through: the bed map is the
	// simulated grid's (metres a cell, or a constant depth where nothing was measured), and a refracted probe
	// sent to its deeper bed missed and fell back to that whole depth of water, as dark hard-edged blotches
	// over shallow sand.
	float z_screen = max(-v.y, 0.0) * min(len0, 1e3);
	float z0 = z_bed >= 0.0 ? min(z_bed, len0 < 1e4 ? z_screen : 1e3) : z_screen;
	float len_r = min(z0 / max(-t.y, 0.05), 60.0);
	// Given up: the straight-through colour, but the refracted ray's path to the same depth (the straight
	// ray's own, longer at a glancing view, would change the tint in a step where the offset is given up).
	vec2 uv_r = uv;
	float len = len0 < 1e4 ? z0 / max(-t.y, 0.05) : len0;
	vec4 cs = proj * view * vec4(p_s, 1.0);
	float d_s = cs.z / cs.w;
	vec4 c = proj * view * vec4(p_s + t * len_r, 1.0);
	if (c.w > 0.0) {
		// An offset that would leave the screen reaches at most half way from the pixel to the edge, rather than
		// being clamped per axis: a clamp read the screen's last row or column for every pixel whose offset passed
		// it, stretching it into streaks along the frame's edge (a glancing view down into the shallows). Half the
		// room keeps neighbouring pixels on neighbouring texels (the offset shrinks smoothly to the edge).
		vec2 off = c.xy / c.w * 0.5 + 0.5 - uv;
		vec2 m = 0.5 / vec2(textureSize(p_screen, 0));
		vec2 room = max(mix(vec2(1.0) - m - uv, uv - m, vec2(lessThan(off, vec2(0.0)))), vec2(0.0));
		vec2 k2 = mix(vec2(1.0), clamp(0.5 * room / abs(off), 0.0, 1.0), vec2(greaterThan(abs(off), vec2(1e-7))));
		float k = min(k2.x, k2.y);
		vec2 q_full = uv + off * k;
		if (p_march) {
			// Marched along the refracted ray over the screen's depth (screen-space refraction, as screen-space
			// reflections march the mirror ray): the first sample where the scene stands in front of the ray is what
			// the ray meets (a stone's side under the water); one hidden by something out of the water or nearer
			// the eye gives the straight-through pixel. The halved offsets
			// below read the same stone at three places where shallow water runs between stones standing out of
			// it, stacking copies of every stone in rows above it.
			const int MS = 10;
			vec3 vs = (view * vec4(p_s, 1.0)).xyz;
			vec3 vt = (view * vec4(t, 0.0)).xyz * (len_r * k);
			vec2 q_last = uv;
			float len_last = len0 < 1e4 ? z0 / max(-t.y, 0.05) : len0;
			bool hidden = false;
			float f_last = 0.0;
			for (int i = 1; i <= MS; i++) {
				// The crossing is found by bisection below: ten fixed steps took the first one past the surface, so
				// every stone under the water read back in ten bands, ribbed and stair-stepped (stream riffle, rocks
				// bench), as screen-space reflections do without refinement. Not jittered: a per-pixel start dithers
				// the hidden test into vertical teeth without a temporal filter to resolve it.
				float f = float(i) / float(MS);
				vec3 pv = vs + vt * f;
				vec4 cp = proj * vec4(pv, 1.0);
				if (cp.w <= 0.0) {
					break;
				}
				vec2 q = cp.xy / cp.w * 0.5 + 0.5;
				float d1 = textureLod(p_depth, q, 0.0).r;
				vec4 b = inv_proj * vec4(q * 2.0 - 1.0, d1, 1.0);
				vec3 w = (inv_view * vec4(b.xyz / b.w, 1.0)).xyz;
				// Only what lies under the water and beyond the surface here can be met: a stone's top standing out
				// of the water, or anything nearer the eye, stands in front of the ray and hides what it meets.
				if (d1 > d_s || w.y > p_s.y) {
					hidden = true;
					break;
				}
				float dz = b.z / b.w - pv.z; // > 0: the scene there is nearer the eye than the ray
				if (dz > 0.0) {
					// Bisect between the last sample in front of the scene and this one behind it.
					float lo = f_last;
					float hi = f;
					for (int j = 0; j < 4; j++) {
						float fm = 0.5 * (lo + hi);
						vec3 pm = vs + vt * fm;
						vec4 cm = proj * vec4(pm, 1.0);
						vec2 qm = cm.xy / cm.w * 0.5 + 0.5;
						float dm = textureLod(p_depth, qm, 0.0).r;
						vec4 bm = inv_proj * vec4(qm * 2.0 - 1.0, dm, 1.0);
						if (bm.z / bm.w - pm.z > 0.0) {
							hi = fm;
						} else {
							lo = fm;
						}
					}
					vec3 ph = vs + vt * hi;
					vec4 ch = proj * vec4(ph, 1.0);
					q = ch.xy / ch.w * 0.5 + 0.5;
					d1 = textureLod(p_depth, q, 0.0).r;
					b = inv_proj * vec4(q * 2.0 - 1.0, d1, 1.0);
					w = (inv_view * vec4(b.xyz / b.w, 1.0)).xyz;
					// Where the crossing is a jump in the screen's depth (a silhouette) rather than a face the ray
					// meets, the ray passed behind a stone, whose far side the screen does not hold; reading the
					// silhouette there stretched the stone's edge down over every row behind it, a comb of vertical
					// streaks. The straight-through pixel stands in, as for a hidden ray. Measured across the
					// bisection's last bracket (the scene's depth just before and at the crossing).
					vec3 pl = vs + vt * lo;
					vec4 cl = proj * vec4(pl, 1.0);
					vec2 ql = cl.xy / cl.w * 0.5 + 0.5;
					vec4 bl = inv_proj * vec4(ql * 2.0 - 1.0, textureLod(p_depth, ql, 0.0).r, 1.0);
					if (abs(bl.z / bl.w - b.z / b.w) > 0.05 + 0.03 * abs(ph.z) + 2.0 * abs(pl.z - ph.z)) {
						hidden = true;
						break;
					}
					q_last = q;
					len_last = d1 > 0.0 ? distance(w, p_s) : len_r * k * hi;
					break;
				}
				q_last = q;
				len_last = d1 > 0.0 ? distance(w, p_s) : len_r * k * f;
				f_last = f;
			}
			// Hidden: what the ray meets is behind the stone on the screen, so the straight-through pixel stands in
			// (the last clear sample, kept, stretched the bed's last row before the stone up over the whole band).
			uv_r = hidden ? uv : q_last;
			len = hidden ? (len0 < 1e4 ? z0 / max(-t.y, 0.05) : len0) : len_last;
		}
		for (int i = 0; i < (p_march ? 0 : 3); i++) {
			vec2 q = mix(uv, q_full, exp2(-float(i)));
			float d1 = texture(p_depth, q).r;
			// Reverse Z: behind the surface is a smaller depth than the surface's own. All four texels the
			// filtered colour read blends: one on a rock's edge left a dotted outline of it on the water.
			vec4 d4 = textureGather(p_depth, q, 0);
			if (max(max(d4.x, d4.y), max(d4.z, d4.w)) <= d_s) {
				uv_r = q;
				len = 1e4;
				if (i > 0 || k < 1.0) {
					// A shortened offset (something in front hid the full one): its point is not on the refracted
					// ray, so its distance is not the path's; the path is the refracted one to the bed. The colour
					// is read at the texel the depth test passed (a filtered read pulled in the rock's edge).
					// Using the sample's distance tinted the water there deeper, in a ghost of the rock's outline.
					vec2 sz = vec2(textureSize(p_depth, 0));
					uv_r = (floor(q * sz) + 0.5) / sz;
					len = len0 < 1e4 ? z0 / max(-t.y, 0.05) : len0;
				} else if (d1 > 0.0) {
					vec4 b = inv_proj * vec4(q * 2.0 - 1.0, d1, 1.0);
					len = distance((inv_view * vec4(b.xyz / b.w, 1.0)).xyz, p_s);
				}
				break;
			}
		}
	}
	// The known bed's path, unless something stands nearer on it (a rock, a hull): the screen's depth jumps
	// from pixel to pixel where waves bend the offset, the bed's does not.
	if (z_bed >= 0.0) {
		len = min(len, z0 / max(-t.y, 0.05) + 0.25);
	}
	vec3 st = max(p_ab, vec3(0.0)) + max(p_sc, vec3(0.0));
	float mu = max(-p_sun_dir.y, 0.05);
	// The light reaching the bed's depth: the sun along its refracted slant, the sky crossing the Snell window.
	float z_b = max(-t.y, 0.0) * min(len, 1e3);
	float w_sun = dot(p_sun_irr, vec3(1.0 / 3.0)) * mu / PI;
	float w_sky = dot(p_sky, vec3(1.0 / 3.0));
	vec3 down = w_sun + w_sky > 0.0 ? (w_sun * exp(-st * z_b / mu) + w_sky * exp(-st * 1.2 * z_b)) / (w_sun + w_sky) : exp(-st * z_b);
	vec3 seen = texture(p_screen, uv_r).rgb * down * exp(-st * len);
	r_seen = seen;
	r_bed = p_s + t * min(len, 1e3);
	{
		vec3 s_sun = w_sun * exp(-st * z_b / mu);
		vec3 s_sky = w_sky * exp(-st * 1.2 * z_b);
		r_sun_share = dot(s_sun, vec3(1.0)) / max(dot(s_sun + s_sky, vec3(1.0)), 1e-6);
	}
	// Scattered along the path (closed form, as the underwater pass): depth grows as -t.y per metre.
	vec3 a = st / mu;
	vec3 ka = st - a * t.y;
	// The sun's share marched in steps over the near water (shadows in the water: a cliff's or a cloud's shadow
	// darkens the water body, not only its surface), each step lit as far as the light reached where its
	// refracted ray entered the water (sun_visibility there), integrated in closed form within the step and
	// jittered per pixel; past the march the last step's light carries on (as the underwater pass, #287).
	const int STEPS = 12;
	float reach = min(len, 4.0 / max(dot(st, vec3(1.0 / 3.0)), 1e-3));
	float dt = reach / float(STEPS);
	vec3 seg_sun = vec3(0.0);
	vec3 vis = vec3(1.0);
	for (int i = 0; i < STEPS; i++) {
		float s0 = float(i) * dt;
		float sj = s0 + p_jitter * dt;
		float z = max(-t.y * sj, 0.0);
		vis = sun_visibility(p_s + t * sj - p_sun_dir * (z / mu));
		vec3 seg = mix(vec3(dt), (1.0 - exp(-ka * dt)) / ka, step(vec3(1e-4), abs(ka * dt)));
		seg_sun += vis * exp(-ka * s0) * seg;
	}
	if (len > reach) {
		float rest = min(len - reach, 1e4);
		vec3 seg = mix(vec3(rest), (1.0 - exp(-ka * rest)) / ka, step(vec3(1e-4), abs(ka * rest)));
		seg_sun += vis * exp(-ka * reach) * seg;
	}
	vec3 kb = st - st * 1.2 * t.y;
	vec3 seg_sky = mix(vec3(len), (1.0 - exp(-kb * len)) / kb, step(vec3(1e-4), abs(kb * len)));
	float cth = dot(p_sun_dir, -t);
	float g2 = p_g * p_g;
	float hg = (1.0 - g2) / (4.0 * PI * pow(max(1.0 + g2 - 2.0 * p_g * cth, 1e-4), 1.5));
	vec3 sun = p_sun_irr * hg * seg_sun;
	vec3 sky = p_sky * 0.5 * seg_sky;
	vec3 single = max(p_sc, vec3(0.0)) * (sun + sky);
	// Turbid water (silt, ash: WorldWater sediment) scatters so much that most of its light has bounced many times,
	// each bounce filtered by the particles' colour, which single scattering misses (muddy water read as grey sky
	// light instead of brown, as the path tracer draws it). Its diffuse reflectance as a deep medium by the similarity
	// relation (reduced albedo w' = b(1 - g) / (a + b(1 - g)); Hapke 1981: R = (1 - sqrt(1 - w')) / (1 + sqrt(1 - w'))),
	// lit by the light coming down, over the depth it holds (diffusion's attenuation sqrt(3 a (a + b'))). Only where
	// the water is that turbid: clear water's colour stays its single scattering, as before.
	vec3 bb = max(p_sc, vec3(0.0)) * (1.0 - p_g);
	vec3 aa = max(p_ab, vec3(0.0));
	vec3 wp = bb / max(aa + bb, vec3(1e-6));
	// Turbid: a transport mean free path (1 / b') of a metre or less, where the light has diffused (clear water's
	// b' is a few hundredths per metre).
	float turbid = smoothstep(0.1, 1.0, max(bb.r, max(bb.g, bb.b)));
	if (turbid > 0.0) {
		vec3 gam = sqrt(max(1.0 - wp, vec3(0.0)));
		// Calibrated against the path tracer's turbid river at flood loads (its paths through the forward-scattering
		// medium redden the light more than the isotropic two-stream does): the same brown in both.
		float wpm = max(max(wp.r, max(wp.g, wp.b)), 1e-4);
		vec3 rd = (1.0 - gam) / (1.0 + gam) * pow(wp / wpm, vec3(1.2)) * (1.12 + 1.25 * max(wpm - 0.42, 0.0));
		vec3 sig = sqrt(3.0 * aa * (aa + bb));
		vec3 held = 1.0 - exp(-2.0 * sig * max(-t.y, 0.05) * min(len, 1e3));
		vec3 multi = rd * (p_sun_irr * mu + p_sky * PI) / PI * held * vis;
		// The diffuse reflectance counts every order of scattering, the first included: it replaces the single term.
		single = mix(single, multi, turbid);
	}
	return seen + single;
}

// The mirror ray from view-space point p_vtx (normal p_n) marched over the screen's depth (screen_reflections):
// steps growing with the square of their index (fine near the water, where contact matters), jittered per
// pixel, a hit where the ray passes behind the depth by less than a thickness growing with distance, then five
// halvings between the last two steps. Its Fresnel share (exact, air into water) blended over p_refl as
// water_raster_reflect() does (written for blend_mix); a miss or the screen's edge fades to the dark around.
vec4 water_screen_reflect(sampler2D p_screen, sampler2D p_depth, vec4 p_refl, vec3 p_n, vec3 p_vtx, vec2 p_uv, float p_surface, float p_jitter, mat4 proj, mat4 inv_proj) {
	vec3 v = normalize(p_vtx);
	vec3 n = dot(p_n, v) > 0.0 ? normalize(p_n - 2.0 * dot(p_n, v) * v) : p_n;
	vec3 r = reflect(v, n);
	float nv = clamp(-dot(n, v), 1e-3, 1.0);
	float st = sqrt(max(1.0 - nv * nv, 0.0)) / 1.33;
	float ct = sqrt(max(1.0 - st * st, 0.0));
	float rs = (nv - 1.33 * ct) / (nv + 1.33 * ct);
	float rp = (1.33 * nv - ct) / (1.33 * nv + ct);
	float f = 0.5 * (rs * rs + rp * rp) * p_surface;
	if (f <= 0.0) {
		return p_refl;
	}
	const int N = 24;
	const float MAX_D = 24.0;
	float t_prev = 0.0;
	float t_hit = -1.0;
	for (int i = 0; i < N; i++) {
		float u = (float(i) + p_jitter) / float(N);
		float t = 0.03 + MAX_D * u * u;
		vec3 q = p_vtx + r * t;
		if (q.z > -0.05) {
			break;
		}
		vec4 c = proj * vec4(q, 1.0);
		vec2 uv = c.xy / c.w * 0.5 + 0.5;
		if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
			break;
		}
		vec4 b = inv_proj * vec4(uv * 2.0 - 1.0, textureLod(p_depth, uv, 0.0).r, 1.0);
		float dz = b.z / b.w - q.z; // > 0: the scene there is nearer the eye than the ray
		if (dz > 0.0 && dz < 0.15 + 0.12 * (t - t_prev) / max(-r.z, 0.1) + 0.05 * t) {
			t_hit = t;
			break;
		}
		t_prev = t;
	}
	if (t_hit < 0.0) {
		vec3 dark = p_refl.rgb * p_refl.a * (1.0 - f);
		float a_d = 1.0 - (1.0 - p_refl.a) * (1.0 - f);
		return vec4(dark / max(a_d, 1e-4), a_d);
	}
	float lo = t_prev;
	float hi = t_hit;
	for (int k = 0; k < 5; k++) {
		float m = 0.5 * (lo + hi);
		vec4 c = proj * vec4(p_vtx + r * m, 1.0);
		vec2 uv = c.xy / c.w * 0.5 + 0.5;
		vec4 b = inv_proj * vec4(uv * 2.0 - 1.0, textureLod(p_depth, uv, 0.0).r, 1.0);
		if (b.z / b.w > (p_vtx + r * m).z) {
			hi = m;
		} else {
			lo = m;
		}
	}
	vec4 c = proj * vec4(p_vtx + r * hi, 1.0);
	vec2 huv = c.xy / c.w * 0.5 + 0.5;
	vec2 e = min(huv, 1.0 - huv);
	float fade = smoothstep(0.0, 0.06, min(e.x, e.y)) * (1.0 - smoothstep(0.7 * MAX_D, MAX_D, hi));
	vec3 seen = textureLod(p_screen, huv, 0.0).rgb * fade;
	float a_new = 1.0 - (1.0 - p_refl.a) * (1.0 - f);
	return vec4((p_refl.rgb * p_refl.a * (1.0 - f) + seen * f) / max(a_new, 1e-4), a_new);
}


vec3 water_clear_through(sampler2D p_screen, sampler2D p_depth, vec3 p_s, vec3 v, vec3 n, vec2 uv, float z_bed,
		vec3 p_ab, vec3 p_sc, float p_g, vec3 p_sun_dir, vec3 p_sun_irr, vec3 p_sky, float p_jitter, mat4 proj,
		mat4 view, mat4 inv_proj, mat4 inv_view) {
	vec3 seen;
	vec3 bed;
	float share;
	return water_clear_through_ex(p_screen, p_depth, p_s, v, n, uv, z_bed, p_ab, p_sc, p_g, p_sun_dir, p_sun_irr, p_sky,
			p_jitter, proj, view, inv_proj, inv_view, true, seen, bed, share);
}
#endif
)";

// One Shader per source for every water node: each Shader resource is compiled on its own (raster variants and the
// path tracer's hit groups), so a scene of small pools or many rivers paid for the whole water shader once per node,
// about half a gigabyte each on a CPU driver. The materials (each node's uniforms) stay per node. Held by the
// materials only: the cache keeps ids, so a shader is freed with the last node using it.
inline Ref<Shader> water_shared_shader(const String &p_code) {
	static HashMap<String, ObjectID> cache;
	const ObjectID *id = cache.getptr(p_code);
	if (id != nullptr) {
		Shader *existing = ObjectDB::get_instance<Shader>(*id);
		if (existing != nullptr) {
			return Ref<Shader>(existing);
		}
	}
	Ref<Shader> shader;
	shader.instantiate();
	shader->set_code(p_code);
	cache[p_code] = shader->get_instance_id();
	return shader;
}

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

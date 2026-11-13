/**************************************************************************/
/*  physx_hair_3d.cpp                                                     */
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

#include "physx_hair_3d.h"
#include "physx_skinned_cloth_3d.h"

#include "core/config/engine.h"
#include "core/math/geometry_3d.h"
#include "core/math/random_pcg.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/skeleton_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "scene/main/window.h"
#include "scene/resources/3d/skin.h"
#include "scene/resources/3d/world_3d.h"
#include "scene/resources/environment.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"
#include "scene/resources/texture_rd.h"
#include "servers/physics_3d/physics_server_3d.h"

#include "modules/modules_enabled.gen.h" // For weather.
#ifdef MODULE_WEATHER_ENABLED
#include "modules/weather/weather_map_3d.h"
#endif

namespace {

// Drawn from PhysXHair3D's simulated points, which the solver writes to hair_positions sorted far to
// near from the camera (fixed-size slots per strand). Every vertex is one side of one of the two
// crossed strips around a slot point: CUSTOM0 = (texel, previous texel, next texel, side -1 or 1),
// CUSTOM1.w = which strip. The strips are rebuilt around the simulated strand, as HairMesh lays them
// out (UV.x across, UV.y along), so the path tracer's hair_strands shading reads the fibre offset the
// same way. rt_vertex_dynamic: the path tracer recaptures it every frame. alpha_motion_vectors: the
// blended strands write their motion (from last frame's positions) for TAA, FSR 2 and DLSS.
//
// HAIR_SCATTERING (hair_shading on) lights the raster strands with the Marschner hair model as
// Unreal Engine evaluates it (Karis 2016), see light(). Off, Godot's standard anisotropic lighting.
const char *HAIR_RENDER_HEAD = R"(
shader_type spatial;
render_mode cull_disabled, rt_vertex_dynamic, rt_force_opaque, hair_strands, alpha_motion_vectors;
)";

const char *HAIR_RENDER_SHADER = R"(

uniform sampler2D hair_positions : filter_nearest, repeat_disable;
uniform sampler2D hair_previous_positions : filter_nearest, repeat_disable; // last frame's
uniform int texture_width = 1;
uniform vec4 hair_color : source_color = vec4(0.3, 0.2, 0.1, 1.0);
uniform vec3 fibre_absorption = vec3(0.4, 0.7, 1.4);
// Colour variation (docs/ideas/hair.md, stage 2s): the red melanin's share of fibre_absorption,
// the share of grey strands, how much the sun has bleached the lengths, and d'Eon et al. 2011's
// mass colour exponent (hair_color = exp(-d sqrt(absorption))).
uniform vec3 fibre_absorption_pheo = vec3(0.0);
uniform float grey_ratio = 0.0;
uniform float sun_bleach = 0.0;
uniform float mass_d = 5.97;
uniform vec2 fibre_roughness = vec2(0.3, 0.3);
uniform float root_width = 0.0004;
uniform float tip_width = 0.0001;
uniform float lod_width_scale = 1.0; // raster: 1 / the share of strands drawn (distance LOD)
uniform float lod_fade = 1.0; // raster: the cross-fade between strands and cards (1: shown in full)
uniform int slot_points = 1; // texels per strand

// The deep opacity maps (Yuksel and Keyser 2008, as Unreal's hair), built by the solver, one per
// light (a row of the texture each): per cell the first hair's depth, the strands' coverage summed
// to the end of three layers past it, and (beside it) the first opaque depth. A directional
// light's map is orthographic; an omni or spot light's a perspective view from the light.
uniform int dom_maps = 0;
uniform sampler2D hair_deep_shadow : filter_nearest, repeat_disable;
// Per map, as the solver's dom_slot: centre and radius (world); first axis and kind (1: local);
// second axis and the half angle's tangent; toward the light and the light's distance.
uniform vec4 dom_slot[16];
uniform vec3 dom_layers = vec3(0.01, 0.04, 0.15);
uniform int dom_cells = 256;
uniform float dom_density = 0.3; // calibrated to the path tracer: fibres pass much of the light on (TT)
// Snow settled on the hair (docs/ideas/hair.md, stage 2r): the share of the hair's top layer it
// covers, and the deep opacity map (dom_snow_slot, -1: none) seen from where the snow comes from,
// which gives each point the hair above it.
uniform float snow_cover = 0.0;
uniform int dom_snow_slot = -1;

// xyz the point, w its share along the strand (by point), or -1 past the strand's end; w + 2 on a
// strand the distance LOD leaves out of the raster draw.
vec4 hair_point(float p_index) {
	int i = int(p_index + 0.5);
	ivec2 texel = ivec2(i % texture_width, i / texture_width);
	// PREVIOUS_FRAME: the vertex function runs again for last frame's position (motion vectors),
	// so the ribbon is rebuilt where the strands were then and TAA, FSR 2 and DLSS follow them.
	return PREVIOUS_FRAME ? texelFetch(hair_previous_positions, texel, 0) : texelFetch(hair_positions, texel, 0);
}

varying float strand_across; // -1..1 across the drawn ribbon (raster)
varying float strand_half_px; // the strand's real half width, in pixels (raster)
varying float strand_drawn_px; // the drawn half width, in pixels (raster)
varying vec3 strand_tangent; // along the strand, view space (raster lighting)
varying flat float strand_seed; // 0..1 per strand: its own shadow-map noise
varying vec4 dom_trans; // vertex to fragment and light(): per map, the light it lets through
varying vec4 dom_past; // vertex to fragment: per map, metres past the first hair (-1: none)
varying vec3 dom_world; // fragment to light(): the fragment's world position
varying vec3 light_tangent; // fragment to light(): along the fibres, view space
varying float hair_snow; // fragment to light(): 1 where snow lies on a card (lit as snow, not fibre)
// fragment to light(): x a card's light left by its own clump (1 for strands), y a thick strand's
// fibre density (0: not one).
varying vec2 hair_clump;

// Coverage summed up to depth p_rel past the first hair, from a cell's layers.
float dom_coverage(vec4 p_cell, float p_rel) {
	if (p_rel <= 0.0) {
		return 0.0;
	} else if (p_rel < dom_layers.x) {
		return p_cell.y * p_rel / dom_layers.x;
	} else if (p_rel < dom_layers.y) {
		return mix(p_cell.y, p_cell.z, (p_rel - dom_layers.x) / (dom_layers.y - dom_layers.x));
	} else if (p_rel < dom_layers.z) {
		return mix(p_cell.z, p_cell.w, (p_rel - dom_layers.y) / (dom_layers.z - dom_layers.y));
	}
	return p_cell.w;
}

// The deep opacity maps at a world point: per map the light let through and the metres past the
// first hair (-1: none).
void dom_lookup(vec3 world, out vec4 r_trans, out vec4 r_past) {
	r_trans = vec4(1.0);
	r_past = vec4(-1.0);
	// The deep opacity maps: how much hair (and whether the head or body) lies between this
	// strand point and each light, smooth across the layers and filtered across 4 cells.
	// Per point, not per fragment: a strand is far thinner than a cell, and the segments
	// are about as long as one.
	float absorb = 1.0;
#ifdef HAIR_SCATTERING
	absorb = 1.0 - 0.91 * dot(sqrt(hair_color.rgb), vec3(0.2126, 0.7152, 0.0722));
#endif
	for (int k = 0; k < 4; k++) {
		if (k >= dom_maps) {
			break;
		}
		vec4 sc = dom_slot[k * 4];
		vec4 su = dom_slot[k * 4 + 1];
		vec4 sv = dom_slot[k * 4 + 2];
		vec4 sl = dom_slot[k * 4 + 3];
		vec2 m;
		float tl;
		if (su.w > 0.5) {
			vec3 d = world - (sc.xyz + sl.xyz * sl.w);
			float z = max(-dot(d, sl.xyz), 1e-4);
			m = (vec2(dot(d, su.xyz), dot(d, sv.xyz)) / (z * sv.w) * 0.5 + 0.5) * float(dom_cells) - 0.5;
			tl = max(length(d) - (sl.w - sc.w), 0.0);
		} else {
			vec3 d = world - sc.xyz;
			m = (vec2(dot(d, su.xyz), dot(d, sv.xyz)) / (2.0 * sc.w) + 0.5) * float(dom_cells) - 0.5;
			tl = sc.w - dot(d, sl.xyz);
		}
		ivec2 c = ivec2(floor(m));
		vec2 f = m - vec2(c);
		ivec2 row = ivec2(0, k * dom_cells);
		float trans = 0.0;
		float front = 1e9;
		for (int j = 0; j < 4; j++) {
			ivec2 o = ivec2(j & 1, j >> 1);
			ivec2 cc = clamp(c + o, ivec2(0), ivec2(dom_cells - 1)) + row;
			vec4 cell = texelFetch(hair_deep_shadow, cc, 0);
			float opaque = texelFetch(hair_deep_shadow, cc + ivec2(dom_cells, 0), 0).x;
			float covered = 0.0;
			if (cell.x < 1e8) {
				covered = dom_coverage(cell, tl - cell.x);
				front = min(front, cell.x);
			}
			vec2 w = mix(1.0 - f, f, vec2(o));
			trans += w.x * w.y * exp(-absorb * dom_density * covered) * (tl > opaque + 0.01 ? 0.0 : 1.0);
		}
		r_trans[k] = trans;
		r_past[k] = front < 1e8 ? max(tl - front, 0.0) : -1.0;
	}
}

// A fibre's absorption and mass colour along its length, p_along (0 root, 1 tip). p_seed: the
// strand's own 0..1, or -1 for a clump of strands (a card), which takes the grey share as a mix.
// Grey hairs grow from follicles that no longer make melanin (canities): the keratin's own faint
// yellow is left. Sunlight breaks eumelanin down faster than pheomelanin (Nogueira and Joekes
// 2004): the lengths that have grown out longest, toward the tips, lighten and turn warmer.
void fibre_look(float p_seed, float p_along, out vec3 r_sigma, out vec3 r_color) {
	r_sigma = fibre_absorption;
	r_color = hair_color.rgb;
	if (grey_ratio <= 0.0 && sun_bleach <= 0.0) {
		return;
	}
	float bleach = sun_bleach * smoothstep(0.2, 1.0, p_along);
	vec3 eu = fibre_absorption - fibre_absorption_pheo;
	r_sigma = eu * (1.0 - 0.8 * bleach) + fibre_absorption_pheo * (1.0 - 0.3 * bleach);
	r_color = exp(-mass_d * sqrt(r_sigma));
	const vec3 grey_sigma = vec3(0.03, 0.045, 0.09);
	vec3 grey_color = exp(-mass_d * sqrt(grey_sigma));
	if (p_seed < 0.0) {
		r_sigma = mix(r_sigma, grey_sigma, grey_ratio);
		r_color = mix(r_color, grey_color, grey_ratio);
	} else if (fract(p_seed * 113.17 + 0.37) < grey_ratio) {
		r_sigma = grey_sigma;
		r_color = grey_color;
	}
}

void vertex() {
	strand_seed = fract(sin(floor((CUSTOM0.x + 0.5) / float(max(slot_points, 1))) * 12.9898) * 43758.5453);
	vec4 here = hair_point(CUSTOM0.x);
	vec4 before = hair_point(CUSTOM0.y);
	vec4 after = hair_point(CUSTOM0.z);
	bool card_strand = here.w > 3.5; // a hair card's strand: drawn by the card mesh only
	bool lod_dropped = here.w > 1.5;
	float along = card_strand ? here.w - 4.0 : (lod_dropped ? here.w - 2.0 : here.w);
	vec3 p = here.xyz;
	vec3 t = (after.w < 0.0 ? p : after.xyz) - (before.w < 0.0 ? p : before.xyz);
	t = dot(t, t) > 1e-14 ? normalize(t) : vec3(0.0, 1.0, 0.0);
	vec3 a = normalize(cross(t, abs(t.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
	vec3 across = CUSTOM1.w > 0.5 ? cross(t, a) : a;
	// Past the strand's end (a shorter strand padded to the slot) the width is zero: nothing drawn.
	float half_width = along < 0.0 || card_strand ? 0.0 : 0.5 * mix(root_width, tip_width, along);
	UV = vec2(CUSTOM0.w > 0.0 ? 1.0 : 0.0, max(along, 0.0));
	VERTEX = p + across * (half_width * CUSTOM0.w);
	NORMAL = cross(across, t);
	TANGENT = across;
	BINORMAL = t;
	strand_across = sign(CUSTOM0.w);
	strand_half_px = 1.0;
	strand_drawn_px = 1.0;
	strand_tangent = normalize(mat3(MODELVIEW_MATRIX) * t);
	dom_trans = vec4(1.0);
	dom_past = vec4(-1.0);
	// The second strip draws nothing in raster (collapsed below), so it skips the lookups.
	if (dom_maps > 0 && !IN_PATH_TRACER && !IN_SHADOW_PASS && !PREVIOUS_FRAME && CUSTOM1.w < 0.5) {
		vec4 lt;
		vec4 lp;
		dom_lookup((MODEL_MATRIX * vec4(p, 1.0)).xyz, lt, lp);
		dom_trans = lt;
		dom_past = lp;
	}
	if (!IN_PATH_TRACER) {
		// Raster (and its shadow maps): AMD TressFX's strand rendering. One ribbon facing the camera,
		// widened by 0.71 px a side so no strand falls between pixels, with its opacity cut to the
		// share of the pixel the real strand covers (TressFX's coverage), alpha blended; shadow maps
		// keep that share of their texels, so the hair shades itself like an opacity map. The path
		// tracer keeps the real-width crossed strips.
		vec3 pv = (MODELVIEW_MATRIX * vec4(p, 1.0)).xyz;
		vec3 tv = normalize(mat3(MODELVIEW_MATRIX) * t);
		vec3 eye = PROJECTION_MATRIX[3][3] > 0.5 ? vec3(0.0, 0.0, -1.0) : normalize(pv);
		vec3 right = cross(tv, eye);
		right = dot(right, right) > 1e-12 ? normalize(right) : normalize(cross(tv, vec3(0.0, 1.0, 0.0)));
		vec4 clip = PROJECTION_MATRIX * vec4(pv, 1.0);
		// abs: the projection flips y (Vulkan), so [1][1] is negative.
		float px = 2.0 * clip.w / max(abs(PROJECTION_MATRIX[1][1]) * VIEWPORT_SIZE.y, 1e-6); // one pixel, in metres
		// The widening is capped (2 mm in shadow maps, about a texel of a close cascade, and 1 cm
		// otherwise): a far cascade's texel, or a pass with a tiny viewport, would otherwise widen
		// every strand to centimetres and flood the pass with fragments. Coverage keeps the total
		// opacity right at any drawn width.
		float grow = min(0.71 * px, IN_SHADOW_PASS ? 0.002 : 0.01);
		// Distance LOD (TressFX): the strands left in are widened so the hair keeps its opacity;
		// those left out, and the padding past a strand's end, collapse and draw nothing.
		half_width = lod_dropped ? 0.0 : half_width * lod_width_scale;
		float drawn = half_width > 0.0 ? half_width + grow : 0.0;
		strand_half_px = half_width / px;
		strand_drawn_px = max(drawn / px, 1e-6);
		// The second strip is not needed facing the camera: collapsed to a line, it draws nothing.
		float side = CUSTOM1.w > 0.5 ? 0.0 : strand_across;
		POSITION = PROJECTION_MATRIX * vec4(pv + right * (drawn * side), 1.0);
		NORMAL = normalize(transpose(mat3(MODELVIEW_MATRIX)) * cross(right, tv));
		TANGENT = normalize(transpose(mat3(MODELVIEW_MATRIX)) * right);
	}
}

void fragment() {
	light_tangent = strand_tangent;
	hair_snow = 0.0;
	hair_clump.x = 1.0;
	hair_clump.y = 0.0;
	vec3 sigma;
	vec3 mass;
	fibre_look(strand_seed, UV.y, sigma, mass);
	ALBEDO = mass;
	ROUGHNESS = fibre_roughness.x;
	ANISOTROPY = 0.8;
	ANISOTROPY_FLOW = vec2(1.0, 0.0); // The highlight runs across the strands.
	HAIR = 1.0;
	HAIR_ABSORPTION = sigma;
	HAIR_ROUGHNESS = fibre_roughness;
	float coverage = 1.0;
	if (!IN_PATH_TRACER) {
		// TressFX coverage: the share of this pixel the strand covers. A strand wider than a pixel
		// is solid with a one-pixel soft edge; a thinner one spreads its width over a tent across
		// the drawn ribbon, so the hair's total opacity matches its real width.
		float d = abs(strand_across) * strand_drawn_px; // pixels from the strand's centre
		if (strand_half_px >= 0.5) {
			coverage = clamp(strand_half_px - d + 0.5, 0.0, 1.0);
		} else {
			coverage = clamp(2.0 * strand_half_px / strand_drawn_px * (1.0 - d / strand_drawn_px), 0.0, 1.0);
		}
	}
#ifdef HAIR_SCATTERING
	// Raster shadow maps only: the path tracer's shadow rays also run with IN_SHADOW_PASS, and
	// there the fibres stay solid (its hair BSDF carries the light through them).
	if (IN_SHADOW_PASS && !IN_PATH_TRACER) {
		// Light passes through a fibre: what the TT lobe carries straight on (Fresnel twice and the
		// absorption across it, Unreal's Tp at normal incidence) is not blocked, as in dual
		// scattering's forward transmittance (Zinke et al. 2008). Blonde hair lets most light
		// through and dark hair little; the shadow behind n strands falls as that to the n-th power.
		coverage *= 1.0 - 0.91 * dot(sqrt(mass), vec3(0.2126, 0.7152, 0.0722));
	}
#endif
	if (IN_SHADOW_PASS) {
		// Shadow maps take no blending: the coverage becomes the share of texels kept (interleaved
		// gradient noise), so the hair's shadow is partial and builds up through the volume.
		// Each strand draws its own threshold (stochastic transparency, Enderton et al. 2010), so a
		// texel behind several strands keeps 1 - (1 - c1)(1 - c2)... of them, not the largest c.
		float noise = fract(52.9829189 * fract(dot(FRAGCOORD.xy, vec2(0.06711056, 0.00583715))) + strand_seed);
		if (coverage <= noise) {
			discard;
		}
		coverage = 1.0;
	}
	dom_world = (INV_VIEW_MATRIX * vec4(VERTEX, 1.0)).xyz;
	if (dom_maps > 0 && !IN_PATH_TRACER && !IN_SHADOW_PASS) {
		// The deep opacity maps, read per strand point (vertex function) and interpolated: the
		// light each lets through, and how far past the first hair this point lies. The hair then
		// skips its own layers in the shadow maps (DIRECTIONAL_SHADOW_OFFSET and
		// POSITIONAL_SHADOW_OFFSET move the lookups to just in front of the first hair), which
		// keeps what lies beyond the groom. The omni and spot lights share one offset, the
		// largest: the extra skipped is hair (in their own maps) or the head and body (in the
		// maps' opaque depths).
		float local_past = -1.0;
		for (int k = 0; k < 4; k++) {
			if (k >= dom_maps) {
				break;
			}
			if (k == dom_snow_slot) {
				continue; // the snow's map: no light
			}
			if (dom_slot[k * 4 + 1].w > 0.5) {
				local_past = max(local_past, dom_past[k]);
			} else if (dom_past[k] >= 0.0) {
				DIRECTIONAL_SHADOW_OFFSET = dom_past[k] + 0.005;
			}
		}
		if (local_past >= 0.0) {
			POSITIONAL_SHADOW_OFFSET = local_past + 0.005;
		}
	}
	// rt_force_opaque: the path tracer traces the strands as solid fibres (ALPHA is 1 there).
	// Blended, as TressFX blends its strands: each fades over what lies behind it by the share of
	// the pixel it covers. Thin strands are faint, so the blend barely depends on draw order.
	// The shadow copy writes no ALPHA: Godot leaves blended materials out of the shadow maps.
#ifndef HAIR_SHADOW_COPY
	ALPHA = coverage * lod_fade;
#endif
}

#ifdef HAIR_SCATTERING
// Unreal Engine's hair shading (Karis, "Physically Based Hair Shading in Unreal", SIGGRAPH 2016):
// Marschner et al.'s three lobes (R reflection, TT through the fibre, TRT the coloured second
// highlight) in d'Eon's form, with Unreal's fits for the azimuthal terms, plus its approximation
// of multiple scattering so the side away from the light is lit through the hair, tinted deeper
// the more hair the light crosses. Widths and tilt follow the path tracer's fibre (Chiang 2016) so
// the highlights sit where the path tracer puts them.
float hair_g(float b, float x) {
	b = max(b, 1e-3);
	return exp(-0.5 * x * x / (b * b)) / (2.5066283 * b);
}

float hair_f(float c) {
	const float f0 = 0.0465; // n = 1.55
	return f0 + (1.0 - f0) * pow(1.0 - clamp(c, 0.0, 1.0), 5.0);
}

void light() {
	vec3 T = normalize(light_tangent);
	vec3 base = ALBEDO;
	float sin_l = clamp(dot(T, LIGHT), -1.0, 1.0);
	float sin_v = clamp(dot(T, VIEW), -1.0, 1.0);
	float cos_d = max(cos(0.5 * abs(asin(sin_v) - asin(sin_l))), 0.05);
	vec3 lp = LIGHT - sin_l * T;
	vec3 vp = VIEW - sin_v * T;
	float cos_phi = dot(lp, vp) * inversesqrt(dot(lp, lp) * dot(vp, vp) + 1e-4);
	float cos_half_phi = sqrt(clamp(0.5 + 0.5 * cos_phi, 0.0, 1.0));
	float n_prime = 1.19 / cos_d + 0.36 * cos_d;
	// Longitudinal width: Chiang's mapping of the fibre roughness, as the path tracer.
	float bm = fibre_roughness.x;
	float b = 0.726 * bm + 0.812 * bm * bm + 3.7 * pow(bm, 20.0);
	const float tilt = 0.035; // cuticle scales, 2 degrees

	vec3 s = vec3(0.0);
	// R
	float sa = sin(-2.0 * tilt);
	float ca = cos(-2.0 * tilt);
	float shift = 2.0 * sa * (ca * cos_half_phi * sqrt(1.0 - sin_v * sin_v) + sa * sin_v);
	s += vec3(hair_g(b * 1.41421 * cos_half_phi, sin_l + sin_v - shift) * 0.25 * cos_half_phi * hair_f(sqrt(clamp(0.5 + 0.5 * dot(VIEW, LIGHT), 0.0, 1.0))));
	// TT
	float a = 1.0 / n_prime;
	float h = cos_half_phi * (1.0 + a * (0.6 - 0.8 * cos_phi));
	float f = hair_f(cos_d * sqrt(clamp(1.0 - h * h, 0.0, 1.0)));
	vec3 tp = pow(base, vec3(0.5 * sqrt(clamp(1.0 - h * h * a * a, 0.0, 1.0)) / cos_d));
	s += hair_g(0.5 * b, sin_l + sin_v - tilt) * exp(-3.65 * cos_phi - 3.98) * (1.0 - f) * (1.0 - f) * tp;
	// TRT
	f = hair_f(0.5 * cos_d);
	tp = pow(base, vec3(0.8 / cos_d));
	s += hair_g(2.0 * b, sin_l + sin_v - 4.0 * tilt) * exp(17.0 * cos_phi - 16.78) * (1.0 - f) * (1.0 - f) * f * tp;
	// Multiple scattering: wrapped light around a normal facing the viewer, blended with
	// Kajiya-Kay's diffuse, tinted by the hair's colour the deeper it is in shadow. Only a
	// directional light's attenuation is its shadow alone.
	vec3 vn = VIEW - T * sin_v;
	vn = dot(vn, vn) > 1e-8 ? normalize(vn) : VIEW;
	float wrap = clamp((dot(vn, LIGHT) + 1.0) * 0.25, 0.0, 1.0);
	float scatter = (1.0 / PI) * mix(wrap, 1.0 - abs(sin_l), 0.33);
	float luma = max(dot(base, vec3(0.2126, 0.7152, 0.0722)), 1e-4);
	// A light with a deep opacity map: its shadow through the hair is the map's (the shadow map
	// lookup skipped the hair's own layers). The map is found by the light's direction: a
	// directional light's own, an omni or spot light's toward its position.
	float attenuation = ATTENUATION;
	float shadow = LIGHT_IS_DIRECTIONAL ? clamp(attenuation, 0.0, 1.0) : 1.0;
	if (dom_maps > 0) {
		vec3 lw = normalize(mat3(INV_VIEW_MATRIX) * LIGHT);
		for (int k = 0; k < 4; k++) {
			if (k >= dom_maps) {
				break;
			}
			if (k == dom_snow_slot) {
				continue; // the snow's map: no light
			}
			vec4 sl = dom_slot[k * 4 + 3];
			bool is_local = dom_slot[k * 4 + 1].w > 0.5;
			if (is_local == LIGHT_IS_DIRECTIONAL) {
				continue;
			}
			vec3 to_light = is_local ? normalize(dom_slot[k * 4].xyz + sl.xyz * sl.w - dom_world) : sl.xyz;
			if (dot(lw, to_light) > 0.999) {
				// Clamped: a transmittance is 0..1, and an out-of-range value interpolated across a
				// strand (seen on llvmpipe; gone with the clamp, the maps' own values check fine per
				// vertex) blacked out 2x2 pixel blocks of strands in the deep shadow path.
				float tr = clamp(dom_trans[k], 0.0, 1.0);
				attenuation *= tr;
				shadow = LIGHT_IS_DIRECTIONAL ? clamp(attenuation, 0.0, 1.0) : tr;
				break;
			}
		}
	}
	attenuation *= hair_clump.x;
	if (hair_clump.y > 0.0 && cos_phi > 0.0) {
		// Thick strands' shadowing-masking (Huang et al. 2025, Eqs. 12-15): with light and view on
		// the same side, fibres in the clump shadow (P_s) or mask (P_m) those seen; a shadowed one
		// gets the light a fibre passes on, a_F.
		float a_f = 0.9091 * dot(sqrt(base), vec3(0.2126, 0.7152, 0.0722));
		float p_s = (1.0 - hair_clump.y) * (1.0 - cos_phi);
		float p_m = (1.0 - hair_clump.y) * 0.2;
		attenuation *= p_s * a_f + max(1.0 - p_m - p_s, 0.0);
	}
	s += sqrt(base) * scatter * pow(base / luma, vec3(1.0 - shadow));
	if (hair_snow > 0.5) {
		// Snow on a card: matte (Lambert; ALBEDO is the snow's), as the snow's own clumps.
		DIFFUSE_LIGHT += max(dot(NORMAL, LIGHT), 0.0) * ATTENUATION * LIGHT_COLOR / PI;
	} else {
		SPECULAR_LIGHT += max(s, vec3(0.0)) * LIGHT_COLOR * attenuation;
	}
}
#endif
)";

// Snow on the hair (docs/ideas/hair.md, stage 2r): small clumps of crystals caught on the strands,
// drawn as their own mesh, one closed blob (an octahedron, smooth shaded) per vertex group, its
// corner in NORMAL. Each group belongs to a slot of hair_positions: CUSTOM0 = (the slot's first
// texel, which clump of the strand, its drawn points, 0). The strand's root (fixed to the scalp)
// keys its random numbers, so a strand keeps its clumps when the blend sort moves it to another
// slot. A clump sits at a random point past the scalp, where the deep opacity map seen from where
// the snow comes from shows little hair above it (the top layer), on the side the snow comes from;
// the others shrink to nothing. Opaque, so the path tracer traces the same clumps.
const char *HAIR_SNOW_HEAD = R"(
shader_type spatial;
render_mode cull_disabled, rt_vertex_dynamic;
#define HAIR_SCATTERING
)";

const char *HAIR_SNOW_FUNCS = R"(
uniform vec3 snow_albedo = vec3(0.85);
uniform float snow_grain = 0.0025; // m, a clump's typical diameter (the load grows it)

float snow_hash(vec3 q, float salt) {
	return fract(sin(dot(q, vec3(12.9898, 78.233, 37.719)) + salt * 17.13) * 43758.5453);
}

void vertex() {
	strand_seed = 0.0;
	dom_trans = vec4(1.0);
	dom_past = vec4(-1.0);
	strand_across = 0.0;
	strand_half_px = 1.0;
	strand_drawn_px = 1.0;
	strand_tangent = vec3(0.0, 1.0, 0.0);
	float base = CUSTOM0.x;
	float n = max(CUSTOM0.z, 2.0);
	vec4 root = hair_point(base);
	vec3 p = root.xyz;
	float size = 0.0;
	vec3 key = floor(root.xyz * 5000.0) + vec3(CUSTOM0.y * 7.0); // 0.2 mm: the root's place
	vec3 up = vec3(0.0, 1.0, 0.0);
	// root.w: 0..1 a drawn strand, 2..3 one the raster's distance LOD leaves out (it still holds
	// snow), past 3.5 a hair card's strand, below 0 none.
	if (snow_cover > 0.0 && dom_snow_slot >= 0 && dom_snow_slot < dom_maps && root.w >= 0.0 && root.w < 3.5) {
		float fk = (0.03 + 0.97 * snow_hash(key, 1.0)) * (n - 1.0);
		float k = min(floor(fk), n - 2.0);
		vec4 a = hair_point(base + k);
		vec4 b = hair_point(base + k + 1.0);
		if (a.w >= 0.0 && a.w < 3.5) {
			b = b.w >= 0.0 && b.w < 3.5 ? b : a;
			p = mix(a.xyz, b.xyz, clamp(fk - k, 0.0, 1.0));
			vec4 st;
			vec4 sp;
			dom_lookup((MODEL_MATRIX * vec4(p, 1.0)).xyz, st, sp);
			float absorb = 1.0 - 0.91 * dot(sqrt(hair_color.rgb), vec3(0.2126, 0.7152, 0.0722));
			// Strands above this point, toward the snow: it lies on the top layer only.
			float above = -log(max(st[dom_snow_slot], 1e-6)) / max(absorb * dom_density, 1e-4);
			float held = snow_cover * exp(-2.0 * above);
			float r = snow_hash(key, 3.0);
			size = snow_grain * (0.4 + 0.6 * snow_cover) * (0.5 + snow_hash(key, 4.0)) * smoothstep(r - 0.05, r + 0.05, held);
			up = normalize(inverse(mat3(MODEL_MATRIX)) * dom_slot[dom_snow_slot * 4 + 3].xyz);
		}
	}
	// An irregular blob, a little flattened where it rests, on the strand's side the snow came from.
	vec3 sc = vec3(0.7 + 0.6 * snow_hash(key, 5.0), 0.7 + 0.6 * snow_hash(key, 6.0), 0.7 + 0.6 * snow_hash(key, 7.0));
	vec3 c = NORMAL * sc; // NORMAL: the blob's corner (VERTEX is its rest place)
	c -= up * dot(c, up) * 0.35;
	VERTEX = p + up * (0.4 * size) + c * (0.5 * size);
	NORMAL = normalize(VERTEX - p - up * (0.4 * size) + up * 1e-9);
}

void fragment() {
	// Snow scatters light many times between its crystals: bright, matte, no fibre highlight.
	ALBEDO = snow_albedo;
	ROUGHNESS = 0.9;
	SPECULAR = 0.3;
}
)";

} // namespace

// Hair cards (docs/ideas/hair.md, stage 2p): the card mesh drawn from the cards' own simulated
// strands (the first slots of hair_positions, sorted far to near so the blend runs back to front
// as the strands'). Each vertex is one side of one of a strip's points in a slot: CUSTOM0 = (texel,
// previous texel, next texel, side -1 or 1), CUSTOM1 = (point, strip, share along). The card in
// the slot (named by the previous positions' w) gives the rest from card_data: the strip's axis at
// rest (groom space) and half width, the other strip's half width, the card's direction at rest
// and the atlas tile. The strip's axis turns with the card's centre line. Opacity comes from the
// atlas's optical depth, blended as the strands are; shadow maps keep that share of their texels
// (stochastic transparency, Enderton et al. 2010). Raster only: under the path tracer the hair
// stays strands.
const char *HAIR_CARD_HEAD = R"(
shader_type spatial;
render_mode cull_disabled, rt_vertex_dynamic, hair_cards, alpha_motion_vectors, depth_prepass_alpha;
)";

const char *HAIR_CARD_FUNCS = R"(
uniform sampler2D card_atlas : filter_linear_mipmap, repeat_disable;
uniform sampler2D card_data : filter_nearest, repeat_disable;
uniform vec2 card_tile = vec2(32.0, 256.0); // a tile's size in the atlas, in texels
uniform float card_clump = 0.0; // wet clumping, as the strands'
// Thick strands (docs/ideas/hair.md, stage 3; Huang et al., EGSR 2025): each clump drawn as one
// camera-facing strip as wide as its cross-section's ellipse seen from the camera, its coverage
// from its strands' total width (no atlas). 0: crossed textured strips (cards).
uniform bool card_thick = false;
// vertex to fragment: x the clump's mean optical depth across the strip as drawn, y the share of
// its cross-section its fibres fill, z its mean optical depth at its own width.
varying vec3 thick_clump;
// The scalp as a sphere (centre, radius) fitted to the strands' roots, in the hair's space (w 0: none).
uniform vec4 card_scalp = vec4(0.0);
varying vec3 card_across; // view space: this strip's axis (raster lighting)
varying float card_side; // -1..1 across the strip
varying float card_tile_u; // the atlas u of this strip's tile centre
varying float card_grow; // how much wider than its clump the strip is drawn (to a pixel)
varying float card_fan; // how far the card's guides fanned apart since the groom (the card widens by it)

void vertex() {
	strand_seed = 0.0;
	dom_trans = vec4(1.0);
	dom_past = vec4(-1.0);
	strand_across = 0.0;
	strand_half_px = 1.0;
	strand_drawn_px = 1.0;
	vec4 here = hair_point(CUSTOM0.x);
	vec4 before = hair_point(CUSTOM0.y);
	vec4 after = hair_point(CUSTOM0.z);
	int ti = int(CUSTOM0.x + 0.5);
	ivec2 texel = ivec2(ti % texture_width, ti / texture_width);
	// The card drawn in this slot (-2 - card in the previous positions' w) and what it holds.
	int card = int(-texelFetch(hair_previous_positions, texel, 0).w - 2.0 + 0.5);
	int point = int(CUSTOM1.x + 0.5);
	int strip = int(CUSTOM1.y + 0.5);
	vec4 own = texelFetch(card_data, ivec2(point * 4 + strip, card), 0); // axis, the other's half width
	float own_half = texelFetch(card_data, ivec2(point * 4 + 1 - strip, card), 0).w;
	vec3 t0 = texelFetch(card_data, ivec2(point * 4 + 2, card), 0).xyz;
	vec4 tiles = texelFetch(card_data, ivec2(point * 4 + 3, card), 0);
	vec2 tile = strip == 0 ? tiles.xy : tiles.zw;
	UV = (tile + vec2(0.5 + (card_tile.x - 1.0) * (CUSTOM0.w * 0.5 + 0.5), 0.5 + (card_tile.y - 1.0) * CUSTOM1.z)) / vec2(textureSize(card_atlas, 0));
	card_side = CUSTOM0.w;
	card_tile_u = (tile.x + 0.5 * card_tile.x) / float(textureSize(card_atlas, 0).x);
	// w: 4 + along + 8 * fan (64ths), the solver's encoding for a card's strand (this frame's: the
	// previous positions' w names the card).
	float w = texelFetch(hair_positions, texel, 0).w;
	float fan_q = floor((w - 4.0) / 8.0);
	float along = clamp(w - 4.0 - 8.0 * fan_q, 0.0, 1.0);
	card_fan = clamp(fan_q / 64.0, 0.5, 4.0);
	vec3 p = here.xyz;
	vec3 t = after.xyz - before.xyz;
	t = dot(t, t) > 1e-14 ? normalize(t) : t0;
	// The strip's axis at rest turned as the card's centre line turned (rotation from t0 to t).
	vec3 ax = own.xyz;
	vec3 k = cross(t0, t);
	float s = length(k);
	if (s > 1e-6) {
		k /= s;
		float c = dot(t0, t);
		ax = ax * c + cross(k, ax) * s + k * dot(k, ax) * (1.0 - c);
	}
	ax -= t * dot(ax, t);
	ax = dot(ax, ax) > 1e-12 ? normalize(ax) : normalize(cross(t, abs(t.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
	// The clump fans out (or closes) with its guides: the card as much wider, and thinner below.
	float half_width = own_half * card_fan * (1.0 - card_clump * smoothstep(0.0, 0.3, along));
	float drawn = half_width;
	card_grow = 1.0;
	if (!IN_PATH_TRACER) {
		// A card under a pixel wide falls between the pixels' centres, and the hair behind it
		// shows: as the strands (TressFX), it is drawn at least 0.71 px a side (capped as theirs,
		// 2 mm in shadow maps and 1 cm otherwise), and its optical depth thinned to match.
		vec4 clip = PROJECTION_MATRIX * (MODELVIEW_MATRIX * vec4(p, 1.0));
		float px = 2.0 * clip.w / max(abs(PROJECTION_MATRIX[1][1]) * VIEWPORT_SIZE.y, 1e-6);
		drawn = max(half_width, min(0.71 * px, IN_SHADOW_PASS ? 0.002 : 0.01));
		card_grow = drawn / max(half_width, 1e-7);
		card_fan *= card_grow;
	}
	thick_clump = vec3(0.0);
	if (card_thick) {
		// The clump's ellipse (half widths along this strip's axis and the other's, fanned and
		// thinned as the strip is) seen from the camera: its half extent across the view, e =
		// t x eye. Strip 1 collapses: one strip a clump.
		float scale = half_width / max(own_half, 1e-7);
		vec3 bx = normalize(cross(t, ax));
		vec3 pv = (MODELVIEW_MATRIX * vec4(p, 1.0)).xyz;
		vec3 eye = PROJECTION_MATRIX[3][3] > 0.5 ? vec3(0.0, 0.0, -1.0) : normalize(pv);
		vec3 e = cross(normalize(mat3(MODELVIEW_MATRIX) * t), eye);
		e = dot(e, e) > 1e-12 ? normalize(transpose(mat3(MODELVIEW_MATRIX)) * e) : ax;
		vec3 a_axis = strip == 0 ? ax : bx;
		vec3 b_axis = strip == 0 ? bx : ax;
		float half_a = (strip == 0 ? own_half : own.w) * scale;
		float half_b = (strip == 0 ? own.w : own_half) * scale;
		// At least the clump's strands packed tight (N fibres in a circle sqrt(N) fibres across):
		// a clump's end or a padding point has no width of its own, and its depth went unbounded.
		float strands = texelFetch(card_data, ivec2(point * 4 + 2, card), 0).w;
		float fibre = mix(root_width, tip_width, along);
		float extent = max(sqrt(pow(half_a * dot(a_axis, e), 2.0) + pow(half_b * dot(b_axis, e), 2.0)), 0.5 * sqrt(max(strands, 1.0)) * fibre);
		float drawn_thick = extent;
		if (!IN_PATH_TRACER) {
			vec4 clip = PROJECTION_MATRIX * vec4(pv, 1.0);
			float px = 2.0 * clip.w / max(abs(PROJECTION_MATRIX[1][1]) * VIEWPORT_SIZE.y, 1e-6);
			drawn_thick = max(extent, min(0.71 * px, IN_SHADOW_PASS ? 0.002 : 0.01));
		}
		// The clump's strands side by side over the drawn width: its mean optical depth.
		thick_clump.x = strands * fibre / max(2.0 * drawn_thick, 1e-7);
		thick_clump.y = clamp(strands * 0.25 * fibre * fibre / max(half_a * half_b, 0.25 * strands * fibre * fibre), 0.0, 1.0);
		thick_clump.z = strands * fibre / max(2.0 * extent, 1e-7);
		ax = e;
		drawn = strip == 0 ? drawn_thick : 0.0;
		card_grow = 1.0;
		card_fan = 1.0;
	}
	VERTEX = p + ax * (drawn * CUSTOM0.w);
	if (card_scalp.w > 0.0) {
		// The card stands for a slab of strands, as wide as one strip and as thick as the other,
		// centred on its simulated strand, which the collision keeps only just off the scalp: the
		// slab's inner half, and a flat strip's edges over the round scalp (the sagitta,
		// half_width^2 / (2 R)), sank into the head, and the scalp showed between the cards at the
		// crown, where each strand on its own lies on it. Near the scalp (a sphere fitted to the
		// roots) the card is raised until the slab clears it; elsewhere it stays put.
		vec3 out_dir = p - card_scalp.xyz;
		float r = length(out_dir);
		if (r > 1e-5) {
			out_dir /= r;
			vec3 bx = normalize(cross(t, ax));
			float a_out = dot(ax, out_dir);
			float other = own.w * half_width / max(own_half, 1e-6);
			float support = half_width * abs(a_out) + other * abs(dot(bx, out_dir));
			float sag = half_width * half_width * (1.0 - a_out * a_out) / (2.0 * max(r, 0.5 * card_scalp.w));
			VERTEX += out_dir * max(support + sag - (r - card_scalp.w), 0.0);
		}
	}
	NORMAL = normalize(cross(ax, t));
	TANGENT = ax;
	BINORMAL = t;
	strand_tangent = normalize(mat3(MODELVIEW_MATRIX) * t);
	card_across = normalize(mat3(MODELVIEW_MATRIX) * ax);
	if (dom_maps > 0 && !IN_PATH_TRACER && !IN_SHADOW_PASS && !PREVIOUS_FRAME) {
		// The deep opacity maps, made from the cards' own strands (each as wide as its clump), read
		// on the card's centre line: the strip's edges reach out of the map's hair sideways, in
		// front of the hair toward the light, and read as lit where the clump's strands are not.
		vec4 lt;
		vec4 lp;
		dom_lookup((MODEL_MATRIX * vec4(p, 1.0)).xyz, lt, lp);
		dom_trans = lt;
		dom_past = lp;
	}
}

void fragment() {
	vec4 texel = texture(card_atlas, UV);
	// The strands' own direction: the card's, tilted across the strip as the baked strands run.
	float tilt = card_thick ? 0.0 : texel.g * 2.0 - 1.0;
	light_tangent = normalize(strand_tangent + card_across * (tilt / max(sqrt(1.0 - tilt * tilt), 0.05)));
	vec3 sigma;
	vec3 mass;
	fibre_look(-1.0, UV.y, sigma, mass); // a clump: its grey share mixed in
	ALBEDO = mass;
	ROUGHNESS = fibre_roughness.x;
	ANISOTROPY = 0.8;
	ANISOTROPY_FLOW = vec2(1.0, 0.0); // as the strands: the highlight runs across them
	HAIR = 1.0;
	HAIR_ABSORPTION = sigma;
	HAIR_ROUGHNESS = fibre_roughness;
	// Snow on the cards (the strands' snow clumps are not drawn with them): from the snow's deep
	// opacity map, as the clumps, the hair's top layer holds it; each texel of the atlas is snow or
	// fibre by its own draw, a clump's 2.5 mm being well under a pixel at card distances. The
	// clumps (one a strand where the snow holds, about 3 strands a cm^2 on top, 2.5 mm across)
	// cover about 15% of the top layer.
	hair_snow = 0.0;
	if (snow_cover > 0.0 && dom_snow_slot >= 0 && dom_snow_slot < dom_maps && !IN_SHADOW_PASS) {
		float absorb = 1.0 - 0.91 * dot(sqrt(hair_color.rgb), vec3(0.2126, 0.7152, 0.0722));
		float above = -log(max(dom_trans[dom_snow_slot], 1e-6)) / max(absorb * dom_density, 1e-4);
		float held = snow_cover * exp(-2.0 * above);
		vec2 texel = floor(UV * vec2(textureSize(card_atlas, 0)));
		float r = fract(sin(dot(vec3(texel, card_tile_u * 997.0), vec3(12.9898, 78.233, 37.719))) * 43758.5453);
		if (r < 0.15 * held) {
			hair_snow = 1.0;
			ALBEDO = vec3(0.85);
			ROUGHNESS = 0.9;
			NORMAL = normalize(mat3(VIEW_MATRIX) * dom_slot[dom_snow_slot * 4 + 3].xyz);
		}
	}
	dom_world = (INV_VIEW_MATRIX * vec4(VERTEX, 1.0)).xyz;
	if (dom_maps > 0 && !IN_PATH_TRACER && !IN_SHADOW_PASS) {
		// As the strands: the shadow map lookups skip the hair's own layers (the maps carry them).
		float local_past = -1.0;
		for (int k = 0; k < 4; k++) {
			if (k >= dom_maps) {
				break;
			}
			if (k == dom_snow_slot) {
				continue; // the snow's map: no light
			}
			if (dom_slot[k * 4 + 1].w > 0.5) {
				local_past = max(local_past, dom_past[k]);
			} else if (dom_past[k] >= 0.0) {
				DIRECTIONAL_SHADOW_OFFSET = dom_past[k] + 0.005;
			}
		}
		if (local_past >= 0.0) {
			POSITIONAL_SHADOW_OFFSET = local_past + 0.005;
		}
	}
	// The two crossed strips hold the same hair seen two ways: each counts by how squarely it
	// faces the view (f^2 / (f^2 + o^2), the two summing to one), as optical depth. A strip seen
	// at an angle packs its strands into less of the screen: per pixel their depth is the
	// strands' projected area (width x length x sin a, a the view's angle to the strands) over
	// the strip's (its width x length x |f|), so it grows by sin a / |f|. Weighted, that is
	// |f| / sqrt(f^2 + o^2) of the atlas's depth. Left out, the hair seen obliquely lost up to
	// a third of its opacity.
	float f = dot(NORMAL, VIEW);
	float o = dot(TANGENT, VIEW);
	float facing = abs(f) / max(sqrt(f * f + o * o), 1e-3);
	// Fanned out (or widened to a pixel), the same strands spread over a wider card: optical
	// depth over the fan.
	// The atlas holds the strands' optical depth (16 bits, R high and B low, over 8).
	float depth = (texel.r * 65280.0 + texel.b * 255.0) * (8.0 / 65535.0);
	if (card_thick) {
		// Thick strands: the chord through the clump's ellipse, sqrt(1 - x^2) across the strip,
		// scaled to the clump's mean (4 / pi); the strip faces the camera.
		depth = thick_clump.x * (4.0 / PI) * sqrt(max(1.0 - card_side * card_side, 0.0));
		facing = 1.0;
	}
	float coverage = 1.0 - exp(-depth * facing / max(card_fan, 0.5));
	// The strip's edges antialiased: a pixel whose centre lies within half a pixel of the edge
	// takes the share of it the strip covers, so a strip a few pixels wide is not a stepped band.
	// Under about three pixels the strip already stands for its strands by its width (vertex()
	// widens it to a pixel and thins it to match), and fading its edges would lose that opacity.
	float edge = (1.0 - abs(card_side)) / max(fwidth(card_side), 1e-6);
	float strip_px = 2.0 / max(fwidth(card_side), 1e-6);
	if (!IN_SHADOW_PASS) {
		coverage *= mix(1.0, clamp(edge + 0.5, 0.0, 1.0), smoothstep(2.0, 4.0, strip_px));
	}
	// Lit as the strands are: a fibre has no surface normal, so the ambient and reflected light
	// take the strands' own ribbon frame, rebuilt here about the card's strand direction: across
	// = T x eye (eye from the camera to the point, view space), normal = across x T, which faces
	// away from the viewer. Facing the viewer instead, the cards took a full reflection of the sky
	// that the strands' frame leaves out (its horizon term), and read about a fifth lighter.
	vec3 eye = normalize(VERTEX);
	vec3 across = cross(light_tangent, eye);
	across = dot(across, across) > 1e-8 ? normalize(across) : card_across;
	NORMAL = normalize(cross(across, light_tangent));
	BINORMAL = light_tangent;
	TANGENT = across;
#ifdef HAIR_SCATTERING
	if (IN_SHADOW_PASS && !IN_PATH_TRACER) {
		// As the strands: what the fibres pass straight on (TT) is not blocked. The strands each
		// block that share of their own coverage, so behind n of them the light is
		// exp(-share * total coverage); a card's coverage is already the clump's combined
		// 1 - exp(-total), so the share applies to its optical depth, not to it (as a factor it
		// let much more light through a dense clump than its strands do).
		float tt_block = 1.0 - 0.91 * dot(sqrt(mass), vec3(0.2126, 0.7152, 0.0722));
		coverage = 1.0 - pow(max(1.0 - coverage, 1e-4), tt_block);
	}
#endif
	// Two passes, as hair cards have been drawn since Scheuermann (GDC 2004): the cards' dense
	// cores (80% or more) write depth first, then everything blends far to near over them. The
	// cards are sorted whole, by their middle, but each runs the length of the hair, so a card
	// behind could still draw over the front of another: a lit card from beside the head showed
	// through the shadowed hair in front of it, and the head's outline read through the cards.
	// Which fragments are core is read from the strip's mean across (its tile at the mip one texel
	// wide), not from the texel itself: a strip a pixel or two wide otherwise passed and failed
	// along its length, and the hair behind showed through in rows of dots.
	vec4 row = textureLod(card_atlas, vec2(card_tile_u, UV.y), log2(card_tile.x));
	float row_depth = card_thick ? thick_clump.x : (row.r * 65280.0 + row.b * 255.0) * (8.0 / 65535.0);
	// (Before the widening to a pixel: the clump's own core.)
	float row_cover = 1.0 - exp(-row_depth * facing / max(card_fan / card_grow, 0.5));
	OPAQUE_PREPASS_THRESHOLD = row_cover >= 0.5 && edge >= 0.5 ? 0.0 : 2.0;
	// The clump's own shadow: the deep opacity maps are read on the card's centre line, so the
	// card is lit as its clump's middle strand, while the clump's strands shade one another. Its
	// mean transmittance through its own optical depth k, (1 - e^-k) / k, scales the light.
	{
		float absorb = 1.0 - 0.91 * dot(sqrt(hair_color.rgb), vec3(0.2126, 0.7152, 0.0722));
		float k = absorb * dom_density * row_depth;
		hair_clump.x = k > 1e-4 ? (1.0 - exp(-k)) / k : 1.0;
		hair_clump.y = 0.0;
		if (card_thick) {
			// Thick strands (Huang et al. 2025, Eq. 7): the lobes dimmed by the fibres the light
			// crosses inside the clump, a_F^(n - 1), n - 1 the chord's optical depth here.
			// a_F: what a fibre passes on forward (TT through its colour, Fresnel in and out).
			float a_f = 0.9091 * dot(sqrt(mass), vec3(0.2126, 0.7152, 0.0722));
			// The fibres the light crosses do not change when the strip is widened to a pixel:
			// counted at the clump's own width.
			float own_depth = thick_clump.z * (4.0 / PI) * sqrt(max(1.0 - card_side * card_side, 0.0));
			// Averaged over the fibres along the chord (each behind 0 to n of the others):
			// (1 - a_F^n) / (n ln(1 / a_F)).
			float kl = own_depth * absorb * dom_density;
			hair_clump.x = kl > 1e-4 ? (1.0 - exp(-kl)) / kl : 1.0;
			hair_clump.y = max(thick_clump.y, 1e-4);
		}
	}
	if (IN_SHADOW_PASS && !IN_PATH_TRACER) {
		float noise = fract(52.9829189 * fract(dot(FRAGCOORD.xy, vec2(0.06711056, 0.00583715))) + texel.a);
		if (coverage <= noise) {
			discard;
		}
		coverage = 1.0;
	}
	ALPHA = IN_SHADOW_PASS ? coverage : coverage * lod_fade;
}
)";

Ref<Shader> PhysXHair3D::render_shaders[2];
Ref<Shader> PhysXHair3D::card_shaders[2];
Ref<Shader> PhysXHair3D::snow_shader;
Ref<Shader> PhysXHair3D::shadow_shaders[2];

void PhysXHair3D::free_shared() {
	render_shaders[0].unref();
	render_shaders[1].unref();
	card_shaders[0].unref();
	card_shaders[1].unref();
	snow_shader.unref();
	shadow_shaders[0].unref();
	shadow_shaders[1].unref();
	HairCardBake::clear_cache();
	HairStrandSolverGPU::free_shared();
}

// Melanin's absorption per fibre diameter, as BaseMaterial3D's Hair (Cycles' eumelanin and
// pheomelanin coefficients).
Vector3 PhysXHair3D::_fibre_absorption() const {
	const float m = -Math::log(MAX(1.0f - melanin, 1e-4f));
	const Vector3 eu(0.506f, 0.841f, 1.653f);
	const Vector3 pheo(0.343f, 0.733f, 1.924f);
	// Wet hair is darker (Lekner and Dorf 1988): water between the fibres nearly matches their
	// index (1.33 against 1.55), so each fibre reflects less back out and light goes deeper into
	// the hair, crossing more fibres before it leaves, and more of it is absorbed. Taken as the
	// fibres absorbing up to half again as much when soaked.
	return (eu * (1.0f - melanin_redness) + pheo * melanin_redness) * m * (1.0f + 0.5f * wet_look);
}

// The fibre roughness, longitudinal and azimuthal. A wet fibre is smoother along its length: the
// water film fills in the cuticle scales and gives a near mirror surface, so the highlight is
// narrower and brighter (Ward et al. 2004; Gupta and Magnenat-Thalmann 2007, roughness
// x (1 - c w)), here halved when soaked.
Vector2 PhysXHair3D::_fibre_roughness() const {
	return Vector2(roughness * (1.0f - 0.5f * wet_look), radial_roughness);
}

// Rain wets the hair and the air dries it. Rain from the WeatherMap3D (mm/h) less what the
// canopy above catches (free throughfall, Gash 1979: the share of the sky the canopy leaves
// open) and nothing under a roof (a ray up from the top of the hair, every half second) fills
// water_capacity mm; drying takes the soaked hair to dry in drying_time at an even rate (the
// constant rate period of drying, while the surface stays wet). The look follows in steps of
// 1%, so the material is not set every frame.
void PhysXHair3D::_update_wetness(double p_delta) {
	const float dt = (float)p_delta;
#ifdef MODULE_WEATHER_ENABLED
	float rain = 0.0f;
	if (weather_wetting && dt > 0.0f && is_inside_tree()) {
		if (const WeatherMap3D *weather = WeatherMap3D::get_active()) {
			const Vector3 at = render_instance ? render_instance->get_global_transform().xform(render_instance->get_aabb()).get_center() : get_global_transform().origin;
			rain = weather->get_rain_at(at);
			if (rain > 0.0f) {
				rain *= 1.0f - CLAMP(weather->get_canopy_closure_at(at), 0.0f, 1.0f);
				rain_shelter_timer -= dt;
				if (rain_shelter_timer <= 0.0f) {
					rain_shelter_timer = 0.5f;
					rain_shelter = 1.0f;
					const Ref<World3D> w = get_world_3d();
					PhysicsDirectSpaceState3D *space = w.is_valid() ? PhysicsServer3D::get_singleton()->space_get_direct_state(w->get_space()) : nullptr;
					if (space) {
						Vector3 top = at;
						if (render_instance) {
							const AABB box = render_instance->get_global_transform().xform(render_instance->get_aabb());
							top.y = box.position.y + box.size.y;
						}
						PhysicsDirectSpaceState3D::RayParameters ray;
						ray.from = top + Vector3(0.0f, 0.1f, 0.0f);
						ray.to = ray.from + Vector3(0.0f, 100.0f, 0.0f);
						PhysicsDirectSpaceState3D::RayResult hit;
						if (space->intersect_ray(ray, hit)) {
							rain_shelter = 0.0f;
						}
					}
				}
				rain *= rain_shelter;
			}
		}
	}
	if (weather_wetting && dt > 0.0f) {
		// The precipitation splits into rain and snow by the air's temperature and humidity
		// (Jennings et al. 2018, as the weather map's own surfaces): only the rain wets at once.
		float air = 0.0f;
		float snowfall = 0.0f;
		if (const WeatherMap3D *weather = WeatherMap3D::get_active()) {
			air = weather->get_air_temperature();
			const float liquid = WeatherMap3D::rain_fraction(air, weather->get_air_humidity());
			snowfall = rain * (1.0f - liquid);
			rain *= liquid;
		}
		// Snow caught by the hair (Hedstrom and Pomeroy 1998's interception: the catch slows as the
		// load nears what the hair holds) melts by a degree-day rate above freezing (0.2 mm an hour a
		// degree, Hock 2003's range for snow) plus a little from the head's warmth; the melt wets it.
		const float cap = MAX(snow_capacity, 0.01f);
		float load = snow * cap;
		load += dt / 3600.0f * snowfall * (1.0f - load / cap);
		const float melt = MIN(load, dt / 3600.0f * (0.2f * MAX(air, 0.0f) + 0.05f));
		load -= melt;
		snow = CLAMP(load / cap, 0.0f, 1.0f);
		wetness += dt * rain / (3600.0f * MAX(water_capacity, 0.01f)) + melt / MAX(water_capacity, 0.01f);
		if (rain <= 0.0f && melt <= 0.0f) {
			wetness -= dt / MAX(drying_time, 1.0f);
		}
		wetness = CLAMP(wetness, 0.0f, 1.0f);
	}
#endif
	if (Math::abs(snow - snow_look) >= 0.01f || (snow != snow_look && (snow == 0.0f || snow == 1.0f))) {
		snow_look = snow;
		if (snow_material.is_valid()) {
			snow_material->set_shader_parameter("snow_cover", snow_look);
		}
		if (card_material.is_valid()) {
			card_material->set_shader_parameter("snow_cover", snow_look);
		}
	}
	if (Math::abs(wetness - wet_look) >= 0.01f || (wetness != wet_look && (wetness == 0.0f || wetness == 1.0f))) {
		wet_look = wetness;
		_set_look(copy_material);
		_set_look(card_material);
		_set_look(snow_material);
		if (card_material.is_valid()) {
			card_material->set_shader_parameter("card_clump", wet_look * wet_clumping);
		}
	}
}

// The colour a mass of such fibres shows (Chiang et al. 2016, section 4.2), for the rasterizer.
// The red melanin's share of _fibre_absorption().
Vector3 PhysXHair3D::_fibre_absorption_pheo() const {
	const float m = -Math::log(MAX(1.0f - melanin, 1e-4f));
	return Vector3(0.343f, 0.733f, 1.924f) * melanin_redness * m * (1.0f + 0.5f * wet_look);
}

// d'Eon et al. 2011's exponent from absorption to the mass colour, by the azimuthal roughness.
float PhysXHair3D::_mass_exponent() const {
	const float b = CLAMP(radial_roughness, 0.01f, 1.0f);
	return 5.969f - 0.215f * b + 2.532f * b * b - 10.73f * b * b * b + 5.574f * b * b * b * b + 0.245f * b * b * b * b * b;
}

Color PhysXHair3D::_mass_color() const {
	const float d = _mass_exponent();
	const Vector3 s = _fibre_absorption();
	return Color(Math::exp(-d * Math::sqrt(s.x)), Math::exp(-d * Math::sqrt(s.y)), Math::exp(-d * Math::sqrt(s.z))).linear_to_srgb();
}

// Per strand the points drawn (local indices, root and tip always): Douglas-Peucker on the groomed
// strand within simplify_tolerance times the root width. r_points: (first, count) per strand, then
// the indices. Returns the most points any strand draws (the render mesh's slot size).
int PhysXHair3D::_render_points(const PackedVector3Array &p_points, const PackedInt32Array &p_counts, PackedInt32Array &r_points) const {
	const int sc = p_counts.size();
	const float eps = MAX(simplify_tolerance, 0.0f) * root_width;
	LocalVector<int> list;
	r_points.resize(sc * 2);
	int *head = r_points.ptrw();
	LocalVector<bool> keep;
	LocalVector<Vector2i> stack;
	int most = 0;
	int start = 0;
	for (int s = 0; s < sc; s++) {
		const int n = MAX(p_counts[s], 0);
		head[s * 2] = list.size();
		keep.resize(n);
		for (int k = 0; k < n; k++) {
			keep[k] = eps <= 0.0f || k == 0 || k == n - 1;
		}
		if (eps > 0.0f && n > 2) {
			stack.clear();
			stack.push_back(Vector2i(0, n - 1));
			while (!stack.is_empty()) {
				const Vector2i r = stack[stack.size() - 1];
				stack.remove_at(stack.size() - 1);
				const Vector3 a = p_points[start + r.x];
				const Vector3 b = p_points[start + r.y];
				float worst = -1.0f;
				int at = -1;
				for (int k = r.x + 1; k < r.y; k++) {
					const float d = Geometry3D::get_closest_point_to_segment(p_points[start + k], a, b).distance_to(p_points[start + k]);
					if (d > worst) {
						worst = d;
						at = k;
					}
				}
				if (at >= 0 && worst > eps) {
					keep[at] = true;
					stack.push_back(Vector2i(r.x, at));
					stack.push_back(Vector2i(at, r.y));
				}
			}
		}
		int kept = 0;
		for (int k = 0; k < n; k++) {
			if (keep[k]) {
				list.push_back(k);
				kept++;
			}
		}
		head[s * 2 + 1] = kept;
		most = MAX(most, kept);
		start += n;
	}
	const int base = r_points.size();
	r_points.resize(base + list.size());
	int *w = r_points.ptrw();
	for (uint32_t i = 0; i < list.size(); i++) {
		w[base + i] = list[i];
	}
	return most;
}

// Guide strands and who follows them (HairWorks guide curves, TressFX follow hairs): a seeded
// random share of the strands simulates, and every other strand follows its three nearest guides
// by root, weighted by inverse squared distance. 8 floats per strand, see HairStrandSolver::build().
PackedFloat32Array PhysXHair3D::_follow_data(const PackedVector3Array &p_points, const PackedInt32Array &p_counts, int p_guide_pool) const {
	const int sc = p_counts.size();
	// Only the first p_guide_pool strands can be guides (the hair cards' strands, after them, follow).
	const int pool = p_guide_pool > 0 ? MIN(p_guide_pool, sc) : sc;
	PackedFloat32Array follow;
	follow.resize(sc * 8);
	float *f = follow.ptrw();
	memset(f, 0, sizeof(float) * sc * 8);

	LocalVector<Vector3> roots;
	roots.resize(sc);
	int start = 0;
	for (int i = 0; i < sc; i++) {
		roots[i] = p_points[start];
		start += p_counts[i];
	}
	const int guide_n = guide_ratio >= 1.0f ? pool : CLAMP((int)Math::ceil(pool * guide_ratio), 1, pool);
	LocalVector<int> order;
	order.resize(pool);
	for (int i = 0; i < pool; i++) {
		order[i] = i;
	}
	RandomPCG rng{ 0x6a1du };
	for (int i = pool - 1; i > 0; i--) {
		SWAP(order[i], order[rng.rand() % (uint32_t)(i + 1)]);
	}
	LocalVector<bool> guide;
	guide.resize(sc);
	for (int i = 0; i < sc; i++) {
		guide[i] = false;
	}
	AABB box;
	for (int i = 0; i < guide_n; i++) {
		const int g = order[i];
		guide[g] = true;
		f[g * 8 + 3] = -1.0f;
		if (i == 0) {
			box = AABB(roots[g], Vector3());
		} else {
			box.expand_to(roots[g]);
		}
	}
	if (guide_n == sc) {
		return follow;
	}

	// A grid of about one guide per cell, searched in growing shells.
	const float cell = MAX(box.get_longest_axis_size() / MAX(Math::pow((float)guide_n, 1.0f / 3.0f), 1.0f), 1e-4f);
	auto cell_of = [&](const Vector3 &p_p) {
		return Vector3i((int)Math::floor(p_p.x / cell), (int)Math::floor(p_p.y / cell), (int)Math::floor(p_p.z / cell));
	};
	HashMap<Vector3i, LocalVector<int>> grid;
	for (int i = 0; i < guide_n; i++) {
		grid[cell_of(roots[order[i]])].push_back(order[i]);
	}
	for (int s = 0; s < sc; s++) {
		if (guide[s]) {
			continue;
		}
		int best[3] = { -1, -1, -1 };
		float best_d[3] = { 1e30f, 1e30f, 1e30f };
		const Vector3i c = cell_of(roots[s]);
		for (int r = 0; r < 256; r++) {
			if (best[2] >= 0 && (r - 1) * cell > Math::sqrt(best_d[2])) {
				break;
			}
			for (int x = -r; x <= r; x++) {
				for (int y = -r; y <= r; y++) {
					for (int z = -r; z <= r; z++) {
						if (MAX(Math::abs(x), MAX(Math::abs(y), Math::abs(z))) != r) {
							continue;
						}
						const LocalVector<int> *list = grid.getptr(c + Vector3i(x, y, z));
						if (list == nullptr) {
							continue;
						}
						for (const int g : *list) {
							const float d = roots[s].distance_squared_to(roots[g]);
							for (int k = 0; k < 3; k++) {
								if (d < best_d[k]) {
									for (int j = 2; j > k; j--) {
										best[j] = best[j - 1];
										best_d[j] = best_d[j - 1];
									}
									best[k] = g;
									best_d[k] = d;
									break;
								}
							}
						}
					}
				}
			}
		}
		float wsum = 0.0f;
		float w[3] = { 0.0f, 0.0f, 0.0f };
		for (int k = 0; k < 3; k++) {
			if (best[k] >= 0) {
				w[k] = 1.0f / (best_d[k] + 1e-8f);
				wsum += w[k];
			}
		}
		for (int k = 0; k < 3; k++) {
			f[s * 8 + k] = best[k] >= 0 ? (float)best[k] : 0.0f;
			f[s * 8 + 4 + k] = wsum > 0.0f ? w[k] / wsum : 0.0f;
		}
	}
	return follow;
}

void PhysXHair3D::_make_material() {
	if (copy_material.is_valid()) {
		copy_material->set_shader_parameter("hair_positions", Variant());
		copy_material->set_shader_parameter("hair_previous_positions", Variant());
		copy_material->set_shader_parameter("hair_deep_shadow", Variant());
	}
	if (render_material.is_valid() && render_material->get_shader().is_valid()) {
		copy_material = render_material->duplicate();
	} else {
		Ref<Shader> &shader = render_shaders[hair_shading ? 1 : 0];
		if (shader.is_null()) {
			shader.instantiate();
			shader->set_code(String(HAIR_RENDER_HEAD) + (hair_shading ? "#define HAIR_SCATTERING\n" : "") + HAIR_RENDER_SHADER);
		}
		copy_material.instantiate();
		copy_material->set_shader(shader);
	}
	_set_look(copy_material);
	copy_material->set_shader_parameter("hair_positions", pos_texture);
	copy_material->set_shader_parameter("hair_previous_positions", prev_pos_texture);
	copy_material->set_shader_parameter("hair_deep_shadow", deep_shadow_texture);
	copy_material->set_shader_parameter("slot_points", draw_slot_points);
	copy_material->set_shader_parameter("dom_cells", HairStrandSolverGPU::DOM_CELLS);
	copy_material->set_shader_parameter("dom_layers", HairStrandSolver::deep_shadow_layers());
	deep_shadow_on = 0;
	copy_material->set_shader_parameter("dom_maps", 0);
	copy_material->set_shader_parameter("texture_width", solver ? solver->get_texture_width() : 1);
	copy_material->set_shader_parameter("root_width", root_width);
	copy_material->set_shader_parameter("tip_width", tip_width);
	copy_material->set_shader_parameter("lod_width_scale", lod_width_scale);
	if (render_instance) {
		render_instance->set_material_override(copy_material);
	}
	_make_shadow_copy();
	if (card_instance && card_bake.is_valid()) {
		Ref<Shader> &shader = card_shaders[hair_shading ? 1 : 0];
		if (shader.is_null()) {
			// The strand shader's uniforms, varyings and light(), with the cards' own vertex() and fragment().
			const String strand = HAIR_RENDER_SHADER;
			const int body = strand.find("void vertex() {");
			const int light = strand.find("#ifdef HAIR_SCATTERING\n// Unreal Engine's hair shading");
			shader.instantiate();
			shader->set_code(String(HAIR_CARD_HEAD) + (hair_shading ? "#define HAIR_SCATTERING\n" : "") + strand.substr(0, body) + HAIR_CARD_FUNCS + (light >= 0 ? strand.substr(light) : String()));
		}
		if (card_material.is_null()) {
			card_material.instantiate();
		}
		card_material->set_shader(shader);
		if (card_bake->texture.is_null()) {
			card_bake->texture = ImageTexture::create_from_image(card_bake->atlas);
		}
		card_material->set_shader_parameter("card_atlas", card_bake->texture);
		card_material->set_shader_parameter("card_data", card_data);
		card_material->set_shader_parameter("card_tile", Vector2(card_bake->tiles[2], card_bake->tiles[3]));
		card_material->set_shader_parameter("hair_positions", pos_texture);
		card_material->set_shader_parameter("hair_previous_positions", prev_pos_texture);
		card_material->set_shader_parameter("slot_points", draw_slot_points);
		card_material->set_shader_parameter("texture_width", solver ? solver->get_texture_width() : 1);
		card_material->set_shader_parameter("hair_deep_shadow", deep_shadow_texture);
		card_material->set_shader_parameter("dom_cells", HairStrandSolverGPU::DOM_CELLS);
		card_material->set_shader_parameter("dom_layers", HairStrandSolver::deep_shadow_layers());
		card_material->set_shader_parameter("dom_maps", 0);
		card_material->set_shader_parameter("card_clump", wet_look * wet_clumping);
		card_material->set_shader_parameter("card_scalp", card_scalp);
		card_material->set_shader_parameter("root_width", root_width);
		card_material->set_shader_parameter("tip_width", tip_width);
		card_material->set_shader_parameter("card_thick", thick_strands);
		card_material->set_shader_parameter("snow_cover", snow_look);
		_set_look(card_material);
		card_instance->set_material_override(card_material);
	}
	if (snow_instance) {
		if (snow_shader.is_null()) {
			// The strand shader's uniforms, varyings and deep opacity lookup, with the snow's own
			// vertex() and fragment() (Godot's standard lighting: snow is a diffuse grain).
			const String strand = HAIR_RENDER_SHADER;
			snow_shader.instantiate();
			snow_shader->set_code(String(HAIR_SNOW_HEAD) + strand.substr(0, strand.find("void vertex() {")) + HAIR_SNOW_FUNCS);
		}
		if (snow_material.is_null()) {
			snow_material.instantiate();
		}
		snow_material->set_shader(snow_shader);
		snow_material->set_shader_parameter("hair_positions", pos_texture);
		snow_material->set_shader_parameter("hair_previous_positions", prev_pos_texture);
		snow_material->set_shader_parameter("texture_width", solver ? solver->get_texture_width() : 1);
		snow_material->set_shader_parameter("hair_deep_shadow", deep_shadow_texture);
		snow_material->set_shader_parameter("dom_cells", HairStrandSolverGPU::DOM_CELLS);
		snow_material->set_shader_parameter("dom_layers", HairStrandSolver::deep_shadow_layers());
		snow_material->set_shader_parameter("dom_maps", 0);
		snow_material->set_shader_parameter("snow_cover", snow_look);
		_set_look(snow_material);
		snow_instance->set_material_override(snow_material);
	}
}

// The fibres' colour and optics (melanin, roughness, wetness) on a hair material.
void PhysXHair3D::_set_look(const Ref<ShaderMaterial> &p_material) const {
	if (p_material.is_null()) {
		return;
	}
	p_material->set_shader_parameter("hair_color", _mass_color());
	p_material->set_shader_parameter("fibre_absorption", _fibre_absorption());
	p_material->set_shader_parameter("fibre_roughness", _fibre_roughness());
	p_material->set_shader_parameter("fibre_absorption_pheo", _fibre_absorption_pheo());
	p_material->set_shader_parameter("mass_d", _mass_exponent());
	p_material->set_shader_parameter("grey_ratio", grey_ratio);
	p_material->set_shader_parameter("sun_bleach", sun_bleaching);
}

void PhysXHair3D::_groom_changed() {
	_queue_build();
}

void PhysXHair3D::_queue_build() {
	if (is_inside_tree()) {
		build_pending = true;
	}
}

void PhysXHair3D::_clear() {
	if (shadow_instance) {
		shadow_instance->queue_free();
		shadow_instance = nullptr;
	}
	if (shadow_material.is_valid()) {
		shadow_material->set_shader_parameter("hair_positions", Variant());
		shadow_material->set_shader_parameter("hair_previous_positions", Variant());
		shadow_material->set_shader_parameter("hair_deep_shadow", Variant());
		shadow_material.unref();
	}
	if (render_instance) {
		render_instance->queue_free();
		render_instance = nullptr;
	}
	if (card_instance) {
		card_instance->queue_free();
		card_instance = nullptr;
	}
	if (card_material.is_valid()) {
		card_material->set_shader_parameter("hair_positions", Variant());
		card_material->set_shader_parameter("hair_previous_positions", Variant());
		card_material.unref();
	}
	card_bake.unref();
	if (snow_instance) {
		snow_instance->queue_free();
		snow_instance = nullptr;
	}
	if (snow_material.is_valid()) {
		snow_material->set_shader_parameter("hair_positions", Variant());
		snow_material->set_shader_parameter("hair_previous_positions", Variant());
		snow_material->set_shader_parameter("hair_deep_shadow", Variant());
		snow_material.unref();
	}
	snow_slot_set = -2;
	card_first = 0;
	card_mode = false;
	card_target = false;
	card_fade = 0.0f;
	card_fade_set = -1.0f;
	if (copy_material.is_valid()) {
		copy_material->set_shader_parameter("hair_positions", Variant());
		copy_material->set_shader_parameter("hair_previous_positions", Variant());
		copy_material->set_shader_parameter("hair_deep_shadow", Variant());
		copy_material.unref();
	}
	if (pos_texture.is_valid()) {
		pos_texture->set_texture_rd_rid(RID());
		pos_texture.unref();
	}
	if (prev_pos_texture.is_valid()) {
		prev_pos_texture->set_texture_rd_rid(RID());
		prev_pos_texture.unref();
	}
	if (deep_shadow_texture.is_valid()) {
		deep_shadow_texture->set_texture_rd_rid(RID());
		deep_shadow_texture.unref();
	}
	if (solver != nullptr) {
		memdelete(solver);
		solver = nullptr;
	}
	built = false;
}

void PhysXHair3D::_build() {
	build_pending = false;
	_clear();
	if (groom.is_null() || !groom->is_valid() || groom->get_point_count() == 0) {
		return;
	}
	const bool editor = Engine::get_singleton()->is_editor_hint();

	render_instance = memnew(MeshInstance3D);
	add_child(render_instance, false, INTERNAL_MODE_BACK);

	if (editor) {
		// The groom, still: a HairMesh with BaseMaterial3D's strand hair.
		Ref<HairMesh> preview;
		preview.instantiate();
		preview->set_groom(groom);
		preview->set_root_width(root_width);
		preview->set_tip_width(tip_width);
		Ref<StandardMaterial3D> m;
		m.instantiate();
		m->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
		m->set_feature(BaseMaterial3D::FEATURE_HAIR, true);
		m->set_hair_color_mode(BaseMaterial3D::HAIR_COLOR_MELANIN);
		m->set_hair_melanin(melanin);
		m->set_hair_melanin_redness(melanin_redness);
		m->set_hair_roughness(roughness);
		m->set_hair_radial_roughness(radial_roughness);
		m->set_hair_direction(BaseMaterial3D::HAIR_DIRECTION_STRANDS);
		preview->set_material(m);
		render_instance->set_mesh(preview);
		built = true;
		return;
	}

	PackedVector3Array pts = groom->get_points();
	PackedInt32Array counts = groom->get_strand_point_counts();
	const int groom_strands = counts.size();

	// The points the strands are drawn through: where a strand runs straight its middle points are
	// left out of the render mesh (Douglas-Peucker on the groomed strand, as Unreal's groom LOD
	// decimation), within simplify_tolerance of the root width, below what a strand's own width
	// shows. Every point still simulates and casts into the deep shadows.
	PackedInt32Array render_points;
	int slot_points = _render_points(pts, counts, render_points);
	const int drawn_points = slot_points; // the strands' drawn points (the slots may hold more)

	// Hair cards: each card's centre line is one more strand, after the groom's, simulated as a
	// follower of its nearest guides and drawn (from the first slots of the output) by the cards.
	// The cards take the groomed points, not the drawn ones: a straight strand simplifies to its two
	// ends, and a card through only those cuts through the head as the strand drapes around it.
	int groom_points = 0;
	for (int i = 0; i < groom_strands; i++) {
		groom_points = MAX(groom_points, counts[i]);
	}
	if (cards_enabled && groom_points >= 2) {
		const int samples = CLAMP(groom_points, 2, 16);
		uint32_t h = hash_murmur3_one_64((uint64_t)pts.size());
		h = hash_murmur3_one_32((uint32_t)(card_count * 64 + samples), h);
		h = hash_murmur3_one_float(root_width, h);
		h = hash_murmur3_one_float(tip_width, h);
		h = hash_murmur3_one_float(pts[0].x, h);
		h = hash_murmur3_one_float(pts[pts.size() - 1].y, h);
		const uint64_t key = ((uint64_t)groom->get_instance_id()) ^ ((uint64_t)h << 32) ^ (uint64_t)h;
		card_bake = HairCardBake::get_cached(key, pts, counts, card_count, samples, root_width, tip_width);
	}
	if (card_bake.is_valid() && card_bake->card_count > 0) {
		card_scalp = _fit_scalp(pts, counts);
		card_first = groom_strands;
		const int cs = card_bake->samples;
		slot_points = MAX(slot_points, cs); // each card strand's slot holds all its points
		const int base = pts.size();
		pts.append_array(card_bake->points);
		// The drawn points: every card point (the header grows by the cards' entries).
		PackedInt32Array rp;
		const int total = groom_strands + card_bake->card_count;
		const int old_list = render_points.size() - groom_strands * 2;
		rp.resize(total * 2 + old_list + card_bake->card_count * cs);
		int *w = rp.ptrw();
		for (int i = 0; i < groom_strands * 2; i++) {
			w[i] = render_points[i];
		}
		for (int i = 0; i < old_list; i++) {
			w[total * 2 + i] = render_points[groom_strands * 2 + i];
		}
		for (int c = 0; c < card_bake->card_count; c++) {
			counts.push_back(cs);
			w[(groom_strands + c) * 2] = old_list + c * cs;
			w[(groom_strands + c) * 2 + 1] = cs;
			for (int k = 0; k < cs; k++) {
				w[total * 2 + old_list + c * cs + k] = k;
			}
		}
		render_points = rp;
		(void)base;
	}

	// Solver input: rest points with their segment lengths, strand ranges.
	PackedFloat32Array rest;
	rest.resize(pts.size() * 4);
	PackedInt32Array strands;
	strands.resize(counts.size() * 2);
	{
		float *r = rest.ptrw();
		int *s = strands.ptrw();
		int start = 0;
		for (int i = 0; i < counts.size(); i++) {
			const int n = counts[i];
			s[i * 2] = start;
			s[i * 2 + 1] = n;
			for (int k = 0; k < n; k++) {
				const Vector3 p = pts[start + k];
				r[(start + k) * 4 + 0] = p.x;
				r[(start + k) * 4 + 1] = p.y;
				r[(start + k) * 4 + 2] = p.z;
				r[(start + k) * 4 + 3] = k > 0 ? p.distance_to(pts[start + k - 1]) : 0.0f;
			}
			if (card_first > 0 && i >= card_first && n > 0) {
				// A card's strand: the first point's w (unused) holds half the clump's thickness,
				// the other strip's half width, which the solver keeps off the body.
				const HairCardBake &b = **card_bake;
				const int c = i - card_first;
				float thick = 0.0f;
				for (int k = 0; k < b.samples; k++) {
					thick += b.half_width[1][c * b.samples + k];
				}
				r[start * 4 + 3] = thick / float(MAX(b.samples, 1));
			}
			start += n;
		}
	}

	// Render mesh: two crossed strips per drawn point, as HairMesh, the positions rebuilt in
	// vertex() from the solver's output, which holds the strands sorted far to near from the camera
	// (for blending): slot_points texels per strand, padded past a short strand's end.
	PackedVector3Array verts;
	PackedVector3Array normals;
	PackedFloat32Array tangents;
	PackedVector2Array uvs;
	PackedFloat32Array custom0;
	PackedFloat32Array custom1;
	PackedInt32Array indices;
	AABB bounds(pts[0], Vector3());
	float longest = 0.0f;
	{
		int start = 0;
		for (int c = 0; c < counts.size(); c++) {
			const int n = counts[c];
			float length = 0.0f;
			for (int k = 0; k < n; k++) {
				bounds.expand_to(pts[start + k]);
				if (k > 0) {
					length += pts[start + k].distance_to(pts[start + k - 1]);
				}
			}
			longest = MAX(longest, length);
			start += MAX(n, 0);
		}
	}
	const int strand_count = counts.size();
	if (drawn_points >= 2) {
		verts.resize(strand_count * drawn_points * 4);
		uvs.resize(verts.size());
		normals.resize(verts.size());
		tangents.resize(verts.size() * 4);
		Vector3 *nr = normals.ptrw();
		float *tg = tangents.ptrw();
		custom0.resize(verts.size() * 4);
		custom1.resize(verts.size() * 4);
		Vector3 *v = verts.ptrw();
		Vector2 *uv = uvs.ptrw();
		float *c0 = custom0.ptrw();
		float *c1 = custom1.ptrw();
		int start = 0;
		for (int slot = 0; slot < strand_count; slot++) {
			const int n = MAX(counts[slot], 1);
			for (int k = 0; k < drawn_points; k++) {
				const int t = slot * slot_points + k; // texel: the solver's slot stride
				const int tm = slot * drawn_points + k; // vertex group
				// Rest position: the groom in groom order (vertex() replaces it).
				const Vector3 p = pts[MIN(start + MIN(k, n - 1), pts.size() - 1)];
				for (int strip = 0; strip < 2; strip++) {
					for (int side = 0; side < 2; side++) {
						const int vi = tm * 4 + strip * 2 + side;
						v[vi] = p;
						nr[vi] = Vector3(0, 0, 1);
						tg[vi * 4 + 0] = 1.0f;
						tg[vi * 4 + 1] = 0.0f;
						tg[vi * 4 + 2] = 0.0f;
						tg[vi * 4 + 3] = 1.0f;
						uv[vi] = Vector2(float(side), float(k) / float(MAX(drawn_points - 1, 1)));
						c0[vi * 4 + 0] = float(t);
						c0[vi * 4 + 1] = float(slot * slot_points + MAX(k - 1, 0));
						c0[vi * 4 + 2] = float(slot * slot_points + MIN(k + 1, drawn_points - 1));
						c0[vi * 4 + 3] = side == 0 ? -1.0f : 1.0f;
						c1[vi * 4 + 0] = 0.0f;
						c1[vi * 4 + 1] = 0.0f;
						c1[vi * 4 + 2] = 0.0f;
						c1[vi * 4 + 3] = float(strip);
					}
				}
			}
			for (int k = 0; k + 1 < drawn_points; k++) {
				for (int strip = 0; strip < 2; strip++) {
					const int v0 = (slot * drawn_points + k) * 4 + strip * 2;
					const int v2 = v0 + 4;
					indices.push_back(v0);
					indices.push_back(v0 + 1);
					indices.push_back(v2);
					indices.push_back(v0 + 1);
					indices.push_back(v2 + 1);
					indices.push_back(v2);
				}
			}
			start += MAX(counts[slot], 0);
		}
	}
	if (verts.is_empty()) {
		render_instance->queue_free();
		render_instance = nullptr;
		return;
	}

	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = verts;
	arrays[Mesh::ARRAY_NORMAL] = normals;
	arrays[Mesh::ARRAY_TANGENT] = tangents;
	arrays[Mesh::ARRAY_TEX_UV] = uvs;
	arrays[Mesh::ARRAY_CUSTOM0] = custom0;
	arrays[Mesh::ARRAY_CUSTOM1] = custom1;
	arrays[Mesh::ARRAY_INDEX] = indices;
	const uint64_t format = ((uint64_t)Mesh::ARRAY_CUSTOM_RGBA_FLOAT << Mesh::ARRAY_FORMAT_CUSTOM0_SHIFT) |
			((uint64_t)Mesh::ARRAY_CUSTOM_RGBA_FLOAT << Mesh::ARRAY_FORMAT_CUSTOM1_SHIFT);
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays, Array(), Dictionary(), format);
	render_instance->set_mesh(mesh);
	// Strands swing up to their length away from where they were groomed.
	render_instance->set_custom_aabb(bounds.grow(longest + 0.05f));

	solver = memnew(HairStrandSolver);
	if (!solver->is_available()) {
		memdelete(solver);
		solver = nullptr;
		render_instance->queue_free();
		render_instance = nullptr;
		ERR_FAIL_MSG("PhysXHair3D: no RenderingDevice, the hair can't be simulated.");
	}
	solver->build(rest, strands, _follow_data(pts, counts, groom_strands), slot_points, render_points);
	cloth_cache = ClothCache(); // the new solver is given the cloth on its first step
	pos_texture.instantiate();
	pos_texture->set_texture_rd_rid(solver->get_position_texture_rd_rid());
	prev_pos_texture.instantiate();
	prev_pos_texture->set_texture_rd_rid(solver->get_previous_position_texture_rd_rid());
	deep_shadow_texture.instantiate();
	deep_shadow_texture->set_texture_rd_rid(solver->get_deep_shadow_texture_rd_rid());
	draw_slot_points = MAX(slot_points, 1);
	if (card_first > 0) {
		_build_cards(bounds.grow(longest + 0.05f));
	}
	_build_snow(bounds.grow(longest + 0.05f), pts, counts, slot_points, drawn_points);
	_make_material();
	built = true;
	_step(0.0);
}

// The scalp as a sphere through the strands' roots (an algebraic least-squares fit: |p|^2 = 2 c.p + k,
// R^2 = k + |c|^2), for the cards' lift over it. w 0 when the roots don't fit a sphere.
Vector4 PhysXHair3D::_fit_scalp(const PackedVector3Array &p_points, const PackedInt32Array &p_counts) {
	double m[4][5] = {};
	int n = 0;
	int start = 0;
	AABB box;
	for (int i = 0; i < p_counts.size(); i++) {
		if (p_counts[i] > 0 && start < p_points.size()) {
			const Vector3 p = p_points[start];
			const double row[4] = { 2.0 * p.x, 2.0 * p.y, 2.0 * p.z, 1.0 };
			const double b = p.length_squared();
			for (int r = 0; r < 4; r++) {
				for (int c = 0; c < 4; c++) {
					m[r][c] += row[r] * row[c];
				}
				m[r][4] += row[r] * b;
			}
			box = n == 0 ? AABB(p, Vector3()) : box.expand(p);
			n++;
		}
		start += MAX(p_counts[i], 0);
	}
	if (n < 8) {
		return Vector4();
	}
	// Gaussian elimination with partial pivoting.
	for (int c = 0; c < 4; c++) {
		int pivot = c;
		for (int r = c + 1; r < 4; r++) {
			if (Math::abs(m[r][c]) > Math::abs(m[pivot][c])) {
				pivot = r;
			}
		}
		if (Math::abs(m[pivot][c]) < 1e-12) {
			return Vector4();
		}
		for (int k = 0; k < 5; k++) {
			SWAP(m[c][k], m[pivot][k]);
		}
		for (int r = 0; r < 4; r++) {
			if (r != c) {
				const double f = m[r][c] / m[c][c];
				for (int k = c; k < 5; k++) {
					m[r][k] -= f * m[c][k];
				}
			}
		}
	}
	const Vector3 centre(m[0][4] / m[0][0], m[1][4] / m[1][1], m[2][4] / m[2][2]);
	const double r2 = m[3][4] / m[3][3] + (double)centre.length_squared();
	const float size = box.get_longest_axis_size();
	if (!(r2 > 0.0) || !Math::is_finite((float)r2)) {
		return Vector4();
	}
	const float radius = (float)Math::sqrt(r2);
	// A patch of roots too flat for a sphere (a beard, a single lock) fits a huge one: no lift.
	if (radius > 4.0f * MAX(size, 1e-4f)) {
		return Vector4();
	}
	return Vector4(centre.x, centre.y, centre.z, radius);
}

// The snow mesh (stage 2r): per strand SNOW_CLUMPS blobs of six vertices (an octahedron), placed in
// vertex() on the strand in their slot; hidden while no snow lies on the hair.
static constexpr int SNOW_CLUMPS = 6;

void PhysXHair3D::_build_snow(const AABB &p_bounds, const PackedVector3Array &p_points, const PackedInt32Array &p_counts, int p_slot_points, int p_drawn_points) {
	const int strands = p_counts.size();
	if (strands <= 0 || p_points.is_empty() || p_drawn_points < 2) {
		return;
	}
	static const Vector3 corners[6] = { Vector3(1, 0, 0), Vector3(-1, 0, 0), Vector3(0, 1, 0), Vector3(0, -1, 0), Vector3(0, 0, 1), Vector3(0, 0, -1) };
	static const int faces[8][3] = { { 0, 2, 4 }, { 2, 1, 4 }, { 1, 3, 4 }, { 3, 0, 4 }, { 2, 0, 5 }, { 1, 2, 5 }, { 3, 1, 5 }, { 0, 3, 5 } };
	const int blobs = strands * SNOW_CLUMPS;
	PackedVector3Array verts;
	PackedVector3Array normals;
	PackedFloat32Array custom0;
	PackedInt32Array indices;
	verts.resize(blobs * 6);
	normals.resize(blobs * 6);
	custom0.resize(blobs * 6 * 4);
	indices.resize(blobs * 24);
	Vector3 *v = verts.ptrw();
	Vector3 *nr = normals.ptrw();
	float *c0 = custom0.ptrw();
	int *ix = indices.ptrw();
	// Rest positions: millimetre blobs spread along the groomed strands, so the path tracer's
	// acceleration structure is built around where the clumps can be (a refit from blobs piled
	// at the origin left its boxes overlapping, and every ray crossed them all).
	int start = 0;
	for (int b = 0; b < blobs; b++) {
		const int slot = b / SNOW_CLUMPS;
		const int n = MAX(p_counts[slot], 1);
		if (b > 0 && b % SNOW_CLUMPS == 0) {
			start += MAX(p_counts[slot - 1], 0);
		}
		const float f = 0.03f + 0.97f * float((b * 2654435761u) % 1024u) / 1023.0f;
		const Vector3 at = p_points[MIN(start + int(f * float(n - 1)), p_points.size() - 1)];
		for (int c = 0; c < 6; c++) {
			const int vi = b * 6 + c;
			v[vi] = at + corners[c] * 0.001f;
			nr[vi] = corners[c];
			c0[vi * 4 + 0] = float(slot * p_slot_points);
			c0[vi * 4 + 1] = float(b % SNOW_CLUMPS);
			c0[vi * 4 + 2] = float(p_drawn_points);
			c0[vi * 4 + 3] = 0.0f;
		}
		for (int f = 0; f < 8; f++) {
			for (int k = 0; k < 3; k++) {
				ix[b * 24 + f * 3 + k] = b * 6 + faces[f][k];
			}
		}
	}
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = verts;
	arrays[Mesh::ARRAY_NORMAL] = normals;
	arrays[Mesh::ARRAY_CUSTOM0] = custom0;
	arrays[Mesh::ARRAY_INDEX] = indices;
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays, Array(), Dictionary(), (uint64_t)Mesh::ARRAY_CUSTOM_RGBA_FLOAT << Mesh::ARRAY_FORMAT_CUSTOM0_SHIFT);
	snow_instance = memnew(MeshInstance3D);
	add_child(snow_instance, false, INTERNAL_MODE_BACK);
	snow_instance->set_mesh(mesh);
	snow_instance->set_custom_aabb(p_bounds);
	snow_instance->set_visible(false);
}

// The card mesh: per card point two crossed strips (four vertices), placed in vertex() from the
// card's simulated strand, which sorts to slot c for card c.
void PhysXHair3D::_build_cards(const AABB &p_bounds) {
	const HairCardBake &b = **card_bake;
	const int cs = b.samples;
	const int nc = b.card_count;
	// What each card holds, read by the card drawn in a slot (the slots follow the draw order, far
	// to near): per point four texels, (strip 0's axis, strip 1's half width), (strip 1's axis,
	// strip 0's half width), (the card's direction at rest, 0), (both strips' tiles: x, y in
	// texels).
	Ref<Image> data = Image::create_empty(cs * 4, nc, false, Image::FORMAT_RGBAF);
	for (int c = 0; c < nc; c++) {
		for (int k = 0; k < cs; k++) {
			const int i = c * cs + k;
			const Vector3 t = (b.points[c * cs + MIN(k + 1, cs - 1)] - b.points[c * cs + MAX(k - 1, 0)]).normalized();
			for (int strip = 0; strip < 2; strip++) {
				const Vector3 ax = b.axis[strip][i];
				data->set_pixel(k * 4 + strip, c, Color(ax.x, ax.y, ax.z, b.half_width[1 - strip][i]));
			}
			data->set_pixel(k * 4 + 2, c, Color(t.x, t.y, t.z, float(b.strand_counts[c])));
			data->set_pixel(k * 4 + 3, c, Color(b.tiles[c * 8 + 0], b.tiles[c * 8 + 1], b.tiles[c * 8 + 4], b.tiles[c * 8 + 5]));
		}
	}
	card_data = ImageTexture::create_from_image(data);
	// The mesh: per slot point two crossed strips (four vertices), placed in vertex() from the
	// simulated points: CUSTOM0 = (texel, previous texel, next texel, side -1 or 1), CUSTOM1 =
	// (point, strip, share along, 0).
	PackedVector3Array verts;
	PackedVector3Array normals;
	PackedFloat32Array custom0, custom1;
	PackedInt32Array indices;
	verts.resize(nc * cs * 4);
	normals.resize(verts.size());
	custom0.resize(verts.size() * 4);
	custom1.resize(verts.size() * 4);
	Vector3 *v = verts.ptrw();
	Vector3 *nr = normals.ptrw();
	float *c0 = custom0.ptrw();
	float *c1 = custom1.ptrw();
	for (int c = 0; c < nc; c++) {
		for (int k = 0; k < cs; k++) {
			const int i = c * cs + k;
			for (int strip = 0; strip < 2; strip++) {
				for (int side = 0; side < 2; side++) {
					const int vi = i * 4 + strip * 2 + side;
					v[vi] = b.points[i];
					nr[vi] = Vector3(0, 0, 1);
					c0[vi * 4 + 0] = float(c * draw_slot_points + k);
					c0[vi * 4 + 1] = float(c * draw_slot_points + MAX(k - 1, 0));
					c0[vi * 4 + 2] = float(c * draw_slot_points + MIN(k + 1, cs - 1));
					c0[vi * 4 + 3] = side == 0 ? -1.0f : 1.0f;
					c1[vi * 4 + 0] = float(k);
					c1[vi * 4 + 1] = float(strip);
					c1[vi * 4 + 2] = float(k) / float(cs - 1);
					c1[vi * 4 + 3] = 0.0f;
				}
			}
		}
		for (int k = 0; k + 1 < cs; k++) {
			for (int strip = 0; strip < 2; strip++) {
				const int v0 = (c * cs + k) * 4 + strip * 2;
				const int v2 = v0 + 4;
				indices.push_back(v0);
				indices.push_back(v0 + 1);
				indices.push_back(v2);
				indices.push_back(v0 + 1);
				indices.push_back(v2 + 1);
				indices.push_back(v2);
			}
		}
	}
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = verts;
	arrays[Mesh::ARRAY_NORMAL] = normals;
	arrays[Mesh::ARRAY_CUSTOM0] = custom0;
	arrays[Mesh::ARRAY_CUSTOM1] = custom1;
	arrays[Mesh::ARRAY_INDEX] = indices;
	const uint64_t format = ((uint64_t)Mesh::ARRAY_CUSTOM_RGBA_FLOAT << Mesh::ARRAY_FORMAT_CUSTOM0_SHIFT) |
			((uint64_t)Mesh::ARRAY_CUSTOM_RGBA_FLOAT << Mesh::ARRAY_FORMAT_CUSTOM1_SHIFT);
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays, Array(), Dictionary(), format);
	card_instance = memnew(MeshInstance3D);
	add_child(card_instance, false, INTERNAL_MODE_BACK);
	card_instance->set_mesh(mesh);
	// As the strands (blended, left out of the shadow maps): the depth pre-pass would otherwise
	// let the cards cast into them.
	card_instance->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
	card_instance->set_custom_aabb(p_bounds);
	card_instance->set_visible(false);
}

void PhysXHair3D::_step(double p_delta) {
	if (solver == nullptr) {
		return;
	}
	HairStrandSolver::Settings s;
	s.damping = damping;
	s.global_stiffness = global_stiffness;
	s.global_range = global_range;
	s.local_stiffness = local_stiffness;
	s.local_iterations = local_iterations;
	s.length_iterations = length_iterations;
	s.vsp_coeff = vsp_coeff;
	s.vsp_accel_threshold = vsp_accel_threshold;
	s.max_speed = max_speed;
	s.gravity = fixed_gravity;
	if (use_world_gravity && is_inside_tree()) {
		const Ref<World3D> w = get_world_3d();
		if (w.is_valid()) {
			PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
			const float mag = ps->area_get_param(w->get_space(), PhysicsServer3D::AREA_PARAM_GRAVITY);
			const Vector3 dir = ps->area_get_param(w->get_space(), PhysicsServer3D::AREA_PARAM_GRAVITY_VECTOR);
			s.gravity = dir * mag;
		}
	}
	s.wind = wind;
#ifdef MODULE_WEATHER_ENABLED
	if (weather_wind && is_inside_tree()) {
		if (const WeatherMap3D *weather = WeatherMap3D::get_active()) {
			s.wind += weather->get_wind_at(get_global_transform().origin);
		}
	}
#endif
	s.gust = gust;
	s.turbulence = turbulence;
	s.turbulence_size = turbulence_size;
	const Transform3D xform = get_global_transform();
	for (const Vector4 &sp : collision_spheres) {
		const Vector3 c = xform.xform(Vector3(sp.x, sp.y, sp.z));
		const float scale = xform.basis.get_scale().x;
		s.spheres.push_back(Vector4(c.x, c.y, c.z, sp.w * scale));
	}
	_body_triangles(s.body_triangles);
	_cloth_settings(s);
	s.body_cell_size = collision_cell_size;
	s.volume_enabled = volume_enabled;
	s.volume_friction = volume_friction;
	s.volume_stiffness = volume_stiffness;
	s.collision_radius = strand_collision ? strand_collision_radius : 0.0f;
	s.collision_friction = strand_collision_friction;
	s.wet = wetness;
	s.wet_clumping = wet_clumping;
	s.output_xform = render_instance ? render_instance->get_global_transform().affine_inverse() : xform.affine_inverse();
	// The draw order is sorted from the viewport's camera.
	const Camera3D *camera = is_inside_tree() ? get_viewport()->get_camera_3d() : nullptr;
	s.view_xform = camera ? camera->get_global_transform().affine_inverse() : Transform3D();
	// Distance LOD (TressFX): from lod_start_distance to lod_end_distance the rasterizer draws a
	// shrinking random share of the strands, down to lod_strand_ratio, widened to keep the hair's
	// opacity. Strands far away are under a pixel, so fewer, wider ones look the same for less
	// fill. Simulation and the path tracer keep every strand.
	s.lod_keep = 1.0f;
	if (lod_enabled && camera) {
		const float d = camera->get_global_transform().origin.distance_to(xform.origin);
		const float t = lod_end_distance > lod_start_distance ? CLAMP((d - lod_start_distance) / (lod_end_distance - lod_start_distance), 0.0f, 1.0f) : (d >= lod_end_distance ? 1.0f : 0.0f);
		s.lod_keep = Math::lerp(1.0f, lod_strand_ratio, t);
	}
	// Hair cards when the hair is small on screen (5% larger to come back, so the switch does not
	// flicker): the strands are hidden and only the guides and the cards' strands run. As
	// Unreal's groom LODs, by screen size: the hair's bounding sphere's diameter over the view's
	// height, so a zoomed-in camera or a larger screen keeps the strands where cards would show
	// as strips. card_screen_size 0: by card_distance instead.
	if (card_instance && card_first > 0) {
		const float d = camera ? camera->get_global_transform().origin.distance_to(xform.origin) : 0.0f;
		bool far = card_target ? d >= 0.95f * card_distance : d >= card_distance;
		if (card_screen_size > 0.0f && camera && render_instance) {
			const float size = _screen_size(camera);
			far = card_target ? size <= 1.05f * card_screen_size : size <= card_screen_size;
		}
		// The path tracer keeps the strands at every distance (it must not lose their detail).
		Ref<Environment> env = camera ? camera->get_environment() : Ref<Environment>();
		if (env.is_null() && get_world_3d().is_valid()) {
			env = get_world_3d()->get_environment();
			if (env.is_null()) {
				env = get_world_3d()->get_fallback_environment();
			}
		}
		const bool path_traced = env.is_valid() && env->is_pathtracing_enabled();
		card_target = far && !path_traced;
		// Cross-faded, as LOD transitions are (Unreal's and Godot's own visibility ranges): cards
		// cannot match the strands exactly (each blends one clump where the strands blend many
		// at different depths, so sunlit hair behind shows through a little more), and a switch in
		// one frame shows that as a step. The cards fade in over the strands, then the strands
		// fade out beneath them, so the hair never thins midway; the strands stay simulated until
		// the cards are drawn alone.
		const float step = card_transition > 0.0f && !path_traced ? float(p_delta) / card_transition : 1.0f;
		card_fade = card_target ? MIN(card_fade + step, 1.0f) : MAX(card_fade - step, 0.0f);
		const bool cards_only = card_fade >= 1.0f;
		if (cards_only != card_mode) {
			card_mode = cards_only;
			if (!card_mode) {
				solver->drop_history(); // the strands' last positions are stale
			}
		}
		card_instance->set_visible(card_fade > 0.0f);
		// One caster at a time: the cards' shadow once they are drawn alone, as the maps then are.
		card_instance->set_cast_shadows_setting(card_mode ? GeometryInstance3D::SHADOW_CASTING_SETTING_ON : GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
		if (render_instance) {
			render_instance->set_visible(card_fade < 1.0f);
		}
		if (card_fade != card_fade_set) {
			card_fade_set = card_fade;
			if (copy_material.is_valid()) {
				copy_material->set_shader_parameter("lod_fade", CLAMP(2.0f - 2.0f * card_fade, 0.0f, 1.0f));
			}
			if (card_material.is_valid()) {
				card_material->set_shader_parameter("lod_fade", CLAMP(2.0f * card_fade, 0.0f, 1.0f));
				// Over the strands while both are drawn.
				card_material->set_render_priority(card_fade > 0.0f && card_fade < 1.0f ? 1 : 0);
			}
		}
		s.card_first = card_first;
		s.cards_only = card_mode;
	} else if (card_fade_set != 0.0f) {
		// No cards (turned off mid-fade): the strands in full.
		card_target = false;
		card_fade = 0.0f;
		card_fade_set = 0.0f;
		if (copy_material.is_valid()) {
			copy_material->set_shader_parameter("lod_fade", 1.0f);
		}
	}
	if (copy_material.is_valid() && !Math::is_equal_approx(1.0f / s.lod_keep, lod_width_scale)) {
		lod_width_scale = 1.0f / s.lod_keep;
		copy_material->set_shader_parameter("lod_width_scale", lod_width_scale);
	}
	// Deep opacity maps (hair_shading's light() applies them): one toward the directional light,
	// and one from each of the omni and spot lights that light the hair most.
	int maps = 0;
	int snow_slot = -1;
	float slots[HairStrandSolverGPU::DOM_SLOTS * 16] = {};
	if (deep_shadows && hair_shading && render_instance && (render_material.is_null() || render_material->get_shader().is_null())) {
		const AABB box = render_instance->get_global_transform().xform(render_instance->get_aabb());
		HairStrandSolver::Settings::DeepShadowLight hair;
		hair.center = box.get_center();
		hair.radius = 0.5f * box.size.length();
		s.root_width = root_width;
		s.tip_width = tip_width;
		if (card_mode && card_bake.is_valid() && card_bake->card_count > 0) {
			// The maps from the cards' strands, each standing for its clump's strands side by side.
			const float clump = float(card_first) / float(card_bake->card_count);
			s.root_width *= clump;
			s.tip_width *= clump;
		}
		if (hair.radius > 0.0f) {
			if (DirectionalLight3D *light = _deep_shadow_light()) {
				HairStrandSolver::Settings::DeepShadowLight dl = hair;
				dl.light = light->get_global_transform().basis.get_column(2).normalized();
				if (HairStrandSolver::deep_shadow_slot(dl, slots + maps * 16)) {
					s.dom_lights.push_back(dl);
					maps++;
				}
			}
			// The snow's map: seen from where the snow comes from, the fall (1 m/s, Locatelli and
			// Hobbs 1974's aggregates) against the wind, so wind-driven snow lands on the windward side.
			if (snow_look > 0.0f && maps < HairStrandSolverGPU::DOM_SLOTS) {
				HairStrandSolver::Settings::DeepShadowLight sl = hair;
				const Vector3 from = Vector3(0.0f, 1.0f, 0.0f) - s.wind;
				sl.light = from.length_squared() > 1e-8f ? from.normalized() : Vector3(0.0f, 1.0f, 0.0f);
				if (HairStrandSolver::deep_shadow_slot(sl, slots + maps * 16)) {
					s.dom_lights.push_back(sl);
					snow_slot = maps;
					maps++;
				}
			}
			LocalVector<Light3D *> locals;
			_deep_shadow_local_lights(hair.center, hair.radius, render_instance->get_layer_mask(), locals);
			for (Light3D *light : locals) {
				if (maps >= HairStrandSolverGPU::DOM_SLOTS) {
					break;
				}
				HairStrandSolver::Settings::DeepShadowLight ll = hair;
				ll.light = light->get_global_transform().origin;
				ll.local = true;
				if (HairStrandSolver::deep_shadow_slot(ll, slots + maps * 16)) {
					s.dom_lights.push_back(ll);
					maps++;
				}
			}
		}
	}
	if (copy_material.is_valid()) {
		if (maps > 0) {
			PackedVector4Array params;
			params.resize(HairStrandSolverGPU::DOM_SLOTS * 4);
			for (int i = 0; i < params.size(); i++) {
				params.write[i] = Vector4(slots[i * 4], slots[i * 4 + 1], slots[i * 4 + 2], slots[i * 4 + 3]);
			}
			copy_material->set_shader_parameter("dom_slot", params);
			if (card_material.is_valid()) {
				card_material->set_shader_parameter("dom_slot", params);
			}
			if (snow_material.is_valid() && snow_slot >= 0) {
				snow_material->set_shader_parameter("dom_slot", params);
			}
		}
		if (snow_slot != snow_slot_set) {
			snow_slot_set = snow_slot;
			copy_material->set_shader_parameter("dom_snow_slot", snow_slot);
			if (card_material.is_valid()) {
				card_material->set_shader_parameter("dom_snow_slot", snow_slot);
			}
			if (snow_material.is_valid()) {
				snow_material->set_shader_parameter("dom_snow_slot", snow_slot);
			}
		}
		if (maps != deep_shadow_on) {
			deep_shadow_on = maps;
			copy_material->set_shader_parameter("dom_maps", maps);
			if (card_material.is_valid()) {
				card_material->set_shader_parameter("dom_maps", maps);
			}
			if (snow_material.is_valid()) {
				snow_material->set_shader_parameter("dom_maps", maps);
			}
		}
	}
	if (snow_instance) {
		// The snow's clumps while snow lies on the strands (the cards hold none yet).
		const bool show = snow_slot >= 0 && !card_mode && render_instance && render_instance->is_visible();
		if (snow_instance->is_visible() != show) {
			snow_instance->set_visible(show);
		}
	}
	_sync_shadow_copy();
	solver->step(xform, p_delta, substeps, s);
}

// The strands cast into the shadow maps through a copy drawn only there (Godot leaves blended
// materials out of them): the hair shades the face, neck and shoulders under it, as the strands
// in Unreal and TressFX do. Not with a material of the user's own (its shader is not ours), and
// not under the path tracer, whose rays meet the strands themselves.
void PhysXHair3D::_make_shadow_copy() {
	if (shadow_instance) {
		shadow_instance->queue_free();
		shadow_instance = nullptr;
	}
	shadow_material.unref();
	shadow_params.clear();
	if (!cast_hair_shadows || render_instance == nullptr || render_instance->get_mesh().is_null() || copy_material.is_null() ||
			(render_material.is_valid() && render_material->get_shader().is_valid())) {
		return;
	}
	Ref<Shader> &shader = shadow_shaders[hair_shading ? 1 : 0];
	if (shader.is_null()) {
		shader.instantiate();
		shader->set_code(String(HAIR_RENDER_HEAD) + (hair_shading ? "#define HAIR_SCATTERING\n" : "") + "#define HAIR_SHADOW_COPY\n" + HAIR_RENDER_SHADER);
	}
	shadow_material.instantiate();
	shadow_material->set_shader(shader);
	List<PropertyInfo> uniforms;
	shader->get_shader_uniform_list(&uniforms);
	for (const PropertyInfo &u : uniforms) {
		if (u.usage & PROPERTY_USAGE_GROUP || u.usage & PROPERTY_USAGE_SUBGROUP || u.usage & PROPERTY_USAGE_CATEGORY) {
			continue;
		}
		shadow_params.push_back(u.name);
	}
	shadow_instance = memnew(MeshInstance3D);
	add_child(shadow_instance, false, INTERNAL_MODE_BACK);
	shadow_instance->set_mesh(render_instance->get_mesh());
	shadow_instance->set_custom_aabb(render_instance->get_custom_aabb());
	shadow_instance->set_layer_mask(render_instance->get_layer_mask());
	shadow_instance->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_SHADOWS_ONLY);
	shadow_instance->set_material_override(shadow_material);
	_sync_shadow_copy();
}

void PhysXHair3D::_sync_shadow_copy() {
	if (shadow_instance == nullptr || shadow_material.is_null() || copy_material.is_null()) {
		return;
	}
	for (const StringName &name : shadow_params) {
		shadow_material->set_shader_parameter(name, copy_material->get_shader_parameter(name));
	}
	bool show = render_instance && render_instance->is_visible() && !card_mode;
	if (show && is_inside_tree()) {
		const Camera3D *camera = get_viewport()->get_camera_3d();
		Ref<Environment> env = camera ? camera->get_environment() : Ref<Environment>();
		if (env.is_null() && get_world_3d().is_valid()) {
			env = get_world_3d()->get_environment();
			if (env.is_null()) {
				env = get_world_3d()->get_fallback_environment();
			}
		}
		if (env.is_valid() && env->is_pathtracing_enabled()) {
			show = false;
		}
	}
	if (shadow_instance->is_visible() != show) {
		shadow_instance->set_visible(show);
	}
}

void PhysXHair3D::set_cast_hair_shadows(bool p_enabled) {
	if (cast_hair_shadows == p_enabled) {
		return;
	}
	cast_hair_shadows = p_enabled;
	_make_shadow_copy();
}

bool PhysXHair3D::is_casting_hair_shadows() const {
	return cast_hair_shadows;
}

DirectionalLight3D *PhysXHair3D::_deep_shadow_light() {
	if (!deep_shadow_light.is_empty()) {
		return Object::cast_to<DirectionalLight3D>(get_node_or_null(deep_shadow_light));
	}
	DirectionalLight3D *light = ObjectDB::get_instance<DirectionalLight3D>(deep_shadow_light_cache);
	if (light && light->is_inside_tree() && light->is_visible_in_tree() && light->has_shadow()) {
		return light;
	}
	// The first shadowed DirectionalLight3D in the scene, searched about once a second.
	const uint64_t frame = Engine::get_singleton()->get_process_frames();
	if (deep_shadow_light_cache.is_valid() || frame >= deep_shadow_search_frame) {
		deep_shadow_light_cache = ObjectID();
		deep_shadow_search_frame = frame + 60;
		Node *root = is_inside_tree() ? get_tree()->get_current_scene() : nullptr;
		if (root == nullptr && is_inside_tree()) {
			root = get_tree()->get_root();
		}
		LocalVector<Node *> stack;
		if (root) {
			stack.push_back(root);
		}
		float best = -1.0f;
		while (!stack.is_empty()) {
			Node *n = stack[stack.size() - 1];
			stack.remove_at(stack.size() - 1);
			if (DirectionalLight3D *dl = Object::cast_to<DirectionalLight3D>(n)) {
				const float energy = dl->get_param(Light3D::PARAM_ENERGY);
				if (dl->is_visible_in_tree() && dl->has_shadow() && energy > best) {
					best = energy;
					deep_shadow_light_cache = dl->get_instance_id();
				}
			}
			for (int i = 0; i < n->get_child_count(); i++) {
				stack.push_back(n->get_child(i));
			}
		}
	}
	return ObjectDB::get_instance<DirectionalLight3D>(deep_shadow_light_cache);
}

// The shadowed omni and spot lights that light the hair most (energy times their falloff at the
// hair's centre), up to deep_shadow_local_lights; the scene is searched for them about once a
// second.
void PhysXHair3D::_deep_shadow_local_lights(const Vector3 &p_center, float p_radius, uint32_t p_layers, LocalVector<Light3D *> &r_lights) {
	r_lights.clear();
	if (deep_shadow_local_lights <= 0 || !is_inside_tree()) {
		return;
	}
	const uint64_t frame = Engine::get_singleton()->get_process_frames();
	if (frame >= deep_shadow_local_search_frame) {
		deep_shadow_local_search_frame = frame + 60;
		deep_shadow_local_cache.clear();
		Node *root = get_tree()->get_current_scene();
		if (root == nullptr) {
			root = get_tree()->get_root();
		}
		LocalVector<Node *> stack;
		stack.push_back(root);
		while (!stack.is_empty()) {
			Node *n = stack[stack.size() - 1];
			stack.remove_at(stack.size() - 1);
			if (Object::cast_to<OmniLight3D>(n) || Object::cast_to<SpotLight3D>(n)) {
				deep_shadow_local_cache.push_back(n->get_instance_id());
			}
			for (int i = 0; i < n->get_child_count(); i++) {
				stack.push_back(n->get_child(i));
			}
		}
	}
	struct Lit {
		Light3D *light;
		float weight;
	};
	LocalVector<Lit> lit;
	for (const ObjectID &id : deep_shadow_local_cache) {
		Light3D *light = ObjectDB::get_instance<Light3D>(id);
		if (light == nullptr || !light->is_inside_tree() || !light->is_visible_in_tree() || !light->has_shadow() || !(light->get_cull_mask() & p_layers)) {
			continue;
		}
		const Transform3D lx = light->get_global_transform();
		const Vector3 to_hair = p_center - lx.origin;
		const float d = to_hair.length();
		const float range = light->get_param(Light3D::PARAM_RANGE);
		if (d > range + p_radius || d < p_radius * 1.05f) {
			continue; // out of reach, or inside the hair (no single view holds it)
		}
		if (SpotLight3D *spot = Object::cast_to<SpotLight3D>(light)) {
			// Outside the cone (grown by the hair's angular size): it does not reach the hair.
			const float cone = Math::deg_to_rad(spot->get_param(Light3D::PARAM_SPOT_ANGLE)) + Math::asin(MIN(p_radius / MAX(d, 1e-4f), 1.0f));
			if (d > 1e-4f && (-lx.basis.get_column(2)).dot(to_hair / d) < Math::cos(MIN(cone, (float)Math::PI))) {
				continue;
			}
		}
		// Godot's omni falloff (get_omni_attenuation) at the hair's centre.
		float nd = MIN(d / MAX(range, 1e-4f), 1.0f);
		nd = nd * nd;
		nd = nd * nd;
		const float window = (1.0f - nd) * (1.0f - nd);
		const float falloff = window * Math::pow(MAX(d, 1e-4f), -light->get_param(Light3D::PARAM_ATTENUATION));
		lit.push_back({ light, light->get_param(Light3D::PARAM_ENERGY) * falloff });
	}
	for (uint32_t i = 1; i < lit.size(); i++) {
		for (uint32_t j = i; j > 0 && lit[j].weight > lit[j - 1].weight; j--) {
			SWAP(lit[j], lit[j - 1]);
		}
	}
	for (uint32_t i = 0; i < lit.size() && (int)i < deep_shadow_local_lights; i++) {
		if (lit[i].weight > 0.0f) {
			r_lights.push_back(lit[i].light);
		}
	}
}

void PhysXHair3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			set_process_internal(true);
			build_pending = true;
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			if (build_pending) {
				_build();
			}
			if (built) {
				_update_wetness(Engine::get_singleton()->is_editor_hint() ? 0.0 : get_process_delta_time());
			}
			if (built && solver != nullptr && simulating) {
				_step(get_process_delta_time());
			}
		} break;
		case NOTIFICATION_EXIT_TREE: {
			_clear();
		} break;
	}
}

void PhysXHair3D::reset_simulation() {
	if (solver != nullptr) {
		solver->reset();
	}
}

MeshInstance3D *PhysXHair3D::get_render_instance() const {
	return render_instance;
}

PhysXHair3D::~PhysXHair3D() {
	_clear();
}

// Properties.

void PhysXHair3D::set_groom(const Ref<HairGroom> &p_groom) {
	if (groom == p_groom) {
		return;
	}
	if (groom.is_valid()) {
		groom->disconnect_changed(callable_mp(this, &PhysXHair3D::_groom_changed));
	}
	groom = p_groom;
	if (groom.is_valid()) {
		groom->connect_changed(callable_mp(this, &PhysXHair3D::_groom_changed));
	}
	_queue_build();
}

Ref<HairGroom> PhysXHair3D::get_groom() const {
	return groom;
}

void PhysXHair3D::set_root_width(float p_width) {
	root_width = MAX(p_width, 0.0f);
	_queue_build();
}

float PhysXHair3D::get_root_width() const {
	return root_width;
}

void PhysXHair3D::set_simplify_tolerance(float p_tolerance) {
	simplify_tolerance = MAX(p_tolerance, 0.0f);
	_queue_build();
}

float PhysXHair3D::get_simplify_tolerance() const {
	return simplify_tolerance;
}

void PhysXHair3D::set_tip_width(float p_width) {
	tip_width = MAX(p_width, 0.0f);
	_queue_build();
}

float PhysXHair3D::get_tip_width() const {
	return tip_width;
}

void PhysXHair3D::set_melanin(float p_melanin) {
	melanin = CLAMP(p_melanin, 0.0f, 1.0f);
	if (built) {
		_make_material();
	}
}

float PhysXHair3D::get_melanin() const {
	return melanin;
}

void PhysXHair3D::set_grey_ratio(float p_ratio) {
	grey_ratio = CLAMP(p_ratio, 0.0f, 1.0f);
	_set_look(copy_material);
	_set_look(card_material);
	_set_look(snow_material);
}

float PhysXHair3D::get_grey_ratio() const {
	return grey_ratio;
}

void PhysXHair3D::set_sun_bleaching(float p_amount) {
	sun_bleaching = CLAMP(p_amount, 0.0f, 1.0f);
	_set_look(copy_material);
	_set_look(card_material);
	_set_look(snow_material);
}

float PhysXHair3D::get_sun_bleaching() const {
	return sun_bleaching;
}

void PhysXHair3D::set_melanin_redness(float p_redness) {
	melanin_redness = CLAMP(p_redness, 0.0f, 1.0f);
	if (built) {
		_make_material();
	}
}

float PhysXHair3D::get_melanin_redness() const {
	return melanin_redness;
}

void PhysXHair3D::set_roughness(float p_roughness) {
	roughness = CLAMP(p_roughness, 0.0f, 1.0f);
	if (built) {
		_make_material();
	}
}

float PhysXHair3D::get_roughness() const {
	return roughness;
}

void PhysXHair3D::set_radial_roughness(float p_roughness) {
	radial_roughness = CLAMP(p_roughness, 0.0f, 1.0f);
	if (built) {
		_make_material();
	}
}

float PhysXHair3D::get_radial_roughness() const {
	return radial_roughness;
}

void PhysXHair3D::set_hair_shading(bool p_enabled) {
	hair_shading = p_enabled;
	if (built) {
		_make_material();
	}
}

bool PhysXHair3D::is_hair_shading() const {
	return hair_shading;
}

void PhysXHair3D::set_lod_enabled(bool p_enabled) {
	lod_enabled = p_enabled;
}

bool PhysXHair3D::is_lod_enabled() const {
	return lod_enabled;
}

void PhysXHair3D::set_lod_start_distance(float p_distance) {
	lod_start_distance = MAX(p_distance, 0.0f);
}

float PhysXHair3D::get_lod_start_distance() const {
	return lod_start_distance;
}

void PhysXHair3D::set_lod_end_distance(float p_distance) {
	lod_end_distance = MAX(p_distance, 0.0f);
}

float PhysXHair3D::get_lod_end_distance() const {
	return lod_end_distance;
}

void PhysXHair3D::set_lod_strand_ratio(float p_ratio) {
	lod_strand_ratio = CLAMP(p_ratio, 0.01f, 1.0f);
}

float PhysXHair3D::get_lod_strand_ratio() const {
	return lod_strand_ratio;
}

void PhysXHair3D::set_render_material(const Ref<ShaderMaterial> &p_material) {
	render_material = p_material;
	if (built && solver != nullptr) {
		_make_material();
	}
}

Ref<ShaderMaterial> PhysXHair3D::get_render_material() const {
	return render_material;
}

void PhysXHair3D::set_simulating(bool p_simulating) {
	if (simulating && !p_simulating && solver != nullptr && built) {
		solver->hold(); // the hair stops: no motion vectors from the last step
	}
	simulating = p_simulating;
}

bool PhysXHair3D::is_simulating() const {
	return simulating;
}

void PhysXHair3D::set_substeps(int p_substeps) {
	substeps = CLAMP(p_substeps, 1, 32);
}

int PhysXHair3D::get_substeps() const {
	return substeps;
}

void PhysXHair3D::set_damping(float p_damping) {
	damping = CLAMP(p_damping, 0.0f, 1.0f);
}

float PhysXHair3D::get_damping() const {
	return damping;
}

void PhysXHair3D::set_global_stiffness(float p_value) {
	global_stiffness = CLAMP(p_value, 0.0f, 1.0f);
}

float PhysXHair3D::get_global_stiffness() const {
	return global_stiffness;
}

void PhysXHair3D::set_global_range(float p_value) {
	global_range = CLAMP(p_value, 0.0f, 1.0f);
}

float PhysXHair3D::get_global_range() const {
	return global_range;
}

void PhysXHair3D::set_local_stiffness(float p_value) {
	local_stiffness = CLAMP(p_value, 0.0f, 1.0f);
}

float PhysXHair3D::get_local_stiffness() const {
	return local_stiffness;
}

void PhysXHair3D::set_local_iterations(int p_value) {
	local_iterations = CLAMP(p_value, 0, 16);
}

int PhysXHair3D::get_local_iterations() const {
	return local_iterations;
}

void PhysXHair3D::set_length_iterations(int p_value) {
	length_iterations = CLAMP(p_value, 0, 16);
}

int PhysXHair3D::get_length_iterations() const {
	return length_iterations;
}

void PhysXHair3D::set_vsp_coeff(float p_value) {
	vsp_coeff = CLAMP(p_value, 0.0f, 1.0f);
}

float PhysXHair3D::get_vsp_coeff() const {
	return vsp_coeff;
}

void PhysXHair3D::set_vsp_accel_threshold(float p_value) {
	vsp_accel_threshold = MAX(p_value, 0.0f);
}

float PhysXHair3D::get_vsp_accel_threshold() const {
	return vsp_accel_threshold;
}

void PhysXHair3D::set_max_speed(float p_value) {
	max_speed = MAX(p_value, 0.0f);
}

float PhysXHair3D::get_max_speed() const {
	return max_speed;
}

void PhysXHair3D::set_use_world_gravity(bool p_use) {
	use_world_gravity = p_use;
}

bool PhysXHair3D::is_using_world_gravity() const {
	return use_world_gravity;
}

void PhysXHair3D::set_fixed_gravity(const Vector3 &p_gravity) {
	fixed_gravity = p_gravity;
}

Vector3 PhysXHair3D::get_fixed_gravity() const {
	return fixed_gravity;
}

void PhysXHair3D::set_weather_wind(bool p_enabled) {
	weather_wind = p_enabled;
}

bool PhysXHair3D::get_weather_wind() const {
	return weather_wind;
}

void PhysXHair3D::set_wind(const Vector3 &p_wind) {
	wind = p_wind;
}

Vector3 PhysXHair3D::get_wind() const {
	return wind;
}

void PhysXHair3D::set_gust(float p_gust) {
	gust = CLAMP(p_gust, 0.0f, 1.0f);
}

float PhysXHair3D::get_gust() const {
	return gust;
}

void PhysXHair3D::set_guide_ratio(float p_ratio) {
	guide_ratio = CLAMP(p_ratio, 0.01f, 1.0f);
	_queue_build();
}

float PhysXHair3D::get_guide_ratio() const {
	return guide_ratio;
}

void PhysXHair3D::set_turbulence(float p_turbulence) {
	turbulence = CLAMP(p_turbulence, 0.0f, 1.0f);
}

float PhysXHair3D::get_turbulence() const {
	return turbulence;
}

void PhysXHair3D::set_turbulence_size(float p_size) {
	turbulence_size = MAX(p_size, 0.001f);
}

float PhysXHair3D::get_turbulence_size() const {
	return turbulence_size;
}

// The collision body's triangles in world space this frame: a MeshInstance3D's surfaces, skinned on
// the CPU as the GPU skins them (skeleton * bone pose * bind pose, weighted) when it has a skin, else
// with its transform. Meant for a low-poly collision proxy of the body (a few thousand triangles).
void PhysXHair3D::_body_triangles(PackedVector3Array &r_tris) {
	r_tris.clear();
	MeshInstance3D *mi = collision_body.is_empty() || !is_inside_tree() ? nullptr : Object::cast_to<MeshInstance3D>(get_node_or_null(collision_body));
	if (mi == nullptr || mi->get_mesh().is_null()) {
		return;
	}
	Ref<Mesh> mesh = mi->get_mesh();
	BodyCache &c = body_cache;
	if (c.mesh_instance != mi->get_instance_id() || c.mesh != mesh->get_rid()) {
		c = BodyCache();
		c.mesh_instance = mi->get_instance_id();
		c.mesh = mesh->get_rid();
		for (int s = 0; s < mesh->get_surface_count(); s++) {
			if (mesh->surface_get_primitive_type(s) != Mesh::PRIMITIVE_TRIANGLES) {
				continue;
			}
			const Array arrays = mesh->surface_get_arrays(s);
			const PackedVector3Array v = arrays[Mesh::ARRAY_VERTEX];
			const PackedInt32Array idx = arrays[Mesh::ARRAY_INDEX];
			const PackedInt32Array b = arrays[Mesh::ARRAY_BONES];
			const PackedFloat32Array w = arrays[Mesh::ARRAY_WEIGHTS];
			const int base = c.verts.size();
			const int per = v.is_empty() ? 0 : b.size() / v.size();
			if (s == 0 || c.per_vertex == 0) {
				c.per_vertex = per;
			}
			c.verts.append_array(v);
			if (idx.is_empty()) {
				for (int i = 0; i + 2 < v.size(); i += 3) {
					c.indices.push_back(base + i);
					c.indices.push_back(base + i + 2);
					c.indices.push_back(base + i + 1);
				}
			} else {
				for (int i = 0; i + 2 < idx.size(); i += 3) {
					// Godot winds triangles clockwise: swapped, so the cross product points out.
					c.indices.push_back(base + idx[i]);
					c.indices.push_back(base + idx[i + 2]);
					c.indices.push_back(base + idx[i + 1]);
				}
			}
			if (per > 0 && per == c.per_vertex && w.size() == b.size()) {
				c.bones.append_array(b);
				c.weights.append_array(w);
			}
		}
		if (c.bones.size() != c.verts.size() * c.per_vertex) {
			c.bones.clear();
			c.weights.clear();
			c.per_vertex = 0;
		}
	}
	const int vc = c.verts.size();
	skinned.resize(vc);
	Vector3 *out = skinned.ptrw();
	Skeleton3D *skeleton = Object::cast_to<Skeleton3D>(mi->get_node_or_null(mi->get_skeleton_path()));
	Ref<Skin> skin = mi->get_skin();
	if (skeleton != nullptr && skin.is_valid() && c.per_vertex > 0) {
		const Transform3D sk = skeleton->get_global_transform();
		LocalVector<Transform3D> bind;
		bind.resize(skin->get_bind_count());
		for (int j = 0; j < skin->get_bind_count(); j++) {
			int bone = skin->get_bind_bone(j);
			if (bone < 0) {
				bone = skeleton->find_bone(skin->get_bind_name(j));
			}
			bind[j] = bone < 0 ? sk : sk * skeleton->get_bone_global_pose(bone) * skin->get_bind_pose(j);
		}
		const int per = c.per_vertex;
		for (int i = 0; i < vc; i++) {
			Vector3 p;
			float wsum = 0.0f;
			for (int k = 0; k < per; k++) {
				const float w = c.weights[i * per + k];
				const int j = c.bones[i * per + k];
				if (w > 0.0f && j >= 0 && j < (int)bind.size()) {
					p += bind[j].xform(c.verts[i]) * w;
					wsum += w;
				}
			}
			out[i] = wsum > 0.0f ? p / wsum : sk.xform(c.verts[i]);
		}
	} else {
		const Transform3D x = mi->get_global_transform();
		for (int i = 0; i < vc; i++) {
			out[i] = x.xform(c.verts[i]);
		}
	}
	r_tris.resize(c.indices.size());
	Vector3 *t = r_tris.ptrw();
	for (int i = 0; i < c.indices.size(); i++) {
		t[i] = out[c.indices[i]];
	}
}

// The cloth the hair rests on (TressFX's SDF collision, fed the cloth's simulated triangles): its
// render copy's triangles as particle triples, given to the solver once per cloth build, which reads
// their positions from the cloth's position texture each frame on the GPU (no readback).
void PhysXHair3D::_cloth_settings(HairStrandSolver::Settings &r_settings) {
	PhysXSkinnedCloth3D *cloth = collision_cloth.is_empty() || !is_inside_tree() ? nullptr : Object::cast_to<PhysXSkinnedCloth3D>(get_node_or_null(collision_cloth));
	MeshInstance3D *ri = cloth ? cloth->get_render_instance() : nullptr;
	const RID tex = cloth ? cloth->get_position_texture_rd_rid() : RID();
	Ref<Mesh> mesh = ri ? ri->get_mesh() : Ref<Mesh>();
	if (ri == nullptr || mesh.is_null() || !tex.is_valid() || !ri->is_inside_tree()) {
		if (cloth_cache.texture.is_valid() && solver) {
			solver->set_cloth(RID(), PackedInt32Array());
		}
		cloth_cache = ClothCache();
		return;
	}
	ClothCache &c = cloth_cache;
	if (c.cloth != cloth->get_instance_id() || c.mesh != mesh->get_rid() || c.texture != tex) {
		c = ClothCache();
		c.cloth = cloth->get_instance_id();
		c.mesh = mesh->get_rid();
		c.texture = tex;
		const PackedInt32Array particles = cloth->get_vertex_particles();
		for (int sf = 0; sf < mesh->get_surface_count(); sf++) {
			if (mesh->surface_get_primitive_type(sf) != Mesh::PRIMITIVE_TRIANGLES) {
				continue;
			}
			const Array arrays = mesh->surface_get_arrays(sf);
			const PackedInt32Array idx = arrays[Mesh::ARRAY_INDEX];
			const PackedVector3Array vx = arrays[Mesh::ARRAY_VERTEX];
			const PackedVector3Array nx = arrays[Mesh::ARRAY_NORMAL];
			const int vc = vx.size();
			const int n = idx.is_empty() ? vc - vc % 3 : idx.size() - idx.size() % 3;
			// The outside is where the mesh's normals face: wound so the cross product points there
			// (Godot winds clockwise; the cloth may be authored either way).
			bool flip = true;
			if (n >= 3 && nx.size() == vc) {
				const int i0 = idx.is_empty() ? 0 : idx[0], i1 = idx.is_empty() ? 1 : idx[1], i2 = idx.is_empty() ? 2 : idx[2];
				if (i0 < vc && i1 < vc && i2 < vc) {
					const Vector3 fn = (vx[i1] - vx[i0]).cross(vx[i2] - vx[i0]);
					flip = fn.dot(nx[i0] + nx[i1] + nx[i2]) < 0.0f;
				}
			}
			for (int i = 0; i < n; i += 3) {
				int v[3];
				bool ok = true;
				for (int k = 0; k < 3; k++) {
					v[k] = idx.is_empty() ? i + k : idx[i + k];
					ok = ok && v[k] >= 0 && v[k] < particles.size();
				}
				if (!ok) {
					continue;
				}
				const int a = particles[v[0]], b = particles[v[1]], d = particles[v[2]];
				if (a == b || b == d || a == d) {
					continue; // welded to a point: no area
				}
				c.tris.push_back(a);
				c.tris.push_back(flip ? d : b);
				c.tris.push_back(flip ? b : d);
			}
		}
		if (solver) {
			solver->set_cloth(tex, c.tris);
		}
	}
	if (c.tris.is_empty()) {
		return;
	}
	r_settings.cloth_xform = ri->get_global_transform();
	// Where the cloth can be: its render copy's bounds (grown by how far it swings).
	const AABB local = ri->get_custom_aabb().has_volume() ? ri->get_custom_aabb() : ri->get_aabb().grow(0.1f);
	r_settings.cloth_bounds = r_settings.cloth_xform.xform(local);
	r_settings.cloth_texture_width = cloth->get_position_texture_width();
	r_settings.cloth_thickness = cloth_thickness;
}

void PhysXHair3D::set_collision_cloth(const NodePath &p_path) {
	collision_cloth = p_path;
	if (cloth_cache.texture.is_valid() && solver) {
		solver->set_cloth(RID(), PackedInt32Array());
	}
	cloth_cache = ClothCache();
}

NodePath PhysXHair3D::get_collision_cloth() const {
	return collision_cloth;
}

void PhysXHair3D::set_cloth_thickness(float p_thickness) {
	cloth_thickness = MAX(p_thickness, 0.0f);
}

float PhysXHair3D::get_cloth_thickness() const {
	return cloth_thickness;
}

void PhysXHair3D::set_collision_body(const NodePath &p_path) {
	collision_body = p_path;
	body_cache = BodyCache();
}

NodePath PhysXHair3D::get_collision_body() const {
	return collision_body;
}

void PhysXHair3D::set_collision_cell_size(float p_size) {
	collision_cell_size = MAX(p_size, 0.001f);
}

float PhysXHair3D::get_collision_cell_size() const {
	return collision_cell_size;
}

void PhysXHair3D::set_deep_shadows(bool p_enabled) {
	deep_shadows = p_enabled;
}

bool PhysXHair3D::is_deep_shadows() const {
	return deep_shadows;
}

void PhysXHair3D::set_deep_shadow_light(const NodePath &p_path) {
	deep_shadow_light = p_path;
	deep_shadow_light_cache = ObjectID();
	deep_shadow_search_frame = 0;
}

NodePath PhysXHair3D::get_deep_shadow_light() const {
	return deep_shadow_light;
}

void PhysXHair3D::set_deep_shadow_local_lights(int p_count) {
	deep_shadow_local_lights = CLAMP(p_count, 0, HairStrandSolverGPU::DOM_SLOTS - 1);
	deep_shadow_local_search_frame = 0;
}

int PhysXHair3D::get_deep_shadow_local_lights() const {
	return deep_shadow_local_lights;
}

void PhysXHair3D::set_volume_enabled(bool p_enabled) {
	volume_enabled = p_enabled;
}

bool PhysXHair3D::is_volume_enabled() const {
	return volume_enabled;
}

void PhysXHair3D::set_volume_friction(float p_friction) {
	volume_friction = CLAMP(p_friction, 0.0f, 1.0f);
}

float PhysXHair3D::get_volume_friction() const {
	return volume_friction;
}

void PhysXHair3D::set_volume_stiffness(float p_stiffness) {
	volume_stiffness = MAX(p_stiffness, 0.0f);
}

float PhysXHair3D::get_volume_stiffness() const {
	return volume_stiffness;
}

void PhysXHair3D::set_strand_collision(bool p_enabled) {
	strand_collision = p_enabled;
}

bool PhysXHair3D::is_strand_collision() const {
	return strand_collision;
}

void PhysXHair3D::set_strand_collision_radius(float p_radius) {
	strand_collision_radius = MAX(p_radius, 0.0f);
}

float PhysXHair3D::get_strand_collision_radius() const {
	return strand_collision_radius;
}

void PhysXHair3D::set_strand_collision_friction(float p_friction) {
	strand_collision_friction = CLAMP(p_friction, 0.0f, 1.0f);
}

void PhysXHair3D::set_wetness(float p_wetness) {
	wetness = CLAMP(p_wetness, 0.0f, 1.0f);
}

float PhysXHair3D::get_wetness() const {
	return wetness;
}

void PhysXHair3D::set_weather_wetting(bool p_enabled) {
	weather_wetting = p_enabled;
}

bool PhysXHair3D::get_weather_wetting() const {
	return weather_wetting;
}

void PhysXHair3D::set_water_capacity(float p_mm) {
	water_capacity = MAX(p_mm, 0.01f);
}

float PhysXHair3D::get_water_capacity() const {
	return water_capacity;
}

void PhysXHair3D::set_snow(float p_snow) {
	snow = CLAMP(p_snow, 0.0f, 1.0f);
}

float PhysXHair3D::get_snow() const {
	return snow;
}

void PhysXHair3D::set_snow_capacity(float p_mm) {
	snow_capacity = MAX(p_mm, 0.01f);
}

float PhysXHair3D::get_snow_capacity() const {
	return snow_capacity;
}

void PhysXHair3D::set_drying_time(float p_seconds) {
	drying_time = MAX(p_seconds, 1.0f);
}

float PhysXHair3D::get_drying_time() const {
	return drying_time;
}

void PhysXHair3D::set_wet_clumping(float p_clumping) {
	wet_clumping = CLAMP(p_clumping, 0.0f, 1.0f);
}

float PhysXHair3D::get_wet_clumping() const {
	return wet_clumping;
}

void PhysXHair3D::set_cards_enabled(bool p_enabled) {
	if (cards_enabled == p_enabled) {
		return;
	}
	cards_enabled = p_enabled;
	_queue_build();
}

bool PhysXHair3D::is_cards_enabled() const {
	return cards_enabled;
}

void PhysXHair3D::set_card_count(int p_count) {
	p_count = CLAMP(p_count, 1, 1024);
	if (card_count == p_count) {
		return;
	}
	card_count = p_count;
	if (cards_enabled) {
		_queue_build();
	}
}

int PhysXHair3D::get_card_count() const {
	return card_count;
}

// The hair's bounding sphere's diameter over the camera's view height (Unreal's screen size).
float PhysXHair3D::_screen_size(const Camera3D *p_camera) const {
	const AABB box = render_instance->get_global_transform().xform(render_instance->get_aabb());
	const float diameter = box.size.length();
	const Projection proj = p_camera->get_camera_projection();
	if (p_camera->get_projection() == Camera3D::PROJECTION_ORTHOGONAL) {
		return diameter * 0.5f * proj.columns[1][1];
	}
	const float z = MAX(-p_camera->get_global_transform().affine_inverse().xform(box.get_center()).z, 1e-3f);
	return diameter * 0.5f * proj.columns[1][1] / z;
}

void PhysXHair3D::set_card_screen_size(float p_size) {
	card_screen_size = CLAMP(p_size, 0.0f, 1.0f);
}

float PhysXHair3D::get_card_screen_size() const {
	return card_screen_size;
}

void PhysXHair3D::set_card_transition(float p_seconds) {
	card_transition = MAX(p_seconds, 0.0f);
}

float PhysXHair3D::get_card_transition() const {
	return card_transition;
}

void PhysXHair3D::set_thick_strands(bool p_enabled) {
	thick_strands = p_enabled;
	if (card_material.is_valid()) {
		card_material->set_shader_parameter("card_thick", thick_strands);
	}
}

bool PhysXHair3D::is_thick_strands() const {
	return thick_strands;
}

float PhysXHair3D::get_screen_size() const {
	if (!is_inside_tree() || render_instance == nullptr) {
		return 0.0f;
	}
	const Camera3D *camera = get_viewport()->get_camera_3d();
	return camera ? _screen_size(camera) : 0.0f;
}

void PhysXHair3D::set_card_distance(float p_distance) {
	card_distance = MAX(p_distance, 0.0f);
}

float PhysXHair3D::get_card_distance() const {
	return card_distance;
}

bool PhysXHair3D::is_drawing_cards() const {
	return card_mode;
}

float PhysXHair3D::get_strand_collision_friction() const {
	return strand_collision_friction;
}

void PhysXHair3D::set_collision_spheres(const PackedVector4Array &p_spheres) {
	collision_spheres = p_spheres;
}

PackedVector4Array PhysXHair3D::get_collision_spheres() const {
	return collision_spheres;
}

void PhysXHair3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_groom", "groom"), &PhysXHair3D::set_groom);
	ClassDB::bind_method(D_METHOD("get_groom"), &PhysXHair3D::get_groom);
	ClassDB::bind_method(D_METHOD("set_root_width", "width"), &PhysXHair3D::set_root_width);
	ClassDB::bind_method(D_METHOD("set_simplify_tolerance", "tolerance"), &PhysXHair3D::set_simplify_tolerance);
	ClassDB::bind_method(D_METHOD("get_simplify_tolerance"), &PhysXHair3D::get_simplify_tolerance);
	ClassDB::bind_method(D_METHOD("get_root_width"), &PhysXHair3D::get_root_width);
	ClassDB::bind_method(D_METHOD("set_tip_width", "width"), &PhysXHair3D::set_tip_width);
	ClassDB::bind_method(D_METHOD("get_tip_width"), &PhysXHair3D::get_tip_width);
	ClassDB::bind_method(D_METHOD("set_melanin", "melanin"), &PhysXHair3D::set_melanin);
	ClassDB::bind_method(D_METHOD("get_melanin"), &PhysXHair3D::get_melanin);
	ClassDB::bind_method(D_METHOD("set_grey_ratio", "ratio"), &PhysXHair3D::set_grey_ratio);
	ClassDB::bind_method(D_METHOD("get_grey_ratio"), &PhysXHair3D::get_grey_ratio);
	ClassDB::bind_method(D_METHOD("set_sun_bleaching", "amount"), &PhysXHair3D::set_sun_bleaching);
	ClassDB::bind_method(D_METHOD("get_sun_bleaching"), &PhysXHair3D::get_sun_bleaching);
	ClassDB::bind_method(D_METHOD("set_melanin_redness", "redness"), &PhysXHair3D::set_melanin_redness);
	ClassDB::bind_method(D_METHOD("get_melanin_redness"), &PhysXHair3D::get_melanin_redness);
	ClassDB::bind_method(D_METHOD("set_roughness", "roughness"), &PhysXHair3D::set_roughness);
	ClassDB::bind_method(D_METHOD("get_roughness"), &PhysXHair3D::get_roughness);
	ClassDB::bind_method(D_METHOD("set_radial_roughness", "roughness"), &PhysXHair3D::set_radial_roughness);
	ClassDB::bind_method(D_METHOD("get_radial_roughness"), &PhysXHair3D::get_radial_roughness);
	ClassDB::bind_method(D_METHOD("set_lod_enabled", "enabled"), &PhysXHair3D::set_lod_enabled);
	ClassDB::bind_method(D_METHOD("is_lod_enabled"), &PhysXHair3D::is_lod_enabled);
	ClassDB::bind_method(D_METHOD("set_lod_start_distance", "distance"), &PhysXHair3D::set_lod_start_distance);
	ClassDB::bind_method(D_METHOD("get_lod_start_distance"), &PhysXHair3D::get_lod_start_distance);
	ClassDB::bind_method(D_METHOD("set_lod_end_distance", "distance"), &PhysXHair3D::set_lod_end_distance);
	ClassDB::bind_method(D_METHOD("get_lod_end_distance"), &PhysXHair3D::get_lod_end_distance);
	ClassDB::bind_method(D_METHOD("set_lod_strand_ratio", "ratio"), &PhysXHair3D::set_lod_strand_ratio);
	ClassDB::bind_method(D_METHOD("get_lod_strand_ratio"), &PhysXHair3D::get_lod_strand_ratio);
	ClassDB::bind_method(D_METHOD("set_hair_shading", "enabled"), &PhysXHair3D::set_hair_shading);
	ClassDB::bind_method(D_METHOD("is_hair_shading"), &PhysXHair3D::is_hair_shading);
	ClassDB::bind_method(D_METHOD("set_render_material", "material"), &PhysXHair3D::set_render_material);
	ClassDB::bind_method(D_METHOD("get_render_material"), &PhysXHair3D::get_render_material);
	ClassDB::bind_method(D_METHOD("set_simulating", "simulating"), &PhysXHair3D::set_simulating);
	ClassDB::bind_method(D_METHOD("is_simulating"), &PhysXHair3D::is_simulating);
	ClassDB::bind_method(D_METHOD("set_substeps", "substeps"), &PhysXHair3D::set_substeps);
	ClassDB::bind_method(D_METHOD("get_substeps"), &PhysXHair3D::get_substeps);
	ClassDB::bind_method(D_METHOD("set_damping", "damping"), &PhysXHair3D::set_damping);
	ClassDB::bind_method(D_METHOD("get_damping"), &PhysXHair3D::get_damping);
	ClassDB::bind_method(D_METHOD("set_global_stiffness", "value"), &PhysXHair3D::set_global_stiffness);
	ClassDB::bind_method(D_METHOD("get_global_stiffness"), &PhysXHair3D::get_global_stiffness);
	ClassDB::bind_method(D_METHOD("set_global_range", "value"), &PhysXHair3D::set_global_range);
	ClassDB::bind_method(D_METHOD("get_global_range"), &PhysXHair3D::get_global_range);
	ClassDB::bind_method(D_METHOD("set_local_stiffness", "value"), &PhysXHair3D::set_local_stiffness);
	ClassDB::bind_method(D_METHOD("get_local_stiffness"), &PhysXHair3D::get_local_stiffness);
	ClassDB::bind_method(D_METHOD("set_local_iterations", "value"), &PhysXHair3D::set_local_iterations);
	ClassDB::bind_method(D_METHOD("get_local_iterations"), &PhysXHair3D::get_local_iterations);
	ClassDB::bind_method(D_METHOD("set_length_iterations", "value"), &PhysXHair3D::set_length_iterations);
	ClassDB::bind_method(D_METHOD("get_length_iterations"), &PhysXHair3D::get_length_iterations);
	ClassDB::bind_method(D_METHOD("set_vsp_coeff", "value"), &PhysXHair3D::set_vsp_coeff);
	ClassDB::bind_method(D_METHOD("get_vsp_coeff"), &PhysXHair3D::get_vsp_coeff);
	ClassDB::bind_method(D_METHOD("set_vsp_accel_threshold", "value"), &PhysXHair3D::set_vsp_accel_threshold);
	ClassDB::bind_method(D_METHOD("get_vsp_accel_threshold"), &PhysXHair3D::get_vsp_accel_threshold);
	ClassDB::bind_method(D_METHOD("set_max_speed", "value"), &PhysXHair3D::set_max_speed);
	ClassDB::bind_method(D_METHOD("get_max_speed"), &PhysXHair3D::get_max_speed);
	ClassDB::bind_method(D_METHOD("set_use_world_gravity", "enabled"), &PhysXHair3D::set_use_world_gravity);
	ClassDB::bind_method(D_METHOD("is_using_world_gravity"), &PhysXHair3D::is_using_world_gravity);
	ClassDB::bind_method(D_METHOD("set_fixed_gravity", "gravity"), &PhysXHair3D::set_fixed_gravity);
	ClassDB::bind_method(D_METHOD("get_fixed_gravity"), &PhysXHair3D::get_fixed_gravity);
	ClassDB::bind_method(D_METHOD("set_weather_wind", "enabled"), &PhysXHair3D::set_weather_wind);
	ClassDB::bind_method(D_METHOD("get_weather_wind"), &PhysXHair3D::get_weather_wind);
	ClassDB::bind_method(D_METHOD("set_wind", "wind"), &PhysXHair3D::set_wind);
	ClassDB::bind_method(D_METHOD("get_wind"), &PhysXHair3D::get_wind);
	ClassDB::bind_method(D_METHOD("set_gust", "gust"), &PhysXHair3D::set_gust);
	ClassDB::bind_method(D_METHOD("get_gust"), &PhysXHair3D::get_gust);
	ClassDB::bind_method(D_METHOD("set_guide_ratio", "ratio"), &PhysXHair3D::set_guide_ratio);
	ClassDB::bind_method(D_METHOD("get_guide_ratio"), &PhysXHair3D::get_guide_ratio);
	ClassDB::bind_method(D_METHOD("set_turbulence", "turbulence"), &PhysXHair3D::set_turbulence);
	ClassDB::bind_method(D_METHOD("get_turbulence"), &PhysXHair3D::get_turbulence);
	ClassDB::bind_method(D_METHOD("set_turbulence_size", "size"), &PhysXHair3D::set_turbulence_size);
	ClassDB::bind_method(D_METHOD("get_turbulence_size"), &PhysXHair3D::get_turbulence_size);
	ClassDB::bind_method(D_METHOD("set_collision_body", "path"), &PhysXHair3D::set_collision_body);
	ClassDB::bind_method(D_METHOD("get_collision_body"), &PhysXHair3D::get_collision_body);
	ClassDB::bind_method(D_METHOD("set_collision_cloth", "path"), &PhysXHair3D::set_collision_cloth);
	ClassDB::bind_method(D_METHOD("get_collision_cloth"), &PhysXHair3D::get_collision_cloth);
	ClassDB::bind_method(D_METHOD("set_cloth_thickness", "thickness"), &PhysXHair3D::set_cloth_thickness);
	ClassDB::bind_method(D_METHOD("get_cloth_thickness"), &PhysXHair3D::get_cloth_thickness);
	ClassDB::bind_method(D_METHOD("set_collision_cell_size", "size"), &PhysXHair3D::set_collision_cell_size);
	ClassDB::bind_method(D_METHOD("get_collision_cell_size"), &PhysXHair3D::get_collision_cell_size);
	ClassDB::bind_method(D_METHOD("set_deep_shadows", "enabled"), &PhysXHair3D::set_deep_shadows);
	ClassDB::bind_method(D_METHOD("is_deep_shadows"), &PhysXHair3D::is_deep_shadows);
	ClassDB::bind_method(D_METHOD("set_deep_shadow_light", "path"), &PhysXHair3D::set_deep_shadow_light);
	ClassDB::bind_method(D_METHOD("get_deep_shadow_light"), &PhysXHair3D::get_deep_shadow_light);
	ClassDB::bind_method(D_METHOD("set_deep_shadow_local_lights", "count"), &PhysXHair3D::set_deep_shadow_local_lights);
	ClassDB::bind_method(D_METHOD("get_deep_shadow_local_lights"), &PhysXHair3D::get_deep_shadow_local_lights);
	ClassDB::bind_method(D_METHOD("set_volume_enabled", "enabled"), &PhysXHair3D::set_volume_enabled);
	ClassDB::bind_method(D_METHOD("is_volume_enabled"), &PhysXHair3D::is_volume_enabled);
	ClassDB::bind_method(D_METHOD("set_volume_friction", "friction"), &PhysXHair3D::set_volume_friction);
	ClassDB::bind_method(D_METHOD("get_volume_friction"), &PhysXHair3D::get_volume_friction);
	ClassDB::bind_method(D_METHOD("set_volume_stiffness", "stiffness"), &PhysXHair3D::set_volume_stiffness);
	ClassDB::bind_method(D_METHOD("get_volume_stiffness"), &PhysXHair3D::get_volume_stiffness);
	ClassDB::bind_method(D_METHOD("set_strand_collision", "enabled"), &PhysXHair3D::set_strand_collision);
	ClassDB::bind_method(D_METHOD("is_strand_collision"), &PhysXHair3D::is_strand_collision);
	ClassDB::bind_method(D_METHOD("set_strand_collision_radius", "radius"), &PhysXHair3D::set_strand_collision_radius);
	ClassDB::bind_method(D_METHOD("get_strand_collision_radius"), &PhysXHair3D::get_strand_collision_radius);
	ClassDB::bind_method(D_METHOD("set_strand_collision_friction", "friction"), &PhysXHair3D::set_strand_collision_friction);
	ClassDB::bind_method(D_METHOD("get_strand_collision_friction"), &PhysXHair3D::get_strand_collision_friction);
	ClassDB::bind_method(D_METHOD("set_wetness", "wetness"), &PhysXHair3D::set_wetness);
	ClassDB::bind_method(D_METHOD("get_wetness"), &PhysXHair3D::get_wetness);
	ClassDB::bind_method(D_METHOD("set_weather_wetting", "enabled"), &PhysXHair3D::set_weather_wetting);
	ClassDB::bind_method(D_METHOD("get_weather_wetting"), &PhysXHair3D::get_weather_wetting);
	ClassDB::bind_method(D_METHOD("set_water_capacity", "mm"), &PhysXHair3D::set_water_capacity);
	ClassDB::bind_method(D_METHOD("get_water_capacity"), &PhysXHair3D::get_water_capacity);
	ClassDB::bind_method(D_METHOD("set_drying_time", "seconds"), &PhysXHair3D::set_drying_time);
	ClassDB::bind_method(D_METHOD("get_drying_time"), &PhysXHair3D::get_drying_time);
	ClassDB::bind_method(D_METHOD("set_snow", "snow"), &PhysXHair3D::set_snow);
	ClassDB::bind_method(D_METHOD("get_snow"), &PhysXHair3D::get_snow);
	ClassDB::bind_method(D_METHOD("set_snow_capacity", "mm"), &PhysXHair3D::set_snow_capacity);
	ClassDB::bind_method(D_METHOD("get_snow_capacity"), &PhysXHair3D::get_snow_capacity);
	ClassDB::bind_method(D_METHOD("set_wet_clumping", "clumping"), &PhysXHair3D::set_wet_clumping);
	ClassDB::bind_method(D_METHOD("get_wet_clumping"), &PhysXHair3D::get_wet_clumping);
	ClassDB::bind_method(D_METHOD("set_cast_hair_shadows", "enabled"), &PhysXHair3D::set_cast_hair_shadows);
	ClassDB::bind_method(D_METHOD("is_casting_hair_shadows"), &PhysXHair3D::is_casting_hair_shadows);
	ClassDB::bind_method(D_METHOD("set_cards_enabled", "enabled"), &PhysXHair3D::set_cards_enabled);
	ClassDB::bind_method(D_METHOD("is_cards_enabled"), &PhysXHair3D::is_cards_enabled);
	ClassDB::bind_method(D_METHOD("set_card_count", "count"), &PhysXHair3D::set_card_count);
	ClassDB::bind_method(D_METHOD("get_card_count"), &PhysXHair3D::get_card_count);
	ClassDB::bind_method(D_METHOD("set_card_screen_size", "size"), &PhysXHair3D::set_card_screen_size);
	ClassDB::bind_method(D_METHOD("get_card_screen_size"), &PhysXHair3D::get_card_screen_size);
	ClassDB::bind_method(D_METHOD("set_card_transition", "seconds"), &PhysXHair3D::set_card_transition);
	ClassDB::bind_method(D_METHOD("get_card_transition"), &PhysXHair3D::get_card_transition);
	ClassDB::bind_method(D_METHOD("set_thick_strands", "enabled"), &PhysXHair3D::set_thick_strands);
	ClassDB::bind_method(D_METHOD("is_thick_strands"), &PhysXHair3D::is_thick_strands);
	ClassDB::bind_method(D_METHOD("get_screen_size"), &PhysXHair3D::get_screen_size);
	ClassDB::bind_method(D_METHOD("set_card_distance", "distance"), &PhysXHair3D::set_card_distance);
	ClassDB::bind_method(D_METHOD("get_card_distance"), &PhysXHair3D::get_card_distance);
	ClassDB::bind_method(D_METHOD("is_drawing_cards"), &PhysXHair3D::is_drawing_cards);
	ClassDB::bind_method(D_METHOD("set_collision_spheres", "spheres"), &PhysXHair3D::set_collision_spheres);
	ClassDB::bind_method(D_METHOD("get_collision_spheres"), &PhysXHair3D::get_collision_spheres);
	ClassDB::bind_method(D_METHOD("reset_simulation"), &PhysXHair3D::reset_simulation);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "groom", PROPERTY_HINT_RESOURCE_TYPE, HairGroom::get_class_static()), "set_groom", "get_groom");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "root_width", PROPERTY_HINT_RANGE, "0,0.01,0.00001,or_greater,suffix:m"), "set_root_width", "get_root_width");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "tip_width", PROPERTY_HINT_RANGE, "0,0.01,0.00001,or_greater,suffix:m"), "set_tip_width", "get_tip_width");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "simplify_tolerance", PROPERTY_HINT_RANGE, "0,4,0.01,or_greater"), "set_simplify_tolerance", "get_simplify_tolerance");
	ADD_GROUP("Look", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "melanin", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_melanin", "get_melanin");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "melanin_redness", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_melanin_redness", "get_melanin_redness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "grey_ratio", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_grey_ratio", "get_grey_ratio");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "sun_bleaching", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_sun_bleaching", "get_sun_bleaching");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "roughness", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_roughness", "get_roughness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "radial_roughness", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_radial_roughness", "get_radial_roughness");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "hair_shading"), "set_hair_shading", "is_hair_shading");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "cast_hair_shadows"), "set_cast_hair_shadows", "is_casting_hair_shadows");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "deep_shadows"), "set_deep_shadows", "is_deep_shadows");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "deep_shadow_light", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "DirectionalLight3D"), "set_deep_shadow_light", "get_deep_shadow_light");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "deep_shadow_local_lights", PROPERTY_HINT_RANGE, "0,3,1"), "set_deep_shadow_local_lights", "get_deep_shadow_local_lights");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "render_material", PROPERTY_HINT_RESOURCE_TYPE, "ShaderMaterial"), "set_render_material", "get_render_material");
	ADD_GROUP("LOD", "lod_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "lod_enabled"), "set_lod_enabled", "is_lod_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lod_start_distance", PROPERTY_HINT_RANGE, "0,100,0.1,or_greater,suffix:m"), "set_lod_start_distance", "get_lod_start_distance");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lod_end_distance", PROPERTY_HINT_RANGE, "0,200,0.1,or_greater,suffix:m"), "set_lod_end_distance", "get_lod_end_distance");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lod_strand_ratio", PROPERTY_HINT_RANGE, "0.01,1,0.01"), "set_lod_strand_ratio", "get_lod_strand_ratio");
	ADD_GROUP("Simulation", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "simulating"), "set_simulating", "is_simulating");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "guide_ratio", PROPERTY_HINT_RANGE, "0.01,1,0.01"), "set_guide_ratio", "get_guide_ratio");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "substeps", PROPERTY_HINT_RANGE, "1,32,1"), "set_substeps", "get_substeps");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "damping", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_damping", "get_damping");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "global_stiffness", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_global_stiffness", "get_global_stiffness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "global_range", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_global_range", "get_global_range");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "local_stiffness", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_local_stiffness", "get_local_stiffness");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "local_iterations", PROPERTY_HINT_RANGE, "0,16,1"), "set_local_iterations", "get_local_iterations");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "length_iterations", PROPERTY_HINT_RANGE, "0,16,1"), "set_length_iterations", "get_length_iterations");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "vsp_coeff", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_vsp_coeff", "get_vsp_coeff");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "vsp_accel_threshold", PROPERTY_HINT_RANGE, "0,200,0.1,or_greater,suffix:m/s²"), "set_vsp_accel_threshold", "get_vsp_accel_threshold");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "max_speed", PROPERTY_HINT_RANGE, "0,50,0.1,or_greater,suffix:m/s"), "set_max_speed", "get_max_speed");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "use_world_gravity"), "set_use_world_gravity", "is_using_world_gravity");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "fixed_gravity", PROPERTY_HINT_NONE, "suffix:m/s²"), "set_fixed_gravity", "get_fixed_gravity");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "wind", PROPERTY_HINT_NONE, "suffix:m/s"), "set_wind", "get_wind");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "weather_wind"), "set_weather_wind", "get_weather_wind");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "gust", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_gust", "get_gust");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "turbulence", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_turbulence", "get_turbulence");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "turbulence_size", PROPERTY_HINT_RANGE, "0.01,1,0.01,or_greater,suffix:m"), "set_turbulence_size", "get_turbulence_size");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_VECTOR4_ARRAY, "collision_spheres"), "set_collision_spheres", "get_collision_spheres");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "collision_body", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "MeshInstance3D"), "set_collision_body", "get_collision_body");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "collision_cloth", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "PhysXSkinnedCloth3D"), "set_collision_cloth", "get_collision_cloth");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cloth_thickness", PROPERTY_HINT_RANGE, "0,0.05,0.001,or_greater,suffix:m"), "set_cloth_thickness", "get_cloth_thickness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "collision_cell_size", PROPERTY_HINT_RANGE, "0.001,0.1,0.001,suffix:m"), "set_collision_cell_size", "get_collision_cell_size");
	ADD_GROUP("Volume", "volume_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "volume_enabled"), "set_volume_enabled", "is_volume_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "volume_friction", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_volume_friction", "get_volume_friction");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "volume_stiffness", PROPERTY_HINT_RANGE, "0,2,0.01,or_greater"), "set_volume_stiffness", "get_volume_stiffness");
	ADD_GROUP("Strand Collision", "strand_collision");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "strand_collision"), "set_strand_collision", "is_strand_collision");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "strand_collision_radius", PROPERTY_HINT_RANGE, "0,0.01,0.0001,or_greater,suffix:m"), "set_strand_collision_radius", "get_strand_collision_radius");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "strand_collision_friction", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_strand_collision_friction", "get_strand_collision_friction");
	ADD_GROUP("Cards", "card");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "cards_enabled"), "set_cards_enabled", "is_cards_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "card_count", PROPERTY_HINT_RANGE, "1,1024,1"), "set_card_count", "get_card_count");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "card_screen_size", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_card_screen_size", "get_card_screen_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "card_transition", PROPERTY_HINT_RANGE, "0,3,0.01,suffix:s"), "set_card_transition", "get_card_transition");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "thick_strands"), "set_thick_strands", "is_thick_strands");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "card_distance", PROPERTY_HINT_RANGE, "0,200,0.1,or_greater,suffix:m"), "set_card_distance", "get_card_distance");
	ADD_GROUP("Wet", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wetness", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_wetness", "get_wetness");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "weather_wetting"), "set_weather_wetting", "get_weather_wetting");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "water_capacity", PROPERTY_HINT_RANGE, "0.1,5,0.1,or_greater,suffix:mm"), "set_water_capacity", "get_water_capacity");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "drying_time", PROPERTY_HINT_RANGE, "60,14400,1,or_greater,suffix:s"), "set_drying_time", "get_drying_time");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "snow", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_snow", "get_snow");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "snow_capacity", PROPERTY_HINT_RANGE, "0.1,10,0.1,or_greater,suffix:mm"), "set_snow_capacity", "get_snow_capacity");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wet_clumping", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_wet_clumping", "get_wet_clumping");
}

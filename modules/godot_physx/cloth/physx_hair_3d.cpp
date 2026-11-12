/**************************************************************************/
/*  physx_hair_3d.cpp                                                     */
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

#include "physx_hair_3d.h"

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
uniform vec2 fibre_roughness = vec2(0.3, 0.3);
uniform float root_width = 0.0004;
uniform float tip_width = 0.0001;
uniform float lod_width_scale = 1.0; // raster: 1 / the share of strands drawn (distance LOD)
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

void vertex() {
	strand_seed = fract(sin(floor((CUSTOM0.x + 0.5) / float(max(slot_points, 1))) * 12.9898) * 43758.5453);
	vec4 here = hair_point(CUSTOM0.x);
	vec4 before = hair_point(CUSTOM0.y);
	vec4 after = hair_point(CUSTOM0.z);
	bool lod_dropped = here.w > 1.5;
	float along = lod_dropped ? here.w - 2.0 : here.w;
	vec3 p = here.xyz;
	vec3 t = (after.w < 0.0 ? p : after.xyz) - (before.w < 0.0 ? p : before.xyz);
	t = dot(t, t) > 1e-14 ? normalize(t) : vec3(0.0, 1.0, 0.0);
	vec3 a = normalize(cross(t, abs(t.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
	vec3 across = CUSTOM1.w > 0.5 ? cross(t, a) : a;
	// Past the strand's end (a shorter strand padded to the slot) the width is zero: nothing drawn.
	float half_width = along < 0.0 ? 0.0 : 0.5 * mix(root_width, tip_width, along);
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
		// The deep opacity maps: how much hair (and whether the head or body) lies between this
		// strand point and each light, smooth across the layers and filtered across 4 cells.
		// Per point, not per fragment: a strand is far thinner than a cell, and the segments
		// are about as long as one.
		vec3 world = (MODEL_MATRIX * vec4(p, 1.0)).xyz;
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
			dom_trans[k] = trans;
			dom_past[k] = front < 1e8 ? max(tl - front, 0.0) : -1.0;
		}
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
	ALBEDO = hair_color.rgb;
	ROUGHNESS = fibre_roughness.x;
	ANISOTROPY = 0.8;
	ANISOTROPY_FLOW = vec2(1.0, 0.0); // The highlight runs across the strands.
	HAIR = 1.0;
	HAIR_ABSORPTION = fibre_absorption;
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
		coverage *= 1.0 - 0.91 * dot(sqrt(hair_color.rgb), vec3(0.2126, 0.7152, 0.0722));
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
	ALPHA = coverage;
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
	vec3 T = normalize(strand_tangent);
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
			vec4 sl = dom_slot[k * 4 + 3];
			bool is_local = dom_slot[k * 4 + 1].w > 0.5;
			if (is_local == LIGHT_IS_DIRECTIONAL) {
				continue;
			}
			vec3 to_light = is_local ? normalize(dom_slot[k * 4].xyz + sl.xyz * sl.w - dom_world) : sl.xyz;
			if (dot(lw, to_light) > 0.999) {
				attenuation *= dom_trans[k];
				shadow = LIGHT_IS_DIRECTIONAL ? clamp(attenuation, 0.0, 1.0) : dom_trans[k];
				break;
			}
		}
	}
	s += sqrt(base) * scatter * pow(base / luma, vec3(1.0 - shadow));
	SPECULAR_LIGHT += max(s, vec3(0.0)) * LIGHT_COLOR * attenuation;
}
#endif
)";

} // namespace

Ref<Shader> PhysXHair3D::render_shaders[2];

void PhysXHair3D::free_shared() {
	render_shaders[0].unref();
	render_shaders[1].unref();
	HairStrandSolverGPU::free_shared();
}

// Melanin's absorption per fibre diameter, as BaseMaterial3D's Hair (Cycles' eumelanin and
// pheomelanin coefficients).
Vector3 PhysXHair3D::_fibre_absorption() const {
	const float m = -Math::log(MAX(1.0f - melanin, 1e-4f));
	const Vector3 eu(0.506f, 0.841f, 1.653f);
	const Vector3 pheo(0.343f, 0.733f, 1.924f);
	return (eu * (1.0f - melanin_redness) + pheo * melanin_redness) * m;
}

// The colour a mass of such fibres shows (Chiang et al. 2016, section 4.2), for the rasterizer.
Color PhysXHair3D::_mass_color() const {
	const float b = CLAMP(radial_roughness, 0.01f, 1.0f);
	const float d = 5.969f - 0.215f * b + 2.532f * b * b - 10.73f * b * b * b + 5.574f * b * b * b * b + 0.245f * b * b * b * b * b;
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
PackedFloat32Array PhysXHair3D::_follow_data(const PackedVector3Array &p_points, const PackedInt32Array &p_counts) const {
	const int sc = p_counts.size();
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
	const int guide_n = guide_ratio >= 1.0f ? sc : CLAMP((int)Math::ceil(sc * guide_ratio), 1, sc);
	LocalVector<int> order;
	order.resize(sc);
	for (int i = 0; i < sc; i++) {
		order[i] = i;
	}
	RandomPCG rng{ 0x6a1du };
	for (int i = sc - 1; i > 0; i--) {
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
	copy_material->set_shader_parameter("hair_color", _mass_color());
	copy_material->set_shader_parameter("fibre_absorption", _fibre_absorption());
	copy_material->set_shader_parameter("fibre_roughness", Vector2(roughness, radial_roughness));
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
	if (render_instance) {
		render_instance->queue_free();
		render_instance = nullptr;
	}
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

	const PackedVector3Array pts = groom->get_points();
	const PackedInt32Array counts = groom->get_strand_point_counts();

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
			start += n;
		}
	}

	// The points the strands are drawn through: where a strand runs straight its middle points are
	// left out of the render mesh (Douglas-Peucker on the groomed strand, as Unreal's groom LOD
	// decimation), within simplify_tolerance of the root width, below what a strand's own width
	// shows. Every point still simulates and casts into the deep shadows.
	PackedInt32Array render_points;
	const int slot_points = _render_points(pts, counts, render_points);

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
	if (slot_points >= 2) {
		verts.resize(strand_count * slot_points * 4);
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
			for (int k = 0; k < slot_points; k++) {
				const int t = slot * slot_points + k;
				// Rest position: the groom in groom order (vertex() replaces it).
				const Vector3 p = pts[MIN(start + MIN(k, n - 1), pts.size() - 1)];
				for (int strip = 0; strip < 2; strip++) {
					for (int side = 0; side < 2; side++) {
						const int vi = t * 4 + strip * 2 + side;
						v[vi] = p;
						nr[vi] = Vector3(0, 0, 1);
						tg[vi * 4 + 0] = 1.0f;
						tg[vi * 4 + 1] = 0.0f;
						tg[vi * 4 + 2] = 0.0f;
						tg[vi * 4 + 3] = 1.0f;
						uv[vi] = Vector2(float(side), float(k) / float(MAX(slot_points - 1, 1)));
						c0[vi * 4 + 0] = float(t);
						c0[vi * 4 + 1] = float(slot * slot_points + MAX(k - 1, 0));
						c0[vi * 4 + 2] = float(slot * slot_points + MIN(k + 1, slot_points - 1));
						c0[vi * 4 + 3] = side == 0 ? -1.0f : 1.0f;
						c1[vi * 4 + 0] = 0.0f;
						c1[vi * 4 + 1] = 0.0f;
						c1[vi * 4 + 2] = 0.0f;
						c1[vi * 4 + 3] = float(strip);
					}
				}
			}
			for (int k = 0; k + 1 < slot_points; k++) {
				for (int strip = 0; strip < 2; strip++) {
					const int v0 = (slot * slot_points + k) * 4 + strip * 2;
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
	solver->build(rest, strands, _follow_data(pts, counts), slot_points, render_points);
	pos_texture.instantiate();
	pos_texture->set_texture_rd_rid(solver->get_position_texture_rd_rid());
	prev_pos_texture.instantiate();
	prev_pos_texture->set_texture_rd_rid(solver->get_previous_position_texture_rd_rid());
	deep_shadow_texture.instantiate();
	deep_shadow_texture->set_texture_rd_rid(solver->get_deep_shadow_texture_rd_rid());
	draw_slot_points = MAX(slot_points, 1);
	_make_material();
	built = true;
	_step(0.0);
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
	s.body_cell_size = collision_cell_size;
	s.volume_enabled = volume_enabled;
	s.volume_friction = volume_friction;
	s.volume_stiffness = volume_stiffness;
	s.collision_radius = strand_collision ? strand_collision_radius : 0.0f;
	s.collision_friction = strand_collision_friction;
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
	if (copy_material.is_valid() && !Math::is_equal_approx(1.0f / s.lod_keep, lod_width_scale)) {
		lod_width_scale = 1.0f / s.lod_keep;
		copy_material->set_shader_parameter("lod_width_scale", lod_width_scale);
	}
	// Deep opacity maps (hair_shading's light() applies them): one toward the directional light,
	// and one from each of the omni and spot lights that light the hair most.
	int maps = 0;
	float slots[HairStrandSolverGPU::DOM_SLOTS * 16] = {};
	if (deep_shadows && hair_shading && render_instance && (render_material.is_null() || render_material->get_shader().is_null())) {
		const AABB box = render_instance->get_global_transform().xform(render_instance->get_aabb());
		HairStrandSolver::Settings::DeepShadowLight hair;
		hair.center = box.get_center();
		hair.radius = 0.5f * box.size.length();
		s.root_width = root_width;
		s.tip_width = tip_width;
		if (hair.radius > 0.0f) {
			if (DirectionalLight3D *light = _deep_shadow_light()) {
				HairStrandSolver::Settings::DeepShadowLight dl = hair;
				dl.light = light->get_global_transform().basis.get_column(2).normalized();
				if (HairStrandSolver::deep_shadow_slot(dl, slots + maps * 16)) {
					s.dom_lights.push_back(dl);
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
		}
		if (maps != deep_shadow_on) {
			deep_shadow_on = maps;
			copy_material->set_shader_parameter("dom_maps", maps);
		}
	}
	solver->step(xform, p_delta, substeps, s);
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
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "roughness", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_roughness", "get_roughness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "radial_roughness", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_radial_roughness", "get_radial_roughness");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "hair_shading"), "set_hair_shading", "is_hair_shading");
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
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "collision_cell_size", PROPERTY_HINT_RANGE, "0.001,0.1,0.001,suffix:m"), "set_collision_cell_size", "get_collision_cell_size");
	ADD_GROUP("Volume", "volume_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "volume_enabled"), "set_volume_enabled", "is_volume_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "volume_friction", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_volume_friction", "get_volume_friction");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "volume_stiffness", PROPERTY_HINT_RANGE, "0,2,0.01,or_greater"), "set_volume_stiffness", "get_volume_stiffness");
	ADD_GROUP("Strand Collision", "strand_collision");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "strand_collision"), "set_strand_collision", "is_strand_collision");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "strand_collision_radius", PROPERTY_HINT_RANGE, "0,0.01,0.0001,or_greater,suffix:m"), "set_strand_collision_radius", "get_strand_collision_radius");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "strand_collision_friction", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_strand_collision_friction", "get_strand_collision_friction");
}

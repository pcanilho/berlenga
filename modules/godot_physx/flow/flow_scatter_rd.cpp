/**************************************************************************/
/*  flow_scatter_rd.cpp                                                   */
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

#include "flow_scatter_rd.h"

#include "servers/rendering/renderer_rd/renderer_scene_render_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_device_binds.h"
#include "servers/rendering/rendering_server_globals.h"

namespace {

// Shared by both passes (binding 5 in each), spliced in where a pass has @PARAMS_BLOCK@. A plain string, not a
// macro: a raw string literal can't span lines inside a #define on GCC / Clang (MSVC took it).
const char *PARAMS_BLOCK = R"(
layout(set = 0, binding = 5, std140) uniform Params {
	mat4 inv_view_projection; // clip -> world meters
	vec4 camera; // xyz meters; w: units per meter
	uvec4 level[10]; // NvFlowSparseLevelParams
	vec4 block_size; // Flow units; w: step scale (cells)
	ivec4 counts; // x: layerAndLevel, y: lights, z: shadow steps, w: max steps
	vec4 world_min; // Flow units
	vec4 world_max;
	vec4 albedo; // rgb; w: strength
	vec4 misc; // x: sigma per meter per unit smoke, y: anisotropy, z: shadow reach (m), w: 1 = sample light shadows
	ivec4 size; // xy: the frame; zw: the scattering texture (the frame / res.x)
	ivec4 res; // x: frame pixels per scattering texel, each way; y: frame index (jitter); z: 1 = temporal blending, w: 1 = a history to blend
	vec4 ambient; // rgb: ambient color (linear, * energy); w: sky contribution
	vec4 sky; // xy: the sky octmap's uv border; z: its last mip; w: 1 = ambient light on, plus sky energy * 2 (see below)
	vec4 sky_xform[3]; // rows: world direction -> sky direction
	mat4 world_to_view; // meters -> the view space Godot's directional light buffer is in
	mat4 view_to_world;
	ivec4 sun; // x: directional lights to use (0: none); y: 1 = sample their shadow cascades
	mat4 prev_view_projection; // world meters -> the previous frame's clip space (temporal blending)
	vec4 sun_map_xform[3]; // rows: world meters -> the sun's volumetric shadow map (u, v, depth in 0..1)
	vec4 sun_map; // xyz: world direction toward the light it is built from; w: 1 = present
	vec4 sky_map_xform[3]; // rows: world meters -> the sky's volumetric shadow map (built looking down)
	vec4 sky_map; // x: single-scattering albedo; y: ground albedo; z: weight of the scattering orders past the octaves; w: 1 = present
} p;

// The world distance (meters) to what a frame pixel shows (reverse Z: 0 is the far plane), at most
// SKY_DISTANCE: it is kept in a 16-bit float, whose largest value is 65504, and a larger one (the sky)
// would no longer match itself there, so the sky's texels would neither blend over time nor upsample.
const float SKY_DISTANCE = 60000.0;
float scene_distance(ivec2 frame_px, float depth) {
	if (depth <= 1e-7) {
		return SKY_DISTANCE;
	}
	vec2 uv = (vec2(frame_px) + 0.5) / vec2(p.size.xy);
	vec4 h = p.inv_view_projection * vec4(uv * 2.0 - 1.0, depth, 1.0);
	return min(length(h.xyz / h.w - p.camera.xyz), SKY_DISTANCE);
}
)";

const char *SCATTER_SHADER = R"(
#[compute]
#version 450

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
@PARAMS_BLOCK@

// Flow's density atlas (r: temperature, b: sun light, a: smoke) and its
// sparse table; see NvFlowShader.hlsli for the lookups ported below.
layout(set = 0, binding = 0) uniform sampler3D density_tex;
layout(set = 0, binding = 1, std430) restrict readonly buffer Table {
	uint table_data[];
};
layout(set = 0, binding = 2) uniform sampler2D depth_tex;
// The scattered light (rgb) and the scene distance its texel stands for (a,
// meters), at the scattering resolution; the upsample pass adds it to the frame.
layout(set = 0, binding = 3, rgba16f) uniform restrict writeonly image2D scatter_img;

struct Light {
	vec4 position_range;
	vec4 color_decay;
	vec4 direction_cutoff;
	vec4 spot;
	mat4 shadow_matrix;
	vec4 atlas_rect;
	vec4 shadow; // x: opacity, y: bias, zw: omni flip offset
};
layout(set = 0, binding = 4, std430) restrict readonly buffer Lights {
	Light lights[];
};

// Godot's shadow atlas (omni: dual paraboloid; spot: projected).
layout(set = 0, binding = 6) uniform sampler2D shadow_atlas;

// Godot's directional lights for this frame (as light_data_inc.glsl; std140, view space) and their
// shadow cascades, as its volumetric fog reads them.
struct DirectionalLightData {
	vec3 direction; // toward the light
	float energy;
	vec3 color;
	float size;
	float specular;
	uint mask;
	float softshadow_angle;
	float soft_shadow_scale;
	bool blend_splits;
	float shadow_opacity;
	float fade_from;
	float fade_to;
	uvec2 pad;
	uint bake_mode;
	float volumetric_fog_energy;
	vec4 shadow_bias;
	vec4 shadow_normal_bias;
	vec4 shadow_transmittance_bias;
	vec4 shadow_z_range;
	vec4 shadow_range_begin;
	vec4 shadow_split_offsets;
	mat4 shadow_matrix1;
	mat4 shadow_matrix2;
	mat4 shadow_matrix3;
	mat4 shadow_matrix4;
	vec2 uv_scale1;
	vec2 uv_scale2;
	vec2 uv_scale3;
	vec2 uv_scale4;
	vec4 projector_rect;
	vec4 projector_u;
	vec4 projector_v;
	vec4 volume_shadow_u;
	vec4 volume_shadow_v;
	vec4 volume_shadow_w;
};
layout(set = 0, binding = 8, std140) uniform DirectionalLights {
	DirectionalLightData data[MAX_DIRECTIONAL_LIGHTS];
} directional_lights;
layout(set = 0, binding = 9) uniform sampler2D directional_shadow_atlas;
#ifndef SHADOW_PASS
// The gas's optical depth from the sun's side (the volumetric shadow map, see the shadow pass below).
layout(set = 0, binding = 11) uniform sampler3D sun_map_tex;
// The same from straight above: the gas between a point and the sky.
layout(set = 0, binding = 12) uniform sampler3D sky_map_tex;
#endif

// The sky's radiance octmap (Godot's reflections and volumetric fog read it).
#ifdef SKY_ARRAY
layout(set = 0, binding = 7) uniform sampler2DArray sky_tex;
#else
layout(set = 0, binding = 7) uniform sampler2D sky_tex;
#endif

vec2 oct_wrap(vec2 v) {
	return (1.0 - abs(v.yx)) * mix(vec2(-1.0), vec2(1.0), greaterThanEqual(v.xy, vec2(0.0)));
}

// As oct_inc.glsl's vec3_to_oct_with_border.
vec2 sky_uv(vec3 n) {
	n /= (abs(n.x) + abs(n.y) + abs(n.z));
	vec2 uv = n.z >= 0.0 ? n.xy : oct_wrap(n.xy);
	uv = uv * 0.5 + 0.5;
	return uv * p.sky.y + p.sky.x;
}

vec3 sky_radiance(vec3 world_dir, float lod) {
	vec3 d = vec3(dot(p.sky_xform[0].xyz, world_dir), dot(p.sky_xform[1].xyz, world_dir), dot(p.sky_xform[2].xyz, world_dir));
#ifdef SKY_ARRAY
	return textureLod(sky_tex, vec3(sky_uv(d), lod), 0.0).rgb;
#else
	return textureLod(sky_tex, sky_uv(d), lod).rgb;
#endif
}

// The average light of the sky's upper (side 1) or lower (side -1) half, as the gas takes it in from all
// around: five taps (straight up or down and four at 30 degrees from the horizon) two mips above the last.
// The last mip alone averages the whole sphere, sky and ground together, and loses the sky's blue.
vec3 sky_hemisphere(float side) {
	float lod = max(p.sky.z - 4.0, 0.0);
	const float c = 0.8660254;
	const float e = 0.5;
	return (sky_radiance(vec3(0.0, side, 0.0), lod) + sky_radiance(vec3(c, e * side, 0.0), lod) + sky_radiance(vec3(-c, e * side, 0.0), lod) + sky_radiance(vec3(0.0, e * side, c), lod) + sky_radiance(vec3(0.0, e * side, -c), lod)) * 0.2;
}

#define BLOCK_DIM_BITS (p.level[1].xyz)
#define TABLE_DIM_LESS_ONE (p.level[2].xyz)
#define TABLE_DIM_BITS_X (p.level[3].x)
#define TABLE_DIM_BITS_XY (p.level[3].y)
#define LOCATION_OFFSET (p.level[3].w)
#define BLOCK_LEVEL_OFFSET_GLOBAL (p.level[4].z)
#define DIM_INV (uintBitsToFloat(p.level[7].xyz))

const uint NO_BLOCK = 0xFFFFFFFFu;

uint location_to_block(ivec4 loc) {
	uvec3 bucket = uvec3(loc.xyz) & TABLE_DIM_LESS_ONE;
	uint bucket1d = (bucket.z << TABLE_DIM_BITS_XY) | (bucket.y << TABLE_DIM_BITS_X) | bucket.x;
	uint range_begin = table_data[(bucket1d << 1u) + 0u];
	uint range_end = table_data[(bucket1d << 1u) + 1u];
	for (uint b = range_begin; b < range_end; b++) {
		uint i = (b << 2u) + LOCATION_OFFSET;
		if (int(table_data[i + 0u]) == loc.x && int(table_data[i + 1u]) == loc.y && int(table_data[i + 2u]) == loc.z && int(table_data[i + 3u]) == loc.w) {
			return b;
		}
	}
	return NO_BLOCK;
}

// One block lookup remembered (consecutive samples mostly share a block).
struct Accessor {
	ivec4 location;
	ivec3 offset; // virtual cell -> atlas texel
	bool valid;
};

void accessor_update(inout Accessor a, ivec4 loc) {
	if (loc == a.location) {
		return;
	}
	a.location = loc;
	uint b = location_to_block(loc);
	a.valid = false;
	if (b != NO_BLOCK) {
		uint v = table_data[b + BLOCK_LEVEL_OFFSET_GLOBAL];
		a.valid = (v >> 31u) != 0u;
		ivec3 base = ivec3(int((v << 1u) | 1u) & 0xFFF, int((v >> 10u) | 1u) & 0x7FF, int((v >> 20u) | 1u) & 0x7FF);
		a.offset = base - (loc.xyz << ivec3(BLOCK_DIM_BITS));
	}
}

// Smoke at a point (Flow units); false where no block holds gas.
bool smoke_at(vec3 pos, vec3 cell_inv, inout Accessor a, out float r_smoke) {
	vec3 vidxf = pos * cell_inv;
	ivec4 loc = ivec4(ivec3(floor(vidxf)) >> ivec3(BLOCK_DIM_BITS), p.counts.x);
	accessor_update(a, loc);
	if (!a.valid) {
		r_smoke = 0.0;
		return false;
	}
	r_smoke = textureLod(density_tex, (vidxf + vec3(a.offset)) * DIM_INV, 0.0).a;
	return true;
}

bool intersect_box(vec3 ro, vec3 rd_inv, vec3 bmin, vec3 bmax, out float t0, out float t1) {
	vec3 a = (bmin - ro) * rd_inv;
	vec3 b = (bmax - ro) * rd_inv;
	vec3 lo = min(a, b);
	vec3 hi = max(a, b);
	t0 = max(max(lo.x, lo.y), lo.z);
	t1 = min(min(hi.x, hi.y), hi.z);
	return t1 > max(t0, 0.0);
}

// Soft edge of a shadow in the gas, as Godot's volumetric fog (INV_FOG_FADE).
const float SHADOW_FADE = 10.0;

// How much of a light reaches pos_m (meters) past scene objects: its shadow, read
// from Godot's shadow atlas like volumetric_fog_process.glsl does.
float light_shadow(Light L, vec3 pos_m) {
	float opacity = L.shadow.x;
	if (opacity <= 0.001) {
		return 1.0;
	}
	float inv_range = 1.0 / L.position_range.w;
	if (L.direction_cutoff.w < -1.5) {
		// Omni: dual paraboloid.
		vec4 uv_rect = L.atlas_rect;
		vec3 local = (L.shadow_matrix * vec4(pos_m, 1.0)).xyz;
		float shadow_len = length(local);
		vec3 dir = shadow_len > 0.0 ? local / shadow_len : vec3(0.0, 0.0, 1.0);
		if (dir.z >= 0.0) {
			uv_rect.xy += L.shadow.zw;
		}
		dir.z = 1.0 + abs(dir.z);
		vec3 pos = vec3(dir.xy / dir.z, shadow_len - L.shadow.y);
		pos.z = 1.0 - pos.z * inv_range;
		pos.xy = uv_rect.xy + (pos.xy * 0.5 + 0.5) * uv_rect.zw;
		float depth = textureLod(shadow_atlas, pos.xy, 0.0).r;
		return mix(1.0 - opacity, 1.0, exp(min(0.0, pos.z - depth) / inv_range * SHADOW_FADE));
	}
	// Spot: projected.
	vec4 splane = L.shadow_matrix * vec4(pos_m, 1.0);
	splane.z -= L.shadow.y;
	float w = abs(splane.w) > 1e-6 ? splane.w : (splane.w >= 0.0 ? 1e-6 : -1e-6);
	splane /= w;
	vec2 uv = splane.xy * L.atlas_rect.zw + L.atlas_rect.xy;
	float depth = textureLod(shadow_atlas, uv, 0.0).r;
	return mix(1.0 - opacity, 1.0, exp(min(0.0, splane.z - depth) / inv_range * SHADOW_FADE));
}

// How much of directional light i reaches view-space point vpos past scene objects: its shadow cascades,
// as volumetric_fog_process.glsl reads them.
float sun_shadow(uint i, vec3 vpos) {
	float opacity = directional_lights.data[i].shadow_opacity;
	if (opacity <= 0.001 || p.sun.y == 0) {
		return 1.0;
	}
	float depth_z = -vpos.z;
	vec4 v = vec4(vpos, 1.0);
	vec4 coord;
	float z_range;
	if (depth_z < directional_lights.data[i].shadow_split_offsets.x) {
		coord = directional_lights.data[i].shadow_matrix1 * v;
		z_range = directional_lights.data[i].shadow_z_range.x;
	} else if (depth_z < directional_lights.data[i].shadow_split_offsets.y) {
		coord = directional_lights.data[i].shadow_matrix2 * v;
		z_range = directional_lights.data[i].shadow_z_range.y;
	} else if (depth_z < directional_lights.data[i].shadow_split_offsets.z) {
		coord = directional_lights.data[i].shadow_matrix3 * v;
		z_range = directional_lights.data[i].shadow_z_range.z;
	} else {
		coord = directional_lights.data[i].shadow_matrix4 * v;
		z_range = directional_lights.data[i].shadow_z_range.w;
	}
	coord /= coord.w;
	float depth = textureLod(directional_shadow_atlas, coord.xy, 0.0).r;
	float shadow = exp(min(0.0, coord.z - depth) * z_range * SHADOW_FADE);
	shadow = mix(shadow, 1.0, smoothstep(directional_lights.data[i].fade_from, directional_lights.data[i].fade_to, vpos.z));
	return mix(1.0 - opacity, 1.0, shadow);
}

// Light arriving through optical depth tau, single scattering plus the multiple-scattering octaves of
// Wrenninge et al. 2013 and Hillaire 2016 (octave i: scattering and extinction scaled by 0.5^i), so
// dense smoke isn't black on its far side as single scattering makes it.
// Henyey-Greenstein, normalized over the sphere (1 / 4 pi when isotropic), as Godot's volumetric fog and
// the path tracer (rt_volume_hg) weigh a light's scattering. (Ambient light comes from all around: no phase.)
float hg_phase(float c, float g) {
	float g2 = g * g;
	return (1.0 - g2) / (4.0 * 3.14159265 * pow(max(1.0 + g2 - 2.0 * g * c, 1e-4), 1.5));
}

// The multiple-scattering octaves (Wrenninge et al. 2013), times the single-scattering albedo per channel: the
// gas's colour times its scattering albedo (sky_map.x), as the path tracer weighs each scatter. Octave i stands
// for higher scattering orders, so it is weighed by that albedo to the i-th power relative to 0.9: the colour
// compounds over the orders as it does under path tracing (a grey-blue gas turns bluer and darker inside, a
// near-white one such as snow powder keeps nearly all of its many-times-scattered light). The orders past the
// third come as one more, wider octave: a geometric series in sky_map.z times the albedo.
// Fitted to the path tracer (docs/ideas/physx.md, "Flow raster light matches path tracing, refit"), each
// light on its own on test puffs under a real sky:
// - SUN_MS_GAIN: the octaves' light, over their sum.
// - SKY_MS_SHARE: the sky's and the ground's light already arrive from all around, so the octaves overcount it;
//   this share of it is left.
// - SKY_GAIN: the sky's light against the sun's (the path tracer's sky-only and sun-and-sky puffs, by colour as
//   well as brightness: too little sky leaves the gas neutral grey where the path tracer's has the sky's blue).
// - SKY_OCCLUSION: the sky reaches a point from the whole hemisphere, mostly along shorter paths out of the
//   gas than the one straight up the sky map holds.
// The densest smoke the gas is taken as: the path tracer's density majorant (RTSparseVolume::density_majorant),
// which clamps the smoke it tracks to it, and Flow's own opacity march (patched, misc/physx_patches 0010).
const float SMOKE_MAX = 2.0;
const float SUN_MS_GAIN = 0.7;
const float SKY_MS_SHARE = 0.21;
const float SKY_GAIN = 3.0;
// Fitted on test puffs lit from the side and from behind the camera: 0, the octaves scatter evenly.
const float MS_ECCENTRICITY = 0.0;
const float SKY_OCCLUSION = 0.15;

// A light's in-scattering through optical depth tau, cos(view, light) c: single scattering and the octaves,
// each with its own phase. The orders past the first need gas around the point to scatter in: tau_around is
// the gas along the light's line through the point (both ways), and where it is thin the light is single
// scattering only (a powder-style ramp, as Schneider 2015 darkens thin cloud edges), so wisps stay see-through
// as under path tracing while the lit face of a deep cloud keeps its many-times-scattered light. Each
// scattering order spreads the light further from the light's direction, so octave i
// takes the anisotropy g * MS_ECCENTRICITY^i (Wrenninge et al. 2013's eccentricity attenuation, as Frostbite's
// clouds, Hillaire 2016): a cloud lit from behind the camera, where the first scattering's forward-leaning phase
// sends little back, still glows with its many-times-scattered light, as under path tracing.
// The single-scattering albedo per channel (raygen.glsl: smoke_color * albedo at each scatter).
vec3 ss_albedo() {
	return p.albedo.rgb * p.sky_map.x;
}
// The orders past the third: the sum over n >= 3 of q^n, q = sky_map.z * albedo.
vec3 octave_tail(vec3 alb) {
	vec3 q = min(p.sky_map.z * alb, vec3(0.98));
	return q * q * q / (1.0 - q);
}
vec3 ms_light(float tau, float tau_around, float c, float g) {
	vec3 alb = ss_albedo();
	vec3 a = alb * (1.0 / 0.9);
	float thick = 1.0 - exp(-2.0 * tau_around);
	float e = MS_ECCENTRICITY;
	return (hg_phase(c, g) * exp(-tau) + thick * (hg_phase(c, g * e) * 0.5 * a * exp(-0.5 * tau) + hg_phase(c, g * e * e) * 0.25 * a * a * exp(-0.25 * tau) + hg_phase(c, g * e * e * e) * octave_tail(alb) * exp(-0.125 * tau))) * (alb * SUN_MS_GAIN);
}

// The light from all around (sky above, ground below) through optical depth tau.
vec3 ms_ambient(float tau) {
	vec3 alb = ss_albedo();
	vec3 a = alb * (1.0 / 0.9);
	float thick = 1.0 - exp(-2.0 * tau);
	return SKY_MS_SHARE * (exp(-tau) + thick * (0.5 * a * exp(-0.5 * tau) + 0.25 * a * a * exp(-0.25 * tau) + octave_tail(alb) * exp(-0.125 * tau))) * alb;
}

float ign(vec2 px) {
	return fract(52.9829189 * fract(dot(px, vec2(0.06711056, 0.00583715))));
}

vec3 march(ivec2 px, ivec2 frame_px, float depth) {
	float upm = p.camera.w;
	vec2 uv = (vec2(frame_px) + 0.5) / vec2(p.size.xy);

	// The view ray, in Flow units, out to the scene (reverse Z: 0 is the far plane).
	vec4 near_h = p.inv_view_projection * vec4(uv * 2.0 - 1.0, 1.0, 1.0);
	vec4 far_h = p.inv_view_projection * vec4(uv * 2.0 - 1.0, max(depth, 1e-7), 1.0);
	vec3 cam = p.camera.xyz * upm;
	vec3 hit = far_h.xyz / far_h.w * upm;
	vec3 dir = hit - near_h.xyz / near_h.w * upm;
	float dir_len = length(dir);
	if (dir_len <= 0.0) {
		return vec3(0.0);
	}
	dir /= dir_len;
	float scene_t = depth > 1e-7 ? length(hit - cam) : 1e20;

	vec3 rd_inv = 1.0 / dir;
	float t0;
	float t1;
	if (!intersect_box(cam, rd_inv, p.world_min.xyz, p.world_max.xyz, t0, t1)) {
		return vec3(0.0);
	}
	t0 = max(t0, 0.0);
	t1 = min(t1, scene_t);
	if (t1 <= t0) {
		return vec3(0.0);
	}

	vec3 block = p.block_size.xyz;
	vec3 cell = block / vec3(p.level[0].xyz + 1u);
	vec3 cell_inv = 1.0 / cell;
	float step_len = min(min(cell.x, cell.y), cell.z) * p.block_size.w;
	float sigma = p.misc.x / upm; // per Flow unit, per unit smoke
	float g = p.misc.y;
	int light_count = p.counts.y;
	int shadow_steps = p.counts.z;

	Accessor view_acc;
	view_acc.location = ivec4(0x40000000);
	view_acc.valid = false;
	Accessor light_acc;
	light_acc.location = ivec4(0x40000000);
	light_acc.valid = false;

	// The ambient light reaching the gas from all around, as Godot's volumetric fog has it:
	// the ambient color mixed with the sky (one isotropic tap up, one along the scattering
	// direction, weighed by the anisotropy) by the sky contribution.
	bool ambient_on = p.sky.w > 0.5;
	vec3 ambient = vec3(0.0);
	// The light from below (the sky's lower half: the ground it stands for, often bright snow), which lights the
	// underside of the gas as the path tracer's bounces off the ground do.
	vec3 ambient_down = vec3(0.0);
	if (ambient_on) {
		ambient = p.ambient.rgb;
		ambient_down = p.ambient.rgb;
		if (p.ambient.w > 0.0) {
			float sky_energy = p.sky.w - 1.0;
			vec3 isotropic = sky_hemisphere(1.0);
			vec3 isotropic_down = sky_hemisphere(-1.0);
			// Each half evenly (no tap along the view for the anisotropy: most of the light is scattered many
			// times, which spreads it evenly, and a tap toward the horizon greyed the sky's blue).
			ambient = mix(ambient, isotropic * sky_energy, p.ambient.w);
			ambient_down = mix(ambient_down, isotropic_down * sky_energy, p.ambient.w);
		}
	}

	float transmittance = 1.0;
	vec3 sum = vec3(0.0);
	// The first step's offset: interleaved gradient noise (Jimenez 2014), moved every frame when the
	// result is blended over time, so the step pattern averages out instead of showing as bands.
	// Each step covers its own stretch of the ray, the last one cut short at the scene (stratified: the
	// sample falls at the same jittered place in every stretch), so the march neither skips the stretch
	// before its first sample nor runs a whole step past the scene.
	float jitter = ign(vec2(px) + (p.res.z != 0 ? 5.588238 * float(p.res.y & 63) : 0.0));
	float seg = t0;
	for (int i = 0; i < p.counts.w && seg < t1; i++) {
		float dt = min(step_len, t1 - seg);
		float t = seg + jitter * dt;
		vec3 pos = cam + dir * t;
		float smoke;
		if (!smoke_at(pos, cell_inv, view_acc, smoke)) {
			// An empty block: jump to where the ray leaves it.
			vec3 bmin = vec3(view_acc.location.xyz) * block;
			float b0;
			float b1;
			intersect_box(cam, rd_inv, bmin, bmin + block, b0, b1);
			seg = max(b1, t) + step_len * 0.01;
			continue;
		}
		if (smoke > 0.001) {
			float alpha = 1.0 - exp(-min(smoke, SMOKE_MAX) * sigma * dt);
			vec3 in_light = vec3(0.0);
			vec3 pos_m = pos / upm;
			// The light the ground below sends up: the sun on it (in the gas's shadow), added in the sun loop, and
			// the sky's lower half.
			// With a ground albedo the ground is lit by the sky above (its own lower half is a stand-in ground,
			// often a dull brown, where a real snowfield under a blue sky is blue-white).
			vec3 ground_light = p.sky_map.y > 0.0 ? ambient * p.sky_map.y : ambient_down;
			for (int l = 0; l < light_count; l++) {
				Light L = lights[l];
				vec3 to_light = L.position_range.xyz - pos_m;
				float d = length(to_light);
				float range = L.position_range.w;
				if (d >= range || d <= 0.0) {
					continue;
				}
				vec3 ldir = to_light / d;
				// Godot's own omni falloff (light_functions / scene_forward).
				float nd = d / range;
				nd *= nd;
				nd *= nd;
				nd = max(1.0 - nd, 0.0);
				float atten = nd * nd * pow(max(d, 0.0001), -L.color_decay.w);
				if (L.direction_cutoff.w > -1.5) {
					float cutoff = L.direction_cutoff.w;
					float scos = max(dot(-ldir, L.direction_cutoff.xyz), cutoff);
					float rim = max(0.0001, (1.0 - scos) / (1.0 - cutoff));
					atten *= 1.0 - pow(rim, L.spot.x);
				}
				if (atten <= 0.0) {
					continue;
				}
				if (p.misc.w > 0.5) {
					// Scene objects between the light and the point.
					atten *= light_shadow(L, pos_m);
					if (atten <= 0.0) {
						continue;
					}
				}
				// Gas between the point and the light.
				float light_tau = 0.0;
				if (shadow_steps > 0) {
					float reach = min(d, p.misc.z) * upm;
					float ds = reach / float(shadow_steps);
					float od = 0.0;
					for (int s = 0; s < shadow_steps; s++) {
						float ls;
						smoke_at(pos + ldir * (ds * (float(s) + 0.5)), cell_inv, light_acc, ls);
						od += min(ls, SMOKE_MAX);
					}
					light_tau = od * sigma * ds;
				}
				in_light += L.color_decay.rgb * (atten * ms_light(light_tau, light_tau, dot(dir, ldir), g));
			}
			if (p.sun.x > 0) {
				vec3 vpos = (p.world_to_view * vec4(pos_m, 1.0)).xyz;
				for (int i = 0; i < p.sun.x; i++) {
					float fog_energy = directional_lights.data[i].volumetric_fog_energy;
					if (fog_energy <= 0.001) {
						continue;
					}
					vec3 ldir = normalize(mat3(p.view_to_world) * directional_lights.data[i].direction);
#ifndef SHADOW_PASS
					if (ambient_on && p.sky_map.y > 0.0 && ldir.y > 0.0) {
						// The sunlit ground below the point (the box's floor), in the gas's own shadow.
						float ground_sun_t = 1.0;
						if (p.sun_map.w > 0.5 && dot(ldir, p.sun_map.xyz) > 0.9999) {
							vec4 gm = vec4(pos_m.x, p.world_min.y / upm, pos_m.z, 1.0);
							vec3 g_uvw = vec3(dot(p.sun_map_xform[0], gm), dot(p.sun_map_xform[1], gm), dot(p.sun_map_xform[2], gm));
							if (all(greaterThanEqual(g_uvw, vec3(0.0))) && all(lessThanEqual(g_uvw, vec3(1.0)))) {
								ground_sun_t = exp(-textureLod(sun_map_tex, g_uvw, 0.0).r);
							}
						}
						ground_light += directional_lights.data[i].color * (directional_lights.data[i].energy * fog_energy * ldir.y * ground_sun_t * p.sky_map.y);
					}
#endif
					float atten = sun_shadow(uint(i), vpos);
					if (atten <= 0.0) {
						continue;
					}
					float light_tau = 0.0;
					// The gas on along the sun's line past the point (the sun map's whole column less the gas
					// toward the sun): with it, how much gas surrounds the point to scatter light on.
					float tau_beyond = 0.0;
					vec3 map_uvw = vec3(-1.0);
#ifndef SHADOW_PASS
					if (p.sun_map.w > 0.5 && dot(ldir, p.sun_map.xyz) > 0.9999) {
						vec4 pm = vec4(pos_m, 1.0);
						map_uvw = vec3(dot(p.sun_map_xform[0], pm), dot(p.sun_map_xform[1], pm), dot(p.sun_map_xform[2], pm));
					}
					if (all(greaterThanEqual(map_uvw, vec3(0.0))) && all(lessThanEqual(map_uvw, vec3(1.0)))) {
						// All the gas toward the sun in one fetch (Frostbite's volumetric shadow maps).
						light_tau = textureLod(sun_map_tex, map_uvw, 0.0).r;
						tau_beyond = max(textureLod(sun_map_tex, vec3(map_uvw.xy, 1.0), 0.0).r - light_tau, 0.0);
					} else
#endif
					if (shadow_steps > 0) {
						float ds = p.misc.z * upm / float(shadow_steps);
						float od = 0.0;
						for (int s = 0; s < shadow_steps; s++) {
							float ls;
							smoke_at(pos + ldir * (ds * (float(s) + 0.5)), cell_inv, light_acc, ls);
							od += min(ls, SMOKE_MAX);
						}
						light_tau = od * sigma * ds;
					}
					in_light += directional_lights.data[i].color * (directional_lights.data[i].energy * fog_energy * atten * ms_light(light_tau, light_tau + tau_beyond, dot(dir, ldir), g));
				}
			}
			if (ambient_on) {
				// How much of the sky the point sees: the gas straight above it (a short march up).
				vec3 sky_t = ms_ambient(0.0);
				vec3 ground_t = sky_t;
				vec3 sky_uvw = vec3(-1.0);
#ifndef SHADOW_PASS
				if (p.sky_map.w > 0.5) {
					vec4 pm = vec4(pos_m, 1.0);
					sky_uvw = vec3(dot(p.sky_map_xform[0], pm), dot(p.sky_map_xform[1], pm), dot(p.sky_map_xform[2], pm));
				}
				if (all(greaterThanEqual(sky_uvw, vec3(0.0))) && all(lessThanEqual(sky_uvw, vec3(1.0)))) {
					// All the gas above in one fetch.
					float od_above = textureLod(sky_map_tex, sky_uvw, 0.0).r;
					sky_t = ms_ambient(od_above * SKY_OCCLUSION);
					// The gas below: the whole column's depth less what lies above.
					float od_column = textureLod(sky_map_tex, vec3(sky_uvw.xy, 1.0), 0.0).r;
					ground_t = ms_ambient(max(od_column - od_above, 0.0) * SKY_OCCLUSION);
				} else
#endif
				if (shadow_steps > 0) {
					float ds = p.misc.z * upm / float(shadow_steps);
					float od = 0.0;
					for (int s = 0; s < shadow_steps; s++) {
						float ls;
						smoke_at(pos + vec3(0.0, ds * (float(s) + 0.5), 0.0), cell_inv, light_acc, ls);
						od += min(ls, SMOKE_MAX);
					}
					// Multiple scattering, approximated as in Wrenninge et al. 2013 ("Oz: the great and
					// volumetric") and Hillaire 2016 (Frostbite sky and clouds): octaves of ever weaker,
					// ever less attenuated light, so dense smoke isn't black inside as single scattering
					// makes it.
					sky_t = ms_ambient(od * sigma * ds);
				}
				// Each half of the sphere around the point: the sky above, the ground below.
				in_light += (ambient * (sky_t * SKY_GAIN) + ground_light * ground_t) * 0.5;
			}
			sum += transmittance * alpha * in_light;
			transmittance *= 1.0 - alpha;
			if (transmittance < 0.01) {
				break;
			}
		}
		seg += dt;
	}
	// Per unit of opacity (the upsample weighs it by Flow's own coverage).
	float opacity = 1.0 - transmittance;
	return opacity > 1e-4 ? sum * p.albedo.w / opacity : vec3(0.0);
}

#ifdef SHADOW_PASS
// The gas's volumetric shadow map from the sun (Params p plus the box below; Frostbite's volumetric shadow
// maps, Hillaire 2015): per texel across the light, the optical depth from the sun's side of the box to each
// of its depth slices, so a point anywhere in or behind the gas reads the gas toward the sun in one fetch.
layout(set = 0, binding = 10, std140) uniform ShadowBox {
	vec4 origin; // meters: the box corner on the sun's side
	vec4 axis_u; // meters: the box's edges (u, v across the light; w along it, away from the sun)
	vec4 axis_v;
	vec4 axis_w;
	ivec4 res; // x: texels each way; y: steps along the light; z: depth slices
} box;
layout(set = 0, binding = 11, r16f) uniform restrict writeonly image3D shadow_img;

void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	if (px.x >= box.res.x || px.y >= box.res.x) {
		return;
	}
	float upm = p.camera.w;
	vec3 cell = p.block_size.xyz / vec3(p.level[0].xyz + 1u);
	vec3 cell_inv = 1.0 / cell;
	vec2 uv = (vec2(px) + 0.5) / float(box.res.x);
	vec3 base = box.origin.xyz + box.axis_u.xyz * uv.x + box.axis_v.xyz * uv.y;
	int steps = max(box.res.y, 1);
	int slices = max(box.res.z, 1);
	float ds = length(box.axis_w.xyz) / float(steps); // meters
	float sigma_ds = p.misc.x * ds;
	Accessor a;
	a.location = ivec4(0x40000000);
	a.valid = false;
	// Fixed steps along the light, jumping over the blocks Flow's sparse table holds no gas in (the
	// view march does the same); each slice takes the optical depth up to its middle.
	vec3 ro = base * upm;
	vec3 rd = box.axis_w.xyz * upm;
	vec3 rd_inv = 1.0 / rd;
	int i = 0;
	float od = 0.0;
	for (int k = 0; k < slices; k++) {
		float wk = (float(k) + 0.5) / float(slices);
		while (i < steps && (float(i) + 0.5) / float(steps) < wk) {
			float w = (float(i) + 0.5) / float(steps);
			float smoke;
			if (!smoke_at(ro + rd * w, cell_inv, a, smoke)) {
				// An empty block: on to the first sample past where the column leaves it.
				vec3 bmin = vec3(a.location.xyz) * p.block_size.xyz;
				float b0;
				float b1;
				intersect_box(ro, rd_inv, bmin, bmin + p.block_size.xyz, b0, b1);
				i = max(i + 1, int(ceil(b1 * float(steps) - 0.5)));
				continue;
			}
			od += min(smoke, SMOKE_MAX) * sigma_ds;
			i++;
		}
		imageStore(shadow_img, ivec3(px, k), vec4(od));
	}
}
#else
void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	if (px.x >= p.size.z || px.y >= p.size.w) {
		return;
	}
	// The frame pixel this texel stands for: the middle of its res x res footprint.
	int r = p.res.x;
	ivec2 frame_px = min(px * r + r / 2, p.size.xy - 1);
	float depth = texelFetch(depth_tex, frame_px, 0).r;
	imageStore(scatter_img, px, vec4(march(px, frame_px, depth), scene_distance(frame_px, depth)));
}
#endif
)";

// Temporal blending of the scattered light at the scattering resolution (as Frostbite's and Godot's
// volumetric fog keep a history): this frame's texel over its reprojected history, the history clamped
// to this frame's 3x3 neighbourhood (Karis 2014) and dropped where the scene distance moved.
const char *TEMPORAL_SHADER = R"(
#[compute]
#version 450

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
@PARAMS_BLOCK@
layout(set = 0, binding = 2) uniform sampler2D depth_tex;
layout(set = 0, binding = 3, rgba16f) uniform restrict readonly image2D scatter_img;
layout(set = 0, binding = 6) uniform sampler2D history_tex;
layout(set = 0, binding = 7, rgba16f) uniform restrict writeonly image2D history_out;

const float HISTORY_WEIGHT = 0.9;

void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	ivec2 low = p.size.zw;
	if (px.x >= low.x || px.y >= low.y) {
		return;
	}
	vec4 cur = imageLoad(scatter_img, px);
	vec4 result = cur;
	if (p.res.w != 0) {
		vec3 mn = cur.rgb;
		vec3 mx = cur.rgb;
		for (int j = -1; j <= 1; j++) {
			for (int i = -1; i <= 1; i++) {
				vec3 c = imageLoad(scatter_img, clamp(px + ivec2(i, j), ivec2(0), low - 1)).rgb;
				mn = min(mn, c);
				mx = max(mx, c);
			}
		}
		int r = p.res.x;
		ivec2 frame_px = min(px * r + r / 2, p.size.xy - 1);
		float depth = max(texelFetch(depth_tex, frame_px, 0).r, 1e-7);
		vec2 uv = (vec2(frame_px) + 0.5) / vec2(p.size.xy);
		vec4 h = p.inv_view_projection * vec4(uv * 2.0 - 1.0, depth, 1.0);
		vec4 prev = p.prev_view_projection * vec4(h.xyz / h.w, 1.0);
		vec2 prev_uv = prev.xy / prev.w * 0.5 + 0.5;
		if (prev.w > 0.0 && all(greaterThanEqual(prev_uv, vec2(0.0))) && all(lessThanEqual(prev_uv, vec2(1.0)))) {
			vec4 hist = textureLod(history_tex, prev_uv, 0.0);
			float moved = abs(hist.a - cur.a) / max(cur.a, 0.01);
			if (moved < 0.1) {
				result.rgb = mix(cur.rgb, clamp(hist.rgb, mn, mx), HISTORY_WEIGHT);
			}
		}
	}
	imageStore(history_out, px, result);
}
)";

// Adds the scattered light to the frame. At a reduced scattering resolution
// each frame pixel blends its four nearest texels, but trusts a texel less the
// further its scene distance is from the pixel's own -- so the glow behind a
// pillar doesn't bleed onto the pillar's edge.
const char *UPSAMPLE_SHADER = R"(
#[compute]
#version 450

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
@PARAMS_BLOCK@
layout(set = 0, binding = 2) uniform sampler2D depth_tex;
layout(set = 0, binding = 3, rgba16f) uniform restrict readonly image2D scatter_img;
layout(set = 0, binding = 4, rgba16f) uniform restrict image2D color_img;
layout(set = 0, binding = 6) uniform sampler2D coverage_tex;

void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	if (px.x >= p.size.x || px.y >= p.size.y) {
		return;
	}
	int r = p.res.x;
	vec3 light;
	if (r <= 1) {
		light = imageLoad(scatter_img, px).rgb;
	} else {
		float d = scene_distance(px, texelFetch(depth_tex, px, 0).r);
		// A joint bilateral upsample (Kopf et al. 2007): the 4x4 texels around the pixel, by a Gaussian
		// of their distance (wider and smoother than the bilinear tent, so the texel grid does not show
		// where the gas thins out) and by how close their scene distance is to the pixel's.
		// The gap a texel may have and still be the same surface: the slope of the pixel's own surface
		// across a texel or two (a floor seen at a grazing angle changes distance fast from row to row,
		// and its light must still blend there, or it steps at every texel row). Each way the smaller
		// of the two one-sided differences, so an edge next to the pixel does not widen it.
		ivec2 hi_px = p.size.xy - 1;
		float dxp = scene_distance(min(px + ivec2(r, 0), hi_px), texelFetch(depth_tex, min(px + ivec2(r, 0), hi_px), 0).r);
		float dxm = scene_distance(max(px - ivec2(r, 0), ivec2(0)), texelFetch(depth_tex, max(px - ivec2(r, 0), ivec2(0)), 0).r);
		float dyp = scene_distance(min(px + ivec2(0, r), hi_px), texelFetch(depth_tex, min(px + ivec2(0, r), hi_px), 0).r);
		float dym = scene_distance(max(px - ivec2(0, r), ivec2(0)), texelFetch(depth_tex, max(px - ivec2(0, r), ivec2(0)), 0).r);
		float slope = max(min(abs(dxp - d), abs(dxm - d)), min(abs(dyp - d), abs(dym - d))) / max(d, 0.01);
		float tolerance = 0.03 + 2.0 * slope;
		vec2 f = (vec2(px) + 0.5) / float(r) - 0.5;
		ivec2 base = ivec2(floor(f));
		ivec2 hi = p.size.zw - 1;
		vec3 sum = vec3(0.0);
		float total = 0.0;
		vec3 nearest = vec3(0.0);
		float nearest_gap = 1e30;
		for (int j = -1; j < 3; j++) {
			for (int i = -1; i < 3; i++) {
				vec4 t = imageLoad(scatter_img, clamp(base + ivec2(i, j), ivec2(0), hi));
				vec2 offset = vec2(base + ivec2(i, j)) - f;
				float spatial = exp(-dot(offset, offset) * (1.0 / (2.0 * 0.7 * 0.7)));
				float gap = abs(t.a - d) / max(d, 0.01);
				float g4 = gap / tolerance;
				g4 *= g4;
				float weight = spatial / (1.0 + g4 * g4);
				sum += t.rgb * weight;
				total += weight;
				if (gap < nearest_gap) {
					nearest_gap = gap;
					nearest = t.rgb;
				}
			}
		}
		light = total > 1e-4 ? sum / total : nearest;
	}
	if (all(equal(light, vec3(0.0)))) {
		return;
	}
	// The light is the gas's own per unit of opacity; the opacity is Flow's, from the ray march that drew the
	// gas (its transmittance in alpha), so light and coverage come from the same samples: two marches with
	// their own jitter disagree at thin edges, where the light then fell short of the drawn gas as a dark,
	// dotted rim.
	float coverage = 1.0 - clamp(texelFetch(coverage_tex, px, 0).a, 0.0, 1.0);
	vec4 c = imageLoad(color_img, px);
	c.rgb += light * coverage;
	imageStore(color_img, px, c);
}
)";

// sizeof(LightStorage::DirectionalLightData) (private there): the std140 struct in the shader above.
constexpr uint32_t DIRECTIONAL_LIGHT_DATA_SIZE = 560;

constexpr uint32_t PARAMS_WORDS = 16 + 4 + 40 + 4 * 8 + 4 * 5 + 16 * 2 + 4 + 16 + 16 + 16;

RID compile(RenderingDevice *p_rd, const char *p_source, const char *p_name, bool p_sky_array = false, bool p_shadow_pass = false) {
	Ref<RDShaderFile> file;
	file.instantiate();
	String source = String(p_source).replace("@PARAMS_BLOCK@", PARAMS_BLOCK);
	String defines = vformat("#define MAX_DIRECTIONAL_LIGHTS %d\n", (int)RendererSceneRender::MAX_DIRECTIONAL_LIGHTS);
	if (p_sky_array) {
		defines += "#define SKY_ARRAY\n";
	}
	if (p_shadow_pass) {
		defines += "#define SHADOW_PASS\n";
	}
	source = source.replace("#version 450\n", "#version 450\n" + defines);
	ERR_FAIL_COND_V_MSG(file->parse_versions_from_text(source) != OK, RID(), vformat("PhysXFlow3D: light scattering %s shader failed to compile.", p_name));
	Ref<RDShaderSPIRV> spirv = file->get_spirv();
	ERR_FAIL_COND_V(spirv.is_null(), RID());
	const String err = spirv->get_stage_compile_error(RD::SHADER_STAGE_COMPUTE);
	ERR_FAIL_COND_V_MSG(!err.is_empty(), RID(), vformat("PhysXFlow3D: light scattering %s shader: %s", p_name, err));
	return p_rd->shader_create_from_spirv(file->get_spirv_stages());
}

} // namespace

bool FlowScatterRD::_ensure(RenderingDevice *p_rd) {
	if (pipeline.is_valid()) {
		return true;
	}
	if (failed) {
		return false;
	}
	failed = true;
	sky_array = RendererSceneRenderRD::get_singleton() != nullptr && RendererSceneRenderRD::get_singleton()->is_using_radiance_octmap_array();
	shader = compile(p_rd, SCATTER_SHADER, "scattering", sky_array);
	ERR_FAIL_COND_V(!shader.is_valid(), false);
	pipeline = p_rd->compute_pipeline_create(shader);
	ERR_FAIL_COND_V(!pipeline.is_valid(), false);
	shadow_shader = compile(p_rd, SCATTER_SHADER, "shadow map", sky_array, true);
	ERR_FAIL_COND_V(!shadow_shader.is_valid(), false);
	shadow_pipeline = p_rd->compute_pipeline_create(shadow_shader);
	ERR_FAIL_COND_V(!shadow_pipeline.is_valid(), false);
	temporal_shader = compile(p_rd, TEMPORAL_SHADER, "temporal");
	ERR_FAIL_COND_V(!temporal_shader.is_valid(), false);
	temporal_pipeline = p_rd->compute_pipeline_create(temporal_shader);
	ERR_FAIL_COND_V(!temporal_pipeline.is_valid(), false);
	upsample_shader = compile(p_rd, UPSAMPLE_SHADER, "upsample");
	ERR_FAIL_COND_V(!upsample_shader.is_valid(), false);
	upsample_pipeline = p_rd->compute_pipeline_create(upsample_shader);
	ERR_FAIL_COND_V(!upsample_pipeline.is_valid(), false);

	RD::SamplerState linear;
	linear.mag_filter = RD::SAMPLER_FILTER_LINEAR;
	linear.min_filter = RD::SAMPLER_FILTER_LINEAR;
	linear.repeat_u = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	linear.repeat_v = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	linear.repeat_w = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	linear_sampler = p_rd->sampler_create(linear);
	RD::SamplerState point;
	point_sampler = p_rd->sampler_create(point);
	RD::SamplerState shadow_state;
	shadow_state.mag_filter = RD::SAMPLER_FILTER_LINEAR;
	shadow_state.min_filter = RD::SAMPLER_FILTER_LINEAR;
	shadow_sampler = p_rd->sampler_create(shadow_state);
	RD::SamplerState sky_state;
	sky_state.mag_filter = RD::SAMPLER_FILTER_LINEAR;
	sky_state.min_filter = RD::SAMPLER_FILTER_LINEAR;
	sky_state.mip_filter = RD::SAMPLER_FILTER_LINEAR;
	sky_state.repeat_u = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	sky_state.repeat_v = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	sky_sampler = p_rd->sampler_create(sky_state);
	params_buffer = p_rd->uniform_buffer_create(PARAMS_WORDS * 4);
	lights_buffer = p_rd->storage_buffer_create(MAX_LIGHTS * sizeof(Light));
	failed = false;
	return true;
}

void FlowScatterRD::run(RenderingDevice *p_rd, const FlowBackend::ScatterSource &p_source, const Params &p_params, const Light *p_lights, int p_light_count, const RID &p_depth, const RID &p_color, const RID &p_coverage, const RID &p_shadow_atlas, const Size2i &p_size, ShadowMap *r_shadow) {
	const bool scatter = p_light_count > 0 || p_params.ambient || p_params.directional_count > 0;
	if (!p_source.valid || (!scatter && (r_shadow == nullptr || !r_shadow->enabled)) || p_size.x <= 0 || p_size.y <= 0 || !_ensure(p_rd)) {
		return;
	}
	const int count = CLAMP(p_light_count, 0, MAX_LIGHTS);
	const int res = MAX(p_params.resolution_divisor, 1);
	const Size2i low((p_size.x + res - 1) / res, (p_size.y + res - 1) / res);
	if (!scatter_texture.is_valid() || scatter_size != low) {
		if (scatter_texture.is_valid()) {
			p_rd->free_rid(scatter_texture);
		}
		RD::TextureFormat tf;
		tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		tf.width = low.x;
		tf.height = low.y;
		tf.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT;
		scatter_texture = p_rd->texture_create(tf, RD::TextureView());
		ERR_FAIL_COND(!scatter_texture.is_valid());
		scatter_size = low;
	}

	// Temporal blending: once a frame (a second view in the same frame marches without it), over the
	// history of the previous frame at the same scattering resolution.
	const uint64_t frame = RSG::rasterizer->get_frame_number();
	const bool temporal = p_params.temporal && scatter && frame != temporal_frame;
	if (temporal) {
		if (!history[0].is_valid() || history_size != low) {
			for (RID &h : history) {
				if (h.is_valid()) {
					p_rd->free_rid(h);
				}
				RD::TextureFormat tf;
				tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
				tf.width = low.x;
				tf.height = low.y;
				tf.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT;
				h = p_rd->texture_create(tf, RD::TextureView());
				ERR_FAIL_COND(!h.is_valid());
			}
			history_size = low;
			history_valid = false;
		}
		if (frame != temporal_frame + 1) {
			history_valid = false; // skipped a frame: the history is stale
		}
	} else if (p_params.temporal == false || !scatter) {
		history_valid = false;
	}
	const bool blend = temporal && history_valid;
	const Projection view_projection = p_params.inv_view_projection.inverse();

	uint32_t words[PARAMS_WORDS] = {};
	float *f = reinterpret_cast<float *>(words);
	int32_t *iw = reinterpret_cast<int32_t *>(words);
	uint32_t w = 0;
	for (int c = 0; c < 4; c++) {
		for (int r = 0; r < 4; r++) {
			f[w++] = (float)p_params.inv_view_projection.columns[c][r];
		}
	}
	f[w++] = (float)p_params.camera_position.x;
	f[w++] = (float)p_params.camera_position.y;
	f[w++] = (float)p_params.camera_position.z;
	f[w++] = FlowBackend::UNITS_PER_METER;
	for (int i = 0; i < 40; i++) {
		words[w++] = p_source.level[i];
	}
	f[w++] = p_source.block_size_world[0];
	f[w++] = p_source.block_size_world[1];
	f[w++] = p_source.block_size_world[2];
	f[w++] = MAX(p_params.step_scale, 0.25f);
	iw[w++] = p_source.layer_and_level;
	iw[w++] = count;
	iw[w++] = MAX(p_params.shadow_steps, 0);
	iw[w++] = MAX(p_params.max_steps, 1);
	for (int i = 0; i < 3; i++) {
		f[w++] = p_source.world_min[i];
	}
	w++;
	for (int i = 0; i < 3; i++) {
		f[w++] = p_source.world_max[i];
	}
	w++;
	f[w++] = p_params.color[0];
	f[w++] = p_params.color[1];
	f[w++] = p_params.color[2];
	f[w++] = p_params.strength;
	f[w++] = p_params.sigma;
	f[w++] = CLAMP(p_params.anisotropy, -0.95f, 0.95f);
	f[w++] = MAX(p_params.shadow_reach, 0.01f);
	f[w++] = (p_params.shadows && p_shadow_atlas.is_valid()) ? 1.0f : 0.0f;
	iw[w++] = p_size.x;
	iw[w++] = p_size.y;
	iw[w++] = low.x;
	iw[w++] = low.y;
	iw[w++] = res;
	iw[w++] = (int32_t)(frame & 0x7fffffff);
	iw[w++] = temporal ? 1 : 0;
	iw[w++] = blend ? 1 : 0;
	RendererRD::TextureStorage *ts = RendererRD::TextureStorage::get_singleton();
	RID sky = p_params.sky_radiance;
	if (sky.is_valid() && (!p_rd->texture_is_valid(sky) || (p_rd->texture_get_format(sky).texture_type == RD::TEXTURE_TYPE_2D_ARRAY) != sky_array)) {
		sky = RID();
	}
	const bool use_sky = p_params.ambient && sky.is_valid() && p_params.sky_contribution > 0.0f;
	if (!sky.is_valid()) {
		sky = ts->texture_rd_get_default(sky_array ? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK : RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
	}
	f[w++] = p_params.ambient ? p_params.ambient_color[0] : 0.0f;
	f[w++] = p_params.ambient ? p_params.ambient_color[1] : 0.0f;
	f[w++] = p_params.ambient ? p_params.ambient_color[2] : 0.0f;
	f[w++] = use_sky ? CLAMP(p_params.sky_contribution, 0.0f, 1.0f) : 0.0f;
	f[w++] = p_params.sky_border[0];
	f[w++] = p_params.sky_border[1];
	f[w++] = use_sky ? (float)MAX((int)p_rd->texture_get_format(sky).mipmaps - 1, 0) : 0.0f;
	// 0: no ambient; else 1 + the sky's energy.
	f[w++] = p_params.ambient ? 1.0f + MAX(p_params.sky_energy, 0.0f) : 0.0f;
	for (int r = 0; r < 3; r++) {
		const Vector3 row = p_params.sky_inverse.rows[r];
		f[w++] = row.x;
		f[w++] = row.y;
		f[w++] = row.z;
		w++;
	}
	const bool sun = p_params.directional_count > 0 && p_params.directional_buffer.is_valid();
	const bool sun_map = r_shadow != nullptr && r_shadow->enabled && _fit_shadow(p_source, *r_shadow, sun_slot);
	// The sky's map, for the ambient light: the gas above each point, looking straight down.
	ShadowMap sky_shadow;
	sky_shadow.to_light = Vector3(0, 1, 0);
	sky_shadow.enabled = scatter && p_params.ambient && p_params.shadow_steps > 0;
	const bool sky_map = sky_shadow.enabled && _fit_shadow(p_source, sky_shadow, sky_slot);
	const Projection to_view(p_params.world_to_view);
	const Projection to_world(p_params.world_to_view.affine_inverse());
	for (const Projection *m : { &to_view, &to_world }) {
		for (int c = 0; c < 4; c++) {
			for (int r = 0; r < 4; r++) {
				f[w++] = (float)m->columns[c][r];
			}
		}
	}
	iw[w++] = sun ? MIN(p_params.directional_count, (int)RendererSceneRender::MAX_DIRECTIONAL_LIGHTS) : 0;
	iw[w++] = (sun && p_params.directional_shadows && p_params.directional_shadow_atlas.is_valid()) ? 1 : 0;
	w += 2;
	for (int c = 0; c < 4; c++) {
		for (int r = 0; r < 4; r++) {
			f[w++] = (float)(blend ? prev_view_projection : view_projection).columns[c][r];
		}
	}
	for (int r = 0; r < 3; r++) {
		const Vector3 row = sun_map ? r_shadow->world_to_shadow.basis.rows[r] : Vector3();
		f[w++] = row.x;
		f[w++] = row.y;
		f[w++] = row.z;
		f[w++] = sun_map ? r_shadow->world_to_shadow.origin[r] : 0.0f;
	}
	const Vector3 map_to_light = sun_map ? r_shadow->to_light.normalized() : Vector3();
	f[w++] = map_to_light.x;
	f[w++] = map_to_light.y;
	f[w++] = map_to_light.z;
	f[w++] = sun_map ? 1.0f : 0.0f;
	for (int r = 0; r < 3; r++) {
		const Vector3 row = sky_map ? sky_shadow.world_to_shadow.basis.rows[r] : Vector3();
		f[w++] = row.x;
		f[w++] = row.y;
		f[w++] = row.z;
		f[w++] = sky_map ? sky_shadow.world_to_shadow.origin[r] : 0.0f;
	}
	f[w++] = CLAMP(p_params.scattering_albedo, 0.0f, 1.0f);
	f[w++] = CLAMP(p_params.ground_albedo, 0.0f, 1.0f);
	{
		// c in the scattering orders past the three octaves, a geometric series: the sum over n >= 3 of (c albedo)^n, c
		// the share of light a scatter keeps inside the gas (fitted with SUN_MS_GAIN to the path tracer on test puffs).
		f[w++] = 0.68f;
	}
	f[w++] = sky_map ? 1.0f : 0.0f;
	ERR_FAIL_COND(w != PARAMS_WORDS);
	p_rd->buffer_update(params_buffer, 0, sizeof(words), words);
	if (count > 0) {
		p_rd->buffer_update(lights_buffer, 0, count * sizeof(Light), p_lights);
	}

	LocalVector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ linear_sampler, RID::from_uint64(p_source.density_rid) })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, RID::from_uint64(p_source.table_rid)));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ point_sampler, p_depth })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 3, scatter_texture));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, lights_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 5, params_buffer));
	// Something must be bound even when the frame has no shadow atlas.
	const RID atlas = p_shadow_atlas.is_valid() ? p_shadow_atlas : RendererRD::TextureStorage::get_singleton()->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_DEPTH);
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 6, Vector<RID>({ shadow_sampler, atlas })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 7, Vector<RID>({ sky_sampler, sky })));
	// Godot's directional light buffer, or a stand-in of its size when no light uses this pass.
	if (!directional_dummy.is_valid()) {
		directional_dummy = p_rd->uniform_buffer_create(RendererSceneRender::MAX_DIRECTIONAL_LIGHTS * DIRECTIONAL_LIGHT_DATA_SIZE);
	}
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 8, sun ? p_params.directional_buffer : directional_dummy));
	const RID dir_atlas = (sun && p_params.directional_shadow_atlas.is_valid()) ? p_params.directional_shadow_atlas : ts->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_DEPTH);
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 9, Vector<RID>({ shadow_sampler, dir_atlas })));
	if (r_shadow != nullptr && r_shadow->enabled) {
		if (sun_map) {
			_run_shadow(p_rd, uniforms, *r_shadow, sun_slot);
		}
	}
	if (sky_map) {
		_run_shadow(p_rd, uniforms, sky_shadow, sky_slot);
	}
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 11, Vector<RID>({ linear_sampler, (sun_map && r_shadow->texture.is_valid()) ? r_shadow->texture : ts->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_3D_BLACK) })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 12, Vector<RID>({ linear_sampler, (sky_map && sky_shadow.texture.is_valid()) ? sky_shadow.texture : ts->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_3D_BLACK) })));
	if (!scatter) {
		return;
	}
	const RID set = p_rd->uniform_set_create(uniforms, shader, 0);
	ERR_FAIL_COND(!set.is_valid());
	RID resolved = scatter_texture;
	RID temporal_set;
	if (temporal) {
		const RID prev = history[history_index];
		resolved = history[history_index ^ 1];
		LocalVector<RD::Uniform> tu;
		tu.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ point_sampler, p_depth })));
		tu.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 3, scatter_texture));
		tu.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 5, params_buffer));
		tu.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 6, Vector<RID>({ linear_sampler, prev })));
		tu.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 7, resolved));
		temporal_set = p_rd->uniform_set_create(tu, temporal_shader, 0);
		ERR_FAIL_COND(!temporal_set.is_valid());
		history_index ^= 1;
		history_valid = true;
		temporal_frame = frame;
		prev_view_projection = view_projection;
	}
	LocalVector<RD::Uniform> up;
	up.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ point_sampler, p_depth })));
	up.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 3, resolved));
	up.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 4, p_color));
	up.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 5, params_buffer));
	up.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 6, Vector<RID>({ point_sampler, p_coverage })));
	const RID up_set = p_rd->uniform_set_create(up, upsample_shader, 0);
	ERR_FAIL_COND(!up_set.is_valid());
	RD::ComputeListID list = p_rd->compute_list_begin();
	p_rd->compute_list_bind_compute_pipeline(list, pipeline);
	p_rd->compute_list_bind_uniform_set(list, set, 0);
	p_rd->compute_list_dispatch(list, (low.x + 7) / 8, (low.y + 7) / 8, 1);
	p_rd->compute_list_end();
	if (temporal_set.is_valid()) {
		list = p_rd->compute_list_begin();
		p_rd->compute_list_bind_compute_pipeline(list, temporal_pipeline);
		p_rd->compute_list_bind_uniform_set(list, temporal_set, 0);
		p_rd->compute_list_dispatch(list, (low.x + 7) / 8, (low.y + 7) / 8, 1);
		p_rd->compute_list_end();
	}
	list = p_rd->compute_list_begin();
	p_rd->compute_list_bind_compute_pipeline(list, upsample_pipeline);
	p_rd->compute_list_bind_uniform_set(list, up_set, 0);
	p_rd->compute_list_dispatch(list, (p_size.x + 7) / 8, (p_size.y + 7) / 8, 1);
	p_rd->compute_list_end();
	p_rd->free_rid(set);
	p_rd->free_rid(up_set);
	if (temporal_set.is_valid()) {
		p_rd->free_rid(temporal_set);
	}
}

// The gas's volumetric shadow map from the sun: the grid's box seen along the light, a texel per column of
// gas and a slice per depth band, holding the optical depth from the sun's side (Frostbite's volumetric
// shadow maps; Unreal's heterogeneous volumes shadow the scene the same way). The box is the gas's bounds,
// fit across and along the light. _fit_shadow places it (before the parameters go up, which carry it to the
// scattering pass); _run_shadow marches it.
bool FlowScatterRD::_fit_shadow(const FlowBackend::ScatterSource &p_source, ShadowMap &r_shadow, MapSlot &r_slot) {
	r_shadow.texture = RID();
	const Vector3 to_light = r_shadow.to_light.normalized();
	if (to_light.is_zero_approx()) {
		return false;
	}
	const Vector3 along = -to_light; // away from the sun
	const Vector3 u_axis = (Math::abs(along.y) < 0.99f ? along.cross(Vector3(0, 1, 0)) : along.cross(Vector3(1, 0, 0))).normalized();
	const Vector3 v_axis = along.cross(u_axis).normalized();
	const float inv = 1.0f / FlowBackend::UNITS_PER_METER;
	const Vector3 lo = Vector3(p_source.world_min[0], p_source.world_min[1], p_source.world_min[2]) * inv;
	const Vector3 hi = Vector3(p_source.world_max[0], p_source.world_max[1], p_source.world_max[2]) * inv;
	Vector3 mn(1e30, 1e30, 1e30);
	Vector3 mx(-1e30, -1e30, -1e30);
	for (int c = 0; c < 8; c++) {
		const Vector3 corner((c & 1) ? hi.x : lo.x, (c & 2) ? hi.y : lo.y, (c & 4) ? hi.z : lo.z);
		const Vector3 q(corner.dot(u_axis), corner.dot(v_axis), corner.dot(along));
		mn = mn.min(q);
		mx = mx.max(q);
	}
	const Vector3 size = (mx - mn).maxf(0.01f);
	const Vector3 origin = u_axis * mn.x + v_axis * mn.y + along * mn.z;
	const Vector3 axes[4] = { origin, u_axis * size.x, v_axis * size.y, along * size.z };
	for (int a = 0; a < 4; a++) {
		r_slot.box_data[a * 4 + 0] = axes[a].x;
		r_slot.box_data[a * 4 + 1] = axes[a].y;
		r_slot.box_data[a * 4 + 2] = axes[a].z;
		r_slot.box_data[a * 4 + 3] = 0.0f;
	}
	int32_t *bi = reinterpret_cast<int32_t *>(r_slot.box_data + 16);
	bi[0] = CLAMP(r_shadow.resolution, 16, 512);
	// A step per gas cell along the light, a slice per two (at least the asked-for counts): coarser, each
	// step adds a dense cell's whole optical depth at once and the shading steps in bands across the gas.
	float cell = 1e30f;
	for (int a = 0; a < 3; a++) {
		cell = MIN(cell, p_source.block_size_world[a] / float(p_source.level[a] + 1u) * inv);
	}
	const float cells_along = cell < 1e29f ? size.z / MAX(cell, 1e-4f) : 0.0f;
	bi[1] = CLAMP(MAX(r_shadow.steps, (int)Math::ceil(cells_along)), 4, 512);
	bi[2] = CLAMP(MAX(r_shadow.slices, (int)Math::ceil(cells_along * 0.5f)), 2, 128);
	bi[3] = 0;

	// World -> (u, v, depth) in 0..1.
	Transform3D xf;
	xf.basis.rows[0] = u_axis / size.x;
	xf.basis.rows[1] = v_axis / size.y;
	xf.basis.rows[2] = along / size.z;
	xf.origin = Vector3(-mn.x / size.x, -mn.y / size.y, -mn.z / size.z);
	r_shadow.world_to_shadow = xf;
	return true;
}

void FlowScatterRD::_run_shadow(RenderingDevice *p_rd, const LocalVector<RD::Uniform> &p_uniforms, ShadowMap &r_shadow, MapSlot &r_slot) {
	if (!r_slot.box_buffer.is_valid()) {
		r_slot.box_buffer = p_rd->uniform_buffer_create(sizeof(r_slot.box_data));
	}
	const int32_t *bi = reinterpret_cast<const int32_t *>(r_slot.box_data + 16);
	const int res = bi[0];
	const int slices = bi[2];
	if (!r_slot.texture.is_valid() || r_slot.res != res || r_slot.slices != slices) {
		if (r_slot.texture.is_valid()) {
			p_rd->free_rid(r_slot.texture);
		}
		RD::TextureFormat tf;
		tf.texture_type = RD::TEXTURE_TYPE_3D;
		tf.format = RD::DATA_FORMAT_R16_SFLOAT;
		tf.width = res;
		tf.height = res;
		tf.depth = slices;
		tf.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT;
		r_slot.texture = p_rd->texture_create(tf, RD::TextureView());
		ERR_FAIL_COND(!r_slot.texture.is_valid());
		r_slot.res = res;
		r_slot.slices = slices;
	}
	p_rd->buffer_update(r_slot.box_buffer, 0, sizeof(r_slot.box_data), r_slot.box_data);

	LocalVector<RD::Uniform> uniforms;
	for (const RD::Uniform &u : p_uniforms) {
		if (u.binding != 11) {
			uniforms.push_back(u);
		}
	}
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 10, r_slot.box_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 11, r_slot.texture));
	const RID set = p_rd->uniform_set_create(uniforms, shadow_shader, 0);
	ERR_FAIL_COND(!set.is_valid());
	RD::ComputeListID list = p_rd->compute_list_begin();
	p_rd->compute_list_bind_compute_pipeline(list, shadow_pipeline);
	p_rd->compute_list_bind_uniform_set(list, set, 0);
	p_rd->compute_list_dispatch(list, (res + 7) / 8, (res + 7) / 8, 1);
	p_rd->compute_list_end();
	p_rd->free_rid(set);
	r_shadow.texture = r_slot.texture;
}

void FlowScatterRD::free(RenderingDevice *p_rd) {
	if (p_rd == nullptr) {
		return;
	}
	for (RID *r : { &pipeline, &shader, &upsample_pipeline, &upsample_shader, &scatter_texture, &linear_sampler, &point_sampler, &shadow_sampler, &sky_sampler, &params_buffer, &lights_buffer, &directional_dummy, &shadow_pipeline, &shadow_shader, &sun_slot.box_buffer, &sun_slot.texture, &sky_slot.box_buffer, &sky_slot.texture, &temporal_pipeline, &temporal_shader, &history[0], &history[1] }) {
		if (r->is_valid()) {
			p_rd->free_rid(*r);
			*r = RID();
		}
	}
}

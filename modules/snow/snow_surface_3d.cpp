/**************************************************************************/
/*  snow_surface_3d.cpp                                                   */
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



#include "snow_surface_3d.h"
#include "snow_clumps_3d.h"

#include "snow_cover_3d.h"
#include "snow_lake_ice_3d.h"

#include "core/config/engine.h"
#include "core/math/random_pcg.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/templates/hash_set.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/physics/character_body_3d.h"
#include "scene/3d/physics/collision_shape_3d.h"
#include "scene/3d/physics/rigid_body_3d.h"
#include "scene/3d/physics/static_body_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "scene/resources/3d/box_shape_3d.h"
#include "scene/resources/3d/capsule_shape_3d.h"
#include "scene/resources/3d/cylinder_shape_3d.h"
#include "scene/resources/3d/height_map_shape_3d.h"
#include "scene/resources/3d/sphere_shape_3d.h"
#include "scene/resources/mesh.h"
#include "scene/resources/surface_tool.h"
#include "scene/resources/material.h"
#include "scene/resources/image_texture.h"

// The surface shader: the same height function in vertex() (geometry, traced from the capture)
// and fragment() (a finer normal from the deformation texels, plus a few millimetres of wind
// ripple), so trails keep their edges at any mesh density. Snow optics as BaseMaterial3D's snow
// (ice F0 0.018, roughness 0.7 dry to 0.35 wet), compacted snow a little darker and smoother.
// Loose, disturbed snow's angle of repose: about 45 degrees for dry snow pushed aside (Abe 2004 measured
// heaps of disaggregated snow layers; debris and plough berms stand at 35 to 50 degrees).
static constexpr float LOOSE_SNOW_TAN_REPOSE = 1.0f;

static const char *SNOW_SURFACE_SHADER = R"(
shader_type spatial;
render_mode cull_back, depth_draw_opaque;

uniform sampler2D ground_tex : filter_linear, repeat_disable;
uniform vec4 ground_rect;
uniform sampler2D snow_tex : filter_linear, repeat_disable;
uniform vec4 snow_rect;
uniform sampler2D catch_tex : filter_linear, repeat_disable;
uniform bool has_catch = false;
uniform sampler2D deform_tex : filter_linear, repeat_disable;
uniform vec4 deform_rect;
uniform sampler2D cups_tex : filter_linear, repeat_disable;
uniform bool has_cups = false;
uniform bool has_road = false; // cups_tex .g traffic packing, .b glaze mm (SnowCover3D)
uniform sampler2D cup_tile : filter_linear_mipmap, repeat_enable;
uniform float cup_tile_size = 3.2;
uniform sampler2D sas_tex : filter_linear, repeat_disable; // SnowCover3D sastrugi: relief (m) times the carving wind's direction
uniform bool has_sas = false;
uniform sampler2D sas_tile : filter_linear_mipmap, repeat_enable;
uniform sampler2D strata_tex : filter_nearest, repeat_disable; // SnowCover3D strata: two bands a channel, band * 4096 + next; band = (kind + 1 + 8 wet) * 256 + bottom (cm)
uniform bool has_strata = false;
uniform float sas_tile_size = 8.0;
uniform vec4 mesh_rect;
uniform float min_depth = 0.03;
uniform float edge_fade = 8.0;
uniform float detail_step = 0.0625;
uniform float vertex_step = 0.0625;
uniform float coarse_step = 0.5;
uniform float micro_relief = 1.0;
uniform vec3 tint = vec3(1.0);
global uniform bool snow_exact_glints;

varying vec3 w_pos;
varying vec3 w_n;
varying vec3 w_t;
varying vec3 w_b;

bool snow_inside(vec2 uv) {
	return uv.x >= 0.0 && uv.y >= 0.0 && uv.x <= 1.0 && uv.y <= 1.0;
}
float snow_ground(vec2 p) {
	return textureLod(ground_tex, (p - ground_rect.xy) / ground_rect.zw, 0.0).r;
}
vec4 snow_pack(vec2 p) {
	vec2 uv = (p - snow_rect.xy) / max(snow_rect.zw, vec2(1e-4));
	if (!snow_inside(uv)) {
		return vec4(0.0, 0.84, 0.0, 0.1);
	}
	vec4 v = textureLod(snow_tex, uv, 0.0);
	// Under a roof the pack lies on the roof (the cover's catch map): none on the ground here.
	if (has_catch && textureLod(catch_tex, uv, 0.0).r > snow_ground(p) + 0.5) {
		v.r = 0.0;
	}
	return v;
}
vec2 snow_deform(vec2 p) {
	if (deform_rect.z <= 0.0) {
		return vec2(0.0);
	}
	vec2 uv = (p - deform_rect.xy) / deform_rect.zw;
	return snow_inside(uv) ? textureLod(deform_tex, uv, 0.0).rg : vec2(0.0);
}
float snow_fade(vec2 p) {
	vec2 d = min(p - mesh_rect.xy, mesh_rect.xy + mesh_rect.zw - p);
	return smoothstep(0.0, edge_fade, min(d.x, d.y));
}
// The deformation averaged over a vertex's spacing where the triangles are coarser than the texels (a
// displacement prefiltered to the mesh's sampling rate, as geometry clipmaps filter each level): without
// it, vertices land on and off a print's wall and a turned edge zigzags. The fragment's normal keeps the
// texels' own detail.
vec2 snow_deform_vertex(vec2 p) {
	float r = 0.5 * max(vertex_step - detail_step, 0.0);
	if (r <= 0.0) {
		return snow_deform(p);
	}
	return 0.25 * (snow_deform(p + vec2(-r, -r)) + snow_deform(p + vec2(r, -r)) + snow_deform(p + vec2(-r, r)) + snow_deform(p + vec2(r, r)));
}
// The pack's depth between cell centres: Catmull-Rom over the 4 x 4 cells around (C1, so the surface has no
// creases along the cell edges as bilinear leaves; Keys 1981), held within the four nearest cells' range so it
// never overshoots into ripples. SnowCover3D.get_surface_depth_at is the same on the CPU.
// With its gradient (per metre) from the same pass over the 16 cells: the Catmull-Rom weights' derivatives
// (zero where the clamp holds it). A normal from central differences took four such passes.
vec3 snow_depth_smooth_grad(vec2 p) {
	vec2 uv = (p - snow_rect.xy) / max(snow_rect.zw, vec2(1e-4));
	if (!snow_inside(uv)) {
		return vec3(0.0);
	}
	if (has_catch && textureLod(catch_tex, uv, 0.0).r > snow_ground(p) + 0.5) {
		return vec3(0.0);
	}
	ivec2 size = textureSize(snow_tex, 0);
	vec2 f = uv * vec2(size) - 0.5;
	vec2 b = floor(f);
	vec2 t = f - b;
	vec4 wx = vec4(t.x * (-0.5 + t.x * (1.0 - 0.5 * t.x)), 1.0 + t.x * t.x * (-2.5 + 1.5 * t.x), t.x * (0.5 + t.x * (2.0 - 1.5 * t.x)), t.x * t.x * (-0.5 + 0.5 * t.x));
	vec4 wz = vec4(t.y * (-0.5 + t.y * (1.0 - 0.5 * t.y)), 1.0 + t.y * t.y * (-2.5 + 1.5 * t.y), t.y * (0.5 + t.y * (2.0 - 1.5 * t.y)), t.y * t.y * (-0.5 + 0.5 * t.y));
	vec4 dx = vec4(-0.5 + t.x * (2.0 - 1.5 * t.x), t.x * (-5.0 + 4.5 * t.x), 0.5 + t.x * (4.0 - 4.5 * t.x), t.x * (-1.0 + 1.5 * t.x));
	vec4 dz = vec4(-0.5 + t.y * (2.0 - 1.5 * t.y), t.y * (-5.0 + 4.5 * t.y), 0.5 + t.y * (4.0 - 4.5 * t.y), t.y * (-1.0 + 1.5 * t.y));
	float sum = 0.0;
	vec2 g = vec2(0.0);
	float lo = 1e9;
	float hi = -1e9;
	for (int j = 0; j < 4; j++) {
		for (int i = 0; i < 4; i++) {
			ivec2 c = clamp(ivec2(b) + ivec2(i - 1, j - 1), ivec2(0), size - 1);
			float v = texelFetch(snow_tex, c, 0).r;
			sum += v * wx[i] * wz[j];
			g += v * vec2(dx[i] * wz[j], wx[i] * dz[j]);
			if (i >= 1 && i <= 2 && j >= 1 && j <= 2) {
				lo = min(lo, v);
				hi = max(hi, v);
			}
		}
	}
	if (sum < lo || sum > hi) {
		return vec3(clamp(sum, lo, hi), 0.0, 0.0);
	}
	return vec3(sum, g * vec2(size) / max(snow_rect.zw, vec2(1e-4)));
}
// The surface's height and its central differences over 2e (as the differences of snow_height at p +- e
// took them, from 2 x 4 passes over the depth's cells): the depth's part from its exact gradient, the
// fine deformation's, the ground's and the fade's from their own cheap differences.
// Suncups (follow-up 41): the cover's relief (ridge to hollow, m) times a suncup tile grown by a damped
// Kuramoto-Sivashinsky ablation equation (cellular hollows, sharp ridges), laid with hex-tiling (Mikkelsen 2022,
// "Practical real-time hex-tiling"): each hex vertex takes the tile at its own turn and offset and the three
// blend with sharpened weights, so no repeat lines up. Height about the pack's mean (hollows under, ridges
// over) and its gradient (per metre).
// Value noise in 3D (0..1), smooth: grains and tool marks in cut walls.
float snow_vhash(vec3 c) {
	uvec3 u = uvec3(ivec3(c) + ivec3(4096));
	uint h = u.x * 73856093u ^ u.y * 19349663u ^ u.z * 83492791u;
	h ^= h >> 13u;
	h *= 1274126177u;
	h ^= h >> 16u;
	return float(h & 0xffffu) / 65535.0;
}
float snow_vnoise(vec3 x) {
	vec3 i = floor(x);
	vec3 f = x - i;
	f = f * f * (3.0 - 2.0 * f);
	float a = mix(mix(snow_vhash(i), snow_vhash(i + vec3(1, 0, 0)), f.x), mix(snow_vhash(i + vec3(0, 1, 0)), snow_vhash(i + vec3(1, 1, 0)), f.x), f.y);
	float b = mix(mix(snow_vhash(i + vec3(0, 0, 1)), snow_vhash(i + vec3(1, 0, 1)), f.x), mix(snow_vhash(i + vec3(0, 1, 1)), snow_vhash(i + vec3(1, 1, 1)), f.x), f.y);
	return mix(a, b, f.z);
}
uint snow_cup_hash(ivec2 v) {
	uvec2 u = uvec2(v) * 1664525u + 1013904223u;
	u.x += u.y * 1664525u;
	u.y += u.x * 1664525u;
	u ^= u >> 16u;
	return u.x + u.y * 1664525u;
}
vec2 snow_road(vec2 p) {
	if (!has_road) {
		return vec2(0.0);
	}
	vec2 uv = (p - snow_rect.xy) / max(snow_rect.zw, vec2(1e-4));
	return snow_inside(uv) ? textureLod(cups_tex, uv, 0.0).gb : vec2(0.0);
}
vec3 snow_cups(vec2 p, float e) {
	if (!has_cups) {
		return vec3(0.0);
	}
	vec2 uv = (p - snow_rect.xy) / max(snow_rect.zw, vec2(1e-4));
	if (!snow_inside(uv)) {
		return vec3(0.0);
	}
	float relief = textureLod(cups_tex, uv, 0.0).r;
	if (relief < 0.002) {
		return vec3(0.0);
	}
	vec2 st = p / cup_tile_size * 2.0 * 1.7320508;
	vec2 sk = vec2(st.x - 0.57735027 * st.y, 1.15470054 * st.y);
	ivec2 base = ivec2(floor(sk));
	vec3 t = vec3(fract(sk), 0.0);
	t.z = 1.0 - t.x - t.y;
	float s = t.z < 0.0 ? 1.0 : 0.0;
	float s2 = 2.0 * s - 1.0;
	vec3 w = vec3(-t.z * s2, s - t.y * s2, s - t.x * s2);
	ivec2 v[3] = ivec2[3](base + ivec2(int(s), int(s)), base + ivec2(int(s), 1 - int(s)), base + ivec2(1 - int(s), int(s)));
	w = pow(max(w, vec3(0.0)), vec3(6.0));
	w /= max(w.x + w.y + w.z, 1e-6);
	vec3 r = vec3(0.0);
	for (int k = 0; k < 3; k++) {
		uint h = snow_cup_hash(v[k]);
		float a = float(h & 1023u) / 1023.0 * 6.2831853;
		vec2 off = vec2(float((h >> 10u) & 1023u), float((h >> 20u) & 1023u)) / 1023.0;
		mat2 rot = mat2(vec2(cos(a), sin(a)), vec2(-sin(a), cos(a)));
		vec3 c = textureLod(cup_tile, rot * p / cup_tile_size + off, max(log2(e * 96.0 / cup_tile_size), 0.0)).rgb;
		// c.gb: the tile's gradient per tile width, turned back to world axes.
		r += w[k] * vec3(c.r - 0.5, (c.gb * rot) / cup_tile_size);
	}
	return relief * r;
}

// Sastrugi (follow-up 46): the cover's relief along the carving wind times a sastrugi tile (ridges along +x,
// prows facing -x, SnowSurface3D.sastrugi_tile), laid with the same hex-tiling as the suncups but turned to the
// wind with a little jitter (+-9 degrees) instead of at random.
vec3 snow_sastrugi(vec2 p, float e) {
	if (!has_sas) {
		return vec3(0.0);
	}
	vec2 uv = (p - snow_rect.xy) / max(snow_rect.zw, vec2(1e-4));
	if (!snow_inside(uv)) {
		return vec3(0.0);
	}
	vec2 v = textureLod(sas_tex, uv, 0.0).rg;
	float relief = length(v);
	if (relief < 0.003) {
		return vec3(0.0);
	}
	float wind = atan(v.y, v.x);
	vec2 st = p / sas_tile_size * 2.0 * 1.7320508;
	vec2 sk = vec2(st.x - 0.57735027 * st.y, 1.15470054 * st.y);
	ivec2 base = ivec2(floor(sk));
	vec3 t = vec3(fract(sk), 0.0);
	t.z = 1.0 - t.x - t.y;
	float s = t.z < 0.0 ? 1.0 : 0.0;
	float s2 = 2.0 * s - 1.0;
	vec3 w = vec3(-t.z * s2, s - t.y * s2, s - t.x * s2);
	ivec2 vv[3] = ivec2[3](base + ivec2(int(s), int(s)), base + ivec2(int(s), 1 - int(s)), base + ivec2(1 - int(s), int(s)));
	w = pow(max(w, vec3(0.0)), vec3(6.0));
	w /= max(w.x + w.y + w.z, 1e-6);
	vec3 r = vec3(0.0);
	for (int k = 0; k < 3; k++) {
		uint h = snow_cup_hash(vv[k] + ivec2(7919, 104729));
		float a = wind + (float(h & 1023u) / 1023.0 - 0.5) * 0.31;
		vec2 off = vec2(float((h >> 10u) & 1023u), float((h >> 20u) & 1023u)) / 1023.0;
		// Into the tile's frame (x along the wind): a turn by -a.
		mat2 rot = mat2(vec2(cos(a), -sin(a)), vec2(sin(a), cos(a)));
		vec3 c = textureLod(sas_tile, rot * p / sas_tile_size + off, max(log2(e * 128.0 / sas_tile_size), 0.0)).rgb;
		r += w[k] * vec3(c.r - 0.5, (c.gb * rot) / sas_tile_size);
	}
	return relief * r;
}

vec3 snow_height_diff(vec2 p, float e, bool p_vertex) {
	vec3 dg = snow_depth_smooth_grad(p);
	vec2 ex = vec2(e, 0.0);
	vec2 ez = vec2(0.0, e);
	float fade = snow_fade(p);
	vec2 dfade = vec2(snow_fade(p + ex) - snow_fade(p - ex), snow_fade(p + ez) - snow_fade(p - ez));
	vec2 dr = p_vertex ? snow_deform_vertex(p) : snow_deform(p);
	vec2 rx0 = p_vertex ? snow_deform_vertex(p - ex) : snow_deform(p - ex);
	vec2 rx1 = p_vertex ? snow_deform_vertex(p + ex) : snow_deform(p + ex);
	vec2 rz0 = p_vertex ? snow_deform_vertex(p - ez) : snow_deform(p - ez);
	vec2 rz1 = p_vertex ? snow_deform_vertex(p + ez) : snow_deform(p + ez);
	vec2 ddr = vec2((rx1.y - rx1.x) - (rx0.y - rx0.x), (rz1.y - rz1.x) - (rz0.y - rz0.x));
	vec3 cups = (snow_cups(p, e) + snow_sastrugi(p, e)) * fade;
	float d = dg.x * fade - dr.x + dr.y + cups.x;
	vec2 dd = dg.yz * fade * 2.0 * e + dg.x * dfade + ddr + cups.yz * 2.0 * e;
	// Under min_depth the snow is the ground material's; the surface tucks 5 cm under the ground.
	float lift = d >= min_depth ? d : mix(-0.05, min_depth, max(d, 0.0) / min_depth);
	vec2 dlift = d >= min_depth ? dd : (d > 0.0 ? dd * (min_depth + 0.05) / max(min_depth, 1e-4) : vec2(0.0));
	vec2 dground = vec2(snow_ground(p + ex) - snow_ground(p - ex), snow_ground(p + ez) - snow_ground(p - ez));
	return vec3(snow_ground(p) + lift, dground + dlift);
}
// Integer lattice hash (pcg2d, Jarzynski and Olano 2020): no sine, so it holds kilometres from the origin
// with no banding or repeats.
float snow_hash(vec2 p) {
	uvec2 v = uvec2(ivec2(p)) * 1664525u + 1013904223u;
	v.x += v.y * 1664525u;
	v.y += v.x * 1664525u;
	v ^= v >> 16u;
	v.x += v.y * 1664525u;
	v.y += v.x * 1664525u;
	v ^= v >> 16u;
	return float(v.x >> 8u) * (1.0 / 16777216.0);
}
float snow_noise(vec2 p) {
	vec2 i = floor(p);
	vec2 f = fract(p);
	f = f * f * (3.0 - 2.0 * f);
	return mix(mix(snow_hash(i), snow_hash(i + vec2(1.0, 0.0)), f.x), mix(snow_hash(i + vec2(0.0, 1.0)), snow_hash(i + vec2(1.0, 1.0)), f.x), f.y);
}
// The same noise with its gradient (x: value, yz: d/dx, d/dy), from the same four corners (Quilez, value
// noise derivatives).
vec3 snow_noise_grad(vec2 p) {
	vec2 i = floor(p);
	vec2 f = fract(p);
	vec2 u = f * f * (3.0 - 2.0 * f);
	vec2 du = 6.0 * f * (1.0 - f);
	float a = snow_hash(i);
	float b = snow_hash(i + vec2(1.0, 0.0));
	float c = snow_hash(i + vec2(0.0, 1.0));
	float d = snow_hash(i + vec2(1.0, 1.0));
	float k = a - b - c + d;
	return vec3(mix(mix(a, b, u.x), mix(c, d, u.x), u.y), du * vec2(b - a + k * u.y, c - a + k * u.x));
}
// Snow sparkle (docs/ideas/snow.md, stage 5): the same function as BaseMaterial3D's snow pass, there with
// its comments. A facet normal and its weight (0: none) for a footprint in metres.
vec4 snow_glint(vec3 p_pos, vec3 p_n, float p_footprint, float p_hoar) {
	// Surface hoar (p_hoar, mm): its crystals are flat ice plates and feathers a few mm to cm across, standing
	// on the snow (Colbeck 1988; Lang et al. 1984), each one a mirror. Over hoar, the cell is the crystal's size,
	// most crystals show a smooth face, and a share hf of the facets are plates set every way, their normals
	// spread evenly over the sky up to 60 degrees off the snow's (past that the coat's grazing Fresnel would only
	// darken the snow under it): far fewer, larger and brighter glints than the grains', from any sun.
	float hf = smoothstep(0.5, 4.0, p_hoar);
	float grain = max(0.002, 0.0006 * p_hoar);
	float share = mix(0.25, 0.35, hf);
	float level = floor(max(log2(max(p_footprint, grain) / grain), 0.0));
	if (level > 5.0) {
		return vec4(p_n, 0.0);
	}
	float cell = grain * exp2(level);
	ivec3 q = ivec3(floor(p_pos / cell));
	uint h = (uint(q.x) * 73856093u) ^ (uint(q.y) * 19349663u) ^ (uint(q.z) * 83492791u) ^ (uint(level) * 2654435761u);
	h ^= h >> 16u;
	h *= 0x7feb352du;
	h ^= h >> 15u;
	h *= 0x846ca68bu;
	h ^= h >> 16u;
	float r0 = float(h & 0x3ffu) / 1023.0;
	float r1 = float((h >> 10u) & 0x3ffu) / 1024.0;
	float r2 = float((h >> 20u) & 0x3ffu) / 1023.0;
	float grains = exp2(2.0 * level);
	float chance = min(share * grains, 1.0);
	if (r0 >= chance) {
		return vec4(p_n, 0.0);
	}
	float ct;
	if (r0 < chance * hf) {
		ct = mix(0.5, 1.0, r1);
	} else {
		float tan2 = 0.16 * r1 / max(1.0 - r1, 1e-3);
		ct = inversesqrt(1.0 + tan2);
	}
	float st = sqrt(max(1.0 - ct * ct, 0.0));
	vec3 t = normalize(cross(p_n, abs(p_n.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
	vec3 b = cross(p_n, t);
	float phi = 6.2831853 * r2;
	vec3 f = normalize(p_n * ct + (t * cos(phi) + b * sin(phi)) * st);
	return vec4(f, share / chance / sqrt(max(share * grains, 1.0)));
}

// Exact sparkle under the path tracer (docs/ideas/snow.md, follow-up 5): the facet of the grain at a point,
// the same function as BaseMaterial3D's snow pass and the path tracer's, there with its comments.
vec3 snow_grain_facet(vec3 p_pos, vec3 p_n, float p_hoar) {
	// Over surface hoar (p_hoar, mm), crystal-sized cells, a share hf of them plates set every way (as snow_glint).
	float hf = smoothstep(0.5, 4.0, p_hoar);
	float grain = max(0.002, 0.0006 * p_hoar);
	ivec3 q = ivec3(floor(p_pos / grain));
	uint h = (uint(q.x) * 73856093u) ^ (uint(q.y) * 19349663u) ^ (uint(q.z) * 83492791u);
	h ^= h >> 16u;
	h *= 0x7feb352du;
	h ^= h >> 15u;
	h *= 0x846ca68bu;
	h ^= h >> 16u;
	float r1 = float(h & 0xffffu) / 65536.0;
	float r2 = float(h >> 16u) / 65536.0;
	uint h2 = h * 0x9e3779b9u;
	h2 ^= h2 >> 15u;
	float ct;
	if (float(h2 & 0xffffu) / 65536.0 < hf) {
		ct = mix(0.5, 1.0, r1);
	} else {
		float tan2 = 0.16 * r1 / max(1.0 - r1, 1e-4);
		ct = inversesqrt(1.0 + tan2);
	}
	float st = sqrt(max(1.0 - ct * ct, 0.0));
	vec3 t = normalize(cross(p_n, abs(p_n.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
	vec3 b = cross(p_n, t);
	float phi = 6.2831853 * r2;
	return normalize(p_n * ct + (t * cos(phi) + b * sin(phi)) * st);
}

// Wind ripples and grain clumps: 4 mm at 35 cm, 1.5 mm at 9 cm. Each octave turns by an irrational angle
// so no lattice axis lines up, and a 1.7 m field varies their strength from patch to patch (smoother and
// rougher snow), so the pattern never reads as a repeat.
// The ripples' gradient (per metre) in one evaluation: the octaves' analytic gradients through their turns
// (v * M is transpose(M) * v). Central differences took four evaluations.
vec2 snow_ripple_grad(vec2 p) {
	const mat2 r1 = mat2(vec2(0.8, 0.6), vec2(-0.6, 0.8));
	const mat2 r2 = mat2(vec2(0.28, -0.96), vec2(0.96, 0.28));
	vec3 s = snow_noise_grad(r2 * p / 1.7 + 41.0);
	vec3 a = snow_noise_grad(r1 * p / 0.35);
	vec3 b = snow_noise_grad(r2 * p / 0.09 + 17.0);
	float strength = 0.55 + 0.9 * s.x;
	vec2 g_strength = 0.9 * (s.yz * r2) / 1.7;
	float o = 0.004 * a.x + 0.0015 * b.x;
	vec2 g_o = 0.004 * (a.yz * r1) / 0.35 + 0.0015 * (b.yz * r2) / 0.09;
	return micro_relief * (g_strength * o + strength * g_o);
}

void vertex() {
	vec2 p = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xz;
	vec2 duv = (p - deform_rect.xy) / max(deform_rect.zw, vec2(1e-4));
	float e = (deform_rect.z > 0.0 && snow_inside(duv)) ? max(detail_step * 2.0, vertex_step) : coarse_step;
	vec3 hd = snow_height_diff(p, e, true);
	float h = hd.x;
	float hx = hd.y;
	float hz = hd.z;
	vec3 n = normalize(vec3(-hx, 2.0 * e, -hz));
	vec3 t = normalize(vec3(2.0 * e, hx, 0.0));
	vec3 b = normalize(cross(n, t));
	VERTEX.y = h - MODEL_MATRIX[3].y;
	NORMAL = n;
	TANGENT = t;
	BINORMAL = b;
	w_pos = vec3(p.x, h, p.y);
	w_n = n;
	w_t = t;
	w_b = b;
}

void fragment() {
	vec2 p = w_pos.xz;
	vec4 pack = snow_pack(p);
	vec2 dr = snow_deform(p);
	float compact = clamp(dr.x / max(pack.r, 0.01), 0.0, 1.0);
	float e = detail_step;
	vec3 hd = snow_height_diff(p, e, false);
	float hx = hd.y;
	float hz = hd.z;
	vec2 rg = snow_ripple_grad(p) * 2.0 * e;
	hx += rg.x;
	hz += rg.y;
	vec3 nf = normalize(vec3(-hx, 2.0 * e, -hz));
	vec3 tn = vec3(dot(nf, normalize(w_t)), dot(nf, normalize(w_b)), dot(nf, normalize(w_n)));
	// The wet channel: liquid saturation to 1, then 1 + the share of the depth that is slush (SnowCover3D).
	float wet = min(pack.b, 1.0);
	// Slush whose water table nears the surface: water fills the pores between the grains, so light goes deep
	// and is absorbed (saturated snow's albedo some 0.4-0.6; Warren 1982), faintly blue, under a glossy water film.
	float share = clamp(pack.b - 1.0, 0.0, 1.0);
	float slush = smoothstep(0.6, 1.0, share);
	// A print pressed below the water table fills with water (the slush layer's top, share x depth over the
	// ground): a dark, mirror-smooth pool in the hole.
	float pool = share > 0.0 ? smoothstep(0.0, 0.015, dr.x - pack.r * (1.0 - share)) : 0.0;
	// Traffic: packed snow is smoothed and dulled by the tyres and the grit they bring (a packed-snow road reads a
	// little grey); a polished glaze is clear ice over it, darker and mirror-smooth where wheels spun.
	vec2 road = snow_road(p);
	float packed = clamp(road.x, 0.0, 1.0);
	float polish = 1.0 - exp(-max(road.y, 0.0) / 0.3);
	ALBEDO = tint * pack.g * (1.0 - 0.08 * compact) * mix(vec3(1.0), vec3(0.5, 0.56, 0.62), slush) * mix(1.0, 0.35, pool);
	ALBEDO *= mix(vec3(1.0), vec3(0.86, 0.85, 0.83), packed) * mix(vec3(1.0), vec3(0.55, 0.6, 0.66), polish);
	ROUGHNESS = mix(mix(clamp(mix(0.7, 0.35, wet) - 0.15 * compact, 0.05, 1.0), 0.08, slush), 0.02, pool);
	ROUGHNESS = mix(mix(ROUGHNESS, 0.45, packed), 0.04, polish);
	// The pack's layers in the walls of ruts and prints (follow-up 53): a steep face at depth d under the
	// untouched surface shows the band the cover's layers put there, as a snow pit's wall is read (Fierz et al.
	// 2009; SWAG): by colour, by grain and by relief. New snow bright, fine and soft; wind slab chalky and smooth;
	// old rounded snow greyer and finer; a melt-freeze crust a glazed ledge standing out of the wall with a shadow
	// line under it; buried surface hoar a dark recessed seam (it brushes out); depth hoar coarse, loose and dull
	// at the base; wet bands darker and glossy. The wall itself carries the cut's tool marks, and the band edges
	// wander a few millimetres.
	float strata_wall = has_strata ? smoothstep(0.35, 0.75, 1.0 - nf.y) : 0.0;
	if (strata_wall > 0.0) {
		vec2 suv = (p - snow_rect.xy) / max(snow_rect.zw, vec2(1e-4));
		float d = snow_ground(p) + pack.r - w_pos.y;
		d += 0.006 * (snow_vnoise(vec3(p * 9.0, 0.0)) - 0.5) + 0.003 * (snow_vnoise(vec3(p * 31.0, 3.0)) - 0.5);
		if (snow_inside(suv) && d > 0.02) {
			vec4 st = textureLod(strata_tex, suv, 0.0);
			float top_b = 0.0;
			float code = 0.0;
			float band_top = 0.0;
			float band_bottom = 1e3;
			float prev_kind = -1.0;
			float above_kind = -1.0;
			for (int c = 0; c < 8; c++) {
				float pair = st[c / 2];
				float hi_b = floor(pair / 4096.0);
				float v = (c % 2 == 0) ? hi_b : pair - hi_b * 4096.0;
				if (v < 1.0) {
					break;
				}
				float k = floor(v / 256.0);
				float bottom = (v - k * 256.0) * 0.01;
				// A crust or a hoar seam is drawn at least 1.5 cm thick so it reads as a line.
				float kk = mod(k - 1.0, 8.0);
				bottom = (kk == 3.0 || kk == 4.0) ? max(bottom, top_b + 0.015) : max(bottom, top_b);
				above_kind = prev_kind;
				prev_kind = kk;
				code = k;
				band_top = top_b;
				band_bottom = bottom;
				top_b = bottom;
				if (d < bottom) {
					break;
				}
			}
			if (code >= 1.0) {
				float kind = mod(code - 1.0, 8.0);
				bool band_wet = code > 8.5;
				vec3 tint_b = vec3(1.0);
				float rough_b = 0.75;
				float grain = 0.004; // m, the size of the grains the wall shows
				float bump = 0.25; // their relief
				float shade = 1.0; // light the wall's relief keeps from the band (recess, ledge shadow)
				if (kind == 1.0) { tint_b = vec3(0.9, 0.91, 0.93); rough_b = 0.88; grain = 0.0025; bump = 0.12; }
				else if (kind == 2.0) { tint_b = vec3(0.8, 0.84, 0.9); rough_b = 0.8; grain = 0.006; bump = 0.3; }
				else if (kind == 3.0) { tint_b = vec3(0.68, 0.79, 0.93); rough_b = 0.1; grain = 0.02; bump = 0.06; }
				else if (kind == 4.0) { tint_b = vec3(0.4, 0.43, 0.5); rough_b = 0.95; grain = 0.003; bump = 0.5; shade = 0.55; }
				else if (kind == 5.0) { tint_b = vec3(0.66, 0.65, 0.62); rough_b = 0.97; grain = 0.006; bump = 0.4; shade = 0.85; }
				if (band_wet) {
					tint_b *= vec3(0.72, 0.78, 0.86);
					rough_b = 0.25;
					bump *= 0.4;
				}
				// A crust stands out of the wall: its top edge catches the light, the snow just under it sits in its
				// shadow. A hoar seam is brushed out: a recess, shadowed at its top.
				float in_band = clamp((d - band_top) / max(band_bottom - band_top, 1e-3), 0.0, 1.0);
				if (kind == 3.0) {
					shade *= mix(1.15, 0.9, in_band);
				}
				if (above_kind == 3.0) {
					shade *= mix(0.6, 1.0, smoothstep(0.0, 0.012, d - band_top));
				}
				if (kind == 4.0) {
					shade *= mix(0.7, 1.0, in_band);
				}
				// Grains: value noise on the wall at the band's grain size (two octaves), plus the cut's tool marks
				// (streaks down the wall), as a bumped normal.
				// Grains under a pixel average out: in raster their relief fades as the pixel's footprint nears their
				// size; the path tracer averages them itself over its jittered samples, so it keeps them.
				float grain_seen = 1.0;
				if (!IN_PATH_TRACER) {
					float foot = (PROJECTION_MATRIX[3][3] != 0.0 ? 1.0 : distance(w_pos, INV_VIEW_MATRIX[3].xyz)) * 2.0 / (abs(PROJECTION_MATRIX[1][1]) * VIEWPORT_SIZE.y);
					grain_seen = clamp(0.7 * grain / max(foot, 1e-6) - 0.75, 0.0, 1.0);
				}
				vec3 q = w_pos / grain;
				vec3 e3 = vec3(0.37, 0.0, 0.0);
				float n0 = snow_vnoise(q) + 0.5 * snow_vnoise(q * 2.3 + 7.1);
				float nx = snow_vnoise(q + e3.xyy) + 0.5 * snow_vnoise((q + e3.xyy) * 2.3 + 7.1);
				float ny = snow_vnoise(q + e3.yxy) + 0.5 * snow_vnoise((q + e3.yxy) * 2.3 + 7.1);
				float nz = snow_vnoise(q + e3.yyx) + 0.5 * snow_vnoise((q + e3.yyx) * 2.3 + 7.1);
				vec3 gn = vec3(nx - n0, ny - n0, nz - n0) / 0.37;
				if (kind == 5.0) {
					// Depth hoar's cups and prisms: faceted, not rounded; the grain cell's own tilt instead of a smooth bump.
					vec3 fc = floor(w_pos / grain) + 5.0;
					gn = mix(gn, vec3(snow_vhash(fc) - 0.5, snow_vhash(fc + 1.7) - 0.5, snow_vhash(fc + 3.1) - 0.5) * 3.0, 0.6);
				}
				vec3 tq = vec3(w_pos.x * 7.0, w_pos.y * 0.6, w_pos.z * 7.0);
				float tm0 = snow_vnoise(tq);
				vec3 tg = vec3(snow_vnoise(tq + vec3(0.2, 0.0, 0.0)) - tm0, 0.0, snow_vnoise(tq + vec3(0.0, 0.0, 0.2)) - tm0) / 0.2;
				vec3 pert = gn * bump * grain_seen + tg * 0.12;
				vec3 nb = normalize(nf - (pert - nf * dot(pert, nf)) * strata_wall);
				nf = nb;
				tn = vec3(dot(nf, normalize(w_t)), dot(nf, normalize(w_b)), dot(nf, normalize(w_n)));
				// Grains also break the colour up: each grain cell its own brightness (crystal faces turned to the
				// light or into the shade), loose coarse grains most, the pores between them darker.
				float speck = snow_vhash(floor(w_pos / (grain * 0.7)) + 11.0);
				float loose = kind == 5.0 ? 1.0 : (kind == 2.0 ? 0.45 : 0.25);
				float pores = mix(1.0, (0.86 + 0.14 * n0 / 1.5) * mix(0.82, 1.12, speck), clamp(bump * 2.0, 0.0, 1.0) * loose * grain_seen);
				ALBEDO = mix(ALBEDO, ALBEDO * tint_b * shade * pores, strata_wall);
				ROUGHNESS = mix(ROUGHNESS, rough_b, strata_wall);
			}
		}
	}
	compact = max(compact, 0.7 * packed + 0.3 * polish); // fewer whole grains to sparkle
	SPECULAR = 0.335; // ice, F0 0.018
	METALLIC = 0.0;
	NORMAL_MAP = tn * 0.5 + 0.5;
	// Sparkle on dry snow: the facet is the coat's near-mirror lobe on its own normal (NORMAL before
	// NORMAL_MAP), weighted by the share of the pixel it covers, with NORMAL_MAP solved to land back on the
	// snow's normal (as BaseMaterial3D's snow pass). Packed snow keeps fewer whole grains; melt water smooths
	// them away.
	float pix = 2.0 / (abs(PROJECTION_MATRIX[1][1]) * VIEWPORT_SIZE.y);
	float fp = PROJECTION_MATRIX[3][3] != 0.0 ? pix : distance(w_pos, INV_VIEW_MATRIX[3].xyz) * pix;
	// Surface hoar (mm) from the cover's catch map alpha (1 + mm / 10); trodden snow has none left.
	float hoar = 0.0;
	if (has_catch) {
		vec2 huv = (p - snow_rect.xy) / max(snow_rect.zw, vec2(1e-4));
		vec4 hc = snow_inside(huv) ? textureLod(catch_tex, huv, 0.0) : vec4(0.0);
		// Under a roof the hoar is on the roof, not here.
		hoar = hc.r > snow_ground(p) + 0.5 ? 0.0 : max(hc.a - 1.0, 0.0) * 10.0 * (1.0 - compact);
	}
	if (IN_PATH_TRACER && snow_exact_glints) {
		// Under the path tracer: the ice specular lies on the facet of the grain hit, for every grain (the
		// samples of a pixel integrate them), at the facet's own roughness; the diffuse keeps the snow's
		// normal. Trodden snow keeps fewer whole grains, the rest stays rough.
		float ew = (1.0 - smoothstep(0.05, 0.3, wet)) * (1.0 - 0.7 * compact);
		if (ew > 0.0) {
			SPECULAR_NORMAL = normalize((VIEW_MATRIX * vec4(snow_grain_facet(w_pos, nf, hoar), 0.0)).xyz);
			ROUGHNESS = mix(ROUGHNESS, 0.22, ew);
		}
	} else {
		vec4 g = snow_glint(w_pos, nf, fp, hoar);
		float gw = g.w * (1.0 - smoothstep(0.05, 0.3, wet)) * (1.0 - 0.7 * compact);
		vec3 f = normalize((VIEW_MATRIX * vec4(g.xyz, 0.0)).xyz);
		mat3 tbf = mat3(TANGENT, BINORMAL, f);
		if (gw > 0.0 && abs(determinant(tbf)) > 1e-3) {
			vec3 k = inverse(tbf) * normalize((VIEW_MATRIX * vec4(nf, 0.0)).xyz);
			if (k.z > 0.0) {
				k /= length(k);
				NORMAL = f;
				NORMAL_MAP = vec3(k.xy * 0.5 + 0.5, 1.0);
				CLEARCOAT = 0.45 * gw;
				CLEARCOAT_ROUGHNESS = 0.5;
			}
		}
	}
}
)";

// ---------------------------------------------------------------------------------------------------------
// Relations

float SnowSurface3D::bekker_sinkage(float p_pressure_kpa, float p_width, float p_n, float p_kc, float p_kphi) {
	return GroundDeformField::bekker_sinkage(p_pressure_kpa, p_width, p_n, p_kc, p_kphi);
}

// Rotten snow (follow-up 41): a wet, isothermal spring pack loses the ice bonds between its grains as the water
// in it rises (Colbeck 1982, grain clusters in wet snow; its strength falls steeply with liquid water content,
// Yamanoi and Endo 2002), so it bears a fraction of a dry pack's load and packs denser under it (wet grains
// slide into each other's gaps, up to about 700 kg/m3 under a boot, and more is pushed aside than packed). p_wet:
// the liquid over what the pack holds.
float SnowSurface3D::rotten_share(float p_wet) {
	return 1.0f - 0.85f * CLAMP(p_wet, 0.0f, 1.0f);
}

float SnowSurface3D::_dent_max(float p_depth, float p_density, float p_wet) const {
	// A column of depth D at density rho lowered by x keeps (1 - f) of the lost mass as compaction:
	// D - x >= (D rho - f x rho) / rho_c, so x <= D (1 - rho / rho_c) / (1 - f rho / rho_c).
	// Wet, the grains pack closer under the load and more of the snow is pushed aside than packed (a bearing
	// failure, not a compaction: the boot goes through).
	const float w = CLAMP(p_wet, 0.0f, 1.0f);
	const float rho_c = MAX(compacted_density, Math::lerp(compacted_density, 700.0f, w));
	const float f = Math::lerp(rim_share, MAX(rim_share, 0.8f), w);
	const float r = p_density / rho_c;
	if (r >= 1.0f || p_depth <= 0.0f) {
		return 0.0f;
	}
	return p_depth * (1.0f - r) / MAX(1.0f - f * r, 1e-3f);
}

// ---------------------------------------------------------------------------------------------------------
// Setup

SnowCover3D *SnowSurface3D::_cover() const {
	if (!snow_cover.is_empty() && is_inside_tree()) {
		SnowCover3D *c = Object::cast_to<SnowCover3D>(get_node_or_null(snow_cover));
		if (c) {
			return c;
		}
	}
	return SnowCover3D::get_active();
}

Vector3 SnowSurface3D::_follow_position() const {
	if (!follow_target.is_empty()) {
		const Node3D *n = Object::cast_to<Node3D>(get_node_or_null(follow_target));
		if (n && n->is_inside_tree()) {
			return n->get_global_position();
		}
	}
	if (is_inside_tree() && get_viewport()) {
		const Camera3D *cam = get_viewport()->get_camera_3d();
		if (cam) {
			return cam->get_global_position();
		}
	}
	return is_inside_tree() ? get_global_position() : Vector3();
}

void SnowSurface3D::_build_mesh() {
	// A tensor grid: detail_spacing over the deformation field, then spacing growing by a fifth per
	// step up to max_spacing. No T-junctions, so no cracks.
	LocalVector<float> half;
	const float dense = deform_extent * 0.5f;
	const float outer = MAX(extent * 0.5f, dense);
	for (float x = detail_spacing; x < dense - 1e-4f; x += detail_spacing) {
		half.push_back(x);
	}
	half.push_back(dense);
	float s = detail_spacing;
	float x = dense;
	while (x < outer - 1e-3f) {
		s = MIN(s * 1.2f, max_spacing);
		x = MIN(x + s, outer);
		half.push_back(x);
	}
	LocalVector<float> c;
	for (int i = int(half.size()) - 1; i >= 0; i--) {
		c.push_back(-half[i]);
	}
	c.push_back(0.0f);
	for (float h : half) {
		c.push_back(h);
	}
	const int n = c.size();
	PackedVector3Array verts;
	PackedVector3Array normals;
	PackedFloat32Array tangents;
	verts.resize(n * n);
	normals.resize(n * n);
	tangents.resize(n * n * 4);
	for (int z = 0; z < n; z++) {
		for (int xi = 0; xi < n; xi++) {
			const int i = z * n + xi;
			verts.set(i, Vector3(c[xi], 0.0f, c[z]));
			normals.set(i, Vector3(0, 1, 0));
			tangents.set(i * 4 + 0, 1.0f);
			tangents.set(i * 4 + 1, 0.0f);
			tangents.set(i * 4 + 2, 0.0f);
			tangents.set(i * 4 + 3, 1.0f);
		}
	}
	PackedInt32Array idx;
	idx.resize((n - 1) * (n - 1) * 6);
	int k = 0;
	for (int z = 0; z < n - 1; z++) {
		for (int xi = 0; xi < n - 1; xi++) {
			const int a = z * n + xi;
			const int b = a + 1;
			const int d = a + n;
			const int e = d + 1;
			idx.set(k++, a);
			idx.set(k++, b);
			idx.set(k++, d);
			idx.set(k++, b);
			idx.set(k++, e);
			idx.set(k++, d);
		}
	}
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = verts;
	arrays[Mesh::ARRAY_NORMAL] = normals;
	arrays[Mesh::ARRAY_TANGENT] = tangents;
	arrays[Mesh::ARRAY_INDEX] = idx;
	mesh.instantiate();
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	mesh->surface_set_material(0, material);
	if (mesh_instance) {
		mesh_instance->set_mesh(mesh);
	}
	mesh_dirty = false;
}

void SnowSurface3D::_ensure_children() {
	if (!mesh_instance) {
		mesh_instance = memnew(MeshInstance3D);
		mesh_instance->set_as_top_level(true);
		add_child(mesh_instance, false, INTERNAL_MODE_FRONT);
	}
	if (mesh_dirty) {
		_build_mesh();
	}
	if (collision_enabled && !body) {
		body = memnew(StaticBody3D);
		body->set_as_top_level(true);
		body->set_collision_layer(collision_layer);
		body->set_collision_mask(collision_mask);
		collision_node = memnew(CollisionShape3D);
		collision_shape.instantiate();
		collision_node->set_shape(collision_shape);
		body->add_child(collision_node);
		add_child(body, false, INTERNAL_MODE_FRONT);
		collision_dirty = true;
	} else if (!collision_enabled && body) {
		body->queue_free();
		body = nullptr;
		collision_node = nullptr;
		collision_shape.unref();
	}
	if (clumps && !clumps_node) {
		clumps_node = memnew(SnowClumps3D);
		add_child(clumps_node, false, INTERNAL_MODE_FRONT);
	} else if (!clumps && clumps_node) {
		clumps_node->queue_free();
		clumps_node = nullptr;
	}
}

void SnowSurface3D::set_clumps(bool p_enabled) {
	clumps = p_enabled;
	if (is_inside_tree()) {
		_ensure_children();
	}
}

// ---------------------------------------------------------------------------------------------------------
// Following

void SnowSurface3D::_follow(Vector3 p_center) {
	field.extent = deform_extent;
	field.resolution = deform_resolution;
	// On the GPU path the tile cache is the GPU's; the CPU copy keeps none.
	field.max_cached_tiles = _gpu_on() ? 0 : max_cached_tiles;
	const Vector2i want_deform = field.origin_for(Vector2(p_center.x, p_center.z));
	const int gh = int(Math::round(extent * 0.5f / ground_spacing));
	const Vector2 center = field.center_for(Vector2(p_center.x, p_center.z));
	const Vector2i want_ground(int(Math::round(center.x / ground_spacing)) - gh, int(Math::round(center.y / ground_spacing)) - gh);
	if (!ground_valid || want_ground != ground_origin || ground_n != 2 * gh + 1) {
		_shift_ground(want_ground);
	}
	if (deform_enabled && (!field.valid || want_deform != field.origin)) {
		_shift_deform(want_deform);
	}
	if (center != mesh_center || !mesh_instance->is_inside_tree() || mesh_instance->get_global_position() != Vector3(center.x, 0, center.y)) {
		mesh_center = center;
		mesh_instance->set_global_transform(Transform3D(Basis(), Vector3(center.x, 0.0f, center.y)));
		if (body) {
			body->set_global_transform(Transform3D(Basis(), Vector3(center.x, 0.0f, center.y)));
			collision_dirty = true;
		}
		_update_material();
	}
}

void SnowSurface3D::_shift_ground(Vector2i p_origin) {
	const int gh = int(Math::round(extent * 0.5f / ground_spacing));
	const int n = 2 * gh + 1;
	LocalVector<float> next;
	next.resize(n * n);
	LocalVector<uint8_t> missing;
	missing.resize(n * n);
	const bool keep = ground_valid && ground_n == n;
	const Vector2i d = p_origin - ground_origin;
	for (int z = 0; z < n; z++) {
		for (int x = 0; x < n; x++) {
			const int sx = x + d.x;
			const int sz = z + d.y;
			if (keep && sx >= 0 && sz >= 0 && sx < n && sz < n) {
				next[z * n + x] = ground[sz * n + sx];
				missing[z * n + x] = 0;
			} else {
				missing[z * n + x] = 1;
			}
		}
	}
	const SnowCover3D *cover = _cover();
	LocalVector<float> row;
	row.resize(n);
	for (int z = 0; z < n; z++) {
		int x = 0;
		while (x < n) {
			if (!missing[z * n + x]) {
				x++;
				continue;
			}
			int e = x;
			while (e < n && missing[z * n + e]) {
				e++;
			}
			const Vector2 o((p_origin.x + x) * ground_spacing, (p_origin.y + z) * ground_spacing);
			if (cover) {
				cover->sample_ground_heights(o, ground_spacing, e - x, 1, row.ptr());
			}
			for (int i = x; i < e; i++) {
				float h = cover ? row[i - x] : 0.0f;
				if (!Math::is_finite(h)) {
					h = cover ? cover->get_ground_height_at(Vector3((p_origin.x + i) * ground_spacing, 0, (p_origin.y + z) * ground_spacing)) : 0.0f;
				}
				if (cover) {
					// Lake ice that bears weight is the ground the deep snow lies on (docs/ideas/snow.md,
					// follow-up 15); open water and thin ice are not.
					const Vector3 at((p_origin.x + i) * ground_spacing, 0, (p_origin.y + z) * ground_spacing);
					const float ice = cover->get_ice_thickness_at(at);
					if (ice >= SnowCover3D::ICE_WALK) {
						h = MAX(h, cover->get_water_level_at(at) + SnowLakeIce3D::freeboard(ice));
					}
				}
				next[z * n + i] = h;
			}
			x = e;
		}
	}
	ground = next;
	ground_n = n;
	ground_origin = p_origin;
	ground_valid = true;
	ground_min = 1e20f;
	ground_max = -1e20f;
	for (float h : ground) {
		ground_min = MIN(ground_min, h);
		ground_max = MAX(ground_max, h);
	}
	collision_dirty = true;
	_upload_ground();
}

void SnowSurface3D::_shift_deform(Vector2i p_origin) {
	if (_gpu_on()) {
		_gpu_shift(p_origin);
	}
	field.shift(p_origin);
	collision_dirty = true;
	_update_material();
}

// ---------------------------------------------------------------------------------------------------------
// Heights

float SnowSurface3D::_ground_at(float p_x, float p_z) const {
	const int n = ground_n;
	if (!ground_valid || n < 2) {
		return 0.0f;
	}
	const float fx = CLAMP(p_x / ground_spacing - ground_origin.x, 0.0f, float(n - 1));
	const float fz = CLAMP(p_z / ground_spacing - ground_origin.y, 0.0f, float(n - 1));
	const int x0 = MIN(int(fx), n - 2);
	const int z0 = MIN(int(fz), n - 2);
	const float tx = fx - x0;
	const float tz = fz - z0;
	auto at = [&](int x, int z) { return ground[z * n + x]; };
	return Math::lerp(Math::lerp(at(x0, z0), at(x0 + 1, z0), tx), Math::lerp(at(x0, z0 + 1), at(x0 + 1, z0 + 1), tx), tz);
}

float SnowSurface3D::_surface_at(float p_x, float p_z, const SnowCover3D *p_cover) const {
	const float g = _ground_at(p_x, p_z);
	if (!p_cover) {
		return g;
	}
	const float half = extent * 0.5f;
	const float edge = MIN(MIN(p_x - (mesh_center.x - half), mesh_center.x + half - p_x), MIN(p_z - (mesh_center.y - half), mesh_center.y + half - p_z));
	const float fade = edge_fade > 0.0f ? Math::smoothstep(0.0f, edge_fade, edge) : (edge >= 0.0f ? 1.0f : 0.0f);
	// Under a roof the pack lies on the roof (the cover's ground there is the roof): none on the ground.
	float d = p_cover->get_ground_height_at(Vector3(p_x, 0, p_z)) > g + 0.5f ? 0.0f : p_cover->get_surface_depth_at(Vector3(p_x, 0, p_z)) * fade;
	if (deform_enabled && field.valid) {
		d += field.rim_at(p_x, p_z) - field.dent_at(p_x, p_z);
	}
	return d >= min_depth ? g + d : g;
}

// ---------------------------------------------------------------------------------------------------------
// Deformation

float SnowSurface3D::press(Vector3 p_position, float p_radius, float p_bottom, float p_sink, bool p_round, Vector3 p_velocity) {
	return _press(p_position, p_radius, Vector2(), 0.0f, false, p_bottom, p_sink, p_round, Vector2(p_velocity.x, p_velocity.z));
}

float SnowSurface3D::press_box(Vector3 p_position, Vector2 p_size, float p_yaw, float p_bottom, float p_sink, Vector3 p_velocity) {
	return _press(p_position, 0.5f * MAX(p_size.x, p_size.y), p_size * 0.5f, p_yaw, true, p_bottom, p_sink, false, Vector2(p_velocity.x, p_velocity.z));
}

GroundDeformField::Column SnowSurface3D::SnowColumns::column(float p_x, float p_z) const {
	GroundDeformField::Column c;
	const Vector3 wp(p_x, 0, p_z);
	const float depth = cover->get_surface_depth_at(wp); // as the surface is drawn
	if (depth < surface->min_depth) {
		return c;
	}
	c.deformable = true;
	c.surface = surface->_ground_at(p_x, p_z) + depth;
	const float density = cover->get_density_at(wp);
	const float wet = cover->get_wetness_at(wp);
	c.max_dent = surface->_dent_max(depth, density, wet);
	c.rim_share = surface->rim_share;
	if (held || broke) {
		const float thickness = cover->get_crust_thickness_at(wp);
		const float over = cover->get_crust_depth_at(wp);
		if (thickness > 0.0f && over < depth) {
			if (held) {
				// Standing on the crust: only the snow over it compacts.
				c.max_dent = MIN(c.max_dent, surface->_dent_max(over, density, wet));
			} else if (over < 0.05f) {
				c.crust = thickness;
			}
		}
	}
	return c;
}

float SnowSurface3D::SnowColumns::sinkage(const GroundDeformField::Body &p_body) const {
	held = false;
	broke = false;
	const Vector3 wp(p_body.center.x, 0, p_body.center.z);
	// Rotten snow bears less: the moduli scale with the share a wet pack keeps.
	const float keep = rotten_share(cover->get_wetness_at(wp));
	const float z = GroundDeformField::body_sinkage(p_body, surface->bekker_n, surface->bekker_kc * keep, surface->bekker_kphi * keep);
	const float strength = cover->get_crust_strength_at(wp);
	if (strength <= 0.0f) {
		return z;
	}
	const float over = cover->get_crust_depth_at(wp);
	// A fresh break near the surface: its plates show (follow-up 38). The footprint at the crust: a box's
	// half size, else the contact area's radius, else half the body's.
	auto note = [&]() {
		if (over < 0.05f) {
			const float r = p_body.box ? 0.8f * p_body.box_half.length() : (p_body.area > 0.0f ? Math::sqrt(p_body.area / float(Math::PI)) : 0.5f * p_body.radius);
			surface->_note_break(p_body.center, r, cover->get_crust_thickness_at(wp));
		}
	};
	if (p_body.mass <= 0.0f) {
		broke = true; // driven through at a set depth: it breaks whatever is there
		note();
		return z;
	}
	const float depth = cover->get_depth_at(wp);
	if (over >= depth) {
		return z;
	}
	// Already broken here (the hole goes past the crust): it goes on breaking at the hole's edge.
	const float above = surface->_dent_max(over, cover->get_density_at(wp));
	if (surface->field.dent_at(wp.x, wp.z) > above + 0.5f * cover->get_crust_thickness_at(wp) + 0.005f) {
		broke = true;
		return z;
	}
	if (GroundDeformField::body_pressure(p_body, MIN(z, MAX(above, 0.005f))) < strength) {
		held = true;
		return MIN(z, above);
	}
	broke = true;
	note();
	return z;
}

float SnowSurface3D::_press(Vector3 p_position, float p_radius, Vector2 p_half, float p_yaw, bool p_box, float p_bottom, float p_sink, bool p_round, Vector2 p_velocity) {
	SnowCover3D *cover = _cover();
	if (!deform_enabled || !cover) {
		return 0.0f;
	}
	SnowColumns cols;
	cols.surface = this;
	cols.cover = cover;
	// On the GPU path the CPU copy takes the press too, so the volume returned and queries made before the
	// next read back see it; the GPU field takes it as a stamp.
	const float moved = field.press(cols, p_position, p_radius, p_half, p_yaw, p_box, p_bottom, p_sink, p_round, contact_tolerance, p_velocity);
	if (_gpu_on() && field.valid) {
		gpu_sink.stamp(cols, p_position, p_radius, p_half, p_yaw, p_box, p_bottom, p_sink, p_round, contact_tolerance, p_velocity);
	}
	if (moved > 0.0f) {
		collision_dirty = true;
	}
	return moved;
}

void SnowSurface3D::_press_movers(double p_delta) {
	SnowCover3D *cover = _cover();
	if (!is_inside_tree() || !cover) {
		return;
	}
	SnowColumns cols;
	cols.surface = this;
	cols.cover = cover;
	field.press_bodies(get_tree(), deform_groups, cols, character_mass, character_contact_area, contact_tolerance, p_delta);
	if (field.shape_dirty) {
		collision_dirty = true;
	}
}

void SnowSurface3D::_relax() {
	SnowCover3D *cover = _cover();
	if (!cover || !field.valid || cover->get_update_count() == seen_cover_update) {
		return;
	}
	seen_cover_update = cover->get_update_count();
	const int n = field.resolution;
	const int tiles = n / TILE;
	const float t = field.texel();
	LocalVector<float> &dent = field.dent;
	LocalVector<float> &rim = field.rim;
	LocalVector<uint8_t> &tile_active = field.tile_active;
	const Vector2i deform_origin = field.origin;
	const float keep = Math::exp(-wind_fill_rate * cover->get_drift_factor() * cover->get_last_step_hours());
	for (int tz = 0; tz < tiles; tz++) {
		for (int tx = 0; tx < tiles; tx++) {
			if (!tile_active[tz * tiles + tx]) {
				continue;
			}
			bool any = false;
			for (int z = tz * TILE; z < (tz + 1) * TILE; z++) {
				for (int x = tx * TILE; x < (tx + 1) * TILE; x++) {
					const int i = z * n + x;
					float dn = dent[i];
					float rm = rim[i];
					if (dn <= 0.0f && rm <= 0.0f) {
						continue;
					}
					const Vector3 wp((deform_origin.x + x + 0.5f) * t, 0, (deform_origin.y + z + 0.5f) * t);
					const float depth = cover->get_depth_at(wp);
					// New snow slides into hollows; wind drifts over the trail and planes the rim.
					dn = MAX(dn - cover->get_new_snow_at(wp) * refill_share, 0.0f) * keep;
					rm *= keep;
					// The pack around settles toward the compacted density: the step shrinks.
					dn = MIN(dn, _dent_max(depth, cover->get_density_at(wp), cover->get_wetness_at(wp)));
					rm = MIN(rm, depth);
					dent[i] = dn > 1e-4f ? dn : 0.0f;
					rim[i] = rm > 1e-4f ? rm : 0.0f;
					any = any || dent[i] > 0.0f || rim[i] > 0.0f;
				}
			}
			tile_active[tz * tiles + tx] = any ? 1 : 0;
		}
	}
	field.dirty = true;
	collision_dirty = true;
}

// Walls that give way (docs/ideas/snow.md, follow-up 54). The print field's walls are the faces of ruts, prints and
// trenches; a face stands while it is lower than the cut's critical height for the layers it cuts
// (SnowCover3D::cut_critical_height: Terzaghi's 2.67 c / rho g, cohesion by layer), less 40 % while a body works the
// tile (the load beside the edge and the shaking: a vehicle driving along a deep rut brings its walls in). A face
// taller than that gives way along a block of it 0.25-0.7 m long (cut walls fail in blocks, not texel by texel), at
// a rate that grows with how far over it stands: the wedge in front of a Rankine plane (45 + phi / 2, 62 degrees for
// snow's 35) comes off the top, down to the failing layer's bottom (a wall over depth hoar falls to the hoar's
// bottom, undercut), and lands at the foot as a debris toe at the angle of repose (38 degrees), bulked by a quarter
// as broken blocks are. The toe is rim, so SnowClumps3D breaks its cohesive part into lumps. On the GPU the changed
// tiles go up through the field's tile cache (seed_tile, pull_cached), the CPU copy being the last read back.
static constexpr float CRUMBLE_RATE = 5.0f; // Hz
static constexpr float WALL_TAN = 2.75f; // a face steeper than 70 degrees (a 62 degree scarp left by a fall stands)
static constexpr float RANKINE_TAN = 1.88f; // tan 62 degrees
static constexpr float WALL_MEAN_TAN = 2.2f; // a face's mean steepness to count as a wall: 65.5 degrees
static constexpr float TOE_TAN = 0.78f; // tan 38 degrees
static constexpr float DEBRIS_BULKING = 1.25f;
static constexpr float WORKED_FACTOR = 0.6f;

int SnowSurface3D::crumble(float p_delta) {
	crumble_accum = 1.0;
	crumble_hold = 0.0;
	return _crumble(p_delta, _gpu_on() && field.valid && gpu_host.gpu.is_valid());
}

int SnowSurface3D::_crumble(double p_delta, bool p_gpu) {
	SnowCover3D *cover = _cover();
	if (!wall_collapse || !field.valid || !cover) {
		return 0;
	}
	crumble_hold -= p_delta;
	if (p_gpu && !crumble_pending.is_empty()) {
		if (gpu_host.pull_cached(field, crumble_pending)) {
			crumble_pending.clear();
			crumble_hold = 0.5;
		}
		return 0;
	}
	crumble_accum += p_delta;
	if (crumble_accum < 1.0 / CRUMBLE_RATE || crumble_hold > 0.0) {
		return 0;
	}
	const float dt = float(MIN(crumble_accum, 0.5));
	crumble_accum = 0.0;
	const int n = field.resolution;
	const int T = GroundDeformField::TILE;
	const int nt = field.tiles();
	const float t = field.texel();
	LocalVector<float> &dent = field.dent;
	LocalVector<float> &rim = field.rim;
	if (int(dent.size()) != n * n || int(rim.size()) != n * n) {
		return 0;
	}
	if (int(crumble_mark.size()) != n * n) {
		crumble_mark.resize(n * n);
	}
	memset(crumble_mark.ptr(), 0, n * n);
	auto h = [&](int i) { return rim[i] - dent[i]; }; // over the untouched surface (smooth at this scale)
	auto rnd = [this]() {
		crumble_seed = hash_murmur3_one_32(crumble_seed);
		return float(crumble_seed & 0xffffff) / 16777215.0f;
	};
	// Value noise on the field's texels (world-anchored, a lattice of p_cell texels): the rough faces of broken snow.
	auto vnoise = [this](int p_x, int p_z, int p_cell) {
		const int wx = p_x + field.origin.x, wz = p_z + field.origin.y;
		const int cx = (wx >= 0 ? wx : wx - p_cell + 1) / p_cell, cz = (wz >= 0 ? wz : wz - p_cell + 1) / p_cell;
		const float fx = float(wx - cx * p_cell) / p_cell, fz = float(wz - cz * p_cell) / p_cell;
		auto lat = [](int a, int b) { return float(hash_murmur3_one_32(uint32_t(b) * 73856093u ^ hash_murmur3_one_32(uint32_t(a))) & 0xffff) / 65535.0f; };
		const float sx = fx * fx * (3.0f - 2.0f * fx), sz = fz * fz * (3.0f - 2.0f * fz);
		return Math::lerp(Math::lerp(lat(cx, cz), lat(cx + 1, cz), sx), Math::lerp(lat(cx, cz + 1), lat(cx + 1, cz + 1), sx), sz);
	};
	static const int DX[4] = { 1, -1, 0, 0 };
	static const int DZ[4] = { 0, 0, 1, -1 };
	struct Cand {
		int i;
		uint8_t dir;
		float height; // m
		float over; // height over the critical height, 0..
		float fail; // m under the top: the failing layer's bottom
		int foot; // texels from the top to the face's foot
	};
	LocalVector<Cand> cands;
	HashMap<int64_t, Vector2> strength; // per cover cell: (critical height, failing bottom)
	const float cell = MAX(cover->get_extent() / MAX(cover->get_resolution(), 1), 0.05f);
	const int reach = MIN(int(Math::ceil(1.5f / t)), 128); // the deepest wall looked for: 1.5 m of drop
	for (int tz = 0; tz < nt; tz++) {
		for (int tx = 0; tx < nt; tx++) {
			const int ti = tz * nt + tx;
			// A cut's top edge can lie just over a tile seam, in an untouched tile: those beside an active tile count.
			bool near_active = field.tile_active[ti] != 0;
			for (int k = 0; k < 4 && !near_active; k++) {
				const int ax = tx + DX[k], az = tz + DZ[k];
				near_active = ax >= 0 && az >= 0 && ax < nt && az < nt && field.tile_active[az * nt + ax] != 0;
			}
			if (!near_active) {
				continue;
			}
			const bool worked = p_gpu ? (int(gpu_host.tile_stamped.size()) == nt * nt && gpu_host.time - gpu_host.tile_stamped[ti] < 1.0) : (int(field.tile_settle.size()) == nt * nt && field.tile_settle[ti] > 0.0f);
			for (int z = tz * T; z < (tz + 1) * T; z++) {
				for (int x = tx * T; x < (tx + 1) * T; x++) {
					const int i = z * n + x;
					const float hi = h(i);
					for (int d = 0; d < 4; d++) {
						const int x1 = x + DX[d];
						const int z1 = z + DZ[d];
						const int xb = x - DX[d];
						const int zb = z - DZ[d];
						if (x1 < 0 || z1 < 0 || x1 >= n || z1 >= n || xb < 0 || zb < 0 || xb >= n || zb >= n) {
							continue;
						}
						// The top edge of a face: a steep drop ahead, none behind.
						if (hi - h(z1 * n + x1) < WALL_TAN * t || h(zb * n + xb) - hi > WALL_TAN * t) {
							continue;
						}
						// Down the face to its foot.
						float foot = h(z1 * n + x1);
						int xx = x1, zz = z1;
						int foot_k = 1;
						for (int k = 0; k < reach; k++) {
							const int xn = xx + DX[d], zn = zz + DZ[d];
							if (xn < 0 || zn < 0 || xn >= n || zn >= n || h(zn * n + xn) > foot - 0.3f * t) {
								break;
							}
							xx = xn;
							zz = zn;
							foot = h(zn * n + xn);
							foot_k++;
						}
						const float height = hi - foot;
						// A wall stands steep over its whole height: a scarp left by a fall (52-64 degrees) with a steep
					// step at its rough top is not one.
					if (height < 0.05f || rim[i] > 0.5f * height || height < WALL_MEAN_TAN * foot_k * t) {
							continue; // a debris face (mostly rim) is loose snow: the field's slump settles it
						}
						const Vector3 at((field.origin.x + x + 0.5f) * t, 0.0f, (field.origin.y + z + 0.5f) * t);
						const int64_t key = (int64_t(Math::floor(at.x / cell)) << 32) ^ int64_t(uint32_t(int(Math::floor(at.z / cell))));
						Vector2 sv;
						if (const Vector2 *c = strength.getptr(key)) {
							sv = *c;
						} else {
							float bottom = 0.0f;
							const float hc_cell = cover->cut_critical_height(at, bottom);
							sv = Vector2(hc_cell, bottom);
							strength.insert(key, sv);
						}
						// A fin (a drop behind as well as ahead: a sliver left between two fallen blocks) has no wall to lean
						// on and falls at any height over a few centimetres.
						const bool fin = hi - h(zb * n + xb) > WALL_TAN * t;
						const float hc = fin ? 0.03f : sv.x * (worked ? WORKED_FACTOR : 1.0f);
						if (height <= hc) {
							continue;
						}
						cands.push_back({ i, uint8_t(d), height, height / MAX(hc, 0.01f) - 1.0f, float(sv.y), foot_k });
						crumble_mark[i] = uint8_t(d + 1);
						break;
					}
				}
			}
		}
	}
	if (cands.is_empty()) {
		return 0;
	}
	HashSet<int> touched_tiles;
	int fell = 0;
	auto take = [&](int i, float amount) {
		const float from_rim = MIN(rim[i], amount);
		rim[i] -= from_rim;
		dent[i] += amount - from_rim;
		touched_tiles.insert((i / n) / T * nt + (i % n) / T);
	};
	for (uint32_t c = 0; c < cands.size(); c++) {
		const Cand &cd = cands[(c * 2654435761u + crumble_seed) % cands.size()];
		if (crumble_mark[cd.i] != cd.dir + 1) {
			continue; // gone with a block already
		}
		const float p = 1.0f - Math::exp(-3.0f * dt * (cd.over + 0.2f));
		if (rnd() > p) {
			continue;
		}
		// The block: along the face (across the drop), 0.25-0.7 m.
		const int d = cd.dir;
		const int ax = DZ[d] != 0 ? 1 : 0; // along-face step
		const int az = DX[d] != 0 ? 1 : 0;
		const int half = MAX(int((0.25f + 0.45f * rnd()) * 0.5f / t), 1);
		const int x0 = cd.i % n, z0 = cd.i / n;
		// Each block breaks its own way: its plane between 52 and 64 degrees, its crack a little above the failing
		// layer's bottom or at it, and the fracture face rough at a few centimetres.
		const float block_tan = RANKINE_TAN * (0.68f + 0.41f * rnd());
		const float block_depth = MIN(cd.height, MAX(cd.fail, 0.02f)) * (0.8f + 0.2f * rnd());
		const int rough_cell = MAX(int(0.06f / t), 2);
		const int lump_cell = MAX(int(0.14f / t), 2);
		float block_volume = 0.0f; // m3 off this block
		float toe_reach = 0.0f; // m from the top edge to the toe's middle
		for (int s = -half; s <= half; s++) {
			const int x = x0 + ax * s, z = z0 + az * s;
			if (x < 0 || z < 0 || x >= n || z >= n) {
				continue;
			}
			const int i = z * n + x;
			if (crumble_mark[i] != d + 1) {
				continue;
			}
			crumble_mark[i] = 0;
			// The wedge off the top, down to the failing depth: the plane rises back from the face at 62 degrees.
			const float base = h(i) - block_depth;
			float removed = 0.0f;
			float cuts[128];
			int cut_n = 0;
			for (int k = 0; k < 128; k++) {
				const int xk = x - DX[d] * k, zk = z - DZ[d] * k;
				if (xk < 0 || zk < 0 || xk >= n || zk >= n) {
					break;
				}
				const int ik = zk * n + xk;
				const float plane = base + (k + 0.5f) * t * block_tan + (vnoise(xk, zk, rough_cell) - 0.5f) * 0.04f;
				const float cut = h(ik) - plane;
				if (cut <= 0.0f) {
					break;
				}
				take(ik, cut);
				removed += cut * t;
				cuts[cut_n++] = cut;
			}
			if (removed <= 0.0f) {
				continue;
			}
			// The toe at the foot of the face: a triangle at the angle of repose, bulked, from the old floor out. It
			// never rises above that profile (so toes never stack into new walls, nor pile onto the far wall's top);
			// what the triangle cannot hold spreads over the toe up to its height, and what still finds no room stays
			// on the wall.
			const float area = removed * DEBRIS_BULKING;
			const float h0 = MIN(Math::sqrt(2.0f * area * TOE_TAN), 0.8f * block_depth);
			const float len = h0 > 0.0f ? area / (0.5f * h0) : 0.0f; // m: the toe's length for its area at height h0
			const float slope = len > 0.0f ? h0 / len : TOE_TAN;
			// The floor at the face's foot, as it was: the toe's profile stands on it, so toes never stack on toes.
			const float floor_h = base + block_depth - cd.height;
			float left = area;
			int toe_end = cd.foot;
			for (int k = cd.foot; k < cd.foot + 512 && left > 1e-6f; k++) {
				const int xk = x + DX[d] * k, zk = z + DZ[d] * k;
				if (xk < 0 || zk < 0 || xk >= n || zk >= n) {
					break;
				}
				const int ik = zk * n + xk;
				const float want = h0 - (k - cd.foot + 0.5f) * t * slope;
				// On the old floor's profile, or, where debris already lies (a narrow rut whose other wall fell first),
				// on top of it as far as the scarp's base: a heap under the base, its top loose, is never a wall.
				const float room = MAX(MIN(floor_h + want, base + h0), MIN(base, h(ik) + want)) - h(ik);
				if (want <= 0.0f || room <= 0.0f) {
					break;
				}
				// The broken blocks lie as lumps: the toe's surface rises and falls at a block's size.
				const float add = MIN(MIN(want * (0.45f + 1.1f * vnoise(xk, zk, lump_cell)), room), left / t);
				rim[ik] += add;
				left -= add * t;
				toe_end = k + 1;
				touched_tiles.insert((ik / n) / T * nt + (ik % n) / T);
			}
			if (left > 1e-6f) {
				float room_sum = 0.0f;
				for (int k = cd.foot; k < toe_end; k++) {
					room_sum += MAX(MAX(floor_h + h0, base) - h((z + DZ[d] * k) * n + x + DX[d] * k), 0.0f) * t;
				}
				const float share = room_sum > 1e-6f ? MIN(left / room_sum, 1.0f) : 0.0f;
				for (int k = cd.foot; k < toe_end && share > 0.0f; k++) {
					const int ik = (z + DZ[d] * k) * n + x + DX[d] * k;
					const float add = MAX(MAX(floor_h + h0, base) - h(ik), 0.0f) * share;
					rim[ik] += add;
					left -= add * t;
				}
			}
			if (left > 1e-6f) {
				// No room for all of it: what the toe cannot take stays on the wall, the cut scaled back evenly (a
				// smaller wedge on the same plane, not a column left standing at the face).
				const float frac = MIN(left / DEBRIS_BULKING / removed, 1.0f);
				for (int k = 0; k < cut_n; k++) {
					const int ik = (z - DZ[d] * k) * n + x - DX[d] * k;
					const float give = cuts[k] * frac;
					const float from_dent = MIN(give, MAX(dent[ik], 0.0f));
					dent[ik] -= from_dent;
					rim[ik] += give - from_dent;
				}
				removed *= 1.0f - frac;
			}
			collapsed_volume += removed * t;
			block_volume += removed * t;
			toe_reach = MAX(toe_reach, 0.5f * (cd.foot + toe_end) * t);
			fell++;
		}
		// The block breaks up as it lands: part of it lies on the toe as lumps (SnowClumps3D's debris), the larger the
		// stronger the snow it broke from (cohesion from the cut's critical height: c = Hc rho g / 2.67).
		if (clumps_node && block_volume > 0.0f) {
			const Vector3 at((field.origin.x + x0 + DX[d] * toe_reach / t + 0.5f) * t, 0.0f, (field.origin.y + z0 + DZ[d] * toe_reach / t + 0.5f) * t);
			// Lumps land in the rut only: below the top edge the block broke from (in world height).
			const int behind = int(0.6f / t); // past the wedge, on the untouched top
			const float field_top = _surface_at((field.origin.x + x0 - DX[d] * behind + 0.5f) * t, (field.origin.y + z0 - DZ[d] * behind + 0.5f) * t, cover);
			const float hc = cd.height / (cd.over + 1.0f);
			const float coh = hc * cover->get_density_at(at) * 9.81f / 2.67f;
			const float scale = CLAMP(0.8f + Math::log(MAX(coh, 1.0f) / 100.0f) * 0.4343f, 0.8f, 4.0f);
			// Cold new powder mostly falls to dust; settled, sintered snow comes down as blocks.
			const float share = 0.04f + 0.26f * CLAMP((scale - 0.8f) / 2.0f, 0.0f, 1.0f);
			// Along the block's toe, in small scatters (a wide one would reach the far side's top in a narrow rut).
			const float toe_half = MAX(0.5f * (toe_reach - cd.foot * t), 0.02f);
			const int drops = CLAMP(int(Math::ceil((2 * half + 1) * t / 0.2f)), 1, 4);
			for (int k = 0; k < drops; k++) {
				const float along = ((k + 0.5f) / drops - 0.5f) * (2 * half + 1) * t;
				const Vector3 ak = at + Vector3(ax * along, 0.0f, az * along);
				clumps_node->drop(ak, MIN(0.4f * toe_half, 0.06f), share * block_volume / drops, cover->get_wetness_at(ak), scale, field_top - 0.05f);
			}
		}
	}
	if (fell == 0) {
		return 0;
	}
	walls_collapsed += fell;
	field.dirty = true;
	collision_dirty = true;
	for (const int ti : touched_tiles) {
		field.tile_active[ti] = 1;
	}
	if (p_gpu) {
		// Up to the GPU field through its cache: each changed tile as the cache packs it (dent | rim << 16).
		LocalVector<uint32_t> data;
		data.resize(T * T);
		const Vector2i t0 = field.origin / T;
		for (const int ti : touched_tiles) {
			const int tx = ti % nt, tz = ti / nt;
			for (int z = 0; z < T; z++) {
				for (int x = 0; x < T; x++) {
					const int i = (tz * T + z) * n + tx * T + x;
					data[z * T + x] = uint32_t(Math::make_half_float(dent[i])) | (uint32_t(Math::make_half_float(rim[i])) << 16);
				}
			}
			const Vector2i w = t0 + Vector2i(tx, tz);
			gpu_host.forget_cached(w);
			gpu_host.seed_tile(w, data.ptr(), 1, max_cached_tiles);
			crumble_pending.push_back(w);
		}
		if (gpu_host.pull_cached(field, crumble_pending)) {
			crumble_pending.clear();
			crumble_hold = 0.5;
		}
	}
	return fell;
}

void SnowSurface3D::clear_deformation() {
	field.clear();
	gpu_host.clear_cache();
	gpu_host.reset = true;
	collision_dirty = true;
}

void SnowSurface3D::refresh_ground() {
	ground_valid = false;
	if (is_inside_tree()) {
		_follow(_follow_position());
	}
}

// ---------------------------------------------------------------------------------------------------------
// GPU and physics

void SnowSurface3D::_upload_ground() {
	const int n = ground_n;
	Vector<uint8_t> bytes;
	bytes.resize(n * n * 4);
	memcpy(bytes.ptrw(), ground.ptr(), n * n * 4);
	if (ground_image.is_null() || ground_image->get_width() != n) {
		ground_image = Image::create_from_data(n, n, false, Image::FORMAT_RF, bytes);
		ground_texture = ImageTexture::create_from_image(ground_image);
	} else {
		ground_image->set_data(n, n, false, Image::FORMAT_RF, bytes);
		ground_texture->update(ground_image);
	}
	if (mesh_instance) {
		const float half = extent * 0.5f;
		mesh_instance->set_custom_aabb(AABB(Vector3(-half, ground_min - 1.0f, -half), Vector3(extent, ground_max - ground_min + 12.0f, extent)));
	}
	gpu_ground_dirty = true;
	_update_material();
}

void SnowSurface3D::_upload_deform() {
	const RID before = deform_texture.is_valid() ? deform_texture->get_rid() : RID();
	field.upload(deform_image, deform_texture);
	if (deform_texture.is_valid() && deform_texture->get_rid() != before) {
		_update_material();
	}
}

Ref<Image> SnowSurface3D::suncup_tile() {
	// A periodic suncup tile: the damped Kuramoto-Sivashinsky equation
	// h_t = -lap h - lap^2 h - |grad h|^2 / 2 - g (h - mean), from small noise. Its linear part is an ablation
	// instability with one fastest wavelength (Betterton 2000's form); the |grad h|^2 term is the eroding front's
	// (walls retreat along their normal), which rounds the hollows and sharpens the ridges; the damping holds a
	// steady cellular field. 96 cells over 64 units, g 0.1, t 60: about 8 cups across. R the height 0..1,
	// G and B its gradient per tile width (half floats), mipmapped.
	static Ref<Image> tile; // CPU only, so it may outlive the rendering server
	if (tile.is_valid()) {
		return tile;
	}
	constexpr int N = 96;
	constexpr float L = 64.0f, DX = L / N, DT = 0.005f, G = 0.1f, T = 60.0f;
	LocalVector<float> h, l, nh;
	h.resize(N * N);
	l.resize(N * N);
	nh.resize(N * N);
	RandomPCG rng(7);
	for (int i = 0; i < N * N; i++) {
		h[i] = 0.1f * (rng.randf() * 2.0f - 1.0f);
	}
	auto at = [&](const LocalVector<float> &a, int x, int z) { return a[((z + N) % N) * N + (x + N) % N]; };
	const int steps = int(T / DT);
	for (int st = 0; st < steps; st++) {
		float mean = 0.0f;
		for (int z = 0; z < N; z++) {
			for (int x = 0; x < N; x++) {
				l[z * N + x] = (at(h, x - 1, z) + at(h, x + 1, z) + at(h, x, z - 1) + at(h, x, z + 1) - 4.0f * h[z * N + x]) / (DX * DX);
				mean += h[z * N + x];
			}
		}
		mean /= N * N;
		for (int z = 0; z < N; z++) {
			for (int x = 0; x < N; x++) {
				const float l2 = (at(l, x - 1, z) + at(l, x + 1, z) + at(l, x, z - 1) + at(l, x, z + 1) - 4.0f * l[z * N + x]) / (DX * DX);
				const float gx = (at(h, x + 1, z) - at(h, x - 1, z)) / (2.0f * DX);
				const float gz = (at(h, x, z + 1) - at(h, x, z - 1)) / (2.0f * DX);
				const int i = z * N + x;
				nh[i] = h[i] + DT * (-l[i] - l2 - 0.5f * (gx * gx + gz * gz) - G * (h[i] - mean));
			}
		}
		SWAP(h, nh);
	}
	float lo = 1e9f, hi = -1e9f;
	for (int i = 0; i < N * N; i++) {
		lo = MIN(lo, h[i]);
		hi = MAX(hi, h[i]);
	}
	const float scale = 1.0f / MAX(hi - lo, 1e-6f);
	Ref<Image> img = Image::create_empty(N, N, false, Image::FORMAT_RGBAH);
	for (int z = 0; z < N; z++) {
		for (int x = 0; x < N; x++) {
			const float v = (h[z * N + x] - lo) * scale;
			const float gx = (at(h, x + 1, z) - at(h, x - 1, z)) * scale * 0.5f * N; // per tile width
			const float gz = (at(h, x, z + 1) - at(h, x, z - 1)) * scale * 0.5f * N;
			img->set_pixel(x, z, Color(v, gx, gz, 1.0f));
		}
	}
	img->generate_mipmaps();
	tile = img;
	return tile;
}

Ref<Image> SnowSurface3D::sastrugi_tile() {
	// A periodic sastrugi tile, wind along +x. Sastrugi are erosional bedforms of a cohesive surface whose
	// hardness varies (Filhol and Sturm 2015, JGR Earth Surface 120): the wind cuts the soft patches and leaves the
	// hard ones as ridges along it. Here: a hardness field of periodic value noise five times finer across the wind
	// than along it, eroded at (1.2 - 1.6 H)^2 (soft fastest), faster on faces turned into the wind (1 + 4 dh/dx),
	// and sheltered (15%) in the lee of higher ground within Werner's (1995) 15 degree shadow, so prows steepen
	// upwind and tails taper downwind; a little diffusion. The height squared: flat scoured floors between raised
	// ridges, as sastrugi fields look. 128 cells; R the height 0..1, G and B its gradient per tile width (half
	// floats), mipmapped.
	static Ref<Image> tile;
	if (tile.is_valid()) {
		return tile;
	}
	constexpr int N = 128;
	constexpr int STEPS = 300;
	constexpr float DX = 1.0f / N;
	constexpr float DT = 0.06f / STEPS;
	LocalVector<float> hard, h, nh;
	hard.resize(N * N);
	h.resize(N * N);
	nh.resize(N * N);
	RandomPCG rng(11);
	auto value_noise = [&](int gx, int gz, float weight) {
		LocalVector<float> g;
		g.resize(gx * gz);
		for (int i = 0; i < gx * gz; i++) {
			g[i] = rng.randf();
		}
		for (int z = 0; z < N; z++) {
			const float fz = float(z) * gz / N;
			const int z0 = int(fz);
			float tz = fz - z0;
			tz = tz * tz * (3.0f - 2.0f * tz);
			for (int x = 0; x < N; x++) {
				const float fx = float(x) * gx / N;
				const int x0 = int(fx);
				float tx = fx - x0;
				tx = tx * tx * (3.0f - 2.0f * tx);
				const int x1 = (x0 + 1) % gx, z1 = (z0 + 1) % gz;
				const float a = Math::lerp(g[z0 * gx + x0], g[z0 * gx + x1], tx);
				const float b = Math::lerp(g[z1 * gx + x0], g[z1 * gx + x1], tx);
				hard[z * N + x] += weight * Math::lerp(a, b, tz);
			}
		}
	};
	for (int i = 0; i < N * N; i++) {
		hard[i] = 0.0f;
		h[i] = 0.0f;
	}
	value_noise(4, 20, 1.0f);
	value_noise(8, 40, 0.35f);
	float lo = 1e9f, hi = -1e9f;
	for (int i = 0; i < N * N; i++) {
		lo = MIN(lo, hard[i]);
		hi = MAX(hi, hard[i]);
	}
	for (int i = 0; i < N * N; i++) {
		hard[i] = (hard[i] - lo) / MAX(hi - lo, 1e-6f);
	}
	auto at = [&](const LocalVector<float> &a, int x, int z) { return a[((z + N) % N) * N + (x + N) % N]; };
	const float tan15 = Math::tan(Math::deg_to_rad(15.0f));
	for (int st = 0; st < STEPS; st++) {
		for (int z = 0; z < N; z++) {
			for (int x = 0; x < N; x++) {
				const int i = z * N + x;
				const float dh = (h[i] - at(h, x - 1, z)) / DX;
				const float soft = CLAMP(1.2f - 1.6f * hard[i], 0.02f, 1.0f);
				float e = soft * soft * (1.0f + 4.0f * MAX(dh, 0.0f));
				for (int k = 1; k <= 24; k++) {
					if (at(h, x - k, z) - h[i] > k * DX * tan15) {
						e *= 0.15f;
						break;
					}
				}
				nh[i] = h[i] - DT * e;
			}
		}
		for (int z = 0; z < N; z++) {
			for (int x = 0; x < N; x++) {
				const int i = z * N + x;
				h[i] = nh[i] + 0.003f * (at(nh, x - 1, z) + at(nh, x + 1, z) + at(nh, x, z - 1) + at(nh, x, z + 1) - 4.0f * nh[i]);
			}
		}
	}
	lo = 1e9f;
	hi = -1e9f;
	for (int i = 0; i < N * N; i++) {
		lo = MIN(lo, h[i]);
		hi = MAX(hi, h[i]);
	}
	const float scale = 1.0f / MAX(hi - lo, 1e-6f);
	for (int i = 0; i < N * N; i++) {
		const float v = (h[i] - lo) * scale;
		nh[i] = v * v;
	}
	Ref<Image> img = Image::create_empty(N, N, false, Image::FORMAT_RGBAH);
	for (int z = 0; z < N; z++) {
		for (int x = 0; x < N; x++) {
			const float gx = (at(nh, x + 1, z) - at(nh, x - 1, z)) * 0.5f * N;
			const float gz = (at(nh, x, z + 1) - at(nh, x, z - 1)) * 0.5f * N;
			img->set_pixel(x, z, Color(nh[z * N + x], gx, gz, 1.0f));
		}
	}
	img->generate_mipmaps();
	tile = img;
	return tile;
}

void SnowSurface3D::_update_material() {
	if (material.is_null()) {
		return;
	}
	const SnowCover3D *cover = _cover();
	material->set_shader_parameter("ground_tex", ground_texture);
	material->set_shader_parameter("ground_rect", Vector4((ground_origin.x - 0.5f) * ground_spacing, (ground_origin.y - 0.5f) * ground_spacing, ground_n * ground_spacing, ground_n * ground_spacing));
	if (cover && cover->get_snow_texture().is_valid()) {
		const Rect2 r = cover->get_map_rect();
		material->set_shader_parameter("snow_tex", cover->get_snow_texture());
		material->set_shader_parameter("snow_rect", Vector4(r.position.x, r.position.y, r.size.x, r.size.y));
		material->set_shader_parameter("catch_tex", cover->get_catch_texture());
		material->set_shader_parameter("has_catch", cover->get_catch_texture().is_valid());
		material->set_shader_parameter("cups_tex", cover->get_cups_texture());
		material->set_shader_parameter("has_cups", cover->get_cups_texture().is_valid() && cover->is_suncups_enabled());
		material->set_shader_parameter("has_road", cover->get_cups_texture().is_valid() && cover->is_traffic_enabled());
		if (cup_tile_texture.is_null()) {
			cup_tile_texture = ImageTexture::create_from_image(suncup_tile());
		}
		material->set_shader_parameter("cup_tile", cup_tile_texture);
		material->set_shader_parameter("sas_tex", cover->get_sastrugi_texture());
		material->set_shader_parameter("has_sas", cover->get_sastrugi_texture().is_valid() && cover->is_sastrugi_enabled());
		if (sas_tile_texture.is_null()) {
			sas_tile_texture = ImageTexture::create_from_image(sastrugi_tile());
		}
		material->set_shader_parameter("sas_tile", sas_tile_texture);
		material->set_shader_parameter("strata_tex", cover->get_strata_texture());
		material->set_shader_parameter("has_strata", cover->get_strata_texture().is_valid() && cover->is_layers_enabled());
	} else {
		material->set_shader_parameter("snow_rect", Vector4());
	}
	const bool gpu_tex = _gpu_on() && gpu_host.texture.is_valid() && gpu_host.texture->get_texture_rd_rid().is_valid();
	if (deform_enabled && field.valid && (gpu_tex || deform_texture.is_valid())) {
		const Rect2 r = field.rect();
		if (gpu_tex) {
			material->set_shader_parameter("deform_tex", gpu_host.texture);
		} else {
			material->set_shader_parameter("deform_tex", deform_texture);
		}
		material->set_shader_parameter("deform_rect", Vector4(r.position.x, r.position.y, r.size.x, r.size.y));
	} else {
		material->set_shader_parameter("deform_rect", Vector4());
	}
	const float half = extent * 0.5f;
	material->set_shader_parameter("mesh_rect", Vector4(mesh_center.x - half, mesh_center.y - half, extent, extent));
	material->set_shader_parameter("min_depth", min_depth);
	material->set_shader_parameter("edge_fade", edge_fade);
	material->set_shader_parameter("detail_step", _texel());
	material->set_shader_parameter("vertex_step", detail_spacing);
	material->set_shader_parameter("coarse_step", MAX(ground_spacing, max_spacing * 0.5f));
	material->set_shader_parameter("micro_relief", micro_relief);
	material->set_shader_parameter("tint", Vector3(tint.r, tint.g, tint.b));
}

void SnowSurface3D::_update_collision() {
	if (!body || collision_shape.is_null() || !collision_dirty) {
		return;
	}
	const SnowCover3D *cover = _cover();
	const float sp = _texel() * 2.0f;
	const int cells = MAX(int(Math::round(collision_extent / sp)), 2);
	const int w = cells + 1;
	PackedFloat32Array heights;
	heights.resize(w * w);
	float *hw = heights.ptrw();
	for (int z = 0; z < w; z++) {
		for (int x = 0; x < w; x++) {
			const float wx = mesh_center.x + (x - cells * 0.5f) * sp;
			const float wz = mesh_center.y + (z - cells * 0.5f) * sp;
			hw[z * w + x] = _surface_at(wx, wz, cover) / sp;
		}
	}
	collision_node->set_scale(Vector3(sp, sp, sp));
	if (collision_shape->get_map_width() != w || collision_shape->get_map_depth() != w) {
		collision_shape->set_map_width(w);
		collision_shape->set_map_depth(w);
	}
	collision_shape->set_map_data(heights);
	collision_dirty = false;
}

SnowSurface3D *SnowSurface3D::active = nullptr;

void SnowSurface3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			active = this;
			_ensure_children();
			if (!Engine::get_singleton()->is_editor_hint()) {
				set_physics_process_internal(true);
			}
		} break;
		case NOTIFICATION_READY: {
			_follow(_follow_position());
			_upload_deform();
			_update_collision();
		} break;
		case NOTIFICATION_EXIT_TREE: {
			if (active == this) {
				active = nullptr;
			}
			set_physics_process_internal(false);
			_gpu_release();
		} break;
		case NOTIFICATION_INTERNAL_PHYSICS_PROCESS: {
			const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
			const double delta = get_physics_process_delta_time();
			_follow(_follow_position());
			const bool on_gpu = _gpu_on();
			if (!on_gpu && gpu_host.gpu.is_valid()) {
				_gpu_release();
			}
			if (on_gpu && field.valid) {
				_gpu_take();
				// The bodies press as stamps for the GPU field (the same maths, docs/ideas/snow.md follow-up 13).
				field.press_settle = 1.0f;
				field.stamp_sink = &gpu_sink;
				_press_movers(delta);
				field.stamp_sink = nullptr;
				_crumble(delta, true);
				collision_accum += delta;
				bool collide = false;
				if (collision_accum >= 1.0 / collision_rate) {
					collision_accum = 0.0;
					collide = true;
				}
				_gpu_submit(delta, collide, false);
			} else if (deform_enabled && field.valid) {
				// Pressed tiles owe a second of slumping: the snow a body pushes aside is loose and slides off
				// where it lies steeper than its repose (Sumner's erosion step, as the granular ground): into
				// the print, or off a thin wall left between two overlapping steps. Only that rim moves: the
				// compacted walls stand.
				field.press_settle = 1.0f;
				_press_movers(delta);
				auto ground_fn = [this](float x, float z) { return _ground_at(x, z); };
				const SnowCover3D *slump_cover = _cover();
				auto params_fn = [slump_cover](float x, float z, float &r_tan, float &r_rate, float &r_max) {
					// Plates of a broken crust are slabs, not loose snow: they stay as heaped.
					if (!slump_cover || slump_cover->get_crust_thickness_at(Vector3(x, 0, z)) > 0.0f) {
						return false;
					}
					r_tan = LOOSE_SNOW_TAN_REPOSE;
					r_rate = 1e3f;
					r_max = 0.0f;
					return true;
				};
				field.slump(delta, ground_fn, params_fn);
				_relax();
				_crumble(delta, false);
			}
			if (!on_gpu || !field.valid) {
				collision_accum += delta;
				if (collision_accum >= 1.0 / collision_rate) {
					collision_accum = 0.0;
					_update_collision();
				}
				if (field.dirty) {
					_upload_deform();
				}
			}
			// The cover's texture and rect move on its own schedule.
			const SnowCover3D *cover = _cover();
			if (cover && cover->get_snow_texture().is_valid() && material.is_valid() && (material->get_shader_parameter("snow_tex") != Variant(cover->get_snow_texture()) || material->get_shader_parameter("cups_tex") != Variant(cover->get_cups_texture()) || material->get_shader_parameter("sas_tex") != Variant(cover->get_sastrugi_texture()) || material->get_shader_parameter("strata_tex") != Variant(cover->get_strata_texture()))) {
				_update_material();
			} else if (cover && material.is_valid()) {
				const Rect2 r = cover->get_map_rect();
				const Vector4 want(r.position.x, r.position.y, r.size.x, r.size.y);
				if (Vector4(material->get_shader_parameter("snow_rect")) != want) {
					_update_material();
				}
			}
			if (cover && cover->get_update_count() != seen_cover_update) {
				collision_dirty = true;
			}
			_crust_plates_step(delta);
			step_msec = double(OS::get_singleton()->get_ticks_usec() - t0) / 1000.0;
		} break;
	}
}

// ---------------------------------------------------------------------------------------------------------
// Crust plates (docs/ideas/snow.md, follow-up 38)

void SnowSurface3D::_note_break(const Vector3 &p_center, float p_radius, float p_thickness) const {
	if (!crust_plates || p_thickness <= 0.0f || p_radius <= 0.0f) {
		return;
	}
	// One set of plates per hole: a body still sinking, or the next step's overlap, adds none.
	for (const CrustBreak &b : crust_breaks) {
		if (Vector2(b.position.x - p_center.x, b.position.z - p_center.z).length() < 0.6f * MAX(b.radius, p_radius)) {
			return;
		}
	}
	CrustBreak b;
	b.position = p_center;
	b.radius = p_radius;
	b.thickness = p_thickness;
	b.crack = GroundDeformField::CRACK_LENGTH * Math::pow(p_thickness / 0.02f, 0.75f);
	// The field's own seed for this press, so each plate lies on its sector's ramp.
	b.seed = hash_murmur3_one_32(uint32_t(int(Math::floor(p_center.x / 0.25f))) * 73856093u ^ uint32_t(int(Math::floor(p_center.z / 0.25f))) * 19349663u);
	if (crust_breaks.size() >= 256) {
		crust_breaks.remove_at(0);
	}
	crust_breaks.push_back(b);
}

void SnowSurface3D::_crust_plates_step(double p_delta) {
	if (crust_breaks.is_empty()) {
		return;
	}
	const SnowCover3D *cover = _cover();
	// A break's plates are laid once the body has sunk (a third of a second on): they sample the hole it made.
	for (CrustBreak &b : crust_breaks) {
		b.age += float(p_delta);
		if (!b.built && b.age >= 0.35f) {
			b.built = true;
			b.surface = cover ? _surface_at(b.position.x + b.radius + 0.5f * b.crack, b.position.z, cover) : 0.0f;
			plates_dirty = true;
		}
	}
	// New snow over them (the surface beside the hole risen past the plates) buries them.
	plates_check += float(p_delta);
	if (cover && plates_check >= 1.0f) {
		plates_check = 0.0f;
		for (uint32_t i = 0; i < crust_breaks.size();) {
			const CrustBreak &b = crust_breaks[i];
			if (b.built && _surface_at(b.position.x + b.radius + 0.5f * b.crack, b.position.z, cover) > b.surface + 0.02f + b.thickness) {
				crust_breaks.remove_at(i);
				plates_dirty = true;
			} else {
				i++;
			}
		}
	}
	if (plates_dirty) {
		plates_dirty = false;
		_build_plates();
	}
}

void SnowSurface3D::_build_plates() {
	// Each break: the field's seven radial sectors (GroundDeformField::press), each a slab from the hole's edge
	// to its own crack reach, its corners on the hole's surface there, so it tips into the hole as the field's
	// ramp does but stays one flat piece with a sharp fractured edge as thick as the crust. The outer edge, the
	// hinge it rocked on, lifts a little (a lip; up to a fifth of the inner edge's drop), and a 3 mm crack
	// parts the plates, so neighbours that tipped differently show a step between them. Pieces of a plate on
	// an elastic foundation break radially, then round at about its characteristic length (Hetenyi 1946).
	const SnowCover3D *cover = _cover();
	if (!plates_instance) {
		plates_instance = memnew(MeshInstance3D);
		plates_instance->set_as_top_level(true);
		add_child(plates_instance, false, INTERNAL_MODE_FRONT);
	}
	if (plates_material.is_null()) {
		// A melt-freeze crust: coarse rounded grains bonded by refrozen water, glazed on top (smoother than
		// new snow) and greyer and bluer in a fresh fracture, where light goes deeper into the bonded grains.
		plates_material.instantiate();
		plates_material->set_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
		plates_material->set_albedo(Color(1, 1, 1));
		plates_material->set_roughness(0.45f);
		plates_material->set_specular(0.335f);
	}
	Ref<SurfaceTool> st;
	st.instantiate();
	st->begin(Mesh::PRIMITIVE_TRIANGLES);
	int faces = 0;
	const int sectors = GroundDeformField::CRACK_SECTORS;
	for (const CrustBreak &b : crust_breaks) {
		if (!b.built || !cover) {
			continue;
		}
		const float albedo = CLAMP(cover->get_albedo_at(b.position), 0.4f, 0.95f);
		const Color top_c(albedo, albedo, albedo);
		const Color side_c(albedo * 0.84f, albedo * 0.88f, albedo * 0.93f);
		for (int k = 0; k < sectors; k++) {
			const uint32_t hs = hash_murmur3_one_32(uint32_t(k), b.seed);
			const float reach = b.crack * (0.5f + 1.1f * float(hs & 0xffff) / 65535.0f);
			// The field's sector k: a = atan2 + pi in [k, k + 1) sevenths of a turn.
			const float gap = 0.0015f; // half the crack's width
			const float a0 = float(Math::TAU) * k / sectors - float(Math::PI);
			const float a1 = float(Math::TAU) * (k + 1) / sectors - float(Math::PI);
			const float r0 = b.radius + 0.004f;
			const float r1 = b.radius + reach;
			const float jag = 0.15f * reach * (float((hs >> 16) & 0xff) / 255.0f - 0.5f);
			const float lip = 0.2f * float((hs >> 24) & 0xff) / 255.0f;
			auto at = [&](float p_r, float p_a, float p_side) {
				// p_side: across the crack, inward from the sector's edge (its normal), by the crack's half width.
				const Vector2 dir(Math::cos(p_a), Math::sin(p_a));
				const Vector2 across(-dir.y * p_side, dir.x * p_side);
				const Vector2 q = Vector2(b.position.x, b.position.z) + dir * p_r + across * gap;
				return Vector3(q.x, 0.0f, q.y);
			};
			Vector3 poly[5] = {
				at(r0, a0, 1.0f), // inner, at the sector's first edge
				at(r1, a0, 1.0f), // outer at the first edge
				at(r1 + jag, 0.5f * (a0 + a1), 0.0f), // the outer crack's jag
				at(r1, a1, -1.0f), // outer at the second edge
				at(r0, a1, -1.0f), // inner at the second edge
			};
			// The plate's tip from the field's ramp (linear from the footprint's edge out to the reach): its outer
			// edge on the crust, and half way out, so the inner edge is extrapolated along the ramp rather than
			// sampled in the footprint's own deeper hole.
			const float mid_r = 0.5f * (r0 + r1);
			const Vector3 m0 = at(mid_r, a0 + 0.25f * (a1 - a0), 0.0f), m1 = at(mid_r, a0 + 0.75f * (a1 - a0), 0.0f);
			const float outer = (_surface_at(poly[1].x, poly[1].z, cover) + _surface_at(poly[2].x, poly[2].z, cover) + _surface_at(poly[3].x, poly[3].z, cover)) / 3.0f;
			const float half_way = 0.5f * (_surface_at(m0.x, m0.z, cover) + _surface_at(m1.x, m1.z, cover));
			const float drop = CLAMP(2.0f * (outer - half_way), 0.0f, 0.5f * b.crack);
			if (drop < 0.003f) {
				continue; // a sector that did not tip (beyond the field's crust): no plate
			}
			// The hinge edge lifts as the plate rocks on it (up to 0.6 of the crust's thickness); the slab's top
			// stands a quarter of its thickness over the field's rounded ramp inside.
			float h[5];
			h[0] = h[4] = outer - drop + 0.25f * b.thickness;
			h[1] = h[2] = h[3] = outer + 3.0f * lip * b.thickness;
			Vector3 top[5], bot[5];
			for (int c = 0; c < 5; c++) {
				top[c] = Vector3(poly[c].x, h[c], poly[c].z);
				bot[c] = top[c] - Vector3(0.0f, b.thickness, 0.0f);
			}
			auto tri = [&](const Vector3 &p0, const Vector3 &p1, const Vector3 &p2, const Color &p_c, const Vector3 &p_out) {
				Vector3 n = (p1 - p0).cross(p2 - p0);
				if (n.length_squared() < 1e-14f) {
					return;
				}
				n.normalize();
				// Clockwise front (Godot's): flip to face p_out.
				const bool flip = n.dot(p_out) > 0.0f;
				const Vector3 nn = flip ? n : -n;
				const Vector3 v[3] = { p0, flip ? p2 : p1, flip ? p1 : p2 };
				for (const Vector3 &q : v) {
					st->set_normal(nn);
					st->set_color(p_c);
					st->add_vertex(q);
				}
				faces++;
			};
			const Vector3 mid_top = (top[0] + top[1] + top[2] + top[3] + top[4]) / 5.0f;
			for (int c = 1; c + 1 < 5; c++) {
				tri(top[0], top[c], top[c + 1], top_c, Vector3(0, 1, 0));
				tri(bot[0], bot[c], bot[c + 1], side_c, Vector3(0, -1, 0));
			}
			for (int c = 0; c < 5; c++) {
				const int d = (c + 1) % 5;
				Vector3 out = (top[c] + top[d]) * 0.5f - mid_top;
				out.y = 0.0f;
				tri(top[c], top[d], bot[d], side_c, out);
				tri(top[c], bot[d], bot[c], side_c, out);
			}
		}
	}
	if (faces == 0) {
		plates_instance->set_mesh(Ref<Mesh>());
		return;
	}
	Ref<ArrayMesh> mesh = st->commit();
	mesh->surface_set_material(0, plates_material);
	plates_instance->set_mesh(mesh);
}

void SnowSurface3D::set_crust_plates(bool p_enabled) {
	crust_plates = p_enabled;
	if (!p_enabled) {
		crust_breaks.clear();
		if (plates_instance) {
			plates_instance->set_mesh(Ref<Mesh>());
		}
	}
}

int SnowSurface3D::get_crust_plate_count() const {
	int n = 0;
	for (const CrustBreak &b : crust_breaks) {
		n += b.built ? 1 : 0;
	}
	return n;
}

// ---------------------------------------------------------------------------------------------------------
// The print field on the GPU (docs/ideas/snow.md, follow-up 13)

bool SnowSurface3D::_gpu_on() const {
	return gpu_simulation && deform_enabled && GroundDeformGPU::is_available();
}

void SnowSurface3D::set_gpu_simulation(bool p_enabled) {
	if (gpu_simulation == p_enabled) {
		return;
	}
	gpu_simulation = p_enabled;
	// Each path keeps its own field: start the new one empty.
	field.invalidate();
	field.clear();
	gpu_host.clear_cache();
	gpu_host.reset = true;
	if (is_inside_tree()) {
		_follow(_follow_position());
	}
	_update_material();
	collision_dirty = true;
}

void SnowSurface3D::_gpu_release() {
	gpu_host.release();
	gpu_ground_dirty = true;
	gpu_cover_dirty = true;
}

void SnowSurface3D::GPUSink::stamp(const GroundDeformField::Columns &p_columns, Vector3 p_position, float p_radius, Vector2 p_half, float p_yaw, bool p_box, float p_bottom, float p_sink, bool p_round, float p_contact_tolerance, Vector2 p_velocity) {
	GroundDeformField &f = surface->field;
	if (!f.valid || p_radius <= 0.0f || (p_box && (p_half.x <= 0.0f || p_half.y <= 0.0f))) {
		return;
	}
	// The footprint's reach, as GroundDeformField::press works it out.
	const float ring = 0.5f * (p_box ? Math::sqrt(p_half.x * p_half.y) : p_radius);
	const float crust = p_columns.column(p_position.x, p_position.z).crust;
	const float crack = crust > 0.0f ? GroundDeformField::CRACK_LENGTH * Math::pow(crust / 0.02f, 0.75f) : 0.0f;
	const float bow = CLAMP(p_velocity.length() / GroundDeformField::BOW_SPEED, 0.0f, 1.0f);
	const float span = (p_box ? p_half.length() : p_radius) + MAX(ring * (1.0f + bow * GroundDeformField::BOW_REACH), 1.6f * crack);
	int x0, x1, z0, z1;
	if (!GroundDeformGPUHost::stamp_bounds(f, p_position, span, x0, x1, z0, z1)) {
		return;
	}
	const SnowColumns *sc = static_cast<const SnowColumns *>(&p_columns);
	uint32_t flags = (p_box ? 1u : 0u) | (p_round ? 2u : 0u) | (sc->held ? 4u : 0u) | (sc->broke ? 8u : 0u);
	const uint32_t seed = hash_murmur3_one_32(uint32_t(int(Math::floor(p_position.x / 0.25f))) * 73856093u ^ uint32_t(int(Math::floor(p_position.z / 0.25f))) * 19349663u);
	float seed_f;
	memcpy(&seed_f, &seed, 4);
	const float v[12] = {
		p_position.x, p_position.y, p_position.z, p_radius,
		p_half.x, p_half.y, p_yaw, p_bottom,
		p_sink, p_contact_tolerance, p_velocity.x, p_velocity.y
	};
	const float d[4] = { crack, float(flags), seed_f, 0.0f };
	surface->gpu_host.add_stamp(f, v, d, x0, x1, z0, z1);
}

void SnowSurface3D::_gpu_shift(Vector2i p_origin) {
	gpu_host.shift_to(field, p_origin, deform_resolution, max_cached_tiles);
}

int SnowSurface3D::_gpu_collision_cells() const {
	const float sp = _texel() * 2.0f;
	return MAX(int(Math::round(collision_extent / sp)), 2);
}

PackedByteArray SnowSurface3D::_gpu_cover_cells(int &r_n, bool &r_valid) const {
	PackedByteArray out;
	const SnowCover3D *cover = _cover();
	r_n = 0;
	r_valid = false;
	if (!cover || !cover->origin_valid || cover->pub.swe.size() != cover->resolution * cover->resolution) {
		return out;
	}
	const int cn = cover->resolution;
	const int cells = cn * cn;
	out.resize(cells * 32);
	float *w = reinterpret_cast<float *>(out.ptrw());
	const bool crust = cover->pub.crust.size() == cells;
	const bool fresh = cover->pub.new_depth.size() == cells;
	const bool ground_ok = cover->cell_ground.size() == cells;
	const bool wet_ok = cover->pub.liquid.size() == cells;
	for (int i = 0; i < cells; i++) {
		w[i * 8 + 0] = cover->pub.swe[i];
		w[i * 8 + 1] = cover->pub.density[i];
		w[i * 8 + 2] = crust ? cover->pub.crust[i] : 0.0f;
		w[i * 8 + 3] = crust ? cover->pub.crust_burial[i] : 0.0f;
		w[i * 8 + 4] = fresh ? cover->pub.new_depth[i] : 0.0f;
		w[i * 8 + 5] = ground_ok ? cover->cell_ground[i] : 0.0f;
		// Liquid water saturation, as SnowCover3D::get_wetness_at (rotten snow: ground_deform's dent_max).
		float wet = 0.0f;
		if (wet_ok) {
			const float hold = cover->liquid_capacity * (cover->pub.swe[i] - cover->pub.liquid[i]);
			wet = hold > 1e-4f ? CLAMP(cover->pub.liquid[i] / hold, 0.0f, 1.0f) : 0.0f;
		}
		if (cover->pub.slush.size() == cells && cover->pub.slush[i] > 0.5f && cover->pub.swe[i] > 0.01f) {
			wet = 1.0f; // saturated from the base (slush over frozen ground)
		}
		w[i * 8 + 6] = wet;
		w[i * 8 + 7] = 0.0f;
	}
	r_n = cn;
	r_valid = true;
	return out;
}

PackedByteArray SnowSurface3D::_gpu_params(double p_delta, int p_cover_n, bool p_cover_valid) const {
	PackedByteArray b;
	b.resize(GroundDeformGPU::PARAMS_BYTES);
	uint8_t *w = b.ptrw();
	memset(w, 0, GroundDeformGPU::PARAMS_BYTES);
	const SnowCover3D *cover = _cover();
	const int n = deform_resolution;
	const int32_t res[4] = { n, n / TILE, ground_n, p_cover_n };
	memcpy(w, res, 16);
	float *f = reinterpret_cast<float *>(w + 16);
	auto put = [&](int p_vec, float a, float bb, float c, float d) {
		f[p_vec * 4 + 0] = a;
		f[p_vec * 4 + 1] = bb;
		f[p_vec * 4 + 2] = c;
		f[p_vec * 4 + 3] = d;
	};
	put(0, float(field.origin.x), float(field.origin.y), _texel(), float(p_delta));
	put(1, float(ground_origin.x), float(ground_origin.y), ground_spacing, p_cover_valid ? 1.0f : 0.0f);
	if (cover && p_cover_valid) {
		put(2, float(cover->origin_cell.x), float(cover->origin_cell.y), cover->_cell_size(), 0.0f);
		const Rect2 r = cover->get_map_rect();
		put(3, r.position.x, r.position.y, r.size.x, r.size.y);
	}
	put(4, compacted_density, rim_share, min_depth, edge_fade);
	const float half = extent * 0.5f;
	put(5, mesh_center.x - half, mesh_center.y - half, extent, extent);
	put(6, gpu_relax_keep, refill_share, 1.0f, LOOSE_SNOW_TAN_REPOSE);
	put(7, mesh_center.x, mesh_center.y, _texel() * 2.0f, float(_gpu_collision_cells()));
	put(8, GroundDeformField::BOW_SPEED, GroundDeformField::BOW_GAIN, GroundDeformField::BOW_REACH, 0.0f);
	put(9, GroundDeformField::CRACK_LENGTH, GroundDeformField::PLATE, float(GroundDeformField::CRACK_SECTORS), 0.0f);
	return b;
}

void SnowSurface3D::_gpu_submit(double p_delta, bool p_collision, bool p_sync) {
	if (gpu_host.gpu.is_null()) {
		gpu_host.gpu.instantiate();
		gpu_host.reset = true;
		gpu_ground_dirty = true;
		gpu_cover_dirty = true;
	}
	if (gpu_host.texture.is_null()) {
		gpu_host.texture.instantiate();
	}
	gpu_host.time += p_delta;
	SnowCover3D *cover = _cover();
	int flags = 0;
	// The cover moved on: refill, drift and settling (SnowSurface3D::_relax), with its new cells.
	if (cover && cover->get_update_count() != seen_cover_update) {
		seen_cover_update = cover->get_update_count();
		gpu_relax_keep = Math::exp(-wind_fill_rate * cover->get_drift_factor() * cover->get_last_step_hours());
		gpu_cover_dirty = true;
		flags |= GroundDeformGPU::STEP_RELAX;
	}
	int cover_n = 0;
	bool cover_valid = false;
	PackedByteArray cover_cells;
	if (gpu_cover_dirty) {
		cover_cells = _gpu_cover_cells(cover_n, cover_valid);
		flags |= GroundDeformGPU::STEP_COVER;
		gpu_cover_dirty = false;
		gpu_cover_n = cover_n;
		gpu_cover_valid = cover_valid;
	} else {
		cover_n = gpu_cover_n;
		cover_valid = gpu_cover_valid;
	}
	PackedByteArray ground_bytes;
	if (gpu_ground_dirty && ground_valid) {
		ground_bytes.resize(ground_n * ground_n * 4);
		memcpy(ground_bytes.ptrw(), ground.ptr(), ground_n * ground_n * 4);
		flags |= GroundDeformGPU::STEP_GROUND;
		gpu_ground_dirty = false;
	}
	if (gpu_host.reset) {
		flags |= GroundDeformGPU::STEP_RESET;
	}
	if (gpu_host.shift.size() > 0 || gpu_host.stores.size() > 0) {
		flags |= GroundDeformGPU::STEP_SHIFT;
	}
	if (gpu_host.stamps.size() > 0) {
		flags |= GroundDeformGPU::STEP_STAMPS;
		gpu_host.slump_until = gpu_host.time + 1.0 + 0.1;
	}
	if (gpu_host.time <= gpu_host.slump_until && p_delta > 0.0) {
		flags |= GroundDeformGPU::STEP_SLUMP;
	}
	if (flags & (GroundDeformGPU::STEP_RESET | GroundDeformGPU::STEP_SHIFT | GroundDeformGPU::STEP_STAMPS | GroundDeformGPU::STEP_SLUMP | GroundDeformGPU::STEP_RELAX)) {
		flags |= GroundDeformGPU::STEP_BLIT;
		gpu_host.changed = true;
	}
	if (p_collision || p_sync) {
		flags |= GroundDeformGPU::STEP_COLLISION;
		if (gpu_host.changed || p_sync) {
			flags |= GroundDeformGPU::STEP_READBACK;
			gpu_host.changed = false;
		}
	}
	if (p_sync) {
		flags |= GroundDeformGPU::STEP_SYNC;
	}
	GroundDeformGPU::Dims d;
	d.n = deform_resolution;
	d.gn = ground_n;
	d.cn = cover_n;
	d.slots = MAX(max_cached_tiles, 1);
	d.coll_w = collision_enabled ? _gpu_collision_cells() + 1 : 0;
	gpu_host.gpu->step(d, flags, _gpu_params(p_delta, cover_n, cover_valid), ground_bytes, cover_cells, gpu_host.stamps, gpu_host.stores, gpu_host.shift, field.origin, mesh_center, gpu_host.uploads);
	gpu_host.stamps.clear();
	gpu_host.stores.clear();
	gpu_host.shift.clear();
	gpu_host.uploads.clear(); // tiles seeded into the cache (collapsed walls, follow-up 54)
	gpu_host.reset = false;
	if (p_sync) {
		_gpu_take();
	}
}

void SnowSurface3D::_gpu_take() {
	if (gpu_host.gpu.is_null()) {
		return;
	}
	if (gpu_host.take_texture()) {
		_update_material();
	}
	PackedFloat32Array heights;
	Vector2 center;
	if (gpu_host.gpu->take_collision(heights, center) && body && collision_shape.is_valid()) {
		const int cells = _gpu_collision_cells();
		const int w = cells + 1;
		if (heights.size() == w * w && center == mesh_center) {
			const float sp = _texel() * 2.0f;
			collision_node->set_scale(Vector3(sp, sp, sp));
			if (collision_shape->get_map_width() != w || collision_shape->get_map_depth() != w) {
				collision_shape->set_map_width(w);
				collision_shape->set_map_depth(w);
			}
			collision_shape->set_map_data(heights);
			collision_dirty = false;
		}
	}
	gpu_host.take_field(field, 1);
}

void SnowSurface3D::sync_deformation() {
	if (!_gpu_on() || !field.valid || !is_inside_tree()) {
		return;
	}
	_gpu_submit(0.0, true, true);
}

// ---------------------------------------------------------------------------------------------------------
// Queries

float SnowSurface3D::get_surface_height_at(Vector3 p_position) const {
	return _surface_at(p_position.x, p_position.z, _cover());
}

float SnowSurface3D::get_dent_at(Vector3 p_position) const {
	return field.dent_at(p_position.x, p_position.z);
}

float SnowSurface3D::get_rim_at(Vector3 p_position) const {
	return field.rim_at(p_position.x, p_position.z);
}

Vector2 SnowSurface3D::get_moved_volume() const {
	return field.moved_volume();
}

Rect2 SnowSurface3D::get_deform_rect() const {
	return field.valid ? field.rect() : Rect2(0, 0, deform_extent, deform_extent);
}

// ---------------------------------------------------------------------------------------------------------
// Settings

#define SNOW_SURFACE_REBUILD \
	mesh_dirty = true;       \
	ground_valid = false;    \
	if (is_inside_tree()) {  \
		_ensure_children();  \
		_follow(_follow_position()); \
	}

void SnowSurface3D::set_snow_cover(const NodePath &p_path) {
	snow_cover = p_path;
	if (is_inside_tree()) {
		ground_valid = false;
		_follow(_follow_position());
	}
}
void SnowSurface3D::set_extent(float p_extent) {
	extent = MAX(p_extent, deform_extent);
	SNOW_SURFACE_REBUILD
}
void SnowSurface3D::set_detail_spacing(float p_spacing) {
	detail_spacing = CLAMP(p_spacing, 0.02f, 4.0f);
	SNOW_SURFACE_REBUILD
}
void SnowSurface3D::set_max_spacing(float p_spacing) {
	max_spacing = CLAMP(p_spacing, detail_spacing, 16.0f);
	SNOW_SURFACE_REBUILD
}
void SnowSurface3D::set_ground_spacing(float p_spacing) {
	ground_spacing = CLAMP(p_spacing, 0.05f, 8.0f);
	SNOW_SURFACE_REBUILD
}
void SnowSurface3D::set_min_depth(float p_depth) {
	min_depth = MAX(p_depth, 0.001f);
	_update_material();
	collision_dirty = true;
}
void SnowSurface3D::set_edge_fade(float p_m) {
	edge_fade = MAX(p_m, 0.0f);
	_update_material();
}
void SnowSurface3D::set_micro_relief(float p_scale) {
	micro_relief = MAX(p_scale, 0.0f);
	_update_material();
}
void SnowSurface3D::set_tint(const Color &p_tint) {
	tint = p_tint;
	_update_material();
}
void SnowSurface3D::set_deform_enabled(bool p_enabled) {
	deform_enabled = p_enabled;
	field.invalidate();
	if (is_inside_tree()) {
		_follow(_follow_position());
	}
	_update_material();
	collision_dirty = true;
}
void SnowSurface3D::set_deform_extent(float p_extent) {
	deform_extent = CLAMP(p_extent, 4.0f, 512.0f);
	extent = MAX(extent, deform_extent);
	field.invalidate();
	field.clear();
	gpu_host.clear_cache();
	SNOW_SURFACE_REBUILD
}
void SnowSurface3D::set_deform_resolution(int p_resolution) {
	deform_resolution = CLAMP((p_resolution + TILE - 1) / TILE * TILE, TILE * 2, 4096);
	field.invalidate();
	field.clear();
	gpu_host.clear_cache();
	SNOW_SURFACE_REBUILD
}
void SnowSurface3D::set_collision_enabled(bool p_enabled) {
	collision_enabled = p_enabled;
	if (is_inside_tree()) {
		_ensure_children();
		_follow(_follow_position());
	}
}
void SnowSurface3D::set_collision_extent(float p_extent) {
	collision_extent = CLAMP(p_extent, 1.0f, deform_extent);
	collision_dirty = true;
}
void SnowSurface3D::set_collision_layer(uint32_t p_layer) {
	collision_layer = p_layer;
	if (body) {
		body->set_collision_layer(p_layer);
	}
}
void SnowSurface3D::set_collision_mask(uint32_t p_mask) {
	collision_mask = p_mask;
	if (body) {
		body->set_collision_mask(p_mask);
	}
}

void SnowSurface3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_snow_cover", "path"), &SnowSurface3D::set_snow_cover);
	ClassDB::bind_method(D_METHOD("get_snow_cover"), &SnowSurface3D::get_snow_cover);
	ClassDB::bind_method(D_METHOD("set_follow_target", "path"), &SnowSurface3D::set_follow_target);
	ClassDB::bind_method(D_METHOD("get_follow_target"), &SnowSurface3D::get_follow_target);
	ClassDB::bind_method(D_METHOD("set_extent", "extent"), &SnowSurface3D::set_extent);
	ClassDB::bind_method(D_METHOD("get_extent"), &SnowSurface3D::get_extent);
	ClassDB::bind_method(D_METHOD("set_detail_spacing", "spacing"), &SnowSurface3D::set_detail_spacing);
	ClassDB::bind_method(D_METHOD("get_detail_spacing"), &SnowSurface3D::get_detail_spacing);
	ClassDB::bind_method(D_METHOD("set_max_spacing", "spacing"), &SnowSurface3D::set_max_spacing);
	ClassDB::bind_method(D_METHOD("get_max_spacing"), &SnowSurface3D::get_max_spacing);
	ClassDB::bind_method(D_METHOD("set_ground_spacing", "spacing"), &SnowSurface3D::set_ground_spacing);
	ClassDB::bind_method(D_METHOD("get_ground_spacing"), &SnowSurface3D::get_ground_spacing);
	ClassDB::bind_method(D_METHOD("set_min_depth", "depth"), &SnowSurface3D::set_min_depth);
	ClassDB::bind_method(D_METHOD("get_min_depth"), &SnowSurface3D::get_min_depth);
	ClassDB::bind_method(D_METHOD("set_edge_fade", "distance"), &SnowSurface3D::set_edge_fade);
	ClassDB::bind_method(D_METHOD("get_edge_fade"), &SnowSurface3D::get_edge_fade);
	ClassDB::bind_method(D_METHOD("set_micro_relief", "scale"), &SnowSurface3D::set_micro_relief);
	ClassDB::bind_method(D_METHOD("get_micro_relief"), &SnowSurface3D::get_micro_relief);
	ClassDB::bind_method(D_METHOD("set_tint", "tint"), &SnowSurface3D::set_tint);
	ClassDB::bind_method(D_METHOD("get_tint"), &SnowSurface3D::get_tint);
	ClassDB::bind_method(D_METHOD("set_deform_enabled", "enabled"), &SnowSurface3D::set_deform_enabled);
	ClassDB::bind_method(D_METHOD("is_deform_enabled"), &SnowSurface3D::is_deform_enabled);
	ClassDB::bind_method(D_METHOD("set_deform_extent", "extent"), &SnowSurface3D::set_deform_extent);
	ClassDB::bind_method(D_METHOD("get_deform_extent"), &SnowSurface3D::get_deform_extent);
	ClassDB::bind_method(D_METHOD("set_deform_resolution", "resolution"), &SnowSurface3D::set_deform_resolution);
	ClassDB::bind_method(D_METHOD("get_deform_resolution"), &SnowSurface3D::get_deform_resolution);
	ClassDB::bind_method(D_METHOD("set_deform_groups", "groups"), &SnowSurface3D::set_deform_groups);
	ClassDB::bind_method(D_METHOD("get_deform_groups"), &SnowSurface3D::get_deform_groups);
	ClassDB::bind_method(D_METHOD("set_compacted_density", "density"), &SnowSurface3D::set_compacted_density);
	ClassDB::bind_method(D_METHOD("get_compacted_density"), &SnowSurface3D::get_compacted_density);
	ClassDB::bind_method(D_METHOD("set_rim_share", "share"), &SnowSurface3D::set_rim_share);
	ClassDB::bind_method(D_METHOD("get_rim_share"), &SnowSurface3D::get_rim_share);
	ClassDB::bind_method(D_METHOD("set_contact_tolerance", "distance"), &SnowSurface3D::set_contact_tolerance);
	ClassDB::bind_method(D_METHOD("get_contact_tolerance"), &SnowSurface3D::get_contact_tolerance);
	ClassDB::bind_method(D_METHOD("set_bekker_n", "n"), &SnowSurface3D::set_bekker_n);
	ClassDB::bind_method(D_METHOD("get_bekker_n"), &SnowSurface3D::get_bekker_n);
	ClassDB::bind_method(D_METHOD("set_bekker_kc", "kc"), &SnowSurface3D::set_bekker_kc);
	ClassDB::bind_method(D_METHOD("get_bekker_kc"), &SnowSurface3D::get_bekker_kc);
	ClassDB::bind_method(D_METHOD("set_bekker_kphi", "kphi"), &SnowSurface3D::set_bekker_kphi);
	ClassDB::bind_method(D_METHOD("get_bekker_kphi"), &SnowSurface3D::get_bekker_kphi);
	ClassDB::bind_method(D_METHOD("set_character_mass", "mass"), &SnowSurface3D::set_character_mass);
	ClassDB::bind_method(D_METHOD("get_character_mass"), &SnowSurface3D::get_character_mass);
	ClassDB::bind_method(D_METHOD("set_character_contact_area", "area"), &SnowSurface3D::set_character_contact_area);
	ClassDB::bind_method(D_METHOD("get_character_contact_area"), &SnowSurface3D::get_character_contact_area);
	ClassDB::bind_method(D_METHOD("set_refill_share", "share"), &SnowSurface3D::set_refill_share);
	ClassDB::bind_method(D_METHOD("get_refill_share"), &SnowSurface3D::get_refill_share);
	ClassDB::bind_method(D_METHOD("set_wind_fill_rate", "rate"), &SnowSurface3D::set_wind_fill_rate);
	ClassDB::bind_method(D_METHOD("get_wind_fill_rate"), &SnowSurface3D::get_wind_fill_rate);
	ClassDB::bind_method(D_METHOD("set_wall_collapse", "enabled"), &SnowSurface3D::set_wall_collapse);
	ClassDB::bind_method(D_METHOD("is_wall_collapse"), &SnowSurface3D::is_wall_collapse);
	ClassDB::bind_method(D_METHOD("get_walls_collapsed"), &SnowSurface3D::get_walls_collapsed);
	ClassDB::bind_method(D_METHOD("get_collapsed_volume"), &SnowSurface3D::get_collapsed_volume);
	ClassDB::bind_method(D_METHOD("crumble", "delta"), &SnowSurface3D::crumble);
	ClassDB::bind_method(D_METHOD("set_max_cached_tiles", "tiles"), &SnowSurface3D::set_max_cached_tiles);
	ClassDB::bind_method(D_METHOD("get_max_cached_tiles"), &SnowSurface3D::get_max_cached_tiles);
	ClassDB::bind_method(D_METHOD("set_collision_enabled", "enabled"), &SnowSurface3D::set_collision_enabled);
	ClassDB::bind_method(D_METHOD("is_collision_enabled"), &SnowSurface3D::is_collision_enabled);
	ClassDB::bind_method(D_METHOD("set_collision_extent", "extent"), &SnowSurface3D::set_collision_extent);
	ClassDB::bind_method(D_METHOD("get_collision_extent"), &SnowSurface3D::get_collision_extent);
	ClassDB::bind_method(D_METHOD("set_collision_layer", "layer"), &SnowSurface3D::set_collision_layer);
	ClassDB::bind_method(D_METHOD("get_collision_layer"), &SnowSurface3D::get_collision_layer);
	ClassDB::bind_method(D_METHOD("set_collision_mask", "mask"), &SnowSurface3D::set_collision_mask);
	ClassDB::bind_method(D_METHOD("get_collision_mask"), &SnowSurface3D::get_collision_mask);
	ClassDB::bind_method(D_METHOD("set_collision_rate", "rate"), &SnowSurface3D::set_collision_rate);
	ClassDB::bind_method(D_METHOD("get_collision_rate"), &SnowSurface3D::get_collision_rate);

	ClassDB::bind_method(D_METHOD("press", "position", "radius", "bottom", "sink", "round", "velocity"), &SnowSurface3D::press, DEFVAL(0.0f), DEFVAL(true), DEFVAL(Vector3()));
	ClassDB::bind_method(D_METHOD("press_box", "position", "size", "yaw", "bottom", "sink", "velocity"), &SnowSurface3D::press_box, DEFVAL(0.0f), DEFVAL(Vector3()));
	ClassDB::bind_static_method("SnowSurface3D", D_METHOD("bekker_sinkage", "pressure_kpa", "width", "n", "kc", "kphi"), &SnowSurface3D::bekker_sinkage);
	ClassDB::bind_static_method("SnowSurface3D", D_METHOD("rotten_share", "wetness"), &SnowSurface3D::rotten_share);
	ClassDB::bind_method(D_METHOD("clear_deformation"), &SnowSurface3D::clear_deformation);
	ClassDB::bind_method(D_METHOD("refresh_ground"), &SnowSurface3D::refresh_ground);
	ClassDB::bind_method(D_METHOD("get_surface_height_at", "position"), &SnowSurface3D::get_surface_height_at);
	ClassDB::bind_method(D_METHOD("set_gpu_simulation", "enabled"), &SnowSurface3D::set_gpu_simulation);
	ClassDB::bind_method(D_METHOD("is_gpu_simulation"), &SnowSurface3D::is_gpu_simulation);
	ClassDB::bind_method(D_METHOD("is_gpu_active"), &SnowSurface3D::is_gpu_active);
	ClassDB::bind_method(D_METHOD("sync_deformation"), &SnowSurface3D::sync_deformation);
	ClassDB::bind_method(D_METHOD("get_dent_at", "position"), &SnowSurface3D::get_dent_at);
	ClassDB::bind_method(D_METHOD("get_rim_at", "position"), &SnowSurface3D::get_rim_at);
	ClassDB::bind_method(D_METHOD("get_moved_volume"), &SnowSurface3D::get_moved_volume);
	ClassDB::bind_method(D_METHOD("set_crust_plates", "enabled"), &SnowSurface3D::set_crust_plates);
	ClassDB::bind_method(D_METHOD("get_crust_plates"), &SnowSurface3D::get_crust_plates);
	ClassDB::bind_method(D_METHOD("get_crust_plate_count"), &SnowSurface3D::get_crust_plate_count);
	ClassDB::bind_method(D_METHOD("set_clumps", "enabled"), &SnowSurface3D::set_clumps);
	ClassDB::bind_method(D_METHOD("is_clumps"), &SnowSurface3D::is_clumps);
	ClassDB::bind_method(D_METHOD("get_clumps_node"), &SnowSurface3D::get_clumps_node);
	ClassDB::bind_method(D_METHOD("get_cached_tile_count"), &SnowSurface3D::get_cached_tile_count);
	ClassDB::bind_method(D_METHOD("get_deform_rect"), &SnowSurface3D::get_deform_rect);
	ClassDB::bind_method(D_METHOD("get_step_time_msec"), &SnowSurface3D::get_step_time_msec);
	ClassDB::bind_method(D_METHOD("get_surface_material"), &SnowSurface3D::get_surface_material);

	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "snow_cover", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "SnowCover3D"), "set_snow_cover", "get_snow_cover");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "follow_target", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D"), "set_follow_target", "get_follow_target");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "extent", PROPERTY_HINT_RANGE, "8,1024,1,suffix:m"), "set_extent", "get_extent");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "detail_spacing", PROPERTY_HINT_RANGE, "0.02,4,0.005,suffix:m"), "set_detail_spacing", "get_detail_spacing");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "max_spacing", PROPERTY_HINT_RANGE, "0.1,16,0.05,suffix:m"), "set_max_spacing", "get_max_spacing");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ground_spacing", PROPERTY_HINT_RANGE, "0.05,8,0.05,suffix:m"), "set_ground_spacing", "get_ground_spacing");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "min_depth", PROPERTY_HINT_RANGE, "0.001,0.5,0.001,suffix:m"), "set_min_depth", "get_min_depth");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "edge_fade", PROPERTY_HINT_RANGE, "0,64,0.5,suffix:m"), "set_edge_fade", "get_edge_fade");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "micro_relief", PROPERTY_HINT_RANGE, "0,4,0.01"), "set_micro_relief", "get_micro_relief");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "tint", PROPERTY_HINT_COLOR_NO_ALPHA), "set_tint", "get_tint");
	ADD_GROUP("Deformation", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "deform_enabled"), "set_deform_enabled", "is_deform_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "clumps"), "set_clumps", "is_clumps");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "crust_plates"), "set_crust_plates", "get_crust_plates");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "deform_extent", PROPERTY_HINT_RANGE, "4,512,1,suffix:m"), "set_deform_extent", "get_deform_extent");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "deform_resolution", PROPERTY_HINT_RANGE, "128,4096,64"), "set_deform_resolution", "get_deform_resolution");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_STRING_ARRAY, "deform_groups"), "set_deform_groups", "get_deform_groups");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "compacted_density", PROPERTY_HINT_RANGE, "100,917,1,suffix:kg/m3"), "set_compacted_density", "get_compacted_density");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "rim_share", PROPERTY_HINT_RANGE, "0,0.9,0.01"), "set_rim_share", "get_rim_share");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "contact_tolerance", PROPERTY_HINT_RANGE, "0,0.5,0.005,suffix:m"), "set_contact_tolerance", "get_contact_tolerance");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "bekker_n", PROPERTY_HINT_RANGE, "0.3,3,0.01"), "set_bekker_n", "get_bekker_n");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "bekker_kc", PROPERTY_HINT_RANGE, "0,100,0.01"), "set_bekker_kc", "get_bekker_kc");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "bekker_kphi", PROPERTY_HINT_RANGE, "0.001,2000,0.01"), "set_bekker_kphi", "get_bekker_kphi");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "character_mass", PROPERTY_HINT_RANGE, "0,500,0.5,suffix:kg"), "set_character_mass", "get_character_mass");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "character_contact_area", PROPERTY_HINT_RANGE, "0.001,1,0.001,suffix:m2"), "set_character_contact_area", "get_character_contact_area");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "refill_share", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_refill_share", "get_refill_share");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wind_fill_rate", PROPERTY_HINT_RANGE, "0,10,0.01"), "set_wind_fill_rate", "get_wind_fill_rate");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_cached_tiles", PROPERTY_HINT_RANGE, "0,65536,1"), "set_max_cached_tiles", "get_max_cached_tiles");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "wall_collapse"), "set_wall_collapse", "is_wall_collapse");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "gpu_simulation"), "set_gpu_simulation", "is_gpu_simulation");
	ADD_GROUP("Collision", "collision_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "collision_enabled"), "set_collision_enabled", "is_collision_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "collision_extent", PROPERTY_HINT_RANGE, "1,512,0.5,suffix:m"), "set_collision_extent", "get_collision_extent");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_layer", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_collision_layer", "get_collision_layer");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_mask", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_collision_mask", "get_collision_mask");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "collision_rate", PROPERTY_HINT_RANGE, "0.5,120,0.5,suffix:Hz"), "set_collision_rate", "get_collision_rate");
}

SnowSurface3D::SnowSurface3D() {
	gpu_sink.surface = this;
	deform_groups.push_back("snow_deform");
	deform_groups.push_back("grass_trample");
	deform_groups.push_back("world_contact"); // every body any system should feel (docs/ideas/world-links.md)
	shader.instantiate();
	shader->set_code(SNOW_SURFACE_SHADER);
	material.instantiate();
	material->set_shader(shader);
}

SnowSurface3D::~SnowSurface3D() {
	_gpu_release();
}

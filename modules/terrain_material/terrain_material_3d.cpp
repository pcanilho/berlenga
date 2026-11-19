/**************************************************************************/
/*  terrain_material_3d.cpp                                               */
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

#include "terrain_material_3d.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"

#include "modules/terrain_deform/terrain_generator_3d.h"
#include "modules/vegetation/grass_field_3d.h"

// --- TerrainMaterialLayer -----------------------------------------------------------------------------------------

#define LAYER_SETTER(m_type, m_name) \
	void TerrainMaterialLayer::set_##m_name(m_type p_value) { \
		m_name = p_value; \
		emit_changed(); \
	}

LAYER_SETTER(const Ref<Texture2D> &, albedo_texture)
LAYER_SETTER(const Ref<Texture2D> &, normal_texture)
LAYER_SETTER(const Ref<Texture2D> &, roughness_texture)
LAYER_SETTER(const Ref<Texture2D> &, height_texture)
LAYER_SETTER(const Color &, color)

void TerrainMaterialLayer::set_roughness(float p_roughness) {
	roughness = CLAMP(p_roughness, 0.0f, 1.0f);
	emit_changed();
}

void TerrainMaterialLayer::set_normal_strength(float p_strength) {
	normal_strength = MAX(p_strength, 0.0f);
	emit_changed();
}

void TerrainMaterialLayer::set_tile_size(float p_size) {
	tile_size = MAX(p_size, 0.01f);
	emit_changed();
}

void TerrainMaterialLayer::set_porosity(float p_porosity) {
	porosity = CLAMP(p_porosity, 0.0f, 1.0f);
	emit_changed();
}

void TerrainMaterialLayer::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_albedo_texture", "texture"), &TerrainMaterialLayer::set_albedo_texture);
	ClassDB::bind_method(D_METHOD("get_albedo_texture"), &TerrainMaterialLayer::get_albedo_texture);
	ClassDB::bind_method(D_METHOD("set_normal_texture", "texture"), &TerrainMaterialLayer::set_normal_texture);
	ClassDB::bind_method(D_METHOD("get_normal_texture"), &TerrainMaterialLayer::get_normal_texture);
	ClassDB::bind_method(D_METHOD("set_roughness_texture", "texture"), &TerrainMaterialLayer::set_roughness_texture);
	ClassDB::bind_method(D_METHOD("get_roughness_texture"), &TerrainMaterialLayer::get_roughness_texture);
	ClassDB::bind_method(D_METHOD("set_height_texture", "texture"), &TerrainMaterialLayer::set_height_texture);
	ClassDB::bind_method(D_METHOD("get_height_texture"), &TerrainMaterialLayer::get_height_texture);
	ClassDB::bind_method(D_METHOD("set_color", "color"), &TerrainMaterialLayer::set_color);
	ClassDB::bind_method(D_METHOD("get_color"), &TerrainMaterialLayer::get_color);
	ClassDB::bind_method(D_METHOD("set_roughness", "roughness"), &TerrainMaterialLayer::set_roughness);
	ClassDB::bind_method(D_METHOD("get_roughness"), &TerrainMaterialLayer::get_roughness);
	ClassDB::bind_method(D_METHOD("set_normal_strength", "strength"), &TerrainMaterialLayer::set_normal_strength);
	ClassDB::bind_method(D_METHOD("get_normal_strength"), &TerrainMaterialLayer::get_normal_strength);
	ClassDB::bind_method(D_METHOD("set_tile_size", "metres"), &TerrainMaterialLayer::set_tile_size);
	ClassDB::bind_method(D_METHOD("get_tile_size"), &TerrainMaterialLayer::get_tile_size);
	ClassDB::bind_method(D_METHOD("set_porosity", "porosity"), &TerrainMaterialLayer::set_porosity);
	ClassDB::bind_method(D_METHOD("get_porosity"), &TerrainMaterialLayer::get_porosity);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "albedo_texture", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "set_albedo_texture", "get_albedo_texture");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "normal_texture", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "set_normal_texture", "get_normal_texture");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "roughness_texture", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "set_roughness_texture", "get_roughness_texture");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "height_texture", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "set_height_texture", "get_height_texture");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "color"), "set_color", "get_color");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "roughness", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_roughness", "get_roughness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "normal_strength", PROPERTY_HINT_RANGE, "0,4,0.01"), "set_normal_strength", "get_normal_strength");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "tile_size", PROPERTY_HINT_RANGE, "0.01,100,0.01,or_greater,suffix:m"), "set_tile_size", "get_tile_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "porosity", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_porosity", "get_porosity");
}

// --- TerrainMaterial3D --------------------------------------------------------------------------------------------

static const char *LAYER_NAMES[TerrainMaterial3D::LAYER_MAX] = { "rock", "scree", "soil", "alluvium", "sand" };
static const Color LAYER_DEFAULT_COLORS[TerrainMaterial3D::LAYER_MAX] = {
	Color(0.36, 0.35, 0.33), Color(0.41, 0.40, 0.37), Color(0.2, 0.22, 0.1), Color(0.28, 0.27, 0.16), Color(0.7, 0.62, 0.46)
};

String TerrainMaterial3D::get_shader_code() const {
	String c;
	c += "// TerrainMaterial3D (docs/ideas/terrain-material.md). Generated: edit the material, not this code.\n";
	c += "shader_type spatial;\nrender_mode cull_back;\n\n";
	c += TerrainGenerator3D::get_geology_shader_code();
	if (grass_tint_enabled) {
		c += GrassField3D::get_ground_shader_code();
	}
	const bool detail = !detail_code.is_empty() && detail_octaves > 0;
	if (detail) {
		c += detail_code;
	}
	// The engine's weather lines, the same text BaseMaterial3D writes into its own shader.
	if (wetness_enabled) {
		c += BaseMaterial3D::get_wetness_shader_code();
	}
	if (snow_enabled) {
		c += BaseMaterial3D::get_snow_shader_code();
	}
	if (ground_deform_enabled) {
		c += BaseMaterial3D::get_ground_deform_shader_code();
	}
	if (far_blend_enabled) {
		// The plant cover seen from afar (vegetation publishes it; rect width 0 = none: the ground stays as it is).
		c += R"(
global uniform sampler2D terrain_cover_map : hint_default_black, filter_linear; // rgb sRGB cover colour, a cover share
global uniform vec4 terrain_cover_rect;
global uniform sampler2D rock_contact_map : hint_default_black, filter_linear; // RockField3D: r damp ring, g banked fines
global uniform vec4 rock_contact_rect;
uniform float tm_far_start = 300.0;
uniform float tm_far_fade = 150.0;
)";
		if (far_start_from_vegetation) {
			// Where a streaming VegetationField3D's placed plants end (0: none streams).
			c += "global uniform float vegetation_near_radius;\n";
		}
	}
	if (sky_occlusion_enabled) {
		// The ground's sky occlusion (TerrainGenerator3D.sky_occlusion; rect width 0 = none).
		c += R"(
global uniform sampler2DArray terrain_sky_map : filter_linear; // the seen sky's nine harmonics, 4 + 4 + 1
global uniform vec4 terrain_sky_rect;
global uniform vec4 terrain_sky_full; // the whole sky's
global uniform vec4 terrain_sky_full2;
global uniform float terrain_sky_full3;
uniform float tm_sky_occlusion = 1.0;

// Irradiance on n from nine harmonics (Ramamoorthi and Hanrahan 2001), the basis of TerrainSky::sh_basis.
float tm_sky_irradiance(vec4 a, vec4 b, float c, vec3 n) {
	return 3.141593 * 0.282095 * a.x
			+ 2.094395 * 0.488603 * (a.y * n.y + a.z * n.z + a.w * n.x)
			+ 0.785398 * (1.092548 * (b.x * n.x * n.y + b.y * n.y * n.z + b.w * n.x * n.z)
					+ 0.315392 * b.z * (3.0 * n.z * n.z - 1.0) + 0.546274 * c * (n.x * n.x - n.y * n.y));
}
)";
	}
	c += R"(
uniform bool tm_geology = true;
uniform float tm_sea_level = 0.0;
uniform float tm_beach_height = 2.5;
uniform float tm_rock_cos = 0.766;
uniform float tm_cliff_cos = 0.643;
uniform float tm_rock_detail_scale = 1.0;
uniform float tm_scree_blocks_distance = 0.0;
uniform float tm_alluvium_area = 5.5;
uniform float tm_blend_depth = 0.2;
uniform int tm_detail_octaves = 4;
uniform float tm_wetness = 1.0;
uniform float tm_snow = 1.0;
uniform float tm_snow_max_slope = 60.0;
)";
	for (int i = 0; i < LAYER_MAX; i++) {
		const Ref<TerrainMaterialLayer> &l = layers[i];
		c += vformat("uniform vec4 tm_%s_color : source_color = vec4(1.0);\n", LAYER_NAMES[i]);
		c += vformat("uniform float tm_%s_roughness = 0.9;\nuniform float tm_%s_normal = 1.0;\nuniform float tm_%s_scale = 0.25;\nuniform float tm_%s_porosity = 0.7;\n", LAYER_NAMES[i], LAYER_NAMES[i], LAYER_NAMES[i], LAYER_NAMES[i]);
		if (l.is_valid() && l->get_albedo_texture().is_valid()) {
			c += vformat("uniform sampler2D tm_%s_albedo_tex : source_color, filter_linear_mipmap_anisotropic, repeat_enable;\n", LAYER_NAMES[i]);
		}
		if (l.is_valid() && l->get_normal_texture().is_valid()) {
			c += vformat("uniform sampler2D tm_%s_normal_tex : hint_normal, filter_linear_mipmap_anisotropic, repeat_enable;\n", LAYER_NAMES[i]);
		}
		if (l.is_valid() && l->get_roughness_texture().is_valid()) {
			c += vformat("uniform sampler2D tm_%s_roughness_tex : hint_default_white, filter_linear_mipmap_anisotropic, repeat_enable;\n", LAYER_NAMES[i]);
		}
		if (l.is_valid() && l->get_height_texture().is_valid()) {
			c += vformat("uniform sampler2D tm_%s_height_tex : hint_default_black, filter_linear_mipmap, repeat_enable;\n", LAYER_NAMES[i]);
		}
	}
	// World-space texturing: plain XZ on gentle ground, biplanar (Quilez 2020: the two dominant axes, two fetches)
	// past the cliff slope so steep faces do not stretch.
	c += R"(
struct TmProj {
	vec2 uv0;
	vec2 uv1;
	ivec2 axes0;
	ivec2 axes1;
	float w0;
	float w1;
};

TmProj tm_projection(vec3 p, vec3 n, float scale, bool cliff) {
	TmProj r;
	if (!cliff) {
		r.uv0 = p.xz * scale;
		r.axes0 = ivec2(0, 2);
		r.uv1 = r.uv0;
		r.axes1 = r.axes0;
		r.w0 = 1.0;
		r.w1 = 0.0;
		return r;
	}
	vec3 a = abs(n);
	int ma = (a.x > a.y && a.x > a.z) ? 0 : ((a.y > a.z) ? 1 : 2);
	int mi = (a.x < a.y && a.x < a.z) ? 0 : ((a.y < a.z) ? 1 : 2);
	int me = 3 - mi - ma;
	ivec2 ax0 = ivec2((ma + 1) % 3, (ma + 2) % 3);
	ivec2 ax1 = ivec2((me + 1) % 3, (me + 2) % 3);
	r.uv0 = vec2(p[ax0.x], p[ax0.y]) * scale;
	r.uv1 = vec2(p[ax1.x], p[ax1.y]) * scale;
	r.axes0 = ax0;
	r.axes1 = ax1;
	vec2 w = vec2(a[ma], a[me]);
	w = clamp((w - 0.5773) / (1.0 - 0.5773), 0.0, 1.0);
	w = w * w * w * w;
	float s = max(w.x + w.y, 1e-4);
	r.w0 = w.x / s;
	r.w1 = w.y / s;
	return r;
}

// Value noise for the layers' large-scale breakup (no textures needed).
float tm_vn_hash(vec3 p) {
	return fract(sin(dot(p, vec3(127.1, 311.7, 74.7))) * 43758.5453123);
}
float tm_vn(vec3 p) {
	vec3 i = floor(p);
	vec3 f = fract(p);
	f = f * f * (3.0 - 2.0 * f);
	return mix(mix(mix(tm_vn_hash(i), tm_vn_hash(i + vec3(1, 0, 0)), f.x), mix(tm_vn_hash(i + vec3(0, 1, 0)), tm_vn_hash(i + vec3(1, 1, 0)), f.x), f.y),
			mix(mix(tm_vn_hash(i + vec3(0, 0, 1)), tm_vn_hash(i + vec3(1, 0, 1)), f.x), mix(tm_vn_hash(i + vec3(0, 1, 1)), tm_vn_hash(i + vec3(1, 1, 1)), f.x), f.y), f.z);
}

// A tangent-space normal from a projection plane as a world-space perturbation (whiteout-style: added to the
// geometric normal along the plane's two axes).
vec3 tm_perturb(vec2 nt, ivec2 axes) {
	vec3 d = vec3(0.0);
	d[axes.x] = nt.x;
	d[axes.y] = nt.y;
	return d;
}
// Layer textures without visible tiling: hex-tiling (Mikkelsen 2022, "Practical Real-Time Hex-Tiling", after Heitz and
// Neyret 2018). The plane is cut into a triangle grid; each vertex owns a copy of the texture at its own random offset
// and rotation, and a pixel blends the three copies of its triangle by barycentric weights sharpened so mostly one
// copy shows. Gradients are rotated with the coordinates, so the mips stay right across the seams, and normal maps
// rotate their xy back into the layer's axes.
vec2 tm_hex_hash(ivec2 v) {
	uint h = uint(v.x) * 0x8DA6B343u ^ uint(v.y) * 0xD8163841u;
	h ^= h >> 13u;
	h *= 0x5BD1E995u;
	h ^= h >> 15u;
	return vec2(float(h & 0xFFFFu), float(h >> 16u)) / 65535.0;
}
vec4 tm_hex(sampler2D t, vec2 uv, vec2 dx, vec2 dy, bool nmap) {
	vec2 st = uv * 1.7320508; // triangles some two thirds of a texture tile on a side: smaller cut features apart
	vec2 sk = vec2(st.x - 0.57735027 * st.y, 1.15470054 * st.y);
	ivec2 b = ivec2(floor(sk));
	vec3 tmp = vec3(fract(sk), 0.0);
	tmp.z = 1.0 - tmp.x - tmp.y;
	float sg = step(0.0, -tmp.z);
	float s2 = 2.0 * sg - 1.0;
	vec3 w = vec3(-tmp.z * s2, sg - tmp.y * s2, sg - tmp.x * s2);
	int si = int(sg);
	ivec2 v[3] = ivec2[3](b + ivec2(si, si), b + ivec2(si, 1 - si), b + ivec2(1 - si, si));
	w = w * w * w * w;
	w /= max(w.x + w.y + w.z, 1e-6);
	vec4 acc = vec4(0.0);
	for (int k = 0; k < 3; k++) {
		vec2 hh = tm_hex_hash(v[k]);
		float a = hh.x * 6.2831853;
		mat2 r = mat2(vec2(cos(a), sin(a)), vec2(-sin(a), cos(a)));
		vec4 c = textureGrad(t, r * uv + hh * 17.0, r * dx, r * dy);
		if (nmap) {
			vec2 nt = transpose(r) * (c.xy * 2.0 - 1.0);
			c.xy = nt * 0.5 + 0.5;
		}
		acc += c * w[k];
	}
	return acc;
}
)";
	// Per layer: albedo, roughness, normal perturbation and height at this point.
	for (int i = 0; i < LAYER_MAX; i++) {
		const Ref<TerrainMaterialLayer> &l = layers[i];
		const String n = LAYER_NAMES[i];
		const bool has_a = l.is_valid() && l->get_albedo_texture().is_valid();
		const bool has_n = l.is_valid() && l->get_normal_texture().is_valid();
		const bool has_r = l.is_valid() && l->get_roughness_texture().is_valid();
		const bool has_h = l.is_valid() && l->get_height_texture().is_valid();
		c += vformat("\nvoid tm_%s(vec3 p, vec3 n, bool cliff, vec3 dpx, vec3 dpy, out vec3 albedo, out float rough, out vec3 dn, out float h) {\n", n);
		c += vformat("\tTmProj pr = tm_projection(p, n, tm_%s_scale, cliff);\n", n);
		if (has_a || has_n || has_r || has_h) {
			// The pixel's footprint in each projection's texture coordinates, for the hex-tiled samples' mips.
			c += vformat("\tvec2 dx0 = vec2(dpx[pr.axes0.x], dpx[pr.axes0.y]) * tm_%s_scale, dy0 = vec2(dpy[pr.axes0.x], dpy[pr.axes0.y]) * tm_%s_scale;\n", n, n);
			c += vformat("\tvec2 dx1 = vec2(dpx[pr.axes1.x], dpx[pr.axes1.y]) * tm_%s_scale, dy1 = vec2(dpy[pr.axes1.x], dpy[pr.axes1.y]) * tm_%s_scale;\n", n, n);
		}
		c += vformat("\talbedo = tm_%s_color.rgb;\n\trough = tm_%s_roughness;\n\tdn = vec3(0.0);\n\th = 0.5;\n", n, n);
		if (has_a) {
			c += vformat("\talbedo *= tm_hex(tm_%s_albedo_tex, pr.uv0, dx0, dy0, false).rgb * pr.w0 + (pr.w1 > 0.0 ? tm_hex(tm_%s_albedo_tex, pr.uv1, dx1, dy1, false).rgb * pr.w1 : vec3(0.0));\n", n, n);
		}
		if (has_r) {
			c += vformat("\trough *= tm_hex(tm_%s_roughness_tex, pr.uv0, dx0, dy0, false).g * pr.w0 + (pr.w1 > 0.0 ? tm_hex(tm_%s_roughness_tex, pr.uv1, dx1, dy1, false).g * pr.w1 : 0.0);\n", n, n);
		}
		if (has_n) {
			c += vformat("\tvec3 n0 = tm_hex(tm_%s_normal_tex, pr.uv0, dx0, dy0, true).xyz * 2.0 - 1.0;\n", n);
			c += vformat("\tdn = tm_perturb(n0.xy * tm_%s_normal, pr.axes0) * pr.w0;\n", n);
			c += vformat("\tif (pr.w1 > 0.0) {\n\t\tvec3 n1 = tm_hex(tm_%s_normal_tex, pr.uv1, dx1, dy1, true).xyz * 2.0 - 1.0;\n\t\tdn += tm_perturb(n1.xy * tm_%s_normal, pr.axes1) * pr.w1;\n\t}\n", n, n);
		}
		if (has_h) {
			c += vformat("\th = tm_hex(tm_%s_height_tex, pr.uv0, dx0, dy0, false).r * pr.w0 + (pr.w1 > 0.0 ? tm_hex(tm_%s_height_tex, pr.uv1, dx1, dy1, false).r * pr.w1 : 0.0);\n", n, n);
		}
		c += "}\n";
	}
	// Sand with no normal texture of its own: the surface's grain-scale relief (sorted grains, impact pits, a few
	// millimetres to some centimetres) as a bump from value noise with analytic derivatives (Quilez), three octaves,
	// each faded out as the pixel footprint nears its wavelength so it never shimmers. A light speckle of darker and
	// paler grains goes with it. Without it an untextured sand layer is one flat colour and the dunes' own shading
	// reads as smeared bands.
	{
		const Ref<TerrainMaterialLayer> &l = layers[LAYER_SAND];
		const bool grain = !(l.is_valid() && l->get_normal_texture().is_valid());
		c += R"(
vec3 tm_grain_n(vec2 x) {
	vec2 i = floor(x);
	vec2 f = x - i;
	vec2 u = f * f * (3.0 - 2.0 * f);
	vec2 du = 6.0 * f * (1.0 - f);
	ivec2 q = ivec2(i);
	float h[4];
	for (int k = 0; k < 4; k++) {
		uint hh = (uint(q.x + (k & 1)) * 0x8DA6B343u) ^ (uint(q.y + (k >> 1)) * 0xD8163841u);
		hh ^= hh >> 13u;
		hh *= 0x5BD1E995u;
		hh ^= hh >> 15u;
		h[k] = float(hh & 0xFFFFu) / 65535.0;
	}
	float e = h[0] - h[1] - h[2] + h[3];
	return vec3(h[0] + (h[1] - h[0]) * u.x + (h[2] - h[0]) * u.y + e * u.x * u.y, du * (vec2(h[1] - h[0], h[2] - h[0]) + e * u.yx));
}
)";
		if (grain) {
			c += R"(vec3 tm_sand_grain(vec3 p, float fp, inout vec3 albedo) {
	vec3 dn = vec3(0.0);
	// Dry sand has no relief of its own at these scales (its grains are under half a millimetre; the
	// centimetre relief is the wind ripples', GroundSurface3D): the octaves are mostly tone, with only a faint
	// tilt left. At full strength the 3.5 cm octave lit as an even stamp of dots under a low sun.
	const float L[3] = float[3](0.035, 0.014, 0.006);
	const float S[3] = float[3](0.0, 0.03, 0.03);
	for (int k = 0; k < 3; k++) {
		float f = 1.0 - smoothstep(0.25 * L[k], 0.6 * L[k], fp);
		if (f <= 0.0) {
			break;
		}
		vec3 v = tm_grain_n(p.xz / L[k] + vec2(17.0 * float(k), 5.0 * float(k)));
		dn -= vec3(v.y, 0.0, v.z) * S[k] * f;
		albedo *= 1.0 + (k == 2 ? 0.12 : 0.04) * (v.x - 0.5) * f;
	}
	return dn;
}
)";
		} else {
			c += "vec3 tm_sand_grain(vec3 p, float fp, inout vec3 albedo) {\n\treturn vec3(0.0);\n}\n";
		}
	}
	// Landscape3D's cliff relief: the shading normal per pixel from the same relief the vertices were pushed by
	// (Landscape3D::_cliff_offset, ported bit for bit), so ledges light cleanly whatever the triangles do. The mesh
	// hands the undisplaced point and scale in CUSTOM0 and the heightfield normal in CUSTOM1; other meshes leave
	// them zero and skip it.
	c += R"(
varying vec4 tm_cl_p;
varying vec3 tm_cl_n;
float tm_cl_hash(int a, int b, int c) {
	uint h = uint(a) * 0x8DA6B343u ^ uint(b) * 0xD8163841u ^ uint(c) * 0xCB1AB31Fu;
	h ^= h >> 13u;
	h *= 0x5BD1E995u;
	h ^= h >> 15u;
	return float(h & 0xFFFFFFu) / float(0xFFFFFF);
}
float tm_cl_noise(vec3 p) {
	vec3 i = floor(p);
	vec3 f = p - i;
	f = f * f * (3.0 - 2.0 * f);
	ivec3 q = ivec3(i);
	float a = mix(mix(tm_cl_hash(q.x, q.y, q.z + 7919), tm_cl_hash(q.x + 1, q.y, q.z + 7919), f.x), mix(tm_cl_hash(q.x, q.y + 1, q.z + 7919), tm_cl_hash(q.x + 1, q.y + 1, q.z + 7919), f.x), f.y);
	float b = mix(mix(tm_cl_hash(q.x, q.y, q.z + 7920), tm_cl_hash(q.x + 1, q.y, q.z + 7920), f.x), mix(tm_cl_hash(q.x, q.y + 1, q.z + 7920), tm_cl_hash(q.x + 1, q.y + 1, q.z + 7920), f.x), f.y);
	return mix(a, b, f.z);
}
float tm_cl_offset(vec3 p) {
	const float T = 3.0;
	float yb = p.y + (tm_cl_noise(p * 0.05) - 0.5) * 4.0;
	int bed = int(floor(yb / T));
	float fy = yb - float(bed) * T;
	float o0 = tm_cl_hash(bed, 11, 3) * 2.0 - 1.0;
	float o1 = tm_cl_hash(bed + 1, 11, 3) * 2.0 - 1.0;
	float ty = clamp((fy - (T - 1.5)) / 1.5, 0.0, 1.0);
	float o = mix(o0, o1, ty * ty * (3.0 - 2.0 * ty));
	float u = p.x * 0.8 + p.z * 0.6 + tm_cl_hash(bed, 5, 1) * 4.5;
	int jb = int(floor(u / 4.5));
	float fu = u - float(jb) * 4.5;
	float j0 = tm_cl_hash(bed, jb, 9) - 0.5, j1 = tm_cl_hash(bed, jb + 1, 9) - 0.5;
	float tu = clamp((fu - 3.0) / 1.5, 0.0, 1.0);
	o += 1.2 * mix(j0, j1, tu * tu * (3.0 - 2.0 * tu));
	o += 0.5 * (tm_cl_noise(p * 0.7) - 0.5) + 0.2 * (tm_cl_noise(p * 2.1 + vec3(3.0, 1.0, 7.0)) - 0.5);
	return o;
}
// The relief's normal at the undisplaced point p (landscape space), differenced over e (the pixel's footprint, so
// relief finer than a pixel fades instead of aliasing).
vec3 tm_cl_normal(vec3 p, vec3 nb, float k, float e) {
	vec3 m = normalize(vec3(nb.x, 0.0, nb.z));
	vec3 t1 = cross(nb, vec3(0.0, 1.0, 0.0));
	t1 = dot(t1, t1) > 1e-8 ? normalize(t1) : vec3(1.0, 0.0, 0.0);
	vec3 t2 = cross(t1, nb);
	vec3 a = p + t1 * e, b = p - t1 * e, c = p + t2 * e, d = p - t2 * e;
	vec3 du = (a + m * k * tm_cl_offset(a)) - (b + m * k * tm_cl_offset(b));
	vec3 dv = (c + m * k * tm_cl_offset(c)) - (d + m * k * tm_cl_offset(d));
	vec3 nn = cross(dv, du);
	nn = dot(nn, nb) < 0.0 ? -nn : nn;
	return dot(nn, nn) > 1e-12 ? normalize(nn) : nb;
}
)";
	if (cliff_self_shadow) {
		// Ledge self-shadows (cliff_self_shadow): the shadow map holds the relief only at the finest level and
		// too coarsely for a 0.5 m ledge past a few tens of metres, so a short march toward the sun through the
		// same analytic relief, as relief mapping's shadow ray (Policarpo et al. 2005; Tatarchuk 2006, parallax
		// occlusion mapping) but against the procedural offset instead of a height texture. A sample q is above
		// the relief when its distance out along m (from the base plane through p) exceeds the offset at its
		// foot. The finest noise term is left out of the march: its shadows are smaller than the steps.
		c += R"(
float tm_cl_offset_coarse(vec3 p) {
	const float T = 3.0;
	float yb = p.y + (tm_cl_noise(p * 0.05) - 0.5) * 4.0;
	int bed = int(floor(yb / T));
	float fy = yb - float(bed) * T;
	float o0 = tm_cl_hash(bed, 11, 3) * 2.0 - 1.0;
	float o1 = tm_cl_hash(bed + 1, 11, 3) * 2.0 - 1.0;
	float ty = clamp((fy - (T - 1.5)) / 1.5, 0.0, 1.0);
	float o = mix(o0, o1, ty * ty * (3.0 - 2.0 * ty));
	float u = p.x * 0.8 + p.z * 0.6 + tm_cl_hash(bed, 5, 1) * 4.5;
	int jb = int(floor(u / 4.5));
	float fu = u - float(jb) * 4.5;
	float j0 = tm_cl_hash(bed, jb, 9) - 0.5, j1 = tm_cl_hash(bed, jb + 1, 9) - 0.5;
	float tu = clamp((fu - 3.0) / 1.5, 0.0, 1.0);
	o += 1.2 * mix(j0, j1, tu * tu * (3.0 - 2.0 * tu));
	o += 0.5 * (tm_cl_noise(p * 0.7) - 0.5);
	return o;
}
// The sun's visibility over the relief at the undisplaced point p (landscape space): l toward the sun, e the pixel's
// footprint, j a 0-1 jitter for the steps.
float tm_cl_shadow(vec3 p, vec3 nb, float k, vec3 l, float e, float j) {
	vec3 m = normalize(vec3(nb.x, 0.0, nb.z));
	float mn = max(dot(m, nb), 0.3);
	float rise = dot(l, nb) / mn; // metres out of the face per metre along the ray
	if (rise <= 0.02) {
		return 1.0; // the face itself turns from the sun: its lighting and the shadow map have it
	}
	vec3 s0 = p + m * (k * tm_cl_offset_coarse(p));
	// Long enough to clear the relief's whole range (about 4 k) at that rise.
	float t_max = clamp(4.0 * k / rise, 1.0, 16.0);
	float vis = 1.0;
	const int STEPS = 12;
	for (int i = 0; i < STEPS; i++) {
		float u = (float(i) + j) / float(STEPS);
		float t = max(t_max * u * u, 2.0 * e);
		vec3 q = s0 + l * t;
		float s = dot(q - p, nb) / mn;
		float d = s - k * tm_cl_offset_coarse(q - m * s);
		// Soft over the sun's disc (0.53 degrees) or the pixel, whichever is wider.
		float w = max(t * 0.0093, e);
		vis = min(vis, smoothstep(-w, w, d + 0.5 * w));
	}
	return vis;
}
)";
	}
	c += "\nvoid vertex() {\n\ttm_cl_p = CUSTOM0;\n\ttm_cl_n = CUSTOM1.xyz;\n";
	if (ground_deform_enabled) {
		// Prints in the ground (GroundSurface3D): the terrain sinks under the printed patch, and the patch, drawn
		// with a copy of this material, takes the printed surface.
		c += "\tground_deform_vertex(MODEL_MATRIX, VERTEX, NORMAL, TANGENT, BINORMAL);\n";
	}
	c += "}\n";
	if (rock_detail_enabled) {
		c += R"(
// Rock detail on steep faces and scree with no textures (rock_detail_enabled): a height in metres from 3D noise in
// world space, so no projection stretches on any face, turned into a normal by Mikkelsen's (2010) surface gradient.
// Faces: joint blocks (3D Worley cells stretched into beds about 1.2 m thick and joints 2.5 m apart, Priest 1993
// spacing), their cracks, and two octaves of roughness. Scree: loose angular stones 0.15 to 0.5 m in a bed of gravel
// and fines.
// pcg3d (Jarzynski and Olano 2020, Hash Functions for GPU Rendering) on the integer cell: exact at any distance from
// the origin, where the old fract(sin()) hash lost its precision and left patches of the same cell values.
vec3 tm_rd_hash3(vec3 p) {
	uvec3 v = uvec3(ivec3(floor(p))) * 1664525u + 1013904223u;
	v.x += v.y * v.z;
	v.y += v.z * v.x;
	v.z += v.x * v.y;
	v ^= v >> 16u;
	v.x += v.y * v.z;
	v.y += v.z * v.x;
	v.z += v.x * v.y;
	return vec3(v) * (1.0 / 4294967296.0);
}
// F1, F2 and the nearest cell's hash.
vec3 tm_rd_worley(vec3 p) {
	vec3 i = floor(p);
	vec3 f = fract(p);
	float f1 = 8.0, f2 = 8.0, id = 0.0;
	for (int z = -1; z <= 1; z++) {
		for (int y = -1; y <= 1; y++) {
			for (int x = -1; x <= 1; x++) {
				vec3 o = vec3(float(x), float(y), float(z));
				vec3 hsh = tm_rd_hash3(i + o);
				vec3 d = o + hsh * 0.8 + 0.1 - f;
				float dd = dot(d, d);
				if (dd < f1) {
					f2 = f1;
					f1 = dd;
					id = hsh.x;
				} else if (dd < f2) {
					f2 = dd;
				}
			}
		}
	}
	return vec3(sqrt(f1), sqrt(f2), id);
}
// Worley with both nearest cells: x, y = F1, F2; z, w = their hashes; n = the direction from the nearest cell's point
// to the second's, the normal of the border between them; c and cb = from p to the nearest and second cells' points.
vec4 tm_rd_worley2(vec3 p, out vec3 n, out vec3 c, out vec3 cb) {
	vec3 i = floor(p);
	vec3 f = fract(p);
	float f1 = 8.0, f2 = 8.0, id1 = 0.0, id2 = 0.0;
	vec3 c1 = vec3(0.0), c2 = vec3(0.0);
	for (int z = -1; z <= 1; z++) {
		for (int y = -1; y <= 1; y++) {
			for (int x = -1; x <= 1; x++) {
				vec3 o = vec3(float(x), float(y), float(z));
				vec3 hsh = tm_rd_hash3(i + o);
				vec3 d = o + hsh * 0.8 + 0.1 - f;
				float dd = dot(d, d);
				if (dd < f1) {
					f2 = f1;
					id2 = id1;
					c2 = c1;
					f1 = dd;
					id1 = hsh.x;
					c1 = d;
				} else if (dd < f2) {
					f2 = dd;
					id2 = hsh.x;
					c2 = d;
				}
			}
		}
	}
	n = normalize(c2 - c1 + 1e-5);
	c = c1;
	cb = c2;
	return vec4(sqrt(f1), sqrt(f2), id1, id2);
}
float tm_rd_noise(vec3 p) {
	vec3 i = floor(p);
	vec3 f = fract(p);
	f = f * f * (3.0 - 2.0 * f);
	float a = tm_rd_hash3(i).x, b = tm_rd_hash3(i + vec3(1, 0, 0)).x, c = tm_rd_hash3(i + vec3(0, 1, 0)).x, d = tm_rd_hash3(i + vec3(1, 1, 0)).x;
	float e = tm_rd_hash3(i + vec3(0, 0, 1)).x, g = tm_rd_hash3(i + vec3(1, 0, 1)).x, h = tm_rd_hash3(i + vec3(0, 1, 1)).x, k = tm_rd_hash3(i + vec3(1, 1, 1)).x;
	return mix(mix(mix(a, b, f.x), mix(c, d, f.x), f.y), mix(mix(e, g, f.x), mix(h, k, f.x), f.y), f.z);
}
// Box-filtered pulse train (Apodaca and Gritz 2000, Advanced RenderMan ch. 11): the share of [x - w/2, x + w/2]
// inside cracks of width c (in periods) centred on every integer, so a pixel sees the cracks' area, never a line.
float tm_rd_cracks(float x, float c, float w) {
	w = max(w, 1e-4);
	float x0 = x - 0.5 * w + 0.5 * c, x1 = x + 0.5 * w + 0.5 * c;
	float i0 = floor(x0) * c + min(fract(x0), c), i1 = floor(x1) * c + min(fract(x1), c);
	return clamp((i1 - i0) / w, 0.0, 1.0);
}
// Frequency clamping (Norton, Rockwood and Skolmoski 1982): a feature of period per fades to its mean as a pixel
// spans a quarter to half of it, so nothing beats into moire at a distance or at a grazing angle.
float tm_rd_clamp(float w, float per) {
	return 1.0 - smoothstep(0.25, 0.5, w / per);
}
// One octave of scree stones at q (in cells): the stone's profile, 0 on the bed to 1 on the tallest tops; wv = the
// Worley values (F1, F2, the cell's hash), se = the distance inside the stone's outline (negative in the gaps).
// Each cell's polygon (straight borders: the distance to the bisector of the two nearest points, not F2 - F1, which
// bows the edges into rounded tiles) is inset by its own share of a cell, and only some cells hold a stone, so the
// stones stand apart in the bed instead of tiling it. A stone is an angular block: two broken faces at their own
// tilts meeting in a crest (the lower of two planes), steep sides, larger stones taller.
float tm_rd_stone1(vec3 q, float fill, float side, out vec3 wv, out float se) {
	vec3 n, c, cb;
	vec4 w4 = tm_rd_worley2(q, n, c, cb);
	wv = w4.xyz;
	float h2 = fract(w4.z * 17.31), h4 = fract(w4.z * 41.37);
	se = dot(0.5 * (c + cb), n) - mix(0.06, 0.24, h2 * h2);
	if (h4 > fill) {
		se = -0.5; // an empty cell: the bed
		return 0.0;
	}
	// Tumbled at all angles: faces tilted up to some 50 degrees, so a block shows a tall side and reads chunky,
	// not as a flat slab lying in the bed.
	vec3 g1 = (vec3(fract(w4.z * 5.77), fract(w4.z * 9.13), fract(w4.z * 3.71)) - 0.5) * 3.4;
	vec3 g2 = (vec3(fract(w4.z * 2.31), fract(w4.z * 6.53), fract(w4.z * 11.9)) - 0.5) * 3.4;
	float top = 0.35 + 0.55 * (1.0 - h2) + min(dot(-c, g1), dot(-c, g2) + 0.12);
	float sb = clamp(se / side, 0.0, 1.0);
	return clamp(top, 0.15, 1.0) * sb * (2.0 - sb);
}
// The scree's stones at p: blocks in cells of 0.45 m (about half the cells), chips in cells of 0.18 m between them
// (a second size class, as talus is poorly sorted), standing up to half as tall. wv and se come from whichever
// stone is on top (se in the large cells' units). kc fades the chips by frequency clamping (1 up close); side is the
// width of the stones' sides in cells (0.05, steep; the shadow rays take wider ones, see tm_rd_pom_shadow).
float tm_rd_stone(vec3 p, float kc, float side, out vec3 wv, out float se) {
	vec3 q = p / (0.45 * tm_rock_detail_scale);
	// Warped so the cells fall in no rows.
	q += (vec3(tm_rd_noise(q * 0.37), tm_rd_noise(q * 0.41 + 5.0), tm_rd_noise(q * 0.33 + 9.0)) - 0.5) * 0.9;
	float big = tm_rd_stone1(q, 0.55, side, wv, se);
	if (se > 0.05 || kc <= 0.0) {
		return big; // well inside a block (no chip can stand above it), or the chips are under a pixel
	}
	vec3 wv2;
	float se2;
	float small = 0.45 * kc * tm_rd_stone1(q * 2.5 + 31.0, 0.6, side, wv2, se2);
	if (small > big) {
		wv = wv2;
	}
	se = max(se, se2 / 2.5);
	return max(big, small);
}
// One block's relief (x, metres) and tone (y, 0 to 1) at q (metres over tm_rock_detail_scale), from its hash h and the
// vector c from q to its point in the stretched block space.
vec4 tm_rd_block(vec3 q, float h, vec3 c) {
	vec3 bh = vec3(h, fract(h * 7.13), fract(h * 13.7));
	float tilt = (tm_rd_noise(q * 0.6 + bh * 40.0) - 0.5) * 0.12;
	// Offsets and facets kept small (5 cm, some 4 degrees): across a shut joint the two blocks meet over a quarter
	// metre, and larger steps drew a crease along every border, a net of polygon outlines over a face seen close.
	vec3 bg = (vec3(bh.y, bh.z, fract(h * 29.3)) - 0.5) * vec3(0.14, 0.08, 0.14);
	float facet = dot(-c * vec3(2.6, 1.3, 3.4), bg);
	return vec4((bh.x - 0.5) * 0.05 + tilt + facet, h, 0.0, 0.0);
}
// Stones stand up to 0.3 m out of the bed: talus blocks are roughly as tall as they are wide (0.2 m read as flat
// slabs lying in the bed).
const float TM_RD_STONE_H = 0.3;
// Height (m) and shade (mean 1 over any area, so the detail never darkens the ground) at p. dx and dy are the axes of
// the pixel's footprint on the ground: the filter widths are anisotropic, long along a face seen edge-on.
// blocks: 0..1, the scree's stones (0 where real stones lie, only the gravel and fines bed left).
vec2 tm_rd_height(vec3 p, float face, float scree, float blocks, vec3 dx, vec3 dy) {
	float hgt = 0.0, shade = 1.0;
	float wmax = max(length(dx), length(dy)) / tm_rock_detail_scale;
	if (face > 0.0) {
		// Blocks bounded by bedding and joints: 3D Worley cells stretched along a regional strike (about 2.6 m along
		// it, 3.4 m across, 1.3 m between beds), warped, so the cracks run irregular and staggered, each block its
		// own shape, as jointed sedimentary and granitic walls break (Priest 1993: joint spacings vary widely within
		// a set). Straight parallel joint sets drew a regular grid of panels over whole faces.
		vec3 q = p / tm_rock_detail_scale;
		// One regional strike: an angle that varies over the ground, applied to world coordinates kilometres from
		// the origin, swung them by tens of metres per metre and crushed the blocks into dense black hatching.
		const vec2 da = vec2(0.766, 0.643), db = vec2(-0.643, 0.766);
		vec3 warp = vec3(tm_rd_noise(q * 0.21), tm_rd_noise(q * 0.23 + 3.0), tm_rd_noise(q * 0.19 + 7.0)) - 0.5;
		vec3 bq = vec3(dot(q.xz, da) / 2.6, q.y / 1.3, dot(q.xz, db) / 3.4) + warp * vec3(0.35, 0.5, 0.35);
		vec3 bn, bc, bc2;
		vec4 bw4 = tm_rd_worley2(bq, bn, bc, bc2);
		vec3 bw = bw4.xyz;
		// Distance to the block's border in metres (half of F2 - F1, times the cells' mean size).
		float d = 0.5 * (bw.y - bw.x) * 2.2;
		// Not every border is an open crack: bedding planes (borders facing up and down) run long and nearly all
		// open, cross joints are discontinuous and end against the beds (Priest 1993, joint persistence), so each
		// border between two blocks draws its own persistence from the pair; closed ones are hairlines or nothing,
		// and neighbouring blocks read as one larger mass instead of a paving of polygons.
		float pair = fract(sin(dot(vec2(min(bw4.z, bw4.w), max(bw4.z, bw4.w)), vec2(127.1, 311.7))) * 43758.5453);
		float bed = smoothstep(0.55, 0.85, abs(bn.y));
		// Joints come in sets (Priest 1993): bedding planes and steep cross joints. A border between two Worley
		// cells at neither attitude is an oblique cut no set makes; drawn open, those closed the cracks into rings
		// of polygons (a honeycomb over a sea cliff seen close). They stay shut, and every crack fades out along
		// its length where a noise says so, so the open ones end against each other instead of closing loops.
		float vert = 1.0 - smoothstep(0.1, 0.25, abs(bn.y));
		float set_k = max(smoothstep(0.8, 0.92, abs(bn.y)), vert);
		float persist = smoothstep(0.45, 0.6, tm_rd_noise(q * 0.45 + 31.0));
		float open = smoothstep(0.0, 0.25, mix(0.55, 0.9, bed) - pair) * set_k * persist;
		// Crack width wanders along the crack, 1 to 7 cm on open ones; a shut joint draws nothing (its hairline
		// outlined every cell).
		float cw = mix(0.0, mix(0.01, 0.07, tm_rd_noise(q * 0.9 + 11.0)), open);
		// The crack's share of the pixel: a box filter of the pixel's width over the crack's edge.
		float pw = wmax + 0.01;
		// Exact overlap of the pixel [d - pw/2, d + pw/2] with the crack [-cw/2, cw/2], so a crack narrower than a
		// pixel draws only its area (a hairline stays faint and continuous instead of breaking into dashes).
		float cov = clamp((min(d + 0.5 * pw, 0.5 * cw) - max(d - 0.5 * pw, -0.5 * cw)) / pw, 0.0, 1.0);
		// Cracks shade to 0.82 and the faces are lifted so the mean over the pattern stays 1 (cracks take some 4%
		// of a face, measured over the cells); past a block's size the pattern is gone to its mean.
		const float cov0 = 0.04;
		float kb = tm_rd_clamp(wmax, 1.2);
		float crack_shade = mix(1.0, mix(1.0, mix(0.9, 0.78, open), cov) / (1.0 - 0.18 * cov0), kb);
		// Block steps and crack relief are edges a pixel cannot hold: gone before one spans a crack's width.
		float kc = tm_rd_clamp(wmax, 0.2);
		// Per-block relief and tone for the nearest block and its neighbour across the border. Each block is its own
		// fracture face: a plane at its own tilt (up to some 4 degrees off the slope) through the block's point, so
		// neighbours catch the light differently, as the facets of a broken wall do.
		vec4 b1 = tm_rd_block(q, bw4.z, bc);
		vec4 b2 = tm_rd_block(q, bw4.w, bc2);
		// Height and tone stay continuous across every border, so no line appears where no crack is (a step there
		// also drew broken dashed lines from the pixel quads' derivatives): across a tight joint the two blocks
		// blend into each other over a crease some 0.25 m wide; on an open crack each block rounds down into it.
		float wx = 0.5 * (1.0 - smoothstep(0.0, max(0.25, 2.0 * pw), d));
		vec4 tight = mix(b1, b2, wx);
		float edge = smoothstep(0.0, max(0.12, 2.0 * pw), d);
		float blk = kc * mix(tight.x, b1.x * edge, open);
		// Tone meets halfway inside an open crack.
		float tone = mix(tight.y, mix(0.5 * (b1.y + b2.y), b1.y, edge), open);
		float rough = tm_rd_clamp(wmax, 0.77) * (tm_rd_noise(q * 1.3) - 0.5) * 0.08 + tm_rd_clamp(wmax, 0.24) * (tm_rd_noise(q * 4.1) - 0.5) * 0.025;
		// Crack relief: the block's edge rounds down into the crack.
		float crel = kc * open * (smoothstep(0.0, 0.06, d) - 1.0) * 0.06;
		hgt += face * (crel + blk + rough);
		shade = mix(shade, crack_shade * (1.0 + kb * 0.14 * (tone - 0.5)), face);
	}
	if (scree > 0.0) {
		vec3 wv;
		float se;
		float stone = blocks * tm_rd_stone(p, tm_rd_clamp(wmax, 0.18), 0.05, wv, se);
		float ks = tm_rd_clamp(wmax, 0.45);
		hgt += scree * ks * stone * TM_RD_STONE_H * tm_rock_detail_scale;
		// Each stone its own tone (fresher and older, lichened faces), mean 1; no darkened rim, which drew the
		// stones' outlines as grooves (paving). The bed between them is shaded by the stones themselves (parallax
		// shadows up close, the normal further off).
		float st_shade = mix(1.0, mix(1.0, 0.8 + 0.4 * wv.z, step(0.0, se)), blocks);
		// Between the stones, a matrix of gravel and fines (talus is poorly sorted: Fuller-like grading, each size
		// filling the gaps of the next, so the clasts stand in a finer bed). Gravel: grit patches a few
		// centimetres across in the bed; fines: grain roughness under a centimetre. Each fades by frequency
		// clamping as the pixel grows, and the gap share comes from the stones' own profile.
		float gap = mix(1.0, 1.0 - smoothstep(0.0, 0.03, se), blocks);
		float kg = tm_rd_clamp(wmax, 0.045);
		float gs = 1.0;
		if (kg > 0.0 && gap > 0.0) {
			// Grit and gravel as noise, not cells: patches of coarser grains standing a little proud of the fines,
			// with no outlines (cells of any kind stamped a pattern of outlined polygons over the bed).
			// Fine noise on coordinates wrapped every 64 m: world positions a kilometre out, times a few dozen per
			// metre, leave a float too few bits for the lattice's fraction and the noise breaks into a grid of dots.
			vec3 pw = mod(p, 64.0);
			float gn = tm_rd_noise(pw * (22.0 / tm_rock_detail_scale) + 17.0);
			float gn2 = tm_rd_noise(pw * (57.0 / tm_rock_detail_scale) + 3.0);
			// Each octave fades by its own period (4.5 and 1.75 cm): past them they beat into rings.
			float k2 = tm_rd_clamp(wmax, 0.0175);
			gn = mix(0.5, gn, kg);
			gn2 = mix(0.5, gn2, k2);
			float gc = smoothstep(0.45, 0.8, 0.6 * gn + 0.4 * gn2);
			float kf = tm_rd_clamp(wmax, 0.0167);
			float fines = kf * (tm_rd_noise(pw * (60.0 / tm_rock_detail_scale)) - 0.5) * 0.004 * tm_rock_detail_scale;
			hgt += scree * gap * (kg * (gc * 0.01 - 0.003) * tm_rock_detail_scale + fines);
			// Grains lighter and darker at random (mean about 1).
			gs = mix(1.0, (0.92 + 0.08 * gc) * (0.88 + 0.24 * gn2) / 0.955, kg);
		}
		shade = mix(shade, mix(1.0, st_shade * mix(1.0, gs, gap), ks), scree);
	}
	return vec2(hgt, shade);
}
// The perturbed normal (surface gradient of the height field) and the shade; past the clamps only the mean is left.
vec3 tm_rock_detail(vec3 p, vec3 n, vec3 sn, float face, float scree, float blocks, vec3 dx, vec3 dy, out float shade) {
	shade = 1.0;
	float wmax = max(length(dx), length(dy)) / tm_rock_detail_scale;
	if (face + scree <= 1e-3 || wmax > 0.6) {
		return sn; // every feature has faded to its mean (shade 1, flat)
	}
	// The difference step grows with the pixel, so it never samples across an edge the pixel cannot resolve.
	// Within the pixel: a step wider than it reached across a stone's foot from the bed (or onto the bed from a
	// side) and flipped the normal from pixel to pixel, a saw-tooth fringe along the stones' bases.
	float e = max(0.0015, 0.5 * wmax * tm_rock_detail_scale);
	vec2 h0 = tm_rd_height(p, face, scree, blocks, dx, dy);
	float hx = tm_rd_height(p + vec3(e, 0.0, 0.0), face, scree, blocks, dx, dy).x;
	float hy = tm_rd_height(p + vec3(0.0, e, 0.0), face, scree, blocks, dx, dy).x;
	float hz = tm_rd_height(p + vec3(0.0, 0.0, e), face, scree, blocks, dx, dy).x;
	vec3 grad = vec3(hx - h0.x, hy - h0.x, hz - h0.x) / e;
	vec3 sg = grad - n * dot(n, grad);
	// Once a pixel spans more than a few centimetres no detail tilts the normal past about 35 degrees: block edges
	// and crack lips are steeper than that over a few centimetres, and seen at a grazing angle or from afar those
	// slopes turned the normal away from every light, drawing hard black hatching over far faces. Up close the
	// stones keep their steep sides.
	float sgl = length(sg);
	sg *= min(1.0, mix(4.0, 0.7, smoothstep(0.01, 0.05, wmax)) / max(sgl, 1e-6));
	shade = h0.y;
	return normalize(sn - sg);
}
// A scree stone's colour (a factor on the scree's albedo) at p: each block its own grey, light to mid (weathered
// rinds differ), some fresh, paler and cooler breaks, a grain of crystals and pores, and crustose lichen in spots on
// the older blocks (grey-green, now and then yellow-green, as Rhizocarpon); the bed between them keeps its own
// colour. Each feature fades to its mean as the pixel grows past it (wmax, in rock-detail scales).
vec3 tm_rd_stone_color(vec3 p, float wmax) {
	float ks = tm_rd_clamp(wmax, 0.45);
	if (ks <= 0.0) {
		return vec3(1.25);
	}
	vec3 wv;
	float se;
	float st = tm_rd_stone(p, tm_rd_clamp(wmax, 0.18), 0.05, wv, se);
	// The bed: dry fines, paler and a little warmer than the blocks' weathered faces (dust); 1.25 is the mean
	// over blocks and bed, which the far ground keeps once the stones fade.
	const vec3 bed = vec3(1.3, 1.27, 1.2);
	const vec3 mean = vec3(1.25);
	float on = smoothstep(0.0, 0.02, se) * step(1e-3, st);
	if (on <= 0.0) {
		return mix(mean, bed, ks);
	}
	float hid = wv.z;
	float fresh = step(fract(hid * 7.71), 0.2);
	float tone = fract(hid * 3.37);
	vec3 col = mix(0.75, 1.5, tone) * mix(vec3(1.04, 1.0, 0.94), vec3(0.97, 1.0, 1.04), fract(hid * 19.3));
	col = mix(col, vec3(1.75, 1.78, 1.85), fresh);
	vec3 q = mod(p, 64.0) / tm_rock_detail_scale; // wrapped, as for the grit
	float kg = tm_rd_clamp(wmax, 0.006);
	col *= 1.0 + kg * (tm_rd_noise(q * 140.0) - 0.5) * 0.3 + tm_rd_clamp(wmax, 0.02) * (tm_rd_noise(q * 45.0 + 7.0) - 0.5) * 0.15;
	float kl = tm_rd_clamp(wmax, 0.05);
	float ln = 0.65 * tm_rd_noise(q * 14.0 + hid * 50.0) + 0.35 * tm_rd_noise(q * 38.0 + 11.0);
	float lichen = kl * (1.0 - fresh) * smoothstep(0.62, 0.68, ln) * step(0.35, fract(hid * 13.1));
	vec3 lc = fract(hid * 23.9) < 0.25 ? vec3(1.05, 1.15, 0.55) : vec3(1.1, 1.16, 1.0);
	col = mix(col, lc, lichen);
	return mix(mean, mix(bed, col, on), ks);
}
// Parallax occlusion mapping for the scree's stones (Tatarchuk 2006, Practical parallax occlusion mapping with
// approximate soft shadows), against the procedural stone profile instead of a height texture. The ground's mesh is
// the stones' tops; the gravel bed lies h below it. The view ray v steps down from p until it passes under a stone,
// then refines linearly between the last two steps. Returns the hit's ground position (on the base plane) and its
// depth below the mesh. j is a 0-1 jitter for the steps.
vec3 tm_rd_pom(vec3 p, vec3 n, vec3 v, float h, float j, float kc, float fp, out float depth) {
	float down = max(-dot(v, n), 0.08); // metres of depth per metre along the ray
	vec3 lat = v - n * dot(v, n); // the ray's drift across the plane per metre along it
	// Long enough to reach the bed, but never drifting more than a stone and a half across the plane.
	float t_max = min(h / down, 0.5 * tm_rock_detail_scale / max(length(lat), 1e-4));
	// Steps of at most 3 mm across the ground (or a pixel's footprint, where that is wider) (more the more grazing the view, as Tatarchuk 2006 scales them): a
	// step passing clean over a stone's corner or side drew the bed behind it, a comb of teeth along the stones'
	// outlines, wherever a step spanned more than a side's width.
	int STEPS = clamp(int(t_max * length(lat) / max(0.003 * tm_rock_detail_scale, fp)) + 8, 12, 96);
	float t0 = 0.0, d0 = 0.0, s0 = 0.0;
	vec3 wv;
	float se;
	for (int i = 1; i <= STEPS; i++) {
		float t = t_max * (float(i) - 1.0 + j) / float(STEPS - 1);
		float d = t * down; // the ray's depth
		float s = h * (1.0 - tm_rd_stone(p + lat * t, kc, 0.05, wv, se)); // the surface's depth there
		if (d >= s) {
			// Between the previous step (above) and this one (below): a few bisections first (the stones' steep
			// sides fall within one step, and a single linear guess drew them as stacked layers), then the linear
			// guess between the last two.
			float t1 = t;
			for (int k = 0; k < 4; k++) {
				float tm = 0.5 * (t0 + t1);
				float dm = tm * down;
				float sm = h * (1.0 - tm_rd_stone(p + lat * tm, kc, 0.05, wv, se));
				if (dm >= sm) {
					t1 = tm;
					d = dm;
					s = sm;
				} else {
					t0 = tm;
					d0 = dm;
					s0 = sm;
				}
			}
			float a = (s0 - d0) / max((s0 - d0) - (s - d), 1e-5);
			float th = mix(t0, t1, clamp(a, 0.0, 1.0));
			depth = th * down;
			return p + lat * th;
		}
		t0 = t;
		d0 = d;
		s0 = s;
	}
	depth = t_max * down;
	return p + lat * t_max;
}
// The sun's visibility from a parallax hit (ground position q, depth below the mesh) over the stones: l toward the
// sun, soft over a penumbra or the pixel e, whichever is wider.
float tm_rd_pom_shadow(vec3 q, float depth, vec3 n, vec3 l, float h, float e, float j) {
	float up = dot(l, n);
	if (up <= 0.02 || depth <= 1e-4) {
		return 1.0;
	}
	vec3 lat = l - n * up;
	float t_max = min(depth / up, 0.5 * tm_rock_detail_scale / max(length(lat), 1e-4));
	float vis = 1.0;
	vec3 wv;
	float se;
	// Steps on a lattice fixed in the world along the light (1 cm across the ground), not fractions of each pixel's
	// own ray: neighbouring pixels then sample the stones at the same points, and the shadow's outline no longer
	// breaks into a comb of teeth where a step lands just inside a stone for one pixel and just outside for the next.
	float latl = length(lat);
	if (latl < 1e-3) {
		return 1.0; // the sun overhead: the stones shade nothing beside them
	}
	float st = 0.01 * tm_rock_detail_scale;
	float off = fract(dot(q, lat / latl) / st);
	int STEPS = min(int(ceil(t_max * latl / st)) + 1, 32);
	for (int i = 1; i <= STEPS; i++) {
		// Half a step out at least: a first sample at the start point itself compared the surface with itself and
		// shadowed it by half, drawing contour lines and a grid of dots wherever the lattice passed near it.
		float t = min((float(i) - off + 0.5) * st / latl, t_max);
		float d = depth - t * up; // the ray's depth
		// Sides some 7 cm wide for the shadow, which softens its outline a little more.
		float s = h * (1.0 - tm_rd_stone(q + lat * t, 1.0, 0.15, wv, se)); // the surface's
		// A penumbra some three times the sun's disc: at the disc's own width a stone's edge falling between two
		// steps cut the shadow's outline into saw teeth.
		float w = max(t * 0.03, e);
		vis = min(vis, smoothstep(-w, w, s - d + 0.002 * tm_rock_detail_scale));
	}
	return vis;
}
)";
	}
	c += R"(
void fragment() {
	vec3 wp = (INV_VIEW_MATRIX * vec4(VERTEX, 1.0)).xyz;
	vec3 wn = normalize((INV_VIEW_MATRIX * vec4(NORMAL, 0.0)).xyz);
	bool cliff = wn.y < tm_cliff_cos;
	// The pixel's footprint on the ground for the layers' texture gradients, as a ray cone's ellipse (Akenine-Moller
	// et al. 2019), as the rock detail takes it: the path tracer has no screen derivatives.
	vec3 tm_dpx, tm_dpy;
	{
		vec3 v = wp - CAMERA_POSITION_WORLD;
		float cone = length(v) * 2.0 / (abs(PROJECTION_MATRIX[1][1]) * VIEWPORT_SIZE.y);
		v = normalize(v);
		vec3 t1 = cross(v, wn);
		t1 = dot(t1, t1) > 1e-8 ? normalize(t1) : normalize(cross(wn, vec3(1.0, 0.0, 0.0) + vec3(0.0, 0.0, 1.0) * step(0.9, abs(wn.x))));
		tm_dpx = t1 * cone;
		tm_dpy = cross(wn, t1) * cone / max(abs(dot(v, wn)), 0.05);
	}
	// Layer weights. Geology from the generator: bare rock, scree on it, the rest soil, alluvium where much water
	// drains; without it, rock past the rock slope and soil elsewhere. Sand on the beach band above the sea.
	float w[5];
	// The slope bands widen with the pixel's footprint: a far hill's mesh normal wanders by more than the band
	// within a pixel or two, and a narrow band drew thin rock-coloured lines along its slope contours there (the
	// far "swirls"). Close up they are as before.
	float tm_fp0 = length(wp - CAMERA_POSITION_WORLD) * 2.0 / (abs(PROJECTION_MATRIX[1][1]) * VIEWPORT_SIZE.y);
	float tm_sb = 1.0 + 2.0 * smoothstep(0.05, 0.6, tm_fp0);
	float steep = 1.0 - smoothstep(tm_rock_cos - 0.08 * tm_sb, tm_rock_cos + 0.04 * tm_sb, wn.y);
	float beach_sand = -1.0;
	float tm_till = 0.0;
	if (tm_geology && terrain_geology_rect.z > 0.0) {
		vec4 g = terrain_geology(wp);
		beach_sand = clamp(-g.x, 0.0, 1.0);
		// The generator knows where soil holds (Heimsath soil on slopes up to its repose); below its cells only
		// faces past the cliff slope are forced bare.
		float cliff_rock = 1.0 - smoothstep(tm_cliff_cos - 0.08 * tm_sb, tm_cliff_cos + 0.04 * tm_sb, wn.y);
		float rock = max(clamp(g.y, 0.0, 1.0), cliff_rock);
		// Moraine till (negative talus): unsorted debris under a thin soil, soil with stones through it (the height
		// blend lets the scree's stones stand out of the soil).
		float till = clamp(-g.z, 0.0, 1.0) * (1.0 - rock);
		tm_till = till;
		// Talus edges are lobed tongues of debris, not the geology map's smooth ovals: the cover is pushed in and
		// out by noise 7 to 40 m across, so a fan reads as lobes from afar. Not stretched down the fall line: turning
		// world coordinates by the pixel's own normal swung them by tens of metres per metre kilometres from the
		// origin, which drew moire swirls and speckle over far slopes.
		float sg = clamp(g.z, 0.0, 1.0);
		if (sg > 0.0) {
			vec3 sp = vec3(wp.x / 20.0, wp.y / 40.0, wp.z / 20.0);
			float edge = min(sg * 3.0, 1.0) * min((1.0 - sg) * 3.0, 1.0); // none inside the fan or off it
			sg = clamp(sg + edge * ((tm_vn(sp) - 0.5) * 0.9 + (tm_vn(sp * 3.1 + 7.0) - 0.5) * 0.3), 0.0, 1.0);
		}
		float scree = sg * (1.0 - rock) + 0.45 * till;
		float rest = max(1.0 - rock - scree, 0.0);
		// On a talus slope the ground between the stones is the debris's own fines (regolith), grey like the scree,
		// not the dark olive of a vegetated topsoil: rockfall keeps burying and stripping it, whatever depth of fine
		// material lies under it (a half-talus cone read olive all over, half its weight soil). Only the fan's
		// fringes and the till keep their soil; the sward grows on them by the generator's own rules.
		float regolith = smoothstep(0.05, 0.3, sg) * (1.0 - till);
		scree += rest * regolith;
		rest *= 1.0 - regolith;
		// Past the rock slope (40 degrees) beside a wall no soil holds, whatever the 8 m geology cell says: a steep gully
		// between a wall and its apron is thin grey debris (a smooth olive wedge with a hard edge at a cliff foot before), and
		// alluvium lies only on gentle ground (a channel's fill does not stand on a 45 degree gully floor).
		// Only beside walls and aprons (the geology's rock or scree near): open steep hillsides keep their soil and sward.
		float bare_near = steep * max(smoothstep(0.02, 0.2, clamp(g.y, 0.0, 1.0)), smoothstep(0.02, 0.2, clamp(g.z, 0.0, 1.0)));
		scree += rest * bare_near;
		rest *= 1.0 - bare_near;
		float alluvium = smoothstep(tm_alluvium_area, tm_alluvium_area + 1.0, g.w) * smoothstep(0.88, 0.95, wn.y);
		w[0] = rock;
		w[1] = scree;
		w[2] = rest * (1.0 - alluvium);
		w[3] = rest * alluvium;
	} else {
		w[0] = steep;
		w[1] = 0.0;
		w[2] = 1.0 - steep;
		w[3] = 0.0;
	}
	// Sand under the sea, and on land where the generator made a beach (its geology's soil channel is -1..0 there);
	// without a generator, on anything below beach_height. A low coastal plain behind the beach keeps its soil.
	float sand = beach_sand >= 0.0 ? max(beach_sand, 1.0 - smoothstep(-0.2, 0.3, wp.y - tm_sea_level)) : 1.0 - smoothstep(tm_beach_height * 0.6, tm_beach_height, wp.y - tm_sea_level);
	// Sand before steep faces shed it: no scree stones stand in the parallax on beaches, seabed or dunes, even where
	// a talus apron reaches under them (the stones would take the sand's colour and read as cracked sand plates).
	float tm_sand_all = sand;
	// Sand the generator laid (beaches, dunes) holds past the rock slope: dune faces stand at the angle of repose
	// (34 degrees, 45 under marram).
	// Past 47 degrees no sand holds at all: a wide band (to 56) left sand half-mixed over the steeper folds of sea
	// cliffs behind beaches, pale sand-coloured curtains running up the rock.
	float sand_steep = beach_sand > 0.0 ? mix(steep, 1.0 - smoothstep(0.682, 0.719, wn.y), beach_sand) : steep;
	sand *= 1.0 - sand_steep;
	for (int i = 0; i < 4; i++) {
		w[i] *= 1.0 - sand;
	}
	w[4] = sand;
	// Each layer present, then height blending (Mishkinis 2013): near a transition the layer standing higher in its
	// height map wins, so stones poke through sand instead of a soft cross-fade.
	vec3 alb[5];
	float rgh[5];
	vec3 dns[5];
	float hb[5];
	float top = -1e9;
	float tm_stone_h = 0.0;
	for (int i = 0; i < 5; i++) {
		alb[i] = vec3(0.0);
		rgh[i] = 1.0;
		dns[i] = vec3(0.0);
		hb[i] = -1e9;
	}
	if (w[0] > 1e-3) {
		vec3 a;
		float r;
		vec3 d;
		float h;
		tm_rock(wp, wn, cliff, tm_dpx, tm_dpy, a, r, d, h);
		alb[0] = a;
		rgh[0] = r;
		dns[0] = d;
		hb[0] = w[0] + h * tm_blend_depth * 2.0;
	}
	if (w[1] > 1e-3) {
		vec3 a;
		float r;
		vec3 d;
		float h;
		tm_scree(wp, wn, cliff, tm_dpx, tm_dpy, a, r, d, h);
		// Patches of fresher and older, lichened blocks: a tone that varies by about a fifth over 10 to 30 m (mean
		// kept), so a talus slope is not one flat colour at a distance (world coordinates, unrotated, as above).
		{
			vec3 sp = vec3(wp.x / 12.0, wp.y / 30.0, wp.z / 12.0);
			a *= 0.9 + 0.2 * tm_vn(sp + 3.0);
		}
		tm_stone_h = h;
		alb[1] = a;
		rgh[1] = r;
		dns[1] = d;
		hb[1] = w[1] + h * tm_blend_depth * 2.0;
	}
	if (w[2] > 1e-3) {
		vec3 a;
		float r;
		vec3 d;
		float h;
		tm_soil(wp, wn, cliff, tm_dpx, tm_dpy, a, r, d, h);
		alb[2] = a;
		rgh[2] = r;
		dns[2] = d;
		hb[2] = w[2] + h * tm_blend_depth * 2.0;
	}
	if (w[3] > 1e-3) {
		vec3 a;
		float r;
		vec3 d;
		float h;
		tm_alluvium(wp, wn, cliff, tm_dpx, tm_dpy, a, r, d, h);
		alb[3] = a;
		rgh[3] = r;
		dns[3] = d;
		hb[3] = w[3] + h * tm_blend_depth * 2.0;
	}
	if (w[4] > 1e-3) {
		vec3 a;
		float r;
		vec3 d;
		float h;
		tm_sand(wp, wn, cliff, tm_dpx, tm_dpy, a, r, d, h);
		d += tm_sand_grain(wp, tm_fp0, a);
		alb[4] = a;
		rgh[4] = r;
		dns[4] = d;
		hb[4] = w[4] + h * tm_blend_depth * 2.0;
	}
	for (int i = 0; i < 5; i++) {
		top = max(top, hb[i]);
	}
	float total = 0.0;
	vec3 albedo = vec3(0.0);
	float rough = 0.0;
	vec3 dn = vec3(0.0);
	float porosity = 0.0;
	float por[5] = float[5](tm_rock_porosity, tm_scree_porosity, tm_soil_porosity, tm_alluvium_porosity, tm_sand_porosity);
	// The height blend's band widens with the pixel's footprint (about 0.1 m at 70 m to 1 m at 700 m at 1080p): a
	// pixel far off spans many of the stones and patches whose heights decide the blend, so it takes their mix. A
	// fixed band cut thin slivers and squiggles wherever two layers' weights were close (talus edges, rock
	// exposure), which beat into moire on far slopes.
	float tm_hb_fp = length(wp - CAMERA_POSITION_WORLD) * 2.0 / (abs(PROJECTION_MATRIX[1][1]) * VIEWPORT_SIZE.y);
	float tm_hb_depth = mix(tm_blend_depth, max(tm_blend_depth, 1.0), smoothstep(0.1, 1.0, tm_hb_fp));
	for (int i = 0; i < 5; i++) {
		float b = w[i] > 1e-3 ? max(hb[i] - top + tm_hb_depth, 0.0) : 0.0;
		total += b;
		albedo += alb[i] * b;
		rough += rgh[i] * b;
		dn += dns[i] * b;
		porosity += por[i] * b;
	}
	total = max(total, 1e-5);
	albedo /= total;
	rough /= total;
	dn /= total;
	porosity /= total;
	vec3 sn = normalize(wn + dn);
	if (tm_cl_p.w > 0.0) {
		// Cliff relief: the face's normal per pixel (see tm_cl_normal), with the layers' own detail on top.
		float cl_fp = length(wp - CAMERA_POSITION_WORLD) * 2.0 / (abs(PROJECTION_MATRIX[1][1]) * VIEWPORT_SIZE.y);
		vec3 cl_n = tm_cl_normal(tm_cl_p.xyz, normalize(tm_cl_n), tm_cl_p.w, max(0.15, cl_fp));
		cl_n = normalize(mat3(MODEL_MATRIX) * cl_n);
		sn = normalize(cl_n + dn);
)";
	if (cliff_self_shadow) {
		// Where the relief is still wider than a pixel and the pixel faces the sun. sun_direction() is zero under
		// the path tracer, which traces the displaced faces' own shadows.
		c += R"(		vec3 cl_l = sun_direction();
		float cl_k = tm_cl_p.w;
		if (dot(cl_l, cl_l) > 0.5 && dot(sn, cl_l) > 0.0 && cl_k > 0.5 * cl_fp) {
			vec3 cl_ll = normalize(inverse(mat3(MODEL_MATRIX)) * cl_l);
			float cl_j = fract(52.9829189 * fract(dot(FRAGCOORD.xy, vec2(0.06711056, 0.00583715))));
			float cl_v = tm_cl_shadow(tm_cl_p.xyz, normalize(tm_cl_n), cl_k, cl_ll, cl_fp, cl_j);
			SUN_SHADOW = mix(1.0, cl_v, smoothstep(0.5 * cl_fp, cl_fp, cl_k));
		}
)";
	}
	c += R"(	}
)";
	if (detail) {
		c += R"(	// The generator's erosion detail below the vertex spacing, faded out where a pixel covers it.
	// The footprint's long axis (stretched by 1 / cos of the view to the ground, up to 10 times): a face seen edge-on
	// under a side sun drew the detail's octaves as fine diagonal hatching while the cone's width alone kept them.
	float footprint = length(wp - CAMERA_POSITION_WORLD) * 2.0 / (abs(PROJECTION_MATRIX[1][1]) * VIEWPORT_SIZE.y);
	footprint /= max(abs(dot(normalize(wp - CAMERA_POSITION_WORLD), wn)), 0.1);
	sn = terrain_detail_normal(wp, sn, tm_detail_octaves, footprint);
)";
	}
	if (rock_detail_enabled) {
		// Rock joints and loose stones where the ground is rock or scree (heightfield detail cannot reach steep faces).
		c += R"(	{
		// The pixel's footprint on the ground as a ray cone's ellipse (Akenine-Moller et al. 2019, texture LOD for
		// ray tracing): the cone's width across the view, stretched by 1 / cos along it. No screen derivatives, so
		// the path tracer (which has none) filters exactly as the raster does.
		vec3 rd_v = wp - CAMERA_POSITION_WORLD;
		float rd_cone = length(rd_v) * 2.0 / (abs(PROJECTION_MATRIX[1][1]) * VIEWPORT_SIZE.y);
		rd_v = normalize(rd_v);
		vec3 rd_t1 = cross(rd_v, wn);
		rd_t1 = dot(rd_t1, rd_t1) > 1e-8 ? normalize(rd_t1) : normalize(cross(wn, vec3(1.0, 0.0, 0.0) + vec3(0.0, 0.0, 1.0) * step(0.9, abs(wn.x))));
		vec3 rd_t2 = cross(wn, rd_t1);
		vec3 rd_dx = rd_t1 * rd_cone, rd_dy = rd_t2 * rd_cone / max(abs(dot(rd_v, wn)), 0.05);
		float rd_shade;
		// Joints and beds on bare faces steeper than about 45 degrees (full past 57); gentler rock keeps the
		// generator's own detail.
		float rd_face = clamp(w[0], 0.0, 1.0) * (1.0 - smoothstep(0.55, 0.72, wn.y));
		float rd_scree = clamp(w[1], 0.0, 1.0) * (1.0 - smoothstep(0.02, 0.25, tm_sand_all));
		// Inside scree_blocks_distance real stones (RockField3D's scree) lie on the talus: the shader keeps only their
		// bed of gravel and fines there, and its parallax blocks take over past it, over the last fifth.
		float rd_blocks = tm_scree_blocks_distance > 0.0 ? smoothstep(0.8 * tm_scree_blocks_distance, tm_scree_blocks_distance, length(wp - CAMERA_POSITION_WORLD)) : 1.0;
		vec3 rd_p = wp;
		// Scree stones in parallax: full while a pixel spans under 3 cm, gone by 8 cm, where the stones themselves
		// have faded to their mean; past a few metres the steps grow with the pixel, so the far blocks keep their sides
		// (the normal alone drew them as flat slabs) at a bounded cost.
		// The footprint's long axis, not the cone's width: at a grazing angle the parallax of stones a pixel cannot
		// resolve along the view stretched into hard black hatching across far slopes.
		float rd_fp = max(length(rd_dx), length(rd_dy));
		float rd_h = TM_RD_STONE_H * tm_rock_detail_scale * rd_scree * rd_blocks * (1.0 - smoothstep(0.03, 0.08, rd_fp / tm_rock_detail_scale));
		if (rd_h > 0.002) {
			// No per-pixel jitter of the steps: the bisection finds the stones' sides without it, and jitter drew a
			// dither over the stones and their shadows wherever no temporal filter averages it out.
			const float rd_j = 0.5;
			float rd_depth;
			// The chips fade as in the normal (tm_rd_height): at full height past their clamp they beat into rings.
			rd_p = tm_rd_pom(wp, wn, rd_v, rd_h, rd_j, tm_rd_clamp(rd_fp / tm_rock_detail_scale, 0.18), rd_fp, rd_depth);
			// The stones shade the bed and each other from the sun, under the path tracer too (no faces to trace).
			vec3 rd_l = relief_sun_direction();
			if (dot(rd_l, rd_l) > 0.5) {
				SUN_SHADOW *= tm_rd_pom_shadow(rd_p, rd_depth, wn, rd_l, rd_h, rd_cone, rd_j);
			}
		}
		sn = tm_rock_detail(rd_p, wn, sn, rd_face, rd_scree, rd_blocks, rd_dx, rd_dy, rd_shade);
		albedo *= rd_shade;
		if (rd_scree > 1e-3) {
			albedo *= mix(vec3(1.0), tm_rd_stone_color(rd_p, max(length(rd_dx), length(rd_dy)) / tm_rock_detail_scale), rd_scree * rd_blocks);
		}
	}
)";
	}
	if (far_blend_enabled) {
		c += "\tvec3 tm_bare = albedo;\n\tfloat tm_far_t = 0.0;\n\tfloat tm_far_cover = 0.0;\n";
	}
	if (ground_deform_enabled) {
		// The soil under the cover, for where the ground is broken open (below).
		c += "\tvec3 tm_soil = albedo;\n\tfloat tm_exposed = 0.0;\n";
	}
	if (grass_tint_enabled) {
		c += "\talbedo = grass_ground(wp, albedo);\n";
		// Moraine till keeps its stones above the turf: the scree layer's tallest blocks stand out of the sward
		// (a grassed moraine is soil and grass with boulders through it, not a meadow).
		// Without a scree height map there are no blocks to pick out: the till keeps the stones' share as a tint.
		const bool stone_h = layers[LAYER_SCREE].is_valid() && layers[LAYER_SCREE]->get_height_texture().is_valid();
		c += vformat("\tif (tm_till > 0.0) {\n\t\talbedo = mix(albedo, alb[1], tm_till * %s);\n\t}\n", stone_h ? "smoothstep(0.5, 0.7, tm_stone_h)" : "0.45");
		// Plants thinned away by distance leave their colour on the ground (VegetationField3D).
		c += "\talbedo = vegetation_far(wp, albedo, CAMERA_POSITION_WORLD);\n";
	}
	if (far_blend_enabled) {
		// Far ground: the bare ground under the plant cover's colour by its cover share, taking over from the near
		// grass tint and vegetation_far() between far_start and far_start + far_fade (one source for both: the
		// vegetation builds the cover map from the same colours). Before rain and snow, which still lie on it.
		// far_start_from_vegetation: the blend ends where a streaming field's placed plants end.
		c += far_start_from_vegetation ? "\tfloat tm_fs = vegetation_near_radius > 0.0 ? max(vegetation_near_radius - tm_far_fade, 0.0) : tm_far_start;\n" : "\tfloat tm_fs = tm_far_start;\n";
		c += R"(	if (terrain_cover_rect.z > 0.0) {
		float far_t = smoothstep(tm_fs, tm_fs + tm_far_fade, length(wp - CAMERA_POSITION_WORLD));
		vec2 cuv = (wp.xz - terrain_cover_rect.xy) / terrain_cover_rect.zw;
		if (far_t > 0.0 && all(greaterThanEqual(cuv, vec2(0.0))) && all(lessThanEqual(cuv, vec2(1.0)))) {
			vec4 cover = textureLod(terrain_cover_map, cuv, 0.0);
			vec3 cs = cover.rgb;
			vec3 cover_lin = mix(cs / 12.92, pow((cs + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), cs));
)";
		if (grass_tint_enabled) {
			// The same sun-azimuth lobes as grass_ground()'s far sward (the bake of the blades): brighter looking
			// down-sun, darker into it, so the far cover and the grass fields agree at any heading.
			c += R"(			vec2 tm_to = wp.xz - grass_ground_view.xz;
			float tm_to_len = length(tm_to);
			if (grass_ground_sun.z > 0.0 && tm_to_len > 1e-3) {
				float c1 = dot(tm_to / tm_to_len, grass_ground_sun.xy);
				cover_lin *= max(1.0 + grass_ground_sun.z * (grass_ground_far_lobe1.rgb * c1 + grass_ground_far_lobe2.rgb * (2.0 * c1 * c1 - 1.0)), vec3(0.1));
			}
			// The cover map holds the far colour at a 30 degree sun; this scales it to the sun's elevation, as the
			// grass fields' own far colour does.
			cover_lin *= grass_ground_far_scale.rgb;
)";
		}
		c += R"(
			albedo = mix(albedo, mix(tm_bare, cover_lin, cover.a), far_t);
			tm_far_t = far_t;
			tm_far_cover = cover.a;
		}
	}
)";
	}
	// Where stones sit (RockField3D.ground_contact): the soil beside them kept damp (darker, smoother: porous darkening
	// as for wet sand, Lagarde 2013) and the fines banked on their uphill side, a finer, browner skin with a little
	// grit, so a stone reads as sitting in the ground. Edges broken by noise; none on bare rock faces.
	c += R"(	if (rock_contact_rect.z > 0.0) {
		vec2 kuv = (wp.xz - rock_contact_rect.xy) / rock_contact_rect.zw;
		if (all(greaterThanEqual(kuv, vec2(0.0))) && all(lessThanEqual(kuv, vec2(1.0)))) {
			vec2 k = textureLod(rock_contact_map, kuv, 0.0).rg;
			float kn = tm_vn(wp * 3.1 + 17.0);
			float soft = smoothstep(0.5, 0.85, wn.y); // ground, not rock faces
			float damp = smoothstep(0.0, 0.8, k.r * (0.8 + 0.4 * kn)) * soft;
			float fines = smoothstep(0.15, 0.85, k.g * (0.7 + 0.6 * tm_vn(wp * 7.3 + 5.0))) * soft;
			float grit = tm_vn(wp * 38.0 + 3.0);
			vec3 fcol = albedo * vec3(0.72, 0.62, 0.48) * (grit > 0.74 ? 1.5 : (grit < 0.22 ? 0.65 : 1.0));
			albedo = mix(albedo, fcol, fines);
			albedo *= mix(1.0, 0.42, damp);
			rough = mix(rough, rough * 0.75, damp);
		}
	}
)";
	// Saturated sand: below the swash's reach (some 0.4 to 0.9 m over the still sea, wandering with the waves' runup)
	// and under the sea, beach and bar sand is wet through: darker and glossier (porous darkening, Lagarde 2013), so a
	// bar just awash reads as the sea's wet floor and not a pale dry plate laid on the water.
	c += R"(	{
		float runup = mix(0.4, 0.9, tm_vn(vec3(wp.x / 23.0, 0.0, wp.z / 23.0)));
		float soaked = w[4] * (1.0 - smoothstep(runup * 0.6, runup, wp.y - tm_sea_level));
		albedo *= mix(1.0, 0.55, soaked);
		rough = mix(rough, rough * 0.45, soaked);
	}
)";
	if (ground_deform_enabled) {
		// Freshly turned ground is darker until it dries.
		// Ruts, digs, puddles and tracked mud open the meadow's cover: the soil layer's own colour shows there,
		// its edge broken by noise, before the turned ground's tint (darker, wet) goes over it.
		c += R"(	tm_exposed = ground_deform_exposed(wp);
	if (tm_exposed > 0.0) {
		tm_exposed = clamp(tm_exposed * (0.75 + 0.5 * tm_vn(wp * 6.0 + 11.0)), 0.0, 1.0);
		albedo = mix(albedo, tm_soil, tm_exposed);
	}
)";
		c += "\tvec2 gd = ground_deform_tint(wp);\n\talbedo *= gd.x;\n\trough = clamp(rough * gd.y, 0.0, 1.0);\n";
		// Wind ripples past their real geometry (GroundSurface3D's 4 m window): the grown field's slope as a normal
		// over the sand of the patch, as BaseMaterial3D draws it.
		c += "\tvec2 gr_far = ground_ripple_far_slope(wp.xz, length(tm_dpx));\n\tsn = normalize(sn - vec3(gr_far.x, 0.0, gr_far.y));\n";
	}
	c += "\tfloat metallic = 0.0;\n\tfloat specular = 0.5;\n";
	if (grass_tint_enabled) {
		// The sward hides the soil's sheen (GrassField3D): without it the meadow past the blades reads pale grey.
		c += "\tspecular = grass_ground_specular(wp, specular);\n";
	}
	if (far_blend_enabled) {
		// Past far_start the cover map hides the sheen by its cover share, as the sward does near.
		c += "\tspecular = mix(specular, mix(0.5, 0.1, tm_far_cover), tm_far_t);\n";
	}
	if (ground_deform_enabled) {
		// The opened soil keeps its sheen (the sward hid it); standing water in ruts (mud stage 2) and the clay it
		// carries (round 3), as BaseMaterial3D draws them (material.cpp ground_water_look()).
		c += R"(	specular = mix(specular, 0.5, tm_exposed);
	{
		float gwd = ground_deform_water(wp);
		if (gwd > 0.0) {
			vec4 gwl = ground_water_look(gwd, ground_wet_at(wp, wn).x, ground_deform_water_mud(wp));
			albedo *= gwl.x;
			rough = mix(rough, gwl.z, max(gwl.y, 0.85 * gwl.w));
			specular = mix(specular, 0.25, max(gwl.y, gwl.w));
			sn = normalize(mix(sn, wn, max(gwl.y, 0.6 * gwl.w)));
		}
	}
)";
	}
	if (wetness_enabled) {
		// Rain soaks the ground by each layer's porosity (Lagarde 2013), before snow lies on it.
		c += "\tweather_wetness_apply(tm_wetness * weather_wetness_at(wp), wn.y, metallic, porosity, albedo, rough);\n";
		// Hoarfrost on clear cold nights (WeatherMap3D dew_and_frost).
		c += "\tweather_frost_apply(weather_frost_at(wp), wn.y, albedo, rough);\n";
	}
	if (snow_enabled) {
		// The snowpack over everything: slides off past its slope, settles first on the flattest facets of the
		// detail normal, then buries it.
		c += R"(	float snow = 0.0;
	float snow_wet = 0.0;
	{
		vec4 snow_s = snow_cover_at(wp);
		snow = snow_cover_weight(snow_s.x * tm_snow, tm_snow_max_slope, wn, sn);
		snow_wet = snow_s.z;
		snow_cover_apply(snow, snow_s.y, snow_wet, albedo, rough, metallic, specular);
		sn = normalize(mix(sn, wn, snow));
	}
)";
	}
	c += R"(	ALBEDO = albedo;
	ROUGHNESS = rough;
	METALLIC = metallic;
	SPECULAR = specular;
	NORMAL = normalize((VIEW_MATRIX * vec4(sn, 0.0)).xyz);
)";
	if (sky_occlusion_enabled) {
		// Raster ambient light sees only the sky the hills leave open (docs/ideas/terrain-material.md, "Sky
		// occlusion"): the irradiance of the sky this ground sees over that of the whole sky, both from nine
		// harmonics, on the shading normal. The path tracer finds the hills itself.
		c += R"(#if !defined(RT)
	if (terrain_sky_rect.z > 0.0 && tm_sky_occlusion > 0.0) {
		vec2 sky_size = vec2(textureSize(terrain_sky_map, 0).xy);
		vec2 sky_g = clamp((wp.xz - terrain_sky_rect.xy) / terrain_sky_rect.zw, 0.0, 1.0);
		vec2 sky_uv = (sky_g * (sky_size - 1.0) + 0.5) / sky_size;
		vec4 sky_a = textureLod(terrain_sky_map, vec3(sky_uv, 0.0), 0.0);
		vec4 sky_b = textureLod(terrain_sky_map, vec3(sky_uv, 1.0), 0.0);
		float sky_c = textureLod(terrain_sky_map, vec3(sky_uv, 2.0), 0.0).x;
		float sky_seen = max(tm_sky_irradiance(sky_a, sky_b, sky_c, sn), 0.0);
		float sky_all = max(tm_sky_irradiance(terrain_sky_full, terrain_sky_full2, terrain_sky_full3, sn), 1e-4);
		// The hidden sky is replaced by ground lit like this one, not by black: the multi-bounce fit of Jimenez et
		// al. 2016 (GTAO) on the ground's albedo.
		float sky_v = clamp(sky_seen / sky_all, 0.0, 1.0);
		float sky_alb = dot(albedo, vec3(0.2126, 0.7152, 0.0722));
		float sky_ma = 2.0404 * sky_alb - 0.3324;
		float sky_mb = -4.7951 * sky_alb + 0.6417;
		float sky_mc = 2.7552 * sky_alb + 0.6903;
		sky_v = max(sky_v, ((sky_v * sky_ma + sky_mb) * sky_v + sky_mc) * sky_v);
		AO = mix(1.0, sky_v, tm_sky_occlusion);
		AO_LIGHT_AFFECT = 0.0;
	}
#endif
)";
	}
	if (snow_enabled) {
		// Sparkle on dry snow. The ground has no mesh tangents, so the frame is built around the normal.
		c += R"(	{
		vec3 snow_t = normalize(cross(abs(sn.z) < 0.99 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0), sn));
		TANGENT = normalize((VIEW_MATRIX * vec4(snow_t, 0.0)).xyz);
		BINORMAL = normalize((VIEW_MATRIX * vec4(cross(sn, snow_t), 0.0)).xyz);
		float snow_pix = 2.0 / (abs(PROJECTION_MATRIX[1][1]) * VIEWPORT_SIZE.y);
		float snow_fp = PROJECTION_MATRIX[3][3] != 0.0 ? snow_pix : length(VERTEX) * snow_pix;
		snow_glint_apply(wp, wn, snow_fp, snow, snow_wet, VIEW_MATRIX, TANGENT, BINORMAL, NORMAL, NORMAL_MAP, NORMAL_MAP_DEPTH, CLEARCOAT, CLEARCOAT_ROUGHNESS);
	}
)";
	}
	c += "}\n";
	return c;
}

void TerrainMaterial3D::_update_params() {
	set_shader_parameter("tm_geology", geology_enabled);
	set_shader_parameter("tm_sea_level", sea_level);
	set_shader_parameter("tm_beach_height", beach_height);
	set_shader_parameter("tm_rock_cos", Math::cos(Math::deg_to_rad(rock_slope)));
	set_shader_parameter("tm_cliff_cos", Math::cos(Math::deg_to_rad(cliff_slope)));
	set_shader_parameter("tm_rock_detail_scale", rock_detail_scale);
	set_shader_parameter("tm_scree_blocks_distance", scree_blocks_distance);
	set_shader_parameter("tm_alluvium_area", alluvium_area);
	set_shader_parameter("tm_blend_depth", blend_depth);
	set_shader_parameter("tm_detail_octaves", detail_octaves);
	set_shader_parameter("tm_snow_max_slope", snow_max_slope);
	if (sky_occlusion_enabled) {
		set_shader_parameter("tm_sky_occlusion", sky_occlusion);
	}
	if (far_blend_enabled) {
		set_shader_parameter("tm_far_start", far_start);
		set_shader_parameter("tm_far_fade", far_fade);
	}
	for (int i = 0; i < LAYER_MAX; i++) {
		const String n = LAYER_NAMES[i];
		const Ref<TerrainMaterialLayer> &l = layers[i];
		set_shader_parameter("tm_" + n + "_color", l.is_valid() ? l->get_color() : LAYER_DEFAULT_COLORS[i]);
		set_shader_parameter("tm_" + n + "_roughness", l.is_valid() ? l->get_roughness() : 0.9f);
		set_shader_parameter("tm_" + n + "_normal", l.is_valid() ? l->get_normal_strength() : 1.0f);
		set_shader_parameter("tm_" + n + "_scale", 1.0f / (l.is_valid() ? l->get_tile_size() : 4.0f));
		set_shader_parameter("tm_" + n + "_porosity", l.is_valid() ? l->get_porosity() : 0.7f);
		if (l.is_valid()) {
			if (l->get_albedo_texture().is_valid()) {
				set_shader_parameter("tm_" + n + "_albedo_tex", l->get_albedo_texture());
			}
			if (l->get_normal_texture().is_valid()) {
				set_shader_parameter("tm_" + n + "_normal_tex", l->get_normal_texture());
			}
			if (l->get_roughness_texture().is_valid()) {
				set_shader_parameter("tm_" + n + "_roughness_tex", l->get_roughness_texture());
			}
			if (l->get_height_texture().is_valid()) {
				set_shader_parameter("tm_" + n + "_height_tex", l->get_height_texture());
			}
		}
	}
}

void TerrainMaterial3D::update_shader() {
	dirty = false;
	shader->set_code(get_shader_code());
	_update_params();
}

void TerrainMaterial3D::_queue_update() {
	if (dirty) {
		return;
	}
	dirty = true;
	callable_mp(this, &TerrainMaterial3D::update_shader).call_deferred();
}

void TerrainMaterial3D::_layer_changed() {
	_queue_update();
}

void TerrainMaterial3D::set_layer(Layer p_layer, const Ref<TerrainMaterialLayer> &p_value) {
	ERR_FAIL_INDEX(p_layer, LAYER_MAX);
	const Callable cb = callable_mp(this, &TerrainMaterial3D::_layer_changed);
	if (layers[p_layer].is_valid() && layers[p_layer]->is_connected(CoreStringName(changed), cb)) {
		layers[p_layer]->disconnect(CoreStringName(changed), cb);
	}
	layers[p_layer] = p_value;
	if (p_value.is_valid() && !p_value->is_connected(CoreStringName(changed), cb)) {
		p_value->connect(CoreStringName(changed), cb);
	}
	update_shader();
}

Ref<TerrainMaterialLayer> TerrainMaterial3D::get_layer(Layer p_layer) const {
	ERR_FAIL_INDEX_V(p_layer, LAYER_MAX, Ref<TerrainMaterialLayer>());
	return layers[p_layer];
}

#define MAT_SETTER(m_type, m_name, m_expr) \
	void TerrainMaterial3D::set_##m_name(m_type p_value) { \
		m_name = m_expr; \
		_update_params(); \
	}
#define MAT_SETTER_CODE(m_type, m_name) \
	void TerrainMaterial3D::set_##m_name(m_type p_value) { \
		m_name = p_value; \
		update_shader(); \
	}

MAT_SETTER(bool, geology_enabled, p_value)
MAT_SETTER(float, sea_level, p_value)
MAT_SETTER(float, beach_height, MAX(p_value, 0.0f))
MAT_SETTER(float, rock_slope, CLAMP(p_value, 0.0f, 90.0f))
MAT_SETTER(float, cliff_slope, CLAMP(p_value, 0.0f, 90.0f))
MAT_SETTER(float, alluvium_area, p_value)
MAT_SETTER(float, blend_depth, MAX(p_value, 0.001f))
MAT_SETTER(int, detail_octaves, CLAMP(p_value, 0, 8))
MAT_SETTER(float, snow_max_slope, CLAMP(p_value, 0.0f, 90.0f))
MAT_SETTER_CODE(const String &, detail_code)
MAT_SETTER_CODE(bool, grass_tint_enabled)
MAT_SETTER_CODE(bool, rock_detail_enabled)
MAT_SETTER(float, rock_detail_scale, CLAMP(p_value, 0.1f, 10.0f))
MAT_SETTER(float, scree_blocks_distance, MAX(p_value, 0.0f))
MAT_SETTER_CODE(bool, wetness_enabled)
MAT_SETTER_CODE(bool, snow_enabled)
MAT_SETTER_CODE(bool, ground_deform_enabled)
MAT_SETTER(float, far_start, MAX(p_value, 0.0f))
MAT_SETTER_CODE(bool, far_start_from_vegetation)
MAT_SETTER_CODE(bool, cliff_self_shadow)
MAT_SETTER(float, far_fade, MAX(p_value, 1.0f))

void TerrainMaterial3D::set_sky_occlusion_enabled(bool p_enabled) {
	sky_occlusion_enabled = p_enabled;
	update_shader();
}

MAT_SETTER(float, sky_occlusion, CLAMP(p_value, 0.0f, 1.0f))

void TerrainMaterial3D::set_far_blend_enabled(bool p_enabled) {
	far_blend_enabled = p_enabled;
	update_shader();
	_update_params();
}

void TerrainMaterial3D::use_generator(Object *p_generator) {
	TerrainGenerator3D *gen = Object::cast_to<TerrainGenerator3D>(p_generator);
	ERR_FAIL_NULL_MSG(gen, "use_generator() needs a TerrainGenerator3D.");
	ERR_FAIL_COND_MSG(!gen->has_result(), "Run the generator's generate() first.");
	sea_level = gen->get_sea_level();
	set_detail_code(gen->get_detail_shader_code());
}

void TerrainMaterial3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("update_shader"), &TerrainMaterial3D::update_shader);
	ClassDB::bind_method(D_METHOD("get_shader_code"), &TerrainMaterial3D::get_shader_code);
	ClassDB::bind_method(D_METHOD("set_layer", "layer", "value"), &TerrainMaterial3D::set_layer);
	ClassDB::bind_method(D_METHOD("get_layer", "layer"), &TerrainMaterial3D::get_layer);
	ClassDB::bind_method(D_METHOD("use_generator", "generator"), &TerrainMaterial3D::use_generator);
#define BIND_PROP(m_type, m_name, m_get, m_hint, m_hint_str) \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &TerrainMaterial3D::set_##m_name); \
	ClassDB::bind_method(D_METHOD(#m_get), &TerrainMaterial3D::m_get); \
	ADD_PROPERTY(PropertyInfo(m_type, #m_name, m_hint, m_hint_str), "set_" #m_name, #m_get);

	for (int i = 0; i < LAYER_MAX; i++) {
		ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, String(LAYER_NAMES[i]) + "_layer", PROPERTY_HINT_RESOURCE_TYPE, "TerrainMaterialLayer"), "set_layer", "get_layer", i);
	}
	BIND_PROP(Variant::BOOL, geology_enabled, is_geology_enabled, PROPERTY_HINT_NONE, "")
	BIND_PROP(Variant::FLOAT, sea_level, get_sea_level, PROPERTY_HINT_NONE, "suffix:m")
	BIND_PROP(Variant::FLOAT, beach_height, get_beach_height, PROPERTY_HINT_RANGE, "0,20,0.01,suffix:m")
	BIND_PROP(Variant::FLOAT, rock_slope, get_rock_slope, PROPERTY_HINT_RANGE, "0,90,0.1,degrees")
	BIND_PROP(Variant::FLOAT, cliff_slope, get_cliff_slope, PROPERTY_HINT_RANGE, "0,90,0.1,degrees")
	BIND_PROP(Variant::FLOAT, alluvium_area, get_alluvium_area, PROPERTY_HINT_RANGE, "2,10,0.01")
	BIND_PROP(Variant::FLOAT, blend_depth, get_blend_depth, PROPERTY_HINT_RANGE, "0.001,1,0.001")
	BIND_PROP(Variant::INT, detail_octaves, get_detail_octaves, PROPERTY_HINT_RANGE, "0,8")
	BIND_PROP(Variant::STRING, detail_code, get_detail_code, PROPERTY_HINT_MULTILINE_TEXT, "")
	BIND_PROP(Variant::BOOL, grass_tint_enabled, is_grass_tint_enabled, PROPERTY_HINT_NONE, "")
	BIND_PROP(Variant::BOOL, rock_detail_enabled, is_rock_detail_enabled, PROPERTY_HINT_NONE, "")
	BIND_PROP(Variant::FLOAT, rock_detail_scale, get_rock_detail_scale, PROPERTY_HINT_RANGE, "0.1,10,0.01")
	BIND_PROP(Variant::FLOAT, scree_blocks_distance, get_scree_blocks_distance, PROPERTY_HINT_RANGE, "0,500,0.1,or_greater,suffix:m")
	BIND_PROP(Variant::BOOL, wetness_enabled, is_wetness_enabled, PROPERTY_HINT_NONE, "")
	BIND_PROP(Variant::BOOL, snow_enabled, is_snow_enabled, PROPERTY_HINT_NONE, "")
	BIND_PROP(Variant::FLOAT, snow_max_slope, get_snow_max_slope, PROPERTY_HINT_RANGE, "0,90,0.1,degrees")
	BIND_PROP(Variant::BOOL, ground_deform_enabled, is_ground_deform_enabled, PROPERTY_HINT_NONE, "")
	BIND_PROP(Variant::BOOL, sky_occlusion_enabled, is_sky_occlusion_enabled, PROPERTY_HINT_NONE, "")
	BIND_PROP(Variant::FLOAT, sky_occlusion, get_sky_occlusion, PROPERTY_HINT_RANGE, "0,1,0.01")
	BIND_PROP(Variant::BOOL, far_blend_enabled, is_far_blend_enabled, PROPERTY_HINT_NONE, "")
	BIND_PROP(Variant::FLOAT, far_start, get_far_start, PROPERTY_HINT_RANGE, "0,5000,1,or_greater,suffix:m")
	BIND_PROP(Variant::FLOAT, far_fade, get_far_fade, PROPERTY_HINT_RANGE, "1,2000,1,or_greater,suffix:m")
	BIND_PROP(Variant::BOOL, far_start_from_vegetation, is_far_start_from_vegetation, PROPERTY_HINT_NONE, "")
	BIND_PROP(Variant::BOOL, cliff_self_shadow, is_cliff_self_shadow, PROPERTY_HINT_NONE, "")
#undef BIND_PROP

	BIND_ENUM_CONSTANT(LAYER_ROCK);
	BIND_ENUM_CONSTANT(LAYER_SCREE);
	BIND_ENUM_CONSTANT(LAYER_SOIL);
	BIND_ENUM_CONSTANT(LAYER_ALLUVIUM);
	BIND_ENUM_CONSTANT(LAYER_SAND);
	BIND_ENUM_CONSTANT(LAYER_MAX);
}

TerrainMaterial3D::TerrainMaterial3D() {
	shader.instantiate();
	set_shader(shader);
	update_shader();
}

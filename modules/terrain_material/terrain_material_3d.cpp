/**************************************************************************/
/*  terrain_material_3d.cpp                                               */
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

#include "terrain_material_3d.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "modules/terrain_deform/terrain_generator_3d.h"
#include "modules/vegetation/grass_field_3d.h"

// --- TerrainMaterialLayer -----------------------------------------------------------------------------------------

#define LAYER_SETTER(m_type, m_name)                         \
	void TerrainMaterialLayer::set_##m_name(m_type p_value) { \
		m_name = p_value;                                     \
		emit_changed();                                       \
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
	Color(0.36, 0.35, 0.33), Color(0.48, 0.46, 0.42), Color(0.2, 0.22, 0.1), Color(0.28, 0.27, 0.16), Color(0.7, 0.62, 0.46)
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
	c += R"(
uniform bool tm_geology = true;
uniform float tm_sea_level = 0.0;
uniform float tm_beach_height = 2.5;
uniform float tm_rock_cos = 0.766;
uniform float tm_cliff_cos = 0.643;
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

// A tangent-space normal from a projection plane as a world-space perturbation (whiteout-style: added to the
// geometric normal along the plane's two axes).
vec3 tm_perturb(vec3 nt, ivec2 axes) {
	vec3 d = vec3(0.0);
	d[axes.x] = nt.x;
	d[axes.y] = nt.y;
	return d;
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
		c += vformat("\nvoid tm_%s(vec3 p, vec3 n, bool cliff, out vec3 albedo, out float rough, out vec3 dn, out float h) {\n", n);
		c += vformat("\tTmProj pr = tm_projection(p, n, tm_%s_scale, cliff);\n", n);
		c += vformat("\talbedo = tm_%s_color.rgb;\n\trough = tm_%s_roughness;\n\tdn = vec3(0.0);\n\th = 0.5;\n", n, n);
		if (has_a) {
			c += vformat("\talbedo *= texture(tm_%s_albedo_tex, pr.uv0).rgb * pr.w0 + (pr.w1 > 0.0 ? texture(tm_%s_albedo_tex, pr.uv1).rgb * pr.w1 : vec3(0.0));\n", n, n);
		}
		if (has_r) {
			c += vformat("\trough *= texture(tm_%s_roughness_tex, pr.uv0).g * pr.w0 + (pr.w1 > 0.0 ? texture(tm_%s_roughness_tex, pr.uv1).g * pr.w1 : 0.0);\n", n, n);
		}
		if (has_n) {
			c += vformat("\tvec3 n0 = texture(tm_%s_normal_tex, pr.uv0).xyz * 2.0 - 1.0;\n", n);
			c += vformat("\tdn = tm_perturb(n0.xy * tm_%s_normal, pr.axes0) * pr.w0;\n", n);
			c += vformat("\tif (pr.w1 > 0.0) {\n\t\tvec3 n1 = texture(tm_%s_normal_tex, pr.uv1).xyz * 2.0 - 1.0;\n\t\tdn += tm_perturb(n1.xy * tm_%s_normal, pr.axes1) * pr.w1;\n\t}\n", n, n);
		}
		if (has_h) {
			c += vformat("\th = texture(tm_%s_height_tex, pr.uv0).r * pr.w0 + (pr.w1 > 0.0 ? texture(tm_%s_height_tex, pr.uv1).r * pr.w1 : 0.0);\n", n, n);
		}
		c += "}\n";
	}
	if (ground_deform_enabled) {
		// Prints in the ground (GroundSurface3D): the terrain sinks under the printed patch, and the patch, drawn
		// with a copy of this material, takes the printed surface.
		c += "\nvoid vertex() {\n\tground_deform_vertex(MODEL_MATRIX, VERTEX, NORMAL, TANGENT, BINORMAL);\n}\n";
	}
	c += R"(
void fragment() {
	vec3 wp = (INV_VIEW_MATRIX * vec4(VERTEX, 1.0)).xyz;
	vec3 wn = normalize((INV_VIEW_MATRIX * vec4(NORMAL, 0.0)).xyz);
	bool cliff = wn.y < tm_cliff_cos;
	// Layer weights. Geology from the generator: bare rock, scree on it, the rest soil, alluvium where much water
	// drains; without it, rock past the rock slope and soil elsewhere. Sand on the beach band above the sea.
	float w[5];
	float steep = 1.0 - smoothstep(tm_rock_cos - 0.08, tm_rock_cos + 0.04, wn.y);
	float beach_sand = -1.0;
	if (tm_geology && terrain_geology_rect.z > 0.0) {
		vec4 g = terrain_geology(wp);
		beach_sand = clamp(-g.x, 0.0, 1.0);
		// The generator knows where soil holds (Heimsath soil on slopes up to its repose); below its cells only
		// faces past the cliff slope are forced bare.
		float cliff_rock = 1.0 - smoothstep(tm_cliff_cos - 0.08, tm_cliff_cos + 0.04, wn.y);
		float rock = max(clamp(g.y, 0.0, 1.0), cliff_rock);
		float scree = clamp(g.z, 0.0, 1.0) * (1.0 - rock);
		float rest = max(1.0 - rock - scree, 0.0);
		float alluvium = smoothstep(tm_alluvium_area, tm_alluvium_area + 1.0, g.w);
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
	sand *= 1.0 - steep;
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
		tm_rock(wp, wn, cliff, a, r, d, h);
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
		tm_scree(wp, wn, cliff, a, r, d, h);
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
		tm_soil(wp, wn, cliff, a, r, d, h);
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
		tm_alluvium(wp, wn, cliff, a, r, d, h);
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
		tm_sand(wp, wn, cliff, a, r, d, h);
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
	for (int i = 0; i < 5; i++) {
		float b = w[i] > 1e-3 ? max(hb[i] - top + tm_blend_depth, 0.0) : 0.0;
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
)";
	if (detail) {
		c += R"(	// The generator's erosion detail below the vertex spacing, faded out where a pixel covers it.
	float footprint = length(wp - CAMERA_POSITION_WORLD) * 2.0 / (abs(PROJECTION_MATRIX[1][1]) * VIEWPORT_SIZE.y);
	sn = terrain_detail_normal(wp, sn, tm_detail_octaves, footprint);
)";
	}
	if (grass_tint_enabled) {
		c += "\talbedo = grass_ground(wp, albedo);\n";
	}
	if (ground_deform_enabled) {
		// Freshly turned ground is darker until it dries.
		c += "\tvec2 gd = ground_deform_tint(wp);\n\talbedo *= gd.x;\n\trough = clamp(rough * gd.y, 0.0, 1.0);\n";
	}
	c += "\tfloat metallic = 0.0;\n\tfloat specular = 0.5;\n";
	if (wetness_enabled) {
		// Rain soaks the ground by each layer's porosity (Lagarde 2013), before snow lies on it.
		c += "\tweather_wetness_apply(tm_wetness * weather_wetness_at(wp), wn.y, metallic, porosity, albedo, rough);\n";
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
	set_shader_parameter("tm_alluvium_area", alluvium_area);
	set_shader_parameter("tm_blend_depth", blend_depth);
	set_shader_parameter("tm_detail_octaves", detail_octaves);
	set_shader_parameter("tm_snow_max_slope", snow_max_slope);
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

#define MAT_SETTER(m_type, m_name, m_expr)                     \
	void TerrainMaterial3D::set_##m_name(m_type p_value) {     \
		m_name = m_expr;                                       \
		_update_params();                                      \
	}
#define MAT_SETTER_CODE(m_type, m_name)                        \
	void TerrainMaterial3D::set_##m_name(m_type p_value) {     \
		m_name = p_value;                                      \
		update_shader();                                       \
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
MAT_SETTER_CODE(bool, wetness_enabled)
MAT_SETTER_CODE(bool, snow_enabled)
MAT_SETTER_CODE(bool, ground_deform_enabled)

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
#define BIND_PROP(m_type, m_name, m_get, m_hint, m_hint_str)                                                          \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &TerrainMaterial3D::set_##m_name);                        \
	ClassDB::bind_method(D_METHOD(#m_get), &TerrainMaterial3D::m_get);                                                \
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
	BIND_PROP(Variant::BOOL, wetness_enabled, is_wetness_enabled, PROPERTY_HINT_NONE, "")
	BIND_PROP(Variant::BOOL, snow_enabled, is_snow_enabled, PROPERTY_HINT_NONE, "")
	BIND_PROP(Variant::FLOAT, snow_max_slope, get_snow_max_slope, PROPERTY_HINT_RANGE, "0,90,0.1,degrees")
	BIND_PROP(Variant::BOOL, ground_deform_enabled, is_ground_deform_enabled, PROPERTY_HINT_NONE, "")
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

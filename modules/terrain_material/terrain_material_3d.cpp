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

#include "core/io/config_file.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/image.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "scene/resources/image_texture.h"
#include "servers/rendering/rendering_server.h"

#include "modules/terrain_deform/terrain_generator_3d.h"
#include "modules/vegetation/grass_field_3d.h"

// --- TerrainMaterialLayer -----------------------------------------------------------------------------------------

#define LAYER_SETTER(m_type, m_name) \
	void TerrainMaterialLayer::set_##m_name(m_type p_value) { \
		m_name = p_value; \
		emit_changed(); \
	}

void TerrainMaterialLayer::set_albedo_texture(const Ref<Texture2D> &p_value) {
	albedo_texture = p_value;
	albedo_mean_valid = false;
	emit_changed();
}

void TerrainMaterialLayer::set_roughness_texture(const Ref<Texture2D> &p_value) {
	roughness_texture = p_value;
	roughness_mean_valid = false;
	emit_changed();
}

void TerrainMaterialLayer::set_detail_only(bool p_enable) {
	detail_only = p_enable;
	emit_changed();
}

// Mean of a texture over a 64x64 reduction (linear: sRGB decoded for colour), white without a readable image.
static Color _texture_mean(const Ref<Texture2D> &p_texture, bool p_srgb) {
	Color mean(1, 1, 1);
	if (p_texture.is_null()) {
		return mean;
	}
	Ref<Image> img = p_texture->get_image();
	if (img.is_null() || img->is_empty()) {
		return mean;
	}
	img = img->duplicate();
	if (img->is_compressed()) {
		img->decompress();
	}
	img->clear_mipmaps();
	img->convert(Image::FORMAT_RGBA8);
	img->resize(64, 64, Image::INTERPOLATE_BILINEAR);
	double sum[3] = { 0.0, 0.0, 0.0 };
	for (int y = 0; y < 64; y++) {
		for (int x = 0; x < 64; x++) {
			const Color c = p_srgb ? img->get_pixel(x, y).srgb_to_linear() : img->get_pixel(x, y);
			sum[0] += c.r;
			sum[1] += c.g;
			sum[2] += c.b;
		}
	}
	for (int k = 0; k < 3; k++) {
		mean[k] = float(MAX(sum[k] / 4096.0, 1e-3));
	}
	return mean;
}

float TerrainMaterialLayer::get_roughness_mean() const {
	if (!roughness_mean_valid) {
		roughness_mean = _texture_mean(roughness_texture, false).g;
		roughness_mean_valid = true;
	}
	return roughness_mean;
}

Color TerrainMaterialLayer::get_albedo_mean() const {
	if (!albedo_mean_valid) {
		albedo_mean = _texture_mean(albedo_texture, true);
		albedo_mean_valid = true;
	}
	return albedo_mean;
}
LAYER_SETTER(const Ref<Texture2D> &, normal_texture)
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
	ClassDB::bind_method(D_METHOD("set_detail_only", "enable"), &TerrainMaterialLayer::set_detail_only);
	ClassDB::bind_method(D_METHOD("is_detail_only"), &TerrainMaterialLayer::is_detail_only);
	ClassDB::bind_method(D_METHOD("get_roughness_mean"), &TerrainMaterialLayer::get_roughness_mean);
	ClassDB::bind_method(D_METHOD("get_albedo_mean"), &TerrainMaterialLayer::get_albedo_mean);
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
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "detail_only"), "set_detail_only", "is_detail_only");
}

// --- TerrainMaterial3D --------------------------------------------------------------------------------------------

static const char *LAYER_NAMES[TerrainMaterial3D::LAYER_MAX] = { "rock", "scree", "soil", "alluvium", "sand" };
static const char *OVERLAY_NAMES[TerrainMaterial3D::OVERLAY_MAX] = { "rock_variant", "cliff", "cured", "bare_soil", "bare_alluvium", "bank", "trail", "bed", "beach", "mud", "burn", "snow" };
// Which layers each overlay sits on and the mask (a component of the layer function's om or om2) that carries it.
// Overlays apply in this order, so a later one wins where masks overlap (a cliff face over the rock variant, mud over a trail).
#define TM_L(m_layer) (1 << TerrainMaterial3D::m_layer)
static const int OVERLAY_PARENTS[TerrainMaterial3D::OVERLAY_MAX] = {
	TM_L(LAYER_ROCK), TM_L(LAYER_ROCK), TM_L(LAYER_SOIL) | TM_L(LAYER_ALLUVIUM), TM_L(LAYER_SOIL), TM_L(LAYER_ALLUVIUM),
	TM_L(LAYER_SOIL) | TM_L(LAYER_ALLUVIUM), TM_L(LAYER_SOIL) | TM_L(LAYER_ALLUVIUM), TM_L(LAYER_ALLUVIUM), TM_L(LAYER_SAND),
	TM_L(LAYER_SOIL) | TM_L(LAYER_ALLUVIUM), TM_L(LAYER_SOIL) | TM_L(LAYER_ALLUVIUM), 0
};
#undef TM_L
static const char *OVERLAY_MASK[TerrainMaterial3D::OVERLAY_MAX] = { "om.y", "om.x", "om.x", "om.y", "om.y", "om.w", "om2.y", "om2.x", "om.x", "om.z", "om2.z", "" };
static const Color LAYER_DEFAULT_COLORS[TerrainMaterial3D::LAYER_MAX] = {
	Color(0.36, 0.35, 0.33), Color(0.41, 0.40, 0.37), Color(0.2, 0.22, 0.1), Color(0.28, 0.27, 0.16), Color(0.7, 0.62, 0.46)
};

String TerrainMaterial3D::get_shader_code() const {
	String c;
	c += "// TerrainMaterial3D (docs/ideas/terrain-material.md). Generated: edit the material, not this code.\n";
	c += "shader_type spatial;\nrender_mode cull_back;\n\n";
	c += TerrainGenerator3D::get_geology_shader_code();
	c += TerrainGenerator3D::get_sand_shader_code();
	c += R"(
// The dune tone's noise: the same hash and value noise as the ground's (material.cpp ground_ripple_hash and
// ground_ripple_vn), so the tone meets the GroundSurface3D patch's without a seam.
float tm_dune_hash(ivec2 p_c, uint p_s) {
	uint h = (uint(p_c.x) * 0x8da6b343u) ^ (uint(p_c.y) * 0xd8163841u) ^ (p_s * 0xcb1ab31fu);
	h = (h ^ (h >> 16u)) * 0x7feb352du;
	h = (h ^ (h >> 15u)) * 0x846ca68bu;
	h ^= h >> 16u;
	return float(h >> 8u) * (1.0 / 16777216.0);
}
float tm_dune_vn(vec2 p_p, uint p_s) {
	vec2 i = floor(p_p);
	vec2 f = p_p - i;
	vec2 u = f * f * (3.0 - 2.0 * f);
	ivec2 c = ivec2(i);
	return mix(mix(tm_dune_hash(c, p_s), tm_dune_hash(c + ivec2(1, 0), p_s), u.x), mix(tm_dune_hash(c + ivec2(0, 1), p_s), tm_dune_hash(c + ivec2(1, 1), p_s), u.x), u.y);
}
// The coarse lag's share on an interdune flat (the dune tone's lag term): the floors between dunes, flat, patchy.
float tm_dune_lag(vec2 p_xz, float p_normal_y, float p_interdune) {
	if (p_interdune < 0.0) {
		return 0.0;
	}
	float thin = p_interdune;
	return thin * smoothstep(0.93, 0.98, p_normal_y) * mix(0.45, 1.0, tm_dune_vn(p_xz / 23.0, 43u));
}
// Granule ripples (megaripples) on that lag: the coarse grains the wind cannot lift creep up into long, low ripples
// 0.5 to 2 m apart and up to 5 cm high (ripple index about 25), broad crests armoured with coarse granules over
// troughs of finer sand (Sharp 1963; Ellwood, Evans and Wilson 1975; Yizhaq 2008; Lammel et al. 2018). In plan the
// crests are sinuous and fork in Y-junctions, and each fades out along its length over a few metres. Crests run
// across p_wind, the dune-building wind (unit, x and z). p_weight: the lag's share; p_fp: the pixel's footprint (m),
// crests closer than about four pixels fade to their mean tone. Returns the slope (dh/dx, dh/dz); p_tint: the albedo
// factor (the crests' coarse grains, paler than the troughs' fines).
// The phase (in wavelengths): the spacing wanders between about 0.5 and 2 m (a warp along the wind whose slope adds
// to or takes from the 1.1 m mean) and the crests bend over metres; they end by tapering out (no phase defects:
// a defect's closed contours read as small rimmed pits).
float tm_granule_phase(vec2 p_xz, vec2 p_k, vec2 p_t) {
	float along = dot(p_xz, p_k);
	float across = dot(p_xz, p_t);
	float s = along / 1.1 + 2.5 * tm_dune_vn(vec2(along / 6.0, across / 5.0), 52u) + 1.2 * tm_dune_vn(p_xz / 9.0, 51u);
	return s;
}
vec2 tm_granule_ripples(vec2 p_xz, float p_weight, vec2 p_wind, float p_fp, out vec3 p_tint) {
	p_tint = vec3(1.0);
	if (p_weight <= 0.0 || dot(p_wind, p_wind) < 0.5) {
		return vec2(0.0);
	}
	vec2 k = p_wind;
	vec2 t = vec2(-k.y, k.x);
	float along = dot(p_xz, k);
	float across = dot(p_xz, t);
	// Crests tens of wavelengths long that taper out at their ends (or merge at a fork).
	float amp = p_weight * smoothstep(0.15, 0.5, tm_dune_vn(vec2(along / 3.0, across / 15.0), 53u));
	if (amp <= 0.0) {
		return vec2(0.0);
	}
	// The slope from the phase's own gradient (the warp turns the crests), by differences.
	float e = 0.03;
	float s = tm_granule_phase(p_xz, k, t);
	vec2 gs = vec2(tm_granule_phase(p_xz + vec2(e, 0.0), k, t) - s, tm_granule_phase(p_xz + vec2(0.0, e), k, t) - s);
	gs /= e;
	float c = 0.5 + 0.5 * cos(6.2831853 * s);
	// Resolved by the local spacing (1 / |grad s|).
	float lam = 1.0 / max(length(gs), 0.3);
	float resolved = 1.0 - smoothstep(0.15 * lam, 0.3 * lam, p_fp);
	vec3 crest = vec3(1.08, 1.05, 0.99);
	vec3 trough = vec3(0.95, 0.92, 0.87);
	// About a third of a ripple's length is crest: the unresolved pixel takes the mean.
	vec3 rt = mix(trough, crest, smoothstep(0.35, 0.8, c));
	vec3 mt = mix(trough, crest, 0.4);
	p_tint = mix(vec3(1.0), mix(mt, rt, resolved), amp);
	// Low and rounded (h = 0.04 amp c, slopes under 7 degrees, so a low sun leaves no hard-edged dark troughs):
	// dh/dp = 0.04 amp dc/ds grad s.
	float dhds = 0.04 * amp * (-3.14159265 * sin(6.2831853 * s));
	return resolved * dhds * gs;
}
// Dune-scale sand colour (docs/ideas/sand.md, "Dune-scale tone"): three of the changes a dune field shows from afar.
// Rubefaction: iron-oxide coats redden grains that have lain still for millennia (Walker 1979; Pye and Tsoar 2009),
// patchy over hundreds of metres and strongest on the stable sand of the interdunes. Interdune flats: the
// wind strips the fine sand and leaves a coarser, darker lag (Sharp 1963; Lancaster 1995). Slip faces: grain flows of
// fine, well-sorted sand, paler than the windward slope (Bagnold 1941). p_interdune: the interdune floor's share
// (TerrainGenerator3D.get_sand_interdune_at()), below 0 off the dune fields or unknown (only the slip faces show there).
vec3 tm_dune_tone(vec2 p_xz, float p_normal_y, float p_interdune) {
	vec3 t = vec3(1.0);
	if (p_interdune >= 0.0) {
		float thin = p_interdune;
		float age = smoothstep(0.25, 0.7, 0.65 * tm_dune_vn(p_xz / 420.0, 41u) + 0.35 * tm_dune_vn(p_xz / 130.0, 42u));
		t *= mix(vec3(1.0), vec3(1.15, 0.93, 0.78), age * (0.7 + 0.3 * thin));
		float lag = tm_dune_lag(p_xz, p_normal_y, p_interdune);
		t *= mix(vec3(1.0), vec3(0.84, 0.74, 0.62), lag);
	}
	// Slopes of about 29 to 33 degrees: the slip face at the sand's repose.
	float slip = smoothstep(0.78, 0.84, p_normal_y) * (1.0 - smoothstep(0.88, 0.93, p_normal_y));
	t *= mix(vec3(1.0), vec3(1.08, 1.07, 1.05), slip);
	return t;
}
)";
	// Soil bare of plants by the climate's dryness (WorldContracts::bare_share_for_precipitation, shared below): the soil
	// and alluvium layers' colours are a vegetated topsoil (olive); in a dry climate most of it is bare soil.
	c += R"(
global uniform sampler2D terrain_climate_map : hint_default_black, filter_linear; // TerrainDataMap3D: g precipitation m a year
global uniform vec4 terrain_climate_rect;
global uniform vec4 weather_climate; // WeatherMap3D's season climate: x precipitation mm a year, y soil moisture, z air temperature C, w 1 when set
uniform bool tm_bare_on = true;
uniform vec4 tm_bare_soil_color : source_color = vec4(0.49, 0.40, 0.33, 1.0);
uniform vec4 tm_bare_alluvium_color : source_color = vec4(0.60, 0.55, 0.49, 1.0);
uniform vec4 tm_cured_cover_color : source_color = vec4(0.52, 0.45, 0.30, 1.0);
)";
	// The shared bare-soil rule and patches (WorldContracts::climate_bare_shader_code(): wc_climate_bare, wc_bare_patch,
	// wc_climate_cured), the same the grass fields thin their blades by; before the grass ground code, whose turf cures
	// by wc_climate_cured (it checks WC_CLIMATE_CODE).
	c += WorldContracts::climate_bare_shader_code();
	if (grass_tint_enabled) {
		c += GrassField3D::get_ground_shader_code();
	}
	const bool detail = !detail_code.is_empty() && detail_octaves > 0;
	if (detail) {
		c += detail_code;
	}
	// The engine's weather lines, the same text BaseMaterial3D writes into its own shader.
	// The engine's camera cut-out (the span between the camera and its subject, which the path tracer also skips).
	c += BaseMaterial3D::get_camera_cutout_shader_code();
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
global uniform sampler2D rock_far_cover_map : hint_default_black, filter_linear; // RockField3D far tier: rgb stone colour, a cover
global uniform sampler2D rock_far_reach_map : hint_default_black, filter_linear; // RockField3D: rg the fade's start and end, 8 m a step (2040 m at 1)
global uniform sampler2D terrain_frost_map : hint_default_black, filter_linear; // RockField3D frost_sorting: r gate, g stone share, b dome
global uniform vec4 terrain_frost_rect; // its origin (x, z) and size (m); size 0 = none
global uniform vec4 rock_far_cover_rect;
global uniform vec4 rock_contact_rect;
global uniform sampler2D terrain_stand_map : hint_default_black, filter_linear; // Landscape3D: r trail wear, g fresh rooting, b burn (1 - years / 10.2, 0 never)
global uniform vec4 terrain_stand_rect;
global uniform vec4 terrain_cliff_relief; // Landscape3D: cliff_relief (m), grid spacing (m), grid origin (x, z); x 0 = none
global uniform vec4 terrain_bank_relief; // Landscape3D: bank_relief (m) in x (0 = none), terracette_relief (share) in y
uniform float tm_far_start = 300.0;
uniform float tm_far_fade = 150.0;
)";
		if (far_start_from_vegetation) {
			// Where a streaming VegetationField3D's placed plants end (0: none streams).
			c += "global uniform float vegetation_near_radius;\n";
		}
	}
	// Landscape3D's cliff relief share round the camera (its face weight): no soil on a shelf between walls.
	c += "global uniform sampler2D terrain_cliff_map : hint_default_black, filter_linear;\nglobal uniform vec4 terrain_cliff_rect; // origin (x, z), size (m); size 0 = none\n";
	if (sky_occlusion_enabled) {
		// The ground's sky occlusion (TerrainGenerator3D.sky_occlusion; rect width 0 = none).
		c += R"(
global uniform sampler2DArray terrain_sky_map : filter_linear; // the seen sky's nine harmonics, 4 + 4 + 1
global uniform vec4 terrain_sky_rect;
global uniform vec4 terrain_sky_full; // the whole sky's
global uniform vec4 terrain_sky_full2;
global uniform float terrain_sky_full3;
uniform float tm_sky_occlusion = 1.0;
// Far sun shadows (TerrainGenerator3D.sun_shadows; size zero = none): the ground's horizon tangent toward the sun on a
// grid turned to the sun's azimuth, frame (to the sun x, z, s0, t0), size (span s, span t, on).
global uniform sampler2D terrain_sun_horizon_map : filter_linear, repeat_disable;
global uniform vec4 terrain_sun_horizon_frame;
global uniform vec4 terrain_sun_horizon_size;
uniform float tm_far_shadow_start = 150.0;

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
uniform bool tm_dune_tone_on = true;
uniform bool tm_granule_on = true;
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
uniform float tm_face_weathering = 1.0; // rock faces weathered by aspect: lichen, moss, seep streaks (0 off)
uniform float tm_latitude = 46.0; // degrees, north positive: which aspects are shaded
uniform float tm_snow = 1.0;
uniform float tm_snow_max_slope = 60.0;

// The grains past where RockField3D's ground detail stops drawing them (set_ground_cover): each class's cover of the
// shore in two maps round the camera, drawn as the grains' aggregate look (Meng et al. 2015): single grains while a
// pixel resolves them, their mean colour once it does not, so the real stones hand over without a line.
uniform bool tm_gc_on = false;
uniform sampler2D tm_gc_near : filter_linear, repeat_disable;
uniform sampler2D tm_gc_far : filter_linear, repeat_disable;
uniform vec4 tm_gc_near_rect = vec4(0.0, 0.0, 1.0, 1.0); // origin x, z and size, m
uniform vec4 tm_gc_far_rect = vec4(0.0, 0.0, 1.0, 1.0);
uniform vec3 tm_gc_reach = vec3(0.0); // where each class's real stones stop (pebbles, gravel, fine), m
uniform vec3 tm_gc_band = vec3(1.0); // the band they hand over in (the class's cell size), m
uniform vec3 tm_gc_size = vec3(0.05, 0.03, 0.015); // each class's mean size, m
uniform vec3 tm_gc_colors[8]; // the rock types' mean albedo (linear)

vec3 tm_gc_hash3(vec2 c) {
	// Hash without sine (Hoskins).
	vec3 p3 = fract(vec3(c.xyx) * vec3(0.1031, 0.1030, 0.0973));
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.xxy + p3.yzz) * p3.zyx);
}

float tm_gc_noise(vec2 p) {
	vec2 i = floor(p);
	vec2 f = fract(p);
	f = f * f * (3.0 - 2.0 * f);
	return mix(mix(tm_gc_hash3(i).x, tm_gc_hash3(i + vec2(1.0, 0.0)).x, f.x), mix(tm_gc_hash3(i + vec2(0.0, 1.0)).x, tm_gc_hash3(i + vec2(1.0, 1.0)).x, f.x), f.y);
}

// The cover (rgb: pebbles, gravel, fine, as area fractions) and the rock type at xz: the near map, faded into the far
// one over its outer eighth.
vec3 tm_gc_cover(vec2 xz, out int type) {
	vec2 un = (xz - tm_gc_near_rect.xy) / tm_gc_near_rect.zw;
	vec2 uf = (xz - tm_gc_far_rect.xy) / tm_gc_far_rect.zw;
	vec2 en = min(un, 1.0 - un);
	float wn = smoothstep(0.0, 0.125, min(en.x, en.y));
	vec2 ef = min(uf, 1.0 - uf);
	float wf = smoothstep(0.0, 0.03, min(ef.x, ef.y));
	vec3 c = texture(tm_gc_far, uf).rgb * wf;
	if (wn > 0.0) {
		c = mix(c, texture(tm_gc_near, un).rgb, wn);
		type = int(texelFetch(tm_gc_near, ivec2(clamp(un, 0.0, 0.999) * vec2(textureSize(tm_gc_near, 0))), 0).a * 255.0 + 0.5) / 32;
	} else {
		type = int(texelFetch(tm_gc_far, ivec2(clamp(uf, 0.0, 0.999) * vec2(textureSize(tm_gc_far, 0))), 0).a * 255.0 + 0.5) / 32;
	}
	return c * 0.5;
}

// One class's grains at p (a cell 1.25 grains wide holds at most one, a share of cells filled for the cover):
// whether p is on one, its tone and its dome's normal (world). fp: the pixel's footprint.
float tm_gc_grain(vec2 p_world, float s, float cover, out float tone, out vec3 gn) {
	// Each class on its own lattice, turned off the world's axes (an axis-aligned lattice of near-regular grains beat
	// against the pixel grid in stripes from above).
	float ang = fract(s * 137.508) * 6.2831853;
	mat2 rot = mat2(vec2(cos(ang), sin(ang)), vec2(-sin(ang), cos(ang)));
	vec2 p = rot * p_world;
	float g = 1.25 * s;
	vec2 cell = floor(p / g);
	vec3 h = tm_gc_hash3(cell + 0.5 * s);
	tone = 0.7 + 0.5 * h.z;
	gn = vec3(0.0, 1.0, 0.0);
	if (h.x >= cover * 1.99) {
		return 0.0;
	}
	float r = 0.5 * s * (0.75 + 0.5 * fract(h.y * 7.13));
	vec2 o = (cell + 0.5 + (h.xy - 0.5) * 0.35) * g;
	vec2 d = (p - o) / r;
	float q = dot(d, d);
	if (q >= 1.0) {
		return 0.0;
	}
	float z = sqrt(1.0 - q);
	vec2 dw = transpose(rot) * d;
	gn = normalize(vec3(dw.x, z * 1.2, dw.y));
	tone *= 0.55 + 0.45 * z; // darker toward its foot (the grains round it and the bed)
	return 1.0;
}
)";
	for (int i = 0; i < LAYER_MAX; i++) {
		const Ref<TerrainMaterialLayer> &l = layers[i];
		c += vformat("uniform vec4 tm_%s_color : source_color = vec4(1.0);\n", LAYER_NAMES[i]);
		c += vformat("uniform float tm_%s_roughness = 0.9;\nuniform float tm_%s_normal = 1.0;\nuniform float tm_%s_scale = 0.25;\nuniform float tm_%s_porosity = 0.7;\n", LAYER_NAMES[i], LAYER_NAMES[i], LAYER_NAMES[i], LAYER_NAMES[i]);
		if (l.is_valid() && l->get_albedo_texture().is_valid()) {
			c += vformat("uniform sampler2D tm_%s_albedo_tex : source_color, filter_linear_mipmap_anisotropic, repeat_enable;\n", LAYER_NAMES[i]);
			c += vformat("uniform vec3 tm_%s_albedo_mean = vec3(1.0);\n", LAYER_NAMES[i]);
		}
		if (l.is_valid() && l->get_normal_texture().is_valid()) {
			c += vformat("uniform sampler2D tm_%s_normal_tex : hint_normal, filter_linear_mipmap_anisotropic, repeat_enable;\n", LAYER_NAMES[i]);
		}
		if (l.is_valid() && l->get_roughness_texture().is_valid()) {
			c += vformat("uniform sampler2D tm_%s_roughness_tex : hint_default_white, filter_linear_mipmap_anisotropic, repeat_enable;\n", LAYER_NAMES[i]);
			c += vformat("uniform float tm_%s_roughness_mean = 1.0;\n", LAYER_NAMES[i]);
		}
		if (l.is_valid() && l->get_height_texture().is_valid()) {
			c += vformat("uniform sampler2D tm_%s_height_tex : hint_default_black, filter_linear_mipmap, repeat_enable;\n", LAYER_NAMES[i]);
		}
	}
	for (int i = 0; i < OVERLAY_MAX; i++) {
		const Ref<TerrainMaterialLayer> &l = overlays[i];
		if (l.is_null()) {
			continue;
		}
		const String n = String("ov_") + OVERLAY_NAMES[i];
		c += vformat("uniform float tm_%s_normal = 1.0;\nuniform float tm_%s_scale = 0.25;\n", n, n);
		if (l->get_albedo_texture().is_valid()) {
			c += vformat("uniform sampler2D tm_%s_albedo_tex : source_color, filter_linear_mipmap_anisotropic, repeat_enable;\nuniform vec3 tm_%s_albedo_mean = vec3(1.0);\n", n, n);
		}
		if (l->get_normal_texture().is_valid()) {
			c += vformat("uniform sampler2D tm_%s_normal_tex : hint_normal, filter_linear_mipmap_anisotropic, repeat_enable;\n", n);
		}
		if (l->get_roughness_texture().is_valid()) {
			c += vformat("uniform sampler2D tm_%s_roughness_tex : hint_default_white, filter_linear_mipmap_anisotropic, repeat_enable;\nuniform float tm_%s_roughness_mean = 1.0;\n", n, n);
		}
		if (l->get_height_texture().is_valid()) {
			c += vformat("uniform sampler2D tm_%s_height_tex : hint_default_black, filter_linear_mipmap, repeat_enable;\n", n);
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
	// The weights sum to 1 whatever their size: on a face whose normal points nearly along a cube diagonal both are
	// tiny (0.6 gives 1e-5), and the old floor of the sum at 1e-4 left them summing to a tenth, the layer's textures
	// near black there.
	float s = w.x + w.y;
	r.w0 = s > 1e-12 ? w.x / s : 1.0;
	r.w1 = s > 1e-12 ? w.y / s : 0.0;
	return r;
}

// Value noise for the layers' large-scale breakup (no textures needed).
// An integer hash of the lattice point: the sin() hash loses its precision once its argument reaches the
// thousands (fine octaves a few hundred metres from the origin), and its value lattice turned into diagonal ripples.
float tm_vn_hash(vec3 p) {
	uvec3 q = uvec3(ivec3(p));
	uint h = (q.x * 1597334677u) ^ (q.y * 3812015801u) ^ (q.z * 2741598763u);
	h ^= h >> 16u;
	h *= 0x7feb352du;
	h ^= h >> 15u;
	h *= 0x846ca68bu;
	h ^= h >> 16u;
	return float(h >> 8u) * (1.0 / 16777216.0);
}
float tm_vn(vec3 p) {
	vec3 i = floor(p);
	vec3 f = fract(p);
	f = f * f * (3.0 - 2.0 * f);
	return mix(mix(mix(tm_vn_hash(i), tm_vn_hash(i + vec3(1, 0, 0)), f.x), mix(tm_vn_hash(i + vec3(0, 1, 0)), tm_vn_hash(i + vec3(1, 1, 0)), f.x), f.y),
			mix(mix(tm_vn_hash(i + vec3(0, 0, 1)), tm_vn_hash(i + vec3(1, 0, 1)), f.x), mix(tm_vn_hash(i + vec3(0, 1, 1)), tm_vn_hash(i + vec3(1, 1, 1)), f.x), f.y), f.z);
}
// Crisp drip stripes (ink streaks and varnish run down a face as sharp-edged stripes of their own width, not as a
// blur): one stripe or none in each cell of width C along the face's horizontal u, at its own place and width
// (wmin..wmax metres), meandering a little down the fall line; the edge filtered over the pixel's footprint fp (a
// box-filtered step, Apodaca and Gritz 2000), and the cells fade to their mean cover once a pixel spans one.
// t: 0 at the stripes' top, 1 where the longest run out; each runs out at its own length and narrows toward its end.
// 0..1 cover.
float tm_drip_1d(float u, float y, float t, float fp, float seed, float C, float wmin, float wmax) {
	u += 0.3 * C * (tm_vn(vec3(u / (3.0 * C), y * 0.12, seed)) - 0.5);
	float ci = floor(u / C);
	float h1 = tm_vn_hash(vec3(ci, seed, 3.0));
	float h2 = tm_vn_hash(vec3(ci, seed, 5.0));
	float h3 = tm_vn_hash(vec3(ci, seed, 7.0));
	float h4 = tm_vn_hash(vec3(ci, seed, 11.0));
	// Thin trickles run out soon and stain less; only the broad stripes run the full length (a thin dark line ruled
	// metres down a face read as a scratch).
	float tt = t / (0.25 + 0.75 * mix(h4, h3, 0.7));
	float present = step(0.4, h1) * step(tt, 1.0);
	// Water stains swell and pinch down their run and wander a little (a stripe of one width ruled straight down read
	// as a scratch or a stretched texture).
	float w = mix(wmin, wmax, h3 * h3) * (1.0 - 0.7 * smoothstep(0.3, 1.0, tt)) * (0.6 + 0.8 * tm_vn(vec3(ci * 3.1, y / 0.9, seed + 2.0)));
	float centre = (ci + 0.15 + 0.7 * h2) * C + 0.4 * C * (tm_vn(vec3(ci * 5.7, y / 1.8, seed + 4.0)) - 0.5);
	float aa = max(max(0.5 * fp, 0.15 * w), 0.002); // half the footprint, or a stain's feathered edge
	// The stripe's share of the footprint (a pulse box-filtered exactly, Apodaca and Gritz 2000): a stripe narrower than
	// the pixel fades with its width. Each fades down its run as its water spreads and dries. Edged by smoothstep over the footprint, a 5 cm trickle drew as a half-dark line a
	// whole pixel wide down metres of face, a scratch.
	float cov = present * (0.55 + 0.45 * h3) * (1.0 - 0.7 * smoothstep(0.0, 1.0, tt)) * max(min(centre + 0.5 * w, u + aa) - max(centre - 0.5 * w, u - aa), 0.0) / (2.0 * aa);
	float mean = 0.6 * 0.5 * (wmin + wmax) / C * (1.0 - smoothstep(0.35, 1.0, t));
	return mix(cov, mean, smoothstep(0.3 * C, C, fp));
}
// On a face: u along the face's horizontal, x on faces turned toward z, z on faces turned toward x (a coordinate
// along the face's own tangent swirled with every bump of the relief), blended between.
float tm_drip(vec3 p, vec3 n, float t, float fp, float seed, float C, float wmin, float wmax) {
	float k = smoothstep(0.35, 0.65, abs(n.x) / (abs(n.x) + abs(n.z) + 1e-5));
	float a = k < 1.0 ? tm_drip_1d(p.x, p.y, t, fp, seed, C, wmin, wmax) : 0.0;
	float b = k > 0.0 ? tm_drip_1d(p.z, p.y, t, fp, seed + 17.0, C, wmin, wmax) : 0.0;
	return mix(a, b, k);
}
// Crustose lichen colonies (Desbenoit, Galin and Akkouche 2004, "Simulating and modeling lichen growth": thalli grow
// out from a centre by aggregation, so the margin is fractal and lobed, never a regular polygon; neighbours meet and
// fuse; the old centre weathers paler or dies back while the young rim is darker and denser, edged by the black
// prothallus). Here a cellular field (Worley 1996) of thalli, one or none per S cell in a colony, each a disc of its
// own size whose margin is pushed in and out by two octaves of noise in the plane (lobes 10 to 30 cm and 3 to 8 cm,
// so no two share an outline), fused with their neighbours by a smooth minimum (Quilez's smin) instead of overlapping
// as decals; the edge filtered over the footprint. Colonies: the chance of a thallus in a cell follows a 2.5 m noise
// at the cell's centre (w3: the coordinate out of the plane), more where shift is higher (shaded faces).
// x: cover 0..1, y: whether the nearest thallus is the yellow-green kind (yellow_bias its share), z: the young rim
// band just inside the margin (0..1), w: tone (each thallus its own, the centre paler).
float tm_ros_dens(vec2 c, float w3, float shift) {
	// Colonies with edges of their own (a soft threshold scattered single thalli evenly over the whole face): dense
	// inside, where thalli crowd and fuse, bare rock between.
	return 0.97 * smoothstep(0.5 - shift, 0.58 - shift, 0.7 * tm_vn(vec3(c * 0.22, w3 * 0.22) + 13.0) + 0.3 * tm_vn(vec3(c * 0.8, w3 * 0.8) + 19.0));
}
vec4 tm_rosettes(vec2 q, float w3, float shift, float yellow_bias, float fp) {
	// One thallus a cell at most, cells of 15 cm: inside a colony thalli of 5 to 11 cm crowd until they touch and fuse into
	// a mosaic with gaps (Armstrong 2011, crustose colonies). Cells of 0.4 m with mostly 3 to 6 cm thalli scattered
	// lone round dots evenly 40 cm apart, which read as a regular pattern of white spots across a face.
	const float S = 0.15;
	vec2 g = q / S;
	vec2 i = floor(g);
	float aa = max(fp, 0.002);
	// The margin's lobes, in metres: shared by every thallus (where two meet they fuse along it).
	float close = 1.0 - smoothstep(0.03, 0.12, fp);
	float lobe = (tm_vn(vec3(q * 7.0, w3 * 7.0) + 5.0) - 0.5) * 0.16 + (tm_vn(vec3(q * 22.0, w3 * 22.0) + 9.0) - 0.5) * 0.06 * close;
	float e = 1e3; // signed distance to the fused margin, metres (negative inside)
	float best = 1e3;
	float yel = 0.0;
	float tone = 1.0;
	float rel = 1.0;
	for (int j = -1; j <= 1; j++) {
		for (int k = -1; k <= 1; k++) {
			vec2 c = i + vec2(float(j), float(k));
			float h0 = tm_vn_hash(vec3(c, 91.0));
			if (h0 < tm_ros_dens((c + 0.5) * S, w3, shift)) {
				float h1 = tm_vn_hash(vec3(c, 93.0));
				float h2 = tm_vn_hash(vec3(c, 95.0));
				float h3 = tm_vn_hash(vec3(c, 97.0));
				vec2 d = (g - (c + 0.15 + 0.7 * vec2(h1, h2))) * S;
				// Radii 5 to 11 cm, touching their neighbours in 15 cm cells (thalli of every age; diameter grows about linearly with age, a fraction of a
				// millimetre a year, so even centuries-old thalli of map lichen stay within 10 to 30 cm across; Innes 1985,
				// "Lichenometry"). Up to 0.6 m they read as flat painted discs across a face.
				float rad = 0.05 + 0.06 * h3;
				float ei = length(d) - rad + lobe * min(rad * 2.5, 1.0);
				// Fuse (smooth minimum over 4 cm).
				float hk = clamp(0.5 + 0.5 * (e - ei) / 0.04, 0.0, 1.0);
				e = mix(e, ei, hk) - 0.04 * hk * (1.0 - hk);
				if (ei < best) {
					best = ei;
					yel = step(1.0 - yellow_bias, fract(h0 * 17.3 + h3)) * step(0.35, h3);
					tone = 0.82 + 0.3 * fract(h1 * 31.7 + h2);
					rel = clamp(length(d) / max(rad, 1e-3), 0.0, 1.0);
				}
			}
		}
	}
	float cov = 1.0 - smoothstep(-aa, aa, e);
	// The young rim: a band 1 to 4 cm inside the margin (a pixel's width further off, then faded with distance).
	float band = (1.0 - smoothstep(0.0, max(0.03, 1.5 * aa), -e)) * cov * (1.0 - smoothstep(0.05, 0.2, fp));
	// The old centre paler and thinner.
	tone *= 1.0 + 0.18 * (1.0 - smoothstep(0.0, 0.7, rel));
	vec4 r = vec4(cov, yel, band, tone);
	// Past the cell's size the thalli average out to their mean cover.
	float far = smoothstep(0.15, 0.6, fp / S);
	return far > 0.0 ? mix(r, vec4(0.55 * tm_ros_dens(q, w3, shift), yellow_bias * 0.6, 0.0, 1.0), far) : r; // a colony's crowded cover
}

// Sand glitter (docs/ideas/terrain-material.md, "Sand glitter"): a few of the quartz grains (clean cleavage and
// crystal faces, mica flakes) are near-mirror facets that flash where they reflect the sun, the glitter Journey's
// sand drew from a random grain normal per pixel (Edwards, GDC 2013). Here physically counted: 0.5 mm grains, a quarter of
// them facets with GGX alpha 0.4 normals, so the flashes stay on their grains as the sun and camera hold still and
// thin out with distance (a pixel's facets are a smaller share of it) instead of turning to noise.
uint tm_sg_hash(ivec3 q, uint p_salt) {
	uint h = (uint(q.x) * 73856093u) ^ (uint(q.y) * 19349663u) ^ (uint(q.z) * 83492791u) ^ (p_salt * 2654435761u);
	h ^= h >> 16u;
	h *= 0x7feb352du;
	h ^= h >> 15u;
	h *= 0x846ca68bu;
	h ^= h >> 16u;
	return h;
}
vec3 tm_sg_tilt(vec3 p_n, float p_r1, float p_r2) {
	float tan2 = 0.16 * p_r1 / max(1.0 - p_r1, 1e-3);
	float ct = inversesqrt(1.0 + tan2);
	float st = sqrt(max(1.0 - ct * ct, 0.0));
	vec3 t = normalize(cross(p_n, abs(p_n.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
	vec3 b = cross(p_n, t);
	float phi = 6.2831853 * p_r2;
	return normalize(p_n * ct + (t * cos(phi) + b * sin(phi)) * st);
}
// How many of the facets in a pixel's footprint mirror the sun, as the share of a sharp coat lobe (alpha 0.011) on the
// half vector that draws them: count = facets in the footprint x the chance one's normal lies within the sun's disc of
// the half vector (GGX alpha 0.4 facets), Poisson drawn per footprint cell (the counting of Zirr and Kaplanyan 2016 /
// Deliot and Belcour 2023 with the sun as the one light). Each such facet adds its share of the pixel times the sun's
// radiance. The draw is fixed per cell, so raster and the path tracer (whose samples share the pixel's cell) show the
// same flashes, and the tracer's image converges to them instead of to per-sample speckle. Zero: no flash.
float tm_sg_coat(vec3 p_wp, vec3 p_n, vec3 p_l, vec3 p_v, float p_fp, float p_share) {
	vec3 h = normalize(p_l + p_v);
	float noh = dot(p_n, h);
	float voh = max(dot(p_v, h), 0.05);
	if (noh <= 0.0 || dot(p_n, p_l) <= 0.0) {
		return 0.0;
	}
	const float grain = 0.0005;
	float fp = max(p_fp, grain);
	// sr a facet catches the sun over: the sun's disc (0.53 degree) blurred by the facets' own curvature to some 1.6
	// degrees across. Wider (it was 3 degrees) dims every flash in proportion, below what a near-flat quartz face
	// mirroring the sun gives: they vanished at 640 wide.
	const float omega_acc = 6.0e-4;
	float dd = noh * noh * (0.16 - 1.0) + 1.0;
	float df = 0.16 / (3.14159265 * dd * dd);
	// Facets: a quarter of the grains' area (cleavage and crystal faces, several to a grain).
	float lam = 0.25 * (fp / grain) * (fp / grain) * df * noh * omega_acc / (4.0 * voh);
	float level = floor(log2(fp / grain));
	uint hh = tm_sg_hash(ivec3(floor(p_wp / (grain * exp2(level)))), uint(level) + 11u);
	float r0 = float(hh & 0xffffu) / 65536.0;
	float r1 = (float(hh >> 16u) + 0.5) / 65536.0;
	// Poisson draw without a loop (the path tracer's hit shader stays small): the CDF up to three facets below a
	// mean of 1.5, the normal approximation above it.
	float k = 0.0;
	if (lam < 1.5) {
		float e = exp(-lam);
		k = r0 > e ? 1.0 : 0.0;
		k += r0 > e * (1.0 + lam) ? 1.0 : 0.0;
		k += r0 > e * (1.0 + lam + 0.5 * lam * lam) ? 1.0 : 0.0;
	} else {
		k = max(round(lam + sqrt(lam) * sqrt(-2.0 * log(r1)) * cos(6.2831853 * r0)), 0.0);
	}
	if (k <= 0.0) {
		return 0.0;
	}
	// Pixel radiance over the coat's peak response: k facets of (grain / footprint)^2 of the pixel, each the sun's
	// irradiance spread over the facet's catch (1 / omega_acc) against D_GGX(alpha 0.011) at its peak times the coat's
	// visibility (0.25 / LoH^2).
	float loh = max(dot(p_l, h), 0.1);
	float area = (grain / fp) * (grain / fp);
	return clamp(k * area * p_share / omega_acc / (1.0 / (3.14159265 * 0.011 * 0.011) * 0.25 / (loh * loh)), 0.0, 1.0);
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
)";
	c += R"(
// The share of the soil bare at p, 0 with no climate (the shared rule, wc_climate_bare above).
float tm_climate_bare(vec3 p) {
	return tm_bare_on ? wc_climate_bare(p) : 0.0;
}
// The share of the soil's plant cover cured to straw by the dry season (wc_climate_cured above).
float tm_climate_cured(vec3 p) {
	return tm_bare_on ? wc_climate_cured(p) : 0.0;
}
// Burn scars. The stand map's burn channel b is 1 - years since the fire / 10.2 (0 never burnt). Burnt ground is char
// and ash over the soil: fresh char has an albedo of about 0.04 to 0.06 and white ash of a complete burn 0.3 or more,
// which wind and the first rains strip within months (Roy and Landmann 2005; Pereira et al. 2015, "Ash and soil
// properties after fire"). The surface darkening of a burn then fades over one to three years as the field layer
// regrows through it, in patches from surviving roots and seed, not as one even fade (Lentile et al. 2006; MODIS
// burn albedo recovery, Gatebe et al. 2014). Returns the share of the pixel still reading burnt.
float tm_burn_cover(vec3 p, float b) {
	float age = (1.0 - b) * 10.2;
	// Regrowth patches about 1.5 m and 5 m across: some ground closes over in the first season, some lies black for three.
	float n = 0.6 * tm_vn(p * 0.67 + 131.0) + 0.4 * tm_vn(p * 0.2 + 137.0);
	float left = 1.0 - smoothstep(0.4, 3.0, age + (n - 0.5) * 2.2);
	// The scar's edge breaks up over a few metres (the fire went out in tongues and unburnt islands).
	float edge = smoothstep(0.35, 0.6, b * 6.0 * (0.75 + 0.5 * tm_vn(p * 0.33 + 139.0)));
	return clamp(left * edge, 0.0, 1.0);
}
// The burnt ground's colour: char, with grey-white ash in drifts in the first months.
vec3 tm_burn_tint(vec3 p, float b) {
	float age = (1.0 - b) * 10.2;
	float ash = (1.0 - smoothstep(0.05, 0.4, age)) * smoothstep(0.45, 0.7, tm_vn(p * 0.9 + 149.0));
	vec3 char_col = vec3(0.052, 0.047, 0.042) * (0.8 + 0.4 * tm_vn(p * 3.1 + 151.0));
	return mix(char_col, vec3(0.32, 0.31, 0.29), ash);
}
// The cured cover's colour at p (its mean tm_cured_cover_color): a dry sward is not one straw tone but standing straw,
// grey weathered litter (dead grass greys within a season as it weathers) and soil between the tussocks, a quarter of
// the ground or so in a semi-arid sward. Tussocks about 0.35 m, blades and gaps under them at 0.1 m, fresher and greyer
// patches over 1.4 m; each scale fades to its mean once a pixel covers it.
vec3 tm_cured_cover(vec3 p, float fp, vec3 soil) {
	float t1 = tm_vn(p * 2.8 + 71.0);
	float t2 = tm_vn(p * 9.0 + 83.0);
	float t3 = tm_vn(p * 0.7 + 97.0);
	float t4 = tm_vn(p * 0.3 + 113.0);
	// Each octave fades to its mean once a pixel spans a quarter of its size (a 0.36 m tussock by 0.14 m pixels): faded
	// at a pixel's size, as before, it aliased into smooth bands under the temporal filter from 30 m on.
	float n1 = 1.0 - smoothstep(0.04, 0.14, fp);
	float n2 = 1.0 - smoothstep(0.012, 0.045, fp);
	float n3 = 1.0 - smoothstep(0.15, 0.5, fp);
	vec3 straw = tm_cured_cover_color.rgb * vec3(1.16, 1.14, 1.04);
	vec3 litter = tm_cured_cover_color.rgb * vec3(0.8, 0.88, 1.08);
	vec3 c = mix(straw, litter, clamp(0.4 + (t3 - 0.5) * 1.6 * n3 + (t2 - 0.5) * 0.7 * n2, 0.0, 1.0));
	// Clumps and swales a few metres across, fuller and sparser, what a dry sward shows from afar where the tussocks
	// have faded to their mean (one flat colour there read as paint).
	c *= 0.72 + 0.56 * t4;
	// Each tussock lit on top and shaded at its base (the clump's own self-shadow, the grain of a bunchgrass sward from
	// a few metres), the bare ground between them showing through: at 0.28 the cover read as flat mustard blobs.
	float tuft = 1.0 - smoothstep(0.36, 0.66, t1);
	c *= mix(0.96, 0.62 + 0.68 * tuft, n1) * mix(1.0, 0.75 + 0.5 * t2, n2);
	float gap = mix(0.32 + 0.4 * (0.5 - t4), smoothstep(0.48, 0.6, t1 + (t2 - 0.5) * 0.3 * n2), n1);
	return mix(c, soil, gap * 0.95);
}
)";
	// Terracettes as relief: the shared step (WorldContracts::terracette_step, which Landscape3D and the plants' roots
	// read too), gated to open soil by the generator's geology.
	c += WorldContracts::terracette_shader_code();
	c += R"(
// At p (world, the undisplaced ground's height in y) on ground of gradient grad: x the steps' height over the ground
// (m), y the change of the slope's steepness, box-filtered over width metres along the fall line. Its share is
// terrain_bank_relief.y (Landscape3D.terracette_relief).
vec2 tm_terracette(vec3 p, vec2 grad, float width) {
	float share = terrain_bank_relief.y;
	if (share > 0.0 && tm_geology && terrain_geology_rect.z > 0.0) {
		share *= wc_terracette_soil(terrain_geology(p));
	}
	return wc_terracette_step(p, grad, width, share);
}
)";
	// Godot passes a sampler to a function only with one filter/repeat/hint setting per function, so the hex sampler
	// is written once per kind of map: albedo (source_color), normal, roughness and height.
	{
		const String hex = R"(vec4 tm_hex(sampler2D t, vec2 uv, vec2 dx, vec2 dy, bool nmap) {
	// Footprints past half a tile all take the last mips anyway; unbounded ones (a face seen edge on: the cone
	// stretches by 1 / cos up to 20 times) sent anisotropic filtering out of range, black patches over the cliffs.
	dx *= min(1.0, 0.5 / max(length(dx), 1e-8));
	dy *= min(1.0, 0.5 / max(length(dy), 1e-8));
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
		const char *kinds[4] = { "a", "n", "r", "h" };
		for (int k = 0; k < 4; k++) {
			c += hex.replace("vec4 tm_hex(", String("vec4 tm_hex_") + kinds[k] + "(");
		}
	}
	// A scan's detail at this point (detail_only: albedo and roughness as ratios to their means), written into sd (albedo
	// ratio), sr (roughness ratio), sdn (normal perturbation) and sh (height), for a layer (tm_<name>) or an overlay
	// (tm_ov_<name>).
	auto scan_code = [](const String &p_n, const Ref<TerrainMaterialLayer> &p_l, bool p_macro) -> String {
		String o;
		const bool a = p_l.is_valid() && p_l->get_albedo_texture().is_valid();
		const bool nn = p_l.is_valid() && p_l->get_normal_texture().is_valid();
		const bool r = p_l.is_valid() && p_l->get_roughness_texture().is_valid();
		const bool h = p_l.is_valid() && p_l->get_height_texture().is_valid();
		o += vformat("\tTmProj pr = tm_projection(p, n, tm_%s_scale, cliff);\n", p_n);
		o += "\tsd = vec3(1.0);\n\tsr = 1.0;\n\tsdn = vec3(0.0);\n\tsh = 0.5;\n";
		if (!(a || nn || r || h)) {
			return o;
		}
		// The pixel's footprint in each projection's texture coordinates, for the hex-tiled samples' mips.
		o += vformat("\tvec2 dx0 = vec2(dpx[pr.axes0.x], dpx[pr.axes0.y]) * tm_%s_scale, dy0 = vec2(dpy[pr.axes0.x], dpy[pr.axes0.y]) * tm_%s_scale;\n", p_n, p_n);
		o += vformat("\tvec2 dx1 = vec2(dpx[pr.axes1.x], dpx[pr.axes1.y]) * tm_%s_scale, dy1 = vec2(dpy[pr.axes1.x], dpy[pr.axes1.y]) * tm_%s_scale;\n", p_n, p_n);
		if (a) {
			// detail_only: the scan gives the detail around the layer's colour (divided by its own mean), so any scan
			// keeps the colour the geology and the layer set.
			o += vformat("\tsd = (tm_hex_a(tm_%s_albedo_tex, pr.uv0, dx0, dy0, false).rgb * pr.w0 + (pr.w1 > 0.0 ? tm_hex_a(tm_%s_albedo_tex, pr.uv1, dx1, dy1, false).rgb * pr.w1 : vec3(0.0))) / tm_%s_albedo_mean;\n", p_n, p_n, p_n);
		}
		if (r) {
			// detail_only: around the layer's roughness too (scans' roughness maps follow no common scale: some wet mud
			// at 0.5, some dry rock at 0.95), so a dry layer is not glossy because its scan's map is.
			o += vformat("\tsr = (tm_hex_r(tm_%s_roughness_tex, pr.uv0, dx0, dy0, false).g * pr.w0 + (pr.w1 > 0.0 ? tm_hex_r(tm_%s_roughness_tex, pr.uv1, dx1, dy1, false).g * pr.w1 : 0.0)) / tm_%s_roughness_mean;\n", p_n, p_n, p_n);
		}
		if (nn) {
			o += vformat("\tvec3 n0 = tm_hex_n(tm_%s_normal_tex, pr.uv0, dx0, dy0, true).xyz * 2.0 - 1.0;\n", p_n);
			o += vformat("\tsdn = tm_perturb(n0.xy * tm_%s_normal, pr.axes0) * pr.w0;\n", p_n);
			o += vformat("\tif (pr.w1 > 0.0) {\n\t\tvec3 n1 = tm_hex_n(tm_%s_normal_tex, pr.uv1, dx1, dy1, true).xyz * 2.0 - 1.0;\n\t\tsdn += tm_perturb(n1.xy * tm_%s_normal, pr.axes1) * pr.w1;\n\t}\n", p_n, p_n);
		}
		if (p_macro && (a || nn)) {
			// The same scan again at six times its size (UE's landscape "macro variation", Far Cry 5's colour and detail
			// variation): a scan only holds the structure of its own tile, so from tens of metres a face or slope showed
			// its 2 m detail filtered to an even tone, with nothing at the metres scale (soft, flat walls). The macro
			// sample adds that scale's tone and relief; hex tiling keeps its repeats apart. Rock only: bare rock is near
			// self-similar over these scales (joints and fractures at every size), loose grains and stones are not (a
			// scree scan six times over is a field of flat boulders).
			o += vformat("\tTmProj pm = tm_projection(p, n, tm_%s_scale * 0.1667, cliff);\n", p_n);
			o += vformat("\tvec2 mx0 = vec2(dpx[pm.axes0.x], dpx[pm.axes0.y]) * tm_%s_scale * 0.1667, my0 = vec2(dpy[pm.axes0.x], dpy[pm.axes0.y]) * tm_%s_scale * 0.1667;\n", p_n, p_n);
			o += vformat("\tvec2 mx1 = vec2(dpx[pm.axes1.x], dpx[pm.axes1.y]) * tm_%s_scale * 0.1667, my1 = vec2(dpy[pm.axes1.x], dpy[pm.axes1.y]) * tm_%s_scale * 0.1667;\n", p_n, p_n);
			if (a) {
				o += vformat("\tvec3 macro_a = (tm_hex_a(tm_%s_albedo_tex, pm.uv0 + 0.37, mx0, my0, false).rgb * pm.w0 + (pm.w1 > 0.0 ? tm_hex_a(tm_%s_albedo_tex, pm.uv1 + 0.37, mx1, my1, false).rgb * pm.w1 : vec3(0.0))) / tm_%s_albedo_mean;\n", p_n, p_n, p_n);
				o += "\tsd *= mix(vec3(1.0), macro_a, 0.6);\n";
			}
			if (nn) {
				o += vformat("\tvec3 mn0 = tm_hex_n(tm_%s_normal_tex, pm.uv0 + 0.37, mx0, my0, true).xyz * 2.0 - 1.0;\n", p_n);
				o += vformat("\tsdn += tm_perturb(mn0.xy * tm_%s_normal * 0.7, pm.axes0) * pm.w0;\n", p_n);
				o += vformat("\tif (pm.w1 > 0.0) {\n\t\tvec3 mn1 = tm_hex_n(tm_%s_normal_tex, pm.uv1 + 0.37, mx1, my1, true).xyz * 2.0 - 1.0;\n\t\tsdn += tm_perturb(mn1.xy * tm_%s_normal * 0.7, pm.axes1) * pm.w1;\n\t}\n", p_n, p_n);
			}
		}
		if (h) {
			o += vformat("\tsh = tm_hex_h(tm_%s_height_tex, pr.uv0, dx0, dy0, false).r * pr.w0 + (pr.w1 > 0.0 ? tm_hex_h(tm_%s_height_tex, pr.uv1, dx1, dy1, false).r * pr.w1 : 0.0);\n", p_n, p_n);
		}
		return o;
	};
	// Overlay scans (docs/ideas/scan-textures.md): the same detail, for the layer functions to blend over their own.
	for (int i = 0; i < OVERLAY_MAX; i++) {
		if (overlays[i].is_null()) {
			continue;
		}
		const String n = String("ov_") + OVERLAY_NAMES[i];
		c += vformat("\nvoid tm_%s(vec3 p, vec3 n, bool cliff, vec3 dpx, vec3 dpy, out vec3 sd, out float sr, out vec3 sdn, out float sh) {\n", n);
		c += scan_code(n, overlays[i], i == OVERLAY_CLIFF || i == OVERLAY_ROCK_VARIANT);
		c += "}\n";
	}
	// Per layer: albedo, roughness, normal perturbation and height at this point. om: the masks of the overlays on
	// this layer (OVERLAY_MASK), 0 where only the layer's own scan shows.
	for (int i = 0; i < LAYER_MAX; i++) {
		const Ref<TerrainMaterialLayer> &l = layers[i];
		const String n = LAYER_NAMES[i];
		const bool has_a = l.is_valid() && l->get_albedo_texture().is_valid();
		c += vformat("\nvoid tm_%s(vec3 p, vec3 n, bool cliff, vec3 dpx, vec3 dpy, vec4 om, vec4 om2, out vec3 albedo, out float rough, out vec3 dn, out float h) {\n", n);
		c += "\tvec3 sd;\n\tfloat sr;\n\tvec3 sdn;\n\tfloat sh;\n\t{\n";
		c += scan_code(n, l, i == LAYER_ROCK).replace("\n\t", "\n\t\t").replace("\tTmProj", "\t\tTmProj");
		c += "\t}\n";
		for (int k = 0; k < OVERLAY_MAX; k++) {
			if (overlays[k].is_null() || !(OVERLAY_PARENTS[k] & (1 << i))) {
				continue;
			}
			// Each overlay over the layer's own scan by its mask, height blended at the edge (Mishkinis 2013): where the
			// mask is part way the scan standing higher in its height map wins, so cracked crust or straw ends in clumps
			// and plates, not a soft cross-fade.
			c += vformat("\tif (%s > 1e-3) {\n", OVERLAY_MASK[k]);
			c += vformat("\t\tvec3 od;\n\t\tfloat orr;\n\t\tvec3 odn;\n\t\tfloat oh;\n\t\ttm_ov_%s(p, n, cliff, dpx, dpy, od, orr, odn, oh);\n", OVERLAY_NAMES[k]);
			c += vformat("\t\tfloat m = clamp(%s, 0.0, 1.0);\n", OVERLAY_MASK[k]);
			c += "\t\tfloat a1 = m + oh * tm_blend_depth;\n\t\tfloat a0 = 1.0 - m + sh * tm_blend_depth;\n\t\tfloat tp = max(a0, a1) - tm_blend_depth;\n";
			c += "\t\tfloat b1 = max(a1 - tp, 0.0);\n\t\tfloat b0 = max(a0 - tp, 0.0);\n\t\tfloat mm = b1 / max(b0 + b1, 1e-5);\n";
			c += "\t\tsd = mix(sd, od, mm);\n\t\tsr = mix(sr, orr, mm);\n\t\tsdn = mix(sdn, odn, mm);\n\t\tsh = mix(sh, oh, mm);\n\t}\n";
		}
		c += vformat("\talbedo = tm_%s_color.rgb * sd;\n\trough = min(tm_%s_roughness * sr, 1.0);\n\tdn = sdn;\n\th = sh;\n", n, n);
		if (has_a || i == LAYER_SAND) {
			// A scan repeats every tile_size (hex tiling hides the seams, not the sameness over tens of metres): the same
			// 9 m and 2.2 m tone patches as the untextured layers below break it up. Sand takes them with or without a
			// scan: a beach berm or dune face of one even tone read as a flat slab.
			c += "\talbedo *= (0.84 + 0.32 * tm_vn(p / 9.0 + 31.0)) * (0.88 + 0.24 * tm_vn(p / 2.2 + 47.0));\n";
		}
		if (!has_a && i != LAYER_SAND) {
			// A layer without textures is one flat colour (a meadow, bank or hillside read as a painted plane): give it
			// procedural tone over 9 m and 2.2 m (patches of damper, stonier or more organic ground) and, while the
			// pixel is finer than them, 0.25 m and 0.06 m clods and grit as tone and a light bump (value noise, the
			// bump by forward differences, each octave faded out as the footprint nears it so nothing shimmers).
			c += R"(	{
		float fp = max(length(dpx), length(dpy));
		albedo *= (0.84 + 0.32 * tm_vn(p / 9.0 + 31.0)) * (0.88 + 0.24 * tm_vn(p / 2.2 + 47.0));
		// Five octaves from 0.6 m to 1 cm, each turned against the last (a lattice kept square drew diagonal
		// ripples), so the ground holds detail down to the pixel close up instead of reading as smeared clouds.
		const float L[5] = float[5](0.6, 0.22, 0.08, 0.03, 0.011);
		const float A[5] = float[5](0.45, 0.4, 0.35, 0.3, 0.25);
		const mat3 R = mat3(vec3(0.00, 0.80, 0.60), vec3(-0.80, 0.36, -0.48), vec3(-0.60, -0.48, 0.64));
		mat3 rk = mat3(1.0);
		for (int k = 0; k < 5; k++) {
			float f = 1.0 - smoothstep(0.25 * L[k], 0.6 * L[k], fp);
			if (f <= 0.0) {
				break;
			}
			vec3 q = rk * p / L[k] + vec3(13.0 * float(k));
			float v0 = tm_vn(q);
			vec3 g = vec3(tm_vn(q + vec3(0.35, 0.0, 0.0)), tm_vn(q + vec3(0.0, 0.35, 0.0)), tm_vn(q + vec3(0.0, 0.0, 0.35))) - v0;
			g = transpose(rk) * g;
			g -= n * dot(g, n);
			dn -= g * A[k] * f;
			albedo *= 1.0 + 0.14 * (v0 - 0.5) * f;
			rk = R * rk;
		}
	}
)";
		}
		if (i == LAYER_ROCK) {
			// Rock slopes and faces from afar: water and lichen stain them in streaks down the fall line and the beds
			// stand out as bands, so a pale face is not one even plane on which only the fine relief reads (crumpled
			// paper). The fine relief is eased there too: the layers' textures and bump are of ground seen from above.
			c += R"(	{
		float face = 1.0 - smoothstep(0.55, 0.85, n.y);
		if (face > 0.0) {
			// The streaks crisp, each its own width and length (stretched value noise read as smudges): stripes 0.1 to
			// 0.8 m wide in 2 m cells, starting at heights 10 m apart (a ledge or bed top) and running 3 to 10 m.
			float fp = max(length(dpx), length(dpy));
			float sy = p.y + 4.0 * tm_vn(vec3(p.x / 15.0, 0.0, p.z / 15.0) + 9.0);
			float band = floor(sy / 10.0);
			float below = 10.0 * (1.0 - fract(sy / 10.0)); // metres under the band's top
			float sk = tm_drip(p, n, below / (3.0 + 7.0 * tm_vn_hash(vec3(band, 61.0, 0.0))), fp, band * 1.7 + 71.0, 2.0, 0.1, 0.8);
			sk *= smoothstep(0.3, 0.55, tm_vn(vec3(p.x / 6.0, band * 3.1, p.z / 6.0) + 17.0));
			float bed = tm_vn(vec3(p.x / 40.0, p.y / 0.9, p.z / 40.0) + 5.0);
			// The scan (rock seen from above) drew soft wrinkles that read as draped cloth on a face: half its tone and
			// most of its bump give way there, the face's relief (Landscape3D cliff relief and its joint steps) shapes it.
			// With a face scan (the cliff overlay, om.x) the detail is a face's own (beds, joints, fractures, photographed
			// facing the wall), so none of it gives way: easing it left walls flat paint with streaks.
			float ease = face * (1.0 - TM_CLIFF_SCAN);
			albedo = mix(albedo, tm_rock_color.rgb, 0.5 * ease);
			albedo *= mix(1.0, (1.1 - 0.5 * sk) * (0.86 + 0.28 * bed) * (0.85 + 0.3 * tm_vn(p / 18.0 + 3.0)), face);
			dn *= 1.0 - 0.85 * ease;
		}
	}
)";
			c = c.replace("TM_CLIFF_SCAN", overlays[OVERLAY_CLIFF].is_valid() ? "clamp(om.x, 0.0, 1.0)" : "0.0"); // the cliff overlay's mask (OVERLAY_MASK)
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
varying vec4 tm_cl_p; // the undisplaced point (landscape space), w the cliff relief's scale
varying vec4 tm_cl_n; // the heightfield normal, w the bank relief's scale
varying float tm_cl_kj; // the joint steps' scale (CUSTOM2.x)
varying vec3 tm_proj_n; // the heightfield's central-difference normal (CUSTOM2.yzw), for the texture projection
varying vec4 tm_tc_p; // the terracettes: xyz the undisplaced ground (world), w 1 where they are drawn
varying vec2 tm_tc_g; // and the heightfield's gradient there
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
// The beds (Landscape3D::_cliff_offset): 12 m units of 1 to 5 beds of log-normal thickness, about one in four hard
// and proud, each bed's ledge swelling and breaking along the face. Returns the beds' offset; r_bed the bed's index.
float tm_cl_proud(vec3 p, int b) {
	float hard = tm_cl_hash(b, 11, 3);
	float base = hard > 0.75 ? 0.6 + 1.4 * (hard - 0.75) * 4.0 : (hard - 0.4) * 0.9;
	float run = tm_cl_noise(vec3(p.x * 0.06, float(b) * 1.37, p.z * 0.06));
	return base * smoothstep(0.4, 0.7, run) * (0.6 + 0.8 * run);
}
float tm_cl_joint(vec3 p, int b) {
	float u = p.x * 0.8 + p.z * 0.6 + tm_cl_hash(b, 5, 1) * 4.5;
	int jb = int(floor(u / 4.5));
	float fu = u - float(jb) * 4.5;
	float j0 = tm_cl_hash(b, jb, 9) - 0.5, j1 = tm_cl_hash(b, jb + 1, 9) - 0.5;
	float tu = clamp((fu - 3.0) / 1.5, 0.0, 1.0);
	return 1.2 * mix(j0, j1, tu * tu * (3.0 - 2.0 * tu));
}
// The beds' and their joint blocks' offset, blended into the next bed's over the lip.
float tm_cl_beds(vec3 p, float p_steep, float p_relief, out int r_bed) {
	const float U = 12.0;
	float yb = p.y + (tm_cl_noise(p * 0.05) - 0.5) * 4.0 + (tm_cl_noise(p * 0.013 + vec3(5.0, 0.0, 2.0)) - 0.5) * 10.0;
	int unit = int(floor(yb / U));
	float fu0 = yb - float(unit) * U;
	int nb = 1 + int(tm_cl_hash(unit, 21, 4) * 4.999);
	float wb[5];
	float wsum = 0.0;
	for (int i = 0; i < 5; i++) {
		wb[i] = i < nb ? exp(1.1 * (tm_cl_hash(unit, i, 17) + tm_cl_hash(unit, i, 19) - 1.0) * 1.7) : 0.0;
		wsum += wb[i];
	}
	int bi = nb - 1;
	float lo = 0.0;
	float T = U;
	float acc = 0.0;
	bool found = false;
	for (int i = 0; i < 5; i++) {
		if (i < nb && !found) {
			float th = U * wb[i] / wsum;
			if (fu0 < acc + th || i == nb - 1) {
				bi = i;
				T = th;
				lo = acc;
				found = true;
			}
			acc += th;
		}
	}
	int bed = unit * 8 + bi;
	int bed_up = bi + 1 < nb ? bed + 1 : (unit + 1) * 8;
	// The bed under this one and its thickness (the last bed of the unit below for the first).
	int bed_dn = bed - 1;
	float T_dn = 0.0;
	if (bi > 0) {
		T_dn = U * wb[bi - 1] / wsum;
	} else {
		int nb_dn = 1 + int(tm_cl_hash(unit - 1, 21, 4) * 4.999);
		float ws_dn = 0.0;
		float wl = 0.0;
		for (int i = 0; i < 5; i++) {
			if (i < nb_dn) {
				wl = exp(1.1 * (tm_cl_hash(unit - 1, i, 17) + tm_cl_hash(unit - 1, i, 19) - 1.0) * 1.7);
				ws_dn += wl;
			}
		}
		bed_dn = (unit - 1) * 8 + nb_dn - 1;
		T_dn = U * wl / ws_dn;
	}
	float fy = fu0 - lo;
	float a_dn = tm_cl_proud(p, bed_dn) + tm_cl_joint(p, bed_dn);
	float a0 = tm_cl_proud(p, bed) + tm_cl_joint(p, bed), a1 = tm_cl_proud(p, bed_up) + tm_cl_joint(p, bed_up);
	// The lip rule of Landscape3D::_cliff_offset: at least 1.2 times the step, and long enough on a steep face that
	// the step leans out no further than the face leans back; what does not fit in the lower bed's top carries on into
	// the bottom of the bed above.
	float steep = min(p_steep, 6.0 * max(p_relief, 0.0));
	float o = a0;
	{
		float d = a0 - a_dn;
		float req = max(min(1.5, max(0.6, T_dn * 0.5)), max(1.2 * abs(d), 2.6 * steep * abs(d)));
		float lo_dn = min(T_dn, req);
		float up_dn = req - lo_dn;
		if (up_dn > 0.0) {
			float carry = a_dn + d * (lo_dn / req);
			float lip_up = min(up_dn, T * 0.9);
			float tb = clamp(fy / lip_up, 0.0, 1.0);
			o = mix(carry, a0, tb * tb * (3.0 - 2.0 * tb));
		}
	}
	{
		float d = a1 - a0;
		float req = max(min(1.5, max(0.6, T * 0.5)), max(1.2 * abs(d), 2.6 * steep * abs(d)));
		float lip = min(T, req);
		float target = req > lip ? a0 + d * (lip / req) : a1;
		float ty = clamp((fy - (T - lip)) / lip, 0.0, 1.0);
		o = mix(o, target, ty * ty * (3.0 - 2.0 * ty));
	}
	r_bed = bed;
	return o;
}
// The joint steps (Landscape3D::_joint_offset): a dipping set stacks blocks down the face, each lip standing out and
// its face leaning back under it; a steep set struck at 37 degrees changes the step from block to block.
float tm_cl_joint_amp(int blk, int col) {
	float h = tm_cl_hash(blk, col, 71);
	float on = tm_cl_hash(blk, col, 73) > 0.3 ? 1.0 : 0.25;
	return (0.25 + 0.6 * h * h) * on;
}
float tm_cl_joints(vec3 p, float steep) {
	const float SD = 2.4, SJ = 2.6;
	float sd = (p.y + 0.1 * p.x - 0.07 * p.z) / SD + 0.35 * (tm_cl_noise(p * 0.15 + vec3(4.0, 4.0, 4.0)) - 0.5);
	float sj = (0.799 * p.x + 0.602 * p.z) / SJ + 0.3 * (tm_cl_noise(p * 0.2 + vec3(9.0, 2.0, 6.0)) - 0.5);
	int cd = int(floor(sd));
	float fr = sd - float(cd);
	float b = 0.3 + 0.4 * tm_cl_hash(cd, 61, 5);
	int blk = cd * 2;
	float f = fr, T = SD;
	if (tm_cl_hash(cd, 67, 5) > 0.45) {
		if (fr < b) {
			f = fr / b;
			T = b * SD;
		} else {
			f = (fr - b) / (1.0 - b);
			T = (1.0 - b) * SD;
			blk += 1;
		}
	}
	int cj = int(floor(sj));
	float gj = sj - float(cj);
	float dist = min(gj, 1.0 - gj) * SJ;
	float seam = 0.5 * (1.0 - smoothstep(0.0, 0.6, dist));
	int cn = gj < 0.5 ? cj - 1 : cj + 1;
	float st = max(steep, 0.05);
	float A = min(mix(tm_cl_joint_amp(blk, cj), tm_cl_joint_amp(blk, cn), seam), 0.75 * T / st);
	// The step at the joint above over a lip of 0.6 m or more, as Landscape3D::_joint_offset.
	int blk_up;
	float T_up;
	if (blk == cd * 2 && tm_cl_hash(cd, 67, 5) > 0.45) {
		blk_up = blk + 1;
		T_up = (1.0 - b) * SD;
	} else {
		float b_up = 0.3 + 0.4 * tm_cl_hash(cd + 1, 61, 5);
		blk_up = (cd + 1) * 2;
		T_up = tm_cl_hash(cd + 1, 67, 5) > 0.45 ? b_up * SD : SD;
	}
	float A_up = min(mix(tm_cl_joint_amp(blk_up, cj), tm_cl_joint_amp(blk_up, cn), seam), 0.75 * T_up / st);
	float r = min(max(0.6, 0.6 * (A + A_up)), 0.45 * T);
	float u = f * T;
	if (u <= T - r) {
		return A * (f - 0.5);
	}
	float t = (u - (T - r)) / r;
	return mix(A * ((T - r) / T - 0.5), -0.5 * A_up, t * t * (3.0 - 2.0 * t));
}
// Without the joint steps (the overhang scale leaves them out, as Landscape3D::_cliff_scale).
float tm_cl_offset_nj(vec3 p, float steep, float k) {
	int bed;
	float o = tm_cl_beds(p, steep, k, bed);
	// Drawn out down the fall line on the steepest faces, as Landscape3D::_cliff_offset.
	vec3 pf = vec3(p.x, p.y * 0.25, p.z);
	float fl = smoothstep(0.25, 0.7, min(steep, 6.0 * max(k, 0.0)));
	o += mix(0.5 * (tm_cl_noise(p * 0.7) - 0.5) + 0.2 * (tm_cl_noise(p * 2.1 + vec3(3.0, 1.0, 7.0)) - 0.5),
			0.5 * (tm_cl_noise(pf * 0.7) - 0.5) + 0.2 * (tm_cl_noise(pf * 2.1 + vec3(3.0, 1.0, 7.0)) - 0.5), fl);
	return o;
}
float tm_cl_offset(vec3 p, float steep, float k) {
	return tm_cl_offset_nj(p, steep, k) + tm_cl_joints(p, steep);
}
// Seep streaks below the beds' ledges (cyanobacterial ink streaks on limestone, Tintenstriche; desert varnish in dry
// climates; Viles 1995, Dorn and Oberlander 1981): water runs off a proud bed's lip and down the face of the softer
// bed under it, staining drip lines that fade down the fall line. The same beds as tm_cl_beds. 0..1.
float tm_cl_seep(vec3 p, vec3 n, float fp) {
	const float U = 12.0;
	float yb = p.y + (tm_cl_noise(p * 0.05) - 0.5) * 4.0 + (tm_cl_noise(p * 0.013 + vec3(5.0, 0.0, 2.0)) - 0.5) * 10.0;
	int unit = int(floor(yb / U));
	float fu0 = yb - float(unit) * U;
	int nb = 1 + int(tm_cl_hash(unit, 21, 4) * 4.999);
	float wb[5];
	float wsum = 0.0;
	for (int i = 0; i < 5; i++) {
		wb[i] = i < nb ? exp(1.1 * (tm_cl_hash(unit, i, 17) + tm_cl_hash(unit, i, 19) - 1.0) * 1.7) : 0.0;
		wsum += wb[i];
	}
	int bi = nb - 1;
	float lo = 0.0;
	float T = U;
	float acc = 0.0;
	bool found = false;
	for (int i = 0; i < 5; i++) {
		if (i < nb && !found) {
			float th = U * wb[i] / wsum;
			if (fu0 < acc + th || i == nb - 1) {
				bi = i;
				T = th;
				lo = acc;
				found = true;
			}
			acc += th;
		}
	}
	int bed = unit * 8 + bi;
	int bed_up = bi + 1 < nb ? bed + 1 : (unit + 1) * 8;
	float below = T - (fu0 - lo); // metres under the top of this bed
	// Every bed top sheds a little water down the face; a proud ledge gathers and sheds the most.
	float ledge = 0.35 + 0.65 * clamp(1.5 * (tm_cl_proud(p, bed_up) - tm_cl_proud(p, bed)), 0.0, 1.0);
	// Wetter beds (joints feeding them) seep further.
	float L = 3.0 + 9.0 * tm_cl_hash(bed, 41, 3);
	// Drip lines from the lip: crisp stripes 5 to 22 cm wide, each running out at its own length, in groups along
	// the bed (a drip line feeds where the lip sheds); darkest at the lip.
	float t = below / L;
	float group = tm_cl_noise(vec3(p.x * 0.2, float(bed) * 0.53, p.z * 0.2));
	float lines = tm_drip(p, n, t, fp, float(bed) * 0.37 + 5.0, 0.9, 0.05, 0.22);
	return ledge * smoothstep(0.0, 0.15, below) * (0.55 + 0.45 * exp(-2.0 * t)) * lines * smoothstep(0.45, 0.7, group);
}
// The banks' slumps (Landscape3D::_bank_offset): scarps and toe lobes in 6 m units along the contour, hummocks.
float tm_bk_offset(vec3 p) {
	const float U = 6.0;
	float yb = p.y + (tm_cl_noise(p * 0.07 + vec3(11.0, 3.0, 5.0)) - 0.5) * 6.0;
	int unit = int(floor(yb / U));
	float t = yb / U - float(unit);
	float strength = 0.4 + 0.6 * tm_cl_hash(unit, 31, 7);
	float run = tm_cl_noise(vec3(p.x * 0.09, float(unit) * 1.71, p.z * 0.09));
	float amp = strength * smoothstep(0.35, 0.65, run);
	float tl = (t - 0.25) / 0.17, ts = (t - 0.78) / 0.12;
	float ends = smoothstep(0.0, 0.1, t) * (1.0 - smoothstep(0.9, 1.0, t));
	float o = amp * (0.9 * exp(-tl * tl) - exp(-ts * ts)) * ends;
	o += 0.35 * (tm_cl_noise(p * 0.35 + vec3(2.0, 9.0, 4.0)) - 0.5) + 0.15 * (tm_cl_noise(p * 1.1 + vec3(7.0, 2.0, 8.0)) - 0.5);
	return o;
}
// The relief's scales k (cliff, bank) eased where the offset changes too fast with height for the face to stay a
// heightfield (Landscape3D::_cliff_scale and _bank_scale), for meshes that displace on their own (the ground
// deformation patch); the landscape's chunks hand theirs in CUSTOM0 and CUSTOM1.
// m: the slope's unit horizontal normal; sampled down the surface's fall line as the landscape's.
vec2 tm_cl_scale(vec3 p, vec2 k, float tn, vec3 m) {
	tn = min(tn, 6.0);
	vec3 st = m * (0.4 / max(tn, 0.1)) - vec3(0.0, 0.4, 0.0);
	float pc = 0.0, pb = 0.0;
	vec2 g = vec2(0.0);
	for (int i = -5; i <= 5; i++) {
		vec3 q = p + st * float(i);
		float oc = k.x > 0.0 ? tm_cl_offset_nj(q, terrain_cliff_relief.x * tn, terrain_cliff_relief.x) : 0.0;
		float ob = k.y > 0.0 ? tm_bk_offset(q) : 0.0;
		if (i > -5) {
			g = max(g, max(vec2(pc - oc, pb - ob), vec2(0.0)) / 0.4 * (1.0 - abs(float(i) - 0.5) / 5.5)); // only a fall folds
		}
		pc = oc;
		pb = ob;
	}
	return min(k, vec2(0.8, 0.6) / max(tn * g, vec2(1e-4)));
}
// Metres out of the face: k.x the cliff relief's scale, k.y the bank's.
// tn: the face's tan(slope) (the lip rule's, at the landscape's full relief scale as Landscape3D::_cliff_point; k
// arrives eased by Landscape3D::_cliff_scale in CUSTOM0).
// k.z the joint steps' (the cliff's scale without its overhang easing: the steps never overhang, Landscape3D::_joint_offset).
float tm_cl_disp(vec3 p, vec3 k, float tn) {
	return (k.x > 0.0 ? k.x * tm_cl_offset_nj(p, terrain_cliff_relief.x * tn, terrain_cliff_relief.x) : 0.0) + (k.y > 0.0 ? k.y * tm_bk_offset(p) : 0.0) + (k.z > 0.0 ? k.z * tm_cl_joints(p, terrain_cliff_relief.x * tn) : 0.0);
}
// The relief's normal at the undisplaced point p (landscape space), differenced over e (the pixel's footprint, so
// relief finer than a pixel fades instead of aliasing).
vec3 tm_cl_normal(vec3 p, vec3 nb, vec3 k, float e) {
	vec3 m = normalize(vec3(nb.x, 0.0, nb.z));
	vec3 t1 = cross(nb, vec3(0.0, 1.0, 0.0));
	t1 = dot(t1, t1) > 1e-8 ? normalize(t1) : vec3(1.0, 0.0, 0.0);
	vec3 t2 = cross(t1, nb);
	vec3 a = p + t1 * e, b = p - t1 * e, c = p + t2 * e, d = p - t2 * e;
	float tn = length(nb.xz) / max(nb.y, 1e-3);
	vec3 du = (a + m * tm_cl_disp(a, k, tn)) - (b + m * tm_cl_disp(b, k, tn));
	vec3 dv = (c + m * tm_cl_disp(c, k, tn)) - (d + m * tm_cl_disp(d, k, tn));
	vec3 nn = cross(dv, du); // faces along nb while the displaced frame keeps its orientation
	float l2 = dot(nn, nn);
	if (l2 <= 1e-12) {
		return nb;
	}
	nn *= inversesqrt(l2);
	// The relief's finest noise changes faster than the scale k (set per vertex over 0.4 m steps) allows for: where its
	// slope across the face folds the field within e, the differences turn the normal past the face's edge, a black
	// fleck under the sun with a white one beside it. A shading normal may not face away from its surface (Schussler
	// et al. 2017): past some 70 degrees from the heightfield's normal it hands over to that, wholly by the fold
	// itself (the frame turned over, nn against nb: the back of a fold, whose mirror image lit a dark streak).
	float cn = dot(nn, nb);
	return cn < 0.35 ? normalize(mix(nb, nn, smoothstep(0.0, 0.35, cn))) : nn;
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
float tm_cl_offset_coarse(vec3 p, float steep, float k) {
	int bed;
	float o = tm_cl_beds(p, steep, k, bed);
	o += mix(0.5 * (tm_cl_noise(p * 0.7) - 0.5), 0.5 * (tm_cl_noise(vec3(p.x, p.y * 0.25, p.z) * 0.7) - 0.5), smoothstep(0.25, 0.7, min(steep, 6.0 * max(k, 0.0))));
	return o;
}
float tm_cl_disp_coarse(vec3 p, vec3 k, float tn) {
	return (k.x > 0.0 ? k.x * tm_cl_offset_coarse(p, terrain_cliff_relief.x * tn, terrain_cliff_relief.x) : 0.0) + (k.y > 0.0 ? k.y * tm_bk_offset(p) : 0.0) + (k.z > 0.0 ? k.z * tm_cl_joints(p, terrain_cliff_relief.x * tn) : 0.0); // the joints' ledges cast shadows
}
// The sun's visibility over the relief at the undisplaced point p (landscape space): l toward the sun, e the pixel's
// footprint, j a 0-1 jitter for the steps.
float tm_cl_shadow(vec3 p, vec3 nb, vec3 k, vec3 l, float e, float j) {
	vec3 m = normalize(vec3(nb.x, 0.0, nb.z));
	float mn = max(dot(m, nb), 0.3);
	float rise = dot(l, nb) / mn; // metres out of the face per metre along the ray
	if (rise <= 0.02) {
		return 1.0; // the face itself turns from the sun: its lighting and the shadow map have it
	}
	float tn = length(nb.xz) / max(nb.y, 1e-3);
	vec3 s0 = p + m * tm_cl_disp_coarse(p, k, tn);
	// Long enough to clear the relief's whole range (about 4 k) at that rise.
	float t_max = clamp(4.0 * (k.x + k.y + k.z) / rise, 1.0, 16.0);
	float vis = 1.0;
	const int STEPS = 12;
	for (int i = 0; i < STEPS; i++) {
		float u = (float(i) + j) / float(STEPS);
		float t = max(t_max * u * u, 2.0 * e);
		vec3 q = s0 + l * t;
		float s = dot(q - p, nb) / mn;
		float d = s - tm_cl_disp_coarse(q - m * s, k, tn);
		// Soft over the sun's disc (0.53 degrees) or the pixel, whichever is wider.
		float w = max(t * 0.0093, e);
		vis = min(vis, smoothstep(-w, w, d + 0.5 * w));
	}
	return vis;
}
)";
	}
	if (ground_deform_enabled) {
		// Landscape3D::_normal_at for the ground patch, from the ground's heights at the landscape's grid nodes: the
		// ENO slope (Landscape3D::_eno_slope) at each node, blended bilinearly.
		c += R"(
float tm_cl_eno(float a2, float a1, float c, float b1, float b2, float s) {
	float back = c - a1, fwd = b1 - c;
	float central = 0.5 * (back + fwd);
	float kink = abs(fwd - back) / s;
	if (kink < 0.18) {
		return central;
	}
	float eno = abs(c - 2.0 * a1 + a2) <= abs(b2 - 2.0 * b1 + c) ? back : fwd;
	float t = clamp((kink - 0.18) / (0.35 - 0.18), 0.0, 1.0);
	return mix(central, eno, t * t * (3.0 - 2.0 * t));
}
float tm_cl_h(vec2 g) {
	vec2 xz = terrain_cliff_relief.zw + g * terrain_cliff_relief.y;
	return textureLod(ground_deform_height, (xz - ground_deform_height_rect.xy) / ground_deform_height_rect.zw, 0.0).r;
}
vec3 tm_cl_node_n(vec2 g) {
	float s = terrain_cliff_relief.y;
	float c = tm_cl_h(g);
	float sx = tm_cl_eno(tm_cl_h(g - vec2(2.0, 0.0)), tm_cl_h(g - vec2(1.0, 0.0)), c, tm_cl_h(g + vec2(1.0, 0.0)), tm_cl_h(g + vec2(2.0, 0.0)), s);
	float sz = tm_cl_eno(tm_cl_h(g - vec2(0.0, 2.0)), tm_cl_h(g - vec2(0.0, 1.0)), c, tm_cl_h(g + vec2(0.0, 1.0)), tm_cl_h(g + vec2(0.0, 2.0)), s);
	return normalize(vec3(-sx, s, -sz));
}
vec3 tm_cl_land_n(vec2 xz) {
	vec2 f = (xz - terrain_cliff_relief.zw) / terrain_cliff_relief.y;
	vec2 g = floor(f);
	vec2 u = f - g;
	return normalize(mix(mix(tm_cl_node_n(g), tm_cl_node_n(g + vec2(1.0, 0.0)), u.x), mix(tm_cl_node_n(g + vec2(0.0, 1.0)), tm_cl_node_n(g + vec2(1.0, 1.0)), u.x), u.y));
}
// Its central-difference normal (Landscape3D::_normal_central_at), for the texture projection on the patch.
vec3 tm_cl_node_cn(vec2 g) {
	float s = terrain_cliff_relief.y;
	return normalize(vec3(-0.5 * (tm_cl_h(g + vec2(1.0, 0.0)) - tm_cl_h(g - vec2(1.0, 0.0))), s, -0.5 * (tm_cl_h(g + vec2(0.0, 1.0)) - tm_cl_h(g - vec2(0.0, 1.0)))));
}
vec3 tm_cl_land_cn(vec2 xz) {
	vec2 f = (xz - terrain_cliff_relief.zw) / terrain_cliff_relief.y;
	vec2 g = floor(f);
	vec2 u = f - g;
	return normalize(mix(mix(tm_cl_node_cn(g), tm_cl_node_cn(g + vec2(1.0, 0.0)), u.x), mix(tm_cl_node_cn(g + vec2(0.0, 1.0)), tm_cl_node_cn(g + vec2(1.0, 1.0)), u.x), u.y));
}
// The face weights (Landscape3D::_relief_weights): cliffs from about 50 degrees, banks from about 40.
vec2 tm_cl_weights(vec3 n) {
	float wt = 1.0 - clamp((n.y - 0.47) / (0.64 - 0.47), 0.0, 1.0);
	float wb = 1.0 - clamp((n.y - 0.643) / (0.766 - 0.643), 0.0, 1.0);
	wt = wt * wt * (3.0 - 2.0 * wt);
	wb = wb * wb * (3.0 - 2.0 * wb) * (1.0 - wt);
	return vec2(wt, wb);
}
// Tent-filtered over 1.5 times the relief's reach either way, as Landscape3D::_relief_weights_at (a fade within one
// cell at a sharp cliff foot folded the surface).
vec2 tm_cl_weights_at(vec2 xz) {
	float r = 2.625 * max(terrain_cliff_relief.x, terrain_bank_relief.x);
	if (r < 0.25 * terrain_cliff_relief.y) {
		return tm_cl_weights(tm_cl_land_n(xz));
	}
	vec2 w = vec2(0.0);
	for (int j = 0; j < 3; j++) {
		for (int i = 0; i < 3; i++) {
			float t = (2.0 - abs(float(i) - 1.0)) * (2.0 - abs(float(j) - 1.0));
			w += tm_cl_weights(tm_cl_land_n(xz + vec2(float(i) - 1.0, float(j) - 1.0) * 2.0 * r)) * t;
		}
	}
	return w / 16.0;
}
)";
	}
	c += "\nvoid vertex() {\n\ttm_cl_p = CUSTOM0;\n\ttm_cl_n = CUSTOM1;\n\ttm_cl_kj = CUSTOM2.x;\n\ttm_proj_n = CUSTOM2.yzw;\n";
	// Terracettes on the landscape's mesh (CUSTOM0 its undisplaced point, CUSTOM1 the heightfield normal), filtered to
	// twice its vertex spacing (and wider with distance, where coarser levels draw): narrower, a grid of a metre or so
	// sampled each 3 m step at two or three vertices and drew them as a sawtooth along contours across its diagonals.
	c += R"(	tm_tc_p = vec4(0.0);
	tm_tc_g = vec2(0.0);
	if (terrain_bank_relief.y > 0.0 && dot(CUSTOM1.xyz, CUSTOM1.xyz) > 0.5) {
		vec3 tc_p = (MODEL_MATRIX * vec4(CUSTOM0.xyz, 1.0)).xyz;
		vec3 tc_n = normalize(mat3(MODEL_MATRIX) * CUSTOM1.xyz);
		vec2 tc_g = -tc_n.xz / max(tc_n.y, 0.05);
		tm_tc_p = vec4(tc_p, 1.0);
		tm_tc_g = tc_g;
		float tc_w = 2.0 * max(terrain_cliff_relief.y > 0.0 ? terrain_cliff_relief.y : 1.0, distance(tc_p, CAMERA_POSITION_WORLD) / 64.0);
		float tc_o = tm_terracette(tc_p, tc_g, tc_w).x;
		VERTEX += inverse(mat3(MODEL_MATRIX)) * vec3(0.0, tc_o, 0.0);
	}
)";
	if (ground_deform_enabled) {
		// Prints in the ground (GroundSurface3D): the terrain sinks under the printed patch, and the patch, drawn
		// with a copy of this material, takes the printed surface.
		c += "\tground_deform_vertex(MODEL_MATRIX, VERTEX, NORMAL, TANGENT, BINORMAL);\n";
		// Read, so the path tracer counts the parameter among what its capture depends on (GroundSurface3D bumps it
		// when the field's texture is rewritten in place, which alone recaptures nothing).
		c += "\tVERTEX.y += 0.0 * ground_deform_serial;\n";
		// The near patch takes the terracettes at its own grid, from the landscape's heightfield normal as the
		// landscape does (the patch's own follows prints).
		c += R"(	if (ground_deform_patch && terrain_bank_relief.y > 0.0 && terrain_cliff_relief.y > 0.0 && ground_deform_rect.z > 0.0) {
		vec3 tc_p = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
		vec3 tc_n = tm_cl_land_n(tc_p.xz);
		vec2 tc_g = -tc_n.xz / max(tc_n.y, 0.05);
		tm_tc_p = vec4(tc_p, 1.0);
		tm_tc_g = tc_g;
		float tc_o = tm_terracette(tc_p, tc_g, 2.0 * max(ground_deform_rect.w, 0.05)).x;
		VERTEX += inverse(mat3(MODEL_MATRIX)) * vec3(0.0, tc_o, 0.0);
	}
)";
		// The patch carries no relief data of its own, so it takes the landscape's here: the same face weight, scale
		// and offset as Landscape3D::_cliff_point, its vertices pushed out as the landscape's are, and CUSTOM0 /
		// CUSTOM1's data for the pixel normal and ledge shadows. Without it the patch drew smooth faces near the camera over relief cliffs.
		// Landscape space is world space here (as for the stand map).
		// And the landscape under the patch steps back into the face as well as down: its relief is linear between
		// vertices a metre or so apart while the patch follows it closely, so it stood out of the patch on the ledges
		// and shaded the patch from the sun (dark faces near the camera).
		c += R"(	if (!ground_deform_patch && CUSTOM0.w + CUSTOM1.w > 0.0 && ground_deform_rect.z > 0.0) {
		float cl_lw = ground_deform_lowered((MODEL_MATRIX * vec4(VERTEX, 1.0)).xz);
		vec2 cl_mh = CUSTOM1.xz;
		if (cl_lw > 0.0 && dot(cl_mh, cl_mh) > 1e-8) {
			VERTEX -= vec3(cl_mh.x, 0.0, cl_mh.y) * inversesqrt(dot(cl_mh, cl_mh)) * (1.5 * (CUSTOM0.w + CUSTOM1.w) * cl_lw);
		}
	}
)";
		c += R"(	if (ground_deform_patch && terrain_cliff_relief.x > 0.0 && ground_deform_rect.z > 0.0) {
		vec3 cl_w = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
		if (terrain_cliff_relief.y > 0.0) {
			tm_proj_n = inverse(mat3(MODEL_MATRIX)) * tm_cl_land_cn(cl_w.xz);
		}
		// The landscape's own heightfield normal (its grid's ENO normals): the patch's over one field texel is
		// steeper next to a cliff's top and foot, so it put relief on faces the landscape leaves plain.
		vec3 cl_nw = terrain_cliff_relief.y > 0.0 ? tm_cl_land_n(cl_w.xz) : vec3(0.0, 1.0, 0.0);
		float cl_ml = length(cl_nw.xz);
		vec2 cl_wts = terrain_cliff_relief.y > 0.0 ? tm_cl_weights_at(cl_w.xz) : vec2(0.0);
		float cl_wt = cl_wts.x;
		float cl_wb = cl_wts.y;
		if (cl_ml > 1e-4 && (cl_wt > 0.0 || (cl_wb > 0.0 && terrain_bank_relief.x > 0.0))) {
			vec3 cl_p = cl_w;
			vec3 cl_m = vec3(cl_nw.x, 0.0, cl_nw.z) / cl_ml;
			vec3 cl_k = vec3(tm_cl_scale(cl_p, vec2(terrain_cliff_relief.x * cl_wt, terrain_bank_relief.x * cl_wb), cl_ml / max(cl_nw.y, 1e-3), cl_m), terrain_cliff_relief.x * cl_wt);
			tm_cl_p = vec4(cl_p, cl_k.x);
			tm_cl_n = vec4(cl_nw, cl_k.y);
			tm_cl_kj = cl_k.z;
			mat4 cl_inv = inverse(MODEL_MATRIX);
			VERTEX = (cl_inv * vec4(cl_w + cl_m * tm_cl_disp(cl_p, cl_k, cl_ml / max(cl_nw.y, 1e-3)), 1.0)).xyz;
			// The vertex normal from the displaced surface around it, as the landscape's (the shadow map's normal
			// bias and the triangles' own lighting go by it).
			NORMAL = normalize(transpose(mat3(cl_inv)) * tm_cl_normal(cl_p, cl_nw, cl_k, 0.3));
		}
	}
)";
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
		// Crack relief: the block's edge rounds down into the crack, at most some 34 degrees (depth 0.45 of the lip's
		// width; smoothstep's steepest slope is 1.5 / width). A 6 cm drop over 6 cm tilted the lips to some 56 degrees
		// and, under the near-field slope clamp, up to 76: on a sunlit sea cliff every lip turned from the sun and the
		// open joints drew as black lines. The lip widens with the pixel (2 pixels) so it never aliases.
		float lip = max(0.06, 2.0 * pw);
		float crel = kc * open * (smoothstep(0.0, lip, d) - 1.0) * min(0.06, 0.45 * lip);
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
	// Faded with the stones' height: where the scree thins into soil (a few per cent of a layer's weight) the stones
	// are millimetres tall and show nothing, but their shadow test, whose penumbra and bias do not shrink with them,
	// still cut full shadows into the soil: dark ovals a metre or two across in the raster and path-traced frames.
	return mix(1.0, vis, smoothstep(0.1, 0.4, h / (TM_RD_STONE_H * tm_rock_detail_scale)));
}
)";
	}
	c += R"(
void fragment() {
	vec3 wp = (INV_VIEW_MATRIX * vec4(VERTEX, 1.0)).xyz;
	if (!IN_SHADOW_PASS && camera_cutout.w > 0.0 && camera_cutout_discard(wp, INV_VIEW_MATRIX, PROJECTION_MATRIX, FRAGCOORD.xy)) {
		discard;
	}
	vec3 wn = normalize((INV_VIEW_MATRIX * vec4(NORMAL, 0.0)).xyz);
	// The texture projection's normal. Landscape3D's vertex normals are one-sided at a kink (ENO), so a wall one cell
	// across gets flat normals at both its rows and was textured from above down its whole height (long vertical
	// smears). Its central-difference normal (CUSTOM2.yzw) sees the wall; where it is steeper than the pixel's
	// normal the projection turns to it. Meshes without it (zero) keep the pixel's normal.
	vec3 tm_pn = wn;
	if (dot(tm_proj_n, tm_proj_n) > 0.25) {
		vec3 pn = normalize(mat3(MODEL_MATRIX) * tm_proj_n);
		tm_pn = normalize(mix(wn, pn, smoothstep(0.0, 0.15, wn.y - pn.y)));
	}
	// Biplanar projection from 35 degrees (cos 0.82), not the cliff slope (50): below that both of Quilez's weights
	// fall on the top axis anyway (the second axis's weight is zero under 35 degrees, so gentle ground costs no second
	// fetch), and plain XZ on a 40 to 50 degree slope stretched the scans by up to 1.6 along the fall line, with a
	// seam where the projection switched at 50.
	bool cliff = tm_pn.y < 0.82;
	// The pixel's footprint on the ground for the layers' texture gradients, as a ray cone's ellipse (Akenine-Moller
	// et al. 2019), as the rock detail takes it: the path tracer has no screen derivatives.
	vec3 tm_dpx, tm_dpy;
	{
		vec3 v = wp - CAMERA_POSITION_WORLD;
		float cone = length(v) * 2.0 / (abs(PROJECTION_MATRIX[1][1]) * VIEWPORT_SIZE.y);
		v = normalize(v);
		vec3 t1 = cross(v, tm_pn);
		t1 = dot(t1, t1) > 1e-8 ? normalize(t1) : normalize(cross(tm_pn, vec3(1.0, 0.0, 0.0) + vec3(0.0, 0.0, 1.0) * step(0.9, abs(tm_pn.x))));
		tm_dpx = t1 * cone;
		tm_dpy = cross(tm_pn, t1) * cone / max(abs(dot(v, tm_pn)), 0.05);
	}
	// Layer weights. Geology from the generator: bare rock, scree on it, the rest soil, alluvium where much water
	// drains; without it, rock past the rock slope and soil elsewhere. Sand on the beach band above the sea.
	float w[5];
	// The slope bands widen with the pixel's footprint: a far hill's mesh normal wanders by more than the band
	// within a pixel or two, and a narrow band drew thin rock-coloured lines along its slope contours there (the
	// far "swirls"). Close up they are as before.
	float tm_fp0 = length(wp - CAMERA_POSITION_WORLD) * 2.0 / (abs(PROJECTION_MATRIX[1][1]) * VIEWPORT_SIZE.y);
	float tm_sb = 1.0 + 2.0 * smoothstep(0.05, 0.6, tm_fp0);
	// The slope thresholds and the geology's rock edge are broken by noise (5 m and 1.3 m, the fine one faded out
	// as the pixel grows): taken as they are, rock and soil met along smooth contours, a clean cut-out edge between
	// grey and olive. Outcrops break up into tongues and islands.
	float tm_edge_n = (tm_vn(wp / 5.0 + 11.0) - 0.5) * 0.24 + (tm_vn(wp / 1.3 + 23.0) - 0.5) * 0.12 * (1.0 - smoothstep(0.03, 0.15, tm_fp0));
	// The layers go by the heightfield's own slope where cliff relief displaced the mesh (CUSTOM1): by the displaced
	// faces' normal a ledge's flat tread took the soil, olive ladder strips under every lip on a grey cliff.
	vec3 tm_cn = dot(tm_cl_n.xyz, tm_cl_n.xyz) > 0.5 ? normalize(mat3(MODEL_MATRIX) * tm_cl_n.xyz) : wn;
	float tm_ny = tm_cn.y + tm_edge_n;
	float steep = 1.0 - smoothstep(tm_rock_cos - 0.08 * tm_sb, tm_rock_cos + 0.04 * tm_sb, tm_ny);
	float beach_sand = -1.0;
	float tm_till = 0.0;
	float tm_gully = 0.0;
	float tm_drain = -1e9; // the generator's log10 drainage area here (m2), for the channel beds' scan
	if (tm_geology && terrain_geology_rect.z > 0.0) {
		vec4 g = terrain_geology(wp);
		tm_drain = g.w;
		beach_sand = clamp(-g.x, 0.0, 1.0);
		// The generator knows where soil holds (Heimsath soil on slopes up to its repose); below its cells only
		// faces past the cliff slope are forced bare.
		float cliff_rock = 1.0 - smoothstep(tm_cliff_cos - 0.08 * tm_sb, tm_cliff_cos + 0.04 * tm_sb, tm_ny);
		float rock = max(smoothstep(0.2, 0.8, clamp(g.y, 0.0, 1.0) + tm_edge_n * 2.5), cliff_rock);
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
		// Shelves inside a cliff (Landscape3D's cliff relief within 6 m, terrain_cliff_map): a ledge past 30 degrees
		// between walls holds rockfall debris, not topsoil (the geology's 8 m cells miss a wall's shelves, which took
		// flat olive islands with hard rims mid-face).
		if (rest > 0.0 && terrain_cliff_rect.z > 0.0) {
			vec2 rr = (wp.xz - terrain_cliff_rect.xy) / terrain_cliff_rect.zw;
			float ro = 6.0 / terrain_cliff_rect.z;
			float wall = max(max(textureLod(terrain_cliff_map, rr + vec2(ro, 0.0), 0.0).r, textureLod(terrain_cliff_map, rr - vec2(ro, 0.0), 0.0).r), max(textureLod(terrain_cliff_map, rr + vec2(0.0, ro), 0.0).r, textureLod(terrain_cliff_map, rr - vec2(0.0, ro), 0.0).r));
			wall = max(wall, textureLod(terrain_cliff_map, rr, 0.0).r);
			wall = smoothstep(0.05, 0.4, wall) * (1.0 - smoothstep(0.82, 0.9, tm_ny));
			wall *= all(greaterThan(rr, vec2(0.02))) && all(lessThan(rr, vec2(0.98))) ? 1.0 : 0.0;
			scree += rest * wall;
			rest *= 1.0 - wall;
		}
		// Fines settle only where the gradient falls off: floodplains and fan surfaces of a few degrees (fluvial fans
		// mostly under 5 degrees, debris-flow fans to about 15; Blair and McPherson 1994). Taken to 32 degrees, slopes
		// of 25 drew flat pale slabs of bare alluvium with hard edges on dry hillsides. The edge noise breaks the rim.
		float alluvium = smoothstep(tm_alluvium_area, tm_alluvium_area + 1.0, g.w) * smoothstep(0.92, 0.97, tm_cn.y + tm_edge_n * 0.15);
		// Rills and gullies (the droplet erosion's channels): the flow strips a channel's floor of its fines and soil
		// to a gravel lag (Poesen et al. 2003), the scree layer's stones and grit; below the slope break its load
		// settles as a fan of fresh fines (the alluvium layer). Rock floors stay rock.
		vec2 tm_gl = terrain_gully(wp, tm_cn.y);
		tm_gully = tm_gl.x * (1.0 - rock) * (0.8 + 0.4 * tm_vn(wp * 0.7 + 31.0));
		tm_gully = clamp(tm_gully, 0.0, 1.0);
		scree += rest * tm_gully;
		rest *= 1.0 - tm_gully;
		alluvium = max(alluvium, smoothstep(0.15, 0.8, tm_gl.y) * smoothstep(0.95, 0.985, tm_cn.y + tm_edge_n * 0.15));
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
	float sand_steep = beach_sand > 0.0 ? mix(steep, 1.0 - smoothstep(0.682, 0.719, tm_cn.y), beach_sand) : steep;
	sand *= 1.0 - sand_steep;
	for (int i = 0; i < 4; i++) {
		w[i] *= 1.0 - sand;
	}
	w[4] = sand;
	// Faces the surface itself stands steep on (cliff relief walls, joint and ledge risers, cut banks: the displaced
	// faces' normal past some 55 degrees) are rock, whatever the heightfield under them says: no loose layer holds
	// past its angle of repose (sand 34, scree 35 to 40, soil and alluvium less), and a riser drawn as the tread's
	// alluvium or beach sand read as flat tan walls with no rock in them. The treads keep the heightfield's layer.
	{
		float tm_riser = 1.0 - smoothstep(0.5, 0.62, tm_pn.y);
		if (tm_riser > 0.0) {
			float moved = 0.0;
			for (int i = 1; i < 5; i++) {
				moved += w[i] * tm_riser;
				w[i] *= 1.0 - tm_riser;
			}
			w[0] += moved;
		}
	}
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
	// The overlay scans' masks (docs/ideas/scan-textures.md): rock faces past some 45 degrees take the face scan, and a
	// second rock scan in 25 m patches.
	vec4 tm_om_rock = vec4(1.0 - smoothstep(0.55, 0.8, tm_pn.y), smoothstep(0.38, 0.62, 0.7 * tm_vn(wp / 25.0 + 71.0) + 0.3 * tm_vn(wp / 8.0 + 5.0)), 0.0, 0.0);
	vec4 tm_om2_rock = vec4(0.0);
	vec4 tm_om_scree = vec4(0.0);
	vec4 tm_om2_scree = vec4(0.0);
	if (w[0] > 1e-3) {
		vec3 a;
		float r;
		vec3 d;
		float h;
		tm_rock(wp, tm_pn, cliff, tm_dpx, tm_dpy, tm_om_rock, tm_om2_rock, a, r, d, h);
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
		tm_scree(wp, tm_pn, cliff, tm_dpx, tm_dpy, tm_om_scree, tm_om2_scree, a, r, d, h);
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
	float tm_bare_k = w[2] + w[3] > 1e-3 ? wc_bare_patch(wp, tm_climate_bare(wp), tm_fp0) : 0.0;
	// The bare share of this pixel's ground, for the turf tint and far plant colour below: none over bare soil.
	float tm_bare_g = tm_bare_k * clamp(w[2] + w[3], 0.0, 1.0);
	// The cover between the bare patches: the soil layer's colour is a green vegetated topsoil, which in a summer-dry
	// climate is straw through the dry season (the grasses cure), not the olive of a humid sward.
	float tm_cured = w[2] + w[3] > 1e-3 ? tm_climate_cured(wp) : 0.0;
	// Straw over the cover between the bare patches, the bare patches' own crust, and the turned soil of ruts and prints.
	float tm_om_mud = TM_OM_MUD;
	// Soil faces too steep for a sward to close over them (42 to 53 degrees by the surface's own normal: banks, slump
	// scarps, cut edges) show the soil's cut face.
	float tm_om_bank = 1.0 - smoothstep(0.6, 0.74, tm_pn.y);
	// Trails walked bare (the stand map's wear, before the trail's own tint and tread below).
	float tm_om_trail = 0.0;
	// Burn scars (the stand map's burn channel): char and ash over the soil, fading as the cover regrows.
	float tm_om_burn = 0.0;
	float tm_burn_b = 0.0;
	if (terrain_stand_rect.z > 0.0) {
		vec2 suv = (wp.xz - terrain_stand_rect.xy) / terrain_stand_rect.zw;
		if (all(greaterThan(suv, vec2(0.0))) && all(lessThan(suv, vec2(1.0)))) {
			vec4 tm_st = textureLod(terrain_stand_map, suv, 0.0);
			tm_om_trail = smoothstep(0.28, 0.48, tm_st.r + (tm_vn(wp * vec3(2.2, 0.0, 2.2) + 31.0) - 0.5) * 0.4);
			if (tm_st.b > 0.01) {
				tm_burn_b = tm_st.b;
				tm_om_burn = tm_burn_cover(wp, tm_st.b);
			}
		}
	}
	vec4 tm_om_soil = vec4(tm_cured * (1.0 - tm_bare_k), tm_bare_k, tm_om_mud, tm_om_bank);
	vec4 tm_om2_soil = vec4(0.0, tm_om_trail, tm_om_burn, 0.0);
	vec4 tm_om_alluvium = vec4(0.8 * tm_cured * (1.0 - tm_bare_k), tm_bare_k, tm_om_mud, tm_om_bank);
	// Channel beds: where the drainage area is a hundred times the alluvium's threshold the flow keeps the fines moving
	// and leaves a bed of rounded gravel (bedload channels; Church 2006), on gentle ground.
	vec4 tm_om2_alluvium = vec4(tm_drain > -1e8 ? smoothstep(tm_alluvium_area + 1.5, tm_alluvium_area + 2.5, tm_drain) * smoothstep(0.9, 0.96, tm_cn.y) : 0.0, tm_om_trail, tm_om_burn, 0.0);
	// The beach: sand within a few metres of the sea (the swash, berm and seabed); dunes inland keep the sand's own.
	vec4 tm_om_sand = vec4(1.0 - smoothstep(tm_beach_height, tm_beach_height + 4.0, wp.y - tm_sea_level), 0.0, 0.0, 0.0);
	vec4 tm_om2_sand = vec4(0.0);
	if (w[2] > 1e-3) {
		vec3 a;
		float r;
		vec3 d;
		float h;
		tm_soil(wp, tm_pn, cliff, tm_dpx, tm_dpy, tm_om_soil, tm_om2_soil, a, r, d, h);
		a *= mix(mix(mix(tm_soil_color.rgb, tm_cured_cover(wp, tm_fp0, tm_bare_soil_color.rgb), tm_cured), tm_bare_soil_color.rgb, tm_bare_k), tm_burn_tint(wp, tm_burn_b), tm_om_burn) / max(tm_soil_color.rgb, vec3(1e-3));
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
		tm_alluvium(wp, tm_pn, cliff, tm_dpx, tm_dpy, tm_om_alluvium, tm_om2_alluvium, a, r, d, h);
		a *= mix(mix(mix(tm_alluvium_color.rgb, tm_cured_cover(wp, tm_fp0, tm_bare_alluvium_color.rgb), 0.8 * tm_cured), tm_bare_alluvium_color.rgb, tm_bare_k), tm_burn_tint(wp, tm_burn_b), tm_om_burn) / max(tm_alluvium_color.rgb, vec3(1e-3));
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
		tm_sand(wp, tm_pn, cliff, tm_dpx, tm_dpy, tm_om_sand, tm_om2_sand, a, r, d, h);
		d += tm_sand_grain(wp, tm_fp0, a);
		// The dune field's colour at this scale: redder old sand, coarse dark interdune flats, pale slip faces.
		float tm_sdepth = terrain_sand_interdune(wp);
		if (tm_dune_tone_on) {
			a *= tm_dune_tone(wp.xz, wn.y, tm_sdepth);
		}
		// Granule ripples on the interdune lag, across the dune-building wind.
		if (tm_granule_on) {
			vec3 gt;
			vec2 gs = tm_granule_ripples(wp.xz, tm_dune_lag(wp.xz, wn.y, tm_sdepth), terrain_sand_wind.xy, tm_fp0, gt);
			a *= gt;
			d -= vec3(gs.x, 0.0, gs.y);
		}
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
	float tm_sand_b = 0.0; // the sand's share of the blend, for its glitter
	float tm_stone_b = 0.0; // rock and scree's share, for the salt bloom
	float tm_rock_b = 0.0; // the rock layer's share, for the snow's facet rule
	float tm_sand_dry = 1.0;
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
		tm_sand_b = i == 4 ? b : tm_sand_b;
		tm_stone_b += i < 2 ? b : 0.0;
		tm_rock_b = i == 0 ? b : tm_rock_b;
	}
	total = max(total, 1e-5);
	tm_sand_b /= total;
	tm_stone_b /= total;
	tm_rock_b /= total;
	albedo /= total;
	rough /= total;
	dn /= total;
	porosity /= total;
	vec3 sn = normalize(wn + dn);
	vec3 tm_macro_n = wn; // the surface's own normal, without the layers' fine bump
	if (tm_cl_p.w > 0.0 || tm_cl_n.w > 0.0 || tm_cl_kj > 0.0) {
		// Cliff and bank relief: the face's normal per pixel (see tm_cl_normal), with the layers' own detail on top.
		float cl_fp = length(wp - CAMERA_POSITION_WORLD) * 2.0 / (abs(PROJECTION_MATRIX[1][1]) * VIEWPORT_SIZE.y);
		vec3 cl_n = tm_cl_normal(tm_cl_p.xyz, normalize(tm_cl_n.xyz), vec3(tm_cl_p.w, tm_cl_n.w, tm_cl_kj), max(0.15, cl_fp));
		cl_n = normalize(mat3(MODEL_MATRIX) * cl_n);
		sn = normalize(cl_n + dn);
		tm_macro_n = cl_n;
)";
	if (cliff_self_shadow) {
		// Where the relief is still wider than a pixel and the pixel faces the sun. sun_direction() is zero under
		// the path tracer, which traces the displaced faces' own shadows.
		c += R"(		vec3 cl_l = sun_direction();
		float cl_k = tm_cl_p.w + tm_cl_n.w + tm_cl_kj;
		if (dot(cl_l, cl_l) > 0.5 && dot(sn, cl_l) > 0.0 && cl_k > 0.5 * cl_fp) {
			vec3 cl_ll = normalize(inverse(mat3(MODEL_MATRIX)) * cl_l);
			float cl_j = fract(52.9829189 * fract(dot(FRAGCOORD.xy, vec2(0.06711056, 0.00583715))));
			float cl_v = tm_cl_shadow(tm_cl_p.xyz, normalize(tm_cl_n.xyz), vec3(tm_cl_p.w, tm_cl_n.w, tm_cl_kj), cl_ll, cl_fp, cl_j);
			SUN_SHADOW = mix(1.0, cl_v, smoothstep(0.5 * cl_fp, cl_fp, cl_k));
		}
)";
	}
	c += R"(	}
)";
	// Terracettes per pixel: the steps' slope, filtered over the pixel's footprint (the geometry carries them as far as
	// its vertices resolve; this draws the tread edges finer and fades them out where a pixel covers a step).
	c += R"(	if (tm_tc_p.w > 0.5) {
		vec2 tc = tm_terracette(tm_tc_p.xyz, tm_tc_g, tm_fp0);
		if (tc.y != 0.0) {
			vec3 tc_n0 = normalize(vec3(-tm_tc_g.x, 1.0, -tm_tc_g.y));
			vec3 tc_n1 = normalize(vec3(-tm_tc_g.x * (1.0 + tc.y), 1.0, -tm_tc_g.y * (1.0 + tc.y)));
			sn = normalize(sn + tc_n1 - tc_n0);
			tm_macro_n = normalize(tm_macro_n + tc_n1 - tc_n0);
		}
	}
)";
	// Rock faces weathered by aspect (docs/ideas/terrain-material.md, "Rock faces weather by aspect"): lichen and moss
	// cover follows how little sun a face gets over the year, the heat load index's idea (McCune and Keon 2002: a
	// face's insolation from latitude, slope and aspect, folded toward the warmer afternoon side, south-west in the
	// north); shaded, damp aspects carry far more (Armstrong 1974; Pentecost 1980). Grey-green crusts, now and then
	// the yellow-green map lichen, dark moss on the most shaded, less steep ledges; seep streaks under the beds'
	// ledges (tm_cl_seep). North is -z.
	c += R"(	if (tm_face_weathering > 0.0) {
		float fw_rock = (w[0] + 0.5 * w[1]) / total;
		float fw_face = fw_rock * (1.0 - smoothstep(0.55, 0.85, tm_macro_n.y)) * tm_face_weathering;
		if (fw_face > 0.0) {
			float fw_lat = radians(clamp(tm_latitude, -89.0, 89.0));
			float fw_el = radians(90.0) - abs(fw_lat); // the equinox noon sun
			float fw_az = radians(tm_latitude >= 0.0 ? 200.0 : 340.0); // toward the equator, a little past noon
			vec3 fw_s = vec3(cos(fw_el) * sin(fw_az), sin(fw_el), -cos(fw_el) * cos(fw_az));
			float fw_ins = clamp(dot(tm_macro_n, fw_s), 0.0, 1.0);
			float shade = 1.0 - smoothstep(0.1, 0.7, fw_ins);
			// Fade the patch noise to its mean as the pixel grows past it.
			float fw_k = 1.0 - smoothstep(0.15, 0.6, tm_fp0);
			// Seep streaks under the ledges first (the lichen grows over and between them): dark ink stripes down the
			// fall line, darker and longer where the face is shaded (it stays wet longer).
			vec3 fw_p = tm_cl_p.w > 0.0 ? tm_cl_p.xyz : wp;
			float seep = tm_cl_seep(fw_p, wn, tm_fp0) * (1.0 - smoothstep(0.55, 0.8, tm_macro_n.y)) * fw_rock * tm_face_weathering;
			seep = mix(0.5 * seep, seep, fw_k);
			albedo = mix(albedo, albedo * vec3(0.62, 0.61, 0.59), seep * (0.45 + 0.3 * shade));
			// Crustose lichen in colonies of rosettes: pale grey-white crusts (Lecanora, Aspicilia) on most faces, the
			// yellow-green map lichen (Rhizocarpon) on the sunnier ones, each thallus edged by its black prothallus
			// line. Colonies 3 m across hold rosettes 0.3 to 0.8 m; cover grows with shade.
			// Rosettes with crisp, filtered edges (value noise thresholded drew blocky blots); in the face's two
			// dominant planes like its textures. The crusts tint the rock toward their own colour rather than paint
			// over it, so its tone and cracks still show.
			TmProj fw_pr = tm_projection(wp, tm_pn, 1.0, true);
			float fw_shift = 0.2 * shade;
			float fw_yb = 0.3 * (1.0 - shade) * smoothstep(0.45, 0.65, tm_vn(wp * 0.21 + 77.0));
			vec4 ros = tm_rosettes(fw_pr.uv0, wp[3 - fw_pr.axes0.x - fw_pr.axes0.y], fw_shift, fw_yb, tm_fp0) * fw_pr.w0;
			if (fw_pr.w1 > 0.0) {
				ros += tm_rosettes(fw_pr.uv1 + 7.31, wp[3 - fw_pr.axes1.x - fw_pr.axes1.y], fw_shift, fw_yb, tm_fp0) * fw_pr.w1;
			}
			float lc = ros.x * fw_face;
			float yellow = ros.y;
			float fw_l = dot(albedo, vec3(0.3, 0.59, 0.11));
			// The map lichen a dull yellow-green (Rhizocarpon geographicum, about 5Y 6/6), not a bright yellow fleck.
			vec3 lcol = mix(vec3(0.86, 0.88, 0.8) * max(fw_l * 1.15, 0.2), vec3(0.36, 0.37, 0.24), yellow);
			lcol *= ros.w * (0.9 + 0.2 * mix(0.5, tm_vn(wp * 9.0 + 5.0), fw_k));
			// Close up a crust is a mosaic of areoles split by fine dark cracks (black between the map lichen's yellow ones).
			float fw_ar = (1.0 - smoothstep(0.0, 0.07, abs(tm_vn(wp * 28.0 + 3.0) - 0.5))) * (1.0 - smoothstep(0.006, 0.02, tm_fp0));
			lcol *= 1.0 - fw_ar * mix(0.25, 0.7, yellow);
			// The young rim darker and denser (the map lichen's black prothallus shows through at its edge).
			lcol *= 1.0 - ros.z * mix(0.22, 0.5, yellow);
			// A thin crust over the rock, not paint: its cover thins and thickens (the rock's tone and cracks show).
			float fw_a = 0.4 * (0.7 + 0.6 * mix(0.5, tm_vn(wp * 3.3 + 21.0), fw_k));
			albedo = mix(albedo, lcol, lc * fw_a * (0.75 + 0.35 * ros.z));
			rough = mix(rough, max(rough, 0.9), lc);
			// Moss on the shaded faces' treads and lips, where water and fines lodge: the relief's surface flatter than
			// the face around it (the heightfield's normal).
			// Faded with the pixel's footprint: a tread is a strip under a metre wide, and drawn on its own at a distance
			// it became a row of dark dashes under every ledge.
			float fw_tread = smoothstep(0.12, 0.35, tm_macro_n.y - tm_cn.y) * fw_k + smoothstep(0.55, 0.8, tm_macro_n.y);
			float moss = shade * shade * clamp(fw_tread, 0.0, 1.0) * smoothstep(0.35, 0.65, mix(0.55, 0.7 * tm_vn(wp * 0.5 + 51.0) + 0.3 * tm_vn(wp * 2.3 + 61.0), fw_k)) * fw_rock * tm_face_weathering;
			albedo = mix(albedo, vec3(0.12, 0.14, 0.07) * (0.8 + 0.4 * mix(0.5, tm_vn(wp * 6.0 + 2.0), fw_k)), moss * 0.7);
			rough = mix(rough, 0.95, moss);
		}
	}
)";
	if (detail) {
		c += R"(	// The generator's erosion detail below the vertex spacing, faded out where a pixel covers it.
	// The footprint's long axis (stretched by 1 / cos of the view to the ground, up to 10 times): a face seen edge-on
	// under a side sun drew the detail's octaves as fine diagonal hatching while the cone's width alone kept them.
	float footprint = length(wp - CAMERA_POSITION_WORLD) * 2.0 / (abs(PROJECTION_MATRIX[1][1]) * VIEWPORT_SIZE.y);
	footprint /= max(abs(dot(normalize(wp - CAMERA_POSITION_WORLD), wn)), 0.1);
	// The detail's gullies run by the surface's own slope, not by the layers' fine bump (procedural clods or a scan's
	// normal map): fed those, the gullies' direction turned every few centimetres and drew blobs with hard rims, a
	// melted-wax look over any bare ground. Its tilt then goes on top of the bumped normal.
	sn = normalize(sn + terrain_detail_normal(wp, tm_macro_n, tm_detail_octaves, footprint) - tm_macro_n);
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
	c += R"(	float tm_gc_spec = 1.0;
	float tm_gc_any = 0.0;
	if (tm_gc_on) {
		// Past each ground detail class's reach: its grains' aggregate (see tm_gc_cover).
		vec3 gc_v = wp - CAMERA_POSITION_WORLD;
		float gc_d = length(gc_v);
		vec3 gc_hid = smoothstep(tm_gc_reach - 0.25 * tm_gc_band, tm_gc_reach + 0.75 * tm_gc_band, vec3(gc_d));
		if (max(gc_hid.x, max(gc_hid.y, gc_hid.z)) > 0.0) {
			int gc_type;
			// Patchy below the map's metre (two octaves): a deposit is never an even smear.
			float gc_patch = 0.7 + 0.45 * tm_gc_noise(wp.xz * 0.9) + 0.2 * (tm_gc_noise(wp.xz * 3.1 + 7.0) - 0.5);
			vec3 gc_cov = tm_gc_cover(wp.xz, gc_type) * gc_hid * gc_patch;
			// Where a deposit thins out its edge is ragged, not a smooth fade: the thin cover is cut by the same patch
			// noise (with a finer octave), so the aggregate's edge breaks up as the stones' own does.
			float gc_rag = tm_gc_noise(wp.xz * 0.6 + 3.0) - 0.5 + 0.6 * (tm_gc_noise(wp.xz * 1.7 + 11.0) - 0.5);
			gc_cov *= smoothstep(vec3(0.0), vec3(1.0), gc_cov * 8.0 + 1.5 * gc_rag);
			// And the mean is never flat: clusters a few grains wide, lighter and darker (a third octave).
			float gc_clump = 0.85 + 0.3 * tm_gc_noise(wp.xz * 5.3 + 1.0);
			if (max(gc_cov.x, max(gc_cov.y, gc_cov.z)) > 0.002) {
				vec3 gc_view = gc_v / max(gc_d, 1e-4);
				float gc_cv = max(abs(dot(gc_view, wn)), 0.05);
				float gc_fp = gc_d * 2.0 / (abs(PROJECTION_MATRIX[1][1]) * VIEWPORT_SIZE.y) / sqrt(max(gc_cv, 0.1)); // across and along the view, their mean
				// Seen from the side the grains stand up off the bed and hide it (a grain about 0.6 of its width tall), and
				// a low sun puts the bed between them in their shade: the aggregate's view and light dependence (Meng et
				// al. 2015), as Poisson cover of grains whose shadow and silhouette lengthen with the tangent of the angle.
				float gc_tv = sqrt(1.0 - gc_cv * gc_cv) / gc_cv;
				vec3 gc_l = relief_sun_direction();
				float gc_ts = 0.0;
				float gc_back = 0.0;
				if (dot(gc_l, gc_l) > 0.5) {
					float cs = max(dot(gc_l, wn), 0.05);
					gc_ts = sqrt(max(1.0 - cs * cs, 0.0)) / cs;
					// Against a low sun the grains show the camera their shaded sides.
					vec3 lf = gc_l - wn * dot(gc_l, wn);
					vec3 vf = gc_view - wn * dot(gc_view, wn);
					gc_back = max(dot(normalize(lf + 1e-5), normalize(vf + 1e-5)), 0.0) * min(gc_ts / 2.0, 1.0);
				}
				// The grains carry the ground's dust (as the field's own grains do: half their colour from far off).
				vec3 gc_stone = mix(tm_gc_colors[clamp(gc_type, 0, 7)], albedo, 0.5);
				vec3 gc_n = vec3(0.0);
				float gc_any = 0.0;
				float gc_shade = 1.0;
				// Fine first, so the larger grains lie over them.
				for (int k = 2; k >= 0; k--) {
					float cov = gc_cov[k];
					if (cov <= 0.002) {
						continue;
					}
					float s = tm_gc_size[k];
					// Single grains while a pixel spans under about a third of one, their mean once it spans more than one (as the
					// drawn stones, which still speckle at a pixel each).
					float res = smoothstep(0.35, 1.2, gc_fp / s);
					float tone;
					vec3 gn;
					float hit = res < 1.0 ? tm_gc_grain(wp.xz, s, cov, tone, gn) : 0.0;
					// The mean: the share of the view the grains fill, at their mean tone (lit tops, shaded sides).
					// (0.8 and 0.75 matched to frames with every grain drawn, from above and at eye height.)
					float seen = 1.0 - exp(-0.8 * cov * (1.0 + 0.75 * gc_tv));
					float shaded = 1.0 - exp(-cov * 0.2 * gc_ts);
					float f = mix(hit, seen, res);
					float t = mix(tone, (0.95 - 0.15 * min(gc_ts / 3.0, 1.0)) * (1.0 - 0.3 * gc_back), res);
					albedo = mix(albedo, gc_stone * t * mix(1.0, gc_clump, res), f * mix(1.0, gc_clump, res));
					// The bed still seen lies partly in their shadow.
					gc_shade *= 1.0 - res * (1.0 - seen) * shaded;
					gc_n = mix(gc_n, gn - vec3(0.0, 1.0, 0.0), hit * (1.0 - res));
					gc_any = max(gc_any, f);
				}
				sn = normalize(sn + gc_n);
				SUN_SHADOW *= gc_shade;
				// The grains hide the bed's sheen, and being rough and in each other's shade send back little of their own
				// (their roughness goes on after the wet sand's gloss, below).
				tm_gc_spec = 1.0 - gc_any;
				tm_gc_any = gc_any;
			}
		}
	}
)";
	if (far_blend_enabled) {
		c += "\tvec3 tm_bare = albedo;\n\tfloat tm_far_t = 0.0;\n\tfloat tm_far_cover = 0.0;\n";
	}
	if (ground_deform_enabled) {
		// The soil under the cover, for where the ground is broken open (below).
		c += "\tvec3 tm_soil = albedo;\n\tfloat tm_exposed = 0.0;\n";
	}
	// The bare ground under the cover, for the stand's trails and rooting (below).
	c += "\tvec3 tm_stand_bare = albedo;\n";
	if (grass_tint_enabled) {
		c += "\talbedo = mix(grass_ground(wp, albedo), albedo, tm_bare_g);\n"; // no turf on the bare patches (the blades skip them too)
		// Moraine till keeps its stones above the turf: the scree layer's tallest blocks stand out of the sward
		// (a grassed moraine is soil and grass with boulders through it, not a meadow).
		// Without a scree height map there are no blocks to pick out: the till keeps the stones' share as a tint.
		const bool stone_h = layers[LAYER_SCREE].is_valid() && layers[LAYER_SCREE]->get_height_texture().is_valid();
		c += vformat("\tif (tm_till > 0.0) {\n\t\talbedo = mix(albedo, alb[1], tm_till * %s);\n\t}\n", stone_h ? "smoothstep(0.5, 0.7, tm_stone_h)" : "0.45");
		// Plants thinned away by distance leave their colour on the ground (VegetationField3D).
		c += "\talbedo = mix(vegetation_far(wp, albedo, CAMERA_POSITION_WORLD), albedo, tm_bare_g);\n";
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
		// The cover map is 4 m texels blurred over 12 m: one flat colour per field. A canopy seen from afar is
		// clumps and gaps: tone varies over 12 m and 2 m (and 0.4 m while a pixel is finer than that), and where the
		// clumps thin the bare ground's own texture shows through, so far meadows and banks are not painted planes.
		c += R"(
			float tm_cv1 = tm_vn(wp * 0.083 + 5.0);
			float tm_cv2 = tm_vn(wp * 0.5 + 9.0);
			float tm_cv3 = mix(tm_vn(wp * 2.5 + 13.0), 0.5, smoothstep(0.1, 0.4, tm_hb_fp));
			cover_lin *= (0.8 + 0.4 * tm_cv1) * (0.86 + 0.28 * tm_cv2) * (0.9 + 0.2 * tm_cv3);
			cover_lin *= mix(vec3(1.06, 1.0, 0.86), vec3(0.95, 1.0, 1.08), tm_cv1);
			float tm_ca = cover.a * smoothstep(0.2, 0.5, 0.4 * tm_cv1 + 0.6 * tm_cv2);
			albedo = mix(albedo, mix(tm_bare, cover_lin, mix(cover.a, tm_ca, 0.6)), far_t);
			tm_far_t = far_t;
			tm_far_cover = cover.a;
		}
	}
)";
	}
	if (ground_deform_enabled) {
		// Ruts, digs, puddles and tracked mud open the meadow's cover: the soil layer's own colour shows there, its
		// edge broken by noise, before the turned ground's tint (darker, wet) goes over it. Before the stones' damp
		// soil below: the stones still sit in a print, and their damp ring stays on it (else a print in gravel shows
		// pale dry soil between them).
		c += R"(	tm_exposed = ground_deform_exposed(wp);
	if (tm_exposed > 0.0) {
		tm_exposed = clamp(tm_exposed * (0.75 + 0.5 * tm_vn(wp * 6.0 + 11.0)), 0.0, 1.0);
		albedo = mix(albedo, tm_soil, tm_exposed);
	}
)";
	}
	// Rill and gully floors carry no sward (the flow scours it) and stay damp after rain: wet soil and gravel darken by
	// a fifth to a third (Lobell and Asner 2002), and read a little smoother.
	c += R"(	if (tm_gully > 0.0) {
		albedo = mix(albedo, tm_stand_bare * 0.78, tm_gully);
		rough = mix(rough, rough * 0.85, tm_gully);
	}
)";
	// The stand's worn trails and boar rooting (WorldStand through Landscape3D's stand map; docs/ideas/terrain-material.md).
	// A trail walked bare: the cover gone, the soil packed (Cole 1995; Liddle 1997: trampled soil is denser, its litter
	// and fine relief worn off), darker and browner, smoother, its clods pressed flat; the tread's edge broken by noise. Rooting: boar turn the topsoil over in patches (Bueno 2013; Welander 2000),
	// dark moist humus in loose clods, the cover gone, fading as it dries and the field layer closes over it (1 to 2
	// years). Neither on bare rock faces or sand. Ground detail lays the stones and vegetation the litter on both.
	c += R"(	if (terrain_stand_rect.z > 0.0) {
		vec2 suv = (wp.xz - terrain_stand_rect.xy) / terrain_stand_rect.zw;
		if (all(greaterThan(suv, vec2(0.0))) && all(lessThan(suv, vec2(1.0)))) {
			vec2 st = textureLod(terrain_stand_map, suv, 0.0).rg;
			vec2 sedge = min(suv, 1.0 - suv);
			float sfade = smoothstep(0.0, 0.06, min(sedge.x, sedge.y));
			float sground = (1.0 - w[0]) * (1.0 - w[4]) * smoothstep(0.55, 0.8, tm_macro_n.y) * sfade;
			if (st.r > 0.0 && sground > 0.0) {
				float tn = tm_vn(wp * vec3(2.2, 0.0, 2.2) + 31.0);
				// A tread's edge is fairly sharp (the sward stands right up to it), ragged at two scales.
				float tr = smoothstep(0.28, 0.48, st.r + (tn - 0.5) * 0.4 + (tm_vn(wp * 9.0 + 5.0) - 0.5) * 0.15) * sground;
				// Trodden soil as the sward's own paths draw it (grass_path_soil): compacted, darker and browner than
				// loose ground (Liddle 1997), damp in the hollows feet press into it.
				vec3 tpack = mix(tm_stand_bare, vec3(0.085, 0.062, 0.042), 0.65) * (0.85 + 0.3 * tm_vn(wp * 2.3 + 9.1));
				float tdamp = smoothstep(0.55, 0.75, tm_vn(wp * 0.7 + 4.4) * 0.7 + tm_vn(wp * 3.1) * 0.3) * smoothstep(0.5, 0.9, st.r);
				tpack *= mix(1.0, 0.62, tdamp);
				albedo = mix(albedo, tpack, tr);
				rough = mix(rough, rough * mix(0.9, 0.7, tdamp), tr);
				sn = normalize(mix(sn, tm_macro_n, 0.55 * tr));
				// The tread is worn down into the ground (a few to some 10 cm on a busy game trail; Cole 1995): its
				// banks as a normal from the wear map's slope, steeper where the sward's turf breaks off at the edge.
				vec2 sdu = vec2(1.0 / float(textureSize(terrain_stand_map, 0).x), 0.0);
				float stx = textureLod(terrain_stand_map, suv + sdu.xy, 0.0).r - textureLod(terrain_stand_map, suv - sdu.xy, 0.0).r;
				float stz = textureLod(terrain_stand_map, suv + sdu.yx, 0.0).r - textureLod(terrain_stand_map, suv - sdu.yx, 0.0).r;
				vec2 sgrad = vec2(stx, stz) / (2.0 * sdu.x * terrain_stand_rect.z); // wear per metre
				float tdepth = 0.08 * sground;
				vec2 tbank = tdepth * sgrad * (0.6 + 0.8 * tn);
				// The turf's broken edge: a narrow band of crumbled soil and torn roots under the lip, in its shade.
				float tedge = smoothstep(0.1, 0.45, tr) * (1.0 - smoothstep(0.55, 0.9, tr));
				albedo *= mix(1.0, 0.72 + 0.2 * tm_vn(wp * 14.0 + 2.0), tedge);
				// Hoof and foot pocking in the tread: shallow prints a hand across, softened where the soil is dry.
				vec3 hq = wp * 5.5;
				float h0 = tm_vn(hq + 17.0);
				float hx = tm_vn(hq + vec3(0.11, 0.0, 0.0) + 17.0);
				float hz = tm_vn(hq + vec3(0.0, 0.0, 0.11) + 17.0);
				vec2 hgrad = vec2(hx - h0, hz - h0) / 0.02 * 0.012; // about 1 cm deep
				sn = normalize(sn + vec3(tbank.x, 0.0, tbank.y) - vec3(hgrad.x, 0.0, hgrad.y) * smoothstep(0.4, 0.9, tr));
				albedo *= mix(1.0, 0.85 + 0.3 * h0, 0.6 * tr); // the prints hold damp and fines darker
				// Grit the feet have bared: coarse sand and small chips, lighter and darker, while a pixel resolves them.
				float tgrit = tm_vn(wp * 38.0 + 29.0);
				float tgr = (1.0 - smoothstep(0.01, 0.04, tm_hb_fp)) * tr;
				albedo *= mix(1.0, tgrit > 0.72 ? 1.35 : (tgrit < 0.25 ? 0.7 : 1.0), 0.7 * tgr);
			}
			if (st.g > 0.0 && sground > 0.0) {
				// Rooted in patches (a sounder works the ground in strips and pits), the patch's edge ragged.
				float rp = tm_vn(wp * 0.6 + 23.0) * 0.6 + tm_vn(wp * 2.1 + 3.0) * 0.4;
				float rt = smoothstep(0.32, 0.5, rp * (0.6 + 0.5 * st.g)) * smoothstep(0.0, 0.25, st.g) * sground;
				if (rt > 0.0) {
					// Clods: lumps a hand to a fist across, their faces catching the light.
					float e = 0.02;
					vec3 cq = wp * 7.0;
					float c0 = tm_vn(cq) + 0.5 * tm_vn(cq * 2.3 + 5.0);
					float cx = tm_vn(cq + vec3(e * 7.0, 0.0, 0.0)) + 0.5 * tm_vn((cq + vec3(e * 7.0, 0.0, 0.0)) * 2.3 + 5.0);
					float cz = tm_vn(cq + vec3(0.0, 0.0, e * 7.0)) + 0.5 * tm_vn((cq + vec3(0.0, 0.0, e * 7.0)) * 2.3 + 5.0);
					vec2 cg = vec2(cx - c0, cz - c0) / e * 0.05;
					float fresh = st.g;
					// Turned humus: dark and moist while fresh, greying as it dries; the bare soil's own tint kept.
					vec3 humus = mix(vec3(0.062, 0.046, 0.033), vec3(0.11, 0.085, 0.06), smoothstep(0.6, 1.4, c0));
					humus = mix(humus, tm_stand_bare * 0.75, (1.0 - fresh) * 0.6);
					// Overturned sods among the humus: the turf's own colour on the clods' tops, browning as they die.
					float sod = smoothstep(1.05, 1.25, c0) * smoothstep(0.4, 0.6, tm_vn(wp * 1.7 + 61.0));
					humus = mix(humus, albedo * vec3(0.8, 0.7, 0.55), sod * 0.7);
					albedo = mix(albedo, humus, rt * mix(0.55, 1.0, fresh));
					rough = mix(rough, mix(0.97, 0.8, fresh * smoothstep(1.0, 0.5, c0)), rt);
					sn = normalize(sn - vec3(cg.x, 0.0, cg.y) * rt * mix(0.4, 1.0, fresh));
				}
			}
		}
	}
)";
	// Frost-sorted ground (RockField3D frost_sorting, the same gate and stone share its stones are placed by): the fine
	// centres of sorted circles and stripes are frost boils, bare silt and clay heaved up each winter so no sward
	// holds, wet through the thaw (Washburn 1956; Walker et al. 2004, frost-boil ecosystems; Kessler and Werner 2003):
	// a pale grey-brown silt, darker and smoother wet. The stone borders take a gravel lag between the stones.
	c += R"(	if (terrain_frost_rect.z > 0.0) {
		vec2 fuv = (wp.xz - terrain_frost_rect.xy) / terrain_frost_rect.zw;
		if (all(greaterThanEqual(fuv, vec2(0.0))) && all(lessThanEqual(fuv, vec2(1.0)))) {
			vec3 fr = textureLod(terrain_frost_map, fuv, 0.0).rgb;
			vec2 fe = min(fuv, 1.0 - fuv);
			float fg = fr.r * smoothstep(0.0, 0.15, min(fe.x, fe.y)) * smoothstep(0.65, 0.85, wn.y); // the map's last 10 m fade (the far cover maps carry the borders' stones on)
			if (fg > 0.0) {
				// Frost heave domes the centres (b, RockField3D's dome share) about 30 cm over the stone borders (the
				// top of the 5 to 30 cm measured, so it reads at eye level): the normal bends by its slope from the
				// texels three either side.
				float fdx = 3.0 / float(textureSize(terrain_frost_map, 0).x);
				vec2 fdh = vec2(textureLod(terrain_frost_map, fuv + vec2(fdx, 0.0), 0.0).b - textureLod(terrain_frost_map, fuv - vec2(fdx, 0.0), 0.0).b, textureLod(terrain_frost_map, fuv + vec2(0.0, fdx), 0.0).b - textureLod(terrain_frost_map, fuv - vec2(0.0, fdx), 0.0).b);
				fdh *= 0.3 / (2.0 * fdx * terrain_frost_rect.z);
				sn = normalize(sn - vec3(fdh.x, 0.0, fdh.y) * smoothstep(0.0, 0.15, min(fe.x, fe.y)));
				float fn = tm_vn(wp * 2.3 + 61.0);
				float fm = tm_vn(wp * 0.9 + 23.0);
				// Wide transitions: the fines thin out into the border over the share's whole range, as the sorting does.
				float centre = (1.0 - smoothstep(0.05, 0.65, fr.g + 0.2 * (fn - 0.5))) * fg;
				float border = smoothstep(0.35, 0.95, fr.g) * fg;
				float grain = tm_vn(wp * 19.0 + 7.0);
				// Silt and clay (a grey-brown, dry about 0.15 in this material's linear albedo; wet about 0.6 of that, darker than the stones round it as
				// in field photographs of sorted circles), drying out first in patches; a little of the ground's own colour stays (algal crusts, the last of the sward).
				vec3 silt = vec3(0.15, 0.13, 0.105) * (0.88 + 0.24 * grain) * mix(0.6, 1.0, smoothstep(0.6, 0.85, fm));
				// The crown drains and dries first; the margin against the stones stays wet and dark.
				silt *= mix(0.6, 1.25, smoothstep(0.1, 0.75, fr.b));
				albedo = mix(albedo, silt, 0.85 * centre);
				rough = mix(rough, mix(0.78, 0.92, smoothstep(0.6, 0.85, fm)), centre); // damp, not puddled: matte
				vec3 lag = vec3(0.15, 0.145, 0.135) * (0.75 + 0.5 * grain);
				albedo = mix(albedo, lag, 0.2 * border);
				rough = mix(rough, max(rough, 0.9), border);
			}
		}
	}
)";
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
	// Far stones (RockField3D's far tier): past where the rockfall blocks and boulders thin out (whole to 300 m, thinned
	// to 600 m), their colour and plan cover from the rocks' 32 m map carries them out to 4 km, so a boulder field
	// stays a stony slope at range instead of fading to the bare layers. Fades at the map's edge.
	c += R"(	if (rock_far_cover_rect.z > 0.0) {
		vec2 fuv = (wp.xz - rock_far_cover_rect.xy) / rock_far_cover_rect.zw;
		if (all(greaterThanEqual(fuv, vec2(0.0))) && all(lessThanEqual(fuv, vec2(1.0)))) {
			vec4 fc = textureLod(rock_far_cover_map, fuv, 0.0);
			// From where that texel's drawn stones stop (a river bed's near its layer's radius; the rest 250 to 700 m).
			vec2 fr = textureLod(rock_far_reach_map, fuv, 0.0).rg * 2040.0;
			fr = fr.y > 0.0 ? fr : vec2(250.0, 700.0);
			float far_k = smoothstep(fr.x, max(fr.y, fr.x + 1.0), length(wp - CAMERA_POSITION_WORLD));
			vec2 edge = min(fuv, 1.0 - fuv);
			far_k *= smoothstep(0.0, 0.05, min(edge.x, edge.y));
			float k = fc.a * far_k * 0.8 * (0.8 + 0.4 * tm_vn(wp * 0.03 + 41.0));
			albedo = mix(albedo, fc.rgb, k);
			rough = mix(rough, max(rough, 0.85), k);
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
		tm_sand_dry = 1.0 - soaked;
		// The grains past the ground detail's reach stay rough on a wet bed.
		rough = mix(rough, 0.72, tm_gc_any);
	}
)";
	if (ground_deform_enabled) {
		// Freshly turned ground is darker until it dries.
		c += "\tvec2 gd = ground_deform_tint(wp);\n\talbedo *= gd.x;\n\trough = clamp(rough * gd.y, 0.0, 1.0);\n";
		// Wind ripples past their real geometry (GroundSurface3D's 4 m window): the grown field's slope as a normal
		// over the sand of the patch, as BaseMaterial3D draws it, and past the patch over the dune field's sand
		// (far_ripples), by this material's sand share.
		c += "\tvec3 gr_sort;\n\tvec3 gr_far = ground_ripple_far_slope(wp.xz, length(tm_dpx), wn.y, tm_sand_b, gr_sort);\n\tsn = normalize(sn - vec3(gr_far.x, 0.0, gr_far.y));\n\talbedo *= gr_far.z * gr_sort;\n";
		// Rain pits (GroundSurface3D rain_pits), as BaseMaterial3D draws them: on this material's sand, none in prints.
		c += R"(	{
		vec4 gm6 = ground_sand_meta6();
		if (gm6.x > 0.0) {
			float gpt;
			vec2 gps = ground_rain_pits(wp.xz, gm6.x * tm_sand_b * (1.0 - smoothstep(0.003, 0.008, abs(ground_deform_relief(wp.xz)))), gm6.z, length(tm_dpx), gpt);
			albedo *= gpt;
			sn = normalize(sn - vec3(gps.x, 0.0, gps.y));
		}
	}
)";
	}
	c += "\tfloat metallic = 0.0;\n\tfloat specular = 0.5 * tm_gc_spec;\n";
	if (grass_tint_enabled) {
		// The sward hides the soil's sheen (GrassField3D): without it the meadow past the blades reads pale grey.
		c += "\tspecular = mix(grass_ground_specular(wp, specular), specular, tm_bare_g);\n";
	}
	if (far_blend_enabled) {
		// Past far_start the cover map hides the sheen by its cover share, as the sward does near.
		c += "\tspecular = mix(specular, mix(0.5, 0.1, tm_far_cover), tm_far_t);\n";
	}
	if (ground_deform_enabled) {
		// The opened soil keeps its sheen (the sward hid it; the ground detail's stones still cover it); standing water in ruts (mud stage 2) and the clay it
		// carries (round 3), as BaseMaterial3D draws them (material.cpp ground_water_look()).
		c += R"(	specular = mix(specular, 0.5 * tm_gc_spec, tm_exposed);
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
	if (ground_deform_enabled) {
		// The ground's pore water (GroundSurface3D's wet map: a wet margin round still water, runoff, the swash) on the
		// terrain past the patch, as the path tracer already draws it (g_wet): never wetted twice with the rain's.
		c += "\tfloat tm_gwet = ground_wet_at(wp, wn).x;\n";
		// Adhesion warts (GroundSurface3D adhesion_warts), as BaseMaterial3D draws them, on this material's sand.
		c += R"(	{
		vec4 gm7 = ground_sand_meta7();
		if (gm7.z > 0.0) {
			float gdamp = smoothstep(0.15, 0.3, tm_gwet);
			float gcap;
			vec2 gas = ground_adhesion_warts(wp.xz, gm7.z * gdamp * tm_sand_b, gm7.xy, length(tm_dpx), gcap);
			sn = normalize(sn - vec3(gas.x, 0.0, gas.y));
			tm_gwet *= 1.0 - 0.8 * gcap;
		}
	}
)";
	} else {
		c += "\tfloat tm_gwet = 0.0;\n";
	}
	if (wetness_enabled) {
		// Rain soaks the ground by each layer's porosity (Lagarde 2013), before snow lies on it.
		// On printed ground a wet track's water lies in its prints (ground_contact_shape()).
		c += ground_deform_enabled ? "\tfloat tm_wwet = weather_wetness_at(wp, ground_contact_shape(wp.xz));\n" : "\tfloat tm_wwet = weather_wetness_at(wp);\n";
		if (salt_bloom) {
			// Salt bloom on the sea cliffs' rock and scree (weather_salt_bloom_apply, as the rocks' shader): kept on steep
			// and overhanging faces, washed off the tops, patchy over 2.5 m, 0.9 m and 0.35 m (octaves turned against each other so the value noise's grid does not
			// show; the finest fades as the pixel nears it); a 4 cm crystal grain.
			c += "\tweather_salt_bloom_apply(weather_salt_at(wp) * tm_stone_b, max(tm_wetness * tm_wwet, tm_gwet), (1.0 - 0.75 * smoothstep(0.25, 0.8, wn.y)), 0.5 * tm_vn(wp / 2.5 + 37.0) + 0.3 * tm_vn(wp.zyx / 0.9 + 53.0) + 0.2 * mix(0.5, tm_vn(wp.yxz / 0.35 + 71.0), 1.0 - smoothstep(0.08, 0.3, tm_fp0)), mix(0.5, tm_vn(wp / 0.04 + 89.0), 1.0 - smoothstep(0.01, 0.04, tm_fp0)), albedo, rough);\n";
		}
		c += "\tweather_wetness_apply(max(tm_wetness * tm_wwet, tm_gwet), wn.y, metallic, porosity, albedo, rough);\n";
		// A film of rain water drowns the grains' facets.
		c += "\ttm_sand_dry *= 1.0 - smoothstep(0.05, 0.3, tm_wetness * tm_wwet);\n";
		// Hoarfrost on clear cold nights (WeatherMap3D dew_and_frost).
		c += "\tweather_frost_apply(weather_frost_at(wp), wn.y, albedo, rough);\n";
		c += "\tweather_rime_apply(weather_rime_at(wp) * (1.0 - smoothstep(0.5, 0.85, wn.y)), wn, wp, albedo, rough);\n";
	} else if (ground_deform_enabled) {
		c += "\t{\n\t\tfloat gw_f = mix(1.0, 0.2, porosity);\n\t\talbedo *= mix(1.0, gw_f, tm_gwet);\n\t\trough *= mix(1.0, gw_f, 0.5 * tm_gwet);\n\t}\n";
	}
	if (ground_deform_enabled) {
		// The salt crust round still water in a dry climate (GroundSurface3D salt_crusts).
		c += "\tground_salt_apply(wp, wn.y, length(fwidth(wp.xz)), albedo, rough);\n";
	}
	if (snow_enabled) {
		// The snowpack over everything: slides off past its slope, settles first on the flattest facets of the
		// detail normal, then buries it; on steep faces (the heightfield's slope, tm_cn) in broken patches.
		c += R"(	float snow = 0.0;
	float snow_wet = 0.0;
	{
		vec4 snow_s = snow_cover_at(wp);
		// Held or shed by the projection's normal (tm_pn): on a wall one cell across the vertex normal is flat at
		// both rows and held snow down the wall's height in white smears.
		vec3 snow_face = tm_pn.y < tm_cn.y ? tm_pn : tm_cn;
		// Bare soil's relief is millimetres to a couple of centimetres (crumbs, crust, raindrop pits), finer than its
		// scan's detail normal draws it at this scale, so a fresh pack of a centimetre or two buries it about as soon
		// as it buries a sward: the thin pack's flattest-facets rule took the scan's normal for bare relief and left
		// bare patches brown under 1.6 cm of fresh snow while the turf round them was white. Taken by the bare share.
		// Gentle bedrock the same: a slab's relief is joints and pits of a few centimetres that a fresh pack fills
		// first (snow smooths the ground it lands on, depressions before knobs: Veitinger et al. 2014), while the
		// rule read the walls of the scan's joints (and of its macro variation's, a metre across) as steep facets and
		// left them bare, black cracks through 2 cm of snow. Faces keep the rule and their broken patches.
		float snow_slab = tm_rock_b * smoothstep(0.8, 0.92, tm_cn.y);
		vec3 snow_n = normalize(mix(sn, wn, 0.75 * max(tm_bare_g, snow_slab)));
		snow = snow_cover_weight(snow_s.x * tm_snow, tm_snow_max_slope, tm_pn, snow_n, wp, snow_face);
		snow_wet = snow_s.z;
		snow_cover_apply(snow, snow_s.y, snow_wet, albedo, rough, metallic, specular);
)";
		if (overlays[OVERLAY_SNOW].is_valid()) {
			// The pack's own grain and crust from a scan (snow_02, docs/ideas/scan-textures.md): its normal over the
			// snow's smooth one and its roughness as a ratio round the pack's, both by the snow's share. Detail only:
			// the colour, sparkle and wetness stay the snow model's.
			c += R"(		vec3 snow_dn = vec3(0.0);
		if (snow > 1e-3) {
			vec3 sv_d;
			float sv_r;
			float sv_h;
			tm_ov_snow(wp, tm_pn, cliff, tm_dpx, tm_dpy, sv_d, sv_r, snow_dn, sv_h);
			rough = clamp(rough * mix(1.0, sv_r, snow * (1.0 - snow_wet)), 0.02, 1.0);
		}
		sn = normalize(mix(sn, normalize(wn + snow_dn), snow));
)";
		} else {
			c += "\t\tsn = normalize(mix(sn, wn, snow));\n";
		}
		c += "\t}\n";
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
		// Far sun shadows (docs/ideas/terrain-material.md, "Far sun shadows"): past tm_far_shadow_start, where the
		// shadow cascades thin out and end, the ground's own horizon toward the sun shades it: lit while the sun
		// stands over the horizon's tangent, with a penumbra of the sun's half degree plus a grid cell's blur.
		// Raster only: the path tracer traces the hills' shadows itself.
		c += R"(#if !defined(RT)
	if (terrain_sun_horizon_size.z > 0.0 && tm_far_shadow_start > 0.0) {
		vec3 fs_l = sun_direction();
		float fs_run = length(fs_l.xz);
		float fs_dist = length(wp - CAMERA_POSITION_WORLD);
		float fs_fade = smoothstep(tm_far_shadow_start, 2.0 * tm_far_shadow_start, fs_dist);
		if (fs_fade > 0.0 && fs_run > 1e-4 && dot(fs_l, fs_l) > 0.5) {
			vec2 fs_d = terrain_sun_horizon_frame.xy;
			vec2 fs_st = vec2(dot(wp.xz, fs_d), dot(wp.xz, vec2(-fs_d.y, fs_d.x))) - terrain_sun_horizon_frame.zw;
			vec2 fs_uv = fs_st / terrain_sun_horizon_size.xy;
			if (all(greaterThanEqual(fs_uv, vec2(0.0))) && all(lessThanEqual(fs_uv, vec2(1.0)))) {
				vec2 fs_size = vec2(textureSize(terrain_sun_horizon_map, 0));
				// The tangent moved to the pixel's own height (the grid is 8 m: crests stand above it, hollows under it).
				vec3 fs_m = textureLod(terrain_sun_horizon_map, (fs_uv * (fs_size - 1.0) + 0.5) / fs_size, 0.0).rgb;
				float fs_hz = fs_m.r + (fs_m.b - wp.y) * fs_m.g;
				float fs_tan = fs_l.y / fs_run;
				// The sun's disc (0.53 degrees) and the grid's own smoothing widen the edge; a small bias keeps the
				// ground from shading itself on its own slope.
				float fs_lit = smoothstep(fs_hz - 0.006, fs_hz + 0.014, fs_tan);
				SUN_SHADOW *= mix(1.0, fs_lit, fs_fade);
			}
		}
	}
#endif
#if !defined(RT)
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
	// Sand glitter on dry, open sand (not under grain cover, rain or snow). Under the path tracer the base specular
	// lies on the facet of the grain each sample hits; in raster the facets mirroring the sun are counted per pixel.
	c += String(R"(	{
		float sg = tm_sand_b * tm_gc_spec * tm_sand_dry)") + (snow_enabled ? " * (1.0 - step(0.5, snow))" : "") + R"(;
		if (sg > 0.01) {
			if (IN_PATH_TRACER) {
				// The pixel's flashes as in raster (the ray cone's footprint, back to the pixel's own width), each
				// sample taking the facet's mirror lobe with the chance of its share: the samples land on grains all
				// over the pixel, so a grain-scale hash is their uniform draw.
				vec3 sg_l = relief_sun_direction();
				float sg_fp = RAY_FOOTPRINT;
				if (dot(sg_l, sg_l) > 0.5 && sg_fp > 0.0) {
					vec3 sg_v = normalize((INV_VIEW_MATRIX * vec4(VIEW, 0.0)).xyz);
					sg_fp *= max(abs(dot(wn, sg_v)), 0.05);
					float coat = tm_sg_coat(wp, sn, sg_l, sg_v, sg_fp, sg);
					if (coat > 0.0 && float(tm_sg_hash(ivec3(floor(wp / 0.0005)), 3u) & 0xffffu) / 65536.0 < coat) {
						SPECULAR_NORMAL = normalize((VIEW_MATRIX * vec4(normalize(sg_l + sg_v), 0.0)).xyz);
						ROUGHNESS = 0.1;
					}
				}
			} else {
				// Raster: the flashes drawn as the coat's lobe turned onto the half vector, so they are shadowed and
				// lit as the sun is.
				vec3 sg_l = sun_direction();
				if (dot(sg_l, sg_l) > 0.5) {
					vec3 sg_v = normalize(CAMERA_POSITION_WORLD - wp);
					vec3 sg_h = normalize(sg_l + sg_v);
					float sg_pix = 2.0 / (abs(PROJECTION_MATRIX[1][1]) * VIEWPORT_SIZE.y);
					float sg_fp = PROJECTION_MATRIX[3][3] != 0.0 ? sg_pix : length(VERTEX) * sg_pix;
					float coat = tm_sg_coat(wp, sn, sg_l, sg_v, sg_fp, sg);
					{
						if (coat > 0.0) {
							vec3 sg_t = normalize(cross(abs(sn.z) < 0.99 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0), sn));
							vec3 vt = normalize((VIEW_MATRIX * vec4(sg_t, 0.0)).xyz);
							vec3 vb = normalize((VIEW_MATRIX * vec4(cross(sn, sg_t), 0.0)).xyz);
							vec3 f = normalize((VIEW_MATRIX * vec4(sg_h, 0.0)).xyz);
							mat3 tbf = mat3(vt, vb, f);
							if (abs(determinant(tbf)) > 1e-3) {
								// NORMAL becomes the facets' (the coat's normal) and NORMAL_MAP lands the base back on
								// the sand's.
								vec3 kk = inverse(tbf) * NORMAL;
								if (kk.z > 0.0) {
									kk /= length(kk);
									TANGENT = vt;
									BINORMAL = vb;
									NORMAL = f;
									NORMAL_MAP = vec3(kk.xy * 0.5 + 0.5, 1.0);
									NORMAL_MAP_DEPTH = 1.0;
									CLEARCOAT = clamp(coat, 0.0, 1.0);
									CLEARCOAT_ROUGHNESS = 0.1;
								}
							}
						}
					}
				}
			}
		}
	}
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
	// The mud overlay's mask: GroundSurface3D's own mud (ruts, digs, wallows and laid mud where the soil is wet
	// enough to be mud; material.cpp ground_deform_mud()), where the ground deforms at all.
	c = c.replace("TM_OM_MUD", ground_deform_enabled ? "ground_deform_mud(wp)" : "0.0");
	return c;
}

void TerrainMaterial3D::_update_params() {
	set_shader_parameter("tm_geology", geology_enabled);
	set_shader_parameter("tm_dune_tone_on", sand_dune_tone);
	set_shader_parameter("tm_granule_on", sand_granule_ripples);
	set_shader_parameter("tm_sea_level", sea_level);
	set_shader_parameter("tm_beach_height", beach_height);
	set_shader_parameter("tm_rock_cos", Math::cos(Math::deg_to_rad(rock_slope)));
	set_shader_parameter("tm_cliff_cos", Math::cos(Math::deg_to_rad(cliff_slope)));
	set_shader_parameter("tm_rock_detail_scale", rock_detail_scale);
	set_shader_parameter("tm_scree_blocks_distance", scree_blocks_distance);
	set_shader_parameter("tm_gc_on", cover_near.is_valid() && cover_far.is_valid());
	if (cover_near.is_valid() && cover_far.is_valid()) {
		set_shader_parameter("tm_gc_near", cover_near);
		set_shader_parameter("tm_gc_far", cover_far);
		set_shader_parameter("tm_gc_near_rect", Vector4(cover_near_rect.position.x, cover_near_rect.position.y, cover_near_rect.size.x, cover_near_rect.size.y));
		set_shader_parameter("tm_gc_far_rect", Vector4(cover_far_rect.position.x, cover_far_rect.position.y, cover_far_rect.size.x, cover_far_rect.size.y));
		set_shader_parameter("tm_gc_reach", cover_reach);
		set_shader_parameter("tm_gc_band", cover_band);
		set_shader_parameter("tm_gc_size", cover_size);
		PackedVector3Array cols;
		cols.resize(8);
		for (int i = 0; i < 8; i++) {
			const Color c = i < cover_colors.size() ? cover_colors[i] : Color(0.35, 0.33, 0.3);
			cols.set(i, Vector3(c.r, c.g, c.b));
		}
		set_shader_parameter("tm_gc_colors", cols);
	}
	set_shader_parameter("tm_alluvium_area", alluvium_area);
	set_shader_parameter("tm_blend_depth", blend_depth);
	set_shader_parameter("tm_detail_octaves", detail_octaves);
	set_shader_parameter("tm_snow_max_slope", snow_max_slope);
	set_shader_parameter("tm_bare_on", climate_bare_soil);
	set_shader_parameter("tm_bare_soil_color", bare_soil_color);
	set_shader_parameter("tm_bare_alluvium_color", bare_alluvium_color);
	set_shader_parameter("tm_cured_cover_color", cured_cover_color);
	if (sky_occlusion_enabled) {
		set_shader_parameter("tm_sky_occlusion", sky_occlusion);
		set_shader_parameter("tm_far_shadow_start", far_shadow_start);
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
				const Color m = l->is_detail_only() ? l->get_albedo_mean() : Color(1, 1, 1);
				set_shader_parameter("tm_" + n + "_albedo_mean", Vector3(m.r, m.g, m.b));
			}
			if (l->get_normal_texture().is_valid()) {
				set_shader_parameter("tm_" + n + "_normal_tex", l->get_normal_texture());
			}
			if (l->get_roughness_texture().is_valid()) {
				set_shader_parameter("tm_" + n + "_roughness_tex", l->get_roughness_texture());
				set_shader_parameter("tm_" + n + "_roughness_mean", l->is_detail_only() ? l->get_roughness_mean() : 1.0f);
			}
			if (l->get_height_texture().is_valid()) {
				set_shader_parameter("tm_" + n + "_height_tex", l->get_height_texture());
			}
		}
	}
	for (int i = 0; i < OVERLAY_MAX; i++) {
		const Ref<TerrainMaterialLayer> &l = overlays[i];
		if (l.is_null()) {
			continue;
		}
		const String n = String("tm_ov_") + OVERLAY_NAMES[i];
		set_shader_parameter(n + "_normal", l->get_normal_strength());
		set_shader_parameter(n + "_scale", 1.0f / l->get_tile_size());
		if (l->get_albedo_texture().is_valid()) {
			set_shader_parameter(n + "_albedo_tex", l->get_albedo_texture());
			const Color m = l->is_detail_only() ? l->get_albedo_mean() : Color(1, 1, 1);
			set_shader_parameter(n + "_albedo_mean", Vector3(m.r, m.g, m.b));
		}
		if (l->get_normal_texture().is_valid()) {
			set_shader_parameter(n + "_normal_tex", l->get_normal_texture());
		}
		if (l->get_roughness_texture().is_valid()) {
			set_shader_parameter(n + "_roughness_tex", l->get_roughness_texture());
			set_shader_parameter(n + "_roughness_mean", l->is_detail_only() ? l->get_roughness_mean() : 1.0f);
		}
		if (l->get_height_texture().is_valid()) {
			set_shader_parameter(n + "_height_tex", l->get_height_texture());
		}
	}
	// The snow scan for every snow drawer (SnowSurface3D reads these globals; one scan, no private copies): its normal,
	// roughness and (1 / tile, normal strength, roughness mean, 1). A material without one leaves them as they are.
	const Ref<TerrainMaterialLayer> &snow_scan = overlays[OVERLAY_SNOW];
	if (snow_scan.is_valid() && snow_scan->get_normal_texture().is_valid()) {
		RenderingServer *rs = RenderingServer::get_singleton();
		rs->global_shader_parameter_set("snow_grain_normal", snow_scan->get_normal_texture()->get_rid());
		if (snow_scan->get_roughness_texture().is_valid()) {
			rs->global_shader_parameter_set("snow_grain_roughness", snow_scan->get_roughness_texture()->get_rid());
		}
		const float rm = snow_scan->get_roughness_texture().is_valid() ? snow_scan->get_roughness_mean() : 0.0f;
		rs->global_shader_parameter_set("snow_grain_params", Vector4(1.0f / snow_scan->get_tile_size(), snow_scan->get_normal_strength(), rm, 1.0f));
	}
	// Copies of this material (GroundSurface3D's near patch) follow its parameters through this signal.
	emit_changed();
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

void TerrainMaterial3D::set_overlay(Overlay p_overlay, const Ref<TerrainMaterialLayer> &p_value) {
	ERR_FAIL_INDEX(p_overlay, OVERLAY_MAX);
	const Callable cb = callable_mp(this, &TerrainMaterial3D::_layer_changed);
	if (overlays[p_overlay].is_valid() && overlays[p_overlay]->is_connected(CoreStringName(changed), cb)) {
		overlays[p_overlay]->disconnect(CoreStringName(changed), cb);
	}
	overlays[p_overlay] = p_value;
	if (p_value.is_valid() && !p_value->is_connected(CoreStringName(changed), cb)) {
		p_value->connect(CoreStringName(changed), cb);
	}
	_queue_update();
}

Ref<TerrainMaterialLayer> TerrainMaterial3D::get_overlay(Overlay p_overlay) const {
	ERR_FAIL_INDEX_V(p_overlay, OVERLAY_MAX, Ref<TerrainMaterialLayer>());
	return overlays[p_overlay];
}

// A scan's map from a file (no import: the art folders are .gdignore'd), mipmapped; grey keeps only its luminance
// (a green grass scan as the fibre detail of straw, whose colour the layer sets).
static Ref<Texture2D> _tm_scan_texture(const String &p_path, bool p_normal, bool p_grey) {
	if (!FileAccess::exists(p_path)) {
		return Ref<Texture2D>();
	}
	Ref<Image> img = Image::load_from_file(p_path);
	if (img.is_null() || img->is_empty()) {
		return Ref<Texture2D>();
	}
	if (p_grey && !p_normal) {
		img->convert(Image::FORMAT_RGB8);
		const int w = img->get_width();
		const int h = img->get_height();
		for (int y = 0; y < h; y++) {
			for (int x = 0; x < w; x++) {
				const Color c = img->get_pixel(x, y);
				const float l = c.get_luminance();
				img->set_pixel(x, y, Color(l, l, l));
			}
		}
	}
	img->generate_mipmaps(p_normal);
	return ImageTexture::create_from_image(img);
}

int TerrainMaterial3D::load_scan_set(const String &p_dir, const PackedStringArray &p_slots, const String &p_maps) {
	const String cfg_path = p_dir.path_join("scans.cfg");
	Ref<ConfigFile> cfg;
	cfg.instantiate();
	if (!DirAccess::dir_exists_absolute(p_dir) || cfg->load(cfg_path) != OK) {
		WARN_PRINT(vformat("TerrainMaterial3D: no scan set at '%s' (fetch the art first): every ground layer is drawn flat.", cfg_path));
		return 0;
	}
	int n = 0;
	const Vector<String> sections = cfg->get_sections();
	for (const String &sec : sections) {
		int layer = -1;
		int overlay = -1;
		for (int i = 0; i < LAYER_MAX; i++) {
			if (sec == LAYER_NAMES[i]) {
				layer = i;
			}
		}
		for (int i = 0; i < OVERLAY_MAX; i++) {
			if (sec == OVERLAY_NAMES[i]) {
				overlay = i;
			}
		}
		if (cfg->has_section_key(sec, "system")) {
			continue; // another system's scan (system="rocks": stones and caves), read by that system's own loader
		}
		if (layer < 0 && overlay < 0) {
			WARN_PRINT(vformat("TerrainMaterial3D: unknown slot [%s] in %s.", sec, cfg_path));
			continue;
		}
		if (!p_slots.is_empty() && !p_slots.has(sec)) {
			continue;
		}
		const String scan = cfg->get_value(sec, "scan", "");
		const String maps = p_maps.is_empty() ? String(cfg->get_value(sec, "maps", "anrh")) : p_maps;
		const bool grey = cfg->get_value(sec, "grey", false);
		const String base = p_dir.path_join(scan).path_join(scan);
		Ref<TerrainMaterialLayer> l;
		if (layer >= 0 && layers[layer].is_valid()) {
			l = layers[layer]->duplicate();
		} else {
			l.instantiate();
			if (layer >= 0) {
				l->set_color(LAYER_DEFAULT_COLORS[layer]);
			}
		}
		if (maps.contains("a")) {
			l->set_albedo_texture(_tm_scan_texture(base + "_albedo.jpg", false, grey));
		}
		if (maps.contains("n")) {
			l->set_normal_texture(_tm_scan_texture(base + "_normal.jpg", true, false));
		}
		if (maps.contains("r")) {
			l->set_roughness_texture(_tm_scan_texture(base + "_roughness.jpg", false, false));
		}
		if (maps.contains("h")) {
			l->set_height_texture(_tm_scan_texture(base + "_height.jpg", false, false));
		}
		if (l->get_albedo_texture().is_null() && l->get_normal_texture().is_null()) {
			WARN_PRINT(vformat("TerrainMaterial3D: scan '%s' for [%s] not found under %s.", scan, sec, p_dir));
			continue;
		}
		l->set_tile_size(cfg->get_value(sec, "tile", l->get_tile_size()));
		l->set_normal_strength(cfg->get_value(sec, "normal", l->get_normal_strength()));
		if (layer >= 0) {
			set_layer(Layer(layer), l);
		} else {
			set_overlay(Overlay(overlay), l);
		}
		n++;
	}
	return n;
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
MAT_SETTER(bool, sand_dune_tone, p_value)
MAT_SETTER(bool, sand_granule_ripples, p_value)
MAT_SETTER(float, sea_level, p_value)
MAT_SETTER(float, beach_height, MAX(p_value, 0.0f))
MAT_SETTER(float, rock_slope, CLAMP(p_value, 0.0f, 90.0f))
MAT_SETTER(float, cliff_slope, CLAMP(p_value, 0.0f, 90.0f))
MAT_SETTER(float, alluvium_area, p_value)
MAT_SETTER(float, blend_depth, MAX(p_value, 0.001f))
MAT_SETTER(int, detail_octaves, CLAMP(p_value, 0, 8))
MAT_SETTER(float, snow_max_slope, CLAMP(p_value, 0.0f, 90.0f))
MAT_SETTER(bool, climate_bare_soil, p_value)
MAT_SETTER(const Color &, bare_soil_color, p_value)
MAT_SETTER(const Color &, bare_alluvium_color, p_value)
MAT_SETTER(const Color &, cured_cover_color, p_value)
MAT_SETTER_CODE(const String &, detail_code)
MAT_SETTER_CODE(bool, grass_tint_enabled)
// RockField3D's ground detail cover (see the shader's tm_gc_cover): null maps turn it off.
void TerrainMaterial3D::set_ground_cover(const Ref<Texture2D> &p_near, const Ref<Texture2D> &p_far, const Rect2 &p_near_rect, const Rect2 &p_far_rect, const Vector3 &p_reach, const Vector3 &p_band, const Vector3 &p_size, const PackedColorArray &p_colors) {
	cover_near = p_near;
	cover_far = p_far;
	cover_near_rect = p_near_rect;
	cover_far_rect = p_far_rect;
	cover_reach = p_reach;
	cover_band = p_band;
	cover_size = p_size;
	cover_colors = p_colors;
	_update_params();
}

MAT_SETTER_CODE(bool, rock_detail_enabled)
MAT_SETTER(float, rock_detail_scale, CLAMP(p_value, 0.1f, 10.0f))
MAT_SETTER(float, scree_blocks_distance, MAX(p_value, 0.0f))
MAT_SETTER_CODE(bool, wetness_enabled)
MAT_SETTER_CODE(bool, salt_bloom)
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
MAT_SETTER(float, far_shadow_start, MAX(p_value, 0.0f))

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
	generator_id = gen->get_instance_id();
	if (!ground_color_registered) {
		WorldContracts::register_ground_color(this);
		ground_color_registered = true;
	}
}

// The shader's layer weights (terrain material fragment) on level-of-detail-free ground: no edge noise, the narrow
// slope bands, the generator's heightfield normal; each layer at its mean colour (a scan is divided by its own mean, and
// the tone patches average to 1).
Color TerrainMaterial3D::contract_ground_albedo_at(const Vector3 &p_position) const {
	const TerrainGenerator3D *gen = Object::cast_to<TerrainGenerator3D>(ObjectDB::get_instance(generator_id));
	if (!gen || !gen->has_result()) {
		return Color(0, 0, 0, -1);
	}
	const float y = gen->get_height(p_position);
	if (Math::is_nan(y)) {
		return Color(0, 0, 0, -1);
	}
	// The heightfield's normal over 2 m.
	const float e = 2.0f;
	const float hx0 = gen->get_height(p_position + Vector3(-e, 0, 0));
	const float hx1 = gen->get_height(p_position + Vector3(e, 0, 0));
	const float hz0 = gen->get_height(p_position + Vector3(0, 0, -e));
	const float hz1 = gen->get_height(p_position + Vector3(0, 0, e));
	float ny = 1.0f;
	if (!Math::is_nan(hx0 + hx1 + hz0 + hz1)) {
		ny = Vector3(hx0 - hx1, 2.0f * e, hz0 - hz1).normalized().y;
	}
	const float rock_cos = Math::cos(Math::deg_to_rad(rock_slope));
	const float cliff_cos = Math::cos(Math::deg_to_rad(cliff_slope));
	const float steep = 1.0f - Math::smoothstep(rock_cos - 0.08f, rock_cos + 0.04f, ny);
	float w[LAYER_MAX] = {};
	float beach_sand = -1.0f;
	const Color g = geology_enabled ? gen->get_material_geology_at(p_position) : Color(Math::NaN, 0, 0, 0);
	if (!Math::is_nan(g.r)) {
		beach_sand = CLAMP(-g.r, 0.0f, 1.0f);
		const float cliff_rock = 1.0f - Math::smoothstep(cliff_cos - 0.08f, cliff_cos + 0.04f, ny);
		const float rock = MAX(Math::smoothstep(0.2f, 0.8f, CLAMP(g.g, 0.0f, 1.0f)), cliff_rock);
		const float till = CLAMP(-g.b, 0.0f, 1.0f) * (1.0f - rock);
		const float sg = CLAMP(g.b, 0.0f, 1.0f);
		float scree = sg * (1.0f - rock) + 0.45f * till;
		float rest = MAX(1.0f - rock - scree, 0.0f);
		const float regolith = Math::smoothstep(0.05f, 0.3f, sg) * (1.0f - till);
		scree += rest * regolith;
		rest *= 1.0f - regolith;
		const float bare_near = steep * MAX(Math::smoothstep(0.02f, 0.2f, CLAMP(g.g, 0.0f, 1.0f)), Math::smoothstep(0.02f, 0.2f, sg));
		scree += rest * bare_near;
		rest *= 1.0f - bare_near;
		float alluvium = Math::smoothstep(alluvium_area, alluvium_area + 1.0f, g.a) * Math::smoothstep(0.92f, 0.97f, ny);
		// Rill and gully floors (as the shader, without its noise): gravel lag, fans of fines below.
		const Vector2 gl = gen->get_gully_at(p_position, ny);
		const float gully = gl.x * (1.0f - rock);
		scree += rest * gully;
		rest *= 1.0f - gully;
		alluvium = MAX(alluvium, float(Math::smoothstep(0.15f, 0.8f, float(gl.y)) * Math::smoothstep(0.95f, 0.985f, ny)));
		w[LAYER_ROCK] = rock;
		w[LAYER_SCREE] = scree;
		w[LAYER_SOIL] = rest * (1.0f - alluvium);
		w[LAYER_ALLUVIUM] = rest * alluvium;
	} else {
		w[LAYER_ROCK] = steep;
		w[LAYER_SOIL] = 1.0f - steep;
	}
	float sand = beach_sand >= 0.0f ? MAX(beach_sand, 1.0f - Math::smoothstep(-0.2f, 0.3f, y - sea_level)) : 1.0f - Math::smoothstep(beach_height * 0.6f, beach_height, y - sea_level);
	const float sand_steep = beach_sand > 0.0f ? Math::lerp(steep, 1.0f - Math::smoothstep(0.682f, 0.719f, ny), beach_sand) : steep;
	sand *= 1.0f - sand_steep;
	for (int i = 0; i < LAYER_SAND; i++) {
		w[i] *= 1.0f - sand;
	}
	w[LAYER_SAND] = sand;
	const float bare = climate_bare_soil ? MAX(WorldContracts::climate_bare_share_at(p_position), 0.0f) : 0.0f;
	const float cured = climate_bare_soil ? MAX(WorldContracts::climate_cured_share_at(p_position), 0.0f) : 0.0f;
	Color c(0, 0, 0, 0);
	float total = 0.0f;
	for (int i = 0; i < LAYER_MAX; i++) {
		if (w[i] <= 0.0f) {
			continue;
		}
		const Ref<TerrainMaterialLayer> &l = layers[i];
		Color lc = (l.is_valid() ? l->get_color() : LAYER_DEFAULT_COLORS[i]).srgb_to_linear();
		if (cured > 0.0f && (i == LAYER_SOIL || i == LAYER_ALLUVIUM)) {
			lc = lc.lerp(cured_cover_color.srgb_to_linear(), i == LAYER_SOIL ? cured : 0.8f * cured); // the cover cured to straw
		}
		if (bare > 0.0f && (i == LAYER_SOIL || i == LAYER_ALLUVIUM)) {
			lc = lc.lerp((i == LAYER_SOIL ? bare_soil_color : bare_alluvium_color).srgb_to_linear(), bare); // the patches' mean, as the shader from afar
		}
		const float por = l.is_valid() ? l->get_porosity() : 0.7f;
		c.r += lc.r * w[i];
		c.g += lc.g * w[i];
		c.b += lc.b * w[i];
		c.a += por * w[i];
		total += w[i];
	}
	if (total <= 1e-6f) {
		return Color(0, 0, 0, -1);
	}
	return Color(c.r / total, c.g / total, c.b / total, c.a / total);
}

void TerrainMaterial3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_ground_cover", "near", "far", "near_rect", "far_rect", "reach", "band", "size", "colors"), &TerrainMaterial3D::set_ground_cover);
	ClassDB::bind_method(D_METHOD("update_shader"), &TerrainMaterial3D::update_shader);
	ClassDB::bind_method(D_METHOD("get_shader_code"), &TerrainMaterial3D::get_shader_code);
	ClassDB::bind_method(D_METHOD("set_layer", "layer", "value"), &TerrainMaterial3D::set_layer);
	ClassDB::bind_method(D_METHOD("get_layer", "layer"), &TerrainMaterial3D::get_layer);
	ClassDB::bind_method(D_METHOD("set_overlay", "overlay", "value"), &TerrainMaterial3D::set_overlay);
	ClassDB::bind_method(D_METHOD("get_overlay", "overlay"), &TerrainMaterial3D::get_overlay);
	ClassDB::bind_method(D_METHOD("load_scan_set", "dir", "slots", "maps"), &TerrainMaterial3D::load_scan_set, DEFVAL(PackedStringArray()), DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("use_generator", "generator"), &TerrainMaterial3D::use_generator);
	ClassDB::bind_method(D_METHOD("get_generator"), &TerrainMaterial3D::get_generator);
	ClassDB::bind_method(D_METHOD("get_ground_albedo_at", "position"), &TerrainMaterial3D::get_ground_albedo_at);
#define BIND_PROP(m_type, m_name, m_get, m_hint, m_hint_str) \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &TerrainMaterial3D::set_##m_name); \
	ClassDB::bind_method(D_METHOD(#m_get), &TerrainMaterial3D::m_get); \
	ADD_PROPERTY(PropertyInfo(m_type, #m_name, m_hint, m_hint_str), "set_" #m_name, #m_get);

	for (int i = 0; i < LAYER_MAX; i++) {
		ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, String(LAYER_NAMES[i]) + "_layer", PROPERTY_HINT_RESOURCE_TYPE, "TerrainMaterialLayer"), "set_layer", "get_layer", i);
	}
	ADD_GROUP("Overlay scans", "");
	for (int i = 0; i < OVERLAY_MAX; i++) {
		ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, String(OVERLAY_NAMES[i]) + "_scan", PROPERTY_HINT_RESOURCE_TYPE, "TerrainMaterialLayer"), "set_overlay", "get_overlay", i);
	}
	ADD_GROUP("", "");
	BIND_PROP(Variant::BOOL, geology_enabled, is_geology_enabled, PROPERTY_HINT_NONE, "")
	BIND_PROP(Variant::BOOL, sand_dune_tone, is_sand_dune_tone, PROPERTY_HINT_NONE, "")
	BIND_PROP(Variant::BOOL, sand_granule_ripples, is_sand_granule_ripples, PROPERTY_HINT_NONE, "")
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
	BIND_PROP(Variant::BOOL, salt_bloom, is_salt_bloom, PROPERTY_HINT_NONE, "")
	BIND_PROP(Variant::BOOL, snow_enabled, is_snow_enabled, PROPERTY_HINT_NONE, "")
	BIND_PROP(Variant::FLOAT, snow_max_slope, get_snow_max_slope, PROPERTY_HINT_RANGE, "0,90,0.1,degrees")
	BIND_PROP(Variant::BOOL, climate_bare_soil, is_climate_bare_soil, PROPERTY_HINT_NONE, "")
	BIND_PROP(Variant::COLOR, bare_soil_color, get_bare_soil_color, PROPERTY_HINT_NONE, "")
	BIND_PROP(Variant::COLOR, bare_alluvium_color, get_bare_alluvium_color, PROPERTY_HINT_NONE, "")
	BIND_PROP(Variant::COLOR, cured_cover_color, get_cured_cover_color, PROPERTY_HINT_NONE, "")
	BIND_PROP(Variant::BOOL, ground_deform_enabled, is_ground_deform_enabled, PROPERTY_HINT_NONE, "")
	BIND_PROP(Variant::BOOL, sky_occlusion_enabled, is_sky_occlusion_enabled, PROPERTY_HINT_NONE, "")
	BIND_PROP(Variant::FLOAT, sky_occlusion, get_sky_occlusion, PROPERTY_HINT_RANGE, "0,1,0.01")
	BIND_PROP(Variant::FLOAT, far_shadow_start, get_far_shadow_start, PROPERTY_HINT_RANGE, "0,5000,1,or_greater,suffix:m")
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
	BIND_ENUM_CONSTANT(OVERLAY_ROCK_VARIANT);
	BIND_ENUM_CONSTANT(OVERLAY_CLIFF);
	BIND_ENUM_CONSTANT(OVERLAY_CURED);
	BIND_ENUM_CONSTANT(OVERLAY_BARE_SOIL);
	BIND_ENUM_CONSTANT(OVERLAY_BARE_ALLUVIUM);
	BIND_ENUM_CONSTANT(OVERLAY_BANK);
	BIND_ENUM_CONSTANT(OVERLAY_TRAIL);
	BIND_ENUM_CONSTANT(OVERLAY_BED);
	BIND_ENUM_CONSTANT(OVERLAY_BEACH);
	BIND_ENUM_CONSTANT(OVERLAY_MUD);
	BIND_ENUM_CONSTANT(OVERLAY_BURN);
	BIND_ENUM_CONSTANT(OVERLAY_SNOW);
	BIND_ENUM_CONSTANT(OVERLAY_MAX);
}

TerrainMaterial3D::TerrainMaterial3D() {
	shader.instantiate();
	set_shader(shader);
	update_shader();
}

TerrainMaterial3D::~TerrainMaterial3D() {
	if (ground_color_registered) {
		WorldContracts::unregister_ground_color(this);
	}
}

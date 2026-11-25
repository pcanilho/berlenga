/**************************************************************************/
/*  terrain_material_3d.h                                                 */
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

#include "scene/resources/material.h"
#include "scene/resources/texture.h"

#include "modules/world/world_contracts.h"

class TerrainGenerator3D;

// One surface of a TerrainMaterial3D (rock, scree, soil, alluvium, sand): textures, tint and how it tiles.
class TerrainMaterialLayer : public Resource {
	GDCLASS(TerrainMaterialLayer, Resource);

	Ref<Texture2D> albedo_texture;
	Ref<Texture2D> normal_texture;
	Ref<Texture2D> roughness_texture;
	Ref<Texture2D> height_texture;
	Color color = Color(1, 1, 1);
	float roughness = 0.9f;
	float normal_strength = 1.0f;
	float tile_size = 4.0f;
	float porosity = 0.7f;
	bool detail_only = true;
	mutable Color albedo_mean;
	mutable bool albedo_mean_valid = false;
	mutable float roughness_mean = 1.0f;
	mutable bool roughness_mean_valid = false;

protected:
	static void _bind_methods();

public:
	void set_detail_only(bool p_enable);
	bool is_detail_only() const { return detail_only; }
	// The albedo texture's mean (linear) and the roughness texture's (green), what the shader divides them by in
	// detail_only mode.
	Color get_albedo_mean() const;
	float get_roughness_mean() const;
	void set_albedo_texture(const Ref<Texture2D> &p_texture);
	Ref<Texture2D> get_albedo_texture() const { return albedo_texture; }
	void set_normal_texture(const Ref<Texture2D> &p_texture);
	Ref<Texture2D> get_normal_texture() const { return normal_texture; }
	void set_roughness_texture(const Ref<Texture2D> &p_texture);
	Ref<Texture2D> get_roughness_texture() const { return roughness_texture; }
	void set_height_texture(const Ref<Texture2D> &p_texture);
	Ref<Texture2D> get_height_texture() const { return height_texture; }
	void set_color(const Color &p_color);
	Color get_color() const { return color; }
	void set_roughness(float p_roughness);
	float get_roughness() const { return roughness; }
	void set_normal_strength(float p_strength);
	float get_normal_strength() const { return normal_strength; }
	void set_tile_size(float p_size);
	float get_tile_size() const { return tile_size; }
	void set_porosity(float p_porosity);
	float get_porosity() const { return porosity; }
};

// A ground material that writes its own shader: layers chosen by the TerrainGenerator3D's geology (or by slope and
// height without one), height-blended, biplanar on cliffs, the generator's erosion detail in the normal, the grass
// fields' ground tint, then the weather's wetness and the snowpack's snow (docs/ideas/terrain-material.md).
class TerrainMaterial3D : public ShaderMaterial, public WorldGroundColorProvider {
	GDCLASS(TerrainMaterial3D, ShaderMaterial);

public:
	enum Layer {
		LAYER_ROCK,
		LAYER_SCREE,
		LAYER_SOIL,
		LAYER_ALLUVIUM,
		LAYER_SAND,
		LAYER_MAX,
	};
	// Scans laid over a layer's own where the shader's masks say (docs/ideas/scan-textures.md): the parent layer keeps
	// its colour and roughness (the scan is detail only), only the detail changes.
	enum Overlay {
		OVERLAY_ROCK_VARIANT, // a second rock scan in 25 m patches, so one scan does not repeat over a whole hillside
		OVERLAY_CLIFF, // rock faces past about 45 degrees: a face scan (beds, joints) for the ground scan seen from above
		OVERLAY_CURED, // soil and alluvium cover cured to straw in summer-dry climates
		OVERLAY_BARE_SOIL, // the climate's bare soil patches
		OVERLAY_BARE_ALLUVIUM, // the climate's bare alluvium patches (silt crusts and cracks)
		OVERLAY_BANK, // soil and alluvium faces from about 42 to 53 degrees: a cut soil face (banks, slump scarps)
		OVERLAY_TRAIL, // trails walked bare (WorldStand's trail wear): packed soil
		OVERLAY_BED, // channel beds (the generator's drainage area a hundred times the alluvium's): rounded river gravel
		OVERLAY_BEACH, // sand the generator laid as beach, and the seabed
		OVERLAY_MUD, // ground turned by ruts, digs and prints (GroundSurface3D's exposed soil)
		OVERLAY_BURN, // burn scars (WorldFire's burn age, Landscape3D's stand map): charred ground, fading as the cover regrows
		OVERLAY_SNOW, // the snowpack's grain and crust (normal and roughness only), over the snow, not a layer
		OVERLAY_MAX,
	};

private:
	Ref<TerrainMaterialLayer> layers[LAYER_MAX];
	Ref<TerrainMaterialLayer> overlays[OVERLAY_MAX];
	bool geology_enabled = true;
	bool sand_granule_ripples = true; // granule ripples on the interdune lag
	bool sand_dune_tone = true; // the sand's dune-scale colour (redder old sand, dark interdune flats, pale slip faces)
	float sea_level = 0.0f;
	float beach_height = 2.5f;
	float rock_slope = 40.0f;
	float cliff_slope = 50.0f;
	float alluvium_area = 5.5f;
	float blend_depth = 0.2f;
	int detail_octaves = 4;
	String detail_code;
	bool grass_tint_enabled = true;
	bool rock_detail_enabled = true;
	float rock_detail_scale = 1.0f;
	float scree_blocks_distance = 0.0f;
	Ref<Texture2D> cover_near, cover_far;
	Rect2 cover_near_rect, cover_far_rect;
	Vector3 cover_reach, cover_band, cover_size;
	PackedColorArray cover_colors;
	bool wetness_enabled = true;
	bool salt_bloom = true; // the spray's salt crust on rock and scree (WeatherMap3D sea spray)
	bool snow_enabled = true;
	float snow_max_slope = 60.0f;
	// Soil and alluvium bare of plants by the climate's dryness (WorldContracts::bare_share_for_precipitation): the
	// layers' colours are a vegetated topsoil; bare, dry soil takes these (Munsell 10YR 5/3 brown loam, 10YR 6/2
	// grey-brown silt).
	bool climate_bare_soil = true;
	Color bare_soil_color = Color(0.49, 0.40, 0.33);
	Color bare_alluvium_color = Color(0.60, 0.55, 0.49);
	// The plant cover on soil and alluvium through the dry season where the climate cures it (WorldContracts::
	// cured_share_for_precipitation): senescent grassland, Munsell 2.5Y 5/4.
	Color cured_cover_color = Color(0.52, 0.45, 0.30);
	bool far_blend_enabled = true;
	float far_start = 300.0;
	bool far_start_from_vegetation = true;
	bool cliff_self_shadow = true;
	float far_fade = 150.0;
	bool ground_deform_enabled = true;
	bool sky_occlusion_enabled = true;
	float sky_occlusion = 1.0f;
	float far_shadow_start = 150.0f; // m: where the far sun shadows start fading in (0: none)

	Ref<Shader> shader;
	bool dirty = false;
	ObjectID generator_id; // use_generator()'s: the ground colour contract reads its geology
	bool ground_color_registered = false;

	void _queue_update();
	void _layer_changed();
	void _update_params();

protected:
	static void _bind_methods();

public:
	void update_shader();
	String get_shader_code() const;

	void set_layer(Layer p_layer, const Ref<TerrainMaterialLayer> &p_value);
	Ref<TerrainMaterialLayer> get_layer(Layer p_layer) const;
	void set_overlay(Overlay p_overlay, const Ref<TerrainMaterialLayer> &p_value);
	Ref<TerrainMaterialLayer> get_overlay(Overlay p_overlay) const;
	// Fills layers and overlays from a folder of scans described by its scans.cfg (one section per slot: scan, tile,
	// maps, grey, normal); returns how many slots took a scan, and warns when the folder or its manifest is missing.
	// p_slots: only those sections (all when empty); p_maps: which maps to load ("anrh", the manifest's when empty).
	int load_scan_set(const String &p_dir, const PackedStringArray &p_slots = PackedStringArray(), const String &p_maps = String());

	void set_geology_enabled(bool p_enabled);
	bool is_geology_enabled() const { return geology_enabled; }
	void set_sand_dune_tone(bool p_enabled);
	bool is_sand_dune_tone() const { return sand_dune_tone; }
	void set_sand_granule_ripples(bool p_enabled);
	bool is_sand_granule_ripples() const { return sand_granule_ripples; }
	void set_sea_level(float p_height);
	float get_sea_level() const { return sea_level; }
	void set_beach_height(float p_height);
	float get_beach_height() const { return beach_height; }
	void set_rock_slope(float p_degrees);
	float get_rock_slope() const { return rock_slope; }
	void set_cliff_slope(float p_degrees);
	float get_cliff_slope() const { return cliff_slope; }
	void set_alluvium_area(float p_log10);
	float get_alluvium_area() const { return alluvium_area; }
	void set_blend_depth(float p_depth);
	float get_blend_depth() const { return blend_depth; }
	void set_detail_octaves(int p_octaves);
	int get_detail_octaves() const { return detail_octaves; }
	void set_detail_code(const String &p_code);
	String get_detail_code() const { return detail_code; }
	void use_generator(Object *p_generator);
	void set_grass_tint_enabled(bool p_enabled);
	bool is_grass_tint_enabled() const { return grass_tint_enabled; }
	void set_rock_detail_enabled(bool p_enabled);
	bool is_rock_detail_enabled() const { return rock_detail_enabled; }
	void set_rock_detail_scale(float p_scale);
	float get_rock_detail_scale() const { return rock_detail_scale; }
	void set_scree_blocks_distance(float p_value);
	float get_scree_blocks_distance() const { return scree_blocks_distance; }
	void set_ground_cover(const Ref<Texture2D> &p_near, const Ref<Texture2D> &p_far, const Rect2 &p_near_rect, const Rect2 &p_far_rect, const Vector3 &p_reach, const Vector3 &p_band, const Vector3 &p_size, const PackedColorArray &p_colors);
	void set_sky_occlusion_enabled(bool p_enabled);
	bool is_sky_occlusion_enabled() const { return sky_occlusion_enabled; }
	void set_sky_occlusion(float p_amount);
	float get_sky_occlusion() const { return sky_occlusion; }
	void set_far_shadow_start(float p_value);
	float get_far_shadow_start() const { return far_shadow_start; }
	void set_far_blend_enabled(bool p_enabled);
	bool is_far_blend_enabled() const { return far_blend_enabled; }
	void set_far_start(float p_value);
	float get_far_start() const { return far_start; }
	void set_far_start_from_vegetation(bool p_value);
	bool is_far_start_from_vegetation() const { return far_start_from_vegetation; }
	void set_cliff_self_shadow(bool p_value);
	bool is_cliff_self_shadow() const { return cliff_self_shadow; }
	void set_far_fade(float p_value);
	float get_far_fade() const { return far_fade; }
	void set_wetness_enabled(bool p_enabled);
	bool is_wetness_enabled() const { return wetness_enabled; }
	void set_salt_bloom(bool p_enabled);
	bool is_salt_bloom() const { return salt_bloom; }
	void set_snow_enabled(bool p_enabled);
	bool is_snow_enabled() const { return snow_enabled; }
	void set_snow_max_slope(float p_degrees);
	float get_snow_max_slope() const { return snow_max_slope; }
	void set_climate_bare_soil(bool p_enabled);
	bool is_climate_bare_soil() const { return climate_bare_soil; }
	void set_bare_soil_color(const Color &p_color);
	Color get_bare_soil_color() const { return bare_soil_color; }
	void set_bare_alluvium_color(const Color &p_color);
	Color get_bare_alluvium_color() const { return bare_alluvium_color; }
	void set_cured_cover_color(const Color &p_color);
	Color get_cured_cover_color() const { return cured_cover_color; }
	void set_ground_deform_enabled(bool p_enabled);
	bool is_ground_deform_enabled() const { return ground_deform_enabled; }

	// WorldGround.albedo_at's ground colour (docs/ideas/world-contracts.md, Ground): the layers' mean colours weighed
	// as the shader weighs them from the generator's geology and slope, without the texture detail; a: the porosity.
	Color contract_ground_albedo_at(const Vector3 &p_position) const override;
	Color get_ground_albedo_at(const Vector3 &p_position) const { return contract_ground_albedo_at(p_position); }

	TerrainMaterial3D();
	~TerrainMaterial3D();
};

VARIANT_ENUM_CAST(TerrainMaterial3D::Layer);
VARIANT_ENUM_CAST(TerrainMaterial3D::Overlay);

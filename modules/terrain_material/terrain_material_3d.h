/**************************************************************************/
/*  terrain_material_3d.h                                                 */
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

#include "scene/resources/material.h"
#include "scene/resources/texture.h"

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

protected:
	static void _bind_methods();

public:
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
class TerrainMaterial3D : public ShaderMaterial {
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

private:
	Ref<TerrainMaterialLayer> layers[LAYER_MAX];
	bool geology_enabled = true;
	float sea_level = 0.0f;
	float beach_height = 2.5f;
	float rock_slope = 40.0f;
	float cliff_slope = 50.0f;
	float alluvium_area = 5.5f;
	float blend_depth = 0.2f;
	int detail_octaves = 4;
	String detail_code;
	bool grass_tint_enabled = true;
	bool wetness_enabled = true;
	bool snow_enabled = true;
	float snow_max_slope = 60.0f;
	bool ground_deform_enabled = true;

	Ref<Shader> shader;
	bool dirty = false;

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

	void set_geology_enabled(bool p_enabled);
	bool is_geology_enabled() const { return geology_enabled; }
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
	void set_wetness_enabled(bool p_enabled);
	bool is_wetness_enabled() const { return wetness_enabled; }
	void set_snow_enabled(bool p_enabled);
	bool is_snow_enabled() const { return snow_enabled; }
	void set_snow_max_slope(float p_degrees);
	float get_snow_max_slope() const { return snow_max_slope; }
	void set_ground_deform_enabled(bool p_enabled);
	bool is_ground_deform_enabled() const { return ground_deform_enabled; }

	TerrainMaterial3D();
};

VARIANT_ENUM_CAST(TerrainMaterial3D::Layer);

/**************************************************************************/
/*  grass_field_3d.h                                                      */
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

#include "plant_species.h"

#include "core/io/image.h"
#include "core/templates/hash_map.h"
#include "scene/3d/node_3d.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"

class MultiMeshInstance3D;
class TerrainDataMap3D;

// Grass blades around the camera (docs/ideas/grass.md), after Ghost of Tsushima (Wohllaib,
// GDC 2021): blades on a jittered grid seeded by world position, clumps that share height,
// lean and colour, a quadratic Bezier blade bent by the wind in the vertex shader, density
// thinned by rank with distance and blades widened to keep coverage. Tiles around the camera
// are built on the CPU when they come into range, one MultiMesh each.
class GrassField3D : public Node3D {
	GDCLASS(GrassField3D, Node3D);

	struct Tile {
		MultiMeshInstance3D *mmi = nullptr;
		int lod = 0;
	};

	NodePath data_map;
	NodePath canopy_field; // A VegetationField3D whose crowns shade the grass (rules.shade_range).
	Ref<PlantSpecies> rules;
	float density = 120.0f; // Blades per square metre, near the camera.
	Vector2 blade_height = Vector2(0.25f, 0.6f);
	float blade_width = 0.04f;
	float clump_size = 1.2f;
	float lean = 0.35f;
	Color base_color = Color(0.1f, 0.2f, 0.05f);
	Color tip_color = Color(0.45f, 0.6f, 0.22f);
	Color dry_color = Color(0.55f, 0.48f, 0.28f);
	float dryness = 0.2f;
	float sun_mean = 0.0f; // VegetationLight::sun_mean() of the data map.
	float color_variation = 0.5f;
	float radius = 40.0f;
	float tile_size = 8.0f;
	Vector2 fallback_wind = Vector2(2.0f, 0.5f);
	float wind_response = 0.12f;
	int max_tiles_per_frame = 12;
	int blade_segments = 4;
	float view_facing = 0.6f;
	// Ground under the grass takes its colour (alpha: strength), through the grass_ground_* globals.
	Color ground_tint = Color(0.05f, 0.09f, 0.025f, 0.85f);
	bool ground_tint_enabled = true;
	Vector4 fade_band; // Last centre and band sent to the shaders.
	static GrassField3D *fade_owner; // The field whose band the ground shader reads.
	bool ground_baked = false;
	class VegetationField3D *_get_canopy(const PlantSpecies *p_rules) const;
	Ref<ImageTexture> ground_texture;

	// Trample field (docs/ideas/grass.md, contact): bend per texel around the camera, pressed by
	// movers and relaxing back, so blades stay stateless and trails persist.
	bool trample_enabled = true;
	StringName trample_group = "grass_trample";
	float trample_size = 32.0f;
	int trample_resolution = 128;
	float trample_radius = 0.4f;
	float trample_recovery = 6.0f; // Half-life in seconds.
	LocalVector<Vector2> trample; // World xz bend per texel, length 0..1.
	Vector2i trample_origin; // World texel of the map's first texel.
	bool trample_active = false;
	Ref<Image> trample_image;
	Ref<ImageTexture> trample_texture;
	HashMap<ObjectID, Vector3> trample_last;

	bool gpu_fill = true; // Fill tiles with the compute shader (stage 3a) when a RenderingDevice exists.
	Ref<Image> height_image; // The data map's height channel, for the GPU fill.
	Rect2 height_rect;

	HashMap<Vector2i, Tile> tiles;
	Ref<ArrayMesh> blade_mesh;
	Ref<ShaderMaterial> material;
	int blade_count = 0;
	double build_msec = 0.0;

	// Far colour bake (docs/ideas/vegetation-lod-streaming.md, section 4): a patch of these blades
	// rendered in a private world, then a white ground under the same light; their ratio is the
	// colour the far ground takes. Runs once each time the blades' look changes.
	class SubViewport *far_bake_vp = nullptr;
	class DirectionalLight3D *far_bake_sun = nullptr;
	MultiMeshInstance3D *far_bake_blades = nullptr;
	Ref<StandardMaterial3D> far_bake_ground_mat;
	int far_bake_state = -1;
	int far_bake_wait = 0;
	bool far_bake_queued = false;
	bool far_baked = false;
	Color far_bake_sum;
	Color far_bake_white;
	Color far_color_baked;
	void _queue_far_bake();
	void _far_bake_start();
	void _far_bake_step();
	void _far_bake_free();
	Color _far_color_hand() const;

	void _ensure_resources();
	void _build_material();
	int _lod_for(float p_distance) const;
	void _build_tile(const Vector2i &p_tile, int p_lod, TerrainDataMap3D *p_map);
	void _build_tile_gpu(const Vector2i &p_tile, int p_lod, TerrainDataMap3D *p_map, const PlantSpecies *p_rules, const class VegetationField3D *p_canopy, const LocalVector<float> &p_light, int p_lg, float p_width);
	void _free_tile(Tile &p_tile);
	void _update_fade(const Vector2 &p_center);
	void _update_tiles(const Vector3 &p_center);
	void _bake_ground(TerrainDataMap3D *p_map);
	void _trample_recenter(const Vector3 &p_center);
	void _trample_step(double p_delta);
	void _trample_upload();
	Vector2 _mover_extent(Node3D *p_node) const; // Radius and half height.

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_gpu_fill(bool p_enabled);
	bool is_gpu_fill() const { return gpu_fill; }
	static bool is_gpu_fill_available();
	static void free_gpu_fill();
	void set_data_map(const NodePath &p_path);
	NodePath get_data_map() const { return data_map; }
	void set_canopy_field(const NodePath &p_path);
	NodePath get_canopy_field() const { return canopy_field; }
	void set_rules(const Ref<PlantSpecies> &p_rules);
	Ref<PlantSpecies> get_rules() const { return rules; }
	void set_density(float p_density);
	float get_density() const { return density; }
	void set_blade_height(const Vector2 &p_height);
	Vector2 get_blade_height() const { return blade_height; }
	void set_blade_width(float p_width);
	float get_blade_width() const { return blade_width; }
	void set_clump_size(float p_size);
	float get_clump_size() const { return clump_size; }
	void set_lean(float p_lean);
	float get_lean() const { return lean; }
	Color get_far_color() const;
	bool is_far_color_baked() const { return far_baked; }
	void set_base_color(const Color &p_color);
	Color get_base_color() const { return base_color; }
	void set_tip_color(const Color &p_color);
	Color get_tip_color() const { return tip_color; }
	void set_dry_color(const Color &p_color);
	Color get_dry_color() const { return dry_color; }
	void set_dryness(float p_dryness);
	float get_dryness() const { return dryness; }
	void set_color_variation(float p_amount);
	float get_color_variation() const { return color_variation; }
	void set_radius(float p_radius);
	float get_radius() const { return radius; }
	void set_tile_size(float p_size);
	float get_tile_size() const { return tile_size; }
	void set_fallback_wind(const Vector2 &p_wind);
	Vector2 get_fallback_wind() const { return fallback_wind; }
	void set_wind_response(float p_response);
	float get_wind_response() const { return wind_response; }

	void set_blade_segments(int p_segments);
	int get_blade_segments() const { return blade_segments; }
	void set_view_facing(float p_facing);
	float get_view_facing() const { return view_facing; }
	void set_ground_tint(const Color &p_color);
	Color get_ground_tint() const { return ground_tint; }
	void set_ground_tint_enabled(bool p_enabled);
	bool is_ground_tint_enabled() const { return ground_tint_enabled; }
	static String get_ground_shader_code();

	void set_trample_enabled(bool p_enabled);
	bool is_trample_enabled() const { return trample_enabled; }
	void set_trample_group(const StringName &p_group) { trample_group = p_group; }
	StringName get_trample_group() const { return trample_group; }
	void set_trample_size(float p_size);
	float get_trample_size() const { return trample_size; }
	void set_trample_resolution(int p_resolution);
	int get_trample_resolution() const { return trample_resolution; }
	void set_trample_radius(float p_radius) { trample_radius = MAX(p_radius, 0.01f); }
	float get_trample_radius() const { return trample_radius; }
	void set_trample_recovery(float p_seconds) { trample_recovery = MAX(p_seconds, 0.0f); }
	float get_trample_recovery() const { return trample_recovery; }

	void add_trample(const Vector3 &p_position, float p_radius, const Vector3 &p_velocity = Vector3(), float p_strength = 1.0f);
	Vector2 get_trample_at(const Vector3 &p_position) const;
	void clear_trample();

	void rebuild();
	void update_at(const Vector3 &p_center);
	int get_blade_count() const { return blade_count; }
	int get_tile_count() const { return tiles.size(); }
	double get_build_time_msec() const { return build_msec; }
	Ref<ShaderMaterial> get_material() const { return material; }

	~GrassField3D();
};

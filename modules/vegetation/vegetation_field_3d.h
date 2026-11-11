/**************************************************************************/
/*  vegetation_field_3d.h                                                 */
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

#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "scene/3d/node_3d.h"

class MultiMeshInstance3D;
class TerrainDataMap3D;

// Places a Biome over a TerrainDataMap3D, Horizon-style (docs/ideas/vegetation-placement.md):
// per tile, species in priority order, a tiled Poisson-disk pattern (Bridson 2007) dithered
// by rank against density x rules, each plant keeping later species out of its footprint.
// One MultiMeshInstance3D per species mesh per tile. Deterministic for a seed.
class VegetationField3D : public Node3D {
	GDCLASS(VegetationField3D, Node3D);

	struct Pattern {
		LocalVector<Vector2> points; // In [0, tile_size).
		LocalVector<float> ranks; // In [0, 1), a random permutation.
		LocalVector<float> randoms; // Per point, for yaw, scale and variant.
	};

	struct Placed {
		Vector2 xz;
		float footprint = 0.0f;
		int species = 0;
	};
	// A placed crown that shades the ground under it.
	struct Crown {
		Vector2 xz;
		float radius = 0.0f;
		float opacity = 0.0f;
		float top = 0.0f; // Crown top above the ground, metres.
		float conifer = 0.0f; // 1 for a needle-leaved crown.
	};
	NodePath weather_map; // A WeatherMap3D that gets the placed canopy as its canopy_map.
	void _publish_canopy(const Rect2 &p_rect);
	// Canopy closure and conifer share for the ground's litter (forest_floor_* globals).
	bool forest_floor_enabled = true;
	Ref<Texture2D> floor_texture;
	void _publish_floor(const Rect2 &p_rect);
	// Placed plants as ground colour beyond their range (vegetation_far_* globals).
	struct FarCell {
		float r = 0.0f, g = 0.0f, b = 0.0f, cover = 0.0f, range = 0.0f;
	};
	LocalVector<FarCell> far_cells;
	Vector2i far_size;
	Ref<Texture2D> far_texture;
	void _publish_far(const Rect2 &p_rect);
	static constexpr float CROWN_CELL = 8.0f;
	LocalVector<Crown> crowns;
	HashMap<Vector2i, LocalVector<int>> crown_grid;
	float max_crown_radius = 0.0f;
	float _shade_at(const Vector2 &p_xz, float *r_conifer = nullptr) const;

	Ref<Biome> biome;
	NodePath data_map;
	float tile_size = 32.0f;
	int seed = 0;
	bool populate_on_ready = true;

	HashMap<uint64_t, Pattern> patterns;
	LocalVector<MultiMeshInstance3D *> generated;
	LocalVector<LocalVector<Vector3>> placements; // Per species, world positions.
	int plant_count = 0;
	double populate_msec = 0.0;
	bool populate_queued = false;

	const Pattern &_get_pattern(float p_spacing, int p_species);
	void _clear_generated();
	void _on_map_baked();
	void _queued_populate();

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	static String get_floor_shader_code();
	void set_forest_floor_enabled(bool p_enabled);
	bool is_forest_floor_enabled() const { return forest_floor_enabled; }
	void set_biome(const Ref<Biome> &p_biome);
	Ref<Biome> get_biome() const { return biome; }
	void set_data_map(const NodePath &p_path);
	NodePath get_data_map() const { return data_map; }
	void set_weather_map(const NodePath &p_path) { weather_map = p_path; }
	NodePath get_weather_map() const { return weather_map; }
	void set_tile_size(float p_size);
	float get_tile_size() const { return tile_size; }
	void set_seed(int p_seed);
	int get_seed() const { return seed; }
	void set_populate_on_ready(bool p_enable);
	bool is_populate_on_ready() const { return populate_on_ready; }

	Error populate();
	void queue_populate();
	void clear();
	int get_plant_count() const { return plant_count; }
	double get_populate_time_msec() const { return populate_msec; }
	PackedVector3Array get_placements(int p_species) const;
	// Canopy shade at a point (0 open sky, 1 dense crown), from the canopy species placed so far.
	float get_shade_at(const Vector3 &p_global_position) const;
	float get_conifer_at(const Vector3 &p_global_position) const;
};

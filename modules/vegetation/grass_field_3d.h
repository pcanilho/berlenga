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

#include "core/templates/hash_map.h"
#include "scene/3d/node_3d.h"
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
	Ref<PlantSpecies> rules;
	float density = 60.0f; // Blades per square metre, near the camera.
	Vector2 blade_height = Vector2(0.25f, 0.6f);
	float blade_width = 0.04f;
	float clump_size = 1.2f;
	float lean = 0.35f;
	Color base_color = Color(0.1f, 0.2f, 0.05f);
	Color tip_color = Color(0.45f, 0.6f, 0.22f);
	Color dry_color = Color(0.55f, 0.48f, 0.28f);
	float dryness = 0.2f;
	float radius = 40.0f;
	float tile_size = 8.0f;
	Vector2 fallback_wind = Vector2(2.0f, 0.5f);
	float wind_response = 0.12f;
	int max_tiles_per_frame = 12;

	HashMap<Vector2i, Tile> tiles;
	Ref<ArrayMesh> blade_mesh;
	Ref<ShaderMaterial> material;
	int blade_count = 0;
	double build_msec = 0.0;

	void _ensure_resources();
	void _build_material();
	int _lod_for(float p_distance) const;
	void _build_tile(const Vector2i &p_tile, int p_lod, TerrainDataMap3D *p_map);
	void _free_tile(Tile &p_tile);
	void _update_tiles(const Vector3 &p_center);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_data_map(const NodePath &p_path);
	NodePath get_data_map() const { return data_map; }
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
	void set_base_color(const Color &p_color);
	Color get_base_color() const { return base_color; }
	void set_tip_color(const Color &p_color);
	Color get_tip_color() const { return tip_color; }
	void set_dry_color(const Color &p_color);
	Color get_dry_color() const { return dry_color; }
	void set_dryness(float p_dryness);
	float get_dryness() const { return dryness; }
	void set_radius(float p_radius);
	float get_radius() const { return radius; }
	void set_tile_size(float p_size);
	float get_tile_size() const { return tile_size; }
	void set_fallback_wind(const Vector2 &p_wind);
	Vector2 get_fallback_wind() const { return fallback_wind; }
	void set_wind_response(float p_response);
	float get_wind_response() const { return wind_response; }

	void rebuild();
	void update_at(const Vector3 &p_center);
	int get_blade_count() const { return blade_count; }
	int get_tile_count() const { return tiles.size(); }
	double get_build_time_msec() const { return build_msec; }
	Ref<ShaderMaterial> get_material() const { return material; }

	~GrassField3D();
};

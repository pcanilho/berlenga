/**************************************************************************/
/*  terrain_deformer_3d.h                                                 */
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

#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "scene/3d/node_3d.h"

class CollisionShape3D;
class MeshInstance3D;
class HeightMapShape3D;

// A brush the deformer composites, read from any object with get_carve_brush() or get_carve_brushes(tile_size)
// (duck-typed, so PhysXRiver3D, TerrainBrush3D and scripts all qualify). Heights and mask are cell-centred samples
// over a world XZ rectangle of size * cell_size.
struct TerrainCarveBrush {
	enum Mode {
		MODE_LOWER, // min(ground, target), Unreal Landmass "Min"
		MODE_RAISE, // max(ground, target), "Max"
		MODE_FLATTEN, // target, "Alpha Blend"
		MODE_ADD, // ground + target, "Additive"
	};

	Rect2 rect; // world X (position.x) and Z (position.y)
	real_t cell_size = 1.0;
	Vector2i size; // samples along X and Z
	Vector<float> heights; // row-major, size.y rows of size.x
	Vector<float> mask; // 0..1, empty = all 1
	Mode mode = MODE_LOWER;

	static bool from_dictionary(const Dictionary &p_dict, TerrainCarveBrush &r_brush, String &r_error);
	// Bilinear sample at a world XZ point. False outside the grid or where a corner height is NaN.
	bool sample(real_t p_x, real_t p_z, float &r_height, float &r_mask) const;
	static float blend(Mode p_mode, float p_ground, float p_target, float p_mask);
};

class TerrainDeformer3D : public Node3D {
	GDCLASS(TerrainDeformer3D, Node3D);

public:
	enum Backend {
		BACKEND_NONE,
		BACKEND_TERRAIN3D,
		BACKEND_HEIGHT_MAP,
	};

private:
	NodePath terrain_path;
	NodePath ground_mesh_path;
	TypedArray<NodePath> brush_paths;
	bool auto_apply = true;
	real_t brush_tile_size = 64.0;

	// Base (uncarved) heights of every grid vertex a brush covers, keyed by pack_cell(). Saved with the scene, so
	// a terrain saved carved still knows its ground; a cell no brush covers any more gets its base height back.
	HashMap<int64_t, float> base_heights;
	real_t base_spacing = 0.0; // grid spacing the keys were made with; a different spacing drops them

	bool apply_queued = false;
	bool waiting_for_terrain = false;
	HashSet<ObjectID> connected_brushes;

public:
	// Backend state, valid during one apply / query. Shared with TerrainGenerator3D (C++ only).
	struct Grid {
		Backend backend = BACKEND_NONE;
		real_t spacing = 1.0; // world units between grid vertices (X and Z)
		Vector2 origin; // world XZ of cell (0, 0)
		// Terrain3D
		Object *terrain = nullptr;
		Object *data = nullptr;
		int region_size = 0;
		HashMap<Vector2i, Ref<Image>> region_maps;
		HashSet<Vector2i> touched_regions;
		// HeightMapShape3D
		Ref<HeightMapShape3D> shape;
		Vector<real_t> map;
		int width = 0;
		int depth = 0;
		real_t height_scale = 1.0;
		real_t height_offset = 0.0;
		bool map_changed = false;
	};

	// Opens Terrain3D (by name) or a CollisionShape3D's HeightMapShape3D as a grid of world heights.
	static bool open_grid(Node *p_terrain, Grid &r_grid);
	static bool grid_get(Grid &r_grid, const Vector2i &p_cell, float &r_height);
	static void grid_set(Grid &r_grid, const Vector2i &p_cell, float p_height);
	// Uploads touched Terrain3D regions (and rebuilds its collision), or stores the HeightMapShape3D's map and
	// regenerates p_ground_mesh from it.
	static void commit_grid(Grid &r_grid, MeshInstance3D *p_ground_mesh);
	static void rebuild_ground_mesh(const Grid &p_grid, MeshInstance3D *p_mesh);

private:
	static int64_t pack_cell(const Vector2i &p_cell) { return (int64_t(p_cell.x) << 32) | int64_t(uint32_t(p_cell.y)); }
	static Vector2i unpack_cell(int64_t p_key) { return Vector2i(int32_t(p_key >> 32), int32_t(uint32_t(p_key & 0xffffffff))); }

	bool _open_grid(Grid &r_grid) const;
	void _commit_grid(Grid &r_grid);

	Vector<Object *> _get_brush_objects() const;
	void _connect_brushes();
	void _on_brush_changed();
	void _on_terrain_ready();
	void _deferred_apply();

	void _set_base_keys(const PackedInt64Array &p_keys);
	PackedInt64Array _get_base_keys() const;
	void _set_base_values(const PackedFloat32Array &p_values);
	PackedFloat32Array _get_base_values() const;
	PackedInt64Array pending_keys;

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_terrain(const NodePath &p_path);
	NodePath get_terrain() const { return terrain_path; }
	void set_ground_mesh(const NodePath &p_path);
	NodePath get_ground_mesh() const { return ground_mesh_path; }
	void set_brushes(const TypedArray<NodePath> &p_brushes);
	TypedArray<NodePath> get_brushes() const { return brush_paths; }
	void set_auto_apply(bool p_enable) { auto_apply = p_enable; }
	bool is_auto_apply() const { return auto_apply; }
	void set_brush_tile_size(real_t p_size) { brush_tile_size = MAX(p_size, real_t(1.0)); }
	real_t get_brush_tile_size() const { return brush_tile_size; }
	void set_base_spacing(real_t p_spacing) { base_spacing = p_spacing; }
	real_t get_base_spacing() const { return base_spacing; }

	void add_brush(Node *p_brush);
	void remove_brush(Node *p_brush);

	Backend get_backend() const;
	// Composes every brush over the base heights and writes the result. Returns the number of grid vertices written.
	int apply();
	// Restores every carved vertex to its base height and forgets them.
	void clear();
	void queue_apply();

	// The ground without any brush (bilinear between grid vertices); NaN outside the terrain.
	real_t get_base_height(const Vector3 &p_global_position) const;
	// The same, in the form PhysXRiver3D's base_height_source calls.
	real_t base_height_at(const Vector2 &p_world_xz) const;
	// The ground as it is now.
	real_t get_height(const Vector3 &p_global_position) const;
	int get_carved_vertex_count() const { return base_heights.size(); }

	PackedStringArray get_configuration_warnings() const override;
};

VARIANT_ENUM_CAST(TerrainDeformer3D::Backend);

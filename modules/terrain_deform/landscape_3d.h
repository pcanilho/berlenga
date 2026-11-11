/**************************************************************************/
/*  landscape_3d.h                                                        */
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
#include "core/templates/local_vector.h"
#include "scene/3d/node_3d.h"
#include "scene/resources/material.h"

class Camera3D;

// A heightfield drawn as static chunk meshes with an LOD chain (docs/ideas/terrain.md, "Landscape3D: staged
// build"). Vertices never move in vertex(): an LOD switch swaps the chunk's mesh, so under path tracing each
// (chunk, level, edge mask) mesh gets one BLAS, built when first drawn and kept, and nothing is refit while the
// camera moves. Edges next to a coarser neighbour drop their odd vertices, so the ground is watertight with no
// skirts. Levels are picked by projected error with hysteresis, by distance to the chunk (not view direction),
// the same for raster and the tracer.
class Landscape3D : public Node3D {
	GDCLASS(Landscape3D, Node3D);

	struct Chunk {
		RID instance;
		float min_h = 0.0f;
		float max_h = 0.0f;
		float error[8] = {}; // largest height gap to level 0, per level, metres
		int lod = 0;
		int mask = 0;
		uint32_t mesh_key = UINT32_MAX;
	};
	struct MeshEntry {
		RID mesh;
		uint64_t last_used = 0;
		int triangles = 0;
		int users = 0;
	};

	NodePath source;
	Ref<Image> height_image;
	Vector2 origin;
	real_t spacing = 1.0;
	int chunk_quads = 64;
	int lod_levels = 5;
	real_t lod_error = 1.0;
	Ref<Material> material;
	bool cast_shadows = true;
	NodePath camera_path;

	// The grid: gw x gh heights from origin, spacing apart (chunks x chunk_quads + 1 a side).
	LocalVector<float> heights;
	int gw = 0;
	int gh = 0;
	int cw = 0; // chunks along x
	int ch = 0; // chunks along z
	int levels = 1; // lod_levels clamped to the chunk size
	LocalVector<Chunk> chunks;
	HashMap<uint32_t, MeshEntry> meshes;
	LocalVector<int> indices[8][16]; // per (level, edge mask)
	uint64_t frame = 0;
	Transform3D last_camera;
	Projection last_projection;
	bool lods_dirty = true;
	bool have_camera = false;

	uint64_t mesh_builds = 0;
	uint64_t lod_switches = 0;
	double last_build_msec = 0.0;
	double last_update_usec = 0.0;

	float _h(int p_x, int p_z) const;
	Vector3 _normal(int p_x, int p_z) const;
	const LocalVector<int> &_indices(int p_level, int p_mask);
	void _measure_chunk(int p_cx, int p_cz);
	RID _mesh(int p_chunk, int p_level, int p_mask);
	void _free_instances();
	void _create_instances();
	void _update_transforms();
	void _apply_material();
	void _evict(bool p_all);
	Camera3D *_camera() const;
	void _pick_lods(const Transform3D &p_camera, const Projection &p_projection, real_t p_viewport_height);
	void _set_chunk_mesh(int p_chunk, int p_level, int p_mask);
	void _measure_task(uint32_t p_index, void *p_userdata);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_source(const NodePath &p_path) { source = p_path; }
	NodePath get_source() const { return source; }
	void set_height_image(const Ref<Image> &p_image) { height_image = p_image; }
	Ref<Image> get_height_image() const { return height_image; }
	void set_origin(const Vector2 &p_origin) { origin = p_origin; }
	Vector2 get_origin() const { return origin; }
	void set_spacing(real_t p_spacing) { spacing = MAX(p_spacing, real_t(0.05)); }
	real_t get_spacing() const { return spacing; }
	void set_chunk_quads(int p_quads);
	int get_chunk_quads() const { return chunk_quads; }
	void set_lod_levels(int p_levels) { lod_levels = CLAMP(p_levels, 1, 8); }
	int get_lod_levels() const { return lod_levels; }
	void set_lod_error(real_t p_pixels);
	real_t get_lod_error() const { return lod_error; }
	void set_material(const Ref<Material> &p_material);
	Ref<Material> get_material() const { return material; }
	void set_cast_shadows(bool p_enable);
	bool is_casting_shadows() const { return cast_shadows; }
	void set_camera(const NodePath &p_path) { camera_path = p_path; }
	NodePath get_camera() const { return camera_path; }

	bool rebuild();
	void clear();
	void update_lods(const Transform3D &p_camera, const Projection &p_projection, real_t p_viewport_height);
	void update_lods_from_camera();

	Vector2i get_chunk_grid() const { return Vector2i(cw, ch); }
	int get_lod_count() const { return levels; }
	int get_chunk_lod(const Vector2i &p_chunk) const;
	int get_chunk_edge_mask(const Vector2i &p_chunk) const;
	real_t get_chunk_lod_error(const Vector2i &p_chunk, int p_level) const;
	PackedVector3Array get_chunk_triangles(const Vector2i &p_chunk) const;
	real_t get_height(const Vector3 &p_position) const;
	AABB get_aabb() const;
	Dictionary get_stats() const;

	Landscape3D();
	~Landscape3D();
};

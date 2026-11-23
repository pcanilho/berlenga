/**************************************************************************/
/*  landscape_3d.h                                                        */
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

#include "core/object/worker_thread_pool.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "modules/world/world_contracts.h"
#include "scene/3d/node_3d.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/material.h"
#include "scene/resources/physics_material.h"
#include "servers/rendering/rendering_server_types.h"

class Camera3D;
class TerrainGenerator3D;

// A heightfield drawn as static chunk meshes with an LOD chain (docs/ideas/terrain.md, "Landscape3D: staged
// build"). Vertices never move in vertex(): an LOD switch swaps the chunk's mesh, so under path tracing each
// (chunk, level, edge mask) mesh gets one BLAS, built when first drawn and kept, and nothing is refit while the
// camera moves. Edges next to a coarser neighbour drop their odd vertices, so the ground is watertight with no
// skirts. Levels are picked by projected error with hysteresis, by distance to the chunk (not view direction),
// the same for raster and the tracer.
//
// Streaming ("Landscape3D: streaming and off-thread builds"): from a TerrainGenerator3D the grid is first read at
// every 4th point only; chunks the camera needs finer than that are read in full before the first frame, the rest
// on worker threads, nearest and most needed first. Meshes are built on worker threads and a new set of levels is
// shown only when all its meshes are ready, so a frame never mixes levels that do not fit.
class Landscape3D : public Node3D, public WorldReliefProvider {
	GDCLASS(Landscape3D, Node3D);

	static constexpr int COARSE = 2; // level read first when streaming (every 4th point)

	struct Chunk {
		RID instance;
		float min_h = 0.0f;
		float max_h = 0.0f;
		float error[8] = {}; // largest height gap to level 0, per level, metres (an estimate while not fine)
		int lod = 0;
		int mask = 0;
		uint64_t mesh_key = UINT64_MAX;
		uint32_t gen = 0; // bumped when heights under the chunk's vertices or normals change
		bool fine = true; // all heights read (false: only every 4th point, the rest interpolated)
		bool loading = false;
		RID body; // static heightfield near the collision target, at full detail
		RID shape;
	};
	struct MeshEntry {
		RID mesh;
		uint64_t last_used = 0;
		int triangles = 0;
		int users = 0;
	};
	struct HeightJob {
		int chunk = 0;
		LocalVector<float> h; // (chunk_quads + 1)^2
		float min_h = 0.0f;
		float max_h = 0.0f;
		float error[8] = {};
	};
	struct MeshJob {
		uint64_t key = 0;
		int chunk = 0;
		int level = 0;
		int mask = 0;
		RenderingServerTypes::SurfaceData surface;
		int triangles = 0;
	};

	NodePath source;
	Ref<Image> height_image;
	Vector2 origin;
	real_t spacing = 1.0;
	int chunk_quads = 64;
	int lod_levels = 5;
	real_t lod_error = 1.0;
	real_t cliff_relief = 0.6; // m, rock ledges built into faces steeper than about 50 degrees at the finest level (0 = off)
	real_t bank_relief = 0.8; // m, slump scarps and lobes on banks of about 40 to 55 degrees at the finest level (0 = off)
	Ref<Material> material;
	bool cast_shadows = true;
	NodePath camera_path;
	bool stream_heights = true;
	bool collision_enabled = true;
	real_t collision_radius = 192.0;
	NodePath collision_target;
	uint32_t collision_layer = 1;
	uint32_t collision_mask = 1;
	Ref<PhysicsMaterial> physics_material;
	int collision_tiles = 0;
	double last_collision_usec = 0.0;

	// The grid: gw x gh heights from origin, spacing apart (chunks x chunk_quads + 1 a side).
	LocalVector<float> heights;
	int gw = 0;
	int gh = 0;
	int vw = 0; // valid points from the source; the grid repeats the last row and column past them
	int vh = 0;
	int cw = 0; // chunks along x
	int ch = 0; // chunks along z
	int levels = 1; // lod_levels clamped to the chunk size
	LocalVector<Chunk> chunks;
	HashMap<uint64_t, MeshEntry> meshes;
	// Holes from the source TerrainGenerator3D (cave mouths): per chunk holding any, one flag per quad
	// (chunk_quads^2, tested at its centre). Triangles whose centroid is in a hole quad are left out; such chunks keep the
	// finest level.
	HashMap<int, LocalVector<uint8_t>> chunk_holes;
	ObjectID hole_source;
	void _refresh_holes(const Rect2 &p_rect);
	void _on_holes_changed(const Rect2 &p_rect);
	bool _hole_tri(const LocalVector<uint8_t> *p_holes, int p_level, int p_a, int p_b, int p_c) const;
	LocalVector<int> indices[8][16]; // per (level, edge mask)
	uint64_t frame = 0;
	Transform3D last_camera;
	Projection last_projection;
	real_t last_viewport_height = 0.0;
	bool lods_dirty = true;
	bool have_camera = false;

	// The stand's ground map (WorldStand worn trails and boar rooting) for TerrainMaterial3D: globals
	// terrain_stand_map (RG8: r trail wear, g fresh rooting) and terrain_stand_rect, a STAND_SIZE square round the camera.
	static constexpr int STAND_RES = 512;
	static constexpr real_t STAND_SIZE = 256.0;
	Ref<ImageTexture> stand_texture;
	Vector2 stand_origin = Vector2(1e30, 1e30);
	uint64_t stand_generation = UINT64_MAX;
	ObjectID stand_id;
	bool stand_published = false;
	void _update_stand_map(const Vector3 &p_camera);
	// The relief contract (WorldReliefProvider, WorldGround.relief_at) and its GPU map for placers: terrain_relief_map
	// (RG16F: r the drawn height over the heightfield in m, g the relief's share) over terrain_relief_rect, a
	// RELIEF_SIZE square round the camera at RELIEF_RES texels.
	static constexpr int RELIEF_RES = 256;
	static constexpr real_t RELIEF_SIZE = 128.0;
	Ref<ImageTexture> relief_texture;
	Vector2 relief_origin = Vector2(1e30, 1e30);
	uint64_t relief_key = 0;
	void _relief_changed(const Rect2 &p_area); // WorldGround.relief_changed over it (world space)
	bool relief_published = false;
	bool relief_registered = false;
	void _update_relief_map(const Vector3 &p_camera);
	void _clear_relief_map();
	Vector2 _relief_at_local(const Vector3 &p_local) const;
	// The cliff relief's share (its face weight, 0..1) over a wider square for the terrain material (no soil on a
	// shelf between walls): terrain_cliff_map (R8) over terrain_cliff_rect, CLIFF_MAP_SIZE round the camera at
	// CLIFF_MAP_RES texels, rebuilt with the relief map.
	static constexpr int CLIFF_MAP_RES = 256;
	static constexpr real_t CLIFF_MAP_SIZE = 512.0;
	Ref<ImageTexture> cliff_map_texture;
	bool cliff_map_published = false;
	void _update_cliff_map(const Vector2 &p_center);
	void _clear_cliff_map();
	void _clear_stand_map();
	// cliff_relief and the grid for meshes drawn over the landscape with the terrain material that carry no relief
	// data of their own (GroundSurface3D's patch): global terrain_cliff_relief = (metres, grid spacing, grid origin),
	// read in world space as the stand map is (the landscape is not moved).
	Vector4 cliff_published;
	real_t bank_published = -1.0;
	void _publish_cliff_relief(bool p_clear = false);

	// Streaming.
	ObjectID stream_source; // the TerrainGenerator3D read from worker threads
	float detail_estimate = 0.0f; // added to the errors of chunks not read in full
	int coarse_chunks = 0; // chunks not read in full
	LocalVector<uint8_t> need; // per chunk: 3 needed before the first frame, 2 needed by the view, 1 not read, 0 read
	LocalVector<HeightJob> height_jobs;
	WorkerThreadPool::GroupID height_group = -1;
	LocalVector<MeshJob> mesh_jobs;
	WorkerThreadPool::GroupID mesh_group = -1;
	LocalVector<int> target_lod; // levels and masks shown when mesh_jobs finish
	LocalVector<int> target_mask;

	uint64_t mesh_builds = 0;
	uint64_t lod_switches = 0;
	uint64_t chunks_streamed = 0;
	double last_build_msec = 0.0;
	double last_source_msec = 0.0; // heights from the source
	double last_measure_msec = 0.0; // level errors
	double last_update_usec = 0.0; // main thread time of the last level pick
	double last_apply_usec = 0.0; // main thread time to hand built meshes to the renderer

	float _h(int p_x, int p_z) const;
	Vector3 _normal(int p_x, int p_z) const;
	Vector3 _normal_at(real_t p_x, real_t p_z) const; // bilinear, in grid units
	float _cliff_offset(const Vector3 &p_world, real_t p_steep, real_t p_relief) const; // p_steep: relief scale times the face's tan(slope)
	float _bank_offset(const Vector3 &p_world) const;
	// The face weights for a heightfield normal: x the cliff's (cliff_relief), y the bank's (bank_relief), each 0..1.
	static Vector2 _relief_weights(const Vector3 &p_normal);
	real_t _relief_fold_scale(real_t p_x, real_t p_z) const; // grid units
	real_t _cliff_scale(const Vector3 &p, real_t p_k, real_t p_tan) const;
	Vector3 _cliff_point(const Vector3 &p_world, real_t p_weight_scale) const;
	void _build_cliff_surface(MeshJob &r_job) const;
	const LocalVector<int> &_indices(int p_level, int p_mask);
	void _crease_indices(int p_cx, int p_cz, int p_level, int p_mask, LocalVector<int> &r_indices) const;
	void _measure(const float *p_h, int p_stride, float *r_error, float &r_min, float &r_max) const;
	void _measure_chunk(int p_cx, int p_cz);
	uint64_t _mesh_key(int p_chunk, int p_level, int p_mask) const;
	void _build_surface(MeshJob &r_job) const;
	void _free_instances();
	void _create_instances();
	void _update_transforms();
	void _apply_material();
	void _evict(bool p_all);
	Camera3D *_camera() const;
	bool _pick_lods(const Transform3D &p_camera, const Projection &p_projection, real_t p_viewport_height);
	void _set_chunk_mesh(int p_chunk, int p_level, int p_mask);
	void _measure_task(uint32_t p_index, void *p_userdata);
	void _height_task(uint32_t p_index, void *p_userdata);
	void _mesh_task(uint32_t p_index, void *p_userdata);
	void _coarse_task(uint32_t p_row, void *p_userdata);
	void _fill_task(uint32_t p_row, void *p_userdata);
	const TerrainGenerator3D *_stream_generator() const;
	void _start_heights(const LocalVector<int> &p_chunks, bool p_wait);
	void _read_chunks(const LocalVector<int> &p_chunks); // in full, now, on worker threads
	void _launch_heights();
	void _finish_heights(bool p_wait);
	void _launch_meshes(bool p_wait);
	void _finish_meshes(bool p_wait);
	void _wait_all();
	void _on_source_changing();
	Node3D *_collision_node() const;
	void _update_collision();
	void _collision_tile(int p_chunk);
	void _free_collision(int p_chunk);
	void _free_all_collision();
	void _update_lods(const Transform3D &p_camera, const Projection &p_projection, real_t p_viewport_height, int p_read);

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
	void set_cliff_relief(real_t p_metres);
	real_t get_cliff_relief() const { return cliff_relief; }
	void set_bank_relief(real_t p_metres);
	real_t get_bank_relief() const { return bank_relief; }
	Vector2 contract_relief_at(const Vector3 &p_position) const override;
	Vector3 get_cliff_point(const Vector3 &p_global_position) const;
	void set_material(const Ref<Material> &p_material);
	Ref<Material> get_material() const { return material; }
	void set_cast_shadows(bool p_enable);
	bool is_casting_shadows() const { return cast_shadows; }
	void set_camera(const NodePath &p_path) { camera_path = p_path; }
	NodePath get_camera() const { return camera_path; }
	void set_collision_enabled(bool p_enable);
	bool is_collision_enabled() const { return collision_enabled; }
	void set_collision_radius(real_t p_radius) { collision_radius = MAX(p_radius, real_t(0.0)); }
	real_t get_collision_radius() const { return collision_radius; }
	void set_collision_target(const NodePath &p_path) { collision_target = p_path; }
	NodePath get_collision_target() const { return collision_target; }
	void set_collision_layer(uint32_t p_layer);
	uint32_t get_collision_layer() const { return collision_layer; }
	void set_collision_mask(uint32_t p_mask);
	uint32_t get_collision_mask() const { return collision_mask; }
	void set_physics_material(const Ref<PhysicsMaterial> &p_material);
	Ref<PhysicsMaterial> get_physics_material() const { return physics_material; }
	void update_collision() { _update_collision(); }
	bool has_chunk_collision(const Vector2i &p_chunk) const;
	void set_stream_heights(bool p_enable) { stream_heights = p_enable; }
	bool is_streaming_heights() const { return stream_heights; }

	bool rebuild();
	void clear();
	void update_lods(const Transform3D &p_camera, const Projection &p_projection, real_t p_viewport_height);
	void update_lods_from_camera();
	void load_all_heights();
	bool is_fully_loaded() const { return coarse_chunks == 0; }

	Vector2i get_chunk_grid() const { return Vector2i(cw, ch); }
	int get_lod_count() const { return levels; }
	int get_hole_chunk_count() const { return chunk_holes.size(); }
	int get_hole_quad_count() const;
	int get_chunk_lod(const Vector2i &p_chunk) const;
	int get_chunk_edge_mask(const Vector2i &p_chunk) const;
	real_t get_chunk_lod_error(const Vector2i &p_chunk, int p_level) const;
	bool is_chunk_loaded(const Vector2i &p_chunk) const;
	PackedVector3Array get_chunk_triangles(const Vector2i &p_chunk) const;
	real_t get_height(const Vector3 &p_position) const;
	AABB get_aabb() const;
	Dictionary get_stats() const;

	Landscape3D();
	~Landscape3D();
};

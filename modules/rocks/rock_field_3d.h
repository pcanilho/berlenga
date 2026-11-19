/**************************************************************************/
/*  rock_field_3d.h                                                       */
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

#pragma once

#include "rock_generator.h"
#include "rock_moss.h"

#include "core/math/random_pcg.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/mutex.h"
#include "core/templates/hash_set.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "scene/3d/node_3d.h"
#include "scene/resources/multimesh.h"
#include "scene/resources/image_texture.h"

class TerrainGenerator3D;
class MultiMeshInstance3D;
class StaticBody3D;

// Rocks and stones over a TerrainGenerator3D's ground (docs/ideas/rocks.md), placed by the processes that put them
// there and shaped by RockGenerator for the rock type of each place:
// - rockfall: the generator's own fallen blocks (get_boulders: shadow-angle run-out, fall sorting);
// - scree: stones packed on the talus aprons by sequential deposition (Visscher and Bolsterli 1972), coarser at the toe;
// - glacial: boulders in the moraine till and the odd erratic of another rock;
// - river: cobbles in the channels, the bed's size set by what the flow can move (Shields, threshold channel; Parker
//   1978) and by downstream fining (Sternberg 1875), rounded by the distance from the head, imbricated upstream;
// - shore: shingle and boulders on lake and sea shores, the size from the beach slope (Bujan et al. 2019) and
//   larger under rock;
// - cliff: jointed blocks set into the steep rock in courses along the beds (Paris et al. 2020's tiled blocks).
// The rock type comes from rock_type, with soft_rock_type on the soft strata bands and erratic_rock_type for erratics;
// river stones carry the rock of their headwaters. Each layer streams in square cells round the follow target (or
// the camera) out to its radius; instances share one mesh per shape (MultiMesh), so one acceleration structure per
// shape serves every stone of it.
class RockField3D : public Node3D {
	GDCLASS(RockField3D, Node3D);

public:
	enum Layer {
		LAYER_ROCKFALL,
		LAYER_SCREE,
		LAYER_GLACIAL,
		LAYER_RIVER,
		LAYER_SHORE,
		LAYER_CLIFF,
		LAYER_TOR,
		LAYER_COAST,
		LAYER_LANDFORM, // hoodoos, limestone pavement (docs/ideas/rocks.md, landforms)
		// Ground detail (docs/ideas/ground-detail.md): the stones under min_size near the camera, as real instanced geometry
		// in three size classes, each drawn out to where it is still a few pixels tall. Off unless ground_detail is set.
		LAYER_DETAIL_PEBBLE, // 4 cm to min_size (to the scree's own smallest on talus)
		LAYER_DETAIL_GRAVEL, // 2 to 4 cm
		LAYER_DETAIL_FINE, // 1 to 2 cm
		LAYER_MAX,
	};

	struct Stone {
		Vector3 position; // instance origin
		Basis basis; // rotation and uniform scale
		float scale = 1.0f; // metres per unit (half the b axis)
		float ground = 0.0f; // ground height under it
		float film = 0.0f; // water film 0..1
		float footprint = 0.0f; // radius where it meets the ground
		uint32_t library = 0; // library key
		uint16_t variant = 0;
		uint16_t layer = 0;
	};

private:
	struct River {
		LocalVector<Vector3> points;
		LocalVector<float> widths, distance, discharge;
		RockGenerator::RockType head_type = RockGenerator::ROCK_GRANITE;
		Rect2 bounds;
	};
	struct RiverSeg {
		Vector2 a, f, side; // start (world xz), downstream and left unit vectors
		float len = 0.0f, w = 0.0f, d50 = 0.0f, s0 = 0.0f, phase = 0.0f;
		RockGenerator::RockType type = RockGenerator::ROCK_GRANITE;
	};
	struct RiverBed {
		float band = 0.0f; // 0..1 inside the bed
		float d50 = 0.0f; // the local median, m
		float silt = 0.0f; // 0..1 the fines draped over the bed in a pool
		Vector2 f, side;
		RockGenerator::RockType type = RockGenerator::ROCK_GRANITE;
	};

	struct Library {
		LocalVector<Ref<ArrayMesh>> meshes;
		LocalVector<Ref<ArrayMesh>> close; // close-up versions, built on demand near the target
		LocalVector<uint8_t> close_state; // 0 none, 1 building, 2 ready
		LocalVector<uint64_t> close_used; // usec the close-up mesh was last wanted
		LocalVector<Vector3> axes;
		LocalVector<float> bottom;
		LocalVector<PackedVector3Array> hulls;
		LocalVector<PackedFloat32Array> potholes; // a pothole slab's floors (x, y, z, r in its frame), for grinders
		LocalVector<Transform3D> caves; // a cave mouth's passage box (the unit cube through it, in its frame)
		// Bedrock forms (coast, landforms) collide by their own triangles: one shape per variant, cooked once and shared
		// by every instance (a fin is 30 to 150 thousand triangles).
		LocalVector<Ref<Shape3D>> trimesh;
		// Surface samples (one per 0.06 units of the mesh) with their normals, for stone caps.
		LocalVector<LocalVector<Vector3>> cap_points, cap_normals;
	};

	// get_obstacles per placement cell: (x, ground y, z, footprint) and the stone's size, kept until a rebuild.
	struct ObstacleCell {
		LocalVector<Vector4> obstacles;
		LocalVector<float> sizes;
	};
	HashMap<Vector3i, ObstacleCell> obstacle_cache;

	struct Group {
		uint32_t library = 0;
		int variant = 0;
		Ref<MultiMesh> multimesh;
		LocalVector<Transform3D> xforms;
		LocalVector<Color> custom;
		LocalVector<real_t> reach; // m: how far out the stone's surface reaches from its origin
		LocalVector<bool> near; // drawn by the close-up instance instead
		LocalVector<bool> broken; // replaced by a breakable copy (break_stone)
		LocalVector<Node *> shapes; // its CollisionShape3D, or null
	};

	struct Cell {
		LocalVector<MultiMeshInstance3D *> instances;
		LocalVector<Group> groups;
		StaticBody3D *body = nullptr;
		int stones = 0;
	};

	NodePath generator_path;
	NodePath follow_target_path;
	NodePath terrain3d_path; // optional Terrain3D (by name, never bundled): cave holes are written into its control map
	void _terrain3d_hole(const Transform3D &p_box);
	int seed = 0;
	RockGenerator::RockType rock_type = RockGenerator::ROCK_GRANITE;
	RockGenerator::RockType soft_rock_type = RockGenerator::ROCK_LIMESTONE;
	RockGenerator::RockType erratic_rock_type = RockGenerator::ROCK_GRANITE;
	bool use_strata = true;
	int variants = 6;
	real_t detail = 1.0;
	real_t lichen_cover = 0.5;
	real_t moss_cover = 0.4;
	// Ground blending: the stone's foot eased into the ground under it (docs/ideas/rocks.md).
	bool ground_blend = true;
	// Coast forms (stacks, arches, platforms) get collision at any distance, so the sea's floor probe sees them.
	bool coast_collision = true;
	bool cave_mouths = false; // portal blocks set into limestone cliff feet, their passages opened as terrain holes; off until the entrance is meshed with the cave behind it
	bool mineral_glints = true;
	bool coast_seabed_dirty = false;
	void _remeasure_sea();
	real_t ground_blend_height = 0.45;
	Ref<ShaderMaterial> ground_material;
	mutable Ref<ShaderMaterial> auto_ground; // the landscape's TerrainMaterial3D when ground_material is unset
	mutable bool auto_ground_found = false;
	Ref<ShaderMaterial> _ground_source() const;
	void _apply_ground(const Ref<ShaderMaterial> &p_material) const;
	bool layer_enabled[LAYER_MAX] = { true, true, true, true, true, true, true, true, true, true, true, true };
	real_t layer_density[LAYER_MAX] = { 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0 };
	// The detail layers' radii are set from the camera (_update_detail_radii) while ground_detail is on.
	real_t layer_radius[LAYER_MAX] = { 600.0, 60.0, 300.0, 120.0, 100.0, 500.0, 900.0, 900.0, 900.0, 20.0, 10.0, 5.0 };
	// Ground detail (docs/ideas/ground-detail.md): opt-in, an experiment.
	bool ground_detail = true;
	real_t ground_detail_pixels = 3.0; // a class is drawn out to where its mean stone is this many pixels tall
	int ground_detail_reference_height = 0; // px; 0: the viewport's own height
	real_t ground_detail_max_radius = 40.0; // m
	int ground_detail_max_per_cell = 40000;
	static bool _is_detail(int p_layer) { return p_layer >= LAYER_DETAIL_PEBBLE && p_layer <= LAYER_DETAIL_FINE; }
	bool _layer_on(int p_layer) const { return layer_enabled[p_layer] && (ground_detail || !_is_detail(p_layer)); }
	void _detail_sizes(Layer p_layer, real_t p_upper, float &r_d1, float &r_d2) const;
	void _update_detail_radii();
	Vector3 _detail_center() const;
	// The far hand-over (docs/ideas/ground-detail.md, "Seamless far edge"): each detail class's cover of the ground in
	// two maps round the camera (1 m and 8 m texels), handed to the ground material (TerrainMaterial3D
	// set_ground_cover), which draws the grains' aggregate look past where each class's real stones stop.
	static constexpr int COVER_N = 256;
	struct CoverJob {
		const RockField3D *field = nullptr;
		const TerrainGenerator3D *gen = nullptr;
		int level = 0;
		Vector2 origin; // world xz of texel (0, 0)'s corner
		float texel = 1.0f;
		float sizes[3][2] = {}; // each class's shore sizes (m)
		float scree_sizes[3][2] = {}; // and talus sizes (m)
		float scree_dmin = 0.15f;
		bool shore = true;
		bool scree = true;
		bool river = true;
		LocalVector<RiverSeg> river_segs; // the river beds under the map
		PackedByteArray data; // RGBA8: pebble, gravel and fine cover (0..0.5 as 0..255), rock type (x 32)
		WorkerThreadPool::TaskID task = WorkerThreadPool::INVALID_TASK_ID;
	};
	bool ground_cover = true;
	bool detail_rivers = true; // ground detail on the river beds
	Ref<ImageTexture> cover_tex[2];
	Vector2 cover_origin[2];
	bool cover_valid[2] = {};
	CoverJob *cover_job = nullptr;
	static void _cover_task(void *p_job);
	static void _cover_fill(CoverJob *p_job);
	void _update_cover(bool p_wait);
	void _cover_finish(bool p_wait);
	void _push_cover();
	void _clear_cover();
	struct DetailSeed {
		Vector2 xz;
		float centre;
		float r;
	};
	void _detail_seeds(Layer p_layer, const Rect2 &p_rect, LocalVector<DetailSeed> &r_seeds) const;
	// Ground detail cells are placed on worker threads (docs/ideas/ground-detail.md, "Stage 2"): the main thread
	// snapshots what the placement reads (the shape libraries and the coarser stones the grains rest against), a
	// worker places the grains and packs each shape's MultiMesh records, and the main thread adds the result.
	struct DetailGroup {
		uint32_t library = 0;
		int variant = 0;
		Vector<float> buffer; // MultiMesh records: 3x4 transform, custom data
		LocalVector<Transform3D> xforms;
		LocalVector<Color> custom;
		LocalVector<real_t> reach;
	};
	struct DetailJob {
		RockField3D *field = nullptr;
		Vector3i key;
		TerrainGenerator3D *gen = nullptr;
		HashMap<uint32_t, Library *> libs;
		LocalVector<DetailSeed> seeds;
		LocalVector<DetailGroup> groups;
		int stones = 0;
		WorkerThreadPool::TaskID task = WorkerThreadPool::INVALID_TASK_ID;
	};
	HashMap<Vector3i, DetailJob *> detail_jobs;
	bool detail_threads = true;
	void _detail_prepare(DetailJob &r_job);
	void _detail_compute(DetailJob &r_job);
	static void _detail_job(void *p_job);
	void _detail_commit(DetailJob &r_job);
	void _detail_finish(bool p_wait, real_t p_budget_msec);
	void _detail_cancel();
	// The coarse layers' cells are placed on worker threads too while streaming (cell_threads): a worker places the
	// cell's stones and packs each shape's MultiMesh records, reading the shape libraries under library_lock; a shape
	// not built yet is noted (missing) and the job is dropped, to be placed again once the main thread has built it.
	// The main thread takes out stones on holes and broken ones, adds the MultiMeshes and the collision.
	struct CoarseGroup : DetailGroup {
		LocalVector<uint32_t> members; // stone indices
	};
	struct CoarseJob {
		RockField3D *field = nullptr;
		Vector3i key;
		TerrainGenerator3D *gen = nullptr;
		LocalVector<Stone> stones;
		LocalVector<CoarseGroup> groups;
		LocalVector<uint32_t> missing; // shape libraries the placement asked for that are not built
		WorkerThreadPool::TaskID task = WorkerThreadPool::INVALID_TASK_ID;
	};
	HashMap<Vector3i, CoarseJob *> coarse_jobs;
	bool cell_threads = true;
	Mutex library_lock; // libraries: workers look shapes up, the main thread adds them
	bool _coarse_place(CoarseJob &r_job);
	void _coarse_pack(const Vector3i &p_key, const LocalVector<Stone> &p_stones, LocalVector<CoarseGroup> &r_groups);
	static void _coarse_job(void *p_job);
	void _coarse_commit(const Vector3i &p_key, LocalVector<Stone> &p_stones, LocalVector<CoarseGroup> &p_groups);
	void _coarse_finish(bool p_wait, real_t p_budget_msec);
	void _coarse_cancel();
	struct LibraryBuild;
	HashMap<uint32_t, LibraryBuild *> library_builds; // started by streaming cells, keyed by every key they build
	bool library_async = false; // while streaming: a missing library is started on workers, the cell waits
	bool library_pending = false; // the cell being placed asked for a library not built yet
	LibraryBuild *_start_libraries(const LocalVector<uint32_t> &p_missing);
	void _finish_libraries(LibraryBuild *p_build);
	void _collect_libraries(bool p_wait);
	Color _stone_custom(const TerrainGenerator3D *p_gen, const Stone &p_stone, const Vector3 &p_axes, RandomPCG &r_rng) const;
	MultiMeshInstance3D *_add_group_instance(const Ref<MultiMesh> &p_multimesh, uint32_t p_library, real_t p_visibility, real_t p_margin, bool p_detail = false);
	real_t min_size = 0.08; // m, b axis: smaller grains stay in the ground material
	real_t river_min_area = 2.0e5;
	int max_stones_per_cell = 20000;
	bool collision_enabled = true;
	real_t collision_min_size = 0.35; // m, b axis
	real_t collision_radius = 80.0;
	uint32_t collision_layer = 1;
	real_t build_budget_msec = 4.0;
	bool close_up_enabled = true;
	real_t close_up_radius = 8.0; // m from the target to a stone's surface
	int close_up_max_shapes = 12;
	bool streaming = true;

	ObjectID generator_id;
	LocalVector<River> rivers;
	bool rivers_ready = false;
	HashMap<uint32_t, Library> libraries;
	HashMap<int, Ref<ShaderMaterial>> materials; // per rock type, with this field's lichen and moss
	HashMap<Vector3i, Cell> cells; // (layer, cell x, cell z)
	LocalVector<Rect2> hole_rects; // holes opened while placing (cave mouths), whose built cells are built again
	ObjectID holes_watched; // the generator whose holes_changed rebuilds cells (holes others open: caves)
	int building = 0;
	void _watch_holes();
	void _on_holes_changed(const Rect2 &p_rect);
	Vector3 last_target;
	bool have_target = false;
	double last_build_msec = 0.0;
	double last_detail_msec = 0.0; // the main thread's share of the ground detail in the last streaming update
	double last_library_msec = 0.0;
	int stone_count = 0;
	HashMap<uint64_t, MultiMeshInstance3D *> close_instances; // (library << 16 | variant)
	Vector3 close_target;
	bool close_dirty = true;
	// Breaking (break_stone): each shape's fracture into rough pieces, built on a worker as stones come near (or at
	// the first hit), as a PhysXBlastAsset; the broken stones by position, so they stay broken when cells rebuild.
	struct Fracture {
		Ref<Resource> asset;
		float volume = 0.0f; // the shape's volume in its own units
		uint8_t state = 0; // 0 none, 1 building, 2 ready, 3 failed
		void *job = nullptr;
		WorkerThreadPool::TaskID task = 0;
	};
	HashMap<uint64_t, Fracture> fractures; // (library << 16 | variant)
	HashSet<Vector3i> broken_stones; // cm
	LocalVector<ObjectID> breakables;
	int break_pieces = 10;
	real_t break_prepare_radius = 6.0;
	real_t break_density = 2650.0;
	void _start_fracture(uint64_t p_key);
	bool _finish_fracture(uint64_t p_key, bool p_wait);
	void _update_breakable();
	Node3D *_spawn_breakable(Group &p_grp, int p_k, uint64_t p_key, bool p_fall);

	// Live rockfall: cliff blocks come loose by a Poisson process whose rate follows the weather (frost, thaw, rain).
	struct PendingFall {
		Vector3 origin; // the block, found again by its position when its fracture is ready
		uint64_t key = 0;
	};
	bool live_rockfall = true;
	// The ground side of the contact (docs/ideas/rocks.md, "Stone and ground contact"): a global mask around the
	// target the terrain material reads, R the damp ring round each stone's foot, G fines banked on its uphill side.
	bool ground_contact = true;
	Ref<ImageTexture> contact_texture;
	Vector2 contact_origin = Vector2(1e30, 1e30);
	bool contact_dirty = true;
	uint64_t contact_usec = 0;
	// The stamping runs on a worker (main thread: deciding, the obstacle list and the upload).
	struct ContactJob {
		Vector2 origin;
		LocalVector<Vector4> obstacles; // x, z, footprint radius, slope
		LocalVector<Vector2> uphill;
		LocalVector<float> damp_share; // 0 on dry talus (it drains: no damp ring), 1 elsewhere
		Vector<uint8_t> data;
		uint64_t start = 0;
		uint64_t end = 0;
	};
	ContactJob *contact_job = nullptr;
	WorkerThreadPool::TaskID contact_task = WorkerThreadPool::INVALID_TASK_ID;
	static void _contact_job(void *p_job);
	void _finish_contact(bool p_wait);
	void _sync_scree_blocks() const;
	void _update_contact(bool p_force);
	void _clear_contact();
	real_t rockfall_rate = 4.0; // falls per game hour in rockfall_radius
	real_t rockfall_radius = 150.0;
	int rockfall_max = 16;
	LocalVector<PendingFall> pending_falls;
	LocalVector<ObjectID> falls;
	double thaw_left = 0.0; // game seconds of thaw left
	real_t last_air = 1e9;
	RandomPCG fall_rng;
	real_t _rockfall_air(const Vector3 &p_pos) const;
	bool _find_stone(const Vector3 &p_origin, Cell *&r_cell, int &r_group, int &r_k);
	bool _pick_fall(const Vector3 &p_center, real_t p_min, real_t p_max, Vector3 &r_origin, uint64_t &r_key);
	void _update_rockfall(double p_delta);
	void _update_process();

	struct CloseJob {
		uint32_t library = 0;
		int variant = 0;
		WorkerThreadPool::TaskID task = WorkerThreadPool::INVALID_TASK_ID;
		void *job = nullptr;
	};
	LocalVector<CloseJob> close_jobs;

	// Moss as geometry on the close-up stones (rock_moss.h): one shell per stone, built on a worker.
	bool moss_cushions = true;
	struct MossShell {
		Node3D *node = nullptr;
		RockMoss::Job *job = nullptr;
		WorkerThreadPool::TaskID task = WorkerThreadPool::INVALID_TASK_ID;
		bool wanted = true;
		int triangles = 0;
	};
	HashMap<Vector3i, MossShell> moss_shells; // by the stone's origin in cm
	HashMap<uint64_t, RockMoss::Source> moss_sources; // the close-up shapes' surfaces (library << 16 | slot)
	void _update_moss(const HashMap<uint64_t, LocalVector<Transform3D>> &p_near_xf, const HashMap<uint64_t, LocalVector<Color>> &p_near_cd, bool p_wait);
	void _finish_moss(bool p_wait);
	void _clear_moss();

	TerrainGenerator3D *_get_generator() const;
	Vector3 _target_position() const;
	real_t _cell_size(Layer p_layer) const;
	int _variant_count(uint32_t p_key) const;
	uint32_t _library_key(RockGenerator::RockType p_type, RockGenerator::Family p_family, int p_bin) const;
	Library *_ensure_library(uint32_t p_key);
	void _ensure_libraries(const LocalVector<uint32_t> &p_keys);
	void _prepare_rivers();
	// Ground detail on river beds (docs/ideas/ground-detail.md, "River and stream beds"): the river layer's segments
	// near a rectangle, each with its bed's median size, and the bed one gives at a point (its pools and riffles).
	void _river_segments(const Rect2 &p_rect, LocalVector<RiverSeg> &r_segs) const;
	static bool _river_bed(const RiverSeg &p_seg, const Vector2 &p_xz, RiverBed &r_bed, float p_texel = 0.0f);
	RockGenerator::RockType _type_at(const TerrainGenerator3D *p_gen, const Vector3 &p_pos) const;

public:
	RockGenerator::RockType get_rock_type_at(const Vector3 &p_position) const { return _type_at(_get_generator(), p_position); }

	// Rockfall dust (docs/ideas/rocks.md, "Rockfall dust with PhysX Flow"): fines thrown up where falling pieces hit
	// (kinetic energy lost times rockfall_dust_per_joule), shed by new fracture faces, and abraded by pieces sliding
	// or rolling on the ground. PhysXRockDust3D takes them into Flow.
	struct Dust {
		Vector3 position; // the piece's underside
		Vector3 velocity; // the piece's motion before the hit (m/s)
		float mass = 0.0f; // kg of fines
		float radius = 0.3f; // the piece's size (m)
	};

private:
	bool rockfall_dust = true;
	real_t dust_per_joule = 2e-5; // kg of airborne fines per joule an impact takes out of a piece
	struct PieceTrack {
		Vector3 velocity;
		uint64_t tick = 0;
		int seen = 0;
	};
	struct DustTrack {
		HashMap<uint32_t, PieceTrack> pieces; // by chunk
		int ticks = 0;
	};
	HashMap<uint64_t, DustTrack> dust_tracks; // by the breakable's ObjectID
	LocalVector<Dust> dust_events;
	struct SnowDust {
		Vector3 at; // mass-weighted sum of positions
		float mass = 0.0f; // kg
		float radius = 0.0f;
	};
	HashMap<Vector2i, SnowDust> snow_dust; // fines settling near their hit, to the snow contract once a second
	float snow_dust_timer = 0.0f;
	real_t dust_total = 0.0; // kg, since the field entered the tree
	int dust_impacts = 0;
	uint64_t dust_tick = 0;
	ObjectID dust_node;
	void _update_dust(double p_delta);
	void _add_dust(const Vector3 &p_position, const Vector3 &p_velocity, float p_mass, float p_radius);
	void _update_dust_node();

private:
	// p_libs and p_seeds: the ground detail's snapshot (DetailJob) when placing off the main thread.
	void _place(Layer p_layer, const Rect2 &p_rect, uint32_t p_seed, LocalVector<Stone> &r_stones, const HashMap<uint32_t, Library *> *p_libs = nullptr, const LocalVector<DetailSeed> *p_seeds = nullptr);
	void _rest(const TerrainGenerator3D *p_gen, Stone &r_stone, const Library &p_lib, const Vector3 &p_xz, real_t p_yaw, real_t p_sink, const Vector3 &p_tilt_axis, real_t p_tilt, real_t p_follow_ground) const;
	Ref<ShaderMaterial> _material(RockGenerator::RockType p_type, RockGenerator::Family p_family = RockGenerator::FAMILY_ROCKFALL, bool p_detail = false);
	void _build_cell(const Vector3i &p_key);
	void _free_cell(Cell &r_cell);
	void _update_streaming(bool p_all);
	void _update_close_up(bool p_wait);
	void _clear_close_up();

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_generator(const NodePath &p_path);
	NodePath get_generator() const { return generator_path; }
	void set_follow_target(const NodePath &p_path) { follow_target_path = p_path; }
	NodePath get_follow_target() const { return follow_target_path; }
	void set_terrain3d(const NodePath &p_path) { terrain3d_path = p_path; }
	NodePath get_terrain3d() const { return terrain3d_path; }
	int write_terrain3d_holes();
	void set_seed(int p_seed);
	int get_seed() const { return seed; }
	void set_rock_type(RockGenerator::RockType p_type);
	RockGenerator::RockType get_rock_type() const { return rock_type; }
	void set_soft_rock_type(RockGenerator::RockType p_type);
	RockGenerator::RockType get_soft_rock_type() const { return soft_rock_type; }
	void set_erratic_rock_type(RockGenerator::RockType p_type);
	RockGenerator::RockType get_erratic_rock_type() const { return erratic_rock_type; }
	void set_use_strata(bool p_use);
	bool get_use_strata() const { return use_strata; }
	void set_variants(int p_variants);
	int get_variants() const { return variants; }
	void set_detail(real_t p_detail);
	real_t get_detail() const { return detail; }
	void set_lichen_cover(real_t p_cover);
	real_t get_lichen_cover() const { return lichen_cover; }
	void set_moss_cover(real_t p_cover);
	real_t get_moss_cover() const { return moss_cover; }
	void set_mineral_glints(bool p_enabled);
	bool is_mineral_glints() const { return mineral_glints; }
	void set_coast_collision(bool p_enabled);
	bool is_coast_collision() const { return coast_collision; }
	void set_cave_mouths(bool p_enabled);
	bool is_cave_mouths() const { return cave_mouths; }
	void set_ground_blend(bool p_enabled);
	void set_ground_contact(bool p_enabled);
	void set_ground_detail(bool p_enabled);
	bool is_ground_detail() const { return ground_detail; }
	void set_ground_detail_pixels(real_t p_pixels);
	real_t get_ground_detail_pixels() const { return ground_detail_pixels; }
	void set_ground_detail_reference_height(int p_height);
	int get_ground_detail_reference_height() const { return ground_detail_reference_height; }
	void set_ground_detail_max_radius(real_t p_radius);
	real_t get_ground_detail_max_radius() const { return ground_detail_max_radius; }
	void set_ground_detail_max_per_cell(int p_count) { ground_detail_max_per_cell = MAX(p_count, 1); }
	int get_ground_detail_max_per_cell() const { return ground_detail_max_per_cell; }
	// Stones the detail layers hold now, and the instance count per layer (for benches).
	int get_ground_detail_count(int p_layer = -1) const;
	void set_moss_cushions(bool p_enabled);
	bool is_moss_cushions_enabled() const { return moss_cushions; }
	int get_moss_shell_count() const;
	int get_moss_triangle_count() const;
	int get_moss_pending() const;
	bool is_ground_contact_enabled() const { return ground_contact; }
	// The contact map's side (m) and resolution (texels a side).
	static constexpr real_t CONTACT_SIZE = 64.0;
	static constexpr int CONTACT_RES = 1024;
	uint64_t get_last_contact_usec() const { return contact_usec; }
	Ref<ImageTexture> get_contact_texture() const { return contact_texture; }
	bool is_ground_blend() const { return ground_blend; }
	void set_ground_blend_height(real_t p_height);
	real_t get_ground_blend_height() const { return ground_blend_height; }
	void set_ground_material(const Ref<ShaderMaterial> &p_material);
	Ref<ShaderMaterial> get_ground_material() const { return ground_material; }
	void set_layer_enabled(int p_layer, bool p_enabled);
	bool is_layer_enabled(int p_layer) const;
	void set_layer_density(int p_layer, real_t p_density);
	real_t get_layer_density(int p_layer) const;
	void set_layer_radius(int p_layer, real_t p_radius);
	real_t get_layer_radius(int p_layer) const;
	void set_min_size(real_t p_size);
	real_t get_min_size() const { return min_size; }
	void set_river_min_area(real_t p_area);
	real_t get_river_min_area() const { return river_min_area; }
	void set_max_stones_per_cell(int p_count) { max_stones_per_cell = MAX(p_count, 1); }
	int get_max_stones_per_cell() const { return max_stones_per_cell; }
	void set_collision_enabled(bool p_enabled);
	bool is_collision_enabled() const { return collision_enabled; }
	void set_collision_min_size(real_t p_size) { collision_min_size = MAX(p_size, real_t(0.0)); }
	real_t get_collision_min_size() const { return collision_min_size; }
	void set_collision_radius(real_t p_radius) { collision_radius = MAX(p_radius, real_t(0.0)); }
	real_t get_collision_radius() const { return collision_radius; }
	void set_collision_layer(uint32_t p_layer) { collision_layer = p_layer; }
	uint32_t get_collision_layer() const { return collision_layer; }
	void set_build_budget_msec(real_t p_msec) { build_budget_msec = MAX(p_msec, real_t(0.1)); }
	real_t get_build_budget_msec() const { return build_budget_msec; }
	void set_cell_threads(bool p_enabled);
	bool is_cell_threads() const { return cell_threads; }
	void set_ground_detail_rivers(bool p_enabled);
	bool is_ground_detail_rivers() const { return detail_rivers; }
	void set_ground_detail_cover(bool p_enabled);
	bool is_ground_detail_cover() const { return ground_cover; }
	Ref<Texture2D> get_ground_detail_cover_texture(int p_level) const;
	void set_ground_detail_threads(bool p_enabled);
	bool is_ground_detail_threads() const { return detail_threads; }
	int get_ground_detail_pending() const { return int(detail_jobs.size()); }
	int get_cells_pending() const { return int(coarse_jobs.size() + library_builds.size()); }
	double get_last_detail_msec() const { return last_detail_msec; }
	void set_close_up_enabled(bool p_enabled);
	bool is_close_up_enabled() const { return close_up_enabled; }
	void set_close_up_radius(real_t p_radius) { close_up_radius = MAX(p_radius, real_t(0.0)); close_dirty = true; }
	real_t get_close_up_radius() const { return close_up_radius; }
	void set_close_up_max_shapes(int p_count) { close_up_max_shapes = MAX(p_count, 1); }
	int get_close_up_max_shapes() const { return close_up_max_shapes; }
	int get_close_up_shape_count() const;
	int get_close_up_triangle_count() const;
	void set_streaming(bool p_streaming);
	bool is_streaming() const { return streaming; }

	// Breaking stones (PhysX Blast; needs the godot_physx module). break_stone swaps the stone nearest p_position
	// (within its reach) for a PhysXDestructible3D of its pieces and applies p_damage there; returns that node.
	void set_break_pieces(int p_pieces) { break_pieces = CLAMP(p_pieces, 2, 48); fractures.clear(); }
	int get_break_pieces() const { return break_pieces; }
	void set_break_prepare_radius(real_t p_radius) { break_prepare_radius = MAX(p_radius, real_t(0.0)); }
	real_t get_break_prepare_radius() const { return break_prepare_radius; }
	void set_break_density(real_t p_density) { break_density = MAX(p_density, real_t(1.0)); }
	real_t get_break_density() const { return break_density; }
	Node *break_stone(const Vector3 &p_position, real_t p_damage = 10.0, real_t p_radius = 1.0);
	void restore_broken();
	int get_broken_count() const { return broken_stones.size(); }
	bool is_breaking_supported() const;

	// Live rockfall (PhysX Blast): cliff blocks near the follow target fall now and then, more in frost, thaw and rain.
	void set_live_rockfall(bool p_enabled);
	bool is_live_rockfall() const { return live_rockfall; }
	void set_live_rockfall_rate(real_t p_rate) { rockfall_rate = MAX(p_rate, real_t(0.0)); }
	real_t get_live_rockfall_rate() const { return rockfall_rate; }
	void set_live_rockfall_radius(real_t p_radius) { rockfall_radius = MAX(p_radius, real_t(1.0)); }
	real_t get_live_rockfall_radius() const { return rockfall_radius; }
	void set_live_rockfall_max(int p_count) { rockfall_max = MAX(p_count, 1); }
	int get_live_rockfall_max() const { return rockfall_max; }
	real_t get_rockfall_trigger() const;
	// Rockfall dust: on by default; the field makes its own PhysXRockDust3D when the engine has Flow.
	void set_rockfall_dust(bool p_enabled);
	bool is_rockfall_dust() const { return rockfall_dust; }
	void set_rockfall_dust_per_joule(real_t p_v) { dust_per_joule = MAX(p_v, real_t(0.0)); }
	real_t get_rockfall_dust_per_joule() const { return dust_per_joule; }
	real_t get_rockfall_dust_total() const { return dust_total; }
	int get_rockfall_dust_impacts() const { return dust_impacts; }
	Node *get_rockfall_dust_node() const;
	// The dust thrown up since the last call (PhysXRockDust3D drains it every physics tick).
	void take_rockfall_dust(LocalVector<Dust> &r_events);
	Node *trigger_rockfall(const Vector3 &p_position, real_t p_radius = 30.0);

	// Drop every built cell and the river cache (after the generator ran again); streaming builds them anew.
	void rebuild();
	// Build every cell of every enabled layer that touches p_rect now, whatever the target (benches, stills).
	int build_area(const Rect2 &p_rect);
	void clear();
	// Where stones of at least p_min_size (b axis, m) meet the ground in p_rect, as VegetationField3D obstacles:
	// (x, ground y, z, footprint radius). Computed from the placement rules, built or not.
	PackedVector4Array get_obstacles(const Rect2 &p_rect, real_t p_min_size = 0.25);
	// The stones of one layer in p_rect, each a Dictionary (position, basis, scale, footprint, film, rock_type,
	// family, variant): what the layer would place, for tests.
	Array get_stones(int p_layer, const Rect2 &p_rect);
	Array get_stone_caps(int p_layer, const Rect2 &p_rect, real_t p_tolerance = 0.03);
	int get_stone_count() const { return stone_count; }
	int get_cell_count() const { return cells.size(); }
	double get_last_build_msec() const { return last_build_msec; }
	double get_last_library_msec() const { return last_library_msec; }
	int get_library_triangle_count() const;
};

VARIANT_ENUM_CAST(RockField3D::Layer);

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

#include "core/object/worker_thread_pool.h"
#include "core/templates/hash_set.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "scene/3d/node_3d.h"
#include "scene/resources/multimesh.h"

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

	struct Library {
		LocalVector<Ref<ArrayMesh>> meshes;
		LocalVector<Ref<ArrayMesh>> close; // close-up versions, built on demand near the target
		LocalVector<uint8_t> close_state; // 0 none, 1 building, 2 ready
		LocalVector<uint64_t> close_used; // usec the close-up mesh was last wanted
		LocalVector<Vector3> axes;
		LocalVector<float> bottom;
		LocalVector<PackedVector3Array> hulls;
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
	bool ground_blend = false;
	// Coast forms (stacks, arches, platforms) get collision at any distance, so the sea's floor probe sees them.
	bool coast_collision = true;
	bool mineral_glints = false;
	bool coast_seabed_dirty = false;
	void _remeasure_sea();
	real_t ground_blend_height = 0.25;
	Ref<ShaderMaterial> ground_material;
	void _apply_ground(const Ref<ShaderMaterial> &p_material) const;
	bool layer_enabled[LAYER_MAX] = { true, true, true, true, true, true, true, true };
	real_t layer_density[LAYER_MAX] = { 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0 };
	real_t layer_radius[LAYER_MAX] = { 600.0, 60.0, 300.0, 120.0, 100.0, 500.0, 900.0, 900.0 };
	real_t min_size = 0.08; // m, b axis: smaller grains stay in the ground material
	real_t river_min_area = 2.0e5;
	int max_stones_per_cell = 12000;
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
	Vector3 last_target;
	bool have_target = false;
	double last_build_msec = 0.0;
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

	struct CloseJob {
		uint32_t library = 0;
		int variant = 0;
		WorkerThreadPool::TaskID task = WorkerThreadPool::INVALID_TASK_ID;
		void *job = nullptr;
	};
	LocalVector<CloseJob> close_jobs;

	TerrainGenerator3D *_get_generator() const;
	Vector3 _target_position() const;
	real_t _cell_size(Layer p_layer) const;
	uint32_t _library_key(RockGenerator::RockType p_type, RockGenerator::Family p_family, int p_bin) const;
	Library *_ensure_library(uint32_t p_key);
	void _ensure_libraries(const LocalVector<uint32_t> &p_keys);
	void _prepare_rivers();
	RockGenerator::RockType _type_at(const TerrainGenerator3D *p_gen, const Vector3 &p_pos) const;
	void _place(Layer p_layer, const Rect2 &p_rect, uint32_t p_seed, LocalVector<Stone> &r_stones);
	void _rest(const TerrainGenerator3D *p_gen, Stone &r_stone, const Library &p_lib, const Vector3 &p_xz, real_t p_yaw, real_t p_sink, const Vector3 &p_tilt_axis, real_t p_tilt, real_t p_follow_ground) const;
	Ref<ShaderMaterial> _material(RockGenerator::RockType p_type);
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
	void set_ground_blend(bool p_enabled);
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

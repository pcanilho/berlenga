/**************************************************************************/
/*  vegetation_field_3d.h                                                 */
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

#include "grass_field_3d.h"
#include "plant_species.h"
#include "vegetation_holes.h"
#include "vegetation_obstacles.h"
#include "vegetation_paths.h"

#include "core/object/worker_thread_pool.h"
#include "core/os/rw_lock.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/templates/safe_refcount.h"
#include "scene/3d/node_3d.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"

class MultiMesh;
class MultiMeshInstance3D;
class PlantSpecies;
class Mesh;
class Image;
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
		bool deadwood = false; // A log: plants on and beside it ignore its footprint.
		bool top = false; // On a stone's top: plants on the ground round the stone and on its top do not crowd each other.
	};
	// A placed log (a species marked "deadwood" whose meshes carry tree_deadwood_axis), in world space.
	struct Deadwood {
		Vector3 a, b;
		float radius = 0.0f;
		bool stone = false; // With pit: a stone a tree's roots grow over (tree_root_stone), kept clear like a pit, not counted as wood.
		bool pit = false; // An uprooted tree's root pit (mesh meta tree_root_pit): a disc of bare soil of this radius round a, not wood.
		int8_t feature = -1; // A place on a log's surface (tree_deadwood_features: 0 peel edge, 1 drying check): a the point, b - a its outward normal (1 cm); not wood.
	};
	// The log nearest a point, as seen from above.
	struct DeadwoodHit {
		float side = 1e9f; // Metres from the log's side across the ground; negative under it.
		float radius = 0.0f;
		float top = 0.0f; // Height of the log's upper surface over the point (under it).
		float axis_y = 0.0f; // Height of the log's axis at the nearest point along it.
		Vector3 normal = Vector3(0, 1, 0); // The surface's normal there.
		Vector2 across; // Unit direction from the axis to the point, across the ground.
	};
	static constexpr float DEADWOOD_CELL = 4.0f;
	static constexpr float DEADWOOD_BESIDE_GAP = 0.15f; // Plants beside a log only where its underside is within this of the ground.
	static constexpr float WOOD_FEEDER_HEIGHT = 0.2f; // How far up a log's side a wood feeder roots (at most its axis).
	static constexpr float WOOD_FEEDER_LEAN_DEG = 15.0f; // How far a wood feeder tips out of the bark.
	static constexpr float WOOD_FEEDER_REACH = 0.6f; // How far along a log a wood feeder looks for a crack or bark edge to root in.
	static constexpr float WOOD_FEEDER_INSET = 0.015f; // How far a wood feeder's foot sits into the log's side (its moss and bark).
	static constexpr float DEADWOOD_MOSS_LIFT = 0.022f; // Plants on a log stand on its moss (the trees' shell, 1.1 to 2.5 cm proud of the bark).
	static constexpr float DUNE_SAND_MIN = 0.3f; // Metres of dune sand (TerrainGenerator3D.get_dune_height()) that count as a dune for RULE_DUNE.
	static constexpr float ROOT_STONE_MARGIN = 0.04f; // Floor plants keep this far off a stone a tree's roots grow over.
	LocalVector<Deadwood> deadwood;
	HashMap<Vector2i, LocalVector<int>> deadwood_grid;
	float deadwood_max_radius = 0.0f;
	// The nearest log within reach of its side, this field's own (under crown_lock).
	bool _deadwood_at(const Vector2 &p_xz, float p_reach, DeadwoodHit &r_hit, float p_min_radius = 0.0f) const;
	// The nearest crack or bark edge on a log's side (tree_deadwood_features) to a point, within reach
	// across the ground, between p_low and p_high over the ground there; p_any: whether the logs there
	// carry features at all.
	bool _deadwood_feature_at(const Vector3 &p_pos, float p_reach, float p_low, float p_high, float p_ground, Vector3 &r_point, Vector3 &r_normal, bool &r_any) const;
	// Whether the point lies in a placed root pit, this field's own (under crown_lock).
	bool _pit_at(const Vector2 &p_xz) const;
	// A placed crown that shades the ground under it.
	struct Crown {
		Vector2 xz;
		float radius = 0.0f;
		float opacity = 0.0f;
		float top = 0.0f; // Crown top above the ground, metres.
		float conifer = 0.0f; // 1 for a needle-leaved crown.
		float stem = 0.0f; // The stem's foot at the soil line (flare and root ridges), metres from its centre.
	};
	NodePath weather_map; // A WeatherMap3D that gets the placed canopy as its canopy_map.
	Vector2 litter_wind; // Downwind lean of the leaf drift; zero: from the weather map's wind.
	Vector2 litter_skew; // The lean of the last placement.
	NodePath canopy_field; // Another VegetationField3D whose crowns also shade this one's plants.
	// Painted or generated density over a world rect: each species reads one channel (PlantSpecies.mask_channel).
	Ref<Texture2D> density_mask;
	Rect2 density_mask_rect;
	PackedVector4Array obstacles;
	VegetationObstacles obstacle_grid;
	Array paths; // Worn paths (VegetationPaths): Dictionaries of points, width, passes a year.
	VegetationPaths path_grid;
	PackedVector4Array stone_tops; // (x, top y, z, cap radius): where stone_top species grow.
	VegetationObstacles stone_top_grid;
	PackedVector3Array stone_top_normals; // Optional, one per stone_tops entry: the cap's surface normal.
	PackedVector3Array stone_top_grid_normals; // stone_top_normals matched to stone_top_grid.items.
	void _build_stone_tops();
	// Canopy closure and conifer share for the ground's litter (forest_floor_* globals).
	bool forest_floor_enabled = true;
	Ref<Texture2D> floor_texture;
	// Placed plants as ground colour beyond their range (vegetation_far_* globals).
	struct FarCell {
		float r = 0.0f, g = 0.0f, b = 0.0f, cover = 0.0f, range = 0.0f;
	};
	Ref<Texture2D> far_texture;
	static constexpr float CROWN_CELL = 8.0f;
	static constexpr float STEM_MARGIN = 0.03f; // Ground plants root this far clear of a stem's foot.
	LocalVector<Crown> crowns;
	// Shade weighed by stand closure and the conifer share, on half-metre cells (_build_stand()).
	LocalVector<float> stand_shade;
	LocalVector<float> stand_conifer;
	Vector2i stand_size;
	Rect2 stand_rect;
	HashMap<Vector2i, LocalVector<int>> crown_grid;
	float max_crown_radius = 0.0f;
	bool _in_stem(const Vector2 &p_xz, float p_margin) const;
	static float near_radius_published;
	void _publish_near_radius(float p_radius);
	static float _foot_radius(const Ref<Mesh> &p_mesh, float p_cap);
	float _shade_at(const Vector2 &p_xz, float *r_conifer = nullptr) const; // No lock: main thread, or under crown_lock.

	Ref<Biome> biome;
	NodePath data_map;
	float tile_size = 32.0f;
	int seed = 0;
	bool populate_on_ready = true;

	HashMap<uint64_t, Pattern> patterns;

	// Placement, split so tiles can be placed on worker threads (docs/ideas/vegetation-lod-streaming.md).
	struct PlaceSpecies {
		Ref<PlantSpecies> sp;
		LocalVector<Ref<Mesh>> meshes;
		LocalVector<Ref<Mesh>> near_meshes;
		Pattern pattern;
		float footprint = 0.0f;
		float canopy_top = 0.0f;
		float stem_foot = 0.0f; // Crowned species: the foot radius of its first mesh (other plants keep out of it).
		float spread = 0.0f; // Ground plants: the widest reach of its meshes' leaves from the stem (unit scale).
		float crown_max = 0.0f; // The largest crown radius it can place.
		float halo = 0.0f; // How far around a tile it must be placed for later species.
		Color far_color; // Linear; alpha: how much ground it hides far away.
		LocalVector<PackedFloat32Array> deadwood_axes; // Per variant: start, end, widest reach, then optionally the reach profile (mesh space); empty if not deadwood.
		LocalVector<PackedFloat32Array> deadwood_features; // Per variant: tree_deadwood_features (8 floats a record: kind, point, normal, half width; mesh space).
		LocalVector<PackedFloat32Array> deadwood_pits; // Per variant: the root pit (tree_root_pit: centre, radius, depth in mesh space) of an uprooted tree; empty if none.
		float deadwood_reach = 0.0f; // How far its logs reach from the plant's origin (times its largest scale); 0: not deadwood.
		LocalVector<PackedFloat32Array> root_stones; // Per variant: the stone its roots grow over (tree_root_stone: centre, half extents in mesh space); empty if none.
		float stone_reach = 0.0f; // How far a root stone reaches from the plant's origin (times its largest scale); 0: none.
		float frontal = 0.0f; // m2 a plant shows the wind at scale 1 (PlantSpecies.wind_frontal_area).
		bool wind_flag = false; // Meta "wind_flag": +X turned down the prevailing wind (flagged trees, krummholz).
		LocalVector<float> bank_slopes; // Per variant, mesh meta "tree_ground_slope".x (radians); empty: no bank forms.
	};
	struct PlaceSetup {
		LocalVector<PlaceSpecies> species;
		TerrainDataMap3D *map = nullptr;
		Rect2 rect;
		Ref<Image> mask_img;
		Rect2 mask_rect;
		const VegetationField3D *other = nullptr;
		Transform3D to_local;
		Transform3D global_xf;
		float tile_size = 32.0f;
		int seed = 0;
		float max_footprint = 0.0f;
		float crown_max = 0.0f;
		float grid_cell = 1.0f;
		float sun_mean = 0.0f; // VegetationLight::sun_mean().
		Vector2 litter_skew; // crown_litter()'s downwind lean.
		Vector2 wind_dir; // The prevailing wind's heading (unit, xz, towards where it blows), or zero.
		float litter_reach = 0.0f; // How far past a crown its litter can land.
		float deadwood_reach = 0.0f; // The widest a log reaches from its origin plus the widest deadwood band of a species.
		VegetationObstacles obstacles; // Rocks the plants keep out of (a copy, for the worker threads).
		VegetationPaths paths; // Worn paths that thin and shorten the plants (a copy).
		GrassField3D::CropFieldMask crop_fields; // Crop fields nothing else grows in (a copy).
		VegetationObstacles stone_tops; // Caps of stones that stone_top species grow on (a copy).
		PackedVector3Array stone_top_normals; // Their surface normals, matched to stone_tops.items (empty: domes).
		VegetationHoles holes; // Openings in the ground (cave mouths): no plant there (a copy).
		const TerrainGenerator3D *dunes = nullptr; // The map's geology source when it has coastal dunes (PlantSpecies.RULE_DUNE); read only.
		const TerrainGenerator3D *coast = nullptr; // The map's geology source when it has a sea (PlantSpecies.RULE_TIDE_HEIGHT and the other coast rules); read only.
		float sea_y = 0.0f; // Its sea's surface, world y.
	};
	struct FarAdd {
		Vector2 xz;
		Color color; // Linear; alpha: the species' far cover.
		float area = 0.0f; // Ground hidden, times the far cover.
		float range = 0.0f;
	};
	struct TileOut {
		struct Species {
			LocalVector<LocalVector<Transform3D>> xforms; // Per variant, field-local.
			LocalVector<LocalVector<Color>> customs;
			LocalVector<Vector3> positions;
		};
		Vector2i tile;
		LocalVector<Species> species;
		LocalVector<Crown> crowns;
		LocalVector<FarAdd> far;
		LocalVector<Deadwood> deadwood;
		LocalVector<Vector3> shelter; // Per plant that shelters sand: global x, z, and its frontal area (m2).
		int count = 0;
	};
	void _publish_deadwood(const LocalVector<const LocalVector<Deadwood> *> &p_lists);
	bool _make_setup(PlaceSetup &r_setup);
	void _place(const PlaceSetup &p_setup, const Rect2i &p_tiles, bool p_halo, LocalVector<TileOut> &r_tiles) const;
	void _commit_species(const PlaceSetup &p_setup, const TileOut &p_tile, int p_species, LocalVector<MultiMeshInstance3D *> &r_nodes);
	// The maps from the placed crowns and far colour: computed anywhere, applied on the main thread.
	struct Maps {
		Rect2 maps_rect; // Canopy, stand shade, forest floor.
		Rect2 far_rect;
		bool want_canopy = false;
		bool want_floor = false;
		bool want_far = true;
		LocalVector<Crown> crowns;
		HashMap<Vector2i, LocalVector<int>> grid;
		float max_radius = 0.0f;
		Ref<Image> canopy;
		LocalVector<float> stand_shade;
		LocalVector<float> stand_conifer;
		Vector2i stand_size;
		Ref<Image> floor;
		Ref<Image> far;
		float far_range = 0.0f;
	};
	void _maps_wants(Maps &r_maps) const;
	static void _compute_maps(Maps &r_maps, const LocalVector<const LocalVector<Crown> *> &p_crowns, const LocalVector<const LocalVector<FarAdd> *> &p_far);
	void _apply_maps(Maps &p_maps);
	// The plant cover seen from far above for distant terrain (terrain_cover_* globals,
	// TerrainMaterial3D's far blend), from the species' rules over the whole map.
	bool cover_map_enabled = false;
	NodePath cover_grass;
	TypedArray<NodePath> cover_grass_layers; // More GrassField3D under cover_grass, top down (an undercoat).
	float cover_cell = 4.0f;
	// Plants past their range as real volume (docs/ideas/vegetation.md, "Far plants as volume"):
	// a voxel shell per tile and species, from the range out to far_volume_range.
	bool far_volume = true;
	float far_volume_range = 150.0f;
	float far_volume_cell = 0.3f;
	Ref<StandardMaterial3D> far_volume_material;
	Ref<ArrayMesh> _build_far_volume(const PlaceSetup &p_setup, const TileOut &p_tile, int p_species) const;
	Ref<Texture2D> cover_texture;
	struct CoverJob {
		const PlaceSetup *setup = nullptr;
		// The grass layers, top down (cover_grass, then cover_grass_layers): rules and far colour (linear).
		LocalVector<Ref<PlantSpecies>> grass;
		LocalVector<Color> grass_far;
		Rect2 rect;
		int w = 0;
		int h = 0;
		uint8_t *out = nullptr;
		// Per species, the crowns' mean shade over its own pattern at a kept share of 0, 0.1 ... 1,
		// as -ln(1 - shade) so species add up.
		LocalVector<LocalVector<float>> crown_k;
		// Pass 0 writes the crowns per texel (k, conifer k, colour times k), blurred by the crown
		// size between the passes; pass 1 adds the plants and grass under them.
		int pass = 0;
		LocalVector<float> crowns; // 5 per texel.
		float plant_shade = 1.0f; // The far match's shade (far_match_view) on the plants' colour, as vegetation_far() gives it.
	};
	float cover_shade = 1.0f; // The far shade the published cover map was built with.
	static void _cover_row(void *p_job, uint32_t p_row);
	void _build_cover(const PlaceSetup &p_setup);
	void _on_grass_far_color();
	mutable RWLock crown_lock; // Crowns and stand: another field reads them from its workers.

	// Streaming around a centre: tiles placed on WorkerThreadPool nearest first, committed on the
	// main thread within a time budget, freed past the radius.
	bool streaming = false;
	NodePath stream_target;
	float stream_radius = 200.0f;
	float stream_budget_msec = 2.0f;
	float stream_maps_radius = 96.0f;
	struct StreamTile {
		TileOut out;
		LocalVector<LocalVector<MultiMeshInstance3D *>> nodes; // Per species.
		LocalVector<bool> committed;
	};
	struct StreamJob {
		VegetationField3D *field = nullptr;
		Vector2i tile;
		LocalVector<TileOut> out;
		WorkerThreadPool::TaskID task = WorkerThreadPool::INVALID_TASK_ID;
		SafeFlag done;
	};
	HashMap<Vector2i, StreamTile *> stream_tiles;
	LocalVector<StreamJob *> stream_jobs;
	PlaceSetup stream_setup;
	bool stream_setup_valid = false;
	ObjectID hole_source; // The generator whose holes_changed this field follows.
	Rect2 holes_dirty; // Ground opened or closed since the streamed tiles were placed (no area: none).
	void _follow_holes(TerrainDataMap3D *p_map);
	void _on_holes_changed(const Rect2 &p_rect);
	void _on_crop_fields_changed();
	bool stream_maps_dirty = false;
	double stream_maps_wait = 0.0;
	Vector2 stream_maps_centre = Vector2(1e30f, 1e30f);
	bool stream_idle = false;
	double stream_frame_max_msec = 0.0; // The slowest frame of _stream_update(), for the bench.
	double stream_publish_msec = 0.0;
	struct StreamMapsJob {
		VegetationField3D *field = nullptr;
		Maps maps;
		LocalVector<const LocalVector<Crown> *> crowns;
		LocalVector<const LocalVector<FarAdd> *> far;
		WorkerThreadPool::TaskID task = WorkerThreadPool::INVALID_TASK_ID;
		SafeFlag done;
	};
	StreamMapsJob *stream_maps_job = nullptr;
	LocalVector<StreamTile *> stream_graveyard; // Freed tiles the maps job may still read.
	static void _stream_job(void *p_job);
	static void _stream_maps_job(void *p_job);
	void _stream_maps_finish();
	void _stream_update(double p_delta);
	void _stream_free_species(StreamTile *p_tile, int p_species);
	void _stream_clear();
	void _stream_publish(const Vector2 &p_centre);
	Vector3 _stream_centre() const;
	LocalVector<MultiMeshInstance3D *> generated;
	// Far cells drawing impostors (PlantGenerator.make_impostor): raster only, so under path
	// tracing they draw the plant's mesh instead (the impostor's "impostor_of").
	LocalVector<MultiMeshInstance3D *> impostor_cells;
	LocalVector<Ref<Mesh>> impostor_meshes; // Per cell.
	// Near meshes on demand (PlantSpecies.near_mesh_source): per far cell, by its instance id, the
	// plants it holds, so the near cell is built once the source has the mesh.
	struct LazyNear {
		MultiMeshInstance3D *far = nullptr;
		MultiMeshInstance3D *near = nullptr;
		ObjectID source;
		int key = 0;
		int variant = 0;
		float range = 0.0f;
		Vector3 centre; // Global, where the visibility range measures from.
		LocalVector<Transform3D> xforms;
		LocalVector<Color> customs;
	};
	HashMap<ObjectID, LazyNear> lazy_near;
	bool has_near_swap = false; // Some cells swap to near meshes (their margins follow path tracing).
	void _set_swap_margins(bool p_traced);
	void _lazy_near_update(const Vector3 &p_camera);
	void _lazy_near_drop(MultiMeshInstance3D *p_far);
	void _lazy_near_free(LazyNear &p_cell);
	static void _fill_cell(const Ref<MultiMesh> &p_mm, const Ref<Mesh> &p_mesh, const LocalVector<Transform3D> &p_xforms, const LocalVector<Color> &p_customs);
	bool impostors_traced = false;
	double impostor_check = 0.0;
	bool _path_traced() const;
	void _update_impostors(bool p_traced);
	LocalVector<LocalVector<Vector3>> placements; // Per species, world positions.
	// Live-view match of the far plant colour (far_match_view; vegetation_far_match.cpp): plants
	// within their range darken the ground they stand on (their shadows, their shaded sides), which
	// the far colour that replaces them past the range does not carry. Reading the frame back, it
	// compares ground under plants with bare ground at the same distance, near (plants drawn) and
	// far (far colour), and scales the far colour's share (vegetation_far_shade) until the two agree.
	bool far_match_view = true;
	float far_shade = 1.0f;
	Ref<Image> far_image; // The far map as published (sqrt of premultiplied colour, cover).
	Rect2 far_image_rect;
	float far_image_range = 0.0f;
	int far_view_state = 0; // 0 idle, 1 waiting, 2 readback in flight.
	int far_view_wait = 0;
	int far_view_round = 0;
	int far_view_misses = 0;
	Vector3 far_view_camera = Vector3(1e30f, 0, 0);
	Vector2 far_view_size;
	PackedVector2Array far_view_px; // Groups of four: near plants, near bare, far plants, far bare.
	float far_view_ratio = 1.0f;
	float far_view_step_shade = 1.0f; // The shade before the last step, and the contrast then: a step that moves nothing (a ground without vegetation_far()) is undone.
	float far_view_step_ratio = 1.0f;
	float far_view_gain = 1.0f; // How the contrast follows the shade (contrast ~ shade^-gain), measured step to step.
	int far_view_groups = 0;
	static float far_shade_published;
	void _far_view_start(int p_wait);
	void _far_view_step();
	void _far_view_issue(RID p_viewport_texture);
	void _far_view_on_data(const PackedByteArray &p_data, int p_width, int p_height, int p_format);
	void _far_view_apply(float p_ratio, int p_groups);
	void _publish_far_shade();
	void _far_cover_sync();
	bool _far_view_active() const { return far_match_view && far_image.is_valid() && far_image_range > 0.0f; }
	// The plants' frontal area per square metre (Raupach's lambda) on a 1 m grid, for sand
	// sheltered from the wind (GroundSurface3D.wind_shelters); rebuilt on demand once plants change.
	static constexpr float SHELTER_CELL = 1.0f;
	LocalVector<Vector3> shelter_static; // populate()'s plants (x, z, area).
	mutable HashMap<Vector2i, float> shelter_grid;
	mutable bool shelter_dirty = true;
	void _build_shelter() const;
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
	void set_density_mask(const Ref<Texture2D> &p_mask) { density_mask = p_mask; }
	Ref<Texture2D> get_density_mask() const { return density_mask; }
	void set_density_mask_rect(const Rect2 &p_rect) { density_mask_rect = p_rect; }
	Rect2 get_density_mask_rect() const { return density_mask_rect; }
	// Rocks on the ground, (x, ground y, z, radius at the soil): no plant grows inside one.
	void set_obstacles(const PackedVector4Array &p_obstacles);
	void set_paths(const Array &p_paths);
	Array get_paths() const { return paths; }
	float get_path_passes_at(const Vector3 &p_position) const { return path_grid.passes_at(Vector2(p_position.x, p_position.z)); }
	// The placed plants' frontal area index (m2 shown to the wind per m2 of ground) at a point, for
	// GroundSurface3D's sand shelter (its wind_shelters).
	float sample_lateral_cover(const Vector3 &p_position) const;
	PackedVector4Array get_obstacles() const { return obstacles; }
	void set_stone_tops(const PackedVector4Array &p_tops);
	PackedVector4Array get_stone_tops() const { return stone_tops; }
	void set_stone_top_normals(const PackedVector3Array &p_normals);
	PackedVector3Array get_stone_top_normals() const { return stone_top_normals; }
	const VegetationObstacles &get_obstacle_grid() const { return obstacle_grid; }
	// Obstacles keep plants this far past their edge: a ground plant's stem off the rock's foot.
	static constexpr float OBSTACLE_MARGIN = 0.03f;
	static constexpr float STONE_TOP_OVERHANG = 0.015f; // How far a plant on a stone top may reach past its cap.
	static constexpr float STONE_TOP_LOW = 0.05f; // A stone top this high over the ground is as damp as it ...
	static constexpr float STONE_TOP_HIGH = 0.8f; // ... and this high, as dry as the open air.
	static constexpr float STONE_TOP_SHELTER_REACH = 2.0f; // How far taller stones shelter a stone top.
	static constexpr float STONE_TOP_DAMP_SPLIT = 0.45f; // A stone top this wet or more is damp (stone_top_damp).
	static constexpr float STONE_TOP_SINK = 0.003f; // Metres a plant on a stone top sits into it (the cap is not a true dome).
	// How far a ground plant's leaves may reach in over a rock's foot; a plant reaching farther is
	// made smaller and leans away from the rock, down to ROCK_FIT_MIN of its size, or not placed.
	static constexpr float ROCK_OVERHANG = 0.08f;
	static constexpr float CREVICE_OVERHANG = 0.1f; // More for crevice plants (PlantSpecies.stone_gap 1), arching over the stones.
	static constexpr float ROCK_FIT_MIN = 0.45f;
	static constexpr float ROCK_LEAN_DEG = 25.0f;
	void set_canopy_field(const NodePath &p_path) { canopy_field = p_path; }
	NodePath get_canopy_field() const { return canopy_field; }
	void set_litter_wind(const Vector2 &p_wind) { litter_wind = p_wind; }
	Vector2 get_litter_wind() const { return litter_wind; }
	void set_weather_map(const NodePath &p_path) { weather_map = p_path; }
	NodePath get_weather_map() const { return weather_map; }
	void set_tile_size(float p_size);
	float get_tile_size() const { return tile_size; }
	void set_seed(int p_seed);
	int get_seed() const { return seed; }
	void set_populate_on_ready(bool p_enable);
	bool is_populate_on_ready() const { return populate_on_ready; }

	void set_cover_map_enabled(bool p_enable);
	bool is_cover_map_enabled() const { return cover_map_enabled; }
	void set_cover_grass(const NodePath &p_path);
	NodePath get_cover_grass() const { return cover_grass; }
	void set_cover_grass_layers(const TypedArray<NodePath> &p_paths);
	TypedArray<NodePath> get_cover_grass_layers() const { return cover_grass_layers; }
	void set_cover_cell(float p_cell) { cover_cell = CLAMP(p_cell, 0.5f, 64.0f); }
	void set_far_volume(bool p_on) { far_volume = p_on; }
	void set_far_match_view(bool p_enabled);
	bool is_far_match_view() const { return far_match_view; }
	float get_far_shade() const { return far_shade; }
	Dictionary get_far_match_stats() const;
	bool is_far_volume() const { return far_volume; }
	void set_far_volume_range(float p_range) { far_volume_range = MAX(p_range, 0.0f); }
	float get_far_volume_range() const { return far_volume_range; }
	void set_far_volume_cell(float p_cell) { far_volume_cell = CLAMP(p_cell, 0.05f, 4.0f); }
	float get_far_volume_cell() const { return far_volume_cell; }
	float get_cover_cell() const { return cover_cell; }
	Ref<Texture2D> get_cover_texture() const { return cover_texture; }

	void set_streaming(bool p_enable);
	bool is_streaming() const { return streaming; }
	void set_stream_target(const NodePath &p_path) { stream_target = p_path; }
	NodePath get_stream_target() const { return stream_target; }
	void set_stream_radius(float p_radius) { stream_radius = MAX(p_radius, 1.0f); }
	float get_stream_radius() const { return stream_radius; }
	void set_stream_budget_msec(float p_msec) { stream_budget_msec = MAX(p_msec, 0.1f); }
	float get_stream_budget_msec() const { return stream_budget_msec; }
	void set_stream_maps_radius(float p_radius) { stream_maps_radius = MAX(p_radius, 8.0f); }
	float get_stream_maps_radius() const { return stream_maps_radius; }
	int get_stream_tile_count() const { return stream_tiles.size(); }
	bool is_stream_idle() const { return stream_idle; }
	Dictionary get_stream_stats();

	Error populate();
	void queue_populate();
	void clear();
	int get_plant_count() const { return plant_count; }
	double get_populate_time_msec() const { return populate_msec; }
	PackedVector3Array get_placements(int p_species) const;
	int get_deadwood_count() const;
	Dictionary get_deadwood_at(const Vector3 &p_global_position, float p_reach = 0.0f, float p_min_radius = 0.0f) const;
	// Canopy shade at a point (0 open sky, 1 dense crown), from the canopy species placed so far.
	float get_shade_at(const Vector3 &p_global_position) const;
	float get_light_at(const Vector3 &p_global_position) const;
	float get_stand_shade_at(const Vector3 &p_global_position) const;
	float get_sward_shade_at(const Vector3 &p_global_position) const;
	// The undergrowth's own litter under its stands (PlantSpecies.litter_color): rgb colour, a cover.
	Color get_sward_litter_at(const Vector3 &p_global_position) const;
	// The published vegetation_near_radius: the smallest stream radius of the streaming fields (0: none).
	static float get_near_radius() { return near_radius_published; }
	// Both at once (the shade returned, the litter in r_litter when given).
	float sward_at(const Vector3 &p_global_position, Color *r_litter) const;
	float get_conifer_at(const Vector3 &p_global_position) const;
	// Leaf fall from the placed crowns, 0 to 1, with a drift past their edges (PlantSpecies.RULE_LITTER).
	static constexpr float LITTER_SPREAD = 6.0f; // Metres: the drift's e-folding distance past a crown's edge.
	float get_litter_at(const Vector3 &p_global_position) const;
};

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

#include "plant_species.h"

#include "core/templates/hash_map.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/rw_lock.h"
#include "core/templates/local_vector.h"
#include "core/templates/safe_refcount.h"
#include "scene/3d/node_3d.h"

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
	};
	// A placed log (a species marked "deadwood" whose meshes carry tree_deadwood_axis), in world space.
	struct Deadwood {
		Vector3 a, b;
		float radius = 0.0f;
	};
	// The log nearest a point, as seen from above.
	struct DeadwoodHit {
		float side = 1e9f; // Metres from the log's side across the ground; negative under it.
		float radius = 0.0f;
		float top = 0.0f; // Height of the log's upper surface over the point (under it).
		Vector3 normal = Vector3(0, 1, 0); // The surface's normal there.
		Vector2 across; // Unit direction from the axis to the point, across the ground.
	};
	static constexpr float DEADWOOD_CELL = 4.0f;
	static constexpr float DEADWOOD_MOSS_LIFT = 0.022f; // Plants on a log stand on its moss (the trees' shell, 1.1 to 2.5 cm proud of the bark).
	LocalVector<Deadwood> deadwood;
	HashMap<Vector2i, LocalVector<int>> deadwood_grid;
	float deadwood_max_radius = 0.0f;
	// The nearest log within reach of its side, this field's own (under crown_lock).
	bool _deadwood_at(const Vector2 &p_xz, float p_reach, DeadwoodHit &r_hit) const;
	// A placed crown that shades the ground under it.
	struct Crown {
		Vector2 xz;
		float radius = 0.0f;
		float opacity = 0.0f;
		float top = 0.0f; // Crown top above the ground, metres.
		float conifer = 0.0f; // 1 for a needle-leaved crown.
	};
	NodePath weather_map; // A WeatherMap3D that gets the placed canopy as its canopy_map.
	Vector2 litter_wind; // Downwind lean of the leaf drift; zero: from the weather map's wind.
	Vector2 litter_skew; // The lean of the last placement.
	NodePath canopy_field; // Another VegetationField3D whose crowns also shade this one's plants.
	// Painted or generated density over a world rect: each species reads one channel (PlantSpecies.mask_channel).
	Ref<Texture2D> density_mask;
	Rect2 density_mask_rect;
	// Canopy closure and conifer share for the ground's litter (forest_floor_* globals).
	bool forest_floor_enabled = true;
	Ref<Texture2D> floor_texture;
	// Placed plants as ground colour beyond their range (vegetation_far_* globals).
	struct FarCell {
		float r = 0.0f, g = 0.0f, b = 0.0f, cover = 0.0f, range = 0.0f;
	};
	Ref<Texture2D> far_texture;
	static constexpr float CROWN_CELL = 8.0f;
	LocalVector<Crown> crowns;
	// Shade weighed by stand closure and the conifer share, on half-metre cells (_build_stand()).
	LocalVector<float> stand_shade;
	LocalVector<float> stand_conifer;
	Vector2i stand_size;
	Rect2 stand_rect;
	HashMap<Vector2i, LocalVector<int>> crown_grid;
	float max_crown_radius = 0.0f;
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
		float crown_max = 0.0f; // The largest crown radius it can place.
		float halo = 0.0f; // How far around a tile it must be placed for later species.
		Color far_color; // Linear; alpha: how much ground it hides far away.
		LocalVector<PackedFloat32Array> deadwood_axes; // Per variant: start, end, radius (mesh space); empty if not deadwood.
		float deadwood_reach = 0.0f; // How far its logs reach from the plant's origin (times its largest scale); 0: not deadwood.
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
		float litter_reach = 0.0f; // How far past a crown its litter can land.
		float deadwood_reach = 0.0f; // The widest a log reaches from its origin plus the widest deadwood band of a species.
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
	};
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

	void set_cover_map_enabled(bool p_enable) { cover_map_enabled = p_enable; }
	bool is_cover_map_enabled() const { return cover_map_enabled; }
	void set_cover_grass(const NodePath &p_path) { cover_grass = p_path; }
	NodePath get_cover_grass() const { return cover_grass; }
	void set_cover_grass_layers(const TypedArray<NodePath> &p_paths) { cover_grass_layers = p_paths; }
	TypedArray<NodePath> get_cover_grass_layers() const { return cover_grass_layers; }
	void set_cover_cell(float p_cell) { cover_cell = CLAMP(p_cell, 0.5f, 64.0f); }
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
	Dictionary get_deadwood_at(const Vector3 &p_global_position, float p_reach = 0.0f) const;
	// Canopy shade at a point (0 open sky, 1 dense crown), from the canopy species placed so far.
	float get_shade_at(const Vector3 &p_global_position) const;
	float get_stand_shade_at(const Vector3 &p_global_position) const;
	float get_conifer_at(const Vector3 &p_global_position) const;
	// Leaf fall from the placed crowns, 0 to 1, with a drift past their edges (PlantSpecies.RULE_LITTER).
	static constexpr float LITTER_SPREAD = 6.0f; // Metres: the drift's e-folding distance past a crown's edge.
	float get_litter_at(const Vector3 &p_global_position) const;
};

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
#include "core/object/worker_thread_pool.h"
#include "core/os/rw_lock.h"
#include "core/templates/local_vector.h"
#include "core/templates/safe_refcount.h"
#include "scene/3d/node_3d.h"

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
		int count = 0;
	};
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
	void set_weather_map(const NodePath &p_path) { weather_map = p_path; }
	NodePath get_weather_map() const { return weather_map; }
	void set_tile_size(float p_size);
	float get_tile_size() const { return tile_size; }
	void set_seed(int p_seed);
	int get_seed() const { return seed; }
	void set_populate_on_ready(bool p_enable);
	bool is_populate_on_ready() const { return populate_on_ready; }

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
	// Canopy shade at a point (0 open sky, 1 dense crown), from the canopy species placed so far.
	float get_shade_at(const Vector3 &p_global_position) const;
	float get_stand_shade_at(const Vector3 &p_global_position) const;
	float get_conifer_at(const Vector3 &p_global_position) const;
};

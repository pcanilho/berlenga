/**************************************************************************/
/*  snow_cover_3d.h                                                       */
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

#include "snow_climate.h"

#include "core/io/image.h"
#include "core/object/worker_thread_pool.h"
#include "modules/ground/ground_height_query.h"
#include "modules/weather/weather_map_3d.h"
#include "modules/world/world_contracts.h"
#include "scene/3d/node_3d.h"
#include "scene/resources/image_texture.h"

class PhysicsDirectSpaceState3D;

class SnowAvalanche3D;
class SnowBodies3D;
class SnowCornice3D;
class SnowLakeIce3D;
class TerrainDeformer3D;
class TerrainDataMap3D;


// The snowpack on a square of cells that follows the camera (docs/ideas/snow.md). Per cell it keeps the
// snow water equivalent, its density, its liquid water and its albedo, and advances them from the weather:
// rain or snow by air temperature and humidity (Jennings et al. 2018), the air cooling with altitude, new
// snow density by temperature (Hedstrom and Pomeroy 1998), settling and albedo ageing (CLASS, Verseghy
// 1991), melt by temperature and absorbed sunlight (Pellicciotti et al. 2005), liquid water held and
// refrozen, snow sliding off steep slopes (SnowSlide, Bernhardt and Schulz 2010) and wind piling it in lee
// and stripping exposed ground (Winstral et al. 2002). Materials read it through the snow_map global
// (BaseMaterial3D Snow), scripts through get_*_at().
class SnowCover3D : public Node3D, public WorldSnowProvider {
	GDCLASS(SnowCover3D, Node3D);
	friend class SnowCornice3D;
	friend class SnowSurface3D;
	friend class SnowLakeIce3D;
	friend class SnowAvalanche3D;

	static SnowCover3D *active;

	// Settings.
	float extent = 512.0f;
	int resolution = 256;
	NodePath follow_target;
	float update_rate = 2.0f; // updates per real second
	NodePath height_source;
	uint32_t ground_collision_mask = 1;
	uint32_t shelter_collision_mask = 0; // with a height source: colliders whose tops catch the snow (roofs)
	bool canopy_enabled = true; // trees in the weather map's canopy hold snow (Hedstrom and Pomeroy 1998)
	NodePath sun_light;
	float lapse_rate = 6.5f; // C per km
	float temperature = -2.0f; // C, without a WeatherMap3D
	float humidity = 0.85f; // without a WeatherMap3D
	float precipitation_rate = 0.0f; // mm/h water, without a WeatherMap3D
	float time_scale = 1.0f; // without a WeatherMap3D (otherwise the map's)
	float melt_threshold = 1.0f; // C
	float melt_temperature_factor = 0.05f; // mm / (h C)
	float melt_shortwave_factor = 0.0094f; // mm m2 / (h W)
	float liquid_capacity = 0.05f; // share of the pack's water it holds as liquid
	bool crust_enabled = true;
	bool hoar_enabled = true; // surface hoar on clear calm nights, buried as a weak layer (round 5)
	bool suncups_enabled = true; // ablation hollows in sunny melt (follow-up 41)
	bool slush_enabled = true; // melt water ponds in the pack's base over frozen ground (follow-up 41)
	bool slide_enabled = true;
	float slide_interval = 600.0f; // weather seconds between slide passes
	float slide_angle = 25.0f; // degrees
	float slide_min_holding = 50.0f; // mm SWE held at 75 degrees and steeper
	float slide_holding_decay = 0.074f; // per degree below 75
	bool drift_enabled = true;
	float drift_distance = 100.0f; // m, dmax
	float drift_strength = 0.6f;
	float drift_threshold_wind = 7.0f; // m/s at 10 m
	bool wind_transport_enabled = true; // the wind moves lying snow (SnowTran-3D)
	float wind_curvature_length = 20.0f; // m, MicroMet's curvature length scale
	bool publish_shader_globals = true;
	float precipitation_gradient = 0.0f; // share more per 100 m above the reference height
	// A TerrainDataMap3D with a baked climate: its precipitation field (orographic, Smith and Barstad 2004)
	// over its recipe's flat-ground precipitation scales what falls, in place of precipitation_gradient.
	NodePath climate_map;

	// The world tier (docs/ideas/snow.md, stage 4): a coarse pack over the whole terrain, started from a baked
	// climate and carried on by the weather at a lower rate. It seeds cells entering this square and draws the
	// snow outside it (snow_world_map).
	Ref<SnowClimate> climate;
	int world_update_every = 8; // updates of this square per world update
	SnowCover3D *world = nullptr;
	int world_counter = 0;
	double world_accum = 0.0;
	bool world_globals_added = false;

	// Internal covers (the world tier, a climate bake): a fixed square, ground from the owner.
	SnowCover3D *owner = nullptr;
	bool driven = false; // inputs set by a climate bake (no weather map)
	Vector2 driven_wind; // m/s at 10 m, the direction it blows to
	Vector3 driven_sun; // toward the sun; zero = night
	float driven_direct = 0.0f; // W/m2 direct normal at the ground, clouds included
	float driven_ref_height = 0.0f; // m, where the driven temperature holds
	HashMap<int, Vector<float>> sx_cache; // driven: Sx per 22.5 degree wind direction
	// Cornices (world tier and bake): the overhang's length along the wind that built it, per cell.
	bool cornices_enabled = false;
	Vector<float> cell_cornice_x;
	Vector<float> cell_cornice_z;
	// Cornice failure (docs/ideas/snow.md, follow-up 6): the tension crack behind each cornice, its opening (m).
	bool cornice_failure_enabled = true;
	Vector<float> cell_cornice_crack;
	struct CorniceFailure {
		Vector3 position; // the crest top
		Vector2 direction; // out over the lee
		float length = 0.0f; // m
		float mass = 0.0f; // kg
	};
	Vector<CorniceFailure> cornice_failures; // since the owner last emitted them
	uint64_t cornice_failure_count = 0;
	Vector2 last_wind; // m/s at 10 m, last update

	// Cells.
	Vector2i origin_cell;
	bool origin_valid = false;
	Vector<float> cell_ground; // m
	Vector<float> cell_swe; // mm water, ice and liquid
	Vector<float> cell_liquid; // mm
	Vector<float> cell_density; // kg/m3
	Vector<float> cell_albedo;
	Vector<float> cell_runoff; // mm/h leaving the pack, last update
	Vector<float> cell_sx; // degrees, Winstral's maximum upwind slope
	Vector<float> cell_canopy; // canopy closure over the cell, 0..1 (WeatherMap3D's canopy)
	Vector<float> cell_canopy_load; // mm of snow water held in the canopy
	Ref<Image> catch_image;
	Ref<ImageTexture> catch_texture;
	Vector<float> cell_new_depth; // m of new snow laid by the last update (SnowSurface3D refills trails with it)
	// The top crust: thickness (m), melt-freeze cycles, snow over it (m), and 1 while its layer is
	// wet since it last froze (a crust in the making, or one melting again).
	Vector<float> cell_crust;
	Vector<float> cell_crust_cycles;
	Vector<float> cell_crust_burial;
	Vector<float> cell_crust_wet;
	// Surface hoar (docs/ideas/snow.md, round 5): the crystals on the surface (mm), and the newest buried layer
	// of them: its crystals (mm), the snow over it (m) and the hours since it was buried.
	Vector<float> cell_hoar;
	Vector<float> cell_cups; // m, suncup relief (ridge to hollow)
	Vector<float> cell_seal; // 0..1, how far frozen ground under the pack stops melt water draining (WorldGround.frost_at)
	Ref<Image> cups_image;
	Ref<ImageTexture> cups_texture;
	Ref<Image> upload_cups;
	Vector<float> cell_hoar_layer;
	Vector<float> cell_hoar_burial;
	Vector<float> cell_hoar_age;
	Vector<uint8_t> cell_height_valid;
	Vector2 sx_direction;
	bool sx_valid = false;
	bool sx_dirty = false; // the ground moved under the baked Sx: rebake in the background
	Vector<float> sx_next;
	Vector2 sx_next_direction;
	Vector2i sx_next_origin;
	WorkerThreadPool::GroupID sx_task = WorkerThreadPool::INVALID_TASK_ID;
	Vector<uint8_t> cell_steep; // ground steeper than slide_angle toward a neighbour
	bool steep_dirty = true;
	double slide_accum = 0.0;

	struct Inputs {
		float dt_h = 0.0f;
		float ref_height = 0.0f;
		float temperature = 0.0f;
		float humidity = 0.0f;
		Vector3 sun_dir; // toward the sun; zero = no sun term
		float direct_normal = 0.0f; // W/m2 at the ground, clear sky
		float wind_factor = 0.0f; // drift weight, 0..1
		float drift_norm = 1.0f;
		float base_precip = 0.0f;
		double weather_dt = 0.0; // s of weather time this update steps
		const WeatherMap3D *weather = nullptr; // main thread only
		WeatherMap3D::Snapshot wx; // what the cells read of it (any thread)
		const TerrainDataMap3D *climate_map = nullptr; // baked, with a recipe
		float climate_background = 0.0f; // mm a year, the recipe's flat-ground precipitation
	} inputs;

	Ref<Image> snow_image;
	Ref<ImageTexture> snow_texture;
	bool globals_added = false;
	double update_accum = 0.0;
	double update_msec = 0.0;
	uint64_t update_count = 0;
	float last_step_hours = 0.0f;
	float last_drift = 0.0f;

	float _cell_size() const { return extent / resolution; }
	Vector3 _follow_position() const;
	void _allocate();
	void _follow(Vector3 p_center);
	void _shift(Vector2i p_new_origin);
	using GroundQuery = GroundHeightQuery;
	GroundQuery _ground_query() const;
	static float _query_ground(const GroundQuery &p_q, Vector3 p_position);
	void _sample_heights(int p_from_row, int p_to_row, const Vector<uint8_t> *p_only = nullptr);
	void _start_sx(Vector2 p_wind_dir);
	void _finish_sx(bool p_wait);
	void _sx_row(uint32_t p_row, void *p_unused);
	float _sx_at(int p_x, int p_z, Vector2 p_upwind) const;
	void _update_steep();
	void _prepare_inputs(double p_weather_dt, bool p_blocking);
	void _update_row(uint32_t p_row, void *p_unused);
	void _slide();
	void _update_hoar(int p_i, const Vector3 &p_pos, float p_air, float p_snow, float p_rain, float p_melt, float p_rho_before, float p_rho, float p_swe, const Inputs &p_in);
	void _clear_hoar(int p_i);
	void _update_crust(float &r_crust, float &r_cycles, float &r_burial, float &r_wet, float p_rain, float p_melt_depth, float p_refrozen, float p_liquid_before, float p_liquid) const;
	void _update(double p_weather_dt, bool p_blocking);
	void _upload(); // the textures and the published copies, now (main thread)
	void _set_global(const StringName &p_name, const Variant &p_value, int p_type);
	void _publish_globals();
	bool _in_world_only(Vector3 p_position) const;
	float _sample(const Vector<float> &p_field, Vector2 p_xz) const;
	Vector3 _cell_center(int p_x, int p_z) const;
	void _setup_internal(SnowCover3D *p_owner, Rect2 p_rect, int p_resolution);
	void _grow_cornices(float p_dt_h, Vector2 p_wind);
	void _fail_cornice(int p_index, float p_length, Vector2 p_dir);
	void _deposit(int p_index, float p_swe_mm, float p_density);
	void _emit_cornice_failures();
	void _blow(float p_dt_h, Vector2 p_wind);
	float last_blown = 0.0f; // mm moved per cell (mean) by the last update's transport
	float last_blown_max = 0.0f; // kg/(m s), largest transport flux in the last update
	void _seed_from_world(const Vector<uint8_t> *p_only);
	void _feed_world();
	void _make_world();
	void _free_world();
	void _publish_world_globals();

	// Background updates (docs/ideas/snow.md, follow-up 14). The cells, slides, drift and cornices (and the
	// world tier's turn) run on a worker thread; the main thread gathers the weather the cells read before
	// it starts and swaps the results in when it is done. Readers on the main thread read `pub`, the
	// published copies (shared buffers, copied on the worker's first write), so they never wait.
	struct Published {
		Vector<float> swe, liquid, density, albedo, runoff, canopy_load, new_depth;
		Vector<float> crust, crust_cycles, crust_burial, crust_wet;
		Vector<float> hoar, hoar_layer, hoar_burial, hoar_age;
		Vector<float> cups;
		Vector<float> cornice_x, cornice_z, cornice_crack;
		Vector<float> ice, white_ice;
		Vector<float> lap; // mg/m2 BC equivalent showing at the surface
	} pub;
	bool background_updates = true;
	WorkerThreadPool::TaskID cover_task = WorkerThreadPool::INVALID_TASK_ID;
	bool cover_task_world = false; // the task carries the world tier's turn
	double world_dt = 0.0;
	// The terrain climate's precipitation per cell (mm a year), read on the main thread.
	Vector<float> cell_climate_precip;
	Vector<float> cell_fire; // W/m2 from WorldFire, read on the main thread each update (empty with no fire)
	// Sea salt on the snow and lake ice (WorldWeather.salt_at, g/m2): its brine melts them below 0 C (salt_melt).
	bool salt_melt = true;
	Vector<float> cell_salt; // empty with none
	Vector<float> cell_salt_seen; // the surface salt last read (new salt is what it rose by)
	Vector<float> cell_salt_pack; // g/m2 of salt in the pack's brine (the meltwater draining out takes its share)
	void _refresh_salt();
	// Wind obstacles (rocks, walls, buildings, vehicles; WorldWeather.obstacle_height_at), m over the ground: Sx
	// counts them as terrain. Read on the main thread a band of rows an update into obstacle_next; an Sx bake takes
	// it in when it starts (its rows read cell_obstacle on workers).
	Vector<float> cell_obstacle, obstacle_next;
	Vector<float> cell_lee; // the near-ground wind behind them over the open (WorldWeather.obstacle_lee_at), for blowing snow
	Vector2i obstacle_origin;
	int obstacle_row = 0; // the sweep reads a band of rows each update
	bool obstacle_sweep_changed = false, obstacle_sweep_any = false;
	bool obstacle_active = false; // the last full sweep found obstacles or lee
	bool drift_obstacles = true;
	// Light-absorbing particles (docs/ideas/world-links.md, dirty snow), mg/m2 of black carbon equivalent: on the
	// surface, under the snow fallen since (deep) with that snow's depth (m) over it, and the deposition from the
	// smoke haze and burnt forest (mg/m2 per hour, read on the main thread every few seconds).
	bool impurities = true;
	Vector<float> cell_lap, cell_lap_deep, cell_lap_cover, cell_lap_in;
	uint64_t lap_refresh_usec = 0;
	struct ImpurityDrop {
		Vector2 center;
		float radius = 0.0f;
		float mg = 0.0f;
	};
	LocalVector<ImpurityDrop> impurity_drops;
	// Crowns shaken from outside the weather (WorldSnow.shake_canopy): applied at the next update's start.
	struct CanopyShake {
		Vector2 center;
		float radius = 0.0f;
		float share = 0.0f;
	};
	LocalVector<CanopyShake> canopy_shakes;
	void _apply_canopy_shakes();
	void _refresh_lap();
	// Water (docs/ideas/snow.md, follow-up 15): the water surface over the cell from the terrain data map
	// (-INF: dry land), read on the main thread, and the lake ice on it (m).
	Vector<float> cell_water;
	Vector<float> cell_ice; // m of lake ice
	Vector<float> cell_white_ice; // m of it that is snow ice (flooded snow, white), the rest clear
	bool lake_ice_enabled = true;
	SnowLakeIce3D *lake_ice = nullptr; // the drawn ice, an internal child
	bool bodies_enabled = true;
	SnowBodies3D *bodies_node = nullptr; // snow carried by moving things, an internal child
	bool avalanches_enabled = true;
	SnowAvalanche3D *avalanche_node = nullptr; // slab release and run-out, an internal child
	const TerrainDataMap3D *_terrain_map() const;
	uint64_t main_usec = 0; // main-thread time of the last update (gather, start, swap in)
	// The textures' images, built off the main thread.
	Ref<Image> upload_snow;
	Ref<Image> upload_catch;
	void _publish_cells();
	// Lake ice broken under bodies too heavy for it (docs/ideas/world-links.md, bodies → lake ice): world x, z and
	// radius, cut from the published ice at once and from the cells at the next update's start.
	struct IceBreak {
		Vector2 center;
		float radius = 0.0f;
	};
	LocalVector<IceBreak> ice_breaks;
	int ice_break_count = 0;
	void _cut_ice(Vector<float> &r_ice, Vector<float> &r_white) const;
	void _ice_load_step();
	void _refresh_cell_inputs(int p_from_row, int p_to_row, const Vector<uint8_t> *p_only);
	void _refresh_fire();
	void _refresh_obstacles();
	void _update_begin(double p_weather_dt, bool p_blocking);
	void _gather_seal(); // main thread: WorldGround.frost_at under the wet cells
	void _update_cells(bool p_parallel);
	void _update_end();
	void _build_upload();
	void _apply_upload();
	void _start_background(double p_weather_dt, bool p_world);
	void _background_task(void *p_unused);
	bool _finish_background(bool p_wait);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	static SnowCover3D *get_active() { return active; }

	void set_extent(float p_extent);
	float get_extent() const { return extent; }
	void set_resolution(int p_resolution);
	int get_resolution() const { return resolution; }
	void set_follow_target(const NodePath &p_path);
	NodePath get_follow_target() const { return follow_target; }
	void set_update_rate(float p_rate);
	float get_update_rate() const { return update_rate; }
	void set_height_source(const NodePath &p_path);
	NodePath get_height_source() const { return height_source; }
	void set_shelter_collision_mask(uint32_t p_mask) { shelter_collision_mask = p_mask; }
	uint32_t get_shelter_collision_mask() const { return shelter_collision_mask; }
	void set_canopy_enabled(bool p_enabled) { canopy_enabled = p_enabled; }
	bool is_canopy_enabled() const { return canopy_enabled; }
	float get_canopy_load_at(Vector3 p_position) const;
	float shake_canopy(Vector3 p_position, float p_radius, float p_share);
	float get_canopy_capacity_at(Vector3 p_position) const;
	static float canopy_capacity(float p_closure, float p_snow_density);
	Ref<Texture2D> get_catch_texture() const { return catch_texture; }
	void set_ground_collision_mask(uint32_t p_mask);
	uint32_t get_ground_collision_mask() const { return ground_collision_mask; }
	void set_sun_light(const NodePath &p_path);
	NodePath get_sun_light() const { return sun_light; }
	void set_lapse_rate(float p_rate);
	float get_lapse_rate() const { return lapse_rate; }
	void set_temperature(float p_celsius);
	float get_temperature() const { return temperature; }
	void set_humidity(float p_humidity);
	float get_humidity() const { return humidity; }
	void set_precipitation_rate(float p_rate);
	float get_precipitation_rate() const { return precipitation_rate; }
	void set_time_scale(float p_scale);
	float get_time_scale() const { return time_scale; }
	void set_melt_threshold(float p_celsius);
	float get_melt_threshold() const { return melt_threshold; }
	void set_melt_temperature_factor(float p_factor);
	float get_melt_temperature_factor() const { return melt_temperature_factor; }
	void set_melt_shortwave_factor(float p_factor);
	float get_melt_shortwave_factor() const { return melt_shortwave_factor; }
	void set_liquid_capacity(float p_share);
	float get_liquid_capacity() const { return liquid_capacity; }
	void set_crust_enabled(bool p_enabled);
	bool is_crust_enabled() const { return crust_enabled; }
	void set_hoar_enabled(bool p_enabled);
	void set_suncups_enabled(bool p_enabled) { suncups_enabled = p_enabled; }
	bool is_suncups_enabled() const { return suncups_enabled; }
	void set_slush_enabled(bool p_enabled) { slush_enabled = p_enabled; }
	bool is_slush_enabled() const { return slush_enabled; }
	float get_slush_at(Vector3 p_position) const; // m, the saturated layer at the pack's base
	static float ground_seal(Vector3 p_frost); // 0..1 from WorldGround.frost_at (thawed top m, frost depth m, weakening)
	static float slush_drain_rate(float p_density, float p_slope_sine, float p_length); // 1/h, Darcy flow off a slope of that length
	static constexpr float SLUSH_SLOPE_LENGTH = 50.0f; // m, the slope a slush layer drains along
	// One step of suncup relief p_relief (m): grown by melt from direct sun (mm water), smoothed by melt from the
	// air, rain (mm) and filled by new snow (m deep); p_dirt mg/m2 of particles on the surface.
	static float suncup_step(float p_relief, float p_sun_melt, float p_air_melt, float p_rain, float p_new_depth, float p_dirt, float p_depth);
	float get_suncup_relief_at(Vector3 p_position) const; // m
	Ref<Texture2D> get_cups_texture() const { return cups_texture; }
	bool is_hoar_enabled() const { return hoar_enabled; }
	void set_slide_enabled(bool p_enabled);
	bool is_slide_enabled() const { return slide_enabled; }
	void set_slide_interval(float p_seconds);
	float get_slide_interval() const { return slide_interval; }
	void set_slide_angle(float p_degrees);
	float get_slide_angle() const { return slide_angle; }
	void set_slide_min_holding(float p_mm);
	float get_slide_min_holding() const { return slide_min_holding; }
	void set_slide_holding_decay(float p_per_degree);
	float get_slide_holding_decay() const { return slide_holding_decay; }
	void set_drift_obstacles(bool p_enabled);
	bool is_drift_obstacles_enabled() const { return drift_obstacles; }
	void set_drift_enabled(bool p_enabled);
	bool is_drift_enabled() const { return drift_enabled; }
	void set_drift_distance(float p_m);
	float get_drift_distance() const { return drift_distance; }
	void set_drift_strength(float p_strength);
	float get_drift_strength() const { return drift_strength; }
	void set_drift_threshold_wind(float p_speed);
	float get_drift_threshold_wind() const { return drift_threshold_wind; }
	void set_wind_transport_enabled(bool p_enabled);
	bool is_wind_transport_enabled() const { return wind_transport_enabled; }
	void set_wind_curvature_length(float p_m);
	float get_wind_curvature_length() const { return wind_curvature_length; }
	// Snow the wind carries over lying snow, kg per metre of width per second, at a 10 m wind of p_wind10
	// m/s over snow of p_density kg/m3 at p_celsius: saltation (Pomeroy and Gray 1990) plus suspension
	// (Pomeroy and Male 1992). 0 at or below the threshold.
	static float transport_flux(float p_wind10, float p_celsius, float p_density);
	static float transport_threshold(float p_celsius, float p_density); // m/s at 10 m
	float get_blown_mean() const { return last_blown; }
	float get_blown_flux_max() const { return last_blown_max; }
	void set_publish_shader_globals(bool p_enabled);
	bool get_publish_shader_globals() const { return publish_shader_globals; }
	void set_precipitation_gradient(float p_share);
	float get_precipitation_gradient() const { return precipitation_gradient; }
	void set_climate_map(const NodePath &p_path);
	NodePath get_climate_map() const { return climate_map; }
	// The data map climate_map points at, when it has a baked climate and a recipe; else null.
	const TerrainDataMap3D *get_climate_map_node() const;
	// Share of the recipe's background precipitation falling at a point: the climate map's, else
	// precipitation_gradient's per 100 m above p_ref_height.
	float get_orographic_factor_at(Vector3 p_position, float p_ref_height) const;
	void set_climate(const Ref<SnowClimate> &p_climate);
	Ref<SnowClimate> get_climate() const { return climate; }
	void set_world_update_every(int p_updates);
	int get_world_update_every() const { return world_update_every; }

	// Runs the climate's season over the square p_rect (p_resolution cells a side) on this cover's ground and
	// settings and stores the result in the climate (docs/ideas/snow.md, stage 4).
	Error bake_climate(Rect2 p_rect, int p_resolution);
	bool has_world() const { return world != nullptr; }
	Rect2 get_world_rect() const;
	float get_world_depth_at(Vector3 p_position) const; // m, the world tier
	float get_world_swe_at(Vector3 p_position) const; // mm, the world tier
	Vector2 get_cornice_at(Vector3 p_position) const; // m, the overhang's length along its wind (world tier)
	float get_cornice_crack_at(Vector3 p_position) const; // m, the opening of the tension crack behind it
	void set_cornice_failure_enabled(bool p_enabled);
	bool is_cornice_failure_enabled() const { return cornice_failure_enabled; }
	uint64_t get_cornice_failure_count() const;
	Ref<Texture2D> get_world_texture() const;
	void advance_world(float p_seconds);

	void advance(float p_seconds);
	void clear();
	void add_snow(float p_swe_mm, float p_density = 100.0f, bool p_world = true);
	void refresh_heights();

	Rect2 get_map_rect() const;
	float get_depth_at(Vector3 p_position) const; // m
	float get_surface_depth_at(Vector3 p_position) const; // m, Catmull-Rom between cells as the drawn surface
	float get_swe_at(Vector3 p_position) const; // mm
	float get_density_at(Vector3 p_position) const; // kg/m3
	float get_albedo_at(Vector3 p_position) const;
	float get_wetness_at(Vector3 p_position) const; // liquid over what the pack holds, 0..1
	float get_runoff_at(Vector3 p_position) const; // mm/h
	float get_ground_height_at(Vector3 p_position) const;
	float get_new_snow_at(Vector3 p_position) const; // m laid by the last update
	// Water (follow-up 15): the water surface at a point from the terrain data map at climate_map (-INF over
	// dry land), and the lake ice on it (m). Snow falling on open water melts into it; on ice thick enough
	// it lies at the water surface.
	float get_water_level_at(Vector3 p_position) const;
	float get_ice_thickness_at(Vector3 p_position) const;
	float get_white_ice_thickness_at(Vector3 p_position) const;
	SnowLakeIce3D *get_lake_ice() const { return lake_ice; }
	SnowBodies3D *get_bodies() const { return bodies_node; }
	SnowAvalanche3D *get_avalanches() const { return avalanche_node; }
	void set_avalanches_enabled(bool p_enabled);
	bool is_avalanches_enabled() const { return avalanches_enabled; }
	void set_bodies_enabled(bool p_enabled);
	bool is_bodies_enabled() const { return bodies_enabled; }

	// The snow contract (WorldContracts::snow()).
	float contract_snow_depth_at(const Vector3 &p_position) const override { return get_depth_at(p_position); }
	float contract_snow_density_at(const Vector3 &p_position) const override;
	float contract_snow_wetness_at(const Vector3 &p_position) const override { return get_wetness_at(p_position); }
	float contract_snow_surface_at(const Vector3 &p_position) const override;
	float contract_snow_crust_at(const Vector3 &p_position) const override { return get_crust_strength_at(p_position); }
	Vector2 contract_snow_lake_ice_at(const Vector3 &p_position) const override;
	void contract_snow_add_impurity(const Vector3 &p_position, float p_radius, float p_mg_per_m2) override { add_impurity(p_position, p_radius, p_mg_per_m2); }
	float contract_snow_impurity_at(const Vector3 &p_position) const override { return get_impurity_at(p_position); }
	float contract_snow_avalanche_pressure_at(const Vector3 &p_position) const override;
	float contract_snow_avalanche_peak_pressure_at(const Vector3 &p_position) const override;
	float contract_snow_canopy_load_at(const Vector3 &p_position) const override { return get_canopy_load_at(p_position); }
	float contract_snow_canopy_capacity_at(const Vector3 &p_position) const override { return get_canopy_capacity_at(p_position); }
	float contract_snow_shake_canopy(const Vector3 &p_position, float p_radius, float p_share) override { return shake_canopy(p_position, p_radius, p_share); }
	void add_impurity(Vector3 p_position, float p_radius, float p_mg_per_m2);
	float get_impurity_at(Vector3 p_position) const;
	// The albedo the particles take off clean snow: 0.35 (1 - e^(-L / 15 mg/m2)), about 0.02 for the first mg/m2 (Warren
	// and Wiscombe 1980) and near 0.35 for a burnt forest's debris or a dust storm's layer (Painter et al. 2007;
	// Gleason et al. 2013).
	static float impurity_albedo_drop(float p_mg_per_m2);
	void set_impurities_enabled(bool p_enabled) { impurities = p_enabled; }
	bool is_impurities_enabled() const { return impurities; }
	void set_lake_ice_enabled(bool p_enabled);
	bool is_lake_ice_enabled() const { return lake_ice_enabled; }
	void set_salt_melt(bool p_enabled) { salt_melt = p_enabled; }
	bool is_salt_melt_enabled() const { return salt_melt; }
	// The freezing point depression of the salt's brine at a point (K): what salt_melt adds to the air there.
	float get_salt_depression_at(Vector3 p_position) const;
	// Lake ice after p_hours at p_air_celsius from p_ice m with p_snow_depth m of snow of p_snow_density
	// kg/m3 on it: Stefan's law with the snow and the air's boundary layer insulating it (Leppaeranta 1993),
	// growing below freezing; it does not melt here (the update melts it in a thaw once the snow is gone).
	static float lake_ice_growth(float p_ice, float p_air_celsius, float p_snow_depth, float p_snow_density, float p_hours);
	static float snow_conductivity(float p_density); // W/(m K), Sturm et al. 1997
	// The load floating ice carries before it breaks: P = A h^2 (kg, h in cm; Gold 1971), A about 10 at failure.
	static float ice_breaking_load(float p_ice, float p_white_share, float p_air_celsius);
	int get_ice_break_count() const { return ice_break_count; }
	static constexpr float ICE_WALK = 0.05f; // m of ice that bears a person (the deep snow and its collision lie on it)
	static constexpr float ICE_HOLD = 0.02f; // m of ice that keeps snow on it (thinner, snow sinks into slush and melts)
	// The top crust (docs/ideas/snow.md, follow-up 3d): its thickness, the snow over it, its melt-freeze
	// cycles and the pressure it bears before breaking (0 while it is wet).
	float get_crust_thickness_at(Vector3 p_position) const; // m
	float get_crust_depth_at(Vector3 p_position) const; // m of snow over it
	float get_crust_cycles_at(Vector3 p_position) const;
	float get_crust_strength_at(Vector3 p_position) const; // kPa
	static float crust_strength(float p_cycles, float p_thickness); // kPa, frozen
	void add_crust(float p_thickness, float p_cycles);
	// Surface hoar (round 5): the crystals on the surface, and the newest buried layer of them (its crystals, the
	// snow over it, days since buried, and the shear strength it holds).
	float get_hoar_at(Vector3 p_position) const; // mm
	float get_buried_hoar_at(Vector3 p_position) const; // mm, 0 none
	float get_buried_hoar_depth_at(Vector3 p_position) const; // m of snow over it
	float get_buried_hoar_age_at(Vector3 p_position) const; // days
	// The surface's frost deposition (kg/m2/h, negative: sublimation) and its temperature under the night sky.
	static float hoar_deposition(float p_air_celsius, float p_humidity, float p_wind, float p_cloud, float p_shortwave, float &r_surface_celsius);
	// A buried surface hoar layer's shear strength (Pa) by crystal size and age (Jamieson and Schweizer 2000).
	static float buried_hoar_strength(float p_size_mm, float p_age_hours);
	static float hoar_deposition_rate(float p_air_celsius, float p_humidity, float p_wind, float p_cloud, float p_shortwave); // kg/m2/h
	void add_hoar(float p_size_mm); // lays crystals on the snow (tests)
	void add_suncups(float p_relief); // sets the suncup relief (m) wherever there is snow (scenes, tests)
	// The ground straight from the height source (finer than the cells), NaN where nothing answers.
	float sample_ground_height(Vector3 p_position) const;
	void sample_ground_heights(Vector2 p_origin, float p_spacing, int p_width, int p_height, float *r_heights) const;
	float get_air_temperature_at(Vector3 p_position) const; // C, by the lapse rate
	float get_snowfall_rate_at(Vector3 p_position) const; // mm/h of water falling as snow
	uint64_t get_update_count() const { return update_count; }
	float get_last_step_hours() const { return last_step_hours; }
	float get_drift_factor() const { return last_drift; } // 0..1, wind above the blowing threshold
	float get_total_water() const; // kg (mm over m2) on the whole map
	double get_update_time_msec() const { return update_msec; }
	double get_main_thread_msec() const { return main_usec / 1000.0; }
	// Background updates (on by default): the cover steps on a worker thread and swaps in when done.
	void set_background_updates(bool p_enabled);
	bool is_background_updates() const { return background_updates; }
	bool is_updating() const { return cover_task != WorkerThreadPool::INVALID_TASK_ID; }
	// Waits for an update in flight and swaps it in (tests and tools; the game never needs to).
	void sync_update();
	Ref<Texture2D> get_snow_texture() const { return snow_texture; }

	// The published relations, for scripts and checks.
	static float snowfall_fraction(float p_celsius, float p_humidity); // Jennings et al. 2018
	static float new_snow_density(float p_celsius); // Hedstrom and Pomeroy 1998, kg/m3
	static float holding_depth(float p_slope_degrees, float p_min_mm, float p_decay); // SnowSlide, mm SWE
	// Cornice growth against the wind over the blowing threshold (Yu et al. 2023): 0 below 1 and above 2,
	// most at 1.4.
	static float cornice_growth_share(float p_wind_ratio);
	// Cornice failure (follow-up 6): the tensile stress at the root of an overhang p_length long with its
	// tension crack p_crack open (kPa), the slab's tensile strength (kPa) and the crack's opening rate (m/day),
	// p_liquid the pack's liquid water over its water.
	static float cornice_root_stress(float p_length, float p_crack, float p_liquid);
	static float cornice_strength(float p_liquid);
	static float cornice_crack_rate(float p_length, float p_crack, float p_celsius, float p_liquid);

	SnowCover3D();
	~SnowCover3D();
};

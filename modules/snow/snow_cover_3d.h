/**************************************************************************/
/*  snow_cover_3d.h                                                       */
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

#include "snow_climate.h"

#include "core/io/image.h"
#include "core/object/worker_thread_pool.h"
#include "scene/3d/node_3d.h"
#include "scene/resources/image_texture.h"

class PhysicsDirectSpaceState3D;

class SnowCornice3D;
class TerrainDeformer3D;
class TerrainDataMap3D;

class WeatherMap3D;

// The snowpack on a square of cells that follows the camera (docs/ideas/snow.md). Per cell it keeps the
// snow water equivalent, its density, its liquid water and its albedo, and advances them from the weather:
// rain or snow by air temperature and humidity (Jennings et al. 2018), the air cooling with altitude, new
// snow density by temperature (Hedstrom and Pomeroy 1998), settling and albedo ageing (CLASS, Verseghy
// 1991), melt by temperature and absorbed sunlight (Pellicciotti et al. 2005), liquid water held and
// refrozen, snow sliding off steep slopes (SnowSlide, Bernhardt and Schulz 2010) and wind piling it in lee
// and stripping exposed ground (Winstral et al. 2002). Materials read it through the snow_map global
// (BaseMaterial3D Snow), scripts through get_*_at().
class SnowCover3D : public Node3D {
	GDCLASS(SnowCover3D, Node3D);
	friend class SnowCornice3D;

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
	bool slide_enabled = true;
	float slide_interval = 600.0f; // weather seconds between slide passes
	float slide_angle = 25.0f; // degrees
	float slide_min_holding = 50.0f; // mm SWE held at 75 degrees and steeper
	float slide_holding_decay = 0.074f; // per degree below 75
	bool drift_enabled = true;
	float drift_distance = 100.0f; // m, dmax
	float drift_strength = 0.6f;
	float drift_threshold_wind = 7.0f; // m/s at 10 m
	bool publish_shader_globals = true;
	float precipitation_gradient = 0.0f; // share more per 100 m above the reference height

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
		const WeatherMap3D *weather = nullptr;
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
	struct GroundQuery {
		TerrainDeformer3D *deformer = nullptr;
		TerrainDataMap3D *data_map = nullptr;
		Object *getter = nullptr;
		PhysicsDirectSpaceState3D *space = nullptr;
		uint32_t mask = 1;
		float y0 = 0.0f;
	};
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
	void _update(double p_weather_dt, bool p_blocking);
	void _upload();
	void _set_global(const StringName &p_name, const Variant &p_value, int p_type);
	void _publish_globals();
	bool _in_world_only(Vector3 p_position) const;
	void _sample_canopy();
	float _sample(const Vector<float> &p_field, Vector2 p_xz) const;
	Vector3 _cell_center(int p_x, int p_z) const;
	void _setup_internal(SnowCover3D *p_owner, Rect2 p_rect, int p_resolution);
	void _grow_cornices(float p_dt_h, Vector2 p_wind);
	void _seed_from_world(const Vector<uint8_t> *p_only);
	void _make_world();
	void _free_world();
	void _publish_world_globals();

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
	void set_drift_enabled(bool p_enabled);
	bool is_drift_enabled() const { return drift_enabled; }
	void set_drift_distance(float p_m);
	float get_drift_distance() const { return drift_distance; }
	void set_drift_strength(float p_strength);
	float get_drift_strength() const { return drift_strength; }
	void set_drift_threshold_wind(float p_speed);
	float get_drift_threshold_wind() const { return drift_threshold_wind; }
	void set_publish_shader_globals(bool p_enabled);
	bool get_publish_shader_globals() const { return publish_shader_globals; }
	void set_precipitation_gradient(float p_share);
	float get_precipitation_gradient() const { return precipitation_gradient; }
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
	Ref<Texture2D> get_world_texture() const;
	void advance_world(float p_seconds);

	void advance(float p_seconds);
	void clear();
	void add_snow(float p_swe_mm, float p_density = 100.0f);
	void refresh_heights();

	Rect2 get_map_rect() const;
	float get_depth_at(Vector3 p_position) const; // m
	float get_swe_at(Vector3 p_position) const; // mm
	float get_density_at(Vector3 p_position) const; // kg/m3
	float get_albedo_at(Vector3 p_position) const;
	float get_wetness_at(Vector3 p_position) const; // liquid over what the pack holds, 0..1
	float get_runoff_at(Vector3 p_position) const; // mm/h
	float get_ground_height_at(Vector3 p_position) const;
	float get_new_snow_at(Vector3 p_position) const; // m laid by the last update
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
	Ref<Texture2D> get_snow_texture() const { return snow_texture; }

	// The published relations, for scripts and checks.
	static float snowfall_fraction(float p_celsius, float p_humidity); // Jennings et al. 2018
	static float new_snow_density(float p_celsius); // Hedstrom and Pomeroy 1998, kg/m3
	static float holding_depth(float p_slope_degrees, float p_min_mm, float p_decay); // SnowSlide, mm SWE
	// Cornice growth against the wind over the blowing threshold (Yu et al. 2023): 0 below 1 and above 2,
	// most at 1.4.
	static float cornice_growth_share(float p_wind_ratio);

	SnowCover3D();
	~SnowCover3D();
};

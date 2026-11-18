/**************************************************************************/
/*  weather_map_3d.h                                                      */
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

#include "modules/world/world_contracts.h"
#include "weather_clouds.h"
#include "weather_contact_wetness.h"
#include "weather_season.h"
#include "weather_shelter.h"
#include "weather_terrain_wind.h"

#include "core/io/image.h"
#include "core/object/object_id.h"
#include "core/object/worker_thread_pool.h"
#include "scene/3d/node_3d.h"
#include "scene/resources/image_texture.h"

// One weather state for a region of the world, after Guerrilla's Nubis weather map
// (Schneider 2015): a square of cells that follows the camera and holds, per cell, the
// mean wind at 10 m, cloud cover and type, rain rate, and the water that rain has left
// on surfaces (a film, then puddles). Everything that reacts to weather reads it: shaders
// through global uniforms, scripts and nodes through get_*_at(). See docs/ideas/weather.md.
class Environment;
class FogMaterial;
class FogVolume;

class WeatherMap3D : public Node3D, public WorldWeatherProvider {
	GDCLASS(WeatherMap3D, Node3D);

public:
	// Gust components: Kaimal-weighted plane waves drifting with the mean wind (Taylor's
	// frozen turbulence). GUSTS_PER_AXIS each for the along-wind, cross-wind and vertical
	// parts; the same numbers go to shaders as the weather_gusts texture.
	static constexpr int GUSTS_PER_AXIS = 8;
	static constexpr int GUST_COUNT = GUSTS_PER_AXIS * 3;

	struct Storm {
		int id = 0;
		Vector2 position; // world x, z (m)
		float radius = 1500.0f; // rain core radius (m)
		float rain_rate = 30.0f; // peak, mm/h
		float cloud_cover = 1.0f;
		float outflow = 10.0f; // peak downburst outflow speed (m/s)
		float lifetime = 3600.0f; // weather seconds; <= 0 lasts until removed
		float age = 0.0f;
	};

private:
	struct Gust {
		Vector2 k; // wave vector in the wind frame (along, across), rad/m
		float amplitude = 0.0f; // per unit sigma
		double phase = 0.0;
	};

	// Settings.
	float extent = 4096.0f;
	int resolution = 128;
	NodePath follow_target;
	float update_rate = 4.0f;
	float time_scale = 1.0f;
	float transition_time = 600.0f;
	float ground_height = 0.0f;

	float wind_speed = 5.0f; // m/s at 10 m
	Vector2 wind_direction = Vector2(1, 0); // the way the wind blows toward (x, z)
	float roughness_length = 0.03f; // z0, m
	float gust_intensity = 1.0f; // scale on the log-law turbulence intensity
	float cloud_cover = 0.4f;
	float cloud_type = 0.3f;
	float cloud_variation = 0.35f;
	float cloud_scale = 6000.0f; // m, size of the cloud cover noise
	float rain_rate = 0.0f; // mm/h
	float temperature = 15.0f; // C
	// The air through the day and the year (docs/ideas/world-links.md, link 1).
	bool air_follows_climate = true; // with a climate: the day's mean from it plus season_anomaly
	bool air_daily_cycle = true; // with a SkyClock: cooler nights, warmer afternoons (Parton and Logan 1981)
	float daily_temperature_range = 10.0f; // K, clear calm day; clouds and rain narrow it
	bool sun_drying = true; // the sun's net radiation in the evaporation (Penman 1948's combination)
	bool dew_and_frost = true; // clear calm nights: dew, and hoarfrost below freezing (Monteith 1957)
	// Season (weather_season.h): the date from a SkyClock, or season_day; the climate from a ClimateRecipe.
	NodePath sky_clock;
	int season_day = 135; // day of the year without a SkyClock (15 May)
	Ref<Resource> climate;
	float season_anomaly = 0.0f; // K: the live air against the climate, over about a month
	bool season_cached = false;
	int season_cache_day = 0;
	float season_cache_anomaly = 0.0f;
	WeatherSeason::State season_cache;
	float humidity = 0.7f; // relative, 0..1
	float surface_storage = 0.5f; // mm a surface holds before water pools
	float puddle_max_depth = 20.0f; // mm
	float puddle_drain_rate = 2.0f; // mm/h
	Ref<Texture2D> painted_map;
	Rect2 painted_map_rect = Rect2(-2048, -2048, 4096, 4096);
	bool publish_shader_globals = true;

	// Cloud layer (docs/ideas/weather.md, step 3).
	float cloud_base_height = 1500.0f; // m
	float cloud_thickness = 1500.0f; // m, of cumulus
	float cloud_density = 1.0f;
	float cloud_shape_scale = 3000.0f; // m, one tile of the shape noise
	// Clouds that form and clear (step 3e).
	bool cloud_evolution = true;
	float cloud_lifetime = 900.0f; // s, turnover of the cover noise's largest octave
	float cloud_churn = 1.5f; // m/s, how fast the cloud shapes change (the shape noise's reads slide apart)
	float cloud_shear = 1.0f; // scale on the wind shear's lean of the cloud tops (0: upright)
	double cloud_time = 0.0; // s of weather time the clouds have evolved
	NodePath sun_light;
	float cloud_shadow_extent = 6144.0f; // m
	int cloud_shadow_resolution = 256;
	float cloud_shadow_interval = 1.0f; // real seconds between bakes

	// The eased base state (what the cells see; the settings above are its targets).
	float cur_wind_speed = 5.0f;
	Vector2 cur_wind_direction = Vector2(1, 0);
	float cur_cloud_cover = 0.4f;
	float cur_cloud_type = 0.3f;
	float cur_rain_rate = 0.0f;
	float cur_temperature = 15.0f;
	float cur_humidity = 0.7f;
	// The air now: the eased mean above plus the day's cycle; its humidity at the mean's dew point.
	float cur_air_temperature = 15.0f;
	float cur_air_humidity = 0.7f;
	float cur_sun_sine = 0.0f; // sine of the sun's elevation (0 below the horizon or without a sun)
	bool sun_known = false; // a sun light or a sky clock says where the sun is
	float diurnal_cache_day_length = -1.0f;
	float diurnal_cache_mean = 0.5f;
	bool eased_once = false;
	// Followers (the sea's lag) watch these: cells refreshed, the base snapped, and weather time skipped
	// by advance().
	uint64_t cells_serial = 0;
	uint64_t snap_cells_serial = 0;
	uint64_t snap_serial = 0;
	double advanced_time = 0.0;

	// Cells, row-major in z then x; the map's corner sits on a whole cell.
	Vector2i origin_cell; // world cell index of cell (0, 0)
	bool origin_valid = false;
	Vector<float> cell_wind; // 2 per cell, mean wind at 10 m (m/s)
	Vector<float> cell_cover;
	Vector<float> cell_type;
	Vector<float> cell_rain; // mm/h
	Vector<float> cell_film; // mm
	Vector<float> cell_puddle; // mm
	Vector<float> cell_frost; // mm of water as hoarfrost on exposed surfaces
	Vector<float> cell_cooling; // K, the exposed surface's temperature less the air's (radiative cooling, sun warming)
	// Water reaching the ground where another node takes the precipitation (a snowpack's runoff), mm/h;
	// negative where none does, so the cell's own rain reaches the ground.
	Vector<float> cell_inflow;
	struct WaterSource {
		ObjectID owner;
		Vector<float> rates; // mm/h, row-major in z then x
		int resolution = 0;
		Rect2 rect;
	};
	LocalVector<WaterSource> water_sources;
	Ref<Image> painted_image;

	Vector<Storm> storms;
	int next_storm_id = 1;

	struct CellPass {
		float dt_h = 0.0f;
		float to_2m = 1.0f;
		Vector2 base_wind;
		float cover_gate = 1.0f;
		bool painted = false;
		float *wind = nullptr;
		float *cover = nullptr;
		float *type = nullptr;
		float *rain = nullptr;
		float *film = nullptr;
		const float *inflow = nullptr;
		float rain_fraction = 1.0f;
		float *puddle = nullptr;
		float *frost = nullptr;
		float *cooling = nullptr;
		bool dew = false;
		// The sun on wet surfaces (sun_drying): clear-sky shortwave on level ground (W/m2), the cloud shadow
		// when one is baked, the air the evaporation sees.
		bool sun = false;
		float shortwave_clear = 0.0f;
		bool use_shadow = false;
		Vector2 shadow_corner;
		float air_temperature = 15.0f;
		float air_humidity = 0.7f;
	};
	CellPass cell_pass;

	Gust gusts[GUST_COUNT];
	float gust_sigma_scale = 1.0f; // turbulence intensity of the base wind, for shaders
	double weather_time = 0.0;
	double update_accum = 0.0;
	Vector2 cloud_drift; // m, how far the cloud noise has drifted

	Ref<Image> wind_image;
	Ref<Image> state_image;
	Ref<Image> frost_image;
	Ref<ImageTexture> frost_texture;
	Ref<Image> gust_image;
	Ref<ImageTexture> wind_texture;
	Ref<ImageTexture> state_texture;
	Ref<ImageTexture> gust_texture;
	bool globals_added = false;

	// The cloud shadow: the sun's transmittance through the base shape over a square in the light's plane,
	// baked on the worker threads and handed to the sun as its projector.
	struct ShadowBake {
		WeatherClouds::State state;
		Vector3 axis_u; // the light's +X
		Vector3 axis_v; // the light's -Y
		Vector3 to_sun; // the light's +Z
		Vector2 corner; // m, in the light's plane
		float texel = 32.0f;
		int resolution = 256;
		int steps = 12;
		bool sky = false; // the sky's map: the beam exp(-tau) in red, the diffuse transmission in green, linear
		Vector<float> transmittance;
		Vector<uint8_t> bytes;
		float *transmittance_w = nullptr; // taken on the main thread, so the rows never copy on write
		uint8_t *bytes_w = nullptr;
		void row(uint32_t p_row, void *);
	};
	ShadowBake *shadow_bake = nullptr; // in flight
	WorkerThreadPool::GroupID shadow_task = WorkerThreadPool::INVALID_TASK_ID;
	double shadow_timer = 0.0;
	// The last finished bake: its frame, the drift it saw, and what the CPU samples.
	Vector3 shadow_axis_u;
	Vector3 shadow_axis_v;
	Vector2 shadow_corner;
	Vector2 shadow_drift;
	float shadow_size = 0.0f;
	int shadow_res = 0;
	Vector<float> shadow_values;
	Ref<Image> shadow_image;
	Ref<ImageTexture> shadow_texture;
	ObjectID shadow_light; // the light we gave our projector to
	// The sky's cloud shadow (its shafts across the sky): the same bake over a square kSkyShadowExtent wide,
	// coarser, read by WeatherSkyMaterial along its view rays (Unreal's cloud shadow map on the atmosphere).
	static constexpr float kSkyShadowExtent = 40960.0f; // m
	static constexpr int kSkyShadowResolution = 256;
	ShadowBake *sky_shadow_bake = nullptr;
	WorkerThreadPool::GroupID sky_shadow_task = WorkerThreadPool::INVALID_TASK_ID;
	double sky_shadow_timer = 0.0;
	Vector3 sky_shadow_axis_u;
	Vector3 sky_shadow_axis_v;
	Vector2 sky_shadow_corner;
	Vector2 sky_shadow_drift;
	Ref<ImageTexture> sky_shadow_texture;
	void _start_sky_shadow_bake();
	void _finish_sky_shadow_bake(bool p_discard);
	void _apply_sky_shadow();

	// Wind near the ground: canopy and obstacle shelter on a fine square, and gust patches.
	WeatherShelter shelter;
	// Wind over terrain (docs/ideas/wind.md): ground heights from terrain_source (a node with
	// get_height(position) or Terrain3D's get_data()), else the first TerrainDeformer3D, Landscape3D,
	// TerrainGenerator3D or Terrain3D in the scene, else rays on terrain_collision_mask.
	WeatherTerrainWind terrain_wind;
	float boundary_layer_height = 1000.0f; // m
	float wind_shear_aloft = 1.5f; // m/s per km above the boundary layer
	bool terrain_wind_enabled = true;
	NodePath terrain_source;
	uint32_t terrain_collision_mask = 1;
	ObjectID terrain_height_object; // what answers get_height for the square being sampled
	bool terrain_height_by_data = false; // Terrain3D: get_data().get_height()
	RID terrain_global_rid;
	bool shelter_enabled = true;
	Ref<Texture2D> canopy_map;
	float gust_patch_size = 6.0f; // m
	float gust_patch_strength = 0.35f; // standard deviation of the patches over the mean wind
	Vector2 patch_drift; // m, how far the patches have travelled (wrapped to their period)
	bool shelter_global_added = false;

	// Wet where water has been (weather_contact_wetness.h).
	bool contact_wetness_enabled = true;
	int contact_wetness_resolution = 512;
	float contact_wetness_size = 64.0f; // m
	float contact_wetness_dry_time = 600.0f; // s, at 20 C, half humidity, light wind
	WeatherContactWetness contact;
	double contact_accum = 0.0;
	bool contact_global_added = false;
	void _update_contact(double p_weather_dt);

	static WeatherMap3D *active;

	// Haze from falling and blowing precipitation (docs/ideas/weather.md, "Precipitation haze").
	struct HazeSource {
		ObjectID source;
		float sigma = 0.0f; // extinction, 1/m
		float height_scale = 0.0f; // m; <= 0: the same at every height
		float albedo = 0.95f;
		Vector3 centre;
		float radius = 0.0f; // m; <= 0: everywhere
		FogVolume *volume = nullptr;
		Ref<FogMaterial> material;
	};
	LocalVector<HazeSource> haze_sources;
	bool aerosol_haze = true;
	float haze_visibility = 40.0f; // km, dry air (relative humidity 0)
	float haze_height = 1200.0f; // m, the aerosol's scale height (the boundary layer)
	float applied_aerosol = -1.0f;
	void _update_aerosol();
	Vector2 applied_sky_aerosol = Vector2(-1.0f, -1.0f); // extinction and froxel reach last given to the sky
	// Under path tracing the aerosol is a traced medium instead of a froxel layer (godrays stage 3).
	bool traced_haze = true;
	Vector4 applied_traced_haze = Vector4(-1.0f, 0.0f, 0.0f, 0.0f);
	float applied_traced_reach = -1.0f;
	void _apply_traced_haze(const Vector4 &p_medium, float p_reach);
	void _apply_sky_aerosol(float p_extinction, float p_from);
	bool drive_environment_fog = true;
	struct FogBase {
		bool saved = false;
		bool fog_enabled = false;
		float fog_density = 0.0f;
		float aerial_perspective = 0.0f;
		bool volumetric_enabled = false;
		float volumetric_density = 0.0f;
		float volumetric_length = 64.0f;
		float volumetric_ambient_inject = 0.0f;
		float volumetric_anisotropy = 0.2f;
	} fog_base;
	Ref<Environment> fog_environment;
	float applied_haze = -1.0f;
	void _apply_haze();
	void _restore_fog();
	float _haze_weight(const HazeSource &p_source, const Vector3 &p_position) const;
	float _haze_base(const HazeSource &p_source) const;

	void _allocate();
	void _build_gusts();
	void _advance(double p_weather_dt, double p_real_dt);
	void _ease_base(double p_dt);
	void _follow(Vector3 p_center);
	void _shift(Vector2i p_new_origin);
	void _update_cells(double p_weather_dt);
	void _update_row(uint32_t p_row, CellPass *p_pass);
	void _gather_water_sources();
	void _set_base_to_settings();
	float _target_temperature() const;
	float _net_radiation(const CellPass &p_pass, Vector3 p_position, float p_cover) const;
	void _update_air();
	void _snap();
	void _fill_gust_image();
	void _upload();
	void _publish_globals();
	void _set_global(const StringName &p_name, const Variant &p_value, int p_type);
	Vector3 _follow_position() const;

	float _cell_size() const { return extent / resolution; }
	float _sample(const Vector<float> &p_field, Vector2 p_xz, int p_stride = 1, int p_offset = 0) const;
	// The arithmetic behind _sample() and get_sun_visibility_at(), shared with Snapshot so both answer alike.
	static float _sample_grid(const Vector<float> &p_field, int p_n, float p_cell_size, Vector2i p_origin_cell, bool p_origin_valid, Vector2 p_xz, int p_stride, int p_offset);
	static float _sample_shadow(const Vector<float> &p_values, int p_res, float p_size, Vector3 p_axis_u, Vector3 p_axis_v, Vector2 p_corner, Vector3 p_position);
	Vector2 _shadow_corner_now() const;
	float _cloud_noise(Vector2 p_xz) const;
	float _cloud_evolve() const;
	float _storm_weight(const Storm &p_storm) const;
	WeatherClouds::State _cloud_state() const;
	void _start_shadow_bake();
	void _finish_shadow_bake(bool p_wait);
	void _apply_shadow();
	void _release_shadow_light();
	void _push_sky();
	Vector3 _gust_at(Vector3 p_position, Vector2 p_mean) const;
	float _patch_at(Vector2 p_xz, float p_height) const;
	void _bake_shelter();
	void _update_terrain_wind();
	void _find_terrain_source();
	float _terrain_height(Vector2 p_xz) const;
	// Height above the ground: p_height when >= 0, else from the terrain under the point (or ground_height).
	float _height_above_ground(Vector3 p_position, float p_height, const float p_terrain[4]) const;

	WeatherSeason::Climate _season_climate() const;

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	// The map other nodes read: the last one that entered the tree.
	static WeatherMap3D *get_active() { return active; }

	void set_extent(float p_extent);
	float get_extent() const { return extent; }
	void set_resolution(int p_resolution);
	int get_resolution() const { return resolution; }
	void set_follow_target(const NodePath &p_path);
	NodePath get_follow_target() const { return follow_target; }
	void set_update_rate(float p_rate);
	float get_update_rate() const { return update_rate; }
	void set_time_scale(float p_scale);
	float get_time_scale() const { return time_scale; }
	void set_transition_time(float p_time);
	float get_transition_time() const { return transition_time; }
	void set_ground_height(float p_height);
	float get_ground_height() const { return ground_height; }

	void set_wind_speed(float p_speed);
	float get_wind_speed() const { return wind_speed; }
	void set_wind_direction(Vector2 p_direction);
	Vector2 get_wind_direction() const { return wind_direction; }
	void set_roughness_length(float p_z0);
	float get_roughness_length() const { return roughness_length; }
	void set_gust_intensity(float p_scale);
	float get_gust_intensity() const { return gust_intensity; }
	void set_cloud_cover(float p_cover);
	float get_cloud_cover() const { return cloud_cover; }
	void set_cloud_type(float p_type);
	float get_cloud_type() const { return cloud_type; }
	void set_cloud_variation(float p_variation);
	float get_cloud_variation() const { return cloud_variation; }
	void set_cloud_scale(float p_scale);
	float get_cloud_scale() const { return cloud_scale; }
	void set_rain_rate(float p_rate);
	float get_rain_rate() const { return rain_rate; }
	void set_temperature(float p_celsius);
	float get_temperature() const { return temperature; }
	// The air now, as everything that melts, freezes, dries, burns or fogs reads it: the eased state (the
	// settings, or the climate's day, are its targets) through the day's cycle.
	float get_air_temperature() const { return cur_air_temperature; }
	float get_air_humidity() const { return cur_air_humidity; }
	float get_air_mean_temperature() const { return cur_temperature; } // the day's mean, without the cycle
	// Open water's surface temperature (C): lakes and the sea follow the air's seasonal mean about a month
	// late (Livingstone and Lotter 1998), never below freezing; without a climate, the day's mean air.
	float get_water_temperature() const;
	float get_air_dew_point() const;
	// The live air at a point: the air above (or below) the map's ground height by the standard lapse rate,
	// 6.5 K/km (ICAO), with the dew point falling 1.8 K/km (the vapour mixing up unchanged).
	float get_air_temperature_at(Vector3 p_position) const;
	float get_air_humidity_at(Vector3 p_position) const;
	float get_sun_sine() const { return cur_sun_sine; }
	void set_air_follows_climate(bool p_enabled);
	bool get_air_follows_climate() const { return air_follows_climate; }
	void set_air_daily_cycle(bool p_enabled);
	bool get_air_daily_cycle() const { return air_daily_cycle; }
	void set_daily_temperature_range(float p_kelvin);
	float get_daily_temperature_range() const { return daily_temperature_range; }
	void set_sun_drying(bool p_enabled);
	bool get_sun_drying() const { return sun_drying; }
	void set_dew_and_frost(bool p_enabled);
	bool get_dew_and_frost() const { return dew_and_frost; }
	// Hoarfrost on exposed surfaces (mm of water) and their temperature (C): the air's plus the radiative
	// cooling of a clear night or the sun's warming (from the cells' net radiation).
	float get_frost_at(Vector3 p_position) const;
	float get_surface_temperature_at(Vector3 p_position) const;
	// Parton and Logan's (1981) day: 0 at the minimum just after sunrise, 1 at the afternoon peak.
	static float diurnal_shape(float p_solar_hour, float p_day_length);
	// Evaporation from a wet surface (mm/h), Penman's (1948) combination of the sun's net radiation (W/m2)
	// and the air's drying power.
	static float penman_evaporation(float p_net_radiation, float p_wind_2m, float p_celsius, float p_humidity);
	// Share of precipitation that falls as rain at an air temperature and relative humidity (0..1), the
	// complement of the snowfall fraction of Jennings et al. (2018).
	static float rain_fraction(float p_celsius, float p_humidity);
	void set_humidity(float p_humidity);
	float get_humidity() const { return humidity; }
	// Background aerosol haze (docs/ideas/weather.md, "Godrays"): what lets the sun's shafts show.
	void set_aerosol_haze(bool p_enabled);
	void set_traced_haze(bool p_enabled);
	bool get_traced_haze() const { return traced_haze; }
	bool get_aerosol_haze() const { return aerosol_haze; }
	void set_haze_visibility(float p_km);
	float get_haze_visibility() const { return haze_visibility; }
	void set_haze_height(float p_m);
	float get_haze_height() const { return haze_height; }
	// Extinction (1/m) at the ground of the aerosol haze for a dry visibility (km) and relative humidity.
	static float aerosol_extinction(float p_dry_visibility_km, float p_humidity);
	void set_surface_storage(float p_mm);
	float get_surface_storage() const { return surface_storage; }
	void set_puddle_max_depth(float p_mm);
	float get_puddle_max_depth() const { return puddle_max_depth; }
	void set_puddle_drain_rate(float p_mm_per_hour);
	float get_puddle_drain_rate() const { return puddle_drain_rate; }
	void set_painted_map(const Ref<Texture2D> &p_map);
	Ref<Texture2D> get_painted_map() const { return painted_map; }
	void set_painted_map_rect(Rect2 p_rect);
	Rect2 get_painted_map_rect() const { return painted_map_rect; }
	void set_publish_shader_globals(bool p_enabled);
	bool get_publish_shader_globals() const { return publish_shader_globals; }

	void set_contact_wetness_enabled(bool p_enabled);
	bool is_contact_wetness_enabled() const { return contact_wetness_enabled; }
	void set_contact_wetness_resolution(int p_resolution);
	int get_contact_wetness_resolution() const { return contact_wetness_resolution; }
	void set_contact_wetness_size(float p_size);
	float get_contact_wetness_size() const { return contact_wetness_size; }
	void set_contact_wetness_dry_time(float p_seconds);
	float get_contact_wetness_dry_time() const { return contact_wetness_dry_time; }
	// Wets a disc where water touched (a splash, a wading leg, a dripping body), 0..1.
	void add_contact_wetness(Vector3 p_position, float p_radius, float p_amount = 1.0f);
	// Wets from a grid of 0..1 values over p_rect (world XZ), e.g. a live water area's wet cells.
	void add_contact_wetness_grid(const PackedFloat32Array &p_values, int p_width, int p_height, Rect2 p_rect);
	void add_contact_wetness_cells(const float *p_values, int p_width, int p_height, Rect2 p_rect);
	float get_contact_wetness_at(Vector3 p_position) const;
	// How long contact wetness takes to fall to 1/e now (s, weather time); 0 = it is not drying (rain).
	float get_contact_drying_time() const;
	Rect2 get_contact_wetness_rect() const { return contact.rect(); }

	void set_cloud_base_height(float p_height);
	float get_cloud_base_height() const { return cloud_base_height; }
	void set_cloud_thickness(float p_thickness);
	float get_cloud_thickness() const { return cloud_thickness; }
	void set_cloud_density(float p_density);
	float get_cloud_density() const { return cloud_density; }
	void set_cloud_shape_scale(float p_scale);
	float get_cloud_shape_scale() const { return cloud_shape_scale; }
	void set_cloud_evolution(bool p_enabled);
	bool get_cloud_evolution() const { return cloud_evolution; }
	void set_cloud_lifetime(float p_seconds);
	float get_cloud_lifetime() const { return cloud_lifetime; }
	void set_cloud_churn(float p_speed);
	float get_cloud_churn() const { return cloud_churn; }
	void set_cloud_shear(float p_scale);
	float get_cloud_shear() const { return cloud_shear; }
	void set_cloud_time(double p_seconds);
	double get_cloud_time() const { return cloud_time; }
	void set_sun_light(const NodePath &p_path);
	NodePath get_sun_light() const { return sun_light; }
	void set_cloud_shadow_extent(float p_extent);
	float get_cloud_shadow_extent() const { return cloud_shadow_extent; }
	void set_cloud_shadow_resolution(int p_resolution);
	int get_cloud_shadow_resolution() const { return cloud_shadow_resolution; }
	void set_cloud_shadow_interval(float p_seconds);
	float get_cloud_shadow_interval() const { return cloud_shadow_interval; }

	void set_terrain_wind_enabled(bool p_enabled);
	bool is_terrain_wind_enabled() const { return terrain_wind_enabled; }
	void set_terrain_source(const NodePath &p_path);
	NodePath get_terrain_source() const { return terrain_source; }
	void set_terrain_collision_mask(uint32_t p_mask);
	uint32_t get_terrain_collision_mask() const { return terrain_collision_mask; }
	void set_terrain_wind_extent(float p_extent);
	float get_terrain_wind_extent() const { return terrain_wind.extent; }
	void set_terrain_wind_resolution(int p_resolution);
	int get_terrain_wind_resolution() const { return terrain_wind.resolution; }
	void set_terrain_curvature_length(float p_length);
	float get_terrain_curvature_length() const { return terrain_wind.curvature_length; }
	void set_terrain_slope_weight(float p_weight);
	float get_terrain_slope_weight() const { return terrain_wind.slope_weight; }
	void set_terrain_curvature_weight(float p_weight);
	float get_terrain_curvature_weight() const { return terrain_wind.curvature_weight; }
	void set_terrain_lee_reach(float p_reach);
	float get_terrain_lee_reach() const { return terrain_wind.lee_reach; }
	void set_terrain_lee_strength(float p_strength);
	float get_terrain_lee_strength() const { return terrain_wind.lee_strength; }
	void set_terrain_floor(float p_floor);
	float get_terrain_floor() const { return terrain_wind.floor; }
	// Rebuild the terrain wind now (after the terrain changed), sampling every height at once.
	void rebake_terrain_wind();
	bool is_terrain_wind_ready() const { return terrain_wind.valid; }
	float get_terrain_wind_derive_time() const { return terrain_wind.derive_ms; }
	Rect2 get_terrain_wind_rect() const { return terrain_wind.valid ? terrain_wind.rect() : Rect2(); }
	// The terrain's MicroMet speed weighting at a point (1: flat), its diverting angle (rad), the ground
	// height it sampled (NaN outside the square) and Sx, the steepest rise upwind within lee_reach (rad;
	// positive in a ridge's lee, Winstral et al. 2002).
	float get_terrain_speed_factor_at(Vector3 p_position) const;
	float get_terrain_divert_at(Vector3 p_position) const;
	float get_terrain_ground_at(Vector3 p_position) const;
	float get_terrain_lee_at(Vector3 p_position) const;

	void set_shelter_enabled(bool p_enabled);
	bool get_shelter_enabled() const { return shelter_enabled; }
	void set_shelter_extent(float p_extent);
	float get_shelter_extent() const { return shelter.extent; }
	void set_shelter_resolution(int p_resolution);
	int get_shelter_resolution() const { return shelter.resolution; }
	void set_canopy_map(const Ref<Texture2D> &p_map);
	Ref<Texture2D> get_canopy_map() const { return canopy_map; }
	void set_canopy_map_rect(Rect2 p_rect);
	Rect2 get_canopy_map_rect() const { return shelter.canopy_rect; }
	void set_canopy_map_max_height(float p_height);
	float get_canopy_map_max_height() const { return shelter.canopy_max_height; }
	void set_gust_patch_size(float p_size);
	float get_gust_patch_size() const { return gust_patch_size; }
	void set_gust_patch_strength(float p_strength);
	float get_gust_patch_strength() const { return gust_patch_strength; }
	int add_canopy(Vector2 p_center, float p_radius, float p_height, float p_density = 2.5f);
	void remove_canopy(int p_id);
	int add_shelter_box(const AABB &p_box);
	void remove_shelter_box(int p_id);
	void clear_shelter();
	void rebake_shelter();
	float get_shelter_bake_time() const { return shelter.bake_ms; }
	// Mean wind at a height over the open-ground wind there: canopy and lee, 0..1.
	float get_shelter_at(Vector3 p_position, float p_height_above_ground = -1.0f) const;
	// The canopy over a point: its height (m) and the share of the sky it closes (0..1).
	float get_canopy_height_at(Vector3 p_position) const;
	float get_canopy_closure_at(Vector3 p_position) const;
	// Water a node delivers to the ground in place of the precipitation over its rect (a snowpack's melt
	// and the rain through it), mm/h on a p_resolution square. Copied; call again to update.
	void set_surface_water_source(Object *p_owner, const PackedFloat32Array &p_rates, int p_resolution, Rect2 p_rect);
	void set_surface_water_source_cells(ObjectID p_owner, const Vector<float> &p_rates, int p_resolution, Rect2 p_rect);
	void clear_surface_water_source(Object *p_owner);
	Ref<Texture2D> get_shelter_texture() const { return shelter.texture; }
	Rect2 get_shelter_rect() const { return shelter.rect(); }
	static String get_wind_shader_code();

	// Jump the eased state to the settings (no transition), e.g. when a scene loads.
	void snap_to_settings();
	// Run the weather forward by p_seconds of weather time at once (wetting, drying, storms).
	void advance(float p_seconds);

	int add_storm(Vector2 p_position, float p_radius, float p_rain_rate, float p_outflow, float p_lifetime);
	void remove_storm(int p_id);
	void clear_storms();
	int get_storm_count() const { return storms.size(); }
	// The storm cells, and how far each has grown (0..1, its rise and decay over its lifetime).
	const Vector<Storm> &get_storms() const { return storms; }
	float get_storm_strength(const Storm &p_storm) const { return _storm_weight(p_storm); }
	// Haze from precipitation: each source (a SnowFall3D, rain) pushes its extinction (1/m), a height
	// scale for a layer near the ground (<= 0: uniform), its albedo, and optionally a centre and radius.
	// Sources add up. With drive_environment_fog, the uniform part goes into the environment's
	// exponential fog, lit by the sky (aerial perspective), and each layer into a lit FogVolume over
	// the ground; the path tracer reads both.
	void set_precipitation_extinction(Object *p_source, float p_sigma, float p_height_scale, float p_albedo = 0.95f, Vector3 p_centre = Vector3(), float p_radius = 0.0f);
	void clear_precipitation_extinction(Object *p_source);
	float get_precipitation_extinction_at(Vector3 p_position) const;
	void set_drive_environment_fog(bool p_enabled);
	bool get_drive_environment_fog() const { return drive_environment_fog; }

	// The cloud layer as the sky draws it now.
	WeatherClouds::State get_cloud_state() const { return _cloud_state(); }
	// The top of the cloud the sky draws over a point (world y, m): the base plus the column's
	// height for its cloud type, up to three times cloud_thickness in a storm's core.
	float get_cloud_top_at(Vector3 p_position) const;

	// Air velocity at a world position (m/s), mean wind on the log law plus gusts.
	// p_height_above_ground < 0 takes it from ground_height.
	Vector3 get_wind_at(Vector3 p_position, float p_height_above_ground = -1.0f) const;
	// Mean wind at 10 m, no gusts (x, z).
	// The mean wind aloft (x, z), for clouds and anything above the surface layer: the log law up to the top
	// of the boundary layer (boundary_layer_height), veering with height through the Ekman spiral (to the right
	// in the northern hemisphere, by about 20 degrees over the sea and 30 over land, Holton 2004), then the
	// free-air wind growing by wind_shear_aloft per km (the thermal wind). p_altitude: m above the ground.
	Vector2 get_wind_aloft(Vector3 p_position, float p_altitude) const;
	void set_boundary_layer_height(float p_height) { boundary_layer_height = MAX(p_height, 50.0f); }
	float get_boundary_layer_height() const { return boundary_layer_height; }
	void set_wind_shear_aloft(float p_shear) { wind_shear_aloft = p_shear; }
	float get_wind_shear_aloft() const { return wind_shear_aloft; }
	// p_terrain: the terrain's speed-up, lee and turning at 10 m above the ground (false: as over flat ground).
	Vector2 get_mean_wind_at(Vector3 p_position, bool p_terrain = true) const;
	// The parts of get_wind_at() for solvers that filter the turbulence themselves (TreeWind3D):
	// the mean air velocity (log law, shelter, gust patches), the along-wind direction of the gust
	// frame and the gust standard deviation there (m/s, shelter included).
	void get_wind_parts(Vector3 p_position, float p_height_above_ground, Vector3 &r_mean, Vector2 &r_along, float &r_sigma) const;
	// The gust waves: per wave (k along, k across) in the base wind's frame (rad/m), amplitude per
	// unit sigma and phase; wave i drives axis i / GUSTS_PER_AXIS (along, across, vertical). The
	// pattern moves with the base wind, so a wave's frequency is k along times get_base_wind_speed().
	void get_gust_waves(LocalVector<Vector4> &r_waves) const;
	Vector2 get_base_wind_direction() const { return cur_wind_direction; }
	float get_base_wind_speed() const { return cur_wind_speed; }
	float get_rain_at(Vector3 p_position) const; // mm/h
	float get_cloud_cover_at(Vector3 p_position) const;
	float get_cloud_type_at(Vector3 p_position) const;
	float get_wetness_at(Vector3 p_position) const; // 0..1
	float get_puddle_at(Vector3 p_position) const; // mm
	float get_cloud_density_at(Vector3 p_position) const; // extinction, 1/m
	float get_sun_visibility_at(Vector3 p_position) const; // 0..1, from the last cloud shadow bake

	// A read-only copy of the rain, cloud cover and cloud shadow with the parameters to sample them, for readers on
	// worker threads. Take it on the main thread. The arrays share the map's buffers (copy on write), so while a
	// snapshot is held each weather update copies them once. Its *_at() answer as get_rain_at(),
	// get_cloud_cover_at() and get_sun_visibility_at() did when it was taken.
	struct Snapshot {
		bool valid = false;
		bool origin_valid = false;
		int resolution = 0;
		float cell_size = 1.0f;
		Vector2i origin_cell;
		Vector<float> rain;
		Vector<float> cover;
		bool has_sun = false; // a sun light is set: the shadow bake, not the cover, dims the sun
		Vector3 shadow_axis_u;
		Vector3 shadow_axis_v;
		Vector2 shadow_corner; // with the cloud drift since the bake applied
		float shadow_size = 0.0f;
		int shadow_res = 0;
		Vector<float> shadow_values;

		float rain_at(Vector3 p_position) const;
		float cloud_cover_at(Vector3 p_position) const;
		float sun_visibility_at(Vector3 p_position) const;
	};
	Snapshot get_snapshot() const;

	// The weather contract (docs/ideas/world-contracts.md).
	float contract_air_temperature_at(const Vector3 &p_position) const override { return get_air_temperature_at(p_position); }
	float contract_air_humidity_at(const Vector3 &p_position) const override { return get_air_humidity_at(p_position); }
	Vector3 contract_wind_at(const Vector3 &p_position, float p_height_above_ground) const override { return get_wind_at(p_position, p_height_above_ground); }
	float contract_precipitation_at(const Vector3 &p_position) const override { return get_rain_at(p_position); }
	float contract_rain_share_at(const Vector3 &p_position) const override { return rain_fraction(get_air_temperature_at(p_position), get_air_humidity_at(p_position)); }
	float contract_surface_wetness_at(const Vector3 &p_position) const override { return get_wetness_at(p_position); }
	float contract_puddle_at(const Vector3 &p_position) const override { return get_puddle_at(p_position); }
	float contract_contact_wetness_at(const Vector3 &p_position) const override { return get_contact_wetness_at(p_position); }
	void contract_add_contact_wetness(const Vector3 &p_position, float p_radius, float p_amount) override { add_contact_wetness(p_position, p_radius, p_amount); }
	float contract_sun_visibility_at(const Vector3 &p_position) const override { return get_sun_visibility_at(p_position); }
	float contract_canopy_closure_at(const Vector3 &p_position) const override { return get_canopy_closure_at(p_position); }
	float contract_frost_at(const Vector3 &p_position) const override { return get_frost_at(p_position); }
	Vector3 contract_mean_wind_at(const Vector3 &p_position) const override {
		const Vector2 m = get_mean_wind_at(p_position);
		return Vector3(m.x, 0.0f, m.y);
	}
	Vector3 contract_wind_aloft_at(const Vector3 &p_position, float p_altitude) const override {
		const Vector2 m = get_wind_aloft(p_position, p_altitude);
		return Vector3(m.x, 0.0f, m.y);
	}

	void set_sky_clock(const NodePath &p_path);
	NodePath get_sky_clock() const { return sky_clock; }
	void set_season_day(int p_day);
	int get_season_day() const { return season_day; }
	void set_climate(const Ref<Resource> &p_climate);
	Ref<Resource> get_climate() const { return climate; }
	float get_season_anomaly() const { return season_anomaly; }
	void set_season_anomaly(float p_kelvin);
	int get_day_of_year() const;
	WeatherSeason::Climate get_season_climate() const { return _season_climate(); } // for C++ users (the ground's regrowth)
	Dictionary get_season();
	Dictionary get_season_at_height(float p_height) const;
	// The season on another day of the year (1-365) at the ground, for rates such as leaf fall.
	Dictionary get_season_on_day(int p_day) const;
	// Bakes the cloud shadow now and waits for it (normally it runs in the background).
	void update_cloud_shadow();
	double get_weather_time() const { return weather_time; }
	// For followers: how many times the base was snapped, whether the cells have been refreshed since
	// (a sample taken before reads the old state), and the weather seconds advance() has skipped.
	uint64_t get_snap_serial() const { return snap_serial; }
	bool are_cells_fresh_since_snap() const { return cells_serial > snap_cells_serial; }
	double get_advanced_time() const { return advanced_time; }

	Ref<Texture2D> get_wind_texture() const { return wind_texture; }
	Ref<Texture2D> get_state_texture() const { return state_texture; }
	Ref<Texture2D> get_gust_texture() const { return gust_texture; }
	Ref<Texture2D> get_cloud_shadow_texture() const { return shadow_texture; }
	Rect2 get_map_rect() const;

	WeatherMap3D();
	~WeatherMap3D();
};

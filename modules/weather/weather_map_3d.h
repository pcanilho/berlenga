/**************************************************************************/
/*  weather_map_3d.h                                                      */
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

#include "weather_clouds.h"
#include "weather_shelter.h"

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

class WeatherMap3D : public Node3D {
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
	};
	CellPass cell_pass;

	Gust gusts[GUST_COUNT];
	float gust_sigma_scale = 1.0f; // turbulence intensity of the base wind, for shaders
	double weather_time = 0.0;
	double update_accum = 0.0;
	Vector2 cloud_drift; // m, how far the cloud noise has drifted

	Ref<Image> wind_image;
	Ref<Image> state_image;
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

	// Wind near the ground: canopy and obstacle shelter on a fine square, and gust patches.
	WeatherShelter shelter;
	bool shelter_enabled = true;
	Ref<Texture2D> canopy_map;
	float gust_patch_size = 6.0f; // m
	float gust_patch_strength = 0.35f; // standard deviation of the patches over the mean wind
	Vector2 patch_drift; // m, how far the patches have travelled (wrapped to their period)
	bool shelter_global_added = false;

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
	void _snap();
	void _fill_gust_image();
	void _upload();
	void _publish_globals();
	void _set_global(const StringName &p_name, const Variant &p_value, int p_type);
	Vector3 _follow_position() const;

	float _cell_size() const { return extent / resolution; }
	float _sample(const Vector<float> &p_field, Vector2 p_xz, int p_stride = 1, int p_offset = 0) const;
	float _cloud_noise(Vector2 p_xz) const;
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
	// The eased state the cells use (the settings are its targets).
	float get_air_temperature() const { return cur_temperature; }
	float get_air_humidity() const { return cur_humidity; }
	// Share of precipitation that falls as rain at an air temperature and relative humidity (0..1), the
	// complement of the snowfall fraction of Jennings et al. (2018).
	static float rain_fraction(float p_celsius, float p_humidity);
	void set_humidity(float p_humidity);
	float get_humidity() const { return humidity; }
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

	void set_cloud_base_height(float p_height);
	float get_cloud_base_height() const { return cloud_base_height; }
	void set_cloud_thickness(float p_thickness);
	float get_cloud_thickness() const { return cloud_thickness; }
	void set_cloud_density(float p_density);
	float get_cloud_density() const { return cloud_density; }
	void set_cloud_shape_scale(float p_scale);
	float get_cloud_shape_scale() const { return cloud_shape_scale; }
	void set_sun_light(const NodePath &p_path);
	NodePath get_sun_light() const { return sun_light; }
	void set_cloud_shadow_extent(float p_extent);
	float get_cloud_shadow_extent() const { return cloud_shadow_extent; }
	void set_cloud_shadow_resolution(int p_resolution);
	int get_cloud_shadow_resolution() const { return cloud_shadow_resolution; }
	void set_cloud_shadow_interval(float p_seconds);
	float get_cloud_shadow_interval() const { return cloud_shadow_interval; }

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
	Vector2 get_mean_wind_at(Vector3 p_position) const;
	float get_rain_at(Vector3 p_position) const; // mm/h
	float get_cloud_cover_at(Vector3 p_position) const;
	float get_cloud_type_at(Vector3 p_position) const;
	float get_wetness_at(Vector3 p_position) const; // 0..1
	float get_puddle_at(Vector3 p_position) const; // mm
	float get_cloud_density_at(Vector3 p_position) const; // extinction, 1/m
	float get_sun_visibility_at(Vector3 p_position) const; // 0..1, from the last cloud shadow bake
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

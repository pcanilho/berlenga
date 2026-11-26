/**************************************************************************/
/*  world_contracts.h                                                     */
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

// World contracts (docs/ideas/world-contracts.md): the interfaces between world systems. A producer (WeatherMap3D)
// implements the plain C++ provider interface and registers itself; consumers ask WorldContracts for the contract
// object, which forwards to the provider, or to a script stub a test has set, or returns the documented default
// when there is neither. Consumers never include the producer's module.

#include "core/object/gdvirtual.gen.h"
#include "core/object/object.h"
#include "core/object/ref_counted.h"
#include "core/math/vector2.h"
#include "core/math/vector3.h"
#include "core/variant/dictionary.h"
#include "core/templates/local_vector.h"
#include "core/templates/hash_map.h"
#include "world_season.h"

class Node;
class Node3D;

// Weather: what the air, the sky and the surface water are doing at a point.
class WorldWeatherProvider {
public:
	virtual float contract_air_temperature_at(const Vector3 &p_position) const = 0; // C
	virtual float contract_air_humidity_at(const Vector3 &p_position) const = 0; // 0..1
	virtual Vector3 contract_wind_at(const Vector3 &p_position, float p_height_above_ground) const = 0; // m/s
	virtual float contract_precipitation_at(const Vector3 &p_position) const = 0; // mm/h of water
	virtual float contract_rain_share_at(const Vector3 &p_position) const = 0; // 0..1
	virtual float contract_surface_wetness_at(const Vector3 &p_position) const = 0; // 0..1
	virtual float contract_puddle_at(const Vector3 &p_position) const = 0; // mm
	virtual float contract_contact_wetness_at(const Vector3 &p_position) const = 0; // 0..1
	virtual void contract_add_contact_wetness(const Vector3 &p_position, float p_radius, float p_amount) = 0;
	virtual float contract_sun_visibility_at(const Vector3 &p_position) const = 0; // 0..1
	virtual float contract_canopy_closure_at(const Vector3 &p_position) const = 0; // 0..1
	virtual float contract_frost_at(const Vector3 &p_position) const { return 0.0f; } // mm
	virtual float contract_glaze_at(const Vector3 &p_position) const { return 0.0f; } // mm of radial clear ice on exposed twigs
	virtual Vector3 contract_rime_at(const Vector3 &p_position) const { return Vector3(); } // mm of rime on exposed twigs, along the way it grew (into the wind)
	virtual float contract_valley_fog_top_at(const Vector3 &p_position) const { return -Math::INF; } // m, the top of valley (radiation) fog over the point
	virtual Vector3 contract_mean_wind_at(const Vector3 &p_position) const { return Vector3(); } // m/s at 10 m, no gusts
	virtual Vector3 contract_wind_aloft_at(const Vector3 &p_position, float p_altitude) const { return Vector3(); } // m/s, free air
	virtual float contract_terrain_lee_at(const Vector3 &p_position) const { return NAN; } // rad, Winstral's Sx; NaN: no terrain wind there
	virtual float contract_rainfall_at(const Vector3 &p_position) const { return 0.0f; } // mm/h of liquid
	virtual float contract_snowfall_at(const Vector3 &p_position) const { return 0.0f; } // mm/h of water falling as snow
	virtual float contract_smoke_at(const Vector3 &p_position) const { return 0.0f; } // g/m2 of smoke particulate in the column
	virtual float contract_smoke_concentration_at(const Vector3 &p_position) const { return contract_smoke_at(p_position) / 2000.0f; } // g/m3 at the point (mixed through a 2 km layer unless the producer knows better)
	virtual float contract_dust_at(const Vector3 &p_position) const { return 0.0f; } // g/m2 of mineral dust in the column (dust storms, haboobs)
	virtual float contract_dust_top_at(const Vector3 &p_position) const { return 0.0f; } // m above the ground, the dust layer's top
	virtual float contract_dust_concentration_at(const Vector3 &p_position) const { return 0.0f; } // g/m3 at the point
	virtual float contract_obstacle_height_at(const Vector3 &p_position) const { return 0.0f; } // m, wind-blocking obstacles over the ground
	virtual float contract_obstacle_lee_at(const Vector3 &p_position) const { return 1.0f; } // 0..1, near-ground wind behind obstacles over the open
	virtual float contract_fog_water_at(const Vector3 &p_position) const { return 0.0f; } // g/m3 of cloud water at the ground
	virtual float contract_fog_drip_at(const Vector3 &p_position) const { return 0.0f; } // mm/h of fog water dripping off the canopy
	virtual float contract_sea_spray_at(const Vector3 &p_position) const { return 0.0f; } // mm/h of sea spray settling
	virtual float contract_salt_at(const Vector3 &p_position) const { return 0.0f; } // g/m2 of sea salt on surfaces
	virtual float contract_canopy_water_at(const Vector3 &p_position) const { return 0.0f; } // mm held on leaves
	virtual float contract_canopy_wetness_at(const Vector3 &p_position) const { return 0.0f; } // 0..1
	virtual float contract_canopy_drip_at(const Vector3 &p_position) const { return 0.0f; } // mm/h
	virtual float contract_ground_water_input_at(const Vector3 &p_position) const { return 0.0f; } // mm/h reaching the ground
	// The calendar and climate (world_season.h): the day of the year (1..365), the climate for the place (false:
	// none), and the season's state at a point's height on a day (p_day < 1: today), the live departure included.
	virtual int contract_day_of_year() const { return 135; }
	virtual bool contract_season_climate(WeatherSeason::Climate &r_climate) const { return false; }
	virtual Dictionary contract_season_at(const Vector3 &p_position, int p_day) const;
	virtual float contract_cloud_cover_at(const Vector3 &p_position) const { return 0.0f; } // 0..1
	virtual float contract_high_cloud_cover_at(const Vector3 &p_position) const { return 0.0f; } // 0..1, the high (cirrus) sheet
	virtual float contract_mid_cloud_cover_at(const Vector3 &p_position) const { return 0.0f; } // 0..1, the mid (altocumulus) sheet
	virtual float contract_cloud_effector_at(const Vector3 &p_position) const { return 0.0f; } // -1..1, cloud added (> 0) or cleared by effectors
	virtual float contract_cloud_base_at(const Vector3 &p_position) const { return 1500.0f; } // m above y = 0, the low cloud's base
	// The orographic factor the weather's precipitation_at already carries at the point (1: none applied).
	virtual float contract_orographic_factor_at(const Vector3 &p_position) const { return 1.0f; }
	virtual float contract_mountain_wave_at(const Vector3 &p_position) const { return 0.0f; } // m, the air's vertical displacement in mountain waves
	virtual float contract_mountain_wave_velocity_at(const Vector3 &p_position) const { return 0.0f; } // m/s, their vertical wind
	virtual float contract_wave_cloud_density_at(const Vector3 &p_position) const { return 0.0f; } // 1/m, wave cloud (lenticular) extinction
	virtual float contract_water_temperature() const { return 15.0f; } // C, open water (lakes lag the air)
	virtual float contract_surface_temperature_at(const Vector3 &p_position) const { return contract_air_temperature_at(p_position); } // C, exposed ground
	virtual float contract_ground_height_at(const Vector3 &p_position) const { return 0.0f; } // m, the terrain the weather sees
	virtual double contract_time_scale() const { return 1.0; } // the weather's time per real second
	virtual ~WorldWeatherProvider() {}
};

class WorldWeather : public RefCounted {
	GDCLASS(WorldWeather, RefCounted);

	friend class WorldContracts;
	WorldWeatherProvider *provider = nullptr; // the built-in contract's; a script stub overrides the virtuals

protected:
	static void _bind_methods();

	GDVIRTUAL1RC(float, _air_temperature_at, Vector3)
	GDVIRTUAL1RC(float, _air_humidity_at, Vector3)
	GDVIRTUAL2RC(Vector3, _wind_at, Vector3, float)
	GDVIRTUAL1RC(float, _precipitation_at, Vector3)
	GDVIRTUAL1RC(float, _rain_share_at, Vector3)
	GDVIRTUAL1RC(float, _surface_wetness_at, Vector3)
	GDVIRTUAL1RC(float, _puddle_at, Vector3)
	GDVIRTUAL1RC(float, _contact_wetness_at, Vector3)
	GDVIRTUAL3(_add_contact_wetness, Vector3, float, float)
	GDVIRTUAL1RC(float, _sun_visibility_at, Vector3)
	GDVIRTUAL1RC(float, _canopy_closure_at, Vector3)
	GDVIRTUAL1RC(float, _frost_at, Vector3)
	GDVIRTUAL1RC(float, _glaze_at, Vector3)
	GDVIRTUAL1RC(Vector3, _rime_at, Vector3)
	GDVIRTUAL1RC(float, _valley_fog_top_at, Vector3)
	GDVIRTUAL1RC(Vector3, _mean_wind_at, Vector3)
	GDVIRTUAL2RC(Vector3, _wind_aloft_at, Vector3, float)
	GDVIRTUAL1RC(float, _terrain_lee_at, Vector3)
	GDVIRTUAL1RC(float, _rainfall_at, Vector3)
	GDVIRTUAL1RC(float, _snowfall_at, Vector3)
	GDVIRTUAL1RC(float, _smoke_at, Vector3)
	GDVIRTUAL1RC(float, _smoke_concentration_at, Vector3)
	GDVIRTUAL1RC(float, _dust_at, Vector3)
	GDVIRTUAL1RC(float, _dust_top_at, Vector3)
	GDVIRTUAL1RC(float, _dust_concentration_at, Vector3)
	GDVIRTUAL1RC(float, _obstacle_height_at, Vector3)
	GDVIRTUAL1RC(float, _obstacle_lee_at, Vector3)
	GDVIRTUAL1RC(float, _fog_water_at, Vector3)
	GDVIRTUAL1RC(float, _fog_drip_at, Vector3)
	GDVIRTUAL1RC(float, _sea_spray_at, Vector3)
	GDVIRTUAL1RC(float, _salt_at, Vector3)
	GDVIRTUAL1RC(float, _canopy_water_at, Vector3)
	GDVIRTUAL1RC(float, _canopy_wetness_at, Vector3)
	GDVIRTUAL1RC(float, _canopy_drip_at, Vector3)
	GDVIRTUAL1RC(float, _ground_water_input_at, Vector3)
	GDVIRTUAL0RC(int, _day_of_year)
	GDVIRTUAL2RC(Dictionary, _season_at, Vector3, int)
	GDVIRTUAL1RC(float, _cloud_cover_at, Vector3)
	GDVIRTUAL1RC(float, _high_cloud_cover_at, Vector3)
	GDVIRTUAL1RC(float, _mid_cloud_cover_at, Vector3)
	GDVIRTUAL1RC(float, _cloud_effector_at, Vector3)
	GDVIRTUAL1RC(float, _cloud_base_at, Vector3)
	GDVIRTUAL1RC(float, _orographic_factor_at, Vector3)
	GDVIRTUAL1RC(float, _mountain_wave_at, Vector3)
	GDVIRTUAL1RC(float, _mountain_wave_velocity_at, Vector3)
	GDVIRTUAL1RC(float, _wave_cloud_density_at, Vector3)
	GDVIRTUAL0RC(float, _water_temperature)
	GDVIRTUAL1RC(float, _surface_temperature_at, Vector3)
	GDVIRTUAL1RC(float, _ground_height_at, Vector3)
	GDVIRTUAL0RC(double, _time_scale)

public:
	// Defaults with no provider: still, mild, dry, open sky.
	static constexpr float DEFAULT_AIR_TEMPERATURE = 15.0f;
	static constexpr float DEFAULT_AIR_HUMIDITY = 0.7f;

	bool has_provider() const { return provider != nullptr || get_script_instance() != nullptr; } // a script stub (set_*) counts
	float air_temperature_at(const Vector3 &p_position) const;
	float air_humidity_at(const Vector3 &p_position) const;
	float dew_point_at(const Vector3 &p_position) const;
	Vector3 wind_at(const Vector3 &p_position, float p_height_above_ground = -1.0f) const;
	float precipitation_at(const Vector3 &p_position) const;
	float rain_share_at(const Vector3 &p_position) const;
	float surface_wetness_at(const Vector3 &p_position) const;
	float puddle_at(const Vector3 &p_position) const;
	float contact_wetness_at(const Vector3 &p_position) const;
	void add_contact_wetness(const Vector3 &p_position, float p_radius, float p_amount = 1.0f);
	float sun_visibility_at(const Vector3 &p_position) const;
	float canopy_closure_at(const Vector3 &p_position) const;
	float frost_at(const Vector3 &p_position) const;
	// Glaze (freezing rain): the radial thickness of clear ice on exposed twigs and wires at the point (mm), as an ice
	// storm leaves it and until it melts or sheds. An ice storm is 6 mm and more; 12-25 mm breaks limbs. 0 with no
	// weather system or none. Readers: trees (load, droop, breaks), wires, surfaces.
	float glaze_at(const Vector3 &p_position) const;
	// Rime on exposed twigs and edges (supercooled cloud freezing on contact, Makkonen 2000): its length is the
	// deposit in mm, its direction horizontal and into the wind it grew from (the feathers point that way).
	Vector3 rime_at(const Vector3 &p_position) const;
	// Valley fog: the absolute height of the radiation fog's top over the point (it fills the cold pools of clear calm
	// nights where their air reaches its dew point), -INF where there is none.
	float valley_fog_top_at(const Vector3 &p_position) const;
	Vector3 mean_wind_at(const Vector3 &p_position) const;
	Vector3 wind_aloft_at(const Vector3 &p_position, float p_altitude) const;
	float terrain_lee_at(const Vector3 &p_position) const;
	float rainfall_at(const Vector3 &p_position) const;
	float snowfall_at(const Vector3 &p_position) const;
	float smoke_at(const Vector3 &p_position) const; // g/m2 of smoke particulate over the point (the haze layer)
	float smoke_concentration_at(const Vector3 &p_position) const; // g/m3 at the point: the column over its mixing depth (a night's cold pool holds smoke near the ground)
	float dust_at(const Vector3 &p_position) const; // g/m2 of mineral dust over the point (dust storms, haboobs)
	float dust_top_at(const Vector3 &p_position) const; // m above the ground, the dust layer's top (a haboob's nose included)
	float dust_concentration_at(const Vector3 &p_position) const; // g/m3 at the point
	// Rocks, walls, buildings and vehicles in the wind: their height over the ground at the point (m, 0 in the
	// open), and the lee of those upwind near the ground (the wind there over the open wind, 0.15 in the
	// cavity behind one, back to 1 about 25 heights downwind; a point raised over the ground, such as a drift's
	// surface, has less, none at 1.2 heights). Snow and sand drift into the lee.
	float obstacle_height_at(const Vector3 &p_position) const;
	float obstacle_lee_at(const Vector3 &p_position) const;
	// Fog at the ground (g/m3 of cloud water: the terrain standing in the cloud), and the part of it the canopy combs
	// out and drips onto the ground (mm/h; already in ground_water_input_at).
	float fog_water_at(const Vector3 &p_position) const;
	float fog_drip_at(const Vector3 &p_position) const;
	// Sea spray settling at the point (mm/h of sea water), blown inland from breaking waves.
	float sea_spray_at(const Vector3 &p_position) const;
	// Sea salt left on surfaces at the point (g/m2): spray and fine aerosol deposited, washed off by rain.
	float salt_at(const Vector3 &p_position) const;
	float canopy_water_at(const Vector3 &p_position) const;
	float canopy_wetness_at(const Vector3 &p_position) const;
	float canopy_drip_at(const Vector3 &p_position) const;
	float ground_water_input_at(const Vector3 &p_position) const;
	int day_of_year() const;
	// C++ only: the climate (false with no provider or none set); its math is in world_season.h.
	bool season_climate(WeatherSeason::Climate &r_climate) const;
	Dictionary season_at(const Vector3 &p_position, int p_day = -1) const;
	float cloud_cover_at(const Vector3 &p_position) const;
	float high_cloud_cover_at(const Vector3 &p_position) const;
	float mid_cloud_cover_at(const Vector3 &p_position) const;
	float cloud_effector_at(const Vector3 &p_position) const;
	float cloud_base_at(const Vector3 &p_position) const;
	// The orographic factor precipitation_at already carries at the point (the terrain climate's precipitation
	// there over its regional mean: more on windward slopes and heights, a rain shadow in the lee; Smith and Barstad
	// 2004): 1 where none is applied. A reader with its own orographic field divides this out so it is not counted
	// twice.
	float orographic_factor_at(const Vector3 &p_position) const;
	float mountain_wave_at(const Vector3 &p_position) const;
	float mountain_wave_velocity_at(const Vector3 &p_position) const;
	float wave_cloud_density_at(const Vector3 &p_position) const;
	float water_temperature() const;
	float surface_temperature_at(const Vector3 &p_position) const;
	float ground_height_at(const Vector3 &p_position) const;
	double time_scale() const;
	// Below-cloud scavenging (washout) of particles in the air by falling rain and snow, 1/s: Lambda = a R^b
	// (R mm/h of water). Fine smoke (PM2.5, accumulation mode) about 1e-5 at 1 mm/h (Laakso et al. 2003; Wang et
	// al. 2010); coarse dust (> 10 um) about 3e-4 (Wang et al. 2010); snow about five times rain for the same water
	// (Kyroe et al. 2009; Zhang et al. 2013). washout_at reads rainfall_at and snowfall_at.
	static float washout_rate(float p_rain_mm_h, float p_snow_mm_h, bool p_coarse);
	float washout_at(const Vector3 &p_position, bool p_coarse) const;
};

// Water: open water of every kind (sea, rivers, pools, live water, standing water on the ground). Several
// producers are live at once; the contract answers with the highest surface over a point.
class WorldWaterProvider {
public:
	// The water surface over the point (m, world space), or -INF where this producer has no water there.
	virtual float contract_water_height_at(const Vector3 &p_position) const = 0;
	virtual Vector3 contract_water_flow_at(const Vector3 &p_position) const { return Vector3(); } // m/s
	virtual float contract_water_wake_depth_at(const Vector3 &p_position) const { return 0.0f; } // m
	// A body moving in the water: owner-keyed so it keeps one moving disturbance; impulse for a single splash.
	virtual void contract_water_disturb(uint64_t p_owner, const Vector3 &p_position, float p_radius, float p_strength) {}
	virtual void contract_water_clear(uint64_t p_owner) {}
	virtual void contract_water_impulse(const Vector3 &p_position, float p_radius, float p_strength) {}
	// An impulse wave in metres and seconds (a landslide, avalanche, calving or rockfall entering the water): one
	// crest and trough p_height high (crest to trough), p_period long, its crest p_width wide, setting off from the
	// point along p_direction. A producer that cannot carry one gets a splash of the crest's height there.
	virtual void contract_water_wave(const Vector3 &p_position, const Vector3 &p_direction, float p_height, float p_period, float p_width) {
		const float r = MAX(0.5f * p_width, 0.5f);
		contract_water_impulse(p_position, r, 0.5f * MAX(p_height, 0.0f) / r);
	}
	// Suspended load (kg/m3): x mineral (silt, clay: brown), y ash and char (grey to black). Zero for clear water.
	virtual Vector2 contract_water_sediment_at(const Vector3 &p_position) const { return Vector2(); }
	// What settled out of this water onto its bed at the point (kg/m2): x mineral, y ash. Stays after the water falls.
	virtual Vector2 contract_water_sediment_deposit_at(const Vector3 &p_position) const { return Vector2(); }
	// Height of the waves breaking at the point (m): the broken waves in the surf zone, whitecaps (cover times wave
	// height) over open water; 0 where none break.
	virtual float contract_water_breaking_at(const Vector3 &p_position) const { return 0.0f; }
	// The last flood's high-water mark over the point: x the highest surface it reached (m, world), y the days since
	// that peak, z and w the flow's direction at the peak (unit, x and z; zero if not known); (-INF, -1, 0, 0) where
	// this producer kept none. Readers: wrack lines, silt lines, flood-scoured plants.
	virtual Vector4 contract_water_high_water_at(const Vector3 &p_position) const { return Vector4(-Math::INF, -1.0f, 0.0f, 0.0f); }
	// A load entering this water at the point (kg/s of each), from runoff, banks or a landslide; it carries it on.
	virtual void contract_water_add_sediment(const Vector3 &p_position, float p_mineral_kg_s, float p_ash_kg_s) {}
	// A load dropped into this water at once (kg of each): a debris flow's or a landslide's fines, a bank's collapse.
	virtual void contract_water_add_sediment_mass(const Vector3 &p_position, float p_mineral_kg, float p_ash_kg) {}
	virtual ~WorldWaterProvider() {}
};

class WorldWater : public RefCounted {
	GDCLASS(WorldWater, RefCounted);

	friend class WorldContracts;
	LocalVector<WorldWaterProvider *> providers; // every live producer; a script stub overrides the virtuals

	WorldWaterProvider *_top_at(const Vector3 &p_position, float &r_height) const;

protected:
	static void _bind_methods();

	GDVIRTUAL1RC(float, _height_at, Vector3)
	GDVIRTUAL1RC(Vector3, _flow_at, Vector3)
	GDVIRTUAL1RC(float, _wake_depth_at, Vector3)
	GDVIRTUAL4(_disturb, uint64_t, Vector3, float, float)
	GDVIRTUAL1(_clear, uint64_t)
	GDVIRTUAL3(_impulse, Vector3, float, float)
	GDVIRTUAL5(_wave, Vector3, Vector3, float, float, float)
	GDVIRTUAL1RC(Vector2, _sediment_at, Vector3)
	GDVIRTUAL1RC(Vector2, _sediment_deposit_at, Vector3)
	GDVIRTUAL1RC(float, _breaking_at, Vector3)
	GDVIRTUAL1RC(Vector4, _high_water_at, Vector3)
	GDVIRTUAL3(_add_sediment, Vector3, float, float)
	GDVIRTUAL3(_add_sediment_mass, Vector3, float, float)

public:
	bool has_provider() const { return !providers.is_empty() || get_script_instance() != nullptr; } // a script stub (set_*) counts
	// The water surface for the point, -INF where there is none: of the producers with water there, the surface
	// nearest above the point, else the nearest below it.
	float height_at(const Vector3 &p_position) const;
	// How deep the point lies under that surface (0 above it or with no water).
	float depth_at(const Vector3 &p_position) const;
	Vector3 flow_at(const Vector3 &p_position) const;
	float wake_depth_at(const Vector3 &p_position) const;
	void disturb(uint64_t p_owner, const Vector3 &p_position, float p_radius, float p_strength);
	void clear(uint64_t p_owner);
	void impulse(const Vector3 &p_position, float p_radius, float p_strength);
	// An impulse wave (m and s) from the point along the direction, given to every producer with water there: a
	// landslide's, an avalanche's or calving ice's (height crest to trough, its period, the crest's width).
	void wave(const Vector3 &p_position, const Vector3 &p_direction, float p_height, float p_period, float p_width);
	// The water's suspended load at the point (kg/m3): x mineral, y ash and char; the shading clouds and darkens by it.
	Vector2 sediment_at(const Vector3 &p_position) const;
	// Silt and ash the water has laid on its bed at the point (kg/m2, every producer's summed), there whether or not
	// water still covers it: slack water by a river's banks and in its pools, a flood's leavings.
	Vector2 sediment_deposit_at(const Vector3 &p_position) const;
	// Height of the waves breaking at the point (m), the largest any producer reports; 0 where none break.
	float breaking_at(const Vector3 &p_position) const;
	// The last flood's high-water mark: x the highest surface reached there (m, world), y days since that peak, z and
	// w the peak flow's direction (unit x, z; zero if not known); (-INF, -1, 0, 0) where no producer kept one. The
	// highest mark of all producers answers.
	Vector4 high_water_at(const Vector3 &p_position) const;
	// A load entering the water that answers at the point (kg/s of mineral and of ash); none there: it is lost.
	void add_sediment(const Vector3 &p_position, float p_mineral_kg_s, float p_ash_kg_s);
	// A load dropped into that water at once (kg of mineral and of ash): mixed into the water around the point.
	void add_sediment_mass(const Vector3 &p_position, float p_mineral_kg, float p_ash_kg);
	// C++: the water over the point from every producer but p_skip (a producer reading the others' water, such as
	// the ground's own standing water), with its flow and suspended load; -INF with none.
	float height_except(const Vector3 &p_position, const WorldWaterProvider *p_skip, Vector3 *r_flow = nullptr, Vector2 *r_sediment = nullptr) const;
};

// Fire: what burns where, for what it heats (snow, the surface water, the air) and chars.
class WorldFireProvider {
public:
	virtual float contract_fire_heat_at(const Vector3 &p_position) const = 0; // W/m2 of ground, now
	virtual float contract_fire_burnt_at(const Vector3 &p_position) const { return 0.0f; } // 0..1
	virtual Vector3 contract_fire_wind_at(const Vector3 &p_position) const { return Vector3(); } // m/s, the fire's indraft near the ground
	virtual float contract_fire_stem_char_at(const Vector3 &p_position) const { return 0.0f; } // m, char a stem foot there took
	virtual float contract_fire_burn_age_at(const Vector3 &p_position) const { return -1.0f; } // years since it last burnt there, -1 never
	virtual float contract_fire_crown_scorch_at(const Vector3 &p_position) const { return 0.0f; } // m, crowns killed up to this height
	virtual float contract_fire_water_repellency_at(const Vector3 &p_position) const { return 0.0f; } // 0..1, burnt soil shedding rain
	virtual void contract_fire_plumes(PackedVector4Array &r_plumes) const {} // appends its convection columns: (x, y, z of the base, convective heat W)
	virtual void contract_fire_smoke(PackedVector4Array &r_sources) const {} // appends its smoke: (x, y, z, kg/s of particulate, its own real time)
	virtual ~WorldFireProvider() {}
};

class WorldFire : public RefCounted {
	GDCLASS(WorldFire, RefCounted);

	friend class WorldContracts;
	LocalVector<WorldFireProvider *> providers;
	// Burnt ground changes, for readers that cache burn_age_at() (regrowth): each report bumps the generation and
	// logs the area; a reader asks for the union since the generation it last saw.
	struct BurnChange {
		uint64_t generation = 0;
		Rect2 area;
	};
	static constexpr int BURN_LOG = 64;
	uint64_t burn_generation = 0;
	LocalVector<BurnChange> burn_log;

protected:
	static void _bind_methods();

	GDVIRTUAL1RC(float, _heat_at, Vector3)
	GDVIRTUAL1RC(float, _burnt_at, Vector3)
	GDVIRTUAL1RC(Vector3, _wind_at, Vector3)
	GDVIRTUAL1RC(float, _stem_char_at, Vector3)
	GDVIRTUAL1RC(float, _burn_age_at, Vector3)
	GDVIRTUAL1RC(float, _crown_scorch_at, Vector3)
	GDVIRTUAL1RC(float, _water_repellency_at, Vector3)
	GDVIRTUAL0RC(PackedVector4Array, _plumes)
	GDVIRTUAL0RC(PackedVector4Array, _smoke_sources)

public:
	bool has_provider() const { return !providers.is_empty() || get_script_instance() != nullptr; } // a script stub (set_*) counts
	// The fire's heat release over the ground at the point (W/m2), the most of any fire there; 0 with none.
	float heat_at(const Vector3 &p_position) const;
	// How burnt the ground is there, 0..1.
	float burnt_at(const Vector3 &p_position) const;
	// The wind the fires draw in near the ground at the point (their indraft, m/s, horizontal), summed over the fires;
	// it adds to the weather's wind. Zero with none.
	Vector3 wind_at(const Vector3 &p_position) const;
	// How deep a stem standing at the point has charred at its foot (m, bark and wood): the flames' and smouldering
	// logs' time there at a charring rate of 0.8 mm a minute. The most of any fire; 0 where none burnt.
	float stem_char_at(const Vector3 &p_position) const;
	// Years since the ground at the point last burnt (game time), for regrowth: the most recent of any producer (a fire
	// in play, an authored WorldBurnScar3D); -1 where it never burnt.
	float burn_age_at(const Vector3 &p_position) const;
	// How high the fire killed tree crowns over the point (m): Van Wagner's lethal scorch height of the front that
	// passed, or a crown that burnt (1000). The most of any fire; 0 where none burnt.
	float crown_scorch_at(const Vector3 &p_position) const;
	// How water-repellent the fire left the soil at the point, 0..1 (1: it takes almost no rain in; the ground's
	// infiltration falls by up to nine tenths). Fades over a year or two. The most of any fire; 0 where none burnt.
	float water_repellency_at(const Vector3 &p_position) const;
	PackedVector4Array plumes() const; // every fire's convection columns (x, y, z, convective heat W)
	PackedVector4Array smoke_sources() const; // every fire's smoke (x, y, z, kg/s of particulate)
	// Producers report where burn_age_at() changed (x, z in metres; a burn scar placed, moved or aged, ground a
	// live fire burnt). Report as the ground burns, at most a few times a second (union the cells in between).
	// WorldContracts::fire_burn_changed() reports to the built-in contract and any stub.
	void burn_changed(const Rect2 &p_area);
	uint64_t get_burn_generation() const { return burn_generation; }
	// The union of the areas reported after generation p_since into r_area; false when the log no longer reaches
	// back that far (read everything again). True with an empty r_area when nothing changed.
	bool burn_changes_since(uint64_t p_since, Rect2 &r_area) const;
	// The same for scripts and tests: {"complete": bool, "area": Rect2}.
	Dictionary get_burn_changes_since(uint64_t p_since) const;
};

// Stand age: years since the last stand-replacing disturbance over the ground (docs/ideas/world-contracts.md, Stand).
// The contract itself keeps it: fire comes from WorldFire (burn_age_at, unless crown_scorch_at says only a surface
// fire passed), wind and beetle kills from events the trees report (add_disturbance). Readers: vegetation (stand age
// and disturbance rules), trees (succession).
class WorldStand : public RefCounted {
	GDCLASS(WorldStand, RefCounted);

public:
	enum Disturbance {
		DISTURBANCE_NONE = 0,
		DISTURBANCE_FIRE = 1,
		DISTURBANCE_WIND = 2,
		DISTURBANCE_BEETLE = 3,
		DISTURBANCE_BEAVER = 4, // a beaver pond drained (age: years since)
		DISTURBANCE_ROOTING = 5, // boar rooting turned the soil (age: fractional years since); ground layer only, see rooting_age_at; new kinds are appended only
		DISTURBANCE_AVALANCHE = 6, // a dense avalanche broke the stand (SnowAvalanche3D stand_damage; age: years since)
		DISTURBANCE_ICE_STORM = 7, // freezing rain loaded the crowns past breaking: broken tops and limbs, bent birch (trees STAND_ICE_STORM; age: years since)
	};
	// A surface fire that scorched crowns only this low (m) leaves the stand standing.
	static constexpr float SURFACE_FIRE_SCORCH = 5.0f;

private:
	friend class WorldContracts;
	struct Event {
		int id = 0;
		Vector2 center;
		float radius = 0.0f;
		int kind = DISTURBANCE_NONE;
		float age = 0.0f; // years
	};
	LocalVector<Event> events;
	int next_id = 1;
	// A uniform grid over the events (64 m cells; a query reads one cell), rebuilt when events are added or removed;
	// events wider than 16 cells sit in a short list every query checks.
	static constexpr float GRID_CELL = 64.0f;
	HashMap<Vector2i, LocalVector<uint32_t>> grid;
	LocalVector<uint32_t> wide;
	void _rebuild_grid();
	// Lightning strikes on trees (struck_tree, strike_age_at): points with ages, in a 16 m grid, at most MAX_STRIKES
	// (the oldest go first).
	struct Strike {
		Vector2 position;
		float age = 0.0f; // years
	};
	static constexpr float STRIKE_CELL = 16.0f;
	static constexpr int MAX_STRIKES = 4096;
	LocalVector<Strike> strikes;
	HashMap<Vector2i, LocalVector<uint32_t>> strike_grid;
	void _rebuild_strike_grid();
	// Browsing (deer): discs with a pressure (0..1) and the height the animals reach (m), in the same 64 m grid
	// scheme as the events.
	struct Browse {
		int id = 0;
		Vector2 center;
		float radius = 0.0f;
		float pressure = 0.0f;
		float reach = 1.8f;
	};
	LocalVector<Browse> browse;
	HashMap<Vector2i, LocalVector<uint32_t>> browse_grid;
	LocalVector<uint32_t> browse_wide;
	void _rebuild_browse_grid();
	void _browse_query(const Vector3 &p_position, float &r_pressure, float &r_reach) const;
	// Worn paths (game trails): the published network as given, and its segments in a 64 m grid for trail_at().
	struct TrailSegment {
		Vector2 a, b;
		float half_width = 0.5f;
		float passes = 0.0f; // per year
	};
	Array trails;
	LocalVector<TrailSegment> trail_segments;
	HashMap<Vector2i, LocalVector<uint32_t>> trail_grid;
	Rect2 trail_bounds;
	struct Change {
		uint64_t generation = 0;
		Rect2 area;
	};
	static constexpr int CHANGE_LOG = 64;
	uint64_t generation = 0;
	LocalVector<Change> change_log;
	void _report(const Rect2 &p_area);
	void _age_and_kind(const Vector3 &p_position, float &r_age, int &r_kind) const;

protected:
	static void _bind_methods();

	GDVIRTUAL1RC(float, _stand_age_at, Vector3)
	GDVIRTUAL1RC(int, _disturbance_at, Vector3)
	GDVIRTUAL1RC(float, _strike_age_at, Vector3)
	GDVIRTUAL1RC(float, _browse_at, Vector3)
	GDVIRTUAL1RC(float, _browse_reach_at, Vector3)
	GDVIRTUAL1RC(float, _rooting_age_at, Vector3)
	GDVIRTUAL1RC(Vector2, _trail_at, Vector3)

public:
	// Years since the last stand-replacing disturbance at the point (the youngest of fire, wind and beetle kills); -1
	// where none is known (old growth).
	float stand_age_at(const Vector3 &p_position) const;
	// The kind behind stand_age_at() there (Disturbance), DISTURBANCE_NONE where none.
	int disturbance_at(const Vector3 &p_position) const;
	// A stand-replacing disturbance over a disc (xz) that happened p_age_years ago (windthrow, a beetle outbreak; fire
	// comes from WorldFire). Returns its id. Each change reports its area.
	int add_disturbance(const Vector3 &p_center, float p_radius, int p_kind, float p_age_years);
	// Many discs of one kind at once (an avalanche track, a windthrow): one grid rebuild and one stand_changed report.
	// Earlier events of the same kind that a new disc covers are replaced, so a path struck again and again keeps
	// one event per disc. Returns the number added.
	int add_disturbances(const PackedVector3Array &p_centers, float p_radius, int p_kind, float p_age_years);
	void set_disturbance_age(int p_id, float p_age_years);
	void remove_disturbance(int p_id);
	void clear_disturbances();
	// A tree struck by lightning: the strike's attachment point (the crown top; WeatherLightning3D records its canopy
	// strikes) and how long ago (years). strike_age_at() answers for a tree whose trunk stands within STRIKE_RADIUS (m)
	// of it: the youngest strike there in years, -1 none. Each strike reports its area.
	static constexpr float STRIKE_RADIUS = 3.0f;
	void add_strike(const Vector3 &p_position, float p_age_years);
	float strike_age_at(const Vector3 &p_position) const;
	// The recorded strikes whose reach (STRIKE_RADIUS) touches p_rect, as (x, z, age years): a copy for readers that
	// place trees off the main thread (VegetationField3D). A script stub's _strike_age_at has no list to copy.
	void get_strikes_in(const Rect2 &p_rect, LocalVector<Vector3> &r_out) const;
	int get_strike_count() const { return int(strikes.size()); }
	void clear_strikes();
	// Browsing pressure (deer, docs/ideas/world-contracts.md): a disc over xz with a pressure 0..1 and the browse line
	// (m; red deer 1.8, fallow 1.6, roe 1.1). browse_at() is the strongest disc's pressure there, eased out over each
	// disc's outer 20 %, 0 where none; browse_reach_at() that disc's reach (0 where none). Changes report their area.
	int add_browse(const Vector3 &p_center, float p_radius, float p_pressure, float p_reach_m = 1.8f);
	void set_browse(int p_id, float p_pressure);
	void remove_browse(int p_id);
	void clear_browse();
	float browse_at(const Vector3 &p_position) const;
	float browse_reach_at(const Vector3 &p_position) const;
	// Boar rooting (DISTURBANCE_ROOTING discs from add_disturbance): it turns the field layer, not the stand, so
	// stand_age_at() and disturbance_at() leave it out; this is its own read: years since the youngest rooting over
	// the point, -1 none.
	float rooting_age_at(const Vector3 &p_position) const;
	// Worn paths: the network published by its maker (VegetationField3D.make_game_trails), an Array of Dictionaries
	// with "points" (PackedVector3Array or PackedVector2Array, world xz), "width" (m), "passes" (per year; or
	// "passes_per_year") and "kind" ("game_trail"). set_trails() replaces the network and reports the old and new
	// areas through stand_changed. trail_at() gives (passes per year of the busiest trail over the point, its tread
	// share 0..1: 1 on the centre line, easing to 0 at the edge), (0, 0) off every trail.
	void set_trails(const Array &p_trails);
	Array get_trails() const { return trails; }
	Vector2 trail_at(const Vector3 &p_position) const;
	// The world's years pass: every event and strike ages (fire ages through its own producers).
	void advance_years(float p_years);
	// Changes, as WorldFire's burn changes (fire's are forwarded here too): signal stand_changed(area), a generation, and
	// the union of the areas reported after a generation (false when the log no longer reaches back that far).
	void notify_stand_changed(const Rect2 &p_area);
	uint64_t get_stand_generation() const { return generation; }
	bool stand_changes_since(uint64_t p_since, Rect2 &r_area) const;
	Dictionary get_stand_changes_since(uint64_t p_since) const;
};

VARIANT_ENUM_CAST(WorldStand::Disturbance);

// Ground: the deformable ground's surface, soil and what lies on it (docs/ideas/world-contracts.md, Ground).
struct WorldGroundSoil {
	bool soft = false; // false: firm ground, rock, no soil
	float cohesion = 0.0f; // kPa
	float friction_angle = 35.0f; // degrees
	float stickiness = 0.0f; // 0..1, how wet clay clings to rubber and boots
};

class WorldGroundProvider {
public:
	virtual float contract_ground_height_at(const Vector3 &p_position) const = 0; // m, the deformed surface
	virtual WorldGroundSoil contract_ground_soil_at(const Vector3 &p_position) const = 0;
	virtual float contract_ground_standing_water_at(const Vector3 &p_position) const { return 0.0f; } // m
	virtual Vector2 contract_ground_ice_at(const Vector3 &p_position) const { return Vector2(); } // (m, broken share)
	virtual Vector3 contract_ground_frost_at(const Vector3 &p_position) const { return Vector3(); } // (thawed top m, frost depth m, weakening)
	virtual float contract_ground_dent_at(const Vector3 &p_position) const { return 0.0f; } // m
	virtual float contract_ground_rim_at(const Vector3 &p_position) const { return 0.0f; } // m
	virtual Vector2 contract_ground_runoff_at(const Vector3 &p_position) const { return Vector2(); } // (depth m, speed m/s)
	virtual float contract_ground_mud_depth_at(const Vector3 &p_position) const { return 0.0f; } // m of slurry
	// Clay a body carries pressed onto the ground as a print (m of film; print as GroundSurface3D.track_mud). Returns m3 laid.
	virtual float contract_ground_track_mud(const Vector3 &p_position, float p_radius, float p_depth, float p_wet, const Vector4 &p_print) { return 0.0f; }
	virtual Vector2 contract_ground_needle_ice_at(const Vector3 &p_position) const { return Vector2(); } // (m, crumbs 0..1)
	virtual Vector3 contract_ground_rut_guidance_at(const Vector3 &p_contact, const Vector3 &p_lateral, float p_half_width) const { return Vector3(); } // (cross slope, wall -lat m, wall +lat m)
	virtual float contract_ground_sand_at(const Vector3 &p_position) const { return 0.0f; } // 0..1, loose sand's share of the top
	virtual float contract_ground_ripple_at(const Vector3 &p_position) const { return 0.0f; } // m, wind ripples about their mean
	virtual float contract_ground_blown_sand_at(const Vector3 &p_position) const { return 0.0f; } // 0..1, dry sand the wind moves there now
	virtual ~WorldGroundProvider() {}
};

// Runoff at world scale (the catchment routing, WeatherRunoff3D): a separate slot on the ground contract, since the
// near ground (GroundSurface3D) and the land's drainage come from different producers.
class WorldRunoffProvider {
public:
	// Loose material (kg) put on the runoff at a ground point since the caller's last call; the runoff carries it down
	// its drainage and hands it to WorldWater.add_sediment where the flow reaches a river or lake.
	virtual void contract_runoff_add_load(const Vector3 &p_position, float p_mineral_kg, float p_ash_kg) = 0;
	virtual ~WorldRunoffProvider() {}
};

// The terrain's climate (TerrainDataMap3D's baked climate): a separate slot, since the climate and the live weather
// come from different producers. The first that covers the point answers.
class WorldClimateProvider {
public:
	// The mean precipitation at the point over the region's (the orographic pattern, Smith and Barstad 2004's linear
	// model as baked); < 0 where the climate does not reach.
	virtual float contract_precipitation_factor_at(const Vector3 &p_position) const = 0;
	virtual ~WorldClimateProvider() {}
};

// Coarse grains lying on the ground (the stone fields, RockField3D): a separate slot on the ground contract, since
// the near ground and the stones come from different producers. Several fields may register; each answers 0 where
// its map does not reach.
class WorldCoverProvider {
public:
	// The share of the ground (0..1) covered by pebbles (x), gravel (y) and fine grains (z) at the point.
	virtual Vector3 contract_cover_at(const Vector3 &p_position) const = 0;
	// The same at many points (world xz) at once, for readers that sample a grid: a provider with a lock takes it once.
	virtual void contract_cover_points(const Vector2 *p_points, int p_count, Vector3 *r_cover) const {
		for (int i = 0; i < p_count; i++) {
			r_cover[i] = contract_cover_at(Vector3(p_points[i].x, 0.0f, p_points[i].y));
		}
	}
	// The stones standing above the ground in a world xz rect, appended as 7 floats each: x, z, ground_y, top_y, the
	// footprint's half axes x and z (world metres) and its yaw (radians, about +y). Only stones whose top stands above their ground. Main thread.
	virtual void contract_stones_near(const Rect2 &p_rect, LocalVector<float> &r_out) const {}
	virtual ~WorldCoverProvider() {}
};

// The bare ground's colour (the terrain material, TerrainMaterial3D): a separate slot on the ground contract, since
// the material and the near ground come from different producers. Several may register; the first that knows the
// point answers.
class WorldGroundColorProvider {
public:
	// The ground's mean diffuse albedo at the point (linear rgb) as its material lays it, dry and bare, averaged over
	// a few metres (no texture detail); a: its porosity (0..1, how much water darkens it), or < 0 where it does not
	// reach.
	virtual Color contract_ground_albedo_at(const Vector3 &p_position) const = 0;
	virtual ~WorldGroundColorProvider() {}
};

// The plant cover over the ground (GrassField3D's sward, VegetationField3D's placed plants): a separate slot on the
// ground contract, since every field is its own producer. Several may register; each lays its cover over the ground
// colour in turn.
class WorldPlantColorProvider {
public:
	// The plants' mean diffuse albedo at the point as seen from above (linear rgb, a few metres' mean, as the far
	// ground draws them); a: the share of the ground they hide (0..1), 0 where the producer has none, < 0 where it
	// does not reach.
	virtual Color contract_plant_albedo_at(const Vector3 &p_position) const = 0;
	virtual ~WorldPlantColorProvider() {}
};

// The drawn ground's relief over the heightfield (Landscape3D's cliff and bank relief): a separate slot on the ground
// contract, since the heightfield and the near ground come from other producers. Placers (plants, rocks, ground
// detail, the near ground) set what they lay at the heightfield's height plus this. Several may register; the
// first that knows the point answers.
class WorldReliefProvider {
public:
	// At the point's xz: x the drawn surface's height over the heightfield's (m, + above it; the relief pushes the
	// surface out along the slope's horizontal normal by d, so about d tan(slope) here), y the relief's share
	// (0..1, the cliff and bank weights). x NAN where the producer does not reach. p_steps: with the terracettes' steps
	// (WorldContracts::terracette_step); false for placers that add the step at their own roots, as the GPU's do.
	virtual Vector2 contract_relief_at(const Vector3 &p_position, bool p_steps = true, bool p_recess = true) const = 0; // p_recess false: without the cliff kit's recess
	// For results cached by what they read (the world cache): the node whose stored properties set the relief, and
	// the rest of its state through p_add (called for each piece of bytes). A provider that returns null here is not
	// cached against, so its readers compute.
	virtual const Object *contract_relief_object() const { return nullptr; }
	virtual void contract_relief_state(void (*p_add)(void *p_user, const void *p_data, size_t p_size), void *p_user) const {}
	virtual ~WorldReliefProvider() {}
};

// The cliff kit's recess (docs/ideas/rocks.md, "Cliff kit recess contract"): where a stone field stands rock pillars
// as the wall of a steep face, the drawn face steps back under them so the pillars are the visible wall (terrain sunk
// under cliff meshes: Carrier, GDC 2018, "Procedural World Generation of Far Cry 5"). Produced by RockField3D, read
// by the landscape's drawn relief mesh (not its collision: the pillars carry their own). Several may register; the
// deepest (depth * share) answers.
class WorldRecessProvider {
public:
	// At a point of the drawn, unrecessed face (world, the relief's displaced position): x how far the face steps back
	// (m, >= 0) along its horizontal outward normal to stand behind the pillar there, y the share (0..1: 1 where a
	// pillar's front covers the point, fading to 0 within its outline's last 0.5 m where no neighbour continues the
	// wall). (0, 0) where no pillar stands. Const and thread-safe (read from mesh workers, per vertex): a producer
	// computes lazily under its own lock and caches.
	virtual Vector2 contract_recess_at(const Vector3 &p_position) const = 0;
	// The same as the step back itself: x y z the world offset that sets the point behind the pillar (horizontal, along
	// the pillar's own outward normal, depth times share), w the share. Along the pillar's normal, not the drawn
	// face's: on a fluted face the flanks' normals point sideways and a step along them leaves the ridges standing.
	virtual Vector4 contract_recess_vector_at(const Vector3 &p_position) const { return Vector4(0, 0, 0, contract_recess_at(p_position).y); }
	virtual void contract_recess_points(const Vector3 *p_points, int p_count, Vector2 *r_recess) const {
		for (int i = 0; i < p_count; i++) {
			r_recess[i] = contract_recess_at(p_points[i]);
		}
	}
	// For results cached by what they read (the world cache), as the relief's.
	virtual const Object *contract_recess_object() const { return nullptr; }
	virtual void contract_recess_state(void (*p_add)(void *p_user, const void *p_data, size_t p_size), void *p_user) const {}
	virtual ~WorldRecessProvider() {}
};

// Frost-sorted patterned ground (docs/ideas/ground-detail.md, "Frost-sorted patterned ground"): where freeze-thaw
// sorts stones from fines into circles and stripes. Produced by RockField3D (frost_sorting); read by CPU placers
// (cover plants keep off the stone borders and the churned centres' edges as grass does from terrain_frost_map).
class WorldFrostSortedProvider {
public:
	// At the point's xz: x the sorting's strength (0..1: the gate of cold, wet, gentle, deep soil), y the stone share
	// of the pattern there (0 a fine centre .. 1 a stone border). (0, 0) where the producer does not sort. Thread-safe.
	virtual Vector2 contract_frost_sorted_at(const Vector3 &p_position) const = 0;
	virtual ~WorldFrostSortedProvider() {}
};

class WorldGround : public RefCounted {
	GDCLASS(WorldGround, RefCounted);

	friend class WorldContracts;
	WorldGroundProvider *provider = nullptr; // the built-in contract's; a script stub overrides the virtuals
	WorldRunoffProvider *runoff_provider = nullptr;

	Dictionary _soil_at_bind(const Vector3 &p_position) const;

protected:
	static void _bind_methods();

	GDVIRTUAL1RC(float, _height_at, Vector3)
	GDVIRTUAL1RC(Dictionary, _soil_at, Vector3)
	GDVIRTUAL1RC(float, _standing_water_at, Vector3)
	GDVIRTUAL1RC(Vector2, _ice_at, Vector3)
	GDVIRTUAL1RC(Vector3, _frost_at, Vector3)
	GDVIRTUAL1RC(float, _dent_at, Vector3)
	GDVIRTUAL1RC(float, _rim_at, Vector3)
	GDVIRTUAL1RC(Vector2, _runoff_at, Vector3)
	GDVIRTUAL1RC(float, _mud_depth_at, Vector3)
	GDVIRTUAL1RC(Vector2, _needle_ice_at, Vector3)
	GDVIRTUAL3RC(Vector3, _rut_guidance_at, Vector3, Vector3, float)
	GDVIRTUAL1RC(float, _sand_at, Vector3)
	GDVIRTUAL1RC(float, _ripple_at, Vector3)
	GDVIRTUAL1RC(float, _blown_sand_at, Vector3)
	GDVIRTUAL3(_add_runoff_load, Vector3, float, float)
	GDVIRTUAL5R(float, _track_mud, Vector3, float, float, float, Vector4)
	GDVIRTUAL1RC(Vector3, _coarse_cover_at, Vector3)
	GDVIRTUAL1RC(PackedFloat32Array, _stones_near, Rect2)
	GDVIRTUAL1RC(Color, _albedo_at, Vector3)
	GDVIRTUAL1RC(Vector2, _relief_at, Vector3)
	GDVIRTUAL1RC(Vector2, _recess_at, Vector3)
	GDVIRTUAL1RC(Vector2, _frost_sorted_at, Vector3)
	GDVIRTUAL1RC(Vector4, _recess_vector_at, Vector3)

public:
	bool has_provider() const { return provider != nullptr || get_script_instance() != nullptr; } // a script stub (set_*) counts
	// The ground's surface (m); with no ground, the first static physics surface under the point, else -INF.
	float height_at(const Vector3 &p_position) const;
	float known_height_at(const Vector3 &p_position) const; // the producer's (or a stub's) deformed surface only; NaN without one (no collider fallback)
	WorldGroundSoil soil_at(const Vector3 &p_position) const;
	float standing_water_at(const Vector3 &p_position) const;
	Vector2 ice_at(const Vector3 &p_position) const;
	Vector3 frost_at(const Vector3 &p_position) const;
	float dent_at(const Vector3 &p_position) const;
	float rim_at(const Vector3 &p_position) const;
	Vector2 runoff_at(const Vector3 &p_position) const;
	float mud_depth_at(const Vector3 &p_position) const;
	Vector2 needle_ice_at(const Vector3 &p_position) const;
	Vector3 rut_guidance_at(const Vector3 &p_contact, const Vector3 &p_lateral, float p_half_width) const;
	float sand_at(const Vector3 &p_position) const;
	float ripple_at(const Vector3 &p_position) const;
	float blown_sand_at(const Vector3 &p_position) const;
	// Loose material washed off the ground at the point since the caller's last call (kg of mineral, of ash and char):
	// the runoff carries it down its drainage into WorldWater.add_sediment where the flow reaches water. Dropped
	// with no runoff producer. Callers: soil erosion, burnt ground's ash (PhysX).
	void add_runoff_load(const Vector3 &p_position, float p_mineral_kg, float p_ash_kg);
	// Mud a body carries out of mud pressed onto firm ground at the point (a film p_depth m thick over p_radius, wet
	// 0..1; p_print x 1 a tyre, 2 a sole, y its yaw, z w the sole's length and width): the ground's mud prints. m3 laid;
	// 0 with no ground producer.
	float track_mud(const Vector3 &p_position, float p_radius, float p_depth, float p_wet, const Vector4 &p_print);
	bool has_runoff() const { return runoff_provider != nullptr || get_script_instance() != nullptr; }
	// The share of the ground (0..1) covered by coarse grains: x pebbles, y gravel, z fine grains (the stone fields'
	// cover maps). A gravel lag armours the sand under it, so blown sand and ripples read pebble + gravel. 0 with no
	// stone field; the largest any field reports.
	Vector3 coarse_cover_at(const Vector3 &p_position) const;
	// coarse_cover_at at many points (world xz) in one call (a script stub answers point by point).
	void coarse_cover_points(const Vector2 *p_points, int p_count, Vector3 *r_cover) const;
	// The stones standing above the ground in a world xz rect, 7 floats each: x, z, ground_y, top_y, footprint half axes
	// x and z (world metres), yaw (radians), from every stone field (RockField3D). For readers that shape what lies round them (the snowpack's
	// moats and lee tails, SnowSurface3D). Empty with no stone field. Main thread.
	PackedFloat32Array stones_near(const Rect2 &p_rect) const;
	// The ground's albedo as drawn (linear rgb, a few metres' mean): the terrain material's colour, darkened by the
	// weather's surface wetness by its porosity, the plant cover over it (grass and plants, by the share they hide),
	// under the snowpack's cover and albedo. a: how much of that is known
	// (1 where a ground colour answers; the snow's cover alone over ground no colour knows; 0 nothing). Readers blend
	// their own setting toward rgb by a. For the light the ground sends up: gas and haze over it.
	Color albedo_at(const Vector3 &p_position) const;
	// The drawn ground's relief over the heightfield at the point's xz: x the height to add to the heightfield's (m),
	// y its share (0..1). (0, 0) with no relief producer. GPU placers read terrain_relief_map (R height m, G share)
	// over terrain_relief_rect instead.
	// p_steps false leaves out the terracettes' steps, for a placer that adds wc_terracette_step at each root itself (a
	// height map sampled every few metres would alias them and count them twice). A script override ignores it.
	// p_recess false: the relief without the cliff kit's recess (for the kit itself, which is fitted to the unrecessed
	// face). The relief producer applies the recess (recess_at) when p_recess is true.
	Vector2 relief_at(const Vector3 &p_position, bool p_steps = true, bool p_recess = true) const;
	// The cliff kit's recess at a point of the drawn, unrecessed face (WorldRecessProvider): x depth m back along the
	// face's horizontal outward normal, y share (0..1). (0, 0) with no producer. Thread-safe for the built-in producers.
	Vector2 recess_at(const Vector3 &p_position) const;
	// Frost-sorted ground at the point's xz (WorldFrostSortedProvider): x the sorting's strength (0..1), y the stone
	// share (0 a fine centre .. 1 a stone border). (0, 0) with no producer; the strongest answers. Thread-safe for the
	// built-in producers. GPU readers: terrain_frost_map (r the gate, g the stone share).
	Vector2 frost_sorted_at(const Vector3 &p_position) const;
	// The step back as a world offset (xyz: horizontal, along the pillar's outward normal, depth times share; w the
	// share): what the drawn face adds to the point. Use this rather than moving along the face's own normal.
	Vector4 recess_vector_at(const Vector3 &p_position) const;
	void recess_points(const Vector3 *p_points, int p_count, Vector2 *r_recess) const;
	bool has_recess() const;
	// A recess producer reports that the recess changed within a world box (it came or went, its pillars' settings
	// changed): signal recess_changed(box), so the drawn relief there is built again.
	void notify_recess_changed(const AABB &p_box);
	// A relief producer reports that the relief over an area changed (its heights streamed in at full resolution, its
	// relief settings changed, it came or went): signal relief_changed(area), so placers lay what sits there again.
	void notify_relief_changed(const Rect2 &p_area);
};

// Snow: the snowpack and lake ice (docs/ideas/world-contracts.md, Snow). Produced by SnowCover3D.
class WorldSnowProvider {
public:
	virtual float contract_snow_depth_at(const Vector3 &p_position) const = 0; // m
	virtual float contract_snow_density_at(const Vector3 &p_position) const = 0; // kg/m3 (100 new powder .. 550 old wet)
	virtual float contract_snow_wetness_at(const Vector3 &p_position) const { return 0.0f; } // liquid over what the pack holds, 0..1
	virtual float contract_snow_surface_at(const Vector3 &p_position) const { return -Math::INF; } // m, the snow top with prints; -INF bare
	virtual float contract_snow_crust_at(const Vector3 &p_position) const { return 0.0f; } // kPa a crust bears, 0 none
	virtual Vector2 contract_snow_lake_ice_at(const Vector3 &p_position) const { return Vector2(); } // (m of lake ice, white share)
	// Light-absorbing particles on the snow, as mg/m2 of black carbon equivalent (dust about 0.013 of its mass).
	virtual void contract_snow_add_impurity(const Vector3 &p_position, float p_radius, float p_mg_per_m2) {}
	virtual float contract_snow_impurity_at(const Vector3 &p_position) const { return 0.0f; }
	// The snow surface's broadband albedo (aged, less the particles on it); < 0 not known.
	virtual float contract_snow_albedo_at(const Vector3 &p_position) const { return -1.0f; }
	// Tyre-road friction on the snow (loose 0.3, packed 0.25, polished to ice 0.05-0.15); < 0 bare or not known.
	virtual float contract_snow_grip_at(const Vector3 &p_position) const { return -1.0f; }
	// Avalanches: the impact pressure at the point now (Pa; the dense flow where it runs, else the powder cloud) and the
	// highest any avalanche has put there.
	virtual float contract_snow_avalanche_pressure_at(const Vector3 &p_position) const { return 0.0f; }
	virtual float contract_snow_avalanche_peak_pressure_at(const Vector3 &p_position) const { return 0.0f; }
	// Snow held in tree crowns (kg per m2 of ground) and what the canopy there can hold; shake_canopy moves a share of
	// the held snow in a radius onto the pack below and returns the kg it moves.
	virtual float contract_snow_canopy_load_at(const Vector3 &p_position) const { return 0.0f; }
	virtual float contract_snow_canopy_capacity_at(const Vector3 &p_position) const { return 0.0f; }
	virtual float contract_snow_shake_canopy(const Vector3 &p_position, float p_radius, float p_share) { return 0.0f; }
	virtual ~WorldSnowProvider() {}
};

class WorldSnow : public RefCounted {
	GDCLASS(WorldSnow, RefCounted);

	friend class WorldContracts;
	WorldSnowProvider *provider = nullptr; // the built-in contract's; a script stub overrides the virtuals

protected:
	static void _bind_methods();

	GDVIRTUAL1RC(float, _depth_at, Vector3)
	GDVIRTUAL1RC(float, _density_at, Vector3)
	GDVIRTUAL1RC(float, _wetness_at, Vector3)
	GDVIRTUAL1RC(float, _surface_at, Vector3)
	GDVIRTUAL1RC(float, _crust_at, Vector3)
	GDVIRTUAL1RC(Vector2, _lake_ice_at, Vector3)
	GDVIRTUAL3(_add_impurity, Vector3, float, float)
	GDVIRTUAL1RC(float, _impurity_at, Vector3)
	GDVIRTUAL1RC(float, _albedo_at, Vector3)
	GDVIRTUAL1RC(float, _grip_at, Vector3)
	GDVIRTUAL1RC(float, _avalanche_pressure_at, Vector3)
	GDVIRTUAL1RC(float, _avalanche_peak_pressure_at, Vector3)
	GDVIRTUAL1RC(float, _canopy_load_at, Vector3)
	GDVIRTUAL1RC(float, _canopy_capacity_at, Vector3)
	GDVIRTUAL3R(float, _shake_canopy, Vector3, float, float)

public:
	bool has_provider() const { return provider != nullptr || get_script_instance() != nullptr; } // a script stub (set_snow) counts
	// Snow on the ground at the point (m); 0 with no snow system.
	float depth_at(const Vector3 &p_position) const;
	// Its bulk density (kg/m3): about 100 for new powder, 250 to 350 settled, 400 to 550 old or wet; 0 bare.
	float density_at(const Vector3 &p_position) const;
	// Liquid water in the pack over what it can hold, 0 (dry) to 1 (slush).
	float wetness_at(const Vector3 &p_position) const;
	// The snow surface (m), prints included near the camera; -INF where there is none.
	float surface_at(const Vector3 &p_position) const;
	// The load a crust in the pack bears before a foot breaks through (kPa), 0 with none.
	float crust_at(const Vector3 &p_position) const;
	// Lake ice: (thickness m, share of it that is white snow ice). Ice of 5 cm and more bears a walker.
	Vector2 lake_ice_at(const Vector3 &p_position) const;
	// Soot, ash and dust on the snow (light-absorbing particles), mg/m2 of black carbon equivalent: dust counts about
	// 0.013 of its mass (mass absorption about 0.1 against 7.5 m2/g). Producers of dust settling on snow add it; the
	// snow darkens and melts sooner. impurity_at: what shows at the surface.
	void add_impurity(const Vector3 &p_position, float p_radius, float p_mg_per_m2);
	float impurity_at(const Vector3 &p_position) const;
	// The snow surface's broadband albedo: fresh about 0.85, old dry toward 0.7, melting toward 0.5, less soot and
	// dust. 0.8 where the snow system does not say (and 0 with no snow system).
	float albedo_at(const Vector3 &p_position) const;
	// Tyre-road friction coefficient on the snow at the point: about 0.3 in loose snow, 0.25 packed by traffic, down to
	// 0.05-0.15 where tyres polished it to ice (by the air and its wetness). -1 bare, or where the snow system does not
	// say (readers then estimate from density, wetness and crust).
	float grip_at(const Vector3 &p_position) const;
	// Avalanche impact pressure at the point (Pa): now (the dense flow where it runs, else the powder cloud; 0 with
	// none), and the highest any avalanche has put there (trees break, buildings fail by it).
	float avalanche_pressure_at(const Vector3 &p_position) const;
	float avalanche_peak_pressure_at(const Vector3 &p_position) const;
	// Snow held in tree crowns over the point (kg per m2 of ground; Hedstrom and Pomeroy interception), and the most
	// the canopy there holds of new snow. 0 with no snow system or no canopy.
	float canopy_load_at(const Vector3 &p_position) const;
	float canopy_capacity_at(const Vector3 &p_position) const;
	// A crown shaken from outside the weather (a stem snapping, a tree thrown or felled, a body brushing it): the share
	// (0..1) of the held snow within the radius drops onto the pack below. Returns the kg moved (0 with none).
	float shake_canopy(const Vector3 &p_position, float p_radius, float p_share);
};

// A body as every system that feels bodies reads it: one reader instead of a copy in each.
struct WorldContactBody {
	Vector3 center; // the shape's centre (the node's position without a shape)
	float radius = 0.3f; // m, across
	float half_height = 0.3f; // m
	bool round = true;
	bool wheel = false; // a tyre lying on its side: half_height is its radius, width its tread
	bool box = false;
	float width = 0.0f; // m, contact width (0: from the shape)
	float mass = 0.0f; // kg on the ground (0: only pushes to its underside)
	// A walker (every CharacterBody3D; meta world_contact_walker true or false overrides): the ground, snow, sand and
	// mud press its soles step by step (WorldWalkerSteps), never its body shape. Grass, plants, branches, water and
	// wet tracks still feel the shape.
	bool walker = false;
	bool feet = false; // meta world_contact_feet: its feet are contact bodies of their own; deformers skip the walker
};

// Karst: stream sinks and springs (docs/ideas/terrain-generation.md, "Karst sinks and springs"). A cave producer
// (CaveNetwork3D) says where surface streams sink into the rock (swallets: ponors, stream sinks) and where the water
// comes out again (resurgences: karst springs at cave mouths low on valley sides, at the water table). The river
// producer (TerrainGenerator3D) ends and starts its rivers on those points: a swallet takes up to its capacity, so a
// low river sinks whole (a dry blind valley below in summer) while a flood overflows down the valley; a resurgence
// delivers its swallets' water (lagged) plus its own base flow. Ford and Williams, Karst Hydrogeology and
// Geomorphology (2007), ch. 5 and 7. The producer fits its swallets to the rivers without karst
// (TerrainGenerator3D.get_rivers(area, false)), so the two never wait on each other.
struct WorldSwallet {
	Vector3 position; // world, on the channel bed where the stream sinks
	float capacity = 0.0f; // m3/s the sink takes; flow above it runs on down the valley
	int resurgence = -1; // id of the resurgence the water comes out at, -1 unknown or beyond the map
	float lag_days = 0.0f; // days from sinking to coming out
	int id = -1; // the producer's own id for it (unique among all producers), -1 none
};

struct WorldResurgence {
	int id = -1; // unique among all producers
	Vector3 position; // world, the spring pool's surface at the mouth
	Vector3 direction; // horizontal unit vector, the way the water leaves the mouth
	float base_flow = 0.0f; // m3/s of its own (rain on the karst itself, autogenic recharge), all year
};

class WorldCavesProvider {
public:
	// Appends the swallets / resurgences whose position lies in the world xz rectangle.
	virtual void contract_swallets(const Rect2 &p_rect, LocalVector<WorldSwallet> &r_out) const = 0;
	virtual void contract_resurgences(const Rect2 &p_rect, LocalVector<WorldResurgence> &r_out) const = 0;
	virtual ~WorldCavesProvider() {}
};

class WorldCaves : public RefCounted {
	GDCLASS(WorldCaves, RefCounted);

	friend class WorldContracts;
	LocalVector<WorldCavesProvider *> providers;

protected:
	static void _bind_methods();

	GDVIRTUAL1RC(Array, _get_swallets, Rect2)
	GDVIRTUAL1RC(Array, _get_resurgences, Rect2)

public:
	bool has_provider() const { return !providers.is_empty() || get_script_instance() != nullptr; } // a script stub (set_caves) counts
	// Every producer's, in the world xz rectangle. Empty with none.
	void swallets(const Rect2 &p_rect, LocalVector<WorldSwallet> &r_out) const;
	void resurgences(const Rect2 &p_rect, LocalVector<WorldResurgence> &r_out) const;
	// The same for scripts and tests. Swallets: {position, capacity, resurgence, lag_days, id}; resurgences:
	// {id, position, direction, base_flow}. A script stub returns these from _get_swallets / _get_resurgences.
	Array get_swallets(const Rect2 &p_rect) const;
	Array get_resurgences(const Rect2 &p_rect) const;
	// A producer reports its sinks or springs changed within a world xz rectangle (built, moved, gone): signal
	// karst_changed(rect), and the rivers there are laid again. Never emitted for a river rebuild (that would loop).
	void notify_karst_changed(const Rect2 &p_rect);
};

class Camera3D;

class WorldContracts : public Object {
	GDCLASS(WorldContracts, Object);

	static WorldContracts *singleton;
	Ref<WorldWeather> builtin_weather;
	Ref<WorldWeather> weather_override;
	WorldWeatherProvider *weather_provider = nullptr;
	Ref<WorldGround> builtin_ground;
	Ref<WorldGround> ground_override;
	WorldGroundProvider *ground_provider = nullptr;
	WorldRunoffProvider *runoff_provider = nullptr;
	LocalVector<WorldCoverProvider *> cover_providers;
	LocalVector<WorldGroundColorProvider *> ground_color_providers;
	LocalVector<WorldReliefProvider *> relief_providers;
	LocalVector<WorldRecessProvider *> recess_providers;
	LocalVector<WorldFrostSortedProvider *> frost_sorted_providers;
	LocalVector<WorldClimateProvider *> climate_providers;
	LocalVector<WorldPlantColorProvider *> plant_color_providers;
	Ref<WorldWater> builtin_water;
	Ref<WorldWater> water_override;
	Ref<WorldFire> builtin_fire;
	Ref<WorldFire> fire_override;
	Ref<WorldCaves> builtin_caves;
	Ref<WorldCaves> caves_override;
	Ref<WorldSnow> builtin_snow;
	Ref<WorldStand> builtin_stand;
	Ref<WorldStand> stand_override;
	Ref<WorldSnow> snow_override;
	WorldSnowProvider *snow_provider = nullptr;
	LocalVector<WorldWaterProvider *> water_stubs;
	void _clear_water_stubs();

protected:
	static void _bind_methods();

public:
	static constexpr const char *CONTACT_GROUP = "world_contact";

	static WorldContracts *get_singleton() { return singleton; }

	// The camera a world system follows from p_node: the editor's 3D view while editing a scene (editor preview),
	// else the node's viewport camera. Null when neither has one.
	static Camera3D *get_view_camera(const Node *p_node);
	// True when p_node is in the scene being edited (not running), where systems draw their editor preview.
	static bool is_editor_preview(const Node *p_node);

	// The contract objects: never null.
	static WorldWeather *weather();
	static WorldWater *water();
	static WorldGround *ground();
	static WorldFire *fire();
	static WorldCaves *caves();
	static WorldStand *stand();
	static WorldSnow *snow();

	// Producers register on entering the tree and unregister on leaving; the last to register is current.
	static void register_weather(WorldWeatherProvider *p_provider);
	static void unregister_weather(WorldWeatherProvider *p_provider);
	static void register_ground(WorldGroundProvider *p_provider);
	static void unregister_ground(WorldGroundProvider *p_provider);
	static void register_runoff(WorldRunoffProvider *p_provider); // one at a time: the last registered answers
	static void unregister_runoff(WorldRunoffProvider *p_provider);
	static void register_cover(WorldCoverProvider *p_provider); // any number: the largest share answers
	static void unregister_cover(WorldCoverProvider *p_provider);
	static const LocalVector<WorldCoverProvider *> *cover_providers_list() { return singleton ? &singleton->cover_providers : nullptr; }
	static void register_climate(WorldClimateProvider *p_provider); // any number: the first that covers the point answers
	static void unregister_climate(WorldClimateProvider *p_provider);
	// The climate's precipitation factor at the point (1 where no climate reaches it).
	static float climate_precipitation_factor_at(const Vector3 &p_position);
	// The share of soil left bare of plants by the climate's dryness (0..0.9): perennial plant cover on soil follows
	// the year's rain, about a tenth of the ground at 200 mm to most of it by 700 mm (rain-use curves of semi-arid
	// rangelands, Le Houerou 1984), so the bare share falls from 0.9 to 0 over 200 to 700 mm. One rule for every
	// reader: the terrain material's soil colour (its shader mirrors it), the stones' rain splash pedestals.
	static float bare_share_for_precipitation(float p_mm);
	// Rock varnish (0..1) from the year's precipitation: thick dark manganese-iron varnish where rock dries out between
	// rare wettings, patchy in semi-arid lands, washed off and outgrown by lichens where wetter (Dorn 2009, "Rock varnish
	// and its use to study climatic change"): none over 400 mm, full under 180 mm. One rule for every reader: the
	// stones (RockField3D) and the terrain's cliffs (its shader mirrors it, wc_varnish).
	static float varnish_for_precipitation(float p_mm);
	// The same at a point, from the climate (0 with none).
	static float varnish_at(const Vector3 &p_position);
	// The share of soil left bare by a cold summer (0..0.97), from the July mean air temperature: tundra's vascular cover
	// follows summer warmth (the Circumpolar Arctic Vegetation Map's bioclimate subzones, Walker et al. 2005; their
	// July means from the CAVM legend: cushion forbs 0 to 3 C, prostrate dwarf shrubs 3 to 5 C, hemiprostrate 5 to 7 C,
	// erect dwarf shrubs 7 to 9 C, low shrubs 9 to 12 C), from a few percent of the ground in polar desert to a closed
	// sward by about 11 C: cover 0.03 + 0.97 smoothstep(2, 11, July). Alpine belts follow the same warmth.
	static float bare_share_for_july(float p_july_c);
	// The July mean air temperature at a point from the season climate (its July value less the July lapse rate up
	// from its reference height; p_position.y the ground's height, as the shaders' p.y); 99 with no weather climate.
	static float climate_july_at(const Vector3 &p_position);
	// The cover's height over its summer height for a July mean (0.3..1): the cold subzones' growth forms are prostrate
	// and cushion plants a few centimetres high, erect dwarf shrubs and herbs at 7 to 9 C, a full sward by about 11 C
	// (Walker et al. 2005; Gould et al. 2003): 0.3 + 0.7 smoothstep(3, 11, July); 1 with no climate (99).
	static float stature_for_july(float p_july_c);
	// On frost-sorted ground (WorldGround.frost_sorted_at's y, the stones' share): the share of cover plants kept, about a
	// fiftieth on the churned fine centres and on the stones, a fifth on the margins between (D'Amico et al. 2015:
	// "almost bare centers and better vegetated margins"). GLSL wc_sorted_keep the same.
	static float sorted_keep(float p_stones);
	// At a point (p_position.y the ground's height: the cold share follows it): the dry share (the season climate's precipitation times the
	// climate's factor there) and the cold share (bare_share_for_july) together, 1 - (1 - dry)(1 - cold); -1 with no
	// weather climate.
	static float climate_bare_share_at(const Vector3 &p_position);
	// The share of the plant cover on soil that is cured (dry, straw-coloured) through the dry season (0..1): annual and
	// perennial grasses of semi-arid and Mediterranean climates senesce in the summer drought and stand as straw until
	// the rains (grassland curing; summer-dry grasslands under about 500 mm a year cure fully, humid ones over about 1000
	// mm stay green), so 1 at 500 mm and below, 0 from 1000 mm. The terrain material's soil and alluvium cover colour
	// (its shader mirrors it: wc_climate_cured).
	static float cured_share_for_precipitation(float p_mm);
	// How green grassland is for a season state, 0..1: greenness follows soil water and temperature (PhenoGrass,
	// Hufkens et al. 2016; the Growing Season Index of Jolly et al. 2005 without its day-length term): drought cures it
	// as the soil dries below about 40% of capacity, and a hard winter (daily means well below freezing) leaves it tan.
	// One rule for the sward (GrassField3D's season greenness) and the ground's cover.
	static float greenness_for_season(float p_soil_moisture, float p_temperature);
	// The cured share for a season state: 1 - greenness_for_season. A dry climate's cover cures through its summer
	// drought and greens in the wet season; a humid one stays green.
	static float cured_share_for_season(float p_soil_moisture, float p_temperature);
	// At a point: from the weather's season there (WorldWeather.season_at: soil_moisture, temperature), else from the
	// year's precipitation (cured_share_for_precipitation); -1 with no weather. GLSL wc_climate_cured(p) the same, from
	// the weather_climate global's y and z (WeatherMap3D publishes them), else the precipitation.
	static float climate_cured_share_at(const Vector3 &p_position);
	// Where on the ground the bare share falls (1 bare soil, 0 plant cover): drylands' cover is patchy, not thin, with
	// clumps and bare soil between them metres across (Rietkerk et al. 2004). Bare where a two-octave value noise (4 m and
	// 1.3 m; mean 0.5, sd 0.16) falls under the share's quantile, so the bare area is the share. The terrain material's
	// bare soil patches (TerrainMaterial3D.climate_bare_soil, tm_bare_patch) and every plant layer read this one pattern:
	// the CPU here, shaders through climate_bare_shader_code(). 0 with no climate.
	static float bare_patch(const Vector3 &p_position, float p_share, float p_footprint = 0.0f);
	static float climate_bare_patch_at(const Vector3 &p_position);
	// GLSL of the same: wc_climate_bare(vec3 p) the share (0 with no climate; reads the globals terrain_climate_map,
	// terrain_climate_rect and weather_climate, which the including shader declares, and weather_climate_july, which
	// this code declares; wc_climate_july(p) the July temperature, 99 with none) and wc_bare_patch(vec3 p, float
	// share, float footprint_m) the pattern, fading to the share past a footprint of about half a metre.
	static String climate_bare_shader_code();
	// GLSL for desert varnish on cliff faces (docs/ideas/terrain-material.md, "Desert varnish on cliffs"), one rule for
	// every face that shows it: the terrain's cliffs (TerrainMaterial3D) and the cliff kit's wall pieces (RockField3D),
	// so a piece and the wall round it carry the same film, curtains and iron band. Needs climate_bare_shader_code
	// (wc_vn) before it, and the globals terrain_cliff_rect and terrain_cliff_foot_map declared by the includer.
	// wc_drip(p, n, t, fp, seed, C, wmin, wmax): crisp drip stripes down a face (0..1 cover); wc_cliff_above_foot(p):
	// metres over the wall's foot (1e3 off the map); wc_cliff_varnish(p, n, fp, k, shade, wall, amount, above_foot): rgb the
	// albedo factor, a the curtains' share (for the sheen).
	static String cliff_varnish_shader_code();
	// Terracettes: on steep grazed hillsides stock walk the slope across along the contours, and their paths settle into
	// regularly spaced steps, flat treads they walk and risers they graze between (Seleb, Chatterjee and Bhamla 2025,
	// a grazing model: bands parallel to the contours, regular by 40 degrees, about 3.4 m apart). One layout for the
	// sward (GrassField3D.terracettes) and the ground's step relief: at p (world, its height in y) on ground of
	// gradient p_gradient (dh/dx, dh/dz), x the tread (0..1: 1 on a tread's middle), y the phase up the step (0..1,
	// the tread from 0 to TERRACETTE_TREAD, then the riser), z how strongly the slope forms them (0 under about 25 degrees,
	// 1 from 35 to 45, 0 past about 51). Bands follow the height, wandering a few decimetres, and fade out in places.
	// The phase goes by height over a fixed rise (TERRACETTE_RISE), so a band keeps to one contour: a rise taken from the
	// local slope (spacing times its sine) moved the phase by a whole step for a few hundredths of a metre's change in
	// rise 100 m up, the bands a scribble of short dashes wherever the slope varied.
	static constexpr float TERRACETTE_SPACING = 3.4f; // Along a 36 degree slope, m (closer on steeper ground).
	static constexpr float TERRACETTE_RISE = 2.0f; // Height from one tread to the next, m.
	// The tread's share of each step's phase: level treads of about a metre and risers some 10 degrees steeper than the
	// slope at 35 degrees (0.6 m steps); at 0.185 the risers stood only 4 degrees steeper and the steps barely read.
	static constexpr float TERRACETTE_TREAD = 0.3f;
	static Vector3 terracette_at(const Vector3 &p_position, const Vector2 &p_gradient);
	// The ground's steps (the relief every reader draws): the profile is level over the tread and climbs over the
	// riser, less its mean, so the ground keeps its height on average; box-filtered over p_width metres along the fall
	// line (the profile's integral is closed form), so a grid or pixel coarser than a tread gets the steps' mean, and
	// faded out once the width nears a step. x the height to add to the smooth ground (m), y the change of its
	// steepness (the surface's gradient is p_gradient times 1 + y). p_share scales them: the producer's setting
	// (Landscape3D.terracette_relief, published in terrain_bank_relief.y) times terracette_soil_share.
	static Vector2 terracette_step(const Vector3 &p_position, const Vector2 &p_gradient, float p_width, float p_share);
	// Where the steps form, from the generator's geology (TerrainGenerator3D.get_material_geology_at: soil depth, rock,
	// talus): open soil only, none on sand, thin soil or in a cell with any rock, talus or till.
	static float terracette_soil_share(const Color &p_geology);
	// GLSL of the same: vec3 wc_terracette(vec3 p, vec2 gradient), vec2 wc_terracette_step(vec3 p, vec2 gradient,
	// float width, float share), float wc_terracette_soil(vec4 geology). Needs climate_bare_shader_code() (wc_vn) first.
	static String terracette_shader_code();
	static void register_ground_color(WorldGroundColorProvider *p_provider); // any number: the first that knows the point answers
	static void unregister_ground_color(WorldGroundColorProvider *p_provider);
	static void register_relief(WorldReliefProvider *p_provider); // any number: the first that knows the point answers
	static void unregister_relief(WorldReliefProvider *p_provider);
	static const LocalVector<WorldReliefProvider *> *relief_providers_list() { return singleton ? &singleton->relief_providers : nullptr; }
	static void register_recess(WorldRecessProvider *p_provider); // any number: the deepest answers
	static void unregister_recess(WorldRecessProvider *p_provider);
	static const LocalVector<WorldRecessProvider *> *recess_providers_list() { return singleton ? &singleton->recess_providers : nullptr; }
	static void register_frost_sorted(WorldFrostSortedProvider *p_provider); // any number: the strongest answers
	static void unregister_frost_sorted(WorldFrostSortedProvider *p_provider);
	static const LocalVector<WorldFrostSortedProvider *> *frost_sorted_providers_list() { return singleton ? &singleton->frost_sorted_providers : nullptr; }
	static const LocalVector<WorldGroundColorProvider *> *ground_color_providers_list() { return singleton ? &singleton->ground_color_providers : nullptr; }
	static void register_plant_color(WorldPlantColorProvider *p_provider); // any number: each lays its cover over the ground in turn
	static void unregister_plant_color(WorldPlantColorProvider *p_provider);
	static const LocalVector<WorldPlantColorProvider *> *plant_color_providers_list() { return singleton ? &singleton->plant_color_providers : nullptr; }
	static void register_water(WorldWaterProvider *p_provider);
	static void unregister_water(WorldWaterProvider *p_provider);
	static void register_fire(WorldFireProvider *p_provider);
	static void unregister_fire(WorldFireProvider *p_provider);
	static void fire_burn_changed(const Rect2 &p_area); // WorldFire.burn_changed() on the contract in use
	static void register_caves(WorldCavesProvider *p_provider); // any number: all their sinks and springs count
	static void unregister_caves(WorldCavesProvider *p_provider);
	static void karst_changed(const Rect2 &p_rect); // WorldCaves.notify_karst_changed() on the built-in contract and any stub
	static void register_snow(WorldSnowProvider *p_provider);
	static void unregister_snow(WorldSnowProvider *p_provider);

	// Tests set stubs (a script extending the contract class) and clear them.
	void set_weather(const Ref<WorldWeather> &p_stub);
	Ref<WorldWeather> get_weather() const;
	void set_ground(const Ref<WorldGround> &p_stub);
	Ref<WorldGround> get_ground() const;
	void set_water(const Ref<WorldWater> &p_stub);
	Ref<WorldWater> get_water() const;
	void set_fire(const Ref<WorldFire> &p_stub);
	Ref<WorldFire> get_fire() const;
	void set_caves(const Ref<WorldCaves> &p_stub);
	Ref<WorldCaves> get_caves() const;
	void set_stand(const Ref<WorldStand> &p_stub);
	Ref<WorldStand> get_stand() const;
	void set_snow(const Ref<WorldSnow> &p_stub);
	Ref<WorldSnow> get_snow() const;
	// A script stub (extending WorldWater) as one more water producer beside the real ones: a pool or sea for a
	// bench without one. clear_overrides() removes it.
	void add_water_stub(const Ref<WorldWater> &p_stub);
	void clear_overrides();

	// Contact bodies: the world_contact group, and how big and heavy a body is (metas trample_radius,
	// touch_radius, ground_* / snow_*, deform_wheel_*, else its first collision shape).
	static WorldContactBody describe_body(const Node3D *p_node);
	// The body's footprint for what it bends and pushes aside (grass, plants, water): (radius, half height) from
	// its trample_radius meta, else its first enabled collision shape (sphere, capsule, cylinder, box), else
	// p_fallback for both.
	static Vector2 body_extent(const Node3D *p_node, float p_fallback);
	// The body's collision geometry as a sphere set, for what its shape bends and pushes aside (docs/ideas/grass.md,
	// "Contact from the real collision shapes"; Crysis 3's collider sphere sets, Sousa 2013; Jahrmann and Wimmer 2017).
	// Each enabled CollisionShape3D at its global transform: a sphere as itself, a capsule or upright cylinder as a
	// chain along its axis, a lying cylinder (a wheel) as a ring of tread-wide spheres round its rim, a box as a grid,
	// any other shape as its bounding box's grid. Also its PhysicalBone3D descendants while they simulate (a ragdoll),
	// its VehicleWheel3D tyres (radius; width from meta deform_wheel_width, else 0.6 of the radius) and the contact
	// points the physics server reports for it (0.08 m each). r_spheres: (centre, radius); r_velocities each one's
	// velocity (the body's linear velocity plus its spin about its origin; p_fallback_velocity where the body has
	// none). A meta trample_radius gives one sphere of that radius at the origin instead. At most 512 spheres.
	static void contact_spheres(const Node3D *p_node, LocalVector<Vector4> &r_spheres, LocalVector<Vector3> &r_velocities, const Vector3 &p_fallback_velocity = Vector3());
	Dictionary describe_body_bind(Node *p_node) const;

	WorldContracts();
	~WorldContracts();
};

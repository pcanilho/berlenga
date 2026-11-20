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
	virtual Vector3 contract_mean_wind_at(const Vector3 &p_position) const { return Vector3(); } // m/s at 10 m, no gusts
	virtual Vector3 contract_wind_aloft_at(const Vector3 &p_position, float p_altitude) const { return Vector3(); } // m/s, free air
	virtual float contract_terrain_lee_at(const Vector3 &p_position) const { return NAN; } // rad, Winstral's Sx; NaN: no terrain wind there
	virtual float contract_rainfall_at(const Vector3 &p_position) const { return 0.0f; } // mm/h of liquid
	virtual float contract_snowfall_at(const Vector3 &p_position) const { return 0.0f; } // mm/h of water falling as snow
	virtual float contract_smoke_at(const Vector3 &p_position) const { return 0.0f; } // g/m2 of smoke particulate in the column
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
	GDVIRTUAL1RC(Vector3, _mean_wind_at, Vector3)
	GDVIRTUAL2RC(Vector3, _wind_aloft_at, Vector3, float)
	GDVIRTUAL1RC(float, _terrain_lee_at, Vector3)
	GDVIRTUAL1RC(float, _rainfall_at, Vector3)
	GDVIRTUAL1RC(float, _snowfall_at, Vector3)
	GDVIRTUAL1RC(float, _smoke_at, Vector3)
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
	Vector3 mean_wind_at(const Vector3 &p_position) const;
	Vector3 wind_aloft_at(const Vector3 &p_position, float p_altitude) const;
	float terrain_lee_at(const Vector3 &p_position) const;
	float rainfall_at(const Vector3 &p_position) const;
	float snowfall_at(const Vector3 &p_position) const;
	float smoke_at(const Vector3 &p_position) const; // g/m2 of smoke particulate over the point (the haze layer)
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
		DISTURBANCE_BEETLE = 3, // new kinds are appended only
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

public:
	// Years since the last stand-replacing disturbance at the point (the youngest of fire, wind and beetle kills); -1
	// where none is known (old growth).
	float stand_age_at(const Vector3 &p_position) const;
	// The kind behind stand_age_at() there (Disturbance), DISTURBANCE_NONE where none.
	int disturbance_at(const Vector3 &p_position) const;
	// A stand-replacing disturbance over a disc (xz) that happened p_age_years ago (windthrow, a beetle outbreak; fire
	// comes from WorldFire). Returns its id. Each change reports its area.
	int add_disturbance(const Vector3 &p_center, float p_radius, int p_kind, float p_age_years);
	void set_disturbance_age(int p_id, float p_age_years);
	void remove_disturbance(int p_id);
	void clear_disturbances();
	// The world's years pass: every event ages (fire ages through its own producers).
	void advance_years(float p_years);
	// Changes, as WorldFire's burn changes (fire's are forwarded here too): signal stand_changed(area), a generation, and
	// the union of the areas reported after a generation (false when the log no longer reaches back that far).
	void stand_changed(const Rect2 &p_area);
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

// Coarse grains lying on the ground (the stone fields, RockField3D): a separate slot on the ground contract, since
// the near ground and the stones come from different producers. Several fields may register; each answers 0 where
// its map does not reach.
class WorldCoverProvider {
public:
	// The share of the ground (0..1) covered by pebbles (x), gravel (y) and fine grains (z) at the point.
	virtual Vector3 contract_cover_at(const Vector3 &p_position) const = 0;
	virtual ~WorldCoverProvider() {}
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

public:
	bool has_provider() const { return provider != nullptr || get_script_instance() != nullptr; } // a script stub (set_*) counts
	// The ground's surface (m); with no ground, the first static physics surface under the point, else -INF.
	float height_at(const Vector3 &p_position) const;
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
	Ref<WorldWater> builtin_water;
	Ref<WorldWater> water_override;
	Ref<WorldFire> builtin_fire;
	Ref<WorldFire> fire_override;
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

	// The contract objects: never null.
	static WorldWeather *weather();
	static WorldWater *water();
	static WorldGround *ground();
	static WorldFire *fire();
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
	static void register_water(WorldWaterProvider *p_provider);
	static void unregister_water(WorldWaterProvider *p_provider);
	static void register_fire(WorldFireProvider *p_provider);
	static void unregister_fire(WorldFireProvider *p_provider);
	static void fire_burn_changed(const Rect2 &p_area); // WorldFire.burn_changed() on the contract in use
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
	Dictionary describe_body_bind(Node *p_node) const;

	WorldContracts();
	~WorldContracts();
};

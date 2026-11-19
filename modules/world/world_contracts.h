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
	GDVIRTUAL1RC(float, _canopy_water_at, Vector3)
	GDVIRTUAL1RC(float, _canopy_wetness_at, Vector3)
	GDVIRTUAL1RC(float, _canopy_drip_at, Vector3)
	GDVIRTUAL1RC(float, _ground_water_input_at, Vector3)
	GDVIRTUAL0RC(int, _day_of_year)
	GDVIRTUAL2RC(Dictionary, _season_at, Vector3, int)
	GDVIRTUAL1RC(float, _cloud_cover_at, Vector3)
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
	float canopy_water_at(const Vector3 &p_position) const;
	float canopy_wetness_at(const Vector3 &p_position) const;
	float canopy_drip_at(const Vector3 &p_position) const;
	float ground_water_input_at(const Vector3 &p_position) const;
	int day_of_year() const;
	// C++ only: the climate (false with no provider or none set); its math is in world_season.h.
	bool season_climate(WeatherSeason::Climate &r_climate) const;
	Dictionary season_at(const Vector3 &p_position, int p_day = -1) const;
	float cloud_cover_at(const Vector3 &p_position) const;
	float water_temperature() const;
	float surface_temperature_at(const Vector3 &p_position) const;
	float ground_height_at(const Vector3 &p_position) const;
	double time_scale() const;
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
	// Suspended load (kg/m3): x mineral (silt, clay: brown), y ash and char (grey to black). Zero for clear water.
	virtual Vector2 contract_water_sediment_at(const Vector3 &p_position) const { return Vector2(); }
	// A load entering this water at the point (kg/s of each), from runoff, banks or a landslide; it carries it on.
	virtual void contract_water_add_sediment(const Vector3 &p_position, float p_mineral_kg_s, float p_ash_kg_s) {}
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
	GDVIRTUAL1RC(Vector2, _sediment_at, Vector3)
	GDVIRTUAL3(_add_sediment, Vector3, float, float)

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
	// The water's suspended load at the point (kg/m3): x mineral, y ash and char; the shading clouds and darkens by it.
	Vector2 sediment_at(const Vector3 &p_position) const;
	// A load entering the water that answers at the point (kg/s of mineral and of ash); none there: it is lost.
	void add_sediment(const Vector3 &p_position, float p_mineral_kg_s, float p_ash_kg_s);
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
	// Producers report where burn_age_at() changed (x, z in metres; a burn scar placed, moved or aged, ground a
	// live fire burnt). Report as the ground burns, at most a few times a second (union the cells in between).
	// WorldContracts::fire_burn_changed() reports to the built-in contract and any stub.
	void burn_changed(const Rect2 &p_area);
	uint64_t get_burn_generation() const { return burn_generation; }
	// The union of the areas reported after generation p_since into r_area; false when the log no longer reaches
	// back that far (read everything again). True with an empty r_area when nothing changed.
	bool burn_changes_since(uint64_t p_since, Rect2 &r_area) const;
};

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
	virtual Vector2 contract_ground_needle_ice_at(const Vector3 &p_position) const { return Vector2(); } // (m, crumbs 0..1)
	virtual Vector3 contract_ground_rut_guidance_at(const Vector3 &p_contact, const Vector3 &p_lateral, float p_half_width) const { return Vector3(); } // (cross slope, wall -lat m, wall +lat m)
	virtual float contract_ground_sand_at(const Vector3 &p_position) const { return 0.0f; } // 0..1, loose sand's share of the top
	virtual float contract_ground_ripple_at(const Vector3 &p_position) const { return 0.0f; } // m, wind ripples about their mean
	virtual float contract_ground_blown_sand_at(const Vector3 &p_position) const { return 0.0f; } // 0..1, dry sand the wind moves there now
	virtual ~WorldGroundProvider() {}
};

class WorldGround : public RefCounted {
	GDCLASS(WorldGround, RefCounted);

	friend class WorldContracts;
	WorldGroundProvider *provider = nullptr; // the built-in contract's; a script stub overrides the virtuals

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
	Ref<WorldWater> builtin_water;
	Ref<WorldWater> water_override;
	Ref<WorldFire> builtin_fire;
	Ref<WorldFire> fire_override;
	Ref<WorldSnow> builtin_snow;
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
	static WorldSnow *snow();

	// Producers register on entering the tree and unregister on leaving; the last to register is current.
	static void register_weather(WorldWeatherProvider *p_provider);
	static void unregister_weather(WorldWeatherProvider *p_provider);
	static void register_ground(WorldGroundProvider *p_provider);
	static void unregister_ground(WorldGroundProvider *p_provider);
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

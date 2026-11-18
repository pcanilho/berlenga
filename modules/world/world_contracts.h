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
#include "core/math/vector3.h"
#include "core/variant/dictionary.h"
#include "core/templates/local_vector.h"

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
	virtual float contract_rainfall_at(const Vector3 &p_position) const { return 0.0f; } // mm/h of liquid
	virtual float contract_snowfall_at(const Vector3 &p_position) const { return 0.0f; } // mm/h of water falling as snow
	virtual float contract_canopy_water_at(const Vector3 &p_position) const { return 0.0f; } // mm held on leaves
	virtual float contract_canopy_wetness_at(const Vector3 &p_position) const { return 0.0f; } // 0..1
	virtual float contract_canopy_drip_at(const Vector3 &p_position) const { return 0.0f; } // mm/h
	virtual float contract_ground_water_input_at(const Vector3 &p_position) const { return 0.0f; } // mm/h reaching the ground
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
	GDVIRTUAL1RC(float, _rainfall_at, Vector3)
	GDVIRTUAL1RC(float, _snowfall_at, Vector3)
	GDVIRTUAL1RC(float, _canopy_water_at, Vector3)
	GDVIRTUAL1RC(float, _canopy_wetness_at, Vector3)
	GDVIRTUAL1RC(float, _canopy_drip_at, Vector3)
	GDVIRTUAL1RC(float, _ground_water_input_at, Vector3)

public:
	// Defaults with no provider: still, mild, dry, open sky.
	static constexpr float DEFAULT_AIR_TEMPERATURE = 15.0f;
	static constexpr float DEFAULT_AIR_HUMIDITY = 0.7f;

	bool has_provider() const { return provider != nullptr; }
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
	float rainfall_at(const Vector3 &p_position) const;
	float snowfall_at(const Vector3 &p_position) const;
	float canopy_water_at(const Vector3 &p_position) const;
	float canopy_wetness_at(const Vector3 &p_position) const;
	float canopy_drip_at(const Vector3 &p_position) const;
	float ground_water_input_at(const Vector3 &p_position) const;
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

public:
	bool has_provider() const { return !providers.is_empty(); }
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
	Ref<WorldWater> builtin_water;
	Ref<WorldWater> water_override;
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

	// Producers register on entering the tree and unregister on leaving; the last to register is current.
	static void register_weather(WorldWeatherProvider *p_provider);
	static void unregister_weather(WorldWeatherProvider *p_provider);
	static void register_water(WorldWaterProvider *p_provider);
	static void unregister_water(WorldWaterProvider *p_provider);

	// Tests set stubs (a script extending the contract class) and clear them.
	void set_weather(const Ref<WorldWeather> &p_stub);
	Ref<WorldWeather> get_weather() const;
	void set_water(const Ref<WorldWater> &p_stub);
	Ref<WorldWater> get_water() const;
	// A script stub (extending WorldWater) as one more water producer beside the real ones: a pool or sea for a
	// bench without one. clear_overrides() removes it.
	void add_water_stub(const Ref<WorldWater> &p_stub);
	void clear_overrides();

	// Contact bodies: the world_contact group, and how big and heavy a body is (metas trample_radius,
	// touch_radius, ground_* / snow_*, deform_wheel_*, else its first collision shape).
	static WorldContactBody describe_body(const Node3D *p_node);
	Dictionary describe_body_bind(Node *p_node) const;

	WorldContracts();
	~WorldContracts();
};

/**************************************************************************/
/*  world_contracts.cpp                                                   */
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

#include "world_contracts.h"

#include "core/object/class_db.h"

#include <type_traits>
#include "scene/3d/node_3d.h"
#include "servers/physics_3d/physics_server_3d.h"
#include "scene/resources/3d/world_3d.h"
#include "scene/main/window.h"
#include "scene/main/scene_tree.h"
#include "scene/3d/physics/collision_shape_3d.h"
#include "scene/3d/physics/character_body_3d.h"
#include "scene/3d/physics/rigid_body_3d.h"
#include "scene/resources/3d/box_shape_3d.h"
#include "scene/resources/3d/capsule_shape_3d.h"
#include "scene/resources/3d/cylinder_shape_3d.h"
#include "scene/resources/3d/sphere_shape_3d.h"

// Weather.

#define WW_CALL(m_virtual, m_provider, m_default, ...) \
	{ \
		std::remove_cv_t<decltype(m_default)> r = m_default; \
		if (GDVIRTUAL_CALL(m_virtual, __VA_ARGS__, r)) { \
			return r; \
		} \
		return provider ? provider->m_provider : m_default; \
	}

float WorldWeather::air_temperature_at(const Vector3 &p_position) const WW_CALL(_air_temperature_at, contract_air_temperature_at(p_position), DEFAULT_AIR_TEMPERATURE, p_position)
float WorldWeather::air_humidity_at(const Vector3 &p_position) const WW_CALL(_air_humidity_at, contract_air_humidity_at(p_position), DEFAULT_AIR_HUMIDITY, p_position)
Vector3 WorldWeather::wind_at(const Vector3 &p_position, float p_height_above_ground) const WW_CALL(_wind_at, contract_wind_at(p_position, p_height_above_ground), Vector3(), p_position, p_height_above_ground)
float WorldWeather::precipitation_at(const Vector3 &p_position) const WW_CALL(_precipitation_at, contract_precipitation_at(p_position), 0.0f, p_position)
float WorldWeather::rain_share_at(const Vector3 &p_position) const WW_CALL(_rain_share_at, contract_rain_share_at(p_position), 1.0f, p_position)
float WorldWeather::surface_wetness_at(const Vector3 &p_position) const WW_CALL(_surface_wetness_at, contract_surface_wetness_at(p_position), 0.0f, p_position)
float WorldWeather::puddle_at(const Vector3 &p_position) const WW_CALL(_puddle_at, contract_puddle_at(p_position), 0.0f, p_position)
float WorldWeather::contact_wetness_at(const Vector3 &p_position) const WW_CALL(_contact_wetness_at, contract_contact_wetness_at(p_position), 0.0f, p_position)
float WorldWeather::sun_visibility_at(const Vector3 &p_position) const WW_CALL(_sun_visibility_at, contract_sun_visibility_at(p_position), 1.0f, p_position)
float WorldWeather::canopy_closure_at(const Vector3 &p_position) const WW_CALL(_canopy_closure_at, contract_canopy_closure_at(p_position), 0.0f, p_position)
float WorldWeather::frost_at(const Vector3 &p_position) const WW_CALL(_frost_at, contract_frost_at(p_position), 0.0f, p_position)
float WorldWeather::rainfall_at(const Vector3 &p_position) const WW_CALL(_rainfall_at, contract_rainfall_at(p_position), 0.0f, p_position)
float WorldWeather::snowfall_at(const Vector3 &p_position) const WW_CALL(_snowfall_at, contract_snowfall_at(p_position), 0.0f, p_position)
float WorldWeather::smoke_at(const Vector3 &p_position) const WW_CALL(_smoke_at, contract_smoke_at(p_position), 0.0f, p_position)
float WorldWeather::canopy_water_at(const Vector3 &p_position) const WW_CALL(_canopy_water_at, contract_canopy_water_at(p_position), 0.0f, p_position)
float WorldWeather::canopy_wetness_at(const Vector3 &p_position) const WW_CALL(_canopy_wetness_at, contract_canopy_wetness_at(p_position), 0.0f, p_position)
float WorldWeather::canopy_drip_at(const Vector3 &p_position) const WW_CALL(_canopy_drip_at, contract_canopy_drip_at(p_position), 0.0f, p_position)
float WorldWeather::ground_water_input_at(const Vector3 &p_position) const WW_CALL(_ground_water_input_at, contract_ground_water_input_at(p_position), 0.0f, p_position)
float WorldWeather::cloud_cover_at(const Vector3 &p_position) const WW_CALL(_cloud_cover_at, contract_cloud_cover_at(p_position), 0.0f, p_position)
float WorldWeather::ground_height_at(const Vector3 &p_position) const WW_CALL(_ground_height_at, contract_ground_height_at(p_position), 0.0f, p_position)
float WorldWeather::washout_rate(float p_rain_mm_h, float p_snow_mm_h, bool p_coarse) {
	const float a = p_coarse ? 3.0e-4f : 1.0e-5f;
	const float b = p_coarse ? 0.75f : 0.8f;
	const float r = MAX(p_rain_mm_h, 0.0f), sn = MAX(p_snow_mm_h, 0.0f);
	return a * (Math::pow(r, b) + 5.0f * Math::pow(sn, b));
}

float WorldWeather::washout_at(const Vector3 &p_position, bool p_coarse) const {
	return washout_rate(rainfall_at(p_position), snowfall_at(p_position), p_coarse);
}

float WorldWeather::surface_temperature_at(const Vector3 &p_position) const {
	float r = 0.0f;
	if (GDVIRTUAL_CALL(_surface_temperature_at, p_position, r)) {
		return r;
	}
	return provider ? provider->contract_surface_temperature_at(p_position) : air_temperature_at(p_position);
}
Dictionary WorldWeather::season_at(const Vector3 &p_position, int p_day) const {
	Dictionary r;
	if (GDVIRTUAL_CALL(_season_at, p_position, p_day, r)) {
		return r;
	}
	if (provider) {
		return provider->contract_season_at(p_position, p_day);
	}
	return WeatherSeason::to_dictionary(WeatherSeason::compute(WeatherSeason::Climate(), p_position.y, p_day >= 1 ? MIN(p_day, 365) : day_of_year()));
}
int WorldWeather::day_of_year() const {
	int r = 135;
	if (GDVIRTUAL_CALL(_day_of_year, r)) {
		return CLAMP(r, 1, 365);
	}
	return provider ? CLAMP(provider->contract_day_of_year(), 1, 365) : 135;
}
bool WorldWeather::season_climate(WeatherSeason::Climate &r_climate) const {
	return provider != nullptr && provider->contract_season_climate(r_climate);
}
float WorldWeather::water_temperature() const {
	float r = DEFAULT_AIR_TEMPERATURE;
	if (GDVIRTUAL_CALL(_water_temperature, r)) {
		return r;
	}
	return provider ? provider->contract_water_temperature() : DEFAULT_AIR_TEMPERATURE;
}
double WorldWeather::time_scale() const {
	double r = 1.0;
	if (GDVIRTUAL_CALL(_time_scale, r)) {
		return r;
	}
	return provider ? provider->contract_time_scale() : 1.0;
}

Dictionary WorldWeatherProvider::contract_season_at(const Vector3 &p_position, int p_day) const {
	WeatherSeason::Climate c;
	contract_season_climate(c);
	return WeatherSeason::to_dictionary(WeatherSeason::compute(c, p_position.y, p_day >= 1 ? MIN(p_day, 365) : contract_day_of_year()));
}

Vector3 WorldWeather::mean_wind_at(const Vector3 &p_position) const WW_CALL(_mean_wind_at, contract_mean_wind_at(p_position), Vector3(), p_position)
Vector3 WorldWeather::wind_aloft_at(const Vector3 &p_position, float p_altitude) const WW_CALL(_wind_aloft_at, contract_wind_aloft_at(p_position, p_altitude), Vector3(), p_position, p_altitude)
float WorldWeather::terrain_lee_at(const Vector3 &p_position) const WW_CALL(_terrain_lee_at, contract_terrain_lee_at(p_position), NAN, p_position)

#undef WW_CALL

void WorldWeather::add_contact_wetness(const Vector3 &p_position, float p_radius, float p_amount) {
	if (GDVIRTUAL_CALL(_add_contact_wetness, p_position, p_radius, p_amount)) {
		return;
	}
	if (provider) {
		provider->contract_add_contact_wetness(p_position, p_radius, p_amount);
	}
}

float WorldWeather::dew_point_at(const Vector3 &p_position) const {
	// Magnus inverted (Alduchov and Eskridge 1996).
	const float t = air_temperature_at(p_position);
	const float g = Math::log(MAX(air_humidity_at(p_position), 0.01f)) + 17.62f * t / (243.12f + t);
	return 243.12f * g / (17.62f - g);
}

void WorldWeather::_bind_methods() {
	ClassDB::bind_method(D_METHOD("has_provider"), &WorldWeather::has_provider);
	ClassDB::bind_method(D_METHOD("air_temperature_at", "position"), &WorldWeather::air_temperature_at);
	ClassDB::bind_method(D_METHOD("air_humidity_at", "position"), &WorldWeather::air_humidity_at);
	ClassDB::bind_method(D_METHOD("dew_point_at", "position"), &WorldWeather::dew_point_at);
	ClassDB::bind_method(D_METHOD("wind_at", "position", "height_above_ground"), &WorldWeather::wind_at, DEFVAL(-1.0f));
	ClassDB::bind_method(D_METHOD("precipitation_at", "position"), &WorldWeather::precipitation_at);
	ClassDB::bind_method(D_METHOD("rain_share_at", "position"), &WorldWeather::rain_share_at);
	ClassDB::bind_method(D_METHOD("surface_wetness_at", "position"), &WorldWeather::surface_wetness_at);
	ClassDB::bind_method(D_METHOD("puddle_at", "position"), &WorldWeather::puddle_at);
	ClassDB::bind_method(D_METHOD("contact_wetness_at", "position"), &WorldWeather::contact_wetness_at);
	ClassDB::bind_method(D_METHOD("add_contact_wetness", "position", "radius", "amount"), &WorldWeather::add_contact_wetness, DEFVAL(1.0f));
	ClassDB::bind_method(D_METHOD("sun_visibility_at", "position"), &WorldWeather::sun_visibility_at);
	ClassDB::bind_method(D_METHOD("canopy_closure_at", "position"), &WorldWeather::canopy_closure_at);
	ClassDB::bind_method(D_METHOD("frost_at", "position"), &WorldWeather::frost_at);
	ClassDB::bind_method(D_METHOD("mean_wind_at", "position"), &WorldWeather::mean_wind_at);
	ClassDB::bind_method(D_METHOD("wind_aloft_at", "position", "altitude"), &WorldWeather::wind_aloft_at);
	ClassDB::bind_method(D_METHOD("terrain_lee_at", "position"), &WorldWeather::terrain_lee_at);
	ClassDB::bind_method(D_METHOD("rainfall_at", "position"), &WorldWeather::rainfall_at);
	ClassDB::bind_method(D_METHOD("snowfall_at", "position"), &WorldWeather::snowfall_at);
	ClassDB::bind_static_method("WorldWeather", D_METHOD("washout_rate", "rain_mm_h", "snow_mm_h", "coarse"), &WorldWeather::washout_rate);
	ClassDB::bind_method(D_METHOD("washout_at", "position", "coarse"), &WorldWeather::washout_at);
	ClassDB::bind_method(D_METHOD("smoke_at", "position"), &WorldWeather::smoke_at);
	ClassDB::bind_method(D_METHOD("canopy_water_at", "position"), &WorldWeather::canopy_water_at);
	ClassDB::bind_method(D_METHOD("canopy_wetness_at", "position"), &WorldWeather::canopy_wetness_at);
	ClassDB::bind_method(D_METHOD("canopy_drip_at", "position"), &WorldWeather::canopy_drip_at);
	ClassDB::bind_method(D_METHOD("ground_water_input_at", "position"), &WorldWeather::ground_water_input_at);
	ClassDB::bind_method(D_METHOD("day_of_year"), &WorldWeather::day_of_year);
	ClassDB::bind_method(D_METHOD("season_at", "position", "day"), &WorldWeather::season_at, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("cloud_cover_at", "position"), &WorldWeather::cloud_cover_at);
	ClassDB::bind_method(D_METHOD("water_temperature"), &WorldWeather::water_temperature);
	ClassDB::bind_method(D_METHOD("surface_temperature_at", "position"), &WorldWeather::surface_temperature_at);
	ClassDB::bind_method(D_METHOD("ground_height_at", "position"), &WorldWeather::ground_height_at);
	ClassDB::bind_method(D_METHOD("time_scale"), &WorldWeather::time_scale);

	GDVIRTUAL_BIND(_air_temperature_at, "position");
	GDVIRTUAL_BIND(_air_humidity_at, "position");
	GDVIRTUAL_BIND(_wind_at, "position", "height_above_ground");
	GDVIRTUAL_BIND(_precipitation_at, "position");
	GDVIRTUAL_BIND(_rain_share_at, "position");
	GDVIRTUAL_BIND(_surface_wetness_at, "position");
	GDVIRTUAL_BIND(_puddle_at, "position");
	GDVIRTUAL_BIND(_contact_wetness_at, "position");
	GDVIRTUAL_BIND(_add_contact_wetness, "position", "radius", "amount");
	GDVIRTUAL_BIND(_sun_visibility_at, "position");
	GDVIRTUAL_BIND(_canopy_closure_at, "position");
	GDVIRTUAL_BIND(_frost_at, "position");
	GDVIRTUAL_BIND(_mean_wind_at, "position");
	GDVIRTUAL_BIND(_wind_aloft_at, "position", "altitude");
	GDVIRTUAL_BIND(_terrain_lee_at, "position");
	GDVIRTUAL_BIND(_rainfall_at, "position");
	GDVIRTUAL_BIND(_snowfall_at, "position");
	GDVIRTUAL_BIND(_smoke_at, "position");
	GDVIRTUAL_BIND(_canopy_water_at, "position");
	GDVIRTUAL_BIND(_canopy_wetness_at, "position");
	GDVIRTUAL_BIND(_canopy_drip_at, "position");
	GDVIRTUAL_BIND(_ground_water_input_at, "position");
	GDVIRTUAL_BIND(_day_of_year);
	GDVIRTUAL_BIND(_season_at, "position", "day");
	GDVIRTUAL_BIND(_cloud_cover_at, "position");
	GDVIRTUAL_BIND(_water_temperature);
	GDVIRTUAL_BIND(_surface_temperature_at, "position");
	GDVIRTUAL_BIND(_ground_height_at, "position");
	GDVIRTUAL_BIND(_time_scale);
}

// Water.

WorldWaterProvider *WorldWater::_top_at(const Vector3 &p_position, float &r_height) const {
	// Each producer answers only where it really has water. Of those, the surface nearest above the point (a
	// cave stream under the sea's level, a pool in a hollow, a river under an overhang), else the nearest below
	// it (a body above the water).
	WorldWaterProvider *above = nullptr;
	WorldWaterProvider *below = nullptr;
	float above_h = Math::INF;
	float below_h = -Math::INF;
	for (WorldWaterProvider *w : providers) {
		const float h = w->contract_water_height_at(p_position);
		if (!Math::is_finite(h)) {
			continue;
		}
		if (h >= p_position.y) {
			if (h < above_h) {
				above_h = h;
				above = w;
			}
		} else if (h > below_h) {
			below_h = h;
			below = w;
		}
	}
	r_height = above ? above_h : below_h;
	return above ? above : below;
}

float WorldWater::height_at(const Vector3 &p_position) const {
	float r = -Math::INF;
	if (GDVIRTUAL_CALL(_height_at, p_position, r)) {
		return r;
	}
	_top_at(p_position, r);
	return r;
}

float WorldWater::depth_at(const Vector3 &p_position) const {
	const float h = height_at(p_position);
	return Math::is_finite(h) ? MAX(h - p_position.y, 0.0f) : 0.0f;
}

Vector3 WorldWater::flow_at(const Vector3 &p_position) const {
	Vector3 r;
	if (GDVIRTUAL_CALL(_flow_at, p_position, r)) {
		return r;
	}
	float h;
	WorldWaterProvider *top = _top_at(p_position, h);
	return top ? top->contract_water_flow_at(p_position) : Vector3();
}

float WorldWater::wake_depth_at(const Vector3 &p_position) const {
	float r = 0.0f;
	if (GDVIRTUAL_CALL(_wake_depth_at, p_position, r)) {
		return r;
	}
	float h;
	WorldWaterProvider *top = _top_at(p_position, h);
	return top ? top->contract_water_wake_depth_at(p_position) : 0.0f;
}

void WorldWater::disturb(uint64_t p_owner, const Vector3 &p_position, float p_radius, float p_strength) {
	if (GDVIRTUAL_CALL(_disturb, p_owner, p_position, p_radius, p_strength)) {
		return;
	}
	for (WorldWaterProvider *w : providers) {
		w->contract_water_disturb(p_owner, p_position, p_radius, p_strength);
	}
}

void WorldWater::clear(uint64_t p_owner) {
	if (GDVIRTUAL_CALL(_clear, p_owner)) {
		return;
	}
	for (WorldWaterProvider *w : providers) {
		w->contract_water_clear(p_owner);
	}
}

void WorldWater::impulse(const Vector3 &p_position, float p_radius, float p_strength) {
	if (GDVIRTUAL_CALL(_impulse, p_position, p_radius, p_strength)) {
		return;
	}
	for (WorldWaterProvider *w : providers) {
		w->contract_water_impulse(p_position, p_radius, p_strength);
	}
}

Vector2 WorldWater::sediment_at(const Vector3 &p_position) const {
	Vector2 r;
	if (GDVIRTUAL_CALL(_sediment_at, p_position, r)) {
		return r;
	}
	float h;
	WorldWaterProvider *top = _top_at(p_position, h);
	return top ? top->contract_water_sediment_at(p_position) : Vector2();
}

void WorldWater::add_sediment(const Vector3 &p_position, float p_mineral_kg_s, float p_ash_kg_s) {
	if (GDVIRTUAL_CALL(_add_sediment, p_position, p_mineral_kg_s, p_ash_kg_s)) {
		return;
	}
	float h;
	if (WorldWaterProvider *top = _top_at(p_position, h)) {
		top->contract_water_add_sediment(p_position, MAX(p_mineral_kg_s, 0.0f), MAX(p_ash_kg_s, 0.0f));
	}
}

void WorldWater::_bind_methods() {
	ClassDB::bind_method(D_METHOD("has_provider"), &WorldWater::has_provider);
	ClassDB::bind_method(D_METHOD("height_at", "position"), &WorldWater::height_at);
	ClassDB::bind_method(D_METHOD("depth_at", "position"), &WorldWater::depth_at);
	ClassDB::bind_method(D_METHOD("flow_at", "position"), &WorldWater::flow_at);
	ClassDB::bind_method(D_METHOD("wake_depth_at", "position"), &WorldWater::wake_depth_at);
	ClassDB::bind_method(D_METHOD("disturb", "owner", "position", "radius", "strength"), &WorldWater::disturb);
	ClassDB::bind_method(D_METHOD("clear", "owner"), &WorldWater::clear);
	ClassDB::bind_method(D_METHOD("impulse", "position", "radius", "strength"), &WorldWater::impulse);
	ClassDB::bind_method(D_METHOD("sediment_at", "position"), &WorldWater::sediment_at);
	ClassDB::bind_method(D_METHOD("add_sediment", "position", "mineral_kg_s", "ash_kg_s"), &WorldWater::add_sediment);

	GDVIRTUAL_BIND(_height_at, "position");
	GDVIRTUAL_BIND(_flow_at, "position");
	GDVIRTUAL_BIND(_wake_depth_at, "position");
	GDVIRTUAL_BIND(_disturb, "owner", "position", "radius", "strength");
	GDVIRTUAL_BIND(_clear, "owner");
	GDVIRTUAL_BIND(_impulse, "position", "radius", "strength");
	GDVIRTUAL_BIND(_sediment_at, "position");
	GDVIRTUAL_BIND(_add_sediment, "position", "mineral_kg_s", "ash_kg_s");
}

// Snow.

float WorldSnow::depth_at(const Vector3 &p_position) const {
	float r = 0.0f;
	if (GDVIRTUAL_CALL(_depth_at, p_position, r)) {
		return r;
	}
	return provider ? provider->contract_snow_depth_at(p_position) : 0.0f;
}

float WorldSnow::density_at(const Vector3 &p_position) const {
	float r = 0.0f;
	if (GDVIRTUAL_CALL(_density_at, p_position, r)) {
		return r;
	}
	return provider ? provider->contract_snow_density_at(p_position) : 0.0f;
}

float WorldSnow::wetness_at(const Vector3 &p_position) const {
	float r = 0.0f;
	if (GDVIRTUAL_CALL(_wetness_at, p_position, r)) {
		return r;
	}
	return provider ? provider->contract_snow_wetness_at(p_position) : 0.0f;
}

float WorldSnow::surface_at(const Vector3 &p_position) const {
	float r = 0.0f;
	if (GDVIRTUAL_CALL(_surface_at, p_position, r)) {
		return r;
	}
	return provider ? provider->contract_snow_surface_at(p_position) : -Math::INF;
}

float WorldSnow::crust_at(const Vector3 &p_position) const {
	float r = 0.0f;
	if (GDVIRTUAL_CALL(_crust_at, p_position, r)) {
		return r;
	}
	return provider ? provider->contract_snow_crust_at(p_position) : 0.0f;
}

Vector2 WorldSnow::lake_ice_at(const Vector3 &p_position) const {
	Vector2 r;
	if (GDVIRTUAL_CALL(_lake_ice_at, p_position, r)) {
		return r;
	}
	return provider ? provider->contract_snow_lake_ice_at(p_position) : Vector2();
}

void WorldSnow::add_impurity(const Vector3 &p_position, float p_radius, float p_mg_per_m2) {
	if (GDVIRTUAL_CALL(_add_impurity, p_position, p_radius, p_mg_per_m2)) {
		return;
	}
	if (provider) {
		provider->contract_snow_add_impurity(p_position, p_radius, p_mg_per_m2);
	}
}

float WorldSnow::impurity_at(const Vector3 &p_position) const {
	float r = 0.0f;
	if (GDVIRTUAL_CALL(_impurity_at, p_position, r)) {
		return r;
	}
	return provider ? provider->contract_snow_impurity_at(p_position) : 0.0f;
}

void WorldSnow::_bind_methods() {
	ClassDB::bind_method(D_METHOD("has_provider"), &WorldSnow::has_provider);
	ClassDB::bind_method(D_METHOD("depth_at", "position"), &WorldSnow::depth_at);
	ClassDB::bind_method(D_METHOD("density_at", "position"), &WorldSnow::density_at);
	ClassDB::bind_method(D_METHOD("wetness_at", "position"), &WorldSnow::wetness_at);
	ClassDB::bind_method(D_METHOD("surface_at", "position"), &WorldSnow::surface_at);
	ClassDB::bind_method(D_METHOD("crust_at", "position"), &WorldSnow::crust_at);
	ClassDB::bind_method(D_METHOD("lake_ice_at", "position"), &WorldSnow::lake_ice_at);
	ClassDB::bind_method(D_METHOD("add_impurity", "position", "radius", "mg_per_m2"), &WorldSnow::add_impurity);
	ClassDB::bind_method(D_METHOD("impurity_at", "position"), &WorldSnow::impurity_at);
	GDVIRTUAL_BIND(_depth_at, "position");
	GDVIRTUAL_BIND(_density_at, "position");
	GDVIRTUAL_BIND(_wetness_at, "position");
	GDVIRTUAL_BIND(_surface_at, "position");
	GDVIRTUAL_BIND(_crust_at, "position");
	GDVIRTUAL_BIND(_lake_ice_at, "position");
	GDVIRTUAL_BIND(_add_impurity, "position", "radius", "mg_per_m2");
	GDVIRTUAL_BIND(_impurity_at, "position");
}

// Fire.

float WorldFire::heat_at(const Vector3 &p_position) const {
	float r = 0.0f;
	if (GDVIRTUAL_CALL(_heat_at, p_position, r)) {
		return r;
	}
	for (WorldFireProvider *f : providers) {
		r = MAX(r, f->contract_fire_heat_at(p_position));
	}
	return r;
}

float WorldFire::burnt_at(const Vector3 &p_position) const {
	float r = 0.0f;
	if (GDVIRTUAL_CALL(_burnt_at, p_position, r)) {
		return r;
	}
	for (WorldFireProvider *f : providers) {
		r = MAX(r, f->contract_fire_burnt_at(p_position));
	}
	return r;
}

Vector3 WorldFire::wind_at(const Vector3 &p_position) const {
	Vector3 r;
	if (GDVIRTUAL_CALL(_wind_at, p_position, r)) {
		return r;
	}
	for (WorldFireProvider *f : providers) {
		r += f->contract_fire_wind_at(p_position);
	}
	return r;
}

float WorldFire::stem_char_at(const Vector3 &p_position) const {
	float r = 0.0f;
	if (GDVIRTUAL_CALL(_stem_char_at, p_position, r)) {
		return r;
	}
	for (WorldFireProvider *f : providers) {
		r = MAX(r, f->contract_fire_stem_char_at(p_position));
	}
	return r;
}

float WorldFire::burn_age_at(const Vector3 &p_position) const {
	float r = -1.0f;
	if (GDVIRTUAL_CALL(_burn_age_at, p_position, r)) {
		return r;
	}
	for (WorldFireProvider *f : providers) {
		const float a = f->contract_fire_burn_age_at(p_position);
		if (a >= 0.0f && (r < 0.0f || a < r)) {
			r = a;
		}
	}
	return r;
}

float WorldFire::crown_scorch_at(const Vector3 &p_position) const {
	float r = 0.0f;
	if (GDVIRTUAL_CALL(_crown_scorch_at, p_position, r)) {
		return r;
	}
	for (WorldFireProvider *f : providers) {
		r = MAX(r, f->contract_fire_crown_scorch_at(p_position));
	}
	return r;
}

float WorldFire::water_repellency_at(const Vector3 &p_position) const {
	float r = 0.0f;
	if (GDVIRTUAL_CALL(_water_repellency_at, p_position, r)) {
		return r;
	}
	for (WorldFireProvider *f : providers) {
		r = MAX(r, f->contract_fire_water_repellency_at(p_position));
	}
	return r;
}

// Each fire's convection columns, all of them: where its plume rises from and the heat it carries up (the weather
// makes fire clouds from them).
PackedVector4Array WorldFire::plumes() const {
	PackedVector4Array r;
	if (GDVIRTUAL_CALL(_plumes, r)) {
		return r;
	}
	for (WorldFireProvider *f : providers) {
		f->contract_fire_plumes(r);
	}
	return r;
}

void WorldFire::burn_changed(const Rect2 &p_area) {
	burn_generation++;
	BurnChange c;
	c.generation = burn_generation;
	c.area = p_area;
	if (burn_log.size() >= uint32_t(BURN_LOG)) {
		burn_log.remove_at(0);
	}
	burn_log.push_back(c);
}

bool WorldFire::burn_changes_since(uint64_t p_since, Rect2 &r_area) const {
	r_area = Rect2();
	if (p_since >= burn_generation) {
		return true;
	}
	if (burn_log.is_empty() || burn_log[0].generation > p_since + 1) {
		return false; // Older reports have left the log.
	}
	bool any = false;
	for (const BurnChange &c : burn_log) {
		if (c.generation > p_since) {
			r_area = any ? r_area.merge(c.area) : c.area;
			any = true;
		}
	}
	return true;
}

Dictionary WorldFire::get_burn_changes_since(uint64_t p_since) const {
	Rect2 area;
	Dictionary d;
	d["complete"] = burn_changes_since(p_since, area);
	d["area"] = area;
	return d;
}

// Each fire's smoke, all of it: particulate (PM2.5) in kg per second of real time, where it rises from (the weather
// carries it as a smoke layer that dims the sun).
PackedVector4Array WorldFire::smoke_sources() const {
	PackedVector4Array r;
	if (GDVIRTUAL_CALL(_smoke_sources, r)) {
		return r;
	}
	for (WorldFireProvider *f : providers) {
		f->contract_fire_smoke(r);
	}
	return r;
}

void WorldFire::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_burn_changes_since", "generation"), &WorldFire::get_burn_changes_since);
	ClassDB::bind_method(D_METHOD("burn_changed", "area"), &WorldFire::burn_changed);
	ClassDB::bind_method(D_METHOD("get_burn_generation"), &WorldFire::get_burn_generation);
	ClassDB::bind_method(D_METHOD("has_provider"), &WorldFire::has_provider);
	ClassDB::bind_method(D_METHOD("heat_at", "position"), &WorldFire::heat_at);
	ClassDB::bind_method(D_METHOD("burnt_at", "position"), &WorldFire::burnt_at);
	GDVIRTUAL_BIND(_heat_at, "position");
	GDVIRTUAL_BIND(_burnt_at, "position");
	ClassDB::bind_method(D_METHOD("wind_at", "position"), &WorldFire::wind_at);
	GDVIRTUAL_BIND(_wind_at, "position");
	ClassDB::bind_method(D_METHOD("stem_char_at", "position"), &WorldFire::stem_char_at);
	GDVIRTUAL_BIND(_stem_char_at, "position");
	ClassDB::bind_method(D_METHOD("burn_age_at", "position"), &WorldFire::burn_age_at);
	GDVIRTUAL_BIND(_burn_age_at, "position");
	ClassDB::bind_method(D_METHOD("crown_scorch_at", "position"), &WorldFire::crown_scorch_at);
	GDVIRTUAL_BIND(_crown_scorch_at, "position");
	ClassDB::bind_method(D_METHOD("water_repellency_at", "position"), &WorldFire::water_repellency_at);
	ClassDB::bind_method(D_METHOD("plumes"), &WorldFire::plumes);
	ClassDB::bind_method(D_METHOD("smoke_sources"), &WorldFire::smoke_sources);
	GDVIRTUAL_BIND(_water_repellency_at, "position");
	GDVIRTUAL_BIND(_plumes);
	GDVIRTUAL_BIND(_smoke_sources);
}

// A script stub standing in as one water producer among the real ones.
class WorldWaterStubProvider : public WorldWaterProvider {
public:
	Ref<WorldWater> stub;
	float contract_water_height_at(const Vector3 &p_position) const override { return stub->height_at(p_position); }
	Vector3 contract_water_flow_at(const Vector3 &p_position) const override { return stub->flow_at(p_position); }
	float contract_water_wake_depth_at(const Vector3 &p_position) const override { return stub->wake_depth_at(p_position); }
	void contract_water_disturb(uint64_t p_owner, const Vector3 &p_position, float p_radius, float p_strength) override { stub->disturb(p_owner, p_position, p_radius, p_strength); }
	void contract_water_clear(uint64_t p_owner) override { stub->clear(p_owner); }
	void contract_water_impulse(const Vector3 &p_position, float p_radius, float p_strength) override { stub->impulse(p_position, p_radius, p_strength); }
	Vector2 contract_water_sediment_at(const Vector3 &p_position) const override { return stub->sediment_at(p_position); }
	void contract_water_add_sediment(const Vector3 &p_position, float p_mineral_kg_s, float p_ash_kg_s) override { stub->add_sediment(p_position, p_mineral_kg_s, p_ash_kg_s); }
};

void WorldContracts::add_water_stub(const Ref<WorldWater> &p_stub) {
	ERR_FAIL_COND(p_stub.is_null());
	WorldWaterStubProvider *w = memnew(WorldWaterStubProvider);
	w->stub = p_stub;
	water_stubs.push_back(w);
	register_water(w);
}

void WorldContracts::_clear_water_stubs() {
	for (WorldWaterProvider *w : water_stubs) {
		unregister_water(w);
		memdelete(static_cast<WorldWaterStubProvider *>(w));
	}
	water_stubs.clear();
}

// Registry.

// Ground.

#define WG_CALL(m_virtual, m_provider, m_default, ...) \
	{ \
		std::remove_cv_t<decltype(m_default)> r = m_default; \
		if (GDVIRTUAL_CALL(m_virtual, __VA_ARGS__, r)) { \
			return r; \
		} \
		return provider ? provider->m_provider : m_default; \
	}

float WorldGround::height_at(const Vector3 &p_position) const {
	float r = 0.0f;
	if (GDVIRTUAL_CALL(_height_at, p_position, r)) {
		return r;
	}
	if (provider) {
		return provider->contract_ground_height_at(p_position);
	}
	// No ground: the first static physics surface under the point (a floor, a road, a terrain's collider).
	SceneTree *tree = SceneTree::get_singleton();
	Window *root = tree ? tree->get_root() : nullptr;
	Ref<World3D> world = root ? root->get_world_3d() : Ref<World3D>();
	PhysicsDirectSpaceState3D *space = world.is_valid() ? world->get_direct_space_state() : nullptr;
	if (space) {
		PhysicsDirectSpaceState3D::RayParameters ray;
		ray.from = p_position + Vector3(0.0f, 1000.0f, 0.0f);
		ray.to = p_position - Vector3(0.0f, 1000.0f, 0.0f);
		ray.collide_with_areas = false;
		PhysicsDirectSpaceState3D::RayResult hit;
		if (space->intersect_ray(ray, hit)) {
			return hit.position.y;
		}
	}
	return -Math::INF;
}

WorldGroundSoil WorldGround::soil_at(const Vector3 &p_position) const {
	Dictionary d;
	if (GDVIRTUAL_CALL(_soil_at, p_position, d)) {
		WorldGroundSoil s;
		s.soft = d.get("soft", false);
		s.cohesion = d.get("cohesion", 0.0f);
		s.friction_angle = d.get("friction_angle", 35.0f);
		s.stickiness = d.get("stickiness", 0.0f);
		return s;
	}
	return provider ? provider->contract_ground_soil_at(p_position) : WorldGroundSoil();
}

Dictionary WorldGround::_soil_at_bind(const Vector3 &p_position) const {
	const WorldGroundSoil s = soil_at(p_position);
	Dictionary d;
	d["soft"] = s.soft;
	d["cohesion"] = s.cohesion;
	d["friction_angle"] = s.friction_angle;
	d["stickiness"] = s.stickiness;
	return d;
}

float WorldGround::standing_water_at(const Vector3 &p_position) const WG_CALL(_standing_water_at, contract_ground_standing_water_at(p_position), 0.0f, p_position)
Vector2 WorldGround::ice_at(const Vector3 &p_position) const WG_CALL(_ice_at, contract_ground_ice_at(p_position), Vector2(), p_position)
Vector3 WorldGround::frost_at(const Vector3 &p_position) const WG_CALL(_frost_at, contract_ground_frost_at(p_position), Vector3(), p_position)
float WorldGround::dent_at(const Vector3 &p_position) const WG_CALL(_dent_at, contract_ground_dent_at(p_position), 0.0f, p_position)
float WorldGround::rim_at(const Vector3 &p_position) const WG_CALL(_rim_at, contract_ground_rim_at(p_position), 0.0f, p_position)
Vector2 WorldGround::runoff_at(const Vector3 &p_position) const WG_CALL(_runoff_at, contract_ground_runoff_at(p_position), Vector2(), p_position)
float WorldGround::mud_depth_at(const Vector3 &p_position) const WG_CALL(_mud_depth_at, contract_ground_mud_depth_at(p_position), 0.0f, p_position)
float WorldGround::sand_at(const Vector3 &p_position) const WG_CALL(_sand_at, contract_ground_sand_at(p_position), 0.0f, p_position)
float WorldGround::ripple_at(const Vector3 &p_position) const WG_CALL(_ripple_at, contract_ground_ripple_at(p_position), 0.0f, p_position)
float WorldGround::blown_sand_at(const Vector3 &p_position) const WG_CALL(_blown_sand_at, contract_ground_blown_sand_at(p_position), 0.0f, p_position)
Vector2 WorldGround::needle_ice_at(const Vector3 &p_position) const WG_CALL(_needle_ice_at, contract_ground_needle_ice_at(p_position), Vector2(), p_position)
Vector3 WorldGround::rut_guidance_at(const Vector3 &p_contact, const Vector3 &p_lateral, float p_half_width) const WG_CALL(_rut_guidance_at, contract_ground_rut_guidance_at(p_contact, p_lateral, p_half_width), Vector3(), p_contact, p_lateral, p_half_width)

void WorldGround::add_runoff_load(const Vector3 &p_position, float p_mineral_kg, float p_ash_kg) {
	if (GDVIRTUAL_CALL(_add_runoff_load, p_position, p_mineral_kg, p_ash_kg)) {
		return;
	}
	if (runoff_provider && (p_mineral_kg > 0.0f || p_ash_kg > 0.0f)) {
		runoff_provider->contract_runoff_add_load(p_position, MAX(p_mineral_kg, 0.0f), MAX(p_ash_kg, 0.0f));
	}
}

void WorldGround::_bind_methods() {
	ClassDB::bind_method(D_METHOD("add_runoff_load", "position", "mineral_kg", "ash_kg"), &WorldGround::add_runoff_load);
	ClassDB::bind_method(D_METHOD("has_runoff"), &WorldGround::has_runoff);
	// The runoff's post-fire debris flows (WeatherRunoff3D): one event per basin per storm, for the moving flow.
	ADD_SIGNAL(MethodInfo("debris_flow", PropertyInfo(Variant::DICTIONARY, "event")));
	GDVIRTUAL_BIND(_add_runoff_load, "position", "mineral_kg", "ash_kg");
	ClassDB::bind_method(D_METHOD("has_provider"), &WorldGround::has_provider);
	ClassDB::bind_method(D_METHOD("height_at", "position"), &WorldGround::height_at);
	ClassDB::bind_method(D_METHOD("soil_at", "position"), &WorldGround::_soil_at_bind);
	ClassDB::bind_method(D_METHOD("standing_water_at", "position"), &WorldGround::standing_water_at);
	ClassDB::bind_method(D_METHOD("ice_at", "position"), &WorldGround::ice_at);
	ClassDB::bind_method(D_METHOD("frost_at", "position"), &WorldGround::frost_at);
	ClassDB::bind_method(D_METHOD("dent_at", "position"), &WorldGround::dent_at);
	ClassDB::bind_method(D_METHOD("rim_at", "position"), &WorldGround::rim_at);
	ClassDB::bind_method(D_METHOD("runoff_at", "position"), &WorldGround::runoff_at);
	ClassDB::bind_method(D_METHOD("mud_depth_at", "position"), &WorldGround::mud_depth_at);
	ClassDB::bind_method(D_METHOD("sand_at", "position"), &WorldGround::sand_at);
	ClassDB::bind_method(D_METHOD("ripple_at", "position"), &WorldGround::ripple_at);
	ClassDB::bind_method(D_METHOD("blown_sand_at", "position"), &WorldGround::blown_sand_at);
	ClassDB::bind_method(D_METHOD("needle_ice_at", "position"), &WorldGround::needle_ice_at);
	ClassDB::bind_method(D_METHOD("rut_guidance_at", "contact", "lateral", "half_width"), &WorldGround::rut_guidance_at);
	GDVIRTUAL_BIND(_height_at, "position");
	GDVIRTUAL_BIND(_soil_at, "position");
	GDVIRTUAL_BIND(_standing_water_at, "position");
	GDVIRTUAL_BIND(_ice_at, "position");
	GDVIRTUAL_BIND(_frost_at, "position");
	GDVIRTUAL_BIND(_dent_at, "position");
	GDVIRTUAL_BIND(_rim_at, "position");
	GDVIRTUAL_BIND(_runoff_at, "position");
	GDVIRTUAL_BIND(_mud_depth_at, "position");
	GDVIRTUAL_BIND(_sand_at, "position");
	GDVIRTUAL_BIND(_ripple_at, "position");
	GDVIRTUAL_BIND(_blown_sand_at, "position");
	GDVIRTUAL_BIND(_needle_ice_at, "position");
	GDVIRTUAL_BIND(_rut_guidance_at, "contact", "lateral", "half_width");
}

WorldContracts *WorldContracts::singleton = nullptr;

WorldWeather *WorldContracts::weather() {
	static Ref<WorldWeather> fallback; // before the singleton exists or after it goes
	if (singleton == nullptr) {
		if (fallback.is_null()) {
			fallback.instantiate();
		}
		return fallback.ptr();
	}
	if (singleton->weather_override.is_valid()) {
		return singleton->weather_override.ptr();
	}
	return singleton->builtin_weather.ptr();
}

WorldGround *WorldContracts::ground() {
	static Ref<WorldGround> fallback; // before the singleton exists or after it goes
	if (singleton == nullptr) {
		if (fallback.is_null()) {
			fallback.instantiate();
		}
		return fallback.ptr();
	}
	if (singleton->ground_override.is_valid()) {
		return singleton->ground_override.ptr();
	}
	return singleton->builtin_ground.ptr();
}

void WorldContracts::register_ground(WorldGroundProvider *p_provider) {
	if (singleton) {
		singleton->ground_provider = p_provider;
		singleton->builtin_ground->provider = p_provider;
		if (singleton->ground_override.is_valid()) {
			singleton->ground_override->provider = p_provider; // a partial stub falls back to the real one
		}
	}
}

void WorldContracts::unregister_ground(WorldGroundProvider *p_provider) {
	if (singleton && singleton->ground_provider == p_provider) {
		register_ground(nullptr);
	}
}

void WorldContracts::register_runoff(WorldRunoffProvider *p_provider) {
	if (singleton) {
		singleton->runoff_provider = p_provider;
		singleton->builtin_ground->runoff_provider = p_provider;
		if (singleton->ground_override.is_valid()) {
			singleton->ground_override->runoff_provider = p_provider;
		}
	}
}

void WorldContracts::unregister_runoff(WorldRunoffProvider *p_provider) {
	if (singleton && singleton->runoff_provider == p_provider) {
		register_runoff(nullptr);
	}
}

void WorldContracts::set_ground(const Ref<WorldGround> &p_stub) {
	ground_override = p_stub;
	if (ground_override.is_valid()) {
		ground_override->provider = ground_provider;
		ground_override->runoff_provider = runoff_provider;
	}
}

Ref<WorldGround> WorldContracts::get_ground() const {
	return ground_override.is_valid() ? ground_override : builtin_ground;
}

void WorldContracts::register_weather(WorldWeatherProvider *p_provider) {
	if (singleton) {
		singleton->weather_provider = p_provider;
		singleton->builtin_weather->provider = p_provider;
		if (singleton->weather_override.is_valid()) {
			singleton->weather_override->provider = p_provider; // a partial stub falls back to the real one
		}
	}
}

void WorldContracts::unregister_weather(WorldWeatherProvider *p_provider) {
	if (singleton && singleton->weather_provider == p_provider) {
		register_weather(nullptr);
	}
}

WorldWater *WorldContracts::water() {
	static Ref<WorldWater> fallback;
	if (singleton == nullptr) {
		if (fallback.is_null()) {
			fallback.instantiate();
		}
		return fallback.ptr();
	}
	if (singleton->water_override.is_valid()) {
		return singleton->water_override.ptr();
	}
	return singleton->builtin_water.ptr();
}

void WorldContracts::register_water(WorldWaterProvider *p_provider) {
	if (singleton && p_provider && !singleton->builtin_water->providers.has(p_provider)) {
		singleton->builtin_water->providers.push_back(p_provider);
		if (singleton->water_override.is_valid()) {
			singleton->water_override->providers = singleton->builtin_water->providers;
		}
	}
}

void WorldContracts::unregister_water(WorldWaterProvider *p_provider) {
	if (singleton) {
		singleton->builtin_water->providers.erase(p_provider);
		if (singleton->water_override.is_valid()) {
			singleton->water_override->providers = singleton->builtin_water->providers;
		}
	}
}

void WorldContracts::set_water(const Ref<WorldWater> &p_stub) {
	water_override = p_stub;
	if (water_override.is_valid()) {
		water_override->providers = builtin_water->providers; // a partial stub falls back to the real ones
	}
}

Ref<WorldWater> WorldContracts::get_water() const {
	return water_override.is_valid() ? water_override : builtin_water;
}

WorldFire *WorldContracts::fire() {
	static Ref<WorldFire> fallback;
	if (singleton == nullptr) {
		if (fallback.is_null()) {
			fallback.instantiate();
		}
		return fallback.ptr();
	}
	return singleton->fire_override.is_valid() ? singleton->fire_override.ptr() : singleton->builtin_fire.ptr();
}

void WorldContracts::register_fire(WorldFireProvider *p_provider) {
	if (singleton && p_provider && !singleton->builtin_fire->providers.has(p_provider)) {
		singleton->builtin_fire->providers.push_back(p_provider);
	}
}

void WorldContracts::unregister_fire(WorldFireProvider *p_provider) {
	if (singleton) {
		singleton->builtin_fire->providers.erase(p_provider);
	}
}

void WorldContracts::fire_burn_changed(const Rect2 &p_area) {
	if (!singleton) {
		return;
	}
	singleton->builtin_fire->burn_changed(p_area);
	if (singleton->fire_override.is_valid()) {
		singleton->fire_override->burn_changed(p_area);
	}
}

void WorldContracts::set_fire(const Ref<WorldFire> &p_stub) {
	fire_override = p_stub;
}

Ref<WorldFire> WorldContracts::get_fire() const {
	return fire_override.is_valid() ? fire_override : builtin_fire;
}

WorldSnow *WorldContracts::snow() {
	static Ref<WorldSnow> fallback;
	if (singleton == nullptr) {
		if (fallback.is_null()) {
			fallback.instantiate();
		}
		return fallback.ptr();
	}
	return singleton->snow_override.is_valid() ? singleton->snow_override.ptr() : singleton->builtin_snow.ptr();
}

void WorldContracts::register_snow(WorldSnowProvider *p_provider) {
	if (singleton && p_provider) {
		singleton->snow_provider = p_provider;
		singleton->builtin_snow->provider = p_provider;
		if (singleton->snow_override.is_valid()) {
			singleton->snow_override->provider = p_provider;
		}
	}
}

void WorldContracts::unregister_snow(WorldSnowProvider *p_provider) {
	if (singleton && singleton->snow_provider == p_provider) {
		singleton->snow_provider = nullptr;
		singleton->builtin_snow->provider = nullptr;
		if (singleton->snow_override.is_valid()) {
			singleton->snow_override->provider = nullptr;
		}
	}
}

void WorldContracts::set_snow(const Ref<WorldSnow> &p_stub) {
	snow_override = p_stub;
	if (snow_override.is_valid()) {
		snow_override->provider = snow_provider; // a partial stub falls back to the real snow
	}
}

Ref<WorldSnow> WorldContracts::get_snow() const {
	return snow_override.is_valid() ? snow_override : builtin_snow;
}

void WorldContracts::set_weather(const Ref<WorldWeather> &p_stub) {
	weather_override = p_stub;
	if (weather_override.is_valid()) {
		weather_override->provider = weather_provider;
	}
}

Ref<WorldWeather> WorldContracts::get_weather() const {
	return weather_override.is_valid() ? weather_override : builtin_weather;
}

void WorldContracts::clear_overrides() {
	weather_override.unref();
	ground_override.unref();
	water_override.unref();
	fire_override.unref();
	snow_override.unref();
	_clear_water_stubs();
}

// Contact bodies.

WorldContactBody WorldContracts::describe_body(const Node3D *p_node) {
	WorldContactBody b;
	if (p_node == nullptr) {
		return b;
	}
	b.center = p_node->get_global_position();
	bool sized = false;
	for (int c = 0; c < p_node->get_child_count(); c++) {
		const CollisionShape3D *cs = Object::cast_to<CollisionShape3D>(p_node->get_child(c));
		if (!cs || cs->is_disabled() || cs->get_shape().is_null()) {
			continue;
		}
		const Ref<Shape3D> shape = cs->get_shape();
		const Basis basis = cs->get_global_basis();
		const float sc = basis.get_scale().x;
		b.center = cs->get_global_position();
		if (const SphereShape3D *sp = Object::cast_to<SphereShape3D>(shape.ptr())) {
			b.radius = b.half_height = sp->get_radius() * sc;
		} else if (const CapsuleShape3D *cp = Object::cast_to<CapsuleShape3D>(shape.ptr())) {
			b.radius = cp->get_radius() * sc;
			b.half_height = cp->get_height() * 0.5f * sc;
		} else if (const CylinderShape3D *cy = Object::cast_to<CylinderShape3D>(shape.ptr())) {
			if (Math::abs(basis.get_column(1).normalized().y) > 0.7f) {
				b.radius = cy->get_radius() * sc;
				b.half_height = cy->get_height() * 0.5f * sc;
				b.round = false;
			} else {
				b.wheel = true;
				b.round = false;
				b.half_height = cy->get_radius() * sc;
				b.width = cy->get_height() * sc;
				b.radius = b.width * 0.5f;
			}
		} else if (const BoxShape3D *bx = Object::cast_to<BoxShape3D>(shape.ptr())) {
			const Vector3 s = bx->get_size() * 0.5f * sc;
			b.radius = MAX(s.x, s.z);
			b.half_height = s.y;
			b.round = false;
			b.box = true;
			b.width = 2.0f * MIN(s.x, s.z);
		}
		sized = true;
		break;
	}
	if (p_node->has_meta(SNAME("deform_wheel_radius"))) {
		b.center = p_node->get_global_position();
		b.wheel = true;
		b.round = false;
		b.box = false;
		b.half_height = p_node->get_meta(SNAME("deform_wheel_radius"));
		b.width = p_node->has_meta(SNAME("deform_wheel_width")) ? float(p_node->get_meta(SNAME("deform_wheel_width"))) : b.half_height * 0.6f;
		b.radius = b.width * 0.5f;
		sized = true;
	}
	if (p_node->has_meta(SNAME("trample_radius")) && !sized) {
		b.radius = b.half_height = p_node->get_meta(SNAME("trample_radius"));
	}
	if (const RigidBody3D *rb = Object::cast_to<RigidBody3D>(p_node)) {
		b.mass = rb->get_mass();
	}
	if (p_node->has_meta(SNAME("ground_mass"))) {
		b.mass = p_node->get_meta(SNAME("ground_mass"));
	}
	return b;
}

Vector2 WorldContracts::body_extent(const Node3D *p_node, float p_fallback) {
	if (p_node->has_meta(SNAME("trample_radius"))) {
		const float r = p_node->get_meta(SNAME("trample_radius"));
		return Vector2(r, r);
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		const CollisionShape3D *cs = Object::cast_to<CollisionShape3D>(p_node->get_child(i));
		if (!cs || cs->is_disabled() || cs->get_shape().is_null()) {
			continue;
		}
		const Ref<Shape3D> shape = cs->get_shape();
		const float sc = cs->get_global_basis().get_scale().x;
		if (const SphereShape3D *sp = Object::cast_to<SphereShape3D>(shape.ptr())) {
			return Vector2(sp->get_radius(), sp->get_radius()) * sc;
		}
		if (const CapsuleShape3D *cp = Object::cast_to<CapsuleShape3D>(shape.ptr())) {
			return Vector2(cp->get_radius(), cp->get_height() * 0.5f) * sc;
		}
		if (const CylinderShape3D *cy = Object::cast_to<CylinderShape3D>(shape.ptr())) {
			return Vector2(cy->get_radius(), cy->get_height() * 0.5f) * sc;
		}
		if (const BoxShape3D *bx = Object::cast_to<BoxShape3D>(shape.ptr())) {
			const Vector3 s = bx->get_size() * 0.5f * sc;
			return Vector2(MAX(s.x, s.z), s.y);
		}
	}
	return Vector2(p_fallback, p_fallback);
}

Dictionary WorldContracts::describe_body_bind(Node *p_node) const {
	const WorldContactBody b = describe_body(Object::cast_to<Node3D>(p_node));
	Dictionary d;
	d["center"] = b.center;
	d["radius"] = b.radius;
	d["half_height"] = b.half_height;
	d["round"] = b.round;
	d["wheel"] = b.wheel;
	d["box"] = b.box;
	d["width"] = b.width;
	d["mass"] = b.mass;
	return d;
}

void WorldContracts::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_weather", "stub"), &WorldContracts::set_weather);
	ClassDB::bind_method(D_METHOD("get_weather"), &WorldContracts::get_weather);
	ClassDB::bind_method(D_METHOD("set_ground", "stub"), &WorldContracts::set_ground);
	ClassDB::bind_method(D_METHOD("get_ground"), &WorldContracts::get_ground);
	ClassDB::bind_method(D_METHOD("set_water", "stub"), &WorldContracts::set_water);
	ClassDB::bind_method(D_METHOD("get_water"), &WorldContracts::get_water);
	ClassDB::bind_method(D_METHOD("add_water_stub", "stub"), &WorldContracts::add_water_stub);
	ClassDB::bind_method(D_METHOD("set_fire", "stub"), &WorldContracts::set_fire);
	ClassDB::bind_method(D_METHOD("get_fire"), &WorldContracts::get_fire);
	ClassDB::bind_method(D_METHOD("set_snow", "stub"), &WorldContracts::set_snow);
	ClassDB::bind_method(D_METHOD("get_snow"), &WorldContracts::get_snow);
	ClassDB::bind_method(D_METHOD("clear_overrides"), &WorldContracts::clear_overrides);
	ClassDB::bind_method(D_METHOD("describe_body", "node"), &WorldContracts::describe_body_bind);
}

WorldContracts::WorldContracts() {
	singleton = this;
	builtin_weather.instantiate();
	builtin_water.instantiate();
	builtin_ground.instantiate();
	builtin_fire.instantiate();
	builtin_snow.instantiate();
}

WorldContracts::~WorldContracts() {
	_clear_water_stubs();
	if (singleton == this) {
		singleton = nullptr;
	}
}

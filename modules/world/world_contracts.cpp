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
#include "core/config/engine.h"

#include "core/math/geometry_2d.h"
#include "core/templates/hash_set.h"

#include "core/object/class_db.h"

#include <type_traits>
#include "scene/3d/node_3d.h"
#include "servers/physics_3d/physics_server_3d.h"
#include "scene/resources/3d/world_3d.h"
#include "scene/main/window.h"
#include "scene/3d/camera_3d.h"
#include "scene/main/viewport.h"
#ifdef TOOLS_ENABLED
#include "editor/editor_interface.h"
#endif
#include "scene/main/scene_tree.h"
#include "scene/3d/physics/collision_shape_3d.h"
#include "scene/3d/physics/character_body_3d.h"
#include "scene/3d/physics/rigid_body_3d.h"
#include "scene/3d/physics/physical_bone_3d.h"
#include "scene/3d/physics/vehicle_body_3d.h"
#include "scene/resources/3d/convex_polygon_shape_3d.h"
#include "scene/resources/3d/concave_polygon_shape_3d.h"
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
float WorldWeather::glaze_at(const Vector3 &p_position) const WW_CALL(_glaze_at, contract_glaze_at(p_position), 0.0f, p_position)
Vector3 WorldWeather::rime_at(const Vector3 &p_position) const WW_CALL(_rime_at, contract_rime_at(p_position), Vector3(), p_position)
float WorldWeather::valley_fog_top_at(const Vector3 &p_position) const WW_CALL(_valley_fog_top_at, contract_valley_fog_top_at(p_position), float(-Math::INF), p_position)
float WorldWeather::rainfall_at(const Vector3 &p_position) const WW_CALL(_rainfall_at, contract_rainfall_at(p_position), 0.0f, p_position)
float WorldWeather::snowfall_at(const Vector3 &p_position) const WW_CALL(_snowfall_at, contract_snowfall_at(p_position), 0.0f, p_position)
float WorldWeather::smoke_at(const Vector3 &p_position) const WW_CALL(_smoke_at, contract_smoke_at(p_position), 0.0f, p_position)
float WorldWeather::dust_at(const Vector3 &p_position) const WW_CALL(_dust_at, contract_dust_at(p_position), 0.0f, p_position)
float WorldWeather::dust_top_at(const Vector3 &p_position) const WW_CALL(_dust_top_at, contract_dust_top_at(p_position), 0.0f, p_position)
float WorldWeather::dust_concentration_at(const Vector3 &p_position) const WW_CALL(_dust_concentration_at, contract_dust_concentration_at(p_position), 0.0f, p_position)
// A stub that only says the column gets it mixed through 2 km.
float WorldWeather::smoke_concentration_at(const Vector3 &p_position) const {
	float r = 0.0f;
	if (GDVIRTUAL_CALL(_smoke_concentration_at, p_position, r)) {
		return r;
	}
	if (provider && !GDVIRTUAL_IS_OVERRIDDEN(_smoke_at)) {
		return provider->contract_smoke_concentration_at(p_position);
	}
	return smoke_at(p_position) / 2000.0f;
}
float WorldWeather::obstacle_height_at(const Vector3 &p_position) const WW_CALL(_obstacle_height_at, contract_obstacle_height_at(p_position), 0.0f, p_position)
float WorldWeather::obstacle_lee_at(const Vector3 &p_position) const WW_CALL(_obstacle_lee_at, contract_obstacle_lee_at(p_position), 1.0f, p_position)
float WorldWeather::fog_water_at(const Vector3 &p_position) const WW_CALL(_fog_water_at, contract_fog_water_at(p_position), 0.0f, p_position)
float WorldWeather::fog_drip_at(const Vector3 &p_position) const WW_CALL(_fog_drip_at, contract_fog_drip_at(p_position), 0.0f, p_position)
float WorldWeather::sea_spray_at(const Vector3 &p_position) const WW_CALL(_sea_spray_at, contract_sea_spray_at(p_position), 0.0f, p_position)
float WorldWeather::salt_at(const Vector3 &p_position) const WW_CALL(_salt_at, contract_salt_at(p_position), 0.0f, p_position)
float WorldWeather::canopy_water_at(const Vector3 &p_position) const WW_CALL(_canopy_water_at, contract_canopy_water_at(p_position), 0.0f, p_position)
float WorldWeather::canopy_wetness_at(const Vector3 &p_position) const WW_CALL(_canopy_wetness_at, contract_canopy_wetness_at(p_position), 0.0f, p_position)
float WorldWeather::canopy_drip_at(const Vector3 &p_position) const WW_CALL(_canopy_drip_at, contract_canopy_drip_at(p_position), 0.0f, p_position)
float WorldWeather::ground_water_input_at(const Vector3 &p_position) const WW_CALL(_ground_water_input_at, contract_ground_water_input_at(p_position), 0.0f, p_position)
float WorldWeather::cloud_cover_at(const Vector3 &p_position) const WW_CALL(_cloud_cover_at, contract_cloud_cover_at(p_position), 0.0f, p_position)
float WorldWeather::high_cloud_cover_at(const Vector3 &p_position) const WW_CALL(_high_cloud_cover_at, contract_high_cloud_cover_at(p_position), 0.0f, p_position)
float WorldWeather::mid_cloud_cover_at(const Vector3 &p_position) const WW_CALL(_mid_cloud_cover_at, contract_mid_cloud_cover_at(p_position), 0.0f, p_position)
float WorldWeather::cloud_effector_at(const Vector3 &p_position) const WW_CALL(_cloud_effector_at, contract_cloud_effector_at(p_position), 0.0f, p_position)
float WorldWeather::cloud_base_at(const Vector3 &p_position) const WW_CALL(_cloud_base_at, contract_cloud_base_at(p_position), 1500.0f, p_position)
float WorldWeather::orographic_factor_at(const Vector3 &p_position) const WW_CALL(_orographic_factor_at, contract_orographic_factor_at(p_position), 1.0f, p_position)
float WorldWeather::mountain_wave_at(const Vector3 &p_position) const WW_CALL(_mountain_wave_at, contract_mountain_wave_at(p_position), 0.0f, p_position)
float WorldWeather::mountain_wave_velocity_at(const Vector3 &p_position) const WW_CALL(_mountain_wave_velocity_at, contract_mountain_wave_velocity_at(p_position), 0.0f, p_position)
float WorldWeather::wave_cloud_density_at(const Vector3 &p_position) const WW_CALL(_wave_cloud_density_at, contract_wave_cloud_density_at(p_position), 0.0f, p_position)
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
	ClassDB::bind_method(D_METHOD("glaze_at", "position"), &WorldWeather::glaze_at);
	ClassDB::bind_method(D_METHOD("rime_at", "position"), &WorldWeather::rime_at);
	ClassDB::bind_method(D_METHOD("valley_fog_top_at", "position"), &WorldWeather::valley_fog_top_at);
	ClassDB::bind_method(D_METHOD("mean_wind_at", "position"), &WorldWeather::mean_wind_at);
	ClassDB::bind_method(D_METHOD("wind_aloft_at", "position", "altitude"), &WorldWeather::wind_aloft_at);
	ClassDB::bind_method(D_METHOD("terrain_lee_at", "position"), &WorldWeather::terrain_lee_at);
	ClassDB::bind_method(D_METHOD("rainfall_at", "position"), &WorldWeather::rainfall_at);
	ClassDB::bind_method(D_METHOD("snowfall_at", "position"), &WorldWeather::snowfall_at);
	ClassDB::bind_static_method("WorldWeather", D_METHOD("washout_rate", "rain_mm_h", "snow_mm_h", "coarse"), &WorldWeather::washout_rate);
	ClassDB::bind_method(D_METHOD("washout_at", "position", "coarse"), &WorldWeather::washout_at);
	ClassDB::bind_method(D_METHOD("smoke_at", "position"), &WorldWeather::smoke_at);
	ClassDB::bind_method(D_METHOD("smoke_concentration_at", "position"), &WorldWeather::smoke_concentration_at);
	ClassDB::bind_method(D_METHOD("dust_at", "position"), &WorldWeather::dust_at);
	ClassDB::bind_method(D_METHOD("dust_top_at", "position"), &WorldWeather::dust_top_at);
	ClassDB::bind_method(D_METHOD("dust_concentration_at", "position"), &WorldWeather::dust_concentration_at);
	ClassDB::bind_method(D_METHOD("obstacle_height_at", "position"), &WorldWeather::obstacle_height_at);
	ClassDB::bind_method(D_METHOD("obstacle_lee_at", "position"), &WorldWeather::obstacle_lee_at);
	ClassDB::bind_method(D_METHOD("fog_water_at", "position"), &WorldWeather::fog_water_at);
	ClassDB::bind_method(D_METHOD("fog_drip_at", "position"), &WorldWeather::fog_drip_at);
	ClassDB::bind_method(D_METHOD("sea_spray_at", "position"), &WorldWeather::sea_spray_at);
	ClassDB::bind_method(D_METHOD("salt_at", "position"), &WorldWeather::salt_at);
	ClassDB::bind_method(D_METHOD("canopy_water_at", "position"), &WorldWeather::canopy_water_at);
	ClassDB::bind_method(D_METHOD("canopy_wetness_at", "position"), &WorldWeather::canopy_wetness_at);
	ClassDB::bind_method(D_METHOD("canopy_drip_at", "position"), &WorldWeather::canopy_drip_at);
	ClassDB::bind_method(D_METHOD("ground_water_input_at", "position"), &WorldWeather::ground_water_input_at);
	ClassDB::bind_method(D_METHOD("day_of_year"), &WorldWeather::day_of_year);
	ClassDB::bind_method(D_METHOD("season_at", "position", "day"), &WorldWeather::season_at, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("cloud_cover_at", "position"), &WorldWeather::cloud_cover_at);
	ClassDB::bind_method(D_METHOD("high_cloud_cover_at", "position"), &WorldWeather::high_cloud_cover_at);
	ClassDB::bind_method(D_METHOD("mid_cloud_cover_at", "position"), &WorldWeather::mid_cloud_cover_at);
	ClassDB::bind_method(D_METHOD("cloud_effector_at", "position"), &WorldWeather::cloud_effector_at);
	ClassDB::bind_method(D_METHOD("cloud_base_at", "position"), &WorldWeather::cloud_base_at);
	ClassDB::bind_method(D_METHOD("orographic_factor_at", "position"), &WorldWeather::orographic_factor_at);
	ClassDB::bind_method(D_METHOD("mountain_wave_at", "position"), &WorldWeather::mountain_wave_at);
	ClassDB::bind_method(D_METHOD("mountain_wave_velocity_at", "position"), &WorldWeather::mountain_wave_velocity_at);
	ClassDB::bind_method(D_METHOD("wave_cloud_density_at", "position"), &WorldWeather::wave_cloud_density_at);
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
	GDVIRTUAL_BIND(_glaze_at, "position");
	GDVIRTUAL_BIND(_rime_at, "position");
	GDVIRTUAL_BIND(_valley_fog_top_at, "position");
	GDVIRTUAL_BIND(_mean_wind_at, "position");
	GDVIRTUAL_BIND(_wind_aloft_at, "position", "altitude");
	GDVIRTUAL_BIND(_terrain_lee_at, "position");
	GDVIRTUAL_BIND(_rainfall_at, "position");
	GDVIRTUAL_BIND(_snowfall_at, "position");
	GDVIRTUAL_BIND(_smoke_at, "position");
	GDVIRTUAL_BIND(_smoke_concentration_at, "position");
	GDVIRTUAL_BIND(_dust_at, "position");
	GDVIRTUAL_BIND(_dust_top_at, "position");
	GDVIRTUAL_BIND(_dust_concentration_at, "position");
	GDVIRTUAL_BIND(_obstacle_height_at, "position");
	GDVIRTUAL_BIND(_obstacle_lee_at, "position");
	GDVIRTUAL_BIND(_fog_water_at, "position");
	GDVIRTUAL_BIND(_fog_drip_at, "position");
	GDVIRTUAL_BIND(_sea_spray_at, "position");
	GDVIRTUAL_BIND(_salt_at, "position");
	GDVIRTUAL_BIND(_canopy_water_at, "position");
	GDVIRTUAL_BIND(_canopy_wetness_at, "position");
	GDVIRTUAL_BIND(_canopy_drip_at, "position");
	GDVIRTUAL_BIND(_ground_water_input_at, "position");
	GDVIRTUAL_BIND(_day_of_year);
	GDVIRTUAL_BIND(_season_at, "position", "day");
	GDVIRTUAL_BIND(_cloud_cover_at, "position");
	GDVIRTUAL_BIND(_high_cloud_cover_at, "position");
	GDVIRTUAL_BIND(_mid_cloud_cover_at, "position");
	GDVIRTUAL_BIND(_cloud_effector_at, "position");
	GDVIRTUAL_BIND(_cloud_base_at, "position");
	GDVIRTUAL_BIND(_orographic_factor_at, "position");
	GDVIRTUAL_BIND(_mountain_wave_at, "position");
	GDVIRTUAL_BIND(_mountain_wave_velocity_at, "position");
	GDVIRTUAL_BIND(_wave_cloud_density_at, "position");
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

float WorldWater::height_except(const Vector3 &p_position, const WorldWaterProvider *p_skip, Vector3 *r_flow, Vector2 *r_sediment) const {
	if (get_script_instance() != nullptr) {
		// A script stub answers for all the water.
		const float h = height_at(p_position);
		if (Math::is_finite(h)) {
			if (r_flow) {
				*r_flow = flow_at(p_position);
			}
			if (r_sediment) {
				*r_sediment = sediment_at(p_position);
			}
		}
		return h;
	}
	const WorldWaterProvider *above = nullptr;
	const WorldWaterProvider *below = nullptr;
	float above_h = Math::INF;
	float below_h = -Math::INF;
	for (const WorldWaterProvider *w : providers) {
		if (w == p_skip) {
			continue;
		}
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
	const WorldWaterProvider *top = above ? above : below;
	if (top) {
		if (r_flow) {
			*r_flow = top->contract_water_flow_at(p_position);
		}
		if (r_sediment) {
			*r_sediment = top->contract_water_sediment_at(p_position);
		}
	}
	return above ? above_h : below_h;
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

void WorldWater::wave(const Vector3 &p_position, const Vector3 &p_direction, float p_height, float p_period, float p_width) {
	if (GDVIRTUAL_CALL(_wave, p_position, p_direction, p_height, p_period, p_width)) {
		return;
	}
	for (WorldWaterProvider *w : providers) {
		w->contract_water_wave(p_position, p_direction, p_height, p_period, p_width);
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

Vector2 WorldWater::sediment_deposit_at(const Vector3 &p_position) const {
	Vector2 r;
	if (GDVIRTUAL_CALL(_sediment_deposit_at, p_position, r)) {
		return r;
	}
	for (const WorldWaterProvider *w : providers) {
		r += w->contract_water_sediment_deposit_at(p_position);
	}
	return r;
}

Vector4 WorldWater::high_water_at(const Vector3 &p_position) const {
	Vector4 r(-Math::INF, -1.0f, 0.0f, 0.0f);
	if (GDVIRTUAL_CALL(_high_water_at, p_position, r)) {
		return r;
	}
	for (const WorldWaterProvider *w : providers) {
		const Vector4 h = w->contract_water_high_water_at(p_position);
		if (h.x > r.x) {
			r = h;
		}
	}
	return r;
}

float WorldWater::breaking_at(const Vector3 &p_position) const {
	float r = 0.0f;
	if (GDVIRTUAL_CALL(_breaking_at, p_position, r)) {
		return MAX(r, 0.0f);
	}
	for (const WorldWaterProvider *w : providers) {
		r = MAX(r, w->contract_water_breaking_at(p_position));
	}
	return r;
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

void WorldWater::add_sediment_mass(const Vector3 &p_position, float p_mineral_kg, float p_ash_kg) {
	if (GDVIRTUAL_CALL(_add_sediment_mass, p_position, p_mineral_kg, p_ash_kg)) {
		return;
	}
	float h;
	if (WorldWaterProvider *top = _top_at(p_position, h)) {
		top->contract_water_add_sediment_mass(p_position, MAX(p_mineral_kg, 0.0f), MAX(p_ash_kg, 0.0f));
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
	ClassDB::bind_method(D_METHOD("wave", "position", "direction", "height", "period", "width"), &WorldWater::wave);
	ClassDB::bind_method(D_METHOD("sediment_at", "position"), &WorldWater::sediment_at);
	ClassDB::bind_method(D_METHOD("sediment_deposit_at", "position"), &WorldWater::sediment_deposit_at);
	ClassDB::bind_method(D_METHOD("breaking_at", "position"), &WorldWater::breaking_at);
	ClassDB::bind_method(D_METHOD("high_water_at", "position"), &WorldWater::high_water_at);
	ClassDB::bind_method(D_METHOD("add_sediment", "position", "mineral_kg_s", "ash_kg_s"), &WorldWater::add_sediment);
	ClassDB::bind_method(D_METHOD("add_sediment_mass", "position", "mineral_kg", "ash_kg"), &WorldWater::add_sediment_mass);

	GDVIRTUAL_BIND(_height_at, "position");
	GDVIRTUAL_BIND(_flow_at, "position");
	GDVIRTUAL_BIND(_wake_depth_at, "position");
	GDVIRTUAL_BIND(_disturb, "owner", "position", "radius", "strength");
	GDVIRTUAL_BIND(_clear, "owner");
	GDVIRTUAL_BIND(_impulse, "position", "radius", "strength");
	GDVIRTUAL_BIND(_wave, "position", "direction", "height", "period", "width");
	GDVIRTUAL_BIND(_sediment_at, "position");
	GDVIRTUAL_BIND(_sediment_deposit_at, "position");
	GDVIRTUAL_BIND(_breaking_at, "position");
	GDVIRTUAL_BIND(_high_water_at, "position");
	GDVIRTUAL_BIND(_add_sediment, "position", "mineral_kg_s", "ash_kg_s");
	GDVIRTUAL_BIND(_add_sediment_mass, "position", "mineral_kg", "ash_kg");
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

float WorldSnow::albedo_at(const Vector3 &p_position) const {
	float r = 0.0f;
	if (GDVIRTUAL_CALL(_albedo_at, p_position, r)) {
		return r;
	}
	if (!provider) {
		return has_provider() ? 0.8f : 0.0f;
	}
	const float a = provider->contract_snow_albedo_at(p_position);
	return a >= 0.0f ? a : 0.8f;
}

float WorldSnow::grip_at(const Vector3 &p_position) const {
	float r = -1.0f;
	if (GDVIRTUAL_CALL(_grip_at, p_position, r)) {
		return r;
	}
	return provider ? provider->contract_snow_grip_at(p_position) : -1.0f;
}

float WorldSnow::avalanche_pressure_at(const Vector3 &p_position) const {
	float r = 0.0f;
	if (GDVIRTUAL_CALL(_avalanche_pressure_at, p_position, r)) {
		return r;
	}
	return provider ? provider->contract_snow_avalanche_pressure_at(p_position) : 0.0f;
}

float WorldSnow::avalanche_peak_pressure_at(const Vector3 &p_position) const {
	float r = 0.0f;
	if (GDVIRTUAL_CALL(_avalanche_peak_pressure_at, p_position, r)) {
		return r;
	}
	return provider ? provider->contract_snow_avalanche_peak_pressure_at(p_position) : 0.0f;
}

float WorldSnow::canopy_load_at(const Vector3 &p_position) const {
	float r = 0.0f;
	if (GDVIRTUAL_CALL(_canopy_load_at, p_position, r)) {
		return r;
	}
	return provider ? provider->contract_snow_canopy_load_at(p_position) : 0.0f;
}

float WorldSnow::canopy_capacity_at(const Vector3 &p_position) const {
	float r = 0.0f;
	if (GDVIRTUAL_CALL(_canopy_capacity_at, p_position, r)) {
		return r;
	}
	return provider ? provider->contract_snow_canopy_capacity_at(p_position) : 0.0f;
}

float WorldSnow::shake_canopy(const Vector3 &p_position, float p_radius, float p_share) {
	float r = 0.0f;
	if (GDVIRTUAL_CALL(_shake_canopy, p_position, p_radius, p_share, r)) {
		return r;
	}
	return provider ? provider->contract_snow_shake_canopy(p_position, p_radius, p_share) : 0.0f;
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
	ClassDB::bind_method(D_METHOD("albedo_at", "position"), &WorldSnow::albedo_at);
	ClassDB::bind_method(D_METHOD("grip_at", "position"), &WorldSnow::grip_at);
	ClassDB::bind_method(D_METHOD("avalanche_pressure_at", "position"), &WorldSnow::avalanche_pressure_at);
	ClassDB::bind_method(D_METHOD("avalanche_peak_pressure_at", "position"), &WorldSnow::avalanche_peak_pressure_at);
	ClassDB::bind_method(D_METHOD("canopy_load_at", "position"), &WorldSnow::canopy_load_at);
	ClassDB::bind_method(D_METHOD("canopy_capacity_at", "position"), &WorldSnow::canopy_capacity_at);
	ClassDB::bind_method(D_METHOD("shake_canopy", "position", "radius", "share"), &WorldSnow::shake_canopy);
	GDVIRTUAL_BIND(_depth_at, "position");
	GDVIRTUAL_BIND(_density_at, "position");
	GDVIRTUAL_BIND(_wetness_at, "position");
	GDVIRTUAL_BIND(_surface_at, "position");
	GDVIRTUAL_BIND(_crust_at, "position");
	GDVIRTUAL_BIND(_lake_ice_at, "position");
	GDVIRTUAL_BIND(_add_impurity, "position", "radius", "mg_per_m2");
	GDVIRTUAL_BIND(_impurity_at, "position");
	GDVIRTUAL_BIND(_albedo_at, "position");
	GDVIRTUAL_BIND(_grip_at, "position");
	GDVIRTUAL_BIND(_avalanche_pressure_at, "position");
	GDVIRTUAL_BIND(_avalanche_peak_pressure_at, "position");
	GDVIRTUAL_BIND(_canopy_load_at, "position");
	GDVIRTUAL_BIND(_canopy_capacity_at, "position");
	GDVIRTUAL_BIND(_shake_canopy, "position", "radius", "share");
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
	void contract_water_wave(const Vector3 &p_position, const Vector3 &p_direction, float p_height, float p_period, float p_width) override { stub->wave(p_position, p_direction, p_height, p_period, p_width); }
	Vector2 contract_water_sediment_at(const Vector3 &p_position) const override { return stub->sediment_at(p_position); }
	Vector2 contract_water_sediment_deposit_at(const Vector3 &p_position) const override { return stub->sediment_deposit_at(p_position); }
	float contract_water_breaking_at(const Vector3 &p_position) const override { return stub->breaking_at(p_position); }
	Vector4 contract_water_high_water_at(const Vector3 &p_position) const override { return stub->high_water_at(p_position); }
	void contract_water_add_sediment(const Vector3 &p_position, float p_mineral_kg_s, float p_ash_kg_s) override { stub->add_sediment(p_position, p_mineral_kg_s, p_ash_kg_s); }
	void contract_water_add_sediment_mass(const Vector3 &p_position, float p_mineral_kg, float p_ash_kg) override { stub->add_sediment_mass(p_position, p_mineral_kg, p_ash_kg); }
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

float WorldGround::known_height_at(const Vector3 &p_position) const {
	float r = 0.0f;
	if (GDVIRTUAL_CALL(_height_at, p_position, r)) {
		return r;
	}
	return provider ? provider->contract_ground_height_at(p_position) : NAN;
}

float WorldGround::height_at(const Vector3 &p_position) const {
	float r = 0.0f;
	if (GDVIRTUAL_CALL(_height_at, p_position, r)) {
		return r;
	}
	if (provider) {
		// A provider declines with NaN where it knows nothing (a ground patch's point far off its lattice with no
		// height source): the colliders below answer instead.
		const float h = provider->contract_ground_height_at(p_position);
		if (Math::is_finite(h)) {
			return h;
		}
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

float WorldGround::track_mud(const Vector3 &p_position, float p_radius, float p_depth, float p_wet, const Vector4 &p_print) {
	float r = 0.0f;
	if (GDVIRTUAL_CALL(_track_mud, p_position, p_radius, p_depth, p_wet, p_print, r)) {
		return r;
	}
	return provider && p_depth > 0.0f ? provider->contract_ground_track_mud(p_position, p_radius, p_depth, p_wet, p_print) : 0.0f;
}

Vector3 WorldGround::coarse_cover_at(const Vector3 &p_position) const {
	Vector3 r;
	if (GDVIRTUAL_CALL(_coarse_cover_at, p_position, r)) {
		return r;
	}
	const LocalVector<WorldCoverProvider *> *list = WorldContracts::cover_providers_list();
	if (list) {
		for (const WorldCoverProvider *c : *list) {
			const Vector3 v = c->contract_cover_at(p_position);
			if (v.x + v.y > r.x + r.y || (v.x + v.y == r.x + r.y && v.z > r.z)) {
				r = v;
			}
		}
	}
	return r;
}

PackedFloat32Array WorldGround::stones_near(const Rect2 &p_rect) const {
	PackedFloat32Array r;
	if (GDVIRTUAL_CALL(_stones_near, p_rect, r)) {
		return r;
	}
	const LocalVector<WorldCoverProvider *> *list = WorldContracts::cover_providers_list();
	if (!list) {
		return r;
	}
	LocalVector<float> all;
	for (const WorldCoverProvider *c : *list) {
		c->contract_stones_near(p_rect, all);
	}
	r.resize(all.size());
	if (!all.is_empty()) {
		memcpy(r.ptrw(), all.ptr(), sizeof(float) * all.size());
	}
	return r;
}

void WorldGround::coarse_cover_points(const Vector2 *p_points, int p_count, Vector3 *r_cover) const {
	if (p_count <= 0) {
		return;
	}
	if (get_script_instance() != nullptr) {
		for (int i = 0; i < p_count; i++) {
			r_cover[i] = coarse_cover_at(Vector3(p_points[i].x, 0.0f, p_points[i].y));
		}
		return;
	}
	for (int i = 0; i < p_count; i++) {
		r_cover[i] = Vector3();
	}
	const LocalVector<WorldCoverProvider *> *list = WorldContracts::cover_providers_list();
	if (!list || list->is_empty()) {
		return;
	}
	if (list->size() == 1) {
		(*list)[0]->contract_cover_points(p_points, p_count, r_cover);
		return;
	}
	LocalVector<Vector3> one;
	one.resize(p_count);
	for (const WorldCoverProvider *c : *list) {
		c->contract_cover_points(p_points, p_count, one.ptr());
		for (int i = 0; i < p_count; i++) {
			const Vector3 &v = one[i];
			Vector3 &r = r_cover[i];
			if (v.x + v.y > r.x + r.y || (v.x + v.y == r.x + r.y && v.z > r.z)) {
				r = v;
			}
		}
	}
}

Color WorldGround::albedo_at(const Vector3 &p_position) const {
	Color r;
	if (GDVIRTUAL_CALL(_albedo_at, p_position, r)) {
		return r;
	}
	Color c(0, 0, 0, 0);
	const LocalVector<WorldGroundColorProvider *> *list = WorldContracts::ground_color_providers_list();
	if (list) {
		for (const WorldGroundColorProvider *g : *list) {
			const Color v = g->contract_ground_albedo_at(p_position);
			if (v.a >= 0.0f) {
				// Rain soaks the ground by its porosity, as the materials draw it (weather_wetness_apply on level
				// ground, Lagarde 2013): a saturated porous soil keeps 0.2 of its dry albedo.
				const float wet = CLAMP(WorldContracts::weather()->surface_wetness_at(p_position), 0.0f, 1.0f);
				const float f = Math::lerp(1.0f, Math::lerp(1.0f, 0.2f, CLAMP(v.a, 0.0f, 1.0f)), wet);
				c = Color(v.r * f, v.g * f, v.b * f, 1.0f);
				break;
			}
		}
	}
	// The plant cover over it (WorldPlantColorProvider): the sward and the placed plants hide the ground by the share
	// they cover. Leaves are not darkened by rain the way soil is (a film on a leaf adds sheen, not much darkening).
	const LocalVector<WorldPlantColorProvider *> *plants = WorldContracts::plant_color_providers_list();
	if (plants) {
		for (const WorldPlantColorProvider *pp : *plants) {
			const Color v = pp->contract_plant_albedo_at(p_position);
			const float k = CLAMP(v.a, 0.0f, 1.0f);
			if (k <= 0.0f) {
				continue;
			}
			if (c.a > 0.0f) {
				c = Color(Math::lerp(c.r, v.r, k), Math::lerp(c.g, v.g, k), Math::lerp(c.b, v.b, k), c.a);
			} else {
				c = Color(v.r, v.g, v.b, k); // No ground colour known: the plants alone, as far as they cover.
			}
		}
	}
	// The snowpack over it, by the materials' cover on level ground (snow_cover_weight: the facets fill from about
	// 1 cm, all buried by about 17 cm).
	const WorldSnow *snow = WorldContracts::snow();
	if (snow->has_provider()) {
		const float depth = snow->depth_at(p_position);
		if (depth > 0.0f) {
			const float d = MIN(depth, 1.0f);
			const float cover = MAX(Math::smoothstep(1.0f - 1.2f * d, 1.2f - 1.2f * d, 1.0f), Math::smoothstep(0.9f, 1.0f, d));
			if (cover > 0.0f) {
				const float a = snow->albedo_at(p_position);
				const Color sc(a, a, a);
				if (c.a > 0.0f) {
					c = Color(Math::lerp(c.r, sc.r, cover), Math::lerp(c.g, sc.g, cover), Math::lerp(c.b, sc.b, cover), 1.0f);
				} else {
					c = Color(sc.r, sc.g, sc.b, cover);
				}
			}
		}
	}
	return c;
}

void WorldGround::_bind_methods() {
	ClassDB::bind_method(D_METHOD("coarse_cover_at", "position"), &WorldGround::coarse_cover_at);
	GDVIRTUAL_BIND(_coarse_cover_at, "position");
	ClassDB::bind_method(D_METHOD("stones_near", "rect"), &WorldGround::stones_near);
	GDVIRTUAL_BIND(_stones_near, "rect");
	ClassDB::bind_method(D_METHOD("albedo_at", "position"), &WorldGround::albedo_at);
	GDVIRTUAL_BIND(_albedo_at, "position");
	ClassDB::bind_method(D_METHOD("relief_at", "position", "steps", "recess"), &WorldGround::relief_at, DEFVAL(true), DEFVAL(true));
	GDVIRTUAL_BIND(_relief_at, "position");
	ClassDB::bind_method(D_METHOD("recess_at", "position"), &WorldGround::recess_at);
	ClassDB::bind_method(D_METHOD("has_recess"), &WorldGround::has_recess);
	GDVIRTUAL_BIND(_recess_at, "position");
	ClassDB::bind_method(D_METHOD("frost_sorted_at", "position"), &WorldGround::frost_sorted_at);
	GDVIRTUAL_BIND(_frost_sorted_at, "position");
	ClassDB::bind_method(D_METHOD("recess_vector_at", "position"), &WorldGround::recess_vector_at);
	GDVIRTUAL_BIND(_recess_vector_at, "position");
	ClassDB::bind_method(D_METHOD("notify_recess_changed", "box"), &WorldGround::notify_recess_changed);
	ADD_SIGNAL(MethodInfo("recess_changed", PropertyInfo(Variant::AABB, "box")));
	ClassDB::bind_method(D_METHOD("notify_relief_changed", "area"), &WorldGround::notify_relief_changed);
	ADD_SIGNAL(MethodInfo("relief_changed", PropertyInfo(Variant::RECT2, "area")));
	ClassDB::bind_method(D_METHOD("add_runoff_load", "position", "mineral_kg", "ash_kg"), &WorldGround::add_runoff_load);
	ClassDB::bind_method(D_METHOD("track_mud", "position", "radius", "depth", "wet", "print"), &WorldGround::track_mud);
	ClassDB::bind_method(D_METHOD("has_runoff"), &WorldGround::has_runoff);
	// The runoff's post-fire debris flows (WeatherRunoff3D): one event per basin per storm, for the moving flow.
	ADD_SIGNAL(MethodInfo("debris_flow", PropertyInfo(Variant::DICTIONARY, "event")));
	GDVIRTUAL_BIND(_add_runoff_load, "position", "mineral_kg", "ash_kg");
	GDVIRTUAL_BIND(_track_mud, "position", "radius", "depth", "wet", "print");
	ClassDB::bind_method(D_METHOD("has_provider"), &WorldGround::has_provider);
	ClassDB::bind_method(D_METHOD("height_at", "position"), &WorldGround::height_at);
	ClassDB::bind_method(D_METHOD("known_height_at", "position"), &WorldGround::known_height_at);
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

bool WorldContracts::is_editor_preview(const Node *p_node) {
	return p_node && Engine::get_singleton()->is_editor_hint() && p_node->is_inside_tree();
}

Camera3D *WorldContracts::get_view_camera(const Node *p_node) {
	if (!p_node || !p_node->is_inside_tree()) {
		return nullptr;
	}
#ifdef TOOLS_ENABLED
	if (Engine::get_singleton()->is_editor_hint() && EditorInterface::get_singleton()) {
		SubViewport *vp = EditorInterface::get_singleton()->get_editor_viewport_3d(0);
		if (vp && vp->get_camera_3d()) {
			return vp->get_camera_3d();
		}
	}
#endif
	Viewport *vp = p_node->get_viewport();
	return vp ? vp->get_camera_3d() : nullptr;
}

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

void WorldContracts::register_cover(WorldCoverProvider *p_provider) {
	if (singleton && p_provider && singleton->cover_providers.find(p_provider) < 0) {
		singleton->cover_providers.push_back(p_provider);
	}
}

void WorldContracts::unregister_cover(WorldCoverProvider *p_provider) {
	if (singleton) {
		singleton->cover_providers.erase(p_provider);
	}
}

void WorldContracts::register_climate(WorldClimateProvider *p_provider) {
	if (singleton && p_provider && singleton->climate_providers.find(p_provider) < 0) {
		singleton->climate_providers.push_back(p_provider);
	}
}

void WorldContracts::unregister_climate(WorldClimateProvider *p_provider) {
	if (singleton) {
		singleton->climate_providers.erase(p_provider);
	}
}

float WorldContracts::climate_precipitation_factor_at(const Vector3 &p_position) {
	if (singleton) {
		for (const WorldClimateProvider *c : singleton->climate_providers) {
			const float f = c->contract_precipitation_factor_at(p_position);
			if (f >= 0.0f) {
				return f;
			}
		}
	}
	return 1.0f;
}

float WorldContracts::bare_share_for_precipitation(float p_mm) {
	return 0.9f * (1.0f - Math::smoothstep(200.0f, 700.0f, p_mm));
}

float WorldContracts::climate_bare_share_at(const Vector3 &p_position) {
	WeatherSeason::Climate cl;
	const WorldWeather *w = weather();
	if (!w || !w->season_climate(cl)) {
		return -1.0f;
	}
	const float dry = bare_share_for_precipitation(cl.precipitation * climate_precipitation_factor_at(p_position));
	const float cold = bare_share_for_july(cl.temperature_july - cl.lapse_rate_july * (float(p_position.y) - cl.reference_height) * 0.001f);
	return 1.0f - (1.0f - dry) * (1.0f - cold);
}

float WorldContracts::bare_share_for_july(float p_july_c) {
	return 0.97f * (1.0f - Math::smoothstep(2.0f, 11.0f, p_july_c));
}

float WorldContracts::stature_for_july(float p_july_c) {
	return 0.3f + 0.7f * Math::smoothstep(3.0f, 11.0f, p_july_c);
}

float WorldContracts::sorted_keep(float p_stones) {
	return 0.02f + 0.18f * Math::smoothstep(0.15f, 0.4f, p_stones) * (1.0f - Math::smoothstep(0.6f, 0.85f, p_stones));
}

float WorldContracts::climate_july_at(const Vector3 &p_position) {
	WeatherSeason::Climate cl;
	const WorldWeather *w = weather();
	if (!w || !w->season_climate(cl)) {
		return 99.0f;
	}
	return cl.temperature_july - cl.lapse_rate_july * (float(p_position.y) - cl.reference_height) * 0.001f;
}

float WorldContracts::cured_share_for_precipitation(float p_mm) {
	return 1.0f - Math::smoothstep(500.0f, 1000.0f, p_mm);
}

float WorldContracts::greenness_for_season(float p_soil_moisture, float p_temperature) {
	return CLAMP(Math::smoothstep(0.1f, 0.4f, p_soil_moisture) * Math::smoothstep(-8.0f, 2.0f, p_temperature), 0.0f, 1.0f);
}

float WorldContracts::cured_share_for_season(float p_soil_moisture, float p_temperature) {
	return 1.0f - greenness_for_season(p_soil_moisture, p_temperature);
}

float WorldContracts::climate_cured_share_at(const Vector3 &p_position) {
	WeatherSeason::Climate cl;
	const WorldWeather *w = weather();
	if (!w || !w->season_climate(cl)) {
		return -1.0f;
	}
	const Dictionary s = w->season_at(p_position);
	if (s.has("soil_moisture")) {
		return cured_share_for_season(float(s["soil_moisture"]), float(s.get("temperature", 10.0f)));
	}
	return cured_share_for_precipitation(cl.precipitation * climate_precipitation_factor_at(p_position));
}

// The terrain shader's value noise (tm_vn), bit for bit: an integer hash at the lattice, smoothstep between.
static float wc_vn_hash(const Vector3 &p_p) {
	const uint32_t qx = uint32_t(int32_t(p_p.x));
	const uint32_t qy = uint32_t(int32_t(p_p.y));
	const uint32_t qz = uint32_t(int32_t(p_p.z));
	uint32_t h = (qx * 1597334677u) ^ (qy * 3812015801u) ^ (qz * 2741598763u);
	h ^= h >> 16u;
	h *= 0x7feb352du;
	h ^= h >> 15u;
	h *= 0x846ca68bu;
	h ^= h >> 16u;
	return float(h >> 8u) * (1.0f / 16777216.0f);
}

static float wc_vn(const Vector3 &p_p) {
	const Vector3 i(Math::floor(p_p.x), Math::floor(p_p.y), Math::floor(p_p.z));
	Vector3 f = p_p - i;
	f = f * f * (Vector3(3, 3, 3) - 2.0f * f);
	auto h = [&](float dx, float dy, float dz) { return wc_vn_hash(i + Vector3(dx, dy, dz)); };
	const float fx = float(f.x), fy = float(f.y), fz = float(f.z);
	const float a = Math::lerp(Math::lerp(h(0, 0, 0), h(1, 0, 0), fx), Math::lerp(h(0, 1, 0), h(1, 1, 0), fx), fy);
	const float b = Math::lerp(Math::lerp(h(0, 0, 1), h(1, 0, 1), fx), Math::lerp(h(0, 1, 1), h(1, 1, 1), fx), fy);
	return Math::lerp(a, b, fz);
}

float WorldContracts::bare_patch(const Vector3 &p_position, float p_share, float p_footprint) {
	if (p_share <= 0.0f) {
		return 0.0f;
	}
	const float v = 0.65f * wc_vn(Vector3(p_position.x / 4.0f, 3.7f, p_position.z / 4.0f)) + 0.35f * wc_vn(Vector3(p_position.x / 1.3f, 8.1f, p_position.z / 1.3f));
	const float b = CLAMP(p_share, 0.01f, 0.99f);
	const float t = 0.5f + 0.16f * Math::log(b / (1.0f - b)) / 1.7f;
	const float e = MAX(0.03f, p_footprint * 0.2f);
	const float px = 1.0f - Math::smoothstep(t - e, t + e, v);
	return Math::lerp(px, p_share, Math::smoothstep(0.15f, 0.6f, p_footprint));
}

float WorldContracts::climate_bare_patch_at(const Vector3 &p_position) {
	return bare_patch(p_position, climate_bare_share_at(p_position), 0.0f);
}

Vector3 WorldContracts::terracette_at(const Vector3 &p_position, const Vector2 &p_gradient) {
	const float steep = p_gradient.length();
	const float form = Math::smoothstep(0.47f, 0.7f, steep) * (1.0f - Math::smoothstep(1.0f, 1.25f, steep));
	if (form <= 0.0f) {
		return Vector3();
	}
	const float x = p_position.x;
	const float z = p_position.z;
	const float sin_s = steep / Math::sqrt(1.0f + steep * steep);
	const float rise = TERRACETTE_RISE;
	const float wander = (wc_vn(Vector3(x * 0.09f + 41.0f, 5.3f, z * 0.09f)) - 0.5f) * 0.9f + (wc_vn(Vector3(x * 0.37f + 7.0f, 9.1f, z * 0.37f)) - 0.5f) * 0.25f;
	const float v = float(p_position.y) / rise + wander * sin_s / rise;
	const float ph = v - Math::floor(v);
	float tread = Math::smoothstep(0.0f, 0.05f, ph) * (1.0f - Math::smoothstep(TERRACETTE_TREAD - 0.04f, TERRACETTE_TREAD + 0.03f, ph));
	tread *= form * Math::smoothstep(0.25f, 0.45f, wc_vn(Vector3(x * 0.05f + 13.0f, 2.9f, z * 0.05f)));
	return Vector3(tread, ph, form);
}

// The step profile in units of the rise: -ph over the tread, back to 0 over the riser, less its mean; and its integral.
static float wc_tc_o(float p_ph) {
	const float t = WorldContracts::TERRACETTE_TREAD;
	const float ph = p_ph - Math::floor(p_ph);
	return (ph < t ? -ph : -t * (1.0f - ph) / (1.0f - t)) + 0.5f * t;
}
static float wc_tc_int(float p_x) {
	const float t = WorldContracts::TERRACETTE_TREAD;
	const float ph = p_x - Math::floor(p_x);
	return ph < t ? -0.5f * ph * ph + 0.5f * t * ph : t / (2.0f * (1.0f - t)) * ((1.0f - ph) * (1.0f - ph) - (1.0f - t) * (1.0f - t)) + 0.5f * t * (ph - t);
}

Vector2 WorldContracts::terracette_step(const Vector3 &p_position, const Vector2 &p_gradient, float p_width, float p_share) {
	if (p_share <= 0.0f) {
		return Vector2();
	}
	const Vector3 t = terracette_at(p_position, p_gradient);
	if (t.z <= 0.0f) {
		return Vector2();
	}
	const float steep = p_gradient.length();
	// The step's strength: the layout's tread at the middle of this step's tread (its fade along the slope included).
	const float k = terracette_at(Vector3(p_position.x, p_position.y - (t.y - 0.5f * TERRACETTE_TREAD) * TERRACETTE_RISE, p_position.z), p_gradient).x * p_share;
	const float d = MAX(0.5f * p_width * steep / TERRACETTE_RISE, 0.004f);
	if (k <= 0.0f || d >= 0.5f) {
		return Vector2();
	}
	const float f = 1.0f - Math::smoothstep(0.3f, 0.5f, d); // the last of it fades rather than stops
	return Vector2(TERRACETTE_RISE * k * (wc_tc_int(t.y + d) - wc_tc_int(t.y - d)), k * (wc_tc_o(t.y + d) - wc_tc_o(t.y - d))) * (f / (2.0f * d));
}

float WorldContracts::terracette_soil_share(const Color &p_geology) {
	if (Math::is_nan(p_geology.r)) {
		return 0.0f;
	}
	return Math::smoothstep(0.05f, 0.2f, p_geology.r) * (1.0f - Math::smoothstep(0.02f, 0.2f, MAX(CLAMP(p_geology.g, 0.0f, 1.0f), Math::abs(p_geology.b))));
}

String WorldContracts::terracette_shader_code() {
	return R"(
// WorldContracts::terracette_at: x the tread, y the phase up the step, z the slope's forming.
vec3 wc_terracette(vec3 p, vec2 grad) {
	float steep = length(grad);
	float form = smoothstep(0.47, 0.7, steep) * (1.0 - smoothstep(1.0, 1.25, steep));
	if (form <= 0.0) {
		return vec3(0.0);
	}
	float sin_s = steep / sqrt(1.0 + steep * steep);
	float rise = 2.0; // WorldContracts::TERRACETTE_RISE
	float wander = (wc_vn(vec3(p.x * 0.09 + 41.0, 5.3, p.z * 0.09)) - 0.5) * 0.9 + (wc_vn(vec3(p.x * 0.37 + 7.0, 9.1, p.z * 0.37)) - 0.5) * 0.25;
	float ph = fract(p.y / rise + wander * sin_s / rise);
	float tread = smoothstep(0.0, 0.05, ph) * (1.0 - smoothstep(0.26, 0.33, ph)); // WorldContracts::TERRACETTE_TREAD 0.3
	tread *= form * smoothstep(0.25, 0.45, wc_vn(vec3(p.x * 0.05 + 13.0, 2.9, p.z * 0.05)));
	return vec3(tread, ph, form);
}
// WorldContracts::terracette_step's profile (the tread's share 0.3) and its integral.
float wc_tc_o(float x) {
	float ph = fract(x);
	return (ph < 0.3 ? -ph : -0.3 * (1.0 - ph) / 0.7) + 0.15;
}
float wc_tc_int(float x) {
	float ph = fract(x);
	return ph < 0.3 ? -0.5 * ph * ph + 0.15 * ph : 0.3 / 1.4 * ((1.0 - ph) * (1.0 - ph) - 0.49) + 0.15 * (ph - 0.3);
}
// WorldContracts::terracette_step: x the steps' height over the smooth ground (m), y the change of its steepness,
// box-filtered over width metres along the fall line; share the producer's (terrain_bank_relief.y) times
// wc_terracette_soil. Blades, stones and decals call it at their roots to stand on the steps the ground draws.
vec2 wc_terracette_step(vec3 p, vec2 grad, float width, float share) {
	if (share <= 0.0) {
		return vec2(0.0);
	}
	vec3 t = wc_terracette(p, grad);
	if (t.z <= 0.0) {
		return vec2(0.0);
	}
	float steep = length(grad);
	float k = wc_terracette(vec3(p.x, p.y - (t.y - 0.15) * 2.0, p.z), grad).x * share;
	float d = max(0.5 * width * steep / 2.0, 0.004);
	if (k <= 0.0 || d >= 0.5) {
		return vec2(0.0);
	}
	float f = 1.0 - smoothstep(0.3, 0.5, d);
	return vec2(2.0 * k * (wc_tc_int(t.y + d) - wc_tc_int(t.y - d)), k * (wc_tc_o(t.y + d) - wc_tc_o(t.y - d))) * (f / (2.0 * d));
}
// WorldContracts::terracette_soil_share from the generator's geology (soil depth, rock, talus).
float wc_terracette_soil(vec4 g) {
	return smoothstep(0.05, 0.2, g.x) * (1.0 - smoothstep(0.02, 0.2, max(clamp(g.y, 0.0, 1.0), abs(g.z))));
}
)";
}

String WorldContracts::climate_bare_shader_code() {
	// WC_CLIMATE_CODE: code included before or after this (GrassField3D's turf) can tell whether these are declared yet.
	return R"(
#define WC_CLIMATE_CODE
float wc_vn_hash(vec3 p) {
	uvec3 q = uvec3(ivec3(p));
	uint h = (q.x * 1597334677u) ^ (q.y * 3812015801u) ^ (q.z * 2741598763u);
	h ^= h >> 16u;
	h *= 0x7feb352du;
	h ^= h >> 15u;
	h *= 0x846ca68bu;
	h ^= h >> 16u;
	return float(h >> 8u) * (1.0 / 16777216.0);
}
float wc_vn(vec3 p) {
	vec3 i = floor(p);
	vec3 f = fract(p);
	f = f * f * (3.0 - 2.0 * f);
	return mix(mix(mix(wc_vn_hash(i), wc_vn_hash(i + vec3(1, 0, 0)), f.x), mix(wc_vn_hash(i + vec3(0, 1, 0)), wc_vn_hash(i + vec3(1, 1, 0)), f.x), f.y),
			mix(mix(wc_vn_hash(i + vec3(0, 0, 1)), wc_vn_hash(i + vec3(1, 0, 1)), f.x), mix(wc_vn_hash(i + vec3(0, 1, 1)), wc_vn_hash(i + vec3(1, 1, 1)), f.x), f.y), f.z);
}
// The year's precipitation at p, mm: TerrainDataMap3D's climate map (g, m a year) where it reaches, else the weather's
// season climate (x mm a year, w 1 when set); -1 with neither.
float wc_climate_mm(vec3 p) {
	float mm = -1.0;
	if (terrain_climate_rect.z > 0.0) {
		vec2 uv = (p.xz - terrain_climate_rect.xy) / terrain_climate_rect.zw;
		if (all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0)))) {
			mm = textureLod(terrain_climate_map, uv, 0.0).g * 1000.0;
		}
	}
	if (mm < 0.0 && weather_climate.w > 0.0) {
		mm = weather_climate.x;
	}
	return mm;
}
// WeatherMap3D's season climate in July: x July mean air temperature C at z (the reference height, m), y the July lapse
// rate K/km, w 1 when set.
global uniform vec4 weather_climate_july;
// WorldContracts::climate_july_at: the July mean at p (p.y the ground's height), 99 with no climate.
float wc_climate_july(vec3 p) {
	return weather_climate_july.w > 0.0 ? weather_climate_july.x - weather_climate_july.y * (p.y - weather_climate_july.z) * 0.001 : 99.0;
}
// WorldContracts::climate_bare_share_at: the dry share (bare_share_for_precipitation) and the cold one
// (bare_share_for_july) together (0 with no climate).
float wc_climate_bare(vec3 p) {
	float mm = wc_climate_mm(p);
	float dry = mm < 0.0 ? 0.0 : 0.9 * (1.0 - smoothstep(200.0, 700.0, mm));
	float cold = 0.97 * (1.0 - smoothstep(2.0, 11.0, wc_climate_july(p)));
	return 1.0 - (1.0 - dry) * (1.0 - cold);
}
// WorldContracts::stature_for_july at p: the cover's height over its summer height (1 with no climate).
float wc_climate_stature(vec3 p) {
	return 0.3 + 0.7 * smoothstep(3.0, 11.0, wc_climate_july(p));
}
// On frost-sorted ground (RockField3D's terrain_frost_map, g the stones' share): the share of plants kept, about a
// fiftieth on the churned fine centres and on the stones, a fifth on the margins between (WorldContracts::sorted_keep).
float wc_sorted_keep(float stones) {
	return 0.02 + 0.18 * smoothstep(0.15, 0.4, stones) * (1.0 - smoothstep(0.6, 0.85, stones));
}
// WorldContracts::cured_share_for_season from the weather's season (weather_climate y soil moisture, z temperature),
// else cured_share_for_precipitation from the climate map (0 with neither).
float wc_climate_cured(vec3 p) {
	if (weather_climate.w > 0.0) {
		return 1.0 - clamp(smoothstep(0.1, 0.4, weather_climate.y) * smoothstep(-8.0, 2.0, weather_climate.z), 0.0, 1.0);
	}
	float mm = wc_climate_mm(p);
	return mm < 0.0 ? 0.0 : 1.0 - smoothstep(500.0, 1000.0, mm);
}
// WorldContracts::bare_patch: 1 bare soil, 0 cover.
float wc_bare_patch(vec3 p, float bare, float fp) {
	if (bare <= 0.0) {
		return 0.0;
	}
	float v = 0.65 * wc_vn(vec3(p.x / 4.0, 3.7, p.z / 4.0)) + 0.35 * wc_vn(vec3(p.x / 1.3, 8.1, p.z / 1.3));
	float b = clamp(bare, 0.01, 0.99);
	float t = 0.5 + 0.16 * log(b / (1.0 - b)) / 1.7;
	float e = max(0.03, fp * 0.2);
	float px = 1.0 - smoothstep(t - e, t + e, v);
	return mix(px, bare, smoothstep(0.15, 0.6, fp));
}
)";
}

void WorldContracts::register_ground_color(WorldGroundColorProvider *p_provider) {
	if (singleton && p_provider && singleton->ground_color_providers.find(p_provider) < 0) {
		singleton->ground_color_providers.push_back(p_provider);
	}
}

void WorldContracts::unregister_ground_color(WorldGroundColorProvider *p_provider) {
	if (singleton) {
		singleton->ground_color_providers.erase(p_provider);
	}
}

void WorldContracts::register_relief(WorldReliefProvider *p_provider) {
	if (singleton && p_provider && singleton->relief_providers.find(p_provider) < 0) {
		singleton->relief_providers.push_back(p_provider);
	}
}

void WorldContracts::unregister_relief(WorldReliefProvider *p_provider) {
	if (singleton) {
		singleton->relief_providers.erase(p_provider);
	}
}

void WorldGround::notify_relief_changed(const Rect2 &p_area) {
	emit_signal(SNAME("relief_changed"), p_area);
}

void WorldContracts::register_recess(WorldRecessProvider *p_provider) {
	if (singleton && p_provider && singleton->recess_providers.find(p_provider) < 0) {
		singleton->recess_providers.push_back(p_provider);
	}
}

void WorldContracts::unregister_recess(WorldRecessProvider *p_provider) {
	if (singleton) {
		singleton->recess_providers.erase(p_provider);
	}
}

void WorldContracts::register_frost_sorted(WorldFrostSortedProvider *p_provider) {
	if (singleton && p_provider && singleton->frost_sorted_providers.find(p_provider) < 0) {
		singleton->frost_sorted_providers.push_back(p_provider);
	}
}

void WorldContracts::unregister_frost_sorted(WorldFrostSortedProvider *p_provider) {
	if (singleton) {
		singleton->frost_sorted_providers.erase(p_provider);
	}
}

Vector2 WorldGround::frost_sorted_at(const Vector3 &p_position) const {
	Vector2 r;
	if (GDVIRTUAL_CALL(_frost_sorted_at, p_position, r)) {
		return Vector2(CLAMP(r.x, 0.0f, 1.0f), CLAMP(r.y, 0.0f, 1.0f));
	}
	Vector2 best;
	const LocalVector<WorldFrostSortedProvider *> *list = WorldContracts::frost_sorted_providers_list();
	if (list) {
		for (const WorldFrostSortedProvider *p : *list) {
			const Vector2 v = p->contract_frost_sorted_at(p_position);
			if (Math::is_finite(v.x) && Math::is_finite(v.y) && v.x > best.x) {
				best = Vector2(CLAMP(v.x, 0.0f, 1.0f), CLAMP(v.y, 0.0f, 1.0f));
			}
		}
	}
	return best;
}

void WorldGround::notify_recess_changed(const AABB &p_box) {
	emit_signal(SNAME("recess_changed"), p_box);
}

bool WorldGround::has_recess() const {
	if (GDVIRTUAL_IS_OVERRIDDEN(_recess_at)) {
		return true;
	}
	const LocalVector<WorldRecessProvider *> *list = WorldContracts::recess_providers_list();
	return list && !list->is_empty();
}

Vector2 WorldGround::recess_at(const Vector3 &p_position) const {
	Vector2 r;
	if (GDVIRTUAL_CALL(_recess_at, p_position, r)) {
		return Vector2(MAX(r.x, 0.0f), CLAMP(r.y, 0.0f, 1.0f));
	}
	Vector2 best;
	const LocalVector<WorldRecessProvider *> *list = WorldContracts::recess_providers_list();
	if (list) {
		for (const WorldRecessProvider *p : *list) {
			const Vector2 v = p->contract_recess_at(p_position);
			if (Math::is_finite(v.x) && Math::is_finite(v.y) && v.x * v.y > best.x * best.y) {
				best = Vector2(MAX(v.x, 0.0f), CLAMP(v.y, 0.0f, 1.0f));
			}
		}
	}
	return best;
}

Vector4 WorldGround::recess_vector_at(const Vector3 &p_position) const {
	Vector4 r;
	if (GDVIRTUAL_CALL(_recess_vector_at, p_position, r)) {
		return r;
	}
	Vector4 best;
	float best_len = 0.0f;
	const LocalVector<WorldRecessProvider *> *list = WorldContracts::recess_providers_list();
	if (list) {
		for (const WorldRecessProvider *p : *list) {
			const Vector4 v = p->contract_recess_vector_at(p_position);
			if (!Math::is_finite(v.x) || !Math::is_finite(v.y) || !Math::is_finite(v.z) || !Math::is_finite(v.w)) {
				continue;
			}
			const float l = Vector3(v.x, v.y, v.z).length() + 0.001f * v.w;
			if (l > best_len) {
				best_len = l;
				best = Vector4(v.x, v.y, v.z, CLAMP(v.w, 0.0f, 1.0f));
			}
		}
	}
	return best;
}

void WorldGround::recess_points(const Vector3 *p_points, int p_count, Vector2 *r_recess) const {
	const LocalVector<WorldRecessProvider *> *list = WorldContracts::recess_providers_list();
	if (GDVIRTUAL_IS_OVERRIDDEN(_recess_at) || !list || list->size() != 1) {
		for (int i = 0; i < p_count; i++) {
			r_recess[i] = recess_at(p_points[i]);
		}
		return;
	}
	(*list)[0]->contract_recess_points(p_points, p_count, r_recess);
	for (int i = 0; i < p_count; i++) {
		const Vector2 v = r_recess[i];
		r_recess[i] = (Math::is_finite(v.x) && Math::is_finite(v.y)) ? Vector2(MAX(v.x, 0.0f), CLAMP(v.y, 0.0f, 1.0f)) : Vector2();
	}
}

Vector2 WorldGround::relief_at(const Vector3 &p_position, bool p_steps, bool p_recess) const {
	Vector2 r;
	if (GDVIRTUAL_CALL(_relief_at, p_position, r)) {
		return r;
	}
	const LocalVector<WorldReliefProvider *> *list = WorldContracts::relief_providers_list();
	if (list) {
		for (const WorldReliefProvider *p : *list) {
			const Vector2 v = p->contract_relief_at(p_position, p_steps, p_recess);
			if (Math::is_finite(v.x)) {
				return Vector2(v.x, CLAMP(v.y, 0.0f, 1.0f));
			}
		}
	}
	return Vector2();
}

void WorldContracts::register_plant_color(WorldPlantColorProvider *p_provider) {
	if (singleton && p_provider && singleton->plant_color_providers.find(p_provider) < 0) {
		singleton->plant_color_providers.push_back(p_provider);
	}
}

void WorldContracts::unregister_plant_color(WorldPlantColorProvider *p_provider) {
	if (singleton) {
		singleton->plant_color_providers.erase(p_provider);
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

// Karst sinks and springs.

void WorldCaves::swallets(const Rect2 &p_rect, LocalVector<WorldSwallet> &r_out) const {
	Array a;
	if (GDVIRTUAL_CALL(_get_swallets, p_rect, a)) {
		for (int i = 0; i < a.size(); i++) {
			const Dictionary d = a[i];
			WorldSwallet w;
			w.position = d.get("position", Vector3());
			w.capacity = float(d.get("capacity", 0.0));
			w.resurgence = int(d.get("resurgence", -1));
			w.lag_days = float(d.get("lag_days", 0.0));
			w.id = int(d.get("id", -1));
			r_out.push_back(w);
		}
		return;
	}
	for (WorldCavesProvider *p : providers) {
		p->contract_swallets(p_rect, r_out);
	}
}

void WorldCaves::resurgences(const Rect2 &p_rect, LocalVector<WorldResurgence> &r_out) const {
	Array a;
	if (GDVIRTUAL_CALL(_get_resurgences, p_rect, a)) {
		for (int i = 0; i < a.size(); i++) {
			const Dictionary d = a[i];
			WorldResurgence w;
			w.id = int(d.get("id", -1));
			w.position = d.get("position", Vector3());
			const Vector3 dir = d.get("direction", Vector3());
			w.direction = Vector3(dir.x, 0, dir.z).length() > 1e-6f ? Vector3(dir.x, 0, dir.z).normalized() : Vector3();
			w.base_flow = float(d.get("base_flow", 0.0));
			r_out.push_back(w);
		}
		return;
	}
	for (WorldCavesProvider *p : providers) {
		p->contract_resurgences(p_rect, r_out);
	}
}

Array WorldCaves::get_swallets(const Rect2 &p_rect) const {
	LocalVector<WorldSwallet> l;
	swallets(p_rect, l);
	Array out;
	for (const WorldSwallet &w : l) {
		Dictionary d;
		d["position"] = w.position;
		d["capacity"] = w.capacity;
		d["resurgence"] = w.resurgence;
		d["lag_days"] = w.lag_days;
		d["id"] = w.id;
		out.push_back(d);
	}
	return out;
}

Array WorldCaves::get_resurgences(const Rect2 &p_rect) const {
	LocalVector<WorldResurgence> l;
	resurgences(p_rect, l);
	Array out;
	for (const WorldResurgence &w : l) {
		Dictionary d;
		d["id"] = w.id;
		d["position"] = w.position;
		d["direction"] = w.direction;
		d["base_flow"] = w.base_flow;
		out.push_back(d);
	}
	return out;
}

void WorldCaves::notify_karst_changed(const Rect2 &p_rect) {
	emit_signal(SNAME("karst_changed"), p_rect);
}

void WorldCaves::_bind_methods() {
	ClassDB::bind_method(D_METHOD("has_provider"), &WorldCaves::has_provider);
	ClassDB::bind_method(D_METHOD("get_swallets", "rect"), &WorldCaves::get_swallets);
	ClassDB::bind_method(D_METHOD("get_resurgences", "rect"), &WorldCaves::get_resurgences);
	ClassDB::bind_method(D_METHOD("notify_karst_changed", "rect"), &WorldCaves::notify_karst_changed);
	GDVIRTUAL_BIND(_get_swallets, "rect");
	GDVIRTUAL_BIND(_get_resurgences, "rect");
	ADD_SIGNAL(MethodInfo("karst_changed", PropertyInfo(Variant::RECT2, "rect")));
}

WorldCaves *WorldContracts::caves() {
	static Ref<WorldCaves> fallback;
	if (singleton == nullptr) {
		if (fallback.is_null()) {
			fallback.instantiate();
		}
		return fallback.ptr();
	}
	return singleton->caves_override.is_valid() ? singleton->caves_override.ptr() : singleton->builtin_caves.ptr();
}

void WorldContracts::register_caves(WorldCavesProvider *p_provider) {
	if (singleton && p_provider && !singleton->builtin_caves->providers.has(p_provider)) {
		singleton->builtin_caves->providers.push_back(p_provider);
	}
}

void WorldContracts::unregister_caves(WorldCavesProvider *p_provider) {
	if (singleton) {
		singleton->builtin_caves->providers.erase(p_provider);
	}
}

void WorldContracts::karst_changed(const Rect2 &p_rect) {
	if (!singleton) {
		return;
	}
	singleton->builtin_caves->notify_karst_changed(p_rect);
	if (singleton->caves_override.is_valid()) {
		singleton->caves_override->notify_karst_changed(p_rect);
	}
}

void WorldContracts::set_caves(const Ref<WorldCaves> &p_stub) {
	caves_override = p_stub;
}

Ref<WorldCaves> WorldContracts::get_caves() const {
	return caves_override.is_valid() ? caves_override : builtin_caves;
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
	// A burn changes the stand's age there too.
	singleton->builtin_stand->notify_stand_changed(p_area);
	if (singleton->stand_override.is_valid()) {
		singleton->stand_override->notify_stand_changed(p_area);
	}
}

WorldStand *WorldContracts::stand() {
	static Ref<WorldStand> fallback;
	if (singleton == nullptr) {
		if (fallback.is_null()) {
			fallback.instantiate();
		}
		return fallback.ptr();
	}
	return singleton->stand_override.is_valid() ? singleton->stand_override.ptr() : singleton->builtin_stand.ptr();
}

void WorldContracts::set_stand(const Ref<WorldStand> &p_stub) {
	stand_override = p_stub;
}

Ref<WorldStand> WorldContracts::get_stand() const {
	return stand_override.is_valid() ? stand_override : builtin_stand;
}

// Stand age.

void WorldStand::_age_and_kind(const Vector3 &p_position, float &r_age, int &r_kind) const {
	r_age = -1.0f;
	r_kind = DISTURBANCE_NONE;
	const WorldFire *f = WorldContracts::fire();
	const float burn = f->burn_age_at(p_position);
	if (burn >= 0.0f) {
		const float scorch = f->crown_scorch_at(p_position);
		// No scorch on record (an authored burn scar) counts as stand-replacing; a low scorch was a surface fire.
		if (!(scorch > 0.0f && scorch < SURFACE_FIRE_SCORCH)) {
			r_age = burn;
			r_kind = DISTURBANCE_FIRE;
		}
	}
	if (events.is_empty()) {
		return;
	}
	const Vector2 p(p_position.x, p_position.z);
	auto check = [&](uint32_t p_i) {
		const Event &e = events[p_i];
		if (e.kind == DISTURBANCE_ROOTING) {
			return; // the field layer's, not the stand's (rooting_age_at)
		}
		if (p.distance_squared_to(e.center) <= e.radius * e.radius && (r_age < 0.0f || e.age < r_age)) {
			r_age = e.age;
			r_kind = e.kind;
		}
	};
	for (uint32_t i : wide) {
		check(i);
	}
	const Vector2i c(int(Math::floor(p.x / GRID_CELL)), int(Math::floor(p.y / GRID_CELL)));
	if (const LocalVector<uint32_t> *in = grid.getptr(c)) {
		for (uint32_t i : *in) {
			check(i);
		}
	}
}

void WorldStand::_rebuild_grid() {
	grid.clear();
	wide.clear();
	for (uint32_t i = 0; i < events.size(); i++) {
		const Event &e = events[i];
		const Vector2i a(int(Math::floor((e.center.x - e.radius) / GRID_CELL)), int(Math::floor((e.center.y - e.radius) / GRID_CELL)));
		const Vector2i b(int(Math::floor((e.center.x + e.radius) / GRID_CELL)), int(Math::floor((e.center.y + e.radius) / GRID_CELL)));
		if (b.x - a.x > 16 || b.y - a.y > 16) {
			wide.push_back(i);
			continue;
		}
		for (int z = a.y; z <= b.y; z++) {
			for (int x = a.x; x <= b.x; x++) {
				grid[Vector2i(x, z)].push_back(i);
			}
		}
	}
}

float WorldStand::stand_age_at(const Vector3 &p_position) const {
	float r = -1.0f;
	if (GDVIRTUAL_CALL(_stand_age_at, p_position, r)) {
		return r;
	}
	int kind;
	_age_and_kind(p_position, r, kind);
	return r;
}

int WorldStand::disturbance_at(const Vector3 &p_position) const {
	int r = DISTURBANCE_NONE;
	if (GDVIRTUAL_CALL(_disturbance_at, p_position, r)) {
		return r;
	}
	float age;
	_age_and_kind(p_position, age, r);
	return r;
}

void WorldStand::_report(const Rect2 &p_area) {
	notify_stand_changed(p_area);
}

int WorldStand::add_disturbance(const Vector3 &p_center, float p_radius, int p_kind, float p_age_years) {
	ERR_FAIL_COND_V_MSG(p_kind <= DISTURBANCE_NONE, 0, "A disturbance needs a kind (fire, wind, beetle, beaver, rooting).");
	Event e;
	e.id = next_id++;
	e.center = Vector2(p_center.x, p_center.z);
	e.radius = MAX(p_radius, 0.0f);
	e.kind = p_kind;
	e.age = MAX(p_age_years, 0.0f);
	events.push_back(e);
	_rebuild_grid();
	_report(Rect2(e.center - Vector2(e.radius, e.radius), Vector2(2.0f * e.radius, 2.0f * e.radius)));
	return e.id;
}

int WorldStand::add_disturbances(const PackedVector3Array &p_centers, float p_radius, int p_kind, float p_age_years) {
	ERR_FAIL_COND_V_MSG(p_kind <= DISTURBANCE_NONE, 0, "A disturbance needs a kind.");
	if (p_centers.is_empty()) {
		return 0;
	}
	const float r = MAX(p_radius, 0.0f);
	Rect2 area;
	bool first = true;
	HashSet<Vector2i> covered; // the new discs by GRID_CELL, to find the events they replace
	for (const Vector3 &c : p_centers) {
		const Rect2 d(Vector2(c.x - r, c.z - r), Vector2(2.0f * r, 2.0f * r));
		area = first ? d : area.merge(d);
		first = false;
		covered.insert(Vector2i(int(Math::floor(c.x / GRID_CELL)), int(Math::floor(c.z / GRID_CELL))));
	}
	// Replace the same kind's events lying inside a new disc (their whole disc within it).
	for (uint32_t i = 0; i < events.size();) {
		const Event &e = events[i];
		bool gone = false;
		if (e.kind == p_kind && e.radius <= r * 1.01f && covered.has(Vector2i(int(Math::floor(e.center.x / GRID_CELL)), int(Math::floor(e.center.y / GRID_CELL))))) {
			for (const Vector3 &c : p_centers) {
				if (Vector2(c.x, c.z).distance_to(e.center) + e.radius <= r * 1.01f) {
					gone = true;
					break;
				}
			}
		}
		if (gone) {
			events.remove_at_unordered(i);
		} else {
			i++;
		}
	}
	for (const Vector3 &c : p_centers) {
		Event e;
		e.id = next_id++;
		e.center = Vector2(c.x, c.z);
		e.radius = r;
		e.kind = p_kind;
		e.age = MAX(p_age_years, 0.0f);
		events.push_back(e);
	}
	_rebuild_grid();
	_report(area);
	return p_centers.size();
}

void WorldStand::set_disturbance_age(int p_id, float p_age_years) {
	for (Event &e : events) {
		if (e.id == p_id) {
			e.age = MAX(p_age_years, 0.0f);
			_report(Rect2(e.center - Vector2(e.radius, e.radius), Vector2(2.0f * e.radius, 2.0f * e.radius)));
			return;
		}
	}
}

void WorldStand::remove_disturbance(int p_id) {
	for (uint32_t i = 0; i < events.size(); i++) {
		if (events[i].id == p_id) {
			const Event e = events[i];
			events.remove_at(i);
			_rebuild_grid();
			_report(Rect2(e.center - Vector2(e.radius, e.radius), Vector2(2.0f * e.radius, 2.0f * e.radius)));
			return;
		}
	}
}

void WorldStand::clear_disturbances() {
	Rect2 area;
	for (uint32_t i = 0; i < events.size(); i++) {
		const Rect2 r(events[i].center - Vector2(events[i].radius, events[i].radius), Vector2(2.0f * events[i].radius, 2.0f * events[i].radius));
		area = i == 0 ? r : area.merge(r);
	}
	const bool any = !events.is_empty();
	events.clear();
	_rebuild_grid();
	if (any) {
		_report(area);
	}
}

static Rect2 _disc_rect(const Vector2 &p_c, float p_r) {
	return Rect2(p_c - Vector2(p_r, p_r), Vector2(2.0f * p_r, 2.0f * p_r));
}

void WorldStand::_rebuild_browse_grid() {
	browse_grid.clear();
	browse_wide.clear();
	for (uint32_t i = 0; i < browse.size(); i++) {
		const Browse &b = browse[i];
		const Vector2i a(int(Math::floor((b.center.x - b.radius) / GRID_CELL)), int(Math::floor((b.center.y - b.radius) / GRID_CELL)));
		const Vector2i e(int(Math::floor((b.center.x + b.radius) / GRID_CELL)), int(Math::floor((b.center.y + b.radius) / GRID_CELL)));
		if (e.x - a.x > 16 || e.y - a.y > 16) {
			browse_wide.push_back(i);
			continue;
		}
		for (int z = a.y; z <= e.y; z++) {
			for (int x = a.x; x <= e.x; x++) {
				browse_grid[Vector2i(x, z)].push_back(i);
			}
		}
	}
}

void WorldStand::_browse_query(const Vector3 &p_position, float &r_pressure, float &r_reach) const {
	r_pressure = 0.0f;
	r_reach = 0.0f;
	if (browse.is_empty()) {
		return;
	}
	const Vector2 p(p_position.x, p_position.z);
	auto check = [&](uint32_t p_i) {
		const Browse &b = browse[p_i];
		const float d = p.distance_to(b.center);
		if (d > b.radius) {
			return;
		}
		const float v = b.pressure * (1.0f - Math::smoothstep(0.8f * b.radius, b.radius, d));
		if (v > r_pressure) {
			r_pressure = v;
			r_reach = b.reach;
		}
	};
	for (uint32_t i : browse_wide) {
		check(i);
	}
	if (const LocalVector<uint32_t> *in = browse_grid.getptr(Vector2i(int(Math::floor(p.x / GRID_CELL)), int(Math::floor(p.y / GRID_CELL))))) {
		for (uint32_t i : *in) {
			check(i);
		}
	}
}

int WorldStand::add_browse(const Vector3 &p_center, float p_radius, float p_pressure, float p_reach_m) {
	Browse b;
	b.id = next_id++;
	b.center = Vector2(p_center.x, p_center.z);
	b.radius = MAX(p_radius, 0.0f);
	b.pressure = CLAMP(p_pressure, 0.0f, 1.0f);
	b.reach = MAX(p_reach_m, 0.0f);
	browse.push_back(b);
	_rebuild_browse_grid();
	_report(_disc_rect(b.center, b.radius));
	return b.id;
}

void WorldStand::set_browse(int p_id, float p_pressure) {
	for (Browse &b : browse) {
		if (b.id == p_id) {
			b.pressure = CLAMP(p_pressure, 0.0f, 1.0f);
			_report(_disc_rect(b.center, b.radius));
			return;
		}
	}
}

void WorldStand::remove_browse(int p_id) {
	for (uint32_t i = 0; i < browse.size(); i++) {
		if (browse[i].id == p_id) {
			const Rect2 r = _disc_rect(browse[i].center, browse[i].radius);
			browse.remove_at(i);
			_rebuild_browse_grid();
			_report(r);
			return;
		}
	}
}

void WorldStand::clear_browse() {
	if (browse.is_empty()) {
		return;
	}
	Rect2 area = _disc_rect(browse[0].center, browse[0].radius);
	for (const Browse &b : browse) {
		area = area.merge(_disc_rect(b.center, b.radius));
	}
	browse.clear();
	_rebuild_browse_grid();
	_report(area);
}

float WorldStand::rooting_age_at(const Vector3 &p_position) const {
	float r = -1.0f;
	if (GDVIRTUAL_CALL(_rooting_age_at, p_position, r)) {
		return r;
	}
	const Vector2 p(p_position.x, p_position.z);
	auto check = [&](uint32_t p_i) {
		const Event &e = events[p_i];
		if (e.kind == DISTURBANCE_ROOTING && p.distance_squared_to(e.center) <= e.radius * e.radius && (r < 0.0f || e.age < r)) {
			r = e.age;
		}
	};
	for (uint32_t i : wide) {
		check(i);
	}
	if (const LocalVector<uint32_t> *in = grid.getptr(Vector2i(int(Math::floor(p.x / GRID_CELL)), int(Math::floor(p.y / GRID_CELL))))) {
		for (uint32_t i : *in) {
			check(i);
		}
	}
	return r;
}

void WorldStand::set_trails(const Array &p_trails) {
	const Rect2 old = trail_bounds;
	const bool had = !trail_segments.is_empty();
	trails = p_trails.duplicate(true);
	trail_segments.clear();
	trail_grid.clear();
	trail_bounds = Rect2();
	bool any = false;
	for (int t = 0; t < trails.size(); t++) {
		const Dictionary d = trails[t];
		LocalVector<Vector2> pts;
		const Variant pv = d.get("points", Variant());
		if (pv.get_type() == Variant::PACKED_VECTOR3_ARRAY) {
			for (const Vector3 &q : PackedVector3Array(pv)) {
				pts.push_back(Vector2(q.x, q.z));
			}
		} else if (pv.get_type() == Variant::PACKED_VECTOR2_ARRAY) {
			for (const Vector2 &q : PackedVector2Array(pv)) {
				pts.push_back(q);
			}
		}
		const float hw = 0.5f * MAX(float(d.get("width", 0.6f)), 0.05f);
		const float passes = MAX(float(d.has("passes") ? d["passes"] : d.get("passes_per_year", 0.0f)), 0.0f);
		for (uint32_t i = 0; i + 1 < pts.size(); i++) {
			TrailSegment sg;
			sg.a = pts[i];
			sg.b = pts[i + 1];
			sg.half_width = hw;
			sg.passes = passes;
			const uint32_t id = trail_segments.size();
			trail_segments.push_back(sg);
			const Rect2 r = Rect2(sg.a, Vector2()).expand(sg.b).grow(hw);
			trail_bounds = any ? trail_bounds.merge(r) : r;
			any = true;
			const Vector2i c0(int(Math::floor(r.position.x / GRID_CELL)), int(Math::floor(r.position.y / GRID_CELL)));
			const Vector2i c1(int(Math::floor(r.get_end().x / GRID_CELL)), int(Math::floor(r.get_end().y / GRID_CELL)));
			for (int z = c0.y; z <= c1.y; z++) {
				for (int x = c0.x; x <= c1.x; x++) {
					trail_grid[Vector2i(x, z)].push_back(id);
				}
			}
		}
	}
	if (had || any) {
		_report(had && any ? old.merge(trail_bounds) : (had ? old : trail_bounds));
	}
}

Vector2 WorldStand::trail_at(const Vector3 &p_position) const {
	Vector2 r;
	if (GDVIRTUAL_CALL(_trail_at, p_position, r)) {
		return r;
	}
	const Vector2 p(p_position.x, p_position.z);
	const LocalVector<uint32_t> *in = trail_grid.getptr(Vector2i(int(Math::floor(p.x / GRID_CELL)), int(Math::floor(p.y / GRID_CELL))));
	if (in == nullptr) {
		return r;
	}
	for (uint32_t i : *in) {
		const TrailSegment &sg = trail_segments[i];
		const Vector2 q = Geometry2D::get_closest_point_to_segment(p, sg.a, sg.b);
		const float d = p.distance_to(q);
		if (d > sg.half_width) {
			continue;
		}
		// The tread: worn flat over the middle half, easing out to the edge.
		const float tread = 1.0f - Math::smoothstep(0.5f * sg.half_width, sg.half_width, d);
		if (sg.passes > r.x || (sg.passes == r.x && tread > r.y)) {
			r = Vector2(sg.passes, tread);
		}
	}
	return r;
}

float WorldStand::browse_at(const Vector3 &p_position) const {
	float r = 0.0f;
	if (GDVIRTUAL_CALL(_browse_at, p_position, r)) {
		return r;
	}
	float reach;
	_browse_query(p_position, r, reach);
	return r;
}

float WorldStand::browse_reach_at(const Vector3 &p_position) const {
	float r = 0.0f;
	if (GDVIRTUAL_CALL(_browse_reach_at, p_position, r)) {
		return r;
	}
	float pressure;
	_browse_query(p_position, pressure, r);
	return r;
}

void WorldStand::_rebuild_strike_grid() {
	strike_grid.clear();
	for (uint32_t i = 0; i < strikes.size(); i++) {
		const Vector2 c = strikes[i].position;
		const Vector2i a(int(Math::floor((c.x - STRIKE_RADIUS) / STRIKE_CELL)), int(Math::floor((c.y - STRIKE_RADIUS) / STRIKE_CELL)));
		const Vector2i b(int(Math::floor((c.x + STRIKE_RADIUS) / STRIKE_CELL)), int(Math::floor((c.y + STRIKE_RADIUS) / STRIKE_CELL)));
		for (int z = a.y; z <= b.y; z++) {
			for (int x = a.x; x <= b.x; x++) {
				strike_grid[Vector2i(x, z)].push_back(i);
			}
		}
	}
}

void WorldStand::add_strike(const Vector3 &p_position, float p_age_years) {
	if (strikes.size() >= uint32_t(MAX_STRIKES)) {
		uint32_t oldest = 0;
		for (uint32_t i = 1; i < strikes.size(); i++) {
			if (strikes[i].age > strikes[oldest].age) {
				oldest = i;
			}
		}
		strikes.remove_at_unordered(oldest);
	}
	Strike s;
	s.position = Vector2(p_position.x, p_position.z);
	s.age = MAX(p_age_years, 0.0f);
	strikes.push_back(s);
	_rebuild_strike_grid();
	_report(Rect2(s.position - Vector2(STRIKE_RADIUS, STRIKE_RADIUS), Vector2(2.0f * STRIKE_RADIUS, 2.0f * STRIKE_RADIUS)));
}

float WorldStand::strike_age_at(const Vector3 &p_position) const {
	float r = -1.0f;
	if (GDVIRTUAL_CALL(_strike_age_at, p_position, r)) {
		return r;
	}
	const Vector2 p(p_position.x, p_position.z);
	const LocalVector<uint32_t> *in = strike_grid.getptr(Vector2i(int(Math::floor(p.x / STRIKE_CELL)), int(Math::floor(p.y / STRIKE_CELL))));
	if (in == nullptr) {
		return r;
	}
	for (uint32_t i : *in) {
		const Strike &s = strikes[i];
		if (p.distance_squared_to(s.position) <= STRIKE_RADIUS * STRIKE_RADIUS && (r < 0.0f || s.age < r)) {
			r = s.age;
		}
	}
	return r;
}

void WorldStand::get_strikes_in(const Rect2 &p_rect, LocalVector<Vector3> &r_out) const {
	r_out.clear();
	const Rect2 r = p_rect.grow(STRIKE_RADIUS);
	for (const Strike &s : strikes) {
		if (r.has_point(s.position)) {
			r_out.push_back(Vector3(s.position.x, s.position.y, s.age));
		}
	}
}

void WorldStand::clear_strikes() {
	if (strikes.is_empty()) {
		return;
	}
	Rect2 area;
	for (uint32_t i = 0; i < strikes.size(); i++) {
		const Rect2 r(strikes[i].position - Vector2(STRIKE_RADIUS, STRIKE_RADIUS), Vector2(2.0f * STRIKE_RADIUS, 2.0f * STRIKE_RADIUS));
		area = i == 0 ? r : area.merge(r);
	}
	strikes.clear();
	strike_grid.clear();
	_report(area);
}

void WorldStand::advance_years(float p_years) {
	if (!(p_years > 0.0f) || (events.is_empty() && strikes.is_empty())) {
		return;
	}
	for (Strike &st : strikes) {
		st.age += p_years;
	}
	if (events.is_empty()) {
		Rect2 sa;
		for (uint32_t i = 0; i < strikes.size(); i++) {
			const Rect2 r(strikes[i].position - Vector2(STRIKE_RADIUS, STRIKE_RADIUS), Vector2(2.0f * STRIKE_RADIUS, 2.0f * STRIKE_RADIUS));
			sa = i == 0 ? r : sa.merge(r);
		}
		_report(sa);
		return;
	}
	Rect2 area;
	for (uint32_t i = 0; i < events.size(); i++) {
		events[i].age += p_years;
		const Rect2 r(events[i].center - Vector2(events[i].radius, events[i].radius), Vector2(2.0f * events[i].radius, 2.0f * events[i].radius));
		area = i == 0 ? r : area.merge(r);
	}
	for (const Strike &st : strikes) {
		area = area.merge(Rect2(st.position - Vector2(STRIKE_RADIUS, STRIKE_RADIUS), Vector2(2.0f * STRIKE_RADIUS, 2.0f * STRIKE_RADIUS)));
	}
	_report(area);
}

void WorldStand::notify_stand_changed(const Rect2 &p_area) {
	generation++;
	Change c;
	c.generation = generation;
	c.area = p_area;
	if (change_log.size() >= uint32_t(CHANGE_LOG)) {
		change_log.remove_at(0);
	}
	change_log.push_back(c);
	emit_signal(SNAME("stand_changed"), p_area);
}

bool WorldStand::stand_changes_since(uint64_t p_since, Rect2 &r_area) const {
	r_area = Rect2();
	if (p_since >= generation) {
		return true;
	}
	if (change_log.is_empty() || change_log[0].generation > p_since + 1) {
		return false;
	}
	bool any = false;
	for (const Change &c : change_log) {
		if (c.generation > p_since) {
			r_area = any ? r_area.merge(c.area) : c.area;
			any = true;
		}
	}
	return true;
}

Dictionary WorldStand::get_stand_changes_since(uint64_t p_since) const {
	Rect2 area;
	Dictionary d;
	d["complete"] = stand_changes_since(p_since, area);
	d["area"] = area;
	return d;
}

void WorldStand::_bind_methods() {
	ClassDB::bind_method(D_METHOD("stand_age_at", "position"), &WorldStand::stand_age_at);
	ClassDB::bind_method(D_METHOD("disturbance_at", "position"), &WorldStand::disturbance_at);
	ClassDB::bind_method(D_METHOD("add_disturbance", "center", "radius", "kind", "age_years"), &WorldStand::add_disturbance);
	ClassDB::bind_method(D_METHOD("add_disturbances", "centers", "radius", "kind", "age_years"), &WorldStand::add_disturbances);
	ClassDB::bind_method(D_METHOD("set_disturbance_age", "id", "age_years"), &WorldStand::set_disturbance_age);
	ClassDB::bind_method(D_METHOD("remove_disturbance", "id"), &WorldStand::remove_disturbance);
	ClassDB::bind_method(D_METHOD("clear_disturbances"), &WorldStand::clear_disturbances);
	ClassDB::bind_method(D_METHOD("advance_years", "years"), &WorldStand::advance_years);
	ClassDB::bind_method(D_METHOD("add_strike", "position", "age_years"), &WorldStand::add_strike);
	ClassDB::bind_method(D_METHOD("strike_age_at", "position"), &WorldStand::strike_age_at);
	ClassDB::bind_method(D_METHOD("get_strike_count"), &WorldStand::get_strike_count);
	ClassDB::bind_method(D_METHOD("clear_strikes"), &WorldStand::clear_strikes);
	ClassDB::bind_method(D_METHOD("add_browse", "center", "radius", "pressure", "reach_m"), &WorldStand::add_browse, DEFVAL(1.8f));
	ClassDB::bind_method(D_METHOD("set_browse", "id", "pressure"), &WorldStand::set_browse);
	ClassDB::bind_method(D_METHOD("remove_browse", "id"), &WorldStand::remove_browse);
	ClassDB::bind_method(D_METHOD("clear_browse"), &WorldStand::clear_browse);
	ClassDB::bind_method(D_METHOD("browse_at", "position"), &WorldStand::browse_at);
	ClassDB::bind_method(D_METHOD("browse_reach_at", "position"), &WorldStand::browse_reach_at);
	ClassDB::bind_method(D_METHOD("rooting_age_at", "position"), &WorldStand::rooting_age_at);
	ClassDB::bind_method(D_METHOD("set_trails", "trails"), &WorldStand::set_trails);
	ClassDB::bind_method(D_METHOD("get_trails"), &WorldStand::get_trails);
	ClassDB::bind_method(D_METHOD("trail_at", "position"), &WorldStand::trail_at);
	ClassDB::bind_method(D_METHOD("notify_stand_changed", "area"), &WorldStand::notify_stand_changed);
	ClassDB::bind_method(D_METHOD("get_stand_generation"), &WorldStand::get_stand_generation);
	ClassDB::bind_method(D_METHOD("get_stand_changes_since", "generation"), &WorldStand::get_stand_changes_since);
	ADD_SIGNAL(MethodInfo("stand_changed", PropertyInfo(Variant::RECT2, "area")));
	GDVIRTUAL_BIND(_stand_age_at, "position");
	GDVIRTUAL_BIND(_disturbance_at, "position");
	GDVIRTUAL_BIND(_strike_age_at, "position");
	GDVIRTUAL_BIND(_browse_at, "position");
	GDVIRTUAL_BIND(_browse_reach_at, "position");
	GDVIRTUAL_BIND(_rooting_age_at, "position");
	GDVIRTUAL_BIND(_trail_at, "position");
	BIND_ENUM_CONSTANT(DISTURBANCE_NONE);
	BIND_ENUM_CONSTANT(DISTURBANCE_FIRE);
	BIND_ENUM_CONSTANT(DISTURBANCE_WIND);
	BIND_ENUM_CONSTANT(DISTURBANCE_BEETLE);
	BIND_ENUM_CONSTANT(DISTURBANCE_BEAVER);
	BIND_ENUM_CONSTANT(DISTURBANCE_ROOTING);
	BIND_ENUM_CONSTANT(DISTURBANCE_AVALANCHE);
	BIND_ENUM_CONSTANT(DISTURBANCE_ICE_STORM);
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
	caves_override.unref();
	snow_override.unref();
	stand_override.unref();
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
	b.walker = p_node->has_meta(SNAME("world_contact_walker")) ? bool(p_node->get_meta(SNAME("world_contact_walker"))) : Object::cast_to<CharacterBody3D>(p_node) != nullptr;
	b.feet = b.walker && bool(p_node->get_meta(SNAME("world_contact_feet"), false));
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

namespace {
constexpr int CONTACT_SPHERES_MAX = 512;

struct ContactSphereSet {
	LocalVector<Vector4> *spheres;
	LocalVector<Vector3> *velocities;
	Vector3 lin; // the body's linear velocity
	Vector3 ang; // its spin, rad/s
	Vector3 origin; // what it spins about
	void add(const Vector3 &p_c, float p_r) {
		if ((int)spheres->size() >= CONTACT_SPHERES_MAX || p_r <= 0.0f) {
			return;
		}
		spheres->push_back(Vector4(p_c.x, p_c.y, p_c.z, p_r));
		velocities->push_back(lin + ang.cross(p_c - origin));
	}
	// A chain of spheres of radius p_r from p_a to p_b, about p_r apart.
	void chain(const Vector3 &p_a, const Vector3 &p_b, float p_r) {
		const float len = p_a.distance_to(p_b);
		const int n = MAX(1, int(Math::ceil(len / MAX(p_r, 0.01f)))) + 1;
		for (int i = 0; i < n; i++) {
			add(n > 1 ? p_a.lerp(p_b, float(i) / float(n - 1)) : p_a, p_r);
		}
	}
	// A box of half extents p_h on p_xf: spheres of half its thinnest side over a grid of the other two.
	void box(const Transform3D &p_xf, const Vector3 &p_hv) {
		const float h[3] = { float(p_hv.x), float(p_hv.y), float(p_hv.z) };
		const float r = MAX(MIN(h[0], MIN(h[1], h[2])), 0.02f);
		int n[3];
		for (int k = 0; k < 3; k++) {
			n[k] = h[k] - r <= 1e-3f ? 1 : MIN(int(Math::ceil(2.0f * (h[k] - r) / r)) + 1, 12);
		}
		auto at = [&](int p_k, int p_i) { return n[p_k] > 1 ? Math::lerp(-(h[p_k] - r), h[p_k] - r, float(p_i) / float(n[p_k] - 1)) : 0.0f; };
		for (int i = 0; i < n[0]; i++) {
			for (int j = 0; j < n[1]; j++) {
				for (int k = 0; k < n[2]; k++) {
					add(p_xf.xform(Vector3(at(0, i), at(1, j), at(2, k))), r);
				}
			}
		}
	}
	// A disc (a tyre) on its axis p_axis through p_c: a ring of tread-wide spheres round the rim.
	void wheel(const Vector3 &p_c, const Vector3 &p_axis, float p_radius, float p_width) {
		const float r = MAX(MIN(p_width * 0.5f, p_radius), 0.02f);
		const Vector3 ax = p_axis.normalized();
		Vector3 u = ax.cross(Vector3(0, 1, 0));
		if (u.length_squared() < 1e-6f) {
			u = ax.cross(Vector3(1, 0, 0));
		}
		u.normalize();
		const Vector3 v = ax.cross(u);
		const float ring = MAX(p_radius - r, 0.0f);
		const int n = ring <= 1e-3f ? 1 : CLAMP(int(Math::ceil(Math::TAU * ring / r)), 6, 32);
		for (int i = 0; i < n; i++) {
			const float a = Math::TAU * float(i) / float(n);
			add(p_c + (u * Math::cos(a) + v * Math::sin(a)) * ring, r);
		}
	}
	void shape(const Transform3D &p_xf, const Ref<Shape3D> &p_shape) {
		const Vector3 sc = p_xf.basis.get_scale();
		const Vector3 up = p_xf.basis.get_column(1).normalized();
		if (const SphereShape3D *sp = Object::cast_to<SphereShape3D>(p_shape.ptr())) {
			add(p_xf.origin, sp->get_radius() * MAX(sc.x, MAX(sc.y, sc.z)));
		} else if (const CapsuleShape3D *cp = Object::cast_to<CapsuleShape3D>(p_shape.ptr())) {
			const float r = cp->get_radius() * MAX(sc.x, sc.z);
			const float a = MAX(cp->get_height() * 0.5f * sc.y - r, 0.0f);
			chain(p_xf.origin - up * a, p_xf.origin + up * a, r);
		} else if (const CylinderShape3D *cy = Object::cast_to<CylinderShape3D>(p_shape.ptr())) {
			const float r = cy->get_radius() * MAX(sc.x, sc.z);
			const float h = cy->get_height() * 0.5f * sc.y;
			if (h < r) {
				wheel(p_xf.origin, up, r, 2.0f * h); // A disc: a wheel, lying or standing.
			} else {
				chain(p_xf.origin - up * (h - r), p_xf.origin + up * (h - r), r);
			}
		} else if (const BoxShape3D *bx = Object::cast_to<BoxShape3D>(p_shape.ptr())) {
			box(Transform3D(p_xf.basis.orthonormalized(), p_xf.origin), bx->get_size() * 0.5f * sc);
		} else {
			AABB bb;
			bool any = false;
			if (const ConvexPolygonShape3D *cv = Object::cast_to<ConvexPolygonShape3D>(p_shape.ptr())) {
				for (const Vector3 &q : cv->get_points()) {
					bb = any ? bb.expand(q) : AABB(q, Vector3());
					any = true;
				}
			} else if (const ConcavePolygonShape3D *cc = Object::cast_to<ConcavePolygonShape3D>(p_shape.ptr())) {
				for (const Vector3 &q : cc->get_faces()) {
					bb = any ? bb.expand(q) : AABB(q, Vector3());
					any = true;
				}
			}
			if (any) {
				box(Transform3D(p_xf.basis.orthonormalized(), p_xf.xform(bb.get_center())), bb.size * 0.5f * sc);
			}
		}
	}
	void shapes_of(const Node *p_node) {
		for (int i = 0; i < p_node->get_child_count(); i++) {
			const CollisionShape3D *cs = Object::cast_to<CollisionShape3D>(p_node->get_child(i));
			if (cs && !cs->is_disabled() && cs->get_shape().is_valid()) {
				shape(cs->get_global_transform(), cs->get_shape());
			}
		}
	}
	// A ragdoll's bones: every PhysicalBone3D under the node that simulates (a few levels down: skeleton, simulator).
	void bones_under(Node *p_node, int p_depth) {
		for (int i = 0; i < p_node->get_child_count() && p_depth > 0; i++) {
			Node *c = p_node->get_child(i);
			if (PhysicalBone3D *pb = Object::cast_to<PhysicalBone3D>(c)) {
				if (pb->is_simulating_physics()) {
					const Vector3 l0 = lin, a0 = ang, o0 = origin;
					lin = pb->get_linear_velocity();
					ang = pb->get_angular_velocity();
					origin = pb->get_global_position();
					shapes_of(pb);
					lin = l0;
					ang = a0;
					origin = o0;
				}
				continue;
			}
			bones_under(c, p_depth - 1);
		}
	}
};
} // namespace

void WorldContracts::contact_spheres(const Node3D *p_node, LocalVector<Vector4> &r_spheres, LocalVector<Vector3> &r_velocities, const Vector3 &p_fallback_velocity) {
	r_spheres.clear();
	r_velocities.clear();
	if (!p_node) {
		return;
	}
	ContactSphereSet set{ &r_spheres, &r_velocities, p_fallback_velocity, Vector3(), p_node->get_global_position() };
	if (p_node->has_meta(SNAME("trample_radius"))) {
		set.add(p_node->get_global_position(), p_node->get_meta(SNAME("trample_radius")));
		return;
	}
	if (const RigidBody3D *rb = Object::cast_to<RigidBody3D>(p_node)) {
		set.lin = rb->get_linear_velocity();
		set.ang = rb->get_angular_velocity();
	} else if (const CharacterBody3D *cb = Object::cast_to<CharacterBody3D>(p_node)) {
		set.lin = cb->get_velocity();
	}
	set.shapes_of(p_node);
	Node *n = const_cast<Node3D *>(p_node);
	for (int i = 0; i < n->get_child_count(); i++) {
		if (const VehicleWheel3D *w = Object::cast_to<VehicleWheel3D>(n->get_child(i))) {
			const float r = w->get_radius();
			const float width = w->has_meta(SNAME("deform_wheel_width")) ? float(w->get_meta(SNAME("deform_wheel_width"))) : r * 0.6f;
			set.wheel(w->get_global_position(), w->get_global_basis().get_column(0), r, width);
		}
	}
	set.bones_under(n, 4);
	// The contact points the physics server reports for the body (contact monitoring on): where it actually touches.
	if (const CollisionObject3D *co = Object::cast_to<CollisionObject3D>(p_node)) {
		PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
		if (ps && Object::cast_to<PhysicsBody3D>(p_node)) {
			if (PhysicsDirectBodyState3D *st = ps->body_get_direct_state(co->get_rid())) {
				const int nc = MIN(st->get_contact_count(), 32);
				for (int i = 0; i < nc; i++) {
					const Vector3 cp = st->get_contact_local_position(i);
					set.add(cp + Vector3(0.0f, 0.08f, 0.0f), 0.08f);
				}
			}
		}
	}
	if (r_spheres.is_empty()) {
		const Vector2 e = body_extent(p_node, 0.4f);
		set.add(p_node->get_global_position(), e.x);
	}
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
	d["walker"] = b.walker;
	d["feet"] = b.feet;
	return d;
}

void WorldContracts::_bind_methods() {
	// A WorldExclusion3D came, went or changed over area (world x, z): placers lay what sits there again.
	ADD_SIGNAL(MethodInfo("exclusions_changed", PropertyInfo(Variant::RECT2, "area")));
	ClassDB::bind_static_method("WorldContracts", D_METHOD("climate_precipitation_factor_at", "position"), &WorldContracts::climate_precipitation_factor_at);
	ClassDB::bind_method(D_METHOD("set_weather", "stub"), &WorldContracts::set_weather);
	ClassDB::bind_method(D_METHOD("get_weather"), &WorldContracts::get_weather);
	ClassDB::bind_method(D_METHOD("set_ground", "stub"), &WorldContracts::set_ground);
	ClassDB::bind_method(D_METHOD("get_ground"), &WorldContracts::get_ground);
	ClassDB::bind_method(D_METHOD("set_water", "stub"), &WorldContracts::set_water);
	ClassDB::bind_method(D_METHOD("get_water"), &WorldContracts::get_water);
	ClassDB::bind_method(D_METHOD("add_water_stub", "stub"), &WorldContracts::add_water_stub);
	ClassDB::bind_method(D_METHOD("set_fire", "stub"), &WorldContracts::set_fire);
	ClassDB::bind_method(D_METHOD("get_fire"), &WorldContracts::get_fire);
	ClassDB::bind_method(D_METHOD("set_caves", "stub"), &WorldContracts::set_caves);
	ClassDB::bind_method(D_METHOD("get_caves"), &WorldContracts::get_caves);
	ClassDB::bind_method(D_METHOD("set_stand", "stub"), &WorldContracts::set_stand);
	ClassDB::bind_method(D_METHOD("get_stand"), &WorldContracts::get_stand);
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
	builtin_caves.instantiate();
	builtin_snow.instantiate();
	builtin_stand.instantiate();
}

WorldContracts::~WorldContracts() {
	_clear_water_stubs();
	if (singleton == this) {
		singleton = nullptr;
	}
}

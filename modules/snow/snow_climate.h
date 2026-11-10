/**************************************************************************/
/*  snow_climate.h                                                        */
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

#include "core/io/resource.h"
#include "core/math/random_pcg.h"

// A place's snow climate (docs/ideas/snow.md, stage 4): the recipe of a winter's weather and, once
// SnowCover3D.bake_climate() has run that winter over the terrain, the snowpack it left. The weather is a
// daily stochastic generator in the manner of WGEN (Richardson 1981): wet and dry days from a two-state
// Markov chain, wet-day amounts exponential, the day's mean temperature a seasonal harmonic plus a
// persistent residual, a daily cycle on top, the day's wind Weibull around a prevailing direction, stormier
// on wet days. The sun follows the latitude and the date.
class SnowClimate : public Resource {
	GDCLASS(SnowClimate, Resource);

public:
	struct Hour {
		float temperature = 0.0f; // C at base_height
		float humidity = 0.8f;
		float precipitation = 0.0f; // mm/h water
		float cloud_cover = 0.0f;
		Vector2 wind; // m/s at 10 m, the direction it blows to
		Vector3 to_sun; // zero at night
		float direct_normal = 0.0f; // W/m2 at the ground, clouds included
	};

private:
	// The recipe.
	float latitude = 46.0f; // degrees, north positive
	float base_height = 0.0f; // m, where the temperatures below hold
	float january_temperature = -1.0f; // C, mean of the coldest month
	float july_temperature = 17.0f; // C, mean of the warmest month
	float daily_range = 8.0f; // C, warmest to coldest hour on a dry day
	float temperature_persistence = 0.7f; // lag-one correlation of the daily residual
	float temperature_variability = 3.0f; // C, standard deviation of the daily residual
	float wet_after_dry = 0.3f; // chance a day is wet after a dry one
	float wet_after_wet = 0.65f; // after a wet one
	float wet_day_precipitation = 9.0f; // mm, mean water on a wet day
	float precipitation_gradient = 0.06f; // share more per 100 m above base_height
	float prevailing_wind_from = 270.0f; // degrees clockwise from north the wind comes from (270: westerly)
	float wind_spread = 30.0f; // degrees, standard deviation of the day's direction
	float wind_speed = 5.0f; // m/s at 10 m, mean daily wind on dry days
	float storm_wind_factor = 1.8f; // wet days' mean wind over dry days'
	int start_day = 274; // day of the year the bake starts (1 October)
	int days = 182; // to about 1 April
	int step_hours = 3;
	int seed = 1;

	// The result.
	Rect2 rect;
	int resolution = 0;
	PackedFloat32Array swe;
	PackedFloat32Array liquid;
	PackedFloat32Array density;
	PackedFloat32Array albedo;
	PackedFloat32Array cornice_x;
	PackedFloat32Array cornice_z;

	// The generator's state.
	RandomPCG rng;
	bool wet = false;
	float residual = 0.0f;
	float day_mean = 0.0f;
	float day_range = 0.0f;
	float day_precip = 0.0f;
	Vector2 day_wind;
	int gen_day = -1;

	float _normal();

protected:
	static void _bind_methods();

public:
	// The generator: start, then one hour at a time (the day's draw is made on its first hour).
	void start_weather();
	Hour weather_at(int p_day, float p_hour);
	// Seasonal mean temperature at base_height on a day of the year.
	float seasonal_temperature(float p_day_of_year) const;
	// The sun's direction (x east, y up, z south) and elevation for the latitude.
	Vector3 sun_direction(float p_day_of_year, float p_hour) const;
	// Daily weather of a whole bake for checks: per day mean temperature, water, wind speed, wet (0/1).
	PackedFloat32Array sample_days(int p_days);

	void set_result(Rect2 p_rect, int p_resolution, const PackedFloat32Array &p_swe, const PackedFloat32Array &p_liquid, const PackedFloat32Array &p_density,
			const PackedFloat32Array &p_albedo, const PackedFloat32Array &p_cornice_x, const PackedFloat32Array &p_cornice_z);
	bool is_baked() const { return resolution > 0 && swe.size() == resolution * resolution; }
	void clear_result();

#define SNOW_CLIMATE_PROP(m_type, m_name)               \
	void set_##m_name(m_type p_value) { m_name = p_value; } \
	m_type get_##m_name() const { return m_name; }
	SNOW_CLIMATE_PROP(float, latitude)
	SNOW_CLIMATE_PROP(float, base_height)
	SNOW_CLIMATE_PROP(float, january_temperature)
	SNOW_CLIMATE_PROP(float, july_temperature)
	SNOW_CLIMATE_PROP(float, daily_range)
	SNOW_CLIMATE_PROP(float, temperature_persistence)
	SNOW_CLIMATE_PROP(float, temperature_variability)
	SNOW_CLIMATE_PROP(float, wet_after_dry)
	SNOW_CLIMATE_PROP(float, wet_after_wet)
	SNOW_CLIMATE_PROP(float, wet_day_precipitation)
	SNOW_CLIMATE_PROP(float, precipitation_gradient)
	SNOW_CLIMATE_PROP(float, prevailing_wind_from)
	SNOW_CLIMATE_PROP(float, wind_spread)
	SNOW_CLIMATE_PROP(float, wind_speed)
	SNOW_CLIMATE_PROP(float, storm_wind_factor)
	SNOW_CLIMATE_PROP(int, start_day)
	SNOW_CLIMATE_PROP(int, days)
	SNOW_CLIMATE_PROP(int, step_hours)
	SNOW_CLIMATE_PROP(int, seed)
	SNOW_CLIMATE_PROP(Rect2, rect)
	SNOW_CLIMATE_PROP(int, resolution)
	SNOW_CLIMATE_PROP(PackedFloat32Array, swe)
	SNOW_CLIMATE_PROP(PackedFloat32Array, liquid)
	SNOW_CLIMATE_PROP(PackedFloat32Array, density)
	SNOW_CLIMATE_PROP(PackedFloat32Array, albedo)
	SNOW_CLIMATE_PROP(PackedFloat32Array, cornice_x)
	SNOW_CLIMATE_PROP(PackedFloat32Array, cornice_z)
#undef SNOW_CLIMATE_PROP
};

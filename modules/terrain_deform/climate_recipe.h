/**************************************************************************/
/*  climate_recipe.h                                                      */
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

#include "terrain_climate.h"

#include "core/io/resource.h"

// Settings for TerrainDataMap3D.bake_climate() (terrain_climate.h; docs/ideas/terrain-generation.md).
class ClimateRecipe : public Resource {
	GDCLASS(ClimateRecipe, Resource);

	TerrainClimate::Params params;
	float sun_cell = 16.0f;
	float precipitation_cell = 64.0f;
	float context_margin = 0.0f;

protected:
	static void _bind_methods();

public:
	const TerrainClimate::Params &get_params() const { return params; }

	void set_latitude(float p_value) {
		params.latitude = p_value;
		emit_changed();
	}
	float get_latitude() const { return params.latitude; }
	void set_reference_height(float p_value) {
		params.reference_height = p_value;
		emit_changed();
	}
	float get_reference_height() const { return params.reference_height; }
	void set_temperature_january(float p_value) {
		params.temperature_january = p_value;
		emit_changed();
	}
	float get_temperature_january() const { return params.temperature_january; }
	void set_temperature_july(float p_value) {
		params.temperature_july = p_value;
		emit_changed();
	}
	float get_temperature_july() const { return params.temperature_july; }
	void set_lapse_rate_january(float p_value) {
		params.lapse_rate_january = p_value;
		emit_changed();
	}
	float get_lapse_rate_january() const { return params.lapse_rate_january; }
	void set_lapse_rate_july(float p_value) {
		params.lapse_rate_july = p_value;
		emit_changed();
	}
	float get_lapse_rate_july() const { return params.lapse_rate_july; }
	void set_precipitation(float p_value) {
		params.precipitation = p_value;
		emit_changed();
	}
	float get_precipitation() const { return params.precipitation; }
	void set_wet_fraction(float p_value) {
		params.wet_fraction = p_value;
		emit_changed();
	}
	float get_wet_fraction() const { return params.wet_fraction; }
	void set_wind_speed(float p_value) {
		params.wind_speed = p_value;
		emit_changed();
	}
	float get_wind_speed() const { return params.wind_speed; }
	void set_wind_from(float p_value) {
		params.wind_from = p_value;
		emit_changed();
	}
	float get_wind_from() const { return params.wind_from; }
	void set_wind_spread(float p_value) {
		params.wind_spread = p_value;
		emit_changed();
	}
	float get_wind_spread() const { return params.wind_spread; }
	void set_moist_stability(float p_value) {
		params.moist_stability = p_value;
		emit_changed();
	}
	float get_moist_stability() const { return params.moist_stability; }
	void set_conversion_time(float p_value) {
		params.conversion_time = p_value;
		emit_changed();
	}
	float get_conversion_time() const { return params.conversion_time; }
	void set_fallout_time(float p_value) {
		params.fallout_time = p_value;
		emit_changed();
	}
	float get_fallout_time() const { return params.fallout_time; }
	void set_precipitation_floor(float p_value) {
		params.precipitation_floor = p_value;
		emit_changed();
	}
	float get_precipitation_floor() const { return params.precipitation_floor; }
	void set_rain_snow_threshold(float p_value) {
		params.rain_snow_threshold = p_value;
		emit_changed();
	}
	float get_rain_snow_threshold() const { return params.rain_snow_threshold; }
	void set_daily_temperature_spread(float p_value) {
		params.daily_temperature_spread = p_value;
		emit_changed();
	}
	float get_daily_temperature_spread() const { return params.daily_temperature_spread; }
	void set_cloud_cover(float p_value) {
		params.cloud_cover = p_value;
		emit_changed();
	}
	float get_cloud_cover() const { return params.cloud_cover; }
	void set_ground_albedo(float p_value) {
		params.ground_albedo = p_value;
		emit_changed();
	}
	float get_ground_albedo() const { return params.ground_albedo; }
	void set_horizon_distance(float p_value) {
		params.horizon_distance = p_value;
		emit_changed();
	}
	float get_horizon_distance() const { return params.horizon_distance; }
	void set_sx_distance(float p_value) {
		params.sx_distance = p_value;
		emit_changed();
	}
	float get_sx_distance() const { return params.sx_distance; }
	void set_precipitation_seasonality(float p_value) {
		params.precipitation_seasonality = p_value;
		emit_changed();
	}
	float get_precipitation_seasonality() const { return params.precipitation_seasonality; }
	void set_precipitation_peak_month(float p_value) {
		params.precipitation_peak_month = p_value;
		emit_changed();
	}
	float get_precipitation_peak_month() const { return params.precipitation_peak_month; }
	void set_melt_factor(float p_value) {
		params.melt_factor = p_value;
		emit_changed();
	}
	float get_melt_factor() const { return params.melt_factor; }
	void set_radiation_melt_factor(float p_value) {
		params.radiation_melt_factor = p_value;
		emit_changed();
	}
	float get_radiation_melt_factor() const { return params.radiation_melt_factor; }
	void set_soil_capacity(float p_value) {
		params.soil_capacity = p_value;
		emit_changed();
	}
	float get_soil_capacity() const { return params.soil_capacity; }
	void set_direct_runoff(float p_value) {
		params.direct_runoff = p_value;
		emit_changed();
	}
	float get_direct_runoff() const { return params.direct_runoff; }
	void set_groundwater_release(float p_value) {
		params.groundwater_release = p_value;
		emit_changed();
	}
	float get_groundwater_release() const { return params.groundwater_release; }
	void set_sun_cell(float p_value) {
		sun_cell = p_value;
		emit_changed();
	}
	float get_sun_cell() const { return sun_cell; }
	void set_precipitation_cell(float p_value) {
		precipitation_cell = p_value;
		emit_changed();
	}
	float get_precipitation_cell() const { return precipitation_cell; }
	void set_context_margin(float p_value) {
		context_margin = p_value;
		emit_changed();
	}
	float get_context_margin() const { return context_margin; }
};

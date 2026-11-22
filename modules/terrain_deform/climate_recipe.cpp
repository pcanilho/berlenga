/**************************************************************************/
/*  climate_recipe.cpp                                                    */
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

#include "climate_recipe.h"

#include "core/object/class_db.h"

void ClimateRecipe::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_latitude", "value"), &ClimateRecipe::set_latitude);
	ClassDB::bind_method(D_METHOD("get_latitude"), &ClimateRecipe::get_latitude);
	ClassDB::bind_method(D_METHOD("set_reference_height", "value"), &ClimateRecipe::set_reference_height);
	ClassDB::bind_method(D_METHOD("get_reference_height"), &ClimateRecipe::get_reference_height);
	ClassDB::bind_method(D_METHOD("set_temperature_january", "value"), &ClimateRecipe::set_temperature_january);
	ClassDB::bind_method(D_METHOD("get_temperature_january"), &ClimateRecipe::get_temperature_january);
	ClassDB::bind_method(D_METHOD("set_temperature_july", "value"), &ClimateRecipe::set_temperature_july);
	ClassDB::bind_method(D_METHOD("get_temperature_july"), &ClimateRecipe::get_temperature_july);
	ClassDB::bind_method(D_METHOD("set_lapse_rate_january", "value"), &ClimateRecipe::set_lapse_rate_january);
	ClassDB::bind_method(D_METHOD("get_lapse_rate_january"), &ClimateRecipe::get_lapse_rate_january);
	ClassDB::bind_method(D_METHOD("set_lapse_rate_july", "value"), &ClimateRecipe::set_lapse_rate_july);
	ClassDB::bind_method(D_METHOD("get_lapse_rate_july"), &ClimateRecipe::get_lapse_rate_july);
	ClassDB::bind_method(D_METHOD("set_precipitation", "value"), &ClimateRecipe::set_precipitation);
	ClassDB::bind_method(D_METHOD("get_precipitation"), &ClimateRecipe::get_precipitation);
	ClassDB::bind_method(D_METHOD("set_wet_fraction", "value"), &ClimateRecipe::set_wet_fraction);
	ClassDB::bind_method(D_METHOD("get_wet_fraction"), &ClimateRecipe::get_wet_fraction);
	ClassDB::bind_method(D_METHOD("set_wind_speed", "value"), &ClimateRecipe::set_wind_speed);
	ClassDB::bind_method(D_METHOD("get_wind_speed"), &ClimateRecipe::get_wind_speed);
	ClassDB::bind_method(D_METHOD("set_wind_from", "value"), &ClimateRecipe::set_wind_from);
	ClassDB::bind_method(D_METHOD("get_wind_from"), &ClimateRecipe::get_wind_from);
	ClassDB::bind_method(D_METHOD("set_wind_spread", "value"), &ClimateRecipe::set_wind_spread);
	ClassDB::bind_method(D_METHOD("get_wind_spread"), &ClimateRecipe::get_wind_spread);
	ClassDB::bind_method(D_METHOD("set_moist_stability", "value"), &ClimateRecipe::set_moist_stability);
	ClassDB::bind_method(D_METHOD("get_moist_stability"), &ClimateRecipe::get_moist_stability);
	ClassDB::bind_method(D_METHOD("set_conversion_time", "value"), &ClimateRecipe::set_conversion_time);
	ClassDB::bind_method(D_METHOD("get_conversion_time"), &ClimateRecipe::get_conversion_time);
	ClassDB::bind_method(D_METHOD("set_fallout_time", "value"), &ClimateRecipe::set_fallout_time);
	ClassDB::bind_method(D_METHOD("get_fallout_time"), &ClimateRecipe::get_fallout_time);
	ClassDB::bind_method(D_METHOD("set_precipitation_floor", "value"), &ClimateRecipe::set_precipitation_floor);
	ClassDB::bind_method(D_METHOD("get_precipitation_floor"), &ClimateRecipe::get_precipitation_floor);
	ClassDB::bind_method(D_METHOD("set_rain_snow_threshold", "value"), &ClimateRecipe::set_rain_snow_threshold);
	ClassDB::bind_method(D_METHOD("get_rain_snow_threshold"), &ClimateRecipe::get_rain_snow_threshold);
	ClassDB::bind_method(D_METHOD("set_daily_temperature_spread", "value"), &ClimateRecipe::set_daily_temperature_spread);
	ClassDB::bind_method(D_METHOD("get_daily_temperature_spread"), &ClimateRecipe::get_daily_temperature_spread);
	ClassDB::bind_method(D_METHOD("set_cloud_cover", "value"), &ClimateRecipe::set_cloud_cover);
	ClassDB::bind_method(D_METHOD("get_cloud_cover"), &ClimateRecipe::get_cloud_cover);
	ClassDB::bind_method(D_METHOD("set_ground_albedo", "value"), &ClimateRecipe::set_ground_albedo);
	ClassDB::bind_method(D_METHOD("get_ground_albedo"), &ClimateRecipe::get_ground_albedo);
	ClassDB::bind_method(D_METHOD("set_horizon_distance", "value"), &ClimateRecipe::set_horizon_distance);
	ClassDB::bind_method(D_METHOD("get_horizon_distance"), &ClimateRecipe::get_horizon_distance);
	ClassDB::bind_method(D_METHOD("set_sx_distance", "value"), &ClimateRecipe::set_sx_distance);
	ClassDB::bind_method(D_METHOD("get_sx_distance"), &ClimateRecipe::get_sx_distance);
	ClassDB::bind_method(D_METHOD("set_precipitation_seasonality", "value"), &ClimateRecipe::set_precipitation_seasonality);
	ClassDB::bind_method(D_METHOD("get_precipitation_seasonality"), &ClimateRecipe::get_precipitation_seasonality);
	ClassDB::bind_method(D_METHOD("set_precipitation_peak_month", "value"), &ClimateRecipe::set_precipitation_peak_month);
	ClassDB::bind_method(D_METHOD("get_precipitation_peak_month"), &ClimateRecipe::get_precipitation_peak_month);
	ClassDB::bind_method(D_METHOD("set_melt_factor", "value"), &ClimateRecipe::set_melt_factor);
	ClassDB::bind_method(D_METHOD("get_melt_factor"), &ClimateRecipe::get_melt_factor);
	ClassDB::bind_method(D_METHOD("set_radiation_melt_factor", "value"), &ClimateRecipe::set_radiation_melt_factor);
	ClassDB::bind_method(D_METHOD("get_radiation_melt_factor"), &ClimateRecipe::get_radiation_melt_factor);
	ClassDB::bind_method(D_METHOD("set_soil_capacity", "value"), &ClimateRecipe::set_soil_capacity);
	ClassDB::bind_method(D_METHOD("get_soil_capacity"), &ClimateRecipe::get_soil_capacity);
	ClassDB::bind_method(D_METHOD("set_direct_runoff", "value"), &ClimateRecipe::set_direct_runoff);
	ClassDB::bind_method(D_METHOD("get_direct_runoff"), &ClimateRecipe::get_direct_runoff);
	ClassDB::bind_method(D_METHOD("set_groundwater_release", "value"), &ClimateRecipe::set_groundwater_release);
	ClassDB::bind_method(D_METHOD("get_groundwater_release"), &ClimateRecipe::get_groundwater_release);
	ClassDB::bind_method(D_METHOD("set_sun_cell", "value"), &ClimateRecipe::set_sun_cell);
	ClassDB::bind_method(D_METHOD("get_sun_cell"), &ClimateRecipe::get_sun_cell);
	ClassDB::bind_method(D_METHOD("set_precipitation_cell", "value"), &ClimateRecipe::set_precipitation_cell);
	ClassDB::bind_method(D_METHOD("get_precipitation_cell"), &ClimateRecipe::get_precipitation_cell);
	ClassDB::bind_method(D_METHOD("set_context_margin", "value"), &ClimateRecipe::set_context_margin);
	ClassDB::bind_method(D_METHOD("get_context_margin"), &ClimateRecipe::get_context_margin);

	ADD_GROUP("Temperature", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "latitude", PROPERTY_HINT_RANGE, "-90,90,0.1,suffix:°"), "set_latitude", "get_latitude");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "reference_height", PROPERTY_HINT_RANGE, "-1000,9000,1,or_less,or_greater,suffix:m"), "set_reference_height", "get_reference_height");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "temperature_january", PROPERTY_HINT_RANGE, "-60,50,0.1,suffix:°C"), "set_temperature_january", "get_temperature_january");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "temperature_july", PROPERTY_HINT_RANGE, "-60,50,0.1,suffix:°C"), "set_temperature_july", "get_temperature_july");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lapse_rate_january", PROPERTY_HINT_RANGE, "0,12,0.1,suffix:K/km"), "set_lapse_rate_january", "get_lapse_rate_january");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lapse_rate_july", PROPERTY_HINT_RANGE, "0,12,0.1,suffix:K/km"), "set_lapse_rate_july", "get_lapse_rate_july");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "rain_snow_threshold", PROPERTY_HINT_RANGE, "-5,5,0.1,suffix:°C"), "set_rain_snow_threshold", "get_rain_snow_threshold");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "daily_temperature_spread", PROPERTY_HINT_RANGE, "0.1,10,0.1,suffix:K"), "set_daily_temperature_spread", "get_daily_temperature_spread");

	ADD_GROUP("Precipitation", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "precipitation", PROPERTY_HINT_RANGE, "0,10000,1,or_greater,suffix:mm"), "set_precipitation", "get_precipitation");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wet_fraction", PROPERTY_HINT_RANGE, "0.01,1,0.01"), "set_wet_fraction", "get_wet_fraction");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wind_speed", PROPERTY_HINT_RANGE, "0.5,50,0.1,suffix:m/s"), "set_wind_speed", "get_wind_speed");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wind_from", PROPERTY_HINT_RANGE, "0,360,0.1,suffix:°"), "set_wind_from", "get_wind_from");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wind_spread", PROPERTY_HINT_RANGE, "1,360,0.1,suffix:°"), "set_wind_spread", "get_wind_spread");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "moist_stability", PROPERTY_HINT_RANGE, "0,0.03,0.0001,suffix:1/s"), "set_moist_stability", "get_moist_stability");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "conversion_time", PROPERTY_HINT_RANGE, "10,10000,1,suffix:s"), "set_conversion_time", "get_conversion_time");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "fallout_time", PROPERTY_HINT_RANGE, "10,10000,1,suffix:s"), "set_fallout_time", "get_fallout_time");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "precipitation_floor", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_precipitation_floor", "get_precipitation_floor");

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "precipitation_seasonality", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_precipitation_seasonality", "get_precipitation_seasonality");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "precipitation_peak_month", PROPERTY_HINT_RANGE, "0,11,1"), "set_precipitation_peak_month", "get_precipitation_peak_month");

	ADD_GROUP("Sun", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cloud_cover", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_cloud_cover", "get_cloud_cover");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ground_albedo", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_ground_albedo", "get_ground_albedo");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "horizon_distance", PROPERTY_HINT_RANGE, "0,50000,1,or_greater,suffix:m"), "set_horizon_distance", "get_horizon_distance");

	ADD_GROUP("Wind exposure", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "sx_distance", PROPERTY_HINT_RANGE, "1,1000,1,suffix:m"), "set_sx_distance", "get_sx_distance");

	ADD_GROUP("Runoff", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "melt_factor", PROPERTY_HINT_RANGE, "0,10,0.01,suffix:mm/d/K"), "set_melt_factor", "get_melt_factor");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "radiation_melt_factor", PROPERTY_HINT_RANGE, "0,0.1,0.0001"), "set_radiation_melt_factor", "get_radiation_melt_factor");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "soil_capacity", PROPERTY_HINT_RANGE, "0,1000,1,suffix:mm"), "set_soil_capacity", "get_soil_capacity");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "direct_runoff", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_direct_runoff", "get_direct_runoff");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "groundwater_release", PROPERTY_HINT_RANGE, "0.01,1,0.01"), "set_groundwater_release", "get_groundwater_release");

	ADD_GROUP("Grid", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "sun_cell", PROPERTY_HINT_RANGE, "1,256,0.1,suffix:m"), "set_sun_cell", "get_sun_cell");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "precipitation_cell", PROPERTY_HINT_RANGE, "4,1024,1,suffix:m"), "set_precipitation_cell", "get_precipitation_cell");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "context_margin", PROPERTY_HINT_RANGE, "0,50000,1,or_greater,suffix:m"), "set_context_margin", "get_context_margin");
}

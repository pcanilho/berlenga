/**************************************************************************/
/*  snow_climate.cpp                                                      */
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

#include "snow_climate.h"

#include "core/object/class_db.h"
#include "modules/terrain_deform/climate_recipe.h"

namespace {
constexpr float SOLAR_CONSTANT = 1353.0f;
constexpr int COLDEST_DAY = 20; // late January
} // namespace

float SnowClimate::_normal() {
	// Box-Muller.
	const float u1 = MAX(rng.randf(), 1e-7f);
	const float u2 = rng.randf();
	return Math::sqrt(-2.0f * Math::log(u1)) * Math::cos(Math::TAU * u2);
}

float SnowClimate::seasonal_temperature(float p_day_of_year) const {
	// One harmonic between the January and July means, coldest in late January.
	const float mean = 0.5f * (january_temperature + july_temperature);
	const float amp = 0.5f * (july_temperature - january_temperature);
	return mean - amp * Math::cos(Math::TAU * (p_day_of_year - COLDEST_DAY) / 365.0f);
}

float SnowClimate::seasonal_lapse_rate(float p_day_of_year, float p_default) const {
	if (recipe.is_null()) {
		return p_default;
	}
	// The terrain climate's harmonic (TerrainClimate::month_value) on the day, coldest in late January.
	const float w = recipe->get_lapse_rate_january();
	const float s = recipe->get_lapse_rate_july();
	return 0.5f * (w + s) - 0.5f * (s - w) * Math::cos(Math::TAU * (p_day_of_year - COLDEST_DAY) / 365.0f);
}

float SnowClimate::wet_day_amount(float p_day_of_year) const {
	if (recipe.is_null()) {
		return wet_day_precipitation;
	}
	const TerrainClimate::Params &cp = recipe->get_params();
	const int month = CLAMP(int((p_day_of_year - 1.0f) / (365.0f / 12.0f)), 0, 11);
	const float per_day = MAX(cp.precipitation, 0.0f) * TerrainClimate::precipitation_share(cp, month) * 12.0f / 365.0f;
	const float p_wet = wet_after_dry / MAX(1.0f + wet_after_dry - wet_after_wet, 1e-3f);
	return per_day / MAX(p_wet, 1e-3f);
}

void SnowClimate::set_recipe(const Ref<ClimateRecipe> &p_recipe) {
	recipe = p_recipe;
	emit_changed();
}

Vector3 SnowClimate::sun_direction(float p_day_of_year, float p_hour) const {
	// Declination (Cooper 1969) and the hour angle at local solar time; x east, y up, z south.
	const float decl = Math::deg_to_rad(23.44f) * Math::sin(Math::TAU * (284.0f + p_day_of_year) / 365.0f);
	const float lat = Math::deg_to_rad(latitude);
	const float w = Math::deg_to_rad(15.0f * (p_hour - 12.0f));
	const float east = -Math::cos(decl) * Math::sin(w);
	const float north = Math::cos(lat) * Math::sin(decl) - Math::sin(lat) * Math::cos(decl) * Math::cos(w);
	const float up = Math::sin(lat) * Math::sin(decl) + Math::cos(lat) * Math::cos(decl) * Math::cos(w);
	return Vector3(east, up, -north);
}

void SnowClimate::start_weather() {
	if (recipe.is_valid()) {
		latitude = recipe->get_latitude();
		base_height = recipe->get_reference_height();
		january_temperature = recipe->get_temperature_january();
		july_temperature = recipe->get_temperature_july();
		temperature_variability = recipe->get_daily_temperature_spread();
		prevailing_wind_from = recipe->get_wind_from();
		wind_spread = recipe->get_wind_spread();
		// The recipe's wind is the wet-day wind (its precipitation follows it); dry days blow softer.
		wind_speed = recipe->get_wind_speed() / MAX(storm_wind_factor, 0.1f);
	}
	rng.seed(uint64_t(seed) * 2654435761ull + 12345u);
	wet = false;
	residual = 0.0f;
	gen_day = -1;
}

SnowClimate::Hour SnowClimate::weather_at(int p_day, float p_hour) {
	if (p_day != gen_day) {
		gen_day = p_day;
		// WGEN (Richardson 1981): occurrence by a two-state Markov chain, amount exponential on wet days.
		wet = rng.randf() < (wet ? wet_after_wet : wet_after_dry);
		const float day_of_year = float((start_day - 1 + p_day) % 365) + 1.0f;
		day_precip = wet ? -wet_day_amount(day_of_year) * Math::log(MAX(1.0f - rng.randf(), 1e-6f)) : 0.0f;
		// Temperature: the seasonal mean plus a first-order autoregressive residual; wet days run a little
		// cooler and swing less through the day.
		residual = temperature_persistence * residual + temperature_variability * Math::sqrt(MAX(1.0f - temperature_persistence * temperature_persistence, 0.0f)) * _normal();
		const float doy = float((start_day - 1 + p_day) % 365) + 1.0f;
		// Wet days 1 C cooler, dry days warmer by as much again over the share of wet days, so the mean holds.
		const float p_wet = wet_after_dry / MAX(1.0f + wet_after_dry - wet_after_wet, 1e-3f);
		day_mean = seasonal_temperature(doy) + residual + (wet ? -1.0f : p_wet / MAX(1.0f - p_wet, 1e-3f));
		day_range = daily_range * (wet ? 0.4f : 1.0f);
		// Wind: Weibull speed (shape 2) around the day's mean, direction around the prevailing one.
		const float mean_speed = wind_speed * (wet ? storm_wind_factor : 1.0f);
		const float speed = mean_speed / 0.8862f * Math::sqrt(-Math::log(MAX(1.0f - rng.randf(), 1e-6f)));
		const float from = Math::deg_to_rad(prevailing_wind_from + wind_spread * _normal());
		// Coming from bearing b (clockwise from north, north = -z), it blows toward b + 180.
		day_wind = Vector2(-Math::sin(from), Math::cos(from)) * speed;
	}
	Hour h;
	const float doy = float((start_day - 1 + p_day) % 365) + 1.0f + p_hour / 24.0f;
	// Warmest mid-afternoon, coldest before dawn.
	h.temperature = day_mean + 0.5f * day_range * Math::cos(Math::TAU * (p_hour - 15.0f) / 24.0f);
	h.humidity = wet ? 0.95f : 0.75f;
	h.precipitation = day_precip / 24.0f;
	h.cloud_cover = wet ? 0.95f : 0.35f;
	h.wind = day_wind;
	const Vector3 s = sun_direction(doy, p_hour);
	if (s.y > 0.01f) {
		const float air_mass = 1.0f / s.y;
		h.to_sun = s;
		// Clear-sky direct beam (Meinel), through clouds by Kasten and Czeplak (1980).
		h.direct_normal = SOLAR_CONSTANT * Math::pow(0.7f, Math::pow(air_mass, 0.678f)) * (1.0f - 0.75f * Math::pow(h.cloud_cover, 3.4f));
	}
	return h;
}

PackedFloat32Array SnowClimate::sample_days(int p_days) {
	PackedFloat32Array out;
	out.resize(MAX(p_days, 0) * 4);
	start_weather();
	for (int d = 0; d < p_days; d++) {
		float t = 0.0f;
		float p = 0.0f;
		float u = 0.0f;
		for (int hr = 0; hr < 24; hr++) {
			const Hour h = weather_at(d, hr + 0.5f);
			t += h.temperature / 24.0f;
			p += h.precipitation;
			u += h.wind.length() / 24.0f;
		}
		out.set(d * 4 + 0, t);
		out.set(d * 4 + 1, p);
		out.set(d * 4 + 2, u);
		out.set(d * 4 + 3, wet ? 1.0f : 0.0f);
	}
	return out;
}

void SnowClimate::set_result(Rect2 p_rect, int p_resolution, const PackedFloat32Array &p_swe, const PackedFloat32Array &p_liquid, const PackedFloat32Array &p_density,
		const PackedFloat32Array &p_albedo, const PackedFloat32Array &p_cornice_x, const PackedFloat32Array &p_cornice_z) {
	rect = p_rect;
	resolution = p_resolution;
	swe = p_swe;
	liquid = p_liquid;
	density = p_density;
	albedo = p_albedo;
	cornice_x = p_cornice_x;
	cornice_z = p_cornice_z;
	emit_changed();
}

void SnowClimate::clear_result() {
	set_result(Rect2(), 0, PackedFloat32Array(), PackedFloat32Array(), PackedFloat32Array(), PackedFloat32Array(), PackedFloat32Array(), PackedFloat32Array());
}

void SnowClimate::_bind_methods() {
	ClassDB::bind_method(D_METHOD("seasonal_temperature", "day_of_year"), &SnowClimate::seasonal_temperature);
	ClassDB::bind_method(D_METHOD("sun_direction", "day_of_year", "hour"), &SnowClimate::sun_direction);
	ClassDB::bind_method(D_METHOD("seasonal_lapse_rate", "day_of_year", "default"), &SnowClimate::seasonal_lapse_rate);
	ClassDB::bind_method(D_METHOD("wet_day_amount", "day_of_year"), &SnowClimate::wet_day_amount);
	ClassDB::bind_method(D_METHOD("set_recipe", "recipe"), &SnowClimate::set_recipe);
	ClassDB::bind_method(D_METHOD("get_recipe"), &SnowClimate::get_recipe);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "recipe", PROPERTY_HINT_RESOURCE_TYPE, "ClimateRecipe"), "set_recipe", "get_recipe");
	ClassDB::bind_method(D_METHOD("sample_days", "days"), &SnowClimate::sample_days);
	ClassDB::bind_method(D_METHOD("is_baked"), &SnowClimate::is_baked);
	ClassDB::bind_method(D_METHOD("clear_result"), &SnowClimate::clear_result);

#define BIND_PROP(m_type, m_name, m_hint, m_hint_string)                                                  \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &SnowClimate::set_##m_name);                   \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &SnowClimate::get_##m_name);                            \
	ADD_PROPERTY(PropertyInfo(m_type, #m_name, m_hint, m_hint_string), "set_" #m_name, "get_" #m_name);

	ADD_GROUP("Place", "");
	BIND_PROP(Variant::FLOAT, latitude, PROPERTY_HINT_RANGE, "-90,90,0.1,suffix:deg")
	BIND_PROP(Variant::FLOAT, base_height, PROPERTY_HINT_RANGE, "-1000,9000,1,or_greater,or_less,suffix:m")
	ADD_GROUP("Temperature", "");
	BIND_PROP(Variant::FLOAT, january_temperature, PROPERTY_HINT_RANGE, "-60,40,0.1,suffix:C")
	BIND_PROP(Variant::FLOAT, july_temperature, PROPERTY_HINT_RANGE, "-60,40,0.1,suffix:C")
	BIND_PROP(Variant::FLOAT, daily_range, PROPERTY_HINT_RANGE, "0,30,0.1,suffix:C")
	BIND_PROP(Variant::FLOAT, temperature_persistence, PROPERTY_HINT_RANGE, "0,0.99,0.01")
	BIND_PROP(Variant::FLOAT, temperature_variability, PROPERTY_HINT_RANGE, "0,15,0.1,suffix:C")
	ADD_GROUP("Precipitation", "");
	BIND_PROP(Variant::FLOAT, wet_after_dry, PROPERTY_HINT_RANGE, "0,1,0.01")
	BIND_PROP(Variant::FLOAT, wet_after_wet, PROPERTY_HINT_RANGE, "0,1,0.01")
	BIND_PROP(Variant::FLOAT, wet_day_precipitation, PROPERTY_HINT_RANGE, "0,100,0.1,suffix:mm")
	BIND_PROP(Variant::FLOAT, precipitation_gradient, PROPERTY_HINT_RANGE, "0,0.5,0.005")
	ADD_GROUP("Wind", "");
	BIND_PROP(Variant::FLOAT, prevailing_wind_from, PROPERTY_HINT_RANGE, "0,360,1,suffix:deg")
	BIND_PROP(Variant::FLOAT, wind_spread, PROPERTY_HINT_RANGE, "0,180,1,suffix:deg")
	BIND_PROP(Variant::FLOAT, wind_speed, PROPERTY_HINT_RANGE, "0,40,0.1,suffix:m/s")
	BIND_PROP(Variant::FLOAT, storm_wind_factor, PROPERTY_HINT_RANGE, "1,5,0.05")
	ADD_GROUP("Season", "");
	BIND_PROP(Variant::INT, start_day, PROPERTY_HINT_RANGE, "1,365,1")
	BIND_PROP(Variant::INT, days, PROPERTY_HINT_RANGE, "1,730,1")
	BIND_PROP(Variant::INT, step_hours, PROPERTY_HINT_RANGE, "1,24,1")
	BIND_PROP(Variant::INT, seed, PROPERTY_HINT_NONE, "")
	ADD_GROUP("Result", "");
	BIND_PROP(Variant::RECT2, rect, PROPERTY_HINT_NONE, "")
	BIND_PROP(Variant::INT, resolution, PROPERTY_HINT_NONE, "")
	BIND_PROP(Variant::PACKED_FLOAT32_ARRAY, swe, PROPERTY_HINT_NONE, "")
	BIND_PROP(Variant::PACKED_FLOAT32_ARRAY, liquid, PROPERTY_HINT_NONE, "")
	BIND_PROP(Variant::PACKED_FLOAT32_ARRAY, density, PROPERTY_HINT_NONE, "")
	BIND_PROP(Variant::PACKED_FLOAT32_ARRAY, albedo, PROPERTY_HINT_NONE, "")
	BIND_PROP(Variant::PACKED_FLOAT32_ARRAY, cornice_x, PROPERTY_HINT_NONE, "")
	BIND_PROP(Variant::PACKED_FLOAT32_ARRAY, cornice_z, PROPERTY_HINT_NONE, "")
#undef BIND_PROP
}

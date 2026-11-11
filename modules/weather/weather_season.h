/**************************************************************************/
/*  weather_season.h                                                      */
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

// The state of the season at a place (docs/ideas/weather.md, "Season state"): the day, the day length and the
// thermal sums plants and the ground respond to. One state for every system (trees, ground plants, snow, water).
//
// Sums run day by day over a climatological year (a harmonic of the January and July means, as TerrainGenerator3D's
// climate), so any date is right at once; WeatherMap3D adds the live weather's departure from that climate as the
// game runs.
// - Photoperiod: Forsythe et al. 1995 (CBM model), the sun's centre 0.8333 degrees below the horizon.
// - Forcing: growing degree days above 5 C since 1 January (Cannell and Smith 1983).
// - Chilling: days with a mean of 0 to 5 C since 1 November (Cannell and Smith 1983).
// - Senescence: Delpierre et al. 2009: from the first day after midsummer the photoperiod is under 13.2 h, add
//   max(0, 20 - T) (P / 13.2)^2 each day.
// - Snow: a degree-day snowpack (Hock 2003) from 1 August, snow share of each day's precipitation from a normal
//   spread of daily means about the rain/snow threshold (Jennings et al. 2018), snow density 300 kg/m3.
// - Soil frost: the frost index of Nelson and Outcalt 1987 in its simplest form, freezing degree days since
//   1 August, thawed back by positive ones, under no snow (snow insulates the ground).
// - Soil moisture: a daily Thornthwaite and Mather 1957 bucket (as McCabe and Markstrom 2007) with Thornthwaite 1948
//   evapotranspiration, spun up over a year.

#include "core/math/math_funcs.h"
#include "core/variant/dictionary.h"

#include <cmath>

namespace WeatherSeason {

struct Climate {
	float latitude = 46.0f; // degrees
	float reference_height = 0.0f; // m
	float temperature_january = 3.0f; // C
	float temperature_july = 22.0f; // C
	float lapse_rate_january = 4.5f; // K/km
	float lapse_rate_july = 6.5f; // K/km
	float precipitation = 900.0f; // mm a year
	float precipitation_seasonality = 0.0f;
	float precipitation_peak_month = 0.0f; // 0 January .. 11 December
	float rain_snow_threshold = 1.0f; // C
	float daily_temperature_spread = 3.0f; // K
	float melt_factor = 1.8f; // mm/day/K
	float soil_capacity = 150.0f; // mm
};

struct State {
	int day_of_year = 1; // 1 .. 365
	float latitude_deg = 46.0f;
	float photoperiod_h = 12.0f;
	float temperature = 0.0f; // the day's climatological mean, C
	float forcing = 0.0f; // K days
	float chilling = 0.0f; // days
	float senescence = 0.0f; // K days (weighted)
	float snow_water = 0.0f; // mm
	float snow_depth = 0.0f; // m
	float snow_cover = 0.0f; // 0..1
	float snow_line_m = 0.0f; // m
	float soil_frost = 0.0f; // K days
	float soil_moisture = 1.0f; // 0..1
};

constexpr float SENESCENCE_PHOTOPERIOD = 13.2f; // h
constexpr float SENESCENCE_BASE = 20.0f; // C
constexpr float FORCING_BASE = 5.0f; // C
constexpr float SNOW_DENSITY = 300.0f; // kg/m3

// Day length in hours (Forsythe et al. 1995).
inline float photoperiod(float p_latitude_deg, int p_day_of_year) {
	const double theta = 0.2163108 + 2.0 * Math::atan(0.9671396 * Math::tan(0.00860 * (p_day_of_year - 186)));
	const double phi = Math::asin(0.39795 * Math::cos(theta));
	const double lat = Math::deg_to_rad(double(p_latitude_deg));
	const double p = Math::deg_to_rad(0.8333);
	const double c = CLAMP((Math::sin(p) + Math::sin(lat) * Math::sin(phi)) / (Math::cos(lat) * Math::cos(phi)), -1.0, 1.0);
	return float(24.0 - (24.0 / Math::PI) * Math::acos(c));
}

// A harmonic through the January and July values, at day of year d (mid-January = day 15).
inline float day_value(float p_january, float p_july, float p_day) {
	return 0.5f * (p_january + p_july) - 0.5f * (p_july - p_january) * Math::cos(float(Math::TAU) * (p_day - 15.0f) / 365.0f);
}

inline float day_temperature(const Climate &p_c, float p_height, float p_day) {
	const float lapse = day_value(p_c.lapse_rate_january, p_c.lapse_rate_july, p_day);
	return day_value(p_c.temperature_january, p_c.temperature_july, p_day) - lapse * (p_height - p_c.reference_height) * 0.001f;
}

inline float day_precipitation(const Climate &p_c, float p_day) {
	const float a = CLAMP(p_c.precipitation_seasonality, 0.0f, 1.0f);
	const float month = (p_day - 15.0f) * 12.0f / 365.0f;
	return p_c.precipitation / 365.0f * (1.0f + a * Math::cos(float(Math::TAU) * (month - p_c.precipitation_peak_month) / 12.0f));
}

// Share of a day's precipitation falling as snow: daily means spread normally about the month's.
inline float snow_share(const Climate &p_c, float p_t) {
	const float s = MAX(p_c.daily_temperature_spread, 0.1f);
	return 0.5f * (1.0f - float(std::erf((p_t - p_c.rain_snow_threshold) / (s * float(Math::SQRT2)))));
}

// Days back from p_day to the most recent p_start (day of year), at least 0.
inline int days_since(int p_day, int p_start) {
	return (p_day - p_start + 365) % 365;
}

// The snowpack at a height on a day, mm of water: built from 1 August.
inline float snow_water_at(const Climate &p_c, float p_height, int p_day, float p_temperature_offset = 0.0f) {
	const int n = days_since(p_day, 213);
	float swe = 0.0f;
	for (int k = 0; k <= n; k++) {
		const float d = float((213 + k - 1) % 365 + 1);
		const float t = day_temperature(p_c, p_height, d) + p_temperature_offset;
		swe += day_precipitation(p_c, d) * snow_share(p_c, t);
		swe = MAX(swe - p_c.melt_factor * MAX(t, 0.0f), 0.0f);
	}
	return swe;
}

// Monthly Thornthwaite heat index and exponent for the climatological year at a height.
inline void thornthwaite_coefficients(const Climate &p_c, float p_height, double &r_heat, double &r_a) {
	r_heat = 0.0;
	for (int m = 0; m < 12; m++) {
		const float t = day_temperature(p_c, p_height, 15.0f + m * 365.0f / 12.0f);
		if (t > 0.0f) {
			r_heat += Math::pow(double(t) / 5.0, 1.514);
		}
	}
	r_a = 6.75e-7 * r_heat * r_heat * r_heat - 7.71e-5 * r_heat * r_heat + 1.792e-2 * r_heat + 0.49239;
}

// Thornthwaite potential evapotranspiration for one day, mm.
inline float day_pet(const Climate &p_c, double p_heat, double p_a, float p_t, int p_day) {
	if (p_t <= 0.0f || p_heat <= 0.0) {
		return 0.0f;
	}
	double e = p_t < 26.5f ? 16.0 * Math::pow(10.0 * p_t / p_heat, p_a) : -415.85 + 32.24 * p_t - 0.43 * p_t * p_t;
	e = MAX(e, 0.0) / 30.0; // mm a 30-day month of 12 h days, per day
	return float(e * photoperiod(p_c.latitude, p_day) / 12.0);
}

// The climatological state at a height on a day of the year (1..365); offsets (K) shift every day's temperature
// for a warmer or colder year.
inline State compute(const Climate &p_c, float p_height, int p_day, float p_temperature_offset = 0.0f) {
	State s;
	const int day = CLAMP(p_day, 1, 365);
	s.day_of_year = day;
	s.latitude_deg = p_c.latitude;
	s.photoperiod_h = photoperiod(p_c.latitude, day);
	s.temperature = day_temperature(p_c, p_height, day) + p_temperature_offset;
	auto t_at = [&](int d) { return day_temperature(p_c, p_height, float(d)) + p_temperature_offset; };
	auto wrap = [](int d) { return (d - 1 + 365) % 365 + 1; };
	for (int d = 1; d <= day; d++) {
		s.forcing += MAX(t_at(d) - FORCING_BASE, 0.0f);
	}
	for (int k = 0, n = days_since(day, 305); k <= n; k++) {
		const float t = t_at(wrap(305 + k));
		if (t >= 0.0f && t <= 5.0f) {
			s.chilling += 1.0f;
		}
	}
	if (day > 172) {
		for (int d = 173; d <= day; d++) {
			const float pp = photoperiod(p_c.latitude, d);
			if (pp < SENESCENCE_PHOTOPERIOD) {
				const float r = pp / SENESCENCE_PHOTOPERIOD;
				s.senescence += MAX(SENESCENCE_BASE - t_at(d), 0.0f) * r * r;
			}
		}
	}
	// Snowpack and soil frost from 1 August.
	float swe = 0.0f;
	float frost = 0.0f;
	for (int k = 0, n = days_since(day, 213); k <= n; k++) {
		const int d = wrap(213 + k);
		const float t = t_at(d);
		swe += day_precipitation(p_c, d) * snow_share(p_c, t);
		swe = MAX(swe - p_c.melt_factor * MAX(t, 0.0f), 0.0f);
		// Snow over about 20 cm (60 mm of water) shields the ground from the air's cold.
		const float shield = CLAMP(1.0f - swe / 60.0f, 0.0f, 1.0f);
		frost = MAX(frost + (t < 0.0f ? -t * shield : -t), 0.0f);
	}
	s.snow_water = swe;
	s.snow_depth = swe / SNOW_DENSITY;
	s.snow_cover = float(Math::tanh(double(s.snow_depth) / 0.1));
	s.soil_frost = frost;
	// The snow line: the lowest height (50 m steps, up to 5 km above the reference) holding over 1 mm of snow.
	s.snow_line_m = p_c.reference_height + 5000.0f;
	if (swe > 1.0f) {
		s.snow_line_m = p_height;
		for (float h = p_height - 50.0f; h > p_c.reference_height - 1000.0f; h -= 50.0f) {
			if (snow_water_at(p_c, h, day, p_temperature_offset) <= 1.0f) {
				break;
			}
			s.snow_line_m = h;
		}
	} else {
		for (float h = p_height + 50.0f; h <= p_c.reference_height + 5000.0f; h += 50.0f) {
			if (snow_water_at(p_c, h, day, p_temperature_offset) > 1.0f) {
				s.snow_line_m = h;
				break;
			}
		}
	}
	// Soil water: a year's spin-up, then the year to the day.
	double heat = 0.0;
	double a = 0.0;
	thornthwaite_coefficients(p_c, p_height, heat, a);
	const float cap = MAX(p_c.soil_capacity, 1.0f);
	float soil = cap;
	float pack = 0.0f;
	for (int k = 0; k < 2 * 365; k++) {
		const int d = wrap(day + 1 + k); // two years, ending on the day itself
		const float t = t_at(d);
		const float p = day_precipitation(p_c, d);
		const float snow = p * snow_share(p_c, t);
		pack += snow;
		const float melt = MIN(pack, p_c.melt_factor * MAX(t, 0.0f));
		pack -= melt;
		const float water = p - snow + melt;
		const float pet = day_pet(p_c, heat, a, t, d);
		if (water >= pet) {
			soil = MIN(soil + water - pet, cap);
		} else {
			// Drying slows as the soil empties (Thornthwaite and Mather 1957).
			soil *= Math::exp(-(pet - water) / cap);
		}
	}
	s.soil_moisture = CLAMP(soil / cap, 0.0f, 1.0f);
	return s;
}

inline Dictionary to_dictionary(const State &p_s) {
	Dictionary d;
	d["day_of_year"] = p_s.day_of_year;
	d["latitude_deg"] = p_s.latitude_deg;
	d["photoperiod_h"] = p_s.photoperiod_h;
	d["temperature"] = p_s.temperature;
	d["forcing"] = p_s.forcing;
	d["chilling"] = p_s.chilling;
	d["senescence"] = p_s.senescence;
	d["snow_depth"] = p_s.snow_depth;
	d["snow_cover"] = p_s.snow_cover;
	d["snow_line_m"] = p_s.snow_line_m;
	d["soil_frost"] = p_s.soil_frost;
	d["soil_moisture"] = p_s.soil_moisture;
	return d;
}

} // namespace WeatherSeason

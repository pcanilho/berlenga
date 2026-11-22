/**************************************************************************/
/*  terrain_runoff.h                                                      */
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
/* "Software"), to deal in the Software without restriction, including   */
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

// Monthly water balance of one climate cell (docs/ideas/terrain-generation.md, "Rain and snowmelt feed the rivers").
// The USGS monthly water balance model (McCabe and Markstrom 2007) with:
//   - rain or snow by the share of days below the rain-snow threshold (daily means normal around the month's mean,
//     as the climate maps' snow share; Jennings et al. 2018 threshold);
//   - snowmelt by temperature index with the sun (Hock 1999: melt = (MF + a I) PDD), the month's positive degree
//     days from its mean and the daily spread (Calov and Greve 2005);
//   - Thornthwaite (1948) potential evapotranspiration and the Thornthwaite and Mather (1957) soil bucket;
//   - direct runoff of a share of the rain, and the surplus through a linear groundwater reservoir (the first store
//     of a Nash 1957 cascade) releasing a share each month.
// Open water: rain and melt less the lake's evaporation, spread over the months as PET.
namespace TerrainRunoff {

static const int DAYS[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

struct Cell {
	float runoff[12]; // mm in each month (negative on a lake that loses more than it gets)
	float snow[12]; // snow water equivalent at the month's end, mm
	float evaporation = 0.0f; // actual evapotranspiration, mm a year
	float precipitation = 0.0f; // mm a year
};

// Positive degree days in a month of p_days days with mean p_t and daily standard deviation p_sigma.
inline float positive_degree_days(float p_t, float p_sigma, int p_days) {
	const float s = MAX(p_sigma, 0.01f);
	const float pdd = s / std::sqrt(float(Math::TAU)) * std::exp(-p_t * p_t / (2.0f * s * s)) + 0.5f * p_t * std::erfc(-p_t / (float(Math::SQRT2) * s));
	return MAX(pdd, 0.0f) * p_days;
}

// p_height: the surface (ground or water); p_precipitation: mm a year; p_shortwave and p_shortwave_winter: W/m2
// (annual and December-February means); p_open_water_evaporation: mm a year, or < 0 on land.
inline void balance(const TerrainClimate::Params &p, float p_height, float p_precipitation, float p_shortwave, float p_shortwave_winter, float p_open_water_evaporation, Cell &r) {
	float t[12], pet[12];
	for (int m = 0; m < 12; m++) {
		t[m] = TerrainClimate::temperature(p, p_height, m);
	}
	const float pet_year = TerrainClimate::thornthwaite_pet(t, p.latitude, pet);
	const float sigma = MAX(p.daily_temperature_spread, 0.1f);
	const float stc = MAX(p.soil_capacity, 1.0f);
	const bool water = p_open_water_evaporation >= 0.0f;
	float pack = 0.0f, soil = stc, ground = 0.0f, aet = 0.0f;
	r.precipitation = p_precipitation;
	// Three years from a full bucket and no snow; the third is kept (the stores then repeat year on year).
	for (int year = 0; year < 3; year++) {
		const bool keep = year == 2;
		aet = 0.0f;
		for (int m = 0; m < 12; m++) {
			const float pm = p_precipitation * TerrainClimate::precipitation_share(p, m);
			const float snow_share = 0.5f * (1.0f + std::erf((p.rain_snow_threshold - t[m]) / (sigma * float(Math::SQRT2))));
			const float snow = pm * snow_share;
			const float rain = pm - snow;
			// Sun on the cell this month: a harmonic through the winter (January) and annual means.
			const float sun = MAX(p_shortwave - (p_shortwave - p_shortwave_winter) * Math::cos(float(Math::TAU) * m / 12.0f), 0.0f);
			const float melt_potential = (p.melt_factor + p.radiation_melt_factor * sun) * positive_degree_days(t[m], sigma, DAYS[m]);
			pack += snow;
			const float melt = MIN(pack, melt_potential);
			pack -= melt;
			float out;
			if (water) {
				const float e = pet_year > 0.0f ? p_open_water_evaporation * pet[m] / pet_year : p_open_water_evaporation / 12.0f;
				out = rain + melt - e;
				aet += e;
			} else {
				const float direct = p.direct_runoff * rain;
				const float input = rain - direct + melt;
				float surplus = 0.0f;
				if (input >= pet[m]) {
					aet += pet[m];
					soil += input - pet[m];
					surplus = MAX(soil - stc, 0.0f);
					soil = MIN(soil, stc);
				} else {
					const float after = soil * std::exp(-(pet[m] - input) / stc);
					aet += input + (soil - after);
					soil = after;
				}
				ground += surplus;
				const float release = CLAMP(p.groundwater_release, 0.0f, 1.0f) * ground;
				ground -= release;
				out = direct + release;
			}
			if (keep) {
				r.runoff[m] = out;
				r.snow[m] = pack;
			}
		}
	}
	r.evaporation = aet;
}

} // namespace TerrainRunoff

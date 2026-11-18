/**************************************************************************/
/*  test_rain.h                                                           */
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

// The rain's relations (docs/ideas/rain.md) against their published values.

#include "../rain_fall_3d.h"
#include "../weather_map_3d.h"
#include "../weather_runoff_3d.h"

#include "tests/test_macros.h"

namespace TestRain {

TEST_CASE("[Weather][Rain] Marshall-Palmer drops carry the rain rate they were drawn for") {
	// R = 6 pi 1e-4 * integral of D^3 v(D) N(D) dD (mm/h, D in mm): the size law and the fall speed must agree
	// within the 20% their fits allow.
	for (float rate : { 1.0f, 10.0f, 50.0f }) {
		const float l = RainFall3D::drop_size_slope(rate);
		double s = 0.0;
		for (float d = 0.0005f; d < 8.0f; d += 0.001f) {
			s += double(d) * d * d * RainFall3D::terminal_velocity(d) * 8000.0 * Math::exp(-double(l) * d) * 0.001;
		}
		const double r = 6.0 * Math::PI * 1e-4 * s;
		CHECK(r > rate * 0.9);
		CHECK(r < rate * 1.3);
	}
}

TEST_CASE("[Weather][Rain] Drop speed, impacts and extinction") {
	CHECK(RainFall3D::terminal_velocity(2.0f) == doctest::Approx(6.49f).epsilon(0.02)); // Gunn and Kinzer 1949: 6.49
	CHECK(RainFall3D::terminal_velocity(5.0f) == doctest::Approx(9.09f).epsilon(0.02)); // 9.09
	const float flux = RainFall3D::impact_flux(10.0f, 0.5f, 8.0f);
	CHECK(flux > 2000.0f);
	CHECK(flux < 4500.0f);
	CHECK(RainFall3D::impact_flux(1.0f, 0.5f, 8.0f) < flux);
	// Atlas 1953: about 1.5 per km at 10 mm/h.
	CHECK(RainFall3D::rain_extinction(10.0f) * 1000.0f == doctest::Approx(1.55f).epsilon(0.05));
	CHECK(RainFall3D::rain_extinction(0.0f) == 0.0f);
	// A 2 mm drop at terminal speed splashes a crown on a dry surface (K over 2,100); a 0.5 mm drizzle drop does not.
	CHECK(RainFall3D::splash_parameter(2.0f, RainFall3D::terminal_velocity(2.0f)) > 2100.0f);
	CHECK(RainFall3D::splash_parameter(0.5f, RainFall3D::terminal_velocity(0.5f)) < 2100.0f);
}

TEST_CASE("[Weather][Rain] Rain and snow split by temperature") {
	CHECK(WeatherMap3D::rain_fraction(10.0f, 0.7f) > 0.99f);
	CHECK(WeatherMap3D::rain_fraction(-5.0f, 0.7f) < 0.01f);
	const float a = WeatherMap3D::rain_fraction(1.0f, 0.7f);
	CHECK(a > 0.05f);
	CHECK(a < 0.95f);
	// Drier air keeps snow to warmer temperatures (Jennings et al. 2018).
	CHECK(WeatherMap3D::rain_fraction(1.0f, 0.5f) < a);
}

TEST_CASE("[Weather][Rain] Rutter canopy drainage") {
	CHECK(WeatherMap3D::canopy_drainage(0.0f, 1.0f) == 0.0f);
	// mm/h: Ds = 0.0014 mm/min at capacity, rising by e^3.7 per mm held over it.
	CHECK(WeatherMap3D::canopy_drainage(1.0f, 1.0f) == doctest::Approx(0.0014f * 60.0f).epsilon(0.01));
	CHECK(WeatherMap3D::canopy_drainage(2.0f, 1.0f) == doctest::Approx(0.0014f * 60.0f * Math::exp(3.7f)).epsilon(0.01));
}

TEST_CASE("[Weather][Rain] SCS curve number and flow speeds") {
	// NRCS NEH 630 ch. 10: CN 75, 50 mm of rain gives 9.3 mm of runoff; none below the initial abstraction.
	CHECK(WeatherRunoff3D::scs_runoff(50.0f, 75.0f) == doctest::Approx(9.29f).epsilon(0.02));
	CHECK(WeatherRunoff3D::scs_runoff(10.0f, 75.0f) == 0.0f);
	CHECK(WeatherRunoff3D::scs_runoff(50.0f, 100.0f) == doctest::Approx(50.0f));
	// TR-55 unpaved shallow flow: 16.13 sqrt(s) ft/s.
	CHECK(WeatherRunoff3D::shallow_flow_velocity(0.04f) == doctest::Approx(16.1345f * 0.3048f * 0.2f).epsilon(0.01));
	CHECK(WeatherRunoff3D::channel_velocity(10.0f) > WeatherRunoff3D::channel_velocity(1.0f));
}

} // namespace TestRain

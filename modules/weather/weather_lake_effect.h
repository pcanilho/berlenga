/**************************************************************************/
/*  weather_lake_effect.h                                                 */
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

#include "core/math/rect2.h"
#include "core/math/vector2.h"
#include "core/math/vector2i.h"
#include "core/math/vector4.h"
#include "core/templates/vector.h"

// Lake-effect snow (docs/ideas/clouds.md): cold air crossing open water much warmer than itself takes up heat and
// moisture and builds convective cloud downwind of the shore, growing with the distance it has crossed (the fetch).
// The water is sampled over a square round the camera (WorldWater against the terrain, a metre deep or more, so
// puddles do not count) over several ticks; for the wind's direction each cell then holds the open water crossed
// upwind of it (km, each kilometre weighted by exp(-s / 60 km) for the distance s back to it, so the cloud and the
// snow fade inland), and how near it lies to the longest fetch across the wind (the axis a single band forms on).
class WeatherLakeEffect {
public:
	int resolution = 64;
	float extent = 128000.0f; // m, 2 km cells
	static constexpr float DECAY = 60000.0f; // m, the bands' reach inland (Niziol et al. 1995: 50 to 100 km)

	Vector2i origin;
	bool water_valid = false;
	bool derived = false;
	bool dirty = true;
	bool any_water = false;
	Vector<float> water; // n * n, 0 or 1
	Vector<float> pending;
	Vector2i pending_origin;
	int pending_row = -1;

	Vector2 wind_dir; // unit, what the field was derived for
	Vector<float> field; // n * n * 2: fetch (km, decayed) and band axis (0..1)
	float fetch_max = 0.0f; // km, the longest over-water fetch in the square along the wind

	float cell_size() const { return extent / resolution; }
	Rect2 rect_of(Vector2i p_origin) const;
	Rect2 rect() const { return rect_of(origin); }
	bool needs_water(Vector2 p_center) const;
	void begin_water(Vector2 p_center);
	bool is_sampling() const { return pending_row >= 0; }
	Vector2 pending_cell_center(int p_x, int p_z) const;
	void finish_water();
	// Again when the wind turns by about 5 degrees.
	bool needs_derive(Vector2 p_wind_dir) const;
	void derive(Vector2 p_wind_dir);
};

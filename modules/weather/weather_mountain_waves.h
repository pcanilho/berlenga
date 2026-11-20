/**************************************************************************/
/*  weather_mountain_waves.h                                              */
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
#include "core/math/vector3.h"
#include "core/templates/vector.h"

// Mountain waves over the terrain (docs/ideas/clouds.md, lenticulars): stably stratified air crossing a ridge
// rises and sinks in waves fixed to the ground. Linear theory (Smith 1980, Tellus 32): the terrain's Fourier
// transform times exp(i m z), with the non-hydrostatic vertical wavenumber m^2 = (k^2 + l^2) (N^2 - s^2) / s^2,
// s = U k + V l the intrinsic frequency; m takes the sign of s where the waves propagate (energy upward, the
// radiation condition) and decays where they do not. Air below the dividing streamline (a mountain's top U / N
// goes over, the rest round; Sheppard 1956, Smith 1989) does not lift, so only the top U / N of the terrain
// counts. The square is sampled through WeatherMap3D's terrain source over several ticks, as the terrain wind.
class WeatherMountainWaves {
public:
	static constexpr int LEVELS = 4;

	int resolution = 128;
	float extent = 51200.0f; // m, 400 m cells: waves 2 pi U / N of 5 to 15 km are 12 cells or more
	float levels[LEVELS] = { 2000.0f, 3500.0f, 5000.0f, 7000.0f }; // m, world heights of the displacement

	Vector2i origin;
	bool heights_valid = false;
	bool derived = false;
	bool dirty = true;
	Vector<float> heights; // n * n, NaN where nothing answered
	Vector<float> pending;
	Vector2i pending_origin;
	int pending_row = -1;

	// What the field was derived for.
	Vector2 wind; // m/s, x and z
	float buoyancy = 0.0f; // N, 1/s
	float ground = 0.0f; // m, the square's mean terrain height
	float peak = 0.0f; // m, the highest terrain in the square
	Vector<float> eta; // n * n * LEVELS, vertical displacement (m) at each level, level fastest
	float eta_max[LEVELS] = {}; // largest upward displacement at each level
	float derive_ms = 0.0f;

	float cell_size() const { return extent / resolution; }
	Rect2 rect_of(Vector2i p_origin) const;
	Rect2 rect() const { return rect_of(origin); }
	bool needs_heights(Vector2 p_center) const;
	void begin_heights(Vector2 p_center);
	bool is_sampling() const { return pending_row >= 0; }
	Vector2 pending_cell_center(int p_x, int p_z) const;
	void finish_heights();
	// Derive again when the wind has turned by about 5 degrees or changed speed by a tenth, or N by a tenth.
	bool needs_derive(Vector2 p_wind, float p_buoyancy) const;
	void derive(Vector2 p_wind, float p_buoyancy);
	// The displacement (m) at a world point, bilinear across the square and linear between the levels (0 outside).
	float eta_at(const Vector3 &p_position) const;

	// In-place radix-2 complex FFT of length p_n (a power of two), stride p_stride; p_inverse without the 1/n.
	static void fft(float *p_re, float *p_im, int p_n, int p_stride, bool p_inverse);
};

// Shared with WeatherClouds::State and the sky: the displacement at a point from the packed field.
float mountain_wave_eta(const Vector<float> &p_eta, int p_n, const Vector2 &p_corner, float p_size, const float *p_levels, const Vector3 &p_position);

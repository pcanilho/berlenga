/**************************************************************************/
/*  weather_terrain_wind.h                                                */
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

#include "core/io/image.h"
#include "core/math/rect2.h"
#include "core/templates/vector.h"
#include "scene/resources/image_texture.h"

// Wind over terrain (docs/ideas/wind.md): the ground's shape steers and scales the mean wind on a square
// around the camera, after MicroMet (Liston and Elder 2006): a speed weighting from the slope along the wind
// and the curvature, W = 1 + gs Ws + gc Wc, and a diverting angle that turns the wind along the contours,
// -0.5 slope sin(2 (aspect - direction)). The lee of upwind ridges (Winstral, Marks and Gurney 2002, Sx)
// slows it further, and near the ground the air follows the surface (an updraught on windward slopes).
// The effect fades with height over the curvature length scale, as the inner layer of flow over hills
// (Jackson and Hunt 1975). WeatherMap3D owns one and applies it in get_wind_at() and weather_wind().
//
// Per cell (RGBA half floats, weather_terrain_map): the speed weighting W, the diverting angle (rad), the
// ground's rise along the turned wind (dz/ds) and the ground height less the square's reference height.
class WeatherTerrainWind {
public:
	float extent = 2048.0f;
	int resolution = 256;
	float curvature_length = 40.0f; // m, MicroMet's curvature length scale (half a ridge's wavelength)
	float slope_weight = 0.5f; // gs
	float curvature_weight = 0.5f; // gc
	float lee_reach = 100.0f; // m, Sx search distance upwind (Winstral et al. 2002: 100 m)
	float lee_strength = 0.4f; // speed lost deep in a ridge's lee (Sx past 25 degrees)
	float floor = -1.0e9f; // the ground never counts below this (a sea surface)

	Vector2i origin; // cell index of cell (0, 0)
	bool valid = false; // cells hold a derived field
	bool heights_valid = false;
	bool derived = false; // derive() ran on the current heights (valid stays false when no height answered)
	bool dirty = true;
	Vector2 baked_direction;
	float reference = 0.0f; // m, subtracted from the ground in the texture
	Vector<float> heights; // n * n, NaN where nothing answered
	Vector<float> cells; // 4 per cell
	Vector<float> lee; // n * n, Sx (rad) for the baked direction

	// Heights come in over several ticks.
	Vector<float> pending;
	Vector2i pending_origin;
	int pending_row = -1;

	float derive_ms = 0.0f;
	Ref<Image> image;
	Ref<ImageTexture> texture;

	float cell_size() const { return extent / resolution; }
	// Height over which the terrain's effect fades (the hills' outer layer, about their half length).
	float fade_length() const { return 2.0f * curvature_length; }
	Rect2 rect() const;
	Rect2 rect_of(Vector2i p_origin) const;
	// The camera has moved an eighth of the square from its centre (or nothing is sampled yet).
	bool needs_heights(Vector2 p_center) const;
	void begin_heights(Vector2 p_center);
	bool is_sampling() const { return pending_row >= 0; }
	// World x, z of a pending cell's centre.
	Vector2 pending_cell_center(int p_x, int p_z) const;
	// Takes the finished pending heights.
	void finish_heights();
	bool needs_derive(Vector2 p_direction) const;
	void derive(Vector2 p_direction);
	// The four values at a world point (bilinear); outside: W 1, no turn, flat, NaN ground.
	void sample(Vector2 p_xz, float r_cell[4]) const;
	float sample_lee(Vector2 p_xz) const;
	// The wind over terrain: p_mean is the horizontal mean wind (x, z) at height p_h above the ground, as it
	// would be on flat ground; returns it turned and scaled, with the vertical part, as (x, y, z).
	static Vector3 apply(const float p_cell[4], Vector2 p_mean, float p_h, float p_length);
	// GLSL for the same: weather_terrain(xz) and weather_terrain_wind(cell, mean, h); needs the globals
	// weather_terrain_map, weather_terrain_rect and weather_terrain_info.
	static String get_shader_code();
};

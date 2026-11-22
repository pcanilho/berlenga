/**************************************************************************/
/*  weather_shelter.h                                                     */
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

#include "core/io/image.h"
#include "core/math/aabb.h"
#include "core/math/rect2.h"
#include "core/templates/local_vector.h"
#include "scene/resources/image_texture.h"

// Wind near the ground: how much plant canopies and buildings shelter it, on a fine square around the
// camera. WeatherMap3D owns one and applies it in get_wind_at() and, through weather_shelter_map, in
// shaders. See docs/ideas/weather.md, "Wind near the ground".
//
// Per cell (RGBA half floats): canopy height hc (m), Cionco's attenuation coefficient a, the lee factor of
// upwind obstacles s (0..1), and the height of the obstacle that sets it (m).
class WeatherShelter {
public:
	struct Canopy {
		int id = 0;
		Vector2 center;
		float radius = 10.0f;
		float height = 15.0f;
		float density = 2.5f;
	};
	struct Obstacle {
		int id = 0;
		AABB box;
	};

	// Wake of a solid obstacle of height H at distance x downwind: a recirculating cavity to about 2 H
	// (Hosker 1984), then recovery over about 6 H, as measured behind dense windbreaks (Heisler and
	// DeWalle 1988: about 0.6 to 0.8 of the open wind at 10 H, nearly all of it by 20 H).
	static constexpr float LEE_MIN = 0.15f;
	static constexpr float LEE_CAVITY = 2.0f;
	static constexpr float LEE_RECOVERY = 6.0f;
	static constexpr float LEE_REACH = 25.0f;

	float extent = 512.0f;
	int resolution = 256;
	float ground_height = 0.0f;

	Vector<Canopy> canopies;
	Vector<Obstacle> obstacles;
	int next_id = 1;
	Ref<Image> canopy_image;
	Rect2 canopy_rect = Rect2(-256, -256, 512, 512);
	float canopy_max_height = 30.0f;

	Vector2i origin; // cell index of cell (0, 0)
	bool valid = false;
	bool dirty = true;
	Vector2 baked_direction;
	Vector<float> cells; // 4 per cell
	float bake_ms = 0.0f;

	Ref<Image> image;
	Ref<ImageTexture> texture;

	float cell_size() const { return extent / resolution; }
	Rect2 rect() const;
	bool needs_bake(Vector2 p_center, Vector2 p_direction) const;
	// p_extra: obstacles found at bake time (nodes in the weather_shelter group), world AABBs.
	void bake(Vector2 p_center, Vector2 p_direction, const LocalVector<AABB> &p_extra);
	// The four values at a world point (bilinear, as the texture is read); outside the square, no shelter.
	void sample(Vector2 p_xz, float r_cell[4]) const;
	// Mean wind at height p_z over the wind on open ground of roughness p_z0 at the same height: the canopy
	// profile and the obstacles' lee, 0..1.
	static float factor(float p_z, float p_z0, const float p_cell[4]);
	// The open-ground log law at p_z over the 10 m wind.
	static float open_profile(float p_z, float p_z0);
};

// Gust patches: blobs of a few metres of faster and slower air carried across fields and water (honami
// over crops, cat's paws over water). Value noise on integer cells with a 1024-cell period, the same bits
// in C++ and in the shader code.
namespace WeatherPatches {
static constexpr int PERIOD = 1024;
static constexpr float NOISE_STD = 0.158f; // standard deviation of the two-octave noise below
float noise(Vector2 p_cells);
} // namespace WeatherPatches

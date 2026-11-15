/**************************************************************************/
/*  weather_contact_wetness.h                                             */
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
#include "core/math/vector3.h"
#include "core/templates/vector.h"
#include "scene/resources/image_texture.h"

// Wet where water has been (docs/ideas/water.md, "Wet where water has been"): a fine square of
// wetness that follows the camera, after the film-industry wet map (Houdini, Stoke) and FluidNinja's
// faded wet map. Each texel keeps the most water that touched it (max), then dries toward 0 over a
// drying time the weather sets. Published as the global weather_contact_map (R8) and
// weather_contact_rect (x, z, size, size), which the wetness readers combine with the rain's.
class WeatherContactWetness {
	int resolution = 0;
	float size = 0.0f; // m, the square's side
	Vector2i origin; // texel index (in world texels) of texel (0, 0)
	bool origin_valid = false;
	Vector<float> wet;
	bool dirty = false; // changed since the last upload
	bool any_wet = false; // some texel above 0 (drying has work to do)
	Ref<Image> image;
	Ref<ImageTexture> texture;

	float _texel() const { return size / resolution; }

public:
	void configure(int p_resolution, float p_size);
	int get_resolution() const { return resolution; }
	float get_size() const { return size; }

	// Keeps the square centred on p_center (moves in whole texels, keeping what overlaps).
	void follow(Vector3 p_center);
	// The square in world XZ.
	Rect2 rect() const;

	// Wets a disc (amount 0..1, max with what is there), fading over its outer quarter.
	void add_disc(Vector3 p_position, float p_radius, float p_amount);
	// Wets from a grid of p_width x p_height values (0..1) over p_rect in world XZ (bilinear).
	void add_grid(const float *p_values, int p_width, int p_height, Rect2 p_rect);
	// Dries every texel by exp(-dt / dry_time).
	void dry(float p_seconds, float p_dry_time);
	float sample(Vector3 p_position) const;

	// Uploads the texels when they changed. Returns the texture (null until configured).
	Ref<ImageTexture> upload();
	Ref<ImageTexture> get_texture() const { return texture; }
};

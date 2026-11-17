/**************************************************************************/
/*  water_contact_map.h                                                   */
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

#include "core/math/vector2.h"
#include "core/templates/local_vector.h"
#include "core/templates/vector.h"

class PhysicsDirectSpaceState3D;

// Rock contact foam's map (docs/ideas/water.md "Rock contact foam"): a top-down height of the static bodies over a
// square round the camera, measured by rays (a coarse pass, then the full resolution only near the waterline) and
// spread over physics ticks, then turned into the horizontal distance to the nearest place that stands out of the
// water (an exact Euclidean distance transform carrying the nearest seed), the steepness of the face there, and
// the wash over rock tops just under the surface.
class WaterContactMap {
public:
	static constexpr float RANGE = 8.0f; // metres of distance the map's red channel spans

	// Starts a measure of the square of p_extent metres at p_center (world xz), p_resolution cells a side, for
	// water at p_level (world y), washing over tops up to p_depth under it.
	void begin(const Vector2 &p_center, float p_extent, int p_resolution, float p_level, float p_depth, uint32_t p_mask);
	// Casts up to p_budget rays; true once the map is finished (its pixels are then ready).
	bool step(PhysicsDirectSpaceState3D *p_space, int p_budget);
	bool is_busy() const { return phase != PHASE_IDLE; }
	void cancel() { phase = PHASE_IDLE; }

	// RGBA8, resolution^2, x along u and z along v: r = distance to the contact / RANGE, g = the face's steepness
	// there (0 beach .. 1 rock face), b = wash over a rock top under the surface, a = 1.
	const Vector<uint8_t> &get_pixels() const { return pixels; }
	Vector2 get_center() const { return center; }
	float get_extent() const { return extent; }
	int get_resolution() const { return resolution; }
	int get_last_ray_count() const { return last_rays; }
	int get_contact_cells() const { return contact_cells; }

private:
	enum Phase {
		PHASE_IDLE,
		PHASE_COARSE,
		PHASE_FINE,
	};
	static constexpr int STRIDE = 4; // fine cells per coarse cell
	static constexpr float NONE = -1e9f; // nothing within reach under the ray

	Phase phase = PHASE_IDLE;
	Vector2 center;
	float extent = 96.0f;
	int resolution = 384;
	float level = 0.0f;
	float depth = 0.6f;
	uint32_t mask = 0xFFFFFFFF;
	int cursor = 0;
	int rays = 0;
	int last_rays = 0;
	int contact_cells = 0;
	LocalVector<float> coarse;
	LocalVector<int> fine_blocks; // coarse cells measured at full resolution
	LocalVector<float> height;
	Vector<uint8_t> pixels;

	float _cast(PhysicsDirectSpaceState3D *p_space, const Vector2 &p_xz);
	void _plan_fine();
	void _finish();
};

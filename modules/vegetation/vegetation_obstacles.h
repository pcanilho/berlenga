/**************************************************************************/
/*  vegetation_obstacles.h                                                */
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
#include "core/math/vector2i.h"
#include "core/math/vector4.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/variant/variant.h"

// Rocks and other solid things standing on the ground that plants cannot grow through: boulders
// (TerrainGenerator3D.get_boulders()), stones, posts. Each is a disc on the ground, (x, ground y,
// z, radius), radius where the rock meets the soil. A grid of 4 m cells finds the ones near a point.
struct VegetationObstacles {
	static constexpr float CELL = 4.0f;
	LocalVector<Vector4> items;
	HashMap<Vector2i, LocalVector<uint32_t>> grid;
	float max_radius = 0.0f;

	void build(const PackedVector4Array &p_items) {
		items.clear();
		grid.clear();
		max_radius = 0.0f;
		for (int i = 0; i < p_items.size(); i++) {
			const Vector4 o = p_items[i];
			if (o.w <= 0.0f) {
				continue;
			}
			items.push_back(o);
			max_radius = MAX(max_radius, float(o.w));
			const Vector2i c(int(Math::floor(o.x / CELL)), int(Math::floor(o.z / CELL)));
			grid[c].push_back(items.size() - 1);
		}
	}

	bool is_empty() const { return items.is_empty(); }

	// The distance from a point on the ground to the nearest obstacle's edge (negative inside it),
	// looked for out to p_reach past the edges; p_reach + 1 when none is that near.
	float clearance(const Vector2 &p_xz, float p_reach) const {
		float best = p_reach + 1.0f;
		if (items.is_empty()) {
			return best;
		}
		const int r = int(Math::ceil((max_radius + p_reach) / CELL));
		const Vector2i c(int(Math::floor(p_xz.x / CELL)), int(Math::floor(p_xz.y / CELL)));
		for (int dz = -r; dz <= r; dz++) {
			for (int dx = -r; dx <= r; dx++) {
				const LocalVector<uint32_t> *bucket = grid.getptr(c + Vector2i(dx, dz));
				if (!bucket) {
					continue;
				}
				for (uint32_t i : *bucket) {
					const Vector4 &o = items[i];
					const float d = Vector2(float(o.x) - p_xz.x, float(o.z) - p_xz.y).length() - float(o.w);
					best = MIN(best, d);
				}
			}
		}
		return best;
	}

	// Whether a point lies within p_margin of an obstacle.
	bool blocks(const Vector2 &p_xz, float p_margin) const {
		return !items.is_empty() && clearance(p_xz, p_margin) < p_margin;
	}
};

/**************************************************************************/
/*  vegetation_paths.h                                                    */
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

#include "core/math/math_funcs.h"
#include "core/math/rect2.h"
#include "core/math/vector2.h"
#include "core/math/vector2i.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"

// Worn paths (docs/ideas/vegetation.md, "Worn paths and trampling"): lines people and animals walk
// along, each with its trodden width and its passes a year. Walkers spread about the line in a
// bell (Cole and Bayfield 1993 measure trampling as passes across a lane; Liddle 1997, ch. 3, the
// spread of feet about a path's centre line), so the passes a point gets fall off as a Gaussian of
// its distance, sigma a quarter of the trodden width. The fields thin and shorten their plants
// with the passes (PlantSpecies.trample_resistance). Each path is a Dictionary: "points"
// (PackedVector3Array, world; y unused), "width" (m), "passes" (a year). A grid of 8 m cells finds
// the segments near a point.
struct VegetationPaths {
	static constexpr float CELL = 8.0f;
	struct Segment {
		Vector2 a, b;
		float sigma = 0.5f;
		float passes = 0.0f;
	};
	LocalVector<Segment> segments;
	HashMap<Vector2i, LocalVector<uint32_t>> grid;
	float max_reach = 0.0f;

	void build(const Array &p_paths) {
		segments.clear();
		grid.clear();
		max_reach = 0.0f;
		for (int i = 0; i < p_paths.size(); i++) {
			const Dictionary d = p_paths[i];
			const PackedVector3Array pts = d.get("points", PackedVector3Array());
			const float width = MAX(float(d.get("width", 1.5f)), 0.05f);
			const float passes = MAX(float(d.get("passes", 0.0f)), 0.0f);
			if (pts.size() < 2 || passes <= 0.0f) {
				continue;
			}
			const float sigma = width * 0.25f;
			const float reach = sigma * 3.5f; // exp(-3.5^2 / 2): under 0.3% of the passes.
			max_reach = MAX(max_reach, reach);
			for (int k = 0; k + 1 < pts.size(); k++) {
				Segment s;
				s.a = Vector2(pts[k].x, pts[k].z);
				s.b = Vector2(pts[k + 1].x, pts[k + 1].z);
				s.sigma = sigma;
				s.passes = passes;
				segments.push_back(s);
				const uint32_t idx = segments.size() - 1;
				const Vector2 lo = s.a.min(s.b) - Vector2(reach, reach);
				const Vector2 hi = s.a.max(s.b) + Vector2(reach, reach);
				for (int z = int(Math::floor(lo.y / CELL)); z <= int(Math::floor(hi.y / CELL)); z++) {
					for (int x = int(Math::floor(lo.x / CELL)); x <= int(Math::floor(hi.x / CELL)); x++) {
						grid[Vector2i(x, z)].push_back(idx);
					}
				}
			}
		}
	}

	bool is_empty() const { return segments.is_empty(); }

	// The passes a year a point gets: the most of any path near it (where paths cross, the busier
	// one's walkers set the wear).
	float passes_at(const Vector2 &p_xz) const {
		if (segments.is_empty()) {
			return 0.0f;
		}
		const LocalVector<uint32_t> *bucket = grid.getptr(Vector2i(int(Math::floor(p_xz.x / CELL)), int(Math::floor(p_xz.y / CELL))));
		if (!bucket) {
			return 0.0f;
		}
		float best = 0.0f;
		for (uint32_t i : *bucket) {
			const Segment &s = segments[i];
			const Vector2 ab = s.b - s.a;
			const float len2 = ab.length_squared();
			const float t = len2 > 1e-8f ? CLAMP((p_xz - s.a).dot(ab) / len2, 0.0f, 1.0f) : 0.0f;
			const float d2 = (s.a + ab * t).distance_squared_to(p_xz);
			best = MAX(best, s.passes * Math::exp(-0.5f * d2 / (s.sigma * s.sigma)));
		}
		return best;
	}

	// Whether any path comes within its reach of a rect (a tile skips the lookups when none does).
	bool any_in(const Rect2 &p_rect) const {
		if (segments.is_empty()) {
			return false;
		}
		for (int z = int(Math::floor(p_rect.position.y / CELL)); z <= int(Math::floor(p_rect.get_end().y / CELL)); z++) {
			for (int x = int(Math::floor(p_rect.position.x / CELL)); x <= int(Math::floor(p_rect.get_end().x / CELL)); x++) {
				if (grid.has(Vector2i(x, z))) {
					return true;
				}
			}
		}
		return false;
	}
};

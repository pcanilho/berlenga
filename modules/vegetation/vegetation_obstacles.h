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
#include "core/object/object.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
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

	// Adds more obstacles to the grid (built or empty).
	void add(const PackedVector4Array &p_items) {
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
	// looked for out to p_reach past the edges; p_reach + 1 when none is that near (an edge further
	// than p_reach never counts, so a grid of only the obstacles near the point answers the same). r_away, if
	// given, gets the unit direction from that obstacle's centre to the point.
	float clearance(const Vector2 &p_xz, float p_reach, Vector2 *r_away = nullptr, int *r_index = nullptr) const {
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
					const Vector2 to(p_xz.x - float(o.x), p_xz.y - float(o.z));
					const float d = to.length() - float(o.w);
					if (d < best && d <= p_reach) {
						best = d;
						if (r_index) {
							*r_index = int(i);
						}
						if (r_away) {
							*r_away = to.length_squared() > 1e-12f ? to.normalized() : Vector2(1.0f, 0.0f);
						}
					}
				}
			}
		}
		return best;
	}

	// The distance from a point to the second-nearest obstacle's edge (another obstacle than the
	// nearest), looked for out to p_reach; p_reach + 1 when there is no second one that near. A
	// point close to two stones lies in a gap between them (PlantSpecies.stone_gap).
	float second_clearance(const Vector2 &p_xz, float p_reach, float *r_first = nullptr) const {
		float first = p_reach + 1.0f;
		float second = p_reach + 1.0f;
		if (r_first) {
			*r_first = first;
		}
		if (items.is_empty()) {
			return second;
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
					const float d = Vector2(p_xz.x - float(o.x), p_xz.y - float(o.z)).length() - float(o.w);
					if (d > p_reach) {
						continue; // Only within the reach: the answer is the same whichever grid holds the rocks.
					}
					if (d < first) {
						second = first;
						first = d;
					} else if (d < second) {
						second = d;
					}
				}
			}
		}
		if (r_first) {
			*r_first = first;
		}
		return second;
	}

	// How sheltered a point at height p_y is by taller items (y their top) within p_reach of it:
	// the steepest angle up to a taller item's edge, as tan (0 open .. 1 at 45 degrees), items
	// towards +Z (the sun's side, opposite the -Z pole) counting fully and those to -Z half.
	float shelter(const Vector2 &p_xz, float p_y, float p_reach) const {
		float best = 0.0f;
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
					const float rise = float(o.y) - p_y;
					if (rise <= 0.05f) {
						continue;
					}
					const Vector2 to(float(o.x) - p_xz.x, float(o.z) - p_xz.y);
					const float len = to.length();
					const float d = len - float(o.w);
					if (d > p_reach) {
						continue;
					}
					const float side = len > 1e-4f ? 0.75f + 0.25f * to.y / len : 1.0f;
					best = MAX(best, CLAMP(side * rise / MAX(d, 0.05f), 0.0f, 1.0f));
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

// The rocks of a RockField3D (its get_obstacles()) fetched as the plant fields need them, in square chunks, instead
// of one world-wide array: a field asks for the rocks round the tiles it is about to place, on the main thread
// (RockField3D places its stone cells there), and the chunks are kept. A stone belongs to the chunk its centre is in
// (get_obstacles() keeps the stones whose centre is in its rect), so the chunks hold exactly the stones of a
// world-wide query, each once.
struct VegetationRockCache {
	static constexpr float CHUNK = 32.0f;
	HashMap<Vector2i, PackedVector4Array> chunks;
	float max_radius = 0.0f; // The widest stone fetched so far.
	static Vector2i chunk_of(float p_x, float p_z) { return Vector2i(int(Math::floor(p_x / CHUNK)), int(Math::floor(p_z / CHUNK))); }
	void clear() {
		chunks.clear();
		max_radius = 0.0f;
	}
	// Fetches the chunks over p_rect not fetched yet, with one get_obstacles() call over the rect they span (the
	// rock field places their cells on its workers together). The new stones are appended to r_new when given.
	// True when any chunk was fetched.
	bool fetch(Object *p_rocks, const Rect2 &p_rect, float p_min_size, PackedVector4Array *r_new = nullptr) {
		if (!p_rocks || !p_rect.has_area()) {
			return false;
		}
		const Vector2i c0 = chunk_of(p_rect.position.x, p_rect.position.y);
		const Vector2i c1 = chunk_of(p_rect.get_end().x, p_rect.get_end().y);
		Vector2i lo(INT32_MAX, INT32_MAX), hi(INT32_MIN, INT32_MIN);
		LocalVector<Vector2i> missing;
		for (int z = c0.y; z <= c1.y; z++) {
			for (int x = c0.x; x <= c1.x; x++) {
				const Vector2i k(x, z);
				if (!chunks.has(k)) {
					missing.push_back(k);
					lo = Vector2i(MIN(lo.x, x), MIN(lo.y, z));
					hi = Vector2i(MAX(hi.x, x), MAX(hi.y, z));
				}
			}
		}
		if (missing.is_empty()) {
			return false;
		}
		for (const Vector2i &k : missing) {
			chunks.insert(k, PackedVector4Array());
		}
		const Rect2 span(Vector2(lo) * CHUNK, Vector2(hi - lo + Vector2i(1, 1)) * CHUNK);
		const PackedVector4Array got = p_rocks->call(SNAME("get_obstacles"), span, p_min_size);
		HashSet<Vector2i> wanted;
		for (const Vector2i &k : missing) {
			wanted.insert(k);
		}
		for (int i = 0; i < got.size(); i++) {
			const Vector4 o = got[i];
			const Vector2i k = chunk_of(o.x, o.z);
			if (!wanted.has(k)) {
				continue; // A chunk fetched before (the span covers some).
			}
			chunks[k].push_back(o);
			max_radius = MAX(max_radius, float(o.w));
			if (r_new) {
				r_new->push_back(o);
			}
		}
		return true;
	}
	// The stones of the fetched chunks over p_rect, appended to r_out.
	void gather(const Rect2 &p_rect, PackedVector4Array &r_out) const {
		const Vector2i c0 = chunk_of(p_rect.position.x, p_rect.position.y);
		const Vector2i c1 = chunk_of(p_rect.get_end().x, p_rect.get_end().y);
		for (int z = c0.y; z <= c1.y; z++) {
			for (int x = c0.x; x <= c1.x; x++) {
				if (const PackedVector4Array *c = chunks.getptr(Vector2i(x, z))) {
					r_out.append_array(*c);
				}
			}
		}
	}
	// Every stone fetched, appended to r_out.
	void gather_all(PackedVector4Array &r_out) const {
		for (const KeyValue<Vector2i, PackedVector4Array> &kv : chunks) {
			r_out.append_array(kv.value);
		}
	}
};

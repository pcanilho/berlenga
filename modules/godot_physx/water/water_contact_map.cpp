/**************************************************************************/
/*  water_contact_map.cpp                                                 */
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

#include "water_contact_map.h"

#include "core/math/math_funcs.h"
#include "servers/physics_3d/physics_server_3d.h"

void WaterContactMap::begin(const Vector2 &p_center, float p_extent, int p_resolution, float p_level, float p_depth, uint32_t p_mask) {
	resolution = MAX(p_resolution / STRIDE, 4) * STRIDE;
	center = p_center;
	extent = MAX(p_extent, 1.0f);
	level = p_level;
	depth = MAX(p_depth, 0.0f);
	mask = p_mask;
	const int nc = resolution / STRIDE;
	coarse.resize(nc * nc);
	height.resize(resolution * resolution);
	fine_blocks.clear();
	cursor = 0;
	rays = 0;
	phase = PHASE_COARSE;
}

float WaterContactMap::_cast(PhysicsDirectSpaceState3D *p_space, const Vector2 &p_xz) {
	// Straight down from well above the water to a little under the deepest wash, onto static bodies only: anything
	// else (a floater, a boat, a player) is stepped through, as the seabed's rays do.
	constexpr float ABOVE = 150.0f;
	constexpr int MAX_SKIPS = 4;
	rays++;
	PhysicsDirectSpaceState3D::RayParameters ray;
	ray.from = Vector3(p_xz.x, level + ABOVE, p_xz.y);
	ray.to = Vector3(p_xz.x, level - depth - 1.0f, p_xz.y);
	ray.collision_mask = mask;
	ray.collide_with_areas = false;
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	for (int skip = 0; skip < MAX_SKIPS; skip++) {
		PhysicsDirectSpaceState3D::RayResult result;
		if (!p_space->intersect_ray(ray, result)) {
			return NONE;
		}
		if (ps->body_get_mode(result.rid) == PhysicsServer3D::BODY_MODE_STATIC) {
			return result.position.y;
		}
		ray.exclude.insert(result.rid);
	}
	return NONE;
}

bool WaterContactMap::step(PhysicsDirectSpaceState3D *p_space, int p_budget) {
	if (phase == PHASE_IDLE || p_space == nullptr) {
		return false;
	}
	const int nc = resolution / STRIDE;
	const float cell = extent / resolution;
	const Vector2 origin = center - Vector2(extent, extent) * 0.5f;
	int budget = MAX(p_budget, STRIDE * STRIDE);
	if (phase == PHASE_COARSE) {
		const int total = nc * nc;
		while (cursor < total && budget > 0) {
			const int cx = cursor % nc;
			const int cz = cursor / nc;
			coarse[cursor] = _cast(p_space, origin + Vector2((cx + 0.5f) * STRIDE, (cz + 0.5f) * STRIDE) * cell);
			cursor++;
			budget--;
		}
		if (cursor < total) {
			return false;
		}
		_plan_fine();
		cursor = 0;
		phase = PHASE_FINE;
	}
	while (cursor < (int)fine_blocks.size() && budget > 0) {
		const int b = fine_blocks[cursor];
		const int bx = (b % nc) * STRIDE;
		const int bz = (b / nc) * STRIDE;
		for (int j = 0; j < STRIDE; j++) {
			for (int i = 0; i < STRIDE; i++) {
				height[(bz + j) * resolution + bx + i] = _cast(p_space, origin + Vector2(bx + i + 0.5f, bz + j + 0.5f) * cell);
			}
		}
		budget -= STRIDE * STRIDE;
		cursor++;
	}
	if (cursor < (int)fine_blocks.size()) {
		return false;
	}
	_finish();
	last_rays = rays;
	phase = PHASE_IDLE;
	return true;
}

void WaterContactMap::_plan_fine() {
	// Every fine cell starts as its coarse cell; a coarse cell is measured again in full where the waterline may
	// pass through it or a rock top lies within the wash: something within reach in its 3x3 neighbourhood that
	// is not all high and dry.
	const int nc = resolution / STRIDE;
	for (int z = 0; z < resolution; z++) {
		for (int x = 0; x < resolution; x++) {
			height[z * resolution + x] = coarse[(z / STRIDE) * nc + x / STRIDE];
		}
	}
	const float dry = level + 1.0f;
	for (int cz = 0; cz < nc; cz++) {
		for (int cx = 0; cx < nc; cx++) {
			bool something = false;
			bool wet = false;
			for (int dz = -1; dz <= 1; dz++) {
				for (int dx = -1; dx <= 1; dx++) {
					const int x = CLAMP(cx + dx, 0, nc - 1);
					const int z = CLAMP(cz + dz, 0, nc - 1);
					const float h = coarse[z * nc + x];
					something = something || h != NONE;
					wet = wet || h < dry;
				}
			}
			if (something && wet) {
				fine_blocks.push_back(cz * nc + cx);
			}
		}
	}
}

void WaterContactMap::_finish() {
	const int n = resolution;
	const float cell = extent / n;
	// The deepest a cell can read: nothing under the ray stands for the bottom of its reach.
	const float floor_h = level - depth - 1.0f;
	LocalVector<float> h;
	h.resize(n * n);
	for (int i = 0; i < n * n; i++) {
		h[i] = height[i] == NONE ? floor_h : height[i];
	}
	// Steepness round a cell: the steepest drop from it (its height, capped a metre over the water so a tall stack
	// reads like any other face) to the cells within two, as a slope. A beach (1:10) gives ~0.1, a rock face several.
	auto steepness = [&](int x, int z) -> float {
		const float top = MIN(h[z * n + x], level + 1.0f);
		float s = 0.0f;
		for (int dz = -2; dz <= 2; dz++) {
			for (int dx = -2; dx <= 2; dx++) {
				if (dx == 0 && dz == 0) {
					continue;
				}
				const int xx = x + dx;
				const int zz = z + dz;
				if (xx < 0 || zz < 0 || xx >= n || zz >= n) {
					continue;
				}
				s = MAX(s, (top - h[zz * n + xx]) / (cell * Math::sqrt(float(dx * dx + dz * dz))));
			}
		}
		// Battjes' surf similarity: spilling on gentle slopes, surging and collapsing on steep ones.
		const float t = CLAMP((s - 0.3f) / (1.0f - 0.3f), 0.0f, 1.0f);
		return t * t * (3.0f - 2.0f * t);
	};

	// Exact Euclidean distance transform to the cells standing out of the water (Felzenszwalb and Huttenlocher
	// 2012), carrying the nearest seed: columns first, then the lower envelope of parabolas along each row.
	constexpr int FAR = 1 << 20;
	LocalVector<int> gy; // nearest seed's z in this column, or -1
	gy.resize(n * n);
	for (int x = 0; x < n; x++) {
		int last = -1;
		for (int z = 0; z < n; z++) {
			if (h[z * n + x] >= level) {
				last = z;
			}
			gy[z * n + x] = last;
		}
		last = -1;
		for (int z = n - 1; z >= 0; z--) {
			if (h[z * n + x] >= level) {
				last = z;
			}
			const int up = gy[z * n + x];
			if (last >= 0 && (up < 0 || last - z < z - up)) {
				gy[z * n + x] = last;
			}
		}
	}
	LocalVector<int> seed; // nearest seed's index, or -1
	seed.resize(n * n);
	LocalVector<int> v;
	LocalVector<float> zb;
	v.resize(n);
	zb.resize(n + 1);
	for (int z = 0; z < n; z++) {
		auto f = [&](int x) -> float {
			const int s = gy[z * n + x];
			if (s < 0) {
				return float(FAR);
			}
			return float((s - z) * (s - z));
		};
		int k = -1;
		for (int q = 0; q < n; q++) {
			const float fq = f(q);
			if (fq >= float(FAR)) {
				continue;
			}
			float s = 0.0f;
			while (k >= 0) {
				const int p = v[k];
				s = ((fq + q * q) - (f(p) + p * p)) / float(2 * q - 2 * p);
				if (s > zb[k]) {
					break;
				}
				k--;
			}
			k++;
			v[k] = q;
			zb[k] = k == 0 ? -1e30f : s;
			zb[k + 1] = 1e30f;
		}
		int j = 0;
		for (int x = 0; x < n; x++) {
			if (k < 0) {
				seed[z * n + x] = -1;
				continue;
			}
			while (j < k && zb[j + 1] < float(x)) {
				j++;
			}
			seed[z * n + x] = gy[z * n + v[j]] * n + v[j];
		}
	}

	pixels.resize(n * n * 4);
	uint8_t *w = pixels.ptrw();
	LocalVector<float> seed_steep;
	seed_steep.resize(n * n);
	for (int i = 0; i < n * n; i++) {
		seed_steep[i] = -1.0f;
	}
	contact_cells = 0;
	for (int z = 0; z < n; z++) {
		for (int x = 0; x < n; x++) {
			const int i = z * n + x;
			float r = 1.0f;
			float g = 0.0f;
			float b = 0.0f;
			const int s = seed[i];
			if (s >= 0) {
				const int sx = s % n;
				const int sz = s / n;
				r = MIN(Math::sqrt(float((sx - x) * (sx - x) + (sz - z) * (sz - z))) * cell / RANGE, 1.0f);
				if (r < 1.0f) {
					if (seed_steep[s] < 0.0f) {
						seed_steep[s] = steepness(sx, sz);
					}
					g = seed_steep[s];
				}
			}
			const float hi = h[i];
			if (hi < level && hi > level - depth && depth > 0.0f) {
				// A rock top under the surface: washes white as crests pass, fading with its depth.
				b = (1.0f - (level - hi) / depth) * steepness(x, z);
			}
			if (g > 0.0f && r < 1.0f && hi < level) {
				contact_cells++;
			}
			w[i * 4 + 0] = (uint8_t)Math::round(CLAMP(r, 0.0f, 1.0f) * 255.0f);
			w[i * 4 + 1] = (uint8_t)Math::round(CLAMP(g, 0.0f, 1.0f) * 255.0f);
			w[i * 4 + 2] = (uint8_t)Math::round(CLAMP(b, 0.0f, 1.0f) * 255.0f);
			w[i * 4 + 3] = 255;
		}
	}
}

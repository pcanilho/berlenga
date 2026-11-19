/**************************************************************************/
/*  desert_springs.cpp                                                    */
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

#include "desert_springs.h"

#include "core/math/math_funcs.h"
#include "core/templates/sort_array.h"

void DesertSprings::clear() {
	wet.clear();
	springs.clear();
	discharge.clear();
	nx = 0;
	ny = 0;
}

// Box mean over a radius in cells, by prefix sums along x then z.
static void _box_mean(const LocalVector<float> &p_in, LocalVector<float> &r_out, int p_nx, int p_ny, int p_r) {
	LocalVector<float> tmp;
	tmp.resize(p_nx * p_ny);
	r_out.resize(p_nx * p_ny);
	LocalVector<double> pre;
	pre.resize(MAX(p_nx, p_ny) + 1);
	for (int z = 0; z < p_ny; z++) {
		pre[0] = 0.0;
		for (int x = 0; x < p_nx; x++) {
			pre[x + 1] = pre[x] + p_in[z * p_nx + x];
		}
		for (int x = 0; x < p_nx; x++) {
			const int a = MAX(x - p_r, 0), b = MIN(x + p_r, p_nx - 1);
			tmp[z * p_nx + x] = float((pre[b + 1] - pre[a]) / (b - a + 1));
		}
	}
	for (int x = 0; x < p_nx; x++) {
		pre[0] = 0.0;
		for (int z = 0; z < p_ny; z++) {
			pre[z + 1] = pre[z] + tmp[z * p_nx + x];
		}
		for (int z = 0; z < p_ny; z++) {
			const int a = MAX(z - p_r, 0), b = MIN(z + p_r, p_ny - 1);
			r_out[z * p_nx + x] = float((pre[b + 1] - pre[a]) / (b - a + 1));
		}
	}
}

void DesertSprings::build(const TerrainGenerator3D *p_gen) {
	clear();
	if (!p_gen || !p_gen->get_sand_sea_rect().has_area()) {
		return;
	}
	rect = p_gen->get_sand_sea_rect();
	const float cell = MAX(MAX(float(p_gen->get_sand_sea_scale()), 4.0f), MAX(rect.size.x, rect.size.y) / 511.0f);
	nx = CLAMP(int(Math::ceil(rect.size.x / cell)) + 1, 2, 512);
	ny = CLAMP(int(Math::ceil(rect.size.y / cell)) + 1, 2, 512);
	const float cx = rect.size.x / (nx - 1), cz = rect.size.y / (ny - 1);
	LocalVector<float> h, depth;
	h.resize(nx * ny);
	depth.resize(nx * ny);
	bool any = false;
	for (int z = 0; z < ny; z++) {
		for (int x = 0; x < nx; x++) {
			const Vector3 p(rect.position.x + cx * x, 0.0f, rect.position.y + cz * z);
			h[z * nx + x] = float(p_gen->get_height(p));
			depth[z * nx + x] = float(p_gen->get_sand_sea_height(p));
			any = any || depth[z * nx + x] > 0.05f;
		}
	}
	if (!any) {
		clear();
		return;
	}
	// The smoothed surface: three box passes, close to a Gaussian over the window.
	const int r = MAX(1, int(Math::round(WINDOW / (3.0f * MAX(cx, cz)))));
	LocalVector<float> a, b;
	_box_mean(h, a, nx, ny, r);
	_box_mean(a, b, nx, ny, r);
	_box_mean(b, a, nx, ny, r);
	// Candidates: hollows at least MIN_HOLLOW deep, inside the edge, deepest first.
	struct Cand {
		float hollow;
		int i;
		bool operator<(const Cand &p_o) const { return hollow > p_o.hollow; }
	};
	LocalVector<Cand> cands;
	const int ex = int(Math::ceil(EDGE / cx)), ez = int(Math::ceil(EDGE / cz));
	for (int z = ez; z < ny - ez; z++) {
		for (int x = ex; x < nx - ex; x++) {
			const int i = z * nx + x;
			const float hollow = a[i] - h[i];
			if (hollow >= MIN_HOLLOW) {
				cands.push_back({ hollow, i });
			}
		}
	}
	if (!cands.is_empty()) {
		SortArray<Cand> sorter;
		sorter.sort(cands.ptr(), cands.size());
	}
	for (const Cand &c : cands) {
		const Vector2 p(rect.position.x + cx * (c.i % nx), rect.position.y + cz * (c.i / nx));
		bool near = false;
		for (const Vector3 &s : springs) {
			if (p.distance_squared_to(Vector2(s.x, s.z)) < SPACING * SPACING) {
				near = true;
				break;
			}
		}
		if (!near) {
			springs.push_back(Vector3(p.x, h[c.i] + RISE, p.y));
			discharge.push_back(CLAMP(DISCHARGE * c.hollow / MIN_HOLLOW, DISCHARGE_MIN, DISCHARGE_MAX));
		}
	}
	// Wetness: the best of the springs within reach, by the ground's height over its water and the distance.
	wet.resize(nx * ny);
	for (int z = 0; z < ny; z++) {
		for (int x = 0; x < nx; x++) {
			const int i = z * nx + x;
			const Vector2 p(rect.position.x + cx * x, rect.position.y + cz * z);
			// Off the sand (its thin edge and bare patches inside count, but the world past the edge does not).
			const float edge = MIN(MIN(p.x - rect.position.x, rect.position.x + rect.size.x - p.x), MIN(p.y - rect.position.y, rect.position.y + rect.size.y - p.y));
			float w = edge < 0.0f ? -1.0f : 0.0f;
			if (w >= 0.0f) {
				for (const Vector3 &s : springs) {
					const float d = p.distance_to(Vector2(s.x, s.z));
					if (d >= REACH) {
						continue;
					}
					const float above = h[i] - s.y;
					const float lift = 1.0f - Math::smoothstep(0.0f, ROOT_REACH, above);
					w = MAX(w, lift * (1.0f - Math::smoothstep(0.6f * REACH, REACH, d)));
				}
			}
			wet[i] = w;
		}
	}
}

float DesertSprings::at(const Vector2 &p_xz) const {
	if (wet.is_empty()) {
		return -1.0f;
	}
	const float gx = (p_xz.x - rect.position.x) / rect.size.x * (nx - 1);
	const float gz = (p_xz.y - rect.position.y) / rect.size.y * (ny - 1);
	if (gx < 0.0f || gz < 0.0f || gx > nx - 1 || gz > ny - 1) {
		return -1.0f;
	}
	const int ix = MIN(int(gx), nx - 2), iz = MIN(int(gz), ny - 2);
	const float fx = gx - ix, fz = gz - iz;
	const float *r0 = &wet[iz * nx + ix];
	const float *r1 = r0 + nx;
	return Math::lerp(Math::lerp(r0[0], r0[1], fx), Math::lerp(r1[0], r1[1], fx), fz);
}

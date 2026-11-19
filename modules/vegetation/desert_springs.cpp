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
	// The springs are the terrain's (TerrainGenerator3D.get_springs(): its lowest interdune floors, the hollows it
	// carves for the water system's pools), so the oasis plants, the pools and the carved ground share one list.
	const PackedVector4Array found = p_gen->get_springs();
	for (int k = 0; k < found.size(); k++) {
		const Vector4 q = found[k];
		springs.push_back(Vector3(q.x, q.y, q.z));
		discharge.push_back(q.w); // L/s; 0: the pools' default.
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

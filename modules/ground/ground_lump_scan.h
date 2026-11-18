/**************************************************************************/
/*  ground_lump_scan.h                                                    */
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

#include "core/math/rect2.h"
#include "core/math/vector3.h"
#include "core/templates/local_vector.h"
#include "ground_deform_field.h"

// Watches a print field's rim in a window round the camera for the lumps it sheds (SnowClumps3D, GroundClods3D;
// docs/ideas/ground-detail.md): which cells' rims grew since the last look (snow or soil heaved up and out of a
// print), and which cells changed at all (a print over a lump crushes it). Reads the field's texels directly, so a
// window of tens of thousands of cells costs tens of microseconds on the main thread.
class GroundLumpScan {
public:
	struct Grown {
		int index = 0; // into owed
		Vector3 center; // the cell's centre (y 0)
		float grew = 0.0f; // m of new rim
		Vector2 out; // away from the print: down the dent's slope
	};

	float cell = 0.0f;
	int n = 0;
	Vector2i origin = Vector2i(INT32_MIN, INT32_MIN);
	LocalVector<float> rim;
	LocalVector<float> dent;
	LocalVector<float> owed; // m3 pushed into the rim and not yet shed, for the owner
	LocalVector<uint8_t> changed; // rim or dent moved by a millimetre or more at the last update

	// A texel of p_values (the field's rim or dent) nearest p_x, p_z; 0 outside the field.
	static float texel_at(const GroundDeformField &p_field, const LocalVector<float> &p_values, float p_x, float p_z) {
		const int res = p_field.resolution;
		if (!p_field.valid || int(p_values.size()) != res * res) {
			return 0.0f;
		}
		const float t = p_field.texel();
		const int x = int(Math::floor(p_x / t)) - p_field.origin.x;
		const int z = int(Math::floor(p_z / t)) - p_field.origin.y;
		if (x < 0 || z < 0 || x >= res || z >= res) {
			return 0.0f;
		}
		return p_values[z * res + x];
	}

	// The finer field inside p_fine_rect (a near field), else p_field.
	const GroundDeformField *main_field = nullptr;
	const GroundDeformField *fine_field = nullptr;
	Rect2 fine_rect;

	float rim_at(float p_x, float p_z) const {
		const GroundDeformField *f = (fine_field && fine_rect.has_point(Vector2(p_x, p_z))) ? fine_field : main_field;
		return f ? texel_at(*f, f->rim, p_x, p_z) : 0.0f;
	}
	float dent_at(float p_x, float p_z) const {
		const GroundDeformField *f = (fine_field && fine_rect.has_point(Vector2(p_x, p_z))) ? fine_field : main_field;
		return f ? texel_at(*f, f->dent, p_x, p_z) : 0.0f;
	}
	float relief_at(const Vector3 &p_at) const { return rim_at(p_at.x, p_at.z) - dent_at(p_at.x, p_at.z); }

	int index_of(const Vector3 &p_at) const {
		if (n <= 0 || cell <= 0.0f) {
			return -1;
		}
		const int x = int(Math::floor(p_at.x / cell)) - origin.x;
		const int z = int(Math::floor(p_at.z / cell)) - origin.y;
		return (x < 0 || z < 0 || x >= n || z >= n) ? -1 : z * n + x;
	}
	bool changed_at(const Vector3 &p_at) const {
		const int i = index_of(p_at);
		return i >= 0 && changed[i];
	}

	void clear() {
		n = 0;
		origin = Vector2i(INT32_MIN, INT32_MIN);
		rim.clear();
		dent.clear();
		owed.clear();
		changed.clear();
	}

	// Looks at the window of p_radius round p_center (cells of p_cell) and lists the cells whose rim grew more than
	// p_step since the last look. Cells new to the window start from what is there (no growth).
	void update(const Vector3 &p_center, float p_radius, float p_cell, float p_step, LocalVector<Grown> &r_grown) {
		r_grown.clear();
		if (!main_field || !main_field->valid) {
			return;
		}
		const int n2 = int(Math::ceil(2.0f * p_radius / p_cell));
		const Vector2i o2(int(Math::floor((p_center.x - p_radius) / p_cell)), int(Math::floor((p_center.z - p_radius) / p_cell)));
		const bool fresh = n2 != n || p_cell != cell;
		if (fresh || o2 != origin) {
			LocalVector<float> rim2, dent2, owed2;
			rim2.resize(n2 * n2);
			dent2.resize(n2 * n2);
			owed2.resize(n2 * n2);
			for (int z = 0; z < n2; z++) {
				for (int x = 0; x < n2; x++) {
					const int ox = x + o2.x - origin.x, oz = z + o2.y - origin.y;
					const int i = z * n2 + x;
					if (!fresh && ox >= 0 && oz >= 0 && ox < n && oz < n) {
						const int j = oz * n + ox;
						rim2[i] = rim[j];
						dent2[i] = dent[j];
						owed2[i] = owed[j];
					} else {
						const float cx = (o2.x + x + 0.5f) * p_cell, cz = (o2.y + z + 0.5f) * p_cell;
						rim2[i] = rim_at(cx, cz);
						dent2[i] = dent_at(cx, cz);
						owed2[i] = 0.0f;
					}
				}
			}
			rim = rim2;
			dent = dent2;
			owed = owed2;
			n = n2;
			cell = p_cell;
			origin = o2;
			changed.resize(n * n);
		}
		const float r2 = p_radius * p_radius;
		for (int z = 0; z < n; z++) {
			for (int x = 0; x < n; x++) {
				const int i = z * n + x;
				const float cx = (origin.x + x + 0.5f) * cell, cz = (origin.y + z + 0.5f) * cell;
				const float r = rim_at(cx, cz);
				const float d = dent_at(cx, cz);
				const float grew = r - rim[i];
				changed[i] = Math::abs(grew) + Math::abs(d - dent[i]) > 0.001f;
				rim[i] = r;
				dent[i] = d;
				if (grew > p_step && (cx - p_center.x) * (cx - p_center.x) + (cz - p_center.z) * (cz - p_center.z) <= r2) {
					Grown g;
					g.index = i;
					g.center = Vector3(cx, 0.0f, cz);
					g.grew = grew;
					r_grown.push_back(g);
				}
			}
		}
		// Away from the print: down the dent's slope over the neighbouring cells.
		for (Grown &g : r_grown) {
			const int x = g.index % n, z = g.index / n;
			const float dx = dent[z * n + MIN(x + 1, n - 1)] - dent[z * n + MAX(x - 1, 0)];
			const float dz = dent[MIN(z + 1, n - 1) * n + x] - dent[MAX(z - 1, 0) * n + x];
			const Vector2 grad(dx, dz);
			g.out = grad.length() > 1e-5f ? -grad.normalized() : Vector2();
		}
	}
};

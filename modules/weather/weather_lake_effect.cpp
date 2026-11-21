/**************************************************************************/
/*  weather_lake_effect.cpp                                               */
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

#include "weather_lake_effect.h"

#include "core/math/math_funcs.h"

Rect2 WeatherLakeEffect::rect_of(Vector2i p_origin) const {
	const float cs = cell_size();
	return Rect2(p_origin.x * cs, p_origin.y * cs, extent, extent);
}

bool WeatherLakeEffect::needs_water(Vector2 p_center) const {
	if (dirty || !water_valid) {
		return !is_sampling() || dirty;
	}
	const Vector2 c = rect().get_center();
	if (Math::abs(p_center.x - c.x) > extent * 0.125f || Math::abs(p_center.y - c.y) > extent * 0.125f) {
		if (is_sampling()) {
			const Vector2 pc = rect_of(pending_origin).get_center();
			return Math::abs(p_center.x - pc.x) > extent * 0.125f || Math::abs(p_center.y - pc.y) > extent * 0.125f;
		}
		return true;
	}
	return false;
}

void WeatherLakeEffect::begin_water(Vector2 p_center) {
	const int n = resolution;
	const float cs = cell_size();
	pending_origin = Vector2i(int(Math::floor(p_center.x / cs)) - n / 2, int(Math::floor(p_center.y / cs)) - n / 2);
	pending.resize(n * n);
	pending.fill(0.0f);
	pending_row = 0;
	dirty = false;
}

Vector2 WeatherLakeEffect::pending_cell_center(int p_x, int p_z) const {
	const float cs = cell_size();
	return Vector2((pending_origin.x + p_x + 0.5f) * cs, (pending_origin.y + p_z + 0.5f) * cs);
}

void WeatherLakeEffect::finish_water() {
	water = pending;
	origin = pending_origin;
	pending.clear();
	pending_row = -1;
	water_valid = true;
	derived = false;
	any_water = false;
	for (int i = 0; i < water.size(); i++) {
		if (water[i] > 0.0f) {
			any_water = true;
			break;
		}
	}
}

bool WeatherLakeEffect::needs_derive(Vector2 p_wind_dir) const {
	if (!water_valid) {
		return false;
	}
	return !derived || wind_dir.dot(p_wind_dir) < 0.9962f;
}

void WeatherLakeEffect::derive(Vector2 p_wind_dir) {
	const int n = resolution;
	const float cs = cell_size();
	wind_dir = p_wind_dir.length_squared() > 1e-8f ? p_wind_dir.normalized() : Vector2(1, 0);
	field.resize(n * n * 2);
	float *f = field.ptrw();
	const float *w = water.ptr();
	fetch_max = 0.0f;
	// Water beyond the square: what its edge cell holds (a sea running off the edge keeps going), out to 200 km.
	auto water_at = [&](float p_x, float p_z) -> float {
		const float fx = CLAMP(p_x - 0.5f, 0.0f, float(n - 1));
		const float fz = CLAMP(p_z - 0.5f, 0.0f, float(n - 1));
		const int x0 = MIN(int(fx), n - 2);
		const int z0 = MIN(int(fz), n - 2);
		const float tx = fx - x0;
		const float tz = fz - z0;
		const float a = Math::lerp(w[z0 * n + x0], w[z0 * n + x0 + 1], tx);
		const float b = Math::lerp(w[(z0 + 1) * n + x0], w[(z0 + 1) * n + x0 + 1], tx);
		return Math::lerp(a, b, tz);
	};
	const int steps = int(200000.0f / cs);
	for (int z = 0; z < n; z++) {
		for (int x = 0; x < n; x++) {
			float fetch = 0.0f; // km, decayed
			float raw = 0.0f; // km, plain
			for (int s = 1; s <= steps; s++) {
				const float d = (s - 0.5f) * cs;
				const float wv = water_at(x + 0.5f - wind_dir.x * d / cs, z + 0.5f - wind_dir.y * d / cs);
				fetch += wv * cs * 0.001f * Math::exp(-d / DECAY);
				raw += wv * cs * 0.001f;
			}
			f[(z * n + x) * 2] = fetch;
			fetch_max = MAX(fetch_max, raw);
		}
	}
	// The band's axis: where the fetch peaks across the wind (16 km either side).
	const Vector2 across(-wind_dir.y, wind_dir.x);
	const int k = MAX(1, int(16000.0f / cs));
	for (int z = 0; z < n; z++) {
		for (int x = 0; x < n; x++) {
			const float here = f[(z * n + x) * 2];
			float best = here;
			for (int j = -k; j <= k; j++) {
				const int xi = CLAMP(int(Math::round(x + across.x * j)), 0, n - 1);
				const int zi = CLAMP(int(Math::round(z + across.y * j)), 0, n - 1);
				best = MAX(best, f[(zi * n + xi) * 2]);
			}
			f[(z * n + x) * 2 + 1] = best > 1e-3f ? here / best : 0.0f;
		}
	}
	derived = true;
}

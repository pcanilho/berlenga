/**************************************************************************/
/*  weather_mountain_waves.cpp                                            */
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

#include "weather_mountain_waves.h"

#include "core/math/math_funcs.h"
#include "core/os/os.h"
#include "core/templates/local_vector.h"

Rect2 WeatherMountainWaves::rect_of(Vector2i p_origin) const {
	const float cs = cell_size();
	return Rect2(p_origin.x * cs, p_origin.y * cs, extent, extent);
}

bool WeatherMountainWaves::needs_heights(Vector2 p_center) const {
	if (dirty || !heights_valid) {
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

void WeatherMountainWaves::begin_heights(Vector2 p_center) {
	const int n = resolution;
	const float cs = cell_size();
	pending_origin = Vector2i(int(Math::floor(p_center.x / cs)) - n / 2, int(Math::floor(p_center.y / cs)) - n / 2);
	pending.resize(n * n);
	pending.fill(Math::NaN);
	pending_row = 0;
	dirty = false;
}

Vector2 WeatherMountainWaves::pending_cell_center(int p_x, int p_z) const {
	const float cs = cell_size();
	return Vector2((pending_origin.x + p_x + 0.5f) * cs, (pending_origin.y + p_z + 0.5f) * cs);
}

void WeatherMountainWaves::finish_heights() {
	heights = pending;
	origin = pending_origin;
	pending.clear();
	pending_row = -1;
	heights_valid = true;
	derived = false;
}

bool WeatherMountainWaves::needs_derive(Vector2 p_wind, float p_buoyancy) const {
	if (!heights_valid) {
		return false;
	}
	if (!derived) {
		return true;
	}
	const float s0 = wind.length();
	const float s1 = p_wind.length();
	if (Math::abs(s1 - s0) > 0.1f * MAX(s0, 1.0f)) {
		return true;
	}
	if (s0 > 0.1f && s1 > 0.1f && (wind / s0).dot(p_wind / s1) < 0.9962f) {
		return true;
	}
	return Math::abs(p_buoyancy - buoyancy) > 0.1f * MAX(buoyancy, 1e-4f);
}

void WeatherMountainWaves::fft(float *p_re, float *p_im, int p_n, int p_stride, bool p_inverse) {
	// Bit reversal.
	for (int i = 1, j = 0; i < p_n; i++) {
		int bit = p_n >> 1;
		for (; j & bit; bit >>= 1) {
			j ^= bit;
		}
		j ^= bit;
		if (i < j) {
			SWAP(p_re[i * p_stride], p_re[j * p_stride]);
			SWAP(p_im[i * p_stride], p_im[j * p_stride]);
		}
	}
	for (int len = 2; len <= p_n; len <<= 1) {
		const double ang = (p_inverse ? 2.0 : -2.0) * Math::PI / len;
		const double wr = Math::cos(ang);
		const double wi = Math::sin(ang);
		for (int i = 0; i < p_n; i += len) {
			double cr = 1.0;
			double ci = 0.0;
			for (int j = 0; j < len / 2; j++) {
				const int a = (i + j) * p_stride;
				const int b = (i + j + len / 2) * p_stride;
				const float ur = p_re[a];
				const float ui = p_im[a];
				const float vr = float(p_re[b] * cr - p_im[b] * ci);
				const float vi = float(p_re[b] * ci + p_im[b] * cr);
				p_re[a] = ur + vr;
				p_im[a] = ui + vi;
				p_re[b] = ur - vr;
				p_im[b] = ui - vi;
				const double t = cr * wr - ci * wi;
				ci = cr * wi + ci * wr;
				cr = t;
			}
		}
	}
}

void WeatherMountainWaves::derive(Vector2 p_wind, float p_buoyancy) {
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	const int n = resolution;
	const float cs = cell_size();
	wind = p_wind;
	buoyancy = p_buoyancy;
	derived = true;
	eta.resize(n * n * LEVELS);
	eta.fill(0.0f);
	for (int k = 0; k < LEVELS; k++) {
		eta_max[k] = 0.0f;
	}
	// The terrain's mean and top (holes where nothing answered take the mean).
	double sum = 0.0;
	int count = 0;
	peak = -1.0e9f;
	const float *h = heights.ptr();
	for (int i = 0; i < n * n; i++) {
		if (Math::is_finite(h[i])) {
			sum += h[i];
			count++;
			peak = MAX(peak, h[i]);
		}
	}
	if (count == 0) {
		ground = 0.0f;
		peak = 0.0f;
		derive_ms = (OS::get_singleton()->get_ticks_usec() - t0) / 1000.0f;
		return;
	}
	ground = float(sum / count);
	const float speed = p_wind.length();
	if (speed < 1.0f || p_buoyancy < 1e-4f) {
		derive_ms = (OS::get_singleton()->get_ticks_usec() - t0) / 1000.0f;
		return; // calm or neutral air: no waves
	}
	// Only the top U / N of the terrain lifts the air over it (the dividing streamline); lower hills wholly.
	const float base = MAX(ground, peak - speed / p_buoyancy);
	LocalVector<float> re;
	LocalVector<float> im;
	re.resize(n * n);
	im.resize(n * n);
	// A cosine taper over the outer eighth keeps the square's edges from wrapping round (the FFT is periodic).
	const int edge = n / 8;
	auto taper = [&](int i) {
		const int d = MIN(i, n - 1 - i);
		return d >= edge ? 1.0f : 0.5f - 0.5f * Math::cos(Math::PI * (d + 0.5f) / edge);
	};
	for (int z = 0; z < n; z++) {
		for (int x = 0; x < n; x++) {
			const float hv = Math::is_finite(h[z * n + x]) ? h[z * n + x] : ground;
			re[z * n + x] = MAX(hv - base, ground - base) * taper(x) * taper(z);
			im[z * n + x] = 0.0f;
		}
	}
	for (int z = 0; z < n; z++) {
		fft(re.ptr() + z * n, im.ptr() + z * n, n, 1, false);
	}
	for (int x = 0; x < n; x++) {
		fft(re.ptr() + x, im.ptr() + x, n, n, false);
	}
	const float n2 = p_buoyancy * p_buoyancy;
	const float dk = Math::TAU / (n * cs);
	LocalVector<float> lre;
	LocalVector<float> lim;
	lre.resize(n * n);
	lim.resize(n * n);
	float *e = eta.ptrw();
	for (int lv = 0; lv < LEVELS; lv++) {
		const float zl = MAX(levels[lv] - base, 0.0f);
		for (int j = 0; j < n; j++) {
			const float l = dk * (j < n / 2 ? j : j - n);
			for (int i = 0; i < n; i++) {
				const float k = dk * (i < n / 2 ? i : i - n);
				const int c = j * n + i;
				const float s = p_wind.x * k + p_wind.y * l;
				const float kap = k * k + l * l;
				float fr = 0.0f;
				float fi = 0.0f;
				if (kap > 0.0f && Math::abs(s) > 1e-3f * p_buoyancy) {
					const float m2 = kap * (n2 - s * s) / (s * s);
					if (m2 >= 0.0f) {
						const float m = SIGN(s) * Math::sqrt(m2);
						fr = Math::cos(m * zl);
						fi = Math::sin(m * zl);
					} else {
						fr = Math::exp(-Math::sqrt(-m2) * zl);
					}
				}
				lre[c] = re[c] * fr - im[c] * fi;
				lim[c] = re[c] * fi + im[c] * fr;
			}
		}
		for (int z = 0; z < n; z++) {
			fft(lre.ptr() + z * n, lim.ptr() + z * n, n, 1, true);
		}
		for (int x = 0; x < n; x++) {
			fft(lre.ptr() + x, lim.ptr() + x, n, n, true);
		}
		const float inv = 1.0f / float(n * n);
		for (int c = 0; c < n * n; c++) {
			const float v = lre[c] * inv;
			e[c * LEVELS + lv] = v;
			eta_max[lv] = MAX(eta_max[lv], v);
		}
	}
	derive_ms = (OS::get_singleton()->get_ticks_usec() - t0) / 1000.0f;
}

float mountain_wave_eta(const Vector<float> &p_eta, int p_n, const Vector2 &p_corner, float p_size, const float *p_levels, const Vector3 &p_position) {
	if (p_eta.size() < p_n * p_n * WeatherMountainWaves::LEVELS || p_n < 2) {
		return 0.0f;
	}
	const float u = (p_position.x - p_corner.x) / p_size * p_n - 0.5f;
	const float v = (p_position.z - p_corner.y) / p_size * p_n - 0.5f;
	if (u < 0.0f || v < 0.0f || u > p_n - 1 || v > p_n - 1) {
		return 0.0f;
	}
	const int x0 = MIN(int(u), p_n - 2);
	const int z0 = MIN(int(v), p_n - 2);
	const float fx = u - x0;
	const float fz = v - z0;
	// Between the levels linearly; below the lowest and above the highest held at the end level.
	int l0 = 0;
	float fl = 0.0f;
	if (p_position.y >= p_levels[WeatherMountainWaves::LEVELS - 1]) {
		l0 = WeatherMountainWaves::LEVELS - 2;
		fl = 1.0f;
	} else if (p_position.y > p_levels[0]) {
		while (l0 < WeatherMountainWaves::LEVELS - 2 && p_position.y > p_levels[l0 + 1]) {
			l0++;
		}
		fl = (p_position.y - p_levels[l0]) / (p_levels[l0 + 1] - p_levels[l0]);
	}
	const float *e = p_eta.ptr();
	auto at = [&](int x, int z) {
		const float *c = e + (z * p_n + x) * WeatherMountainWaves::LEVELS;
		return Math::lerp(c[l0], c[l0 + 1], fl);
	};
	return Math::lerp(Math::lerp(at(x0, z0), at(x0 + 1, z0), fx), Math::lerp(at(x0, z0 + 1), at(x0 + 1, z0 + 1), fx), fz);
}

float WeatherMountainWaves::eta_at(const Vector3 &p_position) const {
	if (!derived) {
		return 0.0f;
	}
	const Rect2 r = rect();
	return mountain_wave_eta(eta, resolution, r.position, extent, levels, p_position);
}

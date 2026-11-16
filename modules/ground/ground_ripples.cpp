/**************************************************************************/
/*  ground_ripples.cpp                                                    */
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

#include "ground_ripples.h"

#include "core/math/math_funcs.h"
#include "core/math/random_pcg.h"

void GroundRipples::step_field(LocalVector<float> &r_h, LocalVector<float> &r_tmp, const LocalVector<float> *p_mask, int p_n, const GroundRipples &p_params, float p_q, Vector2 p_wind) {
	const int nn = p_n * p_n;
	if ((int)r_h.size() != nn || p_q <= 0.0f) {
		return;
	}
	r_tmp.resize(nn);
	const float inv_cell = 1.0f / p_params.cell;
	const float hs = p_params.hop_saturation;
	// Saltation: each cell sends q a hop downwind, splatted over the four cells round where it lands. The
	// window wraps (its edges fade out in the shader).
	for (int i = 0; i < nn; i++) {
		r_tmp[i] = r_h[i];
	}
	for (int z = 0; z < p_n; z++) {
		for (int x = 0; x < p_n; x++) {
			const int i = z * p_n + x;
			const float q = p_mask ? p_q * (*p_mask)[i] : p_q;
			if (q <= 0.0f) {
				continue;
			}
			const float l = MAX((p_params.hop + p_params.hop_gain * hs * Math::tanh(r_h[i] / hs)) * inv_cell, 0.5f);
			const float tx = x + l * p_wind.x;
			const float tz = z + l * p_wind.y;
			const int x0 = (int)Math::floor(tx);
			const int z0 = (int)Math::floor(tz);
			const float fx = tx - x0;
			const float fz = tz - z0;
			const int xa = ((x0 % p_n) + p_n) % p_n;
			const int xb = (xa + 1) % p_n;
			const int za = ((z0 % p_n) + p_n) % p_n;
			const int zb = (za + 1) % p_n;
			r_tmp[i] -= q;
			r_tmp[za * p_n + xa] += q * (1.0f - fx) * (1.0f - fz);
			r_tmp[za * p_n + xb] += q * fx * (1.0f - fz);
			r_tmp[zb * p_n + xa] += q * (1.0f - fx) * fz;
			r_tmp[zb * p_n + xb] += q * fx * fz;
		}
	}
	// Creep (reptation): a share of the difference to the neighbours' mean, 1/6 each side and 1/12 each corner,
	// its share growing with the saltation that drives it.
	const float d = MIN(p_params.d_ref * p_q / p_params.q_ref, 0.9f);
	for (int z = 0; z < p_n; z++) {
		const int zu = (z + p_n - 1) % p_n;
		const int zd = (z + 1) % p_n;
		for (int x = 0; x < p_n; x++) {
			const int xl = (x + p_n - 1) % p_n;
			const int xr = (x + 1) % p_n;
			const float c = r_tmp[z * p_n + x];
			const float side = r_tmp[z * p_n + xl] + r_tmp[z * p_n + xr] + r_tmp[zu * p_n + x] + r_tmp[zd * p_n + x];
			const float corner = r_tmp[zu * p_n + xl] + r_tmp[zu * p_n + xr] + r_tmp[zd * p_n + xl] + r_tmp[zd * p_n + xr];
			r_h[z * p_n + x] = c + d * (side / 6.0f + corner / 12.0f - c);
		}
	}
	// Avalanching past the repose (a safety: the saturating hop keeps ripples well under it).
	const float lim = p_params.tan_repose * p_params.cell;
	for (int axis = 0; axis < 2; axis++) {
		for (int z = 0; z < p_n; z++) {
			for (int x = 0; x < p_n; x++) {
				const int i = z * p_n + x;
				const int j = axis == 0 ? z * p_n + (x + 1) % p_n : ((z + 1) % p_n) * p_n + x;
				const float diff = r_h[j] - r_h[i];
				const float a = Math::abs(diff);
				if (a > lim) {
					const float m = 0.25f * (a - lim) * (diff > 0.0f ? 1.0f : -1.0f);
					r_h[i] += m;
					r_h[j] -= m;
				}
			}
		}
	}
}

void GroundRipples::grow(LocalVector<float> &r_h, int p_n, const GroundRipples &p_params, Vector2 p_wind, int p_steps, uint32_t p_seed) {
	RandomPCG rng(p_seed);
	r_h.resize(p_n * p_n);
	for (int i = 0; i < p_n * p_n; i++) {
		r_h[i] = (float)rng.randfn(0.0, 1e-4);
	}
	LocalVector<float> t;
	for (int s = 0; s < p_steps; s++) {
		step_field(r_h, t, nullptr, p_n, p_params, p_params.q_ref, p_wind);
	}
}

void GroundRipples::step(float p_q, Vector2 p_wind) {
	if (!valid) {
		return;
	}
	step_field(h, tmp, &mask, n, *this, p_q, p_wind);
}

void GroundRipples::move_to(Vector2i p_origin) {
	const int nn = n * n;
	if (valid && p_origin == origin && (int)h.size() == nn) {
		return;
	}
	LocalVector<float> out;
	out.resize(nn);
	LocalVector<float> out_mask;
	out_mask.resize(nn);
	for (int z = 0; z < n; z++) {
		for (int x = 0; x < n; x++) {
			const int wx = p_origin.x + x;
			const int wz = p_origin.y + z;
			const int ox = wx - origin.x;
			const int oz = wz - origin.y;
			const int i = z * n + x;
			if (valid && ox >= 0 && oz >= 0 && ox < n && oz < n && (int)h.size() == nn) {
				out[i] = h[oz * n + ox];
				out_mask[i] = mask[oz * n + ox];
			} else {
				out[i] = tmpl_ready ? tmpl[(((wz % n) + n) % n) * n + (((wx % n) + n) % n)] : 0.0f;
				out_mask[i] = 1.0f;
			}
		}
	}
	h = out;
	mask = out_mask;
	origin = p_origin;
	valid = true;
}

float GroundRipples::sample(float p_x, float p_z) const {
	if (!valid) {
		return 0.0f;
	}
	const float fx = p_x / cell - origin.x;
	const float fz = p_z / cell - origin.y;
	const int x0 = (int)Math::floor(fx);
	const int z0 = (int)Math::floor(fz);
	if (x0 < 0 || z0 < 0 || x0 >= n - 1 || z0 >= n - 1) {
		return 0.0f;
	}
	const float ax = fx - x0;
	const float az = fz - z0;
	const int i = z0 * n + x0;
	return Math::lerp(Math::lerp(h[i], h[i + 1], ax), Math::lerp(h[i + n], h[i + n + 1], ax), az);
}

float GroundRipples::mean() const {
	double s = 0.0;
	for (float v : h) {
		s += v;
	}
	return h.size() ? (float)(s / h.size()) : 0.0f;
}

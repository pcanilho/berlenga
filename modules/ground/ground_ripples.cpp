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

static inline float _rv_hash(int p_x, int p_y, uint32_t p_s) {
	uint32_t h = ((uint32_t)p_x * 0x8da6b343u) ^ ((uint32_t)p_y * 0xd8163841u) ^ (p_s * 0xcb1ab31fu);
	h = (h ^ (h >> 16)) * 0x7feb352du;
	h = (h ^ (h >> 15)) * 0x846ca68bu;
	h ^= h >> 16;
	return (float)(h >> 8) * (1.0f / 16777216.0f);
}

static inline float _rv_noise(Vector2 p_p, uint32_t p_s) {
	const float ix = Math::floor(p_p.x);
	const float iy = Math::floor(p_p.y);
	const float fx = p_p.x - ix;
	const float fy = p_p.y - iy;
	const float ux = fx * fx * (3.0f - 2.0f * fx);
	const float uy = fy * fy * (3.0f - 2.0f * fy);
	const int x = (int)ix;
	const int y = (int)iy;
	return Math::lerp(Math::lerp(_rv_hash(x, y, p_s), _rv_hash(x + 1, y, p_s), ux), Math::lerp(_rv_hash(x, y + 1, p_s), _rv_hash(x + 1, y + 1, p_s), ux), uy);
}

static inline float _rv_smoothstep(float p_a, float p_b, float p_x) {
	const float t = CLAMP((p_x - p_a) / (p_b - p_a), 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

Vector3 GroundRipples::vary(Vector2 p_xz, Vector2 p_k) {
	const Vector2 p = p_xz;
	Vector2 o = 4.0f * (Vector2(_rv_noise(p / 24.0f, 1u), _rv_noise(p / 24.0f, 2u)) - Vector2(0.5f, 0.5f));
	o += 1.2f * (Vector2(_rv_noise(p / 6.0f, 3u), _rv_noise(p / 6.0f, 4u)) - Vector2(0.5f, 0.5f));
	o += 0.06f * (Vector2(_rv_noise(p / 1.7f, 5u), _rv_noise(p / 1.7f, 6u)) - Vector2(0.5f, 0.5f));
	// Patchy over tens of metres: smooth sheets between ripple fields (material.cpp ground_ripple_vary()).
	const float amp = Math::lerp(0.2f, 1.0f, _rv_smoothstep(0.1f, 0.9f, 0.5f * _rv_noise(p / 6.0f, 7u) + 0.5f * _rv_noise(p / 1.8f, 8u))) *
			_rv_smoothstep(0.36f, 0.6f, 0.7f * _rv_noise(p / 16.0f, 15u) + 0.3f * _rv_noise(p / 5.0f, 16u));
	const float kk = p_k.dot(p_k);
	if (kk > 0.0f) {
		const Vector2 kn = p_k / Math::sqrt(kk);
		const Vector2 t(-kn.y, kn.x);
		// Spacing: a stretch along the crests' normal only, so crests crowd and open by up to a fifth over a few metres.
		o += kn * (0.3f * (_rv_noise(Vector2(p.dot(kn) / 2.5f, p.dot(t) / 6.0f), 14u) - 0.5f));
		const int cx = (int)Math::floor(p.x / 2.0f);
		const int cz = (int)Math::floor(p.y / 2.0f);
		float ph = 0.0f;
		for (int dz = -1; dz <= 1; dz++) {
			for (int dx = -1; dx <= 1; dx++) {
				const int x = cx + dx;
				const int z = cz + dz;
				if (_rv_hash(x, z, 9u) >= 0.5f) {
					continue;
				}
				const Vector2 m = (Vector2((float)x, (float)z) + Vector2(_rv_hash(x, z, 10u), _rv_hash(x, z, 11u))) * 2.0f;
				const float half = 0.15f + 0.3f * _rv_hash(x, z, 12u);
				const Vector2 da = p - (m - t * half);
				const Vector2 db = p - (m + t * half);
				const float sgn = _rv_hash(x, z, 13u) < 0.5f ? -1.0f : 1.0f;
				ph += sgn * Math::atan2(da.x * db.y - da.y * db.x, da.dot(db)) * (1.0f - _rv_smoothstep(0.9f, 1.9f, (p - m).length()));
			}
		}
		o += p_k * (ph / kk);
	}
	return Vector3(o.x, o.y, amp);
}

float GroundRipples::tmpl_at(int p_wx, int p_wz) const {
	const Vector3 v = vary(Vector2(p_wx * cell, p_wz * cell), tmpl_k);
	const float fx = p_wx + v.x / cell;
	const float fz = p_wz + v.y / cell;
	const float x0f = Math::floor(fx);
	const float z0f = Math::floor(fz);
	const float ax = fx - x0f;
	const float az = fz - z0f;
	const int x0 = (((int)x0f % n) + n) % n;
	const int z0 = (((int)z0f % n) + n) % n;
	const int x1 = (x0 + 1) % n;
	const int z1 = (z0 + 1) % n;
	const float t = Math::lerp(Math::lerp(tmpl[z0 * n + x0], tmpl[z0 * n + x1], ax), Math::lerp(tmpl[z1 * n + x0], tmpl[z1 * n + x1], ax), az);
	return t * v.z;
}

Vector2 GroundRipples::dominant_k(const LocalVector<float> &p_h, int p_n, float p_cell) {
	if (p_n < 8 || (int)p_h.size() != p_n * p_n) {
		return Vector2();
	}
	const float period = p_n * p_cell;
	const int mmin = MAX(1, (int)Math::ceil(period / 0.4f));
	const int mmax = MIN(p_n / 2 - 1, (int)Math::floor(period / 0.03f));
	if (mmax < mmin) {
		return Vector2();
	}
	const int nk = 2 * mmax + 1;
	LocalVector<float> cs, sn;
	cs.resize(p_n);
	sn.resize(p_n);
	for (int i = 0; i < p_n; i++) {
		cs[i] = Math::cos(Math::TAU * i / p_n);
		sn[i] = Math::sin(Math::TAU * i / p_n);
	}
	// Along x first (every row, kx in -mmax..mmax), then along z: a separable DFT over the wanted band.
	LocalVector<float> re, im;
	re.resize(nk * p_n);
	im.resize(nk * p_n);
	for (int z = 0; z < p_n; z++) {
		for (int a = 0; a < nk; a++) {
			const int kx = a - mmax;
			float sr = 0.0f, si = 0.0f;
			for (int x = 0; x < p_n; x++) {
				const int j = (((kx * x) % p_n) + p_n) % p_n;
				const float v = p_h[z * p_n + x];
				sr += v * cs[j];
				si -= v * sn[j];
			}
			re[a * p_n + z] = sr;
			im[a * p_n + z] = si;
		}
	}
	float best = 0.0f;
	Vector2 bk;
	for (int a = 0; a < nk; a++) {
		const int kx = a - mmax;
		for (int kz = 0; kz <= mmax; kz++) { // half plane: the spectrum of a real field is symmetric
			const int m2 = kx * kx + kz * kz;
			if (m2 < mmin * mmin || m2 > mmax * mmax || (kz == 0 && kx < 0)) {
				continue;
			}
			float sr = 0.0f, si = 0.0f;
			for (int z = 0; z < p_n; z++) {
				const int j = ((kz * z) % p_n + p_n) % p_n;
				const float r = re[a * p_n + z];
				const float i = im[a * p_n + z];
				sr += r * cs[j] + i * sn[j];
				si += i * cs[j] - r * sn[j];
			}
			const float pw = sr * sr + si * si;
			if (pw > best) {
				best = pw;
				bk = Vector2((float)kx, (float)kz) * (Math::TAU / period);
			}
		}
	}
	return bk;
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
				out[i] = tmpl_ready ? tmpl_at(wx, wz) : 0.0f;
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

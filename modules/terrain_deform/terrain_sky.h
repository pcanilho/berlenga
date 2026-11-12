/**************************************************************************/
/*  terrain_sky.h                                                         */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
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
#include "core/object/worker_thread_pool.h"
#include "core/string/string_name.h"

#include <cstdint>

// Sky occlusion of a heightfield (docs/ideas/terrain-material.md, "Sky occlusion"). Each node keeps its terrain
// horizons in 16 directions (horizon mapping, Max 1988) and its sky view factor (Dozier and Frew 1990). From a
// panorama of the sky, each node then gets the light of the sky it sees as nine spherical harmonics (order 2,
// enough for irradiance: Ramamoorthi and Hanrahan 2001; precomputed visibility as in Sloan et al. 2002). Its
// irradiance on a normal over the whole sky's is the occlusion raster ambient light needs, per pixel and normal,
// so a slope facing a low sunset glow behind a ridge loses that glow. A path tracer finds the hills itself.
namespace TerrainSky {

constexpr int DIRECTIONS = 16; // every 22.5 degrees, clockwise from north (-Z)
constexpr int ELEVATIONS = 6; // samples between a direction's horizon and the zenith, for the view factor

struct Job {
	const float *h;
	int nx;
	int ny;
	float cell;
	float max_r; // cells
	float *vis; // nx * ny: sky view factor
	uint8_t *horizon; // nx * ny * DIRECTIONS: the terrain's horizon elevation, 0..90 degrees as 0..255
	float dx[DIRECTIONS];
	float dz[DIRECTIONS];

	float height(int p_x, int p_z) const {
		return h[CLAMP(p_z, 0, ny - 1) * nx + CLAMP(p_x, 0, nx - 1)];
	}

	void row(int z) const {
		for (int x = 0; x < nx; x++) {
			const float h0 = h[z * nx + x];
			// Horn (1981) gradient.
			const float a = height(x - 1, z - 1), b = height(x, z - 1), c = height(x + 1, z - 1);
			const float d = height(x - 1, z), f = height(x + 1, z);
			const float g = height(x - 1, z + 1), hh = height(x, z + 1), k = height(x + 1, z + 1);
			const float dhdx = ((c + 2.0f * f + k) - (a + 2.0f * d + g)) / (8.0f * cell);
			const float dhdz = ((g + 2.0f * hh + k) - (a + 2.0f * b + c)) / (8.0f * cell);
			const float inv = 1.0f / Math::sqrt(dhdx * dhdx + dhdz * dhdz + 1.0f);
			const float nxv = -dhdx * inv, nyv = inv, nzv = -dhdz * inv;
			float view = 0.0f;
			for (int j = 0; j < DIRECTIONS; j++) {
				const float tx = dx[j], tz = dz[j];
				// The terrain's horizon in this direction; past the grid the ground counts as level.
				float best = 0.0f;
				for (float r = 1.0f; r <= max_r; r += MAX(1.0f, r * 0.08f)) {
					const float gx = x + tx * r, gz = z + tz * r;
					if (gx < 0.0f || gz < 0.0f || gx > nx - 1 || gz > ny - 1) {
						break;
					}
					const int x0 = MIN(int(gx), nx - 2), z0 = MIN(int(gz), ny - 2);
					const float fx = gx - x0, fz = gz - z0;
					const float *row0 = h + z0 * nx, *row1 = row0 + nx;
					const float hs = Math::lerp(Math::lerp(row0[x0], row0[x0 + 1], fx), Math::lerp(row1[x0], row1[x0 + 1], fx), fz);
					best = MAX(best, (hs - h0) / (r * cell));
				}
				const float hz = Math::atan(best);
				horizon[size_t(z * nx + x) * DIRECTIONS + j] = uint8_t(CLAMP(int(Math::ceil(hz * (255.0f / (float(Math::PI) * 0.5f)))), 0, 255));
				// The view factor: cosine-weighted sky over this direction's wedge, above the terrain's horizon or
				// the slope's own plane, whichever is higher.
				const float eff = MAX(hz, Math::atan(-(nxv * tx + nzv * tz) / nyv));
				const float de = (float(Math::PI) * 0.5f - eff) / ELEVATIONS;
				for (int e = 0; e < ELEVATIONS; e++) {
					const float el = eff + (e + 0.5f) * de;
					const float ce = Math::cos(el);
					view += MAX(0.0f, tx * ce * nxv + Math::sin(el) * nyv + tz * ce * nzv) * ce * de;
				}
			}
			vis[z * nx + x] = CLAMP(view * (float(Math::TAU) / DIRECTIONS) / float(Math::PI), 0.0f, 1.0f);
		}
	}
	static void run(void *p_job, uint32_t p_row) { static_cast<const Job *>(p_job)->row(int(p_row)); }
};

// p_h: nx x ny heights p_cell apart, row by row along +Z. Horizons are searched out to p_distance metres.
inline void compute(const float *p_h, int p_nx, int p_ny, float p_cell, float p_distance, float *r_vis, uint8_t *r_horizon) {
	if (p_nx < 2 || p_ny < 2) {
		return;
	}
	Job job;
	job.h = p_h;
	job.nx = p_nx;
	job.ny = p_ny;
	job.cell = p_cell;
	job.max_r = p_distance / p_cell;
	job.vis = r_vis;
	job.horizon = r_horizon;
	for (int d = 0; d < DIRECTIONS; d++) {
		const float a = float(Math::TAU) * d / DIRECTIONS;
		job.dx[d] = Math::sin(a);
		job.dz[d] = -Math::cos(a);
	}
	WorkerThreadPool::GroupID g = WorkerThreadPool::get_singleton()->add_native_group_task(&Job::run, &job, p_ny, -1, true, SNAME("TerrainSky"));
	WorkerThreadPool::get_singleton()->wait_for_group_task_completion(g);
}

// The sky's light gathered into bins: DIRECTIONS azimuths (the horizons' own, centred on them) by ELEVATION_BINS
// elevations over the whole sphere.
constexpr int ELEVATION_BINS = 16;
constexpr int BINS = DIRECTIONS * ELEVATION_BINS;
constexpr int SH = 9;

// Real spherical harmonics to order 2 (y up; any fixed axis order works, the shader uses the same).
inline void sh_basis(float x, float y, float z, float *r) {
	r[0] = 0.282095f;
	r[1] = 0.488603f * y;
	r[2] = 0.488603f * z;
	r[3] = 0.488603f * x;
	r[4] = 1.092548f * x * y;
	r[5] = 1.092548f * y * z;
	r[6] = 0.315392f * (3.0f * z * z - 1.0f);
	r[7] = 1.092548f * x * z;
	r[8] = 0.546274f * (x * x - y * y);
}

struct SkyBins {
	float light[BINS] = {}; // luminance times solid angle
	float dir[BINS * 3] = {};
	float el0[ELEVATION_BINS] = {}; // radians, the bin's lower edge
	float el1[ELEVATION_BINS] = {};
	float full[SH] = {}; // the whole sky's harmonics, luminance
};

// An even sky of luminance 1, for the view factor's look before any panorama is read.
inline void even_sky(SkyBins &r_bins) {
	r_bins = SkyBins();
	const float pi = float(Math::PI);
	for (int e = 0; e < ELEVATION_BINS; e++) {
		r_bins.el0[e] = -pi * 0.5f + pi * e / ELEVATION_BINS;
		r_bins.el1[e] = -pi * 0.5f + pi * (e + 1) / ELEVATION_BINS;
		const float em = 0.5f * (r_bins.el0[e] + r_bins.el1[e]);
		for (int j = 0; j < DIRECTIONS; j++) {
			const float az = float(Math::TAU) * j / DIRECTIONS;
			const int b = j * ELEVATION_BINS + e;
			r_bins.light[b] = (Math::sin(r_bins.el1[e]) - Math::sin(r_bins.el0[e])) * (float(Math::TAU) / DIRECTIONS);
			r_bins.dir[b * 3 + 0] = Math::sin(az) * Math::cos(em);
			r_bins.dir[b * 3 + 1] = Math::sin(em);
			r_bins.dir[b * 3 + 2] = -Math::cos(az) * Math::cos(em);
		}
	}
	for (int b = 0; b < BINS; b++) {
		float y[SH];
		sh_basis(r_bins.dir[b * 3], r_bins.dir[b * 3 + 1], r_bins.dir[b * 3 + 2], y);
		for (int k = 0; k < SH; k++) {
			r_bins.full[k] += r_bins.light[b] * y[k];
		}
	}
}

// p_rgb: a p_w x p_h radiance panorama of linear RGB floats in Godot's layout (u = phi / TAU, v = theta / PI,
// direction (-sin phi sin theta, cos theta, -cos phi sin theta)), as RenderingServer.sky_bake_panorama returns it.
inline void gather_sky(const float *p_rgb, int p_w, int p_h, SkyBins &r_bins) {
	even_sky(r_bins);
	const float pi = float(Math::PI);
	for (int b = 0; b < BINS; b++) {
		r_bins.light[b] = 0.0f;
		r_bins.dir[b * 3 + 0] = r_bins.dir[b * 3 + 1] = r_bins.dir[b * 3 + 2] = 0.0f;
	}
	for (int k = 0; k < SH; k++) {
		r_bins.full[k] = 0.0f;
	}
	for (int y = 0; y < p_h; y++) {
		const float theta = (y + 0.5f) / p_h * pi;
		const float st = Math::sin(theta), ct = Math::cos(theta);
		const float dw = st * (pi / p_h) * (2.0f * pi / p_w);
		const int e = CLAMP(int((pi - theta) / pi * ELEVATION_BINS), 0, ELEVATION_BINS - 1); // elevation + 90 degrees
		for (int x = 0; x < p_w; x++) {
			const float phi = (x + 0.5f) / p_w * 2.0f * pi;
			const float dx = -Math::sin(phi) * st, dy = ct, dz = -Math::cos(phi) * st;
			float az = Math::atan2(dx, -dz);
			if (az < 0.0f) {
				az += 2.0f * pi;
			}
			const int j = int(Math::round(az / (2.0f * pi) * DIRECTIONS)) % DIRECTIONS;
			const float *c = p_rgb + (size_t(y) * p_w + x) * 3;
			const float l = MAX(0.0f, 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]) * dw;
			const int b = j * ELEVATION_BINS + e;
			r_bins.light[b] += l;
			r_bins.dir[b * 3 + 0] += dx * l;
			r_bins.dir[b * 3 + 1] += dy * l;
			r_bins.dir[b * 3 + 2] += dz * l;
			float ys[SH];
			sh_basis(dx, dy, dz, ys);
			for (int k = 0; k < SH; k++) {
				r_bins.full[k] += ys[k] * l;
			}
		}
	}
	for (int b = 0; b < BINS; b++) {
		const float *d = r_bins.dir + b * 3;
		const float len = Math::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		if (len > 0.0f) {
			r_bins.dir[b * 3 + 0] /= len;
			r_bins.dir[b * 3 + 1] /= len;
			r_bins.dir[b * 3 + 2] /= len;
		}
	}
}

// The harmonics of the sky light a node sees past its horizons; nothing below level ground is sky.
inline void seen_sky(const SkyBins &p_bins, const uint8_t *p_horizon, float *r_sh) {
	const float to_rad = (float(Math::PI) * 0.5f) / 255.0f;
	for (int k = 0; k < SH; k++) {
		r_sh[k] = 0.0f;
	}
	for (int j = 0; j < DIRECTIONS; j++) {
		const float hz = p_horizon[j] * to_rad;
		for (int e = ELEVATION_BINS / 2; e < ELEVATION_BINS; e++) {
			const float e0 = p_bins.el0[e], e1 = p_bins.el1[e];
			const float open = CLAMP((e1 - MAX(hz, e0)) / (e1 - e0), 0.0f, 1.0f);
			if (open <= 0.0f) {
				continue;
			}
			const int b = j * ELEVATION_BINS + e;
			const float l = p_bins.light[b] * open;
			float y[SH];
			sh_basis(p_bins.dir[b * 3], p_bins.dir[b * 3 + 1], p_bins.dir[b * 3 + 2], y);
			for (int k = 0; k < SH; k++) {
				r_sh[k] += y[k] * l;
			}
		}
	}
}

} // namespace TerrainSky

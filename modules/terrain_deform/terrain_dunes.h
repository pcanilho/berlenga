/**************************************************************************/
/*  terrain_dunes.h                                                       */
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

// Wind-blown sand: dunes on a fine grid over a sand area. Werner's (1995) slab model made deterministic, as Nilles,
// Günther and Müller (GRAPP 2024) do for Paris et al.'s desertscapes (CGF 2019): each step every sandy cell out of the
// wind shadow gives up a slab's worth of sand (less under plants), which hops downwind a fixed length at a time and
// settles at each landing with a probability (higher on sand than on bare ground, certain in the shadow of a crest
// and higher among plants), carried on as an expected amount instead of drawn at random. Slopes of sand over the
// angle of repose avalanche. Plants (marram, after Baas 2002 and Nield and Baas 2008's DECAL) grow where they are
// buried gently, die where they are buried deep or blown out, and hold and trap the sand: foredunes build along the
// plant line, and blowouts and parabolic dunes open where the cover fails. Plain C++ in a fixed order, so a seed gives
// the same dunes everywhere.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

class TerrainDunes {
public:
	struct Params {
		double slab = 0.1; // m of sand a cell gives up in a step, out of the shadow and bare
		double hop = 3.0; // cells a saltating slab travels between landings
		int max_hops = 12; // landings before the rest settles
		double settle_sand = 0.6; // chance to settle on sand
		double settle_bare = 0.4; // on bare ground
		double settle_plants = 0.5; // added at full plant cover
		double shadow_tan = 0.268; // tan 15 degrees: the wind's shadow behind a crest
		double repose_tan = 0.675; // tan 34 degrees: dry sand avalanches past it
		double repose_tan_plants = 1.0; // tan 45 degrees under full cover (roots hold it)
		double plant_growth = 0.02; // cover gained a step where conditions suit
		double plant_burial_ok = 0.02; // m a step of burial (averaged) marram thrives most on: it grows up through it
		double plant_hold = 0.9; // share of the lift the full cover stops
		double venturi = 0.08; // the wind's speed-up a metre above the surroundings (lift and hop)
		int shadow_every = 4; // steps between shadow and speed-up updates
	};
	Params p;

	int w = 0, h = 0;
	double cell = 2.0;
	std::vector<float> base; // the ground under the sand, m
	std::vector<uint8_t> inside; // 1: sand may lie here; 0: out of the area (sand landing there is gone)
	std::vector<uint8_t> water; // 1: water takes the sand that lands on it
	std::vector<float> supply; // m of sand added per step (the beach)
	std::vector<float> plant_ok; // 0..1: how well plants grow (0 on the beach face, salt and swash)
	double wind_x = 1.0, wind_z = 0.0; // unit, the way the wind blows

	std::vector<float> sand; // m of sand over the base
	std::vector<float> plants; // 0..1 cover
	std::vector<uint8_t> shadow;
	std::vector<float> speed; // the wind's speed-up: 1 on the flat, more over crests
	std::vector<float> burial; // m a step of net deposition, averaged over about 30 steps
	double lost = 0.0; // m3 carried out of the area or into the water

	void run(int p_steps) {
		const int n = w * h;
		sand.assign(n, 0.0f);
		plants.assign(n, 0.0f);
		shadow.assign(n, 0);
		speed.assign(n, 1.0f);
		burial.assign(n, 0.0f);
		std::vector<float> before(n, 0.0f), moved(n, 0.0f);
		for (int step = 0; step < p_steps; step++) {
			for (int i = 0; i < n; i++) {
				before[i] = sand[i];
				if (inside[i] && !water[i]) {
					sand[i] += supply[i];
				}
			}
			if (step % p.shadow_every == 0) {
				_shadow();
				_speed();
			}
			// Lift: every cell's slab at once (the step's sand leaves together, then lands), in row order.
			std::fill(moved.begin(), moved.end(), 0.0f);
			for (int i = 0; i < n; i++) {
				// A film thinner than a millimetre stays (it would cost a full carry for nothing).
				if (!inside[i] || water[i] || shadow[i] || sand[i] < 0.001f) {
					continue;
				}
				const float a = std::min(sand[i], float(p.slab * speed[i] * (1.0 - p.plant_hold * plants[i])));
				sand[i] -= a;
				moved[i] = a;
			}
			for (int z = 0; z < h; z++) {
				for (int x = 0; x < w; x++) {
					const int i = z * w + x;
					if (moved[i] > 0.0f) {
						_carry(x, z, moved[i]);
					}
				}
			}
			_avalanche(2);
			_grow(before);
		}
	}

	double height(int i) const { return double(base[i]) + double(sand[i]); }

private:
	double _bilinear_h(double fx, double fz) const {
		const int x0 = std::clamp(int(std::floor(fx)), 0, w - 2), z0 = std::clamp(int(std::floor(fz)), 0, h - 2);
		const double tx = std::clamp(fx - x0, 0.0, 1.0), tz = std::clamp(fz - z0, 0.0, 1.0);
		const int i = z0 * w + x0;
		return (height(i) * (1.0 - tx) + height(i + 1) * tx) * (1.0 - tz) + (height(i + w) * (1.0 - tx) + height(i + w + 1) * tx) * tz;
	}

	// A cell is in the wind's shadow when ground upwind stands above the 15 degree line back from it (Werner 1995;
	// Nilles 2024 sample it bilinearly along the wind).
	void _shadow() {
		const int reach = 12;
		for (int z = 0; z < h; z++) {
			for (int x = 0; x < w; x++) {
				const int i = z * w + x;
				shadow[i] = 0;
				if (!inside[i]) {
					continue;
				}
				const double hi = height(i);
				for (int k = 1; k <= reach; k++) {
					const double fx = x - wind_x * k, fz = z - wind_z * k;
					if (fx < 0.0 || fz < 0.0 || fx > w - 1 || fz > h - 1) {
						break;
					}
					if (_bilinear_h(fx, fz) - hi > p.shadow_tan * k * cell) {
						shadow[i] = 1;
						break;
					}
				}
			}
		}
	}

	// The wind speeds up over ground standing above its surroundings (Jackson and Hunt 1975; Paris 2019 warp the
	// wind the same way): more sand lifts there and it hops further, which caps the crests and spreads the dunes.
	void _speed() {
		const int r = 8;
		const double inv = 1.0 / double((2 * r + 1) * (2 * r + 1));
		// Box mean of the height over (2r+1)^2 cells from running sums, clamped at the edges.
		std::vector<double> row(size_t(w) * h, 0.0);
		for (int z = 0; z < h; z++) {
			double s = 0.0;
			for (int k = -r; k <= r; k++) {
				s += height(z * w + std::clamp(k, 0, w - 1));
			}
			for (int x = 0; x < w; x++) {
				row[z * w + x] = s;
				s += height(z * w + std::min(x + r + 1, w - 1)) - height(z * w + std::max(x - r, 0));
			}
		}
		for (int x = 0; x < w; x++) {
			double s = 0.0;
			for (int k = -r; k <= r; k++) {
				s += row[std::clamp(k, 0, h - 1) * w + x];
			}
			for (int z = 0; z < h; z++) {
				const int i = z * w + x;
				speed[i] = float(std::clamp(1.0 + p.venturi * (height(i) - s * inv), 0.5, 2.5));
				s += row[std::min(z + r + 1, h - 1) * w + x] - row[std::max(z - r, 0) * w + x];
			}
		}
	}

	// The slab hops downwind, settling a share at each landing (the expected amount of Werner's random settling),
	// split bilinearly between the four cells round the landing point.
	void _carry(int x, int z, float a) {
		double fx = x, fz = z;
		double left = a;
		const double done = 0.01 * a; // under it, the rest settles at this landing
		for (int hop = 0; hop < p.max_hops && left > 1.0e-9; hop++) {
			const double len = p.hop * speed[std::clamp(int(fz + 0.5), 0, h - 1) * w + std::clamp(int(fx + 0.5), 0, w - 1)];
			fx += wind_x * len;
			fz += wind_z * len;
			if (fx < 0.0 || fz < 0.0 || fx > w - 1 || fz > h - 1) {
				lost += left * cell * cell;
				return;
			}
			const int x0 = std::min(int(fx), w - 2), z0 = std::min(int(fz), h - 2);
			const double tx = fx - x0, tz = fz - z0;
			const int c[4] = { z0 * w + x0, z0 * w + x0 + 1, (z0 + 1) * w + x0, (z0 + 1) * w + x0 + 1 };
			const double cw[4] = { (1.0 - tx) * (1.0 - tz), tx * (1.0 - tz), (1.0 - tx) * tz, tx * tz };
			const bool last = hop == p.max_hops - 1 || left < done;
			double settled = 0.0;
			for (int k = 0; k < 4; k++) {
				const int j = c[k];
				const double part = left * cw[k];
				if (!inside[j] || water[j]) {
					lost += part * cell * cell; // into the sea, a river or off the area
					settled += part;
					continue;
				}
				double chance = last || shadow[j] ? 1.0 : (sand[j] > 0.01f ? p.settle_sand : p.settle_bare) + p.settle_plants * plants[j];
				chance = std::min(chance, 1.0);
				sand[j] += float(part * chance);
				settled += part * chance;
			}
			left -= settled;
		}
	}

	// Sand steeper than its repose slides to the lower neighbours (eight-neighbour, as Nilles 2024), in passes.
	void _avalanche(int p_passes) {
		static const int dx[8] = { 1, -1, 0, 0, 1, 1, -1, -1 };
		static const int dz[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
		for (int pass = 0; pass < p_passes; pass++) {
			for (int z = 0; z < h; z++) {
				for (int x = 0; x < w; x++) {
					const int i = z * w + x;
					if (sand[i] <= 0.0f || !inside[i]) {
						continue;
					}
					const double tan_r = p.repose_tan + (p.repose_tan_plants - p.repose_tan) * plants[i];
					double hi = height(i);
					for (int k = 0; k < 8; k++) {
						if (x + dx[k] < 0 || z + dz[k] < 0 || x + dx[k] >= w || z + dz[k] >= h) {
							continue;
						}
						const int j = (z + dz[k]) * w + x + dx[k];
						const double dist = (k < 4 ? 1.0 : 1.41421356) * cell;
						const double drop = hi - height(j) - tan_r * dist;
						if (drop <= 0.0) {
							continue;
						}
						const float m = std::min(sand[i], float(0.25 * drop));
						sand[i] -= m;
						if (inside[j] && !water[j]) {
							sand[j] += m;
						} else {
							lost += double(m) * cell * cell;
						}
						hi -= m;
						if (sand[i] <= 0.0f) {
							break;
						}
					}
				}
			}
		}
	}

	// Marram: grows best under steady gentle burial and declines under heavy burial or erosion (Maun 1998's burial
	// response; DECAL's vegetation rule, on the averaged net deposition).
	void _grow(const std::vector<float> &p_before) {
		const int n = w * h;
		for (int i = 0; i < n; i++) {
			if (!inside[i] || water[i]) {
				plants[i] = 0.0f;
				continue;
			}
			const double dh = double(sand[i]) - double(p_before[i]);
			burial[i] += float((dh - burial[i]) / 32.0);
			double v = plants[i];
			const double ok = plant_ok[i];
			// Response to burial: 0.5 on stable ground, 1 at the optimum, 0 at three times it, negative past that and
			// under erosion.
			const double b = burial[i] / p.plant_burial_ok;
			const double resp = b >= 0.0 ? (b <= 1.0 ? 0.5 + 0.5 * b : 1.0 - 0.5 * (b - 1.0)) : 0.5 + 2.0 * b;
			if (resp >= 0.0) {
				if (ok > 0.0) {
					v += p.plant_growth * ok * resp * (1.0 - v) + 0.002 * ok; // a little seeding where it can live
				}
			} else {
				v += p.plant_growth * 2.0 * resp * v; // buried too deep or blown out
			}
			plants[i] = float(std::clamp(v, 0.0, 1.0));
		}
	}
};

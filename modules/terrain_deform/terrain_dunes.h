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
	// A wind regime (desert fields): each wind blows its share of every wind_period steps in turn; empty: wind_x/z.
	struct Wind {
		double x, z, share;
	};
	std::vector<Wind> winds;
	int wind_period = 40;
	bool periodic = false; // the grid wraps (a sand sea: what blows out one side comes in the other)
	std::vector<float> initial; // m of sand at the start (empty: none)

	std::vector<float> sand; // m of sand over the base
	std::vector<float> plants; // 0..1 cover
	std::vector<uint8_t> shadow;
	std::vector<float> speed; // the wind's speed-up: 1 on the flat, more over crests
	std::vector<float> burial; // m a step of net deposition, averaged over about 30 steps
	double lost = 0.0; // m3 carried out of the area or into the water

	void run(int p_steps) {
		const int n = w * h;
		if (int(initial.size()) == n) {
			sand = initial;
		} else {
			sand.assign(n, 0.0f);
		}
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
			bool turned = false;
			if (!winds.empty()) {
				// The wind of this step in the cycle.
				double total = 0.0;
				for (const Wind &wd : winds) {
					total += std::max(wd.share, 0.0);
				}
				const double t = double(step % std::max(wind_period, 1)) / std::max(wind_period, 1) * total;
				double acc = 0.0;
				const Wind *pick = &winds.back();
				for (const Wind &wd : winds) {
					acc += std::max(wd.share, 0.0);
					if (t < acc) {
						pick = &wd;
						break;
					}
				}
				turned = pick->x != wind_x || pick->z != wind_z;
				wind_x = pick->x;
				wind_z = pick->z;
			}
			if (turned || step % p.shadow_every == 0) {
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
		if (periodic) {
			const double wx = fx - std::floor(fx / w) * w, wz = fz - std::floor(fz / h) * h;
			const int x0 = std::min(int(wx), w - 1), z0 = std::min(int(wz), h - 1);
			const int x1 = (x0 + 1) % w, z1 = (z0 + 1) % h;
			const double tx = wx - x0, tz = wz - z0;
			return (height(z0 * w + x0) * (1.0 - tx) + height(z0 * w + x1) * tx) * (1.0 - tz) + (height(z1 * w + x0) * (1.0 - tx) + height(z1 * w + x1) * tx) * tz;
		}
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
					if (!periodic && (fx < 0.0 || fz < 0.0 || fx > w - 1 || fz > h - 1)) {
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
			int c[4];
			double tx, tz;
			if (periodic) {
				fx -= std::floor(fx / w) * w;
				fz -= std::floor(fz / h) * h;
				const int x0 = std::min(int(fx), w - 1), z0 = std::min(int(fz), h - 1);
				const int x1 = (x0 + 1) % w, z1 = (z0 + 1) % h;
				tx = fx - x0;
				tz = fz - z0;
				c[0] = z0 * w + x0;
				c[1] = z0 * w + x1;
				c[2] = z1 * w + x0;
				c[3] = z1 * w + x1;
			} else {
				if (fx < 0.0 || fz < 0.0 || fx > w - 1 || fz > h - 1) {
					lost += left * cell * cell;
					return;
				}
				const int x0 = std::min(int(fx), w - 2), z0 = std::min(int(fz), h - 2);
				tx = fx - x0;
				tz = fz - z0;
				c[0] = z0 * w + x0;
				c[1] = z0 * w + x0 + 1;
				c[2] = (z0 + 1) * w + x0;
				c[3] = (z0 + 1) * w + x0 + 1;
			}
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
						int jx = x + dx[k], jz = z + dz[k];
						if (periodic) {
							jx = (jx + w) % w;
							jz = (jz + h) % h;
						} else if (jx < 0 || jz < 0 || jx >= w || jz >= h) {
							continue;
						}
						const int j = jz * w + jx;
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


// Desert dune fields: Paris, Galin, Peytavie, Guérin and Gain's (CGF 2019) desertscape simulation, which grows
// transverse dunes from a thick sand cover, barchans from a thin one, and linear and star dunes from bimodal and wider
// wind regimes. A random cell lifts a slab (a quarter of a cell; unless the wind's shadow behind a crest, 5 to 15 degrees, holds it);
// the slab hops downwind, faster up slopes facing the wind, and settles at each landing with a chance (60% on sand,
// 40% on bare ground, certain in the shadow), up to three bounces; sand past the repose (33 degrees) slides off at the
// lift and the landing. The grid wraps (a sand sea).
// Lengths are in cells and heights in the same unit, so the dunes scale with the cell: angles hold, a coarser cell gives
// bigger dunes of the same shapes.
// Parallel by bands of rows (the sublattice decomposition of parallel kinetic Monte Carlo): each step cuts the grid
// into an even number of bands at a random offset, runs the even bands side by side, then the odd ones; a band reaches
// at most `reach` rows past its edges, and bands are at least twice that tall, so bands in the same phase never touch
// each other's cells. Each band draws from its own hash of seed, step and band, so a seed gives the same dunes for
// any number of threads.
class TerrainDuneSea {
public:
	struct Params {
		// A quarter of a cell, in height: Paris 2019's 10 cm on their 1 m cells, but on cells of several metres
		// 10%-of-a-cell slabs left the field immature in a few hundred steps (soft rolling lumps with no brinks or
		// slip faces); a quarter grows mature transverse ridges with crisp slip faces in the same steps.
		double slab = 0.25;
		int bounces = 3;
		double shadow_min_tan = 0.08; // about 5 degrees
		double shadow_max_tan = 0.26; // about 15 degrees
		double shadow_reach = 10.0; // cells
		double repose_tan = 0.6; // about 33 degrees
		double deposit_sand = 0.6;
		double deposit_bare = 0.4;
	};
	Params p;
	struct Wind {
		double x, z; // cells a hop (direction and strength)
		double share; // of the steps
	};
	std::vector<Wind> winds{ { 3.0, 0.0, 1.0 } };
	int wind_period = 10; // steps in a cycle of the regime
	uint64_t seed = 1;

	int w = 0, h = 0;
	std::vector<float> base; // in cells of height
	std::vector<float> sand; // in cells of height (the start on input)

	double height(int i) const { return double(base[i]) + double(sand[i]); }

	// Bands for this grid: 0 when it is too short to cut (run_step then runs it whole).
	int band_count() const {
		const int need = 2 * _reach() + 8;
		int k = h / std::max(need, 1);
		k -= k & 1;
		return k >= 2 ? k : 0;
	}

	// One step: every band of p_phase (0 even, 1 odd); with band_count() 0, the whole grid in phase 0.
	// Call phase 0 for all bands, then phase 1, for steps 0, 1, 2, ...
	void run_band(int p_step, int p_band) {
		const int k = band_count();
		const Wind &wd = _wind(p_step);
		Rng r{ _hash(seed, uint64_t(p_step) * 0x10001ull + uint64_t(p_band) + 1), 0, {} };
		if (k == 0) {
			Area a{ 0, h, false };
			for (int e = 0, n = w * h; e < n; e++) {
				_event(r, a, wd);
			}
			return;
		}
		const int off = int(_hash(seed ^ 0xA5A5A5A5ull, uint64_t(p_step)) % uint64_t(h));
		const int z0 = off + p_band * h / k, z1 = off + (p_band + 1) * h / k;
		const int reach = _reach();
		Area a{ z0 - reach, z1 + reach, true };
		for (int e = 0, n = w * (z1 - z0); e < n; e++) {
			_event(r, a, wd, z0, z1 - z0);
		}
	}

	// Serial: p_steps steps.
	void run(int p_steps) {
		const int k = band_count();
		for (int step = 0; step < p_steps; step++) {
			if (k == 0) {
				run_band(step, 0);
				continue;
			}
			for (int phase = 0; phase < 2; phase++) {
				for (int b = phase; b < k; b += 2) {
					run_band(step, b);
				}
			}
		}
	}

private:
	struct Rng {
		uint64_t key, counter;
		std::vector<int> queue; // the slides' work list, one per band
		uint64_t next() { return _hash(key, ++counter); }
		double uniform() { return double(next() >> 11) * (1.0 / 9007199254740992.0); }
	};
	// Rows an event may touch beyond its band: the hops (twice the wind on slopes), the shadow and the slides.
	struct Area {
		int z0, z1; // unwrapped rows
		bool limited;
	};
	static uint64_t _hash(uint64_t a, uint64_t b) {
		uint64_t z = a * 0x9E3779B97F4A7C15ull + b * 0xBF58476D1CE4E5B9ull;
		z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
		z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
		return z ^ (z >> 31);
	}
	int _wrap(int v, int m) const { return ((v % m) + m) % m; }

	int _reach() const {
		double sp = 0.0;
		for (const Wind &wd : winds) {
			sp = std::max(sp, std::sqrt(wd.x * wd.x + wd.z * wd.z));
		}
		return int(std::ceil(2.0 * sp * p.bounces + p.shadow_reach)) + 4;
	}

	// Whether wrapped row p_z lies in the area (rows z0 .. z1-1, unwrapped).
	bool _in(const Area &a, int p_z) const {
		if (!a.limited) {
			return true;
		}
		return _wrap(p_z - a.z0, h) < a.z1 - a.z0;
	}

	const Wind &_wind(int p_step) const {
		double total = 0.0;
		for (const Wind &wd : winds) {
			total += std::max(wd.share, 0.0);
		}
		const double t = double(p_step % std::max(wind_period, 1)) / std::max(wind_period, 1) * total;
		double acc = 0.0;
		for (const Wind &wd : winds) {
			acc += std::max(wd.share, 0.0);
			if (t < acc) {
				return wd;
			}
		}
		return winds.back();
	}

	// Faster up a slope of sand facing the wind, up to twice as fast (Paris 2019).
	void _wind_at(int x, int z, const Wind &p_w, double &r_x, double &r_z) const {
		const double gx = 0.5 * (sand[z * w + _wrap(x + 1, w)] - sand[z * w + _wrap(x - 1, w)]);
		const double gz = 0.5 * (sand[_wrap(z + 1, h) * w + x] - sand[_wrap(z - 1, h) * w + x]);
		const double gl = std::sqrt(gx * gx + gz * gz), wl = std::sqrt(p_w.x * p_w.x + p_w.z * p_w.z);
		double t = 0.0;
		if (gl > 0.0 && wl > 0.0) {
			const double sim = std::clamp((gx * p_w.x + gz * p_w.z) / (gl * wl), 0.0, 1.0);
			t = 0.5 * (sim + std::clamp(gl, 0.0, 1.0));
		}
		r_x = p_w.x * (1.0 + t);
		r_z = p_w.z * (1.0 + t);
	}

	// Bilinear height on the wrapped grid.
	double _height_at(double x, double z) const {
		const double fx = std::floor(x), fz = std::floor(z);
		const int x0 = _wrap(int(fx), w), z0 = _wrap(int(fz), h), x1 = _wrap(x0 + 1, w), z1 = _wrap(z0 + 1, h);
		const double tx = x - fx, tz = z - fz;
		const double a = height(z0 * w + x0) * (1.0 - tx) + height(z0 * w + x1) * tx;
		const double b = height(z1 * w + x0) * (1.0 - tx) + height(z1 * w + x1) * tx;
		return a * (1.0 - tz) + b * tz;
	}

	// The shadow: the steepest rise upwind within reach, marching a cell at a time straight up the wind
	// (no octant snapping, so every wind direction casts the same shadow).
	double _shadow(int x, int z, double wx, double wz) const {
		const double wl = std::sqrt(wx * wx + wz * wz);
		if (wl <= 0.0) {
			return 0.0;
		}
		const double ux = wx / wl, uz = wz / wl;
		const double hp = height(z * w + x);
		double best = 0.0;
		const int reach = int(p.shadow_reach);
		for (int k = 1; k <= reach; k++) {
			const double t = (_height_at(x - ux * k, z - uz * k) - hp) / k;
			best = std::max(best, std::clamp((t - p.shadow_min_tan) / (p.shadow_max_tan - p.shadow_min_tan), 0.0, 1.0));
		}
		return best;
	}

	// Sand past the repose slides to the lower neighbours, a slab at a time, until it stands (or reaches the
	// area's edge rows, which a later step settles).
	void _stabilize(Rng &r, const Area &p_a, int p_i) {
		std::vector<int> &queue = r.queue;
		static const int dx[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };
		static const int dz[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
		static const double len[8] = { 1.0, 1.41421356, 1.0, 1.41421356, 1.0, 1.41421356, 1.0, 1.41421356 };
		Area inner{ p_a.z0 + 1, p_a.z1 - 1, p_a.limited };
		queue.clear();
		queue.push_back(p_i);
		size_t head = 0;
		while (head < queue.size() && head < 4096) {
			const int c = queue[head++];
			if (sand[c] <= 0.0f) {
				continue;
			}
			const int x = c % w, z = c / w;
			if (!_in(inner, z)) {
				continue;
			}
			const double hc = height(c);
			int nb[8];
			double sl[8], sum = 0.0;
			int m = 0;
			for (int k = 0; k < 8; k++) {
				const int j = _wrap(z + dz[k], h) * w + _wrap(x + dx[k], w);
				const double st = hc - height(j);
				if (st > 0.0 && st / len[k] > p.repose_tan) {
					nb[m] = j;
					sl[m] = st / len[k];
					sum += sl[m];
					m++;
				}
			}
			if (m == 0) {
				continue;
			}
			const float a = std::min(sand[c], float(p.slab));
			for (int k = 0; k < m; k++) {
				sand[nb[k]] += float(a * sl[k] / sum);
				queue.push_back(nb[k]);
			}
			sand[c] -= a;
		}
	}

	void _event(Rng &r, const Area &p_a, const Wind &p_w, int p_z0 = 0, int p_rows = 0) {
		const int sx = int(r.next() % uint64_t(w));
		const int sz = p_rows > 0 ? _wrap(p_z0 + int(r.next() % uint64_t(p_rows)), h) : int(r.next() % uint64_t(h));
		const int s0 = sz * w + sx;
		double wx, wz;
		_wind_at(sx, sz, p_w, wx, wz);
		if (sand[s0] <= 0.0f) {
			return;
		}
		if (r.uniform() < _shadow(sx, sz, wx, wz)) {
			_stabilize(r, p_a, s0);
			return;
		}
		const float a = std::min(sand[s0], float(p.slab));
		sand[s0] -= a;
		// A random start inside the cell, so hops shorter than a cell or off the grid axes still land evenly.
		double px = sx + r.uniform(), pz = sz + r.uniform();
		int dx = sx, dz = sz, d = s0;
		for (int bounce = 0; bounce < p.bounces; bounce++) {
			_wind_at(dx, dz, p_w, wx, wz);
			px += wx;
			pz += wz;
			px -= std::floor(px / w) * w;
			pz -= std::floor(pz / h) * h;
			dx = std::min(int(px), w - 1);
			dz = std::min(int(pz), h - 1);
			d = dz * w + dx;
			const double u = r.uniform();
			if (u < _shadow(dx, dz, wx, wz) || u < (sand[d] > 0.0f ? p.deposit_sand : p.deposit_bare)) {
				break;
			}
		}
		sand[d] += a; // settled, or dropped after the last bounce
		_stabilize(r, p_a, s0);
		if (d != s0) {
			_stabilize(r, p_a, d);
		}
	}
};

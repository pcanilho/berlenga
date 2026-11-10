/**************************************************************************/
/*  terrain_coast.h                                                       */
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

// Coast and lakes on the generator's grid (docs/ideas/terrain-generation.md, "Coast and lakes"). Plain C++, single
// thread, fixed order.
//
// Distance: two-pass chamfer distance transform (Borgefors 1986; 3x3 mask with steps 1 and sqrt 2), used for the
// distance offshore (seabed profile) and inland (beach).
//
// Lakes: a stream power landscape drains completely (every pit is filled as it forms), so lakes need a dam. Natural
// lakes in mountain valleys mostly sit behind one: a moraine, a landslide or a rock bar across the valley floor
// (Costa and Schuster 1988; Korup 2002). make_lake() raises such a dam across the valley at a channel cell, with a
// spillway notch on the old channel, then floods the valley above it in height order (Priority-Flood, Barnes,
// Lehman and Mulla 2014) up to the dam's crest or the lowest col on the basin's rim, whichever is lower: the "spill"
// of Fill-Spill-Merge (Barnes, Callaghan and Wickert 2021). A lake whose catchment cannot make up its evaporation
// stands lower and has no outflow (the same water balance: runoff times catchment equals evaporation times area).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <queue>
#include <vector>

class TerrainCoast {
public:
	// Metres to the nearest cell whose mask is non-zero (0 on those cells); a large value when there is none.
	static void distance(int nx, int ny, double dx, const uint8_t *p_mask, std::vector<double> &r_dist) {
		const int n = nx * ny;
		const double big = 1.0e30;
		const double d1 = 1.0;
		const double d2 = 1.4142135623730951;
		r_dist.assign(n, big);
		for (int i = 0; i < n; i++) {
			if (p_mask[i]) {
				r_dist[i] = 0.0;
			}
		}
		double *d = r_dist.data();
		for (int z = 0; z < ny; z++) {
			for (int x = 0; x < nx; x++) {
				double &v = d[z * nx + x];
				if (x > 0) {
					v = std::min(v, d[z * nx + x - 1] + d1);
				}
				if (z > 0) {
					v = std::min(v, d[(z - 1) * nx + x] + d1);
					if (x > 0) {
						v = std::min(v, d[(z - 1) * nx + x - 1] + d2);
					}
					if (x + 1 < nx) {
						v = std::min(v, d[(z - 1) * nx + x + 1] + d2);
					}
				}
			}
		}
		for (int z = ny - 1; z >= 0; z--) {
			for (int x = nx - 1; x >= 0; x--) {
				double &v = d[z * nx + x];
				if (x + 1 < nx) {
					v = std::min(v, d[z * nx + x + 1] + d1);
				}
				if (z + 1 < ny) {
					v = std::min(v, d[(z + 1) * nx + x] + d1);
					if (x + 1 < nx) {
						v = std::min(v, d[(z + 1) * nx + x + 1] + d2);
					}
					if (x > 0) {
						v = std::min(v, d[(z + 1) * nx + x - 1] + d2);
					}
				}
			}
		}
		for (int i = 0; i < n; i++) {
			if (d[i] < big) {
				d[i] *= dx;
			}
		}
	}

	struct LakeParams {
		int nx = 0;
		int ny = 0;
		double dx = 16.0; // m
		double dam_height = 20.0; // m above the valley floor at the dam
		double freeboard = 3.0; // m of dam above the spillway, at its middle
		double dam_half_width = 200.0; // m across the valley at full height
		double face_slope = 0.35; // downstream face, rise over run
		double back_slope = 0.2; // upstream face, under the water
		double spill_slope = 0.04; // the outflow channel cut through the dam
		double min_area = 20000.0; // m^2
		double runoff = 1.0; // m/yr over the catchment
		double evaporation = 1.0; // m/yr off the lake
		int max_cells = 1 << 20;
	};

	struct Lake {
		std::vector<int32_t> cells; // under water
		double level = 0.0;
		double bed = 0.0; // lowest ground under it
		int32_t dam = -1; // the dam's spillway cell
		int32_t outlet = -1; // the dry cell the water leaves by (the spillway or a col); -1 when it has no outflow
		int32_t spill_to = -1; // the cell below the outlet
		double catchment = 0.0; // m^2
		bool endorheic = false;
	};

	// A dam at p_site and the lake behind it, modifying p_h. p_rec: receivers (downstream neighbour, itself at an
	// outlet); p_area: drainage area along them (m^2); p_taken: cells that must stay as they are (sea, other lakes and
	// dams). Returns false (and leaves p_h as it was) when no lake of min_area forms.
	static bool make_lake(const LakeParams &p, int32_t p_site, const int32_t *p_rec, const float *p_area, const uint8_t *p_taken, float *p_h, Lake &r_lake) {
		const int nx = p.nx;
		const int ny = p.ny;
		const int n = nx * ny;
		if (p_site < 0 || p_site >= n || p_rec[p_site] == p_site || p_taken[p_site]) {
			return false;
		}
		// Down-valley direction: towards the cell four steps down the receivers.
		int32_t e = p_site;
		for (int k = 0; k < 4 && p_rec[e] != e; k++) {
			e = p_rec[e];
		}
		const int sx = p_site % nx;
		const int sz = p_site / nx;
		double dirx = double(e % nx - sx);
		double dirz = double(e / nx - sz);
		const double dl = std::sqrt(dirx * dirx + dirz * dirz);
		if (dl < 1e-9) {
			return false;
		}
		dirx /= dl;
		dirz /= dl;
		const std::vector<float> h0(p_h, p_h + n);
		const double floor_h = h0[p_site];
		const double target = floor_h + p.dam_height;
		const double top = target + p.freeboard;
		auto dam_height_at = [&](int x, int z) -> double {
			const double ox = (x - sx) * p.dx;
			const double oz = (z - sz) * p.dx;
			const double along = ox * dirx + oz * dirz;
			const double lat = std::fabs(-ox * dirz + oz * dirx);
			const double w = p.dam_half_width;
			// Crest: rounded along the valley, full height across the floor, sloping down past the half width.
			const double crest = lat <= w ? top - p.freeboard * (lat / w) * (lat / w) : target - p.face_slope * (lat - w);
			return crest - (along >= 0.0 ? p.face_slope : p.back_slope) * std::fabs(along);
		};
		auto restore = [&]() {
			memcpy(p_h, h0.data(), sizeof(float) * n);
		};
		// The dam.
		const double reach = p.dam_half_width + (top - floor_h + 50.0) / std::min(p.face_slope, p.back_slope);
		const int rc = int(std::ceil(reach / p.dx));
		for (int z = std::max(sz - rc, 0); z <= std::min(sz + rc, ny - 1); z++) {
			for (int x = std::max(sx - rc, 0); x <= std::min(sx + rc, nx - 1); x++) {
				const int i = z * nx + x;
				if (p_taken[i]) {
					continue;
				}
				const double hd = dam_height_at(x, z);
				if (hd > p_h[i]) {
					p_h[i] = float(hd);
				}
			}
		}
		// The spillway: the old channel through the dam, from the crest at the target level down to where it meets
		// the valley floor again.
		std::vector<int32_t> notch;
		{
			int32_t c = p_site;
			double run = 0.0;
			p_h[c] = float(target);
			notch.push_back(c);
			while (p_rec[c] != c) {
				const int32_t r = p_rec[c];
				const double ddx = double(r % nx - c % nx);
				const double ddz = double(r / nx - c / nx);
				run += std::sqrt(ddx * ddx + ddz * ddz) * p.dx;
				const double hn = target - p.spill_slope * run;
				if (hn <= h0[r] || p_taken[r]) {
					break;
				}
				p_h[r] = float(std::min(double(p_h[r]), hn));
				notch.push_back(r);
				c = r;
			}
		}
		static const int DX[8] = { -1, 0, 1, -1, 1, -1, 0, 1 };
		static const int DZ[8] = { -1, -1, -1, 0, 0, 1, 1, 1 };
		// The dam site's own basin (everything draining through it): ground there lower than the water so far is a
		// hollow the lake reaches later, not a way out.
		std::vector<uint8_t> basin(n, 0);
		{
			std::vector<int32_t> todo;
			todo.push_back(p_site);
			basin[p_site] = 1;
			for (size_t q = 0; q < todo.size(); q++) {
				const int32_t c = todo[q];
				const int x = c % nx;
				const int z = c / nx;
				for (int k = 0; k < 8; k++) {
					const int xx = x + DX[k];
					const int zz = z + DZ[k];
					if (xx < 0 || zz < 0 || xx >= nx || zz >= ny) {
						continue;
					}
					const int j = zz * nx + xx;
					if (!basin[j] && p_rec[j] == c) {
						basin[j] = 1;
						todo.push_back(j);
					}
				}
			}
		}
		// Flood up the valley in height order from above the dam.
		std::vector<uint8_t> closed(n, 0);
		typedef std::pair<float, int32_t> Item;
		std::priority_queue<Item, std::vector<Item>, std::greater<Item>> heap;
		for (int32_t c : notch) {
			closed[c] = 1;
		}
		// Seed: up the trunk (the donor of most area) to the first cell the dam's back face left as it was.
		int32_t seed = -1;
		{
			int32_t c = p_site;
			for (int steps = 0; steps < 4096 && seed < 0; steps++) {
				int32_t best = -1;
				const int x = c % nx;
				const int z = c / nx;
				for (int k = 0; k < 8; k++) {
					const int xx = x + DX[k];
					const int zz = z + DZ[k];
					if (xx < 0 || zz < 0 || xx >= nx || zz >= ny) {
						continue;
					}
					const int j = zz * nx + xx;
					if (p_rec[j] == c && j != c && (best < 0 || p_area[j] > p_area[best])) {
						best = j;
					}
				}
				if (best < 0 || p_taken[best]) {
					break;
				}
				if (p_h[best] == h0[best]) {
					seed = best;
				}
				c = best;
			}
		}
		if (seed < 0) {
			restore();
			return false;
		}
		closed[seed] = 1;
		heap.push(Item(p_h[seed], seed));
		std::vector<int32_t> popped;
		double level = -1e30;
		int32_t rim = -1;
		int32_t spill = -1;
		bool full = false;
		while (!heap.empty()) {
			const Item it = heap.top();
			heap.pop();
			const int32_t c = it.second;
			if (double(it.first) >= target) {
				full = true;
				break;
			}
			if (p_taken[c] || (double(it.first) < level - 1e-4 && !basin[c])) {
				spill = c; // water runs down from the rim here: into the sea, another lake or a lower valley
				break;
			}
			if (double(it.first) > level) {
				level = it.first;
				rim = c;
			}
			popped.push_back(c);
			if (int(popped.size()) > p.max_cells) {
				restore();
				return false;
			}
			const int x = c % nx;
			const int z = c / nx;
			for (int k = 0; k < 8; k++) {
				const int xx = x + DX[k];
				const int zz = z + DZ[k];
				if (xx < 0 || zz < 0 || xx >= nx || zz >= ny) {
					continue;
				}
				const int j = zz * nx + xx;
				if (!closed[j]) {
					closed[j] = 1;
					heap.push(Item(p_h[j], j));
				}
			}
		}
		if (heap.empty() && !full && spill < 0) {
			full = true; // closed by the grid's edge: the dam holds it
		}
		Lake lake;
		if (full) {
			lake.level = target;
			lake.outlet = p_site;
			lake.spill_to = p_rec[p_site];
		} else {
			if (rim < 0) {
				restore();
				return false;
			}
			// Spills over a col lower than the dam: the col is the outlet, and the dam needs no spillway.
			lake.outlet = rim;
			lake.spill_to = spill;
			for (int32_t c : notch) {
				p_h[c] = float(std::max(double(h0[c]), dam_height_at(c % nx, c / nx)));
			}
		}
		const double lv = full ? target : level;
		std::vector<float> under;
		for (int32_t c : popped) {
			if (double(p_h[c]) < lv - 0.01) {
				under.push_back(p_h[c]);
			}
		}
		// Water balance: the catchment's runoff against the lake's evaporation; a dry basin's lake stands where they
		// match, with no outflow.
		lake.catchment = p_area[p_site];
		double lake_level = lv;
		const double inflow = p.runoff * lake.catchment;
		if (p.evaporation > 0.0 && inflow < p.evaporation * double(under.size()) * p.dx * p.dx) {
			std::sort(under.begin(), under.end());
			const size_t k = size_t(inflow / (p.evaporation * p.dx * p.dx));
			if (k < 2 || k >= under.size()) {
				restore();
				return false;
			}
			lake_level = under[k];
			lake.endorheic = true;
			lake.outlet = -1;
			lake.spill_to = -1;
		}
		lake.level = lake_level;
		lake.dam = p_site;
		lake.bed = lake_level;
		for (int32_t c : popped) {
			if (double(p_h[c]) < lake_level - 0.01) {
				lake.cells.push_back(c);
				lake.bed = std::min(lake.bed, double(p_h[c]));
			}
		}
		if (double(lake.cells.size()) * p.dx * p.dx < p.min_area) {
			restore();
			return false;
		}
		r_lake = std::move(lake);
		return true;
	}
};

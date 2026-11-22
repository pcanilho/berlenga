/**************************************************************************/
/*  terrain_geology.h                                                     */
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

// Geology maps and river network of an eroded heightfield (stage 5 of docs/ideas/terrain-generation.md). Plain C++
// over the grids the stream power run leaves: heights, drainage area, basin fill, D8 receivers and the stack
// (every node after its receiver).
//
// Soil: the exponential soil production function (Heimsath, Dietrich, Nishiizumi and Finkel 1997): bare rock weathers
// into soil at P0, slowing as e^(-h/h0) under a soil of depth h. Where the ground lowers at a rate E, soil settles where
// production meets it: h = h0 ln(P0 / E). E is the local erosion rate of the same laws the run used: channel incision
// K A^m S plus hillslope creep D * (-curvature) on convex ground. Slopes near the critical slope shed their soil
// (landslides). Basin fill (sediment) adds to it. Channels gentle for their drainage area are alluvial: their channel
// steepness index k_s = S A^m (Wobus et al. 2006) falls well below the k_s = U / K of the fastest-uplifting ground, so
// they carry more sediment than they can move and floor their valleys with it (bedrock against alluvial channels,
// Montgomery et al. 1996); they get a full loose cover.
// Rock: exposed where the loose cover is thinner than rock_cover.
// Talus: scree shed by bare rock steeper than the angle of repose, carried down the receiver chain and laid where the
// slope eases below repose (above about 7 degrees), fading with distance from the cliff.
// Rivers: channels are nodes draining more than a minimum area. Each river runs from an outlet (or a junction)
// upstream along the donor with the largest area (Hack's main stem); the other donors start tributaries that join
// it, so trunk rivers come first and every tributary names the river it joins.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

class TerrainGeology {
public:
	struct Params {
		int nx = 0;
		int ny = 0;
		double dx = 1.0;
		double erodibility = 2e-5; // K, as the erosion run
		double area_exponent = 0.5; // m
		double diffusion = 0.01; // D, m^2/yr
		double critical_slope = 0.84; // tan
		double soil_production = 1e-4; // P0, m/yr on bare rock
		double soil_scale = 0.5; // h0, m
		double soil_max = 2.0; // m
		double rock_cover = 0.3; // m of loose cover that hides the rock
		double repose = 0.7; // tan of the angle of repose
		double talus_reach = 120.0; // m below a cliff over which scree thins out
		double steepness_ref = 100.0; // channel steepness S A^m of the fastest-uplifting ground (U_max / K at steady state)
		double alluvial_steepness = 0.2; // channels below this fraction of steepness_ref are alluvial (gravel floors)
	};

	struct River {
		std::vector<int32_t> cells; // head to mouth; a tributary's last cell is the junction on the river it joins
		int joins = -1; // index of the river it joins, -1 at an outlet
	};

	static double smoothstep(double a, double b, double x) {
		const double t = std::min(std::max((x - a) / (b - a), 0.0), 1.0);
		return t * t * (3.0 - 2.0 * t);
	}

	// Outputs: soil (loose cover depth, m), rock (exposure 0..1), talus (0..1).
	static void compute(const Params &p, const float *h, const float *area, const float *sed, const int32_t *rec, const int32_t *stack, float *r_soil, float *r_rock, float *r_talus) {
		const int nx = p.nx;
		const int ny = p.ny;
		const int n = nx * ny;
		const double dx = p.dx;
		std::vector<float> slope(n);
		std::vector<float> src(n);
		for (int z = 0; z < ny; z++) {
			for (int x = 0; x < nx; x++) {
				const int i = z * nx + x;
				const double hi = h[i];
				// Steepest descent to any of the eight neighbours.
				double s = 0.0;
				for (int dz = -1; dz <= 1; dz++) {
					for (int ddx = -1; ddx <= 1; ddx++) {
						const int xx = x + ddx;
						const int zz = z + dz;
						if ((ddx == 0 && dz == 0) || xx < 0 || zz < 0 || xx >= nx || zz >= ny) {
							continue;
						}
						const double d = (ddx != 0 && dz != 0) ? dx * 1.4142135623730951 : dx;
						s = std::max(s, (hi - h[zz * nx + xx]) / d);
					}
				}
				slope[i] = float(s);
				auto at = [&](int xx, int zz) -> double {
					xx = std::min(std::max(xx, 0), nx - 1);
					zz = std::min(std::max(zz, 0), ny - 1);
					return h[zz * nx + xx];
				};
				const double lap = (at(x - 1, z) + at(x + 1, z) + at(x, z - 1) + at(x, z + 1) - 4.0 * hi) / (dx * dx);
				double incision = 0.0;
				const int r = rec[i];
				if (r != i) {
					const int rx = r % nx;
					const int rz = r / nx;
					const double d = (rx != x && rz != z) ? dx * 1.4142135623730951 : dx;
					incision = p.erodibility * std::pow(double(area[i]), p.area_exponent) * std::max((hi - h[r]) / d, 0.0);
				}
				const double e = incision + p.diffusion * std::max(-lap, 0.0);
				double soil = p.soil_scale * std::log(p.soil_production / std::max(e, 1e-12));
				soil = std::min(std::max(soil, 0.0), p.soil_max);
				soil *= 1.0 - smoothstep(0.6 * p.critical_slope, 0.95 * p.critical_slope, s);
				// Alluvial valley floors: gentle channels for their area, fading in over one decade of area.
				const double cell_area = dx * dx;
				const double channel = smoothstep(std::log(20.0 * cell_area), std::log(200.0 * cell_area), std::log(std::max(double(area[i]), 1.0)));
				if (channel > 0.0 && r != i) {
					const int rx = r % nx;
					const int rz = r / nx;
					const double d = (rx != x && rz != z) ? dx * 1.4142135623730951 : dx;
					const double ks = std::max((hi - h[r]) / d, 0.0) * std::pow(double(area[i]), p.area_exponent);
					const double alluvium = p.soil_max * channel * (1.0 - smoothstep(0.5 * p.alluvial_steepness, p.alluvial_steepness, ks / p.steepness_ref));
					soil = std::max(soil, alluvium);
				}
				const double loose = soil + std::max(double(sed[i]), 0.0);
				r_soil[i] = float(loose);
				r_rock[i] = float(1.0 - smoothstep(0.0, p.rock_cover, loose));
				src[i] = float(r_rock[i] * smoothstep(0.9 * p.repose, 1.2 * p.repose, s));
			}
		}
		// Scree runs down the receiver chain from every cliff, thinning with distance. The stack lists every node
		// after its receiver, so walking it backwards visits donors first.
		std::vector<float> carry(src);
		for (int k = n - 1; k >= 0; k--) {
			const int i = stack[k];
			const int r = rec[i];
			if (r == i) {
				continue;
			}
			const int ix = i % nx;
			const int rx = r % nx;
			const double d = (ix != rx && i / nx != r / nx) ? dx * 1.4142135623730951 : dx;
			carry[r] = std::max(carry[r], float(carry[i] * std::exp(-d / p.talus_reach)));
		}
		// Laid where the slope eases below repose, gone on gentle ground; a 3x3 box spreads the D8 lines into fans.
		std::vector<float> lay(n);
		for (int i = 0; i < n; i++) {
			const double s = slope[i];
			// Streams carry it off: none on channels.
			const double channel = smoothstep(std::log(5.0 * dx * dx), std::log(50.0 * dx * dx), std::log(std::max(double(area[i]), 1.0)));
			lay[i] = float(carry[i] * smoothstep(0.12, 0.3, s) * (1.0 - smoothstep(p.repose, 1.15 * p.repose, s)) * (1.0 - channel));
		}
		for (int z = 0; z < ny; z++) {
			for (int x = 0; x < nx; x++) {
				double sum = 0.0;
				int cnt = 0;
				for (int dz = -1; dz <= 1; dz++) {
					for (int ddx = -1; ddx <= 1; ddx++) {
						const int xx = x + ddx;
						const int zz = z + dz;
						if (xx < 0 || zz < 0 || xx >= nx || zz >= ny) {
							continue;
						}
						sum += lay[zz * nx + xx];
						cnt++;
					}
				}
				const int i = z * nx + x;
				const float t = std::min(float(sum / cnt) * 2.0f, 1.0f);
				r_talus[i] = t;
				r_rock[i] *= 1.0f - t;
			}
		}
	}

	// Area drained through the receiver tree (D8), so it never falls downstream (an MFD area can, where flow
	// splits). p_cell_area per node.
	static void d8_area(int n, const int32_t *rec, const int32_t *stack, double p_cell_area, float *r_area) {
		for (int i = 0; i < n; i++) {
			r_area[i] = float(p_cell_area);
		}
		for (int k = n - 1; k >= 0; k--) {
			const int i = stack[k];
			if (rec[i] != i) {
				r_area[rec[i]] += r_area[i];
			}
		}
	}

	// Rivers draining at least p_min_area (by a D8 area, so it grows downstream), trunk rivers first.
	static void rivers(int n, const float *area, const int32_t *rec, double p_min_area, std::vector<River> &r_out) {
		r_out.clear();
		std::vector<int32_t> count(n + 1, 0);
		std::vector<int32_t> outlets;
		for (int i = 0; i < n; i++) {
			if (area[i] < p_min_area) {
				continue;
			}
			if (rec[i] == i) {
				outlets.push_back(i);
			} else {
				count[rec[i] + 1]++;
			}
		}
		for (int i = 0; i < n; i++) {
			count[i + 1] += count[i];
		}
		std::vector<int32_t> donors(count[n]);
		std::vector<int32_t> fill(count.begin(), count.end() - 1);
		for (int i = 0; i < n; i++) {
			if (area[i] >= p_min_area && rec[i] != i) {
				donors[fill[rec[i]]++] = i;
			}
		}
		std::sort(outlets.begin(), outlets.end(), [&](int a, int b) { return area[a] > area[b] || (area[a] == area[b] && a < b); });
		struct Start {
			int32_t cell;
			int parent;
			int32_t junction;
		};
		std::vector<Start> queue;
		for (int32_t o : outlets) {
			queue.push_back({ o, -1, -1 });
		}
		for (size_t q = 0; q < queue.size(); q++) {
			const Start st = queue[q];
			const int index = int(r_out.size());
			River river;
			river.joins = st.parent;
			int32_t cur = st.cell;
			river.cells.push_back(cur);
			while (true) {
				int32_t best = -1;
				for (int k = count[cur]; k < count[cur + 1]; k++) {
					const int32_t d = donors[k];
					if (best < 0 || area[d] > area[best] || (area[d] == area[best] && d < best)) {
						best = d;
					}
				}
				for (int k = count[cur]; k < count[cur + 1]; k++) {
					if (donors[k] != best) {
						queue.push_back({ donors[k], index, cur });
					}
				}
				if (best < 0) {
					break;
				}
				river.cells.push_back(best);
				cur = best;
			}
			std::reverse(river.cells.begin(), river.cells.end());
			if (st.junction >= 0) {
				river.cells.push_back(st.junction);
			}
			r_out.push_back(std::move(river));
		}
	}
};

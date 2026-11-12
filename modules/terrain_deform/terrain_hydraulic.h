/**************************************************************************/
/*  terrain_hydraulic.h                                                   */
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

// Per-tile hydraulic and thermal erosion on a fine grid (stage 4 of docs/ideas/terrain-generation.md). Plain C++,
// single thread, droplets in a fixed order from a seeded hash, so a tile's result depends only on its input.
//
// Water: droplet (particle) erosion as described by Beyer ("Implementation of a method for hydraulic erosion", 2015;
// after Mei et al. 2007's capacity law): each droplet runs downhill with inertia over the bilinear surface; its
// capacity is max(drop, min_slope) * speed * water * Kc; below capacity it erodes the ground under it over a small
// radius, above it deposits at its position; it evaporates as it goes. Chosen over the virtual pipe model (Mei,
// Decaudin and Hu 2007), which on steep mountain slopes left grooves along the grid axes in the bench. Layers (Stava,
// Galin, Benes, Brisbin and Krivanek 2008, two of them): loose cover (soil, scree, settled sediment) erodes at the
// full rate, bedrock under it at rock_hardness times that. Thermal: loose material on slopes steeper than repose
// slides to the lower neighbours in proportion to the excess (Musgrave, Kolb and Mace 1989; eight neighbours as in
// Olsen 2004).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

class TerrainHydraulic {
public:
	struct Params {
		int nx = 0;
		int ny = 0;
		double dx = 2.0; // m
		uint32_t seed = 0;
		double droplets_per_cell = 1.0;
		int max_steps = 64; // per droplet
		double inertia = 0.05;
		double capacity = 4.0; // Kc
		double min_slope = 0.01;
		double erosion = 0.3; // share of the missing capacity taken per step
		double deposition = 0.3; // share of the excess laid down per step
		double evaporation = 0.02; // per step
		double gravity = 4.0;
		int radius = 2; // cells, erosion brush
		double rock_hardness = 0.25; // bedrock erodes at this share of the loose rate
		double repose = 0.7; // tan, thermal erosion of loose material
		double max_depth = 15.0; // m: deepest cut and highest fill against the input, so channels cannot run away
		int thermal_passes = 8;
		double thermal_rate = 0.25; // share of the excess moved per pass
	};

	static uint32_t hash(uint32_t x) {
		x ^= x >> 16;
		x *= 0x7FEB352Du;
		x ^= x >> 15;
		x *= 0x846CA68Bu;
		x ^= x >> 16;
		return x;
	}

	// Inputs: ground (n, m) and loose cover depth (n, m; bedrock below). Outputs: r_ground (n), r_flow (n: droplet
	// water passing through, a stream and wetness measure), r_deposit (n: sediment laid down, m, >= 0).
	static void run(const Params &p, const float *ground, const float *cover, float *r_ground, float *r_flow, float *r_deposit) {
		const int nx = p.nx;
		const int ny = p.ny;
		const int n = nx * ny;
		// Heights in cells (m / dx), so slopes are rise per cell as the droplet law expects.
		std::vector<double> h(n);
		std::vector<double> loose(n);
		std::vector<double> flow(n, 0.0);
		for (int i = 0; i < n; i++) {
			h[i] = ground[i] / p.dx;
			loose[i] = std::max(double(cover[i]), 0.0) / p.dx;
		}
		// Erosion brush: weights over a disc, normalized.
		std::vector<int> bx, bz;
		std::vector<double> bw;
		double bsum = 0.0;
		for (int z = -p.radius; z <= p.radius; z++) {
			for (int x = -p.radius; x <= p.radius; x++) {
				const double r = std::sqrt(double(x * x + z * z));
				if (r <= p.radius) {
					bx.push_back(x);
					bz.push_back(z);
					bw.push_back(1.0 - r / (p.radius + 1.0));
					bsum += bw.back();
				}
			}
		}
		for (double &w : bw) {
			w /= bsum;
		}
		auto height_grad = [&](double px, double pz, double &r_h, double &r_gx, double &r_gz) {
			const int x0 = int(px);
			const int z0 = int(pz);
			const double fx = px - x0;
			const double fz = pz - z0;
			const int i = z0 * nx + x0;
			const double h00 = h[i], h10 = h[i + 1], h01 = h[i + nx], h11 = h[i + nx + 1];
			r_gx = (h10 - h00) * (1.0 - fz) + (h11 - h01) * fz;
			r_gz = (h01 - h00) * (1.0 - fx) + (h11 - h10) * fx;
			r_h = h00 * (1.0 - fx) * (1.0 - fz) + h10 * fx * (1.0 - fz) + h01 * (1.0 - fx) * fz + h11 * fx * fz;
		};
		// Takes up to p_amount (cells of height) at a cell: loose first, then rock at its hardness. Returns the taken.
		const double limit = p.max_depth / p.dx;
		auto take = [&](int i, double p_amount) {
			// Softly: the rate falls to zero as the cut nears max_depth.
			const double cut = ground[i] / p.dx - h[i];
			p_amount *= std::min(std::max(1.0 - cut / limit, 0.0), 1.0);
			const double a = std::min(p_amount, loose[i]);
			loose[i] -= a;
			const double rock = (p_amount - a) * p.rock_hardness;
			h[i] -= a + rock;
			return a + rock;
		};
		const int64_t count = int64_t(p.droplets_per_cell * n);
		for (int64_t k = 0; k < count; k++) {
			const uint32_t r1 = hash(uint32_t(k) * 2u + 1u + p.seed * 0x9E3779B9u);
			const uint32_t r2 = hash(r1 ^ 0x68E31DA4u);
			double px = (r1 & 0xffffff) / double(0x1000000) * (nx - 2);
			double pz = (r2 & 0xffffff) / double(0x1000000) * (ny - 2);
			double dirx = 0.0, dirz = 0.0, speed = 1.0, water = 1.0, sed = 0.0;
			for (int step = 0; step < p.max_steps; step++) {
				const int cx = int(px);
				const int cz = int(pz);
				const int ci = cz * nx + cx;
				const double fx = px - cx;
				const double fz = pz - cz;
				double hh, gx, gz;
				height_grad(px, pz, hh, gx, gz);
				dirx = dirx * p.inertia - gx * (1.0 - p.inertia);
				dirz = dirz * p.inertia - gz * (1.0 - p.inertia);
				const double len = std::sqrt(dirx * dirx + dirz * dirz);
				if (len < 1e-12) {
					break;
				}
				dirx /= len;
				dirz /= len;
				px += dirx;
				pz += dirz;
				if (px < 0.0 || pz < 0.0 || px >= nx - 1 || pz >= ny - 1) {
					break; // left the tile: its load leaves with it
				}
				flow[ci] += water;
				double nh, ngx, ngz;
				height_grad(px, pz, nh, ngx, ngz);
				const double dh = nh - hh;
				const double cap = std::max(-dh, p.min_slope) * speed * water * p.capacity;
				if (sed > cap || dh > 0.0) {
					// Fill the pit (uphill step) or lay down the excess, bilinear at the old position.
					double put = dh > 0.0 ? std::min(dh, sed) : (sed - cap) * p.deposition;
					put = std::min(put, std::max(ground[ci] / p.dx + limit - h[ci], 0.0));
					sed -= put;
					const double w00 = (1.0 - fx) * (1.0 - fz), w10 = fx * (1.0 - fz), w01 = (1.0 - fx) * fz, w11 = fx * fz;
					const int is[4] = { ci, ci + 1, ci + nx, ci + nx + 1 };
					const double ws[4] = { w00, w10, w01, w11 };
					for (int m = 0; m < 4; m++) {
						h[is[m]] += put * ws[m];
						loose[is[m]] += put * ws[m];
					}
				} else {
					// Erode no more than the drop, over the brush.
					const double want = std::min((cap - sed) * p.erosion, -dh);
					double got = 0.0;
					for (size_t b = 0; b < bw.size(); b++) {
						const int x = cx + bx[b];
						const int z = cz + bz[b];
						if (x < 0 || z < 0 || x >= nx || z >= ny) {
							continue;
						}
						got += take(z * nx + x, want * bw[b]);
					}
					sed += got;
				}
				speed = std::sqrt(std::max(speed * speed + dh * -p.gravity, 0.0));
				water *= 1.0 - p.evaporation;
			}
		}
		// Thermal: loose material above repose slides to its lower neighbours.
		std::vector<double> move(n);
		for (int pass = 0; pass < p.thermal_passes; pass++) {
			std::fill(move.begin(), move.end(), 0.0);
			for (int z = 0; z < ny; z++) {
				for (int x = 0; x < nx; x++) {
					const int i = z * nx + x;
					if (loose[i] <= 0.0) {
						continue;
					}
					double excess[8];
					int target[8];
					int k = 0;
					double total = 0.0;
					double most = 0.0;
					for (int oz = -1; oz <= 1; oz++) {
						for (int ox = -1; ox <= 1; ox++) {
							const int xx = x + ox;
							const int zz = z + oz;
							if ((ox == 0 && oz == 0) || xx < 0 || zz < 0 || xx >= nx || zz >= ny) {
								continue;
							}
							const int j = zz * nx + xx;
							const double dist = (ox != 0 && oz != 0) ? 1.4142135623730951 : 1.0;
							const double e = h[i] - h[j] - p.repose * dist;
							if (e > 0.0) {
								excess[k] = e;
								target[k] = j;
								total += e;
								most = std::max(most, e);
								k++;
							}
						}
					}
					if (k == 0) {
						continue;
					}
					const double m = std::min(p.thermal_rate * most * 0.5, loose[i]);
					move[i] -= m;
					for (int q = 0; q < k; q++) {
						move[target[q]] += m * excess[q] / total;
					}
				}
			}
			for (int i = 0; i < n; i++) {
				h[i] += move[i];
				loose[i] += move[i];
			}
		}
		const double per = 1.0 / std::max(p.droplets_per_cell, 1e-6);
		for (int i = 0; i < n; i++) {
			r_ground[i] = float(h[i] * p.dx);
			r_flow[i] = float(flow[i] * per);
			r_deposit[i] = float(std::max(h[i] * p.dx - double(ground[i]), 0.0));
		}
	}
};

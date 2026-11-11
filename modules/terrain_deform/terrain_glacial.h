/**************************************************************************/
/*  terrain_glacial.h                                                     */
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

// Glacial erosion on a heightfield: the glacial stage of Cordonnier, Jouvet, Peytavie et al., "Forming Terrains by
// Glacial Erosion" (ACM TOG 42(4), SIGGRAPH 2023), with the shallow ice approximation in place of their learned
// Stokes emulator, as landscape models do (Braun, Zwartz, Tomkin 1999; Herman and Braun 2008; Egholm et al. 2009):
//   - Ice: SIA flux q = -Gamma H^(n+2) |grad s|^(n-1) grad s, n = 3, Gamma = 2 A (rho g)^n / (n + 2) (Mahaffy 1976),
//     the thickness on each face taken from the side with the higher surface (upwind, mass conserving on steep beds;
//     Jarosch, Schoof, Anslow 2013 use the limited second-order version). Mass balance linear in surface height about
//     the equilibrium line, capped at the accumulation maximum. The ice is solved to its steady state by
//     pseudo-transient iteration with a local pseudo time step (Raess et al., GMD 2022): the ice adjusts in centuries,
//     the bed in millennia, so the bed sees the steady ice of its own shape.
//   - Sliding speed: the depth-averaged speed from the steady flux, |q| / H, times a sliding share; spread over
//     about two ice thicknesses (longitudinal stress coupling, Kamb and Echelmeyer 1986), which SIA lacks.
//   - Erosion: abrasion and quarrying at K_g |u_b|^l (Humphrey and Raymond 1994; Herman et al., Nature 2015:
//     K_g 2.7e-7, l 2.02). None where the bed rises along the flow faster than 1.5 times the ice surface slope (the
//     supercooling threshold that ends overdeepening, Alley et al. 2003; Cook and Swift 2012).
// The ice runs on a grid of about 64 m (every k-th node of the bed grid); erosion goes back to the bed bilinearly.
// Plain C++ in a fixed order, so a seed gives the same terrain everywhere.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

class TerrainGlacial {
public:
	struct Params {
		double ela = 1.0e9; // equilibrium line altitude, metres
		double balance_gradient = 0.006; // (m ice / yr) per metre above the ELA
		double max_accumulation = 2.0; // m ice / yr
		double glen_a = 7.57e-17; // Pa^-3 yr^-1 (2.4e-24 Pa^-3 s^-1, temperate ice; Cuffey and Paterson 2010)
		double sliding = 1.0; // basal sliding speed as a share of the depth-averaged speed
		double erodibility = 2.7e-7; // K_g
		double exponent = 2.02; // l
		double adverse = 1.5; // no erosion where the bed rises along the flow faster than this times the surface slope
		double cell = 64.0; // ice grid spacing, metres (rounded to a whole number of bed cells)
		double max_step_erosion = 25.0; // metres per sub-step at most (the ice is solved again between)
		int first_iterations = 6000;
		int iterations = 200; // warm-started solves
		int max_sub_steps = 24; // per call; the rest of the time is dropped (the ice has worn what it can)
		double smoothing = 3.0; // ice cells: Gaussian sigma for the sliding speed (about 200 m, two to three ice thicknesses)
	};
	Params p;

	// Results of the last erode() call.
	int sub_steps = 0;
	double ice_area = 0.0; // m^2 under more than 1 m of ice
	double ice_max = 0.0; // m
	double eroded_max = 0.0; // m over the call
	double residual = 0.0; // m/yr, largest steady-state imbalance in the last solve

	// The ice grid's thickness, kept between calls for a warm start (cleared when the grid changes).
	std::vector<double> ice;

	// Erodes p_bed (nx * ny, spacing dx) by glacial erosion over p_years. Optional p_sediment: loose fill on top,
	// removed first. Optional p_fixed: cells that stay (outlets, the sea).
	// Optional p_resist (0..1): resistant bedrock (a riegel or cirque threshold) quarried 200 times slower at 1;
	// with p_level (NaN: none) only at and below that height, and never worn below it. Optional p_scale: per-cell
	// multiplier on the erosion rate (rock quality).
	void erode(double *p_bed, int nx, int ny, double dx, double p_years, double *p_sediment = nullptr, const uint8_t *p_fixed = nullptr, const double *p_resist = nullptr, const double *p_level = nullptr, const double *p_scale = nullptr) {
		sub_steps = 0;
		eroded_max = 0.0;
		const int k = std::max(1, (int)std::lround(p.cell / dx));
		const int mx = (nx - 1) / k + 1 + ((nx - 1) % k ? 1 : 0);
		const int mz = (ny - 1) / k + 1 + ((ny - 1) % k ? 1 : 0);
		const double d = dx * k;
		if (mx != gx || mz != gz || d != gd) {
			gx = mx;
			gz = mz;
			gd = d;
			ice.assign(size_t(gx) * gz, 0.0);
			first = true;
		}
		const int m = gx * gz;
		bed.resize(m);
		rate.resize(m);
		flux.resize(m);
		total.assign(size_t(nx) * ny, 0.0);
		double t = 0.0;
		for (int guard = 0; t < p_years && guard < p.max_sub_steps; guard++) {
			for (int z = 0; z < gz; z++) {
				for (int x = 0; x < gx; x++) {
					bed[z * gx + x] = p_bed[std::min(z * k, ny - 1) * nx + std::min(x * k, nx - 1)];
				}
			}
			_solve(first ? p.first_iterations : p.iterations);
			first = false;
			_erosion_rate();
			double rmax = 0.0;
			for (int i = 0; i < m; i++) {
				rmax = std::max(rmax, rate[i]);
			}
			if (rmax <= 1.0e-12) {
				break;
			}
			const double dt = std::min(p_years - t, p.max_step_erosion / rmax);
			// Back to the bed grid, bilinear between ice nodes.
			for (int z = 0; z < ny; z++) {
				const double fz = double(z) / k;
				const int z0 = std::min((int)fz, gz - 1), z1 = std::min(z0 + 1, gz - 1);
				const double tz = fz - z0;
				for (int x = 0; x < nx; x++) {
					const int i = z * nx + x;
					if (p_fixed && p_fixed[i]) {
						continue;
					}
					const double fx = double(x) / k;
					const int x0 = std::min((int)fx, gx - 1), x1 = std::min(x0 + 1, gx - 1);
					const double tx = fx - x0;
					const double r = (rate[z0 * gx + x0] * (1.0 - tx) + rate[z0 * gx + x1] * tx) * (1.0 - tz) + (rate[z1 * gx + x0] * (1.0 - tx) + rate[z1 * gx + x1] * tx) * tz;
					const bool lv = p_level && !std::isnan(p_level[i]);
					const bool hard = p_resist && p_resist[i] > 0.0 && (!lv || p_bed[i] <= p_level[i] + 1.0);
					double e = r * dt / (hard ? 1.0 + 199.0 * p_resist[i] : 1.0);
					if (p_scale) {
						e *= p_scale[i]; // rock quality: joint density and hardness (soft patches dig basins)
					}
					if (lv && p_resist && p_resist[i] > 0.0) {
						e = std::min(e, std::max(0.0, p_bed[i] - p_level[i]));
					}
					if (e <= 0.0) {
						continue;
					}
					p_bed[i] -= e;
					total[i] += e;
					if (p_sediment) {
						p_sediment[i] = std::max(0.0, p_sediment[i] - e);
					}
				}
			}
			t += dt;
			sub_steps++;
		}
		for (size_t i = 0; i < total.size(); i++) {
			eroded_max = std::max(eroded_max, total[i]);
		}
		ice_area = 0.0;
		ice_max = 0.0;
		for (int i = 0; i < m; i++) {
			if (ice[i] > 1.0) {
				ice_area += gd * gd;
			}
			ice_max = std::max(ice_max, ice[i]);
		}
	}

	// Ice thickness at a bed node (bilinear), after erode().
	double ice_at(int x, int z, double dx) const {
		if (ice.empty()) {
			return 0.0;
		}
		const int k = std::max(1, (int)std::lround(gd / dx));
		const double fx = double(x) / k, fz = double(z) / k;
		const int x0 = std::min((int)fx, gx - 1), x1 = std::min(x0 + 1, gx - 1);
		const int z0 = std::min((int)fz, gz - 1), z1 = std::min(z0 + 1, gz - 1);
		const double tx = fx - x0, tz = fz - z0;
		return (ice[z0 * gx + x0] * (1.0 - tx) + ice[z0 * gx + x1] * tx) * (1.0 - tz) + (ice[z1 * gx + x0] * (1.0 - tx) + ice[z1 * gx + x1] * tx) * tz;
	}

	const std::vector<double> &get_total() const { return total; }

private:
	int gx = 0, gz = 0;
	double gd = 0.0;
	bool first = true;
	std::vector<double> bed, rate, flux, total, qx, qz, dc, tmp;

	double _gamma() const {
		const double rg = 910.0 * 9.81;
		return 2.0 * p.glen_a * rg * rg * rg / 5.0;
	}

	// Steady ice by pseudo-transient iteration; fills flux (|q| per cell, m^2/yr).
	void _solve(int p_iterations) {
		const int m = gx * gz;
		const double G = _gamma();
		const double d = gd;
		qx.assign(m, 0.0); // face between i and i + 1
		qz.assign(m, 0.0); // face between i and i + gx
		dc.assign(m, 0.0);
		double res = 0.0;
		std::vector<double> &H = ice;
		for (int it = 0; it < p_iterations; it++) {
			std::fill(dc.begin(), dc.end(), 0.0);
			const bool last = it == p_iterations - 1;
			// Faces in x.
			for (int z = 0; z < gz; z++) {
				for (int x = 0; x + 1 < gx; x++) {
					const int i = z * gx + x, j = i + 1;
					const double si = bed[i] + H[i], sj = bed[j] + H[j];
					const double sx = (sj - si) / d;
					// Cross slope: the mean of the two cells' centred z gradients.
					auto gzc = [&](int c, int cz) {
						const int a = cz > 0 ? c - gx : c, b = cz < gz - 1 ? c + gx : c;
						const double span = (cz > 0 ? 1 : 0) + (cz < gz - 1 ? 1 : 0);
						return span > 0 ? ((bed[b] + H[b]) - (bed[a] + H[a])) / (span * d) : 0.0;
					};
					const double sz = 0.5 * (gzc(i, z) + gzc(j, z));
					const double hu = si > sj ? H[i] : H[j];
					const double h2 = hu * hu;
					const double D = G * h2 * h2 * hu * (sx * sx + sz * sz);
					qx[i] = -D * sx;
					dc[i] = std::max(dc[i], D);
					dc[j] = std::max(dc[j], D);
				}
			}
			for (int z = 0; z + 1 < gz; z++) {
				for (int x = 0; x < gx; x++) {
					const int i = z * gx + x, j = i + gx;
					const double si = bed[i] + H[i], sj = bed[j] + H[j];
					const double sz = (sj - si) / d;
					auto gxc = [&](int c, int cx) {
						const int a = cx > 0 ? c - 1 : c, b = cx < gx - 1 ? c + 1 : c;
						const double span = (cx > 0 ? 1 : 0) + (cx < gx - 1 ? 1 : 0);
						return span > 0 ? ((bed[b] + H[b]) - (bed[a] + H[a])) / (span * d) : 0.0;
					};
					const double sx = 0.5 * (gxc(i, x) + gxc(j, x));
					const double hu = si > sj ? H[i] : H[j];
					const double h2 = hu * hu;
					const double D = G * h2 * h2 * hu * (sx * sx + sz * sz);
					qz[i] = -D * sz;
					dc[i] = std::max(dc[i], D);
					dc[j] = std::max(dc[j], D);
				}
			}
			res = 0.0;
			for (int z = 0; z < gz; z++) {
				for (int x = 0; x < gx; x++) {
					const int i = z * gx + x;
					if (x == 0 || z == 0 || x == gx - 1 || z == gz - 1) {
						H[i] = 0.0; // ice leaves the grid at its edges
						continue;
					}
					const double div = (qx[i] - qx[i - 1] + qz[i] - qz[i - gx]) / d;
					const double s = bed[i] + H[i];
					const double mb = std::min(p.balance_gradient * (s - p.ela), p.max_accumulation);
					const double r = mb - div;
					const double dtau = std::min(0.03 * d * d / (dc[i] + 1e-9), 20.0);
					const double hn = std::max(0.0, H[i] + dtau * r);
					if (last && hn > 1.0) {
						res = std::max(res, std::fabs(r));
					}
					H[i] = hn;
				}
			}
		}
		residual = res;
		for (int z = 0; z < gz; z++) {
			for (int x = 0; x < gx; x++) {
				const int i = z * gx + x;
				const double ax = 0.5 * ((x > 0 ? qx[i - 1] : 0.0) + (x < gx - 1 ? qx[i] : 0.0));
				const double az = 0.5 * ((z > 0 ? qz[i - gx] : 0.0) + (z < gz - 1 ? qz[i] : 0.0));
				flux[i] = std::sqrt(ax * ax + az * az);
			}
		}
	}

	// Erosion rate per ice node (m/yr) from the steady ice.
	void _erosion_rate() {
		const int m = gx * gz;
		const double d = gd;
		tmp.resize(m);
		for (int i = 0; i < m; i++) {
			tmp[i] = ice[i] > 1.0 ? p.sliding * flux[i] / std::max(ice[i], 10.0) : 0.0;
		}
		_blur(tmp, p.smoothing);
		for (int z = 0; z < gz; z++) {
			for (int x = 0; x < gx; x++) {
				const int i = z * gx + x;
				rate[i] = 0.0;
				if (ice[i] <= 1.0 || x == 0 || z == 0 || x == gx - 1 || z == gz - 1) {
					continue;
				}
				auto s_at = [&](int c) { return bed[c] + ice[c]; };
				const double sgx = (s_at(i + 1) - s_at(i - 1)) / (2.0 * d);
				const double sgz = (s_at(i + gx) - s_at(i - gx)) / (2.0 * d);
				const double slope = std::sqrt(sgx * sgx + sgz * sgz);
				// The bed one ice cell along the flow (down the surface), bilinear.
				const double fx = x - sgx / std::max(slope, 1e-6), fz = z - sgz / std::max(slope, 1e-6);
				const double cx = std::clamp(fx, 0.0, double(gx - 1)), cz = std::clamp(fz, 0.0, double(gz - 1));
				const int x0 = std::min((int)cx, gx - 2), z0 = std::min((int)cz, gz - 2);
				const double tx = cx - x0, tz = cz - z0;
				const double bd = (bed[z0 * gx + x0] * (1.0 - tx) + bed[z0 * gx + x0 + 1] * tx) * (1.0 - tz) + (bed[(z0 + 1) * gx + x0] * (1.0 - tx) + bed[(z0 + 1) * gx + x0 + 1] * tx) * tz;
				if ((bd - bed[i]) / d >= p.adverse * slope) {
					continue; // supercooling: the water freezes on the adverse slope and the bed is not quarried
				}
				rate[i] = p.erodibility * std::pow(tmp[i], p.exponent);
			}
		}
	}

	// Separable Gaussian blur, edges clamped.
	void _blur(std::vector<double> &a, double sigma) const {
		if (sigma <= 0.0) {
			return;
		}
		const int r = (int)std::ceil(3.0 * sigma);
		std::vector<double> w(2 * r + 1);
		double ws = 0.0;
		for (int i = -r; i <= r; i++) {
			w[i + r] = std::exp(-0.5 * i * i / (sigma * sigma));
			ws += w[i + r];
		}
		for (double &v : w) {
			v /= ws;
		}
		std::vector<double> b(a.size());
		for (int z = 0; z < gz; z++) {
			for (int x = 0; x < gx; x++) {
				double s = 0.0;
				for (int i = -r; i <= r; i++) {
					s += w[i + r] * a[z * gx + std::clamp(x + i, 0, gx - 1)];
				}
				b[z * gx + x] = s;
			}
		}
		for (int z = 0; z < gz; z++) {
			for (int x = 0; x < gx; x++) {
				double s = 0.0;
				for (int i = -r; i <= r; i++) {
					s += w[i + r] * b[std::clamp(z + i, 0, gz - 1) * gx + x];
				}
				a[z * gx + x] = s;
			}
		}
	}
};

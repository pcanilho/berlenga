/**************************************************************************/
/*  terrain_glacial.h                                                     */
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
#include <functional>
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
		// Solve the steady ice to convergence (multigrid, below) instead of a fixed count of pseudo-transient steps.
		bool converged = false;
		double tolerance = 0.5; // m/yr: the largest imbalance the converged solve leaves
		int max_outer = 300; // cycles at most per solve
	};
	Params p;

	// Results of the last erode() call.
	int sub_steps = 0;
	int outer_steps = 0; // cycles the converged solves took over the call
	double ice_area = 0.0; // m^2 under more than 1 m of ice
	double ice_max = 0.0; // m
	double eroded_max = 0.0; // m over the call
	double residual = 0.0; // m/yr, largest steady-state imbalance in the last solve

	// The ice grid's thickness, kept between calls for a warm start (cleared when the grid changes).
	std::vector<double> ice;

	// Ice flowing in from beyond the grid (a glacier whose catchment lies off the map): at (u, v), the fraction of
	// the grid's width and height, q m^3 of ice a year. Fed in as extra accumulation spread over the nearest interior
	// nodes, and the edge there passes no ice out (a regional model's inflow boundary, as in PISM's regional mode).
	struct Source {
		double u = 0.0, v = 0.0, q = 0.0;
	};
	std::vector<Source> sources;

	// Erodes p_bed (nx * ny, spacing dx) by glacial erosion over p_years. Optional p_sediment: loose fill on top,
	// removed first. Optional p_fixed: cells that stay (outlets, the sea).
	// Optional p_resist (0..1): resistant bedrock (a riegel or cirque threshold) quarried 200 times slower at 1;
	// with p_level (NaN: none) only at and below that height, and never worn below it. Optional p_scale: per-cell
	// multiplier on the erosion rate (rock quality).
	void erode(double *p_bed, int nx, int ny, double dx, double p_years, double *p_sediment = nullptr, const uint8_t *p_fixed = nullptr, const double *p_resist = nullptr, const double *p_level = nullptr, const double *p_scale = nullptr) {
		sub_steps = 0;
		outer_steps = 0;
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
		_build_sources();
		double t = 0.0;
		for (int guard = 0; t < p_years && guard < p.max_sub_steps; guard++) {
			for (int z = 0; z < gz; z++) {
				for (int x = 0; x < gx; x++) {
					bed[z * gx + x] = p_bed[std::min(z * k, ny - 1) * nx + std::min(x * k, nx - 1)];
				}
			}
			if (p.converged) {
				_solve_converged(first);
			} else {
				_solve(first ? p.first_iterations : p.iterations);
			}
			first = false;
			_erosion_rate();
			if (p_scale) {
				// Rock quality acts on the ice grid's rate, so the step limit and the adverse-slope stop (both on
				// that grid) see it: a soft patch digs a basin as wide as the ice samples, never a narrow pit.
				for (int z = 0; z < gz; z++) {
					for (int x = 0; x < gx; x++) {
						rate[z * gx + x] *= p_scale[std::min(z * k, ny - 1) * nx + std::min(x * k, nx - 1)];
					}
				}
			}
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
	std::vector<double> src; // m/yr of ice added per node from sources (empty: none)
	std::vector<int> edge_in; // per node: the interior node an inflow edge node copies its thickness from, or -1

	void _build_sources() {
		src.clear();
		edge_in.clear();
		if (sources.empty() || gx < 3 || gz < 3) {
			return;
		}
		const int m = gx * gz;
		src.assign(m, 0.0);
		edge_in.assign(m, -1);
		const int r = 5; // nodes; Gaussian sigma two nodes (128 m), so a large flux does not land on a single node
		for (const Source &s : sources) {
			if (!(s.q > 0.0)) {
				continue;
			}
			const int cx = std::clamp((int)std::lround(s.u * (gx - 1)), 1, gx - 2);
			const int cz = std::clamp((int)std::lround(s.v * (gz - 1)), 1, gz - 2);
			double ws = 0.0;
			for (int pass = 0; pass < 2; pass++) {
				for (int z = std::max(cz - r, 1); z <= std::min(cz + r, gz - 2); z++) {
					for (int x = std::max(cx - r, 1); x <= std::min(cx + r, gx - 2); x++) {
						const double w = std::exp(-0.125 * double((x - cx) * (x - cx) + (z - cz) * (z - cz)));
						if (pass == 0) {
							ws += w;
						} else {
							src[z * gx + x] += s.q / (gd * gd) * w / ws;
						}
					}
				}
			}
			// The edge beside the entry holds the thickness of the node inside it: no ice leaves there.
			for (int z = std::max(cz - r - 1, 0); z <= std::min(cz + r + 1, gz - 1); z++) {
				for (int x = std::max(cx - r - 1, 0); x <= std::min(cx + r + 1, gx - 1); x++) {
					if (x == 0 || z == 0 || x == gx - 1 || z == gz - 1) {
						edge_in[z * gx + x] = std::clamp(z, 1, gz - 2) * gx + std::clamp(x, 1, gx - 2);
					}
				}
			}
		}
	}

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
						// Ice leaves the grid at its edges, except beside an inflow: there the edge's surface matches the
						// node inside it (no surface slope across the boundary, so no ice crosses it either way).
						H[i] = edge_in.empty() || edge_in[i] < 0 ? 0.0 : std::max(0.0, bed[edge_in[i]] + H[edge_in[i]] - bed[i]);
						continue;
					}
					const double div = (qx[i] - qx[i - 1] + qz[i] - qz[i - gx]) / d;
					const double s = bed[i] + H[i];
					const double mb = std::min(p.balance_gradient * (s - p.ela), p.max_accumulation) + (src.empty() ? 0.0 : src[i]);
					const double r = mb - div;
					double dtau = std::min(0.03 * d * d / (dc[i] + 1e-9), 20.0);
					if (!src.empty() && src[i] > 0.0) {
						dtau = std::min(dtau, 100.0 / src[i]); // a source node rises at most 100 m an iteration
					}
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

	// The face thickness of Jarosch, Schoof and Anslow 2013 (TC 7): MUSCL reconstruction from both sides, D from
	// the thinner or thicker side by the sign of the surface step (their eq. 33), so a thin cell above a cliff cannot pass on more ice than it holds and mass is conserved over steep beds. Limited by
	// minmod: Jarosch et al.'s superbee and van Leer are more compressive, and the relaxation below cycles and runs
	// away with them on steep beds; minmod settles every state tried.
	static double _minmod(double r) { return std::max(0.0, std::min(1.0, r)); }
	static double _muscl_h(double hm, double h0, double h1, double h2, double s0, double s1) {
		auto ratio = [](double num, double den) { return std::fabs(den) < 1.0e-12 ? (std::fabs(num) < 1.0e-12 ? 1.0 : (num > 0.0 ? 1.0e6 : -1.0e6) * (den >= 0.0 ? 1.0 : -1.0)) : num / den; };
		const double r0 = ratio(h0 - hm, h1 - h0);
		const double r1 = ratio(h1 - h0, h2 - h1);
		const double hmi = h0 + 0.5 * _minmod(r0) * (h1 - h0);
		const double hpl = h1 - 0.5 * _minmod(r1) * (h2 - h1);
		const double lo = std::min(hmi, hpl), hi = std::max(hmi, hpl);
		// eq. 33: down to the right (s1 <= s0) takes the left value's side rule, up the other.
		if (s1 <= s0) {
			return hmi <= hpl ? lo : hi;
		}
		return hmi <= hpl ? hi : lo;
	}
	bool muscl = false;

	// The converged solve's settings: safeguarded root steps per node, the coarse-grid correction's damping, the
	// most grid levels, the fine sweeps' relaxation once the cycles stop gaining, and how many cycles without a 5%
	// gain count as stopped.
	static constexpr int k_root_iterations = 30;
	static constexpr double k_cycle_damping = 0.5;
	static constexpr int k_max_levels = 8;
	static constexpr double k_fine_omega = 1.0;
	static constexpr int k_cycle_patience = 12;

	// Face diffusivities and fluxes of the ice as it stands (the same SIA faces as _solve: upwind thickness, cross
	// slope from the neighbours' centred gradients).
	void _faces(std::vector<double> &r_dx, std::vector<double> &r_dz) {
		const int m = gx * gz;
		const double G = _gamma();
		const double d = gd;
		const std::vector<double> &H = ice;
		qx.assign(m, 0.0);
		qz.assign(m, 0.0);
		r_dx.assign(m, 0.0);
		r_dz.assign(m, 0.0);
		auto sf = [&](int c) { return bed[c] + H[c]; };
		auto gzc = [&](int c, int cz) {
			const int a = cz > 0 ? c - gx : c, b = cz < gz - 1 ? c + gx : c;
			const double span = (cz > 0 ? 1 : 0) + (cz < gz - 1 ? 1 : 0);
			return span > 0 ? (sf(b) - sf(a)) / (span * d) : 0.0;
		};
		auto gxc = [&](int c, int cx) {
			const int a = cx > 0 ? c - 1 : c, b = cx < gx - 1 ? c + 1 : c;
			const double span = (cx > 0 ? 1 : 0) + (cx < gx - 1 ? 1 : 0);
			return span > 0 ? (sf(b) - sf(a)) / (span * d) : 0.0;
		};
		for (int z = 0; z < gz; z++) {
			for (int x = 0; x + 1 < gx; x++) {
				const int i = z * gx + x, j = i + 1;
				const double si = sf(i), sj = sf(j);
				const double sx = (sj - si) / d;
				const double sz = 0.5 * (gzc(i, z) + gzc(j, z));
				const double hu = muscl ? _muscl_h(x > 0 ? H[i - 1] : H[i], H[i], H[j], x + 2 < gx ? H[j + 1] : H[j], si, sj) : (si > sj ? H[i] : H[j]);
				const double h2 = hu * hu;
				r_dx[i] = G * h2 * h2 * hu * (sx * sx + sz * sz);
				qx[i] = -r_dx[i] * sx;
			}
		}
		for (int z = 0; z + 1 < gz; z++) {
			for (int x = 0; x < gx; x++) {
				const int i = z * gx + x, j = i + gx;
				const double si = sf(i), sj = sf(j);
				const double sz = (sj - si) / d;
				const double sx = 0.5 * (gxc(i, x) + gxc(j, x));
				const double hu = muscl ? _muscl_h(z > 0 ? H[i - gx] : H[i], H[i], H[j], z + 2 < gz ? H[j + gx] : H[j], si, sj) : (si > sj ? H[i] : H[j]);
				const double h2 = hu * hu;
				r_dz[i] = G * h2 * h2 * hu * (sx * sx + sz * sz);
				qz[i] = -r_dz[i] * sz;
			}
		}
	}

	void _ice_edges() {
		for (int z = 0; z < gz; z++) {
			for (int x = 0; x < gx; x++) {
				if (x == 0 || z == 0 || x == gx - 1 || z == gz - 1) {
					const int i = z * gx + x;
					ice[i] = edge_in.empty() || edge_in[i] < 0 ? 0.0 : std::max(0.0, bed[edge_in[i]] + ice[edge_in[i]] - bed[i]);
				}
			}
		}
	}

	// Steady ice, converged. The steady SIA with H >= 0 is an obstacle problem (Jouvet and Bueler 2012): it is
	// relaxed pointwise by nonlinear projected Gauss-Seidel (each node in turn takes the thickness that balances its
	// own four face fluxes and its mass balance, a safeguarded scalar root, clamped at zero; pointwise smoothers work
	// for the SIA, Bueler 2021), and the smooth error that Gauss-Seidel removes slowly is removed on coarser grids by
	// full approximation storage multigrid V-cycles (Brandt 1977; Bueler 2021's FAS example), the coarse levels
	// seeing the bed by injection. Stops once the largest imbalance is under the tolerance, wherever it started.
	struct Level {
		int nx = 0, nz = 0;
		double d = 0.0;
		std::vector<double> b, h, rhs, src;
		std::vector<uint8_t> fix; // held: the grid's edge
	};

	static double _lv_s(const Level &L, int c) { return L.b[c] + L.h[c]; }
	static double _lv_gz(const Level &L, int c) {
		const int cz = c / L.nx;
		const int a = cz > 0 ? c - L.nx : c, b = cz < L.nz - 1 ? c + L.nx : c;
		const double span = (cz > 0 ? 1 : 0) + (cz < L.nz - 1 ? 1 : 0);
		return span > 0 ? (_lv_s(L, b) - _lv_s(L, a)) / (span * L.d) : 0.0;
	}
	static double _lv_gx(const Level &L, int c) {
		const int cx = c % L.nx;
		const int a = cx > 0 ? c - 1 : c, b = cx < L.nx - 1 ? c + 1 : c;
		const double span = (cx > 0 ? 1 : 0) + (cx < L.nx - 1 ? 1 : 0);
		return span > 0 ? (_lv_s(L, b) - _lv_s(L, a)) / (span * L.d) : 0.0;
	}

	// The node's operator at thickness p_h, neighbours as they stand: outflow per area less the mass balance.
	struct NodeOp {
		double G, d, bi, cross[4], sj[4], hj[4], bg, ela, acc, src;
		bool muscl = false;
		double far[4], near_other[4]; // MUSCL: the thickness beyond the neighbour, and beyond this node, per face
		double operator()(double p_h) const {
			const double si = bi + p_h;
			double out = 0.0;
			for (int k = 0; k < 4; k++) {
				const double sl = (si - sj[k]) / d;
				double hu;
				if (muscl) {
					// Faces 0 and 2 have the neighbour on the low side (left), 1 and 3 on the high side.
					hu = (k & 1) ? _muscl_h(near_other[k], p_h, hj[k], far[k], si, sj[k]) : _muscl_h(far[k], hj[k], p_h, near_other[k], sj[k], si);
				} else {
					hu = si > sj[k] ? p_h : hj[k];
				}
				const double h2 = hu * hu;
				out += G * h2 * h2 * hu * (sl * sl + cross[k] * cross[k]) * sl / d;
			}
			return out - (std::min(bg * (si - ela), acc) + src);
		}
	};
	NodeOp _node_op(const Level &L, int i) const {
		NodeOp o;
		o.G = _gamma();
		o.d = L.d;
		o.bi = L.b[i];
		o.bg = p.balance_gradient;
		o.ela = p.ela;
		o.acc = p.max_accumulation;
		o.src = L.src.empty() ? 0.0 : L.src[i];
		const int nb[4] = { i - 1, i + 1, i - L.nx, i + L.nx };
		const double gzi = _lv_gz(L, i), gxi = _lv_gx(L, i);
		o.cross[0] = 0.5 * (gzi + _lv_gz(L, i - 1));
		o.cross[1] = 0.5 * (gzi + _lv_gz(L, i + 1));
		o.cross[2] = 0.5 * (gxi + _lv_gx(L, i - L.nx));
		o.cross[3] = 0.5 * (gxi + _lv_gx(L, i + L.nx));
		for (int k = 0; k < 4; k++) {
			o.sj[k] = _lv_s(L, nb[k]);
			o.hj[k] = L.h[nb[k]];
		}
		o.muscl = muscl;
		if (muscl) {
			const int x = i % L.nx, z = i / L.nx;
			// West: (i-2, i-1 | i, i+1); east: (i-1, i | i+1, i+2); the same in z. Clamped at the edge as _faces does.
			o.far[0] = x - 1 > 0 ? L.h[i - 2] : L.h[i - 1];
			o.near_other[0] = L.h[i + 1];
			o.near_other[1] = L.h[i - 1];
			o.far[1] = x + 2 < L.nx ? L.h[i + 2] : L.h[i + 1];
			o.far[2] = z - 1 > 0 ? L.h[i - 2 * L.nx] : L.h[i - L.nx];
			o.near_other[2] = L.h[i + L.nx];
			o.near_other[3] = L.h[i - L.nx];
			o.far[3] = z + 2 < L.nz ? L.h[i + 2 * L.nx] : L.h[i + L.nx];
		}
		return o;
	}

	// Projected nonlinear Gauss-Seidel: each node solves op(h) = rhs for h >= 0 (op rises with h).
	void _lv_smooth(Level &L, int p_sweeps, bool p_fine, double p_omega = 1.0) {
		for (int sw = 0; sw < p_sweeps; sw++) {
			const bool back = sw & 1; // alternate the sweep direction, so no direction is favoured
			for (int zz = 1; zz + 1 < L.nz; zz++) {
				const int z = back ? L.nz - 1 - zz : zz;
				for (int xx = 1; xx + 1 < L.nx; xx++) {
					const int x = back ? L.nx - 1 - xx : xx;
					const int i = z * L.nx + x;
					if (L.fix[i]) {
						continue;
					}
					const NodeOp op = _node_op(L, i);
					const double r = L.rhs[i];
					double lo = 0.0, flo = op(0.0) - r;
					if (flo >= 0.0) {
						L.h[i] = 0.0;
						continue;
					}
					double hi = std::max(2.0 * L.h[i], 10.0), fhi = op(hi) - r;
					for (int g = 0; g < 20 && fhi < 0.0; g++) {
						lo = hi;
						flo = fhi;
						hi *= 2.0;
						fhi = op(hi) - r;
					}
					// Illinois regula falsi on [lo, hi].
					int side = 0;
					double hh = L.h[i];
					for (int g = 0; g < k_root_iterations; g++) {
						hh = (lo * fhi - hi * flo) / (fhi - flo);
						const double fh = op(hh) - r;
						if (std::fabs(fh) < 1.0e-5 || hi - lo < 1.0e-4) {
							break;
						}
						if ((fh < 0.0) == (flo < 0.0)) {
							lo = hh;
							flo = fh;
							if (side == -1) {
								fhi *= 0.5;
							}
							side = -1;
						} else {
							hi = hh;
							fhi = fh;
							if (side == 1) {
								flo *= 0.5;
							}
							side = 1;
						}
					}
					L.h[i] = std::max(0.0, L.h[i] + p_omega * (hh - L.h[i]));
				}
			}
			if (p_fine) {
				_ice_edges();
			}
		}
	}

	// rhs - op(h), zero where the thickness sits on its bound and the node would rather lose ice (the constraint
	// holds it there: no correction is wanted).
	void _lv_residual(const Level &L, std::vector<double> &r_r) const {
		r_r.assign(size_t(L.nx) * L.nz, 0.0);
		for (int z = 1; z + 1 < L.nz; z++) {
			for (int x = 1; x + 1 < L.nx; x++) {
				const int i = z * L.nx + x;
				if (L.fix[i]) {
					continue;
				}
				const double r = L.rhs[i] - _node_op(L, i)(L.h[i]);
				r_r[i] = (L.h[i] <= 0.0 && r < 0.0) ? 0.0 : r;
			}
		}
	}

	void _vcycle(std::vector<Level> &p_lv, int l) {
		Level &F = p_lv[l];
		if (l + 1 == int(p_lv.size())) {
			_lv_smooth(F, 40, l == 0);
			return;
		}
		_lv_smooth(F, 2, l == 0);
		Level &C = p_lv[l + 1];
		std::vector<double> r, hc0;
		_lv_residual(F, r);
		// Restriction: injection for the thickness, full weighting for the residual.
		for (int z = 0; z < C.nz; z++) {
			for (int x = 0; x < C.nx; x++) {
				C.h[z * C.nx + x] = F.h[(2 * z) * F.nx + 2 * x];
			}
		}
		hc0 = C.h;
		for (int z = 1; z + 1 < C.nz; z++) {
			for (int x = 1; x + 1 < C.nx; x++) {
				const int c = z * C.nx + x;
				const int f = (2 * z) * F.nx + 2 * x;
				double rw = 4.0 * r[f] + 2.0 * (r[f - 1] + r[f + 1] + r[f - F.nx] + r[f + F.nx]) + r[f - F.nx - 1] + r[f - F.nx + 1] + r[f + F.nx - 1] + r[f + F.nx + 1];
				C.rhs[c] = _node_op(C, c)(C.h[c]) + rw / 16.0;
			}
		}
		_vcycle(p_lv, l + 1);
		// Prolongation of the correction, bilinear; the thickness stays at or above zero.
		for (int z = 1; z + 1 < F.nz; z++) {
			for (int x = 1; x + 1 < F.nx; x++) {
				const int i = z * F.nx + x;
				if (F.fix[i]) {
					continue;
				}
				const int cx0 = x / 2, cz0 = z / 2, cx1 = (x + 1) / 2, cz1 = (z + 1) / 2;
				auto e = [&](int cx, int cz) { const int c = cz * C.nx + cx; return C.h[c] - hc0[c]; };
				const double v = 0.25 * (e(cx0, cz0) + e(cx1, cz0) + e(cx0, cz1) + e(cx1, cz1));
				F.h[i] = std::max(0.0, F.h[i] + k_cycle_damping * v);
			}
		}
		if (l == 0) {
			_ice_edges();
		}
		_lv_smooth(F, 2, l == 0);
	}

	void _solve_converged(bool p_cold) {
		(void)p_cold;
		const int m = gx * gz;
		const double d = gd;
		std::vector<double> &H = ice;
		std::vector<double> Dx, Dz;
		double res = 0.0;
		muscl = true; // the mass-conserving face thickness (Jarosch et al. 2013) for the settled ice
		_ice_edges();
		// The levels: halve while both sides stay odd and at least 5 nodes.
		std::vector<Level> lv(1);
		lv[0].nx = gx;
		lv[0].nz = gz;
		lv[0].d = gd;
		lv[0].b = bed;
		lv[0].src = src;
		lv[0].rhs.assign(m, 0.0);
		lv[0].fix.assign(m, 0);
		for (int z = 0; z < gz; z++) {
			for (int x = 0; x < gx; x++) {
				lv[0].fix[z * gx + x] = (x == 0 || z == 0 || x == gx - 1 || z == gz - 1) ? 1 : 0;
			}
		}
		while (int(lv.size()) < k_max_levels && lv.back().nx % 2 == 1 && lv.back().nz % 2 == 1 && lv.back().nx >= 9 && lv.back().nz >= 9) {
			const Level &F = lv.back();
			Level C;
			C.nx = (F.nx - 1) / 2 + 1;
			C.nz = (F.nz - 1) / 2 + 1;
			C.d = F.d * 2.0;
			const int mc = C.nx * C.nz;
			C.b.resize(mc);
			C.h.assign(mc, 0.0);
			C.rhs.assign(mc, 0.0);
			C.fix.assign(mc, 0);
			if (!F.src.empty()) {
				C.src.assign(mc, 0.0);
			}
			for (int z = 0; z < C.nz; z++) {
				for (int x = 0; x < C.nx; x++) {
					const int c = z * C.nx + x, f = (2 * z) * F.nx + 2 * x;
					C.b[c] = F.b[f];
					C.fix[c] = (x == 0 || z == 0 || x == C.nx - 1 || z == C.nz - 1) ? 1 : 0;
					if (!F.src.empty()) {
						// The same ice a year over the coarse cell's area: the mean rate of the fine nodes it covers.
						double sum = 0.0, w = 0.0;
						for (int dz = -1; dz <= 1; dz++) {
							for (int dx = -1; dx <= 1; dx++) {
								const int xx = 2 * x + dx, zz = 2 * z + dz;
								if (xx >= 0 && zz >= 0 && xx < F.nx && zz < F.nz) {
									const double ww = (dx == 0 ? 2.0 : 1.0) * (dz == 0 ? 2.0 : 1.0);
									sum += ww * F.src[zz * F.nx + xx];
									w += ww;
								}
							}
						}
						C.src[c] = w > 0.0 ? sum / w * 1.0 : 0.0;
					}
				}
			}
			lv.push_back(C);
		}
		double best = 1.0e300;
		std::vector<double> best_h = H;
		std::vector<double> imb;
		int stall = 0;
		bool use_mg = lv.size() > 1;
		for (int it = 0; it < p.max_outer; it++) {
			_faces(Dx, Dz);
			// The imbalance: on the ice, the mass balance less the flux divergence; off it, only a positive one (ice
			// that should start to grow).
			// The measure is the imbalance that all but the worst 0.2% of the ice nodes stay under: a node or two over
			// a cliff in the bed, where the discrete SIA cannot balance (Jarosch et al. 2013), would hold it up.
			res = 0.0;
			imb.clear();
			for (int z = 1; z + 1 < gz; z++) {
				for (int x = 1; x + 1 < gx; x++) {
					const int i = z * gx + x;
					const double mb = std::min(p.balance_gradient * (bed[i] + H[i] - p.ela), p.max_accumulation) + (src.empty() ? 0.0 : src[i]);
					const double r = mb - (qx[i] - qx[i - 1] + qz[i] - qz[i - gx]) / d;
					if (H[i] > 1.0 || (H[i] <= 0.0 && r > 0.0)) {
						imb.push_back(std::fabs(r));
					}
				}
			}
			if (!imb.empty()) {
				const size_t skip = std::min(imb.size() - 1, std::max<size_t>(2, imb.size() / 500));
				std::nth_element(imb.begin(), imb.begin() + skip, imb.end(), std::greater<double>());
				res = imb[skip];
			}
			if (res < p.tolerance) {
				break;
			}
			// Once the cycles stop gaining (the coarse grids see the bed only roughly, and near the margin their
			// correction can hold the fine grid back), the fine grid relaxes on its own.
			if (res > 10.0 * best || !std::isfinite(res)) {
				// A cycle that ran away (the coarse grids' correction at a steep, thick margin): back to the best
				// state seen, and on with the fine grid's own relaxation, which cannot.
				H = best_h;
				use_mg = false;
				res = best;
			} else if (res < 0.95 * best) {
				best = res;
				best_h = H;
				stall = 0;
			} else if (++stall >= k_cycle_patience) {
				use_mg = false;
			}
			outer_steps++;
			lv[0].h = H;
			// _ice_edges and the smoother work on ice; keep them the same vector.
			std::swap(lv[0].h, H);
			if (use_mg) {
				_vcycle(lv, 0);
			} else {
				_lv_smooth(lv[0], 6, true, k_fine_omega);
			}
			std::swap(lv[0].h, H);
		}
		residual = res;
		_faces(Dx, Dz);
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
				// The flow's direction: the node's ice flux (the mean of its faces), else down the surface. A centred
				// surface gradient would not do at a cliff's foot, where the ice falling from the cliff top dominates it.
				double ux = 0.5 * (qx[i - 1] + qx[i]), uz = 0.5 * (qz[i - gx] + qz[i]);
				double ul = std::sqrt(ux * ux + uz * uz);
				if (ul < 1e-9) {
					ux = -(s_at(i + 1) - s_at(i - 1));
					uz = -(s_at(i + gx) - s_at(i - gx));
					ul = std::sqrt(ux * ux + uz * uz);
				}
				ux /= std::max(ul, 1e-12);
				uz /= std::max(ul, 1e-12);
				// The bed and the ice surface one ice cell downstream, bilinear; the surface slope the supercooling test
				// compares with is the drop over that cell (one-sided: what the ice ahead of the node does).
				const double cx = std::clamp(x + ux, 0.0, double(gx - 1)), cz = std::clamp(z + uz, 0.0, double(gz - 1));
				const int x0 = std::min((int)cx, gx - 2), z0 = std::min((int)cz, gz - 2);
				const double tx = cx - x0, tz = cz - z0;
				auto bil = [&](auto f) { return (f(z0 * gx + x0) * (1.0 - tx) + f(z0 * gx + x0 + 1) * tx) * (1.0 - tz) + (f((z0 + 1) * gx + x0) * (1.0 - tx) + f((z0 + 1) * gx + x0 + 1) * tx) * tz; };
				const double bd = bil([&](int c) { return bed[c]; });
				const double slope = std::max(0.0, s_at(i) - bil(s_at)) / d;
				if ((bd - bed[i]) / d >= p.adverse * slope) {
					continue; // supercooling: the water freezes on the adverse slope and the bed is not quarried
				}
				rate[i] = p.erodibility * std::pow(tmp[i], p.exponent);
			}
		}
		if (!edge_in.empty()) {
			// The trough runs on past the map at an inflow: its edge wears as the node inside it does, so no step is
			// left at the boundary.
			for (int i = 0; i < m; i++) {
				if (edge_in[i] >= 0) {
					rate[i] = rate[edge_in[i]];
				}
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

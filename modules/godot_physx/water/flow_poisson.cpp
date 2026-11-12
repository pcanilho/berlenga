/**************************************************************************/
/*  flow_poisson.cpp                                                      */
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

#include "flow_poisson.h"

#include "core/error/error_macros.h"
#include "core/math/math_funcs.h"
#include "core/typedefs.h"

#include <cstring>

namespace {

// One grid level of the preconditioner, in float (the preconditioner only needs to be roughly right;
// CG itself runs in double). cx/cz: the conductance between two unknowns (0 onto anything else, and 0
// past the last column or row); inv_diag: 1 / the full diagonal, 0 where there is no unknown, so links
// to fixed cells and islands act as Dirichlet zero inside the V-cycle.
struct Level {
	int nx = 0;
	int nz = 0;
	LocalVector<float> cx;
	LocalVector<float> cz;
	LocalVector<float> diag;
	LocalVector<float> inv_diag;
	LocalVector<float> x;
	LocalVector<float> b;
	LocalVector<float> r;

	void resize(int p_nx, int p_nz) {
		nx = p_nx;
		nz = p_nz;
		const int n = nx * nz;
		for (LocalVector<float> *v : { &cx, &cz, &diag, &inv_diag, &x, &b, &r }) {
			v->resize(n);
			memset(v->ptr(), 0, n * sizeof(float));
		}
	}
};

// One Gauss-Seidel sweep, forward or backward (the backward sweep is the forward one's adjoint, so a
// forward pre-smooth and a backward post-smooth keep the V-cycle symmetric).
void gs_sweep(Level &p_l, bool p_forward) {
	const int nx = p_l.nx;
	const int n = nx * p_l.nz;
	const float *cx = p_l.cx.ptr();
	const float *cz = p_l.cz.ptr();
	const float *id = p_l.inv_diag.ptr();
	const float *b = p_l.b.ptr();
	float *x = p_l.x.ptr();
	// cx/cz are 0 past the last column and row, so only reads outside the arrays need guards.
	for (int k = 0; k < n; k++) {
		const int c = p_forward ? k : n - 1 - k;
		if (id[c] == 0.0f) {
			continue;
		}
		float s = b[c];
		if (c > 0) {
			s += cx[c - 1] * x[c - 1];
		}
		if (c + 1 < n) {
			s += cx[c] * x[c + 1];
		}
		if (c >= nx) {
			s += cz[c - nx] * x[c - nx];
		}
		if (c + nx < n) {
			s += cz[c] * x[c + nx];
		}
		x[c] = s * id[c];
	}
}

void residual(Level &p_l) {
	const int nx = p_l.nx;
	const int n = nx * p_l.nz;
	const float *cx = p_l.cx.ptr();
	const float *cz = p_l.cz.ptr();
	const float *dg = p_l.diag.ptr();
	const float *b = p_l.b.ptr();
	const float *x = p_l.x.ptr();
	float *r = p_l.r.ptr();
	for (int c = 0; c < n; c++) {
		float s = b[c] - dg[c] * x[c];
		if (c > 0) {
			s += cx[c - 1] * x[c - 1];
		}
		if (c + 1 < n) {
			s += cx[c] * x[c + 1];
		}
		if (c >= nx) {
			s += cz[c - nx] * x[c - nx];
		}
		if (c + nx < n) {
			s += cz[c] * x[c + nx];
		}
		r[c] = p_l.inv_diag[c] != 0.0f ? s : 0.0f;
	}
}

// The Galerkin coarse level of 2 x 2 aggregates (P piecewise constant, A_c = P^T A P): the conductances
// crossing between two aggregates add up, those inside one drop out of its diagonal.
void coarsen(const Level &p_f, Level &p_c) {
	p_c.resize((p_f.nx + 1) / 2, (p_f.nz + 1) / 2);
	for (int z = 0; z < p_f.nz; z++) {
		for (int x = 0; x < p_f.nx; x++) {
			const int f = z * p_f.nx + x;
			const int c = (z / 2) * p_c.nx + x / 2;
			p_c.diag[c] += p_f.diag[f];
			if (x < p_f.nx - 1) {
				if ((x & 1) == 0) {
					p_c.diag[c] -= 2.0f * p_f.cx[f];
				} else {
					p_c.cx[c] += p_f.cx[f];
				}
			}
			if (z < p_f.nz - 1) {
				if ((z & 1) == 0) {
					p_c.diag[c] -= 2.0f * p_f.cz[f];
				} else {
					p_c.cz[c] += p_f.cz[f];
				}
			}
		}
	}
	// An aggregate whose diagonal round-off left near zero has no outside links: drop it (its fine cells
	// then get no correction from here, which keeps the cycle symmetric).
	const int n = p_c.nx * p_c.nz;
	for (int c = 0; c < n; c++) {
		const int xi = c % p_c.nx;
		const float off = p_c.cx[c] + p_c.cz[c] + (xi > 0 ? p_c.cx[c - 1] : 0.0f) + (c >= p_c.nx ? p_c.cz[c - p_c.nx] : 0.0f);
		if (p_c.diag[c] > 1e-5f * off && p_c.diag[c] > 0.0f) {
			p_c.inv_diag[c] = 1.0f / p_c.diag[c];
			continue;
		}
		p_c.diag[c] = 0.0f;
		p_c.cx[c] = 0.0f;
		p_c.cz[c] = 0.0f;
		if (xi > 0) {
			p_c.cx[c - 1] = 0.0f;
		}
		if (c >= p_c.nx) {
			p_c.cz[c - p_c.nx] = 0.0f;
		}
	}
}

// Unsmoothed aggregation undershoots the smooth error (a constant per aggregate fits it only roughly), so
// the coarse correction is scaled up: the over-correction of Braess 1995 (Towards algebraic multigrid for
// elliptic problems of second order). A scale keeps the cycle symmetric; 1.8 measured best on the flow
// grids (CG iterations 30 -> 14 at 320 x 320).
constexpr float COARSE_SCALE = 1.8f;

void v_cycle(LocalVector<Level> &p_levels, int p_l) {
	Level &l = p_levels[p_l];
	const int n = l.nx * l.nz;
	memset(l.x.ptr(), 0, n * sizeof(float));
	if (p_l == (int)p_levels.size() - 1) {
		for (int s = 0; s < 8; s++) {
			gs_sweep(l, true);
			gs_sweep(l, false);
		}
		return;
	}
	gs_sweep(l, true);
	residual(l);
	Level &c = p_levels[p_l + 1];
	float *cb = c.b.ptr();
	memset(cb, 0, c.nx * c.nz * sizeof(float));
	const float *r = l.r.ptr();
	for (int z = 0; z < l.nz; z++) {
		float *row = cb + (z / 2) * c.nx;
		const float *fr = r + z * l.nx;
		for (int x = 0; x < l.nx; x++) {
			row[x >> 1] += fr[x];
		}
	}
	v_cycle(p_levels, p_l + 1);
	const float *cxv = c.x.ptr();
	float *x = l.x.ptr();
	const float *id = l.inv_diag.ptr();
	for (int z = 0; z < l.nz; z++) {
		const float *row = cxv + (z / 2) * c.nx;
		for (int xi = 0; xi < l.nx; xi++) {
			const int f = z * l.nx + xi;
			if (id[f] != 0.0f) {
				x[f] += COARSE_SCALE * row[xi >> 1];
			}
		}
	}
	gs_sweep(l, false);
}

} // namespace

int FlowPoisson::solve(int p_nx, int p_nz, const LocalVector<float> &p_cx, const LocalVector<float> &p_cz, const LocalVector<int> &p_kind, int p_islands, LocalVector<float> &p_psi, double p_rel_tol, int p_max_iter) {
	const int n = p_nx * p_nz;
	ERR_FAIL_COND_V(p_nx < 1 || p_nz < 1, 0);
	ERR_FAIL_COND_V((int)p_cx.size() != n || (int)p_cz.size() != n || (int)p_kind.size() != n || (int)p_psi.size() != n, 0);
	const int dim = n + p_islands;
	// Unknown index per cell: the cell itself, n + k for island k, -1 fixed.
	LocalVector<int> idx;
	idx.resize(n);
	for (int c = 0; c < n; c++) {
		const int kd = p_kind[c];
		idx[c] = kd == UNKNOWN ? c : (kd >= ISLAND && kd - ISLAND < p_islands ? n + kd - ISLAND : -1);
	}
	LocalVector<double> diag;
	LocalVector<double> rhs;
	LocalVector<double> x;
	diag.resize(dim);
	rhs.resize(dim);
	x.resize(dim);
	for (int i = 0; i < dim; i++) {
		diag[i] = 0.0;
		rhs[i] = 0.0;
		x[i] = 0.0;
	}
	// Level 0 holds the grid unknowns' couplings; the few faces that touch an island are kept apart.
	struct Link {
		int a;
		int b;
		double w;
	};
	LocalVector<Link> island_links;
	LocalVector<Level> levels;
	levels.resize(1);
	Level &l0 = levels[0];
	l0.resize(p_nx, p_nz);
	auto add_face = [&](int p_a, int p_b, float p_w, float &r_level_w) {
		if (!(p_w > 0.0f) || !Math::is_finite(p_w)) {
			return;
		}
		const int ia = idx[p_a];
		const int ib = idx[p_b];
		if ((ia < 0 && ib < 0) || ia == ib) {
			return;
		}
		if (ia >= 0) {
			diag[ia] += p_w;
			if (ib < 0) {
				rhs[ia] += (double)p_w * p_psi[p_b];
			}
		}
		if (ib >= 0) {
			diag[ib] += p_w;
			if (ia < 0) {
				rhs[ib] += (double)p_w * p_psi[p_a];
			}
		}
		if (ia >= 0 && ib >= 0) {
			if (ia < n && ib < n) {
				r_level_w = p_w;
			} else {
				island_links.push_back({ ia, ib, (double)p_w });
			}
		}
	};
	for (int z = 0; z < p_nz; z++) {
		for (int xi = 0; xi < p_nx; xi++) {
			const int c = z * p_nx + xi;
			if (xi < p_nx - 1) {
				add_face(c, c + 1, p_cx[c], l0.cx[c]);
			}
			if (z < p_nz - 1) {
				add_face(c, c + p_nx, p_cz[c], l0.cz[c]);
			}
		}
	}
	for (int c = 0; c < n; c++) {
		if (idx[c] == c && diag[c] > 0.0) {
			l0.diag[c] = (float)diag[c];
			l0.inv_diag[c] = (float)(1.0 / diag[c]);
		}
	}
	// Starting guess: the caller's psi; an island starts at its cells' mean.
	{
		LocalVector<int> island_cells;
		island_cells.resize(p_islands);
		for (int k = 0; k < p_islands; k++) {
			island_cells[k] = 0;
		}
		for (int c = 0; c < n; c++) {
			if (idx[c] >= n) {
				x[idx[c]] += p_psi[c];
				island_cells[idx[c] - n]++;
			} else if (idx[c] >= 0) {
				x[c] = p_psi[c];
			}
		}
		for (int k = 0; k < p_islands; k++) {
			x[n + k] /= MAX(island_cells[k], 1);
		}
	}
	while (levels[levels.size() - 1].nx > 2 || levels[levels.size() - 1].nz > 2) {
		levels.resize(levels.size() + 1);
		coarsen(levels[levels.size() - 2], levels[levels.size() - 1]);
	}

	// y = A x: the grid stencil (in double), then the island links.
	auto apply = [&](const double *p_x, double *p_y) {
		const float *cx = levels[0].cx.ptr();
		const float *cz = levels[0].cz.ptr();
		const double *dg = diag.ptr();
		for (int c = 0; c < n; c++) {
			double s = dg[c] * p_x[c];
			if (c > 0) {
				s -= cx[c - 1] * p_x[c - 1];
			}
			if (c + 1 < n) {
				s -= cx[c] * p_x[c + 1];
			}
			if (c >= p_nx) {
				s -= cz[c - p_nx] * p_x[c - p_nx];
			}
			if (c + p_nx < n) {
				s -= cz[c] * p_x[c + p_nx];
			}
			p_y[c] = s;
		}
		for (int k = n; k < dim; k++) {
			p_y[k] = dg[k] * p_x[k];
		}
		for (const Link &l : island_links) {
			p_y[l.a] -= l.w * p_x[l.b];
			p_y[l.b] -= l.w * p_x[l.a];
		}
	};
	// z = M r: one symmetric V-cycle on the grid unknowns, Jacobi on the islands.
	auto precondition = [&](const double *p_r, double *p_z) {
		float *b = levels[0].b.ptr();
		const float *id = levels[0].inv_diag.ptr();
		for (int c = 0; c < n; c++) {
			b[c] = id[c] != 0.0f ? (float)p_r[c] : 0.0f;
		}
		v_cycle(levels, 0);
		const float *xv = levels[0].x.ptr();
		for (int c = 0; c < n; c++) {
			p_z[c] = xv[c];
		}
		for (int k = n; k < dim; k++) {
			p_z[k] = diag[k] > 0.0 ? p_r[k] / diag[k] : 0.0;
		}
	};

	// Preconditioned conjugate gradients, in double.
	LocalVector<double> r;
	LocalVector<double> zv;
	LocalVector<double> p;
	LocalVector<double> ap;
	r.resize(dim);
	zv.resize(dim);
	p.resize(dim);
	ap.resize(dim);
	double *xp = x.ptr();
	double *rp = r.ptr();
	double *zp = zv.ptr();
	double *pp = p.ptr();
	double *app = ap.ptr();
	apply(xp, app);
	double b_norm = 0.0;
	double r_norm = 0.0;
	for (int i = 0; i < dim; i++) {
		rp[i] = diag[i] > 0.0 ? rhs[i] - app[i] : 0.0;
		b_norm += rhs[i] * rhs[i];
		r_norm += rp[i] * rp[i];
	}
	const double target = p_rel_tol * p_rel_tol * MAX(b_norm, 1e-300);
	int iter = 0;
	if (r_norm > target) {
		precondition(rp, zp);
		double rz = 0.0;
		for (int i = 0; i < dim; i++) {
			pp[i] = zp[i];
			rz += rp[i] * zp[i];
		}
		for (iter = 1; iter <= p_max_iter; iter++) {
			apply(pp, app);
			double pap = 0.0;
			for (int i = 0; i < dim; i++) {
				pap += pp[i] * app[i];
			}
			if (!(pap > 0.0)) {
				break;
			}
			const double alpha = rz / pap;
			r_norm = 0.0;
			for (int i = 0; i < dim; i++) {
				xp[i] += alpha * pp[i];
				rp[i] -= alpha * app[i];
				r_norm += rp[i] * rp[i];
			}
			if (r_norm <= target) {
				break;
			}
			precondition(rp, zp);
			double rz_new = 0.0;
			for (int i = 0; i < dim; i++) {
				rz_new += rp[i] * zp[i];
			}
			const double beta = rz_new / rz;
			rz = rz_new;
			for (int i = 0; i < dim; i++) {
				pp[i] = zp[i] + beta * pp[i];
			}
		}
	}
	for (int c = 0; c < n; c++) {
		if (idx[c] >= 0 && diag[idx[c]] > 0.0) {
			p_psi[c] = (float)xp[idx[c]];
		}
	}
	return iter;
}

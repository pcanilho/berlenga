/**************************************************************************/
/*  flow_poisson.h                                                        */
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

#include "core/templates/local_vector.h"

// The stream-function solve the flow bakes share (PhysXWaterSurface3D's sea and lake current,
// PhysXRiver3D's diffusive-wave river): a symmetric, conductance-weighted Laplace problem on a grid,
// div(c grad psi) = 0, with fixed values (the banks, the domain edge) and islands (a rock or an islet,
// one unknown psi for the whole patch, set so no net flow goes round it).
//
// Multigrid-preconditioned conjugate gradients (McAdams, Sifakis and Teran 2010; the V-cycle after
// Briggs, Henson and McCormick, A Multigrid Tutorial): 2 x 2 aggregation with the Galerkin coarse
// operator (coarse conductance = the fine conductances crossing between aggregates), the coarse
// correction scaled up as unsmoothed aggregation wants, a forward Gauss-Seidel sweep before and a
// backward one after, so the preconditioner is symmetric; islands get a Jacobi step beside the V-cycle.
// CG then converges in 15 to 20 iterations whatever the grid size, banks and islands (SOR needed
// hundreds to thousands of sweeps).
class FlowPoisson {
public:
	enum {
		UNKNOWN = 0,
		FIXED = 1,
		ISLAND = 2, // ISLAND + k: island k
	};

	// p_nx x p_nz cells, row-major (c = z * p_nx + x).
	// p_cx[c]: the conductance of the face between c and c + 1 (along x); p_cz[c]: between c and c + p_nx.
	// p_kind[c]: UNKNOWN, FIXED (p_psi[c] holds its value) or ISLAND + k.
	// p_psi: fixed values and the starting guess in; the solution out (island cells all take their island's).
	// Converged when the residual falls below p_rel_tol of the right-hand side's. Returns the CG iterations.
	static int solve(int p_nx, int p_nz, const LocalVector<float> &p_cx, const LocalVector<float> &p_cz, const LocalVector<int> &p_kind, int p_islands, LocalVector<float> &p_psi, double p_rel_tol = 1e-6, int p_max_iter = 400);
};

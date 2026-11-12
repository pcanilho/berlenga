/**************************************************************************/
/*  test_xpbd_cloth_grid.h                                                */
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

#include "../cloth/xpbd_cloth_solver.h"

#include "tests/test_macros.h"

namespace TestXPBDClothGrid {

TEST_CASE("[Modules][PhysX] Generated cloth grid faces +Z") {
	XPBDClothSolver solver;
	solver.build_grid(4, 5, Vector2(0.3, 0.42));
	solver.reset(Transform3D());
	const LocalVector<Vector3> &pos = solver.get_positions();
	const LocalVector<int> &tri = solver.get_indices();
	REQUIRE(tri.size() == 3u * 2u * 3u * 4u);
	for (uint32_t t = 0; t + 2 < tri.size(); t += 3) {
		const Vector3 a = pos[tri[t]];
		const Vector3 b = pos[tri[t + 1]];
		const Vector3 c = pos[tri[t + 2]];
		// Godot's front faces wind clockwise: their normal is (c - a) x (b - a).
		CHECK_MESSAGE((c - a).cross(b - a).z > 0.0f, "Triangle ", t / 3, " faces away from +Z.");
	}
}

// A strip hung by its top edge and pulled down far harder than it can hold.
static void _hang_strip(XPBDClothSolver &r_solver, float p_tear_factor) {
	r_solver.settings.tear_factor = p_tear_factor;
	r_solver.settings.stretch_compliance = 5.0e-3f; // the softest stiffness maps to
	r_solver.build_grid(3, 12, Vector2(0.075, 1.1));
	for (int x = 0; x < 3; x++) {
		r_solver.set_pinned(x, true);
	}
	r_solver.reset(Transform3D());
}

static float _lowest(const XPBDClothSolver &p_solver) {
	float y = 1.0e20f;
	for (uint32_t i = 0; i < p_solver.get_positions().size(); i++) {
		y = MIN(y, p_solver.get_positions()[i].y);
	}
	return y;
}

TEST_CASE("[Modules][PhysX] Cloth without a tear factor never tears") {
	XPBDClothSolver solver;
	_hang_strip(solver, 0.0f);
	const int n = solver.vertex_count();
	int torn = 0;
	for (int f = 0; f < 120; f++) {
		torn += solver.step(1.0f / 60.0f, Vector3(0, -400.0f, 0), Vector3());
	}
	CHECK(torn == 0);
	CHECK(solver.vertex_count() == n);
	// and the same load as above, under its limit (about 1.11 x rest at 400 m/s²), holds with tearing on
	XPBDClothSolver held;
	_hang_strip(held, 1.15f);
	int held_torn = 0;
	for (int f = 0; f < 120; f++) {
		held_torn += held.step(1.0f / 60.0f, Vector3(0, -400.0f, 0), Vector3());
	}
	CHECK(held_torn == 0);
	CHECK(_lowest(solver) > -3.0f);
}

TEST_CASE("[Modules][PhysX] Overstretched cloth tears apart and the piece falls away") {
	XPBDClothSolver solver;
	_hang_strip(solver, 1.15f);
	const int n = solver.vertex_count();
	const int tris = solver.get_indices().size();
	// 0.2 kg/m² x 0.0825 m² at 4000 m/s² is 66 N on three edges along it, each 5e-3 m/N soft over 10 cm:
	// about twice their rest length, far past 1.15.
	int torn = 0;
	for (int f = 0; f < 120; f++) {
		torn += solver.step(1.0f / 60.0f, Vector3(0, -4000.0f, 0), Vector3());
	}
	CHECK(torn > 0);
	CHECK(solver.vertex_count() == n + torn);
	// Splitting keeps every triangle (unlike a cut), and the texture follows each new vertex.
	CHECK((int)solver.get_indices().size() == tris);
	CHECK((int)solver.get_uvs().size() == solver.vertex_count());
	// Free of the top edge, the strip drops: 2 s at 4000 m/s² is far past anything still hanging.
	CHECK(_lowest(solver) < -50.0f);
	for (uint32_t i = 0; i < solver.get_positions().size(); i++) {
		const Vector3 p = solver.get_positions()[i];
		REQUIRE_MESSAGE(p.is_finite(), "Vertex ", i, " is not finite after tearing.");
	}
}

TEST_CASE("[Modules][PhysX] A tear opens a few vertices a step") {
	XPBDClothSolver solver;
	_hang_strip(solver, 1.01f);
	for (int f = 0; f < 10; f++) {
		CHECK(solver.step(1.0f / 60.0f, Vector3(0, -4000.0f, 0), Vector3()) <= XPBDClothSolver::MAX_TEARS_PER_STEP);
	}
}

} // namespace TestXPBDClothGrid

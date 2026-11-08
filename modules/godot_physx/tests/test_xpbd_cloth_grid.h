/**************************************************************************/
/*  test_xpbd_cloth_grid.h                                                            */
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

} // namespace TestXPBDClothGrid

/**************************************************************************/
/*  plant_browse.cpp                                                      */
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


// Deer browsing on the ground flora (docs/ideas/vegetation.md, "Deer browsing"): browsed bramble and raspberry (their
// builders read _browse), the grazed lawns deer keep in clearings and rides, and the floor's response to pressure in
// make_floor (PlantSpecies.RULE_BROWSE). Gill 1992; Kirby 2001; Gill and Fuller 2007, Ibis 149; Morecroft et al. 2001
// (Wytham exclosures); Kuijper et al. 2009 (grazing lawns in Bialowieza); Putman 1986.

#include "plant_generator.h"

namespace {
Basis browse_frame(const Vector3 &p_forward) {
	const Vector3 z = p_forward.normalized();
	Vector3 x = Vector3(0, 1, 0).cross(z);
	if (x.length_squared() < 1e-8f) {
		x = Vector3(1, 0, 0);
	}
	x.normalize();
	return Basis(x, z.cross(x).normalized(), z);
}
} // namespace

float PlantGenerator::_browse() const {
	return season.has("browse") ? CLAMP(float(season["browse"]), 0.0f, 1.0f) : 0.0f;
}

// A deer lawn (Putman 1986; Kuijper et al. 2009): where deer graze the same ground through the year, fine bents and
// red fescue (Agrostis capillaris, Festuca rubra) and creeping herbs form a dense, even sward 2 to 8 cm high. Most blade
// ends are torn rather than cut (deer have no upper incisors) and dry pale for a few days; the regrowth under them is
// bright green. No flowering stems survive.
void PlantGenerator::_build_grazed_lawn() {
	// Many tillers close together: a mat, not a tuft.
	const int blades = int(_rand(150.0f, 220.0f));
	for (int i = 0; i < blades; i++) {
		Leaf l;
		l.shape = LEAF_LINEAR;
		l.length = _rand(0.02f, 0.065f);
		l.width = _rand(0.0016f, 0.003f);
		l.fold = 0.4f;
		l.bend = _rand(0.15f, 0.8f);
		l.twist = _rand(-0.5f, 0.5f);
		l.segments = 4;
		l.cut = rng.randf() < 0.65f ? _rand(0.2f, 0.5f) : 0.0f;
		const float a = _rand(0.0f, Math::TAU);
		const float rr = 0.09f * Math::sqrt(_rand(0.0f, 1.0f));
		const Vector3 at(Math::cos(a) * rr, 0.0f, Math::sin(a) * rr);
		const float az = _rand(0.0f, Math::TAU);
		const float el = _rand(0.95f, 1.5f);
		const Vector3 d(Math::cos(az) * Math::cos(el), Math::sin(el), Math::sin(az) * Math::cos(el));
		// Uncut blades are the new leaves: a shade lighter.
		const float shade = l.cut > 0.0f ? _rand(0.85f, 1.0f) : _rand(1.0f, 1.12f);
		_add_leaf(SURFACE_LEAF, Transform3D(browse_frame(d), at), l, 0.0f, 0.85f, Color(shade, shade, shade * 0.95f));
	}
}

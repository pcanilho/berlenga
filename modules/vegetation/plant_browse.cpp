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
// red fescue (Agrostis capillaris, Festuca rubra) form a dense, even turf cropped to 2 to 5 cm (some 20,000 to 40,000
// tillers a square metre in grazed swards; Hodgson 1990), with white clover and a mossy thatch in it. Most blade ends
// are torn rather than cut (deer have no upper incisors) and dry pale for a few days; uncut new leaves stand a little
// above the crop. No flowering stems survive. One mesh covers a 20 cm cell.
void PlantGenerator::_build_grazed_lawn() {
	const float crop = _rand(0.025f, 0.04f); // The height the sward is grazed to here.
	// Tillers: hundreds, close together.
	const int blades = int(_rand(300.0f, 380.0f));
	for (int i = 0; i < blades; i++) {
		Leaf l;
		l.shape = LEAF_LINEAR;
		l.width = _rand(0.0009f, 0.0018f); // Fine-leaved bents and fescues, rolled.
		l.fold = 0.5f;
		l.bend = _rand(0.1f, 0.5f);
		l.twist = _rand(-0.4f, 0.4f);
		l.segments = 2; // Short blades: two rows bend enough; keeps a cell near 3k triangles.
		const float el = _rand(1.1f, 1.5f);
		const bool grazed = rng.randf() < 0.82f;
		if (grazed) {
			// Grown past the crop and bitten off at it: the kept part reaches the crop height (+-15%).
			const float keep = crop * _rand(0.85f, 1.15f) / Math::sin(el);
			l.length = keep * _rand(1.3f, 2.0f);
			l.cut = 1.0f - keep / l.length;
		} else {
			l.length = crop * _rand(0.6f, 1.5f) / Math::sin(el); // Young leaves come up between the bitten ones.
		}
		// A 24 cm cell, thinning over its rim so neighbouring cells (placed about 20 cm apart) knit together.
		const Vector3 at(_rand(-0.12f, 0.12f) * _rand(0.7f, 1.0f), 0.0f, _rand(-0.12f, 0.12f) * _rand(0.7f, 1.0f));
		const float az = _rand(0.0f, Math::TAU);
		const Vector3 d(Math::cos(az) * Math::cos(el), Math::sin(el), Math::sin(az) * Math::cos(el));
		const float shade = grazed ? _rand(0.8f, 0.95f) : _rand(1.0f, 1.15f);
		_add_leaf(SURFACE_LEAF, Transform3D(browse_frame(d), at), l, 0.0f, 0.8f, Color(shade, shade, shade * 0.95f));
	}
	// The thatch: older blades lying flat at the base, darker, filling the gaps to the ground.
	const int thatch = int(_rand(50.0f, 70.0f));
	for (int i = 0; i < thatch; i++) {
		Leaf l;
		l.shape = LEAF_LINEAR;
		l.width = _rand(0.0012f, 0.002f);
		l.length = _rand(0.02f, 0.045f);
		l.fold = 0.3f;
		l.bend = _rand(0.0f, 0.3f);
		l.segments = 2;
		const float a = _rand(0.0f, Math::TAU);
		const float rr = 0.12f * Math::sqrt(_rand(0.0f, 1.0f));
		const float az = _rand(0.0f, Math::TAU);
		const float el = _rand(0.05f, 0.25f);
		const Vector3 d(Math::cos(az) * Math::cos(el), Math::sin(el), Math::sin(az) * Math::cos(el));
		_add_leaf(SURFACE_LEAF, Transform3D(browse_frame(d), Vector3(Math::cos(a) * rr, 0.003f, Math::sin(a) * rr)), l, 0.0f, 0.55f, Color(0.7f, 0.68f, 0.5f));
	}
	// White clover (Trifolium repens) creeping through: trifoliate leaves on stalks at the crop height, the leaflets
	// round, 6 to 10 mm, held flat (grazing keeps them small).
	const int clovers = int(_rand(3.0f, 9.0f));
	for (int i = 0; i < clovers; i++) {
		const float a = _rand(0.0f, Math::TAU);
		const float rr = 0.09f * Math::sqrt(_rand(0.0f, 1.0f));
		const Vector3 base(Math::cos(a) * rr, 0.0f, Math::sin(a) * rr);
		const float az = _rand(0.0f, Math::TAU);
		const Vector3 sd(Math::cos(az) * 0.25f, 1.0f, Math::sin(az) * 0.25f);
		const float stalk = crop * _rand(0.6f, 1.0f);
		const Transform3D top = _add_stem(Transform3D(browse_frame(sd), base), stalk, 0.0006f, 0.0005f, 0.1f, 2, 0.0f);
		const float spin = _rand(0.0f, Math::TAU);
		for (int q = 0; q < 3; q++) {
			const float b = spin + q * Math::TAU / 3.0f;
			const Vector3 ld(Math::cos(b), _rand(0.05f, 0.2f), Math::sin(b));
			Leaf l;
			l.shape = LEAF_ROUND;
			l.length = _rand(0.006f, 0.01f);
			l.width = l.length * 0.85f;
			l.fold = 0.15f;
			l.bend = 0.2f;
			l.segments = 4;
			_add_leaf(SURFACE_PETAL, Transform3D(browse_frame(ld), top.origin), l, 0.6f, 0.95f);
		}
	}
}

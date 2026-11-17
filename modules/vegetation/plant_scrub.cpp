/**************************************************************************/
/*  plant_scrub.cpp                                                       */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/* Copyright (c) 2026-present Paulo Canilho and Berlenga contributors.    */
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

// Mediterranean scrub (docs/ideas/vegetation.md, "Next round candidates (17 Nov)"): the low,
// aromatic, evergreen dwarf shrubs of garrigue and maquis. Measurements from Flora iberica
// (Castroviejo et al.: vol. 3 Cistaceae, vol. 12 Lamiaceae), Blondel et al. 2010 ("The
// Mediterranean Region") and Stace (2019) for the cultivated species. The tall woody shrubs of the
// maquis (gum rockrose, kermes oak, lentisk, dwarf fan palm) come from TreeGenerator as shrub
// species and take the same rules through apply_scrub_rules().

#include "plant_generator.h"
#include "plant_species.h"

namespace {
const float SC_GOLDEN = 2.39996323f;

Basis sc_frame(const Vector3 &p_forward, const Vector3 &p_up_hint) {
	const Vector3 z = p_forward.normalized();
	Vector3 x = p_up_hint.cross(z);
	if (x.length_squared() < 1e-8f) {
		x = Vector3(1, 0, 0).cross(z);
		if (x.length_squared() < 1e-8f) {
			x = Vector3(0, 0, 1).cross(z);
		}
	}
	x.normalize();
	return Basis(x, z.cross(x).normalized(), z);
}

Vector3 sc_dir(float p_azimuth, float p_elevation) {
	return Vector3(Math::cos(p_azimuth) * Math::cos(p_elevation), Math::sin(p_elevation), Math::sin(p_azimuth) * Math::cos(p_elevation));
}

// The direction at angle p_a round a shoot frame, across its axis.
Vector3 sc_radial(const Transform3D &p_at, float p_a) {
	return p_at.basis.get_column(0) * Math::cos(p_a) + p_at.basis.get_column(1) * Math::sin(p_a);
}
} // namespace

// Rosemary (Salvia rosmarinus): an erect, dense evergreen shrub 0.5 to 1.5 m (garrigue plants
// browsed and wind-cut to 40 to 80 cm); leaves linear, 1.5 to 4 cm by 2 to 4 mm, leathery, the
// margins rolled under, dark green above and white-felted below, opposite and crowded towards the
// shoot tips; pale blue two-lipped flowers 1 to 1.2 cm in short axillary clusters near the tips,
// from late autumn to spring (Flora iberica 12; Upson and Andrews 2004 for the habit).
void PlantGenerator::_build_rosemary() {
	leaf_wear = 0.1f;
	Leaf lf;
	lf.shape = LEAF_LINEAR;
	lf.width = 0.0032f;
	lf.fold = 0.45f; // Rolled margins.
	lf.bend = 0.25f;
	lf.segments = 1;
	Leaf lip;
	lip.shape = LEAF_ROUND;
	lip.fold = 0.3f;
	lip.bend = 0.5f;
	lip.segments = 2;
	float progress = 0.0f, dead = 0.0f;
	const Bloom bloom = _bloom(false, &progress, &dead);
	auto shoot = [&](const LocalVector<Transform3D> &p_fr, float p_wind, bool p_flower) {
		const float l = p_fr[0].origin.distance_to(p_fr[p_fr.size() - 1].origin);
		const int nodes = MAX(int(l / 0.0082f), 3);
		for (int k = 0; k < nodes; k++) {
			const float s = 0.15f + 0.85f * float(k) / MAX(nodes - 1, 1);
			const Transform3D at = _frame_at(p_fr, s);
			// Decussate pairs, the leaves longest at mid-shoot, short and erect at the tip.
			const float a0 = (k & 1) * Math::PI * 0.5f;
			const float size = Math::lerp(0.9f, 0.55f, s * s);
			for (int q = 0; q < 2; q++) {
				const Vector3 radial = sc_radial(at, a0 + q * Math::PI);
				lf.length = _rand(0.02f, 0.034f) * size;
				const float up = Math::lerp(0.35f, 1.3f, s * s);
				_add_leaf(SURFACE_LEAF, Transform3D(sc_frame((radial + at.basis.get_column(2) * up).normalized(), at.basis.get_column(2)), at.origin), lf, Math::lerp(p_wind, 1.0f, s));
			}
			if (p_flower && s > 0.55f && s < 0.95f && _rand(0.0f, 1.0f) < 0.35f) {
				// A small axillary cluster: one to three flowers, the lower lip hanging, the upper
				// hooded, the two stamens arching out.
				const Wither w(this, bloom == BLOOM_SEED ? 0.8f : dead * 0.6f);
				const int nf = int(_rand(1.0f, 3.99f));
				for (int f = 0; f < nf; f++) {
					const Vector3 radial = sc_radial(at, a0 + Math::PI * 0.5f + (f & 1) * Math::PI + _rand(-0.4f, 0.4f));
					const Vector3 fd = (radial + Vector3(0, 0.35f, 0)).normalized();
					const Vector3 fat = at.origin + radial * 0.004f;
					_add_head(SURFACE_CENTRE, Transform3D(sc_frame(fd, Vector3(0, 1, 0)), fat), 0.0014f, 0.005f, Color(1, 1, 1), 2, 5); // The calyx.
					const Vector3 mouth = fat + fd * 0.007f;
					lip.length = _rand(0.006f, 0.0075f);
					lip.width = lip.length * 0.9f;
					_add_leaf(SURFACE_PETAL, Transform3D(sc_frame((fd - Vector3(0, 0.9f, 0)).normalized(), fd), mouth), lip, 1.0f); // Lower lip.
					lip.length = _rand(0.005f, 0.006f);
					lip.width = lip.length * 0.75f;
					_add_leaf(SURFACE_PETAL, Transform3D(sc_frame((fd + Vector3(0, 0.8f, 0)).normalized(), fd), mouth), lip, 1.0f); // Upper lip, a hood.
				}
			}
		}
	};
	// Erect woody stems from a short trunk, branching all the way up into laterals that carry
	// short leafy side shoots: the dense, rounded mass of foliage, not bare rods.
	const int stems = int(_rand(7.0f, 10.0f));
	const float spin = _rand(0.0f, Math::TAU);
	const float tall = _rand(0.4f, 0.8f);
	for (int i = 0; i < stems; i++) {
		const float az = spin + i * SC_GOLDEN;
		const Transform3D base(sc_frame(sc_dir(az, _rand(0.85f, 1.3f)), Vector3(0, 1, 0)), Vector3(_rand(-0.03f, 0.03f), 0.0f, _rand(-0.03f, 0.03f)));
		const float len = tall * _rand(0.55f, 1.0f);
		const LocalVector<Transform3D> fr = _stem_frames(base, len, _rand(-0.3f, 0.05f), 6);
		_add_stem_along(fr, 0.007f, 0.0025f, 0.0f);
		const Vector3 outward = Vector3(Math::cos(az), 0.0f, Math::sin(az));
		const int subs = int(_rand(7.0f, 10.0f));
		for (int k = 0; k < subs; k++) {
			const float u = _rand(0.15f, 0.92f);
			const Transform3D at = _frame_at(fr, u);
			const float a = _rand(0.0f, Math::TAU);
			const Vector3 d = (outward * 0.65f + Vector3(Math::cos(a) * 0.35f, 0.9f, Math::sin(a) * 0.35f)).normalized();
			const float bl = len * _rand(0.22f, 0.38f) * (1.15f - u * 0.5f);
			const LocalVector<Transform3D> br = _stem_frames(Transform3D(sc_frame(d, Vector3(0, 1, 0)), at.origin), bl, _rand(-0.2f, 0.1f), 4);
			_add_stem_along(br, 0.0024f, 0.0011f, 0.3f);
			const bool flower = bloom != BLOOM_NONE && _rand(0.0f, 1.0f) < 0.7f;
			shoot(br, 0.35f, flower);
			const int sides = int(_rand(1.0f, 3.99f));
			for (int j = 0; j < sides; j++) {
				const Transform3D bat = _frame_at(br, _rand(0.25f, 0.8f));
				const float b = _rand(0.0f, Math::TAU);
				const Vector3 sd = (bat.basis.get_column(2) + bat.basis.get_column(0) * Math::cos(b) * 0.8f + bat.basis.get_column(1) * Math::sin(b) * 0.8f + Vector3(0, 0.3f, 0)).normalized();
				const LocalVector<Transform3D> sh = _stem_frames(Transform3D(sc_frame(sd, Vector3(0, 1, 0)), bat.origin), _rand(0.035f, 0.07f), -0.1f, 2);
				_add_stem_along(sh, 0.0011f, 0.0007f, 0.5f);
				shoot(sh, 0.5f, flower && _rand(0.0f, 1.0f) < 0.5f);
			}
		}
		// The stem's own leafy top.
		LocalVector<Transform3D> top;
		for (uint32_t k = fr.size() / 2; k < fr.size(); k++) {
			top.push_back(fr[k]);
		}
		shoot(top, 0.3f, bloom != BLOOM_NONE);
	}
	plant_height = MAX(plant_height, 0.3f);
}

// French lavender (Lavandula stoechas): a compact, much-branched shrub 30 to 100 cm, grey-felted;
// leaves linear to oblong, 1 to 4 cm by 2 to 5 mm, margins rolled, crowded and in leafy axillary
// tufts; the flowering shoots end in a short bare stalk (1 to 3 cm in subsp. stoechas) under a
// dense, four-angled spike 2 to 3 cm of dark purple flowers, crowned by three to five large purple
// sterile bracts 1 to 5 cm ("rabbit ears"), March to June. Acid, sandy and schist soils
// (Flora iberica 12; Upson and Andrews 2004, "The genus Lavandula").
void PlantGenerator::_build_french_lavender() {
	leaf_wear = 0.1f;
	Leaf lf;
	lf.shape = LEAF_LINEAR;
	lf.width = 0.0036f;
	lf.fold = 0.4f;
	lf.bend = 0.2f;
	lf.segments = 1;
	Leaf bract;
	bract.shape = LEAF_OVATE;
	bract.fold = 0.3f;
	bract.bend = -0.2f; // Flaring back at the tip.
	bract.twist = 0.3f;
	bract.segments = 4;
	// A short, much-branched woody base; from its tips and upper half the upright leafy shoots
	// rise over a dome, so the bush is rounded and leafy to the ground, the spikes held above.
	LocalVector<Transform3D> starts;
	const int woody = int(_rand(5.0f, 8.0f));
	const float spin = _rand(0.0f, Math::TAU);
	const float tall = _rand(0.15f, 0.26f);
	for (int i = 0; i < woody; i++) {
		const float az = spin + i * SC_GOLDEN;
		const Transform3D base(sc_frame(sc_dir(az, _rand(0.3f, 0.75f)), Vector3(0, 1, 0)), Vector3(_rand(-0.02f, 0.02f), 0.0f, _rand(-0.02f, 0.02f)));
		const LocalVector<Transform3D> fr = _stem_frames(base, _rand(0.08f, 0.16f), _rand(-0.3f, 0.0f), 3);
		_add_stem_along(fr, 0.0045f, 0.003f, 0.0f);
		const Vector3 outward = Vector3(Math::cos(az), 0.0f, Math::sin(az));
		const int n = int(_rand(4.0f, 6.99f));
		for (int k = 0; k < n; k++) {
			const float u = k == 0 ? 1.0f : _rand(0.3f, 0.95f);
			const Transform3D at = _frame_at(fr, u);
			const float a = _rand(0.0f, Math::TAU);
			// The outer shoots lean out with the woody stem, so the bush is a dome, not a fan.
			const Vector3 d = (outward * Math::lerp(0.3f, 0.75f, u) + Vector3(Math::cos(a) * 0.3f, 1.0f, Math::sin(a) * 0.3f)).normalized();
			starts.push_back(Transform3D(sc_frame(d, Vector3(0, 1, 0)), at.origin));
		}
	}
	for (const Transform3D &base : starts) {
		float progress = 0.0f, dead = 0.0f;
		const Bloom bloom = _bloom(false, &progress, &dead);
		const float len = tall * _rand(0.6f, 1.0f);
		const LocalVector<Transform3D> fr = _stem_frames(base, len, _rand(-0.15f, 0.15f), 5);
		_add_stem_along(fr, 0.0022f, 0.0011f, 0.15f);
		// The leafy part: the lower two thirds of the shoot, leaves in opposite pairs with
		// axillary tufts; the top a short bare stalk under the spike.
		const float leafy = 0.68f;
		const int nodes = MAX(int(len * leafy / 0.0075f), 3);
		for (int k = 0; k < nodes; k++) {
			const float s = 0.1f + (leafy - 0.1f) * float(k) / MAX(nodes - 1, 1);
			const Transform3D at = _frame_at(fr, s);
			const float a0 = (k & 1) * Math::PI * 0.5f;
			for (int q = 0; q < 2; q++) {
				const Vector3 radial = sc_radial(at, a0 + q * Math::PI);
				lf.length = _rand(0.016f, 0.03f);
				_add_leaf(SURFACE_LEAF, Transform3D(sc_frame((radial + at.basis.get_column(2) * 0.9f).normalized(), at.basis.get_column(2)), at.origin), lf, Math::lerp(0.15f, 1.0f, s));
				if (s < 0.62f && _rand(0.0f, 1.0f) < 0.75f) {
					// The axillary tuft of short leaves.
					for (int t = 0; t < 3; t++) {
						const Vector3 td = (radial * 0.6f + at.basis.get_column(2) + sc_radial(at, a0 + q * Math::PI + (t - 1) * 0.6f) * 0.3f).normalized();
						lf.length = _rand(0.008f, 0.013f);
						_add_leaf(SURFACE_LEAF, Transform3D(sc_frame(td, at.basis.get_column(2)), at.origin + radial * 0.002f), lf, Math::lerp(0.15f, 1.0f, s));
					}
				}
			}
		}
		if (bloom == BLOOM_NONE) {
			continue;
		}
		// The spike on its short bare stalk, the bracts on top.
		const Wither w(this, bloom == BLOOM_SEED ? 0.85f : dead * 0.7f);
		const Transform3D tip = fr[fr.size() - 1];
		const Vector3 ax = tip.basis.get_column(2);
		const float grow = progress < 0.0f ? 1.0f : CLAMP(0.55f + progress * 1.5f, 0.55f, 1.0f);
		const float sl = _rand(0.02f, 0.03f) * grow;
		const float sr = sl * 0.24f;
		// Four-angled, densely packed: a dark core with the calyx rows standing out as knobs.
		_add_head(SURFACE_CENTRE, Transform3D(sc_frame(ax, Vector3(0, 1, 0)), tip.origin), sr, sl, Color(1, 1, 1), 5, 8);
		const int rows = 6;
		for (int k = 0; k < rows; k++) {
			for (int q = 0; q < 4; q++) {
				const float a = q * Math::PI * 0.5f + (k & 1) * Math::PI * 0.25f;
				const Transform3D at(tip.basis, tip.origin + ax * (sl * (0.12f + 0.76f * k / float(rows - 1))));
				const Vector3 radial = sc_radial(at, a);
				const float bulge = Math::sin(Math::PI * (0.12f + 0.76f * k / float(rows - 1)));
				_add_head(SURFACE_CENTRE, Transform3D(sc_frame((radial + ax * 0.4f).normalized(), ax), at.origin + radial * sr * 0.75f * bulge), sr * 0.42f, sr * 0.8f, Color(1, 1, 1), 2, 5);
				if (bloom == BLOOM_FLOWER && _rand(0.0f, 1.0f) < 0.18f) {
					// An open corolla here and there, the small flowers barely out of the calyx.
					Leaf c = bract;
					c.shape = LEAF_ROUND;
					c.length = 0.0035f;
					c.width = 0.0035f;
					c.twist = 0.0f;
					_add_leaf(SURFACE_PETAL, Transform3D(sc_frame((radial + ax * 0.3f).normalized(), ax), at.origin + radial * sr * 1.1f), c, 1.0f, 1.0f, Color(0.7f, 0.55f, 0.75f));
				}
			}
		}
		const int nb = int(_rand(3.0f, 5.99f));
		const Vector3 crown = tip.origin + ax * sl * 0.95f;
		for (int k = 0; k < nb; k++) {
			const float a = k * Math::TAU / nb + _rand(-0.25f, 0.25f);
			const Vector3 radial = sc_radial(tip, a);
			bract.length = _rand(0.014f, 0.024f) * grow;
			bract.width = bract.length * _rand(0.5f, 0.62f);
			_add_leaf(SURFACE_PETAL, Transform3D(sc_frame((ax * 1.6f + radial).normalized(), ax), crown + radial * sr * 0.3f), bract, 1.0f);
		}
	}
	plant_height = MAX(plant_height, 0.2f);
}

// Thyme (Thymus vulgaris and kin, Thymus mastichina on acid ground): a dwarf, cushion-forming
// subshrub 10 to 30 cm of wiry, woody, much-branched stems; leaves 4 to 8 mm by 1.5 to 3 mm,
// grey-green, the margins rolled, in opposite pairs with leafy tufts in the axils; pale pink to
// mauve flowers 4 to 6 mm in rounded terminal heads, April to June (Flora iberica 12; Morales 1986).
void PlantGenerator::_build_thyme() {
	leaf_wear = 0.1f;
	Leaf lf;
	lf.shape = LEAF_LANCEOLATE;
	lf.width = 0.003f;
	lf.fold = 0.4f;
	lf.bend = 0.2f;
	lf.segments = 1;
	Leaf petal;
	petal.shape = LEAF_ROUND;
	petal.fold = 0.2f;
	petal.bend = 0.4f;
	petal.segments = 2;
	// Low woody stems spreading from the root, each branching into many short leafy shoots that
	// pack into the cushion; the flowering tips stand a little above it.
	LocalVector<Transform3D> starts;
	const int woody = int(_rand(6.0f, 10.0f));
	const float spin = _rand(0.0f, Math::TAU);
	const float tall = _rand(0.06f, 0.12f);
	for (int i = 0; i < woody; i++) {
		const float az = spin + i * SC_GOLDEN;
		const Transform3D base(sc_frame(sc_dir(az, _rand(0.3f, 0.8f)), Vector3(0, 1, 0)), Vector3());
		const LocalVector<Transform3D> fr = _stem_frames(base, _rand(0.04f, 0.09f), _rand(-0.2f, 0.1f), 3);
		_add_stem_along(fr, 0.0022f, 0.0014f, 0.0f);
		const Vector3 outward = Vector3(Math::cos(az), 0.0f, Math::sin(az));
		const int n = int(_rand(10.0f, 16.0f));
		for (int k = 0; k < n; k++) {
			const float u = k == 0 ? 1.0f : _rand(0.15f, 0.95f);
			const Transform3D at = _frame_at(fr, u);
			const float a = _rand(0.0f, Math::TAU);
			const Vector3 d = (outward * Math::lerp(0.2f, 0.7f, u) + Vector3(Math::cos(a) * 0.35f, 1.0f, Math::sin(a) * 0.35f)).normalized();
			starts.push_back(Transform3D(sc_frame(d, Vector3(0, 1, 0)), at.origin));
		}
	}
	for (const Transform3D &base : starts) {
		float progress = 0.0f, dead = 0.0f;
		const Bloom bloom = _bloom(false, &progress, &dead);
		const float len = tall * _rand(0.35f, 1.0f);
		const LocalVector<Transform3D> fr = _stem_frames(base, len, _rand(-0.25f, 0.05f), 3);
		_add_stem_along(fr, 0.001f, 0.0006f, 0.3f);
		const int nodes = MAX(int(len / 0.0042f), 3);
		for (int k = 0; k < nodes; k++) {
			const float s = 0.2f + 0.72f * float(k) / MAX(nodes - 1, 1);
			const Transform3D at = _frame_at(fr, s);
			const float a0 = (k & 1) * Math::PI * 0.5f;
			for (int q = 0; q < 2; q++) {
				const Vector3 radial = sc_radial(at, a0 + q * Math::PI);
				lf.length = _rand(0.006f, 0.0085f);
				_add_leaf(SURFACE_LEAF, Transform3D(sc_frame((radial + at.basis.get_column(2) * 0.7f).normalized(), at.basis.get_column(2)), at.origin), lf, Math::lerp(0.25f, 1.0f, s));
				if (_rand(0.0f, 1.0f) < 0.5f) {
					lf.length = _rand(0.003f, 0.005f);
					const Vector3 td = (radial * 0.5f + at.basis.get_column(2)).normalized();
					_add_leaf(SURFACE_LEAF, Transform3D(sc_frame(td, at.basis.get_column(2)), at.origin + radial * 0.0012f), lf, Math::lerp(0.25f, 1.0f, s));
				}
			}
		}
		if (bloom == BLOOM_NONE || _rand(0.0f, 1.0f) > 0.55f) {
			continue;
		}
		// The rounded head: calyces packed in a ball, the small two-lipped flowers open round it.
		const Wither w(this, bloom == BLOOM_SEED ? 0.8f : dead * 0.6f);
		const Transform3D tip = fr[fr.size() - 1];
		const Vector3 ax = tip.basis.get_column(2);
		// The calyces are small, mostly hidden by the open flowers: the head reads pink.
		const float hr = _rand(0.0035f, 0.0045f);
		_add_head(SURFACE_CENTRE, Transform3D(sc_frame(ax, Vector3(0, 1, 0)), tip.origin - ax * hr * 0.3f), hr, hr * 1.6f, Color(1, 1, 1), 3, 7);
		if (bloom == BLOOM_SEED) {
			continue;
		}
		const int nf = int(_rand(12.0f, 18.0f));
		for (int f = 0; f < nf; f++) {
			const float a = f * SC_GOLDEN;
			const float el = _rand(-0.2f, 0.9f);
			const Vector3 d = (sc_radial(tip, a) * Math::cos(el) + ax * Math::sin(el)).normalized();
			const Vector3 at = tip.origin + ax * hr * 0.4f + d * hr * 1.1f;
			petal.length = _rand(0.0045f, 0.0055f);
			petal.width = petal.length * 1.1f;
			_add_leaf(SURFACE_PETAL, Transform3D(sc_frame(d, ax), at), petal, 1.0f); // The broad lower lip.
			petal.length *= 0.6f;
			petal.width = petal.length;
			_add_leaf(SURFACE_PETAL, Transform3D(sc_frame((d + ax * 0.8f).normalized(), d), at), petal, 1.0f);
		}
	}
	plant_height = MAX(plant_height, 0.06f);
}

// Sage-leaved rockrose (Cistus salviifolius): a low, spreading, often sprawling shrub 30 to 60 cm,
// up to a metre across; leaves opposite, ovate to elliptic, 1 to 4 cm, short-stalked, grey-green,
// rough and wrinkled with a sunken net of veins (the sage leaf); white flowers 4 to 5 cm, five
// crumpled-silk petals with a yellow spot at the base round a boss of yellow stamens, singly on long
// stalks (2 to 6 cm) from the upper axils, April to May, each open one morning; then woody
// five-valved capsules in their sepals. In summer drought the leaves fold up and turn grey, and part
// of them fall (summer semi-deciduous: Harley 1985; Correia et al. 1992, Flora iberica 3).
void PlantGenerator::_build_sage_rockrose() {
	leaf_wear = 0.15f;
	Leaf lf;
	lf.shape = LEAF_OVATE;
	lf.petiole = 0.1f;
	lf.fold = 0.3f;
	lf.bend = 0.4f;
	lf.segments = 5;
	Leaf petal;
	petal.shape = LEAF_ROUND;
	petal.fold = 0.15f;
	petal.bend = -0.15f;
	petal.segments = 5;
	// Summer dormancy (July and August, into September): smaller, folded, greyer leaves, fewer of them.
	const int day = bloom_day;
	const float drought = (day >= 180 && day <= 270) ? Math::sin(Math::PI * float(day - 180) / 90.0f) : 0.0f;
	const Color summer = Color(1, 1, 1).lerp(Color(0.97f, 0.88f, 0.7f), drought); // Vertex colours are unorm: a tint can only darken, so the drought shows olive-grey.
	lf.fold = Math::lerp(0.3f, 0.65f, drought);
	float progress = 0.0f, dead = 0.0f;
	const Bloom bloom = _bloom(false, &progress, &dead);
	auto shoot = [&](const LocalVector<Transform3D> &p_fr, float p_wind, bool p_flower) {
		const float l = p_fr[0].origin.distance_to(p_fr[p_fr.size() - 1].origin);
		const int nodes = MAX(int(l / 0.016f), 2);
		for (int k = 0; k < nodes; k++) {
			const float s = 0.25f + 0.75f * float(k) / MAX(nodes - 1, 1);
			const Transform3D at = _frame_at(p_fr, s);
			const float a0 = (k & 1) * Math::PI * 0.5f;
			for (int q = 0; q < 2; q++) {
				if (drought > 0.0f && _rand(0.0f, 1.0f) < drought * 0.35f * (1.0f - s)) {
					continue; // The older leaves shed in the drought.
				}
				const Vector3 radial = sc_radial(at, a0 + q * Math::PI);
				lf.length = _rand(0.018f, 0.032f) * Math::lerp(1.0f, 0.75f, drought) * Math::lerp(1.0f, 0.7f, s * s);
				lf.width = lf.length * _rand(0.5f, 0.62f);
				const Vector3 d = (radial + at.basis.get_column(2) * 0.35f + Vector3(0, 0.25f, 0)).normalized();
				_add_leaf(SURFACE_LEAF, Transform3D(sc_frame(d, Vector3(0, 1, 0)), at.origin), lf, Math::lerp(p_wind, 1.0f, s), 1.0f, summer);
			}
			if (!p_flower || s < 0.55f || _rand(0.0f, 1.0f) > 0.3f) {
				continue;
			}
			// A flower or capsule on its long, slender stalk, held out above the leaves.
			const float side = a0 + Math::PI * 0.5f + (_rand(0.0f, 1.0f) < 0.5f ? 0.0f : Math::PI);
			const Vector3 radial = sc_radial(at, side);
			const Vector3 pd = (radial * 0.6f + Vector3(0, 1.0f, 0)).normalized();
			const Transform3D top = _add_stem(Transform3D(sc_frame(pd, Vector3(0, 1, 0)), at.origin), _rand(0.025f, 0.05f), 0.0008f, 0.0006f, 0.25f, 3, 0.6f);
			const Basis fb = sc_frame((top.basis.get_column(2) + Vector3(0, 1.2f, 0)).normalized(), radial);
			// The sepals: five, the outer heart-shaped.
			Leaf sepal = lf;
			sepal.length = 0.009f;
			sepal.width = 0.0065f;
			sepal.fold = 0.4f;
			sepal.bend = -0.2f;
			sepal.segments = 3;
			sepal.petiole = 0.0f;
			if (bloom == BLOOM_FLOWER && _rand(0.0f, 1.0f) < 0.6f) {
				const float open = progress < 0.0f ? 1.0f : CLAMP(0.4f + progress * 1.2f, 0.4f, 1.0f);
				const float pr = _rand(0.019f, 0.024f) * open;
				const float spin = _rand(0.0f, Math::TAU);
				for (int p = 0; p < 5; p++) {
					const float a = spin + p * Math::TAU / 5.0f + _rand(-0.1f, 0.1f);
					const Vector3 pr_dir = fb.get_column(0) * Math::cos(a) + fb.get_column(1) * Math::sin(a);
					const float rise = Math::lerp(1.1f, 0.25f, open) + _rand(-0.1f, 0.15f);
					const Vector3 fwd = (pr_dir * Math::cos(rise) + fb.get_column(2) * Math::sin(rise)).normalized();
					petal.length = pr;
					petal.width = pr * _rand(1.05f, 1.2f); // Broader than long, overlapping.
					petal.twist = _rand(-0.25f, 0.25f); // Crumpled silk.
					petal.fold = _rand(0.1f, 0.3f);
					_add_leaf(SURFACE_PETAL, Transform3D(sc_frame(fwd, fb.get_column(2)), top.origin + pr_dir * 0.002f), petal, 1.0f);
				}
				// The boss of many yellow stamens round the ovary.
				_add_head(SURFACE_CENTRE, Transform3D(fb, top.origin), 0.0055f * open, 0.005f, Color(1, 1, 1), 3, 9);
			} else if (bloom != BLOOM_NONE) {
				// The capsule in its sepals (or a bud, before).
				const Wither w(this, bloom == BLOOM_SEED ? 0.85f : 0.0f);
				const bool capsule = bloom == BLOOM_SEED;
				_add_head(SURFACE_CENTRE, Transform3D(fb, top.origin), capsule ? 0.0035f : 0.004f, capsule ? 0.007f : 0.009f, capsule ? Color(0.75f, 0.55f, 0.3f) : Color(0.45f, 0.65f, 0.35f), 3, 6);
				for (int p = 0; p < 5; p++) {
					const float a = p * Math::TAU / 5.0f;
					const Vector3 sd = (fb.get_column(0) * Math::cos(a) + fb.get_column(1) * Math::sin(a) + fb.get_column(2) * 1.6f).normalized();
					_add_leaf(SURFACE_LEAF, Transform3D(sc_frame(sd, fb.get_column(2)), top.origin), sepal, 1.0f, 1.0f, summer);
				}
			}
		}
	};
	// Spreading, sprawling woody stems, branching out sideways into leafy shoots.
	const int stems = int(_rand(5.0f, 8.0f));
	const float spin = _rand(0.0f, Math::TAU);
	for (int i = 0; i < stems; i++) {
		const float az = spin + i * SC_GOLDEN;
		const Transform3D base(sc_frame(sc_dir(az, _rand(0.35f, 0.85f)), Vector3(0, 1, 0)), Vector3(_rand(-0.02f, 0.02f), 0.0f, _rand(-0.02f, 0.02f)));
		const float len = _rand(0.22f, 0.4f);
		const LocalVector<Transform3D> fr = _stem_frames(base, len, _rand(-0.35f, -0.05f), 5);
		_add_stem_along(fr, 0.0055f, 0.002f, 0.0f);
		const Vector3 outward = Vector3(Math::cos(az), 0.0f, Math::sin(az));
		const int subs = int(_rand(6.0f, 9.99f));
		for (int k = 0; k < subs; k++) {
			const float u = _rand(0.15f, 0.95f);
			const Transform3D at = _frame_at(fr, u);
			const float a = _rand(0.0f, Math::TAU);
			const Vector3 d = (outward * 0.6f + Vector3(Math::cos(a) * 0.45f, 0.8f, Math::sin(a) * 0.45f)).normalized();
			const LocalVector<Transform3D> br = _stem_frames(Transform3D(sc_frame(d, Vector3(0, 1, 0)), at.origin), _rand(0.08f, 0.16f), _rand(-0.2f, 0.05f), 3);
			_add_stem_along(br, 0.0018f, 0.0011f, 0.3f);
			shoot(br, 0.3f, bloom != BLOOM_NONE);
		}
		LocalVector<Transform3D> top;
		for (uint32_t k = fr.size() / 2; k < fr.size(); k++) {
			top.push_back(fr[k]);
		}
		shoot(top, 0.25f, bloom != BLOOM_NONE);
	}
	plant_height = MAX(plant_height, 0.15f);
}

// The scrub sets (Blondel et al. 2010, ch. 6; Rivas-Martinez 2011 for the belts; Flora iberica).
// Mediterranean scrub is summer-dry and mild in winter: rosemary and the rockroses are cut back by
// January means much under 3 C, and the scrub gives way to Atlantic heath where the rain passes
// about 1,100 mm and the summer is not dry (aridity, rain over potential evaporation, from about
// 0.2 semi-arid to 0.75 dry sub-humid). Within it, soil sorts the two forms: garrigue, low and
// open with rock and bare ground between the bushes, on thin soil and limestone; maquis, closed and
// taller, where the soil is deeper. Shared by TreeGenerator's woody scrub species.
void PlantGenerator::apply_scrub_rules(const Ref<PlantSpecies> &p_species, ScrubStand p_stand) {
	ERR_FAIL_COND(p_species.is_null());
	const Ref<PlantSpecies> &sp = p_species;
	sp->set_rule_range(PlantSpecies::RULE_JANUARY_TEMPERATURE, Vector2(3.0f, 100.0f));
	sp->set_rule_fade(PlantSpecies::RULE_JANUARY_TEMPERATURE, 2.0f);
	sp->set_rule_range(PlantSpecies::RULE_PRECIPITATION, Vector2(250.0f, 1100.0f));
	sp->set_rule_fade(PlantSpecies::RULE_PRECIPITATION, 150.0f);
	sp->set_rule_range(PlantSpecies::RULE_ARIDITY, Vector2(0.2f, 0.75f));
	sp->set_rule_fade(PlantSpecies::RULE_ARIDITY, 0.12f);
	sp->set_rule_range(PlantSpecies::RULE_SHADE, Vector2(0.0f, 0.35f));
	sp->set_rule_fade(PlantSpecies::RULE_SHADE, 0.15f);
	sp->set_rule_range(PlantSpecies::RULE_WETNESS, Vector2(-1000.0f, 8.0f));
	sp->set_rule_fade(PlantSpecies::RULE_WETNESS, 1.0f);
	switch (p_stand) {
		case SCRUB_GARRIGUE:
			// Thin soil, rock between the bushes, up to steep stony slopes.
			sp->set_rule_range(PlantSpecies::RULE_SOIL_DEPTH, Vector2(0.0f, 0.45f));
			sp->set_rule_fade(PlantSpecies::RULE_SOIL_DEPTH, 0.15f);
			sp->set_rule_range(PlantSpecies::RULE_ROCK, Vector2(0.0f, 0.85f));
			sp->set_rule_fade(PlantSpecies::RULE_ROCK, 0.1f);
			sp->set_rule_range(PlantSpecies::RULE_SLOPE, Vector2(0.0f, 40.0f));
			sp->set_rule_fade(PlantSpecies::RULE_SLOPE, 5.0f);
			break;
		case SCRUB_MAQUIS:
			// Deeper soil, little bare rock.
			sp->set_rule_range(PlantSpecies::RULE_SOIL_DEPTH, Vector2(0.35f, 100000.0f));
			sp->set_rule_fade(PlantSpecies::RULE_SOIL_DEPTH, 0.15f);
			sp->set_rule_range(PlantSpecies::RULE_ROCK, Vector2(0.0f, 0.4f));
			sp->set_rule_fade(PlantSpecies::RULE_ROCK, 0.15f);
			sp->set_rule_range(PlantSpecies::RULE_SLOPE, Vector2(0.0f, 35.0f));
			sp->set_rule_fade(PlantSpecies::RULE_SLOPE, 5.0f);
			break;
		case SCRUB_MAX:
			break;
	}
	sp->set_meta("scrub", int(p_stand));
}

TypedArray<Resource> PlantGenerator::make_scrub(ScrubStand p_stand, int p_variants, bool p_textures, const Dictionary &p_season) {
	using P = PlantGenerator;
	struct ScrubSpec {
		Preset preset;
		float spacing, footprint, density, scale_min, scale_max;
		float patch_scale; // 0: no patches.
		float patch_cover;
		float visibility;
		float align;
	};
	LocalVector<ScrubSpec> set;
	switch (p_stand) {
		case SCRUB_GARRIGUE:
			// Open: scattered rosemary bushes, thyme cushions and rockrose between bare stony
			// ground, and a thin dry grass in the gaps.
			set = LocalVector<ScrubSpec>({ { P::PRESET_ROSEMARY, 1.2f, 0.4f, 0.55f, 0.7f, 1.3f, 9.0f, 0.5f, 45.0f, 0.3f },
					{ P::PRESET_THYME, 0.45f, 0.12f, 0.55f, 0.7f, 1.3f, 5.0f, 0.45f, 30.0f, 0.7f },
					{ P::PRESET_SAGE_ROCKROSE, 1.0f, 0.35f, 0.4f, 0.7f, 1.2f, 7.0f, 0.4f, 40.0f, 0.5f },
					{ P::PRESET_DRY_GRASS, 0.3f, 0.03f, 0.35f, 0.5f, 0.8f, 0.0f, 0.0f, 25.0f, 0.5f } });
			break;
		case SCRUB_MAQUIS:
			// Closed low scrub between the tall shrubs: rockrose and lavender under the gum
			// rockrose, kermes oak and lentisk TreeGenerator adds, a little rosemary.
			set = LocalVector<ScrubSpec>({ { P::PRESET_SAGE_ROCKROSE, 0.8f, 0.35f, 0.8f, 0.8f, 1.4f, 8.0f, 0.6f, 40.0f, 0.5f },
					{ P::PRESET_FRENCH_LAVENDER, 0.6f, 0.2f, 0.7f, 0.8f, 1.3f, 6.0f, 0.5f, 40.0f, 0.3f },
					{ P::PRESET_ROSEMARY, 1.3f, 0.4f, 0.3f, 0.8f, 1.4f, 9.0f, 0.35f, 45.0f, 0.3f },
					{ P::PRESET_DRY_GRASS, 0.35f, 0.03f, 0.3f, 0.5f, 0.8f, 0.0f, 0.0f, 25.0f, 0.5f } });
			break;
		case SCRUB_MAX:
			break;
	}
	TypedArray<Resource> out;
	int seed_base = 10700 + int(p_stand) * 100;
	for (const ScrubSpec &f : set) {
		Ref<PlantGenerator> gen;
		gen.instantiate();
		gen->set_textures(p_textures);
		gen->set_preset(f.preset);
		gen->apply_preset_colors();
		if (f.preset == P::PRESET_DRY_GRASS) {
			// Summer-dry grasses between the bushes: straw-coloured most of the year.
			gen->set_leaf_color(Color(0.42f, 0.38f, 0.22f));
		}
		gen->set_seed(seed_base);
		gen->set_season(p_season);
		Ref<PlantSpecies> sp;
		sp.instantiate();
		TypedArray<Mesh> meshes;
		const TypedArray<ArrayMesh> v = gen->generate_variants(MAX(p_variants, 1));
		for (int i = 0; i < v.size(); i++) {
			meshes.push_back(v[i]);
		}
		sp->set_meshes(meshes);
		sp->set_spacing(f.spacing);
		sp->set_footprint(f.footprint);
		sp->set_density(f.density * get_presence(f.preset, p_season));
		sp->set_scale_range(Vector2(f.scale_min, f.scale_max));
		sp->set_align_to_ground(f.align);
		apply_scrub_rules(sp, p_stand);
		if (f.preset == P::PRESET_THYME || f.preset == P::PRESET_FRENCH_LAVENDER) {
			// The sunniest ground first.
			sp->set_rule_range(PlantSpecies::RULE_SUNLIGHT, Vector2(150.0f, 2000.0f));
			sp->set_rule_fade(PlantSpecies::RULE_SUNLIGHT, 25.0f);
		}
		if (f.patch_scale > 0.0f) {
			sp->set_patch_scale(f.patch_scale);
			sp->set_patch_cover(f.patch_cover);
			sp->set_patch_seed(seed_base);
		}
		sp->set_visibility_range(f.visibility);
		tag_species(sp, f.preset);
		out.push_back(sp);
		seed_base += 19;
	}
	return out;
}

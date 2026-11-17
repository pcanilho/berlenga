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

// The frame every scrub shrub shares: a dense, rounded mound leafy to the ground. Woody stems
// leave the root crown in all directions over the dome, the low ones nearly flat and the inner
// ones upright, each as long as the dome's surface in its direction (an ellipsoid of radius p_r
// and height p_h), and all along them, from near the base, short leafy twigs, so the foliage
// closes over the wood; p_twig dresses each twig and each stem's top with leaves and flowers
// (its frames, its wind, and how high up the dome its tip is, 0 to 1). p_rise_min, the sine of
// the lowest stem's elevation.
template <typename F>
void PlantGenerator::_scrub_mound(float p_r, float p_h, int p_stems, float p_stem_r, float p_twig_len, float p_twig_step, float p_rise_min, F p_twig) {
	const float spin = _rand(0.0f, Math::TAU);
	for (int i = 0; i < p_stems; i++) {
		const float az = spin + i * SC_GOLDEN + _rand(-0.2f, 0.2f);
		// Directions spread evenly over the dome (sin of the elevation uniform) above p_rise_min:
		// erect shrubs keep few low stems.
		const float el = Math::asin(_rand(p_rise_min, 0.97f));
		const float ce = Math::cos(el) / p_r;
		const float se = Math::sin(el) / p_h;
		const float reach = 1.0f / Math::sqrt(ce * ce + se * se);
		// The twigs stand out past the stem's tip by about their length, so the stem stops short.
		const float len = MAX(reach - p_twig_len * 0.6f, reach * 0.4f) * _rand(0.8f, 1.0f);
		const Transform3D base(sc_frame(sc_dir(az, el), Vector3(0, 1, 0)), Vector3(_rand(-0.015f, 0.015f), 0.0f, _rand(-0.015f, 0.015f)));
		// Low stems arch up at the end, upright ones bow a little out.
		const LocalVector<Transform3D> fr = _stem_frames(base, len, el < 0.6f ? _rand(0.05f, 0.25f) : _rand(-0.2f, 0.05f), 5);
		_add_stem_along(fr, p_stem_r, p_stem_r * 0.35f, 0.0f);
		const Vector3 outward = Vector3(Math::cos(az), 0.0f, Math::sin(az));
		const int twigs = MAX(int(len / p_twig_step), 2);
		for (int k = 0; k < twigs; k++) {
			const float u = 0.1f + 0.85f * (k + _rand(0.0f, 0.8f)) / twigs;
			const Transform3D at = _frame_at(fr, u);
			const float a = (k & 1 ? 1.0f : -1.0f) * _rand(0.7f, 1.6f);
			const Vector3 side = at.basis.get_column(0) * Math::cos(a) + at.basis.get_column(1) * Math::sin(a);
			// Out from the stem, outward and a little up: the twigs fill the dome's shell.
			const Vector3 d = (at.basis.get_column(2) * 0.6f + side * 0.6f + outward * 0.35f + Vector3(0, 0.35f, 0)).normalized();
			const float tl = p_twig_len * _rand(0.6f, 1.2f) * (1.0f - 0.35f * u);
			const LocalVector<Transform3D> tw = _stem_frames(Transform3D(sc_frame(d, Vector3(0, 1, 0)), at.origin), tl, _rand(-0.15f, 0.15f), 2);
			_add_stem_along(tw, p_stem_r * 0.3f, p_stem_r * 0.18f, 0.4f);
			p_twig(tw, 0.4f, CLAMP(tw[tw.size() - 1].origin.y / p_h, 0.0f, 1.0f));
		}
		LocalVector<Transform3D> top;
		for (uint32_t k = fr.size() * 2 / 3; k < fr.size(); k++) {
			top.push_back(fr[k]);
		}
		p_twig(top, 0.3f, CLAMP(fr[fr.size() - 1].origin.y / p_h, 0.0f, 1.0f));
	}
}

// Rosemary (Salvia rosmarinus): an erect, dense evergreen shrub 0.5 to 1.5 m (garrigue plants
// browsed and wind-cut to 40 to 80 cm); leaves linear, 1.5 to 4 cm by 2 to 4 mm, leathery, the
// margins rolled under, dark green above and white-felted below, opposite and crowded towards the
// shoot tips; pale blue two-lipped flowers 1 to 1.2 cm in short axillary clusters near the tips,
// from late autumn to spring (Flora iberica 12; Upson and Andrews 2004 for the habit).
void PlantGenerator::_build_rosemary() {
	leaf_wear = 0.1f;
	Leaf lf;
	lf.shape = LEAF_LINEAR;
	lf.width = 0.0038f;
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
	// A short leafy shoot: leaves in crossed pairs crowded on it, spreading, shorter towards the tip.
	auto shoot = [&](const LocalVector<Transform3D> &p_fr, float p_wind, float p_up) {
		const float l = p_fr[0].origin.distance_to(p_fr[p_fr.size() - 1].origin);
		const int nodes = MAX(int(l / 0.0068f), 3);
		const bool flower = bloom != BLOOM_NONE && p_up > 0.35f && _rand(0.0f, 1.0f) < 0.75f;
		for (int k = 0; k < nodes; k++) {
			const float s = 0.05f + 0.95f * float(k) / MAX(nodes - 1, 1);
			const Transform3D at = _frame_at(p_fr, s);
			const float a0 = (k & 1) * Math::PI * 0.5f;
			const float size = Math::lerp(1.0f, 0.6f, s * s);
			for (int q = 0; q < 2; q++) {
				const Vector3 radial = sc_radial(at, a0 + q * Math::PI);
				lf.length = _rand(0.022f, 0.034f) * size;
				_add_leaf(SURFACE_LEAF, Transform3D(sc_frame((radial + at.basis.get_column(2) * Math::lerp(0.5f, 1.2f, s)).normalized(), at.basis.get_column(2)), at.origin), lf, Math::lerp(p_wind, 1.0f, s));
			}
			if (flower && s > 0.3f && s < 0.9f && _rand(0.0f, 1.0f) < 0.5f) {
				// A small axillary cluster: the hooded upper lip, the broad lower lip hanging.
				const Wither w(this, bloom == BLOOM_SEED ? 0.8f : dead * 0.6f);
				const int nf = int(_rand(1.0f, 2.99f));
				for (int f = 0; f < nf; f++) {
					const Vector3 radial = sc_radial(at, a0 + Math::PI * 0.5f + (f & 1) * Math::PI + _rand(-0.4f, 0.4f));
					const Vector3 fd = (radial + Vector3(0, 0.35f, 0)).normalized();
					const Vector3 mouth = at.origin + radial * 0.004f + fd * 0.006f;
					_add_head(SURFACE_PETAL, Transform3D(sc_frame(fd, Vector3(0, 1, 0)), at.origin + radial * 0.003f), 0.0018f, 0.007f, Color(0.9f, 0.9f, 0.95f), 2, 5); // The corolla tube.
					lip.length = _rand(0.006f, 0.0075f);
					lip.width = lip.length * 0.95f;
					_add_leaf(SURFACE_PETAL, Transform3D(sc_frame((fd - Vector3(0, 0.9f, 0)).normalized(), fd), mouth), lip, 1.0f);
					lip.length = _rand(0.005f, 0.006f);
					lip.width = lip.length * 0.8f;
					_add_leaf(SURFACE_PETAL, Transform3D(sc_frame((fd + Vector3(0, 0.8f, 0)).normalized(), fd), mouth), lip, 1.0f);
				}
			}
		}
	};
	const float h = _rand(0.4f, 0.75f);
	_scrub_mound(h * _rand(0.6f, 0.8f), h, int(_rand(18.0f, 24.0f)), 0.0045f, 0.065f, 0.02f, 0.45f, shoot);
	plant_height = MAX(plant_height, 0.3f);
}

// French lavender (Lavandula stoechas): a compact, much-branched shrub 30 to 100 cm, grey-felted;
// leaves linear to oblong, 1 to 4 cm by 2 to 5 mm, margins rolled, ascending and crowded, with
// leafy axillary tufts; the flowering shoots end in a short bare stalk (1 to 3 cm in subsp.
// stoechas) under a dense, four-angled spike 2 to 3 cm of dark purple flowers, crowned by three to
// five large purple sterile bracts 1 to 5 cm ("rabbit ears"), March to June. Acid, sandy and
// schist soils (Flora iberica 12; Upson and Andrews 2004, "The genus Lavandula").
void PlantGenerator::_build_french_lavender() {
	leaf_wear = 0.1f;
	Leaf lf;
	lf.shape = LEAF_LINEAR;
	lf.width = 0.0036f;
	lf.fold = 0.4f;
	lf.bend = 0.15f;
	lf.segments = 1;
	Leaf bract;
	bract.shape = LEAF_OVATE;
	bract.fold = 0.3f;
	bract.bend = -0.2f; // Flaring back at the tip.
	bract.twist = 0.3f;
	bract.segments = 4;
	auto spike = [&](const Transform3D &p_tip, Bloom p_bloom, float p_progress, float p_dead) {
		const Wither w(this, p_bloom == BLOOM_SEED ? 0.85f : p_dead * 0.7f);
		const Vector3 ax = p_tip.basis.get_column(2);
		const float grow = p_progress < 0.0f ? 1.0f : CLAMP(0.55f + p_progress * 1.5f, 0.55f, 1.0f);
		const float sl = _rand(0.02f, 0.03f) * grow;
		const float sr = sl * 0.24f;
		// Four-angled, densely packed: a dark core with the calyx rows standing out as knobs.
		_add_head(SURFACE_CENTRE, Transform3D(sc_frame(ax, Vector3(0, 1, 0)), p_tip.origin), sr, sl, Color(1, 1, 1), 5, 8);
		const int rows = 6;
		for (int k = 0; k < rows; k++) {
			const float f = 0.12f + 0.76f * k / float(rows - 1);
			const Transform3D at(p_tip.basis, p_tip.origin + ax * (sl * f));
			for (int q = 0; q < 4; q++) {
				const float a = q * Math::PI * 0.5f + (k & 1) * Math::PI * 0.25f;
				const Vector3 radial = sc_radial(at, a);
				const float bulge = Math::sin(Math::PI * f);
				_add_head(SURFACE_CENTRE, Transform3D(sc_frame((radial + ax * 0.4f).normalized(), ax), at.origin + radial * sr * 0.75f * bulge), sr * 0.42f, sr * 0.8f, Color(1, 1, 1), 2, 5);
			}
		}
		const int nb = int(_rand(3.0f, 5.99f));
		const Vector3 crown = p_tip.origin + ax * sl * 0.95f;
		for (int k = 0; k < nb; k++) {
			const float a = k * Math::TAU / nb + _rand(-0.25f, 0.25f);
			const Vector3 radial = sc_radial(p_tip, a);
			bract.length = _rand(0.014f, 0.024f) * grow;
			bract.width = bract.length * _rand(0.5f, 0.62f);
			_add_leaf(SURFACE_PETAL, Transform3D(sc_frame((ax * 1.6f + radial).normalized(), ax), crown + radial * sr * 0.3f), bract, 1.0f);
		}
	};
	// A leafy shoot: narrow leaves held up close along it (ascending, not spread like a frond), with
	// short leafy tufts in the lower axils; the upper shoots end in a flower stalk and spike.
	auto shoot = [&](const LocalVector<Transform3D> &p_fr, float p_wind, float p_up) {
		const float l = p_fr[0].origin.distance_to(p_fr[p_fr.size() - 1].origin);
		const int nodes = MAX(int(l / 0.0085f), 3);
		for (int k = 0; k < nodes; k++) {
			const float s = 0.05f + 0.95f * float(k) / MAX(nodes - 1, 1);
			const Transform3D at = _frame_at(p_fr, s);
			const float a0 = (k & 1) * Math::PI * 0.5f;
			for (int q = 0; q < 2; q++) {
				const Vector3 radial = sc_radial(at, a0 + q * Math::PI + _rand(-0.2f, 0.2f));
				lf.length = _rand(0.015f, 0.028f) * Math::lerp(1.0f, 0.65f, s);
				_add_leaf(SURFACE_LEAF, Transform3D(sc_frame((radial * 0.45f + at.basis.get_column(2)).normalized(), at.basis.get_column(2)), at.origin), lf, Math::lerp(p_wind, 1.0f, s));
				if (s < 0.6f && _rand(0.0f, 1.0f) < 0.25f) {
					for (int t = 0; t < 2; t++) {
						const Vector3 td = (radial * 0.35f + at.basis.get_column(2) + sc_radial(at, a0 + q * Math::PI + (t ? 0.5f : -0.5f)) * 0.2f).normalized();
						lf.length = _rand(0.008f, 0.013f);
						_add_leaf(SURFACE_LEAF, Transform3D(sc_frame(td, at.basis.get_column(2)), at.origin + radial * 0.002f), lf, Math::lerp(p_wind, 1.0f, s));
					}
				}
			}
		}
		if (p_up < 0.45f || _rand(0.0f, 1.0f) > 0.55f) {
			return;
		}
		float progress = 0.0f, dead = 0.0f;
		const Bloom bloom = _bloom(false, &progress, &dead);
		if (bloom == BLOOM_NONE) {
			return;
		}
		// The bare flower stalk rising clear of the leaves, then the spike.
		const Transform3D end = p_fr[p_fr.size() - 1];
		const Vector3 d = (end.basis.get_column(2) * 0.4f + Vector3(0, 1, 0)).normalized();
		const Transform3D tip = _add_stem(Transform3D(sc_frame(d, Vector3(0, 1, 0)), end.origin), _rand(0.03f, 0.06f), 0.0011f, 0.0009f, _rand(-0.1f, 0.1f), 3, 0.7f);
		spike(Transform3D(sc_frame(tip.basis.get_column(2), Vector3(0, 1, 0)), tip.origin), bloom, progress, dead);
	};
	const float h = _rand(0.2f, 0.32f);
	_scrub_mound(h * _rand(0.9f, 1.2f), h, int(_rand(18.0f, 24.0f)), 0.0035f, 0.06f, 0.017f, 0.2f, shoot);
	plant_height = MAX(plant_height, 0.2f);
}

// Thyme (Thymus vulgaris and kin, Thymus mastichina on acid ground): a dwarf, cushion-forming
// subshrub 10 to 30 cm of wiry, woody, much-branched stems; leaves 4 to 8 mm by 1.5 to 3 mm,
// grey-green, the margins rolled, in opposite pairs with leafy tufts in the axils; tiny pale pink
// to mauve flowers 4 to 6 mm in small rounded or whorled heads at the shoot tips, April to June
// (Flora iberica 12; Morales 1986).
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
	auto shoot = [&](const LocalVector<Transform3D> &p_fr, float p_wind, float p_up) {
		const float l = p_fr[0].origin.distance_to(p_fr[p_fr.size() - 1].origin);
		const int nodes = MAX(int(l / 0.004f), 3);
		for (int k = 0; k < nodes; k++) {
			const float s = 0.1f + 0.9f * float(k) / MAX(nodes - 1, 1);
			const Transform3D at = _frame_at(p_fr, s);
			const float a0 = (k & 1) * Math::PI * 0.5f;
			for (int q = 0; q < 2; q++) {
				const Vector3 radial = sc_radial(at, a0 + q * Math::PI);
				lf.length = _rand(0.0045f, 0.0065f);
				_add_leaf(SURFACE_LEAF, Transform3D(sc_frame((radial + at.basis.get_column(2) * 0.7f).normalized(), at.basis.get_column(2)), at.origin), lf, Math::lerp(p_wind, 1.0f, s));
				if (_rand(0.0f, 1.0f) < 0.35f) {
					lf.length = _rand(0.003f, 0.0045f);
					_add_leaf(SURFACE_LEAF, Transform3D(sc_frame((radial * 0.5f + at.basis.get_column(2)).normalized(), at.basis.get_column(2)), at.origin + radial * 0.0012f), lf, Math::lerp(p_wind, 1.0f, s));
				}
			}
		}
		if (p_up < 0.4f || _rand(0.0f, 1.0f) > 0.6f) {
			return;
		}
		float progress = 0.0f, dead = 0.0f;
		const Bloom bloom = _bloom(false, &progress, &dead);
		if (bloom == BLOOM_NONE) {
			return;
		}
		// A short stalk just clear of the leaves, and the small head: whorls of calyces with the
		// tiny two-lipped flowers open round them, the head about a centimetre across.
		const Wither w(this, bloom == BLOOM_SEED ? 0.8f : dead * 0.6f);
		const Transform3D end = p_fr[p_fr.size() - 1];
		const Transform3D tip = _add_stem(Transform3D(sc_frame((end.basis.get_column(2) * 0.3f + Vector3(0, 1, 0)).normalized(), Vector3(0, 1, 0)), end.origin), _rand(0.004f, 0.012f), 0.0006f, 0.0005f, 0.0f, 2, 0.8f);
		const Vector3 ax = tip.basis.get_column(2);
		const float hr = _rand(0.0022f, 0.0028f);
		_add_head(SURFACE_CENTRE, Transform3D(sc_frame(ax, Vector3(0, 1, 0)), tip.origin), hr, hr * 1.6f, Color(1, 1, 1), 3, 6);
		if (bloom == BLOOM_SEED) {
			return;
		}
		// The flowers stand out all over the head, not in one ring: each a short tube 2 to 3 mm
		// with its small spreading lower lip, so the head reads as a little pink ball.
		const int nf = int(_rand(10.0f, 15.0f));
		const Vector3 centre = tip.origin + ax * hr * 0.8f;
		for (int f = 0; f < nf; f++) {
			const float a = f * SC_GOLDEN;
			const float el = Math::asin(_rand(-0.3f, 0.95f));
			const Vector3 rd = sc_radial(tip, a);
			const Vector3 d = (rd * Math::cos(el) + ax * Math::sin(el)).normalized();
			const Vector3 at = centre + d * hr * 0.8f;
			const float tl = _rand(0.002f, 0.0026f);
			_add_head(SURFACE_PETAL, Transform3D(sc_frame(d, ax), at), 0.001f, tl, Color(0.95f, 0.95f, 0.95f), 2, 5);
			petal.length = _rand(0.002f, 0.0026f);
			petal.width = petal.length * 1.2f;
			_add_leaf(SURFACE_PETAL, Transform3D(sc_frame((d - ax * 0.4f).normalized(), d), at + d * tl), petal, 1.0f);
		}
	};
	// A low, tight cushion: about as wide as it is tall, twice at most, the shoots short and packed.
	const float h = _rand(0.06f, 0.11f);
	_scrub_mound(h * _rand(1.1f, 1.5f), h, int(_rand(26.0f, 34.0f)), 0.0018f, 0.02f, 0.0065f, 0.35f, shoot);
	plant_height = MAX(plant_height, 0.05f);
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
	lf.segments = 4;
	Leaf petal;
	petal.shape = LEAF_ROUND;
	petal.fold = 0.15f;
	petal.bend = -0.15f;
	petal.segments = 5;
	// Summer dormancy (July and August, into September): smaller, folded, greyer leaves, fewer of them.
	const int day = bloom_day;
	const float drought = (day >= 180 && day <= 270) ? Math::sin(Math::PI * float(day - 180) / 90.0f) : 0.0f;
	// The drought's grey-white cast: the folded leaves show their felted undersides (Harley 1985).
	// From the fixed preset colour, so a generator reused across the year never drifts.
	leaf_color = Color(0.25f, 0.3f, 0.18f).lerp(Color(0.56f, 0.57f, 0.52f), drought * 0.9f);
	const Color summer = Color(1, 1, 1);
	lf.fold = Math::lerp(0.3f, 0.65f, drought);
	float progress = 0.0f, dead = 0.0f;
	const Bloom bloom = _bloom(false, &progress, &dead);
	Leaf sepal = lf;
	sepal.length = 0.009f;
	sepal.width = 0.0065f;
	sepal.fold = 0.4f;
	sepal.bend = -0.2f;
	sepal.segments = 3;
	sepal.petiole = 0.0f;
	auto shoot = [&](const LocalVector<Transform3D> &p_fr, float p_wind, float p_up) {
		const float l = p_fr[0].origin.distance_to(p_fr[p_fr.size() - 1].origin);
		const int nodes = MAX(int(l / 0.012f), 2);
		for (int k = 0; k < nodes; k++) {
			const float s = 0.15f + 0.85f * float(k) / MAX(nodes - 1, 1);
			const Transform3D at = _frame_at(p_fr, s);
			const float a0 = (k & 1) * Math::PI * 0.5f;
			for (int q = 0; q < 2; q++) {
				if (drought > 0.0f && _rand(0.0f, 1.0f) < drought * 0.35f * (1.0f - s)) {
					continue; // The older leaves shed in the drought.
				}
				const Vector3 radial = sc_radial(at, a0 + q * Math::PI);
				lf.length = _rand(0.018f, 0.03f) * Math::lerp(1.0f, 0.75f, drought) * Math::lerp(1.0f, 0.75f, s * s);
				lf.width = lf.length * _rand(0.5f, 0.62f);
				const Vector3 d = (radial + at.basis.get_column(2) * 0.35f + Vector3(0, 0.25f, 0)).normalized();
				_add_leaf(SURFACE_LEAF, Transform3D(sc_frame(d, Vector3(0, 1, 0)), at.origin), lf, Math::lerp(p_wind, 1.0f, s), 1.0f, summer);
			}
		}
		if (bloom == BLOOM_NONE || p_up < 0.35f || _rand(0.0f, 1.0f) > 0.4f) {
			return;
		}
		// A flower or capsule on its long, slender stalk from near the shoot's tip, held out above
		// the leaves.
		const Transform3D at = _frame_at(p_fr, _rand(0.7f, 1.0f));
		const Vector3 radial = sc_radial(at, _rand(0.0f, Math::TAU));
		const Vector3 pd = (radial * 0.5f + Vector3(0, 1.0f, 0)).normalized();
		const Transform3D top = _add_stem(Transform3D(sc_frame(pd, Vector3(0, 1, 0)), at.origin), _rand(0.025f, 0.05f), 0.0008f, 0.0006f, 0.25f, 3, 0.6f);
		const Basis fb = sc_frame((top.basis.get_column(2) + Vector3(0, 1.2f, 0)).normalized(), radial);
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
		} else {
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
	};
	const float h = _rand(0.28f, 0.42f);
	_scrub_mound(h * _rand(1.0f, 1.3f), h, int(_rand(17.0f, 22.0f)), 0.0045f, 0.075f, 0.02f, 0.4f, shoot);
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

/**************************************************************************/
/*  plant_alpine.cpp                                                      */
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

// Alpine and scree plants (docs/ideas/vegetation.md, "Alpine and scree plants"): the low flora
// above the treeline, sorted by snow and wind along the micro-relief. Measurements from Lauber,
// Wagner and Gygax, "Flora Helvetica" (2018), Stace (2019) and the Biological Flora accounts
// cited in the note.

#include "plant_generator.h"
#include "plant_species.h"

namespace {
const float ALP_GOLDEN = 2.39996323f;

Basis alp_frame(const Vector3 &p_forward, const Vector3 &p_up_hint) {
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

Vector3 alp_dir(float p_azimuth, float p_elevation) {
	return Vector3(Math::cos(p_azimuth) * Math::cos(p_elevation), Math::sin(p_elevation), Math::sin(p_azimuth) * Math::cos(p_elevation));
}
} // namespace

// Moss campion (Silene acaulis): a dense hemispherical cushion 5 to 30 cm across, built of
// thousands of short shoots packed side by side, each ending in a tiny rosette of stiff linear
// leaves 6 to 12 mm; old leaves stay dead and brown inside, so the cushion is solid. Pink
// five-petalled flowers 9 to 12 mm, almost stalkless, dot the surface in June to August, often on
// the sunny side first.
void PlantGenerator::_build_moss_campion() {
	leaf_wear = 0.1f;
	const float r = _rand(0.04f, 0.1f);
	const float h = r * _rand(0.45f, 0.6f);
	// The dead, packed core: a closed dome, so the cushion never reads hollow between rosettes.
	_add_head(SURFACE_STEM, Transform3D(alp_frame(Vector3(0, 1, 0), Vector3(1, 0, 0)), Vector3(0, -0.004f, 0)), r * 0.97f, h * 0.95f, Color(0.55f, 0.5f, 0.4f), 5, 14);
	auto surface = [&](float p_u, float p_a, Vector3 &r_n) -> Vector3 {
		// A dome of radius r and height h: x = r u, y = h sqrt(1 - u^2), its normal.
		const float u = MIN(p_u, 0.999f);
		const float y = h * Math::sqrt(1.0f - u * u);
		const Vector3 radial(Math::cos(p_a), 0.0f, Math::sin(p_a));
		r_n = (radial * (u / r) + Vector3(0, Math::sqrt(1.0f - u * u) / h, 0)).normalized();
		return radial * (r * u) + Vector3(0, y, 0);
	};
	// Rosettes about 8 mm apart over the dome (Vogel's spiral on the disc, lifted onto it).
	const int n = CLAMP(int(Math::PI * r * r * 1.3f / (0.008f * 0.008f)), 30, 420);
	Leaf l;
	l.shape = LEAF_LINEAR;
	l.fold = 0.3f;
	l.bend = 0.15f;
	l.segments = 2;
	const float spin = _rand(0.0f, Math::TAU);
	for (int i = 0; i < n; i++) {
		const float u = Math::sqrt((i + 0.5f) / n);
		const float a = spin + i * ALP_GOLDEN;
		Vector3 nrm;
		const Vector3 at = surface(u, a, nrm);
		const Basis nb = alp_frame(nrm, Vector3(1, 0, 0));
		const int leaves = 5;
		const float lspin = _rand(0.0f, Math::TAU);
		for (int k = 0; k < leaves; k++) {
			const float b = lspin + k * ALP_GOLDEN;
			const Vector3 radial = nb.get_column(0) * Math::cos(b) + nb.get_column(1) * Math::sin(b);
			const Vector3 d = (radial * 0.75f + nrm * 0.7f).normalized();
			l.length = _rand(0.006f, 0.01f);
			l.width = 0.0012f;
			_add_leaf(SURFACE_LEAF, Transform3D(alp_frame(d, nrm), at - nrm * 0.001f), l, 0.0f, 0.75f);
		}
	}
	plant_height = MAX(plant_height, h);
	// Flowers sessile on the surface, more on the sunny (here +Z) side.
	float progress = 0.0f, dead = 0.0f;
	const Bloom bloom = _bloom(false, &progress, &dead);
	if (bloom == BLOOM_NONE) {
		return;
	}
	const Wither wither(this, dead);
	Leaf p;
	p.shape = LEAF_SPATULATE;
	p.fold = 0.05f;
	p.bend = -0.25f;
	p.segments = 3;
	const int flowers = int(n * _rand(0.12f, 0.25f) * (bloom == BLOOM_SEED ? 0.5f : 1.0f));
	for (int i = 0; i < flowers; i++) {
		const float a = _rand(0.0f, Math::TAU);
		if (Math::sin(a) < -0.4f && _rand(0.0f, 1.0f) < 0.6f) {
			continue;
		}
		Vector3 nrm;
		const Vector3 at = surface(Math::sqrt(_rand(0.0f, 0.92f)), a, nrm) + nrm * 0.004f;
		const Basis fb = alp_frame(nrm, Vector3(1, 0, 0));
		if (bloom == BLOOM_SEED) {
			_add_head(SURFACE_STEM, Transform3D(fb, at), 0.0018f, 0.004f, Color(0.8f, 0.7f, 0.5f), 2, 6);
			continue;
		}
		const float sz = _rand(0.0045f, 0.006f);
		p.length = sz;
		p.width = sz * 0.7f;
		const float fs = _rand(0.0f, Math::TAU);
		for (int k = 0; k < 5; k++) {
			const float pa = fs + k * Math::TAU / 5.0f;
			const Vector3 d = (fb.get_column(0) * Math::cos(pa) + fb.get_column(1) * Math::sin(pa) + nrm * 0.15f).normalized();
			_add_leaf(SURFACE_PETAL, Transform3D(alp_frame(d, nrm), at), p, 1.0f);
		}
		_add_head(SURFACE_CENTRE, Transform3D(fb, at), 0.0012f, 0.0015f, Color(1, 1, 1), 2, 5);
	}
}

// Purple saxifrage (Saxifraga oppositifolia): a loose mat of trailing stems 5 to 20 cm, densely
// set with tiny opposite leaves 2 to 5 mm in four ranks, blue-green with lime-pitted tips; purple
// cups 10 to 20 mm at the shoot ends, the first flowers after the snow melts.
void PlantGenerator::_build_purple_saxifrage() {
	leaf_wear = 0.1f;
	const int shoots = int(_rand(26.0f, 42.0f));
	const float spin = _rand(0.0f, Math::TAU);
	Leaf l;
	l.shape = LEAF_OVATE;
	l.fold = 0.35f;
	l.bend = -0.2f;
	l.segments = 2;
	Leaf p;
	p.shape = LEAF_ROUND;
	p.fold = -0.25f;
	p.bend = -0.3f;
	p.segments = 3;
	float progress = 0.0f, dead = 0.0f;
	const Bloom bloom = _bloom(false, &progress, &dead);
	for (int s = 0; s < shoots; s++) {
		const float az = spin + s * ALP_GOLDEN;
		const float len = _rand(0.03f, 0.075f);
		// Prostrate: out along the ground, the tip rising a little.
		const Transform3D base(alp_frame(alp_dir(az, _rand(0.0f, 0.15f)), Vector3(0, 1, 0)), Vector3(_rand(-0.03f, 0.03f), 0.003f, _rand(-0.03f, 0.03f)));
		const LocalVector<Transform3D> frames = _stem_frames(base, len, _rand(-0.3f, -0.05f), 6);
		_add_stem_along(frames, 0.0008f, 0.0006f, 0.0f);
		// Leaves overlap like tiles and hide the stem.
		const int pairs = int(len / 0.0022f);
		for (int k = 0; k < pairs; k++) {
			const float t = (k + 0.5f) / pairs;
			const Transform3D at = _frame_at(frames, t);
			const float rot = (k % 2) * Math::PI * 0.5f; // Decussate: each pair at right angles to the last.
			for (int side = 0; side < 2; side++) {
				const float a = rot + side * Math::PI;
				const Vector3 radial = at.basis.get_column(0) * Math::cos(a) + at.basis.get_column(1) * Math::sin(a);
				const Vector3 d = (radial + at.basis.get_column(2) * 1.1f).normalized();
				l.length = _rand(0.0035f, 0.0052f) * (1.0f - 0.25f * t);
				l.width = l.length * 0.6f;
				_add_leaf(SURFACE_LEAF, Transform3D(alp_frame(d, radial), at.origin), l, 0.2f);
			}
		}
		if (bloom == BLOOM_NONE || _rand(0.0f, 1.0f) < 0.15f) {
			continue;
		}
		const Wither wither(this, dead);
		const Transform3D tip = _frame_at(frames, 1.0f);
		const Vector3 face = (tip.basis.get_column(2) * 0.4f + Vector3(0, 1, 0)).normalized();
		const Basis fb = alp_frame(face, Vector3(1, 0, 0));
		if (bloom == BLOOM_SEED) {
			_add_head(SURFACE_STEM, Transform3D(fb, tip.origin), 0.002f, 0.004f, Color(0.7f, 0.55f, 0.35f), 2, 6);
			continue;
		}
		const float sz = _rand(0.005f, 0.008f) * (0.7f + 0.3f * MAX(progress, 0.0f));
		p.length = sz;
		p.width = sz * 0.8f;
		const float fs = _rand(0.0f, Math::TAU);
		for (int k = 0; k < 5; k++) {
			const float pa = fs + k * Math::TAU / 5.0f;
			const Vector3 d = (fb.get_column(0) * Math::cos(pa) + fb.get_column(1) * Math::sin(pa) + face * 0.7f).normalized();
			_add_leaf(SURFACE_PETAL, Transform3D(alp_frame(d, face), tip.origin + face * 0.001f), p, 1.0f);
		}
		_add_head(SURFACE_CENTRE, Transform3D(fb, tip.origin + face * 0.002f), 0.0014f, 0.002f, Color(1, 1, 1), 2, 6);
	}
	plant_height = MAX(plant_height, 0.02f);
}

// Mountain avens (Dryas octopetala): a creeping woody mat; leaves 1 to 3 cm, oblong with deep round
// scallops, dark glossy green above, white-felted below, on short stalks; solitary flowers 2.5 to
// 4 cm on stalks 3 to 10 cm, eight (seven to ten) white petals round a boss of yellow stamens; then
// the styles lengthen into a twisted plume of silky feathers 2 to 3 cm.
void PlantGenerator::_build_mountain_avens() {
	leaf_wear = 0.2f;
	const int shoots = int(_rand(12.0f, 22.0f));
	const float spin = _rand(0.0f, Math::TAU);
	Leaf l;
	l.shape = LEAF_LANCEOLATE;
	l.lobes = 5;
	l.lobe_depth = 0.35f;
	l.round_lobes = true;
	l.petiole = 0.25f;
	l.fold = 0.25f;
	l.bend = 0.35f; // The margins roll under.
	l.segments = 5;
	for (int s = 0; s < shoots; s++) {
		const float az = spin + s * ALP_GOLDEN;
		const float len = _rand(0.04f, 0.09f);
		const Transform3D base(alp_frame(alp_dir(az, _rand(0.0f, 0.12f)), Vector3(0, 1, 0)), Vector3(0, 0.003f, 0));
		const LocalVector<Transform3D> frames = _stem_frames(base, len, -0.15f, 6);
		_add_stem_along(frames, 0.0014f, 0.001f, 0.0f);
		// The prostrate woody shoots fork, so the mat closes instead of reading as spokes.
		LocalVector<LocalVector<Transform3D>> tips;
		tips.push_back(LocalVector<Transform3D>(frames));
		const int forks = int(_rand(1.0f, 2.99f));
		for (int f = 0; f < forks; f++) {
			const Transform3D at = _frame_at(frames, _rand(0.3f, 0.6f));
			const float side = (f & 1) ? 1.0f : -1.0f;
			const Vector3 d = (at.basis.get_column(2) + at.basis.get_column(0) * side * _rand(0.6f, 1.1f)).normalized();
			const Vector3 flat = Vector3(d.x, MAX(d.y, 0.0f) * 0.3f, d.z).normalized();
			const LocalVector<Transform3D> fork = _stem_frames(Transform3D(alp_frame(flat, Vector3(0, 1, 0)), at.origin), len * _rand(0.4f, 0.7f), -0.1f, 4);
			_add_stem_along(fork, 0.001f, 0.0008f, 0.0f);
			tips.push_back(LocalVector<Transform3D>(fork));
		}
		for (const LocalVector<Transform3D> &tip : tips) {
			// The leaves crowd each shoot's end in a loose rosette, the old ones dead and brown below them.
			const int leaves = int(_rand(6.0f, 10.0f));
			const int dead_leaves = int(_rand(2.0f, 5.0f));
			for (int k = 0; k < leaves + dead_leaves; k++) {
				const bool old = k < dead_leaves;
				const float t = old ? Math::lerp(0.25f, 0.55f, (k + 0.5f) / dead_leaves) : Math::lerp(0.55f, 1.0f, (k - dead_leaves + 0.5f) / leaves);
				const Transform3D at = _frame_at(tip, t);
				const float a = k * ALP_GOLDEN;
				const Vector3 radial = (at.basis.get_column(0) * Math::sin(a) + at.basis.get_column(2) * 0.4f).normalized();
				const Vector3 d = (radial + Vector3(0, old ? _rand(-0.1f, 0.15f) : _rand(0.25f, 0.6f), 0)).normalized();
				l.length = _rand(0.015f, 0.028f) * (old ? 0.8f : 1.0f);
				l.width = l.length * 0.38f;
				l.twist = _rand(-0.3f, 0.3f);
				const Wither wither(this, old ? 1.0f : 0.0f);
				_add_leaf(SURFACE_LEAF, Transform3D(alp_frame(d, Vector3(0, 1, 0)), at.origin), l, 0.3f);
			}
		}
	}
	plant_height = MAX(plant_height, 0.03f);
	const int stems = int(_rand(1.0f, 4.99f));
	Leaf petal;
	petal.shape = LEAF_OVATE;
	petal.fold = -0.1f;
	petal.bend = -0.15f;
	petal.segments = 3;
	Leaf feather;
	feather.shape = LEAF_LINEAR;
	feather.width = 0.0012f;
	feather.fold = 0.0f;
	feather.segments = 4;
	for (int s = 0; s < stems; s++) {
		float progress = 0.0f, dead = 0.0f;
		const Bloom bloom = _bloom(false, &progress, &dead);
		if (bloom == BLOOM_NONE) {
			continue;
		}
		const Wither wither(this, dead * 0.4f); // The plumes stay silvery.
		const float az = spin + s * 2.1f;
		const Transform3D top = _add_stem(Transform3D(alp_frame(alp_dir(az, _rand(1.3f, 1.5f)), Vector3(0, 1, 0)), Vector3(Math::cos(az), 0.0f, Math::sin(az)) * _rand(0.01f, 0.04f)), _rand(0.04f, 0.09f), 0.0009f, 0.0007f, _rand(0.0f, 0.3f), 5, 0.0f);
		const Vector3 face = (top.basis.get_column(2) * 0.3f + Vector3(0, 1, 0)).normalized();
		const Basis fb = alp_frame(face, Vector3(1, 0, 0));
		if (bloom == BLOOM_SEED) {
			// The plume: twisted feathery styles from the head, spread and silky.
			const int n = int(_rand(14.0f, 22.0f));
			for (int k = 0; k < n; k++) {
				const float a = k * ALP_GOLDEN;
				const Vector3 radial = fb.get_column(0) * Math::cos(a) + fb.get_column(1) * Math::sin(a);
				const Vector3 d = (face * 1.2f + radial * (0.3f + 0.5f * progress)).normalized();
				feather.length = _rand(0.016f, 0.026f);
				feather.bend = _rand(0.5f, 1.2f);
				feather.twist = 2.5f;
				_add_leaf(SURFACE_PETAL, Transform3D(alp_frame(d, radial), top.origin), feather, 1.0f);
			}
			continue;
		}
		const int petals = 8;
		const float sz = _rand(0.011f, 0.017f);
		petal.length = sz;
		petal.width = sz * 0.6f;
		const float fs = _rand(0.0f, Math::TAU);
		for (int k = 0; k < petals; k++) {
			const float pa = fs + k * Math::TAU / petals;
			const Vector3 d = (fb.get_column(0) * Math::cos(pa) + fb.get_column(1) * Math::sin(pa) + face * 0.25f).normalized();
			_add_leaf(SURFACE_PETAL, Transform3D(alp_frame(d, face), top.origin), petal, 1.0f);
		}
		_add_head(SURFACE_CENTRE, Transform3D(fb, top.origin), 0.004f, 0.003f, Color(1, 1, 1), 3, 10);
	}
}

// Curved sedge (Carex curvula): dense tufts of thin, channelled leaves 5 to 15 cm, curved to one side
// like a sickle; the dead leaf tips curl and turn brown early (a fungus, Pleospora), so the alpine
// sward reads brownish green from afar. Small brown spikes in July.
void PlantGenerator::_build_alpine_sedge() {
	const int blades = int(_rand(40.0f, 65.0f));
	const float lean = _rand(0.0f, Math::TAU); // The tuft's curve, mostly one way (the wind).
	Leaf l;
	l.shape = LEAF_LINEAR;
	l.fold = 0.75f;
	l.segments = 6;
	for (int i = 0; i < blades; i++) {
		const float az = lean + _rand(-1.2f, 1.2f);
		const Vector3 at(_rand(-0.012f, 0.012f), 0.0f, _rand(-0.012f, 0.012f));
		l.length = _rand(0.05f, 0.13f);
		l.width = _rand(0.0012f, 0.002f);
		l.bend = _rand(0.8f, 1.8f);
		l.twist = _rand(-0.5f, 0.5f);
		const bool tip_dead = _rand(0.0f, 1.0f) < 0.5f;
		const Color tint = tip_dead ? Color(0.95f, 0.85f, 0.65f) : Color(1, 1, 1); // Browned tips (the fungus) tint the blade.
		_add_leaf(SURFACE_LEAF, Transform3D(alp_frame(alp_dir(az, _rand(1.05f, 1.45f)), Vector3(0, 1, 0)), at), l, 0.0f, 0.85f, tint);
	}
	const int stems = int(_rand(0.0f, 3.99f));
	for (int s = 0; s < stems; s++) {
		float dead = 0.0f;
		const Bloom bloom = _bloom(false, nullptr, &dead);
		if (bloom == BLOOM_NONE) {
			continue;
		}
		const Wither wither(this, dead);
		const Transform3D top = _add_stem(Transform3D(alp_frame(alp_dir(_rand(0.0f, Math::TAU), _rand(1.4f, 1.55f)), Vector3(0, 1, 0)), Vector3()), _rand(0.04f, 0.1f), 0.0006f, 0.0005f, 0.2f, 4, 0.0f);
		_add_head(SURFACE_CENTRE, top, 0.002f, 0.009f, Color(1, 1, 1), 3, 6);
	}
}

// Dwarf willow (Salix herbacea): the smallest tree, its stems running under the soil and moss;
// above it only short shoots 1 to 5 cm, each with two to four round, finely scalloped, glossy leaves
// 0.5 to 2 cm, net-veined; tiny red-tinged catkins in June and July. Carpets snowbeds.
void PlantGenerator::_build_dwarf_willow() {
	leaf_wear = 0.15f;
	const int shoots = int(_rand(18.0f, 34.0f));
	Leaf l;
	l.shape = LEAF_ROUND;
	l.petiole = 0.2f;
	l.fold = -0.05f;
	l.bend = 0.1f;
	l.lobes = 6;
	l.lobe_depth = 0.04f; // The fine scallops.
	l.round_lobes = true;
	l.segments = 4;
	for (int s = 0; s < shoots; s++) {
		const float a = _rand(0.0f, Math::TAU);
		const Vector3 at = Vector3(Math::cos(a), 0.0f, Math::sin(a)) * Math::sqrt(_rand(0.0f, 1.0f)) * 0.08f;
		const Transform3D top = _add_stem(Transform3D(alp_frame(alp_dir(_rand(0.0f, Math::TAU), _rand(0.6f, 1.2f)), Vector3(0, 1, 0)), at), _rand(0.004f, 0.012f), 0.0012f, 0.0009f, 0.2f, 3, 0.0f);
		const int leaves = int(_rand(2.0f, 4.99f));
		const float spin = _rand(0.0f, Math::TAU);
		for (int k = 0; k < leaves; k++) {
			const float b = spin + k * ALP_GOLDEN;
			const Vector3 d = (alp_dir(b, 0.0f) + Vector3(0, _rand(0.05f, 0.3f), 0)).normalized();
			l.length = _rand(0.009f, 0.018f);
			l.width = l.length * 0.95f;
			_add_leaf(SURFACE_LEAF, Transform3D(alp_frame(d, Vector3(0, 1, 0)), top.origin), l, 0.3f);
		}
		float dead = 0.0f;
		const Bloom bloom = _bloom(false, nullptr, &dead);
		if (bloom != BLOOM_NONE && _rand(0.0f, 1.0f) < 0.35f) {
			const Wither wither(this, dead);
			_add_head(SURFACE_CENTRE, Transform3D(top.basis, top.origin + top.basis.get_column(2) * 0.002f), 0.0018f, 0.006f, Color(1, 1, 1), 3, 6);
		}
	}
	plant_height = MAX(plant_height, 0.02f);
}

// Alpine toadflax (Linaria alpina): a scree wanderer; trailing glaucous stems 5 to 15 cm creep over
// and through the moving stones, set with whorls of three or four fleshy linear leaves 5 to 12 mm;
// short spikes of violet snapdragon flowers 13 to 20 mm with a long spur and an orange palate,
// June to September.
void PlantGenerator::_build_alpine_toadflax() {
	leaf_wear = 0.05f;
	const int shoots = int(_rand(4.0f, 9.0f));
	const float spin = _rand(0.0f, Math::TAU);
	Leaf l;
	l.shape = LEAF_LINEAR;
	l.fold = 0.5f; // Thick, rounded beneath.
	l.bend = 0.1f;
	l.segments = 3;
	Leaf lip;
	lip.shape = LEAF_ROUND;
	lip.fold = -0.3f;
	lip.bend = -0.4f;
	lip.segments = 3;
	for (int s = 0; s < shoots; s++) {
		const float az = spin + s * ALP_GOLDEN;
		const float len = _rand(0.05f, 0.12f);
		const Transform3D base(alp_frame(alp_dir(az, _rand(0.15f, 0.45f)), Vector3(0, 1, 0)), Vector3(0, 0.002f, 0));
		const LocalVector<Transform3D> frames = _stem_frames(base, len, _rand(-0.6f, -0.2f), 6);
		_add_stem_along(frames, 0.001f, 0.0008f, 0.2f);
		const int whorls = int(len / 0.006f);
		for (int k = 0; k < whorls; k++) {
			const float t = (k + 0.5f) / whorls * 0.85f;
			const Transform3D at = _frame_at(frames, t);
			const int n = 4;
			for (int j = 0; j < n; j++) {
				const float a = k * 0.6f + j * Math::TAU / n;
				const Vector3 radial = at.basis.get_column(0) * Math::cos(a) + at.basis.get_column(1) * Math::sin(a);
				const Vector3 d = (radial + at.basis.get_column(2) * 0.9f).normalized();
				l.length = _rand(0.007f, 0.012f);
				l.width = 0.0022f;
				_add_leaf(SURFACE_LEAF, Transform3D(alp_frame(d, radial), at.origin), l, 0.4f);
			}
		}
		float dead = 0.0f;
		const Bloom bloom = _bloom(false, nullptr, &dead);
		if (bloom == BLOOM_NONE || _rand(0.0f, 1.0f) < 0.25f) {
			continue;
		}
		const Wither wither(this, dead);
		const int flowers = int(_rand(2.0f, 5.99f));
		for (int k = 0; k < flowers; k++) {
			const Transform3D at = _frame_at(frames, Math::lerp(0.8f, 1.0f, (k + 0.5f) / flowers));
			const float a = k * ALP_GOLDEN;
			const Vector3 radial = at.basis.get_column(0) * Math::cos(a) + at.basis.get_column(1) * Math::sin(a);
			const Vector3 face = (radial + Vector3(0, 0.4f, 0)).normalized();
			const Basis fb = alp_frame(face, Vector3(0, 1, 0));
			if (bloom == BLOOM_SEED) {
				_add_head(SURFACE_STEM, Transform3D(fb, at.origin + face * 0.003f), 0.0025f, 0.004f, Color(0.7f, 0.6f, 0.4f), 2, 6);
				continue;
			}
			// The tube and spur back from the mouth, then the two lips, the lower with its orange palate.
			const Vector3 mouth = at.origin + face * 0.01f;
			_add_head(SURFACE_PETAL, Transform3D(alp_frame(-face, Vector3(0, 1, 0)), mouth), 0.0028f, 0.014f, Color(1, 1, 1), 3, 7);
			lip.length = 0.006f;
			lip.width = 0.007f;
			_add_leaf(SURFACE_PETAL, Transform3D(alp_frame((face + fb.get_column(1) * 0.8f).normalized(), face), mouth), lip, 1.0f);
			_add_leaf(SURFACE_PETAL, Transform3D(alp_frame((face - fb.get_column(1) * 0.6f).normalized(), face), mouth), lip, 1.0f);
			_add_head(SURFACE_CENTRE, Transform3D(fb, mouth - fb.get_column(1) * 0.0015f), 0.0022f, 0.0018f, Color(1, 1, 1), 2, 6);
		}
	}
	plant_height = MAX(plant_height, 0.025f);
}

// The alpine sets (docs/ideas/vegetation.md, "Alpine and scree plants"): above the treeline by the
// climate map's mean annual temperature, sorted by curvature (ridges blown free of snow, hollows
// where it lies late), the sun and the scree.
static constexpr float ALP_TREELINE_SEASON = 6.4f; // Growing season mean at the treeline, C.

TypedArray<Resource> PlantGenerator::make_alpine(AlpineStand p_stand, int p_variants, bool p_textures, const Dictionary &p_season) {
	using P = PlantGenerator;
	struct AlpineSpec {
		Preset preset;
		float spacing, footprint, density, scale_min, scale_max;
		float patch_scale; // 0: no patches.
		float patch_cover;
		float visibility;
		float align;
	};
	LocalVector<AlpineSpec> set;
	switch (p_stand) {
		case ALPINE_FELLFIELD:
			// Exposed ridges: cushions and mats far apart on gravel, the sedge in the lee.
			set = LocalVector<AlpineSpec>({ { P::PRESET_MOSS_CAMPION, 0.45f, 0.12f, 0.6f, 0.7f, 1.4f, 0.0f, 0.0f, 30.0f, 0.9f },
					{ P::PRESET_PURPLE_SAXIFRAGE, 0.35f, 0.08f, 0.5f, 0.8f, 1.3f, 3.0f, 0.5f, 25.0f, 0.8f },
					{ P::PRESET_MOUNTAIN_AVENS, 0.4f, 0.1f, 0.45f, 0.8f, 1.4f, 4.0f, 0.45f, 30.0f, 0.8f },
					{ P::PRESET_ALPINE_SEDGE, 0.25f, 0.03f, 0.35f, 0.7f, 1.0f, 2.5f, 0.4f, 30.0f, 0.4f } });
			break;
		case ALPINE_GRASSLAND:
			// The closed sward of the slopes: curved sedge, avens and campion in it.
			set = LocalVector<AlpineSpec>({ { P::PRESET_ALPINE_SEDGE, 0.1f, 0.02f, 0.95f, 0.8f, 1.2f, 0.0f, 0.0f, 35.0f, 0.4f },
					{ P::PRESET_MOUNTAIN_AVENS, 0.5f, 0.1f, 0.3f, 0.8f, 1.3f, 5.0f, 0.35f, 30.0f, 0.8f },
					{ P::PRESET_MOSS_CAMPION, 0.8f, 0.1f, 0.2f, 0.6f, 1.1f, 0.0f, 0.0f, 30.0f, 0.9f } });
			break;
		case ALPINE_SNOWBED:
			// Late-lying snow: dwarf willow carpets with moss, a little sedge.
			set = LocalVector<AlpineSpec>({ { P::PRESET_DWARF_WILLOW, 0.12f, 0.02f, 0.9f, 0.8f, 1.3f, 3.0f, 0.65f, 25.0f, 0.8f },
					{ P::PRESET_MOSS, 0.3f, 0.0f, 0.6f, 0.8f, 1.4f, 4.0f, 0.5f, 25.0f, 1.0f },
					{ P::PRESET_ALPINE_SEDGE, 0.3f, 0.03f, 0.3f, 0.6f, 0.9f, 0.0f, 0.0f, 30.0f, 0.4f } });
			break;
		case ALPINE_SCREE:
			// Moving scree: toadflax wandering through it, saxifrage mats holding it, avens over it.
			set = LocalVector<AlpineSpec>({ { P::PRESET_ALPINE_TOADFLAX, 0.6f, 0.06f, 0.45f, 0.8f, 1.3f, 0.0f, 0.0f, 25.0f, 0.6f },
					{ P::PRESET_PURPLE_SAXIFRAGE, 0.7f, 0.08f, 0.35f, 0.8f, 1.3f, 0.0f, 0.0f, 25.0f, 0.8f },
					{ P::PRESET_MOUNTAIN_AVENS, 1.0f, 0.1f, 0.25f, 0.8f, 1.5f, 0.0f, 0.0f, 30.0f, 0.8f } });
			break;
		case ALPINE_MAX:
			break;
	}
	TypedArray<Resource> out;
	int seed_base = 9500 + int(p_stand) * 100;
	for (const AlpineSpec &f : set) {
		Ref<PlantGenerator> gen;
		gen.instantiate();
		gen->set_textures(p_textures);
		gen->set_preset(f.preset);
		gen->apply_preset_colors();
		if (f.preset == P::PRESET_MOSS) {
			// Snowbed mosses (Polytrichum sexangulare, Kiaeria): dark olive.
			gen->set_leaf_color(Color(0.16f, 0.24f, 0.08f));
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
		// Above the treeline, which is set by the growing season's warmth, not the year's: trees stop
		// where the season (days over 0.9 C) averages 6.4 C (Paulsen and Körner 2014). A mean annual
		// band would miss oceanic mountains, mild in winter but cool all summer, whose treeline lies
		// far lower (Scotland about 650 m). Faded over 0.8 C so the treeline is an ecotone; only polar
		// cold (mean annual under -12 C) stops the plants.
		sp->set_rule_range(PlantSpecies::RULE_SEASON_TEMPERATURE, Vector2(-100.0f, ALP_TREELINE_SEASON));
		sp->set_rule_fade(PlantSpecies::RULE_SEASON_TEMPERATURE, 0.8f);
		sp->set_rule_range(PlantSpecies::RULE_TEMPERATURE, Vector2(-12.0f, 100.0f));
		sp->set_rule_fade(PlantSpecies::RULE_TEMPERATURE, 2.0f);
		switch (p_stand) {
			case ALPINE_FELLFIELD:
				// Convex ground (curvature negative), blown free of snow; up to steep gravel.
				sp->set_rule_range(PlantSpecies::RULE_CURVATURE, Vector2(-1000.0f, -0.001f));
				sp->set_rule_fade(PlantSpecies::RULE_CURVATURE, 0.002f);
				// Wind-blasted ridges hold alpine heath below the climatic treeline, most of all in
				// oceanic mountains (Grace 1989; Crawford 2008, "Plants at the Margin").
				sp->set_rule_range(PlantSpecies::RULE_SEASON_TEMPERATURE, Vector2(-100.0f, ALP_TREELINE_SEASON + 1.0f));
				sp->set_rule_range(PlantSpecies::RULE_SLOPE, Vector2(0.0f, 35.0f));
				sp->set_rule_fade(PlantSpecies::RULE_SLOPE, 5.0f);
				sp->set_rule_range(PlantSpecies::RULE_TALUS, Vector2(0.0f, 0.4f));
				sp->set_rule_fade(PlantSpecies::RULE_TALUS, 0.15f);
				break;
			case ALPINE_GRASSLAND:
				sp->set_rule_range(PlantSpecies::RULE_SLOPE, Vector2(0.0f, 38.0f));
				sp->set_rule_fade(PlantSpecies::RULE_SLOPE, 5.0f);
				sp->set_rule_range(PlantSpecies::RULE_SOIL_DEPTH, Vector2(0.15f, 100000.0f));
				sp->set_rule_fade(PlantSpecies::RULE_SOIL_DEPTH, 0.1f);
				sp->set_rule_range(PlantSpecies::RULE_TALUS, Vector2(0.0f, 0.2f));
				sp->set_rule_fade(PlantSpecies::RULE_TALUS, 0.1f);
				break;
			case ALPINE_SNOWBED:
				// Hollows (curvature positive) where the snow lies into summer, best on the shaded side.
				sp->set_rule_range(PlantSpecies::RULE_CURVATURE, Vector2(0.001f, 1000.0f));
				sp->set_rule_fade(PlantSpecies::RULE_CURVATURE, 0.002f);
				sp->set_rule_range(PlantSpecies::RULE_SLOPE, Vector2(0.0f, 20.0f));
				sp->set_rule_fade(PlantSpecies::RULE_SLOPE, 5.0f);
				sp->set_rule_range(PlantSpecies::RULE_SUNLIGHT, Vector2(0.0f, 170.0f));
				sp->set_rule_fade(PlantSpecies::RULE_SUNLIGHT, 30.0f);
				break;
			case ALPINE_SCREE:
				sp->set_rule_range(PlantSpecies::RULE_TALUS, Vector2(0.3f, 1.0f));
				sp->set_rule_fade(PlantSpecies::RULE_TALUS, 0.15f);
				sp->set_rule_range(PlantSpecies::RULE_SLOPE, Vector2(0.0f, 38.0f));
				sp->set_rule_fade(PlantSpecies::RULE_SLOPE, 4.0f);
				break;
			case ALPINE_MAX:
				break;
		}
		if (f.patch_scale > 0.0f) {
			sp->set_patch_scale(f.patch_scale);
			sp->set_patch_cover(f.patch_cover);
			sp->set_patch_seed(seed_base);
		}
		sp->set_visibility_range(f.visibility);
		sp->set_meta("alpine", true);
		out.push_back(sp);
		seed_base += 17;
	}
	return out;
}

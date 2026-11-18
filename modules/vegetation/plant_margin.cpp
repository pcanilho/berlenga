/**************************************************************************/
/*  plant_margin.cpp                                                      */
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

// Field margin plants (docs/ideas/vegetation.md, "Farmland", stage 3): the arable weeds of the
// headland and the uncropped strip (corn poppy, cornflower, scentless mayweed), and the verge's
// cow parsley under the hedge. Measurements from Stace (2019, "New Flora of the British Isles"),
// Wilson and King (2003, "Arable plants: a field guide") and the Biological Flora accounts.

#include "grass_field_3d.h"
#include "plant_generator.h"
#include "plant_species.h"

namespace {
const float MG_GOLDEN = 2.39996323f;

Basis mg_frame(const Vector3 &p_forward, const Vector3 &p_up_hint) {
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

Vector3 mg_dir(float p_azimuth, float p_elevation) {
	return Vector3(Math::cos(p_azimuth) * Math::cos(p_elevation), Math::sin(p_elevation), Math::sin(p_azimuth) * Math::cos(p_elevation));
}

// A stem rising from the ground, leaning out by p_lean (radians from upright) toward p_azimuth.
Transform3D mg_stem_base(float p_azimuth, float p_lean) {
	const Vector3 up = (mg_dir(p_azimuth, 0.0f) * Math::sin(p_lean) + Vector3(0, Math::cos(p_lean), 0)).normalized();
	return Transform3D(mg_frame(up, Vector3(1, 0, 0)), Vector3());
}
} // namespace

// Corn poppy (Papaver rhoeas): an annual of disturbed arable ground. A rosette of pinnately lobed,
// bristly leaves 5 to 15 cm; stiff, bristly stems 20 to 70 cm, a few branches; the buds hang on
// their stalks and straighten as they open; four crumpled scarlet petals 2 to 4 cm, often with a
// black blotch at the base, round a ring of dark stamens; the fruit an urn-shaped capsule 1 to 2 cm
// with a flat, rayed lid. Flowers June to August, on and off.
void PlantGenerator::_build_corn_poppy() {
	leaf_wear = 0.15f;
	Leaf l;
	l.shape = LEAF_LANCEOLATE;
	l.lobes = 5;
	l.lobe_depth = 0.6f;
	l.petiole = 0.15f;
	l.segments = 8;
	const int leaves = int(_rand(5.0f, 10.0f));
	const float spin = _rand(0.0f, Math::TAU);
	for (int k = 0; k < leaves; k++) {
		const Vector3 d = mg_dir(spin + k * MG_GOLDEN, _rand(0.2f, 0.6f));
		l.length = _rand(0.05f, 0.14f);
		l.width = l.length * 0.3f;
		l.fold = _rand(0.15f, 0.3f);
		l.bend = _rand(0.4f, 0.8f);
		l.twist = _rand(-0.4f, 0.4f);
		_add_leaf(SURFACE_LEAF, Transform3D(mg_frame(d, Vector3(0, 1, 0)), Vector3(0, 0.003f, 0)), l, 0.0f);
	}
	plant_height = MAX(plant_height, 0.06f);
	const int stems = int(_rand(1.0f, 3.99f));
	Leaf p;
	p.shape = LEAF_ROUND;
	p.fold = 0.25f; // Crumpled silk, cupped.
	p.bend = -0.3f;
	p.twist = 0.15f;
	p.segments = 5;
	for (int s = 0; s < stems; s++) {
		float progress = 0.0f, dead = 0.0f;
		const Bloom bloom = _bloom(false, &progress, &dead);
		if (bloom == BLOOM_NONE) {
			continue;
		}
		const Wither wither(this, dead);
		const float h = _rand(0.25f, 0.65f);
		const LocalVector<Transform3D> frames = _stem_frames(mg_stem_base(spin + s * 2.3f, _rand(0.05f, 0.25f)), h, _rand(-0.15f, 0.15f), 8);
		_add_stem_along(frames, 0.0025f, 0.0016f, 0.0f);
		// A few smaller lobed leaves up the stem, clasping.
		for (int k = 0; k < 3; k++) {
			const Transform3D at = _frame_at(frames, 0.15f + 0.15f * k);
			l.length = _rand(0.03f, 0.06f) * (1.0f - 0.2f * k);
			l.width = l.length * 0.3f;
			l.petiole = 0.0f;
			const Vector3 ld = (mg_dir(spin + s + k * MG_GOLDEN, 0.0f) + at.basis.get_column(2) * 0.5f).normalized();
			_add_leaf(SURFACE_LEAF, Transform3D(mg_frame(ld, Vector3(0, 1, 0)), at.origin), l, 0.3f + 0.1f * k);
		}
		const int branches = int(_rand(0.0f, 2.99f));
		for (int b = 0; b <= branches; b++) {
			Transform3D tip;
			if (b == branches) {
				tip = frames[frames.size() - 1];
			} else {
				const Transform3D at = _frame_at(frames, _rand(0.45f, 0.75f));
				const Vector3 d = (mg_dir(spin + b * MG_GOLDEN + 0.7f, 0.0f) * 0.55f + Vector3(0, 1, 0)).normalized();
				tip = _add_stem(Transform3D(mg_frame(d, Vector3(0, 1, 0)), at.origin), _rand(0.08f, 0.2f), 0.0016f, 0.0012f, -0.1f, 5, 0.5f);
			}
			const float r = _rand(0.0f, 1.0f);
			if (bloom == BLOOM_SEED || (b < branches && r < progress * 0.7f)) {
				// The capsule: an urn on its stalk, the flat lid on top.
				const Vector3 z = tip.basis.get_column(2);
				const Wither wp(this, dead * 0.6f);
				_add_head(SURFACE_CENTRE, Transform3D(tip.basis, tip.origin), 0.0055f, 0.014f, Color(1, 1, 1), 4, 10);
				_add_head(SURFACE_CENTRE, Transform3D(tip.basis, tip.origin + z * 0.013f), 0.0045f, 0.0015f, Color(0.75f, 0.7f, 0.6f), 2, 10);
				continue;
			}
			if (b < branches && r > 0.75f) {
				// A bud hanging on its crook: an oval of green, bristly sepals.
				const Vector3 down = (tip.basis.get_column(0) * 0.4f - Vector3(0, 1, 0)).normalized();
				const Transform3D crook = _add_stem(tip, 0.02f, 0.0012f, 0.001f, 1.6f, 4, 0.9f);
				_add_head(SURFACE_LEAF, Transform3D(mg_frame(down, Vector3(1, 0, 0)), crook.origin), 0.005f, 0.014f, Color(1, 1, 1), 4, 8);
				continue;
			}
			const Vector3 face = (tip.basis.get_column(2) * 0.6f + Vector3(0, 1, 0)).normalized();
			const Basis fb = mg_frame(face, Vector3(1, 0, 0));
			const float sz = _rand(0.022f, 0.036f);
			p.length = sz;
			p.width = sz * 1.15f;
			const float fs = _rand(0.0f, Math::TAU);
			for (int k = 0; k < 4; k++) {
				// Two outer petals larger, overlapping two inner ones.
				const float pa = fs + k * Math::PI * 0.5f;
				const float rise = k % 2 == 0 ? 0.55f : 0.8f;
				Leaf pk = p;
				pk.length *= k % 2 == 0 ? 1.0f : 0.85f;
				const Vector3 d = (fb.get_column(0) * Math::cos(pa) + fb.get_column(1) * Math::sin(pa) + face * rise).normalized();
				_add_leaf(SURFACE_PETAL, Transform3D(mg_frame(d, face), tip.origin + face * (k % 2 == 0 ? 0.0f : 0.002f)), pk, 1.0f);
			}
			// The young capsule with its lid, ringed by the dark stamens.
			_add_head(SURFACE_CENTRE, Transform3D(fb, tip.origin), 0.004f, 0.007f, Color(1, 1, 1), 3, 8);
			_add_head(SURFACE_CENTRE, Transform3D(fb, tip.origin + face * 0.002f), 0.0065f, 0.003f, Color(0.12f, 0.1f, 0.12f), 2, 12);
		}
		plant_height = MAX(plant_height, frames[frames.size() - 1].origin.y);
	}
}

// Cornflower (Centaurea cyanus): an arable annual, slender, grey-felted and branched, 30 to 80 cm;
// narrow leaves 4 to 10 cm, the lower ones with a few lobes; solitary heads 1.5 to 3 cm, an
// egg-shaped involucre of bracts with dark, fringed edges, the outer florets enlarged, funnel
// shaped and bright blue, spreading round the purple-blue inner ones. June to August.
void PlantGenerator::_build_cornflower() {
	leaf_wear = 0.1f;
	const float spin = _rand(0.0f, Math::TAU);
	Leaf l;
	l.shape = LEAF_LINEAR;
	l.segments = 6;
	const int basal = int(_rand(3.0f, 6.0f));
	for (int k = 0; k < basal; k++) {
		l.length = _rand(0.05f, 0.1f);
		l.width = l.length * 0.1f;
		l.lobes = k % 2 == 0 ? 2 : 0;
		l.lobe_depth = 0.4f;
		l.fold = 0.25f;
		l.bend = _rand(0.4f, 0.9f);
		_add_leaf(SURFACE_LEAF, Transform3D(mg_frame(mg_dir(spin + k * MG_GOLDEN, _rand(0.3f, 0.7f)), Vector3(0, 1, 0)), Vector3(0, 0.003f, 0)), l, 0.0f);
	}
	plant_height = MAX(plant_height, 0.05f);
	auto add_head = [&](const Transform3D &p_top, bool p_seed) {
		const Basis f = p_top.basis.orthonormalized();
		const Vector3 z = f.get_column(2);
		// The involucre: an egg of bracts under the florets.
		_add_head(SURFACE_CENTRE, Transform3D(f, p_top.origin - z * 0.006f), 0.0045f, 0.011f, Color(1, 1, 1), 4, 10);
		if (p_seed) {
			return;
		}
		Leaf fl;
		fl.shape = LEAF_SPATULATE;
		fl.lobes = 2;
		fl.lobe_depth = 0.35f; // The funnel's toothed mouth.
		fl.fold = 0.45f;
		fl.bend = -0.2f;
		fl.segments = 3;
		const int outer = int(_rand(6.0f, 10.0f));
		const float a0 = _rand(0.0f, Math::TAU);
		for (int k = 0; k < outer; k++) {
			const float a = a0 + k * Math::TAU / outer + _rand(-0.12f, 0.12f);
			const Vector3 radial = f.get_column(0) * Math::cos(a) + f.get_column(1) * Math::sin(a);
			fl.length = _rand(0.011f, 0.015f);
			fl.width = fl.length * 0.55f;
			const Vector3 d = (radial + z * _rand(0.35f, 0.7f)).normalized();
			_add_leaf(SURFACE_PETAL, Transform3D(mg_frame(d, z), p_top.origin + z * 0.004f + radial * 0.002f), fl, 1.0f);
		}
		// The inner florets: a tuft of short, darker tubes.
		Leaf in;
		in.shape = LEAF_LINEAR;
		in.length = 0.006f;
		in.width = 0.0012f;
		in.fold = 0.0f;
		in.bend = 0.0f;
		in.segments = 2;
		for (int n = 0; n < 10; n++) {
			const float u = Math::sqrt((n + 0.5f) / 10.0f);
			const float a = n * MG_GOLDEN;
			const Vector3 radial = f.get_column(0) * Math::cos(a) + f.get_column(1) * Math::sin(a);
			_add_leaf(SURFACE_PETAL, Transform3D(mg_frame((z + radial * u * 0.6f).normalized(), z), p_top.origin + z * 0.004f + radial * u * 0.003f), in, 1.0f, 1.0f, Color(0.55f, 0.35f, 0.7f));
		}
	};
	const int stems = int(_rand(1.0f, 2.99f));
	for (int s = 0; s < stems; s++) {
		float progress = 0.0f, dead = 0.0f;
		const Bloom bloom = _bloom(false, &progress, &dead);
		if (bloom == BLOOM_NONE) {
			continue;
		}
		const Wither wither(this, dead);
		const float h = _rand(0.3f, 0.75f);
		const LocalVector<Transform3D> frames = _stem_frames(mg_stem_base(spin + s * 2.1f, _rand(0.05f, 0.2f)), h, _rand(-0.15f, 0.15f), 9);
		_add_stem_along(frames, 0.002f, 0.0012f, 0.0f);
		const int leaves = int(_rand(5.0f, 9.0f));
		for (int k = 0; k < leaves; k++) {
			const float u = 0.1f + 0.6f * float(k) / leaves;
			const Transform3D at = _frame_at(frames, u);
			const float a = spin + k * MG_GOLDEN;
			const Vector3 radial = at.basis.get_column(0) * Math::cos(a) + at.basis.get_column(1) * Math::sin(a);
			l.length = _rand(0.04f, 0.08f) * (1.0f - 0.5f * u);
			l.width = l.length * 0.08f;
			l.lobes = 0;
			l.bend = 0.3f;
			_add_leaf(SURFACE_LEAF, Transform3D(mg_frame((radial + at.basis.get_column(2) * 1.2f).normalized(), at.basis.get_column(2)), at.origin), l, u);
		}
		add_head(frames[frames.size() - 1], bloom == BLOOM_SEED);
		// Branches from the upper half, each with a head.
		const int branches = int(_rand(1.0f, 4.99f));
		for (int b = 0; b < branches; b++) {
			Transform3D at = _frame_at(frames, _rand(0.45f, 0.85f));
			const float a = _rand(0.0f, Math::TAU);
			const Vector3 radial = at.basis.get_column(0) * Math::cos(a) + at.basis.get_column(1) * Math::sin(a);
			at.basis = mg_frame(at.basis.get_column(2) + radial * 0.6f, Vector3(0, 1, 0));
			const Transform3D t2 = _add_stem(at, _rand(0.06f, 0.2f), 0.0012f, 0.001f, -0.25f, 5, 0.6f);
			add_head(t2, bloom == BLOOM_SEED || _rand(0.0f, 1.0f) < progress * 0.5f);
		}
		plant_height = MAX(plant_height, frames[frames.size() - 1].origin.y);
	}
}

// Scentless mayweed (Tripleurospermum inodorum): a sprawling annual of field edges and gateways,
// 15 to 60 cm, branched from the base; the leaves cut two or three times into thread-like segments,
// feathery; daisy-like heads 2 to 4 cm, 12 to 25 white rays round a domed yellow disc. June to
// October.
void PlantGenerator::_build_mayweed() {
	leaf_wear = 0.1f;
	const float spin = _rand(0.0f, Math::TAU);
	// A feathery leaf: a midrib with thread-like segments either side.
	auto feather = [&](const Transform3D &p_base, float p_length, float p_wind, float p_droop) {
		const LocalVector<Transform3D> fr = _stem_frames(p_base, p_length, p_droop, 5);
		_add_stem(p_base, p_length, 0.0007f, 0.0004f, p_droop, 5, p_wind, SURFACE_LEAF);
		Leaf th;
		th.shape = LEAF_LINEAR;
		th.fold = 0.0f;
		th.bend = 0.3f;
		th.segments = 2;
		const int n = int(p_length / 0.0035f);
		for (int k = 0; k < n; k++) {
			const float t = 0.1f + 0.88f * float(k) / MAX(n - 1, 1);
			const Transform3D at = _frame_at(fr, t);
			const float side = (k % 2 == 0) ? 1.0f : -1.0f;
			th.length = p_length * 0.3f * Math::sin(Math::PI * MIN(t * 1.05f, 1.0f)) + 0.005f;
			th.width = 0.0012f;
			const Vector3 d = (at.basis.get_column(0) * side + at.basis.get_column(2) * 0.7f + Vector3(0, 0.2f, 0)).normalized();
			_add_leaf(SURFACE_LEAF, Transform3D(mg_frame(d, at.basis.get_column(1)), at.origin), th, Math::lerp(p_wind, 1.0f, t));
		}
	};
	const int stems = int(_rand(2.0f, 5.99f));
	float top = 0.04f;
	for (int s = 0; s < stems; s++) {
		float progress = 0.0f, dead = 0.0f;
		const Bloom bloom = _bloom(false, &progress, &dead);
		// Sprawling: the stems lean well out before turning up.
		const float lean = _rand(0.35f, 0.9f);
		const float h = bloom == BLOOM_NONE ? _rand(0.08f, 0.14f) : _rand(0.18f, 0.5f);
		const Wither wither(this, dead);
		const LocalVector<Transform3D> frames = _stem_frames(mg_stem_base(spin + s * MG_GOLDEN, lean), h, -lean * 0.8f, 8);
		_add_stem_along(frames, 0.0022f, 0.0012f, 0.0f);
		// Leafy all the way up: the feathery leaves crowd the stem.
		const int leaves = int(_rand(10.0f, 16.0f));
		for (int k = 0; k < leaves; k++) {
			const float u = 0.05f + 0.8f * float(k) / leaves;
			const Transform3D at = _frame_at(frames, u);
			const float a = spin + k * MG_GOLDEN;
			const Vector3 radial = at.basis.get_column(0) * Math::cos(a) + at.basis.get_column(1) * Math::sin(a);
			feather(Transform3D(mg_frame((radial + at.basis.get_column(2) * 0.8f).normalized(), at.basis.get_column(2)), at.origin), _rand(0.03f, 0.065f) * (1.0f - 0.45f * u), u, 0.3f);
		}
		top = MAX(top, frames[frames.size() - 1].origin.y);
		if (bloom == BLOOM_NONE) {
			continue;
		}
		Leaf ray;
		ray.shape = LEAF_SPATULATE;
		ray.width = 0.0035f;
		ray.fold = 0.05f;
		ray.bend = 0.25f;
		ray.segments = 3;
		auto head = [&](const Transform3D &p_top) {
			if (bloom == BLOOM_SEED) {
				// Rays fallen: the brown dome of achenes.
				_add_head(SURFACE_CENTRE, p_top, 0.0045f, 0.0045f, Color(0.5f, 0.42f, 0.22f));
				return;
			}
			ray.length = _rand(0.01f, 0.016f);
			// Older heads: the rays bent back as the disc domes up.
			ray.bend = _rand(0.0f, 1.0f) < progress ? 0.9f : 0.25f;
			_add_ray_flower(p_top, 0.0045f, int(_rand(13.0f, 22.0f)), ray);
		};
		head(frames[frames.size() - 1]);
		const int branches = int(_rand(1.0f, 3.99f));
		for (int b = 0; b < branches; b++) {
			Transform3D at = _frame_at(frames, _rand(0.5f, 0.85f));
			const float a = _rand(0.0f, Math::TAU);
			const Vector3 radial = at.basis.get_column(0) * Math::cos(a) + at.basis.get_column(1) * Math::sin(a);
			at.basis = mg_frame(Vector3(0, 1, 0) + radial * 0.5f, Vector3(1, 0, 0));
			head(_add_stem(at, _rand(0.04f, 0.12f), 0.0012f, 0.001f, -0.1f, 4, 0.6f));
		}
	}
	plant_height = MAX(plant_height, top);
}

// Cow parsley (Anthriscus sylvestris): the verge's umbellifer, flowering first of them, April to
// June. Hollow, furrowed stems 60 to 150 cm; fern-like leaves cut two or three times, up to 30 cm;
// compound umbels 2 to 6 cm across of 6 to 12 rays with no bracts under them, lacy white; the
// fruits smooth, slender, 7 to 10 mm, green then black; the stems stand dead into summer.
void PlantGenerator::_build_cow_parsley() {
	leaf_wear = 0.15f;
	const float spin = _rand(0.0f, Math::TAU);
	// A fern-like leaf: pinnae on a stalk, each cut again.
	auto frond = [&](const Transform3D &p_base, float p_length, float p_wind, float p_droop) {
		const LocalVector<Transform3D> fr = _stem_frames(p_base, p_length, p_droop, 6);
		_add_stem(p_base, p_length, 0.0016f, 0.0006f, p_droop, 6, p_wind);
		const int pinnae = int(p_length / 0.03f) + 2;
		for (int k = 0; k < pinnae; k++) {
			const float t = 0.3f + 0.68f * float(k) / MAX(pinnae - 1, 1);
			const Transform3D at = _frame_at(fr, t);
			const float side = (k % 2 == 0) ? 1.0f : -1.0f;
			Leaf l;
			l.shape = LEAF_LANCEOLATE;
			l.length = p_length * 0.32f * (1.15f - t) + 0.01f;
			l.width = l.length * 0.5f;
			l.lobes = 5;
			l.lobe_depth = 0.75f;
			l.fold = 0.1f;
			l.bend = 0.25f;
			l.segments = 5;
			_add_leaf(SURFACE_LEAF, Transform3D(mg_frame((at.basis.get_column(0) * side + at.basis.get_column(2) * 0.5f).normalized(), at.basis.get_column(1)), at.origin), l, Math::lerp(p_wind, 1.0f, t));
		}
	};
	const int basal = int(_rand(4.0f, 8.0f));
	for (int i = 0; i < basal; i++) {
		const float u = float(i) / basal;
		frond(Transform3D(mg_frame(mg_dir(spin + i * MG_GOLDEN, Math::lerp(0.5f, 0.95f, u)), Vector3(0, 1, 0)), Vector3(0, 0.003f, 0)), _rand(0.14f, 0.3f), 0.0f, _rand(0.5f, 0.9f));
	}
	plant_height = MAX(plant_height, 0.15f);
	const Color fruit(0.55f, 0.6f, 0.35f);
	auto umbel = [&](const Transform3D &p_top, float p_radius, bool p_seed, float p_ripe) {
		const Vector3 z(0, 1, 0);
		const Basis f = mg_frame(z, Vector3(1, 0, 0));
		const Vector3 c = p_top.origin;
		Leaf fl;
		fl.shape = LEAF_ROUND;
		fl.length = 0.0015f;
		fl.width = 0.0017f;
		fl.fold = 0.1f;
		fl.bend = 0.1f;
		fl.segments = 2;
		Leaf fr;
		fr.shape = LEAF_LINEAR;
		fr.length = 0.008f;
		fr.width = 0.0012f;
		fr.fold = 0.3f;
		fr.bend = 0.0f;
		fr.segments = 2;
		const int rays = int(_rand(6.0f, 12.99f));
		for (int r = 0; r < rays; r++) {
			const float a = r * Math::TAU / rays + _rand(-0.2f, 0.2f);
			const Vector3 radial = f.get_column(0) * Math::cos(a) + f.get_column(1) * Math::sin(a);
			const float u = _rand(0.75f, 1.0f);
			// A flat-topped, open umbel; in fruit the rays stiffen upward.
			const Vector3 tip = c + radial * p_radius * u * (p_seed ? 0.75f : 1.0f) + z * p_radius * (p_seed ? 0.75f : 0.45f);
			const Vector3 d = tip - c;
			_add_stem(Transform3D(mg_frame(d, Vector3(1, 0, 0)), c), d.length(), 0.0006f, 0.0005f, 0.0f, 1, 0.85f);
			const int florets = 9;
			const float ur = p_radius * 0.22f;
			for (int k = 0; k < florets; k++) {
				const float uk = Math::sqrt((k + 0.5f) / florets);
				const float ak = k * MG_GOLDEN + a;
				const Vector3 rk = f.get_column(0) * Math::cos(ak) + f.get_column(1) * Math::sin(ak);
				const Vector3 at = tip + rk * ur * uk + z * 0.002f * (1.0f - uk);
				if (p_seed) {
					// Paired fruits, upright, greening to black.
					_add_leaf(SURFACE_CENTRE, Transform3D(mg_frame((z + rk * 0.25f).normalized(), rk), at), fr, 1.0f, 1.0f, fruit.lerp(Color(0.12f, 0.1f, 0.08f), p_ripe));
				} else {
					_add_leaf(SURFACE_PETAL, Transform3D(mg_frame((z + rk * 0.4f).normalized(), rk), at), fl, 1.0f);
				}
			}
		}
	};
	const int stems = int(_rand(1.0f, 2.99f));
	for (int s = 0; s < stems; s++) {
		float progress = 0.0f, dead = 0.0f;
		const Bloom bloom = _bloom(false, &progress, &dead);
		if (bloom == BLOOM_NONE) {
			continue;
		}
		const Wither wither(this, dead);
		const float h = _rand(0.6f, 1.3f);
		const LocalVector<Transform3D> frames = _stem_frames(mg_stem_base(spin + s * 2.4f, _rand(0.03f, 0.12f)), h, _rand(-0.1f, 0.1f), 12);
		_add_stem_along(frames, 0.0045f, 0.0022f, 0.0f);
		for (int k = 0; k < 3; k++) {
			const Transform3D at = _frame_at(frames, 0.25f + 0.18f * k);
			const float a = spin + k * MG_GOLDEN;
			const Vector3 radial = at.basis.get_column(0) * Math::cos(a) + at.basis.get_column(1) * Math::sin(a);
			frond(Transform3D(mg_frame((radial + at.basis.get_column(2) * 0.6f).normalized(), at.basis.get_column(2)), at.origin), _rand(0.07f, 0.14f) * (1.0f - 0.25f * k), 0.3f + 0.15f * k, 0.35f);
		}
		const bool seed = bloom == BLOOM_SEED;
		umbel(frames[frames.size() - 1], _rand(0.025f, 0.04f), seed, dead);
		// Side umbels from the upper leaf joints, opening after the first.
		const int branches = int(_rand(2.0f, 4.99f));
		for (int b = 0; b < branches; b++) {
			Transform3D at = _frame_at(frames, _rand(0.62f, 0.88f));
			const float a = spin + b * MG_GOLDEN + 1.1f;
			const Vector3 radial = at.basis.get_column(0) * Math::cos(a) + at.basis.get_column(1) * Math::sin(a);
			at.basis = mg_frame(at.basis.get_column(2) + radial * 0.6f, Vector3(0, 1, 0));
			const Transform3D t2 = _add_stem(at, _rand(0.1f, 0.25f), 0.0022f, 0.0015f, -0.25f, 5, 0.6f);
			umbel(t2, _rand(0.018f, 0.03f), seed || _rand(0.0f, 1.0f) < progress * 0.6f, dead);
		}
		plant_height = MAX(plant_height, frames[frames.size() - 1].origin.y + 0.03f);
	}
}

// The plants at a crop field's edge (docs/ideas/vegetation.md, "Farmland", stage 3), placed by their
// distance from the nearest field's edge (PlantSpecies field_edge_range, negative inside the field):
// arable weeds thickest in the outer metres of the crop, where the drill and the sprayer miss and
// the hedge's seed bank spills in (Marshall 1989; Wilson and Aebischer 1995: the outer 2 to 6 m of
// a field hold most of its weeds), and the verge's tall herbs on the grass strip by the hedge.
TypedArray<Resource> PlantGenerator::make_margin(MarginStand p_stand, int p_variants, bool p_textures, const Dictionary &p_season) {
	using P = PlantGenerator;
	struct MarginSpec {
		Preset preset;
		float spacing, footprint, density, scale_min, scale_max;
		float patch_scale; // 0: no patches.
		float patch_cover;
		float visibility;
		float align;
		Vector2 edge; // Metres from the field's edge, negative inside.
		float edge_fade;
	};
	LocalVector<MarginSpec> set;
	switch (p_stand) {
		case MARGIN_HEADLAND:
			// The crop's outer metres and the uncropped strip: poppies in drifts where the soil was
			// turned, cornflowers fewer (scarce under modern spraying), mayweed along the very edge
			// and in the gateways.
			set = LocalVector<MarginSpec>({ { P::PRESET_CORN_POPPY, 0.3f, 0.06f, 0.55f, 0.8f, 1.2f, 7.0f, 0.4f, 40.0f, 0.3f, Vector2(-5.0f, 0.6f), 2.5f },
					{ P::PRESET_CORNFLOWER, 0.45f, 0.04f, 0.35f, 0.8f, 1.2f, 9.0f, 0.3f, 35.0f, 0.3f, Vector2(-3.0f, 0.6f), 1.5f },
					{ P::PRESET_MAYWEED, 0.35f, 0.08f, 0.5f, 0.8f, 1.2f, 5.0f, 0.45f, 30.0f, 0.5f, Vector2(-1.5f, 1.0f), 1.0f } });
			break;
		case MARGIN_VERGE:
			// The grass strip between the field and the hedge (GrassField3D.hedge_offset, 3.5 m out):
			// cow parsley's froth, nettles in patches at the hedge foot where nutrients run off the
			// field, knapweed and cocksfoot.
			set = LocalVector<MarginSpec>({ { P::PRESET_COW_PARSLEY, 0.5f, 0.15f, 0.6f, 0.8f, 1.25f, 6.0f, 0.55f, 60.0f, 0.2f, Vector2(0.5f, 2.6f), 0.5f },
					{ P::PRESET_COCKSFOOT, 0.25f, 0.05f, 0.55f, 0.8f, 1.2f, 0.0f, 0.0f, 35.0f, 0.4f, Vector2(0.3f, 2.8f), 0.5f },
					{ P::PRESET_NETTLE, 0.35f, 0.08f, 0.45f, 0.8f, 1.2f, 5.0f, 0.3f, 40.0f, 0.2f, Vector2(1.4f, 2.8f), 0.6f },
					{ P::PRESET_KNAPWEED, 0.5f, 0.05f, 0.3f, 0.8f, 1.2f, 4.0f, 0.4f, 35.0f, 0.3f, Vector2(0.5f, 3.0f), 0.6f } });
			break;
		case MARGIN_MAX:
			break;
	}
	TypedArray<Resource> out;
	int seed_base = 12100 + int(p_stand) * 100;
	for (const MarginSpec &f : set) {
		Ref<PlantGenerator> gen;
		gen.instantiate();
		gen->set_textures(p_textures);
		gen->set_preset(f.preset);
		gen->apply_preset_colors();
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
		sp->set_rule_range(PlantSpecies::RULE_FIELD_EDGE, f.edge);
		sp->set_rule_fade(PlantSpecies::RULE_FIELD_EDGE, f.edge_fade);
		sp->set_rule_range(PlantSpecies::RULE_SLOPE, Vector2(0.0f, 30.0f));
		sp->set_rule_fade(PlantSpecies::RULE_SLOPE, 5.0f);
		if (f.patch_scale > 0.0f) {
			sp->set_patch_scale(f.patch_scale);
			sp->set_patch_cover(f.patch_cover);
			sp->set_patch_seed(seed_base);
		}
		sp->set_visibility_range(f.visibility);
		sp->set_meta("field_margin", true);
		tag_species(sp, f.preset);
		out.push_back(sp);
		seed_base += 13;
	}
	return out;
}

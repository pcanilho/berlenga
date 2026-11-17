/**************************************************************************/
/*  plant_saltmarsh.cpp                                                   */
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

// Salt marsh and shingle plants (docs/ideas/vegetation.md, "Salt marsh and shingle plants"): the
// marsh zoned by tidal height from glasswort and cord-grass on bare mud to sea lavender and sea
// aster above, and the strandline flora of shingle backshores. Measurements from Stace (2019),
// Rodwell (2000) and the Biological Flora accounts cited in the note.

#include "plant_generator.h"
#include "plant_species.h"

namespace {
const float SM_GOLDEN = 2.39996323f;

Basis sm_frame(const Vector3 &p_forward, const Vector3 &p_up_hint) {
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

Vector3 sm_dir(float p_azimuth, float p_elevation) {
	return Vector3(Math::cos(p_azimuth) * Math::cos(p_elevation), Math::sin(p_elevation), Math::sin(p_azimuth) * Math::cos(p_elevation));
}
} // namespace

// Glasswort (Salicornia europaea agg.): leafless, the leaves fused into fleshy sheaths round the
// stem, so a plant is a little candelabra of jointed, translucent green segments 3 to 5 mm thick
// and 8 to 15 mm long, 10 to 30 cm tall, branching in opposite pairs from the nodes. The flowers
// are sunk in the upper segments. In autumn the plants turn red and purple.
void PlantGenerator::_build_glasswort() {
	leaf_wear = 0.0f;
	if (bloom_day > 0) {
		// Glasswort reddens from late August, red and purple by October (betacyanins; Stace 2019),
		// and the dead stems stand brown into winter.
		const float day = float(bloom_day);
		const float red = Math::smoothstep(235.0f, 290.0f, day) * (1.0f - Math::smoothstep(330.0f, 360.0f, day));
		const float brown = MIN(Math::smoothstep(320.0f, 350.0f, day) + (day < 120.0f ? 1.0f : 0.0f), 1.0f);
		if (red + brown > 0.0f) { // From the preset's green, so each variant takes the same colour.
			stem_color = Color(0.34f, 0.52f, 0.12f).lerp(Color(0.5f, 0.1f, 0.2f), red).lerp(Color(0.34f, 0.24f, 0.16f), brown);
			leaf_color = stem_color;
		}
	}
	const float h = _rand(0.08f, 0.25f);
	const float thick = _rand(0.0024f, 0.0032f); // Segments 4 to 6 mm thick at the base.
	// One jointed axis: each segment swells from a narrow node to its full girth and back.
	auto jointed = [&](const Transform3D &p_base, float p_length, float p_radius, float p_bend) -> LocalVector<Transform3D> {
		const int segs = MAX(int(p_length / _rand(0.009f, 0.014f)), 2);
		const LocalVector<Transform3D> frames = _stem_frames(p_base, p_length, p_bend, segs);
		for (int i = 0; i < segs; i++) {
			const float t0 = float(i) / segs, t1 = float(i + 1) / segs, tm = (t0 + t1) * 0.5f;
			const float r = p_radius * (1.0f - 0.35f * float(i) / segs); // Thinner towards the tip.
			LocalVector<Transform3D> lower;
			lower.push_back(_frame_at(frames, t0));
			lower.push_back(_frame_at(frames, tm));
			_add_stem_along(lower, r * 0.7f, r, 0.0f);
			LocalVector<Transform3D> upper;
			upper.push_back(_frame_at(frames, tm));
			upper.push_back(_frame_at(frames, t1));
			_add_stem_along(upper, r, r * 0.72f, 0.0f);
		}
		_add_head(SURFACE_STEM, frames[frames.size() - 1], p_radius * 0.65f, p_radius * 1.6f, Color(1, 1, 1), 3, 8); // The blunt tip.
		return LocalVector<Transform3D>(frames);
	};
	const Transform3D base(sm_frame(Vector3(_rand(-0.15f, 0.15f), 1.0f, _rand(-0.15f, 0.15f)), Vector3(1, 0, 0)), Vector3());
	const LocalVector<Transform3D> main = jointed(base, h, thick, _rand(-0.1f, 0.1f));
	// Opposite branches from the lower and middle nodes, each pair at right angles to the last,
	// curving up so the plant is a candelabra; long ones branch again.
	const int nodes = MAX(int(h / 0.012f), 3);
	for (int k = 1; k < nodes - 1; k++) {
		const float t = float(k) / nodes;
		if (t > 0.75f) {
			break;
		}
		const Transform3D at = _frame_at(main, t);
		const float rot = (k % 2) * Math::PI * 0.5f + _rand(-0.2f, 0.2f);
		for (int side = 0; side < 2; side++) {
			if (_rand(0.0f, 1.0f) < 0.15f) {
				continue;
			}
			const float a = rot + side * Math::PI;
			const Vector3 out = at.basis.get_column(0) * Math::cos(a) + at.basis.get_column(1) * Math::sin(a);
			const Vector3 d = (out * 0.9f + at.basis.get_column(2) * 0.45f).normalized();
			const float len = h * (1.0f - t) * _rand(0.4f, 0.65f); // Out, then turned up.
			// Up-curving: the bend is negative to lift the tip towards the light.
			const LocalVector<Transform3D> br = jointed(Transform3D(sm_frame(d, Vector3(0, 1, 0)), at.origin), len, thick * 0.85f, -0.9f);
			if (len > 0.06f) {
				const Transform3D bat = _frame_at(br, 0.5f);
				const Vector3 o2 = bat.basis.get_column(0) * (side ? 1.0f : -1.0f);
				jointed(Transform3D(sm_frame((o2 * 0.6f + bat.basis.get_column(2)).normalized(), Vector3(0, 1, 0)), bat.origin), len * 0.45f, thick * 0.65f, -0.5f);
			}
		}
	}
	plant_height = MAX(plant_height, h);
}

// Common cord-grass (Spartina anglica): dense clonal clumps of stiff, erect stems 30 to 100 cm;
// flat, stiff, grey-green leaves 10 to 40 cm by 6 to 15 mm spreading at 30 to 45 degrees, the
// tips in-rolled and often withered; two to six erect, close-pressed yellow-brown spikes 10 to
// 25 cm in late summer that stand into winter.
void PlantGenerator::_build_cord_grass() {
	leaf_wear = 0.3f;
	const int stems = int(_rand(12.0f, 26.0f));
	const float clump = _rand(0.06f, 0.14f);
	float progress = 0.0f, dead = 0.0f;
	const Bloom bloom = _bloom(false, &progress, &dead);
	Leaf l;
	l.shape = LEAF_LINEAR;
	l.fold = 0.25f;
	l.segments = 6;
	for (int s = 0; s < stems; s++) {
		const float a = _rand(0.0f, Math::TAU);
		const Vector3 at = Vector3(Math::cos(a), 0, Math::sin(a)) * clump * Math::sqrt(_rand(0.0f, 1.0f));
		const float h = _rand(0.3f, 0.8f);
		const Vector3 up = (Vector3(Math::cos(a), 0, Math::sin(a)) * _rand(0.05f, 0.22f) + Vector3(0, 1, 0)).normalized();
		const LocalVector<Transform3D> frames = _stem_frames(Transform3D(sm_frame(up, Vector3(1, 0, 0)), at), h, _rand(0.0f, 0.15f), 6);
		_add_stem_along(frames, 0.0028f, 0.0016f, 0.0f);
		// Alternate leaves in two ranks up the stem, stiffly spreading, the lowest dead.
		const int leaves = int(_rand(5.0f, 9.0f));
		const float rank = _rand(0.0f, Math::TAU);
		for (int k = 0; k < leaves; k++) {
			const float t = Math::lerp(0.08f, 0.82f, float(k) / MAX(leaves - 1, 1));
			const Transform3D fat = _frame_at(frames, t);
			const float side = (k % 2) ? 1.0f : -1.0f;
			const Vector3 out = sm_dir(rank + (side > 0 ? 0.0f : Math::PI) + _rand(-0.3f, 0.3f), 0.0f);
			const Vector3 d = (out * _rand(0.55f, 0.75f) + fat.basis.get_column(2)).normalized();
			l.length = _rand(0.1f, 0.35f) * (1.0f - 0.4f * t);
			l.width = _rand(0.006f, 0.012f);
			l.bend = _rand(0.3f, 0.7f);
			l.twist = _rand(-0.4f, 0.4f);
			const Wither w(this, k == 0 ? 1.0f : (k == 1 ? 0.5f : 0.0f));
			_add_leaf(SURFACE_LEAF, Transform3D(sm_frame(d, Vector3(0, 1, 0)), fat.origin), l, 0.3f);
		}
		if (bloom == BLOOM_NONE || _rand(0.0f, 1.0f) < 0.4f) {
			continue;
		}
		// The spikes: two to six, pressed close to the axis, each a slim flattened column.
		const Wither w(this, dead * 0.8f);
		const int spikes = int(_rand(2.0f, 6.0f));
		for (int k = 0; k < spikes; k++) {
			const float slen = _rand(0.08f, 0.2f);
			const Transform3D at2 = _frame_at(frames, 1.0f - 0.07f * k);
			const Vector3 d = (at2.basis.get_column(2) + sm_dir(k * SM_GOLDEN, 0.0f) * 0.12f).normalized();
			(void)_add_stem(Transform3D(sm_frame(d, Vector3(0, 1, 0)), at2.origin), slen, 0.0022f, 0.0012f, 0.05f, 4, 1.0f, SURFACE_CENTRE);
		}
	}
	plant_height = MAX(plant_height, 0.3f);
}

// Sea purslane (Atriplex portulacoides): a low, spreading shrublet 20 to 60 cm of woody, rooting
// stems, densely clothed in opposite, elliptic to spoon-shaped leaves 2 to 5 cm, thick and covered
// in silvery-grey bladder hairs (mealy); lines the creek edges in grey bands.
void PlantGenerator::_build_sea_purslane() {
	leaf_wear = 0.15f;
	const int stems = int(_rand(9.0f, 15.0f));
	Leaf l;
	l.shape = LEAF_SPATULATE;
	l.fold = 0.2f;
	l.bend = 0.2f;
	l.petiole = 0.15f;
	l.segments = 4;
	for (int s = 0; s < stems; s++) {
		const float az = s * SM_GOLDEN + _rand(-0.3f, 0.3f);
		const float len = _rand(0.12f, 0.28f);
		// Sprawling: low out from the base, then rising.
		const LocalVector<Transform3D> frames = _stem_frames(Transform3D(sm_frame(sm_dir(az, _rand(0.12f, 0.45f)), Vector3(0, 1, 0)), Vector3()), len, -0.4f, 7);
		_add_stem_along(frames, 0.003f, 0.0015f, 0.0f);
		LocalVector<LocalVector<Transform3D>> shoots;
		shoots.push_back(LocalVector<Transform3D>(frames));
		const int forks = int(_rand(2.0f, 4.0f));
		for (int f = 0; f < forks; f++) {
			const Transform3D at = _frame_at(frames, _rand(0.3f, 0.8f));
			const Vector3 d = (at.basis.get_column(2) + at.basis.get_column(0) * ((f & 1) ? 0.8f : -0.8f) + Vector3(0, 0.4f, 0)).normalized();
			const LocalVector<Transform3D> br = _stem_frames(Transform3D(sm_frame(d, Vector3(0, 1, 0)), at.origin), len * _rand(0.3f, 0.55f), -0.3f, 4);
			_add_stem_along(br, 0.0018f, 0.001f, 0.0f);
			shoots.push_back(LocalVector<Transform3D>(br));
		}
		for (const LocalVector<Transform3D> &sh : shoots) {
			const int pairs = int(_rand(9.0f, 14.0f)); // Densely clothed: the grey mass hides the stems.
			for (int k = 0; k < pairs; k++) {
				const float t = Math::lerp(0.12f, 1.0f, (k + 0.5f) / pairs);
				const Transform3D at = _frame_at(sh, t);
				const float rot = (k % 2) * Math::PI * 0.5f;
				for (int side = 0; side < 2; side++) {
					const float a = rot + side * Math::PI;
					const Vector3 out = at.basis.get_column(0) * Math::cos(a) + at.basis.get_column(1) * Math::sin(a);
					const Vector3 d = (out + at.basis.get_column(2) * 0.6f + Vector3(0, 0.25f, 0)).normalized();
					l.length = _rand(0.025f, 0.05f) * (1.0f - 0.3f * t);
					l.width = l.length * 0.42f;
					l.twist = _rand(-0.3f, 0.3f);
					_add_leaf(SURFACE_LEAF, Transform3D(sm_frame(d, Vector3(0, 1, 0)), at.origin), l, 0.3f);
				}
			}
		}
	}
	plant_height = MAX(plant_height, 0.15f);
}

// Sea aster (Tripolium pannonicum): a fleshy, hairless plant 15 to 60 cm; narrow, thick, slightly
// glaucous leaves 5 to 12 cm, the lower ones in a loose rosette; a branched, flat-topped head of
// daisies 10 to 20 mm across, pale lilac rays round a yellow disc (rayless forms common), July to
// October; the heads go to brown pappus.
void PlantGenerator::_build_sea_aster() {
	leaf_wear = 0.15f;
	Leaf l;
	l.shape = LEAF_LANCEOLATE;
	l.fold = 0.35f; // Fleshy, keeled.
	l.bend = 0.4f;
	l.segments = 6;
	const int rosette = int(_rand(5.0f, 9.0f));
	const float spin = _rand(0.0f, Math::TAU);
	for (int k = 0; k < rosette; k++) {
		const Vector3 d = sm_dir(spin + k * SM_GOLDEN, _rand(0.35f, 0.8f));
		l.length = _rand(0.06f, 0.12f);
		l.width = l.length * 0.14f;
		l.twist = _rand(-0.3f, 0.3f);
		_add_leaf(SURFACE_LEAF, Transform3D(sm_frame(d, Vector3(0, 1, 0)), Vector3()), l, 0.0f);
	}
	float progress = 0.0f, dead = 0.0f;
	const Bloom bloom = _bloom(false, &progress, &dead);
	const int stems = int(_rand(1.0f, 3.99f));
	Leaf ray;
	ray.shape = LEAF_LINEAR;
	ray.fold = -0.05f;
	ray.bend = -0.2f;
	ray.segments = 3;
	for (int s = 0; s < stems; s++) {
		const float h = _rand(0.15f, 0.5f);
		const Vector3 up = (sm_dir(spin + s * 2.2f, 0.0f) * _rand(0.05f, 0.2f) + Vector3(0, 1, 0)).normalized();
		const LocalVector<Transform3D> frames = _stem_frames(Transform3D(sm_frame(up, Vector3(1, 0, 0)), Vector3()), h, _rand(0.0f, 0.15f), 6);
		_add_stem_along(frames, 0.0028f, 0.0015f, 0.0f);
		// Stem leaves, smaller up the stem.
		for (int k = 0; k < 6; k++) {
			const float t = Math::lerp(0.1f, 0.7f, k / 5.0f);
			const Transform3D at = _frame_at(frames, t);
			const Vector3 d = (sm_dir(spin + k * SM_GOLDEN, 0.0f) * 0.8f + at.basis.get_column(2)).normalized();
			l.length = _rand(0.04f, 0.08f) * (1.0f - 0.6f * t);
			l.width = l.length * 0.12f;
			_add_leaf(SURFACE_LEAF, Transform3D(sm_frame(d, Vector3(0, 1, 0)), at.origin), l, 0.4f);
		}
		if (bloom == BLOOM_NONE) {
			continue;
		}
		const Wither w(this, dead);
		// A loose, flat-topped corymb: branches from the top third, each ending in a head.
		const int heads = int(_rand(8.0f, 18.0f));
		for (int k = 0; k < heads; k++) {
			const Transform3D at = _frame_at(frames, _rand(0.7f, 0.95f));
			const Vector3 d = (sm_dir(k * SM_GOLDEN, 0.0f) * _rand(0.4f, 0.9f) + Vector3(0, 1, 0)).normalized();
			const float blen = _rand(0.02f, 0.07f);
			const Transform3D top = _add_stem(Transform3D(sm_frame(d, Vector3(0, 1, 0)), at.origin), blen, 0.0012f, 0.0009f, -0.1f, 3, 0.5f);
			const Vector3 face = (top.basis.get_column(2) * 0.4f + Vector3(0, 1, 0)).normalized();
			const Basis fb = sm_frame(face, Vector3(1, 0, 0));
			if (bloom == BLOOM_SEED) {
				_add_head(SURFACE_CENTRE, Transform3D(fb, top.origin), 0.004f, 0.006f, Color(0.75f, 0.68f, 0.55f), 3, 8); // Brown pappus.
				continue;
			}
			_add_head(SURFACE_CENTRE, Transform3D(fb, top.origin), 0.0042f, 0.0025f, Color(1, 1, 1), 2, 8);
			if (_rand(0.0f, 1.0f) < 0.25f) {
				continue; // Rayless form.
			}
			const int rays = int(_rand(12.0f, 18.0f));
			const float rl = _rand(0.0075f, 0.0105f); // Heads 15 to 20 mm across.
			for (int r = 0; r < rays; r++) {
				const float ra = r * Math::TAU / rays;
				const Vector3 rd = (fb.get_column(0) * Math::cos(ra) + fb.get_column(1) * Math::sin(ra) + face * 0.05f).normalized();
				ray.length = rl;
				ray.width = 0.0022f;
				_add_leaf(SURFACE_PETAL, Transform3D(sm_frame(rd, face), top.origin), ray, 1.0f);
			}
		}
	}
	plant_height = MAX(plant_height, 0.08f);
}

// Common sea lavender (Limonium vulgare): a rosette of leathery, elliptic leaves 5 to 15 cm on
// long stalks, with a prominent midrib; stiff, leafless flower stems 15 to 40 cm branching above
// into a flat-topped spray of one-sided spikes of small lavender-purple flowers, July to
// September; the papery calyces stand on through winter.
void PlantGenerator::_build_sea_lavender() {
	leaf_wear = 0.2f;
	Leaf l;
	l.shape = LEAF_OVATE;
	l.petiole = 0.35f;
	l.fold = 0.15f;
	l.bend = 0.3f;
	l.segments = 6;
	const int leaves = int(_rand(6.0f, 12.0f));
	const float spin = _rand(0.0f, Math::TAU);
	for (int k = 0; k < leaves; k++) {
		const Vector3 d = sm_dir(spin + k * SM_GOLDEN, _rand(0.15f, 0.55f));
		l.length = _rand(0.06f, 0.14f);
		l.width = l.length * 0.32f;
		l.twist = _rand(-0.2f, 0.2f);
		_add_leaf(SURFACE_LEAF, Transform3D(sm_frame(d, Vector3(0, 1, 0)), Vector3()), l, 0.0f);
	}
	float progress = 0.0f, dead = 0.0f;
	const Bloom bloom = _bloom(false, &progress, &dead);
	if (bloom == BLOOM_NONE) {
		plant_height = MAX(plant_height, 0.04f);
		return;
	}
	const Wither w(this, bloom == BLOOM_SEED ? 0.6f : dead * 0.5f); // The calyces dry papery.
	const int stems = int(_rand(1.0f, 4.0f));
	for (int s = 0; s < stems; s++) {
		const float h = _rand(0.15f, 0.35f);
		const Vector3 up = (sm_dir(spin + s * 2.4f, 0.0f) * _rand(0.05f, 0.25f) + Vector3(0, 1, 0)).normalized();
		const LocalVector<Transform3D> frames = _stem_frames(Transform3D(sm_frame(up, Vector3(1, 0, 0)), Vector3()), h, _rand(-0.1f, 0.1f), 5);
		_add_stem_along(frames, 0.0016f, 0.0011f, 0.0f);
		// A corymb: primary branches from the upper part fork again, all turned out and up so the
		// spikes end near one level in a flat-topped spray 10 to 25 cm across.
		const float top_y = frames[frames.size() - 1].origin.y;
		const int branches = int(_rand(4.0f, 7.0f));
		for (int b = 0; b < branches; b++) {
			const float bt = Math::lerp(0.5f, 0.92f, float(b) / branches);
			const Transform3D at = _frame_at(frames, bt);
			const float reach = (top_y - at.origin.y) + _rand(0.01f, 0.03f); // Up to the spray's level.
			const Vector3 d = (sm_dir(b * SM_GOLDEN + s, 0.0f) * _rand(0.5f, 0.8f) + Vector3(0, 0.9f, 0)).normalized();
			const float bl = MAX(reach * 1.1f, 0.03f);
			const LocalVector<Transform3D> br = _stem_frames(Transform3D(sm_frame(d, Vector3(0, 1, 0)), at.origin), bl, -0.2f, 4);
			_add_stem_along(br, 0.0011f, 0.0007f, 0.6f);
			const int forks = int(_rand(3.0f, 6.0f));
			for (int f = 0; f < forks; f++) {
				const Transform3D fat = _frame_at(br, Math::lerp(0.45f, 1.0f, float(f) / MAX(forks - 1, 1)));
				const Vector3 fd = (sm_dir(b * 1.7f + f * 2.1f, 0.0f) * 0.9f + Vector3(0, 0.45f, 0)).normalized();
				const LocalVector<Transform3D> sub = _stem_frames(Transform3D(sm_frame(fd, Vector3(0, 1, 0)), fat.origin), _rand(0.015f, 0.035f), -0.1f, 3);
				_add_stem_along(sub, 0.0007f, 0.0005f, 0.8f);
				for (int q = 0; q < 2; q++) {
					const Transform3D qat = _frame_at(sub, q == 0 ? 0.5f : 1.0f);
					const Vector3 qd = (qat.basis.get_column(2) * 0.7f + sm_dir(f * 2.3f + q * 1.9f, 0.0f) * 0.5f + Vector3(0, 0.2f, 0)).normalized();
					const LocalVector<Transform3D> spike = _stem_frames(Transform3D(sm_frame(qd, Vector3(0, 1, 0)), qat.origin), _rand(0.015f, 0.03f), 0.25f, 3);
					_add_stem_along(spike, 0.0006f, 0.0004f, 1.0f);
					// Flowers crowded in two rows on the spike's upper side: funnels 5 to 6 mm.
					const int fl = int(_rand(7.0f, 12.0f));
					for (int k = 0; k < fl; k++) {
						const Transform3D sat = _frame_at(spike, (k + 0.5f) / fl);
						const Vector3 n = (Vector3(0, 1, 0) + sat.basis.get_column(0) * ((k & 1) ? 0.35f : -0.35f)).normalized();
						_add_head(SURFACE_PETAL, Transform3D(sm_frame(n, sat.basis.get_column(2)), sat.origin + n * 0.001f), 0.0026f, 0.0025f, Color(1, 1, 1), 2, 6);
					}
				}
			}
		}
	}
	plant_height = MAX(plant_height, 0.15f);
}

// Sea kale (Crambe maritima): a low mound 30 to 75 cm across of a few huge leaves 15 to 40 cm,
// thick, waxy and glaucous blue-grey, ovate with wavy, crinkled, shallowly lobed margins and a
// broad pale midrib, on stout stalks; in June dense, branched, flat-topped heads of white,
// honey-scented four-petalled flowers 10 to 16 mm, then round pea-sized pods.
void PlantGenerator::_build_sea_kale() {
	leaf_wear = 0.12f;
	Leaf l;
	l.shape = LEAF_OVATE;
	l.lobes = 5;
	l.lobe_depth = 0.18f;
	l.round_lobes = true;
	l.petiole = 0.25f;
	l.segments = 10;
	const int leaves = int(_rand(6.0f, 12.0f));
	const float spin = _rand(0.0f, Math::TAU);
	for (int k = 0; k < leaves; k++) {
		const float a = spin + k * SM_GOLDEN;
		const bool inner = k >= leaves * 0.6f;
		const Vector3 d = sm_dir(a, inner ? _rand(0.7f, 1.0f) : _rand(0.25f, 0.55f));
		l.length = _rand(0.15f, 0.35f) * (inner ? 0.75f : 1.0f);
		l.width = l.length * _rand(0.6f, 0.75f);
		l.fold = _rand(0.25f, 0.45f); // Cupped, with the margins waved.
		l.bend = _rand(0.4f, 0.9f);
		l.twist = _rand(-0.6f, 0.6f); // The crinkle.
		const Wither w(this, (k == 0 && _rand(0.0f, 1.0f) < 0.4f) ? _rand(0.15f, 0.35f) : 0.0f); // Now and then the oldest yellowing.
		_add_leaf(SURFACE_LEAF, Transform3D(sm_frame(d, Vector3(0, 1, 0)), Vector3(Math::cos(a), 0, Math::sin(a)) * 0.01f), l, 0.0f);
	}
	float progress = 0.0f, dead = 0.0f;
	const Bloom bloom = _bloom(false, &progress, &dead);
	plant_height = MAX(plant_height, 0.15f);
	if (bloom == BLOOM_NONE) {
		return;
	}
	const Wither w(this, dead * 0.6f); // Stems straw-coloured as the pods ripen.
	const int stems = int(_rand(1.0f, 3.99f));
	Leaf p;
	p.shape = LEAF_ROUND;
	p.fold = -0.1f;
	p.bend = -0.2f;
	p.segments = 3;
	for (int s = 0; s < stems; s++) {
		const Vector3 up = (sm_dir(spin + s * 2.1f, 0.0f) * 0.3f + Vector3(0, 1, 0)).normalized();
		// A dense, much-branched, domed head 20 to 40 cm across: branches from the upper stem, each
		// with short pedicels of flowers (or, after, round pods), all ending near one dome.
		const LocalVector<Transform3D> axis = _stem_frames(Transform3D(sm_frame(up, Vector3(1, 0, 0)), Vector3()), _rand(0.15f, 0.25f), 0.05f, 5);
		const int branches = int(_rand(8.0f, 14.0f));
		for (int b = 0; b < branches; b++) {
			const float bt = Math::lerp(0.4f, 1.0f, float(b) / branches);
			const Transform3D at = _frame_at(axis, bt);
			const Vector3 d = (sm_dir(spin + b * SM_GOLDEN, 0.0f) * _rand(0.8f, 1.0f) + Vector3(0, 0.25f + 0.45f * bt, 0)).normalized();
			const LocalVector<Transform3D> br = _stem_frames(Transform3D(sm_frame(d, Vector3(0, 1, 0)), at.origin), _rand(0.08f, 0.16f) * (1.3f - 0.6f * bt), -0.35f, 4); // Spreading, the lower ones longer: a dome.
			_add_stem_along(br, 0.0025f, 0.0012f, 0.4f);
			const int fl = int(_rand(6.0f, 11.0f));
			for (int k = 0; k < fl; k++) {
				const Transform3D fat = _frame_at(br, Math::lerp(0.35f, 1.0f, (k + 0.5f) / fl));
				const float fa = b * 1.3f + k * SM_GOLDEN;
				const Vector3 pd = (sm_dir(fa, 0.0f) * 0.6f + Vector3(0, 1, 0)).normalized();
				const Transform3D tip = _add_stem(Transform3D(sm_frame(pd, Vector3(0, 1, 0)), fat.origin), _rand(0.01f, 0.02f), 0.0009f, 0.0007f, 0.0f, 2, 0.9f);
				if (bloom == BLOOM_SEED || _rand(0.0f, 1.0f) < progress * 0.7f) { // The lower flowers set first.
					// The pods: pea-sized, round, green ripening to pale straw.
					const Wither wp(this, dead * 0.5f);
					_add_head(SURFACE_CENTRE, Transform3D(Basis(), tip.origin + Vector3(0, 0.004f, 0)), 0.0045f, 0.009f, Color(0.95f, 1.0f, 0.85f), 3, 8);
					continue;
				}
				const float sz = _rand(0.005f, 0.007f);
				p.length = sz;
				p.width = sz * 0.85f;
				for (int q = 0; q < 4; q++) {
					const float pa = fa + q * Math::PI * 0.5f;
					const Vector3 pdir = (Vector3(Math::cos(pa), 0.35f, Math::sin(pa))).normalized();
					_add_leaf(SURFACE_PETAL, Transform3D(sm_frame(pdir, Vector3(0, 1, 0)), tip.origin), p, 1.0f);
				}
				_add_head(SURFACE_CENTRE, Transform3D(Basis(), tip.origin), 0.0015f, 0.002f, Color(1, 1, 1), 2, 5);
			}
		}
		const Transform3D top = axis[axis.size() - 1];
		_add_stem_along(axis, 0.006f, 0.0035f, 0.0f);
		plant_height = MAX(plant_height, top.origin.y);
	}
}

// Sea campion (Silene uniflora): a cushion or trailing mat of weak stems 8 to 20 cm, with
// opposite, fleshy, glaucous, lance-shaped leaves 1 to 2 cm; white flowers 20 to 25 mm, the five
// petals deeply notched, from a swollen, pink-veined bladder calyx, May to August.
void PlantGenerator::_build_sea_campion() {
	leaf_wear = 0.1f;
	const int shoots = int(_rand(10.0f, 20.0f));
	const float spin = _rand(0.0f, Math::TAU);
	Leaf l;
	l.shape = LEAF_LANCEOLATE;
	l.fold = 0.35f;
	l.bend = 0.15f;
	l.segments = 3;
	Leaf p;
	p.shape = LEAF_SPATULATE;
	p.lobes = 1;
	p.lobe_depth = 0.45f; // The deep notch.
	p.round_lobes = true;
	p.fold = -0.05f;
	p.bend = -0.25f;
	p.segments = 3;
	float progress = 0.0f, dead = 0.0f;
	const Bloom bloom = _bloom(false, &progress, &dead);
	for (int s = 0; s < shoots; s++) {
		const float az = spin + s * SM_GOLDEN;
		const float len = _rand(0.06f, 0.16f);
		const LocalVector<Transform3D> frames = _stem_frames(Transform3D(sm_frame(sm_dir(az, _rand(0.05f, 0.35f)), Vector3(0, 1, 0)), Vector3()), len, -0.35f, 5);
		_add_stem_along(frames, 0.0011f, 0.0008f, 0.0f);
		const int pairs = int(len / 0.012f);
		for (int k = 0; k < pairs; k++) {
			const Transform3D at = _frame_at(frames, (k + 0.6f) / pairs * 0.9f);
			const float rot = (k % 2) * Math::PI * 0.5f;
			for (int side = 0; side < 2; side++) {
				const float a = rot + side * Math::PI;
				const Vector3 out = at.basis.get_column(0) * Math::cos(a) + at.basis.get_column(1) * Math::sin(a);
				const Vector3 d = (out + at.basis.get_column(2) * 0.8f).normalized();
				l.length = _rand(0.012f, 0.022f);
				l.width = l.length * 0.4f;
				_add_leaf(SURFACE_LEAF, Transform3D(sm_frame(d, Vector3(0, 1, 0)), at.origin), l, 0.3f);
			}
		}
		if (bloom == BLOOM_NONE || _rand(0.0f, 1.0f) < 0.45f) {
			continue;
		}
		const Wither w(this, dead);
		// Nodding a little at the shoot's raised tip.
		const Transform3D tip = frames[frames.size() - 1];
		const Vector3 face = (tip.basis.get_column(2) * 0.6f + Vector3(0, 0.8f, 0)).normalized();
		const Basis fb = sm_frame(face, Vector3(0, 1, 0));
		const Vector3 calyx_at = tip.origin + face * 0.006f;
		_add_head(SURFACE_CENTRE, Transform3D(fb, tip.origin), 0.0045f, 0.012f, Color(1, 1, 1), 4, 10); // The bladder.
		if (bloom == BLOOM_SEED) {
			continue;
		}
		const float sz = _rand(0.008f, 0.011f);
		p.length = sz;
		p.width = sz * 0.75f;
		const float fs = _rand(0.0f, Math::TAU);
		for (int k = 0; k < 5; k++) {
			const float pa = fs + k * Math::TAU / 5.0f;
			const Vector3 d = (fb.get_column(0) * Math::cos(pa) + fb.get_column(1) * Math::sin(pa) + face * 0.2f).normalized();
			_add_leaf(SURFACE_PETAL, Transform3D(sm_frame(d, face), calyx_at + face * 0.006f), p, 1.0f);
		}
	}
	plant_height = MAX(plant_height, 0.04f);
}

// Yellow horned-poppy (Glaucium flavum): a rosette of grey-blue, rough-hairy leaves 15 to 30 cm,
// deeply lobed and crinkled; branched, glaucous flower stems 30 to 90 cm with clasping leaves;
// yellow, four-petalled poppies 6 to 9 cm across, June to September, and the long, slender, curved
// seed pods 15 to 30 cm that give it its name.
void PlantGenerator::_build_horned_poppy() {
	leaf_wear = 0.2f;
	Leaf l;
	l.shape = LEAF_OVATE;
	l.lobes = 4;
	l.lobe_depth = 0.55f;
	l.round_lobes = true;
	l.petiole = 0.1f;
	l.segments = 8;
	const int leaves = int(_rand(8.0f, 14.0f));
	const float spin = _rand(0.0f, Math::TAU);
	for (int k = 0; k < leaves; k++) {
		const Vector3 d = sm_dir(spin + k * SM_GOLDEN, _rand(0.15f, 0.5f));
		l.length = _rand(0.12f, 0.26f);
		l.width = l.length * 0.42f;
		l.fold = _rand(0.2f, 0.4f);
		l.bend = _rand(0.4f, 0.8f);
		l.twist = _rand(-0.5f, 0.5f);
		_add_leaf(SURFACE_LEAF, Transform3D(sm_frame(d, Vector3(0, 1, 0)), Vector3()), l, 0.0f);
	}
	float progress = 0.0f, dead = 0.0f;
	const Bloom bloom = _bloom(false, &progress, &dead);
	plant_height = MAX(plant_height, 0.1f);
	if (bloom == BLOOM_NONE) {
		return;
	}
	const int stems = int(_rand(1.0f, 3.99f));
	Leaf p;
	p.shape = LEAF_ROUND;
	p.fold = 0.2f; // Cupped, crumpled silk.
	p.bend = -0.3f;
	p.twist = 0.2f;
	p.segments = 4;
	for (int s = 0; s < stems; s++) {
		const float h = _rand(0.3f, 0.7f);
		const Vector3 up = (sm_dir(spin + s * 2.3f, 0.0f) * _rand(0.1f, 0.3f) + Vector3(0, 1, 0)).normalized();
		const LocalVector<Transform3D> frames = _stem_frames(Transform3D(sm_frame(up, Vector3(1, 0, 0)), Vector3()), h, _rand(-0.2f, 0.2f), 7);
		_add_stem_along(frames, 0.004f, 0.0025f, 0.0f);
		const int branches = int(_rand(2.0f, 5.0f));
		for (int b = 0; b <= branches; b++) {
			Transform3D tip;
			if (b == branches) {
				tip = frames[frames.size() - 1];
			} else {
				const Transform3D at = _frame_at(frames, Math::lerp(0.4f, 0.9f, float(b) / branches));
				// A clasping leaf under each branch.
				l.length = _rand(0.04f, 0.07f);
				l.width = l.length * 0.6f;
				l.lobe_depth = 0.3f;
				const Vector3 ld = (sm_dir(b * SM_GOLDEN, 0.0f) + at.basis.get_column(2) * 0.4f).normalized();
				_add_leaf(SURFACE_LEAF, Transform3D(sm_frame(ld, Vector3(0, 1, 0)), at.origin), l, 0.4f);
				const Vector3 d = (sm_dir(b * SM_GOLDEN + 0.4f, 0.0f) * 0.7f + Vector3(0, 1, 0)).normalized();
				tip = _add_stem(Transform3D(sm_frame(d, Vector3(0, 1, 0)), at.origin), _rand(0.06f, 0.15f), 0.0025f, 0.0018f, -0.1f, 4, 0.5f);
			}
			// Old flowers have become pods; the newest is open.
			const bool pod = bloom == BLOOM_SEED || (b < branches - 1 && _rand(0.0f, 1.0f) < progress);
			if (pod) {
				const Wither wp(this, dead * 0.7f);
				const Vector3 pd = (tip.basis.get_column(2) + Vector3(0, 0.3f, 0)).normalized();
				(void)_add_stem(Transform3D(sm_frame(pd, Vector3(0, 1, 0)), tip.origin), _rand(0.15f, 0.28f), 0.0028f, 0.0012f, _rand(0.4f, 0.9f), 8, 0.8f, SURFACE_CENTRE);
				continue;
			}
			const Vector3 face = (tip.basis.get_column(2) * 0.4f + Vector3(0, 1, 0)).normalized();
			const Basis fb = sm_frame(face, Vector3(1, 0, 0));
			const float sz = _rand(0.03f, 0.042f);
			p.length = sz;
			p.width = sz * 1.0f;
			const float fs = _rand(0.0f, Math::TAU);
			for (int k = 0; k < 4; k++) {
				const float pa = fs + k * Math::PI * 0.5f;
				const Vector3 d = (fb.get_column(0) * Math::cos(pa) + fb.get_column(1) * Math::sin(pa) + face * 0.6f).normalized();
				_add_leaf(SURFACE_PETAL, Transform3D(sm_frame(d, face), tip.origin), p, 1.0f);
			}
			_add_head(SURFACE_CENTRE, Transform3D(fb, tip.origin), 0.006f, 0.008f, Color(1, 1, 1), 3, 10); // The stamens round the young pod.
		}
		plant_height = MAX(plant_height, frames[frames.size() - 1].origin.y);
	}
}

// The marsh zoned by height in the tidal frame, low to high, and the shingle backshore above the
// tides (docs/ideas/vegetation.md, "Salt marsh and shingle plants"). Heights for a mesotidal coast
// as metres above mean sea level (Adam 1990; Rodwell 2000; Silvestri, Defina and Marani 2005):
// lower limits set by flooding, upper ones by competition (Bertness and Ellison 1987).
TypedArray<Resource> PlantGenerator::make_saltmarsh(SaltmarshStand p_stand, int p_variants, bool p_textures, const Dictionary &p_season) {
	using P = PlantGenerator;
	struct MarshSpec {
		Preset preset;
		float spacing, footprint, density, scale_min, scale_max;
		float patch_scale; // 0: no patches.
		float patch_cover;
		float visibility;
		float align;
	};
	LocalVector<MarshSpec> set;
	Vector2 tide;
	float tide_fade = 0.15f;
	switch (p_stand) {
		case SALTMARSH_PIONEER:
			// Bare mud about mean high water neaps: glasswort scattered, cord-grass in clonal clumps.
			set = LocalVector<MarshSpec>({ { P::PRESET_GLASSWORT, 0.12f, 0.02f, 0.55f, 0.7f, 1.3f, 3.0f, 0.55f, 25.0f, 0.6f },
					{ P::PRESET_CORD_GRASS, 0.35f, 0.08f, 0.6f, 0.7f, 1.3f, 5.0f, 0.4f, 40.0f, 0.3f } });
			tide = Vector2(0.0f, 0.7f);
			break;
		case SALTMARSH_MID:
			// Low to mid marsh: purslane bands, aster and lavender in a saltmarsh-grass sward.
			set = LocalVector<MarshSpec>({ { P::PRESET_SEA_PURSLANE, 0.3f, 0.1f, 0.6f, 0.8f, 1.4f, 4.0f, 0.45f, 35.0f, 0.6f },
					{ P::PRESET_SEA_ASTER, 0.35f, 0.05f, 0.45f, 0.7f, 1.2f, 3.0f, 0.5f, 30.0f, 0.5f },
					{ P::PRESET_SEA_LAVENDER, 0.3f, 0.06f, 0.5f, 0.8f, 1.2f, 4.0f, 0.5f, 30.0f, 0.6f },
					{ P::PRESET_DRY_GRASS, 0.12f, 0.02f, 0.6f, 0.5f, 0.8f, 0.0f, 0.0f, 30.0f, 0.5f } });
			tide = Vector2(0.5f, 1.3f);
			break;
		case SALTMARSH_HIGH:
			// High marsh: thrift cushions, sea lavender, sea rush clumps in a fine fescue sward.
			set = LocalVector<MarshSpec>({ { P::PRESET_DRY_GRASS, 0.1f, 0.02f, 0.75f, 0.5f, 0.8f, 0.0f, 0.0f, 30.0f, 0.5f },
					{ P::PRESET_THRIFT, 0.3f, 0.05f, 0.5f, 0.8f, 1.3f, 3.0f, 0.5f, 30.0f, 0.8f },
					{ P::PRESET_SEA_LAVENDER, 0.4f, 0.06f, 0.4f, 0.8f, 1.2f, 4.0f, 0.4f, 30.0f, 0.6f },
					{ P::PRESET_RUSH, 0.6f, 0.08f, 0.35f, 0.7f, 1.1f, 6.0f, 0.35f, 35.0f, 0.3f },
					{ P::PRESET_SEA_ASTER, 0.6f, 0.05f, 0.25f, 0.6f, 1.0f, 0.0f, 0.0f, 30.0f, 0.5f } });
			tide = Vector2(1.1f, 1.9f);
			tide_fade = 0.2f;
			break;
		case SALTMARSH_SHINGLE:
			// The backshore above the highest tides: sea kale mounds far apart, campion mats, horned-poppy.
			set = LocalVector<MarshSpec>({ { P::PRESET_SEA_KALE, 1.6f, 0.3f, 0.45f, 0.8f, 1.3f, 0.0f, 0.0f, 50.0f, 0.4f },
					{ P::PRESET_SEA_CAMPION, 0.6f, 0.08f, 0.4f, 0.8f, 1.3f, 5.0f, 0.4f, 30.0f, 0.8f },
					{ P::PRESET_HORNED_POPPY, 1.2f, 0.15f, 0.4f, 0.8f, 1.2f, 0.0f, 0.0f, 40.0f, 0.4f },
					{ P::PRESET_THRIFT, 0.8f, 0.05f, 0.3f, 0.8f, 1.2f, 0.0f, 0.0f, 30.0f, 0.8f } });
			tide = Vector2(1.8f, 4.0f);
			tide_fade = 0.3f;
			break;
		case SALTMARSH_MAX:
			break;
	}
	TypedArray<Resource> out;
	int seed_base = 9900 + int(p_stand) * 100;
	for (const MarshSpec &f : set) {
		Ref<PlantGenerator> gen;
		gen.instantiate();
		gen->set_textures(p_textures);
		gen->set_preset(f.preset);
		gen->apply_preset_colors();
		if (f.preset == P::PRESET_DRY_GRASS) {
			// Common saltmarsh-grass (Puccinellia maritima) and red fescue: a fine, grey-green sward.
			gen->set_leaf_color(Color(0.26f, 0.34f, 0.2f));
		} else if (f.preset == P::PRESET_RUSH) {
			// Sea rush (Juncus maritimus): stiff, pale grey-green.
			gen->set_leaf_color(Color(0.3f, 0.36f, 0.24f));
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
		sp->set_rule_range(PlantSpecies::RULE_TIDE_HEIGHT, tide);
		sp->set_rule_fade(PlantSpecies::RULE_TIDE_HEIGHT, tide_fade);
		if (p_stand == SALTMARSH_SHINGLE) {
			// On the beach's backshore (the beach mask fading inland), up to steep storm ridges.
			sp->set_rule_range(PlantSpecies::RULE_BEACH, Vector2(0.15f, 1.0f));
			sp->set_rule_fade(PlantSpecies::RULE_BEACH, 0.1f);
			sp->set_rule_range(PlantSpecies::RULE_SLOPE, Vector2(0.0f, 15.0f));
			sp->set_rule_fade(PlantSpecies::RULE_SLOPE, 4.0f);
		} else {
			// Marshes form only on sheltered, low-energy coasts, on nearly level mud, off the beaches.
			sp->set_rule_range(PlantSpecies::RULE_COAST_EXPOSURE, Vector2(0.0f, 0.3f));
			sp->set_rule_fade(PlantSpecies::RULE_COAST_EXPOSURE, 0.1f);
			sp->set_rule_range(PlantSpecies::RULE_BEACH, Vector2(0.0f, 0.3f));
			sp->set_rule_fade(PlantSpecies::RULE_BEACH, 0.1f);
			sp->set_rule_range(PlantSpecies::RULE_SLOPE, Vector2(0.0f, 3.0f));
			sp->set_rule_fade(PlantSpecies::RULE_SLOPE, 1.5f);
		}
		if (f.patch_scale > 0.0f) {
			sp->set_patch_scale(f.patch_scale);
			sp->set_patch_cover(f.patch_cover);
			sp->set_patch_seed(seed_base);
		}
		sp->set_visibility_range(f.visibility);
		sp->set_meta("saltmarsh", true);
		tag_species(sp, f.preset);
		out.push_back(sp);
		seed_base += 13;
	}
	return out;
}

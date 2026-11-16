/**************************************************************************/
/*  plant_waterside.cpp                                                   */
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

// Waterside and wet-stone plants (docs/ideas/vegetation.md, "Waterside and wet-stone plants"):
// the tall herbs of stream banks and fen edges, the aquatic mosses on stream stones and the
// thallose liverworts of wet rock and waterfall spray. Measurements from Stace, "New Flora of the
// British Isles" (2019), and Atherton, Bosanquet and Lawley, "Mosses and Liverworts of Britain and
// Ireland" (2010).

#include "plant_generator.h"
#include "plant_species.h"

namespace {
const float WS_GOLDEN = 2.39996323f;

Basis ws_frame(const Vector3 &p_forward, const Vector3 &p_up_hint) {
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

Vector3 ws_dir(float p_azimuth, float p_elevation) {
	return Vector3(Math::cos(p_azimuth) * Math::cos(p_elevation), Math::sin(p_elevation), Math::sin(p_azimuth) * Math::cos(p_elevation));
}

Transform3D ws_base(float p_azimuth, float p_elevation, const Vector3 &p_at = Vector3()) {
	return Transform3D(ws_frame(ws_dir(p_azimuth, p_elevation), Vector3(0, 1, 0)), p_at);
}
} // namespace

void PlantGenerator::_add_disc_leaf(const Transform3D &p_at, float p_radius, float p_cleft, float p_droop, float p_wave, int p_teeth, float p_wind_base, int p_rings, int p_sectors, float p_saw) {
	Builder &b = builders[SURFACE_LEAF];
	_begin_organ(p_at.origin);
	const Basis f = p_at.basis.orthonormalized();
	// The disc's centre lies along the blade from where the stalk meets it in the sinus.
	const float sinus = 0.8f;
	const Vector3 centre = p_at.origin + f.get_column(2) * p_radius * sinus;
	const int first = b.verts.size();
	const float ph = _rand(0.0f, Math::TAU);
	const float wave_k = _rand(4.0f, 6.0f);
	const float a0 = -Math::PI * 0.5f + p_cleft; // The sinus points back, to -z.
	const float span = Math::TAU - 2.0f * p_cleft;
	for (int r = 0; r <= p_rings; r++) {
		const float u = float(r) / p_rings;
		for (int k = 0; k <= p_sectors; k++) {
			const float a = a0 + span * k / p_sectors;
			// Teeth: a saw round the margin, sharp points between rounded notches; deep, they are
			// spiny lobes (sea holly).
			const float saw = p_teeth > 0 ? 1.0f - p_saw * Math::abs(Math::sin(0.5f * p_teeth * a)) : 1.0f;
			const float rr = p_radius * u * saw * (1.0f + 0.04f * Math::sin(3.0f * a + ph));
			const float lx = Math::cos(a) * rr;
			const float lz = Math::sin(a) * rr;
			// The margin droops and waves; the middle is a shallow cup where the stalk holds it.
			const float ly = p_radius * (-p_droop * u * u + p_wave * u * u * u * Math::sin(wave_k * a + ph) + 0.04f * (1.0f - u) * (1.0f - u));
			b.verts.push_back(centre + f.get_column(0) * lx + f.get_column(1) * ly + f.get_column(2) * lz);
			// Inside the round leaf texture's outline (a disc of radius 0.5 about its middle), the
			// sinus towards its base, where the main veins start.
			b.uvs.push_back(Vector2(0.5f + 0.46f * lx / p_radius, 0.5f + 0.46f * lz / p_radius));
			b.uv2s.push_back(Vector2(MIN(1.0f, p_wind_base + (1.0f - p_wind_base) * u), 0.0f));
			const float ao = 0.8f + 0.2f * u;
			b.colors.push_back(Color(ao, ao, ao));
		}
	}
	for (int r = 0; r < p_rings; r++) {
		for (int k = 0; k < p_sectors; k++) {
			const int a = first + r * (p_sectors + 1) + k;
			const int c = a + p_sectors + 1;
			b.indices.append_array(PackedInt32Array({ a, c, a + 1, a + 1, c, c + 1 }));
		}
	}
	_tag_organ(b);
}

// Marsh marigold (Caltha palustris): a hairless clump 15 to 50 cm of long-stalked, kidney-shaped,
// glossy dark leaves 5 to 15 cm across with rounded teeth, and hollow branched stems of golden,
// cup-shaped flowers 2.5 to 5 cm across, five (to eight) shining sepals and many stamens, March to
// May; then a star of five to fifteen green follicles. Marshes, wet woods, stream and spring edges.
void PlantGenerator::_build_marsh_marigold() {
	// Leaves through the year: new ones from late winter, dying down in autumn; a few small
	// winter leaves on the crown.
	const FrondSeason fs = _frond_season(10.0f, 260.0f, 380.0f, 320.0f);
	const float grown = MAX(fs.unfurl, 0.3f);
	const int leaves = int(_rand(6.0f, 12.0f));
	const float spin = _rand(0.0f, Math::TAU);
	Leaf l;
	l.shape = LEAF_ROUND;
	l.lobes = 9;
	l.lobe_depth = 0.05f;
	l.round_lobes = true;
	l.fold = 0.18f;
	l.bend = 0.22f;
	l.segments = 8;
	{
		const Wither wither(this, fs.dead);
		for (int i = 0; i < leaves; i++) {
			const float u = float(i) / leaves; // Outer (older) leaves on longer, more arching stalks.
			const float az = spin + i * WS_GOLDEN;
			const float stalk = _rand(0.08f, 0.2f) * Math::lerp(1.0f, 0.55f, u) * grown;
			const Transform3D top = _add_stem(ws_base(az, Math::lerp(0.9f, 1.35f, u) + _rand(-0.1f, 0.1f)), stalk, 0.0028f, 0.0022f, _rand(0.25f, 0.6f), 5, 0.0f);
			// Kidney-shaped: wider than long, a deep, narrow sinus, the margin a little turned down.
			const float rad = _rand(0.03f, 0.05f) * Math::lerp(1.0f, 0.7f, u) * grown;
			const Vector3 d = (ws_dir(az, 0.0f) + Vector3(0, _rand(0.2f, 0.5f), 0)).normalized();
			_add_disc_leaf(Transform3D(ws_frame(d, Vector3(0, 1, 0)), top.origin), rad, 0.22f, 0.18f, 0.04f, 24, 0.6f, 4, 20);
		}
	}
	// Flowering stems: hollow, branching, each branch ending in a flower or, after, the follicles.
	const int stems = int(_rand(1.0f, 3.99f));
	for (int s = 0; s < stems; s++) {
		float dead = 0.0f;
		const Bloom bloom = _bloom(false, nullptr, &dead);
		if (bloom == BLOOM_NONE) {
			continue;
		}
		const Wither wither(this, dead);
		const float az = _rand(0.0f, Math::TAU);
		const Transform3D base = ws_base(az, _rand(1.05f, 1.35f));
		const float len = _rand(0.12f, 0.28f);
		const float bend = _rand(0.1f, 0.35f);
		const LocalVector<Transform3D> frames = _stem_frames(base, len, bend, 6);
		const Transform3D top = _add_stem(base, len, 0.003f, 0.0022f, bend, 6, 0.0f);
		LocalVector<Transform3D> tips;
		tips.push_back(top);
		const int branches = int(_rand(1.0f, 3.99f));
		for (int b = 0; b < branches; b++) {
			Transform3D at = _frame_at(frames, _rand(0.55f, 0.85f));
			const float a = _rand(0.0f, Math::TAU);
			const Vector3 radial = at.basis.get_column(0) * Math::cos(a) + at.basis.get_column(1) * Math::sin(a);
			at.basis = ws_frame(at.basis.get_column(2) + radial * 0.8f, Vector3(0, 1, 0));
			tips.push_back(_add_stem(at, _rand(0.04f, 0.09f), 0.0018f, 0.0015f, -0.25f, 3, 0.5f));
			// A stem leaf at the fork, smaller and short-stalked.
			Leaf sl = l;
			sl.length = _rand(0.025f, 0.04f);
			sl.width = sl.length * 1.15f;
			_add_leaf(SURFACE_LEAF, Transform3D(ws_frame((radial + Vector3(0, 0.4f, 0)).normalized(), Vector3(0, 1, 0)), at.origin), sl, 0.6f);
		}
		for (const Transform3D &t : tips) {
			// The flower faces up and out.
			const Vector3 face = (t.basis.get_column(2) + Vector3(0, 0.8f, 0)).normalized();
			if (bloom == BLOOM_FLOWER) {
				// A shallow, open cup of shining sepals (the cup flower's axis is its frame's z).
				_add_cup_flower(Transform3D(ws_frame(face, Vector3(1, 0, 0)), t.origin), _rand(0.016f, 0.022f));
			} else {
				// Follicles: a star of beaked green pods, spreading.
				const int n = int(_rand(5.0f, 10.0f));
				const Basis f = ws_frame(face, Vector3(1, 0, 0));
				for (int k = 0; k < n; k++) {
					const float a = k * Math::TAU / n;
					const Vector3 radial = f.get_column(0) * Math::cos(a) + f.get_column(1) * Math::sin(a);
					_add_stem(Transform3D(ws_frame((face + radial * 0.7f).normalized(), face), t.origin), 0.011f, 0.0022f, 0.0006f, -0.3f, 3, 0.8f);
				}
			}
		}
	}
}

// Meadowsweet (Filipendula ulmaria): stiff, often reddish stems 60 to 120 cm; pinnate leaves of
// two to five pairs of toothed leaflets 2 to 8 cm with tiny ones between, the end leaflet three- to
// five-lobed, white-downy beneath; frothy, irregular cymes of tiny cream flowers, June to September,
// then spirally twisted fruits; the dead stalks stand brown through winter. Tall-herb fen,
// stream and ditch banks, wet meadows (Rodwell, NVC M27).
void PlantGenerator::_build_meadowsweet() {
	const FrondSeason fs = _frond_season(150.0f, 350.0f, 420.0f, 280.0f);
	// A pinnate leaf along a frame: leaflets in pairs, small ones between, a lobed end leaflet.
	auto pinnate = [&](const Transform3D &p_base, float p_length, float p_droop, float p_wind) {
		const int segs = 5;
		const LocalVector<Transform3D> fr = _stem_frames(p_base, p_length, p_droop, segs);
		_add_stem(p_base, p_length, 0.0016f, 0.0008f, p_droop, segs, p_wind);
		const int pairs = int(_rand(2.0f, 4.99f));
		Leaf lf;
		lf.shape = LEAF_OVATE;
		lf.lobes = 6;
		lf.lobe_depth = 0.12f;
		lf.fold = 0.2f;
		lf.bend = 0.25f;
		lf.segments = 6;
		for (int k = 0; k < pairs; k++) {
			const float s = 0.3f + 0.6f * float(k) / MAX(pairs - 1, 1);
			const Transform3D at = _frame_at(fr, s);
			const float sz = p_length * Math::lerp(0.28f, 0.4f, s);
			for (int side = -1; side <= 1; side += 2) {
				lf.length = sz * _rand(0.9f, 1.1f);
				lf.width = lf.length * 0.45f;
				const Vector3 d = (at.basis.get_column(0) * float(side) + at.basis.get_column(2) * 0.55f).normalized();
				_add_leaf(SURFACE_LEAF, Transform3D(ws_frame(d, at.basis.get_column(1)), at.origin), lf, Math::lerp(p_wind, 1.0f, s));
			}
			if (k + 1 < pairs) {
				// The tiny leaflets between the pairs.
				const Transform3D mid = _frame_at(fr, s + 0.3f / MAX(pairs - 1, 1));
				Leaf tl = lf;
				tl.length = sz * 0.18f;
				tl.width = tl.length * 0.6f;
				tl.lobes = 2;
				tl.segments = 3;
				for (int side = -1; side <= 1; side += 2) {
					const Vector3 d = (mid.basis.get_column(0) * float(side) + mid.basis.get_column(2) * 0.4f).normalized();
					_add_leaf(SURFACE_LEAF, Transform3D(ws_frame(d, mid.basis.get_column(1)), mid.origin), tl, Math::lerp(p_wind, 1.0f, s));
				}
			}
		}
		Leaf end;
		end.shape = LEAF_PALMATE;
		end.lobes = 1;
		end.lobe_depth = 0.45f;
		end.length = p_length * 0.38f;
		end.width = end.length * 0.9f;
		end.fold = 0.2f;
		end.bend = 0.3f;
		end.segments = 6;
		const Transform3D tip = fr[fr.size() - 1];
		_add_leaf(SURFACE_LEAF, Transform3D(ws_frame(tip.basis.get_column(2), tip.basis.get_column(1)), tip.origin), end, 1.0f);
	};
	const float spin = _rand(0.0f, Math::TAU);
	if (fs.unfurl > 0.0f) {
		const Wither wither(this, fs.dead);
		const int basal = int(_rand(3.0f, 6.99f));
		for (int i = 0; i < basal; i++) {
			const float az = spin + i * WS_GOLDEN;
			pinnate(ws_base(az, _rand(0.75f, 1.15f), Vector3(0, 0.005f, 0)), _rand(0.25f, 0.4f) * fs.unfurl, _rand(0.3f, 0.6f), 0.0f);
		}
	}
	// Flowering stems: in flower, in fruit, or standing dead in winter.
	const int stems = int(_rand(1.0f, 3.99f));
	const Color fruit_tint(0.55f, 0.5f, 0.32f);
	for (int s = 0; s < stems; s++) {
		float dead = 0.0f;
		const Bloom bloom = _bloom(false, nullptr, &dead);
		if (bloom == BLOOM_NONE && fs.unfurl < 0.85f) {
			continue; // Before the stems bolt.
		}
		const Wither wither(this, bloom == BLOOM_NONE ? fs.dead : dead);
		const Transform3D base = ws_base(spin + s * 2.1f, _rand(1.4f, 1.53f));
		const float len = _rand(0.6f, 1.05f) * (bloom == BLOOM_NONE ? 0.75f : 1.0f);
		const float bend = _rand(-0.08f, 0.08f);
		const LocalVector<Transform3D> frames = _stem_frames(base, len, bend, 10);
		const Transform3D top = _add_stem(base, len, 0.0035f, 0.002f, bend, 10, 0.0f);
		// Stem leaves, alternate, smaller upwards.
		for (int k = 0; k < 4; k++) {
			const float t = 0.15f + 0.17f * k;
			const Transform3D at = _frame_at(frames, t);
			const float a = spin + k * WS_GOLDEN;
			const Vector3 radial = at.basis.get_column(0) * Math::cos(a) + at.basis.get_column(1) * Math::sin(a);
			pinnate(Transform3D(ws_frame((radial + at.basis.get_column(2) * 0.6f).normalized(), Vector3(0, 1, 0)), at.origin), _rand(0.13f, 0.2f) * (1.0f - 0.18f * k), 0.35f, t);
		}
		if (bloom == BLOOM_NONE) {
			continue; // Leafy stems not yet in bud.
		}
		// The cyme: unequal branches from the top, the lower ones longer, so the head is a
		// lopsided, frothy plate; each ends in clusters of florets.
		const Color tint = bloom == BLOOM_SEED ? fruit_tint : Color(1, 1, 1);
		const int rays = int(_rand(8.0f, 13.0f));
		for (int r = 0; r < rays; r++) {
			const float t = Math::lerp(0.88f, 1.0f, float(r) / rays);
			const Transform3D at = r == rays - 1 ? top : _frame_at(frames, t);
			const float a = spin + r * WS_GOLDEN;
			const Vector3 radial = at.basis.get_column(0) * Math::cos(a) + at.basis.get_column(1) * Math::sin(a);
			const Vector3 d = (at.basis.get_column(2) * 1.0f + radial * _rand(0.6f, 1.1f)).normalized();
			// Lower branches longer, so the head is a broad, uneven, flat-topped froth.
			const float rl = _rand(0.05f, 0.1f) * Math::lerp(1.0f, 0.45f, float(r) / rays);
			const LocalVector<Transform3D> rf = _stem_frames(Transform3D(ws_frame(d, Vector3(0, 1, 0)), at.origin), rl, 0.15f, 3);
			_add_stem(Transform3D(ws_frame(d, Vector3(0, 1, 0)), at.origin), rl, 0.0012f, 0.0008f, 0.15f, 3, 0.7f);
			const int clusters = int(_rand(3.0f, 4.99f));
			for (int c = 0; c < clusters; c++) {
				const Transform3D ct = _frame_at(rf, Math::lerp(0.5f, 1.0f, float(c) / MAX(clusters - 1, 1)));
				const int florets = int(_rand(10.0f, 16.0f));
				const float cr = _rand(0.012f, 0.018f);
				for (int f = 0; f < florets; f++) {
					const float u = Math::sqrt((f + 0.5f) / florets);
					const float fa = f * WS_GOLDEN + a;
					const Vector3 off = (ct.basis.get_column(0) * Math::cos(fa) + ct.basis.get_column(1) * Math::sin(fa)) * cr * u + Vector3(0, cr * 0.5f * (1.0f - u * u), 0);
					_add_head(SURFACE_PETAL, Transform3D(ws_frame(Vector3(0, 1, 0), Vector3(1, 0, 0)), ct.origin + off), _rand(0.0032f, 0.004f), 0.003f, tint, 2, 4);
				}
			}
		}
	}
}

// Butterbur (Petasites hybridus): dense clonal colonies on stream banks and gravel bars. Before
// the leaves, in March to May, stout pinkish spikes 10 to 40 cm of many small lilac-pink flower
// heads; then the huge leaves, round and heart-based, 30 to 90 cm across, grey-downy beneath, on
// grooved hollow stalks up to a metre, dying back in autumn. The female spikes lengthen to 60 cm in
// fruit, white with pappus.
void PlantGenerator::_build_butterbur() {
	const FrondSeason fs = _frond_season(250.0f, 450.0f, 380.0f, 300.0f);
	const float spin = _rand(0.0f, Math::TAU);
	if (fs.unfurl > 0.0f) {
		const Wither wither(this, fs.dead);
		const int leaves = int(_rand(3.0f, 6.99f));
		for (int i = 0; i < leaves; i++) {
			const float az = spin + i * WS_GOLDEN;
			const float u = float(i) / leaves;
			const float stalk = _rand(0.35f, 0.75f) * Math::lerp(1.0f, 0.7f, u) * (0.35f + 0.65f * fs.unfurl);
			const Transform3D top = _add_stem(ws_base(az, _rand(1.15f, 1.4f), Vector3(Math::cos(az), 0.0f, Math::sin(az)) * _rand(0.0f, 0.12f)), stalk, 0.008f, 0.005f, _rand(0.1f, 0.35f), 8, 0.0f);
			// The blade spreads level from the stalk's top, a little tilted to the light; its broad
			// margin droops and waves, young leaves still cupped.
			const float rad = _rand(0.15f, 0.26f) * (0.25f + 0.75f * fs.unfurl);
			const Vector3 d = (ws_dir(az, 0.0f) + Vector3(0, _rand(0.05f, 0.3f), 0)).normalized();
			_add_disc_leaf(Transform3D(ws_frame(d, Vector3(0, 1, 0)), top.origin), rad, 0.16f, Math::lerp(-0.1f, 0.3f, fs.unfurl), 0.07f, 40, 0.4f, 9, 44);
		}
	}
	// The spring spikes, a few per clump.
	const int spikes = int(_rand(1.0f, 4.99f));
	const Color seed_tint(1.25f, 1.25f, 1.2f);
	for (int s = 0; s < spikes; s++) {
		float progress = 0.0f, dead = 0.0f;
		const Bloom bloom = _bloom(false, &progress, &dead);
		if (bloom == BLOOM_NONE) {
			continue;
		}
		const Wither wither(this, dead);
		const bool fruit = bloom == BLOOM_SEED;
		const float az = _rand(0.0f, Math::TAU);
		const Transform3D base = ws_base(az, _rand(1.4f, 1.55f), Vector3(Math::cos(az), 0.0f, Math::sin(az)) * _rand(0.02f, 0.15f));
		const float len = fruit ? _rand(0.3f, 0.5f) : _rand(0.12f, 0.28f);
		const LocalVector<Transform3D> frames = _stem_frames(base, len, 0.05f, 6);
		_add_stem(base, len, 0.007f, 0.005f, 0.05f, 6, 0.0f);
		// Scale leaves up the stalk, pale and clasping.
		Leaf br;
		br.shape = LEAF_LANCEOLATE;
		br.fold = 0.5f;
		br.bend = -0.2f;
		br.segments = 3;
		for (int k = 0; k < 5; k++) {
			const Transform3D at = _frame_at(frames, 0.08f + 0.1f * k);
			const float a = k * WS_GOLDEN;
			const Vector3 radial = at.basis.get_column(0) * Math::cos(a) + at.basis.get_column(1) * Math::sin(a);
			br.length = _rand(0.025f, 0.04f);
			br.width = br.length * 0.35f;
			_add_leaf(SURFACE_STEM, Transform3D(ws_frame((at.basis.get_column(2) + radial * 0.3f).normalized(), radial), at.origin + radial * 0.005f), br, 0.0f);
		}
		// The raceme: an ovoid of heads on short stalks over the upper half.
		const int heads = int(_rand(25.0f, 45.0f));
		for (int h = 0; h < heads; h++) {
			const float t = Math::lerp(0.5f, 1.0f, (h + 0.5f) / heads);
			const Transform3D at = _frame_at(frames, t);
			const float girth = Math::sin(Math::PI * (t - 0.45f) / 0.6f);
			const float a = h * WS_GOLDEN;
			const Vector3 radial = at.basis.get_column(0) * Math::cos(a) + at.basis.get_column(1) * Math::sin(a);
			const float reach = (fruit ? 0.03f : 0.018f) * MAX(girth, 0.3f);
			const Vector3 c = at.origin + radial * reach + at.basis.get_column(2) * 0.004f;
			_add_stem(Transform3D(ws_frame(radial + at.basis.get_column(2) * 0.4f, at.basis.get_column(2)), at.origin), reach, 0.0012f, 0.001f, 0.0f, 1, 0.6f);
			if (fruit) {
				_add_head(SURFACE_PETAL, Transform3D(ws_frame(radial, at.basis.get_column(2)), c), 0.008f, 0.009f, seed_tint, 3, 7);
			} else {
				_add_head(SURFACE_PETAL, Transform3D(ws_frame(radial + at.basis.get_column(2), at.basis.get_column(2)), c), 0.0055f, 0.006f, Color(1, 1, 1), 3, 7);
			}
		}
	}
}

// Aquatic mosses of stream stones (Platyhypnidium riparioides, Fontinalis antipyretica): stiff,
// dark green to blackish shoots 5 to 15 cm, sparsely branched, trailing downstream in the current
// from where they hold to the rock. +X is downstream (PlantSpecies.follow_flow); they lie on the
// stone, so they do not move with the wind.
void PlantGenerator::_build_river_moss() {
	const int shoots = int(_rand(18.0f, 28.0f));
	leaf_wear = 0.0f;
	Leaf l;
	l.shape = LEAF_OVATE;
	l.fold = 0.35f;
	l.bend = 0.2f;
	l.segments = 2;
	const int from[SURFACE_MAX] = { builders[0].verts.size(), builders[1].verts.size(), builders[2].verts.size(), builders[3].verts.size() };
	auto shoot = [&](const Transform3D &p_base, float p_length) {
		const int segs = 6;
		const float droop = _rand(0.1f, 0.35f) * (rng.randf() < 0.5f ? 1.0f : -0.4f); // Laid on the stone by the current, some tips lifting.
		LocalVector<Transform3D> fr = _stem_frames(p_base, p_length, droop, segs);
		_add_stem(p_base, p_length, 0.0005f, 0.0003f, droop, segs, 0.0f);
		const int n = int(p_length / 0.0026f);
		for (int k = 0; k < n; k++) {
			const float s = (k + 0.5f) / n;
			const Transform3D at = _frame_at(fr, s);
			const float a = k * WS_GOLDEN;
			const Vector3 radial = at.basis.get_column(0) * Math::cos(a) + at.basis.get_column(1) * Math::sin(a);
			l.length = Math::lerp(0.0075f, 0.005f, s);
			l.width = l.length * 0.6f;
			// Leaves crowded along the stem, spreading at about 45 degrees and pressed forward by
			// the current, so a shoot reads 5 to 7 mm wide.
			_add_leaf(SURFACE_LEAF, Transform3D(ws_frame((at.basis.get_column(2) + radial).normalized(), radial), at.origin + radial * 0.0003f), l, 0.0f);
		}
		return fr;
	};
	for (int i = 0; i < shoots; i++) {
		// Held to the stone upstream of its middle, trailing over it, so the clump fits on a cobble's
		// top (its reach from the origin is what the placement fits to the cap).
		const Vector3 at(_rand(-0.035f, -0.01f), 0.002f, _rand(-0.025f, 0.025f));
		const float az = _rand(-0.4f, 0.4f) + at.z * 5.0f; // Fanning a little from the hold.
		const float len = _rand(0.035f, 0.08f);
		const LocalVector<Transform3D> fr = shoot(ws_base(az, _rand(0.06f, 0.2f), at), len);
		const int branches = int(_rand(0.0f, 2.99f));
		for (int b = 0; b < branches; b++) {
			const Transform3D bt = _frame_at(fr, _rand(0.3f, 0.7f));
			const float side = rng.randf() < 0.5f ? -1.0f : 1.0f;
			const Vector3 d = (bt.basis.get_column(2) * 1.5f + bt.basis.get_column(0) * side).normalized();
			shoot(Transform3D(ws_frame(d, Vector3(0, 1, 0)), bt.origin), len * _rand(0.3f, 0.55f));
		}
	}
	// Still: lying in the water on the stone, not in the air.
	for (int s = 0; s < SURFACE_MAX; s++) {
		Vector2 *w = builders[s].uv2s.ptrw();
		for (int i = from[s]; i < builders[s].uv2s.size(); i++) {
			w[i].x = 0.0f;
		}
	}
}

// Great scented liverwort (Conocephalum conicum, the snakeskin liverwort) and its kin on wet,
// shaded rock and waterfall spray: flat, glossy green ribbons (thalli) 1 to 2 cm wide, forking in
// two (dichotomous) every few centimetres, lying in overlapping mats on the rock; the surface a net
// of hexagonal air chambers, each with a pore.
void PlantGenerator::_build_liverwort() {
	// Each lobe a ribbon of three columns (the midline lower: the groove along the thallus), on
	// the plain centre surface (opaque, no backlight): no leaf outline or veins, the gloss and the
	// darker midline carry it.
	Builder &bd = builders[SURFACE_CENTRE];
	struct Tip {
		Vector3 at;
		float az;
		float width;
		int depth;
	};
	LocalVector<Tip> tips;
	const int starts = int(_rand(6.0f, 10.0f));
	const float spin = _rand(0.0f, Math::TAU);
	for (int i = 0; i < starts; i++) {
		const float az = spin + i * WS_GOLDEN;
		tips.push_back({ Vector3(Math::cos(az), 0.0f, Math::sin(az)) * _rand(0.0f, 0.025f), az, _rand(0.011f, 0.016f), 0 });
	}
	float lift = 0.0012f;
	const int rows = 5;
	for (uint32_t i = 0; i < tips.size() && i < 120; i++) {
		const Tip t = tips[i];
		const float len = _rand(0.025f, 0.04f);
		const float curve = _rand(-0.25f, 0.25f); // Radians over the lobe.
		const bool fork = t.depth < 3 && rng.randf() < 0.7f;
		const bool end = t.depth >= 3;
		lift += 0.00015f; // Later lobes lie over earlier ones.
		_begin_organ(t.at);
		const int first = bd.verts.size();
		const float shade = _rand(0.85f, 1.1f);
		Vector3 p = t.at;
		float az = t.az;
		for (int r = 0; r <= rows; r++) {
			const float u = float(r) / rows;
			const Vector3 fwd(Math::cos(az), 0.0f, Math::sin(az));
			const Vector3 side(-fwd.z, 0.0f, fwd.x);
			// Widening a little to the fork; a free end rounds off to a notched tip.
			float hw = 0.5f * t.width * (1.0f + 0.15f * u);
			float notch = 0.0f;
			if (end && u > 0.6f) {
				const float e = (u - 0.6f) / 0.4f;
				hw *= Math::sqrt(MAX(1.0f - e * e, 0.0f)) * 0.8f + 0.2f;
				notch = 0.004f * e * e;
			}
			const float up = lift + 0.002f * u * u * (end ? 1.0f : 0.3f); // Growing tips lift.
			const Vector3 row[3] = { p - side * hw + Vector3(0, up + 0.0005f, 0), p - fwd * notch + Vector3(0, up, 0), p + side * hw + Vector3(0, up + 0.0005f, 0) };
			for (int c = 0; c < 3; c++) {
				bd.verts.push_back(row[c]);
				bd.uvs.push_back(Vector2(c * 0.5f, u));
				bd.uv2s.push_back(Vector2(0.0f, 0.0f)); // Pressed to the rock: still in the wind.
				const float k = shade * (c == 1 ? 0.72f : 1.05f);
				bd.colors.push_back(Color(k, k, k));
			}
			if (r < rows) {
				az += curve / rows;
				p += Vector3(Math::cos(az), 0.0f, Math::sin(az)) * (len / rows);
			}
		}
		for (int r = 0; r < rows; r++) {
			const int r0 = first + r * 3;
			const int r1 = r0 + 3;
			for (int c = 0; c < 2; c++) {
				// Counter-clockwise from above, as every builder winds a surface's upper side.
				bd.indices.append_array(PackedInt32Array({ r0 + c, r1 + c, r0 + c + 1, r0 + c + 1, r1 + c, r1 + c + 1 }));
			}
		}
		_tag_organ(bd);
		if (end) {
			continue;
		}
		const Vector3 next = p - Vector3(Math::cos(az), 0.0f, Math::sin(az)) * 0.003f;
		if (fork) {
			const float spread = _rand(0.3f, 0.5f);
			tips.push_back({ next, az - spread, t.width * 0.9f, t.depth + 1 });
			tips.push_back({ next, az + spread, t.width * 0.9f, t.depth + 1 });
		} else {
			tips.push_back({ next, az + _rand(-0.15f, 0.15f), t.width, t.depth + 1 });
		}
	}
	plant_height = MAX(plant_height, 0.01f);
}

// The waterside sets (docs/ideas/vegetation.md, "Waterside and wet-stone plants"): bank herbs by
// their distance to the water, liverworts in waterfall spray. The stones' own flora (aquatic moss,
// liverworts on wet tops) is in make_stones(STONES_RIVER).
TypedArray<Resource> PlantGenerator::make_banks(BankStand p_stand, int p_variants, bool p_textures, const Dictionary &p_season) {
	using P = PlantGenerator;
	struct BankSpec {
		Preset preset;
		float spacing, footprint, density, scale_min, scale_max;
		Vector2 water; // Distance to the water (m, negative in it); fades over a third of its width.
		float patch_scale, patch_cover; // Clonal stands; 0: none.
		float visibility;
		float spray_min = 0.0f; // > 0: only where waterfall spray reaches this (RULE_SPRAY).
		float max_slope = 0.0f; // > 0: degrees.
		float align = 0.3f;
		float rock_foot = 0.0f;
	};
	LocalVector<BankSpec> set;
	switch (p_stand) {
		case BANKS_STREAM:
			// Marsh marigold at the water's very edge and at springs; butterbur colonies on the
			// gravel banks of lowland and valley streams; meadowsweet in the tall-herb band behind
			// (Rodwell, NVC M27 Filipendula-Angelica; Stace 2019).
			set = LocalVector<BankSpec>({ { P::PRESET_MARSH_MARIGOLD, 0.45f, 0.12f, 0.7f, 0.8f, 1.2f, Vector2(-0.15f, 1.2f), 4.0f, 0.55f, 35.0f, 0.0f, 25.0f, 0.4f, 1.0f },
					{ P::PRESET_BUTTERBUR, 0.6f, 0.25f, 1.0f, 0.8f, 1.2f, Vector2(0.0f, 4.0f), 9.0f, 0.45f, 70.0f, 0.0f, 22.0f, 0.2f },
					{ P::PRESET_MEADOWSWEET, 0.6f, 0.18f, 0.8f, 0.8f, 1.2f, Vector2(0.6f, 7.0f), 7.0f, 0.5f, 60.0f, 0.0f, 25.0f, 0.15f } });
			break;
		case BANKS_LAKE:
			// Still water: no current to sort gravel, so no butterbur; marsh marigold in the
			// wet margin, meadowsweet behind it.
			set = LocalVector<BankSpec>({ { P::PRESET_MARSH_MARIGOLD, 0.45f, 0.12f, 0.8f, 0.8f, 1.2f, Vector2(-0.1f, 1.5f), 5.0f, 0.6f, 35.0f, 0.0f, 15.0f, 0.4f },
					{ P::PRESET_MEADOWSWEET, 0.6f, 0.18f, 0.85f, 0.8f, 1.2f, Vector2(0.8f, 8.0f), 8.0f, 0.55f, 60.0f, 0.0f, 15.0f, 0.15f } });
			break;
		case BANKS_FALLS:
			// Spray zones: thallose liverworts and marsh marigold on the constantly wet ground and
			// ledges round the plunge (Ratcliffe 1968).
			set = LocalVector<BankSpec>({ { P::PRESET_LIVERWORT, 0.12f, 0.0f, 0.9f, 0.7f, 1.2f, Vector2(-0.05f, 15.0f), 1.5f, 0.6f, 18.0f, 0.3f, 0.0f, 1.0f, 2.0f },
					{ P::PRESET_MARSH_MARIGOLD, 0.45f, 0.12f, 0.6f, 0.8f, 1.15f, Vector2(-0.1f, 6.0f), 3.0f, 0.5f, 35.0f, 0.15f, 35.0f, 0.4f, 1.0f } });
			break;
		case BANKS_MAX:
			break;
	}
	TypedArray<Resource> out;
	int seed_base = 7000 + int(p_stand) * 100;
	for (const BankSpec &f : set) {
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
		sp->set_rock_foot(f.rock_foot);
		sp->set_rule_range(PlantSpecies::RULE_WATER_DISTANCE, f.water);
		sp->set_rule_fade(PlantSpecies::RULE_WATER_DISTANCE, MAX((f.water.y - f.water.x) * 0.3f, 0.1f));
		if (f.spray_min > 0.0f) {
			sp->set_rule_range(PlantSpecies::RULE_SPRAY, Vector2(f.spray_min, 1.0f));
			sp->set_rule_fade(PlantSpecies::RULE_SPRAY, f.spray_min * 0.5f);
		}
		if (f.max_slope > 0.0f) {
			sp->set_rule_range(PlantSpecies::RULE_SLOPE, Vector2(0.0f, f.max_slope));
			sp->set_rule_fade(PlantSpecies::RULE_SLOPE, 5.0f);
		}
		if (f.patch_scale > 0.0f) {
			sp->set_patch_scale(f.patch_scale);
			sp->set_patch_cover(f.patch_cover);
			sp->set_patch_seed(seed_base);
		}
		sp->set_visibility_range(f.visibility);
		sp->set_far_color(Color(0, 0, 0, 0)); // A narrow band along the water: the ground's colour carries it far.
		sp->set_meta("banks", true);
		out.push_back(sp);
		seed_base += 11;
	}
	return out;
}

/**************************************************************************/
/*  plant_burn.cpp                                                        */
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

// Burnt ground and regrowth (docs/ideas/vegetation.md, "Burnt ground and regrowth"): what a heath, grass or scrub fire
// leaves standing and what comes back, by the years since the burn (the season's "burn_age"; PlantSpecies.RULE_BURN_AGE
// places them). Gimingham 1972 and FEIS (Calluna vulgaris) for heather; FEIS (Chamerion angustifolium) for fireweed;
// Atherton, Bosanquet and Lawley 2010 and Field Bryology 129 (2023) for the fire mosses.

#include "plant_generator.h"
#include "plant_species.h"

namespace {
const float BURN_GOLDEN = 2.39996323f;

Basis burn_frame(const Vector3 &p_forward, const Vector3 &p_up_hint) {
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

Vector3 burn_dir(float p_azimuth, float p_elevation) {
	return Vector3(Math::cos(p_azimuth) * Math::cos(p_elevation), Math::sin(p_elevation), Math::sin(p_azimuth) * Math::cos(p_elevation));
}

Transform3D burn_base(float p_azimuth, float p_elevation, const Vector3 &p_at = Vector3()) {
	return Transform3D(burn_frame(burn_dir(p_azimuth, p_elevation), Vector3(0, 1, 0)), p_at);
}
} // namespace

// Years since the ground burnt, from the season (make_burn sets it per age band); without one, two years.
float PlantGenerator::_burn_age() const {
	return season.has("burn_age") ? MAX(float(season["burn_age"]), 0.0f) : 2.0f;
}

// Multiplies the vertex colours added to a surface since p_from (stems take no tint of their own).
void PlantGenerator::_tint_since(Surface p_surface, int p_from, const Color &p_tint) {
	PackedColorArray &c = builders[p_surface].colors;
	for (int k = p_from; k < c.size(); k++) {
		Color v = c[k];
		v.r *= p_tint.r;
		v.g *= p_tint.g;
		v.b *= p_tint.b;
		c.write[k] = v;
	}
}

// Rosebay willowherb, "fireweed" (Chamerion angustifolium; FEIS; Myerscough 1980, Biological Flora): floods burns from
// wind-blown seed and the rhizomes that survived, in clonal stands. Unbranched reddish stems 0.5 to 1.5 m, crowded
// spiral lanceolate leaves 5 to 15 cm, sessile, dark above with a pale midrib; a terminal raceme 15 to 40 cm of magenta
// four-petalled flowers 2 to 3 cm (two upper petals broader), dark purple sepals between, opening from the bottom up
// over drooping buds; below them long slender red-purple pods that split into four curling valves and release
// white-plumed seed in late summer. The first year after a burn: leafy shoots only, short.
void PlantGenerator::_build_fireweed() {
	const float age = _burn_age();
	const bool young = age < 1.0f;
	// Past about six years the stand thins as shrubs and grasses close in.
	const float vigour = young ? 0.35f + 0.4f * age : (age > 6.0f ? 0.8f : 1.0f);
	const int stems = int(young ? _rand(2.0f, 5.0f) : _rand(3.0f, 8.0f));
	float progress = -1.0f;
	float dead = 0.0f;
	const Bloom bloom = young ? BLOOM_NONE : _bloom(false, &progress, &dead);
	if (bloom == BLOOM_SEED) {
		progress = 1.3f;
	}
	const float spin = _rand(0.0f, Math::TAU);
	for (int i = 0; i < stems; i++) {
		const float r = 0.16f * Math::sqrt(_rand(0.0f, 1.0f));
		const float a = _rand(0.0f, Math::TAU);
		Transform3D base = burn_base(_rand(0.0f, Math::TAU), _rand(1.45f, 1.56f), Vector3(Math::cos(a) * r, 0.0f, Math::sin(a) * r));
		const float len = (young ? _rand(0.15f, 0.4f) : _rand(0.7f, 1.4f)) * vigour;
		const float bend = _rand(-0.12f, 0.12f);
		const int segs = 12;
		const LocalVector<Transform3D> fr = _stem_frames(base, len, bend, segs);
		_add_stem(base, len, 0.0045f * MAX(vigour, 0.6f), 0.0016f, bend, segs, 0.0f);
		// The raceme over the top quarter to third; leaves below it, smaller towards it.
		const bool raceme = !young && (bloom != BLOOM_NONE || progress < 0.0f || _rand(0.0f, 1.0f) < 0.7f);
		const float leafy_to = raceme ? _rand(0.62f, 0.72f) : 0.97f;
		const int leaves = int(len * leafy_to / 0.013f); // Crowded: a leaf every centimetre or so.
		const Wither wither_leaves(this, bloom == BLOOM_SEED ? 0.35f : dead * 0.3f); // Autumn reddens them first.
		for (int k = 0; k < leaves; k++) {
			const float s = Math::lerp(0.06f, leafy_to, (k + _rand(0.0f, 0.5f)) / leaves);
			const Transform3D at = _frame_at(fr, s);
			const float az = spin + i + k * BURN_GOLDEN;
			const Vector3 radial = at.basis.get_column(0) * Math::cos(az) + at.basis.get_column(1) * Math::sin(az);
			Leaf l;
			l.shape = LEAF_LANCEOLATE;
			const float u = s / leafy_to;
			l.length = Math::lerp(0.07f, 0.15f, Math::sin(float(Math::PI) * MIN(u * 0.9f + 0.1f, 1.0f))) * (young ? 0.75f : 1.0f) * _rand(0.85f, 1.1f);
			l.length *= u > 0.85f ? Math::lerp(1.0f, 0.5f, (u - 0.85f) / 0.15f) : 1.0f;
			l.width = l.length * _rand(0.13f, 0.19f);
			l.fold = 0.35f;
			l.bend = _rand(0.4f, 0.9f); // Spreading, the tips drooping.
			l.twist = _rand(-0.3f, 0.3f);
			l.segments = 7;
			const Vector3 d = (radial * 1.0f + at.basis.get_column(2) * _rand(0.4f, 0.8f)).normalized();
			_add_leaf(SURFACE_LEAF, Transform3D(burn_frame(d, Vector3(0, 1, 0)), at.origin), l, s, Math::lerp(0.75f, 1.0f, u));
		}
		if (!raceme) {
			continue;
		}
		const Wither wither(this, dead * 0.6f);
		const int flowers = int(_rand(14.0f, 30.0f) * vigour);
		Leaf petal;
		petal.shape = LEAF_OVATE;
		petal.fold = 0.25f;
		petal.segments = 4;
		Leaf sepal;
		sepal.shape = LEAF_LINEAR;
		sepal.width = 0.0022f;
		sepal.fold = 0.3f;
		sepal.segments = 3;
		for (int k = 0; k < flowers; k++) {
			const float u = float(k) / MAX(flowers - 1, 1); // 0 at the bottom of the raceme.
			const float s = Math::lerp(leafy_to, 0.995f, u);
			const Transform3D at = _frame_at(fr, s);
			const float az = spin + k * BURN_GOLDEN + i;
			const Vector3 axis = at.basis.get_column(2);
			const Vector3 radial = (at.basis.get_column(0) * Math::cos(az) + at.basis.get_column(1) * Math::sin(az)).normalized();
			// Without a season, mid-flowering: pods low, flowers open in the middle, buds over the top fifth.
			const float p = progress < 0.0f ? 0.55f : progress;
			if (u < p - 0.35f) {
				// A pod, 3 to 8 cm, ascending close to the stem; split open in seed.
				const Vector3 d = (axis * 1.6f + radial).normalized();
				const float plen = _rand(0.035f, 0.075f);
				const int from = builders[SURFACE_STEM].colors.size();
				const Transform3D pt = _add_stem(Transform3D(burn_frame(d, Vector3(0, 1, 0)), at.origin), plen, 0.0012f, 0.0007f, _rand(0.0f, 0.2f), 3, s);
				_tint_since(SURFACE_STEM, from, bloom == BLOOM_SEED ? Color(0.75f, 0.65f, 0.6f) : Color(1.1f, 0.85f, 1.0f));
				if (bloom == BLOOM_SEED && _rand(0.0f, 1.0f) < 0.7f) {
					// The valves curl back and the plumed seeds spill out: a white tuft at the tip.
					_add_head(SURFACE_CENTRE, pt, 0.008f, 0.012f, Color(1, 1, 1), 3, 7);
				}
				continue;
			}
			if (u > p + 0.05f) {
				// Buds on short pedicels, drooping at the tip of the raceme.
				const float droop = Math::smoothstep(0.8f, 1.0f, u);
				const Vector3 d = (radial + axis * (0.6f - 1.2f * droop)).normalized();
				_add_head(SURFACE_STEM, Transform3D(burn_frame(d, Vector3(0, 1, 0)), at.origin + radial * 0.004f), 0.0025f, Math::lerp(0.01f, 0.005f, u), Color(1.1f, 0.6f, 0.9f), 3, 6);
				continue;
			}
			// An open flower on a pedicel 1 to 2 cm, facing out; the ovary below is the young pod.
			const Vector3 d = (radial * 1.0f + axis * 0.3f).normalized();
			const Transform3D ft = _add_stem(Transform3D(burn_frame(d, Vector3(0, 1, 0)), at.origin), _rand(0.012f, 0.02f), 0.0007f, 0.0006f, 0.1f, 2, s);
			const Basis fb = burn_frame(ft.basis.get_column(2), axis);
			const float fsize = _rand(0.013f, 0.018f);
			for (int q = 0; q < 4; q++) {
				// Two broader upper petals, two narrower lower ones (slightly zygomorphic).
				const bool upper = q < 2;
				const float pa = (upper ? 0.55f : -0.55f) + (q % 2 == 0 ? 1.0f : -1.0f) * (upper ? 0.5f : 0.45f) + (upper ? Math::PI * 0.5f : -Math::PI * 0.5f);
				const Vector3 pd = (fb.get_column(0) * Math::cos(pa) + fb.get_column(1) * Math::sin(pa) + fb.get_column(2) * 0.35f).normalized();
				petal.length = fsize * (upper ? 1.0f : 0.85f);
				petal.width = petal.length * (upper ? 0.75f : 0.6f);
				petal.bend = -0.2f;
				_add_leaf(SURFACE_PETAL, Transform3D(burn_frame(pd, fb.get_column(2)), ft.origin), petal, 1.0f);
				// A narrow dark purple sepal between the petals.
				const float sa = pa + Math::PI * 0.25f;
				const Vector3 sd = (fb.get_column(0) * Math::cos(sa) + fb.get_column(1) * Math::sin(sa) + fb.get_column(2) * 0.2f).normalized();
				sepal.length = fsize * 0.8f;
				sepal.bend = 0.1f;
				_add_leaf(SURFACE_STEM, Transform3D(burn_frame(sd, fb.get_column(2)), ft.origin), sepal, 1.0f, 0.7f, Color(0.7f, 0.45f, 0.75f));
			}
			// The white style, its four-lobed stigma curled back, and the stamens.
			const Transform3D st(burn_frame((fb.get_column(2) - fb.get_column(1) * 0.4f).normalized(), axis), ft.origin);
			_add_stem(st, fsize * 0.9f, 0.0005f, 0.0004f, 0.3f, 2, 1.0f, SURFACE_CENTRE);
		}
	}
}

// Fire mosses on bare burnt soil (Funaria hygrometrica, with Ceratodon purpureus; Atherton et al. 2010): in the first
// months to three years a low pale yellow-green carpet of tiny shoots, then a forest of setae 2 to 5 cm, orange-yellow
// to red-brown and twisting with the damp, each with a nodding, pear-shaped, curved capsule, green and then orange-
// brown; Ceratodon's setae are purple-red with upright cylindrical capsules. Fades as vascular plants close in.
void PlantGenerator::_build_fire_moss() {
	const float age = _burn_age();
	// The carpet thickens to about two years, then breaks up.
	const float prime = Math::smoothstep(0.1f, 0.8f, age) * (1.0f - 0.5f * Math::smoothstep(2.0f, 3.5f, age));
	const float radius = _rand(0.07f, 0.2f);
	const float height = _rand(0.004f, 0.009f);
	Builder &b = builders[SURFACE_CENTRE];
	const int rings = 8;
	const int sectors = 32;
	const int first = b.verts.size();
	float ph[6];
	for (float &p : ph) {
		p = _rand(0.0f, Math::TAU);
	}
	auto rim = [&](float p_a) {
		// Ragged: the carpet grows into the gaps between ash and stones.
		return radius * (1.0f + 0.25f * Math::sin(3.0f * p_a + ph[0]) + 0.15f * Math::sin(7.0f * p_a + ph[1]) + 0.1f * Math::sin(13.0f * p_a + ph[5]));
	};
	for (int r = 0; r <= rings; r++) {
		const float u = float(r) / rings;
		for (int k = 0; k < sectors; k++) {
			const float a = k * Math::TAU / sectors;
			const float rr = rim(a) * u;
			const float x = Math::cos(a) * rr;
			const float z = Math::sin(a) * rr;
			const float lump = 1.0f + 0.25f * Math::sin(x * 80.0f + ph[2]) * Math::sin(z * 75.0f + ph[3]) + 0.12f * Math::sin(x * 170.0f + ph[4] + z * 120.0f);
			const float y = height * Math::pow(MAX(0.0f, 1.0f - u * u * u), 0.6f) * lump - 0.002f;
			b.verts.push_back(Vector3(x, y, z));
			b.uvs.push_back(Vector2(x, z) / 0.05f);
			b.uv2s.push_back(Vector2(0.05f, 0.0f));
			const float ao = Math::lerp(1.0f, 0.7f, u * u);
			b.colors.push_back(Color(ao, ao, ao));
		}
	}
	for (int r = 0; r < rings; r++) {
		for (int k = 0; k < sectors; k++) {
			const int a = first + r * sectors + k;
			const int c = first + r * sectors + (k + 1) % sectors;
			b.indices.append_array(PackedInt32Array({ a, c, a + sectors, c, c + sectors, a + sectors }));
		}
	}
	// The setae: Funaria's orange, twisting, with nodding pear-shaped capsules; one in five Ceratodon, purple-red and
	// upright. Capsules ripen from green to orange-brown over the season.
	float progress = -1.0f;
	_bloom(false, &progress);
	const float ripe = progress < 0.0f ? 0.6f : CLAMP(progress, 0.0f, 1.0f);
	const int setae = int(_rand(90.0f, 200.0f) * (0.25f + 0.75f * prime));
	for (int s = 0; s < setae; s++) {
		const float a = _rand(0.0f, Math::TAU);
		const float u = Math::sqrt(_rand(0.0f, 0.9f));
		const float rr = rim(a) * u;
		const float y = height * Math::pow(MAX(0.0f, 1.0f - u * u * u), 0.6f) - 0.002f;
		const Vector3 at(Math::cos(a) * rr, y, Math::sin(a) * rr);
		const bool ceratodon = _rand(0.0f, 1.0f) < 0.2f;
		const float len = ceratodon ? _rand(0.012f, 0.03f) : _rand(0.02f, 0.05f);
		const float bend = ceratodon ? _rand(0.0f, 0.2f) : _rand(0.2f, 0.7f); // Funaria's seta curves and twists.
		const int from = builders[SURFACE_STEM].colors.size();
		const Transform3D top = _add_stem(burn_base(_rand(0.0f, Math::TAU), _rand(1.25f, 1.55f), at), len, 0.00028f, 0.0002f, bend, 4, 0.2f);
		const float k = _rand(0.8f, 1.15f);
		_tint_since(SURFACE_STEM, from, ceratodon ? Color(0.75f * k, 0.35f * k, 0.55f * k) : Color(k, k, k));
		const Color capsule = (ceratodon ? Color(0.75f, 0.45f, 0.6f) : Color(1, 1, 1)).lerp(Color(1.4f, 1.9f, 0.9f), 1.0f - ripe) * _rand(0.85f, 1.1f);
		if (ceratodon) {
			_add_head(SURFACE_PETAL, top, 0.0007f, 0.0028f, capsule, 2, 6);
		} else {
			// Nodding: the capsule hangs from the bent tip, asymmetric (a curved pear).
			const Vector3 down = (top.basis.get_column(2) * 0.3f + Vector3(_rand(-0.3f, 0.3f), -1.0f, _rand(-0.3f, 0.3f))).normalized();
			_add_head(SURFACE_PETAL, Transform3D(burn_frame(down, top.basis.get_column(2)), top.origin), 0.0012f, 0.0035f, capsule, 3, 7);
		}
	}
	plant_height = MAX(plant_height, height + 0.04f);
}

// Burnt heather and scrub stems (Gimingham 1972; Davies et al. 2010 on burn severity): a hot fire takes the leaves and
// twigs and leaves the thicker stems standing as black skeletons 10 to 40 cm, grey-white ash on their tips. Rain takes
// the ash within weeks; the stems weather silver-grey and fall over the next 2 to 5 years. Young heather resprouts
// from the stem bases in the first season.
void PlantGenerator::_build_charred_stems() {
	const float age = _burn_age();
	const float weather = Math::smoothstep(0.3f, 4.0f, age); // Black to silver-grey.
	const float fallen = Math::smoothstep(1.5f, 5.0f, age); // Share of the stems down.
	const float ash = 1.0f - Math::smoothstep(0.02f, 0.25f, age);
	const Color tint = Color(1, 1, 1).lerp(Color(6.0f, 5.6f, 5.2f), weather) * _rand(0.85f, 1.15f);
	const int stems = int(_rand(10.0f, 18.0f) * (1.0f - 0.4f * fallen));
	LocalVector<Transform3D> tips;
	for (int i = 0; i < stems; i++) {
		const bool down = _rand(0.0f, 1.0f) < fallen;
		const float az = _rand(0.0f, Math::TAU);
		const Vector3 at(_rand(-0.06f, 0.06f), 0.0f, _rand(-0.06f, 0.06f));
		// Heather's stems lean out from the burnt crown and kink at each old branching (three pieces, each turned);
		// fallen ones lie at a low angle.
		Transform3D piece = burn_base(az, down ? _rand(0.05f, 0.3f) : _rand(0.75f, 1.3f), at);
		const float len = _rand(0.12f, 0.3f) * (down ? 0.7f : 1.0f) * (1.0f - 0.25f * weather);
		float r = _rand(0.0025f, 0.0045f);
		const int from = builders[SURFACE_STEM].colors.size();
		for (int k = 0; k < 3; k++) {
			const float pl = len * (k == 0 ? 0.45f : 0.3f) * _rand(0.8f, 1.2f);
			const float bend = _rand(-0.5f, 0.5f);
			const LocalVector<Transform3D> fr = _stem_frames(piece, pl, bend, 3);
			const Transform3D top = _add_stem(piece, pl, r, r * 0.7f, bend, 3, 0.1f * k);
			r *= 0.7f;
			// Side branches burnt back to stubs, some forked once.
			const int subs = int(_rand(1.0f, 3.5f));
			for (int q = 0; q < subs; q++) {
				Transform3D bt = _frame_at(fr, _rand(0.3f, 0.95f));
				const float a = _rand(0.0f, Math::TAU);
				bt.basis = burn_frame(bt.basis.get_column(2) * 0.7f + Vector3(Math::cos(a), 0.5f, Math::sin(a)) * 0.7f, Vector3(0, 1, 0));
				const float bl = _rand(0.02f, 0.06f) * (1.0f - 0.4f * weather);
				const Transform3D st = _add_stem(bt, bl, MIN(r, 0.0012f), 0.0005f, _rand(-0.5f, 0.5f), 2, 0.2f);
				tips.push_back(st);
				if (_rand(0.0f, 1.0f) < 0.5f) {
					const float fa = _rand(0.0f, Math::TAU);
					const Transform3D ft(burn_frame(st.basis.get_column(2) + Vector3(Math::cos(fa), 0.3f, Math::sin(fa)) * 0.6f, Vector3(0, 1, 0)), st.origin);
					tips.push_back(_add_stem(ft, bl * 0.5f, 0.0005f, 0.0003f, 0.2f, 2, 0.4f));
				}
			}
			// The next piece turns at the kink.
			const float ka = _rand(0.0f, Math::TAU);
			const Vector3 d = (top.basis.get_column(2) + Vector3(Math::cos(ka), 0.2f, Math::sin(ka)) * _rand(0.3f, 0.6f)).normalized();
			piece = Transform3D(burn_frame(d, Vector3(0, 1, 0)), top.origin);
			if (k == 2) {
				tips.push_back(top);
			}
		}
		_tint_since(SURFACE_STEM, from, tint);
	}
	// Grey-white ash on the tips and in the forks, fresh after the fire.
	if (ash > 0.0f) {
		for (const Transform3D &t : tips) {
			if (_rand(0.0f, 1.0f) < 0.7f * ash) {
				_add_head(SURFACE_CENTRE, t, _rand(0.002f, 0.0035f), _rand(0.003f, 0.006f), Color(1, 1, 1) * _rand(0.8f, 1.05f), 2, 5);
			}
		}
	}
	// Resprouts from the stem bases: short green leafy shoots, from the first season to about five years.
	const float sprout = Math::smoothstep(0.15f, 0.6f, age) * (1.0f - Math::smoothstep(4.0f, 6.0f, age));
	if (sprout > 0.0f) {
		Leaf sc;
		sc.shape = LEAF_LINEAR;
		sc.width = 0.003f;
		sc.fold = 0.0f;
		sc.segments = 1;
		const int shoots = int(_rand(4.0f, 12.0f) * sprout);
		for (int i = 0; i < shoots; i++) {
			const Transform3D base = burn_base(_rand(0.0f, Math::TAU), _rand(0.9f, 1.4f), Vector3(_rand(-0.05f, 0.05f), 0.0f, _rand(-0.05f, 0.05f)));
			const float len = _rand(0.02f, 0.05f) + 0.04f * MIN(age, 3.0f);
			const LocalVector<Transform3D> fr = _stem_frames(base, len, 0.2f, 3);
			const int from = builders[SURFACE_STEM].colors.size();
			_add_stem(base, len, 0.0009f, 0.0005f, 0.2f, 3, 0.3f);
			_tint_since(SURFACE_STEM, from, Color(5.0f, 7.0f, 3.0f)); // Green young bark.
			const int scales = int(len / 0.006f);
			for (int k = 0; k < scales; k++) {
				const float s = 0.15f + 0.85f * float(k) / MAX(scales - 1, 1);
				const Transform3D at = _frame_at(fr, s);
				const float a = k * Math::PI * 0.5f;
				const Vector3 radial = at.basis.get_column(0) * Math::cos(a) + at.basis.get_column(1) * Math::sin(a);
				sc.length = _rand(0.007f, 0.012f);
				_add_leaf(SURFACE_LEAF, Transform3D(burn_frame((radial * 0.7f + at.basis.get_column(2)).normalized(), Vector3(0, 1, 0)), at.origin), sc, Math::lerp(0.3f, 0.8f, s));
			}
		}
	}
}

// A grass tussock resprouting after the fire (Molinia, Deschampsia and the fescues; Grime et al. 2007): the fire takes
// the leaves to black stubble a few centimetres high over a charred crown; the buds inside the crown survive and a
// bright green flush pushes through within two to four weeks in the growing season. By the end of the first year the
// tussock is whole again with a few black stubs left in its base.
void PlantGenerator::_build_burnt_tussock() {
	const float age = _burn_age();
	const float regrow = Math::smoothstep(0.0f, 1.0f, age);
	const float crown_r = _rand(0.04f, 0.08f);
	// The charred crown: a low black hump, mostly hidden by the stubble.
	_add_head(SURFACE_STEM, Transform3D(burn_frame(Vector3(0, 1, 0), Vector3(1, 0, 0)), Vector3(0, -0.006f, 0)), crown_r * 0.75f, _rand(0.008f, 0.014f), Color(1, 1, 1), 3, 12);
	// The stubble: leaf sheaths and culm bases burnt off blunt 2 to 6 cm up, splayed; fewer as the new leaves hide
	// and rot them.
	const int stubs = int(_rand(70.0f, 120.0f) * (1.0f - 0.7f * regrow));
	LocalVector<Transform3D> stub_tops;
	for (int i = 0; i < stubs; i++) {
		const float r = crown_r * Math::sqrt(_rand(0.0f, 1.0f));
		const float a = _rand(0.0f, Math::TAU);
		const float elev = Math::lerp(1.45f, 0.9f, r / crown_r) + _rand(-0.15f, 0.15f);
		const int from = builders[SURFACE_STEM].colors.size();
		const float rad = _rand(0.0012f, 0.0022f);
		stub_tops.push_back(_add_stem(burn_base(a + _rand(-0.4f, 0.4f), elev, Vector3(Math::cos(a) * r, 0.0f, Math::sin(a) * r)), _rand(0.02f, 0.06f), rad, rad * 0.8f, _rand(0.0f, 0.3f), 2, 0.0f));
		_tint_since(SURFACE_STEM, from, Color(1, 1, 1) * _rand(0.7f, 1.4f));
	}
	// Ash on the fresh stubble's burnt ends.
	const float ash = 1.0f - Math::smoothstep(0.02f, 0.2f, age);
	for (const Transform3D &t : stub_tops) {
		if (_rand(0.0f, 1.0f) < 0.35f * ash) {
			_add_head(SURFACE_CENTRE, t, 0.0018f, 0.0015f, Color(1, 1, 1) * _rand(0.6f, 0.85f), 2, 5);
		}
	}
	// The green flush from the crown: short and upright at first, then the full arching tussock.
	const float flush = Math::smoothstep(0.02f, 0.1f, age);
	if (flush <= 0.0f) {
		return;
	}
	const int blades = int(Math::lerp(25.0f, 80.0f, regrow) * flush * _rand(0.8f, 1.2f));
	for (int i = 0; i < blades; i++) {
		const float r = crown_r * 0.8f * Math::sqrt(_rand(0.0f, 1.0f));
		const float a = _rand(0.0f, Math::TAU);
		Leaf l;
		l.shape = LEAF_LINEAR;
		l.length = Math::lerp(0.04f, 0.38f, regrow) * _rand(0.6f, 1.15f) * flush;
		l.width = Math::lerp(0.002f, 0.004f, regrow);
		l.fold = 0.5f;
		l.bend = Math::lerp(0.1f, 1.0f, regrow) * _rand(0.6f, 1.3f);
		l.twist = _rand(-0.4f, 0.4f);
		l.segments = int(Math::lerp(3.0f, 8.0f, regrow));
		const float elev = Math::lerp(1.5f, 1.05f, r / crown_r) + _rand(-0.12f, 0.12f);
		_add_leaf(SURFACE_LEAF, Transform3D(burn_frame(burn_dir(a + _rand(-0.5f, 0.5f), elev), Vector3(0, 1, 0)), Vector3(Math::cos(a) * r, 0.01f, Math::sin(a) * r)), l, 0.0f, 0.9f);
	}
}

// The burn sets (docs/ideas/vegetation.md, "Burnt ground and regrowth"): each preset in bands of the years since the burn,
// built at the band's age and placed by PlantSpecies.RULE_BURN_AGE, so a burn scar shows its own stage and older scars
// theirs. Unburnt ground (-1) takes none of them; the stand's own plants come back by their burn_recovery.
TypedArray<Resource> PlantGenerator::make_burn(BurnStand p_stand, int p_variants, bool p_textures, const Dictionary &p_season) {
	using P = PlantGenerator;
	struct BurnSpec {
		Preset preset;
		float age; // Years since the burn the meshes show.
		Vector2 ages; // Where it grows, years since the burn.
		float fade;
		float spacing, footprint, density, scale_min, scale_max;
		float patch_scale, patch_cover;
		float visibility;
	};
	LocalVector<BurnSpec> set;
	switch (p_stand) {
		case BURN_HEATH:
			// Heather moorland after a hot burn: black stems, then fire moss and fireweed on the bare peat while the
			// heather comes back from seed and stem bases (Gimingham 1972); purple moor-grass resprouts at once.
			set = LocalVector<BurnSpec>({ { P::PRESET_CHARRED_STEMS, 0.05f, Vector2(0.0f, 0.3f), 0.1f, 0.4f, 0.12f, 0.85f, 0.9f, 1.3f, 0.0f, 0.0f, 40.0f },
					{ P::PRESET_CHARRED_STEMS, 1.0f, Vector2(0.3f, 2.0f), 0.3f, 0.45f, 0.12f, 0.75f, 0.85f, 1.25f, 0.0f, 0.0f, 40.0f },
					{ P::PRESET_CHARRED_STEMS, 3.2f, Vector2(2.0f, 5.0f), 0.8f, 0.6f, 0.12f, 0.5f, 0.8f, 1.2f, 6.0f, 0.6f, 40.0f },
					{ P::PRESET_FIRE_MOSS, 1.2f, Vector2(0.25f, 3.0f), 0.5f, 0.35f, 0.0f, 0.55f, 0.8f, 1.4f, 5.0f, 0.5f, 25.0f },
					{ P::PRESET_FIREWEED, 0.6f, Vector2(0.35f, 1.3f), 0.25f, 0.8f, 0.15f, 0.25f, 0.8f, 1.2f, 9.0f, 0.4f, 60.0f },
					{ P::PRESET_FIREWEED, 2.5f, Vector2(1.3f, 6.5f), 1.0f, 0.9f, 0.2f, 0.4f, 0.85f, 1.2f, 12.0f, 0.45f, 90.0f },
					{ P::PRESET_BURNT_TUSSOCK, 0.03f, Vector2(0.0f, 0.12f), 0.05f, 1.1f, 0.15f, 0.35f, 0.8f, 1.2f, 7.0f, 0.5f, 40.0f },
					{ P::PRESET_BURNT_TUSSOCK, 0.4f, Vector2(0.12f, 1.2f), 0.3f, 1.1f, 0.15f, 0.35f, 0.8f, 1.2f, 7.0f, 0.5f, 40.0f } });
			break;
		case BURN_GRASS:
			// Grassland: the tussocks green up through the black within weeks; fire moss and fireweed only in gaps.
			set = LocalVector<BurnSpec>({ { P::PRESET_BURNT_TUSSOCK, 0.03f, Vector2(0.0f, 0.12f), 0.05f, 0.5f, 0.12f, 0.8f, 0.8f, 1.2f, 0.0f, 0.0f, 40.0f },
					{ P::PRESET_BURNT_TUSSOCK, 0.3f, Vector2(0.12f, 0.6f), 0.15f, 0.5f, 0.12f, 0.8f, 0.8f, 1.2f, 0.0f, 0.0f, 40.0f },
					{ P::PRESET_BURNT_TUSSOCK, 0.9f, Vector2(0.6f, 1.5f), 0.4f, 0.55f, 0.12f, 0.7f, 0.85f, 1.2f, 0.0f, 0.0f, 40.0f },
					{ P::PRESET_FIRE_MOSS, 0.8f, Vector2(0.25f, 2.0f), 0.4f, 0.6f, 0.0f, 0.25f, 0.7f, 1.2f, 6.0f, 0.3f, 25.0f },
					{ P::PRESET_FIREWEED, 2.0f, Vector2(1.0f, 5.0f), 0.8f, 1.6f, 0.2f, 0.2f, 0.8f, 1.1f, 14.0f, 0.3f, 90.0f } });
			break;
		case BURN_SCRUB:
			// Scrub (gorse, broom, rockrose): taller charred skeletons standing longer; fireweed and resprouting grasses
			// fill between (the shrubs' own resprouts come from the trees and scrub sets' burn_recovery).
			set = LocalVector<BurnSpec>({ { P::PRESET_CHARRED_STEMS, 0.05f, Vector2(0.0f, 0.3f), 0.1f, 0.9f, 0.3f, 0.7f, 1.6f, 2.4f, 0.0f, 0.0f, 60.0f },
					{ P::PRESET_CHARRED_STEMS, 1.5f, Vector2(0.3f, 3.0f), 0.4f, 1.0f, 0.3f, 0.6f, 1.5f, 2.2f, 0.0f, 0.0f, 60.0f },
					{ P::PRESET_CHARRED_STEMS, 4.0f, Vector2(3.0f, 7.0f), 1.0f, 1.3f, 0.3f, 0.4f, 1.4f, 2.0f, 8.0f, 0.5f, 60.0f },
					{ P::PRESET_FIRE_MOSS, 1.0f, Vector2(0.25f, 2.5f), 0.4f, 0.5f, 0.0f, 0.35f, 0.8f, 1.3f, 5.0f, 0.4f, 25.0f },
					{ P::PRESET_FIREWEED, 2.5f, Vector2(0.8f, 6.0f), 0.8f, 1.0f, 0.2f, 0.35f, 0.85f, 1.2f, 10.0f, 0.45f, 90.0f },
					{ P::PRESET_BURNT_TUSSOCK, 0.4f, Vector2(0.0f, 1.2f), 0.3f, 0.9f, 0.15f, 0.4f, 0.8f, 1.2f, 7.0f, 0.5f, 40.0f } });
			break;
		case BURN_MAX:
			break;
	}
	TypedArray<Resource> out;
	int seed_base = 9800 + int(p_stand) * 100;
	for (const BurnSpec &f : set) {
		Dictionary season = p_season.duplicate();
		season["burn_age"] = f.age;
		Ref<PlantGenerator> gen;
		gen.instantiate();
		gen->set_textures(p_textures);
		gen->set_preset(f.preset);
		gen->apply_preset_colors();
		gen->set_seed(seed_base);
		gen->set_season(season);
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
		sp->set_align_to_ground(f.preset == P::PRESET_FIRE_MOSS ? 1.0f : 0.3f);
		sp->set_rule_range(PlantSpecies::RULE_BURN_AGE, f.ages);
		sp->set_rule_fade(PlantSpecies::RULE_BURN_AGE, f.fade);
		sp->set_rule_range(PlantSpecies::RULE_SLOPE, Vector2(0.0f, 40.0f));
		sp->set_rule_fade(PlantSpecies::RULE_SLOPE, 5.0f);
		if (f.patch_scale > 0.0f) {
			sp->set_patch_scale(f.patch_scale);
			sp->set_patch_cover(f.patch_cover);
			sp->set_patch_seed(seed_base);
		}
		sp->set_visibility_range(f.visibility);
		sp->set_meta("burn", true);
		sp->set_meta("burn_age", f.age);
		if (f.preset == P::PRESET_CHARRED_STEMS) {
			// Far off a fresh burn reads black, an old one grey.
			const float w = Math::smoothstep(0.3f, 4.0f, f.age);
			Color far = Color(0.04f, 0.036f, 0.032f).lerp(Color(0.2f, 0.19f, 0.18f), w);
			far.a = 0.4f;
			sp->set_far_color(far);
		}
		tag_species(sp, f.preset);
		out.push_back(sp);
		seed_base += 13;
	}
	return out;
}

/**************************************************************************/
/*  plant_bog.cpp                                                         */
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


// Bog and moor plants (docs/ideas/vegetation.md, "Bog and moor plants"): the flowering plants of
// blanket bog and valley mire between the sphagnum (whose hummocks are _build_moss()). Measurements
// from Stace, "New Flora of the British Isles" (2019), and Rodwell, "British Plant Communities"
// vol. 2 (M17 to M21 mires).

#include "plant_generator.h"
#include "plant_species.h"

namespace {
const float BOG_GOLDEN = 2.39996323f;

Basis bog_frame(const Vector3 &p_forward, const Vector3 &p_up_hint) {
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

Vector3 bog_dir(float p_azimuth, float p_elevation) {
	return Vector3(Math::cos(p_azimuth) * Math::cos(p_elevation), Math::sin(p_elevation), Math::sin(p_azimuth) * Math::cos(p_elevation));
}

Transform3D bog_base(float p_azimuth, float p_elevation, const Vector3 &p_at = Vector3()) {
	return Transform3D(bog_frame(bog_dir(p_azimuth, p_elevation), Vector3(0, 1, 0)), p_at);
}
} // namespace

// Common cotton grass (Eriophorum angustifolium): a loose tuft of narrow, channelled leaves 15 to
// 40 cm, red-tipped from midsummer; smooth stems 20 to 60 cm with two to seven drooping spikelets.
// They flower dark in April and May, then each spikelet becomes a tuft of white bristles 3 to 4 cm,
// the "cotton" that whitens a bog from late May into August.
void PlantGenerator::_build_cottongrass() {
	const int blades = int(_rand(8.0f, 16.0f));
	for (int i = 0; i < blades; i++) {
		Leaf l;
		l.shape = LEAF_LINEAR;
		l.length = _rand(0.15f, 0.35f);
		l.width = _rand(0.003f, 0.005f);
		l.fold = 0.7f; // Channelled, triangular towards the tip.
		l.bend = _rand(0.4f, 1.3f);
		l.twist = _rand(-0.6f, 0.6f);
		l.segments = 7;
		const Vector3 at(_rand(-0.02f, 0.02f), 0.0f, _rand(-0.02f, 0.02f));
		_add_leaf(SURFACE_LEAF, Transform3D(bog_frame(bog_dir(_rand(0.0f, Math::TAU), _rand(1.0f, 1.45f)), Vector3(0, 1, 0)), at), l, 0.0f, 0.85f);
	}
	const int stems = int(_rand(1.0f, 4.99f));
	Leaf bristle;
	bristle.shape = LEAF_LINEAR;
	bristle.width = 0.0016f;
	bristle.fold = 0.0f;
	bristle.segments = 3;
	for (int s = 0; s < stems; s++) {
		float progress = 0.0f, dead = 0.0f;
		const Bloom bloom = _bloom(true, &progress, &dead); // Without a season: mostly in cotton, its look.
		if (bloom == BLOOM_NONE) {
			continue;
		}
		const Wither wither(this, dead * 0.6f); // The stems brown; the cotton stays white.
		const float az = _rand(0.0f, Math::TAU);
		const Transform3D top = _add_stem(bog_base(az, _rand(1.38f, 1.55f), Vector3(_rand(-0.015f, 0.015f), 0.0f, _rand(-0.015f, 0.015f))), _rand(0.22f, 0.5f), 0.0014f, 0.001f, _rand(-0.2f, 0.2f), 7, 0.0f);
		const int spikes = int(_rand(2.0f, 5.99f));
		for (int k = 0; k < spikes; k++) {
			// Each on a slender stalk, nodding to one side.
			const float a = az + k * BOG_GOLDEN;
			const Vector3 d = (bog_dir(a, 0.0f) * _rand(0.5f, 1.0f) + Vector3(0, _rand(-0.1f, 0.5f), 0)).normalized();
			const Transform3D st = _add_stem(Transform3D(bog_frame(d, Vector3(0, 1, 0)), top.origin), _rand(0.01f, 0.035f), 0.0005f, 0.0004f, 0.4f, 3, 0.8f);
			if (bloom != BLOOM_SEED) {
				_add_head(SURFACE_CENTRE, st, 0.0018f, 0.006f, Color(1, 1, 1), 3, 6);
				continue;
			}
			// The cotton: thousands of fine bristles read from any distance as one soft, hanging ball, so
			// a white ovoid core carries the mass and short crinkled bristles break its outline.
			const Vector3 hang = (st.basis.get_column(2) + Vector3(0, -0.6f, 0)).normalized();
			const float ball = _rand(0.012f, 0.017f) * (0.6f + 0.4f * progress);
			const Vector3 centre = st.origin + st.basis.get_column(2) * 0.003f + hang * ball * 0.5f;
			_add_head(SURFACE_PETAL, Transform3D(bog_frame(hang, Vector3(0, 1, 0)), centre), ball, ball * 1.15f, Color(1, 1, 1), 4, 9);
			_add_head(SURFACE_PETAL, Transform3D(bog_frame(-hang, Vector3(0, 1, 0)), centre), ball, ball * 0.8f, Color(1, 1, 1), 3, 9);
			const int n = int(_rand(40.0f, 55.0f));
			for (int b = 0; b < n; b++) {
				const Vector3 r = Vector3(_rand(-1.0f, 1.0f), _rand(-1.0f, 1.0f), _rand(-1.0f, 1.0f)).normalized();
				const Vector3 bd = (hang * 0.35f + r).normalized();
				bristle.length = ball * _rand(0.35f, 0.7f) * (1.0f + 0.5f * MAX(bd.dot(hang), 0.0f));
				bristle.bend = _rand(-1.6f, 1.6f);
				bristle.twist = _rand(-1.5f, 1.5f);
				_add_leaf(SURFACE_PETAL, Transform3D(bog_frame(bd, r), centre + bd * ball * 0.85f), bristle, 1.0f);
			}
		}
	}
}

// Bog asphodel (Narthecium ossifragum): flat fans of stiff, curved, iris-like leaves 5 to 20 cm
// from a creeping rhizome; stems 10 to 40 cm with a spike of yellow six-pointed stars and woolly
// orange anthers in July and August; afterwards the whole plant turns deep orange, the capsules
// standing into winter.
void PlantGenerator::_build_bog_asphodel() {
	const int fans = int(_rand(2.0f, 4.99f));
	Leaf l;
	l.shape = LEAF_LINEAR;
	l.fold = 0.05f; // Flat, edge-on to the stem (equitant).
	l.segments = 6;
	const float spin = _rand(0.0f, Math::TAU);
	for (int f = 0; f < fans; f++) {
		const float az = spin + f * BOG_GOLDEN;
		const Vector3 at = Vector3(Math::cos(az), 0.0f, Math::sin(az)) * _rand(0.0f, 0.04f);
		const int n = int(_rand(4.0f, 7.0f));
		// The fan's plane: the leaves stand in it, curving out to either side like a sickle.
		const Vector3 plane = bog_dir(az + Math::PI * 0.5f, 0.0f);
		for (int k = 0; k < n; k++) {
			const float u = (float(k) / MAX(n - 1, 1)) * 2.0f - 1.0f; // -1 .. 1 across the fan.
			const Vector3 d = (Vector3(0, 1, 0) + plane * u * 0.55f).normalized();
			l.length = _rand(0.07f, 0.16f) * (1.0f - 0.3f * Math::abs(u));
			l.width = _rand(0.003f, 0.005f);
			l.bend = (u < 0.0f ? -1.0f : 1.0f) * _rand(0.2f, 0.5f);
			l.twist = 0.0f;
			// The blade's flat face looks along the fan's normal, so the fan is a flat spray.
			const Vector3 face = plane.cross(d).normalized();
			_add_leaf(SURFACE_LEAF, Transform3D(bog_frame(d, face), at), l, 0.0f);
		}
	}
	const int stems = int(_rand(1.0f, 3.99f));
	Leaf tepal;
	tepal.shape = LEAF_LANCEOLATE;
	tepal.fold = 0.2f;
	tepal.bend = -0.1f;
	tepal.segments = 3;
	for (int s = 0; s < stems; s++) {
		float dead = 0.0f;
		const Bloom bloom = _bloom(false, nullptr, &dead);
		if (bloom == BLOOM_NONE) {
			continue;
		}
		const Wither wither(this, dead * 0.5f); // Orange, not grey: the capsules colour the plant.
		const Transform3D base = bog_base(spin + s * 2.0f, _rand(1.42f, 1.55f));
		const float len = _rand(0.12f, 0.3f);
		const LocalVector<Transform3D> frames = _stem_frames(base, len, _rand(-0.1f, 0.1f), 6);
		_add_stem(base, len, 0.0013f, 0.001f, 0.0f, 6, 0.0f);
		const int flowers = int(_rand(6.0f, 14.0f));
		for (int k = 0; k < flowers; k++) {
			const float t = Math::lerp(0.6f, 1.0f, (k + 0.5f) / flowers);
			const Transform3D at = _frame_at(frames, t);
			const float a = k * BOG_GOLDEN;
			const Vector3 radial = at.basis.get_column(0) * Math::cos(a) + at.basis.get_column(1) * Math::sin(a);
			const Vector3 face = (radial + at.basis.get_column(2) * 0.6f).normalized();
			const Vector3 c = at.origin + radial * 0.006f;
			_add_stem(Transform3D(bog_frame(face, at.basis.get_column(2)), at.origin), 0.006f, 0.0005f, 0.0004f, 0.0f, 1, 0.7f);
			if (bloom == BLOOM_SEED) {
				// A narrow orange capsule, pointing up.
				_add_head(SURFACE_CENTRE, Transform3D(bog_frame(at.basis.get_column(2), radial), c), 0.0018f, 0.009f, Color(1, 1, 1), 3, 6);
				continue;
			}
			const Basis fb = bog_frame(face, at.basis.get_column(2));
			for (int p = 0; p < 6; p++) {
				const float pa = p * Math::TAU / 6.0f;
				const Vector3 d = (fb.get_column(0) * Math::cos(pa) + fb.get_column(1) * Math::sin(pa) + face * 0.25f).normalized();
				tepal.length = 0.0075f;
				tepal.width = 0.0022f;
				_add_leaf(SURFACE_PETAL, Transform3D(bog_frame(d, face), c), tepal, 1.0f);
			}
			_add_head(SURFACE_CENTRE, Transform3D(fb, c + face * 0.002f), 0.0018f, 0.0025f, Color(1, 1, 1), 2, 6);
		}
	}
}

// Round-leaved sundew (Drosera rotundifolia): a flat rosette 2 to 6 cm across of round blades
// 6 to 10 mm on hairy stalks 1 to 3 cm, red in the open, each blade fringed and covered with red
// glandular tentacles tipped with a glistening drop of mucilage; a thin stem 5 to 20 cm of small
// white flowers in June to August. On wet sphagnum lawns and bare peat.
void PlantGenerator::_build_sundew() {
	leaf_wear = 0.0f;
	const int leaves = int(_rand(5.0f, 10.0f));
	const float spin = _rand(0.0f, Math::TAU);
	Leaf l;
	l.shape = LEAF_ROUND;
	l.fold = -0.1f; // Shallow spoons.
	l.bend = -0.15f;
	l.segments = 5;
	for (int i = 0; i < leaves; i++) {
		const float az = spin + i * BOG_GOLDEN;
		const Transform3D top = _add_stem(bog_base(az, _rand(0.08f, 0.3f)), _rand(0.012f, 0.028f), 0.0006f, 0.0005f, -0.1f, 3, 0.0f);
		const float r = _rand(0.003f, 0.005f);
		l.length = 2.0f * r;
		l.width = 2.0f * r;
		const Vector3 d = (bog_dir(az, 0.0f) + Vector3(0, _rand(0.15f, 0.4f), 0)).normalized();
		const Basis lb = bog_frame(d, Vector3(0, 1, 0));
		_add_leaf(SURFACE_LEAF, Transform3D(lb, top.origin), l, 0.0f);
		// Tentacles: long ones round the rim, short ones over the blade, each with its drop.
		const Vector3 centre = top.origin + lb.get_column(2) * r;
		const Vector3 up = lb.get_column(1);
		const int rim = 16;
		for (int k = 0; k < rim; k++) {
			const float a = k * Math::TAU / rim;
			const Vector3 radial = lb.get_column(0) * Math::cos(a) + lb.get_column(2) * Math::sin(a);
			if (radial.dot(lb.get_column(2)) < -0.75f) {
				continue; // Not over the stalk.
			}
			const Vector3 foot = centre + radial * r * 0.95f;
			const Vector3 td = (radial + up * 0.9f).normalized();
			const float len = _rand(0.0035f, 0.006f);
			const Transform3D tip = _add_stem(Transform3D(bog_frame(td, up), foot), len, 0.00018f, 0.00012f, 0.3f, 2, 0.0f, SURFACE_PETAL);
			_add_head(SURFACE_CENTRE, tip, 0.0005f, 0.0009f, Color(1, 1, 1), 2, 5);
		}
		for (int k = 0; k < 7; k++) {
			const float a = k * BOG_GOLDEN;
			const float u = Math::sqrt((k + 0.5f) / 7.0f) * 0.6f;
			const Vector3 radial = lb.get_column(0) * Math::cos(a) + lb.get_column(2) * Math::sin(a);
			const Vector3 foot = centre + radial * r * u;
			const Transform3D tip = _add_stem(Transform3D(bog_frame((up + radial * 0.3f).normalized(), radial), foot), _rand(0.0012f, 0.0022f), 0.00016f, 0.00012f, 0.0f, 1, 0.0f, SURFACE_PETAL);
			_add_head(SURFACE_CENTRE, tip, 0.00045f, 0.0008f, Color(1, 1, 1), 2, 5);
		}
	}
	const int stems = int(_rand(0.0f, 1.99f));
	for (int s = 0; s < stems; s++) {
		float dead = 0.0f;
		const Bloom bloom = _bloom(false, nullptr, &dead);
		if (bloom == BLOOM_NONE) {
			continue;
		}
		const Wither wither(this, dead);
		const Transform3D top = _add_stem(bog_base(_rand(0.0f, Math::TAU), _rand(1.4f, 1.55f)), _rand(0.06f, 0.16f), 0.0006f, 0.0005f, 0.6f, 6, 0.0f);
		// A one-sided spike, the top curled; a few small white five-petalled flowers.
		for (int k = 0; k < 3; k++) {
			const Vector3 c = top.origin - top.basis.get_column(2) * 0.006f * k;
			if (bloom == BLOOM_SEED) {
				_add_head(SURFACE_STEM, Transform3D(top.basis, c), 0.0012f, 0.003f);
				continue;
			}
			_add_cup_flower(Transform3D(bog_frame(Vector3(0, 1, 0), Vector3(1, 0, 0)), c), 0.0028f);
		}
	}
}

// The bog sets (docs/ideas/vegetation.md, "Bog and moor plants"): sphagnum hummocks and lawns in a
// hummock and hollow pattern, cotton grass, bog asphodel and sundew in the wet lawns, heather on the
// hummocks; where the ground stays wet (the topographic wetness index) on gentle slopes.
TypedArray<Resource> PlantGenerator::make_bog(BogStand p_stand, int p_variants, bool p_textures, const Dictionary &p_season) {
	using P = PlantGenerator;
	struct BogSpec {
		Preset preset;
		float spacing, footprint, density, scale_min, scale_max;
		Color leaf; // Sphagnum: the species' colour (alpha 0: the preset's).
		int patch; // 1: on the hummocks, -1: in the hollows between them, 0: anywhere.
		float visibility;
		float wet_min; // Wetness index.
	};
	const float hummock_scale = 3.0f; // Hummocks 1 to 2 m apart: the patch noise's scale.
	LocalVector<BogSpec> set;
	const Color red(0.42f, 0.11f, 0.08f, 1.0f); // S. capillifolium on the hummock tops.
	const Color ochre(0.45f, 0.38f, 0.13f, 1.0f); // S. papillosum, low hummocks and lawns.
	const Color green(0.3f, 0.42f, 0.1f, 1.0f); // S. cuspidatum in the wet hollows.
	switch (p_stand) {
		case BOG_BLANKET:
			set = LocalVector<BogSpec>({ { P::PRESET_SPHAGNUM, 0.6f, 0.3f, 0.9f, 0.8f, 1.3f, red, 1, 40.0f, 9.0f },
					{ P::PRESET_HEATHER, 0.35f, 0.12f, 0.7f, 0.7f, 1.0f, Color(), 1, 40.0f, 8.0f },
					{ P::PRESET_SPHAGNUM, 0.55f, 0.25f, 0.8f, 1.0f, 1.5f, ochre, 0, 40.0f, 9.5f },
					{ P::PRESET_COTTONGRASS, 0.3f, 0.06f, 0.75f, 0.8f, 1.2f, Color(), -1, 50.0f, 9.0f },
					{ P::PRESET_BOG_ASPHODEL, 0.22f, 0.04f, 0.6f, 0.8f, 1.2f, Color(), -1, 30.0f, 9.0f },
					{ P::PRESET_SUNDEW, 0.15f, 0.0f, 0.5f, 0.8f, 1.2f, Color(), -1, 12.0f, 10.0f } });
			break;
		case BOG_VALLEY:
			set = LocalVector<BogSpec>({ { P::PRESET_SPHAGNUM, 0.55f, 0.25f, 0.9f, 1.0f, 1.6f, green, -1, 40.0f, 10.0f },
					{ P::PRESET_SPHAGNUM, 0.6f, 0.3f, 0.8f, 0.8f, 1.2f, ochre, 1, 40.0f, 9.5f },
					{ P::PRESET_COTTONGRASS, 0.25f, 0.06f, 0.9f, 0.8f, 1.25f, Color(), 0, 50.0f, 9.5f },
					{ P::PRESET_BOG_ASPHODEL, 0.25f, 0.04f, 0.4f, 0.8f, 1.2f, Color(), 1, 30.0f, 9.5f },
					{ P::PRESET_SUNDEW, 0.15f, 0.0f, 0.6f, 0.8f, 1.2f, Color(), -1, 12.0f, 10.0f },
					{ P::PRESET_RUSH, 0.6f, 0.15f, 0.5f, 0.8f, 1.1f, Color(), 0, 30.0f, 10.0f } });
			break;
		case BOG_MAX:
			break;
	}
	TypedArray<Resource> out;
	int seed_base = 8000 + int(p_stand) * 100;
	for (const BogSpec &f : set) {
		Ref<PlantGenerator> gen;
		gen.instantiate();
		gen->set_textures(p_textures);
		gen->set_preset(f.preset);
		gen->apply_preset_colors();
		if (f.leaf.a > 0.0f) {
			gen->set_leaf_color(Color(f.leaf.r, f.leaf.g, f.leaf.b));
			gen->set_centre_color(Color(f.leaf.r, f.leaf.g, f.leaf.b) * 0.72f);
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
		sp->set_align_to_ground(f.preset == P::PRESET_SPHAGNUM ? 1.0f : 0.4f);
		// Peat lies on gentle ground only; out of open water; where water gathers.
		sp->set_rule_range(PlantSpecies::RULE_SLOPE, Vector2(0.0f, 8.0f));
		sp->set_rule_fade(PlantSpecies::RULE_SLOPE, 3.0f);
		sp->set_rule_range(PlantSpecies::RULE_WETNESS, Vector2(f.wet_min, 1000.0f));
		sp->set_rule_fade(PlantSpecies::RULE_WETNESS, 1.5f);
		if (f.patch != 0) {
			sp->set_patch_scale(hummock_scale);
			sp->set_patch_cover(0.4f);
			sp->set_patch_seed(8000 + int(p_stand)); // One hummock pattern for the whole set.
			sp->set_patch_invert(f.patch < 0);
		}
		sp->set_visibility_range(f.visibility);
		sp->set_meta("bog", true);
		tag_species(sp, f.preset);
		out.push_back(sp);
		seed_base += 11;
	}
	return out;
}

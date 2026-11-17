/**************************************************************************/
/*  plant_heath.cpp                                                       */
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


// Atlantic heath (docs/ideas/vegetation.md, "Next round candidates (17 Nov)"): the dwarf shrubs of
// acid, oceanic heath beside heather. Measurements from Stace (2019) and the Biological Flora
// accounts: Bannister 1965 (Erica cinerea), Bannister 1966 (Erica tetralix), Bell and Tallis 1973
// (Empetrum nigrum).

#include "plant_generator.h"
#include "plant_species.h"

namespace {
const float HE_GOLDEN = 2.39996323f;

Basis he_frame(const Vector3 &p_forward, const Vector3 &p_up_hint) {
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

Vector3 he_dir(float p_azimuth, float p_elevation) {
	return Vector3(Math::cos(p_azimuth) * Math::cos(p_elevation), Math::sin(p_elevation), Math::sin(p_azimuth) * Math::cos(p_elevation));
}
} // namespace

// Bell heather (Erica cinerea): a dwarf shrub 15 to 60 cm, wiry, much-branched woody stems; the
// leaves needle-like, 4 to 7 mm, rolled under, dark glossy green, in whorls of three, with leafy
// short shoots crowded in their axils so the shoots look bushy; crimson-purple, egg-shaped bells
// 5 to 7 mm in short terminal racemes and whorls, July to September. The driest, sunniest heath.
void PlantGenerator::_build_bell_heather() {
	leaf_wear = 0.1f;
	Leaf nd;
	nd.shape = LEAF_LINEAR;
	nd.width = 0.002f; // A rolled needle reads about this wide; a little over so the mass holds at range.
	nd.fold = 0.25f;
	nd.bend = 0.15f;
	nd.segments = 1;
	float progress = 0.0f, dead = 0.0f;
	const Bloom bloom = _bloom(false, &progress, &dead);
	// One leafy shoot: whorls of three needles every 3 to 4 mm, the bell cluster at the tip.
	auto shoot = [&](const Transform3D &p_base, float p_len, float p_r, float p_wind, bool p_flower) {
		const LocalVector<Transform3D> fr = _stem_frames(p_base, p_len, _rand(-0.3f, 0.2f), 3);
		_add_stem_along(fr, p_r, p_r * 0.5f, p_wind);
		const float spike = p_flower ? _rand(0.25f, 0.4f) : 0.0f;
		const int whorls = MAX(int(p_len * (1.0f - spike) / 0.0058f), 2);
		for (int k = 0; k < whorls; k++) {
			const float s = 0.08f + (1.0f - spike - 0.08f) * float(k) / MAX(whorls - 1, 1);
			const Transform3D at = _frame_at(fr, s);
			for (int q = 0; q < 3; q++) {
				const float a = q * Math::TAU / 3.0f + (k & 1) * Math::PI / 3.0f;
				const Vector3 radial = at.basis.get_column(0) * Math::cos(a) + at.basis.get_column(1) * Math::sin(a);
				nd.length = _rand(0.005f, 0.007f);
				_add_leaf(SURFACE_LEAF, Transform3D(he_frame((radial * 0.9f + at.basis.get_column(2)).normalized(), at.basis.get_column(2)), at.origin), nd, Math::lerp(p_wind, 1.0f, s));
				// The leafy short shoots in the axils that make bell heather look bushy: a tuft of
				// small needles fanning out beside each leaf.
				if (_rand(0.0f, 1.0f) < 0.25f) {
					const Vector3 tb = at.origin + radial * 0.0015f;
					const Vector3 ta = (radial * 0.5f + at.basis.get_column(2)).normalized();
					for (int t = 0; t < 3; t++) {
						const float ta2 = t * HE_GOLDEN;
						const Vector3 side = ta.cross(radial).normalized();
						const Vector3 td = (ta + radial * Math::cos(ta2) * 0.7f + side * Math::sin(ta2) * 0.7f).normalized();
						nd.length = _rand(0.0035f, 0.005f);
						_add_leaf(SURFACE_LEAF, Transform3D(he_frame(td, ta), tb), nd, Math::lerp(p_wind, 1.0f, s));
					}
				}
			}
		}
		if (!p_flower) {
			return;
		}
		const Wither w(this, bloom == BLOOM_SEED ? 0.7f : dead * 0.6f); // Spent bells brown on the shoots.
		const int bells = int(_rand(5.0f, 10.0f));
		for (int k = 0; k < bells; k++) {
			const float s = 1.0f - spike + spike * (k + 0.5f) / bells;
			const Transform3D at = _frame_at(fr, s);
			const float a = k * HE_GOLDEN;
			const Vector3 radial = at.basis.get_column(0) * Math::cos(a) + at.basis.get_column(1) * Math::sin(a);
			// Each bell on a short stalk, hanging out and down from the axis.
			const Vector3 d = (radial * 0.8f - at.basis.get_column(2) * 0.35f + Vector3(0, -0.2f, 0)).normalized();
			const Vector3 bat = at.origin + radial * 0.0015f;
			// The corolla: a closed, egg-shaped (urceolate) bell 5 to 7 mm.
			const float bl = _rand(0.005f, 0.0065f);
			_add_head(SURFACE_PETAL, Transform3D(he_frame(d, Vector3(0, 1, 0)), bat), bl * 0.42f, bl, Color(1, 1, 1), 2, 6);
		}
	};
	// A domed cushion: short woody stems spreading from the base, each crowded with leafy shoots
	// that fan out and up so the foliage closes over the wood.
	const int stems = int(_rand(8.0f, 12.0f));
	const float spin = _rand(0.0f, Math::TAU);
	for (int i = 0; i < stems; i++) {
		const float az = spin + i * HE_GOLDEN;
		const Transform3D base(he_frame(he_dir(az, _rand(0.55f, 1.2f)), Vector3(0, 1, 0)), Vector3(_rand(-0.03f, 0.03f), 0.0f, _rand(-0.03f, 0.03f)));
		const float len = _rand(0.06f, 0.15f);
		const float bend = _rand(-0.4f, 0.0f);
		const LocalVector<Transform3D> fr = _stem_frames(base, len, bend, 4);
		_add_stem_along(fr, 0.0022f, 0.0012f, 0.0f);
		const Vector3 outward = Vector3(Math::cos(az), 0.0f, Math::sin(az));
		const int subs = int(_rand(10.0f, 15.0f));
		for (int k = 0; k < subs; k++) {
			const float u = _rand(0.2f, 1.0f);
			const Transform3D at = _frame_at(fr, u);
			const float a = _rand(0.0f, Math::TAU);
			const Vector3 d = (at.basis.get_column(2) * 0.5f + outward * 0.5f + Vector3(Math::cos(a) * 0.5f, 0.55f, Math::sin(a) * 0.5f)).normalized();
			const bool flower = bloom != BLOOM_NONE && u > 0.5f && _rand(0.0f, 1.0f) < 0.75f;
			shoot(Transform3D(he_frame(d, Vector3(0, 1, 0)), at.origin), _rand(0.03f, 0.07f), 0.0008f, 0.4f, flower);
		}
		shoot(fr[fr.size() - 1], _rand(0.03f, 0.06f), 0.0009f, 0.5f, bloom != BLOOM_NONE);
	}
	plant_height = MAX(plant_height, 0.1f);
}

// Cross-leaved heath (Erica tetralix): a straggling dwarf shrub 10 to 60 cm of the wet heath;
// grey-green, downy leaves 2 to 4 mm in whorls of four (the "cross"), pressed up the shoot; pale
// rose-pink bells 6 to 9 mm, nodding in a tight terminal umbel of 4 to 12, July to September.
void PlantGenerator::_build_cross_leaved_heath() {
	leaf_wear = 0.1f;
	Leaf nd;
	nd.shape = LEAF_LINEAR;
	nd.width = 0.0021f;
	nd.fold = 0.25f;
	nd.bend = 0.1f;
	nd.segments = 1;
	float progress = 0.0f, dead = 0.0f;
	const Bloom bloom = _bloom(false, &progress, &dead);
	const int stems = int(_rand(4.0f, 8.0f));
	const float spin = _rand(0.0f, Math::TAU);
	for (int i = 0; i < stems; i++) {
		const float az = spin + i * HE_GOLDEN;
		// Straggling: the stems lean out and up.
		const Transform3D base(he_frame(he_dir(az, _rand(0.6f, 1.2f)), Vector3(0, 1, 0)), Vector3(_rand(-0.03f, 0.03f), 0.0f, _rand(-0.03f, 0.03f)));
		const float len = _rand(0.1f, 0.26f);
		const LocalVector<Transform3D> fr = _stem_frames(base, len, -0.35f, 5);
		_add_stem_along(fr, 0.0018f, 0.0009f, 0.0f);
		// Few branches, each an upright leafy shoot.
		LocalVector<LocalVector<Transform3D>> shoots;
		shoots.push_back(LocalVector<Transform3D>(fr));
		const int subs = int(_rand(4.0f, 8.0f));
		for (int k = 0; k < subs; k++) {
			const Transform3D at = _frame_at(fr, _rand(0.2f, 0.8f));
			const float a = _rand(0.0f, Math::TAU);
			const Vector3 d = (Vector3(Math::cos(a) * 0.4f, 1.0f, Math::sin(a) * 0.4f)).normalized();
			const LocalVector<Transform3D> br = _stem_frames(Transform3D(he_frame(d, Vector3(0, 1, 0)), at.origin), len * _rand(0.35f, 0.6f), -0.1f, 3);
			_add_stem_along(br, 0.001f, 0.0006f, 0.3f);
			shoots.push_back(LocalVector<Transform3D>(br));
		}
		for (const LocalVector<Transform3D> &sh : shoots) {
			const float l = sh[0].origin.distance_to(sh[sh.size() - 1].origin);
			const int whorls = MAX(int(l / 0.0048f), 3);
			for (int k = 0; k < whorls; k++) {
				const float s = 0.05f + 0.9f * float(k) / MAX(whorls - 1, 1);
				const Transform3D at = _frame_at(sh, s);
				for (int q = 0; q < 4; q++) {
					const float a = q * Math::PI * 0.5f + (k & 1) * Math::PI * 0.25f;
					const Vector3 radial = at.basis.get_column(0) * Math::cos(a) + at.basis.get_column(1) * Math::sin(a);
					nd.length = _rand(0.0045f, 0.006f);
					_add_leaf(SURFACE_LEAF, Transform3D(he_frame((radial * 0.75f + at.basis.get_column(2)).normalized(), at.basis.get_column(2)), at.origin), nd, Math::lerp(0.3f, 1.0f, s)); // Spreading up the shoot.
				}
			}
			if (bloom == BLOOM_NONE) {
				continue;
			}
			// The umbel: bells nodding round the tip, all to one side a little.
			const Wither w(this, bloom == BLOOM_SEED ? 0.7f : dead * 0.6f);
			const Transform3D tip = sh[sh.size() - 1];
			const int bells = int(_rand(4.0f, 12.0f));
			for (int k = 0; k < bells; k++) {
				const float a = k * Math::TAU / bells + _rand(-0.2f, 0.2f);
				const Vector3 radial = tip.basis.get_column(0) * Math::cos(a) + tip.basis.get_column(1) * Math::sin(a);
				const Vector3 bat = tip.origin + radial * 0.002f;
				const Vector3 d = (radial * 0.55f + Vector3(0, -0.8f, 0)).normalized(); // Nodding.
				// An egg-shaped bell 6 to 9 mm, nodding.
				const float bl = _rand(0.006f, 0.008f);
				_add_head(SURFACE_PETAL, Transform3D(he_frame(d, Vector3(0, 1, 0)), bat), bl * 0.4f, bl, Color(1, 1, 1), 2, 6);
			}
		}
	}
	plant_height = MAX(plant_height, 0.08f);
}

// Crowberry (Empetrum nigrum): a prostrate evergreen dwarf shrub, trailing stems 20 to 60 cm long,
// rooting as they go, into mats; leaves 4 to 6 mm, crowded spirally, needle-like with the margins
// rolled under to a white line beneath, glossy dark green, bronzing in winter; tiny purple flowers
// in spring, then black berries 5 mm, August to October.
void PlantGenerator::_build_crowberry() {
	leaf_wear = 0.05f;
	Leaf nd;
	nd.shape = LEAF_LINEAR;
	nd.width = 0.002f;
	nd.fold = 0.45f; // Rolled.
	nd.bend = 0.2f;
	nd.segments = 1;
	const int day = bloom_day;
	const bool berries = day >= 215 && day <= 300;
	const int stems = int(_rand(10.0f, 16.0f));
	const float spin = _rand(0.0f, Math::TAU);
	for (int i = 0; i < stems; i++) {
		const float az = spin + i * HE_GOLDEN + _rand(-0.2f, 0.2f);
		const float len = _rand(0.08f, 0.2f);
		// Trailing: out over the ground, the tip turning up a little.
		const Transform3D base(he_frame(he_dir(az, _rand(0.08f, 0.3f)), Vector3(0, 1, 0)), Vector3());
		const LocalVector<Transform3D> fr = _stem_frames(base, len, 0.15f, 6);
		_add_stem_along(fr, 0.0016f, 0.0009f, 0.0f);
		LocalVector<LocalVector<Transform3D>> shoots;
		shoots.push_back(LocalVector<Transform3D>(fr));
		const int subs = int(_rand(4.0f, 8.0f));
		for (int k = 0; k < subs; k++) {
			const Transform3D at = _frame_at(fr, _rand(0.2f, 0.9f));
			const float side = (k & 1) ? 1.0f : -1.0f;
			const Vector3 d = (at.basis.get_column(2) + at.basis.get_column(0) * side * 0.9f + Vector3(0, 0.35f, 0)).normalized();
			const LocalVector<Transform3D> br = _stem_frames(Transform3D(he_frame(d, Vector3(0, 1, 0)), at.origin), len * _rand(0.25f, 0.5f), -0.15f, 3);
			_add_stem_along(br, 0.0009f, 0.0006f, 0.3f);
			shoots.push_back(LocalVector<Transform3D>(br));
		}
		for (const LocalVector<Transform3D> &sh : shoots) {
			const float l = sh[0].origin.distance_to(sh[sh.size() - 1].origin);
			// The leaves crowded spirally, densest on the young upper half of each shoot.
			const int n = MAX(int(l / 0.0018f), 4);
			for (int k = 0; k < n; k++) {
				const float s = 0.12f + 0.88f * float(k) / MAX(n - 1, 1);
				const Transform3D at = _frame_at(sh, s);
				const float a = k * HE_GOLDEN;
				const Vector3 radial = at.basis.get_column(0) * Math::cos(a) + at.basis.get_column(1) * Math::sin(a);
				nd.length = _rand(0.005f, 0.007f);
				_add_leaf(SURFACE_LEAF, Transform3D(he_frame((radial + at.basis.get_column(2) * 0.5f + Vector3(0, 0.3f, 0)).normalized(), at.basis.get_column(2)), at.origin), nd, Math::lerp(0.2f, 1.0f, s));
			}
			if (berries && _rand(0.0f, 1.0f) < 0.5f) {
				// One to three black berries in the leaf axils behind the tip.
				const int nb = int(_rand(1.0f, 3.99f));
				for (int k = 0; k < nb; k++) {
					const Transform3D at = _frame_at(sh, _rand(0.55f, 0.8f));
					const Vector3 o = Vector3(_rand(-1.0f, 1.0f), _rand(0.2f, 0.8f), _rand(-1.0f, 1.0f)).normalized() * 0.0035f;
					_add_head(SURFACE_CENTRE, Transform3D(Basis(), at.origin + o), 0.0026f, 0.005f, Color(1, 1, 1), 4, 8);
				}
			}
		}
	}
	plant_height = MAX(plant_height, 0.03f);
}

// The heath sets (Gimingham 1972, "Ecology of Heathlands"; Rodwell 1991, H1 to H22). Heath is
// oceanic: wet enough (over about 700 mm a year) and mild in winter (bell heather is cut back
// where January means fall below about -4 C; Bannister 1965), open (no canopy), below the alpine
// belt, and sorted by water: dry heath on free-draining ground, wet heath where water gathers but
// short of the bog, maritime heath on exposed cliff tops, wind-pruned low.
TypedArray<Resource> PlantGenerator::make_heath(HeathStand p_stand, int p_variants, bool p_textures, const Dictionary &p_season) {
	using P = PlantGenerator;
	struct HeathSpec {
		Preset preset;
		float spacing, footprint, density, scale_min, scale_max;
		float patch_scale; // 0: no patches.
		float patch_cover;
		float visibility;
		float align;
	};
	LocalVector<HeathSpec> set;
	switch (p_stand) {
		case HEATH_DRY:
			// Heather dominant in its growth-phase patchwork, bell heather on the warmest, driest
			// spots, a little crowberry where it is cooler.
			set = LocalVector<HeathSpec>({ { P::PRESET_HEATHER, 0.35f, 0.12f, 0.9f, 0.6f, 1.4f, 6.0f, 0.65f, 35.0f, 0.5f },
					{ P::PRESET_BELL_HEATHER, 0.3f, 0.1f, 0.6f, 0.7f, 1.3f, 4.0f, 0.45f, 30.0f, 0.5f },
					{ P::PRESET_CROWBERRY, 0.4f, 0.12f, 0.25f, 0.8f, 1.3f, 5.0f, 0.35f, 25.0f, 0.9f },
					{ P::PRESET_DRY_GRASS, 0.25f, 0.03f, 0.35f, 0.5f, 0.8f, 0.0f, 0.0f, 25.0f, 0.5f } });
			break;
		case HEATH_WET:
			// Cross-leaved heath with heather, cottongrass tufts and sphagnum in the wettest spots.
			set = LocalVector<HeathSpec>({ { P::PRESET_CROSS_LEAVED_HEATH, 0.25f, 0.08f, 0.8f, 0.7f, 1.3f, 4.0f, 0.55f, 30.0f, 0.5f },
					{ P::PRESET_HEATHER, 0.4f, 0.12f, 0.6f, 0.6f, 1.1f, 6.0f, 0.5f, 35.0f, 0.5f },
					{ P::PRESET_COTTONGRASS, 0.45f, 0.06f, 0.35f, 0.7f, 1.1f, 0.0f, 0.0f, 40.0f, 0.4f },
					{ P::PRESET_SPHAGNUM, 0.6f, 0.25f, 0.4f, 0.8f, 1.2f, 3.0f, 0.35f, 35.0f, 1.0f } });
			break;
		case HEATH_MARITIME:
			// Cliff-top heath: heather and bell heather pruned flat by salt wind, with thrift and
			// crowberry mats.
			set = LocalVector<HeathSpec>({ { P::PRESET_HEATHER, 0.3f, 0.1f, 0.85f, 0.45f, 0.75f, 5.0f, 0.6f, 35.0f, 0.8f },
					{ P::PRESET_BELL_HEATHER, 0.28f, 0.08f, 0.65f, 0.5f, 0.85f, 4.0f, 0.5f, 30.0f, 0.8f },
					{ P::PRESET_THRIFT, 0.35f, 0.05f, 0.5f, 0.8f, 1.2f, 3.0f, 0.45f, 30.0f, 0.8f },
					{ P::PRESET_CROWBERRY, 0.4f, 0.12f, 0.35f, 0.8f, 1.2f, 5.0f, 0.4f, 25.0f, 0.9f } });
			break;
		case HEATH_MAX:
			break;
	}
	TypedArray<Resource> out;
	int seed_base = 10300 + int(p_stand) * 100;
	for (const HeathSpec &f : set) {
		Ref<PlantGenerator> gen;
		gen.instantiate();
		gen->set_textures(p_textures);
		gen->set_preset(f.preset);
		gen->apply_preset_colors();
		if (f.preset == P::PRESET_DRY_GRASS) {
			// Bristle bent and wavy hair-grass: fine, wiry, grey-green to straw.
			gen->set_leaf_color(Color(0.3f, 0.34f, 0.2f));
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
		// Oceanic, open, below the alpine belt (which takes over from the season's 6.4 C treeline).
		sp->set_rule_range(PlantSpecies::RULE_PRECIPITATION, Vector2(700.0f, 100000.0f));
		sp->set_rule_fade(PlantSpecies::RULE_PRECIPITATION, 150.0f);
		sp->set_rule_range(PlantSpecies::RULE_JANUARY_TEMPERATURE, Vector2(-4.0f, 100.0f));
		sp->set_rule_fade(PlantSpecies::RULE_JANUARY_TEMPERATURE, 1.5f);
		sp->set_rule_range(PlantSpecies::RULE_SEASON_TEMPERATURE, Vector2(6.4f, 100.0f));
		sp->set_rule_fade(PlantSpecies::RULE_SEASON_TEMPERATURE, 0.8f);
		sp->set_rule_range(PlantSpecies::RULE_SHADE, Vector2(0.0f, 0.3f));
		sp->set_rule_fade(PlantSpecies::RULE_SHADE, 0.15f);
		sp->set_rule_range(PlantSpecies::RULE_ROCK, Vector2(0.0f, 0.5f));
		sp->set_rule_fade(PlantSpecies::RULE_ROCK, 0.15f);
		switch (p_stand) {
			case HEATH_DRY:
				sp->set_rule_range(PlantSpecies::RULE_WETNESS, Vector2(-1000.0f, 7.5f));
				sp->set_rule_fade(PlantSpecies::RULE_WETNESS, 1.0f);
				sp->set_rule_range(PlantSpecies::RULE_SLOPE, Vector2(0.0f, 35.0f));
				sp->set_rule_fade(PlantSpecies::RULE_SLOPE, 5.0f);
				if (f.preset == P::PRESET_BELL_HEATHER) {
					// The sunniest, driest slopes (Bannister 1965).
					sp->set_rule_range(PlantSpecies::RULE_SUNLIGHT, Vector2(150.0f, 2000.0f));
					sp->set_rule_fade(PlantSpecies::RULE_SUNLIGHT, 25.0f);
				} else if (f.preset == P::PRESET_CROWBERRY) {
					sp->set_rule_range(PlantSpecies::RULE_SUNLIGHT, Vector2(0.0f, 170.0f));
					sp->set_rule_fade(PlantSpecies::RULE_SUNLIGHT, 25.0f);
				}
				break;
			case HEATH_WET:
				// Where water gathers, short of the bog's wettest ground (wetness 9 and up).
				sp->set_rule_range(PlantSpecies::RULE_WETNESS, Vector2(7.0f, f.preset == P::PRESET_SPHAGNUM ? 1000.0f : 10.0f));
				sp->set_rule_fade(PlantSpecies::RULE_WETNESS, 1.0f);
				sp->set_rule_range(PlantSpecies::RULE_SLOPE, Vector2(0.0f, 15.0f));
				sp->set_rule_fade(PlantSpecies::RULE_SLOPE, 4.0f);
				break;
			case HEATH_MARITIME:
				// Cliff tops open to the sea's wind and salt, above the reach of the waves.
				sp->set_rule_range(PlantSpecies::RULE_COAST_EXPOSURE, Vector2(0.4f, 1.0f));
				sp->set_rule_fade(PlantSpecies::RULE_COAST_EXPOSURE, 0.15f);
				sp->set_rule_range(PlantSpecies::RULE_TIDE_HEIGHT, Vector2(6.0f, 150.0f));
				sp->set_rule_fade(PlantSpecies::RULE_TIDE_HEIGHT, 3.0f);
				sp->set_rule_range(PlantSpecies::RULE_SLOPE, Vector2(0.0f, 30.0f));
				sp->set_rule_fade(PlantSpecies::RULE_SLOPE, 5.0f);
				break;
			case HEATH_MAX:
				break;
		}
		if (f.patch_scale > 0.0f) {
			sp->set_patch_scale(f.patch_scale);
			sp->set_patch_cover(f.patch_cover);
			sp->set_patch_seed(seed_base);
		}
		sp->set_visibility_range(f.visibility);
		sp->set_meta("heath", true);
		tag_species(sp, f.preset);
		out.push_back(sp);
		seed_base += 19;
	}
	return out;
}

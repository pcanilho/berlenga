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

// Sand dune plants (docs/ideas/vegetation.md, "Dune grass"): the grasses that build dunes by
// trapping blown sand, and sea holly. Measurements from Stace, "New Flora of the British Isles"
// (2019), Huiskes (1979, Biological Flora: Ammophila arenaria) and Rodwell, "British Plant
// Communities" vol. 5 (SD4 to SD8 dunes). The shelter they give the sand is
// VegetationField3D.sample_lateral_cover(), from each plant's frontal area.

#include "plant_generator.h"
#include "plant_species.h"

namespace {
const float DUNE_GOLDEN = 2.39996323f;

Basis dune_frame(const Vector3 &p_forward, const Vector3 &p_up_hint) {
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

Vector3 dune_dir(float p_azimuth, float p_elevation) {
	return Vector3(Math::cos(p_azimuth) * Math::cos(p_elevation), Math::sin(p_elevation), Math::sin(p_azimuth) * Math::cos(p_elevation));
}

Transform3D dune_base(float p_azimuth, float p_elevation, const Vector3 &p_at = Vector3()) {
	return Transform3D(dune_frame(dune_dir(p_azimuth, p_elevation), Vector3(0, 1, 0)), p_at);
}
} // namespace

// A dense, tapering grass spike or spike-like panicle along the frame's z: two domes back to back,
// the long one towards the tip.
void PlantGenerator::_add_dune_spike(const Transform3D &p_top, float p_length, float p_radius) {
	const Basis f = p_top.basis.orthonormalized();
	const Vector3 mid = p_top.origin + f.get_column(2) * p_length * 0.35f;
	_add_head(SURFACE_CENTRE, Transform3D(f, mid), p_radius, p_length * 0.65f, Color(1, 1, 1), 5, 8);
	_add_head(SURFACE_CENTRE, Transform3D(dune_frame(-f.get_column(2), f.get_column(1)), mid), p_radius, p_length * 0.35f, Color(1, 1, 1), 3, 8);
	// The spikelets pressed along it break its outline.
	const int n = int(p_length * 160.0f);
	for (int i = 0; i < n; i++) {
		const float t = (i + 0.5f) / n;
		const float a = i * DUNE_GOLDEN;
		const float profile = t < 0.35f ? Math::sqrt(MAX(0.0f, 1.0f - Math::pow((0.35f - t) / 0.35f, 2.0f))) : Math::sqrt(MAX(0.0f, 1.0f - Math::pow((t - 0.35f) / 0.65f, 2.0f)));
		const Vector3 out = f.get_column(0) * Math::cos(a) + f.get_column(1) * Math::sin(a);
		const Vector3 at = p_top.origin + f.get_column(2) * p_length * t + out * p_radius * profile * 0.8f;
		_add_head(SURFACE_CENTRE, Transform3D(dune_frame((f.get_column(2) + out * 0.35f).normalized(), out), at), p_radius * 0.35f, p_radius * 1.1f, Color(1, 1, 1), 2, 5);
	}
}

// Marram (Ammophila arenaria): dense tufts from creeping rhizomes, leaves 50 to 90 cm, grey-green,
// tightly rolled inward so they are stiff and sharp, a third of them dead and straw-coloured inside
// the tuft; stems 60 to 120 cm with a pale, dense, spike-like panicle 7 to 22 cm, July and August,
// standing bleached through winter. It grows up through burial of up to a metre of sand a year.
void PlantGenerator::_build_marram() {
	const int blades = int(_rand(45.0f, 70.0f));
	for (int i = 0; i < blades; i++) {
		const bool old = _rand(0.0f, 1.0f) < 0.3f;
		const Wither wither(this, old ? _rand(0.7f, 1.0f) : 0.0f);
		Leaf l;
		l.shape = LEAF_LINEAR;
		l.length = _rand(0.45f, 0.85f) * (old ? 0.8f : 1.0f);
		l.width = _rand(0.0035f, 0.005f); // Rolled: a few millimetres across.
		l.fold = 0.92f;
		l.bend = old ? _rand(1.0f, 1.9f) : _rand(0.15f, 0.6f); // Stiff; the dead ones flop.
		l.twist = _rand(-0.9f, 0.9f);
		l.segments = 8;
		const Vector3 at(_rand(-0.04f, 0.04f), 0.0f, _rand(-0.04f, 0.04f));
		_add_leaf(SURFACE_LEAF, Transform3D(dune_frame(dune_dir(_rand(0.0f, Math::TAU), old ? _rand(0.7f, 1.2f) : _rand(1.0f, 1.45f)), Vector3(0, 1, 0)), at), l, 0.0f, 0.9f);
	}
	const int culms = int(_rand(0.0f, 3.99f));
	for (int c = 0; c < culms; c++) {
		float progress = 0.0f, dead = 0.0f;
		const Bloom bloom = _bloom(false, &progress, &dead);
		if (bloom == BLOOM_NONE) {
			continue;
		}
		const Wither wither(this, bloom == BLOOM_SEED ? 0.35f : dead * 0.4f); // Bleached, not browned.
		const Transform3D top = _add_stem(dune_base(_rand(0.0f, Math::TAU), _rand(1.4f, 1.53f), Vector3(_rand(-0.03f, 0.03f), 0.0f, _rand(-0.03f, 0.03f))), _rand(0.55f, 0.95f), 0.0022f, 0.0016f, _rand(-0.15f, 0.15f), 8, 0.0f);
		_add_dune_spike(top, _rand(0.1f, 0.2f), _rand(0.007f, 0.01f));
	}
}

// Lyme grass (Leymus arenarius): loose tufts of broad, flat, glaucous blue-grey leaves 30 to 60 cm and
// 8 to 15 mm wide, arching; stout stems 60 to 120 cm with a dense spike 15 to 30 cm, June to August.
// The pioneer of the embryo dunes on the strandline, where it traps the first sand.
void PlantGenerator::_build_lyme_grass() {
	const int blades = int(_rand(14.0f, 24.0f));
	for (int i = 0; i < blades; i++) {
		const bool old = _rand(0.0f, 1.0f) < 0.15f;
		const Wither wither(this, old ? _rand(0.6f, 1.0f) : 0.0f);
		Leaf l;
		l.shape = LEAF_LINEAR;
		l.length = _rand(0.3f, 0.6f);
		l.width = _rand(0.013f, 0.02f); // Broad and flat, the widest dune grass.
		l.fold = 0.2f;
		l.bend = _rand(0.6f, 1.4f);
		l.twist = _rand(-0.5f, 0.5f);
		l.segments = 8;
		const Vector3 at(_rand(-0.025f, 0.025f), 0.0f, _rand(-0.025f, 0.025f));
		_add_leaf(SURFACE_LEAF, Transform3D(dune_frame(dune_dir(_rand(0.0f, Math::TAU), _rand(0.95f, 1.4f)), Vector3(0, 1, 0)), at), l, 0.0f, 0.8f);
	}
	const int culms = int(_rand(0.0f, 2.99f));
	for (int c = 0; c < culms; c++) {
		float progress = 0.0f, dead = 0.0f;
		const Bloom bloom = _bloom(false, &progress, &dead);
		if (bloom == BLOOM_NONE) {
			continue;
		}
		const Wither wither(this, bloom == BLOOM_SEED ? 0.5f : dead * 0.5f);
		const Transform3D top = _add_stem(dune_base(_rand(0.0f, Math::TAU), _rand(1.42f, 1.54f)), _rand(0.5f, 0.9f), 0.003f, 0.0022f, _rand(-0.12f, 0.12f), 8, 0.0f);
		_add_dune_spike(top, _rand(0.15f, 0.28f), _rand(0.008f, 0.011f));
	}
}

// Sea holly (Eryngium maritimum): a stiff, waxy, glaucous blue-white plant 20 to 60 cm; basal leaves
// round, three-lobed, leathery, with a thickened white margin of spines; branched stems with
// clasping leaves, each branch ending in a round head 1.5 to 3 cm of powder-blue flowers in a
// ruff of spiny bracts, July to September.
void PlantGenerator::_build_sea_holly() {
	// Spiny, holly-like blades: five to seven deep points round a stiff, wavy disc.
	const int basal = int(_rand(4.0f, 7.99f));
	const float spin = _rand(0.0f, Math::TAU);
	for (int i = 0; i < basal; i++) {
		const float az = spin + i * DUNE_GOLDEN;
		const Transform3D stalk = _add_stem(dune_base(az, _rand(0.5f, 0.9f)), _rand(0.03f, 0.07f), 0.0025f, 0.002f, 0.2f, 4, 0.0f);
		const Vector3 d = (dune_dir(az, 0.0f) + Vector3(0, _rand(0.3f, 0.7f), 0)).normalized();
		_add_disc_leaf(Transform3D(dune_frame(d, Vector3(0, 1, 0)), stalk.origin), _rand(0.035f, 0.05f), 0.35f, 0.0f, 0.14f, int(_rand(5.0f, 7.99f)), 0.9f, 4, 48, 0.32f);
	}
	const int stems = int(_rand(1.0f, 2.99f));
	for (int s = 0; s < stems; s++) {
		float progress = 0.0f, dead = 0.0f;
		const Bloom bloom = _bloom(false, &progress, &dead);
		const Wither wither(this, dead);
		const float az = _rand(0.0f, Math::TAU);
		// Stout, branching low: a stiff, rounded bush rather than a stalk.
		Transform3D top = _add_stem(dune_base(az, _rand(1.25f, 1.45f)), _rand(0.07f, 0.14f), 0.0055f, 0.0045f, _rand(-0.2f, 0.2f), 5, 0.0f);
		for (int tier = 0; tier < 2; tier++) {
			const int branches = int(_rand(2.0f, 3.99f));
			Transform3D next = top;
			for (int k = 0; k < branches; k++) {
				const float ba = az + tier * 1.1f + k * Math::TAU / branches + _rand(-0.3f, 0.3f);
				const Vector3 bd = (dune_dir(ba, 0.0f) * 0.8f + Vector3(0, 1, 0)).normalized();
				const Transform3D br = _add_stem(Transform3D(dune_frame(bd, Vector3(0, 1, 0)), top.origin), _rand(0.05f, 0.1f) * (1.0f - 0.3f * tier), 0.0038f, 0.003f, 0.15f, 4, 0.6f);
				// A clasping leaf at the fork, spiny like the basal ones.
				_add_disc_leaf(Transform3D(dune_frame((dune_dir(ba, 0.0f) + Vector3(0, 0.6f, 0)).normalized(), Vector3(0, 1, 0)), top.origin), _rand(0.02f, 0.03f), 0.5f, -0.05f, 0.12f, 5, 0.8f, 3, 40, 0.38f);
				if (k == 0 && tier == 0) {
					next = br;
				}
				if (tier == 0 && k == 0) {
					continue; // The leader branches again.
				}
				if (bloom == BLOOM_NONE) {
					continue;
				}
				// The head: an ovoid of tiny flowers, blue once open, brown once over.
				const float hr = _rand(0.008f, 0.012f) * (0.7f + 0.3f * CLAMP(progress * 3.0f, 0.0f, 1.0f));
				const Basis hb = dune_frame(br.basis.get_column(2), Vector3(0, 1, 0));
				_add_head(SURFACE_CENTRE, Transform3D(hb, br.origin), hr, hr * 1.4f, Color(1, 1, 1), 4, 10);
				_add_head(SURFACE_CENTRE, Transform3D(dune_frame(-hb.get_column(2), hb.get_column(1)), br.origin), hr, hr * 0.5f, Color(1, 1, 1), 2, 10);
				// The ruff: five to seven stiff, spine-tipped bracts spreading under the head.
				const int bracts = int(_rand(5.0f, 7.99f));
				for (int j = 0; j < bracts; j++) {
					const float ja = j * Math::TAU / bracts;
					const Vector3 jd = (hb.get_column(0) * Math::cos(ja) + hb.get_column(1) * Math::sin(ja) - hb.get_column(2) * 0.25f).normalized();
					Leaf bl;
					bl.shape = LEAF_LANCEOLATE;
					bl.length = hr * _rand(1.8f, 2.4f);
					bl.width = hr * 0.5f;
					bl.lobes = 3;
					bl.lobe_depth = 0.45f;
					bl.fold = 0.3f;
					bl.bend = -0.2f;
					bl.segments = 4;
					_add_leaf(SURFACE_LEAF, Transform3D(dune_frame(jd, hb.get_column(2)), br.origin - hb.get_column(2) * hr * 0.2f), bl, 0.9f);
				}
			}
			top = next;
		}
	}
}

// The dune sets (docs/ideas/vegetation.md, "Dune grass"): placed by distance to the water, since the
// sea's strandline is where dunes start; the grasses shelter the sand (wind_frontal_area) when the
// field is in a GroundSurface3D's wind_shelters.
TypedArray<Resource> PlantGenerator::make_dunes(DuneStand p_stand, int p_variants, bool p_textures, const Dictionary &p_season) {
	using P = PlantGenerator;
	struct DuneSpec {
		Preset preset;
		float spacing, footprint, density, scale_min, scale_max;
		Vector2 distance; // Metres from the water.
		Vector2 stability; // Dune stability (PlantSpecies.RULE_DUNE, marram cover) where a generator built the dunes.
		float patch_scale; // 0: no patches (rhizome clones in patches otherwise).
		float patch_cover;
		float visibility;
	};
	LocalVector<DuneSpec> set;
	switch (p_stand) {
		case DUNES_EMBRYO:
			// Embryo dunes on the strandline: lyme grass first, sea couch's role; a few marram.
			set = LocalVector<DuneSpec>({ { P::PRESET_LYME_GRASS, 0.6f, 0.2f, 0.7f, 0.8f, 1.2f, Vector2(2.0f, 40.0f), Vector2(0.0f, 0.3f), 5.0f, 0.45f, 70.0f },
					{ P::PRESET_MARRAM, 0.7f, 0.25f, 0.35f, 0.7f, 1.0f, Vector2(6.0f, 60.0f), Vector2(0.0f, 0.3f), 6.0f, 0.4f, 80.0f },
					{ P::PRESET_SEA_HOLLY, 0.9f, 0.15f, 0.25f, 0.8f, 1.1f, Vector2(4.0f, 60.0f), Vector2(0.0f, 0.3f), 0.0f, 0.0f, 40.0f } });
			break;
		case DUNES_MOBILE:
			// Mobile (yellow) foredunes: marram dominant, its tussocks with bare sand between, lyme
			// grass on the seaward foot, sea holly.
			set = LocalVector<DuneSpec>({ { P::PRESET_MARRAM, 0.45f, 0.2f, 0.85f, 0.8f, 1.3f, Vector2(8.0f, 250.0f), Vector2(0.15f, 0.8f), 7.0f, 0.65f, 80.0f },
					{ P::PRESET_LYME_GRASS, 0.6f, 0.2f, 0.4f, 0.8f, 1.1f, Vector2(5.0f, 60.0f), Vector2(0.15f, 0.8f), 5.0f, 0.35f, 70.0f },
					{ P::PRESET_SEA_HOLLY, 0.8f, 0.15f, 0.35f, 0.8f, 1.2f, Vector2(8.0f, 250.0f), Vector2(0.15f, 0.8f), 0.0f, 0.0f, 40.0f } });
			break;
		case DUNES_FIXED:
			// Fixed (grey) dunes behind: marram thinning out without fresh sand, a dry sward,
			// moss between.
			set = LocalVector<DuneSpec>({ { P::PRESET_MARRAM, 0.9f, 0.2f, 0.4f, 0.6f, 0.95f, Vector2(30.0f, 400.0f), Vector2(0.6f, 1.0f), 6.0f, 0.4f, 80.0f },
					{ P::PRESET_DRY_GRASS, 0.18f, 0.05f, 0.8f, 0.6f, 0.9f, Vector2(30.0f, 400.0f), Vector2(0.6f, 1.0f), 0.0f, 0.0f, 30.0f },
					{ P::PRESET_SEA_HOLLY, 1.2f, 0.15f, 0.2f, 0.7f, 1.0f, Vector2(30.0f, 300.0f), Vector2(0.6f, 1.0f), 0.0f, 0.0f, 40.0f },
					{ P::PRESET_MOSS, 0.3f, 0.0f, 0.5f, 0.8f, 1.4f, Vector2(30.0f, 400.0f), Vector2(0.6f, 1.0f), 4.0f, 0.5f, 25.0f } });
			break;
		case DUNES_MAX:
			break;
	}
	TypedArray<Resource> out;
	int seed_base = 9000 + int(p_stand) * 100;
	for (const DuneSpec &f : set) {
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
		sp->set_align_to_ground(0.3f);
		sp->set_rule_range(PlantSpecies::RULE_WATER_DISTANCE, f.distance);
		sp->set_rule_fade(PlantSpecies::RULE_WATER_DISTANCE, 2.0f);
		// On a generator's dunes, the dune's stability under marram places the sets: embryo and
		// mobile dunes where the sand still moves, fixed (grey) dunes where marram has held it.
		// Off the dune sand (RULE_DUNE -1) none of them grow.
		sp->set_rule_range(PlantSpecies::RULE_DUNE, f.stability);
		sp->set_rule_fade(PlantSpecies::RULE_DUNE, 0.1f);
		sp->set_rule_range(PlantSpecies::RULE_SLOPE, Vector2(0.0f, 34.0f)); // Dry sand's angle of repose.
		sp->set_rule_fade(PlantSpecies::RULE_SLOPE, 4.0f);
		if (f.patch_scale > 0.0f) {
			sp->set_patch_scale(f.patch_scale);
			sp->set_patch_cover(f.patch_cover);
			sp->set_patch_seed(seed_base);
		}
		sp->set_visibility_range(f.visibility);
		sp->set_meta("dunes", true);
		out.push_back(sp);
		seed_base += 13;
	}
	return out;
}

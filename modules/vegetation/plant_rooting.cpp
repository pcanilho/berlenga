/**************************************************************************/
/*  plant_rooting.cpp                                                     */
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

// Boar rooting (docs/ideas/vegetation.md, "Boar rooting"): wild boar dig for roots, bulbs and soil animals, turning the
// topsoil 5-15 cm deep in patches from a square metre to hundreds, mostly in moist, rich woodland and meadow edges.
// The sward and herbs are torn out and left as overturned sods, grass side down; the cover drops by half or more and
// comes back over one to two years, ruderals and nitrophilous herbs first (Bueno, Alados, Gomez-Garcia, Barrio and
// Garcia-Gonzalez 2009, Ecol. Model.; Bueno et al. 2013, Plant Ecol.; Welander 2000, J. Zool.; Barrios-Garcia and
// Ballari 2012, Biol. Invasions). WorldStand.rooting_age_at gives the years since (PlantSpecies.RULE_ROOTING places by
// it); the turned soil itself is ground detail's.

#include "plant_generator.h"
#include "plant_species.h"

namespace {
Basis rooting_frame(const Vector3 &p_forward, const Vector3 &p_up_hint) {
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

Vector3 rooting_dir(float p_azimuth, float p_elevation) {
	return Vector3(Math::cos(p_azimuth) * Math::cos(p_elevation), Math::sin(p_elevation), Math::sin(p_azimuth) * Math::cos(p_elevation));
}
} // namespace

// Lumpy, not smooth: the vertices of a clod added since p_from pushed in and out about p_centre by a 3D value noise at a
// few cm (torn soil breaks along roots and crumbs).
void PlantGenerator::_lump_since(int p_from, const Vector3 &p_centre, float p_radius, float p_amount) {
	PackedVector3Array &v = builders[SURFACE_CENTRE].verts;
	const float off = _rand(0.0f, 100.0f);
	auto noise = [off](const Vector3 &q) {
		return Math::sin(q.x * 61.0f + off) * Math::sin(q.y * 53.0f + off * 1.7f) * Math::sin(q.z * 67.0f + off * 0.6f) * 0.6f + Math::sin(q.x * 137.0f - q.z * 121.0f + off) * Math::sin(q.y * 149.0f + off * 2.3f) * 0.4f;
	};
	for (int i = p_from; i < v.size(); i++) {
		const Vector3 d = v[i] - p_centre;
		const float l = d.length();
		if (l < 1e-6f) {
			continue;
		}
		v.write[i] = p_centre + d * (1.0f + p_amount * noise(v[i]) * MIN(l / p_radius, 1.0f));
	}
}

// Years since the ground was rooted, from the season (make_rooting sets it per age band); without one, a month.
float PlantGenerator::_rooting_age() const {
	return season.has("rooting_age") ? MAX(float(season["rooting_age"]), 0.0f) : 0.08f;
}

// Rooted sods: a patch of turf torn up and flung over by a boar's snout, 3 to 8 clods 4 to 16 cm across, most grass side
// down (pale roots and soil up), some on edge; the torn grass yellows within weeks, the clods slump and crumble over a
// season and green shoots come up between them. The surface: soil clods (centre), roots and grass bases (stem), the
// grass (leaf).
void PlantGenerator::_build_rooted_sod() {
	const float age = _rooting_age();
	// One rooting look game-wide: the terrain's turned humus (TerrainMaterial3D) and ground detail's clods fade by the same
	// freshness, 1 - smoothstep(0.25, 2, years); the sods slump into the soil with it.
	const float settle = Math::smoothstep(0.25f, 2.0f, age); // Rain and frost slump the clods into the soil.
	const float wilt = Math::smoothstep(0.02f, 0.3f, age); // Torn-up grass dries within weeks.
	const float rot = Math::smoothstep(0.3f, 1.5f, age); // Then it rots down and goes.
	const int clods = MAX(int(_rand(3.0f, 8.99f) * (1.0f - 0.35f * settle)), 2);
	for (int c = 0; c < clods; c++) {
		const float a = _rand(0.0f, Math::TAU);
		const float rr = 0.17f * Math::sqrt(_rand(0.0f, 1.0f));
		const Vector3 at(Math::cos(a) * rr, 0.0f, Math::sin(a) * rr);
		const float r = _rand(0.02f, 0.08f);
		const float h = r * _rand(0.45f, 0.85f) * (1.0f - 0.6f * settle);
		const bool flipped = rng.randf() < 0.65f; // Grass side down: the root mat and soil up.
		// Tipped as it landed: up to 50 degrees, less as it slumps.
		const float tip = _rand(0.0f, 0.9f) * (1.0f - 0.6f * settle);
		const Vector3 up = rooting_dir(_rand(0.0f, Math::TAU), float(Math::PI) * 0.5f - tip);
		const Basis b = rooting_frame(up, Vector3(1, 0, 0));
		// The clod: a flattened lump of topsoil, half sunk. Its root-mat side reads paler and fibrous (the tint).
		const float shade = _rand(0.75f, 1.15f);
		// Humus as the terrain draws it (0.062 to 0.11), drying paler with the freshness.
		Color soil = (flipped ? Color(1.1f, 1.02f, 0.92f) * shade : Color(0.9f, 0.85f, 0.8f) * shade).lerp(Color(1.4f, 1.32f, 1.22f), settle * 0.6f);
		soil.a = 1.0f; // A scaled tint scales the alpha, which the shader reads as wilting.
		const Vector3 base = at - Vector3(0, h * (0.5f + 0.35f * settle), 0);
		const int from = builders[SURFACE_CENTRE].verts.size();
		_add_head(SURFACE_CENTRE, Transform3D(b, base), r, h, soil, 5, 11);
		_lump_since(from, base, r, 0.35f);
		const Vector3 top = at + up * h * (0.65f - 0.4f * settle);
		if (flipped) {
			// Roots up: a tangle of fine pale roots, 1 to 4 cm, curling over the top; they darken as they rot.
			const int roots = int(_rand(8.0f, 18.0f) * (1.0f - 0.7f * rot));
			for (int k = 0; k < roots; k++) {
				const float ra = _rand(0.0f, Math::TAU);
				const Vector3 o = top + b.xform(Vector3(Math::cos(ra), Math::sin(ra), 0.0f)) * r * _rand(0.0f, 0.7f);
				Vector3 d = (up * _rand(0.3f, 1.0f) + b.xform(Vector3(Math::cos(ra), Math::sin(ra), 0.0f)) * _rand(0.4f, 1.0f)).normalized();
				d.y = MAX(d.y, 0.15f); // Roots splay over the clod, not down into the ground.
				d.normalize();
				const int from = builders[SURFACE_STEM].colors.size();
				const float rad = _rand(0.0004f, 0.0009f);
				_add_stem(Transform3D(rooting_frame(d, up), o), _rand(0.01f, 0.04f), rad, rad * 0.5f, _rand(0.3f, 0.9f), 3, 0.0f);
				_tint_since(SURFACE_STEM, from, Color(1, 1, 1).lerp(Color(0.55f, 0.48f, 0.4f), rot) * _rand(0.85f, 1.1f));
			}
			// The grass, now under: a few blades poke out from the clod's rim and lie on the ground.
			const int edge = int(_rand(6.0f, 14.0f) * (1.0f - rot));
			for (int k = 0; k < edge; k++) {
				const float ea = _rand(0.0f, Math::TAU);
				const Vector3 o = at + Vector3(Math::cos(ea), 0.0f, Math::sin(ea)) * r * 0.9f + Vector3(0, 0.004f, 0);
				Leaf l;
				l.shape = LEAF_LINEAR;
				l.length = _rand(0.04f, 0.12f);
				l.width = _rand(0.002f, 0.004f);
				l.fold = 0.4f;
				l.bend = _rand(0.0f, 0.3f);
				l.twist = _rand(-0.5f, 0.5f);
				l.segments = 3;
				const Color dry = Color(1, 1, 1).lerp(Color(2.1f, 1.45f, 1.0f), wilt);
				_add_leaf(SURFACE_LEAF, Transform3D(rooting_frame(rooting_dir(ea + _rand(-0.5f, 0.5f), _rand(0.02f, 0.2f)), Vector3(0, 1, 0)), o), l, 0.0f, 0.7f, dry * _rand(0.85f, 1.1f));
			}
		} else {
			// Grass side up but tipped: the tuft leans with the clod, its blades splayed and drying.
			const int blades = int(_rand(14.0f, 30.0f) * (1.0f - 0.8f * rot));
			for (int k = 0; k < blades; k++) {
				const float ba = _rand(0.0f, Math::TAU);
				const Vector3 o = top + b.xform(Vector3(Math::cos(ba), Math::sin(ba), 0.0f)) * r * 0.6f * Math::sqrt(_rand(0.0f, 1.0f));
				const Vector3 d = (up * _rand(0.4f, 1.0f) + b.xform(Vector3(Math::cos(ba), Math::sin(ba), 0.0f)) * _rand(0.2f, 0.8f)).normalized();
				Leaf l;
				l.shape = LEAF_LINEAR;
				l.length = _rand(0.05f, 0.15f) * (1.0f - 0.4f * wilt);
				l.width = _rand(0.002f, 0.004f);
				l.fold = 0.45f;
				l.bend = _rand(0.3f, 1.0f) * (1.0f + wilt);
				l.twist = _rand(-0.4f, 0.4f);
				l.segments = 4;
				const Color dry = Color(1, 1, 1).lerp(Color(2.1f, 1.45f, 1.0f), wilt * _rand(0.7f, 1.0f));
				_add_leaf(SURFACE_LEAF, Transform3D(rooting_frame(d, up), o), l, 0.0f, 0.8f, dry);
			}
		}
	}
	// No loose crumbs: ground detail lays the humus clods, the terrain the turned soil (one rooting look).
	// Regrowth between the clods: seedlings and shoots from root fragments, green and upright, from about two months.
	const float shoots = Math::smoothstep(0.12f, 0.8f, age);
	const int n = int(_rand(10.0f, 24.0f) * shoots);
	for (int k = 0; k < n; k++) {
		const float a = _rand(0.0f, Math::TAU);
		const float rr = 0.2f * Math::sqrt(_rand(0.0f, 1.0f));
		Leaf l;
		l.shape = rng.randf() < 0.6f ? LEAF_LINEAR : LEAF_LANCEOLATE;
		l.length = l.shape == LEAF_LINEAR ? _rand(0.03f, 0.09f) * (0.5f + 0.5f * shoots) : _rand(0.012f, 0.03f);
		l.width = l.shape == LEAF_LINEAR ? _rand(0.0015f, 0.003f) : l.length * 0.45f;
		l.fold = 0.3f;
		l.bend = _rand(0.1f, 0.5f);
		l.segments = 3;
		_add_leaf(SURFACE_LEAF, Transform3D(rooting_frame(rooting_dir(_rand(0.0f, Math::TAU), _rand(0.9f, 1.4f)), Vector3(0, 1, 0)), Vector3(Math::cos(a) * rr, 0.0f, Math::sin(a) * rr)), l, 0.0f, 0.9f, Color(1.05f, 1.1f, 0.95f));
	}
}

// The rooting sets (docs/ideas/vegetation.md, "Boar rooting"): the sods by age band, then what comes first on the
// bare soil, placed by PlantSpecies.RULE_ROOTING; the stand's own field layer comes back by the rooting rules make_floor
// gives it.
TypedArray<Resource> PlantGenerator::make_rooting(RootingStand p_stand, int p_variants, bool p_textures, const Dictionary &p_season) {
	using P = PlantGenerator;
	struct Spec {
		Preset preset;
		float age; // Years since rooting the meshes show (the sods' stage).
		Vector2 ages; // Where it grows, years since rooting.
		float fade;
		float spacing, footprint, density, scale_min, scale_max;
		float patch_scale, patch_cover;
		float visibility;
	};
	LocalVector<Spec> set;
	switch (p_stand) {
		case ROOTING_WOOD:
			// Woodland: fresh sods, then herb Robert, nettle and broad-leaved dock on the loosened, nutrient-rich soil;
			// foxglove and raspberry follow in the second year where light reaches (Welander 2000; Kotanen 1995).
			set = LocalVector<Spec>({ { P::PRESET_ROOTED_SOD, 0.1f, Vector2(0.0f, 0.25f), 0.05f, 0.3f, 0.1f, 0.95f, 0.8f, 1.25f, 0.0f, 0.0f, 30.0f },
					{ P::PRESET_ROOTED_SOD, 0.5f, Vector2(0.25f, 0.9f), 0.1f, 0.32f, 0.1f, 0.85f, 0.8f, 1.2f, 0.0f, 0.0f, 30.0f },
					{ P::PRESET_ROOTED_SOD, 1.3f, Vector2(0.9f, 2.0f), 0.2f, 0.4f, 0.1f, 0.6f, 0.8f, 1.15f, 3.0f, 0.6f, 25.0f },
					{ P::PRESET_HERB_ROBERT, 0.0f, Vector2(0.25f, 1.6f), 0.3f, 0.45f, 0.1f, 0.5f, 0.8f, 1.2f, 3.0f, 0.55f, 25.0f },
					{ P::PRESET_NETTLE, 0.0f, Vector2(0.4f, 2.5f), 0.5f, 0.7f, 0.15f, 0.45f, 0.7f, 1.1f, 4.0f, 0.5f, 40.0f },
					{ P::PRESET_DOCK, 0.0f, Vector2(0.3f, 2.5f), 0.5f, 0.9f, 0.2f, 0.35f, 0.8f, 1.2f, 4.0f, 0.45f, 40.0f } });
			break;
		case ROOTING_MEADOW:
			// Meadow and woodland edge: sods, then mayweed, dock, thistle and plantain (the ruderal flush), the grasses
			// closing over in the second year (Bueno et al. 2013).
			set = LocalVector<Spec>({ { P::PRESET_ROOTED_SOD, 0.1f, Vector2(0.0f, 0.25f), 0.05f, 0.28f, 0.1f, 0.95f, 0.85f, 1.3f, 0.0f, 0.0f, 30.0f },
					{ P::PRESET_ROOTED_SOD, 0.5f, Vector2(0.25f, 0.9f), 0.1f, 0.3f, 0.1f, 0.85f, 0.8f, 1.25f, 0.0f, 0.0f, 30.0f },
					{ P::PRESET_ROOTED_SOD, 1.3f, Vector2(0.9f, 2.0f), 0.2f, 0.38f, 0.1f, 0.6f, 0.8f, 1.2f, 3.0f, 0.6f, 25.0f },
					{ P::PRESET_MAYWEED, 0.0f, Vector2(0.25f, 1.5f), 0.3f, 0.4f, 0.1f, 0.55f, 0.8f, 1.2f, 3.0f, 0.55f, 30.0f },
					{ P::PRESET_DOCK, 0.0f, Vector2(0.3f, 2.5f), 0.5f, 0.8f, 0.2f, 0.4f, 0.8f, 1.2f, 4.0f, 0.5f, 40.0f },
					{ P::PRESET_THISTLE, 0.0f, Vector2(0.4f, 2.5f), 0.5f, 0.9f, 0.2f, 0.3f, 0.8f, 1.2f, 5.0f, 0.4f, 40.0f },
					{ P::PRESET_PLANTAIN, 0.0f, Vector2(0.3f, 2.0f), 0.5f, 0.35f, 0.08f, 0.45f, 0.8f, 1.2f, 3.0f, 0.5f, 20.0f } });
			break;
		case ROOTING_MAX:
			break;
	}
	TypedArray<Resource> out;
	int seed_base = 9900 + int(p_stand) * 100;
	for (const Spec &f : set) {
		Dictionary season = p_season.duplicate();
		if (f.preset == P::PRESET_ROOTED_SOD) {
			season["rooting_age"] = f.age;
		}
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
		sp->set_density(f.density * (f.preset == P::PRESET_ROOTED_SOD ? 1.0f : get_presence(f.preset, p_season)));
		sp->set_scale_range(Vector2(f.scale_min, f.scale_max));
		sp->set_align_to_ground(f.preset == P::PRESET_ROOTED_SOD ? 1.0f : 0.3f);
		sp->set_rule_range(PlantSpecies::RULE_ROOTING, f.ages);
		sp->set_rule_fade(PlantSpecies::RULE_ROOTING, f.fade);
		sp->set_rule_range(PlantSpecies::RULE_SLOPE, Vector2(0.0f, 35.0f));
		sp->set_rule_fade(PlantSpecies::RULE_SLOPE, 5.0f);
		if (f.patch_scale > 0.0f) {
			sp->set_patch_scale(f.patch_scale);
			sp->set_patch_cover(f.patch_cover);
			sp->set_patch_seed(seed_base);
		}
		sp->set_visibility_range(f.visibility);
		sp->set_meta("rooting", true);
		if (f.preset == P::PRESET_ROOTED_SOD) {
			sp->set_meta("rooting_age", f.age);
			// Far off, fresh rooting reads as dark turned soil, older as a dull green-brown.
			// The terrain's humus colours, then greening as it closes over.
			Color far = Color(0.062f, 0.046f, 0.033f).lerp(Color(0.1f, 0.11f, 0.05f), Math::smoothstep(0.25f, 2.0f, f.age));
			far.a = 0.6f;
			sp->set_far_color(far);
		}
		tag_species(sp, f.preset);
		out.push_back(sp);
		seed_base += 13;
	}
	return out;
}

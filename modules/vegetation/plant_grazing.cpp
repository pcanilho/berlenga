/**************************************************************************/
/*  plant_grazing.cpp                                                     */
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

// Grazing roughs and dung (docs/ideas/vegetation.md, "Grazing roughs and dung").
// - Cattle avoid grazing round their own dung for months, so a grazed pasture is dotted with rank, dark green tufts
//   round old pats, two to four times the lawn's height, flowering when the lawn cannot (MacDiarmid and Watkin 1972,
//   J. Br. Grassl. Soc. 27, "The cattle dung patch"; the rejected area is several times the pat's own). A pat is 20 to
//   30 cm across and 2 to 4 cm high, glossy and olive for a day or two, then crusts, cracks, breaks up and is grassed
//   through within months (decay over one to two seasons, faster in wet summers).
// - Horses dung in latrines, patches of the paddock they do not graze, so a horse pasture is a mosaic of closely
//   grazed lawns and tall roughs with piles of droppings, nettles and thistles on the enriched ground (Odberg and
//   Francis-Smith 1976, Equine Vet. J., "lawns and roughs").
// - Deer leave pellet groups, 30 to 100 pellets of about a centimetre, dark and glossy when fresh, scattered where
//   they browse and bed; no rough forms round them.

#include "plant_generator.h"
#include "plant_species.h"

namespace {
Basis graze_frame(const Vector3 &p_forward, const Vector3 &p_up_hint) {
	const Vector3 z = p_forward.normalized();
	Vector3 x = p_up_hint.cross(z);
	if (x.length_squared() < 1e-8f) {
		x = Vector3(1, 0, 0).cross(z);
	}
	x.normalize();
	return Basis(x, z.cross(x).normalized(), z);
}

Vector3 graze_dir(float p_azimuth, float p_elevation) {
	return Vector3(Math::cos(p_azimuth) * Math::cos(p_elevation), Math::sin(p_elevation), Math::sin(p_azimuth) * Math::cos(p_elevation));
}

// Dung is not alive and does not sway: no wind weight, colour alpha 1 (no withering).
void dung_still(PackedVector2Array &p_uv2s, int p_from, PackedColorArray &p_colors) {
	for (int i = p_from; i < p_uv2s.size(); i++) {
		p_uv2s.write[i] = Vector2(0.0f, p_uv2s[i].y);
		Color c = p_colors[i];
		c.a = 1.0f;
		p_colors.write[i] = c;
	}
}
} // namespace

float PlantGenerator::_dung_age() const {
	return season.has("dung_age") ? MAX(float(season["dung_age"]), 0.0f) : 0.008f;
}

// A cattle pat: a low dome with the coiled rim of a pat dropped from height, slumping and crusting with age, then
// broken into pieces with grass growing through. Fresh: dark olive, glossy (roughness by age); a week: a paler grey-
// brown crust over a dark core; months: pale, fibrous, broken, mostly grass.
void PlantGenerator::_build_cow_pat() {
	const float age = _dung_age();
	const float crust = Math::smoothstep(0.005f, 0.08f, age); // Days to weeks.
	const float broken = Math::smoothstep(0.15f, 0.6f, age); // Months.
	const int still_from = builders[SURFACE_CENTRE].uv2s.size();
	const float r = _rand(0.1f, 0.15f);
	const float h = Math::lerp(_rand(0.025f, 0.04f), 0.012f, broken);
	Builder &cb = builders[SURFACE_CENTRE];
	const int rings = 9;
	const int sectors = 28;
	const int first = cb.verts.size();
	const float wob = _rand(0.0f, Math::TAU);
	for (int k = 0; k <= rings; k++) {
		const float t = float(k) / rings;
		for (int s = 0; s < sectors; s++) {
			const float a = s * Math::TAU / sectors;
			// An uneven outline and the pat's coils (ridges a centimetre or two apart), smoothed as it slumps.
			const float edge = 1.0f + 0.12f * Math::sin(a * 3.0f + wob) + 0.06f * Math::sin(a * 7.0f - wob);
			const float rr = r * t * edge;
			const float coil = 0.004f * Math::sin(t * 18.0f + a * 0.5f) * (1.0f - 0.7f * crust);
			float y = h * Math::pow(MAX(1.0f - t * t, 0.0f), 0.6f) + coil * (1.0f - t);
			// Broken: cracks into plates, the middle sunk and gone in places.
			float shade = 0.9f + 0.2f * Math::sin(a * 11.0f + t * 9.0f) * Math::sin(t * 13.0f);
			if (broken > 0.0f) {
				const float plate = Math::sin(a * 5.0f + wob) * Math::sin(t * 8.0f + a);
				y *= 1.0f - broken * 0.6f * Math::smoothstep(0.5f, 0.9f, plate);
				shade *= 1.0f - 0.35f * broken * Math::smoothstep(0.6f, 0.95f, plate); // Gaps: dark fibre and soil.
			}
			cb.verts.push_back(Vector3(Math::cos(a) * rr, y + 0.002f, Math::sin(a) * rr));
			cb.uvs.push_back(Vector2(float(s) / sectors, t));
			cb.uv2s.push_back(Vector2(0.0f, 0.0f));
			// The crust pales to grey-brown from the top and the rim; the old pat is straw-pale fibre.
			const Color fresh(1.0f, 1.0f, 1.0f);
			const Color crusted(1.9f, 1.75f, 1.7f);
			const Color old(2.6f, 2.4f, 2.0f);
			Color c = fresh.lerp(crusted, crust * (0.6f + 0.4f * t)).lerp(old, broken);
			cb.colors.push_back(c * shade);
		}
	}
	for (int k = 0; k < rings; k++) {
		for (int s = 0; s < sectors; s++) {
			const int i0 = first + k * sectors + s;
			const int i1 = first + k * sectors + (s + 1) % sectors;
			cb.indices.append_array(PackedInt32Array({ i0, i1, i0 + sectors, i1, i1 + sectors, i0 + sectors }));
		}
	}
	_tag_organ(cb);
	plant_height = MAX(plant_height, h);
	// Grass through an old pat and round its rim (the pat smothered the sward under it; it comes back from the edge).
	const int blades = int(Math::lerp(0.0f, 160.0f, broken) + (broken > 0.0f ? _rand(0.0f, 20.0f) : 0.0f));
	for (int i = 0; i < blades; i++) {
		const float a = _rand(0.0f, Math::TAU);
		const float rr = r * Math::sqrt(_rand(0.0f, 1.0f)) * 1.1f;
		Leaf l;
		l.shape = LEAF_LINEAR;
		l.length = _rand(0.03f, 0.09f);
		l.width = _rand(0.002f, 0.004f);
		l.fold = 0.4f;
		l.bend = _rand(0.3f, 0.9f);
		l.segments = 3;
		const Vector3 d = (Vector3(0, 1, 0) + graze_dir(_rand(0.0f, Math::TAU), 0.0f) * _rand(0.1f, 0.5f)).normalized();
		_add_leaf(SURFACE_LEAF, Transform3D(graze_frame(d, Vector3(0, 1, 0)), Vector3(Math::cos(a) * rr, 0.0f, Math::sin(a) * rr)), l, 0.0f, 0.8f, Color(1, 1, 1) * _rand(0.85f, 1.1f));
	}
	dung_still(cb.uv2s, still_from, cb.colors);
}

// Horse droppings: a pile of 6 to 14 balls 5 to 7 cm across, some broken, olive-khaki with straw in them; with age
// they pale, crumble and spread into a fibrous mat.
void PlantGenerator::_build_horse_dung() {
	const float age = _dung_age();
	const float pale = Math::smoothstep(0.01f, 0.2f, age);
	const float crumble = Math::smoothstep(0.1f, 0.5f, age);
	const int still_from = builders[SURFACE_CENTRE].uv2s.size();
	Builder &cb = builders[SURFACE_CENTRE];
	const int balls = int(_rand(6.0f, 14.0f));
	for (int b = 0; b < balls; b++) {
		const float a = _rand(0.0f, Math::TAU);
		const float rr = 0.09f * Math::sqrt(_rand(0.0f, 1.0f)) * (1.0f + crumble);
		const float br = _rand(0.025f, 0.035f) * (1.0f - 0.3f * crumble);
		const float up = b < balls / 3 ? br * 0.9f : 0.0f; // A few lie on the others.
		const Vector3 at(Math::cos(a) * rr, up - br * 0.15f, Math::sin(a) * rr);
		const float k = _rand(0.85f, 1.15f);
		const Color tint = Color(k, k, k).lerp(Color(2.0f, 2.0f, 2.1f), pale);
		// Round balls (a hemisphere over the ground line), flattening as they crumble.
		_add_head(SURFACE_CENTRE, Transform3D(graze_frame(graze_dir(_rand(0.0f, Math::TAU), _rand(1.3f, 1.57f)), Vector3(1, 0, 0)), at), br, br * Math::lerp(1.05f, 0.6f, crumble), tint, 5, 10);
	}
	// Straw fibres sticking out.
	const int fibres = int(_rand(10.0f, 20.0f));
	for (int i = 0; i < fibres; i++) {
		const float a = _rand(0.0f, Math::TAU);
		const Vector3 at(Math::cos(a) * _rand(0.0f, 0.1f), _rand(0.01f, 0.04f), Math::sin(a) * _rand(0.0f, 0.1f));
		Transform3D base(graze_frame(graze_dir(_rand(0.0f, Math::TAU), _rand(-0.3f, 0.4f)), Vector3(0, 1, 0)), at);
		_add_stem(base, _rand(0.01f, 0.035f), 0.0008f, 0.0006f, 0.0f, 2, 0.0f);
	}
	plant_height = MAX(plant_height, 0.08f);
	dung_still(cb.uv2s, still_from, cb.colors);
	dung_still(builders[SURFACE_STEM].uv2s, 0, builders[SURFACE_STEM].colors);
}

// A deer pellet group: 30 to 90 pellets 1 to 1.5 cm long, in a loose heap and scattered round it (dropped on the
// move); near black and glossy, browning and dulling over weeks, crumbling over months.
void PlantGenerator::_build_deer_pellets() {
	const float age = _dung_age();
	const float dull = Math::smoothstep(0.01f, 0.15f, age);
	const float crumble = Math::smoothstep(0.2f, 0.6f, age);
	const int still_from = builders[SURFACE_CENTRE].uv2s.size();
	Builder &cb = builders[SURFACE_CENTRE];
	const int pellets = int(_rand(30.0f, 90.0f) * (1.0f - 0.5f * crumble));
	const Vector2 drift(_rand(-0.08f, 0.08f), _rand(-0.08f, 0.08f));
	for (int p = 0; p < pellets; p++) {
		const float a = _rand(0.0f, Math::TAU);
		const bool heap = rng.randf() < 0.6f;
		const float rr = heap ? 0.04f * Math::sqrt(_rand(0.0f, 1.0f)) : _rand(0.03f, 0.16f);
		const Vector2 off = heap ? Vector2() : drift * rr / 0.16f;
		const float len = _rand(0.01f, 0.015f);
		const float rad = len * _rand(0.38f, 0.5f);
		const float stack = heap ? _rand(0.0f, 0.012f) * (1.0f - rr / 0.04f) : 0.0f;
		const Vector3 at(Math::cos(a) * rr + off.x, stack - rad * 0.3f, Math::sin(a) * rr + off.y);
		// Lying on its side: the dome along the pellet's length.
		const Vector3 along = graze_dir(_rand(0.0f, Math::TAU), _rand(-0.3f, 0.3f));
		const float k = _rand(0.8f, 1.25f);
		const Color tint = Color(k, k, k).lerp(Color(2.4f, 2.0f, 1.7f), dull);
		_add_head(SURFACE_CENTRE, Transform3D(graze_frame(along, Vector3(0, 1, 0)), at - along * len * 0.5f), rad, len, tint, 2, 6);
	}
	plant_height = MAX(plant_height, 0.03f);
	dung_still(cb.uv2s, still_from, cb.colors);
}

// The rough round an old pat or in a latrine: rank grass 20 to 45 cm tall, dark from the dung's nitrogen, in a ring
// that is thickest at the pat's edge and thins outward (the rejection zone), some culms flowering, last year's
// dead leaves in the base.
void PlantGenerator::_build_dung_rough() {
	const float ring = _rand(0.3f, 0.5f);
	const int blades = int(_rand(500.0f, 750.0f));
	for (int i = 0; i < blades; i++) {
		const float a = _rand(0.0f, Math::TAU);
		const float t = Math::sqrt(_rand(0.0f, 1.0f));
		const float rr = ring * t;
		const float tall = 1.0f - 0.45f * Math::smoothstep(0.6f, 1.0f, t); // Shorter at the rough's edge.
		Leaf l;
		l.shape = LEAF_LINEAR;
		l.length = _rand(0.2f, 0.42f) * tall;
		l.width = _rand(0.004f, 0.008f);
		l.fold = 0.35f;
		l.bend = _rand(0.5f, 1.5f);
		l.twist = _rand(-0.6f, 0.6f);
		l.segments = 5;
		const float out = _rand(0.1f, 0.5f) + 0.4f * t; // Leaning out of the tuft.
		const Vector3 d = (Vector3(0, 1, 0) + graze_dir(a + _rand(-0.6f, 0.6f), 0.0f) * out).normalized();
		const bool dead = rng.randf() < 0.12f;
		const Color tint = dead ? Color(2.6f, 1.9f, 2.2f) * _rand(0.8f, 1.0f) : Color(1, 1, 1) * _rand(0.82f, 1.12f);
		_add_leaf(SURFACE_LEAF, Transform3D(graze_frame(d, Vector3(0, 1, 0)), Vector3(Math::cos(a) * rr, 0.0f, Math::sin(a) * rr)), l, 0.0f, 0.85f, tint);
	}
	// Flowering culms: cocksfoot-like one-sided panicles and timothy-like spikes over the rough.
	const int culms = int(_rand(5.0f, 10.0f));
	for (int c = 0; c < culms; c++) {
		const float a = _rand(0.0f, Math::TAU);
		const float rr = ring * 0.7f * Math::sqrt(_rand(0.0f, 1.0f));
		Transform3D base(graze_frame(graze_dir(a, _rand(1.3f, 1.52f)), Vector3(0, 1, 0)), Vector3(Math::cos(a) * rr, 0.0f, Math::sin(a) * rr));
		const Transform3D top = _add_stem(base, _rand(0.32f, 0.55f), 0.0014f, 0.001f, _rand(-0.25f, 0.25f), 8, 0.0f);
		// A slender, nodding panicle: a few short branches with clustered spikelets.
		for (int k = 0; k < 4; k++) {
			Transform3D br(graze_frame(top.basis.get_column(2) * 1.2f + graze_dir(_rand(0.0f, Math::TAU), 0.0f), Vector3(0, 1, 0)), top.origin - top.basis.get_column(2) * (0.012f * k));
			_add_stem(br, _rand(0.015f, 0.035f), 0.0016f, 0.001f, 0.0f, 2, 1.0f, SURFACE_CENTRE);
		}
	}
}

TypedArray<Resource> PlantGenerator::make_grazing(GrazeSet p_set, int p_variants, bool p_textures, const Dictionary &p_season) {
	using P = PlantGenerator;
	struct Spec {
		Preset preset;
		float age; // "dung_age", years.
		float radius; // sward_mound at scale 1, m (0: the sward stays).
		float spacing, density, scale_min, scale_max;
		float patch_scale, patch_cover;
		float visibility;
	};
	LocalVector<Spec> set;
	switch (p_set) {
		case GRAZE_CATTLE:
			// Pats of every age, a rough round the older ones (the rough outlasts the pat); docks and nettles here and
			// there on the richest spots. About one pat per 10 m2 over a season (MacDiarmid and Watkin 1972).
			set = LocalVector<Spec>({ { P::PRESET_DUNG_ROUGH, 0.4f, 0.4f, 4.5f, 0.45f, 0.8f, 1.3f, 0.0f, 0.0f, 60.0f },
					{ P::PRESET_COW_PAT, 0.005f, 0.12f, 6.0f, 0.25f, 0.85f, 1.2f, 0.0f, 0.0f, 35.0f },
					{ P::PRESET_COW_PAT, 0.06f, 0.12f, 5.0f, 0.35f, 0.85f, 1.2f, 0.0f, 0.0f, 35.0f },
					{ P::PRESET_COW_PAT, 0.4f, 0.12f, 5.0f, 0.3f, 0.85f, 1.2f, 0.0f, 0.0f, 30.0f },
					{ P::PRESET_DOCK, 0.0f, 0.0f, 3.0f, 0.5f, 0.8f, 1.3f, 16.0f, 0.12f, 60.0f },
					{ P::PRESET_NETTLE, 0.0f, 0.0f, 0.8f, 0.6f, 0.8f, 1.2f, 22.0f, 0.06f, 60.0f } });
			break;
		case GRAZE_HORSE:
			// Latrines: patches of the paddock (a quarter of it, in clumps) where the horses dung and do not graze:
			// dense roughs, piles of droppings, nettles and thistles. The lawns between are the sward's own.
			set = LocalVector<Spec>({ { P::PRESET_DUNG_ROUGH, 0.5f, 0.4f, 1.1f, 0.8f, 0.9f, 1.4f, 18.0f, 0.25f, 70.0f },
					{ P::PRESET_HORSE_DUNG, 0.01f, 0.0f, 2.2f, 0.35f, 0.85f, 1.15f, 18.0f, 0.25f, 30.0f },
					{ P::PRESET_HORSE_DUNG, 0.25f, 0.0f, 2.0f, 0.45f, 0.85f, 1.15f, 18.0f, 0.25f, 30.0f },
					{ P::PRESET_NETTLE, 0.0f, 0.0f, 0.7f, 0.5f, 0.8f, 1.2f, 18.0f, 0.2f, 60.0f },
					{ P::PRESET_THISTLE, 0.0f, 0.0f, 1.4f, 0.4f, 0.8f, 1.2f, 18.0f, 0.22f, 60.0f } });
			break;
		case GRAZE_DEER:
			// Pellet groups where the deer browse hard (their lawns), fresh and weathered.
			set = LocalVector<Spec>({ { P::PRESET_DEER_PELLETS, 0.01f, 0.0f, 4.0f, 0.4f, 0.8f, 1.2f, 0.0f, 0.0f, 20.0f },
					{ P::PRESET_DEER_PELLETS, 0.2f, 0.0f, 4.0f, 0.4f, 0.8f, 1.2f, 0.0f, 0.0f, 20.0f } });
			break;
		case GRAZE_MAX:
			break;
	}
	TypedArray<Resource> out;
	int seed_base = 11000 + int(p_set) * 100;
	for (const Spec &f : set) {
		Dictionary season = p_season.duplicate();
		const bool dung = f.preset == P::PRESET_COW_PAT || f.preset == P::PRESET_HORSE_DUNG || f.preset == P::PRESET_DEER_PELLETS;
		if (dung) {
			season["dung_age"] = f.age;
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
		sp->set_footprint(MAX(f.radius, 0.1f));
		sp->set_density(f.density * get_presence(f.preset, p_season));
		sp->set_scale_range(Vector2(f.scale_min, f.scale_max));
		sp->set_align_to_ground(1.0f);
		if (f.patch_scale > 0.0f) {
			sp->set_patch_scale(f.patch_scale);
			sp->set_patch_cover(f.patch_cover);
			// One latrine layout for the whole horse set: every species shares its patches.
			sp->set_patch_seed(p_set == GRAZE_HORSE ? 11100 : seed_base);
		}
		// Out in the light of the pasture or lawn, not on steep or waterlogged ground.
		sp->set_rule_range(PlantSpecies::RULE_SHADE, Vector2(0.0f, 0.4f));
		sp->set_rule_fade(PlantSpecies::RULE_SHADE, 0.1f);
		sp->set_rule_range(PlantSpecies::RULE_SLOPE, Vector2(0.0f, 30.0f));
		sp->set_rule_fade(PlantSpecies::RULE_SLOPE, 5.0f);
		sp->set_rule_range(PlantSpecies::RULE_WATER_DISTANCE, Vector2(2.0f, 1000000.0f));
		sp->set_rule_fade(PlantSpecies::RULE_WATER_DISTANCE, 1.0f);
		if (p_set == GRAZE_DEER) {
			sp->set_rule_range(PlantSpecies::RULE_BROWSE, Vector2(0.3f, 1.0f));
			sp->set_rule_fade(PlantSpecies::RULE_BROWSE, 0.15f);
			sp->set_rule_range(PlantSpecies::RULE_SHADE, Vector2(0.0f, 0.9f)); // Deer dung in the wood too.
		}
		sp->set_visibility_range(f.visibility);
		if (f.radius > 0.0f) {
			sp->set_meta("sward_mound", f.radius); // The rough or pat carries its own grass: the field's blades keep out.
		}
		if (dung) {
			sp->set_meta("dung_age", f.age);
		}
		sp->set_meta("grazing", true);
		tag_species(sp, f.preset);
		out.push_back(sp);
		seed_base += 13;
	}
	return out;
}

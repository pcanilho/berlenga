/**************************************************************************/
/*  plant_webs.cpp                                                        */
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

// Dewy webs (docs/ideas/vegetation.md, "Dewy webs").
// - Money spiders (Linyphiidae) spin small horizontal, non-sticky sheet webs in the sward, with scaffold threads up
//   into the grass, and reach very high numbers in grassland and crops (Sunderland, Fraser and Dixon 1986,
//   Pedobiologia 29: "can reach 160 per m2"; Bristowe 1958, over a million an acre from August to December). Orb
//   weavers (garden spiders) hang vertical orb webs between tall dry stems in late summer and autumn.
// - Silk is 1 to 4 microns thick: unseen until dew or hoarfrost beads it, when a meadow turns grey-white with webs on
//   a still autumn morning and they vanish again as the sun dries them.
// - The threads are drawn by the silk shader (PlantGenerator::_plant_shader(), PLANT_SILK) from the UVs laid here: the
//   sheet's plane in metres (COLOR.b 0), single threads along UV.x with UV.y across (COLOR.b 0.5), and the orb's own
//   plane round its hub (COLOR.b 1). COLOR.r thins the sheet towards its edge.

#include "plant_generator.h"
#include "plant_species.h"

namespace {
const float SILK_RIBBON = 0.006f; // Half width of the strips a single thread is drawn on (m).
const int ORB_FRAME = 5; // The orb's frame: a pentagon, its first corner at ORB_FRAME_AT (rad); the shader's rim matches.
const float ORB_FRAME_AT = 0.3f;
const float ORB_FRAME_OUT = 1.4f; // Its corners over the capture spiral's radius: its sides just clear the spiral.

Basis web_frame(const Vector3 &p_forward) {
	const Vector3 z = p_forward.normalized();
	Vector3 x = Vector3(0, 1, 0).cross(z);
	if (x.length_squared() < 1e-8f) {
		x = Vector3(1, 0, 0).cross(z);
	}
	x.normalize();
	return Basis(x, z.cross(x).normalized(), z);
}
} // namespace

// One thread from p_from to p_to: two crossed strips, so it is seen from any side.
void PlantGenerator::_add_silk_line(const Vector3 &p_from, const Vector3 &p_to, float p_wind_from, float p_wind_to) {
	Builder &b = builders[SURFACE_CENTRE];
	const Vector3 d = p_to - p_from;
	const float len = d.length();
	if (len < 1e-3f) {
		return;
	}
	const Basis f = web_frame(d);
	const float phase = _rand(0.0f, 100.0f); // Where along the thread its beads fall.
	for (int k = 0; k < 2; k++) {
		const Vector3 side = (k == 0 ? f.get_column(0) : f.get_column(1)) * SILK_RIBBON;
		const int base = b.verts.size();
		const Vector3 p[4] = { p_from - side, p_from + side, p_to + side, p_to - side };
		const float u[4] = { phase, phase, phase + len, phase + len };
		const float v[4] = { -SILK_RIBBON, SILK_RIBBON, SILK_RIBBON, -SILK_RIBBON };
		const float w[4] = { p_wind_from, p_wind_from, p_wind_to, p_wind_to };
		for (int i = 0; i < 4; i++) {
			b.verts.push_back(p[i]);
			b.uvs.push_back(Vector2(u[i], v[i]));
			b.uv2s.push_back(Vector2(w[i], 0.0f));
			b.colors.push_back(Color(1.0f, 1.0f, 0.5f, 1.0f));
		}
		b.indices.push_back(base);
		b.indices.push_back(base + 1);
		b.indices.push_back(base + 2);
		b.indices.push_back(base);
		b.indices.push_back(base + 2);
		b.indices.push_back(base + 3);
	}
}

// A sheet web: most are a money spider's small flat sheet a few centimetres across close to the ground (in a hoof
// print, a hollow, between tillers), some a larger hammock or dome 10 to 20 cm across higher in the grass, held by
// scaffold threads up to the blade tips.
void PlantGenerator::_build_sheet_web() {
	Builder &b = builders[SURFACE_CENTRE];
	const bool hammock = rng.randf() < 0.4f;
	const float radius = hammock ? _rand(0.07f, 0.11f) : _rand(0.025f, 0.05f);
	const float height = hammock ? _rand(0.05f, 0.14f) : _rand(0.01f, 0.04f);
	const float sag = hammock ? radius * _rand(-0.35f, 0.3f) : radius * _rand(-0.05f, 0.1f); // > 0: a hammock, < 0: a dome.
	const float spin = _rand(0.0f, Math::TAU); // The sheet's own threads run every way: rotate the shader's families.
	const Vector2 shift(_rand(-1.0f, 1.0f), _rand(-1.0f, 1.0f));
	const float lobe_a = _rand(0.0f, Math::TAU), lobe_b = _rand(0.0f, Math::TAU);
	const float stretch = _rand(0.7f, 1.0f);
	const int rings = 5, sectors = 14;
	const float wind = MIN(height * 3.0f, 0.4f);
	auto rim = [&](float a) {
		return radius * (1.0f + 0.18f * Math::sin(2.0f * a + lobe_a) + 0.1f * Math::sin(3.0f * a + lobe_b));
	};
	const int base = b.verts.size();
	for (int r = 0; r <= rings; r++) {
		const float t = float(r) / rings;
		const int n = r == 0 ? 1 : sectors;
		for (int s = 0; s < n; s++) {
			const float a = float(s) / sectors * Math::TAU;
			const float rr = rim(a) * t;
			const float x = Math::cos(a) * rr * stretch, z = Math::sin(a) * rr;
			const float y = height - sag * (1.0f - t * t);
			b.verts.push_back(Vector3(x, y, z));
			const Vector2 uv = Vector2(x, z).rotated(spin) + shift;
			b.uvs.push_back(uv);
			b.uv2s.push_back(Vector2(wind, 0.0f));
			b.colors.push_back(Color(1.0f - t * t * t, 1.0f, 0.0f, 1.0f)); // Thinning out to nothing at the edge.
		}
	}
	// The centre fan, then the rings.
	for (int s = 0; s < sectors; s++) {
		b.indices.push_back(base);
		b.indices.push_back(base + 1 + s);
		b.indices.push_back(base + 1 + (s + 1) % sectors);
	}
	for (int r = 1; r < rings; r++) {
		const int i0 = base + 1 + (r - 1) * sectors, i1 = base + 1 + r * sectors;
		for (int s = 0; s < sectors; s++) {
			const int s1 = (s + 1) % sectors;
			b.indices.push_back(i0 + s);
			b.indices.push_back(i1 + s);
			b.indices.push_back(i1 + s1);
			b.indices.push_back(i0 + s);
			b.indices.push_back(i1 + s1);
			b.indices.push_back(i0 + s1);
		}
	}
	// Scaffold: threads from the sheet's edge and middle up to the blade tips round it, and a few down to the ground.
	const int up = hammock ? int(_rand(6.0f, 12.0f)) : int(_rand(2.0f, 5.0f));
	for (int k = 0; k < up; k++) {
		const float a = _rand(0.0f, Math::TAU);
		const float from_r = rim(a) * _rand(0.3f, 1.0f);
		const Vector3 from(Math::cos(a) * from_r * stretch, height - sag * (1.0f - (from_r / radius) * (from_r / radius) * 0.8f), Math::sin(a) * from_r);
		const float out = _rand(0.0f, 0.06f);
		const Vector3 to = from + Vector3(Math::cos(a) * out, _rand(0.04f, 0.16f), Math::sin(a) * out);
		_add_silk_line(from, to, wind, MIN(wind + 0.3f, 0.7f));
	}
	if (hammock) {
		for (int k = 0; k < 3; k++) {
			const float a = _rand(0.0f, Math::TAU);
			const float from_r = rim(a) * 0.95f;
			const Vector3 from(Math::cos(a) * from_r * stretch, height, Math::sin(a) * from_r);
			_add_silk_line(from, Vector3(from.x * 1.3f, 0.0f, from.z * 1.3f), wind, 0.0f);
		}
	}
	plant_height = MAX(plant_height, height + 0.1f);
}

// An orb web in the open: hung vertically between two dry stems (old umbellifer or grass stalks), its hub a third of
// the way down, the capture spiral 3 to 5 mm apart over 25 to 35 radials, inside a frame of five threads tied to the
// stems and down to the ground. A share of them (Zygiella-like) leave one sector open for the signal thread.
void PlantGenerator::_build_orb_web() {
	const float radius = _rand(0.07f, 0.14f);
	const float spacing = _rand(0.003f, 0.005f);
	const int radials = int(_rand(25.0f, 35.0f));
	const float open = rng.randf() < 0.25f ? _rand(0.06f, 0.1f) : 0.0f;
	silk_orb[0] = radius;
	silk_orb[1] = spacing;
	silk_orb[2] = float(radials);
	silk_orb[3] = open;
	const float frame_r = radius * ORB_FRAME_OUT;
	const float hub = _rand(0.25f, 0.5f) + frame_r;
	const float half_gap = frame_r * _rand(1.15f, 1.6f); // The stems either side.
	const float stem_h = hub + frame_r + _rand(0.08f, 0.3f);
	const float tilt = _rand(-0.25f, 0.25f); // Orbs hang a little off the vertical.
	// The web's plane: x across between the stems, y up (tilted), its normal along z.
	const Vector3 ax(1, 0, 0);
	const Vector3 ay = Vector3(0, Math::cos(tilt), Math::sin(tilt));
	const Vector3 centre(0, hub, 0);
	auto wind_at = [&](const Vector3 &p) { return CLAMP(p.y / stem_h, 0.0f, 1.0f) * 0.8f; };
	Builder &b = builders[SURFACE_CENTRE];
	const int rings = 6, sectors = 20;
	const float mesh_r = frame_r * 1.03f;
	const int base = b.verts.size();
	for (int r = 0; r <= rings; r++) {
		const float t = float(r) / rings;
		const int n = r == 0 ? 1 : sectors;
		for (int s = 0; s < n; s++) {
			const float a = float(s) / sectors * Math::TAU;
			const Vector2 q = Vector2(Math::cos(a), Math::sin(a)) * mesh_r * t;
			const Vector3 p = centre + ax * q.x + ay * q.y;
			b.verts.push_back(p);
			b.uvs.push_back(q);
			b.uv2s.push_back(Vector2(wind_at(p), 0.0f));
			b.colors.push_back(Color(1.0f, 1.0f, 1.0f, 1.0f));
		}
	}
	for (int s = 0; s < sectors; s++) {
		b.indices.push_back(base);
		b.indices.push_back(base + 1 + s);
		b.indices.push_back(base + 1 + (s + 1) % sectors);
	}
	for (int r = 1; r < rings; r++) {
		const int i0 = base + 1 + (r - 1) * sectors, i1 = base + 1 + r * sectors;
		for (int s = 0; s < sectors; s++) {
			const int s1 = (s + 1) % sectors;
			b.indices.push_back(i0 + s);
			b.indices.push_back(i1 + s);
			b.indices.push_back(i1 + s1);
			b.indices.push_back(i0 + s);
			b.indices.push_back(i1 + s1);
			b.indices.push_back(i0 + s1);
		}
	}
	// The stems' feet and leans first, so the anchors meet them.
	Vector3 feet[2], leans[2];
	for (int k = 0; k < 2; k++) {
		feet[k] = Vector3((k == 0 ? -1.0f : 1.0f) * half_gap + _rand(-0.02f, 0.02f), 0.0f, _rand(-0.02f, 0.02f));
		leans[k] = Vector3(_rand(-0.05f, 0.05f), 1.0f, _rand(-0.05f, 0.05f)).normalized();
	}
	auto on_stem = [&](int k, float y) { return feet[k] + leans[k] * (y / leans[k].y); };
	// The frame and its anchors: each corner tied out to the nearer stem (or down to the ground from the low ones).
	Vector3 corners[ORB_FRAME];
	for (int k = 0; k < ORB_FRAME; k++) {
		const float a = ORB_FRAME_AT + float(k) * Math::TAU / ORB_FRAME;
		corners[k] = centre + (ax * Math::cos(a) + ay * Math::sin(a)) * frame_r;
	}
	for (int k = 0; k < ORB_FRAME; k++) {
		const Vector3 &c = corners[k];
		const Vector3 &n = corners[(k + 1) % ORB_FRAME];
		_add_silk_line(c, n, wind_at(c), wind_at(n));
		if (c.y < hub - frame_r * 0.5f && rng.randf() < 0.6f) {
			const Vector3 g(c.x * 1.4f + _rand(-0.05f, 0.05f), 0.0f, c.z + _rand(-0.05f, 0.05f));
			_add_silk_line(c, g, wind_at(c), 0.0f);
		} else {
			const Vector3 s = on_stem(c.x >= 0.0f ? 1 : 0, CLAMP(c.y + _rand(-0.05f, 0.08f), 0.05f, stem_h - 0.05f));
			_add_silk_line(c, s, wind_at(c), wind_at(s));
		}
	}
	// The two stems: dead and straw-coloured, a seed head on top. Built after the silk, so the silk stays the plant's own
	// (organ 0) and each stem sways about its own base.
	_tag_organ(builders[SURFACE_CENTRE]);
	for (int k = 0; k < 2; k++) {
		const Vector3 foot = feet[k];
		Transform3D base_t(web_frame(leans[k]), foot);
		const Transform3D top = _add_stem(base_t, stem_h, 0.0016f, 0.0008f, _rand(-0.08f, 0.08f), 10, 0.0f);
		// A dead knapweed-like head, dark brown, its bracts spread.
		_add_head(SURFACE_STEM, top, 0.006f, 0.012f, Color(0.55f, 0.42f, 0.32f), 3, 8);
		// Side branches lower down, some with a smaller head.
		const int sides = int(_rand(1.0f, 3.0f));
		for (int h = 0; h < sides; h++) {
			const float a = _rand(0.0f, Math::TAU);
			const float at = _rand(0.45f, 0.8f);
			Transform3D br(web_frame(Vector3(Math::cos(a), 1.6f, Math::sin(a))), foot + base_t.basis.get_column(2) * stem_h * at);
			const Transform3D bt = _add_stem(br, _rand(0.06f, 0.15f), 0.0008f, 0.0006f, _rand(-0.2f, 0.2f), 3, at);
			_add_head(SURFACE_STEM, bt, 0.0045f, 0.009f, Color(0.55f, 0.42f, 0.32f), 3, 6);
		}
	}
	plant_height = MAX(plant_height, stem_h);
}

TypedArray<Resource> PlantGenerator::make_dew_webs(int p_variants, bool p_textures, const Dictionary &p_season) {
	struct Spec {
		Preset preset;
		float spacing, density, visibility, shade;
	};
	// Sheets everywhere in the sward (a few per square metre, of the many spiders' webs, the ones big enough to see);
	// orbs a few metres apart where tall dead stems stand in the open.
	const Spec set[2] = { { PRESET_SHEET_WEB, 0.55f, 0.75f, 22.0f, 0.6f }, { PRESET_ORB_WEB, 3.0f, 0.5f, 30.0f, 0.45f } };
	TypedArray<Resource> out;
	for (int i = 0; i < 2; i++) {
		const Spec &f = set[i];
		Ref<PlantGenerator> gen;
		gen.instantiate();
		gen->set_textures(p_textures);
		gen->set_preset(f.preset);
		gen->apply_preset_colors();
		gen->set_seed(13000 + 100 * i);
		gen->set_season(p_season);
		Ref<PlantSpecies> sp;
		sp.instantiate();
		TypedArray<Mesh> meshes;
		const TypedArray<ArrayMesh> v = gen->generate_variants(MAX(p_variants, 1) * (i == 0 ? 2 : 1));
		for (int k = 0; k < v.size(); k++) {
			meshes.push_back(v[k]);
		}
		sp->set_meshes(meshes);
		sp->set_spacing(f.spacing);
		sp->set_footprint(0.1f);
		sp->set_density(f.density * get_presence(f.preset, p_season));
		sp->set_scale_range(Vector2(0.85f, 1.2f));
		sp->set_align_to_ground(i == 0 ? 1.0f : 0.0f); // Orbs hang plumb.
		sp->set_rule_range(PlantSpecies::RULE_SHADE, Vector2(0.0f, f.shade));
		sp->set_rule_fade(PlantSpecies::RULE_SHADE, 0.1f);
		sp->set_rule_range(PlantSpecies::RULE_SLOPE, Vector2(0.0f, 35.0f));
		sp->set_rule_fade(PlantSpecies::RULE_SLOPE, 5.0f);
		sp->set_rule_range(PlantSpecies::RULE_WATER_DISTANCE, Vector2(0.5f, 1000000.0f));
		sp->set_rule_fade(PlantSpecies::RULE_WATER_DISTANCE, 0.5f);
		sp->set_visibility_range(f.visibility);
		sp->set_meta("silk", true);
		tag_species(sp, f.preset);
		out.push_back(sp);
	}
	return out;
}

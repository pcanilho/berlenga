/**************************************************************************/
/*  plant_hay.cpp                                                         */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
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

// Round hay bales (docs/ideas/vegetation.md, "Hay bales and cut stalks").
// - A large round bale is rolled in the baler's chamber: the hay wound in layers round its axis, the ends showing the
//   spiral, the edges rounded, bound with twine bands across the width. 5 x 5 ft (1.52 m) is the usual reference, a
//   4 x 4 ft bale about half its hay (UF/IFAS EDIS AN326). Lying on its round side it settles a little flat.
// - The mesh: the axis along +X (the baler throws it out with its ends to the sides of the windrow), the bale centred
//   on x and z, standing on y = 0. The body's surface a few centimetres in under hay strands wound round it, loose ends
//   lifting off; the ends domed a little, the spiral's layers in their colour; twine in bands.

#include "plant_generator.h"

void PlantGenerator::_build_hay_bale() {
	// Width and diameter: 4 x 4, 4 x 5 or 5 x 5 ft by the seed.
	const int kind = MIN(int(rng.randf() * 3.0f), 2);
	const float wid = kind == 2 ? 1.52f : 1.22f;
	const float dia = kind == 0 ? 1.22f : 1.52f;
	const float r0 = 0.5f * dia;
	const float edge = 0.07f; // The rounded edge between body and ends.
	const float sag = _rand(0.03f, 0.06f); // Settled flat where it lies.
	const float ph = _rand(0.0f, Math::TAU);
	const float layer = _rand(0.025f, 0.04f); // The spiral's pitch on the ends.
	// Radius of the body at angle a round the axis (0 at the top) and x along it: lumpy, flatter at the bottom.
	auto body_r = [&](float p_a, float p_x) {
		const float lump = 0.006f * Math::sin(p_a * 7.0f + ph) + 0.004f * Math::sin(p_a * 17.0f - p_x * 9.0f) + 0.003f * Math::sin(p_x * 23.0f + p_a * 3.0f);
		return r0 + lump;
	};
	auto to_world = [&](float p_a, float p_x, float p_r) {
		Vector3 v(p_x, r0 + p_r * Math::cos(p_a), p_r * Math::sin(p_a));
		// Settled: the bottom flattened onto the ground.
		v.y = MAX(v.y, sag + (v.y - sag) * 0.25f);
		v.y -= sag;
		return v;
	};
	Builder &cb = builders[SURFACE_CENTRE];
	const int na = 144;
	// The body in rings about 9 mm apart across its width: each ring a wound band of hay of its own shade and a hair
	// proud or sunk, so the surface carries the fine circumferential grain of the layers (geometry, not a texture).
	const int nx = 140;
	const int first = int(cb.verts.size());
	// The body and its rounded edges as one lathe: along u from one end's centre over the edge, the body, the other
	// edge to the other end's centre; round a.
	LocalVector<Vector2> prof; // (x, radius) per ring.
	const int ne = 10; // Rings on each end face.
	const int nr = 6; // Rings round each edge.
	const float hw = 0.5f * wid;
	for (int i = 0; i <= ne; i++) {
		const float t = float(i) / ne;
		const float rr = (r0 - edge) * t;
		const float dome = 0.03f * (1.0f - t * t); // The end bulges a little.
		prof.push_back(Vector2(-hw - dome, rr));
	}
	for (int i = 1; i < nr; i++) {
		const float a = 0.5f * float(Math::PI) * float(i) / nr;
		prof.push_back(Vector2(-hw + edge - edge * Math::cos(a), r0 - edge + edge * Math::sin(a)));
	}
	for (int i = 0; i <= nx; i++) {
		prof.push_back(Vector2(-hw + edge + (wid - 2.0f * edge) * float(i) / nx, r0));
	}
	for (int i = nr - 1; i >= 1; i--) {
		const float a = 0.5f * float(Math::PI) * float(i) / nr;
		prof.push_back(Vector2(hw - edge + edge * Math::cos(a), r0 - edge + edge * Math::sin(a)));
	}
	for (int i = ne; i >= 0; i--) {
		const float t = float(i) / ne;
		const float dome = 0.03f * (1.0f - t * t);
		prof.push_back(Vector2(hw + dome, (r0 - edge) * t));
	}
	const int np = int(prof.size());
	// Bands: a hash of the band a point across the width falls in, about 9 mm wide (an estimate of a wound layer's
	// visible strands), smoothed into its neighbours.
	const uint32_t band_seed = uint32_t(rng.rand());
	auto band_hash = [&](int p_i) {
		return float(hash_murmur3_one_32(uint32_t(p_i) + band_seed) & 0xFFFF) / 65535.0f;
	};
	auto band_shade = [&](float p_x) {
		const float u = p_x / 0.009f;
		const int i = int(Math::floor(u));
		const float f = u - float(i);
		const float b = Math::lerp(band_hash(i), band_hash(i + 1), Math::smoothstep(0.3f, 0.7f, f));
		return 0.74f + 0.38f * b;
	};
	auto band_lift = [&](float p_x) {
		const float u = p_x / 0.009f;
		const int i = int(Math::floor(u));
		return (band_hash(i * 7 + 3) - 0.5f) * 0.006f;
	};
	for (int j = 0; j < np; j++) {
		const Vector2 pr = prof[j];
		const bool end_face = j <= ne || j >= np - 1 - ne;
		for (int i = 0; i <= na; i++) {
			const float a = Math::TAU * float(i) / na;
			const float rr = pr.y >= r0 - 1e-4f ? body_r(a, pr.x) + band_lift(pr.x) : pr.y * body_r(a, pr.x) / r0;
			cb.verts.push_back(to_world(a, pr.x, rr));
			cb.uvs.push_back(Vector2(float(i) / na, float(j) / (np - 1)));
			cb.uv2s.push_back(Vector2(0.0f, 0.0f));
			float n;
			if (end_face) {
				// The spiral: the wound layers' edges, darker between them, the outer ones weathered paler.
				const float s = (pr.y + layer * a / Math::TAU) / layer;
				const float band = Math::abs(Math::fmod(s, 1.0f) - 0.5f) * 2.0f;
				n = 0.72f + 0.3f * Math::smoothstep(0.15f, 0.6f, band) + 0.06f * Math::sin(a * 31.0f + pr.y * 50.0f);
			} else {
				// The ring's band, drifting a little round the body as the strands cross over.
				const float rb = band_shade(pr.x + 0.006f * Math::sin(a * 5.0f + pr.x * 40.0f));
				n = rb * (0.94f + 0.06f * Math::sin(a * 23.0f + pr.x * 310.0f));
			}
			cb.colors.push_back(Color(n, n, n));
		}
	}
	{
		// Winding as the turf lip's grids: outward.
		const Vector3 e1 = cb.verts[first + na + 1] - cb.verts[first];
		const Vector3 e2 = cb.verts[first + 1] - cb.verts[first];
		const Vector3 out = Vector3(-1, 0, 0);
		const bool flip = e1.cross(e2).dot(out) < 0.0f;
		for (int j = 0; j < np - 1; j++) {
			for (int i = 0; i < na; i++) {
				const int i0 = first + j * (na + 1) + i;
				const int i1 = i0 + na + 1;
				if (flip) {
					cb.indices.append_array(PackedInt32Array({ i0, i0 + 1, i1, i0 + 1, i1 + 1, i1 }));
				} else {
					cb.indices.append_array(PackedInt32Array({ i0, i1, i0 + 1, i0 + 1, i1, i1 + 1 }));
				}
			}
		}
	}
	_tag_organ(cb);
	// Hay strands wound round the body, a few loose ends lifting off; more over the ends' outer layers.
	const int strands = int(wid * dia * _rand(2600.0f, 3000.0f));
	for (int i = 0; i < strands; i++) {
		const float a = _rand(0.0f, Math::TAU);
		const float x = _rand(-hw + 0.02f, hw - 0.02f);
		const float rr = body_r(a, x) + 0.002f;
		const Vector3 at = to_world(a, x, rr);
		if (at.y < 0.12f) {
			continue; // Under the bale, where it sits on them.
		}
		const Vector3 outn = Vector3(0.0f, Math::cos(a), Math::sin(a));
		Vector3 tang = Vector3(0.0f, -Math::sin(a), Math::cos(a)) * (rng.randf() < 0.5f ? 1.0f : -1.0f);
		if (at.y < r0 && tang.y < 0.0f) {
			tang = -tang; // On the lower half, wound up the body, not down under it.
		}
		const bool loose = rng.randf() < 0.06f;
		const Vector3 dir = (tang + Vector3(_rand(-0.25f, 0.25f), 0.0f, 0.0f) + outn * (loose ? _rand(0.2f, 0.8f) : 0.0f)).normalized();
		Leaf l;
		l.shape = LEAF_LINEAR;
		l.length = _rand(0.12f, 0.32f);
		l.width = _rand(0.004f, 0.009f);
		l.fold = 0.3f;
		l.bend = loose ? _rand(-0.3f, 0.6f) : l.length / r0; // Following the body's curve.
		l.twist = _rand(-0.6f, 0.6f);
		l.segments = 3;
		const float s = _rand(0.75f, 1.15f);
		Basis f;
		{
			const Vector3 z = dir;
			Vector3 xx = outn.cross(z);
			if (xx.length_squared() < 1e-8f) {
				xx = Vector3(1, 0, 0);
			}
			xx.normalize();
			f = Basis(xx, z.cross(xx).normalized(), z);
		}
		_add_leaf(SURFACE_LEAF, Transform3D(f, at), l, 0.0f, 0.85f, Color(s, s, s));
	}
	// Twine: bands round the body, wound in a slow spiral across the width.
	const int bands = kind == 2 ? 9 : 7;
	for (int b = 0; b < bands; b++) {
		const float x = -hw + edge + (wid - 2.0f * edge) * (float(b) + 0.5f) / bands;
		LocalVector<Vector3> pts;
		LocalVector<float> radii;
		LocalVector<float> wind;
		const int n = 64;
		for (int i = 0; i <= n; i++) {
			const float a = Math::TAU * float(i) / n;
			const float xi = x + 0.04f * (float(i) / n - 0.5f);
			Vector3 p = to_world(a, xi, body_r(a, xi) + 0.004f);
			if (p.y < 0.01f) {
				p.y = 0.01f;
			}
			pts.push_back(p);
			radii.push_back(0.0018f);
			wind.push_back(0.0f);
		}
		_add_root_path(pts, radii, wind);
	}
	plant_height = MAX(plant_height, dia);
}

Ref<ArrayMesh> PlantGenerator::make_hay_bale(int p_seed, bool p_textures) {
	Ref<PlantGenerator> gen;
	gen.instantiate();
	gen->set_textures(p_textures);
	gen->set_preset(PRESET_HAY_BALE);
	gen->apply_preset_colors();
	gen->set_seed(p_seed);
	return gen->generate();
}

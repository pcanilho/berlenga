/**************************************************************************/
/*  plant_bank.cpp                                                        */
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


// Turf overhangs on cut banks (docs/ideas/vegetation.md, "Turf overhangs on cut banks").
// - A river bank of a root-bound topsoil over weaker soil fails in layers: the flow undercuts the lower layer, the
//   rooted sod above holds on as an overhanging cantilever, and in time breaks off (cantilever failures of composite
//   banks: Thorne and Tovey 1981, Earth Surface Processes and Landforms 6). Roots add much of the sod's strength
//   (Pollen and Simon 2005, Water Resources Research 41, W07025). Scarps of slumps and terrace edges show the same lip.
// - The mesh: a sod slab 0.7 to 1.5 m along the edge (+Z), standing out 12 to 35 cm over the drop (+X; the field
//   stands its origin on the edge), 10 to 20 cm thick with a ragged lower edge (dimensions are estimates from field
//   photographs, not measured); its cut face and underside dark topsoil, the sward's blades leaning out and hanging
//   over the edge, roots trailing 10 to 45 cm from the underside and the face's foot.

#include "plant_generator.h"
#include "plant_species.h"

namespace {
Basis bank_frame(const Vector3 &p_forward, const Vector3 &p_up_hint) {
	const Vector3 z = p_forward.normalized();
	Vector3 x = p_up_hint.cross(z);
	if (x.length_squared() < 1e-8f) {
		x = Vector3(1, 0, 0).cross(z);
	}
	x.normalize();
	return Basis(x, z.cross(x).normalized(), z);
}
} // namespace

void PlantGenerator::_build_turf_lip() {
	const float len = _rand(0.7f, 1.5f);
	const float ov_mean = _rand(0.12f, 0.35f);
	const float thick_mean = _rand(0.1f, 0.2f);
	const float droop = _rand(0.0f, 0.05f);
	const float ph = _rand(0.0f, Math::TAU);
	// Along the edge: how far it stands out, how thick, both ragged; ends taper back into the bank.
	auto ov_at = [&](float z) {
		const float t = z / (0.5f * len);
		const float taper = Math::sqrt(MAX(1.0f - t * t * t * t, 0.0f));
		return MAX(ov_mean * taper * (0.75f + 0.18f * Math::sin(z * 6.1f + ph) + 0.07f * Math::sin(z * 21.0f - ph)), 0.01f);
	};
	auto thick_at = [&](float z, float x) {
		return thick_mean * (0.85f + 0.1f * Math::sin(z * 9.0f + ph) + 0.05f * Math::sin(z * 31.0f + x * 17.0f));
	};
	auto top_at = [&](float x, float ov) {
		const float u = CLAMP(x / ov, 0.0f, 1.0f);
		return -droop * u * u;
	};
	Builder &cb = builders[SURFACE_CENTRE];
	const int along = int(Math::ceil(len / 0.04f));
	// A grid of verts from p(i, j), i over nu (across), j over along; wound so the outward side faces out.
	auto grid = [&](int nu, const Vector3 &p_out, auto p_point, const Color &p_tint) {
		const int first = int(cb.verts.size());
		for (int j = 0; j <= along; j++) {
			const float z = (float(j) / along - 0.5f) * len;
			for (int i = 0; i <= nu; i++) {
				const Vector3 v = p_point(float(i) / nu, z);
				cb.verts.push_back(v);
				cb.uvs.push_back(Vector2(float(i) / nu, float(j) / along));
				cb.uv2s.push_back(Vector2(0.0f, 0.0f));
				const float n = 0.85f + 0.3f * Math::sin(v.x * 41.0f + v.z * 23.0f) * Math::sin(v.y * 37.0f - v.z * 11.0f);
				cb.colors.push_back(Color(p_tint.r * n, p_tint.g * n, p_tint.b * n));
			}
		}
		// Winding: as the field's other ground meshes (cross of the first two edges along the outward side).
		const Vector3 e1 = cb.verts[first + nu + 1] - cb.verts[first];
		const Vector3 e2 = cb.verts[first + 1] - cb.verts[first];
		const bool flip = e1.cross(e2).dot(p_out) < 0.0f;
		for (int j = 0; j < along; j++) {
			for (int i = 0; i < nu; i++) {
				const int i0 = first + j * (nu + 1) + i;
				const int i1 = i0 + nu + 1;
				if (flip) {
					cb.indices.append_array(PackedInt32Array({ i0, i0 + 1, i1, i0 + 1, i1 + 1, i1 }));
				} else {
					cb.indices.append_array(PackedInt32Array({ i0, i1, i0 + 1, i0 + 1, i1, i1 + 1 }));
				}
			}
		}
	};
	// The overhang's top: the sward's thatch and root mat (olive over the soil), from just behind the edge out to the lip.
	grid(5, Vector3(0, 1, 0), [&](float u, float z) {
		const float ov = ov_at(z);
		const float x = Math::lerp(-0.06f, ov, u);
		return Vector3(x, top_at(x, ov) + 0.004f, z);
	}, Color(1.45f, 1.7f, 0.95f));
	// The cut face: down from the lip's top to its ragged foot, leaning back a little.
	grid(5, Vector3(1, 0, 0), [&](float u, float z) {
		const float ov = ov_at(z);
		const float t = thick_at(z, ov);
		const float y = Math::lerp(top_at(ov, ov) + 0.004f, -t, u);
		const float x = ov - 0.03f * u + 0.012f * Math::sin(z * 27.0f + u * 5.0f + ph);
		return Vector3(x, y, z);
	}, Color(1.0f, 1.0f, 1.0f));
	// The underside: from the face's foot back into the undercut bank, rising a little as the flow cut it.
	grid(4, Vector3(0, -1, 0), [&](float u, float z) {
		const float ov = ov_at(z) - 0.03f;
		const float x = Math::lerp(ov, -0.05f, u);
		const float y = -thick_at(z, x) + 0.04f * u * u;
		return Vector3(x, y, z);
	}, Color(0.8f, 0.78f, 0.75f)); // In its own shadow and damp.
	_tag_organ(cb);
	// The sward on the lip: blades leaning out over the edge and, past it, hanging down the face.
	const int blades = int(len * (ov_mean + 0.25f) * _rand(1300.0f, 1700.0f));
	for (int i = 0; i < blades; i++) {
		const float z = _rand(-0.48f, 0.48f) * len;
		const float ov = ov_at(z);
		const float x = _rand(-0.25f, ov);
		const float out = Math::smoothstep(-0.25f, ov, x); // 0 well back, 1 at the lip.
		const float lean = Math::lerp(0.2f, 1.25f, out) + _rand(-0.2f, 0.2f);
		const float side = _rand(-0.5f, 0.5f);
		const Vector3 dir = Vector3(Math::sin(lean) * Math::cos(side), Math::cos(lean), Math::sin(lean) * Math::sin(side)).normalized();
		Leaf l;
		l.shape = LEAF_LINEAR;
		l.length = _rand(0.07f, 0.24f) * Math::lerp(1.0f, 1.35f, out);
		l.width = _rand(0.003f, 0.007f);
		l.fold = 0.3f;
		l.bend = Math::lerp(0.3f, 1.6f, out) * _rand(0.7f, 1.2f); // Hanging over the drop.
		l.twist = _rand(-0.5f, 0.5f);
		l.segments = 4;
		const float s = _rand(0.8f, 1.15f);
		_add_leaf(SURFACE_LEAF, Transform3D(bank_frame(dir, Vector3(0, 1, 0)), Vector3(x, top_at(x, ov), z)), l, 0.0f, 0.8f, Color(s, s, s));
	}
	// Roots trailing from the underside and the face's foot: fine, pale, curling, some forked.
	const int roots = int(len * _rand(40.0f, 70.0f));
	for (int i = 0; i < roots; i++) {
		const float z = _rand(-0.47f, 0.47f) * len;
		const float ov = ov_at(z);
		const float x = rng.randf() < 0.35f ? ov - 0.03f : _rand(0.0f, ov - 0.03f);
		const Vector3 at(x, -thick_at(z, x) + 0.005f, z);
		const Vector3 dir = Vector3(_rand(-0.15f, 0.35f), -1.0f, _rand(-0.25f, 0.25f)).normalized();
		const float r0 = _rand(0.0006f, 0.0022f);
		const float rl = _rand(0.1f, 0.45f) * (0.6f + 0.4f * x / MAX(ov, 0.01f));
		const Transform3D e = _add_stem(Transform3D(bank_frame(dir, Vector3(1, 0, 0)), at), rl, r0, r0 * 0.3f, _rand(-0.4f, 0.4f), 4, 0.25f);
		if (rl > 0.25f && rng.randf() < 0.4f) {
			const Vector3 side_dir = (dir + Vector3(_rand(-0.6f, 0.6f), 0.0f, _rand(-0.6f, 0.6f))).normalized();
			_add_stem(Transform3D(bank_frame(side_dir, Vector3(1, 0, 0)), at.lerp(e.origin, 0.4f)), rl * _rand(0.25f, 0.5f), r0 * 0.5f, r0 * 0.2f, _rand(-0.5f, 0.5f), 3, 0.3f);
		}
	}
	plant_height = MAX(plant_height, 0.3f);
}

TypedArray<Resource> PlantGenerator::make_bank_turf(int p_variants, bool p_textures, const Dictionary &p_season) {
	TypedArray<Resource> out;
	Ref<PlantGenerator> gen;
	gen.instantiate();
	gen->set_textures(p_textures);
	gen->set_preset(PRESET_TURF_LIP);
	gen->apply_preset_colors();
	gen->set_seed(12600);
	gen->set_season(p_season);
	Ref<PlantSpecies> sp;
	sp.instantiate();
	TypedArray<Mesh> meshes;
	const TypedArray<ArrayMesh> v = gen->generate_variants(MAX(p_variants, 1));
	for (int i = 0; i < v.size(); i++) {
		meshes.push_back(v[i]);
	}
	sp->set_meshes(meshes);
	// Along the edge, close enough to read as one broken lip (lips 0.7 to 1.5 m long, 0.9 m apart).
	sp->set_spacing(0.9f);
	sp->set_footprint(0.3f);
	sp->set_density(0.9f);
	sp->set_scale_range(Vector2(0.85f, 1.15f));
	sp->set_align_to_ground(0.0f); // Level on the top, not tipped down the bank.
	sp->set_random_yaw(false);
	// A drop of 0.35 m or more past the edge; on ground under 35 degrees (the lip's top), where grass grows.
	sp->set_rule_range(PlantSpecies::RULE_BANK_EDGE, Vector2(0.35f, 10000.0f));
	sp->set_rule_fade(PlantSpecies::RULE_BANK_EDGE, 0.1f);
	sp->set_rule_range(PlantSpecies::RULE_SLOPE, Vector2(0.0f, 35.0f));
	sp->set_rule_fade(PlantSpecies::RULE_SLOPE, 5.0f);
	sp->set_rule_range(PlantSpecies::RULE_WATER_DEPTH, Vector2(0.0f, 0.0f));
	sp->set_visibility_range(45.0f);
	tag_species(sp, PRESET_TURF_LIP);
	out.push_back(sp);
	return out;
}

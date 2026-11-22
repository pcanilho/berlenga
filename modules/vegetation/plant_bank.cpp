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

// Roots on bare scarps (docs/ideas/vegetation.md, "Roots on bare scarps").
// - Where a river or a slump cuts a bank, the face shows the roots of what grows on top: the sward's fine roots as a
//   fibrous mat close under the top, thinning with depth, and near trees and shrubs their woody laterals. Grass roots
//   follow Jackson et al. 1996 (Oecologia 108): the share of roots above depth d (cm) is 1 - beta^d, beta 0.952 for
//   grasses, 0.970 for trees.
// - Grown by space colonization (Runions, Lane and Prusinkiewicz 2007, "Modeling trees with a space colonization
//   algorithm"): attraction points scattered on the face by that depth profile, root tips stepping toward the points
//   near them and pulled down (gravitropism), points taken once a root passes; radii by the pipe model (a parent's
//   r^2.5 the sum of its children's), so roots thicken toward the top.
// - The mesh: origin on the bank's top edge, +X out over the drop, +Z along the edge; the face falls at bank_face
//   (radians) and the roots lie on it, some dipping under its surface, the finest ends hanging free off it.

void PlantGenerator::_add_root_path(const LocalVector<Vector3> &p_points, const LocalVector<float> &p_radii, const LocalVector<float> &p_wind) {
	const int n = int(p_points.size());
	if (n < 2) {
		return;
	}
	Builder &b = builders[SURFACE_STEM];
	_begin_organ(p_points[0]);
	const int sides = p_radii[0] > 0.006f ? 7 : (p_radii[0] > 0.0015f ? 5 : 3);
	const int first = b.verts.size();
	Vector3 prev_x;
	for (int i = 0; i < n; i++) {
		const Vector3 t = (p_points[MIN(i + 1, n - 1)] - p_points[MAX(i - 1, 0)]).normalized();
		// Parallel transport of the ring's frame along the path, so the tube does not twist.
		Vector3 x = i == 0 ? (Math::abs(t.y) < 0.9f ? Vector3(0, 1, 0) : Vector3(1, 0, 0)).cross(t) : prev_x - t * prev_x.dot(t);
		if (x.length_squared() < 1e-10f) {
			x = Vector3(0, 0, 1).cross(t);
		}
		x.normalize();
		prev_x = x;
		const Vector3 y = t.cross(x);
		const float tv = float(i) / (n - 1);
		for (int s = 0; s < sides; s++) {
			const float a = s * Math::TAU / sides;
			b.verts.push_back(p_points[i] + (x * Math::cos(a) + y * Math::sin(a)) * p_radii[i]);
			b.uvs.push_back(Vector2(float(s) / sides, tv));
			b.uv2s.push_back(Vector2(p_wind[i], 0.0f));
			// The upper side lit and washed paler, the side against the face in its shadow.
			const float up = 0.5f + 0.5f * (x * Math::cos(a) + y * Math::sin(a)).dot(Vector3(0.5f, 0.85f, 0.0f).normalized());
			const float ao = 0.72f + 0.33f * up;
			b.colors.push_back(Color(ao, ao, ao));
		}
		plant_height = MAX(plant_height, p_points[i].y);
	}
	for (int i = 0; i < n - 1; i++) {
		for (int s = 0; s < sides; s++) {
			const int a = first + i * sides + s;
			const int c = first + i * sides + (s + 1) % sides;
			b.indices.append_array(PackedInt32Array({ a, c, a + sides, c, c + sides, a + sides }));
		}
	}
	_tag_organ(b);
}

void PlantGenerator::_build_bank_roots(bool p_woody) {
	const float cot = 1.0f / Math::tan(CLAMP(bank_face, 0.6f, 1.55f));
	const float len = p_woody ? _rand(1.0f, 1.8f) : _rand(0.8f, 1.3f);
	const float beta = p_woody ? 0.970f : 0.952f; // Jackson et al. 1996, per cm.
	const float depth_max = p_woody ? 0.9f : 0.55f;
	const float ph = _rand(0.0f, Math::TAU);
	// The face, in (along, depth): its surface point, standing out a little where the roots lie on it.
	auto on_face = [&](float p_z, float p_d, float p_off) {
		return Vector3(p_d * cot + p_off, -p_d, p_z);
	};
	// Attraction points, by the depth profile: Y = 1 - beta^d, d = ln(1 - Y) / ln(beta) cm.
	struct Pt {
		Vector2 p; // (along, depth), m.
		bool alive = true;
	};
	LocalVector<Pt> pts;
	const int n_pts = p_woody ? 300 : 950;
	const float y_cap = 1.0f - Math::pow(beta, depth_max * 100.0f);
	for (int i = 0; i < n_pts; i++) {
		const float y = rng.randf() * y_cap;
		const float d = Math::log(1.0f - y) / Math::log(beta) * 0.01f;
		pts.push_back({ Vector2(_rand(-0.5f, 0.5f) * len, MAX(d, 0.015f)) });
	}
	// Nodes: (along, depth), parent, radius.
	struct Node {
		Vector2 p;
		int parent = -1;
		float r = 0.0f;
		int children = 0;
		int seed = -1;
		float target = 0.0f;
	};
	LocalVector<Node> nodes;
	const int seeds = p_woody ? int(_rand(2.0f, 4.99f)) : int(len * _rand(50.0f, 70.0f));
	for (int i = 0; i < seeds; i++) {
		Node s;
		s.p = Vector2(_rand(-0.47f, 0.47f) * len, p_woody ? _rand(0.04f, 0.3f) : _rand(0.01f, 0.05f));
		nodes.push_back(s);
		if (p_woody) {
			// A woody lateral runs on along the face before it branches: its first 15 to 40 cm laid down here, sideways
			// and a little down, so the colonization branches off along it, not from one point.
			int at = int(nodes.size()) - 1;
			Vector2 dir = Vector2(rng.randf() < 0.5f ? -1.0f : 1.0f, _rand(0.15f, 0.6f)).normalized();
			const int steps = int(_rand(6.0f, 16.0f));
			for (int k = 0; k < steps; k++) {
				dir = (dir + Vector2(0.0f, _rand(-0.25f, 0.35f))).normalized();
				Node c;
				c.p = nodes[at].p + dir * 0.025f;
				c.p.x = CLAMP(c.p.x, -0.5f * len, 0.5f * len);
				c.p.y = MAX(c.p.y, 0.02f);
				c.parent = at;
				nodes[at].children++;
				nodes.push_back(c);
				at = int(nodes.size()) - 1;
			}
		}
	}
	const float step = p_woody ? 0.025f : 0.012f;
	const float influence = p_woody ? 0.3f : 0.09f;
	const float kill = p_woody ? 0.06f : 0.022f;
	const Vector2 gravity(0.0f, p_woody ? 0.35f : 0.8f); // +depth: down the face.
	for (int iter = 0; iter < (p_woody ? 80 : 60); iter++) {
		HashMap<int, Vector2> pull;
		for (const Pt &a : pts) {
			if (!a.alive) {
				continue;
			}
			int best = -1;
			float bd = influence * influence;
			for (uint32_t k = 0; k < nodes.size(); k++) {
				const float dd = nodes[k].p.distance_squared_to(a.p);
				if (dd < bd) {
					bd = dd;
					best = int(k);
				}
			}
			if (best >= 0) {
				const Vector2 dir = (a.p - nodes[best].p).normalized();
				Vector2 *have = pull.getptr(best);
				if (have) {
					*have += dir;
				} else {
					pull[best] = dir;
				}
			}
		}
		if (pull.is_empty()) {
			break;
		}
		for (const KeyValue<int, Vector2> &kv : pull) {
			Vector2 dir = kv.value.normalized() + gravity * 0.5f + Vector2(_rand(-0.25f, 0.25f), _rand(-0.1f, 0.1f));
			if (dir.length_squared() < 1e-8f) {
				continue;
			}
			dir.normalize();
			Node c;
			c.p = nodes[kv.key].p + dir * step;
			c.p.y = MAX(c.p.y, 0.008f);
			c.parent = kv.key;
			nodes[kv.key].children++;
			nodes.push_back(c);
		}
		for (Pt &a : pts) {
			if (!a.alive) {
				continue;
			}
			for (uint32_t k = 0; k < nodes.size(); k++) {
				if (nodes[k].p.distance_squared_to(a.p) < kill * kill) {
					a.alive = false;
					break;
				}
			}
		}
	}
	// Pipe model radii, from the tips up (children always follow their parent in the list).
	const float r_tip = p_woody ? 0.0016f : 0.0006f;
	const float gamma = 2.5f;
	LocalVector<float> acc;
	acc.resize(nodes.size());
	for (uint32_t k = 0; k < nodes.size(); k++) {
		acc[k] = 0.0f;
	}
	for (int k = int(nodes.size()) - 1; k >= 0; k--) {
		if (nodes[k].children == 0) {
			acc[k] = Math::pow(r_tip, gamma);
		}
		nodes[k].r = Math::pow(acc[k], 1.0f / gamma);
		if (nodes[k].parent >= 0) {
			acc[nodes[k].parent] += acc[k];
		}
	}
	// Woody roots: each lateral 1.5 to 4.5 cm across where it leaves the face, its tips kept fine (radii remapped on a
	// log scale between the tip and the base, so the pipe model's taper is kept).
	if (p_woody) {
		for (uint32_t k = 0; k < nodes.size(); k++) {
			if (nodes[k].parent >= 0) {
				nodes[k].seed = nodes[nodes[k].parent].seed;
			} else {
				nodes[k].seed = int(k);
				nodes[k].target = _rand(0.0075f, 0.0225f);
			}
		}
		LocalVector<float> e;
		e.resize(nodes.size());
		for (uint32_t k = 0; k < nodes.size(); k++) {
			const Node &sd = nodes[nodes[k].seed];
			e[k] = sd.r > r_tip * 1.01f ? Math::log(sd.target / r_tip) / Math::log(sd.r / r_tip) : 1.0f;
		}
		for (uint32_t k = 0; k < nodes.size(); k++) {
			nodes[k].r = r_tip * Math::pow(MAX(nodes[k].r / r_tip, 1.0f), e[k]);
		}
	}
	// Where a root lies: on the face, standing out a little, now and then dipping under it, bulging out at the top.
	auto lie = [&](const Node &p_n, float p_seed) {
		const float wave = Math::sin(p_n.p.x * (p_woody ? 9.0f : 23.0f) + p_seed + ph) * Math::sin(p_n.p.y * (p_woody ? 7.0f : 31.0f) - p_seed * 1.7f);
		const float off = p_n.r * (0.6f + 0.9f * wave) + (p_woody ? 0.004f : 0.0015f);
		return on_face(p_n.p.x, p_n.p.y, off);
	};
	// Paths: from each tip up to the first node already drawn (or a seed), drawn tip-last so radii run thick to thin.
	LocalVector<uint8_t> drawn;
	drawn.resize(nodes.size());
	for (uint32_t k = 0; k < nodes.size(); k++) {
		drawn[k] = 0;
	}
	for (int k = int(nodes.size()) - 1; k >= 0; k--) {
		if (nodes[k].children != 0) {
			continue;
		}
		LocalVector<int> chain;
		int at = k;
		while (at >= 0) {
			chain.push_back(at);
			if (drawn[at]) {
				break;
			}
			drawn[at] = 1;
			at = nodes[at].parent;
		}
		chain.invert();
		if (chain.size() < 4 && nodes[chain[0]].parent >= 0) {
			continue; // A stub of a step or two: drawn, it reads as a thorn on the root.
		}
		const float sd = float(chain[0]) * 0.37f;
		LocalVector<Vector3> points;
		LocalVector<float> radii;
		LocalVector<float> wind;
		if (nodes[chain[0]].parent < 0) {
			// A seed: the root comes out of the bank behind the face.
			const Node &s = nodes[chain[0]];
			const Vector3 in = on_face(s.p.x, s.p.y, -0.04f) + Vector3(0.0f, p_woody ? 0.02f : 0.006f, 0.0f);
			points.push_back(in);
			radii.push_back(s.r * 1.05f);
			wind.push_back(0.0f);
		}
		for (int id : chain) {
			points.push_back(lie(nodes[id], sd));
			radii.push_back(MAX(nodes[id].r, r_tip * 0.6f));
			wind.push_back(0.0f);
		}
		if (points.size() < 2) {
			continue;
		}
		// The finest ends hang free off the face (a tip below the top's undercut, with no face under it to lie on).
		const Node &tip = nodes[k];
		if (rng.randf() < (p_woody ? 0.5f : 0.5f)) {
			Vector3 p = points[points.size() - 1];
			const Vector3 prev = points[points.size() - 2];
			Vector3 dir = (p - prev).normalized() + Vector3(0.35f, 0.0f, 0.0f);
			dir.y = MIN(dir.y, 0.0f); // Hanging: never up.
			const int segs = int(_rand(3.0f, p_woody ? 8.0f : 11.0f));
			const float seg_len = p_woody ? 0.03f : 0.018f;
			for (int i = 0; i < segs; i++) {
				dir = (dir + Vector3(0.0f, -0.45f, 0.0f) + Vector3(_rand(-0.15f, 0.15f), 0.0f, _rand(-0.15f, 0.15f))).normalized();
				p += dir * seg_len;
				points.push_back(p);
				radii.push_back(MAX(tip.r, r_tip) * (1.0f - 0.6f * float(i + 1) / segs));
				wind.push_back(0.15f * float(i + 1) / segs);
			}
		}
		_add_root_path(points, radii, wind);
	}
	plant_height = MAX(plant_height, 0.05f);
}

TypedArray<Resource> PlantGenerator::make_bank_roots(int p_variants, bool p_textures, const Dictionary &p_season) {
	// Two species: the sward's fine roots on any bare earthen scarp, a tree's woody ones near the canopy. Each variant
	// is grown on its own face angle (mesh meta "bank_face_slope", radians); the field takes the one nearest the face.
	TypedArray<Resource> out;
	for (int kind = 0; kind < 2; kind++) {
		const bool woody = kind == 1;
		const Preset pr = woody ? PRESET_BANK_ROOTS_WOODY : PRESET_BANK_ROOTS;
		Ref<PlantGenerator> gen;
		gen.instantiate();
		gen->set_textures(p_textures);
		gen->set_preset(pr);
		gen->apply_preset_colors();
		gen->set_season(p_season);
		TypedArray<Mesh> meshes;
		const int n = MAX(p_variants, 1);
		for (int i = 0; i < n; i++) {
			// Faces from 50 to 88 degrees: slumped scarps to the near-vertical cut of an outer bend.
			gen->bank_face = Math::deg_to_rad(n > 1 ? Math::lerp(50.0f, 88.0f, float(i) / (n - 1)) : 75.0f);
			gen->set_seed((woody ? 12900 : 12800) + i);
			Ref<ArrayMesh> m = gen->generate();
			if (m.is_valid()) {
				m->set_meta(SNAME("bank_face_slope"), gen->bank_face);
			}
			meshes.push_back(m);
		}
		Ref<PlantSpecies> sp;
		sp.instantiate();
		sp->set_meshes(meshes);
		sp->set_spacing(woody ? 1.5f : 0.9f);
		sp->set_footprint(0.05f); // Under a sod lip, not instead of it.
		sp->set_density(woody ? 0.8f : 0.95f);
		sp->set_scale_range(Vector2(0.85f, 1.15f));
		sp->set_align_to_ground(0.0f);
		sp->set_random_yaw(false);
		// A bare face of 0.4 m or more past the edge, in soil (not on rock), on land.
		sp->set_rule_range(PlantSpecies::RULE_BANK_EDGE, Vector2(0.4f, 10000.0f));
		sp->set_rule_fade(PlantSpecies::RULE_BANK_EDGE, 0.1f);
		sp->set_rule_range(PlantSpecies::RULE_ROCK, Vector2(0.0f, 0.4f));
		sp->set_rule_fade(PlantSpecies::RULE_ROCK, 0.1f);
		sp->set_rule_range(PlantSpecies::RULE_WATER_DEPTH, Vector2(0.0f, 0.0f));
		if (woody) {
			// Within about 4 m of the canopy's edge (laterals reach past the crown), inside the forest too; the top under
			// 10000 so that with no forest known none are placed (an open-ended interior rule would place them anywhere).
			sp->set_rule_range(PlantSpecies::RULE_FOREST_EDGE, Vector2(-4.0f, 9000.0f));
			sp->set_rule_fade(PlantSpecies::RULE_FOREST_EDGE, 1.0f);
		}
		sp->set_visibility_range(woody ? 60.0f : 35.0f);
		tag_species(sp, pr);
		out.push_back(sp);
	}
	return out;
}

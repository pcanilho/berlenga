/**************************************************************************/
/*  tree_bark_detail.cpp                                                  */
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

#include "tree_bark_detail.h"

#include "vegetation_field_3d.h"

#include "core/math/random_pcg.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/templates/hash_set.h"
#include "scene/3d/camera_3d.h"
#include "scene/main/viewport.h"
#include "scene/resources/material.h"
#include "servers/rendering/rendering_server.h"

namespace {

// What each bark family sheds as loose pieces, in metres: a piece's mean length (up the stem; round it for curls),
// width and thickness, pieces a square metre of stem, how far its free edge lifts or curls (share of its length), the
// inner bark a broken edge or a lifted underside shows, the colour without a bark texture, and the bark tile (m).
struct BarkSpec {
	BarkDetail3D::PieceKind kind;
	float length, width, thick;
	float per_m2;
	float lift;
	Color inner;
	Color fallback;
	float tile;
};

// Sources: Vaucher, Tree Bark (2003); Wojtech, Bark: A Field Guide to Trees of the Northeast (2011).
const BarkSpec BARK_SPECS[TreeGenerator::BARK_MAX] = {
	// Smooth (beech, holly): no loose pieces.
	{ BarkDetail3D::PIECE_PLATE, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, Color(0.5f, 0.45f, 0.4f), Color(0.45f, 0.44f, 0.42f), 0.4f },
	// Furrowed (oaks, ash, lime): the ridges are the stem's own geometry; no loose pieces.
	{ BarkDetail3D::PIECE_PLATE, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, Color(0.45f, 0.35f, 0.27f), Color(0.36f, 0.33f, 0.3f), 0.4f },
	// Plated (Scots pine, larch): thin plates 5 to 15 cm lifting at their lower edge over the orange-red inner bark.
	{ BarkDetail3D::PIECE_PLATE, 0.09f, 0.055f, 0.012f, 35.0f, 0.2f, Color(0.56f, 0.34f, 0.22f), Color(0.36f, 0.29f, 0.24f), 0.4f },
	// Scaly (holm oak, blackthorn, carob): small squarish scales a few centimetres across.
	{ BarkDetail3D::PIECE_SCALE, 0.035f, 0.028f, 0.007f, 110.0f, 0.12f, Color(0.47f, 0.34f, 0.25f), Color(0.27f, 0.25f, 0.23f), 0.4f },
	// Flaking (plane-like, saltbush, calligonum): thin flakes curling out at one end, paler fresh bark under them.
	{ BarkDetail3D::PIECE_FLAKE, 0.08f, 0.05f, 0.0025f, 30.0f, 0.5f, Color(0.72f, 0.66f, 0.46f), Color(0.5f, 0.47f, 0.41f), 0.4f },
	// Papery (birches): horizontal strips of the white outer bark peeling round the stem and rolling outward.
	// The rolled underside is the paper's pale salmon-cream inner face, not the darker living bark.
	{ BarkDetail3D::PIECE_CURL, 0.2f, 0.045f, 0.0008f, 22.0f, 1.0f, Color(0.8f, 0.68f, 0.58f), Color(0.88f, 0.86f, 0.82f), 0.4f },
	// Lenticelled (hazel, rowan): thin skin peeling in short strips.
	{ BarkDetail3D::PIECE_CURL, 0.04f, 0.012f, 0.0005f, 6.0f, 0.8f, Color(0.58f, 0.46f, 0.35f), Color(0.5f, 0.45f, 0.4f), 0.4f },
	// Stone pine: long flat grey-brown plates over orange-red fissures.
	{ BarkDetail3D::PIECE_PLATE, 0.18f, 0.07f, 0.015f, 14.0f, 0.12f, Color(0.6f, 0.36f, 0.24f), Color(0.45f, 0.4f, 0.36f), 0.4f },
	// Cork: its ridges are the stem's own geometry.
	{ BarkDetail3D::PIECE_PLATE, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, Color(0.6f, 0.35f, 0.22f), Color(0.55f, 0.5f, 0.44f), 1.2f },
	// Maritime pine: thick dark plates a hand across over dull red-brown.
	{ BarkDetail3D::PIECE_PLATE, 0.15f, 0.09f, 0.018f, 14.0f, 0.16f, Color(0.5f, 0.25f, 0.15f), Color(0.26f, 0.21f, 0.18f), 0.9f },
};

float bd_noise(uint32_t p_seed, int p_i) {
	uint32_t h = p_seed * 2654435761u ^ uint32_t(p_i) * 2246822519u;
	h ^= h >> 15;
	h *= 2221713035u;
	h ^= h >> 16;
	return float(h & 0xffffff) / float(0xffffff);
}

struct ShapeBuilder {
	PackedVector3Array verts;
	PackedVector3Array normals;
	PackedFloat32Array tangents;
	PackedColorArray colors;
	PackedVector2Array uvs;
	PackedInt32Array indices;
	int group_start = 0;

	int add(const Vector3 &p_v, float p_outer, float p_edge = 0.0f) {
		verts.push_back(p_v);
		normals.push_back(Vector3());
		colors.push_back(Color(p_outer, p_edge, 0, 1));
		uvs.push_back(Vector2(p_v.x, p_v.y));
		return verts.size() - 1;
	}
	// A triangle facing p_out's side (so the winding is right whichever way it was listed).
	void tri(int p_a, int p_b, int p_c, const Vector3 &p_out) {
		const Vector3 n = (verts[p_b] - verts[p_a]).cross(verts[p_c] - verts[p_a]);
		if (n.dot(p_out) > 0.0f) {
			// Godot's front faces wind clockwise: the cross product of a front face points away from the viewer.
			SWAP(p_b, p_c);
		}
		indices.push_back(p_a);
		indices.push_back(p_b);
		indices.push_back(p_c);
	}
	void quad(int p_a, int p_b, int p_c, int p_d, const Vector3 &p_out) {
		tri(p_a, p_b, p_c, p_out);
		tri(p_a, p_c, p_d, p_out);
	}
	// Smooth normals over the triangles listed since the last group: area weighted, pointing out.
	void close_group() {
		for (int t = group_start; t + 2 < indices.size(); t += 3) {
			const int a = indices[t], b = indices[t + 1], c = indices[t + 2];
			const Vector3 n = (verts[c] - verts[a]).cross(verts[b] - verts[a]); // out, for a clockwise front face
			normals.write[a] += n;
			normals.write[b] += n;
			normals.write[c] += n;
		}
		group_start = indices.size();
	}
	Ref<ArrayMesh> finish() {
		tangents.resize(verts.size() * 4);
		for (int i = 0; i < verts.size(); i++) {
			Vector3 n = normals[i];
			n = n.length_squared() > 1e-20f ? n.normalized() : Vector3(0, 0, 1);
			normals.write[i] = n;
			Vector3 t = Vector3(1, 0, 0) - n * n.x;
			t = t.length_squared() > 1e-8f ? t.normalized() : Vector3(0, 1, 0);
			tangents.write[i * 4 + 0] = t.x;
			tangents.write[i * 4 + 1] = t.y;
			tangents.write[i * 4 + 2] = t.z;
			tangents.write[i * 4 + 3] = 1.0f;
		}
		Array arrays;
		arrays.resize(Mesh::ARRAY_MAX);
		arrays[Mesh::ARRAY_VERTEX] = verts;
		arrays[Mesh::ARRAY_NORMAL] = normals;
		arrays[Mesh::ARRAY_TANGENT] = tangents;
		arrays[Mesh::ARRAY_COLOR] = colors;
		arrays[Mesh::ARRAY_TEX_UV] = uvs;
		arrays[Mesh::ARRAY_INDEX] = indices;
		Ref<ArrayMesh> mesh;
		mesh.instantiate();
		mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
		return mesh;
	}
};

} // namespace

/* Shapes */

BarkDetail3D::PieceKind BarkDetail3D::_piece_kind(TreeGenerator::BarkKind p_bark) {
	return BARK_SPECS[CLAMP(int(p_bark), 0, int(TreeGenerator::BARK_MAX) - 1)].kind;
}

real_t BarkDetail3D::get_piece_size(int p_bark) {
	const BarkSpec &bs = BARK_SPECS[CLAMP(p_bark, 0, int(TreeGenerator::BARK_MAX) - 1)];
	return bs.per_m2 > 0.0f ? 0.5f * (bs.length + bs.width) : 0.0f;
}

// One piece in its own frame: x round the stem, y up it, z out of it; the stem's surface at z = 0, the piece's
// origin at its middle where it lies on the stem. Plates, scales and flakes: an irregular outline (a ring of 14
// points, each its own radius) as a thin slab, domed a little, its free edge lifted (plates and scales at the lower
// edge, as weathered pine plates lift; flakes curl out at the top). Curls: a strip round the stem, lying on it for
// its first part and rolled outward over the rest. COLOR.r: 1 the weathered outer face, 0 the inner face, 0.4 the
// broken edges.
Ref<ArrayMesh> BarkDetail3D::_make_shape(TreeGenerator::BarkKind p_bark, int p_shape) {
	const BarkSpec &bs = BARK_SPECS[p_bark];
	const uint32_t seed = uint32_t(p_bark) * 977u + uint32_t(p_shape) * 131u + 17u;
	ShapeBuilder sb;
	if (bs.kind == PIECE_CURL && p_shape >= 6) {
		// Where a sheet has come away: a torn horizontal patch of the paler inner bark, flush on the stem (bent to a
		// typical stem's curvature), the strips standing off at its ends. Its top shows the inner bark.
		const int K = 14;
		const float L = bs.length * (1.0f + 0.8f * bd_noise(seed, 0));
		const float W = bs.width * (0.8f + 0.8f * bd_noise(seed, 1));
		const float T = 0.0004f;
		const float stem_r = 0.12f;
		int top[K + 1][2], bot[K + 1][2];
		for (int i = 0; i <= K; i++) {
			const float x = (float(i) / K - 0.5f) * L;
			const float t = Math::abs(2.0f * float(i) / K - 1.0f);
			const float taper = 1.0f - 0.6f * Math::pow(t, 2.5f) * (0.5f + bd_noise(seed, 300 + i));
			const float z = -x * x / (2.0f * stem_r) + 0.0004f;
			for (int j = 0; j < 2; j++) {
				const float y = (j == 0 ? -0.5f : 0.5f) * W * taper * (0.75f + 0.35f * bd_noise(seed, 320 + i * 2 + j));
				top[i][j] = sb.add(Vector3(x, y, z + 0.5f * T), 0.5f); // the next ply down: paler, part salmon
				bot[i][j] = sb.add(Vector3(x, y, z - 0.5f * T), 0.0f);
			}
		}
		for (int i = 0; i < K; i++) {
			sb.quad(top[i][0], top[i + 1][0], top[i + 1][1], top[i][1], Vector3(0, 0, 1));
		}
		sb.close_group();
		for (int i = 0; i < K; i++) {
			sb.quad(bot[i][0], bot[i + 1][0], bot[i + 1][1], bot[i][1], Vector3(0, 0, -1));
		}
		sb.close_group();
		return sb.finish();
	}
	if (bs.kind == PIECE_CURL) {
		// A horizontal strip torn along the lenticels: its middle still on the stem (bent to a typical stem's
		// curvature), one or both ends peeling off it and rolled into a scroll standing off the bark.
		const int M = 16; // samples an arm
		// Shapes 0 and 1 are long strips peeling at both ends, 2 and 3 wide sheets, 4 and 5 single curls; 6 and 7 are
		// the bared patches above.
		const float L = bs.length * (0.75f + 0.5f * bd_noise(seed, 0)) * (p_shape < 2 ? 1.8f : 1.0f);
		const float W = bs.width * (0.7f + 0.6f * bd_noise(seed, 1)) * (p_shape == 2 || p_shape == 3 ? 1.8f : 1.0f);
		const float T = bs.thick * 1.8f; // paper bark peels several plies thick
		const bool both = p_shape < 2 || bd_noise(seed, 5) < 0.3f;
		const float stem_r = 0.12f;
		const float lift_angle = 1.2f + 0.4f * bd_noise(seed, 8); // lifts steeply, so the small roll clears the stem
		const float side = bd_noise(seed, 4) < 0.5f ? -1.0f : 1.0f;
		struct Arm {
			LocalVector<Vector2> p, n; // (round, out)
		};
		auto make_arm = [&](float p_len, float p_attached, float p_roll, float p_dir) {
			Arm arm;
			Vector2 q(0, 0);
			float h = 0.0f;
			const float ds = p_len / M;
			for (int i = 0; i <= M; i++) {
				const float sa = float(i) * ds;
				arm.p.push_back(Vector2(q.x * p_dir, q.y));
				arm.n.push_back(Vector2(-Math::sin(h) * p_dir, Math::cos(h)));
				float k;
				if (sa < p_attached) {
					k = -1.0f / stem_r; // lies on the stem
				} else {
					const float f = (sa - p_attached) / MAX(p_len - p_attached, 1e-4f);
					// Peels off the stem at an angle, then rolls back on itself (birch bark rolls with its white face out,
					// the salmon underside showing at the lip and under the lift).
					const float free = MAX(p_len - p_attached, 1e-4f);
					k = f < 0.22f ? lift_angle / (0.22f * free) : -1.0f / (p_roll * (1.0f - 0.3f * f));
				}
				h += k * ds;
				q += Vector2(Math::cos(h), Math::sin(h)) * ds;
			}
			return arm;
		};
		const float roll = 0.004f + 0.003f * bd_noise(seed, 3); // a tight scroll hugging the stem
		Arm right, left;
		if (both) {
			right = make_arm(0.5f * L, 0.12f * L, roll, side);
			left = make_arm(0.5f * L * (0.6f + 0.4f * bd_noise(seed, 6)), 0.12f * L, roll * (0.8f + 0.4f * bd_noise(seed, 7)), -side);
		} else {
			right = make_arm(0.7f * L, 0.2f * L, roll, side);
			left = make_arm(0.3f * L, 1.0f, roll, -side); // the far end stays on the stem
		}
		LocalVector<Vector2> c, n;
		for (int i = M; i >= 1; i--) {
			c.push_back(left.p[i]);
			n.push_back(left.n[i]);
		}
		for (int i = 0; i <= M; i++) {
			c.push_back(right.p[i]);
			n.push_back(right.n[i]);
		}
		const int K = int(c.size());
		// Torn paper: each long edge wanders (two octaves), and the strip narrows raggedly toward each free end.
		LocalVector<float> ylo, yhi;
		for (int i = 0; i < K; i++) {
			const float t = Math::abs(float(i - M)) / M; // 0 middle, 1 at the ends
			const float taper = 1.0f - 0.45f * Math::pow(t, 3.0f) * (0.6f + 0.8f * bd_noise(seed, 200 + i));
			ylo.push_back(-0.5f * W * taper * (0.85f + 0.2f * bd_noise(seed, 40 + i) + 0.1f * bd_noise(seed, 80 + i / 3)));
			yhi.push_back(0.5f * W * taper * (0.85f + 0.2f * bd_noise(seed, 120 + i) + 0.1f * bd_noise(seed, 160 + i / 3)));
		}
		auto at = [&](int i, float y, float off) {
			const Vector2 q = c[i] + n[i] * off;
			return Vector3(q.x, y, q.y);
		};
		LocalVector<int> top, bot; // 3 across: low edge, middle, high edge
		for (int i = 0; i < K; i++) {
			for (int j = 0; j < 3; j++) {
				const float y = j == 0 ? ylo[i] : (j == 2 ? yhi[i] : 0.5f * (ylo[i] + yhi[i]));
				const float sag = j == 1 ? 0.0f : 0.1f * W * (Math::abs(float(i - M)) / M); // a little cupped where free
				top.push_back(sb.add(at(i, y, 0.5f * T + sag), 1.0f));
				bot.push_back(sb.add(at(i, y, -0.5f * T + sag), 0.0f));
			}
		}
		auto nrm = [&](int i) { return Vector3(n[i].x, 0.0f, n[i].y); };
		for (int i = 0; i + 1 < K; i++) {
			for (int j = 0; j < 2; j++) {
				sb.quad(top[i * 3 + j], top[(i + 1) * 3 + j], top[(i + 1) * 3 + j + 1], top[i * 3 + j + 1], nrm(i));
			}
		}
		sb.close_group();
		for (int i = 0; i + 1 < K; i++) {
			for (int j = 0; j < 2; j++) {
				sb.quad(bot[i * 3 + j], bot[(i + 1) * 3 + j], bot[(i + 1) * 3 + j + 1], bot[i * 3 + j + 1], -nrm(i));
			}
		}
		sb.close_group();
		for (int e = 0; e < 2; e++) {
			const int j = e == 0 ? 0 : 2;
			const Vector3 out(0, e == 0 ? -1.0f : 1.0f, 0);
			for (int i = 0; i + 1 < K; i++) {
				const int qa = sb.add(sb.verts[top[i * 3 + j]], 1.0f, 1.0f), qb = sb.add(sb.verts[top[(i + 1) * 3 + j]], 1.0f, 1.0f);
				const int qc = sb.add(sb.verts[bot[(i + 1) * 3 + j]], 1.0f, 1.0f), qd = sb.add(sb.verts[bot[i * 3 + j]], 1.0f, 1.0f);
				sb.quad(qa, qb, qc, qd, out);
			}
			sb.close_group();
		}
		for (int e = 0; e < 2; e++) {
			const int i = e == 0 ? 0 : K - 1;
			const int i2 = e == 0 ? 1 : K - 2;
			const Vector3 out = (Vector3(c[i].x, 0, c[i].y) - Vector3(c[i2].x, 0, c[i2].y)).normalized();
			for (int j = 0; j < 2; j++) {
				const int qa = sb.add(sb.verts[top[i * 3 + j]], 1.0f, 1.0f), qb = sb.add(sb.verts[top[i * 3 + j + 1]], 1.0f, 1.0f);
				const int qc = sb.add(sb.verts[bot[i * 3 + j + 1]], 1.0f, 1.0f), qd = sb.add(sb.verts[bot[i * 3 + j]], 1.0f, 1.0f);
				sb.quad(qa, qb, qc, qd, out);
			}
			sb.close_group();
		}
		return sb.finish();
	}
	// Slabs.
	const int N = 14;
	const float L = bs.length * (0.8f + 0.4f * bd_noise(seed, 0));
	const float W = bs.width * (0.8f + 0.4f * bd_noise(seed, 1));
	const float T = bs.thick * (0.8f + 0.4f * bd_noise(seed, 2));
	const float lift = bs.lift * (0.6f + 0.8f * bd_noise(seed, 3));
	float radius[N];
	for (int i = 0; i < N; i++) {
		// Two octaves of noise round the outline: broken, a little angular.
		radius[i] = 0.82f + 0.24f * bd_noise(seed, 20 + i) + 0.1f * bd_noise(seed, 40 + i / 2);
	}
	auto mid = [&](float p_x, float p_y) {
		// The middle surface: lies on the stem, its free edge off it.
		const float yn = CLAMP(p_y / (0.5f * L), -1.0f, 1.0f);
		const float xn = CLAMP(p_x / (0.5f * W), -1.0f, 1.0f);
		float z;
		if (bs.kind == PIECE_FLAKE) {
			z = lift * L * MAX(0.0f, yn - 0.1f) * MAX(0.0f, yn - 0.1f) + 0.08f * L * xn * xn; // curls out at the top
		} else {
			z = lift * L * 0.5f * MAX(0.0f, -yn) * MAX(0.0f, -yn) * (1.0f + 0.3f * xn); // the lower edge lifts
		}
		return z + 0.3f * T; // on the plate it peels from, its underside a little into the relief
	};
	auto thick = [&](float p_rho) { return T * (0.75f + 0.25f * (1.0f - p_rho * p_rho)); };
	const float rings[2] = { 0.5f, 1.0f };
	int tc = sb.add(Vector3(0, 0, mid(0, 0) + 0.5f * thick(0)), 1.0f);
	int bc = sb.add(Vector3(0, 0, mid(0, 0) - 0.5f * thick(0)), 0.0f);
	int top[2][N], bot[2][N];
	for (int r = 0; r < 2; r++) {
		for (int i = 0; i < N; i++) {
			const float a = float(Math::TAU) * (float(i) + 0.3f * (bd_noise(seed, 60 + i) - 0.5f)) / N;
			const float rho = rings[r] * (r == 1 ? radius[i] : 0.5f * (1.0f + radius[i]) * 0.9f);
			const float x = 0.5f * W * rho * Math::cos(a), y = 0.5f * L * rho * Math::sin(a);
			const float z = mid(x, y);
			const float h = 0.5f * thick(rings[r]);
			top[r][i] = sb.add(Vector3(x, y, z + h), 1.0f);
			bot[r][i] = sb.add(Vector3(x, y, z - h), 0.0f);
		}
	}
	const Vector3 up(0, 0, 1);
	for (int i = 0; i < N; i++) {
		const int k = (i + 1) % N;
		sb.tri(tc, top[0][i], top[0][k], up);
		sb.quad(top[0][i], top[1][i], top[1][k], top[0][k], up);
	}
	sb.close_group();
	for (int i = 0; i < N; i++) {
		const int k = (i + 1) % N;
		sb.tri(bc, bot[0][i], bot[0][k], -up);
		sb.quad(bot[0][i], bot[1][i], bot[1][k], bot[0][k], -up);
	}
	sb.close_group();
	// The broken edge, round the outline: a soft normal round it (a worn edge, not a cut one).
	int et[N], eb[N];
	for (int i = 0; i < N; i++) {
		et[i] = sb.add(sb.verts[top[1][i]], 1.0f, 1.0f);
		eb[i] = sb.add(sb.verts[bot[1][i]], 1.0f, 1.0f);
	}
	for (int i = 0; i < N; i++) {
		const int k = (i + 1) % N;
		const Vector3 m = 0.5f * (sb.verts[et[i]] + sb.verts[et[k]]);
		sb.quad(et[i], et[k], eb[k], eb[i], Vector3(m.x, m.y, 0.0f));
	}
	sb.close_group();
	return sb.finish();
}

/* Material */

Ref<Shader> BarkDetail3D::_shader() {
	if (shader.is_valid()) {
		return shader;
	}
	shader.instantiate();
	shader->set_code(R"(
shader_type spatial;
render_mode blend_mix, depth_draw_opaque, cull_back, diffuse_burley, specular_schlick_ggx;

// Loose bark pieces (BarkDetail3D): the stem's own bark texture on the weathered outer face, the inner bark on the
// underside and the broken edges.
uniform sampler2D bark_albedo : source_color, filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D bark_normal : hint_normal, filter_linear_mipmap_anisotropic, repeat_enable;
uniform bool has_albedo = false;
uniform bool has_normal = false;
uniform vec4 bark_tint : source_color = vec4(1.0);
uniform vec3 fallback_color : source_color = vec3(0.36, 0.3, 0.25);
uniform vec3 inner_color : source_color = vec3(0.6, 0.35, 0.2);
uniform float bark_tile = 0.4;
uniform float lift_tint = 0.0; // paper bark: the peeled layers off the stem show their cream-salmon inner plies

varying vec4 v_custom;
varying vec3 v_local;

void vertex() {
	v_custom = INSTANCE_CUSTOM;
	v_local = VERTEX;
}

float bd_hash(vec3 p) {
	p = fract(p * 0.3183099 + 0.1);
	p *= 17.0;
	return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float bd_noise(vec3 x) {
	vec3 i = floor(x);
	vec3 f = fract(x);
	f = f * f * (3.0 - 2.0 * f);
	return mix(mix(mix(bd_hash(i), bd_hash(i + vec3(1, 0, 0)), f.x), mix(bd_hash(i + vec3(0, 1, 0)), bd_hash(i + vec3(1, 1, 0)), f.x), f.y),
			mix(mix(bd_hash(i + vec3(0, 0, 1)), bd_hash(i + vec3(1, 0, 1)), f.x), mix(bd_hash(i + vec3(0, 1, 1)), bd_hash(i + vec3(1, 1, 1)), f.x), f.y), f.z);
}

void fragment() {
	vec2 uv = UV / bark_tile + fract(v_custom.w * vec2(13.1, 7.7));
	float o = smoothstep(0.3, 0.7, COLOR.r);
	vec3 outer = has_albedo ? texture(bark_albedo, uv).rgb * bark_tint.rgb : fallback_color * (0.8 + 0.4 * bd_noise(v_local * 180.0 + v_custom.w * 50.0));
	outer *= v_custom.rgb * (0.85 + 0.3 * fract(v_custom.w * 31.3));
	outer = mix(outer, inner_color * (0.85 + 0.15 * bd_noise(v_local * 90.0)), lift_tint * smoothstep(0.002, 0.012, v_local.z));
	// Fresh inner bark: smoother, mottled, darker where it has been open longest (toward the piece's edge).
	float n = bd_noise(v_local * 260.0 + v_custom.w * 37.0);
	// The inner bark in the stem's own hue (its tint, brightness kept), so it is the same tree, a fresher layer.
	vec3 tint = v_custom.rgb / max(dot(v_custom.rgb, vec3(0.299, 0.587, 0.114)), 0.05);
	vec3 inner = inner_color * mix(vec3(1.0), tint, 0.5) * (0.8 + 0.35 * n);
	// Broken edges: the weathered layers in section, the outer bark darkened, a trace of the inner.
	vec3 edge = mix(outer, inner, 0.2) * (0.6 + 0.2 * n);
	ALBEDO = mix(mix(inner, outer, o), edge, COLOR.g);
	ROUGHNESS = mix(0.68, 0.9, o);
	if (has_normal) {
		NORMAL_MAP = mix(vec3(0.5, 0.5, 1.0), texture(bark_normal, uv).rgb, o * (1.0 - COLOR.g));
	}
}
)");
	return shader;
}

Ref<Mesh> BarkDetail3D::_trunk_mesh_of(const Dictionary &p_trunk) const {
	Ref<Mesh> mesh = p_trunk.get("mesh", Ref<Mesh>());
	if (mesh.is_null() && !manual) {
		const VegetationField3D *veg = Object::cast_to<VegetationField3D>(get_node_or_null(vegetation_path));
		const int s = p_trunk.get("species", -1), v = p_trunk.get("variant", -1);
		if (veg && veg->get_biome().is_valid() && s >= 0 && s < veg->get_biome()->get_species().size()) {
			const Ref<PlantSpecies> sp = veg->get_biome()->get_species()[s];
			if (sp.is_valid() && v >= 0 && v < sp->get_meshes().size()) {
				mesh = sp->get_meshes()[v];
			}
		}
	}
	return mesh;
}

static int bd_bark_surface(const Ref<Mesh> &p_mesh) {
	const Ref<ArrayMesh> am = p_mesh;
	if (am.is_valid()) {
		for (int i = 0; i < am->get_surface_count(); i++) {
			if (am->surface_get_name(i) == "bark") {
				return i;
			}
		}
	}
	return p_mesh.is_valid() && p_mesh->get_surface_count() > 0 ? 0 : -1;
}

Ref<Material> BarkDetail3D::_bark_material_of(const Dictionary &p_trunk) const {
	const Ref<Mesh> mesh = _trunk_mesh_of(p_trunk);
	const int si = bd_bark_surface(mesh);
	return si >= 0 ? mesh->surface_get_material(si) : Ref<Material>();
}

// The stem's capsule chain (mesh space), as VegetationField3D.get_trunks_near follows it, up to p_cap metres.
BarkDetail3D::Profile *BarkDetail3D::_profile_chain(const PackedFloat32Array &c, float p_cap, int p_start) {
	if (c.size() < p_start + 8) {
		return nullptr;
	}
	Profile *pr = memnew(Profile);
	Vector3 prev(c[p_start], c[p_start + 1], c[p_start + 2]);
	pr->a0 = prev;
	float along = 0.0f;
	for (int k = p_start; k + 7 < c.size(); k += 8) {
		const Vector3 a(c[k], c[k + 1], c[k + 2]), b(c[k + 3], c[k + 4], c[k + 5]);
		if (k > p_start && a.distance_to(prev) > 1e-3f) {
			break;
		}
		const float l = a.distance_to(b);
		if (l > 1e-3f) {
			const Vector3 u = (b - a) / l;
			const Vector3 ref = Math::abs(u.x) < 0.9f ? Vector3(1, 0, 0) : Vector3(0, 0, 1);
			const Vector3 e1 = (ref - u * u.dot(ref)).normalized();
			pr->a.push_back(a);
			pr->u.push_back(u);
			pr->e1.push_back(e1);
			pr->e2.push_back(u.cross(e1));
			pr->len.push_back(l);
			pr->r.push_back(c[k + 6]);
			pr->along0.push_back(along);
			along += l;
		}
		prev = b;
		if (along >= p_cap) {
			break;
		}
	}
	if (pr->a.is_empty()) {
		memdelete(pr);
		return nullptr;
	}
	pr->total = MIN(along, p_cap);
	pr->nl = MAX(1, int(Math::ceil(pr->total / pr->step)));
	return pr;
}

// The trunk's stems: its main chain, then each further chain that leaves it low (below p_cap up the main stem), thick
// (6 cm or more) and heading up, as the second stem of a forked birch does; linked through `next`, each capped where
// the main stem's cap is. Branches are left out.
BarkDetail3D::Profile *BarkDetail3D::_profile_stems(const PackedFloat32Array &c, float p_cap) {
	Profile *head = _profile_chain(c, p_cap, 0);
	if (!head) {
		return nullptr;
	}
	const Vector3 up = head->u[0];
	head->first = head;
	Profile *tail = head;
	Vector3 prev(c[0], c[1], c[2]);
	int chains = 0;
	for (int k = 0; k + 7 < c.size() && chains < 4; k += 8) {
		const Vector3 a(c[k], c[k + 1], c[k + 2]), b(c[k + 3], c[k + 4], c[k + 5]);
		const bool starts = k > 0 && a.distance_to(prev) > 1e-3f;
		prev = b;
		if (!starts) {
			continue;
		}
		const float h = (a - head->a0).dot(up);
		const float l = a.distance_to(b);
		if (h > p_cap - 0.4f || c[k + 6] < 0.06f || l < 1e-3f || (b - a).dot(up) / l < 0.6f) {
			continue;
		}
		Profile *st = _profile_chain(c, p_cap - h, k);
		if (st) {
			st->skip = 0.15f; // clear of the crotch
			st->base_h = h;
			st->a0 = head->a0; // the trunk's base: where the instance transform puts it
			st->first = head;
			tail->next = st;
			tail = st;
			chains++;
		}
	}
	return head;
}

Array BarkDetail3D::make_bark_profile(const PackedVector3Array &p_vertices, const PackedColorArray &p_colors, const PackedFloat32Array &p_colliders) {
	Profile *stems = _profile_stems(p_colliders, PROFILE_CAP);
	Array out;
	for (Profile *pr = stems; pr; pr = pr->next) {
		pr->verts = p_vertices;
		pr->colors = p_colors;
		_build_profile(pr);
		const int n = pr->nl * pr->na;
		PackedFloat32Array head = { float(pr->na), float(pr->nl), pr->step };
		PackedFloat32Array rad;
		rad.resize(n);
		PackedByteArray col;
		col.resize(n * 3);
		uint8_t *cw = col.ptrw();
		for (int i = 0; i < n; i++) {
			rad.set(i, pr->rad[i]);
			for (int ch = 0; ch < 3; ch++) {
				cw[i * 3 + ch] = uint8_t(CLAMP(int(Math::round(pr->col[i][ch] * 127.5f)), 0, 255)); // 0..2
			}
		}
		out.push_back(Array{ head, rad, col });
	}
	_free_profile(stems);
	return out;
}

void BarkDetail3D::_free_profile(Profile *p_profile) {
	while (p_profile) {
		Profile *n = p_profile->next;
		memdelete(p_profile);
		p_profile = n;
	}
}

BarkDetail3D::Profile *BarkDetail3D::_profile(const Ref<Mesh> &p_mesh) {
	if (p_mesh.is_null()) {
		return nullptr;
	}
	if (Profile **found = profiles.getptr(p_mesh.ptr())) {
		return *found;
	}
	Profile *pr = nullptr;
	const PackedFloat32Array c = p_mesh->get_meta(SNAME("tree_colliders"), PackedFloat32Array());
	const Array packed = p_mesh->get_meta(SNAME("tree_bark_profile"), Array());
	if (!packed.is_empty() && packed[0].get_type() == Variant::ARRAY) {
		// Binned when the tree was built (TreeGenerator, make_bark_profile): nothing to read back. One entry a stem,
		// in the order _profile_stems finds them.
		pr = _profile_stems(c, PROFILE_CAP);
		int i = 0;
		bool ok = pr != nullptr;
		for (Profile *st = pr; st && ok; st = st->next, i++) {
			ok = i < packed.size();
			if (!ok) {
				break;
			}
			const Array e = packed[i];
			ok = e.size() == 3;
			if (!ok) {
				break;
			}
			const PackedFloat32Array head = e[0], rad = e[1];
			const PackedByteArray col = e[2];
			ok = head.size() >= 3;
			if (!ok) {
				break;
			}
			st->na = int(head[0]);
			st->step = head[2];
			st->nl = MIN(st->nl, int(head[1]));
			const int n = st->nl * st->na;
			ok = rad.size() >= n && col.size() >= n * 3;
			if (!ok) {
				break;
			}
			st->rad.resize(n);
			st->col.resize(n);
			for (int q = 0; q < n; q++) {
				st->rad[q] = rad[q];
				st->col[q] = Color(col[q * 3] / 127.5f, col[q * 3 + 1] / 127.5f, col[q * 3 + 2] / 127.5f);
			}
			st->built = true;
		}
		if (!ok) {
			_free_profile(pr);
			pr = nullptr;
		}
	}
	const int si = bd_bark_surface(p_mesh);
	if (!pr && si >= 0) {
		// A mesh without one (or from an older build): its bark surface read back here (a stall on the render
		// thread), binned on the worker.
		pr = _profile_stems(c, MIN(float(height) + 0.2f, PROFILE_CAP));
		if (pr) {
			const Array arrays = p_mesh->surface_get_arrays(si);
			for (Profile *st = pr; st; st = st->next) {
				st->verts = arrays[Mesh::ARRAY_VERTEX];
				st->colors = arrays[Mesh::ARRAY_COLOR];
			}
		}
	}
	profiles.insert(p_mesh.ptr(), pr);
	return pr;
}

// Bins a profile's bark surface (any thread; once).
void BarkDetail3D::_build_profile(Profile *pr) {
	if (!pr || pr->built) {
		return;
	}
	{
		const int n = pr->nl * pr->na;
		pr->rad.resize(n);
		pr->col.resize(n);
		LocalVector<int> cnt;
		cnt.resize(n);
		for (int i = 0; i < n; i++) {
			pr->rad[i] = 0.0f;
			pr->col[i] = Color(0, 0, 0, 0);
			cnt[i] = 0;
		}
		const PackedVector3Array &verts = pr->verts;
		const PackedColorArray &colors = pr->colors;
		const bool has_col = colors.size() == verts.size();
		for (int vi = 0; vi < verts.size(); vi++) {
			const Vector3 v = verts[vi];
			int best = -1;
			float best_d = 1e9f, best_t = 0.0f;
			for (uint32_t sgi = 0; sgi < pr->a.size(); sgi++) {
				const float t = (v - pr->a[sgi]).dot(pr->u[sgi]);
				if (t < -0.03f || t > pr->len[sgi] + 0.03f) {
					continue;
				}
				const float d = (v - pr->a[sgi] - pr->u[sgi] * t).length();
				if (d < best_d && d < 1.8f * pr->r[sgi] + 0.05f) {
					best = int(sgi);
					best_d = d;
					best_t = t;
				}
			}
			if (best < 0) {
				continue;
			}
			// A vertex on another stem of the trunk (closer to its surface, measured in radii) is that stem's: at a
			// fork, each stem's scan keeps only its own bark, never the other stem across the crotch.
			bool other = false;
			const float mine = best_d / MAX(pr->r[best], 0.01f);
			for (const Profile *o = pr->first; o && !other; o = o->next) {
				if (o == pr) {
					continue;
				}
				for (uint32_t sgi = 0; sgi < o->a.size(); sgi++) {
					const float t = (v - o->a[sgi]).dot(o->u[sgi]);
					if (t < 0.0f || t > o->len[sgi]) {
						continue;
					}
					if ((v - o->a[sgi] - o->u[sgi] * t).length() / MAX(o->r[sgi], 0.01f) < mine) {
						other = true;
						break;
					}
				}
			}
			if (other) {
				continue;
			}
			const float al = pr->along0[best] + CLAMP(best_t, 0.0f, pr->len[best]);
			const int li = int(al / pr->step);
			if (li < 0 || li >= pr->nl) {
				continue;
			}
			const Vector3 d = v - pr->a[best] - pr->u[best] * best_t;
			float th = Math::atan2(d.dot(pr->e2[best]), d.dot(pr->e1[best]));
			if (th < 0.0f) {
				th += float(Math::TAU);
			}
			const int ai = CLAMP(int(th / float(Math::TAU) * pr->na), 0, pr->na - 1);
			const int idx = li * pr->na + ai;
			pr->rad[idx] = MAX(pr->rad[idx], best_d);
			if (has_col) {
				pr->col[idx] += colors[vi];
				cnt[idx]++;
			}
		}
		// Cells far off their row (another stem at a fork, a branch collar, a crotch's inner faces): dropped, filled
		// like empty ones below.
		{
			LocalVector<float> row;
			for (int li = 0; li < pr->nl; li++) {
				row.clear();
				for (int ai = 0; ai < pr->na; ai++) {
					if (pr->rad[li * pr->na + ai] > 0.0f) {
						row.push_back(pr->rad[li * pr->na + ai]);
					}
				}
				if (row.size() < 8) {
					continue;
				}
				row.sort();
				const float med = row[row.size() / 2];
				for (int ai = 0; ai < pr->na; ai++) {
					float &rr = pr->rad[li * pr->na + ai];
					if (Math::abs(rr - med) > MAX(0.03f, 0.25f * med)) {
						rr = 0.0f;
					}
				}
			}
		}
		// Cells no vertex fell in: the mean of the filled ones round them (a few passes), else the capsule.
		for (int pass = 0; pass < 4; pass++) {
			for (int li = 0; li < pr->nl; li++) {
				for (int ai = 0; ai < pr->na; ai++) {
					const int idx = li * pr->na + ai;
					if (pr->rad[idx] > 0.0f) {
						continue;
					}
					float sum = 0.0f;
					int m = 0;
					for (int dl = -1; dl <= 1; dl++) {
						for (int da = -2; da <= 2; da++) {
							const int l2 = li + dl;
							if (l2 < 0 || l2 >= pr->nl) {
								continue;
							}
							const float rr = pr->rad[l2 * pr->na + (ai + da + pr->na) % pr->na];
							if (rr > 0.0f) {
								sum += rr;
								m++;
							}
						}
					}
					if (m > 0) {
						pr->rad[idx] = -sum / m; // negative: filled this pass, read as filled from the next
					}
				}
			}
			for (float &rr : pr->rad) {
				rr = Math::abs(rr);
			}
		}
		for (int i = 0; i < n; i++) {
			pr->col[i] = cnt[i] > 0 ? pr->col[i] / float(cnt[i]) : Color(1, 1, 1, 1);
		}
		// Colourless cells take the row's mean colour.
		for (int li = 0; li < pr->nl; li++) {
			Color sum(0, 0, 0, 0);
			int m = 0;
			for (int ai = 0; ai < pr->na; ai++) {
				if (cnt[li * pr->na + ai] > 0) {
					sum += pr->col[li * pr->na + ai];
					m++;
				}
			}
			if (m > 0) {
				for (int ai = 0; ai < pr->na; ai++) {
					if (cnt[li * pr->na + ai] == 0) {
						pr->col[li * pr->na + ai] = sum / float(m);
					}
				}
			}
		}
		if (!has_col) {
			for (Color &cc : pr->col) {
				cc = Color(1, 1, 1, 1);
			}
		}
	}
	pr->verts = PackedVector3Array();
	pr->colors = PackedColorArray();
	pr->built = true;
}

void BarkDetail3D::_ensure_family(TreeGenerator::BarkKind p_bark, const Ref<Material> &p_bark_material) {
	Family &f = families[p_bark];
	const BarkSpec &bs = BARK_SPECS[p_bark];
	if (f.material.is_null()) {
		f.material.instantiate();
		f.material->set_shader(_shader());
		f.material->set_shader_parameter("fallback_color", bs.fallback);
		f.material->set_shader_parameter("inner_color", bs.inner);
		f.material->set_shader_parameter("bark_tile", bs.tile);
		f.material->set_shader_parameter("lift_tint", bs.kind == PIECE_CURL ? 0.15f : 0.0f);
		for (int s = 0; s < SHAPES; s++) {
			f.meshes[s] = _make_shape(p_bark, s);
			const AABB ab = f.meshes[s]->get_aabb();
			f.shape_size[s] = Vector2(MAX(ab.size.x, 0.001f), MAX(ab.size.y, 0.001f));
		}
	}
	if (!f.textured) {
		// The stem's own bark texture, from the first trunk of this family that has one.
		const Ref<ShaderMaterial> sm = p_bark_material;
		const Ref<BaseMaterial3D> bm = p_bark_material;
		Ref<Texture2D> albedo, normal;
		Color tint(1, 1, 1);
		if (sm.is_valid()) {
			albedo = sm->get_shader_parameter("texture_albedo");
			normal = sm->get_shader_parameter("texture_normal");
			const Variant c = sm->get_shader_parameter("albedo");
			if (c.get_type() == Variant::COLOR) {
				tint = c;
			}
		} else if (bm.is_valid()) {
			albedo = bm->get_texture(BaseMaterial3D::TEXTURE_ALBEDO);
			normal = bm->get_texture(BaseMaterial3D::TEXTURE_NORMAL);
			tint = bm->get_albedo();
		}
		if (albedo.is_valid()) {
			f.material->set_shader_parameter("bark_albedo", albedo);
			f.material->set_shader_parameter("has_albedo", true);
			f.material->set_shader_parameter("bark_tint", tint);
			if (normal.is_valid()) {
				f.material->set_shader_parameter("bark_normal", normal);
				f.material->set_shader_parameter("has_normal", true);
			}
			f.textured = true;
		}
	}
	if (!f.instances[0] && is_inside_tree()) {
		for (int s = 0; s < SHAPES; s++) {
			f.multimeshes[s].instantiate();
			f.multimeshes[s]->set_transform_format(MultiMesh::TRANSFORM_3D);
			f.multimeshes[s]->set_use_custom_data(true);
			f.multimeshes[s]->set_mesh(f.meshes[s]);
			MultiMeshInstance3D *mmi = memnew(MultiMeshInstance3D);
			mmi->set_multimesh(f.multimeshes[s]);
			mmi->set_material_override(f.material);
			add_child(mmi, false, INTERNAL_MODE_BACK);
			mmi->set_as_top_level(true);
			mmi->set_global_transform(Transform3D());
			f.instances[s] = mmi;
		}
	}
}

/* Placement */

// A trunk's pieces, the same for the same tree wherever it is drawn from: up its capsules from 0.3 m (above the
// root flare) to `height`, at the family's rate a square metre, each where the stem's relief (TreeGenerator.
// bark_height, LOD 0's own height image) has a plate top for the plate families, lying on it, its outer face out.
void BarkDetail3D::_place(Trunk &r_trunk, const Dictionary &p_trunk, const Profile *p_profile) const {
	const TreeGenerator::BarkKind bark = TreeGenerator::BarkKind(r_trunk.bark);
	const BarkSpec &bs = BARK_SPECS[bark];
	if (bs.per_m2 <= 0.0f) {
		return;
	}
	const Array segs = p_trunk.get("segments", Array());
	const int seed = p_trunk.get("seed", 0);
	RandomPCG rng(uint64_t(uint32_t(seed)) * 6364136223846793005ull + uint64_t(bark));
	const bool plates = bs.kind == PIECE_PLATE || bs.kind == PIECE_SCALE;
	if (p_profile && p_profile->nl > 0 && !p_profile->a.is_empty()) {
		// On the mesh's own bark surface (mesh space, then the instance's transform), taking its vertex colour.
		const Profile &pr = *p_profile;
		const Family &fam = families[bark];
		const Basis ib = p_trunk.get("basis", Basis());
		const Transform3D to_world(ib, Vector3(p_trunk.get("base", Vector3())) - ib.xform(pr.a0));
		const int na = pr.na;
		const bool plates_kind = bs.kind == PIECE_PLATE || bs.kind == PIECE_SCALE;
		const int l0 = int((pr.skip > 0.0f ? pr.skip : 0.3f) / pr.step); // above the root flare, or clear of the crotch
		const int l1 = MIN(pr.nl, int(MIN(pr.total, float(height) - pr.base_h) / pr.step));
		if (l1 - l0 < 4) {
			return;
		}
		LocalVector<int> seg_of;
		seg_of.resize(pr.nl);
		for (int li = 0, sgi = 0; li < pr.nl; li++) {
			while (sgi + 1 < int(pr.a.size()) && pr.along0[sgi + 1] <= li * pr.step) {
				sgi++;
			}
			seg_of[li] = sgi;
		}
		auto wrap = [&](int c) { return ((c % na) + na) % na; };
		auto at = [&](int li, int ai) { return li * na + wrap(ai); };
		auto cell_w = [&](int li) { return float(Math::TAU) * pr.r[seg_of[li]] / na; };
		// Cells taken, so pieces never pile on each other.
		LocalVector<uint8_t> occ;
		occ.resize(pr.nl * na);
		memset(occ.ptr(), 0, occ.size());
		auto claim = [&](int lc, int ac, int hr, int hc) {
			if (lc - hr < l0 || lc + hr >= l1) {
				return false;
			}
			for (int li = lc - hr; li <= lc + hr; li++) {
				for (int ai = ac - hc; ai <= ac + hc; ai++) {
					if (occ[at(li, ai)]) {
						return false;
					}
				}
			}
			for (int li = lc - hr; li <= lc + hr; li++) {
				for (int ai = ac - hc; ai <= ac + hc; ai++) {
					occ[at(li, ai)] = 1;
				}
			}
			return true;
		};
		// One piece: row (fractional), angle, radius, scale round / along, spin, shape.
		auto emit = [&](float p_row, float p_th, float p_rad, float p_sx, float p_sy, float p_spin, int p_shape, const Color &p_col, float p_sz = -1.0f) {
			const int li = CLAMP(int(p_row), 0, pr.nl - 1);
			const int sgi = seg_of[li];
			if (p_rad < 0.5f * pr.r[sgi]) {
				return; // a cell the scan could not fill
			}
			const float t = p_row * pr.step - pr.along0[sgi];
			const Vector3 u = pr.u[sgi];
			const Vector3 out = pr.e1[sgi] * Math::cos(p_th) + pr.e2[sgi] * Math::sin(p_th);
			const Vector3 round = u.cross(out);
			Basis basis(round, u, out); // columns: x round the stem, y up it, z out
			basis = basis * Basis(Vector3(0, 0, 1), p_spin);
			basis.scale_local(Vector3(p_sx, p_sy, p_sz > 0.0f ? p_sz : (plates_kind ? 1.0f : 0.5f * (p_sx + p_sy)))); // plates keep their thickness
			const Vector3 pos = pr.a[sgi] + u * t + out * p_rad;
			// Never on another stem's side of a fork: a piece nearer another stem (in its radii) belongs to none.
			const float mine = p_rad / MAX(pr.r[sgi], 0.01f);
			for (const Profile *o = pr.first; o; o = o->next) {
				if (o == &pr) {
					continue;
				}
				for (uint32_t q = 0; q < o->a.size(); q++) {
					const float tq = (pos - o->a[q]).dot(o->u[q]);
					if (tq >= 0.0f && tq <= o->len[q] && (pos - o->a[q] - o->u[q] * tq).length() / MAX(o->r[q], 0.01f) < mine + 0.1f) {
						return;
					}
				}
			}
			Piece pc;
			pc.xform = to_world * Transform3D(basis, pos);
			pc.custom = Color(p_col.r, p_col.g, p_col.b, rng.randf());
			pc.shape = p_shape;
			r_trunk.pieces.push_back(pc);
		};
		auto th_of = [&](float p_col) { return (p_col + 0.5f) / na * float(Math::TAU); };

		if (bs.kind == PIECE_PLATE || bs.kind == PIECE_SCALE) {
			// The stem's own plates: cells above the bark round them, eroded once so furrows split them, then joined.
			// A share of them shed their outer layer: a piece fitted inside the plate's bounds, lifting at its lower
			// edge, so it never lies across a furrow.
			const int n = pr.nl * na;
			LocalVector<uint8_t> topc, kept;
			topc.resize(n);
			kept.resize(n);
			for (int li = 0; li < pr.nl; li++) {
				for (int ai = 0; ai < na; ai++) {
					float sum = 0.0f;
					int m = 0;
					for (int dl = -3; dl <= 3; dl++) {
						const int l2 = li + dl;
						if (l2 < 0 || l2 >= pr.nl) {
							continue;
						}
						for (int da = -4; da <= 4; da++) {
							sum += pr.rad[at(l2, ai + da)];
							m++;
						}
					}
					topc[li * na + ai] = pr.rad[li * na + ai] > sum / m + 0.0004f ? 1 : 0;
				}
			}
			for (int li = 0; li < pr.nl; li++) {
				for (int ai = 0; ai < na; ai++) {
					const bool up = li + 1 >= pr.nl || topc[at(li + 1, ai)], down = li == 0 || topc[at(li - 1, ai)];
					kept[li * na + ai] = topc[li * na + ai] && up && down && topc[at(li, ai - 1)] && topc[at(li, ai + 1)];
				}
			}
			LocalVector<int> label;
			label.resize(n);
			for (int i = 0; i < n; i++) {
				label[i] = -1;
			}
			LocalVector<Vector2i> stack, cells;
			const float share = CLAMP(bs.per_m2 / 35.0f * 0.65f * float(density), 0.0f, 0.75f);
			for (int ls = l0; ls < l1; ls++) {
				for (int as = 0; as < na; as++) {
					if (!kept[ls * na + as] || label[ls * na + as] >= 0) {
						continue;
					}
					// Flood the plate, keeping its columns unwrapped from the seed.
					cells.clear();
					stack.clear();
					stack.push_back(Vector2i(ls, as));
					label[ls * na + as] = 1;
					while (!stack.is_empty()) {
						const Vector2i c = stack[stack.size() - 1];
						stack.remove_at(stack.size() - 1);
						cells.push_back(c);
						const Vector2i nb[4] = { Vector2i(c.x + 1, c.y), Vector2i(c.x - 1, c.y), Vector2i(c.x, c.y + 1), Vector2i(c.x, c.y - 1) };
						for (const Vector2i &q : nb) {
							if (q.x < 0 || q.x >= pr.nl || cells.size() + stack.size() > 4000) {
								continue;
							}
							const int qi = at(q.x, q.y);
							if (kept[qi] && label[qi] < 0) {
								label[qi] = 1;
								stack.push_back(q);
							}
						}
					}
					if (cells.size() < 4 || rng.randf() >= share) {
						continue;
					}
					int rmin = 1 << 30, rmax = -(1 << 30), cmin = 1 << 30, cmax = -(1 << 30);
					float rsum = 0.0f, rtop = 0.0f;
					Color col(0, 0, 0, 0);
					for (const Vector2i &c : cells) {
						rmin = MIN(rmin, c.x);
						rmax = MAX(rmax, c.x);
						cmin = MIN(cmin, c.y);
						cmax = MAX(cmax, c.y);
						const int ci = at(c.x, c.y);
						rsum += pr.rad[ci];
						rtop = MAX(rtop, pr.rad[ci]);
						col += pr.col[ci];
					}
					if (rmax - rmin < 1 || rmin < l0 || rmax >= l1 || cmax - cmin >= na / 3) {
						continue;
					}
					const float lc = 0.5f * (rmin + rmax);
					// The eroded ring back on (one cell each side), then inside the plate's furrows.
					const float ext_along = (rmax - rmin + 3) * pr.step * 0.78f;
					const float ext_round = (cmax - cmin + 3) * cell_w(int(lc)) * 0.78f;
					const int shape = int(rng.rand() % SHAPES);
					const float sx = CLAMP(MIN(ext_round, 2.0f * bs.width) / fam.shape_size[shape].x, 0.3f, 3.0f);
					const float sy = CLAMP(MIN(ext_along, 2.0f * bs.length) / fam.shape_size[shape].y, 0.3f, 3.0f);
					const float rad = 0.5f * (rsum / cells.size() + rtop);
					emit(lc + 0.5f, th_of(0.5f * (cmin + cmax)), rad, sx, sy, (rng.randf() - 0.5f) * 0.12f, shape, col / float(cells.size()));
				}
			}
			return;
		}
		if (bs.kind == PIECE_CURL) {
			// Paper bark tears along the lenticel bands: bands up the stem, each with a run of strips side by side
			// round part of it (wide sheets, long strips, short curls), sizes log-normal.
			float row = l0 + 2.0f + rng.randf() * 4.0f;
			while (row < l1 - 2) {
				const int lc = int(row);
				const float cw = cell_w(lc);
				int in_band = 2 + int(rng.randf() * 5.0f);
				float col = rng.randf() * na;
				if (rng.randf() < 0.75f) {
					// The bared patch the band's strips came off, under them (it takes no cells: they lie on it).
					const int ps = 6 + int(rng.rand() % 2);
					// At most a quarter of the way round, 25 cm: a patch longer than that would not follow the stem.
					const float arc = MIN(0.25f, 0.25f * float(Math::TAU) * pr.r[seg_of[lc]]);
					const float size = MIN(CLAMP(Math::exp(rng.randfn(0.2f, 0.3f)), 0.7f, 2.2f), arc / fam.shape_size[6].x);
					const float pc = col + 0.5f * fam.shape_size[ps].x * size / cw * (0.4f + 0.4f * rng.randf());
					const float prad = pr.rad[at(lc, int(pc))];
					// Bent to this stem: the shape's 12 cm curvature, rescaled round by `size`, made the stem's radius.
					emit(lc + 0.5f, th_of(pc), prad, size, CLAMP(size * 0.8f, 0.6f, 1.6f), (rng.randf() - 0.5f) * 0.08f, ps, pr.col[at(lc, int(pc))], CLAMP(size * size * 0.12f / MAX(prad, 0.03f), 0.3f, 3.0f));
				}
				for (int k = 0; k < in_band; k++) {
					const int shape = int(rng.rand() % 6);
					const float arc = MIN(0.3f, 0.3f * float(Math::TAU) * pr.r[seg_of[lc]]);
					const float size = MIN(CLAMP(Math::exp(rng.randfn(0.0f, 0.3f)), 0.6f, 1.8f), arc / fam.shape_size[shape].x);
					const Vector2 ext = fam.shape_size[shape] * size;
					const int hc = MAX(1, int(Math::ceil(0.5f * ext.x / cw)));
					const int hr = MAX(0, int(Math::ceil(0.5f * ext.y / pr.step)));
					const int ac = int(col) + hc;
					const int lr = lc + int(Math::round(rng.randfn(0.0f, 0.6f)));
					if (claim(lr, ac, hr, hc)) {
						// Depth scaled so the attached middle takes this stem's curvature (the shape is bent to 12 cm),
						// within limits that keep the scroll round.
						const float srad = pr.rad[at(lr, ac)];
						const float sz = CLAMP(size * size * 0.12f / MAX(srad, 0.03f), 0.6f * size, 1.6f * size);
						emit(lr + 0.5f, th_of(float(ac)), srad - 0.0005f, size, size, (rng.randf() - 0.5f) * 0.12f, shape, pr.col[at(lr, ac)], sz);
					}
					col = float(ac + hc) + rng.randf() * 3.0f;
				}
				// Bands every 2 to 8 cm, closer with density.
				row += (0.02f + 0.06f * rng.randf()) / pr.step / MAX(float(density), 0.1f);
			}
			return;
		}
		// Flakes: scattered, none on another.
		for (int li = l0; li < l1; li++) {
			const float area = float(Math::TAU) * pr.r[seg_of[li]] * pr.step;
			const float want = area * bs.per_m2 * float(density);
			const int count = int(want) + (rng.randf() < want - int(want) ? 1 : 0);
			for (int k = 0; k < count; k++) {
				const int shape = int(rng.rand() % SHAPES);
				const float size = CLAMP(Math::exp(rng.randfn(0.0f, 0.25f)), 0.6f, 1.6f);
				const Vector2 ext = fam.shape_size[shape] * size;
				const int ac = int(rng.randf() * na);
				const int hc = MAX(0, int(0.5f * ext.x / cell_w(li)));
				const int hr = MAX(0, int(0.5f * ext.y / pr.step));
				if (claim(li, ac, hr, hc)) {
					emit(li + rng.randf(), th_of(float(ac)), pr.rad[at(li, ac)], size, size, (rng.randf() - 0.5f) * 0.3f, shape, pr.col[at(li, ac)]);
				}
			}
		}
		return;
	}
	// The relief's depth on this trunk, for what counts as a plate top.
	float hmin = 0.0f;
	if (plates && segs.size() > 0) {
		const Array s0 = segs[0];
		const float r0 = float(s0[2]);
		for (int k = 0; k < 64; k++) {
			hmin = MIN(hmin, TreeGenerator::bark_height(bark, float(Math::TAU) * k / 64.0f, 0.5f + 0.37f * k, r0));
		}
	}
	float along0 = 0.0f;
	const Vector3 base = p_trunk.get("base", Vector3());
	for (int si = 0; si < segs.size(); si++) {
		const Array s = segs[si];
		const Vector3 a = s[0], b = s[1];
		const float r = s[2];
		const Vector3 d = b - a;
		const float len = d.length();
		if (len < 1e-3f || r < 0.02f) {
			along0 += len;
			continue;
		}
		const Vector3 u = d / len;
		const Vector3 ref = Math::abs(u.x) < 0.9f ? Vector3(1, 0, 0) : Vector3(0, 0, 1);
		const Vector3 e1 = (ref - u * u.dot(ref)).normalized();
		const Vector3 e2 = u.cross(e1);
		const float lo = MAX(0.0f, 0.3f - along0), hi = MIN(len, float(height) - along0);
		if (hi <= lo) {
			along0 += len;
			if (along0 >= height) {
				break;
			}
			continue;
		}
		const float area = float(Math::TAU) * r * (hi - lo);
		const float want = area * bs.per_m2 * float(density) * (plates ? 1.8f : 1.0f); // plate families try more: only tops take one
		const int count = int(want) + (rng.randf() < want - int(want) ? 1 : 0);
		for (int k = 0; k < count; k++) {
			const float t = lo + (hi - lo) * rng.randf();
			const float th = float(Math::TAU) * rng.randf();
			const float along = along0 + t;
			const float h = TreeGenerator::bark_height(bark, th, along, r);
			if (plates && h < 0.4f * hmin) {
				continue; // in a furrow
			}
			const Vector3 out = e1 * Math::cos(th) + e2 * Math::sin(th);
			const Vector3 round = u.cross(out);
			const float size = CLAMP(Math::exp(rng.randfn(0.0f, 0.25f)), 0.6f, 1.6f);
			const float spin = (rng.randf() - 0.5f) * (bs.kind == PIECE_SCALE ? 0.7f : 0.3f);
			Basis basis(round, u, out); // columns: x round the stem, y up it, z out
			basis = basis * Basis(Vector3(0, 0, 1), spin);
			basis.scale_local(Vector3(size, size, size));
			// On the plate tops for plates (0); the others follow the relief.
			const Vector3 p = a + u * t + out * (r + (plates ? 0.0f : h));
			Piece pc;
			pc.xform = Transform3D(basis, p);
			pc.custom = Color(1, 1, 1, rng.randf());
			pc.shape = int(rng.rand() % SHAPES);
			r_trunk.pieces.push_back(pc);
		}
		along0 += len;
		if (along0 >= height) {
			break;
		}
	}
	(void)base;
}

/* Streaming */

static uint64_t bd_trunk_key(const Dictionary &p_trunk) {
	return (uint64_t(uint32_t(int(p_trunk.get("seed", 0)))) << 16) ^ uint64_t(int(p_trunk.get("preset", -1)));
}

void BarkDetail3D::_place_job(void *p_job) {
	PlaceJob *j = static_cast<PlaceJob *>(p_job);
	j->out.resize(j->dicts.size());
	for (uint32_t i = 0; i < j->dicts.size(); i++) {
		const Dictionary &d = j->dicts[i];
		Trunk &t = j->out[i];
		t.key = bd_trunk_key(d);
		t.bark = int(TreeGenerator::bark_kind_for_preset(int(d.get("preset", -1))));
		t.base = d.get("base", Vector3());
		if (!j->profs[i]) {
			j->field->_place(t, d, nullptr);
		}
		for (Profile *st = j->profs[i]; st; st = st->next) {
			_build_profile(st);
			j->field->_place(t, d, st);
		}
	}
}

// Trunks a worker has placed: those still near go in (seen this update, so they are not dropped at once).
void BarkDetail3D::_finish_job(bool p_wait) {
	if (!job || (!p_wait && !WorkerThreadPool::get_singleton()->is_task_completed(job->task))) {
		return;
	}
	WorkerThreadPool::get_singleton()->wait_for_task_completion(job->task);
	for (Trunk &t : job->out) {
		if (!trunks.has(t.key)) {
			t.seen = updates;
			trunks.insert(t.key, t);
			dirty = true;
		}
	}
	memdelete(job);
	job = nullptr;
}

real_t BarkDetail3D::_pixel_angle() const {
	const Viewport *vp = is_inside_tree() ? get_viewport() : nullptr;
	const Camera3D *cam = vp ? vp->get_camera_3d() : nullptr;
	const real_t fov = cam ? cam->get_fov() : 75.0;
	const real_t h = reference_height > 0 ? real_t(reference_height) : (vp ? MAX(real_t(vp->get_visible_rect().size.y), real_t(1.0)) : 1080.0);
	return 2.0 * Math::tan(Math::deg_to_rad(fov) * 0.5) / h;
}

real_t BarkDetail3D::get_reach(int p_bark) const {
	const real_t s = get_piece_size(p_bark);
	return s > 0.0 ? MIN(s / (detail_pixels * _pixel_angle()), max_reach) : 0.0;
}

Vector3 BarkDetail3D::_center() const {
	if (const Node3D *t = Object::cast_to<Node3D>(get_node_or_null(target_path))) {
		return t->get_global_position();
	}
	const Viewport *vp = is_inside_tree() ? get_viewport() : nullptr;
	const Camera3D *cam = vp ? vp->get_camera_3d() : nullptr;
	return cam ? cam->get_global_position() : get_global_position();
}

void BarkDetail3D::update_now() {
	if (!enabled || !is_inside_tree()) {
		return;
	}
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	updates++;
	const Vector3 center = _center();
	last_center = center;
	real_t reach = 0.0;
	for (int k = 0; k < TreeGenerator::BARK_MAX; k++) {
		reach = MAX(reach, get_reach(k));
	}
	_finish_job(false);
	LocalVector<Dictionary> fresh;
	LocalVector<Profile *> fresh_profs;
	HashSet<uint64_t> wanted_later;
	Array found;
	if (manual) {
		found = manual_trunks;
	} else if (VegetationField3D *veg = Object::cast_to<VegetationField3D>(get_node_or_null(vegetation_path))) {
		found = veg->get_trunks_near(center, float(reach) + 1.0f, float(height));
	}
	for (int i = 0; i < found.size(); i++) {
		const Dictionary d = found[i];
		const Vector3 base = d.get("base", Vector3());
		const int preset = d.get("preset", -1);
		if (preset < 0) {
			continue;
		}
		const uint64_t key = bd_trunk_key(d);
		Trunk *tr = trunks.getptr(key);
		if (tr) {
			tr->seen = updates;
			continue;
		}
		const int bk = int(TreeGenerator::bark_kind_for_preset(preset));
		if (BARK_SPECS[bk].per_m2 <= 0.0f) {
			tr = &trunks[key]; // nothing to place: kept so it is not asked again
			tr->key = key;
			tr->bark = bk;
			tr->base = base;
			tr->seen = updates;
			continue;
		}
		_ensure_family(TreeGenerator::BarkKind(bk), _bark_material_of(d));
		Profile *prof = _profile(_trunk_mesh_of(d));
		if (sync) {
			tr = &trunks[key];
			tr->key = key;
			tr->bark = bk;
			tr->base = base;
			if (!prof) {
				_place(*tr, d, nullptr);
			}
			for (Profile *st = prof; st; st = st->next) {
				_build_profile(st);
				_place(*tr, d, st);
			}
			tr->seen = updates;
			dirty = true;
		} else if (!job) {
			fresh.push_back(d);
			fresh_profs.push_back(prof);
		} else {
			wanted_later.insert(key);
		}
	}
	if (!fresh.is_empty()) {
		job = memnew(PlaceJob);
		job->field = this;
		job->dicts = fresh;
		job->profs = fresh_profs;
		job->task = WorkerThreadPool::get_singleton()->add_native_task(&BarkDetail3D::_place_job, job, false, "BarkDetail3D pieces");
	}
	(void)wanted_later;
	LocalVector<uint64_t> gone;
	for (const KeyValue<uint64_t, Trunk> &kv : trunks) {
		if (kv.value.seen != updates) {
			gone.push_back(kv.key);
		}
	}
	for (uint64_t k : gone) {
		trunks.erase(k);
		dirty = true;
	}
	_draw();
	const double ms = double(OS::get_singleton()->get_ticks_usec() - t0) / 1000.0;
	update_msec_sum += ms;
	update_msec_worst = MAX(update_msec_worst, ms);
	update_runs++;
}

// Each family's pieces on the trunks within its reach, one buffer upload per shape when that set changed.
void BarkDetail3D::_draw() {
	static thread_local LocalVector<float> buf;
	uint64_t sig[TreeGenerator::BARK_MAX] = {};
	const Vector3 c = last_center;
	for (const KeyValue<uint64_t, Trunk> &kv : trunks) {
		const Trunk &t = kv.value;
		if (BARK_SPECS[t.bark].per_m2 > 0.0f && Vector2(t.base.x - c.x, t.base.z - c.z).length() <= get_reach(t.bark) + 0.5) {
			sig[t.bark] = sig[t.bark] * 31u + (t.key | 1u) * 2654435761u;
		}
	}
	piece_count = 0;
	for (int k = 0; k < TreeGenerator::BARK_MAX; k++) {
		Family &f = families[k];
		if (!f.instances[0]) {
			continue;
		}
		const bool changed = dirty || sig[k] != last_sig[k];
		last_sig[k] = sig[k];
		const real_t reach = get_reach(k) + 0.5;
		for (int s = 0; s < SHAPES; s++) {
			if (!changed) {
				piece_count += f.multimeshes[s]->get_instance_count();
				continue;
			}
			buf.clear();
			for (const KeyValue<uint64_t, Trunk> &kv : trunks) {
				const Trunk &t = kv.value;
				if (t.bark != k || Vector2(t.base.x - c.x, t.base.z - c.z).length() > reach) {
					continue;
				}
				for (const Piece &p : t.pieces) {
					if (p.shape != s) {
						continue;
					}
					const Transform3D &x = p.xform;
					for (int row = 0; row < 3; row++) {
						buf.push_back(x.basis.rows[row][0]);
						buf.push_back(x.basis.rows[row][1]);
						buf.push_back(x.basis.rows[row][2]);
						buf.push_back(x.origin[row]);
					}
					buf.push_back(p.custom.r);
					buf.push_back(p.custom.g);
					buf.push_back(p.custom.b);
					buf.push_back(p.custom.a);
				}
			}
			const int n = int(buf.size() / 16);
			Ref<MultiMesh> &mm = f.multimeshes[s];
			mm->set_instance_count(n);
			if (n > 0) {
				Vector<float> v;
				v.resize(buf.size());
				memcpy(v.ptrw(), buf.ptr(), buf.size() * sizeof(float));
				RS::get_singleton()->multimesh_set_buffer(mm->get_rid(), v);
				// One TLAS entry a piece over the shared shape, not a merged copy of every piece.
				RS::get_singleton()->multimesh_set_rt_instanced(mm->get_rid(), true);
			}
			f.instances[s]->set_visible(n > 0);
			piece_count += n;
		}
	}
	dirty = false;
}

void BarkDetail3D::_clear() {
	_finish_job(true);
	trunks.clear();
	for (Family &f : families) {
		for (int s = 0; s < SHAPES; s++) {
			if (f.instances[s]) {
				f.instances[s]->queue_free();
				f.instances[s] = nullptr;
			}
			f.multimeshes[s].unref();
		}
	}
	piece_count = 0;
	dirty = true;
}

void BarkDetail3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			set_process(enabled);
		} break;
		case NOTIFICATION_PROCESS: {
			since += get_process_delta_time();
			const Vector3 c = _center();
			if (since >= 0.25 || c.distance_to(last_center) > 0.5) {
				since = 0.0;
				update_now();
			}
		} break;
		case NOTIFICATION_EXIT_TREE: {
			_clear();
		} break;
	}
}

/* Properties */

void BarkDetail3D::set_enabled(bool p_enabled) {
	enabled = p_enabled;
	if (!enabled) {
		_clear();
	}
	if (is_inside_tree()) {
		set_process(enabled);
	}
}

void BarkDetail3D::set_vegetation(const NodePath &p_path) {
	vegetation_path = p_path;
	_clear();
}

void BarkDetail3D::set_target(const NodePath &p_path) {
	target_path = p_path;
}

void BarkDetail3D::set_height(real_t p_height) {
	height = CLAMP(p_height, real_t(0.5), real_t(PROFILE_CAP - 0.2f));
	_clear(); // the job is done: the profiles (built up to the old height) can go
	for (KeyValue<const Mesh *, Profile *> &kv : profiles) {
		_free_profile(kv.value);
	}
	profiles.clear();
}

void BarkDetail3D::set_density(real_t p_density) {
	density = MAX(p_density, real_t(0.0));
	_clear();
}

void BarkDetail3D::set_detail_pixels(real_t p_pixels) {
	detail_pixels = MAX(p_pixels, real_t(0.5));
	dirty = true;
}

void BarkDetail3D::set_reference_height(int p_height) {
	reference_height = MAX(p_height, 0);
	dirty = true;
}

void BarkDetail3D::set_max_reach(real_t p_reach) {
	max_reach = MAX(p_reach, real_t(1.0));
	dirty = true;
}

void BarkDetail3D::set_trunks(const Array &p_trunks) {
	manual = true;
	manual_trunks = p_trunks;
	_clear();
}

void BarkDetail3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_enabled", "enabled"), &BarkDetail3D::set_enabled);
	ClassDB::bind_method(D_METHOD("is_enabled"), &BarkDetail3D::is_enabled);
	ClassDB::bind_method(D_METHOD("set_vegetation", "path"), &BarkDetail3D::set_vegetation);
	ClassDB::bind_method(D_METHOD("get_vegetation"), &BarkDetail3D::get_vegetation);
	ClassDB::bind_method(D_METHOD("set_target", "path"), &BarkDetail3D::set_target);
	ClassDB::bind_method(D_METHOD("get_target"), &BarkDetail3D::get_target);
	ClassDB::bind_method(D_METHOD("set_height", "height"), &BarkDetail3D::set_height);
	ClassDB::bind_method(D_METHOD("get_height"), &BarkDetail3D::get_height);
	ClassDB::bind_method(D_METHOD("set_density", "density"), &BarkDetail3D::set_density);
	ClassDB::bind_method(D_METHOD("get_density"), &BarkDetail3D::get_density);
	ClassDB::bind_method(D_METHOD("set_detail_pixels", "pixels"), &BarkDetail3D::set_detail_pixels);
	ClassDB::bind_method(D_METHOD("get_detail_pixels"), &BarkDetail3D::get_detail_pixels);
	ClassDB::bind_method(D_METHOD("set_reference_height", "height"), &BarkDetail3D::set_reference_height);
	ClassDB::bind_method(D_METHOD("get_reference_height"), &BarkDetail3D::get_reference_height);
	ClassDB::bind_method(D_METHOD("set_max_reach", "reach"), &BarkDetail3D::set_max_reach);
	ClassDB::bind_method(D_METHOD("get_max_reach"), &BarkDetail3D::get_max_reach);
	ClassDB::bind_method(D_METHOD("set_trunks", "trunks"), &BarkDetail3D::set_trunks);
	ClassDB::bind_method(D_METHOD("update_now"), &BarkDetail3D::update_now);
	ClassDB::bind_static_method("BarkDetail3D", D_METHOD("make_bark_profile", "vertices", "colors", "colliders"), &BarkDetail3D::make_bark_profile);
	ClassDB::bind_method(D_METHOD("set_sync", "sync"), &BarkDetail3D::set_sync);
	ClassDB::bind_method(D_METHOD("is_sync"), &BarkDetail3D::is_sync);
	ClassDB::bind_method(D_METHOD("is_placing"), &BarkDetail3D::is_placing);
	ClassDB::bind_method(D_METHOD("get_piece_count"), &BarkDetail3D::get_piece_count);
	ClassDB::bind_method(D_METHOD("get_trunk_count"), &BarkDetail3D::get_trunk_count);
	ClassDB::bind_method(D_METHOD("get_update_msec_mean"), &BarkDetail3D::get_update_msec_mean);
	ClassDB::bind_method(D_METHOD("get_update_msec_worst"), &BarkDetail3D::get_update_msec_worst);
	ClassDB::bind_method(D_METHOD("get_reach", "bark"), &BarkDetail3D::get_reach);
	ClassDB::bind_static_method("BarkDetail3D", D_METHOD("get_piece_size", "bark"), &BarkDetail3D::get_piece_size);
	ClassDB::bind_static_method("BarkDetail3D", D_METHOD("get_bark_kind", "preset"), &BarkDetail3D::get_bark_kind);

	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enabled"), "set_enabled", "is_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "vegetation", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "VegetationField3D"), "set_vegetation", "get_vegetation");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "target", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D"), "set_target", "get_target");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "height", PROPERTY_HINT_RANGE, "0.5,4,0.1,suffix:m"), "set_height", "get_height");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "density", PROPERTY_HINT_RANGE, "0,4,0.01"), "set_density", "get_density");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "detail_pixels", PROPERTY_HINT_RANGE, "0.5,16,0.1"), "set_detail_pixels", "get_detail_pixels");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "reference_height", PROPERTY_HINT_RANGE, "0,4320,1,suffix:px"), "set_reference_height", "get_reference_height");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "max_reach", PROPERTY_HINT_RANGE, "1,100,0.5,suffix:m"), "set_max_reach", "get_max_reach");

	BIND_ENUM_CONSTANT(PIECE_PLATE);
	BIND_ENUM_CONSTANT(PIECE_SCALE);
	BIND_ENUM_CONSTANT(PIECE_FLAKE);
	BIND_ENUM_CONSTANT(PIECE_CURL);
	BIND_ENUM_CONSTANT(PIECE_MAX);
}

BarkDetail3D::BarkDetail3D() {
}

BarkDetail3D::~BarkDetail3D() {
	_finish_job(true);
	for (KeyValue<const Mesh *, Profile *> &kv : profiles) {
		_free_profile(kv.value);
	}
}

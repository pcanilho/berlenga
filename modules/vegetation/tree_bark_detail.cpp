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
	{ BarkDetail3D::PIECE_PLATE, 0.09f, 0.055f, 0.008f, 35.0f, 0.22f, Color(0.66f, 0.32f, 0.17f), Color(0.36f, 0.29f, 0.24f), 0.4f },
	// Scaly (holm oak, blackthorn, carob): small squarish scales a few centimetres across.
	{ BarkDetail3D::PIECE_SCALE, 0.035f, 0.028f, 0.004f, 110.0f, 0.12f, Color(0.47f, 0.34f, 0.25f), Color(0.27f, 0.25f, 0.23f), 0.4f },
	// Flaking (plane-like, saltbush, calligonum): thin flakes curling out at one end, paler fresh bark under them.
	{ BarkDetail3D::PIECE_FLAKE, 0.08f, 0.05f, 0.0025f, 30.0f, 0.5f, Color(0.72f, 0.66f, 0.46f), Color(0.5f, 0.47f, 0.41f), 0.4f },
	// Papery (birches): horizontal strips of the white outer bark peeling round the stem and rolling outward.
	// The rolled underside is the paper's pale salmon-cream inner face, not the darker living bark.
	{ BarkDetail3D::PIECE_CURL, 0.13f, 0.042f, 0.0008f, 16.0f, 1.0f, Color(0.8f, 0.68f, 0.58f), Color(0.88f, 0.86f, 0.82f), 0.4f },
	// Lenticelled (hazel, rowan): thin skin peeling in short strips.
	{ BarkDetail3D::PIECE_CURL, 0.04f, 0.012f, 0.0005f, 6.0f, 0.8f, Color(0.58f, 0.46f, 0.35f), Color(0.5f, 0.45f, 0.4f), 0.4f },
	// Stone pine: long flat grey-brown plates over orange-red fissures.
	{ BarkDetail3D::PIECE_PLATE, 0.18f, 0.07f, 0.014f, 14.0f, 0.14f, Color(0.72f, 0.36f, 0.2f), Color(0.45f, 0.4f, 0.36f), 0.4f },
	// Cork: its ridges are the stem's own geometry.
	{ BarkDetail3D::PIECE_PLATE, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, Color(0.6f, 0.35f, 0.22f), Color(0.55f, 0.5f, 0.44f), 1.2f },
	// Maritime pine: thick dark plates a hand across over dull red-brown.
	{ BarkDetail3D::PIECE_PLATE, 0.15f, 0.09f, 0.02f, 14.0f, 0.18f, Color(0.5f, 0.25f, 0.15f), Color(0.26f, 0.21f, 0.18f), 0.9f },
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

	int add(const Vector3 &p_v, float p_outer) {
		verts.push_back(p_v);
		normals.push_back(Vector3());
		colors.push_back(Color(p_outer, 0, 0, 1));
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
	if (bs.kind == PIECE_CURL) {
		const int M = 18;
		const float L = bs.length * (0.8f + 0.4f * bd_noise(seed, 0));
		const float W = bs.width * (0.75f + 0.5f * bd_noise(seed, 1));
		const float T = bs.thick;
		const float flat = 0.35f + 0.25f * bd_noise(seed, 2); // share lying on the stem
		const float roll = L * (0.07f + 0.08f * bd_noise(seed, 3)); // the roll's radius
		const float side = bd_noise(seed, 4) < 0.5f ? -1.0f : 1.0f; // which end peels
		// The centreline: heading +x, turning toward +z (outward) once off the stem.
		LocalVector<Vector3> c, n;
		Vector2 p(-0.5f * flat * L, 0.0f);
		float heading = 0.0f;
		const float ds = L / M;
		for (int i = 0; i <= M; i++) {
			const float s = float(i) / M;
			c.push_back(Vector3(p.x * side, 0.0f, p.y));
			n.push_back(Vector3(-Math::sin(heading) * side, 0.0f, Math::cos(heading)));
			const float rr = roll * (1.0f - 0.35f * MAX(0.0f, s - flat) / MAX(1.0f - flat, 1e-3f)); // tighter toward its end
			if (s >= flat) {
				heading += ds / rr;
			}
			p += Vector2(Math::cos(heading), Math::sin(heading)) * ds;
		}
		// A slight sag across the strip, so it is not a ruled sheet.
		const float sag = 0.15f * W;
		int top[19][3], bot[19][3];
		for (int i = 0; i <= M; i++) {
			for (int j = 0; j < 3; j++) {
				// Torn paper: each long edge wanders, and the strip narrows toward its free end.
				const float tear = j == 1 ? 1.0f : 0.75f + 0.5f * bd_noise(seed, 40 + i * 3 + j);
				const float y = (float(j) - 1.0f) * 0.5f * W * tear * (1.0f - 0.35f * float(i) / M * bd_noise(seed, 10 + j));
				const float zs = (j == 1 ? 0.0f : 1.0f) * sag * MAX(0.0f, float(i) / M - flat);
				const Vector3 base = c[i] + Vector3(0, y, 0) + n[i] * zs;
				top[i][j] = sb.add(base + n[i] * (0.5f * T), 1.0f);
				bot[i][j] = sb.add(base - n[i] * (0.5f * T), 0.0f);
			}
		}
		for (int i = 0; i < M; i++) {
			for (int j = 0; j < 2; j++) {
				sb.quad(top[i][j], top[i + 1][j], top[i + 1][j + 1], top[i][j + 1], n[i]);
			}
		}
		sb.close_group();
		for (int i = 0; i < M; i++) {
			for (int j = 0; j < 2; j++) {
				sb.quad(bot[i][j], bot[i + 1][j], bot[i + 1][j + 1], bot[i][j + 1], -n[i]);
			}
		}
		sb.close_group();
		// Edges: the two long sides and the two ends.
		for (int e = 0; e < 2; e++) {
			const int j = e == 0 ? 0 : 2;
			const Vector3 out(0, e == 0 ? -1.0f : 1.0f, 0);
			for (int i = 0; i < M; i++) {
				const int a = sb.add(sb.verts[top[i][j]], 0.4f), b = sb.add(sb.verts[top[i + 1][j]], 0.4f);
				const int cc = sb.add(sb.verts[bot[i + 1][j]], 0.4f), d = sb.add(sb.verts[bot[i][j]], 0.4f);
				sb.quad(a, b, cc, d, out);
			}
			sb.close_group();
		}
		for (int e = 0; e < 2; e++) {
			const int i = e == 0 ? 0 : M;
			const Vector3 out = e == 0 ? Vector3(-side, 0, 0) : (c[M] - c[M - 1]).normalized();
			for (int j = 0; j < 2; j++) {
				const int a = sb.add(sb.verts[top[i][j]], 0.4f), b = sb.add(sb.verts[top[i][j + 1]], 0.4f);
				const int cc = sb.add(sb.verts[bot[i][j + 1]], 0.4f), d = sb.add(sb.verts[bot[i][j]], 0.4f);
				sb.quad(a, b, cc, d, out);
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
		return z - 0.3f * T; // its foot a little into the stem's relief
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
		et[i] = sb.add(sb.verts[top[1][i]], 0.4f);
		eb[i] = sb.add(sb.verts[bot[1][i]], 0.4f);
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
	// Fresh inner bark: smoother, mottled, darker where it has been open longest (toward the piece's edge).
	float n = bd_noise(v_local * 260.0 + v_custom.w * 37.0);
	vec3 inner = inner_color * (0.8 + 0.35 * n);
	ALBEDO = mix(inner, outer, o);
	ROUGHNESS = mix(0.68, 0.9, o);
	if (has_normal) {
		NORMAL_MAP = mix(vec3(0.5, 0.5, 1.0), texture(bark_normal, uv).rgb, o);
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
BarkDetail3D::Profile *BarkDetail3D::_profile_chain(const PackedFloat32Array &c, float p_cap) {
	if (c.size() < 8) {
		return nullptr;
	}
	Profile *pr = memnew(Profile);
	Vector3 prev(c[0], c[1], c[2]);
	pr->a0 = prev;
	float along = 0.0f;
	for (int k = 0; k + 7 < c.size(); k += 8) {
		const Vector3 a(c[k], c[k + 1], c[k + 2]), b(c[k + 3], c[k + 4], c[k + 5]);
		if (k > 0 && a.distance_to(prev) > 1e-3f) {
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

Array BarkDetail3D::make_bark_profile(const PackedVector3Array &p_vertices, const PackedColorArray &p_colors, const PackedFloat32Array &p_colliders) {
	Profile *pr = _profile_chain(p_colliders, PROFILE_CAP);
	if (!pr) {
		return Array();
	}
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
	memdelete(pr);
	return Array{ head, rad, col };
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
	if (packed.size() == 3) {
		// Binned when the tree was built (TreeGenerator, make_bark_profile): nothing to read back.
		const PackedFloat32Array head = packed[0], rad = packed[1];
		const PackedByteArray col = packed[2];
		if (head.size() >= 3) {
			pr = _profile_chain(c, head[1] * head[2] + 1e-4f);
		}
		if (pr) {
			pr->na = int(head[0]);
			pr->step = head[2];
			pr->nl = MIN(pr->nl, int(head[1]));
			const int n = pr->nl * pr->na;
			if (rad.size() >= n && col.size() >= n * 3) {
				pr->rad.resize(n);
				pr->col.resize(n);
				for (int i = 0; i < n; i++) {
					pr->rad[i] = rad[i];
					pr->col[i] = Color(col[i * 3] / 127.5f, col[i * 3 + 1] / 127.5f, col[i * 3 + 2] / 127.5f);
				}
				pr->built = true;
			} else {
				memdelete(pr);
				pr = nullptr;
			}
		}
	}
	const int si = bd_bark_surface(p_mesh);
	if (!pr && si >= 0) {
		// A mesh without one: its bark surface read back here (a stall on the render thread), binned on the worker.
		pr = _profile_chain(c, MIN(float(height) + 0.2f, PROFILE_CAP));
		if (pr) {
			const Array arrays = p_mesh->surface_get_arrays(si);
			pr->verts = arrays[Mesh::ARRAY_VERTEX];
			pr->colors = arrays[Mesh::ARRAY_COLOR];
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
		for (int s = 0; s < SHAPES; s++) {
			f.meshes[s] = _make_shape(p_bark, s);
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
		// On the mesh's own bark surface: each piece at the outermost bark vertex of its cell (mesh space, then the
		// instance's transform), taking that cell's vertex colour.
		const Profile &pr = *p_profile;
		const Basis ib = p_trunk.get("basis", Basis());
		const Transform3D to_world(ib, Vector3(p_trunk.get("base", Vector3())) - ib.xform(pr.a0));
		const float top = MIN(pr.total, float(height));
		uint32_t sgi = 0;
		for (int li = int(0.3f / pr.step); li < pr.nl; li++) {
			const float al_lo = li * pr.step;
			if (al_lo >= top) {
				break;
			}
			while (sgi + 1 < pr.a.size() && pr.along0[sgi + 1] <= al_lo) {
				sgi++;
			}
			const float r = pr.r[sgi];
			if (r < 0.02f) {
				continue;
			}
			const float area = float(Math::TAU) * r * pr.step;
			const float want = area * bs.per_m2 * float(density) * (plates ? 1.8f : 1.0f); // plate families try more: only tops take one
			const int count = int(want) + (rng.randf() < want - int(want) ? 1 : 0);
			for (int k = 0; k < count; k++) {
				const float along = al_lo + pr.step * rng.randf();
				const float th = float(Math::TAU) * rng.randf();
				const int ai = CLAMP(int(th / float(Math::TAU) * pr.na), 0, pr.na - 1);
				const int idx = li * pr.na + ai;
				const float rad = pr.rad[idx];
				if (plates) {
					// A plate top: near the highest of the bark round it.
					float mn = 1e9f, mx = 0.0f;
					for (int dl = -2; dl <= 2; dl++) {
						const int l2 = CLAMP(li + dl, 0, pr.nl - 1);
						for (int da = -3; da <= 3; da++) {
							const float rr = pr.rad[l2 * pr.na + (ai + da + pr.na) % pr.na];
							mn = MIN(mn, rr);
							mx = MAX(mx, rr);
						}
					}
					if (mx - mn > 0.002f && rad < mn + 0.65f * (mx - mn)) {
						continue; // in a furrow
					}
				}
				const float t = along - pr.along0[sgi];
				const Vector3 u = pr.u[sgi];
				const Vector3 out = pr.e1[sgi] * Math::cos(th) + pr.e2[sgi] * Math::sin(th);
				const Vector3 round = u.cross(out);
				const float size = CLAMP(Math::exp(rng.randfn(0.0f, 0.25f)), 0.6f, 1.6f);
				const float spin = (rng.randf() - 0.5f) * (bs.kind == PIECE_SCALE ? 0.7f : 0.3f);
				Basis basis(round, u, out); // columns: x round the stem, y up it, z out
				basis = basis * Basis(Vector3(0, 0, 1), spin);
				basis.scale_local(Vector3(size, size, size));
				const Vector3 p = pr.a[sgi] + u * t + out * rad;
				const Color c = pr.col[idx];
				Piece pc;
				pc.xform = to_world * Transform3D(basis, p);
				pc.custom = Color(c.r, c.g, c.b, rng.randf());
				pc.shape = int(rng.rand() % SHAPES);
				r_trunk.pieces.push_back(pc);
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
		_build_profile(j->profs[i]);
		j->field->_place(t, d, j->profs[i]);
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
			_build_profile(prof);
			_place(*tr, d, prof);
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
		if (kv.value) {
			memdelete(kv.value);
		}
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
		if (kv.value) {
			memdelete(kv.value);
		}
	}
}

/**************************************************************************/
/*  vegetation_voxel_lod.cpp                                                */
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

#include "vegetation_voxel_lod.h"

#include "core/math/random_pcg.h"
#include "core/templates/hash_map.h"
#include "scene/resources/material.h"
#include "scene/resources/texture.h"

namespace {

struct Voxel {
	float area = 0.0f;
	Vector3 albedo; // Area-weighted, linear.
	Vector3 normal; // Area-weighted, oriented outward from the source's centre line.
};

// The albedo a surface shows at a UV: material colour, vertex colour, texture (alpha tested).
struct SurfaceLook {
	Color base = Color(1, 1, 1, 1);
	bool vertex_color = false;
	bool vertex_srgb = false; // Vertex colours are linear unless the material says sRGB.
	Ref<Image> image;
	float scissor = -1.0f;
};

Ref<Image> look_image(const Ref<Texture2D> &p_tex) {
	if (p_tex.is_null()) {
		return Ref<Image>();
	}
	Ref<Image> img = p_tex->get_image();
	if (img.is_null()) {
		return img;
	}
	img = img->duplicate();
	if (img->is_compressed()) {
		img->decompress();
	}
	img->clear_mipmaps();
	img->convert(Image::FORMAT_RGBA8);
	return img;
}

SurfaceLook surface_look(const Ref<Mesh> &p_mesh, int p_surface) {
	SurfaceLook l;
	const Ref<Material> mat = p_mesh->surface_get_material(p_surface);
	const Ref<BaseMaterial3D> m = mat;
	if (m.is_valid()) {
		l.base = m->get_albedo().srgb_to_linear();
		l.vertex_color = m->get_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR);
		l.vertex_srgb = m->get_flag(BaseMaterial3D::FLAG_SRGB_VERTEX_COLOR);
		l.image = look_image(m->get_texture(BaseMaterial3D::TEXTURE_ALBEDO));
		if (m->get_transparency() == BaseMaterial3D::TRANSPARENCY_ALPHA_SCISSOR) {
			l.scissor = m->get_alpha_scissor_threshold();
		} else if (l.image.is_valid() && m->get_transparency() != BaseMaterial3D::TRANSPARENCY_DISABLED) {
			l.scissor = 0.5f;
		}
		return l;
	}
	// A ShaderMaterial made from BaseMaterial3D's generated code (leaf flutter and the like)
	// keeps its uniform names.
	const Ref<ShaderMaterial> sm = mat;
	if (sm.is_null() || sm->get_shader().is_null()) {
		return l;
	}
	const String code = sm->get_shader()->get_code();
	const Variant albedo = sm->get_shader_parameter("albedo");
	if (albedo.get_type() == Variant::COLOR) {
		l.base = Color(albedo).srgb_to_linear();
	}
	l.vertex_color = code.contains("albedo_tex *= COLOR");
	l.vertex_srgb = code.contains("Vertex Color is sRGB: Enabled");
	l.image = look_image(sm->get_shader_parameter("texture_albedo"));
	const Variant sc = sm->get_shader_parameter("alpha_scissor_threshold");
	if (sc.get_type() == Variant::FLOAT && code.contains("ALPHA_SCISSOR_THRESHOLD")) {
		l.scissor = float(sc);
	} else if (l.image.is_valid() && code.contains("ALPHA")) {
		l.scissor = 0.5f;
	}
	return l;
}

} // namespace

Ref<ArrayMesh> VegetationVoxelLod::make_shell(const Ref<Mesh> &p_mesh, float p_voxel, uint32_t p_seed) {
	ERR_FAIL_COND_V(p_mesh.is_null() || p_voxel <= 0.0f, Ref<ArrayMesh>());
	const AABB box = p_mesh->get_aabb().grow(p_voxel * 0.5f);
	const Vector3i dim(MAX(1, int(Math::ceil(box.size.x / p_voxel))), MAX(1, int(Math::ceil(box.size.y / p_voxel))), MAX(1, int(Math::ceil(box.size.z / p_voxel))));
	ERR_FAIL_COND_V_MSG(int64_t(dim.x) * dim.y * dim.z > 8000000, Ref<ArrayMesh>(), "VegetationVoxelLod: too many voxels.");
	LocalVector<Voxel> grid;
	grid.resize(dim.x * dim.y * dim.z);
	auto at = [&](int x, int y, int z) -> int { return (z * dim.y + y) * dim.x + x; };
	// The outward direction for two-sided leaves: away from the source's long axis through its
	// middle (a hedge's or a crown's core), and up.
	const Vector3 centre = p_mesh->get_aabb().get_center();
	RandomPCG rng(p_seed);
	const float step = p_voxel / 3.0f; // Several samples per voxel face.
	for (int s = 0; s < p_mesh->get_surface_count(); s++) {
		if (p_mesh->surface_get_primitive_type(s) != Mesh::PRIMITIVE_TRIANGLES) {
			continue;
		}
		const Array arr = p_mesh->surface_get_arrays(s);
		const PackedVector3Array verts = arr[Mesh::ARRAY_VERTEX];
		if (verts.is_empty()) {
			continue;
		}
		const PackedInt32Array idx = arr[Mesh::ARRAY_INDEX];
		const PackedColorArray cols = arr[Mesh::ARRAY_COLOR];
		const PackedVector2Array uvs = arr[Mesh::ARRAY_TEX_UV];
		const SurfaceLook look = surface_look(p_mesh, s);
		const bool use_col = look.vertex_color && cols.size() == verts.size();
		const bool use_tex = look.image.is_valid() && uvs.size() == verts.size();
		const int tris = idx.is_empty() ? verts.size() / 3 : idx.size() / 3;
		for (int t = 0; t < tris; t++) {
			const int i0 = idx.is_empty() ? t * 3 : idx[t * 3];
			const int i1 = idx.is_empty() ? t * 3 + 1 : idx[t * 3 + 1];
			const int i2 = idx.is_empty() ? t * 3 + 2 : idx[t * 3 + 2];
			const Vector3 a = verts[i0], b = verts[i1], c = verts[i2];
			const Vector3 cr = (b - a).cross(c - a);
			const float area = cr.length() * 0.5f;
			if (area < 1e-10f) {
				continue;
			}
			const Vector3 n = cr / (2.0f * area);
			const int samples = MAX(1, int(Math::ceil(area / (step * step))));
			const float share = area / samples;
			for (int k = 0; k < samples; k++) {
				float u = rng.randf(), v = rng.randf();
				if (u + v > 1.0f) {
					u = 1.0f - u;
					v = 1.0f - v;
				}
				const float w = 1.0f - u - v;
				const Vector3 p = a * w + b * u + c * v;
				Color col = look.base;
				if (use_col) {
					const Color vc = cols[i0] * w + cols[i1] * u + cols[i2] * v;
					col *= look.vertex_srgb ? vc.srgb_to_linear() : vc;
				}
				if (use_tex) {
					const Vector2 uv = uvs[i0] * w + uvs[i1] * u + uvs[i2] * v;
					const int px = CLAMP(int(Math::fposmod(uv.x, 1.0f) * look.image->get_width()), 0, look.image->get_width() - 1);
					const int py = CLAMP(int(Math::fposmod(uv.y, 1.0f) * look.image->get_height()), 0, look.image->get_height() - 1);
					const Color tc = look.image->get_pixel(px, py);
					if (look.scissor >= 0.0f && tc.a < look.scissor) {
						continue; // A cut-out part of the card: nothing there.
					}
					col *= tc.srgb_to_linear();
				}
				const Vector3 g = (p - box.position) / p_voxel;
				const int x = CLAMP(int(g.x), 0, dim.x - 1), y = CLAMP(int(g.y), 0, dim.y - 1), z = CLAMP(int(g.z), 0, dim.z - 1);
				Voxel &vx = grid[at(x, y, z)];
				Vector3 out = p - centre;
				out.y = MAX(out.y, 0.0f) + 0.3f * box.size.y;
				const Vector3 no = n.dot(out) < 0.0f ? -n : n;
				vx.area += share;
				vx.albedo += Vector3(col.r, col.g, col.b) * share;
				vx.normal += no * share;
			}
		}
	}
	// Covered: the leaf area in a voxel blocks about half its own area in projection (randomly
	// turned leaves), so the light let through is exp(-0.5 * area / face); covered past half.
	const float face = p_voxel * p_voxel;
	LocalVector<float> alpha;
	alpha.resize(grid.size());
	for (uint32_t i = 0; i < grid.size(); i++) {
		alpha[i] = 1.0f - Math::exp(-0.5f * grid[i].area / face);
	}
	auto solid = [&](int x, int y, int z) -> bool {
		if (x < 0 || y < 0 || z < 0 || x >= dim.x || y >= dim.y || z >= dim.z) {
			return false;
		}
		return alpha[at(x, y, z)] > 0.35f;
	};
	PackedVector3Array out_v;
	PackedVector3Array out_n;
	PackedColorArray out_c;
	PackedInt32Array out_i;
	const Vector3i dirs[6] = { Vector3i(1, 0, 0), Vector3i(-1, 0, 0), Vector3i(0, 1, 0), Vector3i(0, -1, 0), Vector3i(0, 0, 1), Vector3i(0, 0, -1) };
	for (int z = 0; z < dim.z; z++) {
		for (int y = 0; y < dim.y; y++) {
			for (int x = 0; x < dim.x; x++) {
				if (!solid(x, y, z)) {
					continue;
				}
				const Voxel &vx = grid[at(x, y, z)];
				// Self-shadowing the shell cannot show. Around the voxel: the fraction of its 26
				// neighbours that are filled, as an ambient occlusion (the hedge's deep interior is
				// dark in the full mesh). Into the canopy: see the face loop.
				int around = 0;
				for (int dz = -1; dz <= 1; dz++) {
					for (int dy = -1; dy <= 1; dy++) {
						for (int dx = -1; dx <= 1; dx++) {
							around += (dx | dy | dz) != 0 && solid(x + dx, y + dy, z + dz) ? 1 : 0;
						}
					}
				}
				const float ao = 1.0f - 0.45f * float(around) / 26.0f;
				const Vector3 albedo = vx.albedo / MAX(vx.area, 1e-8f) * ao;
				const float nl = vx.normal.length() / MAX(vx.area, 1e-8f); // 1: all leaves one way; 0: every way.
				const Vector3 mean_n = vx.normal.length() > 1e-8f ? vx.normal.normalized() : Vector3(0, 1, 0);
				// A partly filled voxel's faces sit in from its edge: the shell is not a lattice of
				// whole cubes.
				const float inset = 0.5f * p_voxel * CLAMP((0.9f - alpha[at(x, y, z)]) / 0.55f, 0.0f, 1.0f) * 0.35f;
				const Vector3 c0 = box.position + Vector3(x + 0.5f, y + 0.5f, z + 0.5f) * p_voxel;
				// The canopy's rounded outward direction (the "spherical normals" foliage trick:
				// a leaf mass lit as one soft volume), from the core and up.
				Vector3 round = c0 - centre;
				round.y = MAX(round.y, 0.0f) + 0.3f * box.size.y;
				round = round.normalized();
				for (int d = 0; d < 6; d++) {
					if (solid(x + dirs[d].x, y + dirs[d].y, z + dirs[d].z)) {
						continue;
					}
					const Vector3 fn(dirs[d].x, dirs[d].y, dirs[d].z);
					// The face's two in-plane axes.
					const Vector3 ua = d < 2 ? Vector3(0, 1, 0) : Vector3(1, 0, 0);
					const Vector3 va = fn.cross(ua);
					const Vector3 fc = c0 + fn * (0.5f * p_voxel - inset);
					const float h = 0.5f * p_voxel;
					// The leaves' own mean normal where they agree, the face's where they scatter: a
					// rough voxel lit like a leaf cloud, not a cube.
					// A leaf canopy is darker than a flat sheet of its leaves: the leaves a view ray
					// meets below the outer layer sit in each other's shade. Single scattering in a
					// leaf cloud (Ross 1981; Goudriaan 1977) brings a deep canopy's reflectance to
					// about half its leaf albedo; a single layer keeps it. The optical depth behind
					// this face is the leaf area met going in, k = 0.5.
					float tau = 0.0f;
					for (int k = 0; k < 8; k++) {
						const int qx = x - dirs[d].x * k, qy = y - dirs[d].y * k, qz = z - dirs[d].z * k;
						if (qx < 0 || qy < 0 || qz < 0 || qx >= dim.x || qy >= dim.y || qz >= dim.z) {
							break;
						}
						tau += 0.5f * grid[at(qx, qy, qz)].area / face;
					}
					// A sparse hedge also lets the view through its gaps to its own shaded inside:
					// the rays the leaves in front do not stop (exp(-tau) of them) end on shade at
					// about a fifth of the leaves' brightness.
					const float canopy = 0.45f + 0.55f * Math::exp(-0.5f * tau);
					const float cover = 1.0f - Math::exp(-tau);
					const Vector3 shade = albedo * (cover * canopy + (1.0f - cover) * 0.2f);
					const Vector3 sn = (mean_n * nl + round * (1.0f - nl) * 0.75f + fn * 0.25f).normalized();
					const int base = out_v.size();
					const Vector3 corners[4] = { fc - ua * h - va * h, fc + ua * h - va * h, fc + ua * h + va * h, fc - ua * h + va * h };
					for (int k = 0; k < 4; k++) {
						out_v.push_back(corners[k]);
						out_n.push_back(sn);
						out_c.push_back(Color(shade.x, shade.y, shade.z)); // Linear: the material reads it so.
					}
					// Wound so the face looks out along fn.
					const bool flip = ua.cross(va).dot(fn) < 0.0f;
					if (!flip) {
						out_i.push_back(base);
						out_i.push_back(base + 2);
						out_i.push_back(base + 1);
						out_i.push_back(base);
						out_i.push_back(base + 3);
						out_i.push_back(base + 2);
					} else {
						out_i.push_back(base);
						out_i.push_back(base + 1);
						out_i.push_back(base + 2);
						out_i.push_back(base);
						out_i.push_back(base + 2);
						out_i.push_back(base + 3);
					}
				}
			}
		}
	}
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	if (out_v.is_empty()) {
		return mesh;
	}
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = out_v;
	arrays[Mesh::ARRAY_NORMAL] = out_n;
	arrays[Mesh::ARRAY_COLOR] = out_c;
	arrays[Mesh::ARRAY_INDEX] = out_i;
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	Ref<StandardMaterial3D> m;
	m.instantiate();
	m->set_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
	m->set_roughness(1.0f);
	m->set_specular(0.3f);
	m->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
	mesh->surface_set_material(0, m);
	return mesh;
}

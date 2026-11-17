/**************************************************************************/
/*  rock_moss.cpp                                                         */
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

#include "rock_moss.h"

#include "core/math/random_pcg.h"
#include "scene/resources/shader.h"
#include "scene/resources/surface_tool.h"

#include <cmath>

namespace RockMoss {

static Ref<ShaderMaterial> shared_material;
static Ref<ShaderMaterial> shoot_material;

// The rock shader's noise (rk_hash3, rk_noise3, rk_fbm3), in floats as the GPU runs it, so the shell lies exactly
// where the shader paints its moss.
static inline float fract1(float x) {
	return x - std::floor(x);
}

static float hash3(float x, float y, float z) {
	x = fract1(x * 0.3183099f + 0.71f) * 17.0f;
	y = fract1(y * 0.3183099f + 0.113f) * 17.0f;
	z = fract1(z * 0.3183099f + 0.419f) * 17.0f;
	return fract1(x * y * z * (x + y + z));
}

static float noise3(const Vector3 &p) {
	const float ix = std::floor(p.x), iy = std::floor(p.y), iz = std::floor(p.z);
	float fx = p.x - ix, fy = p.y - iy, fz = p.z - iz;
	fx = fx * fx * (3.0f - 2.0f * fx);
	fy = fy * fy * (3.0f - 2.0f * fy);
	fz = fz * fz * (3.0f - 2.0f * fz);
	auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
	const float x00 = lerp(hash3(ix, iy, iz), hash3(ix + 1, iy, iz), fx);
	const float x10 = lerp(hash3(ix, iy + 1, iz), hash3(ix + 1, iy + 1, iz), fx);
	const float x01 = lerp(hash3(ix, iy, iz + 1), hash3(ix + 1, iy, iz + 1), fx);
	const float x11 = lerp(hash3(ix, iy + 1, iz + 1), hash3(ix + 1, iy + 1, iz + 1), fx);
	return lerp(lerp(x00, x10, fy), lerp(x01, x11, fy), fz);
}

static float fbm3(const Vector3 &p) {
	return noise3(p) * 0.5f + noise3(p * 2.03f + Vector3(11, 11, 11)) * 0.25f + noise3(p * 4.11f + Vector3(23, 23, 23)) * 0.125f + noise3(p * 8.3f + Vector3(37, 37, 37)) * 0.0625f;
}

static inline float smooth(float e0, float e1, float x) {
	const float t = CLAMP((x - e0) / (e1 - e0), 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

void build(void *p_job) {
	Job *job = static_cast<Job *>(p_job);
	const Source &src = job->source;
	const Params &pm = job->params;
	const int nv = src.vertices.size();
	if (nv == 0 || src.indices.size() < 3 || src.uv.size() != nv || src.uv2.size() != nv || src.color.size() != nv || src.normals.size() != nv) {
		return;
	}
	const Basis &b = pm.xform.basis;
	const float s = b.get_column(0).length(); // metres per unit
	const Color &cd = pm.custom;
	const float v_scale = cd.b > 0.0f ? cd.b : s;
	const float packed = cd.a;
	const float v_random = packed < 1.0f ? packed : std::fmod(packed, 64.0f) / 64.0f;
	const float q = std::floor(packed / 64.0f);
	const Vector2 v_slope = packed >= 64.0f ? Vector2(std::fmod(q, 256.0f), std::floor(q / 256.0f)) / 255.0f * 4.0f - Vector2(2, 2) : Vector2();
	const float seed = v_random * 61.7f;
	const Vector3 pole = Vector3(pm.pole_direction.x, 0, pm.pole_direction.z) + Vector3(1e-5f, 0, 0);
	const Vector3 pole_n = pole.normalized();
	const Vector3 origin = pm.xform.origin;

	// The shader's moss weight at each vertex, and the shell's depth there (metres): mats about a centimetre,
	// cushions to 3.5 cm where the moss is thickest, lumpy at a few centimetres; the margin sunk 3 mm into the rock.
	LocalVector<float> weight, depth, up;
	weight.resize(nv);
	depth.resize(nv);
	up.resize(nv);
	const float thr = 0.22f;
	for (int i = 0; i < nv; i++) {
		const Vector3 wn = b.xform(src.normals[i]).normalized();
		up[i] = wn.y;
		const Vector3 wp = pm.xform.xform(src.vertices[i]);
		const Vector3 lp(src.uv[i].x, src.uv2[i].x, src.uv[i].y);
		const Vector3 mp = lp * v_scale + Vector3(seed, seed, seed);
		const float cavity = src.color[i].r;
		const float fresh = src.color[i].g;
		float contact = 0.0f;
		if (cd.b > 0.0f) {
			const float above = wp.y - (cd.r + v_slope.dot(Vector2(wp.x - origin.x, wp.z - origin.z)));
			contact = 1.0f - smooth(0.0f, pm.contact_height * MAX(v_scale, 0.25f), above);
			if (above < -0.01f) {
				weight[i] = -1.0f; // under the ground
				continue;
			}
		}
		const float shade_side = pm.aspect_strength * (wn.x * pole_n.x + wn.z * pole_n.z);
		const float term = 0.9f * shade_side + 0.45f * (1.0f - cavity) + 0.8f * contact * (1.0f - smooth(0.4f, 0.85f, wn.y)) + 0.25f * MAX(wn.y, 0.0f) - 0.4f * MAX(-wn.y - 0.3f, 0.0f);
		const float place = smooth(0.2f, 0.7f, term) * (1.0f - fresh);
		const float w = pm.moss_cover * place * smooth(0.42f, 0.58f, fbm3(mp * 2.2f + Vector3(17, 17, 17)) + 0.25f * pm.moss_cover - 0.12f);
		weight[i] = w;
	}
	// The weight smoothed over the shape a few centimetres (Jacobi passes over its edges): the cavity term follows
	// the grain at millimetres, and a weight that flickers vertex to vertex leaves a shell of single raised corners
	// sunk in the rock. Moss grows in patches.
	{
		LocalVector<float> sum, cnt;
		sum.resize(nv);
		cnt.resize(nv);
		for (int pass = 0; pass < 6; pass++) {
			for (int i = 0; i < nv; i++) {
				sum[i] = MAX(weight[i], 0.0f);
				cnt[i] = 1.0f;
			}
			for (int t = 0; t + 2 < src.indices.size(); t += 3) {
				const int a = src.indices[t], c = src.indices[t + 1], d = src.indices[t + 2];
				const float wa = MAX(weight[a], 0.0f), wc = MAX(weight[c], 0.0f), wd = MAX(weight[d], 0.0f);
				sum[a] += wc + wd;
				sum[c] += wa + wd;
				sum[d] += wa + wc;
				cnt[a] += 2.0f;
				cnt[c] += 2.0f;
				cnt[d] += 2.0f;
			}
			for (int i = 0; i < nv; i++) {
				if (weight[i] >= 0.0f) {
					weight[i] = sum[i] / cnt[i];
				}
			}
		}
	}
	for (int i = 0; i < nv; i++) {
		const float w = weight[i];
		if (w < 0.0f) {
			weight[i] = 0.0f;
			depth[i] = -0.003f;
			continue;
		}
		const Vector3 mp = Vector3(src.uv[i].x, src.uv2[i].x, src.uv[i].y) * v_scale + Vector3(seed, seed, seed);
		if (w < thr) {
			depth[i] = -0.003f;
		} else {
			// A cushion stands up at its edge within a centimetre or two (a rounded rim that shades the rock beside
			// it), then swells to its full depth inside; the thickest moss where it is most at home.
			const float rel = CLAMP((w - thr) / MAX(pm.moss_cover - thr, 0.02f), 0.0f, 1.0f);
			const float grow = smooth(0.0f, 0.04f, rel);
			const float full = Math::lerp(0.012f, 0.035f, smooth(0.15f, 0.8f, rel));
			// Lumps: cushions of a few centimetres, each a dome of shoots, on a slower swell.
			const float lump = 0.45f + 0.7f * fbm3(mp * 7.0f + Vector3(3, 3, 3)) + 0.45f * fbm3(mp * 22.0f + Vector3(9, 1, 5));
			depth[i] = MAX(full * grow * lump, 0.005f * grow + 0.0015f);
		}
	}

	// Triangles with any moss: the shell over them, its vertices lifted along the shape's normal.
	const int nt = src.indices.size() / 3;
	LocalVector<int> remap;
	remap.resize(nv);
	for (int i = 0; i < nv; i++) {
		remap[i] = -1;
	}
	PackedVector3Array out_v;
	PackedVector2Array out_uv, out_uv2;
	PackedColorArray out_c;
	PackedInt32Array out_i;
	for (int t = 0; t < nt; t++) {
		const int a = src.indices[t * 3], c = src.indices[t * 3 + 1], d = src.indices[t * 3 + 2];
		if (MAX(weight[a], MAX(weight[c], weight[d])) < thr) {
			continue;
		}
		for (int k = 0; k < 3; k++) {
			const int i = src.indices[t * 3 + k];
			if (remap[i] < 0) {
				remap[i] = out_v.size();
				const Vector3 n = src.normals[i].normalized();
				out_v.push_back(src.vertices[i] + n * (depth[i] / s));
				// The material's pattern space, in metres and per stone (the rock shader's own: the stone's rest position
				// times its scale plus its seed), so a path-traced hit reads it from the vertices as raster does.
				const Vector3 mp = Vector3(src.uv[i].x, src.uv2[i].x, src.uv[i].y) * v_scale + Vector3(seed, seed, seed);
				out_uv.push_back(Vector2(mp.x, mp.z));
				out_uv2.push_back(Vector2(mp.y, 0.0f));
				// r: how much at home the moss is (0 at its edge, 1 at its best; thick moss greener), g: the depth over a
				// full cushion, b: how much it faces up, a: the edge (0.5 on the moss's contour; the material cuts it
				// ragged there).
				const float rel = (weight[i] - thr) / MAX(pm.moss_cover - thr, 0.02f);
				out_c.push_back(Color(CLAMP(rel, 0.0f, 1.0f), CLAMP(depth[i] / 0.035f, 0.0f, 1.0f), up[i] * 0.5f + 0.5f, CLAMP(0.5f + 3.0f * rel, 0.0f, 1.0f)));
			}
			out_i.push_back(remap[i]);
		}
	}
	if (out_i.is_empty()) {
		return;
	}
	// The close-up shape is a few millimetres a vertex; the shell needs about a centimetre (its lumps are 3 cm and
	// the shoots are the material's). Simplified (meshoptimizer, the vertices a subset of the shell's own, so the
	// attributes stay) to within 1.5 mm, then compacted.
	if (SurfaceTool::simplify_func && out_i.size() > 3000) {
		AABB box(out_v[0], Vector3());
		for (int i = 1; i < out_v.size(); i++) {
			box.expand_to(out_v[i]);
		}
		const float extent = MAX(box.get_longest_axis_size(), 1e-6f);
		Vector<uint32_t> dst;
		dst.resize(out_i.size());
		float err = 0.0f;
		const size_t n = SurfaceTool::simplify_func(dst.ptrw(), (const unsigned int *)out_i.ptr(), out_i.size(), (const float *)out_v.ptr(), out_v.size(), sizeof(Vector3), MAX(out_i.size() / 10, 3000), (0.0015f / s) / extent, 0, &err);
		if (n >= 3) {
			LocalVector<int> keep;
			keep.resize(out_v.size());
			for (int i = 0; i < out_v.size(); i++) {
				keep[i] = -1;
			}
			PackedVector3Array cv;
			PackedVector2Array cuv, cuv2;
			PackedColorArray cc;
			PackedInt32Array ci;
			for (size_t k = 0; k < n; k++) {
				const int i = int(dst[k]);
				if (keep[i] < 0) {
					keep[i] = cv.size();
					cv.push_back(out_v[i]);
					cuv.push_back(out_uv[i]);
					cuv2.push_back(out_uv2[i]);
					cc.push_back(out_c[i]);
				}
				ci.push_back(keep[i]);
			}
			out_v = cv;
			out_uv = cuv;
			out_uv2 = cuv2;
			out_c = cc;
			out_i = ci;
		}
	}
	// Normals of the shell itself (area weighted), so its lumps light as lumps.
	PackedVector3Array out_n;
	out_n.resize(out_v.size());
	Vector3 *nw = out_n.ptrw();
	for (int i = 0; i < out_n.size(); i++) {
		nw[i] = Vector3();
	}
	for (int t = 0; t < out_i.size() / 3; t++) {
		const int a = out_i[t * 3], c = out_i[t * 3 + 1], d = out_i[t * 3 + 2];
		// Godot's front faces wind clockwise: (c - a) x (d - a) points into the surface.
		const Vector3 fn = (out_v[d] - out_v[a]).cross(out_v[c] - out_v[a]);
		nw[a] += fn;
		nw[c] += fn;
		nw[d] += fn;
	}
	for (int i = 0; i < out_n.size(); i++) {
		nw[i] = nw[i].length_squared() > 1e-20f ? nw[i].normalized() : Vector3(0, 1, 0);
	}
	// Shoots (as the trees' moss on lying wood): tufts of tapered blades a centimetre or two long over the cushion,
	// lit yellow-green at their tips and dark between, standing up from it and arching over. Towards the edge they get
	// shorter and sparser, and a few stand alone on the bare rock past it, so the moss thins out raggedly instead of
	// ending at a line. One triangle per blade face, both faces.
	{
		RandomPCG rng;
		rng.seed(uint64_t(Math::abs(seed) * 1000.0f) * 2654435761u + uint64_t(nv) * 97u + 11u);
		PackedVector3Array sv, sn;
		PackedVector2Array suv;
		PackedColorArray sc;
		const float density = 2200.0f; // tufts per m^2 over a full cushion (one per 2 cm square)
		const int max_blades = 7000;
		int blades_made = 0;
		auto tufts_on = [&](const Vector3 &a, const Vector3 &b, const Vector3 &c, const Vector3 &na, const Vector3 &nb, const Vector3 &nc, float carpet, float tone, float area_m2) {
			float want = area_m2 * density * carpet;
			int n = int(want);
			if (rng.randf() < want - float(n)) {
				n++;
			}
			for (int k = 0; k < n && blades_made < max_blades; k++) {
				float u = rng.randf(), v = rng.randf();
				if (u + v > 1.0f) {
					u = 1.0f - u;
					v = 1.0f - v;
				}
				const Vector3 p = a + (b - a) * u + (c - a) * v;
				const Vector3 nn = (na * (1.0f - u - v) + nb * u + nc * v).normalized();
				const Vector3 t1 = nn.cross(Math::abs(nn.y) < 0.9f ? Vector3(0, 1, 0) : Vector3(1, 0, 0)).normalized();
				const Vector3 t2 = nn.cross(t1);
				const int blades = 3 + int(rng.randf() * (1.0f + 3.0f * carpet));
				const float len = Math::lerp(0.006f, 0.016f, rng.randf()) * (0.7f + 0.8f * tone) * Math::lerp(0.4f, 1.0f, carpet) / s;
				const float az0 = rng.randf() * float(Math::TAU);
				const float sh = Math::lerp(0.8f, 1.25f, rng.randf());
				// Tip and root colours (linear): yellow-green lit tips, dark olive in the tuft.
				const Color tip_c = Color(0.095f * sh, 0.13f * sh, 0.025f * sh).lerp(Color(0.14f, 0.13f, 0.05f), 0.4f * (1.0f - tone));
				const Color root_c = Color(0.018f, 0.035f, 0.008f);
				for (int bl = 0; bl < blades; bl++) {
					const float az = az0 + float(Math::TAU) * (float(bl) + 0.4f * rng.randf()) / float(blades);
					const float tilt = Math::lerp(0.15f, 0.75f, rng.randf());
					const Vector3 side = t1 * Math::cos(az) + t2 * Math::sin(az);
					const Vector3 dir = (nn * Math::cos(tilt) + side * Math::sin(tilt)).normalized();
					const float l = len * Math::lerp(0.7f, 1.15f, rng.randf());
					const float wd = Math::lerp(0.0012f, 0.0022f, rng.randf()) / s;
					const Vector3 across = dir.cross(nn).normalized() * wd;
					const Vector3 tip = p + dir * (0.55f * l) + (dir * 0.6f + side * 0.5f - nn * 0.1f).normalized() * (0.45f * l);
					const Vector3 bn = nn; // shaded as the cushion's surface
					const Vector3 vp[3] = { p - across, p + across, tip };
					const Color vc[3] = { root_c, root_c, tip_c };
					const float vy[3] = { 0.0f, 0.0f, 1.0f };
					for (int face = 0; face < 2; face++) {
						for (int j = 0; j < 3; j++) {
							const int jj = face == 0 ? j : (j == 0 ? 0 : 3 - j);
							sv.push_back(vp[jj]);
							sn.push_back(bn);
							suv.push_back(Vector2(0.0f, vy[jj]));
							sc.push_back(vc[jj]);
						}
					}
					blades_made++;
				}
			}
		};
		const float s2 = s * s;
		// Over the cushion: its own triangles, the carpet thinning towards the edge.
		for (int t = 0; t + 2 < out_i.size(); t += 3) {
			const int a = out_i[t], b = out_i[t + 1], c = out_i[t + 2];
			const float rel = (out_c[a].r + out_c[b].r + out_c[c].r) / 3.0f;
			const float edge = (out_c[a].a + out_c[b].a + out_c[c].a) / 3.0f;
			if (edge < 0.5f) {
				continue;
			}
			const float tone = (out_c[a].g + out_c[b].g + out_c[c].g) / 3.0f;
			const float area = 0.5f * (out_v[b] - out_v[a]).cross(out_v[c] - out_v[a]).length() * s2;
			tufts_on(out_v[a], out_v[b], out_v[c], out_n[a], out_n[b], out_n[c], smooth(0.0f, 0.5f, rel), tone, area);
		}
		// Past the edge, on the bare rock: a few short lone tufts where the moss nearly reaches.
		for (int t = 0; t < nt; t++) {
			const int a = src.indices[t * 3], c = src.indices[t * 3 + 1], d = src.indices[t * 3 + 2];
			const float wmax = MAX(weight[a], MAX(weight[c], weight[d]));
			if (wmax >= thr || wmax < thr * 0.7f) {
				continue;
			}
			const float carpet = 0.12f * smooth(thr * 0.7f, thr, wmax);
			const float area = 0.5f * (src.vertices[c] - src.vertices[a]).cross(src.vertices[d] - src.vertices[a]).length() * s2;
			tufts_on(src.vertices[a], src.vertices[c], src.vertices[d], src.normals[a].normalized(), src.normals[c].normalized(), src.normals[d].normalized(), carpet, 0.2f, area);
		}
		if (!sv.is_empty()) {
			job->shoot_arrays.resize(Mesh::ARRAY_MAX);
			job->shoot_arrays[Mesh::ARRAY_VERTEX] = sv;
			job->shoot_arrays[Mesh::ARRAY_NORMAL] = sn;
			job->shoot_arrays[Mesh::ARRAY_TEX_UV] = suv;
			job->shoot_arrays[Mesh::ARRAY_COLOR] = sc;
			job->shoots = blades_made;
		}
	}
	job->arrays.resize(Mesh::ARRAY_MAX);
	job->arrays[Mesh::ARRAY_VERTEX] = out_v;
	job->arrays[Mesh::ARRAY_NORMAL] = out_n;
	job->arrays[Mesh::ARRAY_TEX_UV] = out_uv;
	job->arrays[Mesh::ARRAY_TEX_UV2] = out_uv2;
	job->arrays[Mesh::ARRAY_COLOR] = out_c;
	job->arrays[Mesh::ARRAY_INDEX] = out_i;
	job->triangles = out_i.size() / 3 + job->shoots * 2;
}

Ref<Material> get_material() {
	if (shared_material.is_valid()) {
		return shared_material;
	}
	Ref<Shader> sh;
	sh.instantiate();
	// Moss as its shoots: leafy shoots a few millimetres across packed into the cushion (domain-warped cells, each a
	// shoot tip catching the light, the gaps between them dark), greener and darker where it is thick and damp,
	// yellow-olive and paler where thin on a dry top; matte, with light through the thin shoots.
	sh->set_code(R"(shader_type spatial;
render_mode cull_back, diffuse_burley, specular_schlick_ggx;
uniform vec3 moss_deep = vec3(0.022, 0.05, 0.008);
uniform vec3 moss_tip = vec3(0.075, 0.1, 0.022);
uniform vec3 moss_dry = vec3(0.12, 0.11, 0.05);
float mh(vec3 p) { p = fract(p * 0.3183099 + vec3(0.71, 0.113, 0.419)); p *= 17.0; return fract(p.x * p.y * p.z * (p.x + p.y + p.z)); }
float mn(vec3 p) {
	vec3 i = floor(p), f = fract(p);
	f = f * f * (3.0 - 2.0 * f);
	return mix(mix(mix(mh(i), mh(i + vec3(1, 0, 0)), f.x), mix(mh(i + vec3(0, 1, 0)), mh(i + vec3(1, 1, 0)), f.x), f.y),
			mix(mix(mh(i + vec3(0, 0, 1)), mh(i + vec3(1, 0, 1)), f.x), mix(mh(i + vec3(0, 1, 1)), mh(i + vec3(1, 1, 1)), f.x), f.y), f.z);
}
// Shoot tips: the distance to the nearest jittered point (Worley), 1 at a tip, 0 in the gaps.
float shoots(vec3 p) {
	vec3 i = floor(p), f = fract(p);
	float d = 9.0;
	for (int z = -1; z <= 1; z++) {
		for (int y = -1; y <= 1; y++) {
			for (int x = -1; x <= 1; x++) {
				vec3 o = vec3(float(x), float(y), float(z));
				vec3 c = o + vec3(mh(i + o), mh(i + o + 7.1), mh(i + o + 13.7));
				d = min(d, length(c - f));
			}
		}
	}
	return 1.0 - smoothstep(0.1, 0.75, d);
}
float tips(vec3 p) {
	vec3 q = p * 260.0; // shoots about 4 mm apart
	q += vec3(mn(p * 40.0), mn(p * 40.0 + 5.0), mn(p * 40.0 + 9.0)) * 1.6; // packed, not on a lattice
	return shoots(q);
}
// Tufts: the shoots grow in bundles about 12 mm across, each a rounded top with a dark gap round it. This is the
// scale a mat reads at from a metre or more (the shoots themselves are under a pixel there).
float tufts(vec3 p) {
	// Warped twice (at 7 cm and 2 cm) so a slanted face never cuts the cells into regular rows.
	vec3 q = p * 85.0;
	q += vec3(mn(p * 5.0 + 21.0), mn(p * 5.0 + 27.0), mn(p * 5.0 + 33.0)) * 3.2;
	q += vec3(mn(p * 14.0), mn(p * 14.0 + 3.0), mn(p * 14.0 + 11.0)) * 1.3;
	return shoots(q);
}
void fragment() {
	vec3 m_pos = vec3(UV.x, UV2.x, UV.y); // metres, stone-aligned, seeded per stone (see build())
	float thick = COLOR.r;
	float h = COLOR.g;
	float u = tufts(m_pos);
	float t = tips(m_pos);
	// Patches of colour at a few centimetres: shoots that grew in different years and spots.
	float patch = mn(m_pos * 18.0) * 0.6 + mn(m_pos * 55.0) * 0.4;
	vec3 tip = mix(moss_tip, moss_dry, smoothstep(0.55, 0.25, thick) * smoothstep(0.4, 0.9, COLOR.b * 2.0 - 1.0));
	// Lit tuft tops, dark gaps between them; the shoots fleck the tops.
	float lit = smoothstep(0.05, 0.85, u) * (0.75 + 0.35 * t);
	vec3 col = mix(moss_deep, tip * (0.8 + 0.4 * patch), clamp(lit, 0.0, 1.0));
	// The cushion's thin edge lets the rock's dark show through between sparse shoots.
	col *= mix(0.6, 1.0, smoothstep(0.0, 0.35, h));
	// Colour drifts over a cushion: greener and darker in its dips, yellower on its lit domes.
	col *= mix(0.75, 1.15, smoothstep(0.2, 0.9, h));
	ALBEDO = col;
	// A ragged edge: the cushion ends along its contour (COLOR.a 0.5) broken at a few millimetres to a few centimetres.
	// Lobes and bays a few centimetres across break the outline at the scale it reads from a metre.
	float rag = mn(m_pos * 90.0) * 0.6 + mn(m_pos * 260.0) * 0.4;
	float lobe = mn(m_pos * 24.0 + 13.0) * 0.65 + mn(m_pos * 9.0 + 7.0) * 0.35;
	ALPHA = COLOR.a + (rag - 0.5) * 0.7 + (lobe - 0.5) * 0.7;
	ALPHA_SCISSOR_THRESHOLD = 0.5;
	ROUGHNESS = 0.82 + 0.1 * (1.0 - u);
	SPECULAR = 0.3;
	// The tufts as relief: tops up, gaps down (bump from the same field, 2 mm steps).
	float e = 0.002;
	vec3 g = vec3(tufts(m_pos + vec3(e, 0, 0)) - u, tufts(m_pos + vec3(0, e, 0)) - u, tufts(m_pos + vec3(0, 0, e)) - u) / e;
	// The pattern space is the stone's own (uniform scale), so its gradient turns into view space with the model view.
	g = (VIEW_MATRIX * (MODEL_MATRIX * vec4(g, 0.0))).xyz;
	g /= max(length(MODEL_MATRIX[0].xyz), 1e-4);
	vec3 n = normalize(NORMAL);
	g -= n * dot(g, n);
	NORMAL = normalize(n - 0.006 * g);
	// The tuft gaps and the low edge of the cushion in their own shade.
	AO = mix(0.45, 1.0, u) * mix(0.55, 1.0, smoothstep(0.5, 0.9, COLOR.a));
	// Light through thin shoots (a leaf a cell or two thick).
	BACKLIGHT = col * 0.6;
}
)");
	shared_material.instantiate();
	shared_material->set_shader(sh);
	return shared_material;
}

// The shoots: vertex colour (dark olive root to lit tip), matte, light through the thin blades.
Ref<Material> get_shoot_material() {
	if (shoot_material.is_valid()) {
		return shoot_material;
	}
	Ref<Shader> sh;
	sh.instantiate();
	sh->set_code(R"(shader_type spatial;
render_mode cull_back, diffuse_burley, specular_schlick_ggx;
void fragment() {
	ALBEDO = COLOR.rgb;
	ROUGHNESS = 0.8;
	SPECULAR = 0.3;
	AO = mix(0.5, 1.0, UV.y);
	BACKLIGHT = COLOR.rgb * 0.7;
}
)");
	shoot_material.instantiate();
	shoot_material->set_shader(sh);
	return shoot_material;
}

void finish() {
	shared_material.unref();
	shoot_material.unref();
}

} // namespace RockMoss

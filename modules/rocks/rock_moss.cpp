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

#include "scene/resources/shader.h"
#include "scene/resources/surface_tool.h"

#include <cmath>

namespace RockMoss {

static Ref<ShaderMaterial> shared_material;

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
				weight[i] = 0.0f; // under the ground
				depth[i] = -0.003f;
				continue;
			}
		}
		const float shade_side = pm.aspect_strength * (wn.x * pole_n.x + wn.z * pole_n.z);
		const float place = smooth(0.35f, 0.85f, wn.y + 0.35f * (1.0f - cavity) + 0.3f * contact + 0.45f * shade_side) * (1.0f - fresh);
		const float w = pm.moss_cover * place * smooth(0.42f, 0.58f, fbm3(mp * 2.2f + Vector3(17, 17, 17)) + 0.25f * pm.moss_cover - 0.12f);
		weight[i] = w;
		if (w < thr) {
			depth[i] = -0.003f;
		} else {
			const float grow = smooth(thr, thr + 0.25f, w);
			const float full = Math::lerp(0.01f, 0.035f, smooth(0.45f, 0.85f, w));
			const float lump = 0.55f + 0.9f * fbm3(mp * 9.0f + Vector3(3, 3, 3));
			depth[i] = MAX(full * grow * lump, 0.0015f);
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
				// r: the moss weight (thick moss greener), g: the depth over the full cushion, b: how much it faces up.
				out_c.push_back(Color(weight[i], CLAMP(depth[i] / 0.035f, 0.0f, 1.0f), up[i] * 0.5f + 0.5f, 1));
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
	job->arrays.resize(Mesh::ARRAY_MAX);
	job->arrays[Mesh::ARRAY_VERTEX] = out_v;
	job->arrays[Mesh::ARRAY_NORMAL] = out_n;
	job->arrays[Mesh::ARRAY_TEX_UV] = out_uv;
	job->arrays[Mesh::ARRAY_TEX_UV2] = out_uv2;
	job->arrays[Mesh::ARRAY_COLOR] = out_c;
	job->arrays[Mesh::ARRAY_INDEX] = out_i;
	job->triangles = out_i.size() / 3;
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
	vec3 q = p * 85.0;
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
	ALBEDO = col;
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
	AO = mix(0.45, 1.0, u);
	// Light through thin shoots (a leaf a cell or two thick).
	BACKLIGHT = col * 0.6;
}
)");
	shared_material.instantiate();
	shared_material->set_shader(sh);
	return shared_material;
}

void finish() {
	shared_material.unref();
}

} // namespace RockMoss

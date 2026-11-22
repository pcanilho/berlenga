/**************************************************************************/
/*  plant_impostor.cpp                                                    */
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


// Octahedral impostors (Brucks, "Octahedral Impostors", 2018; docs/ideas/vegetation-lod-streaming.md):
// a plant baked from a grid of directions over the upper hemisphere into an atlas, drawn far away
// as one quad that blends the three nearest views. Raster only: VegetationField3D gives the path
// tracer the real mesh, which keeps depth and shadows.

#include "plant_generator.h"
#include "plant_species.h"

#include "modules/weather/weather_map_3d.h"

#include "core/object/worker_thread_pool.h"

namespace {

// Hemi-octahedral map (Brucks): a direction with y >= 0 to [-1, 1]^2 and back.
Vector2 hemi_encode(Vector3 p_d) {
	p_d.y = MAX(p_d.y, 0.0f);
	const float s = Math::abs(p_d.x) + p_d.y + Math::abs(p_d.z);
	const Vector2 p(p_d.x / s, p_d.z / s);
	return Vector2(p.x + p.y, p.x - p.y);
}

Vector3 hemi_decode(const Vector2 &p_g) {
	const Vector2 p((p_g.x + p_g.y) * 0.5f, (p_g.x - p_g.y) * 0.5f);
	return Vector3(p.x, 1.0f - Math::abs(p.x) - Math::abs(p.y), p.y).normalized();
}

// The frame's right and up axes; the shader builds the same ones.
void frame_basis(const Vector3 &p_d, Vector3 &r_right, Vector3 &r_up) {
	r_right = Math::abs(p_d.y) > 0.999f ? Vector3(1, 0, 0) : Vector3(0, 1, 0).cross(p_d).normalized();
	r_up = p_d.cross(r_right);
}

// Full octahedral normal encoding to [0, 1]^2 (Cigolle et al., "A Survey of Efficient
// Representations for Independent Unit Vectors", JCGT 2014).
Vector2 oct_encode(const Vector3 &p_n) {
	const float s = Math::abs(p_n.x) + Math::abs(p_n.y) + Math::abs(p_n.z);
	Vector2 p(p_n.x / s, p_n.y / s);
	if (p_n.z < 0.0f) {
		p = Vector2((1.0f - Math::abs(p.y)) * (p.x >= 0.0f ? 1.0f : -1.0f), (1.0f - Math::abs(p.x)) * (p.y >= 0.0f ? 1.0f : -1.0f));
	}
	return p * 0.5f + Vector2(0.5f, 0.5f);
}

float srgb_to_linear(float p_c) {
	return p_c <= 0.04045f ? p_c / 12.92f : Math::pow((p_c + 0.055f) / 1.055f, 2.4f);
}

float linear_to_srgb(float p_c) {
	p_c = CLAMP(p_c, 0.0f, 1.0f);
	return p_c <= 0.0031308f ? p_c * 12.92f : 1.055f * Math::pow(p_c, 1.0f / 2.4f) - 0.055f;
}

Vector3 to_linear(const Color &p_c) {
	return Vector3(srgb_to_linear(p_c.r), srgb_to_linear(p_c.g), srgb_to_linear(p_c.b));
}

Color param_color(const Ref<ShaderMaterial> &p_mat, const StringName &p_name, const Color &p_default) {
	const Variant v = p_mat.is_valid() ? p_mat->get_shader_parameter(p_name) : Variant();
	if (v.get_type() == Variant::COLOR) {
		return v;
	}
	if (v.get_type() == Variant::VECTOR3) {
		const Vector3 c = v;
		return Color(c.x, c.y, c.z);
	}
	return p_default;
}

Ref<Image> param_image(const Ref<ShaderMaterial> &p_mat, const StringName &p_name) {
	if (p_mat.is_null()) {
		return Ref<Image>();
	}
	Ref<Texture2D> tex = p_mat->get_shader_parameter(p_name);
	if (tex.is_null()) {
		return Ref<Image>();
	}
	Ref<Image> img = tex->get_image();
	if (img.is_null() || img->is_empty()) {
		return Ref<Image>();
	}
	img = img->duplicate();
	if (img->is_compressed()) {
		img->decompress();
	}
	img->clear_mipmaps();
	img->convert(Image::FORMAT_RGBA8);
	return img;
}

Color sample_bilinear(const Ref<Image> &p_img, Vector2 p_uv) {
	const int w = p_img->get_width();
	const int h = p_img->get_height();
	const float x = (p_uv.x - Math::floor(p_uv.x)) * w - 0.5f;
	const float y = (p_uv.y - Math::floor(p_uv.y)) * h - 0.5f;
	const int x0 = int(Math::floor(x));
	const int y0 = int(Math::floor(y));
	const float fx = x - x0;
	const float fy = y - y0;
	auto px = [&](int p_x, int p_y) {
		return p_img->get_pixel(CLAMP(p_x, 0, w - 1), CLAMP(p_y, 0, h - 1));
	};
	return px(x0, y0).lerp(px(x0 + 1, y0), fx).lerp(px(x0, y0 + 1).lerp(px(x0 + 1, y0 + 1), fx), fy);
}

struct BakeSurface {
	PackedVector3Array verts;
	PackedVector3Array normals;
	PackedColorArray colors;
	PackedVector2Array uvs;
	PackedInt32Array indices;
	Vector3 base; // Linear.
	Vector3 back; // Linear back light colour, when there is no back light texture.
	Vector3 dead; // Linear.
	Ref<Image> albedo;
	Ref<Image> backlight;
};

// One shaded point of the plant, as the plant shader colours it (no lighting).
struct BakeSample {
	Vector3 albedo; // Linear.
	Vector3 normal; // Object space, towards the viewer.
	float back = 0.0f; // Back light over albedo luminance.
	float depth = -1e30f; // Towards the viewer, from the centre.
};

void shade(const BakeSurface &p_s, const Vector3 &p_bary, int p_i0, int p_i1, int p_i2, const Vector3 &p_view, BakeSample &r_out) {
	Color col(1, 1, 1, 1);
	if (p_s.colors.size()) {
		col = p_s.colors[p_i0] * p_bary.x + p_s.colors[p_i1] * p_bary.y + p_s.colors[p_i2] * p_bary.z;
	}
	Vector3 albedo = p_s.base * Vector3(col.r, col.g, col.b);
	Vector3 back = p_s.back;
	if (p_s.uvs.size() && (p_s.albedo.is_valid() || p_s.backlight.is_valid())) {
		const Vector2 uv = p_s.uvs[p_i0] * p_bary.x + p_s.uvs[p_i1] * p_bary.y + p_s.uvs[p_i2] * p_bary.z;
		if (p_s.albedo.is_valid()) {
			albedo *= to_linear(sample_bilinear(p_s.albedo, uv));
		}
		if (p_s.backlight.is_valid()) {
			const Color b = sample_bilinear(p_s.backlight, uv);
			back = Vector3(b.r, b.g, b.b);
		}
	}
	// Wear, as the plant shader: yellow, then brown.
	const float dead = 1.0f - col.a;
	if (dead > 0.0f) {
		const float luma = albedo.dot(Vector3(0.2126f, 0.7152f, 0.0722f));
		const Vector3 yellow = Vector3(0.30f, 0.20f, 0.025f) * CLAMP(luma * 8.0f, 0.6f, 1.4f);
		const Vector3 brown = p_s.dead * CLAMP(luma * 6.0f, 0.55f, 1.5f);
		const float t = Math::smoothstep(0.35f, 0.85f, dead);
		albedo = albedo.lerp(yellow.lerp(brown, t), Math::smoothstep(0.0f, 0.5f, dead));
		back *= 1.0f - 0.7f * dead;
	}
	Vector3 n = p_s.normals.size() ? (p_s.normals[p_i0] * p_bary.x + p_s.normals[p_i1] * p_bary.y + p_s.normals[p_i2] * p_bary.z) : Vector3();
	if (n.length_squared() < 1e-12f) {
		const Vector3 a = p_s.verts[p_i0];
		n = (p_s.verts[p_i1] - a).cross(p_s.verts[p_i2] - a);
	}
	n.normalize();
	if (n.dot(p_view) < 0.0f) {
		n = -n; // Two sided, as the plant shader draws it.
	}
	const float luma = MAX(albedo.dot(Vector3(0.2126f, 0.7152f, 0.0722f)), 1e-4f);
	r_out.albedo = albedo;
	r_out.normal = n;
	r_out.back = back.dot(Vector3(0.2126f, 0.7152f, 0.0722f)) / luma;
}

// Rescales each mip's coverage so that as many texels pass the alpha test as at the top level
// (Castaño, "Computing Alpha Mipmaps", 2010): otherwise thin stems and leaves fade away with
// distance.
void keep_coverage(Ref<Image> &r_img, float p_threshold) {
	const int levels = r_img->get_mipmap_count() + 1;
	auto coverage = [&](int p_level, float p_scale) {
		int64_t ofs = 0;
		int64_t bytes = 0;
		int w = 0;
		int h = 0;
		r_img->get_mipmap_offset_size_and_dimensions(p_level, ofs, bytes, w, h);
		const uint8_t *d = r_img->ptr() + ofs;
		int pass = 0;
		for (int i = 0; i < w * h; i++) {
			pass += d[i * 4 + 3] / 255.0f * p_scale > p_threshold;
		}
		return float(pass) / float(MAX(w * h, 1));
	};
	const float want = coverage(0, 1.0f);
	Vector<uint8_t> data = r_img->get_data();
	for (int l = 1; l < levels; l++) {
		float lo = 0.0f;
		float hi = 4.0f;
		for (int it = 0; it < 12; it++) {
			const float mid = (lo + hi) * 0.5f;
			(coverage(l, mid) < want ? lo : hi) = mid;
		}
		const float scale = (lo + hi) * 0.5f;
		int64_t ofs = 0;
		int64_t bytes = 0;
		int w = 0;
		int h = 0;
		r_img->get_mipmap_offset_size_and_dimensions(l, ofs, bytes, w, h);
		uint8_t *d = data.ptrw() + ofs;
		for (int i = 0; i < w * h; i++) {
			d[i * 4 + 3] = uint8_t(CLAMP(Math::round(d[i * 4 + 3] * scale), 0.0f, 255.0f));
		}
	}
	r_img = Image::create_from_data(r_img->get_width(), r_img->get_height(), true, r_img->get_format(), data);
}

template <typename F>
void call_frame(void *p_userdata, uint32_t p_index) {
	(*static_cast<F *>(p_userdata))(p_index);
}

} // namespace

Ref<ArrayMesh> PlantGenerator::make_impostor(const Ref<Mesh> &p_mesh, int p_frames, int p_frame_size) {
	ERR_FAIL_COND_V(p_mesh.is_null(), Ref<ArrayMesh>());
	const int frames = CLAMP(p_frames, 2, 32);
	const int size = CLAMP(p_frame_size, 8, 512);
	constexpr int SS = 4; // Samples per texel side.
	const int hi = size * SS;

	LocalVector<BakeSurface> surfaces;
	for (int i = 0; i < p_mesh->get_surface_count(); i++) {
		const Array arrays = p_mesh->surface_get_arrays(i);
		BakeSurface s;
		s.verts = arrays[Mesh::ARRAY_VERTEX];
		s.normals = arrays[Mesh::ARRAY_NORMAL];
		s.colors = arrays[Mesh::ARRAY_COLOR];
		s.uvs = arrays[Mesh::ARRAY_TEX_UV];
		s.indices = arrays[Mesh::ARRAY_INDEX];
		if (s.verts.is_empty()) {
			continue;
		}
		if (s.indices.is_empty()) {
			s.indices.resize(s.verts.size());
			for (int k = 0; k < s.verts.size(); k++) {
				s.indices.write[k] = k;
			}
		}
		const Ref<ShaderMaterial> mat = p_mesh->surface_get_material(i);
		s.base = to_linear(param_color(mat, SNAME("base_color"), Color(1, 1, 1)));
		s.back = to_linear(param_color(mat, SNAME("backlight_color"), Color(0, 0, 0)));
		s.dead = to_linear(param_color(mat, SNAME("dead_color"), Color(0.36f, 0.24f, 0.11f)));
		s.albedo = param_image(mat, SNAME("albedo_tex"));
		s.backlight = param_image(mat, SNAME("backlight_tex"));
		surfaces.push_back(s);
	}
	ERR_FAIL_COND_V(surfaces.is_empty(), Ref<ArrayMesh>());

	const AABB box = p_mesh->get_aabb();
	const Vector3 centre = box.get_center();
	const float radius = MAX(box.size.length() * 0.5f, 0.01f);

	const int atlas = frames * size;
	Vector<uint8_t> albedo_data;
	Vector<uint8_t> normal_data;
	albedo_data.resize(atlas * atlas * 4);
	normal_data.resize(atlas * atlas * 4);
	memset(albedo_data.ptrw(), 0, albedo_data.size());
	memset(normal_data.ptrw(), 0, normal_data.size());

	Vector<Color> rects; // Per frame: centre and half size in the frame's right and up axes.
	rects.resize(frames * frames);
	LocalVector<uint8_t> filled; // Texels with colour: covered, or bled into.
	filled.resize(atlas * atlas);
	memset(filled.ptr(), 0, filled.size());
	uint8_t *ad_ptr = albedo_data.ptrw();
	uint8_t *nd_ptr = normal_data.ptrw();
	Color *rect_ptr = rects.ptrw();
	// Each frame on its own worker: they write disjoint parts of the atlas.
	auto bake_frame = [&](uint32_t p_index) {
		const int fx = int(p_index) % frames;
		const int fy = int(p_index) / frames;
		LocalVector<BakeSample> samples;
		samples.resize(hi * hi);
		// Frame directions at the cells' centres, so the lowest ring looks a little down on the
		// plant rather than exactly edge-on (where flat leaves vanish).
		const Vector3 d = hemi_decode((Vector2(float(fx), float(fy)) + Vector2(0.5f, 0.5f)) / float(frames) * 2.0f - Vector2(1, 1));
		Vector3 right;
		Vector3 up;
		frame_basis(d, right, up);
		for (BakeSample &b : samples) {
			b = BakeSample();
		}
		// Orthographic, looking along -d; the frame fits the plant as seen from d (a texel of
		// margin), so a low wide plant fills its side views.
		Vector2 lo(1e30f, 1e30f);
		Vector2 hi_b(-1e30f, -1e30f);
		for (const BakeSurface &s : surfaces) {
			for (int k = 0; k < s.verts.size(); k++) {
				const Vector3 q = s.verts[k] - centre;
				const Vector2 e(q.dot(right), q.dot(up));
				lo = lo.min(e);
				hi_b = hi_b.max(e);
			}
		}
		Vector2 half = (hi_b - lo) * 0.5f;
		half = half.max(Vector2(radius, radius) * 0.02f) * (1.0f + 2.0f / size);
		const Vector2 mid = (hi_b + lo) * 0.5f;
		rect_ptr[fy * frames + fx] = Color(mid.x, mid.y, half.x, half.y);
		auto project = [&](const Vector3 &p_v) {
			const Vector3 q = p_v - centre;
			return Vector3(((q.dot(right) - mid.x) / half.x * 0.5f + 0.5f) * hi, (0.5f - (q.dot(up) - mid.y) / half.y * 0.5f) * hi, q.dot(d));
		};
		for (const BakeSurface &s : surfaces) {
			for (int t = 0; t + 2 < s.indices.size(); t += 3) {
				const int i0 = s.indices[t];
				const int i1 = s.indices[t + 1];
				const int i2 = s.indices[t + 2];
				const Vector3 a = project(s.verts[i0]);
				const Vector3 b = project(s.verts[i1]);
				const Vector3 c = project(s.verts[i2]);
				const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
				if (Math::abs(area) < 1e-12f) {
					continue;
				}
				const int x0 = MAX(int(Math::floor(MIN(a.x, MIN(b.x, c.x)))), 0);
				const int x1 = MIN(int(Math::ceil(MAX(a.x, MAX(b.x, c.x)))), hi - 1);
				const int y0 = MAX(int(Math::floor(MIN(a.y, MIN(b.y, c.y)))), 0);
				const int y1 = MIN(int(Math::ceil(MAX(a.y, MAX(b.y, c.y)))), hi - 1);
				for (int y = y0; y <= y1; y++) {
					for (int x = x0; x <= x1; x++) {
						const float px = x + 0.5f;
						const float py = y + 0.5f;
						const float w0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) / area;
						const float w1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) / area;
						const float w2 = 1.0f - w0 - w1;
						if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) {
							continue;
						}
						const float z = a.z * w0 + b.z * w1 + c.z * w2;
						BakeSample &out = samples[y * hi + x];
						if (z <= out.depth) {
							continue;
						}
						shade(s, Vector3(w0, w1, w2), i0, i1, i2, d, out);
						out.depth = z;
					}
				}
			}
		}
		// Down to the frame's texels: coverage, and the mean of the covered samples.
		for (int y = 0; y < size; y++) {
			for (int x = 0; x < size; x++) {
				Vector3 alb;
				Vector3 n;
				float back = 0.0f;
				float depth = 0.0f;
				int hits = 0;
				for (int sy = 0; sy < SS; sy++) {
					for (int sx = 0; sx < SS; sx++) {
						const BakeSample &b = samples[(y * SS + sy) * hi + x * SS + sx];
						if (b.depth > -1e29f) {
							alb += b.albedo;
							n += b.normal;
							back += b.back;
							depth += b.depth;
							hits++;
						}
					}
				}
				const int64_t o = (int64_t(fy * size + y) * atlas + fx * size + x) * 4;
				if (hits == 0) {
					continue;
				}
				filled[o / 4] = 1;
				alb /= float(hits);
				n = n.length_squared() > 1e-12f ? n.normalized() : d;
				const Vector2 on = oct_encode(n);
				uint8_t *ad = ad_ptr + o;
				ad[0] = uint8_t(Math::round(linear_to_srgb(alb.x) * 255.0f));
				ad[1] = uint8_t(Math::round(linear_to_srgb(alb.y) * 255.0f));
				ad[2] = uint8_t(Math::round(linear_to_srgb(alb.z) * 255.0f));
				// Coverage raised by half: a leaf narrower than a texel still passes the alpha test, as
				// the mesh's leaf cards grow with distance to keep their area.
				ad[3] = uint8_t(Math::round(MIN(float(hits) / float(SS * SS) * 2.0f, 1.0f) * 255.0f));
				uint8_t *nd = nd_ptr + o;
				nd[0] = uint8_t(Math::round(CLAMP(on.x, 0.0f, 1.0f) * 255.0f));
				nd[1] = uint8_t(Math::round(CLAMP(on.y, 0.0f, 1.0f) * 255.0f));
				nd[2] = uint8_t(Math::round(CLAMP(depth / float(hits) / radius * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f));
				nd[3] = uint8_t(Math::round(CLAMP(back / float(hits) * 0.5f, 0.0f, 1.0f) * 255.0f));
			}
		}
		// Bleed colour, normal and depth into the empty texels around the plant, inside the
		// frame, so filtering and mips do not pull in black.
		LocalVector<uint8_t> was;
		was.resize(size * size);
		for (int pass = 0; pass < 8; pass++) {
			for (int y = 0; y < size; y++) {
				for (int x = 0; x < size; x++) {
					was[y * size + x] = filled[int64_t(fy * size + y) * atlas + fx * size + x];
				}
			}
			for (int y = 0; y < size; y++) {
				for (int x = 0; x < size; x++) {
					const int64_t t = int64_t(fy * size + y) * atlas + fx * size + x;
					if (was[y * size + x]) {
						continue;
					}
					int sum[7] = {};
					int count = 0;
					for (int k = 0; k < 4; k++) {
						const int nx = x + (k == 0 ? -1 : (k == 1 ? 1 : 0));
						const int ny = y + (k == 2 ? -1 : (k == 3 ? 1 : 0));
						if (nx < 0 || ny < 0 || nx >= size || ny >= size || !was[ny * size + nx]) {
							continue;
						}
						const int64_t q = int64_t(fy * size + ny) * atlas + fx * size + nx;
						for (int ch = 0; ch < 3; ch++) {
							sum[ch] += ad_ptr[q * 4 + ch];
						}
						for (int ch = 0; ch < 4; ch++) {
							sum[3 + ch] += nd_ptr[q * 4 + ch];
						}
						count++;
					}
					if (count) {
						for (int ch = 0; ch < 3; ch++) {
							ad_ptr[t * 4 + ch] = uint8_t(sum[ch] / count);
						}
						for (int ch = 0; ch < 4; ch++) {
							nd_ptr[t * 4 + ch] = uint8_t(sum[3 + ch] / count);
						}
						filled[t] = 1;
					}
				}
			}
		}
	};
	WorkerThreadPool::GroupID group = WorkerThreadPool::get_singleton()->add_native_group_task(&call_frame<decltype(bake_frame)>, &bake_frame, frames * frames, -1, true, SNAME("Bake plant impostor"));
	WorkerThreadPool::get_singleton()->wait_for_group_task_completion(group);

	Ref<Image> albedo_img = Image::create_from_data(atlas, atlas, false, Image::FORMAT_RGBA8, albedo_data);
	Ref<Image> normal_img = Image::create_from_data(atlas, atlas, false, Image::FORMAT_RGBA8, normal_data);
	albedo_img->generate_mipmaps();
	normal_img->generate_mipmaps();
	keep_coverage(albedo_img, 0.5f);

	Ref<ShaderMaterial> mat;
	mat.instantiate();
	mat->set_shader(_impostor_shader());
	mat->set_shader_parameter("impostor_albedo", ImageTexture::create_from_image(albedo_img));
	mat->set_shader_parameter("impostor_normal", ImageTexture::create_from_image(normal_img));
	mat->set_shader_parameter("frames", float(frames));
	Ref<Image> rect_img = Image::create_empty(frames, frames, false, Image::FORMAT_RGBAF);
	for (int k = 0; k < frames * frames; k++) {
		rect_img->set_pixel(k % frames, k / frames, rects[k]);
	}
	mat->set_shader_parameter("frame_rects", ImageTexture::create_from_image(rect_img));
	mat->set_shader_parameter("centre", centre);
	mat->set_shader_parameter("radius", radius);
	// The source's wind response, so the far plant leans as the near one does.
	const Ref<ShaderMaterial> src = p_mesh->surface_get_material(0);
	if (src.is_valid()) {
		const Variant st = src->get_shader_parameter(SNAME("stiffness"));
		const Variant h = src->get_shader_parameter(SNAME("height"));
		const Variant r = src->get_shader_parameter(SNAME("roughness"));
		if (st.get_type() == Variant::FLOAT) {
			mat->set_shader_parameter("stiffness", st);
		}
		if (h.get_type() == Variant::FLOAT) {
			mat->set_shader_parameter("height", h);
		}
		if (r.get_type() == Variant::FLOAT) {
			mat->set_shader_parameter("roughness", r);
		}
	}

	// One quad; the vertex shader turns it to the view.
	PackedVector3Array verts = { Vector3(-1, -1, 0), Vector3(1, -1, 0), Vector3(1, 1, 0), Vector3(-1, 1, 0) };
	PackedVector3Array normals = { Vector3(0, 0, 1), Vector3(0, 0, 1), Vector3(0, 0, 1), Vector3(0, 0, 1) };
	PackedInt32Array indices = { 0, 2, 1, 0, 3, 2 };
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = verts;
	arrays[Mesh::ARRAY_NORMAL] = normals;
	arrays[Mesh::ARRAY_INDEX] = indices;
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	mesh->surface_set_material(0, mat);
	mesh->set_custom_aabb(AABB(centre - Vector3(radius, radius, radius), Vector3(radius, radius, radius) * 2.0f));
	mesh->set_meta(SNAME("impostor_of"), p_mesh);
	return mesh;
}

// The species keeps its meshes up to the range and draws impostors of them beyond it, through
// VegetationField3D's near/far cells (dithered crossfade; the path tracer gets the meshes).
void PlantGenerator::add_impostors(const Ref<Resource> &p_species, float p_range, int p_frames, int p_frame_size) {
	Ref<PlantSpecies> sp = p_species;
	ERR_FAIL_COND(sp.is_null());
	ERR_FAIL_COND_MSG(sp->get_near_meshes().size() > 0, "The species already has near meshes.");
	const TypedArray<Mesh> meshes = sp->get_meshes();
	TypedArray<Mesh> impostors;
	for (int i = 0; i < meshes.size(); i++) {
		const Ref<Mesh> m = meshes[i];
		impostors.push_back(m.is_valid() ? Ref<Mesh>(make_impostor(m, p_frames, p_frame_size)) : Ref<Mesh>());
	}
	sp->set_near_meshes(meshes);
	sp->set_meshes(impostors);
	sp->set_near_range(p_range);
}

Ref<Shader> PlantGenerator::impostor_shader;

Ref<Shader> PlantGenerator::_impostor_shader() {
	if (impostor_shader.is_valid()) {
		return impostor_shader;
	}
	String code = "shader_type spatial;\nrender_mode cull_disabled;\n\n";
	code += shader_common_code();
	code += R"(
uniform sampler2D impostor_albedo : source_color, filter_linear_mipmap, repeat_disable;
uniform sampler2D impostor_normal : filter_linear_mipmap, repeat_disable; // xy: octahedral normal, z: depth, w: back light.
uniform float frames = 8.0;
uniform sampler2D frame_rects : filter_nearest; // Per frame: centre and half size in its right and up axes.
uniform vec3 centre;
uniform float radius = 1.0;
uniform float roughness = 0.7;
uniform float stiffness = 1.0;
uniform float height = 0.3;
uniform bool depth_offset = true; // Off: the quad's own depth (cheaper, but it cuts into the ground).

varying vec2 v_uv0;
varying vec2 v_uv1;
varying vec2 v_uv2;
varying flat vec2 v_f0;
varying flat vec2 v_f1;
varying flat vec2 v_f2;
varying flat vec3 v_w;
varying float v_light;
varying flat vec3 v_d0;
varying flat vec3 v_d1;
varying flat vec3 v_d2;
varying vec3 v_world;
varying flat float v_scale;

vec3 hemi_decode(vec2 g) {
	vec2 p = vec2(g.x + g.y, g.x - g.y) * 0.5;
	return normalize(vec3(p.x, 1.0 - abs(p.x) - abs(p.y), p.y));
}

vec2 hemi_encode(vec3 d) {
	d.y = max(d.y, 0.0);
	vec2 p = d.xz / (abs(d.x) + d.y + abs(d.z));
	return vec2(p.x + p.y, p.x - p.y);
}

void frame_basis(vec3 d, out vec3 r, out vec3 u) {
	r = abs(d.y) > 0.999 ? vec3(1.0, 0.0, 0.0) : normalize(cross(vec3(0.0, 1.0, 0.0), d));
	u = cross(d, r);
}

// Where the view ray through a point of the quad crosses frame f's plane, in the frame's UV.
vec2 frame_uv(vec2 f, vec3 p, vec3 ray, out vec3 d) {
	d = hemi_decode((f + 0.5) / frames * 2.0 - 1.0);
	vec3 r;
	vec3 u;
	frame_basis(d, r, u);
	float dn = dot(ray, d);
	dn = abs(dn) < 1e-4 ? (dn < 0.0 ? -1e-4 : 1e-4) : dn;
	vec3 q = p + ray * (dot(centre - p, d) / dn) - centre;
	vec4 rect = texelFetch(frame_rects, ivec2(f), 0);
	return vec2(dot(q, r) - rect.x, rect.y - dot(q, u)) / rect.zw * 0.5 + 0.5;
}

vec3 oct_decode(vec2 e) {
	e = e * 2.0 - 1.0;
	vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
	float t = max(-n.z, 0.0);
	n.xy += vec2(n.x >= 0.0 ? -t : t, n.y >= 0.0 ? -t : t);
	return normalize(n);
}

void vertex() {
	mat3 to_local = inverse(mat3(MODEL_MATRIX));
	// Towards the viewer, in the plant's space: the camera, or the light's direction in an
	// orthographic shadow pass, so the shadow comes from the matching view.
	bool ortho = PROJECTION_MATRIX[3][3] == 1.0;
	vec3 cam_local = (inverse(MODEL_MATRIX) * vec4(INV_VIEW_MATRIX[3].xyz, 1.0)).xyz;
	vec3 to_view = ortho ? normalize(to_local * INV_VIEW_MATRIX[2].xyz) : normalize(cam_local - centre);
	vec3 r;
	vec3 u;
	vec3 view_d = normalize(vec3(to_view.x, max(to_view.y, 0.0), to_view.z) + vec3(0.0, 1e-4, 0.0));
	frame_basis(view_d, r, u);
	vec3 p = centre + (VERTEX.x * r + VERTEX.y * u) * radius;
	vec3 ray = ortho ? -to_view : normalize(p - cam_local);

	// The three nearest frames and their weights (Brucks).
	vec2 g = (hemi_encode(view_d) * 0.5 + 0.5) * frames - 0.5;
	vec2 base = clamp(floor(g), vec2(0.0), vec2(frames - 2.0));
	vec2 f = clamp(g - base, 0.0, 1.0);
	if (f.x + f.y <= 1.0) {
		v_f0 = base;
		v_f1 = base + vec2(1.0, 0.0);
		v_f2 = base + vec2(0.0, 1.0);
		v_w = vec3(1.0 - f.x - f.y, f.x, f.y);
	} else {
		v_f0 = base + vec2(1.0, 1.0);
		v_f1 = base + vec2(1.0, 0.0);
		v_f2 = base + vec2(0.0, 1.0);
		v_w = vec3(f.x + f.y - 1.0, 1.0 - f.y, 1.0 - f.x);
	}
	vec3 d0;
	vec3 d1;
	vec3 d2;
	v_uv0 = frame_uv(v_f0, p, ray, d0);
	v_uv1 = frame_uv(v_f1, p, ray, d1);
	v_uv2 = frame_uv(v_f2, p, ray, d2);
	v_d0 = d0;
	v_d1 = d1;
	v_d2 = d2;

	// The plant's mean lean in the wind, as its mesh leans (stiffer plants less).
	vec3 wind = to_local * plant_wind(MODEL_MATRIX[3].xyz, max(height * 0.66, 0.05));
	float resp = (grass_wind.z > 0.0 ? grass_wind.z : 0.12) / max(stiffness, 0.05);
	float ws = length(wind.xz);
	if (ws > 1e-4) {
		float ang = min(ws * resp, 1.1);
		p.xz += wind.xz / ws * sin(ang) * 0.5 * max(p.y, 0.0);
	}
	// Far distance, as the plant shader: thinned by rank, the survivors grown a little.
	v_scale = 1.0;
	v_light = vegetation_light_of(INSTANCE_CUSTOM.y);
	if (INSTANCE_CUSTOM.y > 0.0) {
		float range = floor(INSTANCE_CUSTOM.y);
		float dist = length(MODEL_MATRIX[3].xyz - MAIN_CAM_INV_VIEW_MATRIX[3].xyz);
		float keep = clamp((range - dist) / (range * 0.65), 0.0, 1.0);
		v_scale = smoothstep(INSTANCE_CUSTOM.x, INSTANCE_CUSTOM.x + 0.12, keep * 1.12) * (1.0 + 0.25 * (1.0 - keep));
		p *= v_scale;
	}
	VERTEX = p;
	NORMAL = to_view;
	v_world = (MODEL_MATRIX * vec4(p, 1.0)).xyz;
}

vec4 frame_tex(sampler2D tex, vec2 f, vec2 uv, vec2 dx, vec2 dy) {
	if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
		return vec4(0.0);
	}
	return textureGrad(tex, (f + uv) / frames, dx / frames, dy / frames);
}

void fragment() {
	// One set of derivatives for all three frames: they map the quad alike.
	vec2 dx = dFdx(v_uv0);
	vec2 dy = dFdy(v_uv0);
	vec4 a0 = frame_tex(impostor_albedo, v_f0, v_uv0, dx, dy);
	vec4 a1 = frame_tex(impostor_albedo, v_f1, v_uv1, dx, dy);
	vec4 a2 = frame_tex(impostor_albedo, v_f2, v_uv2, dx, dy);
	vec3 cw = v_w * vec3(a0.a, a1.a, a2.a);
	float cover = cw.x + cw.y + cw.z;
	vec3 wn = cw / max(cover, 1e-4);
	vec4 b0 = frame_tex(impostor_normal, v_f0, v_uv0, dx, dy);
	vec4 b1 = frame_tex(impostor_normal, v_f1, v_uv1, dx, dy);
	vec4 b2 = frame_tex(impostor_normal, v_f2, v_uv2, dx, dy);
	vec3 albedo = a0.rgb * wn.x + a1.rgb * wn.y + a2.rgb * wn.z;
	albedo = vegetation_light_tint(albedo, v_light);
	vec3 n_local = normalize(oct_decode(b0.xy) * wn.x + oct_decode(b1.xy) * wn.y + oct_decode(b2.xy) * wn.z + vec3(0.0, 1e-4, 0.0));
	float back = (b0.w * wn.x + b1.w * wn.y + b2.w * wn.z) * 2.0;
	// Pixel depth offset: the baked depth moves the fragment to the plant's surface, so the quad
	// meets the ground and the grass as the mesh would.
	float depth_local = ((b0.z * wn.x + b1.z * wn.y + b2.z * wn.z) * 2.0 - 1.0) * radius * v_scale * length(MODEL_MATRIX[0].xyz);
	// The shadow caster sits a little behind the surface it rebuilds from the light's frames, so
	// the camera's frames (which rebuild it slightly differently) do not shadow themselves.
	if (IN_SHADOW_PASS) {
		depth_local -= radius * 0.08 * v_scale * length(MODEL_MATRIX[0].xyz);
	}
	bool ortho = PROJECTION_MATRIX[3][3] == 1.0;
	vec3 ray = ortho ? vec3(0.0, 0.0, -1.0) : normalize(VERTEX);
	vec3 d_view = normalize((VIEW_MATRIX * vec4(mat3(MODEL_MATRIX) * (v_d0 * wn.x + v_d1 * wn.y + v_d2 * wn.z), 0.0)).xyz);
	float along = max(dot(-ray, d_view), 0.2);
	vec3 surface = VERTEX - ray * depth_local / along;
	vec4 clip = PROJECTION_MATRIX * vec4(surface, 1.0);
	DEPTH = depth_offset ? clip.z / clip.w : FRAGCOORD.z;
	// Light and shadows at the plant's surface too, not at the quad (which lies inside the plant
	// and would be in its own shadow).
	LIGHT_VERTEX = depth_offset ? surface : VERTEX;

	vec3 n_world = normalize(mat3(MODEL_MATRIX) * n_local);
	NORMAL = normalize((VIEW_MATRIX * vec4(n_world, 0.0)).xyz);
	float rough = roughness;
	float metallic = 0.0;
	float specular = 0.35;
	weather_wetness_apply(weather_wetness_at(v_world), n_world.y, metallic, 0.15, albedo, rough);
	vec4 snow_s = snow_cover_at(v_world);
	float snow = snow_cover_weight(snow_s.x, 60.0, n_world, n_world);
	snow_cover_apply(snow, snow_s.y, snow_s.z, albedo, rough, metallic, specular);
	ALBEDO = albedo;
	ALPHA = cover;
	ALPHA_SCISSOR_THRESHOLD = 0.5;
	ROUGHNESS = rough;
	METALLIC = metallic;
	SPECULAR = specular;
	BACKLIGHT = albedo * back * (1.0 - snow);
}
)";
	impostor_shader.instantiate();
	impostor_shader->set_code(code);
	return impostor_shader;
}

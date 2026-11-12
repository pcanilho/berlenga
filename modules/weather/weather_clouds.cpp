/**************************************************************************/
/*  weather_clouds.cpp                                                    */
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

#include "weather_clouds.h"

#include "core/io/image.h"
#include "core/math/math_funcs.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/mutex.h"
#include "scene/resources/image_texture.h"

namespace WeatherClouds {

namespace {

uint32_t hash_u(uint32_t p_h) {
	p_h ^= p_h >> 13;
	p_h *= 0x5bd1e995u;
	p_h ^= p_h >> 15;
	return p_h;
}

float smoothstep(float p_a, float p_b, float p_x) {
	const float t = CLAMP((p_x - p_a) / (p_b - p_a), 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

// --- Tiling noise for the shape textures (made once). -------------------------------------------------

uint32_t hash3(int p_x, int p_y, int p_z) {
	return hash_u(uint32_t(p_x) * 0x8da6b343u ^ uint32_t(p_y) * 0xd8163841u ^ uint32_t(p_z) * 0xcb1ab31fu);
}

int wrap(int p_i, int p_period) {
	const int m = p_i % p_period;
	return m < 0 ? m + p_period : m;
}

// Perlin's improved gradient noise (2002), lattice wrapped every p_period cells; about -1..1.
float perlin(const Vector3 &p_p, int p_period) {
	static const float G[12][3] = { { 1, 1, 0 }, { -1, 1, 0 }, { 1, -1, 0 }, { -1, -1, 0 }, { 1, 0, 1 }, { -1, 0, 1 }, { 1, 0, -1 }, { -1, 0, -1 }, { 0, 1, 1 }, { 0, -1, 1 }, { 0, 1, -1 }, { 0, -1, -1 } };
	const int ix = int(Math::floor(p_p.x));
	const int iy = int(Math::floor(p_p.y));
	const int iz = int(Math::floor(p_p.z));
	const Vector3 f = p_p - Vector3(ix, iy, iz);
	auto fade = [](float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); };
	const Vector3 u(fade(f.x), fade(f.y), fade(f.z));
	float c[2][2][2];
	for (int dz = 0; dz < 2; dz++) {
		for (int dy = 0; dy < 2; dy++) {
			for (int dx = 0; dx < 2; dx++) {
				const float *g = G[hash3(wrap(ix + dx, p_period), wrap(iy + dy, p_period), wrap(iz + dz, p_period)) % 12];
				c[dz][dy][dx] = g[0] * (f.x - dx) + g[1] * (f.y - dy) + g[2] * (f.z - dz);
			}
		}
	}
	auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
	const float y0 = lerp(lerp(c[0][0][0], c[0][0][1], u.x), lerp(c[0][1][0], c[0][1][1], u.x), u.y);
	const float y1 = lerp(lerp(c[1][0][0], c[1][0][1], u.x), lerp(c[1][1][0], c[1][1][1], u.x), u.y);
	return lerp(y0, y1, u.z);
}

// Worley cellular noise (1996), one feature point a cell, wrapped every p_period cells, inverted
// (1 at the points) so it makes the billows Nubis builds clouds from.
float worley(const Vector3 &p_p, int p_period) {
	const int ix = int(Math::floor(p_p.x));
	const int iy = int(Math::floor(p_p.y));
	const int iz = int(Math::floor(p_p.z));
	float best = 1e9f;
	for (int dz = -1; dz <= 1; dz++) {
		for (int dy = -1; dy <= 1; dy++) {
			for (int dx = -1; dx <= 1; dx++) {
				const uint32_t h = hash3(wrap(ix + dx, p_period), wrap(iy + dy, p_period), wrap(iz + dz, p_period));
				const Vector3 feature = Vector3(ix + dx, iy + dy, iz + dz) + Vector3((h & 1023u) / 1023.0f, ((h >> 10) & 1023u) / 1023.0f, ((h >> 20) & 1023u) / 1023.0f);
				best = MIN(best, (feature - p_p).length_squared());
			}
		}
	}
	return 1.0f - CLAMP(Math::sqrt(best), 0.0f, 1.0f);
}

float remap(float p_v, float p_lo, float p_hi, float p_nlo, float p_nhi) {
	return p_nlo + (p_v - p_lo) * (p_nhi - p_nlo) / (p_hi - p_lo);
}

struct NoiseJob {
	int size = 0;
	bool detail = false;
	float *out = nullptr;

	void slice(uint32_t p_z, void *) {
		for (int y = 0; y < size; y++) {
			for (int x = 0; x < size; x++) {
				const Vector3 u = (Vector3(x, y, int(p_z)) + Vector3(0.5f, 0.5f, 0.5f)) / float(size);
				float v;
				if (detail) {
					// Worley at three frequencies, as Nubis's detail texture.
					v = worley(u * 4.0f, 4) * 0.625f + worley(u * 8.0f, 8) * 0.25f + worley(u * 16.0f, 16) * 0.125f;
				} else {
					// Perlin-Worley (Schneider 2015): Perlin fbm dilated by Worley, then carved by a Worley fbm.
					float pf = 0.0f;
					float amp = 1.0f;
					float norm = 0.0f;
					for (int o = 0; o < 4; o++) {
						const int period = 4 << o;
						pf += amp * perlin(u * float(period), period);
						norm += amp;
						amp *= 0.5f;
					}
					pf = CLAMP(pf / norm * 0.5f + 0.5f, 0.0f, 1.0f);
					const float w1 = worley(u * 4.0f, 4);
					const float w2 = worley(u * 8.0f, 8);
					const float w3 = worley(u * 16.0f, 16);
					const float wf = w1 * 0.625f + w2 * 0.25f + w3 * 0.125f;
					const float pw = CLAMP(remap(pf, wf - 1.0f, 1.0f, 0.0f, 1.0f), 0.0f, 1.0f);
					const float carve = w2 * 0.625f + w3 * 0.25f + worley(u * 32.0f, 32) * 0.125f;
					v = CLAMP(remap(pw, -(1.0f - carve), 1.0f, 0.0f, 1.0f), 0.0f, 1.0f);
				}
				out[(int(p_z) * size + y) * size + x] = v;
			}
		}
	}
};

Mutex noise_mutex;
Vector<float> shape_data;
Vector<float> detail_data;
Ref<ImageTexture3D> shape_tex;
Ref<ImageTexture3D> detail_tex;

Vector<float> make_noise(int p_size, bool p_detail) {
	Vector<float> data;
	data.resize(p_size * p_size * p_size);
	NoiseJob job;
	job.size = p_size;
	job.detail = p_detail;
	job.out = data.ptrw();
	const WorkerThreadPool::GroupID g = WorkerThreadPool::get_singleton()->add_template_group_task(&job, &NoiseJob::slice, (void *)nullptr, p_size, -1, true, String("Cloud noise"));
	WorkerThreadPool::get_singleton()->wait_for_group_task_completion(g);
	// Equalise to an even spread over 0..1 (rank over count), so a cover threshold c keeps the share c
	// of the noise: cover 0.4 then clouds about 40% of the sky.
	struct Ranked {
		float v;
		int i;
		bool operator<(const Ranked &p_other) const { return v < p_other.v; }
	};
	Vector<Ranked> ranked;
	ranked.resize(data.size());
	for (int i = 0; i < data.size(); i++) {
		ranked.write[i] = { data[i], i };
	}
	ranked.sort();
	float *w = data.ptrw();
	for (int r = 0; r < ranked.size(); r++) {
		w[ranked[r].i] = float(r) / float(ranked.size() - 1);
	}
	return data;
}

// With box-filtered mipmaps, so the sky can read far clouds at the scale of its steps.
Ref<ImageTexture3D> make_texture(const Vector<float> &p_data, int p_size) {
	Vector<Ref<Image>> slices;
	Vector<float> level = p_data;
	int size = p_size;
	while (size >= 1) {
		for (int z = 0; z < size; z++) {
			Vector<uint8_t> bytes;
			bytes.resize(size * size);
			for (int i = 0; i < size * size; i++) {
				bytes.write[i] = uint8_t(CLAMP(int(level[z * size * size + i] * 255.0f + 0.5f), 0, 255));
			}
			slices.push_back(Image::create_from_data(size, size, false, Image::FORMAT_R8, bytes));
		}
		if (size == 1) {
			break;
		}
		const int half = size / 2;
		Vector<float> next;
		next.resize(half * half * half);
		for (int z = 0; z < half; z++) {
			for (int y = 0; y < half; y++) {
				for (int x = 0; x < half; x++) {
					float s = 0.0f;
					for (int k = 0; k < 8; k++) {
						s += level[((2 * z + (k >> 2)) * size + 2 * y + ((k >> 1) & 1)) * size + 2 * x + (k & 1)];
					}
					next.write[(z * half + y) * half + x] = s * 0.125f;
				}
			}
		}
		level = next;
		size = half;
	}
	Ref<ImageTexture3D> tex;
	tex.instantiate();
	tex->create(Image::FORMAT_R8, p_size, p_size, p_size, true, slices);
	return tex;
}

// Trilinear, wrapped, as the sky's repeat sampler.
float sample_tiled(const Vector<float> &p_data, int p_size, const Vector3 &p_uvw) {
	const Vector3 p = p_uvw * float(p_size) - Vector3(0.5f, 0.5f, 0.5f);
	const int x0 = int(Math::floor(p.x));
	const int y0 = int(Math::floor(p.y));
	const int z0 = int(Math::floor(p.z));
	const Vector3 t = p - Vector3(x0, y0, z0);
	const float *d = p_data.ptr();
	auto at = [&](int x, int y, int z) { return d[(wrap(z, p_size) * p_size + wrap(y, p_size)) * p_size + wrap(x, p_size)]; };
	auto lerp = [](float a, float b, float s) { return a + (b - a) * s; };
	const float c00 = lerp(at(x0, y0, z0), at(x0 + 1, y0, z0), t.x);
	const float c10 = lerp(at(x0, y0 + 1, z0), at(x0 + 1, y0 + 1, z0), t.x);
	const float c01 = lerp(at(x0, y0, z0 + 1), at(x0 + 1, y0, z0 + 1), t.x);
	const float c11 = lerp(at(x0, y0 + 1, z0 + 1), at(x0 + 1, y0 + 1, z0 + 1), t.x);
	return lerp(lerp(c00, c10, t.y), lerp(c01, c11, t.y), t.z);
}

} // namespace

// --- The cover noise of the weather map (not tiling: a hashed lattice). ----------------------------

float hash2(int p_x, int p_y) {
	return float(hash_u(uint32_t(p_x) * 0x8da6b343u ^ uint32_t(p_y) * 0xd8163841u) & 0xffffffu) / float(0xffffff);
}

float value_noise(Vector2 p_p) {
	const float fx = Math::floor(p_p.x);
	const float fy = Math::floor(p_p.y);
	const int ix = int(fx);
	const int iy = int(fy);
	float tx = p_p.x - fx;
	float ty = p_p.y - fy;
	tx = tx * tx * (3.0f - 2.0f * tx);
	ty = ty * ty * (3.0f - 2.0f * ty);
	const float a = hash2(ix, iy);
	const float b = hash2(ix + 1, iy);
	const float c = hash2(ix, iy + 1);
	const float d = hash2(ix + 1, iy + 1);
	return Math::lerp(Math::lerp(a, b, tx), Math::lerp(c, d, tx), ty);
}

// Quilez's fbm rotation (36.87 degrees, an angle whose multiples never line up again): each octave's
// lattice turned against the last and against the world's axes, so no grid shows through.
Vector2 rotate_lattice(Vector2 p_p) {
	return Vector2(0.8f * p_p.x - 0.6f * p_p.y, 0.6f * p_p.x + 0.8f * p_p.y);
}

float cover_noise(Vector2 p_xz, Vector2 p_drift, float p_scale) {
	const Vector2 p = (p_xz - p_drift) / MAX(p_scale, 1.0f);
	float v = 0.0f;
	float amp = 0.5f;
	float freq = 1.0f;
	float norm = 0.0f;
	Vector2 q = rotate_lattice(p);
	for (int o = 0; o < 4; o++) {
		v += amp * value_noise(q + Vector2(17.1f * o, 31.7f * o));
		norm += amp;
		amp *= 0.5f;
		q = rotate_lattice(q) * 2.03f;
	}
	return v / norm;
}

// --- The density. ---------------------------------------------------------------------------------

float top_height(const State &p_state, float p_type) {
	return p_state.thickness * Math::lerp(0.25f, 1.0f, smoothstep(0.0f, 0.7f, p_type)) * (1.0f + 4.0f * smoothstep(0.8f, 1.0f, p_type));
}

void State::update_top_max() {
	float t = type;
	for (int i = 0; i < storm_count; i++) {
		t = MAX(t, 0.9f * storms[i][3]);
	}
	top_max = base_height + top_height(*this, t);
}

Vector2 cover_type_at(const State &p_state, Vector2 p_xz) {
	float cover = p_state.cover;
	float t = p_state.type;
	if (p_state.variation > 0.0f) {
		cover = CLAMP(cover + (cover_noise(p_xz, p_state.drift, p_state.cover_scale) - 0.5f) * 2.0f * p_state.variation, 0.0f, 1.0f);
	}
	for (int i = 0; i < p_state.storm_count; i++) {
		const float *s = p_state.storms[i];
		const float dx = p_xz.x - s[0];
		const float dz = p_xz.y - s[1];
		const float x2 = (dx * dx + dz * dz) / (s[2] * s[2]);
		const float wide = Math::exp(-x2 / (1.6f * 1.6f)) * s[3];
		cover = 1.0f - (1.0f - cover) * (1.0f - wide);
		t = MAX(t, 0.9f * wide);
	}
	return Vector2(cover, t);
}

// The shape noise is stretched twice vertically, so a column through the layer sees about one
// value of it: the share of sky the clouds cover then follows the cover setting, and the height
// profile, not the noise, shapes bases and tops.
//
// The shape noise is one tile, shape_scale wide, so read once it brings the same clouds back every
// tile, in a grid seen to the horizon. It is read twice instead: once turned 36.87 degrees against
// the world's axes, once turned twice that and 1.618 times larger. The two tiles' periods have an
// irrational ratio, so their sum never repeats (the aperiodic tiling idea of Wang tiles and of
// Quilez's "texture repetition", with two lookups). The sum of two even values is triangular; its
// CDF evens it out again, so cover still means share of sky.
constexpr float SHAPE_SECOND = 0.618034f; // the second read's frequency against the first

Vector2 rotate_shape_a(Vector2 p_p) {
	return rotate_lattice(p_p);
}

Vector2 rotate_shape_b(Vector2 p_p) {
	return rotate_lattice(rotate_lattice(p_p)) * SHAPE_SECOND + Vector2(0.37f, 0.71f);
}

float even_sum(float p_a, float p_b) {
	const float s = p_a + p_b;
	return s < 1.0f ? 0.5f * s * s : 1.0f - 0.5f * (2.0f - s) * (2.0f - s);
}

Vector3 shape_coord(const State &p_state, const Vector3 &p_pos) {
	const Vector2 xz = rotate_shape_a(Vector2(p_pos.x - p_state.drift.x, p_pos.z - p_state.drift.y) / p_state.shape_scale);
	return Vector3(xz.x, p_pos.y * 0.7f / p_state.shape_scale, xz.y);
}

float shape_at(const State &p_state, const Vector3 &p_pos) {
	const Vector2 p = Vector2(p_pos.x - p_state.drift.x, p_pos.z - p_state.drift.y) / p_state.shape_scale;
	const float y = p_pos.y * 0.7f / p_state.shape_scale;
	const Vector2 a = rotate_shape_a(p);
	const Vector2 b = rotate_shape_b(p);
	const Vector<float> &noise = shape_noise();
	return even_sum(sample_tiled(noise, SHAPE_SIZE, Vector3(a.x, y, a.y)), sample_tiled(noise, SHAPE_SIZE, Vector3(b.x, y * SHAPE_SECOND + 0.53f, b.y)));
}

float extinction_at(const State &p_state, const Vector3 &p_pos) {
	if (p_pos.y < p_state.base_height || p_pos.y > p_state.top_max) {
		return 0.0f;
	}
	const Vector2 ct = cover_type_at(p_state, Vector2(p_pos.x, p_pos.z));
	if (ct.x <= 0.001f) {
		return 0.0f;
	}
	const float h = (p_pos.y - p_state.base_height) / top_height(p_state, ct.y);
	if (h >= 1.0f) {
		return 0.0f;
	}
	const float cu = smoothstep(0.2f, 0.8f, ct.y);
	const float profile = smoothstep(0.0f, Math::lerp(0.2f, 0.05f, cu), h) * (1.0f - smoothstep(1.0f - Math::lerp(0.15f, 0.6f, cu), 1.0f, h));
	const float n = shape_at(p_state, p_pos);
	// Nubis's coverage remap, remap(n p, 1 - c, 1, 0, 1): where the noise is high the cloud reaches
	// further up the profile, which domes the tops.
	const float shape = MAX(n * profile - (1.0f - ct.x), 0.0f) / MAX(ct.x, 0.001f);
	// Frostbite's measured extinction: stratus about 0.04, cumulus up to 0.1 per metre.
	return shape * Math::lerp(0.04f, 0.1f, ct.y) * p_state.density;
}

const Vector<float> &shape_noise() {
	MutexLock lock(noise_mutex);
	if (shape_data.is_empty()) {
		shape_data = make_noise(SHAPE_SIZE, false);
	}
	return shape_data;
}

const Vector<float> &detail_noise() {
	MutexLock lock(noise_mutex);
	if (detail_data.is_empty()) {
		detail_data = make_noise(DETAIL_SIZE, true);
	}
	return detail_data;
}

Ref<Texture3D> shape_texture() {
	const Vector<float> &data = shape_noise();
	MutexLock lock(noise_mutex);
	if (shape_tex.is_null()) {
		shape_tex = make_texture(data, SHAPE_SIZE);
	}
	return shape_tex;
}

Ref<Texture3D> detail_texture() {
	const Vector<float> &data = detail_noise();
	MutexLock lock(noise_mutex);
	if (detail_tex.is_null()) {
		detail_tex = make_texture(data, DETAIL_SIZE);
	}
	return detail_tex;
}

void free_textures() {
	MutexLock lock(noise_mutex);
	shape_tex.unref();
	detail_tex.unref();
}

// The functions above in the sky's shading language. Keep them in step.
const char *GLSL = R"(
uniform sampler3D cloud_shape : filter_linear_mipmap, repeat_enable;
uniform sampler3D cloud_detail : filter_linear_mipmap, repeat_enable;
uniform float cloud_base_height = 1500.0;
uniform float cloud_thickness = 1500.0;
uniform float cloud_top_max = 2875.0;
uniform float cloud_density = 1.0;
uniform float cloud_cover = 0.4;
uniform float cloud_type = 0.3;
uniform float cloud_variation = 0.35;
uniform float cloud_cover_scale = 6000.0;
uniform float cloud_shape_scale = 3000.0;
uniform vec2 cloud_drift = vec2(0.0);
uniform int cloud_storm_count = 0;
uniform vec4 cloud_storms[8];

float wc_hash2(int x, int y) {
	uint h = uint(x) * 2376512323u ^ uint(y) * 3625334849u;
	h ^= h >> 13u;
	h *= 1540483477u;
	h ^= h >> 15u;
	return float(h & 16777215u) / 16777215.0;
}

float wc_value_noise(vec2 p) {
	vec2 f = floor(p);
	ivec2 i = ivec2(f);
	vec2 t = p - f;
	t = t * t * (3.0 - 2.0 * t);
	float a = wc_hash2(i.x, i.y);
	float b = wc_hash2(i.x + 1, i.y);
	float c = wc_hash2(i.x, i.y + 1);
	float d = wc_hash2(i.x + 1, i.y + 1);
	return mix(mix(a, b, t.x), mix(c, d, t.x), t.y);
}

vec2 wc_rotate(vec2 p) {
	return vec2(0.8 * p.x - 0.6 * p.y, 0.6 * p.x + 0.8 * p.y);
}

float wc_cover_noise(vec2 xz) {
	vec2 p = (xz - cloud_drift) / max(cloud_cover_scale, 1.0);
	float v = 0.0;
	float amp = 0.5;
	float norm = 0.0;
	vec2 q = wc_rotate(p);
	for (int o = 0; o < 4; o++) {
		v += amp * wc_value_noise(q + vec2(17.1, 31.7) * float(o));
		norm += amp;
		amp *= 0.5;
		q = wc_rotate(q) * 2.03;
	}
	return v / norm;
}

// The shape noise read twice, turned and at two scales with an irrational ratio, evened out again
// (shape_at() on the CPU): no tile repeats.
float wc_shape(vec3 p, float p_step) {
	vec2 xz = (p.xz - cloud_drift) / cloud_shape_scale;
	// Stretched 1.4 times in height: towers, but not so tall that a cloud is one column through the
	// whole layer, cut flat at its base and top (slabs with vertical walls).
	float y = p.y * 0.7 / cloud_shape_scale;
	vec2 a = wc_rotate(xz);
	vec2 b = wc_rotate(wc_rotate(xz)) * 0.618034 + vec2(0.37, 0.71);
	float lod = log2(p_step / (cloud_shape_scale / 128.0));
	float na = textureLod(cloud_shape, vec3(a.x, y, a.y), max(lod, 0.0)).r;
	float nb = textureLod(cloud_shape, vec3(b.x, y * 0.618034 + 0.53, b.y), max(lod + log2(0.618034), 0.0)).r;
	float s = na + nb;
	return s < 1.0 ? 0.5 * s * s : 1.0 - 0.5 * (2.0 - s) * (2.0 - s);
}

float wc_top_height(float t) {
	return cloud_thickness * mix(0.25, 1.0, smoothstep(0.0, 0.7, t)) * (1.0 + 4.0 * smoothstep(0.8, 1.0, t));
}

vec2 wc_cover_type(vec2 xz) {
	float cover = cloud_cover;
	float t = cloud_type;
	if (cloud_variation > 0.0) {
		cover = clamp(cover + (wc_cover_noise(xz) - 0.5) * 2.0 * cloud_variation, 0.0, 1.0);
	}
	for (int i = 0; i < 8; i++) {
		if (i >= cloud_storm_count) {
			break;
		}
		vec4 s = cloud_storms[i];
		vec2 d = xz - s.xy;
		float x2 = dot(d, d) / (s.z * s.z);
		float wide = exp(-x2 / (1.6 * 1.6)) * s.w;
		cover = 1.0 - (1.0 - cover) * (1.0 - wide);
		t = max(t, 0.9 * wide);
	}
	return vec2(cover, t);
}

// Extinction (1/m) at p. p_detail erodes the edges with the detail noise (Nubis); the shadow bake
// and the CPU leave it out. p_step is the march's step (m): the noise is read at that scale.
float wc_extinction(vec3 p, vec2 ct, bool p_detail, float p_step) {
	if (ct.x <= 0.001) {
		return 0.0;
	}
	float h = (p.y - cloud_base_height) / wc_top_height(ct.y);
	if (h <= 0.0 || h >= 1.0) {
		return 0.0;
	}
	float cu = smoothstep(0.2, 0.8, ct.y);
	float profile = smoothstep(0.0, mix(0.2, 0.05, cu), h) * (1.0 - smoothstep(1.0 - mix(0.15, 0.6, cu), 1.0, h));
	float texel = cloud_shape_scale / 128.0;
	float n = wc_shape(p, p_step);
	float shape = max(n * profile - (1.0 - ct.x), 0.0) / max(ct.x, 0.001);
	if (p_detail && shape > 0.0) {
		// Erode the edges with the detail noise, wispy below and billowing higher up (Nubis 2017).
		vec2 sxz = wc_rotate(p.xz - cloud_drift);
		vec3 qd = vec3(sxz.x, p.y, sxz.y) / cloud_shape_scale * 8.0;
		float d = textureLod(cloud_detail, qd, max(log2(p_step / (texel * 0.25)), 0.0)).r;
		float e = 0.45 * mix(d, 1.0 - d, clamp(h * 4.0, 0.0, 1.0));
		shape = clamp((shape - e) / (1.0 - e), 0.0, 1.0);
	}
	return shape * mix(0.04, 0.1, ct.y) * cloud_density;
}
)";

} // namespace WeatherClouds

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

// Alligator noise (Side Effects' anoise, which Nubis Cubed builds its wisps from): every cell's feature point
// has a random strength that falls off smoothly over one cell; the strongest contribution minus the second
// leaves scalloped ridges between the features. Wrapped every p_period cells, 0..1.
float alligator(const Vector3 &p_p, int p_period) {
	const int ix = int(Math::floor(p_p.x));
	const int iy = int(Math::floor(p_p.y));
	const int iz = int(Math::floor(p_p.z));
	float first = 0.0f;
	float second = 0.0f;
	for (int dz = -1; dz <= 1; dz++) {
		for (int dy = -1; dy <= 1; dy++) {
			for (int dx = -1; dx <= 1; dx++) {
				const uint32_t h = hash3(wrap(ix + dx, p_period), wrap(iy + dy, p_period), wrap(iz + dz, p_period));
				const Vector3 feature = Vector3(ix + dx, iy + dy, iz + dz) + Vector3((h & 1023u) / 1023.0f, ((h >> 10) & 1023u) / 1023.0f, ((h >> 20) & 1023u) / 1023.0f);
				const float t = CLAMP(1.0f - (feature - p_p).length(), 0.0f, 1.0f);
				const float c = (hash_u(h ^ 0x68e31da4u) & 1023u) / 1023.0f * t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
				if (c > first) {
					second = first;
					first = c;
				} else if (c > second) {
					second = c;
				}
			}
		}
	}
	return first - second;
}

// Curl noise (Bridson, Hourihan and Nordenstam, "Curl-Noise for Procedural Fluid Flow", SIGGRAPH 2007): the
// curl of three Perlin potentials, a divergence-free field that turns the alligator scallops into the curling
// wisps Nubis Cubed calls Curly-Alligator. Wrapped every p_period cells; about -1..1 a component.
Vector3 curl(const Vector3 &p_p, int p_period) {
	const float e = 0.02f;
	auto pot = [&](const Vector3 &q, int k) { return perlin(q + Vector3(17.3f, 41.9f, 7.1f) * float(k), p_period); };
	auto d = [&](int k, int axis) {
		Vector3 o;
		o[axis] = e;
		return (pot(p_p + o, k) - pot(p_p - o, k)) / (2.0f * e);
	};
	// curl (A, B, C) = (dC/dy - dB/dz, dA/dz - dC/dx, dB/dx - dA/dy)
	return Vector3(d(2, 1) - d(1, 2), d(0, 2) - d(2, 0), d(1, 0) - d(0, 1));
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

// The detail texture of Nubis Cubed (Schneider, Advances 2023): four channels, low and high frequency
// Curly-Alligator wisps (r, g) and low and high frequency billows (b, a, inverted Worley), each tiling.
// The sky blends low to high frequency over the depth into the cloud and wisps to billows over height.
struct DetailJob {
	int size = 0;
	float *out = nullptr; // 4 floats a texel

	void slice(uint32_t p_z, void *) {
		for (int y = 0; y < size; y++) {
			for (int x = 0; x < size; x++) {
				const Vector3 u = (Vector3(x, y, int(p_z)) + Vector3(0.5f, 0.5f, 0.5f)) / float(size);
				// One curl field for both wisp channels, so the fine wisps curl with the coarse ones.
				const Vector3 c = curl(u * 4.0f, 4) * 0.035f;
				const Vector3 w = u + c;
				float *o = out + ((int(p_z) * size + y) * size + x) * 4;
				o[0] = alligator(w * 4.0f, 4) * 0.625f + alligator(w * 8.0f, 8) * 0.25f + alligator(w * 16.0f, 16) * 0.125f;
				o[1] = alligator(w * 12.0f, 12) * 0.625f + alligator(w * 24.0f, 24) * 0.25f + alligator(w * 48.0f, 48) * 0.125f;
				o[2] = worley(u * 4.0f, 4) * 0.625f + worley(u * 8.0f, 8) * 0.25f + worley(u * 16.0f, 16) * 0.125f;
				o[3] = worley(u * 12.0f, 12) * 0.625f + worley(u * 24.0f, 24) * 0.25f + worley(u * 48.0f, 48) * 0.125f;
			}
		}
	}
};

Mutex noise_mutex;
Vector<float> shape_data;
Vector<float> detail_data;
Ref<ImageTexture3D> shape_tex;
Ref<ImageTexture3D> detail_tex;

// Equalise every channel to an even spread over 0..1 (rank over count), so a cover threshold c keeps the
// share c of the noise: cover 0.4 then clouds about 40% of the sky.
void equalise(Vector<float> &r_data, int p_channels) {
	struct Ranked {
		float v;
		int i;
		bool operator<(const Ranked &p_other) const { return v < p_other.v; }
	};
	const int n = r_data.size() / p_channels;
	Vector<Ranked> ranked;
	ranked.resize(n);
	float *w = r_data.ptrw();
	for (int c = 0; c < p_channels; c++) {
		for (int i = 0; i < n; i++) {
			ranked.write[i] = { r_data[i * p_channels + c], i };
		}
		ranked.sort();
		for (int r = 0; r < n; r++) {
			w[ranked[r].i * p_channels + c] = float(r) / float(n - 1);
		}
	}
}

Vector<float> make_noise(int p_size, bool p_detail) {
	Vector<float> data;
	data.resize(p_size * p_size * p_size);
	NoiseJob job;
	job.size = p_size;
	job.detail = p_detail;
	job.out = data.ptrw();
	const WorkerThreadPool::GroupID g = WorkerThreadPool::get_singleton()->add_template_group_task(&job, &NoiseJob::slice, (void *)nullptr, p_size, -1, true, String("Cloud noise"));
	WorkerThreadPool::get_singleton()->wait_for_group_task_completion(g);
	equalise(data, 1);
	return data;
}

Vector<float> make_detail(int p_size) {
	Vector<float> data;
	data.resize(p_size * p_size * p_size * 4);
	DetailJob job;
	job.size = p_size;
	job.out = data.ptrw();
	const WorkerThreadPool::GroupID g = WorkerThreadPool::get_singleton()->add_template_group_task(&job, &DetailJob::slice, (void *)nullptr, p_size, -1, true, String("Cloud detail noise"));
	WorkerThreadPool::get_singleton()->wait_for_group_task_completion(g);
	equalise(data, 4);
	return data;
}

// With box-filtered mipmaps, so the sky can read far clouds at the scale of its steps. p_channels 1 (R8) or
// 4 (RGBA8), interleaved.
Ref<ImageTexture3D> make_texture(const Vector<float> &p_data, int p_size, int p_channels = 1) {
	const Image::Format format = p_channels == 4 ? Image::FORMAT_RGBA8 : Image::FORMAT_R8;
	const int nc = p_channels;
	Vector<Ref<Image>> slices;
	Vector<float> level = p_data;
	int size = p_size;
	while (size >= 1) {
		for (int z = 0; z < size; z++) {
			Vector<uint8_t> bytes;
			bytes.resize(size * size * nc);
			for (int i = 0; i < size * size * nc; i++) {
				bytes.write[i] = uint8_t(CLAMP(int(level[z * size * size * nc + i] * 255.0f + 0.5f), 0, 255));
			}
			slices.push_back(Image::create_from_data(size, size, false, format, bytes));
		}
		if (size == 1) {
			break;
		}
		const int half = size / 2;
		Vector<float> next;
		next.resize(half * half * half * nc);
		for (int z = 0; z < half; z++) {
			for (int y = 0; y < half; y++) {
				for (int x = 0; x < half; x++) {
					for (int c = 0; c < nc; c++) {
						float s = 0.0f;
						for (int k = 0; k < 8; k++) {
							s += level[(((2 * z + (k >> 2)) * size + 2 * y + ((k >> 1) & 1)) * size + 2 * x + (k & 1)) * nc + c];
						}
						next.write[((z * half + y) * half + x) * nc + c] = s * 0.125f;
					}
				}
			}
		}
		level = next;
		size = half;
	}
	Ref<ImageTexture3D> tex;
	tex.instantiate();
	tex->create(format, p_size, p_size, p_size, true, slices);
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

// Clouds that form and clear (docs/ideas/weather.md, step 3e): each octave of the cover noise moves through
// time as a stack of independent slices, blended by smoothstep from one to the next. The blend keeps the noise's
// spread (Heitz and Neyret 2018, variance-preserving blending: divide by the length of the weights), so the sky
// does not grow flatter halfway between slices and cover still means share of sky. Octave o turns over in
// lifetime * 2.03^(-2o/3): smaller eddies live shorter (Kolmogorov, t ~ l^(2/3)).
constexpr float EVOLVE_OCTAVE_RATE = 1.6031f; // 2.03^(2/3)

Vector2 evolve_slice_offset(int p_slice, int p_octave) {
	return Vector2(hash2(p_slice, 7 + p_octave), hash2(p_slice, 13 + p_octave)) * 4096.0f;
}

float evolving_value_noise(Vector2 p_q, float p_time, int p_octave) {
	const float k = Math::floor(p_time);
	const int ki = int(k);
	const float w = smoothstep(0.0f, 1.0f, p_time - k);
	const float a = value_noise(p_q + evolve_slice_offset(ki, p_octave)) - 0.5f;
	const float b = value_noise(p_q + evolve_slice_offset(ki + 1, p_octave)) - 0.5f;
	const float len = Math::sqrt((1.0f - w) * (1.0f - w) + w * w);
	return CLAMP(0.5f + (a * (1.0f - w) + b * w) / len, 0.0f, 1.0f);
}

float cover_noise(Vector2 p_xz, Vector2 p_drift, float p_scale, float p_evolve) {
	const Vector2 p = (p_xz - p_drift) / MAX(p_scale, 1.0f);
	float v = 0.0f;
	float amp = 0.5f;
	float norm = 0.0f;
	float rate = 1.0f;
	Vector2 q = rotate_lattice(p);
	for (int o = 0; o < 4; o++) {
		const Vector2 qo = q + Vector2(17.1f * o, 31.7f * o);
		v += amp * (p_evolve < 0.0f ? value_noise(qo) : evolving_value_noise(qo, p_evolve * rate + 0.37f * o, o));
		norm += amp;
		amp *= 0.5f;
		rate *= EVOLVE_OCTAVE_RATE;
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
	base_min = base_height;
	top_max = base_height + top_height(*this, t);
	for (int i = 0; i < storm_count; i++) {
		t = MAX(t, 0.9f * storms[i][3]);
		if (storm_base[i] > 0.0f) {
			base_min = MIN(base_min, storm_base[i]);
			top_max = MAX(top_max, storm_base[i] + top_height(*this, MAX(type, 0.9f * storms[i][3])));
		}
	}
	top_max = MAX(top_max, base_height + top_height(*this, t));
}

float base_at(const State &p_state, Vector2 p_xz) {
	float b = p_state.base_height;
	for (int i = 0; i < p_state.storm_count; i++) {
		if (p_state.storm_base[i] <= 0.0f) {
			continue;
		}
		const float *s = p_state.storms[i];
		const float dx = p_xz.x - s[0];
		const float dz = p_xz.y - s[1];
		// Level out to about four times the shield's radius, where its fall-off (cover_type_at, without its growth: a
		// cloud fading out keeps its base) has left under a percent of cover: a base following the fall-off itself
		// sloped the cap's edges down to the map's base. Beyond, the step is under clouds too thin to show it.
		const float g = Math::exp(-(dx * dx + dz * dz) / (s[2] * s[2]) / (1.6f * 1.6f));
		b = Math::lerp(b, p_state.storm_base[i], smoothstep(0.005f, 0.1f, g));
	}
	return b;
}

Vector2 cover_type_at(const State &p_state, Vector2 p_xz) {
	float cover = p_state.cover;
	float t = p_state.type;
	if (p_state.variation > 0.0f) {
		// The patches vary with the cover, not by a fixed amount: a 0.1 sky with 0.35 of variation had
		// stretches at 0.45 that filled half a clear sky.
		cover = CLAMP(cover + (cover_noise(p_xz, p_state.drift, p_state.cover_scale, p_state.evolve) - 0.5f) * 2.0f * p_state.variation * MIN(1.0f, 2.5f * cover), 0.0f, 1.0f);
	}
	// Clusters (wc_cover_type in the shader): the threshold wanders by cloud-sized patches, so neighbouring clouds
	// come out large and merged or small and few, not a field of puffs all one size. Overcast decks keep theirs.
	cover = CLAMP(cover * Math::lerp(CLAMP(1.0f + 4.0f * (cover_noise(p_xz + Vector2(5311.0f, 2719.0f), p_state.drift, p_state.shape_scale * 1.3f, p_state.evolve) - 0.5f), 0.15f, 1.9f), 1.0f, smoothstep(0.5f, 0.85f, cover)), 0.0f, 1.0f);
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

// Cirrus patches: the cover noise at the high layer's own scale and drift, thresholded as the low cover is;
// plus each storm's anvil, spread downwind of its core along the wind aloft (an anvil reaches several times
// a cell's core radius downwind, so the shield arrives hours before the rain). wc_high_cover in the sky.
float high_cover_at(const State &p_state, Vector2 p_xz) {
	float c = 0.0f;
	if (p_state.high_cover > 0.001f) {
		const float n = cover_noise(p_xz + Vector2(9137.0f, 3301.0f), p_state.high_drift, p_state.high_scale, p_state.evolve);
		c = CLAMP((n - (1.0f - p_state.high_cover)) / MAX(p_state.high_cover, 0.001f) * 1.5f, 0.0f, 1.0f) * MIN(1.0f, 2.0f * p_state.high_cover);
	}
	for (int i = 0; i < p_state.storm_count; i++) {
		const float *s = p_state.storms[i];
		const Vector2 centre = Vector2(s[0], s[1]) + p_state.high_dir * (2.5f * s[2]);
		const float x2 = (p_xz - centre).length_squared() / (9.0f * s[2] * s[2]);
		c = 1.0f - (1.0f - c) * (1.0f - Math::exp(-x2) * s[3]);
	}
	return c;
}

float high_tau_at(const State &p_state, Vector2 p_xz) {
	return high_cover_at(p_state, p_xz) * p_state.high_tau;
}

// Altocumulus patches: the cover noise at the mid layer's own scale and drift, thresholded as the low cover is;
// plus a band round each storm's anvil (mid-level cloud spreads from a storm's outflow too). wc_mid_cover in the
// sky.
float mid_cover_at(const State &p_state, Vector2 p_xz) {
	float c = 0.0f;
	if (p_state.mid_cover > 0.001f) {
		const float n = cover_noise(p_xz + Vector2(4721.0f, 8363.0f), p_state.mid_drift, p_state.mid_scale, p_state.evolve);
		c = CLAMP((n - (1.0f - p_state.mid_cover)) / MAX(p_state.mid_cover, 0.001f) * 1.5f, 0.0f, 1.0f) * MIN(1.0f, 2.0f * p_state.mid_cover);
	}
	for (int i = 0; i < p_state.storm_count; i++) {
		const float *s = p_state.storms[i];
		const Vector2 centre = Vector2(s[0], s[1]) + p_state.high_dir * (1.2f * s[2]);
		const float x2 = (p_xz - centre).length_squared() / (4.0f * s[2] * s[2]);
		c = 1.0f - (1.0f - c) * (1.0f - 0.6f * Math::exp(-x2) * s[3]);
	}
	return c;
}

float mid_tau_at(const State &p_state, Vector2 p_xz) {
	return mid_cover_at(p_state, p_xz) * p_state.mid_tau;
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

constexpr float SHAPE_LARGE = 0.381966f; // the large clouds' read's frequency against the first

// (1 - w) a + w b of two even values, evened out again by its CDF (a trapezoid): cover still means share of sky.
float even_mix(float p_a, float p_b, float p_w) {
	const float x = (1.0f - p_w) * p_a + p_w * p_b;
	const float lo = MIN(p_w, 1.0f - p_w);
	const float hi = MAX(p_w, 1.0f - p_w);
	if (lo < 1e-4f) {
		return x;
	}
	if (x < lo) {
		return x * x / (2.0f * lo * hi);
	}
	if (x > hi) {
		return 1.0f - (1.0f - x) * (1.0f - x) / (2.0f * lo * hi);
	}
	return (x - 0.5f * lo) / hi;
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
	// The second read slides against the first (step 3e), so their sum, the clouds, change shape. A slide across
	// keeps the share of sky: each read still averages over the whole tile's area (rising through the tile did
	// not, since one height slice of a 128^3 tile differs from the next and the whole sky pulsed with it).
	const float n = even_sum(sample_tiled(noise, SHAPE_SIZE, Vector3(a.x, y, a.y)), sample_tiled(noise, SHAPE_SIZE, Vector3(b.x + p_state.shift.x, y * SHAPE_SECOND + 0.53f, b.y + p_state.shift.y)));
	if (p_state.size_mix <= 0.0f) {
		return n;
	}
	// Large clouds among the small (wc_shape in the shader): a third read 2.618 times larger, weighted by a slow noise.
	const Vector2 c = rotate_lattice(Vector2(-p.y, p.x)) * SHAPE_LARGE + Vector2(0.19f, 0.83f);
	const float nc = sample_tiled(noise, SHAPE_SIZE, Vector3(c.x - p_state.shift.y, y * SHAPE_LARGE + 0.29f, c.y + p_state.shift.x));
	const float w = p_state.size_mix * 0.75f * smoothstep(0.25f, 0.75f, value_noise(p * 0.31f + Vector2(41.3f, 17.9f)));
	return even_mix(n, nc, w);
}

Vector2 shear_offset(const State &p_state, float p_y, float p_base) {
	const float z = MAX(p_y - p_base, 0.0f) / MAX(p_state.thickness, 1.0f);
	return p_state.shear * MIN(z * z, 9.0f);
}

Vector2 shear_offset(const State &p_state, float p_y) {
	return shear_offset(p_state, p_y, p_state.base_height);
}

float extinction_at(const State &p_state, const Vector3 &p_pos) {
	if (p_pos.y < p_state.base_min || p_pos.y > p_state.top_max) {
		return 0.0f;
	}
	const float b = base_at(p_state, Vector2(p_pos.x, p_pos.z));
	if (p_pos.y < b) {
		return 0.0f;
	}
	const Vector2 ct = cover_type_at(p_state, Vector2(p_pos.x, p_pos.z));
	if (ct.x <= 0.001f) {
		return 0.0f;
	}
	const float h = (p_pos.y - b) / top_height(p_state, ct.y);
	if (h >= 1.0f) {
		return 0.0f;
	}
	const float cu = smoothstep(0.2f, 0.8f, ct.y);
	// Domed tops through the remap, a flat base after it (wc_extinction in the shader).
	// Broken cloud domes over more of its height whatever its type: a narrow band at the top cut the few
	// clouds of a fair sky into flat-topped blocks. Only a deck (high cover) keeps the stratus' flat top.
	const float round_top = Math::lerp(0.6f, Math::lerp(0.15f, 0.6f, cu), smoothstep(0.35f, 0.75f, ct.x));
	const float dome = 1.0f - smoothstep(1.0f - round_top, 1.0f, h);
	const float base = smoothstep(0.0f, Math::lerp(0.2f, 0.03f, cu), h);
	const Vector2 so = shear_offset(p_state, p_pos.y, b);
	const float n = shape_at(p_state, Vector3(p_pos.x - so.x, p_pos.y, p_pos.z - so.y));
	// Nubis's coverage remap, remap(n p, 1 - c, 1, 0, 1): where the noise is high the cloud reaches
	// further up the profile, which domes the tops. Then times the cover (Schneider 2015): the few
	// small clouds of a fair sky are thin and light, where the remap alone made every one as dense as
	// an overcast deck.
	const float shape = MAX(n * dome - (1.0f - ct.x), 0.0f) / MAX(ct.x, 0.001f) * base * ct.x;
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
		detail_data = make_detail(DETAIL_SIZE);
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
		detail_tex = make_texture(data, DETAIL_SIZE, 4);
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
uniform vec2 cloud_shear = vec2(0.0); // m, the tops' lean one thickness up (shear_offset() on the CPU)
uniform float cloud_size_mix = 0.0; // 0..1, how much of the large clouds' read (shape_at() on the CPU)
// Step 3e: the cover noise's time in lifetimes (below 0: frozen), how far the shape's second read has slid
// (xy) and the detail noise risen (z), in texture units, wrapped.
uniform float cloud_evolve = -1.0;
uniform vec3 cloud_churn = vec3(0.0);
uniform int cloud_storm_count = 0;
uniform vec4 cloud_storms[8];
// Each storm's own cloud base (m; <= 0: cloud_base_height), and the lowest base of all (the march's bounds).
uniform float cloud_storm_base[8];
uniform float cloud_base_min = 1500.0;
// Nubis Cubed's sharpening exponent at a cloud's thin edge (1: none).
uniform float cloud_edge_sharpness = 0.45;
// How deep the detail noise erodes the shape (Nubis's erosion), on its base depth.
uniform float cloud_detail_strength = 1.5;
// The high layer (State::high_*).
uniform float cloud_high_cover = 0.0;
uniform float cloud_high_height = 8000.0;
uniform float cloud_high_tau = 1.0;
uniform float cloud_high_scale = 24000.0;
uniform vec2 cloud_high_drift = vec2(0.0);
uniform vec2 cloud_high_dir = vec2(1.0, 0.0);
// The mid layer (State::mid_*).
uniform float cloud_mid_cover = 0.0;
uniform float cloud_mid_height = 4500.0;
uniform float cloud_mid_thickness = 400.0;
uniform float cloud_mid_tau = 4.0;
uniform float cloud_mid_scale = 12000.0;
uniform vec2 cloud_mid_drift = vec2(0.0);
uniform vec2 cloud_mid_dir = vec2(1.0, 0.0);

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

vec2 wc_slice_offset(int k, int o) {
	return vec2(wc_hash2(k, 7 + o), wc_hash2(k, 13 + o)) * 4096.0;
}

float wc_evolving_value_noise(vec2 q, float t, int o) {
	float k = floor(t);
	int ki = int(k);
	float w = smoothstep(0.0, 1.0, t - k);
	float a = wc_value_noise(q + wc_slice_offset(ki, o)) - 0.5;
	float b = wc_value_noise(q + wc_slice_offset(ki + 1, o)) - 0.5;
	return clamp(0.5 + (a * (1.0 - w) + b * w) / sqrt((1.0 - w) * (1.0 - w) + w * w), 0.0, 1.0);
}

float wc_cover_noise_at(vec2 xz, float p_scale) {
	vec2 p = (xz - cloud_drift) / max(p_scale, 1.0);
	float v = 0.0;
	float amp = 0.5;
	float norm = 0.0;
	float rate = 1.0;
	vec2 q = wc_rotate(p);
	for (int o = 0; o < 4; o++) {
		vec2 qo = q + vec2(17.1, 31.7) * float(o);
		v += amp * (cloud_evolve < 0.0 ? wc_value_noise(qo) : wc_evolving_value_noise(qo, cloud_evolve * rate + 0.37 * float(o), o));
		norm += amp;
		amp *= 0.5;
		rate *= 1.6031;
		q = wc_rotate(q) * 2.03;
	}
	return v / norm;
}

float wc_cover_noise(vec2 xz) {
	return wc_cover_noise_at(xz, cloud_cover_scale);
}

// (1 - w) a + w b of two even values, evened out again by its CDF (a trapezoid), so cover still means share of sky
// (even_mix() on the CPU).
float wc_even_mix(float a, float b, float w) {
	float x = (1.0 - w) * a + w * b;
	float lo = min(w, 1.0 - w);
	float hi = max(w, 1.0 - w);
	if (lo < 1e-4) {
		return x;
	}
	if (x < lo) {
		return x * x / (2.0 * lo * hi);
	}
	if (x > hi) {
		return 1.0 - (1.0 - x) * (1.0 - x) / (2.0 * lo * hi);
	}
	return (x - 0.5 * lo) / hi;
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
	float nb = textureLod(cloud_shape, vec3(b.x + cloud_churn.x, y * 0.618034 + 0.53, b.y + cloud_churn.y), max(lod + log2(0.618034), 0.0)).r;
	float s = na + nb;
	float n = s < 1.0 ? 0.5 * s * s : 1.0 - 0.5 * (2.0 - s) * (2.0 - s);
	if (cloud_size_mix <= 0.0) {
		return n;
	}
	// Large clouds among the small (shape_at() on the CPU): a third read 2.618 times larger, weighted by a
	// slow noise, so some stretches hold a few big cumulus and others many small ones. One feature size cut
	// by one threshold gave a sky of puffs all one size, packed in rows to the horizon; fair-weather cumulus
	// sizes run over a decade (Plank 1969: an exponential size distribution).
	vec2 c = wc_rotate(vec2(-xz.y, xz.x)) * 0.381966 + vec2(0.19, 0.83);
	float nc = textureLod(cloud_shape, vec3(c.x - cloud_churn.y, y * 0.381966 + 0.29, c.y + cloud_churn.x), max(lod + log2(0.381966), 0.0)).r;
	float w = cloud_size_mix * 0.75 * smoothstep(0.25, 0.75, wc_value_noise(xz * 0.31 + vec2(41.3, 17.9)));
	return wc_even_mix(n, nc, w);
}

float wc_top_height(float t) {
	return cloud_thickness * mix(0.25, 1.0, smoothstep(0.0, 0.7, t)) * (1.0 + 4.0 * smoothstep(0.8, 1.0, t));
}

vec2 wc_cover_type(vec2 xz) {
	float cover = cloud_cover;
	float t = cloud_type;
	if (cloud_variation > 0.0) {
		cover = clamp(cover + (wc_cover_noise(xz) - 0.5) * 2.0 * cloud_variation * min(1.0, 2.5 * cover), 0.0, 1.0);
	}
	// Clusters (cover_type_at() on the CPU): the threshold wanders by cloud-sized patches, so neighbouring
	// clouds come out large and merged or small and few. One threshold over the whole sky cut the shape noise
	// into a field of puffs all one size. Overcast decks keep theirs.
	cover = clamp(cover * mix(clamp(1.0 + 4.0 * (wc_cover_noise_at(xz + vec2(5311.0, 2719.0), cloud_shape_scale * 1.3) - 0.5), 0.15, 1.9), 1.0, smoothstep(0.5, 0.85, cover)), 0.0, 1.0);
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

// The cloud base over xz (base_at() on the CPU): cloud_base_height, blended to each storm's own base.
float wc_base_at(vec2 xz) {
	float b = cloud_base_height;
	for (int i = 0; i < 8; i++) {
		if (i >= cloud_storm_count) {
			break;
		}
		if (cloud_storm_base[i] <= 0.0) {
			continue;
		}
		vec4 s = cloud_storms[i];
		vec2 d = xz - s.xy;
		b = mix(b, cloud_storm_base[i], smoothstep(0.005, 0.1, exp(-dot(d, d) / (s.z * s.z) / (1.6 * 1.6))));
	}
	return b;
}

// The high layer's cover (high_cover_at() on the CPU).
float wc_high_cover(vec2 xz) {
	float c = 0.0;
	if (cloud_high_cover > 0.001) {
		float n = wc_cover_noise_at(xz + vec2(9137.0, 3301.0) - (cloud_high_drift - cloud_drift), cloud_high_scale);
		c = clamp((n - (1.0 - cloud_high_cover)) / max(cloud_high_cover, 0.001) * 1.5, 0.0, 1.0) * min(1.0, 2.0 * cloud_high_cover);
	}
	for (int i = 0; i < cloud_storm_count; i++) {
		vec4 s = cloud_storms[i];
		vec2 centre = s.xy + cloud_high_dir * (2.5 * s.z);
		vec2 d = xz - centre;
		c = 1.0 - (1.0 - c) * (1.0 - exp(-dot(d, d) / (9.0 * s.z * s.z)) * s.w);
	}
	return c;
}

// The mid layer's cover (mid_cover_at() on the CPU).
float wc_mid_cover(vec2 xz) {
	float c = 0.0;
	if (cloud_mid_cover > 0.001) {
		float n = wc_cover_noise_at(xz + vec2(4721.0, 8363.0) - (cloud_mid_drift - cloud_drift), cloud_mid_scale);
		c = clamp((n - (1.0 - cloud_mid_cover)) / max(cloud_mid_cover, 0.001) * 1.5, 0.0, 1.0) * min(1.0, 2.0 * cloud_mid_cover);
	}
	for (int i = 0; i < cloud_storm_count; i++) {
		vec4 s = cloud_storms[i];
		vec2 centre = s.xy + cloud_high_dir * (1.2 * s.z);
		vec2 d = xz - centre;
		c = 1.0 - (1.0 - c) * (1.0 - 0.6 * exp(-dot(d, d) / (4.0 * s.z * s.z)) * s.w);
	}
	return c;
}

// Extinction (1/m) at p. p_detail erodes the edges with the detail noise (Nubis); the shadow bake
// and the CPU leave it out. p_step is the march's step (m): the noise is read at that scale.
// r_profile: the cloud's depth there before erosion, 0 at its edge to 1 at its core (Nubis Cubed's
// dimensional profile), which the lighting reads for dark edges, the inner glow and the ambient shell.
float wc_extinction_p(vec3 p, vec2 ct, bool p_detail, float p_step, out float r_profile) {
	r_profile = 0.0;
	if (ct.x <= 0.001) {
		return 0.0;
	}
	float b0 = wc_base_at(p.xz);
	float h = (p.y - b0) / wc_top_height(ct.y);
	if (h <= 0.0 || h >= 1.0) {
		return 0.0;
	}
	float cu = smoothstep(0.2, 0.8, ct.y);
	// The tops dome through the coverage remap (the noise must be higher to reach further up); the base is
	// applied after it, so cumulus sit on a flat condensation level instead of rounding off underneath as
	// they did with the whole profile inside the remap (Schneider 2015's height gradient).
	// Broken cloud domes over more of its height whatever its type (dome in extinction_at on the CPU): a
	// narrow band at the top cut the few clouds of a fair sky into flat-topped blocks. Only a deck keeps
	// the stratus' flat top.
	float round_top = mix(0.6, mix(0.15, 0.6, cu), smoothstep(0.35, 0.75, ct.x));
	float dome = 1.0 - smoothstep(1.0 - round_top, 1.0, h);
	float base = smoothstep(0.0, mix(0.2, 0.03, cu), h);
	float texel = cloud_shape_scale / 128.0;
	// Wind shear: the tissue higher up was carried further downwind (and veered) while it rose, so the
	// tower leans and its top streams out (Schneider 2015's cloud top offset), squared in height so the
	// base stays upright. The cover stays read at the column's base.
	float zs = max(p.y - b0, 0.0) / max(cloud_thickness, 1.0);
	p.xz -= cloud_shear * min(zs * zs, 9.0);
	float n = wc_shape(p, p_step);
	float shape = max(n * dome - (1.0 - ct.x), 0.0) / max(ct.x, 0.001) * base;
	r_profile = clamp(shape, 0.0, 1.0);
	if (p_detail && shape > 0.0) {
		// Nubis Cubed's up-rez (Schneider, Advances 2023): wisps (Curly-Alligator) low frequency inside and high
		// frequency at the thin edge; billows (inverted Worley) rounded at the edge and finer toward the core;
		// wispy under the cloud, billowing higher up and in towering cumulus.
		vec2 sxz = wc_rotate(p.xz - cloud_drift);
		vec3 qd = vec3(sxz.x, p.y, sxz.y) / cloud_shape_scale * 8.0 - vec3(0.0, cloud_churn.z, 0.0);
		// Warped by the detail's own coarse billows at a quarter of its frequency (domain warping), so the edge
		// tears into swirls at the scale of the cloud as well as within the texture's own curl.
		float lod_w = max(log2(p_step / texel), 0.0);
		vec2 warp = vec2(textureLod(cloud_detail, qd * 0.25 + vec3(0.31, 0.17, 0.73), lod_w).b, textureLod(cloud_detail, qd * 0.25 + vec3(0.59, 0.41, 0.11), lod_w).b) - 0.5;
		qd += vec3(warp.x, 0.25 * (warp.x + warp.y), warp.y) * mix(1.2, 0.6, clamp(h * 2.0, 0.0, 1.0));
		// Mip by the footprint: an eighth of the shape's tile in 128 texels, so a texel is an eighth of the shape's
		// (2.9 m at the default scale), read at the step's footprint so far billows average instead of aliasing.
		vec4 dn = textureLod(cloud_detail, qd, max(log2(p_step / (texel * 0.125)), 0.0));
		float wispy = mix(dn.g, dn.r, r_profile);
		float billowy = mix(dn.b, dn.a, pow(r_profile, 0.25));
		float billow = clamp(h * 4.0, 0.0, 1.0) * mix(0.6, 1.0, cu);
		float e = cloud_detail_strength * mix(0.45 * wispy, 0.4 * billowy, billow);
		shape = clamp((shape - e) / (1.0 - e), 0.0, 1.0);
		// Sharpening (Nubis Cubed): thin density raised so each billow ends in a defined edge instead of a
		// long fade, more where the cloud is thin.
		shape = pow(shape, mix(cloud_edge_sharpness, 1.0, r_profile));
	}
	// Times the cover after the erosion (Schneider 2015; wc_extinction on the CPU).
	return shape * ct.x * mix(0.04, 0.1, ct.y) * cloud_density;
}

float wc_extinction(vec3 p, vec2 ct, bool p_detail, float p_step) {
	float profile;
	return wc_extinction_p(p, ct, p_detail, p_step, profile);
}
)";

} // namespace WeatherClouds

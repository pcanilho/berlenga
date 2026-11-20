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

#include "weather_mountain_waves.h"

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

CoverNoiseFrame cover_noise_frame(Vector2 p_drift, float p_scale, float p_evolve) {
	CoverNoiseFrame f;
	f.drift = p_drift;
	f.scale = MAX(p_scale, 1.0f);
	f.evolve = p_evolve >= 0.0f;
	float rate = 1.0f;
	for (int o = 0; o < 4; o++) {
		if (f.evolve) {
			// As evolving_value_noise: the two slices either side of the octave's time and the blend between them.
			const float t = p_evolve * rate + 0.37f * o;
			const float k = Math::floor(t);
			const int ki = int(k);
			f.w[o] = smoothstep(0.0f, 1.0f, t - k);
			f.offset_a[o] = evolve_slice_offset(ki, o);
			f.offset_b[o] = evolve_slice_offset(ki + 1, o);
			f.len[o] = Math::sqrt((1.0f - f.w[o]) * (1.0f - f.w[o]) + f.w[o] * f.w[o]);
		}
		rate *= EVOLVE_OCTAVE_RATE;
	}
	return f;
}

float cover_noise(const CoverNoiseFrame &p_frame, Vector2 p_xz) {
	const Vector2 p = (p_xz - p_frame.drift) / p_frame.scale;
	float v = 0.0f;
	float amp = 0.5f;
	float norm = 0.0f;
	Vector2 q = rotate_lattice(p);
	for (int o = 0; o < 4; o++) {
		const Vector2 qo = q + Vector2(17.1f * o, 31.7f * o);
		float n;
		if (p_frame.evolve) {
			const float w = p_frame.w[o];
			const float a = value_noise(qo + p_frame.offset_a[o]) - 0.5f;
			const float b = value_noise(qo + p_frame.offset_b[o]) - 0.5f;
			n = CLAMP(0.5f + (a * (1.0f - w) + b * w) / p_frame.len[o], 0.0f, 1.0f);
		} else {
			n = value_noise(qo);
		}
		v += amp * n;
		norm += amp;
		amp *= 0.5f;
		q = rotate_lattice(q) * 2.03f;
	}
	return v / norm;
}

float cover_noise(Vector2 p_xz, Vector2 p_drift, float p_scale, float p_evolve) {
	return cover_noise(cover_noise_frame(p_drift, p_scale, p_evolve), p_xz);
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
		t = MAX(t, storms[i][3]); // the cumulonimbus body's height (storm_body_at)
		if (storm_base[i] > 0.0f) {
			base_min = MIN(base_min, storm_base[i]);
			top_max = MAX(top_max, storm_base[i] + top_height(*this, MAX(type, storms[i][3])));
		}
		if (storm_outflow[i] > 0.0f) {
			base_min = MIN(base_min, (storm_base[i] > 0.0f ? storm_base[i] : base_height) - SHELF_DROP);
		}
	}
	top_max = MAX(top_max, base_height + top_height(*this, t));
}

// A shelf cloud (arcus) on a storm's gust front: the outflow's cold air undercuts the warm moist inflow ahead
// of the storm and lifts it to saturation below the storm's base, so a wedge of cloud hangs lower along the
// front's leading edge, sharp at the front and rising back into the storm's base (Doswell 1985; WMO
// International Cloud Atlas, arcus). The front runs out ahead of the rain core by the outflow (1.1 core radii
// plus 0.04 per m/s), on the side the storm moves toward (WeatherMap3D steers storms with the wind at about 1 km).
float shelf_drop(const State &p_state, int p_storm, Vector2 p_xz) {
	const float out = p_state.storm_outflow[p_storm];
	if (out <= 0.0f) {
		return 0.0f;
	}
	const float *s = p_state.storms[p_storm];
	const Vector2 d(p_xz.x - s[0], p_xz.y - s[1]);
	const float rr = d.length();
	if (rr < 1.0f) {
		return 0.0f;
	}
	const float r_gf = s[2] * (1.5f + 0.04f * out); // just outside the tower's base (up to 1.2 radii)
	const float u = (rr - r_gf) / (0.6f * s[2]);
	const float arc = smoothstep(0.65f, 0.92f, d.dot(p_state.storm_dir) / rr); // the leading front only, about 40 degrees each side
	const float wedge = smoothstep(-1.2f, -0.1f, u) * (1.0f - smoothstep(-0.1f, 0.05f, u));
	return SHELF_DROP * MIN(out / 15.0f, 1.0f) * s[3] * arc * wedge;
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
	for (int i = 0; i < p_state.storm_count; i++) {
		b -= shelf_drop(p_state, i, p_xz);
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
		// The shelf: one continuous wedge (the lifted inflow condenses all along the front), under the storm's own
		// tower, which it is part of.
		cover = MAX(cover, 0.95f * shelf_drop(p_state, i, p_xz) / SHELF_DROP);
	}
	return Vector2(cover, t);
}

float storm_column_type(const State &p_state, Vector2 p_xz) {
	float t = 0.0f;
	for (int i = 0; i < p_state.storm_count; i++) {
		const float *s = p_state.storms[i];
		const Vector2 ac = Vector2(s[0], s[1]) + p_state.high_dir * (3.2f * s[2]);
		t = MAX(t, s[3] * (1.0f - smoothstep(5.0f, 5.6f, (p_xz - ac).length() / s[2])));
	}
	return t;
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

// Cumulus as single clouds (wc_cells in the shader). A grid of cells, cell_size across, each holding at most one
// cloud: a jittered centre, a size from a skewed draw (many small, a few large: Plank 1969's size spectrum), a
// height growing with its width, and a body that is straight-sided low down and domed above (a cumulus' profile).
// The cell holds a cloud with a probability that follows the local cover, which the cover noise and the cluster
// noise vary (cover_type_at): clouds come in groups with clear sky between. Returns the strongest cloud's
// envelope, 0 outside to 1 a third of a radius in; the shape noise then carves it. Cutting one continuous noise
// at one threshold made shapeless blobs, every top at one height, far rows of stacked pancakes.
float cells_at(const State &p_state, const Vector3 &p_pos, float p_base, Vector2 p_ct, float p_warp) {
	const float cell = p_state.cell_size;
	const Vector2 q = Vector2(p_pos.x - p_state.drift.x, p_pos.z - p_state.drift.y) / cell;
	const int gx = int(Math::floor(q.x));
	const int gz = int(Math::floor(q.y));
	const float top = top_height(p_state, p_ct.y);
	// More cells hold a cloud while they come and go: a cloud spends about a quarter of its life growing or dissolving.
	const float present = p_ct.x * (p_state.evolve >= 0.0f ? 2.9f : 2.2f);
	const float grow = Math::lerp(1.0f, 1.3f, smoothstep(0.3f, 0.7f, p_ct.x));
	float best = 0.0f;
	for (int j = -1; j <= 1; j++) {
		for (int i = -1; i <= 1; i++) {
			const int cx = gx + i;
			const int cz = gz + j;
			// Each cloud's life: its own clock, a cycle each cloud lifetime (State::evolve counts lifetimes) give or
			// take a fifth, offset per cell. Each cycle draws a new cloud from the cell. It grows (wider, then taller
			// as its thermals rise), lives, then dissolves from the edges in: fair-weather cumulus last some 10 to 40
			// minutes (Plank 1969). Frozen, every cloud is in mid life.
			float age = 0.5f;
			int sx = cx;
			int sz = cz;
			if (p_state.evolve >= 0.0f) {
				const float ph = p_state.evolve * Math::lerp(0.8f, 1.25f, hash2(cx + 911, cz - 37)) + hash2(cx - 511, cz + 263);
				const float kc = Math::floor(ph);
				age = ph - kc;
				sx = cx + 7349 * int(kc);
				sz = cz - 3203 * int(kc);
			}
			// Present where the cover here beats the cell's draw, fading over a margin: the cover is read at the sample,
			// so a hard test cut a cloud off by a wall where it crossed a change in cover; now it shrinks toward it.
			const float keep = CLAMP((present - hash2(sx + 7919, sz - 4241)) * 4.0f, 0.0f, 1.0f);
			if (keep <= 0.0f) {
				continue;
			}
			const Vector2 centre = Vector2(float(cx) + 0.15f + 0.7f * hash2(sx, sz + 3571), float(cz) + 0.15f + 0.7f * hash2(sx - 2713, sz));
			const float size = Math::pow(hash2(sx + 1237, sz + 9109), 1.8f);
			const float r = MIN((0.2f + 0.55f * size) * grow, 0.95f) * Math::sqrt(keep) * Math::lerp(0.7f, 1.0f, smoothstep(0.0f, 0.2f, age));
			const float u = (q - centre).length() / r;
			if (u >= 1.0f) {
				continue;
			}
			// Height from width: humilis about half as tall as wide, mediocris nearly as tall, congestus taller.
			const float hgt = MIN(top, Math::lerp(0.3f, 1.0f, smoothstep(0.0f, 0.3f, age)) * 2.0f * r * cell * Math::lerp(0.3f, 1.1f, smoothstep(0.2f, 0.9f, p_ct.y)) * Math::lerp(0.75f, 1.25f, hash2(sx + 5, sz + 11)));
			const float hh = (p_pos.y - p_base) / hgt;
			if (hh <= 0.0f || hh >= 1.0f) {
				continue;
			}
			// The body: straight-sided up to half its height, a little narrower at the base.
			const float k = (hh - 0.45f) / 0.55f;
			float rr = hh < 0.45f ? 1.0f : Math::sqrt(MAX(1.0f - k * k, 0.0f));
			rr *= Math::lerp(0.85f, 1.0f, smoothstep(0.0f, 0.12f, hh));
			float env = (rr - u * p_warp) / 0.6f;
			// Thermal bubbles on top (Neyret 1997's hierarchical bubbles): three domes, each a spheroid half to two
			// thirds the cloud's width, off its axis (all within its cell) and at their own heights, so the top rises in rounded turrets
			// of different heights, the cauliflower crest, not one smooth cap.
			for (int b = 0; b < 3; b++) {
				const float a = float(Math::TAU) * hash2(sx + 31 * b, sz + 577);
				const float off = 0.3f * hash2(sx - 97 * b, sz + 1499);
				const Vector2 cb = centre + Vector2(Math::cos(a), Math::sin(a)) * (off * r);
				const float rb = r * (0.45f + 0.15f * hash2(sx + 401 * b, sz - 733));
				const float yb = Math::lerp(0.5f, 0.75f, hash2(sx - 61 * b, sz - 3331));
				const float vb = 0.3f * rb / r + 0.1f;
				const Vector2 dq = (q - cb) / rb;
				const float dy = (hh - yb) / vb;
				const float d = Math::sqrt(dq.length_squared() + dy * dy);
				env = MAX(env, (1.0f - d * p_warp) / 0.6f);
			}
			// Born thin, then dissolving: the outline shrinks into the noise from the edges in.
			env = env * smoothstep(0.0f, 0.08f, age) - 1.2f * smoothstep(0.75f, 1.0f, age);
			best = MAX(best, CLAMP(env, 0.0f, 1.0f));
		}
	}
	return best;
}

// Density effectors (State::effectors; wc_effector in the shader): each a sphere whose strength falls off linearly
// to its edge (as Sunshine Clouds 2's point effectors, Bonkahe 2025), summed. A positive one widens the cloud
// envelope there, so the shape noise carves a cloud round it (a fire's plume feeding a cumulus, its pyrocumulus);
// a negative one scales the density down (a blast clearing a hole). Clamped to -1..1.
float effector_at(const State &p_state, const Vector3 &p_pos) {
	float e = 0.0f;
	for (int i = 0; i < p_state.effector_count; i++) {
		const float *f = p_state.effectors[i];
		const float d = Vector3(f[0], f[1], f[2]).distance_to(p_pos);
		if (d < f[3]) {
			e += f[4] * (1.0f - d / f[3]);
		}
	}
	return CLAMP(e, -1.0f, 1.0f);
}


// Wave clouds (lenticulars). Air carried up a mountain wave by eta metres cools at the dry rate (9.8 K/km) and
// its dew point at 1.8 K/km, so it saturates once eta passes 125 m per kelvin of dew point depression (Wallace and
// Hobbs 2006, 3.5); past that it condenses at the moist adiabat's rate, about 2 g/kg a kilometre near 0 C,
// falling with temperature (Clausius-Clapeyron, 6% a kelvin). Optical extinction from the condensed water,
// 1.5 LWC / (rho r_e) with droplets of 10 um, ice of 30 um below -25 C (Stephens 1978). The humidity aloft is the
// air round about (Manabe and Wetherald's profile), moistened where the mid or high layer has cover:
// saturated in its clouds and moist round them (wave_layer_rh()). Within a moist layer the humidity varies in thin sheets (a few hundred
// metres; the radiosonde's stepped profiles), which stacks the lens into plates (pile d'assiettes).
// The air round about (not a lifted parcel): Manabe and Wetherald's (1967) relative humidity profile,
// RH = RH0 (p / p0 - 0.02) / 0.98 (pressure with an 8 km scale height), the temperature at the model's 6.5 K/km;
// the free troposphere is drier than the ground air, so only the moist layers make wave cloud.
static float wave_env_depression(float p_t0, float p_rh0, float p_dz) {
	const float t = p_t0 - 0.0065f * p_dz;
	const float q = Math::exp(-MAX(p_dz, 0.0f) / 8000.0f);
	const float rh = CLAMP(p_rh0 * (q - 0.02f) / 0.98f, 0.01f, 1.0f);
	const float tk = t + 273.15f;
	return -Math::log(rh) * tk * tk / 5423.0f;
}

// A moist layer's relative humidity in its clear air: saturated in its clouds, 0.93 at their edges, falling to
// 0.68 half a cover's width of noise outside them (a moist layer is moist round its clouds as
// well, which is where the waves raise lenses without other cloud).
static float wave_layer_rh(float p_noise, float p_cover, float p_c) {
	const float u = (p_noise - (1.0f - p_cover)) / MAX(p_cover, 0.001f);
	float rh = CLAMP(0.93f + 0.5f * u, 0.5f, 0.93f);
	if (p_c > 0.001f) {
		rh = MAX(rh, 1.0f - 0.03f * (1.0f - p_c));
	}
	return rh;
}

static float wave_rh_depression(float p_rh, float p_tk) {
	return -Math::log(MAX(p_rh, 0.01f)) * p_tk * p_tk / 5423.0f;
}

float wave_dew_depression_at(const State &p_state, const Vector3 &p_pos) {
	const float dz = p_pos.y - p_state.air_y0;
	const float tk = p_state.air_t0 - 0.0065f * dz + 273.15f;
	float dep = wave_env_depression(p_state.air_t0, p_state.air_rh0, dz);
	const Vector2 xz(p_pos.x, p_pos.z);
	if (p_state.mid_cover > 0.001f) {
		const float n = cover_noise(xz + Vector2(4721.0f, 8363.0f), p_state.mid_drift, p_state.mid_scale, p_state.evolve);
		const float rh = wave_layer_rh(n, p_state.mid_cover, mid_cover_at(p_state, xz));
		const float d = (p_pos.y - p_state.mid_height) / (1.5f * p_state.mid_thickness + 300.0f);
		const float w = Math::exp(-d * d) * MIN(1.0f, 4.0f * p_state.mid_cover);
		const float plates = 0.5f + 0.5f * Math::sin(Math::TAU * (p_pos.y - p_state.mid_height) / 350.0f);
		dep = Math::lerp(dep, MIN(dep, wave_rh_depression(rh, tk) + 0.4f * plates), w);
	}
	if (p_state.high_cover > 0.001f) {
		const float n = cover_noise(xz + Vector2(9137.0f, 3301.0f), p_state.high_drift, p_state.high_scale, p_state.evolve);
		const float rh = wave_layer_rh(n, p_state.high_cover, high_cover_at(p_state, xz));
		const float d = (p_pos.y - p_state.high_height) / 800.0f;
		const float w = Math::exp(-d * d) * MIN(1.0f, 4.0f * p_state.high_cover);
		dep = Math::lerp(dep, MIN(dep, wave_rh_depression(rh, tk)), w);
	}
	return dep;
}

float wave_extinction_at(const State &p_state, const Vector3 &p_pos) {
	const bool in_a = p_state.wave_y1 > p_state.wave_y0 && p_pos.y >= p_state.wave_y0 && p_pos.y <= p_state.wave_y1;
	const bool in_b = p_state.wave_y3 > p_state.wave_y2 && p_pos.y >= p_state.wave_y2 && p_pos.y <= p_state.wave_y3;
	if (!in_a && !in_b) {
		return 0.0f;
	}
	const float eta = mountain_wave_eta(p_state.wave_eta, p_state.wave_n, p_state.wave_corner, p_state.wave_size, p_state.wave_levels, p_pos);
	if (eta <= 0.0f) {
		return 0.0f;
	}
	const float x = eta - 125.0f * wave_dew_depression_at(p_state, p_pos);
	if (x <= 0.0f) {
		return 0.0f;
	}
	const float tc = p_state.air_t0 - 0.0065f * (p_pos.y - p_state.air_y0) - 0.0098f * eta;
	const float rate = 2.0e-6f * Math::exp(0.06f * MAX(tc, -40.0f)); // kg/kg a metre of lift
	const float rho_air = 1.225f * Math::exp(-p_pos.y / 8500.0f);
	const float water = rho_air * rate * x; // kg/m^3
	const float ice = CLAMP((-20.0f - tc) / 10.0f, 0.0f, 1.0f);
	return 1.5f * water / Math::lerp(1000.0f * 10e-6f, 917.0f * 30e-6f, ice);
}

Vector4 wave_slab(const State &p_state, const float *p_eta_max) {
	const float split = 0.5f * (p_state.mid_height + p_state.high_height);
	float y0 = 1.0e9f;
	float y1 = -1.0e9f;
	float y2 = 1.0e9f;
	float y3 = -1.0e9f;
	for (float y = 1000.0f; y <= 10000.0f; y += 250.0f) {
		// The largest lift at this height (linear between the levels, as mountain_wave_eta()).
		float e;
		const float *lv = p_state.wave_levels;
		if (y <= lv[0]) {
			e = p_eta_max[0];
		} else if (y >= lv[3]) {
			e = p_eta_max[3];
		} else {
			int k = 0;
			while (k < 2 && y > lv[k + 1]) {
				k++;
			}
			e = Math::lerp(p_eta_max[k], p_eta_max[k + 1], (y - lv[k]) / (lv[k + 1] - lv[k]));
		}
		if (e <= 0.0f) {
			continue;
		}
		// The moistest the air there can be now: each layer at its fullest local cover.
		const State &s = p_state;
		const float dz = y - s.air_y0;
		const float tk = s.air_t0 - 0.0065f * dz + 273.15f;
		float dep = wave_env_depression(s.air_t0, s.air_rh0, dz);
		if (s.mid_cover > 0.001f) {
			const float d = (y - s.mid_height) / (1.5f * s.mid_thickness + 300.0f);
			dep = Math::lerp(dep, MIN(dep, wave_rh_depression(0.93f, tk)), Math::exp(-d * d) * MIN(1.0f, 4.0f * s.mid_cover));
		}
		if (s.high_cover > 0.001f) {
			const float d = (y - s.high_height) / 800.0f;
			dep = Math::lerp(dep, MIN(dep, wave_rh_depression(0.93f, tk)), Math::exp(-d * d) * MIN(1.0f, 4.0f * s.high_cover));
		}
		if (e > 125.0f * dep) {
			if (y < split) {
				y0 = MIN(y0, y - 250.0f);
				y1 = MAX(y1, y + 250.0f);
			} else {
				y2 = MIN(y2, y - 250.0f);
				y3 = MAX(y3, y + 250.0f);
			}
		}
	}
	return Vector4(y0, MIN(y1, split), MAX(y2, split), y3);
}

// Stratocumulus: a deck broken into closed cells, each a slow overturning under the inversion whose middle rises
// thick and whose rim sinks thin, 1.5 to 3 km across for a 300 to 500 m deep layer (aspect about 30:1; Wood 2012,
// Mon. Wea. Rev. 140), lined up into rolls along the wind when it mixes the layer (Atkinson and Zhang 1996).
float sc_share(float p_type) {
	return smoothstep(0.06f, 0.14f, p_type) * (1.0f - smoothstep(0.3f, 0.42f, p_type));
}

// Layered cloud (stratus, stratocumulus) is one sheet from the deck's noise, not single clouds: broken stratocumulus
// at half cover is flat patches of the sheet, not puffs (WMO International Cloud Atlas: Sc "in patches, sheet or
// layer", St "a generally grey cloud layer with a fairly uniform base"). So the single clouds give way to the sheet
// below type 0.36, and the sheet reaches down to a broken sky.
float layered_share(float p_type) {
	return 1.0f - smoothstep(0.22f, 0.36f, p_type);
}

float sc_cells_at(const State &p_state, Vector2 p_xz) {
	const Vector2 q0 = p_xz - p_state.drift;
	const Vector2 d = p_state.sc_dir;
	const Vector2 q(q0.dot(d) / (p_state.sc_cell * p_state.sc_roll), (q0.x * -d.y + q0.y * d.x) / p_state.sc_cell);
	const int gx = int(Math::floor(q.x));
	const int gz = int(Math::floor(q.y));
	float f1 = 9.0f;
	float f2 = 9.0f;
	for (int j = -1; j <= 1; j++) {
		for (int i = -1; i <= 1; i++) {
			const Vector2 c(float(gx + i) + hash2(gx + i + 4057, gz + j - 911), float(gz + j) + hash2(gx + i - 6311, gz + j + 2207));
			const float dist = (q - c).length();
			if (dist < f1) {
				f2 = f1;
				f1 = dist;
			} else if (dist < f2) {
				f2 = dist;
			}
		}
	}
	return smoothstep(0.0f, 0.7f, f2 - f1);
}

// A storm's cumulonimbus: one body, not a field of single cumulus (cells_at makes clouds of a kilometre or two).
// Built as an envelope with a hard edge, as Nubis shapes a storm (Schneider and Vos 2015: the base shape times the
// height signal, high-frequency erosion only at the edge, so the mass reads as one isosurface; Schneider 2022,
// "The Real-Time Volumetric Superstorms of Horizon Forbidden West": dense, hard-edged storm volumes), with the
// structure a cumulonimbus shows (Houze 1993, Cloud Dynamics, 8; WMO International Cloud Atlas, Cb incus):
// - the tower: about 1.6 core radii across, leaning downwind with height, its wall pushed in and out by the shape
//   noise (the cauliflower turrets; the detail erosion then billows its skin), up to the anvil's base, with an
//   overshooting dome over its axis to the storm's top;
// - the anvil: a flat, thin ice sheet under the tropopause (its top at 97% of the storm's height), spreading
//   downwind along the wind aloft as a fan (1 core radius each side at the tower, widening 0.35 per radius out, to
//   7 radii), a sharp edge upwind and a frayed one downwind, thick over the tower (17% of the height) and thinning
//   to 7% far out, its underside a flat ceiling;
// - mammatus: under the anvil's overhang (away from the tower), where the air below is dry (sublimation drives
//   them; Schultz et al. 2006), the underside sags into lobes up to 500 m deep, 1 to 2 km across on a jittered
//   grid.
// Returns the envelope (0 to 1; the anvil's ice at 0.55 of the tower's density); r_near: the clear zone round the
// tower where single cells fade (the updraft's subsidence).
float mammatus_depth(Vector2 p_xz, Vector2 p_drift) {
	const Vector2 q = (p_xz - p_drift) / 1600.0f;
	const int gx = int(Math::floor(q.x));
	const int gz = int(Math::floor(q.y));
	float dep = 0.0f;
	for (int j = -1; j <= 1; j++) {
		for (int i = -1; i <= 1; i++) {
			const int cx = gx + i;
			const int cz = gz + j;
			const Vector2 cp(float(cx) + hash2(cx + 811, cz - 3373), float(cz) + hash2(cx - 2939, cz + 617));
			const float h = hash2(cx + 4127, cz + 9133);
			const float h2 = hash2(cx - 6143, cz + 2207);
			// Sizes and depths vary cell to cell (0.45 to 0.85 of the grid across, 0.25 to 1 deep) and a fifth of the
			// cells hold none, so no row of identical puffs repeats.
			if (h2 < 0.2f) {
				continue;
			}
			const float r = (q - cp).length() / (0.45f + 0.4f * h);
			dep = MAX(dep, (0.25f + 0.75f * h2) * Math::sqrt(MAX(1.0f - r * r, 0.0f)));
		}
	}
	// In patches about 6 km across: mammatus come in fields with smooth stretches between.
	return 700.0f * dep * smoothstep(0.3f, 0.65f, value_noise(q * 0.27f + Vector2(17.3f, 5.1f)));
}

// A storm's tower (wc_storm_tower in the shader): a core column and eight rising bubbles, each a thermal 0.7 to 1.4
// core radii across set round the axis at its own height and bearing, larger higher up, and a crown. A cumulonimbus
// is a few huge rounded towers, each lit on its sun side and grey on its shadow side (the shading comes from the
// bubbles' own shape); outlines from noise alone came out as an even mottle over a cylinder. Offsets in metres from
// the leaned axis; returns 0 to 1.
static float storm_tower(const Vector2 &p_d, float p_y, float p_r, float p_h, float p_noise, int p_seed) {
	const float hh = p_y / p_h;
	const float jitter = 0.15f * (p_noise - 0.45f);
	// Several km wide at its base (1.5 core radii across) and widening up into the anvil (2.4 across).
	const float rc = 0.75f + 0.45f * smoothstep(0.0f, 0.85f, hh);
	float e = 1.0f - smoothstep(0.85f, 1.0f, p_d.length() / (p_r * rc) - jitter);
	for (int k = 0; k < 13; k++) {
		const int hk_seed = p_seed * 17 + k;
		// The last is the crown, over the axis.
		const float hk = k < 12 ? 0.1f + 0.068f * k : 0.8f;
		const float th = Math::TAU * hash2(hk_seed, 7919);
		const float o = k < 12 ? (0.45f + 0.35f * hash2(hk_seed, 104729)) * (0.8f + 0.5f * hk) : 0.15f;
		float rk = k < 12 ? (0.35f + 0.2f * hash2(hk_seed, 3301)) * (0.8f + 0.9f * hk) : 0.6f;
		rk = MIN(rk, (0.99f - hk) * p_h / p_r); // under the column's top
		const Vector3 q(p_d.x / p_r - o * Math::cos(th), (p_y - hk * p_h) / p_r, p_d.y / p_r - o * Math::sin(th));
		e = MAX(e, 1.0f - smoothstep(0.82f, 1.0f, q.length() / rk - jitter));
	}
	return e;
}

float storm_body_at(const State &p_state, const Vector3 &p_pos, float p_base, float p_noise, float *r_near, float *r_thin) {
	float best = 0.0f;
	float thin = 1.0f;
	float near = 0.0f;
	const Vector2 xz(p_pos.x, p_pos.z);
	const Vector2 dir = p_state.high_dir;
	for (int i = 0; i < p_state.storm_count; i++) {
		const float *s = p_state.storms[i];
		if (s[3] <= 0.01f) {
			continue;
		}
		const float hgt = top_height(p_state, 1.0f) * s[3];
		const Vector2 d = xz - Vector2(s[0], s[1]);
		const float a = d.dot(dir) / s[2];
		const float c = (d.x * -dir.y + d.y * dir.x) / s[2];
		// The tower.
		const float hh = (p_pos.y - p_base) / hgt;
		const float ax = (d - dir * (s[2] * 0.3f * hh * hh)).length() / s[2];
		// The single cells give way over the whole updraft region and its inflow (about three radii), so the tower
		// stands alone above its flanking cloud instead of hidden in a crowd of congestus as tall as itself.
		near = MAX(near, (1.0f - smoothstep(2.6f, 3.8f, ax)) * s[3]);
		float e = 0.0f;
		float k = 1.0f;
		if (hh > 0.0f && hh < 1.0f && ax < 2.4f) {
			// Hard outlines (the last fifth of each bubble): a wide soft rim let the erosion bore through into foam.
			e = storm_tower(d - dir * (s[2] * 0.3f * hh * hh), p_pos.y - p_base, s[2], hgt, p_noise, i);
		}
		// The anvil.
		if (a > -1.6f && a < 7.5f) {
			const float frac = 0.97f - (0.17f - 0.1f * smoothstep(0.0f, 6.0f, a));
			float sag = 0.0f;
			const float w = 1.6f + 0.4f * MAX(a + 1.2f, 0.0f); // wider than the tower's top (up to 1.6 radii)
			// Pouches under the sheet only, not at its thin side edges, where they hung as loose lumps.
			const float overhang = smoothstep(2.0f, 3.0f, ax) * (1.0f - 0.7f * smoothstep(0.6f, 0.95f, p_state.air_rh0)) * (1.0f - smoothstep(0.5f, 0.75f, Math::abs(c) / w));
			if (overhang > 0.0f) {
				sag = mammatus_depth(xz, p_state.high_drift) * overhang * smoothstep(-0.5f, 1.0f, a) * (1.0f - smoothstep(4.0f, 6.0f, a));
			}
			const float hb = p_base + frac * hgt - sag;
			const float ht = p_base + 0.97f * hgt;
			if (p_pos.y > hb && p_pos.y < ht) {
				const float horiz = (1.0f - smoothstep(0.85f, 1.0f, Math::abs(c) / w + 0.3f * (p_noise - 0.5f))) * smoothstep(-1.55f, -1.4f, a) * (1.0f - smoothstep(5.0f, 7.0f, a + 2.0f * (p_noise - 0.5f)));
				const float vert = smoothstep(hb, hb + 30.0f, p_pos.y) * (1.0f - smoothstep(ht - 60.0f, ht, p_pos.y));
				// The anvil is ice: optically about 10 deep over its kilometre or two near the core (extinction near
				// 0.01 per metre) and thinning to 1 or 2 far downwind (Lilly 1988; Garrett 2005), a tenth of the
				// water tower's. Kept as a solid envelope for the erosion, made thin by the density (r_thin).
				const float ea = 0.55f * horiz * vert;
				if (ea > e) {
					e = ea;
					k = 0.15f * (1.0f - 0.7f * smoothstep(1.0f, 6.0f, a));
					// The pouches carry the anvil's falling, evaporating precipitation (Schultz et al. 2006): denser than
					// the sheet, so each shades its own lower half and its neighbours, rather than a faint decal.
					// The sheet over them as dense, so seen from afar the underside is one surface with pouches, not
					// separate lumps floating under a faint veil.
					k = MAX(k, 0.45f * overhang * smoothstep(-0.5f, 1.0f, a) * (1.0f - smoothstep(4.0f, 6.0f, a)));
					// The pouches themselves denser still (they carry the anvil's falling, evaporating precipitation;
					// Schultz et al. 2006), so each shades its own lower half.
					k = MAX(k, 0.7f * MIN(sag / 200.0f, 1.0f) * (1.0f - smoothstep(hb, hb + 900.0f, p_pos.y)));
				}
			}
		}
		if (e > best) {
			best = e;
			thin = k;
		}
	}
	if (r_near) {
		*r_near = near;
	}
	if (r_thin) {
		*r_thin = thin;
	}
	return best;
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
	const float eff = p_state.effector_count > 0 ? effector_at(p_state, p_pos) : 0.0f;
	if (ct.x <= 0.001f && eff <= 0.0f) {
		return 0.0f;
	}
	// Under a storm's anvil the column reaches the storm's height for its body only; the single cells keep their own
	// type's height (wc_extinction_p in the shader).
	const float tcol = p_state.storm_count > 0 ? MAX(ct.y, storm_column_type(p_state, Vector2(p_pos.x, p_pos.z))) : ct.y;
	if (p_pos.y >= b + top_height(p_state, tcol)) {
		return 0.0f;
	}
	const float h = MIN((p_pos.y - b) / top_height(p_state, ct.y), 1.0f);
	const float own = p_pos.y < b + top_height(p_state, ct.y) ? 1.0f : 0.0f;
	const float cu = smoothstep(0.2f, 0.8f, ct.y);
	// Domed tops through the remap, a flat base after it (wc_extinction in the shader).
	// Broken cloud domes over more of its height whatever its type: a narrow band at the top cut the few
	// clouds of a fair sky into flat-topped blocks. Only a deck (high cover) keeps the stratus' flat top.
	const float round_top = Math::lerp(0.6f, Math::lerp(0.15f, 0.6f, cu), smoothstep(0.35f, 0.75f, ct.x));
	const float dome = 1.0f - smoothstep(1.0f - round_top, 1.0f, h);
	const float base = smoothstep(0.0f, Math::lerp(0.2f, 0.03f, cu), h);
	const Vector2 so = shear_offset(p_state, p_pos.y, b);
	// Flat bases (wc_extinction_p): the lowest 120 m read the noise at its top, so the outline drops straight to the base.
	const float n = shape_at(p_state, Vector3(p_pos.x - so.x, MAX(p_pos.y, b + 120.0f), p_pos.z - so.y));
	float near_body = 0.0f;
	float thin = 1.0f;
	const float body = p_state.storm_count > 0 ? storm_body_at(p_state, p_pos, b, n, &near_body, &thin) : 0.0f;
	// Nubis's coverage remap, remap(n p, 1 - c, 1, 0, 1): where the noise is high the cloud reaches
	// further up the profile, which domes the tops. Then times the cover (Schneider 2015): the few
	// small clouds of a fair sky are thin and light, where the remap alone made every one as dense as
	// an overcast deck.
	// The single clouds (cells_at), carved by the shape noise into lobes: only their shell, the inside stays whole.
	// A deck (high cover) is the continuous noise's, as before: overcast is one sheet, not cells.
	// The shape noise warps each cloud's outline (its distances stretched by up to half where the noise is low)
	// and carves its shell.
	float env = CLAMP(cells_at(p_state, Vector3(p_pos.x - so.x, p_pos.y, p_pos.z - so.y), b, ct, 1.0f) + 1.5f * MAX(eff, 0.0f), 0.0f, 1.0f);
	// The storm's body: solid through, its outline the shape noise's where the envelope falls off.

	const float lay = layered_share(ct.y);
	const float cells = MAX(n - (1.0f - env), 0.0f) / MAX(env, 0.001f) * base * (1.0f - lay) * own;
	const float sc = sc_share(ct.y);
	// A sheet: its noise lifted so an overcast layer closes (the shape noise alone left gaps at any cover); a
	// stratocumulus deck is then mostly its cells, thick in the middle and thin at the rims.
	// Each cell a rounded heap: its top highest in the middle and lower toward the rim, the outline roughened by
	// the shape noise, so a roll reads as a row of lumpy cells with darker thin gaps between, not one extruded slab.
	float cell = 1.0f;
	if (sc > 0.0f) {
		const float cv = CLAMP(sc_cells_at(p_state, Vector2(p_pos.x, p_pos.z)) + 0.5f * (n - 0.5f), 0.0f, 1.0f);
		const float ht = 0.25f + 0.75f * cv;
		// The base sags under each cell's middle and rises toward its rim (the thick middle reaches lower), so the
		// underside is a row of lumps.
		const float lift = 0.15f * (1.0f - cv);
		cell = Math::lerp(1.0f, (1.0f - smoothstep(0.5f * ht, ht, h)) * smoothstep(lift, lift + 0.08f, h) * (0.5f + 0.7f * cv), sc);
	}
	const float nd = lay > 0.0f ? Math::lerp(n, (0.5f + 0.5f * n) * cell, lay) : n;
	const float deck = own * MAX(nd * dome - (1.0f - ct.x), 0.0f) / MAX(ct.x, 0.001f) * base * smoothstep(Math::lerp(0.55f, 0.15f, lay), Math::lerp(0.85f, 0.45f, lay), ct.x);
	// Stratocumulus cells dense through their middles (liquid water 0.2 to 0.5 g/m3, extinction about 0.03 to 0.06
	// per metre; Wood 2012) with a soft thin rim: the linear remap left each cell an even, faint disc.
	const float deck_sc = sc > 0.0f ? Math::lerp(deck, MIN(deck * 3.0f, 1.0f), sc) : deck;
	// The shelf: a solid wedge from its lowered base to 400 m above the storm's own base, smooth underneath, its
	// upper face roughened by the noise (the lifted inflow condenses all along the front as one cloud).
	float shelf = 0.0f;
	float shelf_top = 0.0f;
	for (int i = 0; i < p_state.storm_count; i++) {
		const float dr = shelf_drop(p_state, i, Vector2(p_pos.x, p_pos.z));
		if (dr > shelf_top) {
			shelf_top = dr;
			shelf = dr / SHELF_DROP;
		}
	}
	// Its top a smooth slope rippled by the shape noise at large scale only: the noise thresholding the whole wedge
	// left a frilled, grainy rim.
	// A wedge: its top lowest at the leading edge and rising back toward the storm (about 1.5 km over the drop), where
	// it joins the tower's base, so from the front it reads as a sloping band, not a flat-topped brim.
	const float wedge_top = 0.4f * shelf_top + 300.0f + 1500.0f * (1.0f - shelf);
	const float wedge = shelf > 0.0f ? smoothstep(0.3f, 0.6f, shelf) * (1.0f - smoothstep(0.7f, 1.0f, (p_pos.y - b) / wedge_top + 0.25f * (n - 0.5f))) : 0.0f;
	// The deck gives way there too: the storm's raised cover made a flat-topped sheet round its base (a brim).
	const float other = MAX(MAX(cells, deck_sc) * (1.0f - MAX(near_body, shelf)), wedge);
	const float shape = MAX(other, body) * (1.0f + MIN(eff, 0.0f));
	// The storm's body its own density whatever the cover and type around it: the tower a cumulus' (0.1 per metre),
	// the anvil ice (thin).
	if (body > other) {
		return shape * 0.1f * thin * p_state.density;
	}
	// Frostbite's measured extinction: stratus about 0.04, cumulus up to 0.1 per metre; a fair sky's clouds a
	// little lighter than a deck's.
	return shape * Math::lerp(0.4f, 1.0f, ct.x) * Math::lerp(0.04f, 0.1f, ct.y) * p_state.density;
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
uniform float cloud_storm_rain[8]; // peak mm/h times growth (round 8)
uniform float cloud_storm_outflow[8]; // m/s
uniform vec2 cloud_storm_dir = vec2(1.0, 0.0); // the way the storms move
uniform vec3 cloud_air = vec3(15.0, 0.6, 0.0); // ground temperature (C), relative humidity, its height (m)
uniform vec2 cloud_rain_slant = vec2(0.0); // horizontal metres per metre of fall (the low wind over the drops' 7 m/s)
uniform float cloud_base_min = 1500.0;
// Density effectors (WeatherMap3D.add_cloud_effector): world centre and radius (m), strength (> 0 adds cloud).
uniform int cloud_effector_count = 0;
uniform vec4 cloud_effectors[8];
uniform float cloud_effector_strength[8];
// Point lights in the clouds (WeatherMap3D.add_cloud_light): world position and reach (m), linear colour times energy.
uniform int cloud_light_count = 0;
uniform vec4 cloud_lights[8];
uniform vec3 cloud_light_colors[8];
// Nubis Cubed's sharpening exponent at a cloud's thin edge (1: none).
uniform float cloud_edge_sharpness = 0.45;
// How deep the detail noise erodes the shape (Nubis's erosion), on its base depth.
uniform float cloud_detail_strength = 1.5;
// How deep the turrets cut into each cumulus: billows at the cloud's own scale over its top half and down its sides
// (cauliflower crests; 0: one smooth dome).
uniform float cloud_turret_strength = 0.9;
// The cumulus cells' size (m): each holds at most one cloud (State::cell_size).
uniform float cloud_cell_size = 1200.0;
// Stratocumulus cells (State::sc_*): the wind's direction, the roll stretch along it, the cell size (m).
uniform vec4 cloud_sc = vec4(1.0, 0.0, 1.0, 1800.0);
// The high layer (State::high_*).
uniform float cloud_high_cover = 0.0;
uniform float cloud_high_height = 8000.0;
uniform float cloud_high_tau = 1.0;
uniform float cloud_high_scale = 24000.0;
uniform vec2 cloud_high_drift = vec2(0.0);
uniform vec2 cloud_high_dir = vec2(1.0, 0.0);
uniform float cloud_high_cc = 0.0;
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

// A storm's cumulonimbus body (storm_body_at() on the CPU, which explains it).
float wc_mammatus_depth(vec2 xz) {
	vec2 q = (xz - cloud_high_drift) / 1600.0;
	ivec2 g = ivec2(floor(q));
	float dep = 0.0;
	for (int j = -1; j <= 1; j++) {
		for (int i = -1; i <= 1; i++) {
			ivec2 c = g + ivec2(i, j);
			vec2 cp = vec2(c) + vec2(wc_hash2(c.x + 811, c.y - 3373), wc_hash2(c.x - 2939, c.y + 617));
			float h = wc_hash2(c.x + 4127, c.y + 9133);
			float h2 = wc_hash2(c.x - 6143, c.y + 2207);
			if (h2 < 0.2) {
				continue;
			}
			float r = length(q - cp) / (0.45 + 0.4 * h);
			dep = max(dep, (0.25 + 0.75 * h2) * sqrt(max(1.0 - r * r, 0.0)));
		}
	}
	return 700.0 * dep * smoothstep(0.3, 0.65, wc_value_noise(q * 0.27 + vec2(17.3, 5.1)));
}

// A storm's tower (storm_tower() on the CPU, which explains it).
float wc_storm_tower(vec2 d, float y, float r, float h, float n, int seed) {
	float hh = y / h;
	float jitter = 0.15 * (n - 0.45);
	float rc = 0.75 + 0.45 * smoothstep(0.0, 0.85, hh);
	float e = 1.0 - smoothstep(0.85, 1.0, length(d) / (r * rc) - jitter);
	for (int k = 0; k < 13; k++) {
		int ks = seed * 17 + k;
		float hk = k < 12 ? 0.1 + 0.068 * float(k) : 0.8;
		float th = 6.28318531 * wc_hash2(ks, 7919);
		float o = k < 12 ? (0.45 + 0.35 * wc_hash2(ks, 104729)) * (0.8 + 0.5 * hk) : 0.15;
		float rk = k < 12 ? (0.35 + 0.2 * wc_hash2(ks, 3301)) * (0.8 + 0.9 * hk) : 0.6;
		rk = min(rk, (0.99 - hk) * h / r);
		vec3 q = vec3(d.x / r - o * cos(th), (y - hk * h) / r, d.y / r - o * sin(th));
		e = max(e, 1.0 - smoothstep(0.82, 1.0, length(q) / rk - jitter));
	}
	return e;
}

float wc_storm_body(vec3 p, float b, float n, float p_step, out float r_near, out float r_thin) {
	float best = 0.0;
	r_near = 0.0;
	r_thin = 1.0;
	vec2 dir = cloud_high_dir;
	for (int i = 0; i < 8; i++) {
		if (i >= cloud_storm_count) {
			break;
		}
		vec4 s = cloud_storms[i];
		if (s.w <= 0.01) {
			continue;
		}
		float hgt = wc_top_height(1.0) * s.w;
		vec2 d = p.xz - s.xy;
		float a = dot(d, dir) / s.z;
		float c = (d.x * -dir.y + d.y * dir.x) / s.z;
		float hh = (p.y - b) / hgt;
		float ax = length(d - dir * (s.z * 0.3 * hh * hh)) / s.z;
		r_near = max(r_near, (1.0 - smoothstep(2.6, 3.8, ax)) * s.w);
		float e = 0.0;
		float k = 1.0;
		if (hh > 0.0 && hh < 1.0 && ax < 2.4) {
			e = wc_storm_tower(d - dir * (s.z * 0.3 * hh * hh), p.y - b, s.z, hgt, n, i);
		}
		if (a > -1.6 && a < 7.5) {
			float frac = 0.97 - (0.17 - 0.1 * smoothstep(0.0, 6.0, a));
			float w = 1.6 + 0.4 * max(a + 1.2, 0.0);
			float overhang = smoothstep(2.0, 3.0, ax) * (1.0 - 0.7 * smoothstep(0.6, 0.95, cloud_air.y)) * (1.0 - smoothstep(0.5, 0.75, abs(c) / w));
			float sag = overhang > 0.0 ? wc_mammatus_depth(p.xz) * overhang * smoothstep(-0.5, 1.0, a) * (1.0 - smoothstep(4.0, 6.0, a)) : 0.0;
			float hb = b + frac * hgt - sag;
			float ht = b + 0.97 * hgt;
			if (p.y > hb && p.y < ht) {
				float horiz = (1.0 - smoothstep(0.85, 1.0, abs(c) / w + 0.3 * (n - 0.5))) * smoothstep(-1.55, -1.4, a) * (1.0 - smoothstep(5.0, 7.0, a + 2.0 * (n - 0.5)));
				float vert = smoothstep(hb, hb + 30.0, p.y) * (1.0 - smoothstep(ht - 60.0, ht, p.y));
				float ea = 0.55 * horiz * vert; // ice: thin by the density (storm_body_at() on the CPU)
				if (ea > e) {
					e = ea;
					k = 0.15 * (1.0 - 0.7 * smoothstep(1.0, 6.0, a));
					k = max(k, 0.45 * overhang * smoothstep(-0.5, 1.0, a) * (1.0 - smoothstep(4.0, 6.0, a)));
					k = max(k, 0.7 * min(sag / 200.0, 1.0) * (1.0 - smoothstep(hb, hb + 900.0, p.y)));
				}
			}
		}
		if (e > best) {
			best = e;
			r_thin = k;
		}
	}
	return best;
}

// A storm's shelf cloud: how far its gust front lowers the base at xz (shelf_drop() on the CPU).
float wc_shelf_drop(int i, vec2 xz) {
	float o = cloud_storm_outflow[i];
	if (o <= 0.0) {
		return 0.0;
	}
	vec4 s = cloud_storms[i];
	vec2 d = xz - s.xy;
	float rr = length(d);
	if (rr < 1.0) {
		return 0.0;
	}
	float u = (rr - s.z * (1.5 + 0.04 * o)) / (0.6 * s.z);
	float arc = smoothstep(0.65, 0.92, dot(d, cloud_storm_dir) / rr);
	float wedge = smoothstep(-1.2, -0.1, u) * (1.0 - smoothstep(-0.1, 0.05, u));
	return 450.0 * min(o / 15.0, 1.0) * s.w * arc * wedge;
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
		// The shelf: one continuous wedge under the storm's own tower (cover_type_at() on the CPU).
		cover = max(cover, 0.95 * wc_shelf_drop(i, xz) / 450.0);
	}
	return vec2(cover, t);
}

// How high a storm's column reaches over xz, as a cloud type (storm_column_type() on the CPU).
float wc_storm_column(vec2 xz) {
	float t = 0.0;
	for (int i = 0; i < 8; i++) {
		if (i >= cloud_storm_count) {
			break;
		}
		vec4 s = cloud_storms[i];
		t = max(t, s.w * (1.0 - smoothstep(5.0, 5.6, length(xz - s.xy - cloud_high_dir * (3.2 * s.z)) / s.z)));
	}
	return t;
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
	for (int i = 0; i < 8; i++) {
		if (i >= cloud_storm_count) {
			break;
		}
		b -= wc_shelf_drop(i, xz);
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

// Stratocumulus (sc_share(), sc_cells_at() on the CPU, which explain them).
float wc_sc_share(float t) {
	return smoothstep(0.06, 0.14, t) * (1.0 - smoothstep(0.3, 0.42, t));
}

float wc_sc_cells(vec2 xz) {
	vec2 q0 = xz - cloud_drift;
	vec2 d = cloud_sc.xy;
	vec2 q = vec2(dot(q0, d) / (cloud_sc.w * cloud_sc.z), dot(q0, vec2(-d.y, d.x)) / cloud_sc.w);
	int gx = int(floor(q.x));
	int gz = int(floor(q.y));
	float f1 = 9.0;
	float f2 = 9.0;
	for (int j = -1; j <= 1; j++) {
		for (int i = -1; i <= 1; i++) {
			vec2 c = vec2(float(gx + i) + wc_hash2(gx + i + 4057, gz + j - 911), float(gz + j) + wc_hash2(gx + i - 6311, gz + j + 2207));
			float dist = length(q - c);
			if (dist < f1) {
				f2 = f1;
				f1 = dist;
			} else if (dist < f2) {
				f2 = dist;
			}
		}
	}
	return smoothstep(0.0, 0.7, f2 - f1);
}

// Density effectors (effector_at() on the CPU, which explains them).
float wc_effector(vec3 p) {
	float e = 0.0;
	for (int i = 0; i < 8; i++) {
		if (i >= cloud_effector_count) {
			break;
		}
		vec4 f = cloud_effectors[i];
		float d = distance(f.xyz, p);
		if (d < f.w) {
			e += cloud_effector_strength[i] * (1.0 - d / f.w);
		}
	}
	return clamp(e, -1.0, 1.0);
}

// Wave clouds (wave_extinction_at() on the CPU, which explains them). The field's four levels are its RGBA.
uniform sampler2D cloud_wave_field : filter_linear, repeat_disable;
uniform vec4 cloud_wave_rect = vec4(0.0); // corner x, z, size (m), texels across
uniform vec4 cloud_wave_levels = vec4(2000.0, 3500.0, 5000.0, 7000.0);
uniform vec4 cloud_wave_slab = vec4(0.0); // m, where wave cloud can form: round the mid layer (x..y), above (z..w)

float wc_wave_eta(vec3 p) {
	vec2 uv = (p.xz - cloud_wave_rect.xy) / cloud_wave_rect.z;
	if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
		return 0.0;
	}
	vec4 e = textureLod(cloud_wave_field, uv, 0.0);
	vec4 lv = cloud_wave_levels;
	if (p.y <= lv.x) {
		return e.x;
	} else if (p.y <= lv.y) {
		return mix(e.x, e.y, (p.y - lv.x) / (lv.y - lv.x));
	} else if (p.y <= lv.z) {
		return mix(e.y, e.z, (p.y - lv.y) / (lv.z - lv.y));
	} else if (p.y <= lv.w) {
		return mix(e.z, e.w, (p.y - lv.z) / (lv.w - lv.z));
	}
	return e.w;
}

float wc_wave_layer_rh(float n, float cover, float c) {
	float u = (n - (1.0 - cover)) / max(cover, 0.001);
	float rh = clamp(0.93 + 0.5 * u, 0.5, 0.93);
	if (c > 0.001) {
		rh = max(rh, 1.0 - 0.03 * (1.0 - c));
	}
	return rh;
}

// The mid and high layers' cover from a cover noise already read (wc_mid_cover(), wc_high_cover() without the read).
float wc_layer_cover_n(float n, float cover, vec2 xz, bool high) {
	float c = clamp((n - (1.0 - cover)) / max(cover, 0.001) * 1.5, 0.0, 1.0) * min(1.0, 2.0 * cover);
	for (int i = 0; i < cloud_storm_count; i++) {
		vec4 s = cloud_storms[i];
		vec2 centre = s.xy + cloud_high_dir * ((high ? 2.5 : 1.2) * s.z);
		vec2 d = xz - centre;
		c = 1.0 - (1.0 - c) * (1.0 - (high ? 1.0 : 0.6) * exp(-dot(d, d) / ((high ? 9.0 : 4.0) * s.z * s.z)) * s.w);
	}
	return c;
}

float wc_wave_depression(vec3 p) {
	float dz = p.y - cloud_air.z;
	float t = cloud_air.x - 0.0065 * dz;
	float tk = t + 273.15;
	float rh = clamp(cloud_air.y * (exp(-max(dz, 0.0) / 8000.0) - 0.02) / 0.98, 0.01, 1.0);
	float dep = -log(rh) * tk * tk / 5423.0;
	if (cloud_mid_cover > 0.001) {
		float n = wc_cover_noise_at(p.xz + vec2(4721.0, 8363.0) - (cloud_mid_drift - cloud_drift), cloud_mid_scale);
		float rh = wc_wave_layer_rh(n, cloud_mid_cover, wc_layer_cover_n(n, cloud_mid_cover, p.xz, false));
		float d = (p.y - cloud_mid_height) / (1.5 * cloud_mid_thickness + 300.0);
		float w = exp(-d * d) * min(1.0, 4.0 * cloud_mid_cover);
		float plates = 0.5 + 0.5 * sin(6.2831853 * (p.y - cloud_mid_height) / 350.0);
		dep = mix(dep, min(dep, -log(rh) * tk * tk / 5423.0 + 0.4 * plates), w);
	}
	if (cloud_high_cover > 0.001) {
		float n = wc_cover_noise_at(p.xz + vec2(9137.0, 3301.0) - (cloud_high_drift - cloud_drift), cloud_high_scale);
		float rh = wc_wave_layer_rh(n, cloud_high_cover, wc_layer_cover_n(n, cloud_high_cover, p.xz, true));
		float d = (p.y - cloud_high_height) / 800.0;
		float w = exp(-d * d) * min(1.0, 4.0 * cloud_high_cover);
		dep = mix(dep, min(dep, -log(rh) * tk * tk / 5423.0), w);
	}
	return dep;
}

// r_lift: metres past saturation (the lens's depth there, for the lighting's profile). p_jitter (m) roughens the
// saturation level a little (the sky's striations); the CPU leaves it out.
// The extinction of the water condensed x metres of lift past saturation, at p lifted by eta.
float wc_wave_sigma(vec3 p, float eta, float x) {
	float tc = cloud_air.x - 0.0065 * (p.y - cloud_air.z) - 0.0098 * eta;
	float rate = 2.0e-6 * exp(0.06 * max(tc, -40.0));
	float rho_air = 1.225 * exp(-p.y / 8500.0);
	float ice = clamp((-20.0 - tc) / 10.0, 0.0, 1.0);
	return 1.5 * rho_air * rate * x / mix(0.01, 917.0 * 30e-6, ice);
}

// p_dep >= 0: the dew point depression already read nearby (the sun samples reuse the step's; the moist layers
// vary over kilometres, so 40 to 160 m toward the sun changes it little).
float wc_wave_extinction_d(vec3 p, float p_jitter, float p_dep, out float r_lift, out float r_dep) {
	r_lift = 0.0;
	r_dep = p_dep;
	bool in_a = cloud_wave_slab.y > cloud_wave_slab.x && p.y >= cloud_wave_slab.x && p.y <= cloud_wave_slab.y;
	bool in_b = cloud_wave_slab.w > cloud_wave_slab.z && p.y >= cloud_wave_slab.z && p.y <= cloud_wave_slab.w;
	if (!in_a && !in_b) {
		return 0.0;
	}
	float eta = wc_wave_eta(p);
	if (eta <= 0.0) {
		return 0.0;
	}
	if (r_dep < 0.0) {
		r_dep = wc_wave_depression(p);
	}
	float x = eta - 125.0 * r_dep + p_jitter;
	if (x <= 0.0) {
		return 0.0;
	}
	r_lift = x;
	return wc_wave_sigma(p, eta, x);
}

float wc_wave_extinction(vec3 p, float p_jitter, out float r_lift) {
	float dep;
	return wc_wave_extinction_d(p, p_jitter, -1.0, r_lift, dep);
}

// Extinction (1/m) at p. p_detail erodes the edges with the detail noise (Nubis); the shadow bake
// and the CPU leave it out. p_step is the march's step (m): the noise is read at that scale.
// r_profile: the cloud's depth there before erosion, 0 at its edge to 1 at its core (Nubis Cubed's
// dimensional profile), which the lighting reads for dark edges, the inner glow and the ambient shell.
// Cumulus as single clouds (cells_at() on the CPU, which explains it).
float wc_cells(vec3 p, float b0, vec2 ct, float p_warp, out float r_hh, out vec4 r_local, out float r_decay) {
	r_hh = 0.0;
	r_decay = 0.0;
	r_local = vec4(0.0, 0.0, 0.0, 1.0);
	vec2 q = (p.xz - cloud_drift) / cloud_cell_size;
	ivec2 g = ivec2(floor(q));
	float top = wc_top_height(ct.y);
	float present = ct.x * (cloud_evolve >= 0.0 ? 2.9 : 2.2);
	float grow = mix(1.0, 1.3, smoothstep(0.3, 0.7, ct.x));
	float best = 0.0;
	for (int j = -1; j <= 1; j++) {
		for (int i = -1; i <= 1; i++) {
			int cx = g.x + i;
			int cz = g.y + j;
			// Each cloud's life (cells_at() on the CPU): its own clock, a cycle each cloud lifetime give or take a
			// fifth, offset per cell; each cycle draws a new cloud (position, size, presence) from the cell.
			float age = 0.5;
			int sx = cx;
			int sz = cz;
			if (cloud_evolve >= 0.0) {
				float ph = cloud_evolve * mix(0.8, 1.25, wc_hash2(cx + 911, cz - 37)) + wc_hash2(cx - 511, cz + 263);
				float kc = floor(ph);
				age = ph - kc;
				sx = cx + 7349 * int(kc);
				sz = cz - 3203 * int(kc);
			}
			// Present where the cover (read here, not at the cloud's centre) beats the cell's own draw, fading over a
			// margin: the cloud shrinks toward thinner cover instead of being cut off by a wall where it crosses it.
			float keep = clamp((present - wc_hash2(sx + 7919, sz - 4241)) * 4.0, 0.0, 1.0);
			if (keep <= 0.0) {
				continue;
			}
			vec2 centre = vec2(float(cx) + 0.15 + 0.7 * wc_hash2(sx, sz + 3571), float(cz) + 0.15 + 0.7 * wc_hash2(sx - 2713, sz));
			float size = pow(wc_hash2(sx + 1237, sz + 9109), 1.8);
			float r = min((0.2 + 0.55 * size) * grow, 0.95) * sqrt(keep) * mix(0.7, 1.0, smoothstep(0.0, 0.2, age));
			float u = length(q - centre) / r;
			if (u >= 1.0) {
				continue;
			}
			float hgt = min(top, mix(0.3, 1.0, smoothstep(0.0, 0.3, age)) * 2.0 * r * cloud_cell_size * mix(0.3, 1.1, smoothstep(0.2, 0.9, ct.y)) * mix(0.75, 1.25, wc_hash2(sx + 5, sz + 11)));
			float hh = (p.y - b0) / hgt;
			if (hh <= 0.0 || hh >= 1.0) {
				continue;
			}
			float k = (hh - 0.45) / 0.55;
			float rr = hh < 0.45 ? 1.0 : sqrt(max(1.0 - k * k, 0.0));
			rr *= mix(0.85, 1.0, smoothstep(0.0, 0.12, hh));
			float env = (rr - u * p_warp) / 0.6;
			for (int b = 0; b < 3; b++) {
				float a = TAU * wc_hash2(sx + 31 * b, sz + 577);
				float off = 0.3 * wc_hash2(sx - 97 * b, sz + 1499);
				vec2 cb = centre + vec2(cos(a), sin(a)) * (off * r);
				float rb = r * (0.45 + 0.15 * wc_hash2(sx + 401 * b, sz - 733));
				float yb = mix(0.5, 0.75, wc_hash2(sx - 61 * b, sz - 3331));
				float vb = 0.3 * rb / r + 0.1;
				vec2 dq = (q - cb) / rb;
				float dy = (hh - yb) / vb;
				float d = sqrt(dot(dq, dq) + dy * dy);
				env = max(env, (1.0 - d * p_warp) / 0.6);
			}
			// Born thin, then dissolving: the outline shrinks into the noise from the edges in.
			float decay = smoothstep(0.75, 1.0, age);
			env = clamp(env * smoothstep(0.0, 0.08, age) - 1.2 * decay, 0.0, 1.0);
			if (env > best) {
				r_decay = decay;
				best = env;
				r_hh = hh;
				// The cloud's own frame, its radius 1 across: turrets scale with the cloud. Offset by cell.
				r_local = vec4(vec3((q - centre).x / r, (p.y - b0) / (r * cloud_cell_size), (q - centre).y / r) + vec3(wc_hash2(sx + 17, sz + 29), 0.0, wc_hash2(sx + 43, sz + 71)) * 7.0, r * cloud_cell_size);
			}
		}
	}
	return best;
}

float wc_extinction_p(vec3 p, vec2 ct, bool p_detail, float p_step, out float r_profile) {
	r_profile = 0.0;
	vec3 p_unsheared = p;
	if (ct.x <= 0.001 && (cloud_effector_count == 0 || wc_effector(p) <= 0.0)) {
		return 0.0;
	}
	float b0 = wc_base_at(p.xz);
	// Under a storm's anvil the column reaches the storm's height, for its body only: the single cells there keep
	// their own type's height (raising the type made them a crowd of congestus as tall as the tower).
	float tcol = cloud_storm_count > 0 ? max(ct.y, wc_storm_column(p.xz)) : ct.y;
	if (p.y <= b0 || p.y >= b0 + wc_top_height(tcol)) {
		return 0.0;
	}
	float h = min((p.y - b0) / wc_top_height(ct.y), 1.0);
	float own = step(p.y, b0 + wc_top_height(ct.y)); // inside the cells' own layer
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
	// Single clouds carved by the shape noise; a deck is the continuous noise's (extinction_at on the CPU).
	float hc;
	vec4 loc;
	float decay;
	float env = wc_cells(p, b0, ct, 1.0, hc, loc, decay);
	float eff = cloud_effector_count > 0 ? wc_effector(p_unsheared) : 0.0;
	env = clamp(env + 1.5 * max(eff, 0.0), 0.0, 1.0);
	// Flat bases on one condensation level: below a cloud's lowest stretch (120 m) the noise and the turrets are
	// read at its top, so the outline drops straight to the base instead of rounding up into floating puffs.
	float y_lift = max(b0 + 120.0 - p.y, 0.0);
	float n = wc_shape(p + vec3(0.0, y_lift, 0.0), p_step);
	loc.y += y_lift / loc.w;
	hc = mix(h, hc, step(0.001, env));
	if (env > 0.0) {
		// Turrets: billows in the cloud's own frame (about four across it, finer octaves inside) cut into the envelope,
		// fully over its upper half and less down its sides, so the top is a cluster of rounded crests (cauliflower,
		// Neyret 1997's rising thermals) and the walls bulge. Cut into the envelope, before the coverage remap, they
		// move the outline; as an erosion after it they only roughened the skin of one round dome. The base stays flat.
		float lobes = textureLod(cloud_detail, loc.xyz * 0.4, max(log2(p_step / loc.w * 0.4 * 128.0), 0.0)).b;
		env = max(env - cloud_turret_strength * (1.0 - lobes) * mix(0.4, 1.0, smoothstep(0.15, 0.6, hc)) * smoothstep(0.0, 0.25, hc), 0.0);
	}
	float near_body = 0.0;
	float thin = 1.0;
	float body = cloud_storm_count > 0 ? wc_storm_body(p_unsheared, b0, n, p_step, near_body, thin) : 0.0; // storm_body_at() on the CPU
	float lay = 1.0 - smoothstep(0.22, 0.36, ct.y); // layered_share() on the CPU
	float cells = max(n - (1.0 - env), 0.0) / max(env, 0.001) * base * (1.0 - lay) * own;
	float sc = wc_sc_share(ct.y);
	float cell = 1.0; // rounded heaps, as on the CPU
	if (sc > 0.0) {
		float cv = clamp(wc_sc_cells(p.xz) + 0.5 * (n - 0.5), 0.0, 1.0);
		float ht = 0.25 + 0.75 * cv;
		float lift = 0.15 * (1.0 - cv);
		cell = mix(1.0, (1.0 - smoothstep(0.5 * ht, ht, h)) * smoothstep(lift, lift + 0.08, h) * (0.5 + 0.7 * cv), sc);
	}
	float nd = lay > 0.0 ? mix(n, (0.5 + 0.5 * n) * cell, lay) : n;
	float deck = own * max(nd * dome - (1.0 - ct.x), 0.0) / max(ct.x, 0.001) * base * smoothstep(mix(0.55, 0.15, lay), mix(0.85, 0.45, lay), ct.x);
	deck = sc > 0.0 ? mix(deck, min(deck * 3.0, 1.0), sc) : deck; // dense cell middles, as on the CPU
	// The shelf: a solid wedge from its lowered base up past the storm's base (shelf_drop() + 400 m), smooth under,
	// its upper face the noise's (extinction_at() on the CPU).
	float shelf = 0.0;
	float shelf_top = 0.0;
	for (int i = 0; i < 8; i++) {
		if (i >= cloud_storm_count) {
			break;
		}
		float dr = wc_shelf_drop(i, p_unsheared.xz);
		if (dr > shelf_top) {
			shelf_top = dr;
			shelf = dr / 450.0;
		}
	}
	float wedge_top = 0.4 * shelf_top + 300.0 + 1500.0 * (1.0 - shelf); // a sloping wedge (extinction_at() on the CPU)
	float wedge = shelf > 0.0 ? smoothstep(0.3, 0.6, shelf) * (1.0 - smoothstep(0.7, 1.0, (p.y - b0) / wedge_top + 0.25 * (n - 0.5))) : 0.0;
	// Inside the storm's body (and its shelf) their own outlines rule: single cells there would stand out of
	// them as loose puffs.
	float other = max(max(cells, deck) * (1.0 - max(near_body, shelf)), wedge);
	float shape = max(other, body) * (1.0 + min(eff, 0.0));
	bool storm_body = body > other; // its own density (extinction_at() on the CPU)
	// The tower keeps more of its envelope: its lobes are kilometre turrets, and the cumulus erosion at full strength
	// broke it into a heap of small puffs.
	// The shelf is laminar (lifted smoothly along the gust front): its face smooth, only lightly textured.
	float ero = storm_body && thin >= 1.0 ? 0.45 : (wedge > 0.0 && wedge >= other ? 0.3 : 1.0);
	r_profile = clamp(shape, 0.0, 1.0);
	if (p_detail && shape > 0.0) {
		// Nubis Cubed's up-rez (Schneider, Advances 2023): wisps (Curly-Alligator) low frequency inside and high
		// frequency at the thin edge; billows rounded at the edge and finer toward the core, eroding by one minus the
		// inverted Worley so the feature points stay as bulges (eroding by the Worley itself bored pits into the
		// surface, a cracked cotton look);
		// wispy under the cloud, billowing higher up and in towering cumulus.
		vec2 sxz = wc_rotate(p.xz - cloud_drift);
		vec3 qd = vec3(sxz.x, p.y, sxz.y) / cloud_shape_scale * 8.0 - vec3(0.0, cloud_churn.z, 0.0);
		// Warped by the detail's own coarse billows at a quarter of its frequency (domain warping), so the edge
		// tears into swirls at the scale of the cloud as well as within the texture's own curl.
		float lod_w = max(log2(p_step / texel), 0.0);
		vec2 warp = vec2(textureLod(cloud_detail, qd * 0.25 + vec3(0.31, 0.17, 0.73), lod_w).b, textureLod(cloud_detail, qd * 0.25 + vec3(0.59, 0.41, 0.11), lod_w).b) - 0.5;
		qd += vec3(warp.x, 0.25 * (warp.x + warp.y), warp.y) * mix(1.2, 0.6, clamp(h * 2.0, 0.0, 1.0));
		// Small turrets on the turrets: the world-scale coarse billows (cells about 100 to 400 m) erode the upper shell
		// a little more, so the crests cut into the envelope above (wc_cells' frame) carry smaller lobes of their own.
		float lobes = warp.x + 0.5;
		float e_big = 0.35 * cloud_turret_strength * (1.0 - lobes) * smoothstep(0.2, 0.7, hc) * mix(0.5, 1.0, cu);
		e_big *= ero;
		shape = clamp((shape - e_big) / max(1.0 - e_big, 0.05), 0.0, 1.0);
		// Mip by the footprint: an eighth of the shape's tile in 128 texels, so a texel is an eighth of the shape's
		// (2.9 m at the default scale), read at the step's footprint so far billows average instead of aliasing.
		vec4 dn = textureLod(cloud_detail, qd, max(log2(p_step / (texel * 0.125)), 0.0));
		float wispy = mix(dn.g, dn.r, r_profile);
		float billowy = 1.0 - mix(dn.b, dn.a, pow(r_profile, 0.25));
		float billow = clamp(hc * 4.0, 0.0, 1.0) * mix(0.6, 1.0, cu);
		// A dissolving cloud loses its billows to wisps (evaporating at the edges into the dry air it mixes with).
		billow *= 1.0 - 0.8 * decay;
		float e = cloud_detail_strength * mix(0.45 * wispy, 0.4 * billowy, billow) * (1.0 + 0.6 * decay) * ero;
		shape = clamp((shape - e) / (1.0 - e), 0.0, 1.0);
		// Sharpening (Nubis Cubed): thin density raised so each billow ends in a defined edge instead of a
		// long fade, more where the cloud is thin.
		shape = pow(shape, mix(cloud_edge_sharpness, 1.0, r_profile));
	}
	// Times the cover after the erosion (Schneider 2015; wc_extinction on the CPU).
	return shape * (storm_body ? 0.1 * thin : mix(0.4, 1.0, ct.x) * mix(0.04, 0.1, ct.y)) * cloud_density;
}

float wc_extinction(vec3 p, vec2 ct, bool p_detail, float p_step) {
	float profile;
	return wc_extinction_p(p, ct, p_detail, p_step, profile);
}
)";

} // namespace WeatherClouds

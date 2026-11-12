/**************************************************************************/
/*  weather_shelter.cpp                                                   */
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

#include "weather_shelter.h"

#include "core/math/math_funcs.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/os.h"

namespace WeatherPatches {

static inline float _hash(int p_x, int p_y) {
	uint32_t h = uint32_t(p_x & (PERIOD - 1)) * 1597334677u ^ uint32_t(p_y & (PERIOD - 1)) * 3812015801u;
	h ^= h >> 16;
	h *= 0x7feb352du;
	h ^= h >> 15;
	h *= 0x846ca68bu;
	h ^= h >> 16;
	return float(h >> 8) * (1.0f / 16777216.0f);
}

static inline float _value(Vector2 p) {
	const float fx = Math::floor(p.x);
	const float fy = Math::floor(p.y);
	const int ix = int(fx);
	const int iy = int(fy);
	const float tx = p.x - fx;
	const float ty = p.y - fy;
	const float ux = tx * tx * (3.0f - 2.0f * tx);
	const float uy = ty * ty * (3.0f - 2.0f * ty);
	const float a = _hash(ix, iy);
	const float b = _hash(ix + 1, iy);
	const float c = _hash(ix, iy + 1);
	const float d = _hash(ix + 1, iy + 1);
	return Math::lerp(Math::lerp(a, b, ux), Math::lerp(c, d, ux), uy);
}

float noise(Vector2 p_cells) {
	return 0.65f * _value(p_cells) + 0.35f * _value(p_cells * 2.0f + Vector2(17.0f, 17.0f));
}

} // namespace WeatherPatches

Rect2 WeatherShelter::rect() const {
	const float cs = cell_size();
	return Rect2(origin.x * cs, origin.y * cs, extent, extent);
}

bool WeatherShelter::needs_bake(Vector2 p_center, Vector2 p_direction) const {
	if (!valid || dirty) {
		return true;
	}
	const Vector2 c = rect().get_center();
	if (Math::abs(p_center.x - c.x) > extent * 0.125f || Math::abs(p_center.y - c.y) > extent * 0.125f) {
		return true;
	}
	// The lee follows the wind: rebake when it has turned by more than about 3 degrees.
	return p_direction.dot(baked_direction) < 0.9986f;
}

float WeatherShelter::open_profile(float p_z, float p_z0) {
	const float z0 = MAX(p_z0, 1e-5f);
	return Math::log(MAX(p_z, 1.5f * z0) / z0) / Math::log(10.0f / z0);
}

float WeatherShelter::factor(float p_z, float p_z0, const float p_cell[4]) {
	const float z = MAX(p_z, 0.01f);
	float f = 1.0f;
	const float hc = p_cell[0];
	const float a = p_cell[1];
	if (hc > 0.05f && a > 0.01f) {
		// Above a canopy the log law starts from a displaced ground d = 0.7 hc with roughness 0.1 hc
		// (Raupach 1994), meeting the open-ground wind at a blending height; inside it the wind falls off
		// exponentially from its top (Cionco 1965): u(z) = u(hc) exp(a (z / hc - 1)).
		const float zb = MAX(60.0f, 4.0f * hc);
		if (z < zb) {
			const float d = 0.7f * hc;
			const float z0c = 0.1f * hc;
			const float ub = open_profile(zb, p_z0);
			const float span = Math::log((zb - d) / z0c);
			float u;
			if (z >= hc) {
				u = ub * Math::log((z - d) / z0c) / span;
			} else {
				u = ub * Math::log((hc - d) / z0c) / span * Math::exp(a * (z / hc - 1.0f));
			}
			f = CLAMP(u / MAX(open_profile(z, p_z0), 1e-4f), 0.0f, 1.0f);
		}
	}
	const float s = p_cell[2];
	const float heff = p_cell[3];
	if (s < 1.0f && heff > 0.05f) {
		f *= Math::lerp(s, 1.0f, Math::smoothstep(heff, 2.0f * heff, z));
	}
	return CLAMP(f, 0.0f, 1.0f);
}

void WeatherShelter::sample(Vector2 p_xz, float r_cell[4]) const {
	r_cell[0] = 0.0f;
	r_cell[1] = 0.0f;
	r_cell[2] = 1.0f;
	r_cell[3] = 0.0f;
	if (!valid) {
		return;
	}
	const int n = resolution;
	const float cs = cell_size();
	// Cell centres, as a linearly filtered texture.
	const float gx = p_xz.x / cs - origin.x - 0.5f;
	const float gy = p_xz.y / cs - origin.y - 0.5f;
	if (gx < -0.5f || gy < -0.5f || gx > n - 0.5f || gy > n - 0.5f) {
		return;
	}
	const int x0 = CLAMP(int(Math::floor(gx)), 0, n - 1);
	const int y0 = CLAMP(int(Math::floor(gy)), 0, n - 1);
	const int x1 = MIN(x0 + 1, n - 1);
	const int y1 = MIN(y0 + 1, n - 1);
	const float tx = CLAMP(gx - x0, 0.0f, 1.0f);
	const float ty = CLAMP(gy - y0, 0.0f, 1.0f);
	const float *c = cells.ptr();
	for (int k = 0; k < 4; k++) {
		const float a = c[(y0 * n + x0) * 4 + k];
		const float b = c[(y0 * n + x1) * 4 + k];
		const float d = c[(y1 * n + x0) * 4 + k];
		const float e = c[(y1 * n + x1) * 4 + k];
		r_cell[k] = Math::lerp(Math::lerp(a, b, tx), Math::lerp(d, e, tx), ty);
	}
}

namespace {

struct LeePass {
	int n = 0;
	float cs = 0.0f;
	Vector2 dir;
	int steps = 0;
	const float *height = nullptr;
	float *cells = nullptr;

	void row(uint32_t p_row, void *) {
		const int y = int(p_row);
		for (int x = 0; x < n; x++) {
			float s = 1.0f;
			float heff = 0.0f;
			const float own = height[y * n + x];
			if (own > 0.0f) {
				s = WeatherShelter::LEE_MIN;
				heff = own;
			} else {
				for (int k = 1; k <= steps; k++) {
					const float dist = k * cs;
					const int ux = int(Math::round(x - dir.x * k));
					const int uy = int(Math::round(y - dir.y * k));
					if (ux < 0 || uy < 0 || ux >= n || uy >= n) {
						break;
					}
					const float h = height[uy * n + ux];
					if (h <= 0.0f || dist > WeatherShelter::LEE_REACH * h) {
						continue;
					}
					const float xr = MAX(dist - WeatherShelter::LEE_CAVITY * h, 0.0f) / (WeatherShelter::LEE_RECOVERY * h);
					const float r = WeatherShelter::LEE_MIN + (1.0f - WeatherShelter::LEE_MIN) * (1.0f - Math::exp(-xr));
					if (r < s) {
						s = r;
						heff = h;
					}
				}
			}
			cells[(y * n + x) * 4 + 2] = s;
			cells[(y * n + x) * 4 + 3] = heff;
		}
	}
};

} // namespace

void WeatherShelter::bake(Vector2 p_center, Vector2 p_direction, const LocalVector<AABB> &p_extra) {
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	const int n = resolution;
	const float cs = cell_size();
	origin = Vector2i(int(Math::floor(p_center.x / cs)) - n / 2, int(Math::floor(p_center.y / cs)) - n / 2);
	baked_direction = p_direction.length_squared() > 1e-8f ? p_direction.normalized() : Vector2(1, 0);
	cells.resize(n * n * 4);
	float *c = cells.ptrw();
	Vector<float> heights;
	heights.resize(n * n);
	float *h = heights.ptrw();
	const Vector2 corner(origin.x * cs, origin.y * cs);

	// Canopy: the painted image, then the stamps (the taller wins).
	const bool use_image = canopy_image.is_valid() && !canopy_image->is_empty() && canopy_rect.size.x > 0.0f && canopy_rect.size.y > 0.0f;
	const bool float_image = use_image && canopy_image->get_format() >= Image::FORMAT_RF && canopy_image->get_format() <= Image::FORMAT_RGBAH;
	const int iw = use_image ? canopy_image->get_width() : 0;
	const int ih = use_image ? canopy_image->get_height() : 0;
	for (int y = 0; y < n; y++) {
		for (int x = 0; x < n; x++) {
			const Vector2 p = corner + Vector2(x + 0.5f, y + 0.5f) * cs;
			float hc = 0.0f;
			float a = 0.0f;
			if (use_image) {
				const Vector2 uv = (p - canopy_rect.position) / canopy_rect.size;
				if (uv.x >= 0.0f && uv.y >= 0.0f && uv.x < 1.0f && uv.y < 1.0f) {
					const Color col = canopy_image->get_pixel(MIN(int(uv.x * iw), iw - 1), MIN(int(uv.y * ih), ih - 1));
					hc = float_image ? col.r : col.r * canopy_max_height;
					a = col.g * 4.0f;
				}
			}
			for (const Canopy &cp : canopies) {
				const float dist = p.distance_to(cp.center);
				if (dist >= cp.radius) {
					continue;
				}
				// The edge of a stand thins over its last tenth.
				const float e = Math::smoothstep(cp.radius, cp.radius * 0.9f, dist);
				const float ch = cp.height * e;
				if (ch > hc) {
					hc = ch;
					a = cp.density;
				}
			}
			c[(y * n + x) * 4 + 0] = hc;
			c[(y * n + x) * 4 + 1] = a;
			h[y * n + x] = 0.0f;
		}
	}

	// Obstacles: the height of their top above the ground, over the cells they cover.
	float hmax = 0.0f;
	auto add_box = [&](const AABB &p_box) {
		const float top = p_box.position.y + p_box.size.y - ground_height;
		if (top <= 0.0f) {
			return;
		}
		const int x0 = MAX(int(Math::floor((p_box.position.x - corner.x) / cs)), 0);
		const int y0 = MAX(int(Math::floor((p_box.position.z - corner.y) / cs)), 0);
		const int x1 = MIN(int(Math::ceil((p_box.position.x + p_box.size.x - corner.x) / cs)), n);
		const int y1 = MIN(int(Math::ceil((p_box.position.z + p_box.size.z - corner.y) / cs)), n);
		for (int y = y0; y < y1; y++) {
			for (int x = x0; x < x1; x++) {
				h[y * n + x] = MAX(h[y * n + x], top);
			}
		}
		hmax = MAX(hmax, top);
	};
	for (const Obstacle &o : obstacles) {
		add_box(o.box);
	}
	for (const AABB &box : p_extra) {
		add_box(box);
	}

	// The lee: march upwind from every cell and keep the strongest shelter.
	LeePass pass;
	pass.n = n;
	pass.cs = cs;
	pass.dir = baked_direction;
	pass.steps = hmax > 0.0f ? MIN(int(Math::ceil(LEE_REACH * hmax / cs)), n) : 0;
	pass.height = h;
	pass.cells = c;
	if (pass.steps > 0) {
		const WorkerThreadPool::GroupID g = WorkerThreadPool::get_singleton()->add_template_group_task(&pass, &LeePass::row, nullptr, n, -1, true, SNAME("WeatherShelter lee"));
		WorkerThreadPool::get_singleton()->wait_for_group_task_completion(g);
	} else {
		for (int i = 0; i < n * n; i++) {
			c[i * 4 + 2] = 1.0f;
			c[i * 4 + 3] = 0.0f;
		}
	}

	// Out to the texture.
	Vector<uint8_t> bytes;
	bytes.resize(n * n * 8);
	uint16_t *hb = reinterpret_cast<uint16_t *>(bytes.ptrw());
	for (int i = 0; i < n * n * 4; i++) {
		hb[i] = Math::make_half_float(c[i]);
	}
	if (image.is_null() || image->get_width() != n) {
		image = Image::create_from_data(n, n, false, Image::FORMAT_RGBAH, bytes);
		texture = ImageTexture::create_from_image(image);
	} else {
		image->set_data(n, n, false, Image::FORMAT_RGBAH, bytes);
		texture->update(image);
	}
	valid = true;
	dirty = false;
	bake_ms = float(OS::get_singleton()->get_ticks_usec() - t0) / 1000.0f;
}

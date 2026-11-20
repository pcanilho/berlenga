/**************************************************************************/
/*  weather_contact_wetness.cpp                                           */
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

#include "weather_contact_wetness.h"

#include "core/math/math_funcs.h"

void WeatherContactWetness::configure(int p_resolution, float p_size) {
	p_resolution = CLAMP(p_resolution, 16, 4096);
	p_size = MAX(p_size, 1.0f);
	if (p_resolution == resolution && Math::is_equal_approx(p_size, size)) {
		return;
	}
	resolution = p_resolution;
	size = p_size;
	wet.resize(resolution * resolution);
	wet.fill(0.0f);
	origin_valid = false;
	any_wet = false;
	wet_box.clear();
	bytes.resize(resolution * resolution);
	bytes.fill(0);
	_all_dirty();
	image.unref();
	texture.unref();
}

void WeatherContactWetness::follow(Vector3 p_center) {
	if (resolution <= 0) {
		return;
	}
	const float t = _texel();
	const Vector2i want(int(Math::floor(p_center.x / t)) - resolution / 2, int(Math::floor(p_center.z / t)) - resolution / 2);
	if (!origin_valid) {
		origin = want;
		origin_valid = true;
		_all_dirty();
		return;
	}
	// Recentre once the centre has moved an eighth of the square (keeps shifts rare).
	const Vector2i d = want - origin;
	if (Math::abs(d.x) < resolution / 8 && Math::abs(d.y) < resolution / 8) {
		return;
	}
	const int n = resolution;
	Vector<float> moved;
	moved.resize(n * n);
	moved.fill(0.0f);
	const float *src = wet.ptr();
	float *dst = moved.ptrw();
	// Only the wet box can hold water: it moves with the square, clipped to it.
	Box nb;
	if (any_wet && !wet_box.empty()) {
		nb.add(MAX(wet_box.x0 - d.x, 0), MAX(wet_box.z0 - d.y, 0), MIN(wet_box.x1 - d.x, n - 1), MIN(wet_box.z1 - d.y, n - 1));
	}
	bool any = false;
	if (!nb.empty()) {
		for (int z = nb.z0; z <= nb.z1; z++) {
			const int sz = z + d.y;
			for (int x = nb.x0; x <= nb.x1; x++) {
				const float v = src[sz * n + x + d.x];
				dst[z * n + x] = v;
				any = any || v > 0.0f;
			}
		}
	}
	wet = moved;
	origin = want;
	any_wet = any;
	wet_box = any ? nb : Box();
	_all_dirty();
}

Rect2 WeatherContactWetness::rect() const {
	if (resolution <= 0 || !origin_valid) {
		return Rect2();
	}
	const float t = _texel();
	return Rect2(origin.x * t, origin.y * t, size, size);
}

void WeatherContactWetness::add_disc(Vector3 p_position, float p_radius, float p_amount) {
	if (resolution <= 0 || !origin_valid || p_amount <= 0.0f) {
		return;
	}
	const float t = _texel();
	const float r = MAX(p_radius, 0.5f * t);
	const float a = MIN(p_amount, 1.0f);
	const float cx = p_position.x / t - origin.x - 0.5f;
	const float cz = p_position.z / t - origin.y - 0.5f;
	const float rt = r / t;
	const int x0 = MAX(int(Math::floor(cx - rt)), 0);
	const int x1 = MIN(int(Math::ceil(cx + rt)), resolution - 1);
	const int z0 = MAX(int(Math::floor(cz - rt)), 0);
	const int z1 = MIN(int(Math::ceil(cz + rt)), resolution - 1);
	if (x0 > x1 || z0 > z1) {
		return;
	}
	float *w = wet.ptrw();
	bool changed = false;
	for (int z = z0; z <= z1; z++) {
		for (int x = x0; x <= x1; x++) {
			const float q = Math::sqrt((x - cx) * (x - cx) + (z - cz) * (z - cz)) / MAX(rt, 1e-3f);
			if (q > 1.0f) {
				continue;
			}
			const float v = a * CLAMP((1.0f - q) * 4.0f, 0.0f, 1.0f);
			float &cell = w[z * resolution + x];
			if (v > cell) {
				cell = v;
				changed = true;
			}
		}
	}
	if (changed) {
		dirty = true;
		any_wet = true;
		wet_box.add(x0, z0, x1, z1);
		dirty_box.add(x0, z0, x1, z1);
	}
}

void WeatherContactWetness::add_grid(const float *p_values, int p_width, int p_height, Rect2 p_rect) {
	if (resolution <= 0 || !origin_valid || p_values == nullptr || p_width <= 0 || p_height <= 0 || p_rect.size.x <= 0.0f || p_rect.size.y <= 0.0f) {
		return;
	}
	const float t = _texel();
	const int x0 = MAX(int(Math::floor(p_rect.position.x / t)) - origin.x, 0);
	const int x1 = MIN(int(Math::ceil((p_rect.position.x + p_rect.size.x) / t)) - origin.x, resolution - 1);
	const int z0 = MAX(int(Math::floor(p_rect.position.y / t)) - origin.y, 0);
	const int z1 = MIN(int(Math::ceil((p_rect.position.y + p_rect.size.y) / t)) - origin.y, resolution - 1);
	// Bilinear between the grid's cell centres, so coarse cells leave soft edges, not stair steps.
	auto at = [&](int p_x, int p_z) { return p_values[CLAMP(p_z, 0, p_height - 1) * p_width + CLAMP(p_x, 0, p_width - 1)]; };
	if (x0 > x1 || z0 > z1) {
		return;
	}
	float *w = wet.ptrw();
	bool changed = false;
	for (int z = z0; z <= z1; z++) {
		const float wz = (origin.y + z + 0.5f) * t;
		const float gz = (wz - p_rect.position.y) / p_rect.size.y * p_height - 0.5f;
		if (gz < -0.5f || gz > p_height - 0.5f) {
			continue;
		}
		const int iz = int(Math::floor(gz));
		const float fz = gz - iz;
		for (int x = x0; x <= x1; x++) {
			const float wx = (origin.x + x + 0.5f) * t;
			const float gx = (wx - p_rect.position.x) / p_rect.size.x * p_width - 0.5f;
			if (gx < -0.5f || gx > p_width - 0.5f) {
				continue;
			}
			const int ix = int(Math::floor(gx));
			const float fx = gx - ix;
			const float v = MIN(Math::lerp(Math::lerp(at(ix, iz), at(ix + 1, iz), fx), Math::lerp(at(ix, iz + 1), at(ix + 1, iz + 1), fx), fz), 1.0f);
			float &cell = w[z * resolution + x];
			if (v > cell) {
				cell = v;
				changed = true;
			}
		}
	}
	if (changed) {
		dirty = true;
		any_wet = true;
		wet_box.add(x0, z0, x1, z1);
		dirty_box.add(x0, z0, x1, z1);
	}
}

void WeatherContactWetness::dry(float p_seconds, float p_dry_time) {
	if (!any_wet || p_seconds <= 0.0f) {
		return;
	}
	const float k = p_dry_time > 0.0f ? Math::exp(-p_seconds / p_dry_time) : 0.0f;
	if (k >= 1.0f) {
		return;
	}
	if (wet_box.empty()) {
		any_wet = false;
		return;
	}
	float *w = wet.ptrw();
	// Walk the wet box only, and shrink it to what is still wet.
	Box still;
	for (int z = wet_box.z0; z <= wet_box.z1; z++) {
		float *row = w + z * resolution;
		int rx0 = resolution, rx1 = -1;
		for (int x = wet_box.x0; x <= wet_box.x1; x++) {
			if (row[x] <= 0.0f) {
				continue;
			}
			float v = row[x] * k;
			if (v < 1.0f / 512.0f) {
				v = 0.0f; // (below what R8 shows)
			} else {
				rx0 = MIN(rx0, x);
				rx1 = MAX(rx1, x);
			}
			row[x] = v;
		}
		still.add(rx0, z, rx1, z);
	}
	dirty_box.add(wet_box.x0, wet_box.z0, wet_box.x1, wet_box.z1);
	wet_box = still;
	any_wet = !still.empty();
	dirty = true;
}

float WeatherContactWetness::sample(Vector3 p_position) const {
	if (resolution <= 0 || !origin_valid) {
		return 0.0f;
	}
	const float t = _texel();
	const int x = int(Math::floor(p_position.x / t)) - origin.x;
	const int z = int(Math::floor(p_position.z / t)) - origin.y;
	if (x < 0 || z < 0 || x >= resolution || z >= resolution) {
		return 0.0f;
	}
	return wet[z * resolution + x];
}

Ref<ImageTexture> WeatherContactWetness::upload() {
	if (resolution <= 0) {
		return texture;
	}
	if (!dirty && texture.is_valid()) {
		return texture;
	}
	// Requantise the changed box only; skip the GPU copy when no byte changed (slow drying moves a texel's byte
	// every few ticks, not every tick).
	bool changed = texture.is_null();
	if (!dirty_box.empty()) {
		uint8_t *b = bytes.ptrw();
		const float *w = wet.ptr();
		for (int z = dirty_box.z0; z <= dirty_box.z1; z++) {
			const int row = z * resolution;
			for (int x = dirty_box.x0; x <= dirty_box.x1; x++) {
				const uint8_t q = uint8_t(CLAMP(Math::round(w[row + x] * 255.0f), 0.0f, 255.0f));
				if (q != b[row + x]) {
					b[row + x] = q;
					changed = true;
				}
			}
		}
	}
	dirty_box.clear();
	dirty = false;
	if (!changed) {
		return texture;
	}
	if (image.is_null()) {
		image = Image::create_from_data(resolution, resolution, false, Image::FORMAT_R8, bytes);
	} else {
		image->set_data(resolution, resolution, false, Image::FORMAT_R8, bytes);
	}
	if (texture.is_null()) {
		texture = ImageTexture::create_from_image(image);
	} else {
		texture->update(image);
	}
	return texture;
}

/**************************************************************************/
/*  weather_terrain_wind.cpp                                              */
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

#include "weather_terrain_wind.h"

#include "core/math/math_funcs.h"
#include "core/os/os.h"

Rect2 WeatherTerrainWind::rect_of(Vector2i p_origin) const {
	const float cs = cell_size();
	return Rect2(p_origin.x * cs, p_origin.y * cs, extent, extent);
}

Rect2 WeatherTerrainWind::rect() const {
	return rect_of(origin);
}

bool WeatherTerrainWind::needs_heights(Vector2 p_center) const {
	if (dirty || !heights_valid) {
		return !is_sampling() || dirty;
	}
	const Vector2 c = rect().get_center();
	if (Math::abs(p_center.x - c.x) > extent * 0.125f || Math::abs(p_center.y - c.y) > extent * 0.125f) {
		// Already sampling round about here: let it finish.
		if (is_sampling()) {
			const Vector2 pc = rect_of(pending_origin).get_center();
			return Math::abs(p_center.x - pc.x) > extent * 0.125f || Math::abs(p_center.y - pc.y) > extent * 0.125f;
		}
		return true;
	}
	return false;
}

void WeatherTerrainWind::begin_heights(Vector2 p_center) {
	const int n = resolution;
	const float cs = cell_size();
	pending_origin = Vector2i(int(Math::floor(p_center.x / cs)) - n / 2, int(Math::floor(p_center.y / cs)) - n / 2);
	pending.resize(n * n);
	pending.fill(Math::NaN);
	pending_row = 0;
	dirty = false;
}

Vector2 WeatherTerrainWind::pending_cell_center(int p_x, int p_z) const {
	const float cs = cell_size();
	return Vector2((pending_origin.x + p_x + 0.5f) * cs, (pending_origin.y + p_z + 0.5f) * cs);
}

void WeatherTerrainWind::finish_heights() {
	heights = pending;
	origin = pending_origin;
	pending.clear();
	pending_row = -1;
	heights_valid = true;
	derived = false; // derive again for the new square
}

bool WeatherTerrainWind::needs_derive(Vector2 p_direction) const {
	if (!heights_valid) {
		return false;
	}
	// The turn and the lee follow the wind: derive again when it has turned by about 3 degrees.
	return !derived || p_direction.dot(baked_direction) < 0.9986f;
}

void WeatherTerrainWind::derive(Vector2 p_direction) {
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	const int n = resolution;
	const float cs = cell_size();
	const Vector2 d = p_direction.length_squared() > 1e-8f ? p_direction.normalized() : Vector2(1, 0);
	baked_direction = d;

	// Heights with holes filled by the mean of what answered, and the floor (sea surface) applied.
	LocalVector<float> z;
	z.resize(n * n);
	double sum = 0.0;
	int count = 0;
	for (int i = 0; i < n * n; i++) {
		const float h = heights[i];
		if (Math::is_finite(h)) {
			sum += MAX(h, floor);
			count++;
		}
	}
	derived = true;
	if (count == 0) {
		// Nothing answered (no terrain, no colliders): flat, and heights above the ground stay on ground_height.
		valid = false;
		return;
	}
	const float fill = float(sum / count);
	for (int i = 0; i < n * n; i++) {
		const float h = heights[i];
		z[i] = Math::is_finite(h) ? MAX(h, floor) : fill;
	}
	reference = z[(n / 2) * n + n / 2];
	auto at = [&](float p_x, float p_y) -> float {
		// Bilinear in cell units, clamped to the square.
		const float x = CLAMP(p_x, 0.0f, float(n - 1));
		const float y = CLAMP(p_y, 0.0f, float(n - 1));
		const int x0 = MIN(int(x), n - 2);
		const int y0 = MIN(int(y), n - 2);
		const float tx = x - x0;
		const float ty = y - y0;
		const float a = Math::lerp(z[y0 * n + x0], z[y0 * n + x0 + 1], tx);
		const float b = Math::lerp(z[(y0 + 1) * n + x0], z[(y0 + 1) * n + x0 + 1], tx);
		return Math::lerp(a, b, ty);
	};

	cells.resize(n * n * 4);
	lee.resize(n * n);
	float *c = cells.ptrw();
	float *lw = lee.ptrw();
	const float g = MAX(curvature_length, cs) / cs; // curvature length in cells
	const float lee_steps = MAX(1.0f, lee_reach / cs);
	const int lee_n = MIN(int(Math::ceil(lee_steps)), 64);
	for (int y = 0; y < n; y++) {
		for (int x = 0; x < n; x++) {
			const int i = y * n + x;
			const float h = z[i];
			// Slope from central differences over a cell either side.
			const float gx = (at(x + 1, y) - at(x - 1, y)) / (2.0f * cs);
			const float gz = (at(x, y + 1) - at(x, y - 1)) / (2.0f * cs);
			const Vector2 grad(gx, gz);
			const float slope = Math::atan(grad.length());
			// Curvature over the length scale and four times it, four directions each (Liston and Elder 2006, eq. 1);
			// the scale that stands out most sets the speed-up, so a broad hill and a small knoll both show.
			float curv = 0.0f;
			for (int sc = 0; sc < 2; sc++) {
				const float gs = g * (sc == 0 ? 1.0f : 4.0f);
				const float ds = gs / float(Math::SQRT2);
				const float l = gs * cs;
				const float c1 = 0.25f * ((h - 0.5f * (at(x - gs, y) + at(x + gs, y))) / (2.0f * l) + (h - 0.5f * (at(x, y - gs) + at(x, y + gs))) / (2.0f * l) + (h - 0.5f * (at(x - ds, y - ds) + at(x + ds, y + ds))) / (2.0f * float(Math::SQRT2) * l) + (h - 0.5f * (at(x - ds, y + ds) + at(x + ds, y - ds))) / (2.0f * float(Math::SQRT2) * l));
				if (Math::abs(c1) > Math::abs(curv)) {
					curv = c1;
				}
			}
			// The slope along the wind (rad, positive climbing) as MicroMet's slope term, +-0.5 at 45 degrees. The
			// curvature as linear theory over low hills has it (Jackson and Hunt 1975; Taylor and Lee 1984): a crest
			// speeds the wind up by about 2 H / L, which is about four times the curvature measured at the hill's
			// own scale; curvature_weight 0.5 gives that.
			const float along = Math::atan(grad.dot(d));
			const float ws = 0.5f * CLAMP(along / float(Math::PI * 0.25), -1.0f, 1.0f);
			const float wc = 8.0f * curv;
			float w = 1.0f + slope_weight * ws + curvature_weight * wc;
			// The diverting angle turns the wind toward the contours: removing the part of the wind that climbs
			// the slope turns it by 0.5 slope sin(2 phi) for small slopes, MicroMet's angle.
			Vector2 turned = d;
			if (grad.length_squared() > 1e-10f) {
				const Vector2 up = grad.normalized();
				turned = (d - up * (slope * d.dot(up))).normalized();
			}
			const float divert = d.angle_to(turned);
			// Sx: the steepest rise upwind within the reach (Winstral et al. 2002).
			float sx = -1.0f;
			for (int s = 1; s <= lee_n; s++) {
				const float dist = s * lee_reach / lee_n;
				const float u = at(x - d.x * dist / cs, y - d.y * dist / cs);
				sx = MAX(sx, Math::atan((u - h) / dist));
			}
			lw[i] = sx;
			w *= 1.0f - lee_strength * CLAMP((Math::rad_to_deg(sx) - 5.0f) / 20.0f, 0.0f, 1.0f);
			c[i * 4 + 0] = CLAMP(w, 0.3f, 2.0f);
			c[i * 4 + 1] = divert;
			c[i * 4 + 2] = grad.dot(turned);
			c[i * 4 + 3] = h - reference;
		}
	}
	valid = true;

	if (image.is_null() || image->get_width() != n) {
		image = Image::create_empty(n, n, false, Image::FORMAT_RGBAH);
	}
	{
		Vector<uint8_t> data;
		data.resize(n * n * 8);
		uint16_t *p = reinterpret_cast<uint16_t *>(data.ptrw());
		for (int i = 0; i < n * n * 4; i++) {
			p[i] = Math::make_half_float(c[i]);
		}
		image->set_data(n, n, false, Image::FORMAT_RGBAH, data);
	}
	if (texture.is_null() || texture->get_width() != n) {
		texture = ImageTexture::create_from_image(image);
	} else {
		texture->update(image);
	}
	derive_ms = (OS::get_singleton()->get_ticks_usec() - t0) / 1000.0f;
}

void WeatherTerrainWind::sample(Vector2 p_xz, float r_cell[4]) const {
	r_cell[0] = 1.0f;
	r_cell[1] = 0.0f;
	r_cell[2] = 0.0f;
	r_cell[3] = Math::NaN;
	if (!valid) {
		return;
	}
	const int n = resolution;
	const float cs = cell_size();
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
		const float a = Math::lerp(c[(y0 * n + x0) * 4 + k], c[(y0 * n + x1) * 4 + k], tx);
		const float b = Math::lerp(c[(y1 * n + x0) * 4 + k], c[(y1 * n + x1) * 4 + k], tx);
		r_cell[k] = Math::lerp(a, b, ty);
	}
	r_cell[3] += reference;
}

float WeatherTerrainWind::sample_lee(Vector2 p_xz) const {
	if (!valid) {
		return 0.0f;
	}
	const int n = resolution;
	const float cs = cell_size();
	const int x = int(Math::floor(p_xz.x / cs)) - origin.x;
	const int y = int(Math::floor(p_xz.y / cs)) - origin.y;
	if (x < 0 || y < 0 || x >= n || y >= n) {
		return 0.0f;
	}
	return lee[y * n + x];
}

Vector3 WeatherTerrainWind::apply(const float p_cell[4], Vector2 p_mean, float p_h, float p_length) {
	// The hill's inner layer: the change fades over about the length scale above the ground.
	const float fade = Math::exp(-MAX(p_h, 0.0f) / MAX(p_length, 1.0f));
	const float w = 1.0f + (p_cell[0] - 1.0f) * fade;
	const Vector2 m = p_mean.rotated(p_cell[1] * fade) * w;
	// Near the ground the air follows the surface: w = u . grad z.
	return Vector3(m.x, m.length() * p_cell[2] * fade, m.y);
}

String WeatherTerrainWind::get_shader_code() {
	String c;
	c += "global uniform sampler2D weather_terrain_map;\n";
	c += "global uniform vec4 weather_terrain_rect;\n";
	c += "global uniform vec4 weather_terrain_info; // reference height, curvature length, on, -\n";
	c += "\n";
	c += "// The terrain's speed weighting, diverting angle, rise along the wind and ground height at x, z.\n";
	c += "vec4 weather_terrain(vec2 xz) {\n";
	c += "	if (weather_terrain_info.z <= 0.0 || weather_terrain_rect.z <= 0.0) {\n";
	c += "		return vec4(1.0, 0.0, 0.0, -1e9);\n";
	c += "	}\n";
	c += "	vec2 u = (xz - weather_terrain_rect.xy) / weather_terrain_rect.zw;\n";
	c += "	if (u.x < 0.0 || u.y < 0.0 || u.x > 1.0 || u.y > 1.0) {\n";
	c += "		return vec4(1.0, 0.0, 0.0, -1e9);\n";
	c += "	}\n";
	c += "	vec4 t = textureLod(weather_terrain_map, u, 0.0);\n";
	c += "	t.w += weather_terrain_info.x;\n";
	c += "	return t;\n";
	c += "}\n\n";
	c += "vec3 weather_terrain_wind(vec4 t, vec2 mean, float h) {\n";
	c += "	float fade = exp(-max(h, 0.0) / max(weather_terrain_info.y, 1.0));\n";
	c += "	float w = 1.0 + (t.x - 1.0) * fade;\n";
	c += "	float a = t.y * fade;\n";
	c += "	vec2 m = vec2(mean.x * cos(a) - mean.y * sin(a), mean.x * sin(a) + mean.y * cos(a)) * w;\n";
	c += "	return vec3(m.x, length(m) * t.z * fade, m.y);\n";
	c += "}\n\n";
	return c;
}

/**************************************************************************/
/*  road_relief_3d.cpp                                                    */
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

#include "road_relief_3d.h"

#include "core/math/math_funcs.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "scene/3d/physics/collision_shape_3d.h"
#include "scene/3d/physics/static_body_3d.h"
#include "scene/resources/3d/concave_polygon_shape_3d.h"
#include "scene/resources/mesh.h"

namespace {

// PCG-style integer hash (Jarzynski and Olano 2020's pcg3d reduced to one output).
inline uint32_t rr_hash(uint32_t p_v) {
	uint32_t s = p_v * 747796405u + 2891336453u;
	uint32_t w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
	return (w >> 22u) ^ w;
}

inline uint32_t rr_hash3(int32_t p_x, int32_t p_z, uint32_t p_salt) {
	return rr_hash(uint32_t(p_x) * 73856093u ^ rr_hash(uint32_t(p_z) * 19349663u ^ rr_hash(p_salt)));
}

inline float rr_unit(uint32_t p_h) {
	return float(p_h & 0xFFFFFFu) / float(0x1000000);
}

inline float rr_smooth(float p_t) {
	return p_t * p_t * p_t * (p_t * (p_t * 6.0f - 15.0f) + 10.0f);
}

// Gradient noise (Perlin 2002's quintic fade) in -1..1, from world coordinates.
float rr_noise(float p_x, float p_z, uint32_t p_salt) {
	const float fx = Math::floor(p_x);
	const float fz = Math::floor(p_z);
	const int32_t ix = int32_t(fx);
	const int32_t iz = int32_t(fz);
	const float tx = p_x - fx;
	const float tz = p_z - fz;
	float c[4];
	for (int k = 0; k < 4; k++) {
		const int dx = k & 1;
		const int dz = k >> 1;
		const float a = rr_unit(rr_hash3(ix + dx, iz + dz, p_salt)) * float(Math::TAU);
		c[k] = Math::cos(a) * (tx - dx) + Math::sin(a) * (tz - dz);
	}
	const float u = rr_smooth(tx);
	const float v = rr_smooth(tz);
	return 1.41421356f * Math::lerp(Math::lerp(c[0], c[1], u), Math::lerp(c[2], c[3], u), v);
}

} // namespace

void RoadRelief3D::_build_waves() {
	// ISO 8608 harmonic superposition: bands log-spaced over wavelengths 0.12 to 3 m (longer ones are the sags'),
	// four random directions each, so a line across the surface in any direction carries about the class's
	// displacement spectrum Gd(n) = Gd(n0) (n / n0)^-2, n0 = 0.1 cycles/m.
	waves.clear();
	if (roughness_class == ROUGHNESS_NONE) {
		return;
	}
	const float gd0 = roughness_class == ROUGHNESS_A ? 16e-6f : (roughness_class == ROUGHNESS_B ? 64e-6f : 256e-6f);
	const int bands = 16;
	const int dirs = 4;
	const float n_lo = 1.0f / 3.0f;
	const float n_hi = 1.0f / 0.12f;
	const float r = Math::pow(n_hi / n_lo, 1.0f / float(bands));
	uint32_t h = rr_hash(uint32_t(seed) * 2654435761u + 977u);
	for (int b = 0; b < bands; b++) {
		const float n0 = n_lo * Math::pow(r, float(b));
		const float n1 = n0 * r;
		const float n = Math::sqrt(n0 * n1);
		const float gd = gd0 * (0.1f / n) * (0.1f / n);
		const float amp = Math::sqrt(2.0f * gd * (n1 - n0) / float(dirs));
		for (int d = 0; d < dirs; d++) {
			h = rr_hash(h);
			const float theta = rr_unit(h) * float(Math::PI);
			h = rr_hash(h);
			const float nn = n * (0.9f + 0.2f * rr_unit(h));
			h = rr_hash(h);
			Wave w;
			w.kx = float(Math::TAU) * nn * Math::cos(theta);
			w.kz = float(Math::TAU) * nn * Math::sin(theta);
			w.amp = amp;
			w.phase = rr_unit(h) * float(Math::TAU);
			waves.push_back(w);
		}
	}
}

float RoadRelief3D::_sag(float p_wx, float p_wz) const {
	// Settlement: two octaves of gradient noise, 24 and 9 m across.
	const uint32_t s = uint32_t(seed) * 31u + 5u;
	return sag_depth * (0.7f * rr_noise(p_wx / 24.0f, p_wz / 24.0f, s) + 0.3f * rr_noise(p_wx / 9.0f, p_wz / 9.0f, s + 1u));
}

float RoadRelief3D::_features(float p_wx, float p_wz, const Basis &p_to_road) const {
	float h = 0.0f;
	// Birdbaths: one candidate per 5 m world cell (a jittered grid), kept by the density; an ellipse of cosine-squared
	// profile, 0.5 to 1.6 m across its half axes.
	{
		const float c = 5.0f;
		const float keep = depression_density * c * c / 100.0f;
		const int32_t cx = int32_t(Math::floor(p_wx / c));
		const int32_t cz = int32_t(Math::floor(p_wz / c));
		for (int dz = -1; dz <= 1; dz++) {
			for (int dx = -1; dx <= 1; dx++) {
				uint32_t k = rr_hash3(cx + dx, cz + dz, uint32_t(seed) * 7u + 101u);
				if (rr_unit(k) >= keep) {
					continue;
				}
				k = rr_hash(k);
				const float ox = (float(cx + dx) + rr_unit(k)) * c;
				k = rr_hash(k);
				const float oz = (float(cz + dz) + rr_unit(k)) * c;
				k = rr_hash(k);
				const float a = 0.5f + 1.1f * rr_unit(k);
				k = rr_hash(k);
				const float b = a * (0.45f + 0.55f * rr_unit(k));
				k = rr_hash(k);
				const float ang = rr_unit(k) * float(Math::PI);
				k = rr_hash(k);
				const float depth = depression_depth * (0.35f + 0.65f * rr_unit(k));
				const float ex = p_wx - ox;
				const float ez = p_wz - oz;
				const float u = (ex * Math::cos(ang) + ez * Math::sin(ang)) / a;
				const float v = (-ex * Math::sin(ang) + ez * Math::cos(ang)) / b;
				const float rr = Math::sqrt(u * u + v * v);
				if (rr < 1.0f) {
					const float q = 0.5f + 0.5f * Math::cos(float(Math::PI) * rr);
					h -= depth * q * q;
				}
			}
		}
	}
	// Utility patches: one candidate per 12 m cell, a rectangle laid along the road, 1 to 3 m by 0.8 to 2 m, its
	// surface a few mm proud or sunk with a stepped edge and a slight sag.
	{
		const float c = 12.0f;
		const float keep = patch_density * c * c / 100.0f;
		const int32_t cx = int32_t(Math::floor(p_wx / c));
		const int32_t cz = int32_t(Math::floor(p_wz / c));
		for (int dz = -1; dz <= 1; dz++) {
			for (int dx = -1; dx <= 1; dx++) {
				uint32_t k = rr_hash3(cx + dx, cz + dz, uint32_t(seed) * 7u + 202u);
				if (rr_unit(k) >= keep) {
					continue;
				}
				k = rr_hash(k);
				const float ox = (float(cx + dx) + rr_unit(k)) * c;
				k = rr_hash(k);
				const float oz = (float(cz + dz) + rr_unit(k)) * c;
				k = rr_hash(k);
				const float hx = 0.5f + rr_unit(k);
				k = rr_hash(k);
				const float hz = 0.4f + 0.6f * rr_unit(k);
				k = rr_hash(k);
				const float step = (rr_unit(k) - 0.6f) * 0.012f;
				// In the road's frame (patches are cut square to the street): the offsets from the patch centre
				// along and across, from the point's world offset.
				const Vector3 l = p_to_road.xform(Vector3(p_wx - ox, 0.0f, p_wz - oz));
				const float u = Math::abs(float(l.x)) / hx;
				const float v = Math::abs(float(l.z)) / hz;
				if (u < 1.0f && v < 1.0f) {
					const float sag = (1.0f - u * u) * (1.0f - v * v);
					h += step - 0.003f * sag;
				}
			}
		}
	}
	// Potholes: one candidate per 15 m cell, 0.15 to 0.45 m across with a ragged rim, steep walls and a flat floor.
	{
		const float c = 15.0f;
		const float keep = pothole_density * c * c / 100.0f;
		const int32_t cx = int32_t(Math::floor(p_wx / c));
		const int32_t cz = int32_t(Math::floor(p_wz / c));
		for (int dz = -1; dz <= 1; dz++) {
			for (int dx = -1; dx <= 1; dx++) {
				uint32_t k = rr_hash3(cx + dx, cz + dz, uint32_t(seed) * 7u + 303u);
				if (rr_unit(k) >= keep) {
					continue;
				}
				k = rr_hash(k);
				const float ox = (float(cx + dx) + rr_unit(k)) * c;
				k = rr_hash(k);
				const float oz = (float(cz + dz) + rr_unit(k)) * c;
				k = rr_hash(k);
				const float rad = 0.15f + 0.3f * rr_unit(k);
				k = rr_hash(k);
				const float ph1 = rr_unit(k) * float(Math::TAU);
				k = rr_hash(k);
				const float ph2 = rr_unit(k) * float(Math::TAU);
				k = rr_hash(k);
				const float depth = pothole_depth * (0.6f + 0.4f * rr_unit(k));
				const float ex = p_wx - ox;
				const float ez = p_wz - oz;
				const float dist = Math::sqrt(ex * ex + ez * ez);
				if (dist > 1.6f * rad) {
					continue;
				}
				const float th = Math::atan2(ez, ex);
				const float rim = rad * (1.0f + 0.22f * Math::sin(3.0f * th + ph1) + 0.12f * Math::sin(5.0f * th + ph2));
				const float t = dist / MAX(rim, 1e-3f);
				if (t < 1.0f) {
					// Steep wall over the outer fifth, flat floor inside.
					const float w = CLAMP((1.0f - t) / 0.2f, 0.0f, 1.0f);
					h -= depth * rr_smooth(w);
				}
			}
		}
	}
	return h;
}

float RoadRelief3D::local_height(float p_x, float p_z) const {
	const float half = 0.5f * width;
	const float z = CLAMP(p_z, -half, half);
	const float x = CLAMP(p_x, 0.0f, length);
	// Cross section: a crown falling to both kerbs, or one slope falling to +Z, and the gutter pans.
	float h = one_way ? -cross_slope * (z + half) : -cross_slope * Math::abs(z);
	const float into_gutter = one_way ? (z - (half - gutter_width)) : (Math::abs(z) - (half - gutter_width));
	if (into_gutter > 0.0f && gutter_width > 0.0f) {
		const float g = CLAMP(into_gutter / gutter_width, 0.0f, 1.0f);
		h -= gutter_drop * rr_smooth(g);
	}
	const Transform3D xf = _relief_xform();
	const Vector3 wp = xf.xform(Vector3(x, 0.0f, z));
	const float wx = float(wp.x);
	const float wz = float(wp.z);
	// Ruts: two wheel paths per lane, each a dip with low shoulders of displaced asphalt (the W of shear rutting),
	// deepening and fading along the road and wandering a little sideways.
	if (traffic > 0.0f && rut_depth > 0.0f && lanes > 0) {
		const float lane_w = width / float(lanes);
		const uint32_t s = uint32_t(seed) * 13u + 17u;
		// Rutting varies with the asphalt's mix and compaction along the street: two octaves, 30 and 7 m.
		const float along = CLAMP(0.5f + 0.5f * (0.7f * rr_noise(wx / 30.0f + 3.1f, wz / 30.0f, s) + 0.45f * rr_noise(wx / 7.0f, wz / 7.0f, s + 2u)), 0.0f, 1.0f);
		const float depth = rut_depth * traffic * (0.2f + 0.8f * along);
		const float wander = 0.08f * rr_noise(wx / 12.0f, wz / 12.0f, s + 1u);
		const float sigma = 0.2f;
		for (int i = 0; i < lanes; i++) {
			const float lc = -half + lane_w * (float(i) + 0.5f);
			for (int side = -1; side <= 1; side += 2) {
				const float u = z - (lc + float(side) * 0.5f * MIN(track, lane_w - 0.4f) + wander);
				const float dip = Math::exp(-u * u / (2.0f * sigma * sigma));
				const float ua = Math::abs(u) - 2.3f * sigma;
				const float shoulder = Math::exp(-ua * ua / (2.0f * 0.36f * sigma * sigma));
				h -= depth * (dip - 0.25f * shoulder);
			}
		}
	}
	for (const Wave &w : waves) {
		h += w.amp * Math::cos(w.kx * wx + w.kz * wz + w.phase);
	}
	h += _sag(wx, wz);
	h += _features(wx, wz, xf.basis.inverse());
	h -= _inlet_dip(x, z);
	h += _height_map_at(x, z);
	return h;
}

float RoadRelief3D::_height_map_at(float p_x, float p_z) const {
	if (hm_w < 2 || hm_h < 2) {
		return 0.0f;
	}
	const float u = CLAMP(p_x / length, 0.0f, 1.0f) * float(hm_w - 1);
	const float v = CLAMP((p_z + 0.5f * width) / width, 0.0f, 1.0f) * float(hm_h - 1);
	const int i = MIN(int(u), hm_w - 2);
	const int j = MIN(int(v), hm_h - 2);
	const float fx = u - i;
	const float fz = v - j;
	const float *r0 = &hm[j * hm_w + i];
	const float *r1 = r0 + hm_w;
	return Math::lerp(Math::lerp(r0[0], r0[1], fx), Math::lerp(r1[0], r1[1], fx), fz);
}

void RoadRelief3D::set_height_map(const Ref<Image> &p_v) {
	height_map = p_v;
	hm.clear();
	hm_w = 0;
	hm_h = 0;
	if (p_v.is_valid() && !p_v->is_empty()) {
		Ref<Image> img = p_v;
		if (img->is_compressed() || img->get_format() != Image::FORMAT_RF) {
			img = p_v->duplicate();
			if (img->is_compressed()) {
				img->decompress();
			}
			img->convert(Image::FORMAT_RF);
		}
		hm_w = img->get_width();
		hm_h = img->get_height();
		hm.resize(hm_w * hm_h);
		const PackedByteArray data = img->get_data();
		memcpy(hm.ptr(), data.ptr(), sizeof(float) * hm.size());
	}
	_queue_build();
}

// A storm-drain inlet: a curb-and-grate inlet's grate, 0.9 m along and the gutter's width across, set 3 cm into the
// gutter pan with a short apron round it (a depressed gutter at the inlet, as highway drainage manuals have it: FHWA
// HEC-22, 2013, ch. 4), so water running down the gutter falls in.
static constexpr float INLET_LEN = 0.9f;
static constexpr float INLET_DROP = 0.03f;

Transform3D RoadRelief3D::_relief_xform() const {
	if (!is_inside_tree()) {
		return get_transform();
	}
	if (!relief_frame.is_empty()) {
		if (const Node3D *f = Object::cast_to<Node3D>(get_node_or_null(relief_frame))) {
			return f->get_global_transform().affine_inverse() * get_global_transform();
		}
	}
	return get_global_transform();
}

void RoadRelief3D::set_relief_frame(const NodePath &p_v) {
	relief_frame = p_v;
	_queue_build();
}

void RoadRelief3D::_resolve_inlets() {
	inlet_at.clear();
	if (!inlets.is_empty()) {
		for (const Vector2 &v : inlets) {
			inlet_at.push_back(Vector2(v.x, v.y < 0.0f ? -1.0f : 1.0f));
		}
		return;
	}
	if (inlet_spacing <= 0.0f || !is_inside_tree()) {
		return;
	}
	// Along the street, from the world (or relief_frame): the same inlets whatever pieces the street is laid in.
	const Transform3D xf = _relief_xform();
	const Vector3 dir = xf.basis.get_column(0).normalized();
	const float o = float(xf.origin.dot(dir));
	for (int side = -1; side <= 1; side += 2) {
		if (one_way && side < 0) {
			continue; // a one-way street falls to its +Z kerb: its gutter is there
		}
		const int k0 = int(Math::floor(o / inlet_spacing)) - 1;
		const int k1 = int(Math::floor((o + length) / inlet_spacing)) + 1;
		for (int k = k0; k <= k1; k++) {
			const float jitter = (rr_unit(rr_hash3(k, side, uint32_t(seed) * 7u + 401u)) - 0.5f) * 0.5f * inlet_spacing;
			const float x = (float(k) + 0.5f) * inlet_spacing + jitter - o;
			if (x > INLET_LEN && x < length - INLET_LEN) {
				inlet_at.push_back(Vector2(x, float(side)));
			}
		}
	}
}

float RoadRelief3D::_inlet_dip(float p_x, float p_z) const {
	if (inlet_at.is_empty() || gutter_width <= 0.0f) {
		return 0.0f;
	}
	const float half = 0.5f * width;
	float dip = 0.0f;
	for (const Vector2 &in : inlet_at) {
		const float into = in.y > 0.0f ? p_z - (half - gutter_width) : (-half + gutter_width) - p_z;
		if (into < -0.25f) {
			continue;
		}
		const float ax = Math::abs(p_x - in.x) - 0.5f * INLET_LEN;
		if (ax > 0.4f) {
			continue;
		}
		// The grate flat at the bottom, the apron sloping up over 0.4 m along and 0.25 m into the road.
		const float fx = 1.0f - rr_smooth(CLAMP(ax / 0.4f, 0.0f, 1.0f));
		const float fz = 1.0f - rr_smooth(CLAMP(-into / 0.25f, 0.0f, 1.0f));
		dip = MAX(dip, INLET_DROP * fx * fz);
	}
	return dip;
}

// Priority-Flood (Barnes, Lehman and Mulla 2014): from the outlets inward in order of height, each cell raised to the
// lowest spill level that reaches it; the water a full relief holds is that level less the bed.
void RoadRelief3D::_flood(const LocalVector<float> &p_h, int p_nx, int p_nz, float p_dx, float p_dz) {
	const int n = p_nx * p_nz;
	pond.resize(n);
	inlet_mask.resize(n);
	for (int k = 0; k < n; k++) {
		inlet_mask[k] = 0;
	}
	const float half = 0.5f * width;
	for (const Vector2 &in : inlet_at) {
		const int i0 = MAX(int(Math::floor((in.x - 0.5f * INLET_LEN) / p_dx)), 0);
		const int i1 = MIN(int(Math::ceil((in.x + 0.5f * INLET_LEN) / p_dx)), p_nx - 1);
		for (int j = 0; j < p_nz; j++) {
			const float z = -half + j * p_dz;
			const bool in_pan = in.y > 0.0f ? z >= half - gutter_width : z <= -half + gutter_width;
			if (in_pan) {
				for (int i = i0; i <= i1; i++) {
					inlet_mask[j * p_nx + i] = 1;
				}
			}
		}
	}
	struct Item {
		float h;
		int k;
	};
	LocalVector<Item> heap;
	LocalVector<uint8_t> done;
	done.resize(n);
	auto less = [](const Item &a, const Item &b) { return a.h > b.h; }; // a min-heap
	auto push = [&](int k, float h) {
		heap.push_back({ h, k });
		int c = int(heap.size()) - 1;
		while (c > 0) {
			const int p = (c - 1) / 2;
			if (!less(heap[p], heap[c])) {
				break;
			}
			SWAP(heap[p], heap[c]);
			c = p;
		}
	};
	auto pop = [&]() {
		const Item top = heap[0];
		heap[0] = heap[heap.size() - 1];
		heap.resize(heap.size() - 1);
		int c = 0;
		const int m = int(heap.size());
		while (true) {
			const int l = 2 * c + 1;
			const int r = l + 1;
			int b = c;
			if (l < m && less(heap[b], heap[l])) {
				b = l;
			}
			if (r < m && less(heap[b], heap[r])) {
				b = r;
			}
			if (b == c) {
				break;
			}
			SWAP(heap[b], heap[c]);
			c = b;
		}
		return top;
	};
	LocalVector<float> fill;
	fill.resize(n);
	for (int k = 0; k < n; k++) {
		const int i = k % p_nx;
		done[k] = 0;
		if (i == 0 || i == p_nx - 1 || inlet_mask[k]) {
			done[k] = 1;
			fill[k] = p_h[k];
			push(k, p_h[k]);
		}
	}
	while (!heap.is_empty()) {
		const Item it = pop();
		const int i = it.k % p_nx;
		const int j = it.k / p_nx;
		const int nb[4][2] = { { i - 1, j }, { i + 1, j }, { i, j - 1 }, { i, j + 1 } };
		for (const auto &q : nb) {
			if (q[0] < 0 || q[1] < 0 || q[0] >= p_nx || q[1] >= p_nz) {
				continue;
			}
			const int kq = q[1] * p_nx + q[0];
			if (done[kq]) {
				continue;
			}
			done[kq] = 1;
			fill[kq] = MAX(p_h[kq], it.h);
			push(kq, fill[kq]);
		}
	}
	for (int k = 0; k < n; k++) {
		pond[k] = fill[k] - p_h[k];
	}
}

bool RoadRelief3D::_grid_point(const Vector3 &p_world, float &r_u, float &r_v) const {
	if (!is_inside_tree() || grid_nx < 2 || grid_nz < 2) {
		return false;
	}
	const Vector3 l = get_global_transform().affine_inverse().xform(p_world);
	if (l.x < 0.0f || l.x > length || Math::abs(float(l.z)) > 0.5f * width) {
		return false;
	}
	r_u = CLAMP(float(l.x) / grid_dx, 0.0f, float(grid_nx - 1));
	r_v = CLAMP((float(l.z) + 0.5f * width) / grid_dz, 0.0f, float(grid_nz - 1));
	return true;
}

float RoadRelief3D::get_pond_depth_at(const Vector3 &p_world) const {
	float u, v;
	if (pond.size() != uint32_t(grid_nx * grid_nz) || !_grid_point(p_world, u, v)) {
		return 0.0f;
	}
	const int i = MIN(int(u), grid_nx - 2);
	const int j = MIN(int(v), grid_nz - 2);
	const float fx = u - i;
	const float fz = v - j;
	const float *r0 = &pond[j * grid_nx + i];
	const float *r1 = r0 + grid_nx;
	return Math::lerp(Math::lerp(r0[0], r0[1], fx), Math::lerp(r1[0], r1[1], fx), fz);
}

bool RoadRelief3D::is_inlet_at(const Vector3 &p_world) const {
	float u, v;
	if (inlet_mask.size() != uint32_t(grid_nx * grid_nz) || !_grid_point(p_world, u, v)) {
		return false;
	}
	return inlet_mask[int(Math::round(v)) * grid_nx + int(Math::round(u))] != 0;
}

bool RoadRelief3D::contains(const Vector3 &p_world) const {
	const Vector3 l = get_global_transform().affine_inverse().xform(p_world);
	return l.x >= 0.0f && l.x <= length && Math::abs(float(l.z)) <= 0.5f * width;
}

float RoadRelief3D::get_height_at(const Vector3 &p_world) const {
	if (!is_inside_tree()) {
		return -Math::INF;
	}
	const Transform3D xf = get_global_transform();
	// The surface's y above the world point: the road's frame may be tilted (a hill street), so step to the surface
	// point straight above in two fixed-point passes.
	Vector3 l = xf.affine_inverse().xform(p_world);
	if (l.x < 0.0f || l.x > length || Math::abs(float(l.z)) > 0.5f * width) {
		return -Math::INF;
	}
	Vector3 s;
	for (int i = 0; i < 3; i++) {
		s = xf.xform(Vector3(l.x, local_height(float(l.x), float(l.z)), l.z));
		l = xf.affine_inverse().xform(Vector3(p_world.x, s.y, p_world.z));
	}
	return float(s.y);
}

float RoadRelief3D::_grid_height(float p_x, float p_z) const {
	const float u = CLAMP(p_x / grid_dx, 0.0f, float(grid_nx - 1));
	const float v = CLAMP((p_z + 0.5f * width) / grid_dz, 0.0f, float(grid_nz - 1));
	const int i = MIN(int(u), grid_nx - 2);
	const int j = MIN(int(v), grid_nz - 2);
	const float fx = u - i;
	const float fz = v - j;
	const float *r0 = &grid[j * grid_nx + i];
	const float *r1 = r0 + grid_nx;
	// The quad's two triangles, split along (i, j)-(i + 1, j + 1) as the mesh is.
	if (fx >= fz) {
		return r0[0] + fx * (r0[1] - r0[0]) + fz * (r1[1] - r0[1]);
	}
	return r0[0] + fz * (r1[0] - r0[0]) + fx * (r1[1] - r1[0]);
}

float RoadRelief3D::get_surface_height_at(const Vector3 &p_world) const {
	if (!is_inside_tree() || grid_nx < 2 || grid_nz < 2) {
		return -Math::INF;
	}
	const Transform3D xf = get_global_transform();
	const Transform3D inv = xf.affine_inverse();
	Vector3 l = inv.xform(p_world);
	if (l.x < 0.0f || l.x > length || Math::abs(float(l.z)) > 0.5f * width) {
		return -Math::INF;
	}
	Vector3 s;
	for (int i = 0; i < 2; i++) {
		s = xf.xform(Vector3(l.x, _grid_height(float(l.x), float(l.z)), l.z));
		l = inv.xform(Vector3(p_world.x, s.y, p_world.z));
	}
	return float(s.y);
}

void RoadRelief3D::_queue_build() {
	if (dirty) {
		return;
	}
	dirty = true;
	if (is_inside_tree()) {
		callable_mp(this, &RoadRelief3D::_build).call_deferred();
	}
}

void RoadRelief3D::rebuild() {
	dirty = true;
	_build();
}

void RoadRelief3D::_build() {
	if (!dirty || !is_inside_tree()) {
		return;
	}
	dirty = false;
	built_xform = _relief_xform();
	_build_waves();
	_resolve_inlets();
	const int nx = MAX(int(Math::ceil(length / cell)), 1) + 1;
	const int nz = MAX(int(Math::ceil(width / cell)), 1) + 1;
	const float dx = length / float(nx - 1);
	const float dz = width / float(nz - 1);
	LocalVector<float> hs;
	hs.resize(nx * nz);
	for (int j = 0; j < nz; j++) {
		for (int i = 0; i < nx; i++) {
			hs[j * nx + i] = local_height(i * dx, -0.5f * width + j * dz);
		}
	}
	_flood(hs, nx, nz, dx, dz);
	grid = hs;
	grid_nx = nx;
	grid_nz = nz;
	grid_dx = dx;
	grid_dz = dz;
	PackedVector3Array verts;
	PackedVector3Array normals;
	PackedFloat32Array tangents;
	PackedVector2Array uvs;
	PackedInt32Array idx;
	verts.resize(nx * nz);
	normals.resize(nx * nz);
	tangents.resize(nx * nz * 4);
	uvs.resize(nx * nz);
	for (int j = 0; j < nz; j++) {
		for (int i = 0; i < nx; i++) {
			const int k = j * nx + i;
			const float h = hs[k];
			const float hxm = hs[j * nx + MAX(i - 1, 0)];
			const float hxp = hs[j * nx + MIN(i + 1, nx - 1)];
			const float hzm = hs[MAX(j - 1, 0) * nx + i];
			const float hzp = hs[MIN(j + 1, nz - 1) * nx + i];
			const float gx = (hxp - hxm) / (dx * float(MIN(i + 1, nx - 1) - MAX(i - 1, 0)));
			const float gz = (hzp - hzm) / (dz * float(MIN(j + 1, nz - 1) - MAX(j - 1, 0)));
			verts.write[k] = Vector3(i * dx, h, -0.5f * width + j * dz);
			normals.write[k] = Vector3(-gx, 1.0f, -gz).normalized();
			const Vector3 t = Vector3(1.0f, gx, 0.0f).normalized();
			tangents.write[k * 4 + 0] = t.x;
			tangents.write[k * 4 + 1] = t.y;
			tangents.write[k * 4 + 2] = t.z;
			tangents.write[k * 4 + 3] = 1.0f;
			uvs.write[k] = Vector2(i * dx, j * dz); // metres along, across from the -Z kerb (as Kit.road's uv2)
		}
	}
	idx.resize((nx - 1) * (nz - 1) * 6);
	int w = 0;
	for (int j = 0; j < nz - 1; j++) {
		for (int i = 0; i < nx - 1; i++) {
			const int a = j * nx + i;
			const int b = a + 1;
			const int c = a + nx + 1;
			const int d = a + nx;
			// Godot's front faces wind clockwise seen from their side (here from above).
			idx.write[w++] = a;
			idx.write[w++] = b;
			idx.write[w++] = c;
			idx.write[w++] = a;
			idx.write[w++] = c;
			idx.write[w++] = d;
		}
	}
	Array arr;
	arr.resize(Mesh::ARRAY_MAX);
	arr[Mesh::ARRAY_VERTEX] = verts;
	arr[Mesh::ARRAY_NORMAL] = normals;
	arr[Mesh::ARRAY_TANGENT] = tangents;
	arr[Mesh::ARRAY_TEX_UV] = uvs;
	arr[Mesh::ARRAY_TEX_UV2] = uvs;
	arr[Mesh::ARRAY_INDEX] = idx;
	Ref<ArrayMesh> road_mesh;
	road_mesh.instantiate();
	road_mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arr);
	set_mesh(road_mesh);

	if (collision) {
		if (body == nullptr) {
			body = memnew(StaticBody3D);
			CollisionShape3D *cs = memnew(CollisionShape3D);
			body->add_child(cs);
			add_child(body, false, INTERNAL_MODE_BACK);
		}
		body->set_collision_layer(collision_layer);
		PackedVector3Array faces;
		faces.resize(idx.size());
		for (int i = 0; i < idx.size(); i++) {
			faces.write[i] = verts[idx[i]];
		}
		Ref<ConcavePolygonShape3D> shape;
		shape.instantiate();
		shape->set_faces(faces);
		Object::cast_to<CollisionShape3D>(body->get_child(0))->set_shape(shape);
	} else if (body) {
		body->queue_free();
		body = nullptr;
	}
}

void RoadRelief3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			set_notify_transform(true);
			dirty = true;
			_build();
		} break;
		case NOTIFICATION_TRANSFORM_CHANGED: {
			// The relief follows the world (or relief_frame): a moved road is a different stretch of street. One
			// that only moves with its frame (a street sliding past a train window) keeps its relief.
			if (!_relief_xform().is_equal_approx(built_xform)) {
				_queue_build();
			}
		} break;
	}
}

#define RR_SETTER(m_name, m_member, m_expr) \
	void RoadRelief3D::set_##m_name(float p_v) { \
		m_member = (m_expr); \
		_queue_build(); \
	}

RR_SETTER(length, length, MAX(p_v, 0.1f))
RR_SETTER(width, width, MAX(p_v, 0.5f))
RR_SETTER(cross_slope, cross_slope, CLAMP(p_v, 0.0f, 0.1f))
RR_SETTER(gutter_width, gutter_width, CLAMP(p_v, 0.0f, 1.0f))
RR_SETTER(gutter_drop, gutter_drop, CLAMP(p_v, 0.0f, 0.1f))
RR_SETTER(traffic, traffic, CLAMP(p_v, 0.0f, 1.0f))
RR_SETTER(rut_depth, rut_depth, CLAMP(p_v, 0.0f, 0.05f))
RR_SETTER(track, track, CLAMP(p_v, 1.0f, 2.5f))
RR_SETTER(sag_depth, sag_depth, CLAMP(p_v, 0.0f, 0.05f))
RR_SETTER(depression_density, depression_density, CLAMP(p_v, 0.0f, 4.0f))
RR_SETTER(depression_depth, depression_depth, CLAMP(p_v, 0.0f, 0.05f))
RR_SETTER(patch_density, patch_density, CLAMP(p_v, 0.0f, 0.69f))
RR_SETTER(pothole_density, pothole_density, CLAMP(p_v, 0.0f, 0.44f))
RR_SETTER(pothole_depth, pothole_depth, CLAMP(p_v, 0.0f, 0.12f))
RR_SETTER(cell, cell, CLAMP(p_v, 0.02f, 1.0f))
RR_SETTER(inlet_spacing, inlet_spacing, CLAMP(p_v, 0.0f, 500.0f))

void RoadRelief3D::set_inlets(const PackedVector2Array &p_v) {
	inlets = p_v;
	_queue_build();
}

#undef RR_SETTER

void RoadRelief3D::set_lanes(int p_v) {
	lanes = CLAMP(p_v, 0, 8);
	_queue_build();
}

void RoadRelief3D::set_one_way(bool p_v) {
	one_way = p_v;
	_queue_build();
}

void RoadRelief3D::set_roughness_class(RoughnessClass p_v) {
	roughness_class = p_v;
	_queue_build();
}

void RoadRelief3D::set_seed(int p_v) {
	seed = p_v;
	_queue_build();
}

void RoadRelief3D::set_collision(bool p_v) {
	collision = p_v;
	_queue_build();
}

void RoadRelief3D::set_collision_layer(uint32_t p_v) {
	collision_layer = p_v;
	if (body) {
		body->set_collision_layer(collision_layer);
	}
}

void RoadRelief3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("local_height", "x", "z"), &RoadRelief3D::local_height);
	ClassDB::bind_method(D_METHOD("get_height_at", "world_position"), &RoadRelief3D::get_height_at);
	ClassDB::bind_method(D_METHOD("get_surface_height_at", "world_position"), &RoadRelief3D::get_surface_height_at);
	ClassDB::bind_method(D_METHOD("set_relief_frame", "path"), &RoadRelief3D::set_relief_frame);
	ClassDB::bind_method(D_METHOD("get_relief_frame"), &RoadRelief3D::get_relief_frame);
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "relief_frame", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D"), "set_relief_frame", "get_relief_frame");
	ClassDB::bind_method(D_METHOD("get_pond_depth_at", "world_position"), &RoadRelief3D::get_pond_depth_at);
	ClassDB::bind_method(D_METHOD("is_inlet_at", "world_position"), &RoadRelief3D::is_inlet_at);
	ClassDB::bind_method(D_METHOD("contains", "world_position"), &RoadRelief3D::contains);
	ClassDB::bind_method(D_METHOD("rebuild"), &RoadRelief3D::rebuild);
	ClassDB::bind_method(D_METHOD("set_height_map", "image"), &RoadRelief3D::set_height_map);
	ClassDB::bind_method(D_METHOD("get_height_map"), &RoadRelief3D::get_height_map);

#define RR_BIND(m_name, m_get, m_type, m_hint, m_hint_str)                                                   \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &RoadRelief3D::set_##m_name);                   \
	ClassDB::bind_method(D_METHOD(#m_get), &RoadRelief3D::m_get);                                           \
	ADD_PROPERTY(PropertyInfo(Variant::m_type, #m_name, m_hint, m_hint_str), "set_" #m_name, #m_get);

	ADD_GROUP("Street", "");
	RR_BIND(length, get_length, FLOAT, PROPERTY_HINT_RANGE, "0.1,500,0.1,or_greater,suffix:m")
	RR_BIND(width, get_width, FLOAT, PROPERTY_HINT_RANGE, "0.5,40,0.1,suffix:m")
	RR_BIND(lanes, get_lanes, INT, PROPERTY_HINT_RANGE, "0,8,1")
	RR_BIND(one_way, is_one_way, BOOL, PROPERTY_HINT_NONE, "")
	RR_BIND(cross_slope, get_cross_slope, FLOAT, PROPERTY_HINT_RANGE, "0,0.1,0.001")
	RR_BIND(gutter_width, get_gutter_width, FLOAT, PROPERTY_HINT_RANGE, "0,1,0.01,suffix:m")
	RR_BIND(gutter_drop, get_gutter_drop, FLOAT, PROPERTY_HINT_RANGE, "0,0.1,0.001,suffix:m")
	ADD_GROUP("Wear", "");
	RR_BIND(traffic, get_traffic, FLOAT, PROPERTY_HINT_RANGE, "0,1,0.01")
	RR_BIND(rut_depth, get_rut_depth, FLOAT, PROPERTY_HINT_RANGE, "0,0.05,0.001,suffix:m")
	RR_BIND(track, get_track, FLOAT, PROPERTY_HINT_RANGE, "1,2.5,0.01,suffix:m")
	RR_BIND(roughness_class, get_roughness_class, INT, PROPERTY_HINT_ENUM, "A (Very Good),B (Good),C (Average),None")
	RR_BIND(sag_depth, get_sag_depth, FLOAT, PROPERTY_HINT_RANGE, "0,0.05,0.001,suffix:m")
	RR_BIND(depression_density, get_depression_density, FLOAT, PROPERTY_HINT_RANGE, "0,4,0.01")
	RR_BIND(depression_depth, get_depression_depth, FLOAT, PROPERTY_HINT_RANGE, "0,0.05,0.001,suffix:m")
	RR_BIND(patch_density, get_patch_density, FLOAT, PROPERTY_HINT_RANGE, "0,0.69,0.01")
	RR_BIND(pothole_density, get_pothole_density, FLOAT, PROPERTY_HINT_RANGE, "0,0.44,0.01")
	RR_BIND(pothole_depth, get_pothole_depth, FLOAT, PROPERTY_HINT_RANGE, "0,0.12,0.001,suffix:m")
	RR_BIND(seed, get_seed, INT, PROPERTY_HINT_NONE, "")
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "height_map", PROPERTY_HINT_RESOURCE_TYPE, "Image"), "set_height_map", "get_height_map");
	ADD_GROUP("Drainage", "");
	RR_BIND(inlet_spacing, get_inlet_spacing, FLOAT, PROPERTY_HINT_RANGE, "0,500,0.1,suffix:m")
	RR_BIND(inlets, get_inlets, PACKED_VECTOR2_ARRAY, PROPERTY_HINT_NONE, "")
	ADD_GROUP("Mesh", "");
	RR_BIND(cell, get_cell, FLOAT, PROPERTY_HINT_RANGE, "0.02,1,0.01,suffix:m")
	RR_BIND(collision, has_collision, BOOL, PROPERTY_HINT_NONE, "")
	RR_BIND(collision_layer, get_collision_layer, INT, PROPERTY_HINT_LAYERS_3D_PHYSICS, "")

#undef RR_BIND

	BIND_ENUM_CONSTANT(ROUGHNESS_A);
	BIND_ENUM_CONSTANT(ROUGHNESS_B);
	BIND_ENUM_CONSTANT(ROUGHNESS_C);
	BIND_ENUM_CONSTANT(ROUGHNESS_NONE);
}

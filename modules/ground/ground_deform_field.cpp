/**************************************************************************/
/*  ground_deform_field.cpp                                               */
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

#include "ground_deform_field.h"

#include "core/io/image.h"
#include "core/math/math_funcs.h"
#include "core/templates/hashfuncs.h"
#include "scene/3d/physics/character_body_3d.h"
#include "scene/3d/physics/collision_shape_3d.h"
#include "scene/3d/physics/rigid_body_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/resources/3d/box_shape_3d.h"
#include "scene/resources/3d/capsule_shape_3d.h"
#include "scene/resources/3d/cylinder_shape_3d.h"
#include "scene/resources/3d/sphere_shape_3d.h"
#include "scene/resources/image_texture.h"

// ---------------------------------------------------------------------------------------------------------
// Following and the tile cache

Vector2i GroundDeformField::origin_for(Vector2 p_center) const {
	const float tile = tile_size();
	const Vector2i ct(int(Math::floor(p_center.x / tile)), int(Math::floor(p_center.y / tile)));
	const int nt = tiles();
	return (ct - Vector2i(nt / 2, nt / 2)) * TILE;
}

Vector2 GroundDeformField::center_for(Vector2 p_center) const {
	const float tile = tile_size();
	return Vector2(Math::floor(p_center.x / tile), Math::floor(p_center.y / tile)) * tile;
}

void GroundDeformField::_mark(int p_x, int p_z, float p_settle) {
	const int nt = tiles();
	const int i = (p_z / TILE) * nt + p_x / TILE;
	tile_active[i] = 1;
	if (p_settle > tile_settle[i]) {
		tile_settle[i] = p_settle;
	}
}

void GroundDeformField::_store_tile(Vector2i p_tile_world, int p_tx, int p_tz) {
	const int n = resolution;
	const int c = channels();
	CachedTile t;
	t.data.resize(TILE * TILE * c);
	uint16_t *w = t.data.ptrw();
	for (int z = 0; z < TILE; z++) {
		for (int x = 0; x < TILE; x++) {
			const int i = (p_tz * TILE + z) * n + p_tx * TILE + x;
			const int o = (z * TILE + x) * c;
			w[o + 0] = Math::make_half_float(dent[i]);
			w[o + 1] = Math::make_half_float(rim[i]);
			if (c > 2) {
				w[o + 2] = Math::make_half_float(fresh[i]);
			}
		}
	}
	t.used = ++cache_clock;
	cache.insert(p_tile_world, t);
}

bool GroundDeformField::_load_tile(Vector2i p_tile_world, int p_tx, int p_tz) {
	const int n = resolution;
	const int c = channels();
	CachedTile *t = cache.getptr(p_tile_world);
	if (t && t->data.size() != TILE * TILE * c) {
		cache.erase(p_tile_world);
		t = nullptr;
	}
	for (int z = 0; z < TILE; z++) {
		for (int x = 0; x < TILE; x++) {
			const int i = (p_tz * TILE + z) * n + p_tx * TILE + x;
			const int o = (z * TILE + x) * c;
			dent[i] = t ? Math::half_to_float(t->data[o + 0]) : 0.0f;
			rim[i] = t ? Math::half_to_float(t->data[o + 1]) : 0.0f;
			if (c > 2) {
				fresh[i] = t ? Math::half_to_float(t->data[o + 2]) : 0.0f;
			}
		}
	}
	if (t) {
		cache.erase(p_tile_world);
		return true;
	}
	return false;
}

void GroundDeformField::_trim_cache() {
	while (int(cache.size()) > max_cached_tiles) {
		Vector2i oldest;
		uint64_t best = UINT64_MAX;
		for (const KeyValue<Vector2i, CachedTile> &kv : cache) {
			if (kv.value.used < best) {
				best = kv.value.used;
				oldest = kv.key;
			}
		}
		cache.erase(oldest);
	}
}

void GroundDeformField::shift(Vector2i p_origin) {
	const int n = resolution;
	const int nt = tiles();
	const bool keep = valid && int(dent.size()) == n * n && (!track_fresh || int(fresh.size()) == n * n);
	if (!keep) {
		dent.resize(n * n);
		rim.resize(n * n);
		fresh.resize(track_fresh ? n * n : 0);
		tile_active.resize(nt * nt);
		tile_settle.resize(nt * nt);
		tile_wait.resize(nt * nt);
		for (int i = 0; i < n * n; i++) {
			dent[i] = 0.0f;
			rim[i] = 0.0f;
		}
		for (float &f : fresh) {
			f = 0.0f;
		}
		for (int i = 0; i < nt * nt; i++) {
			tile_active[i] = 0;
			tile_settle[i] = 0.0f;
			tile_wait[i] = 0.0f;
		}
	}
	const Vector2i old_tile0 = origin / TILE;
	const Vector2i new_tile0 = p_origin / TILE;
	if (keep) {
		if (p_origin == origin) {
			return;
		}
		// Tiles leaving the field go to the cache.
		for (int tz = 0; tz < nt; tz++) {
			for (int tx = 0; tx < nt; tx++) {
				const Vector2i w = old_tile0 + Vector2i(tx, tz);
				const Vector2i l = w - new_tile0;
				const bool stays = l.x >= 0 && l.y >= 0 && l.x < nt && l.y < nt;
				if (!stays && tile_active[tz * nt + tx]) {
					_store_tile(w, tx, tz);
				}
			}
		}
		LocalVector<float> old_dent;
		LocalVector<float> old_rim;
		LocalVector<float> old_fresh;
		LocalVector<uint8_t> old_active;
		LocalVector<float> old_settle;
		old_dent = dent;
		old_rim = rim;
		old_fresh = fresh;
		old_active = tile_active;
		old_settle = tile_settle;
		for (int tz = 0; tz < nt; tz++) {
			for (int tx = 0; tx < nt; tx++) {
				const Vector2i w = new_tile0 + Vector2i(tx, tz);
				const Vector2i o = w - old_tile0;
				if (o.x >= 0 && o.y >= 0 && o.x < nt && o.y < nt) {
					for (int z = 0; z < TILE; z++) {
						const int src = (o.y * TILE + z) * n + o.x * TILE;
						const int dst = (tz * TILE + z) * n + tx * TILE;
						memcpy(&dent[dst], &old_dent[src], TILE * sizeof(float));
						memcpy(&rim[dst], &old_rim[src], TILE * sizeof(float));
						if (track_fresh) {
							memcpy(&fresh[dst], &old_fresh[src], TILE * sizeof(float));
						}
					}
					tile_active[tz * nt + tx] = old_active[o.y * nt + o.x];
					tile_settle[tz * nt + tx] = old_settle[o.y * nt + o.x];
				} else {
					tile_active[tz * nt + tx] = _load_tile(w, tx, tz) ? 1 : 0;
					tile_settle[tz * nt + tx] = 0.0f;
				}
				tile_wait[tz * nt + tx] = 0.0f;
			}
		}
	} else {
		for (int tz = 0; tz < nt; tz++) {
			for (int tx = 0; tx < nt; tx++) {
				tile_active[tz * nt + tx] = _load_tile(new_tile0 + Vector2i(tx, tz), tx, tz) ? 1 : 0;
			}
		}
	}
	_trim_cache();
	origin = p_origin;
	valid = true;
	dirty = true;
	shape_dirty = true;
}

void GroundDeformField::clear() {
	for (float &d : dent) {
		d = 0.0f;
	}
	for (float &r : rim) {
		r = 0.0f;
	}
	for (float &f : fresh) {
		f = 0.0f;
	}
	for (uint8_t &a : tile_active) {
		a = 0;
	}
	for (float &s : tile_settle) {
		s = 0.0f;
	}
	for (float &w : tile_wait) {
		w = 0.0f;
	}
	cache.clear();
	dirty = true;
	shape_dirty = true;
}

void GroundDeformField::settle(Rect2 p_rect, float p_seconds) {
	if (!valid) {
		return;
	}
	const float tile = tile_size();
	const int nt = tiles();
	const Rect2 r = rect();
	const int x0 = MAX(int(Math::floor((p_rect.position.x - r.position.x) / tile)), 0);
	const int z0 = MAX(int(Math::floor((p_rect.position.y - r.position.y) / tile)), 0);
	const int x1 = MIN(int(Math::floor((p_rect.position.x + p_rect.size.x - r.position.x) / tile)), nt - 1);
	const int z1 = MIN(int(Math::floor((p_rect.position.y + p_rect.size.y - r.position.y) / tile)), nt - 1);
	for (int z = z0; z <= z1; z++) {
		for (int x = x0; x <= x1; x++) {
			tile_settle[z * nt + x] = MAX(tile_settle[z * nt + x], p_seconds);
		}
	}
}

// ---------------------------------------------------------------------------------------------------------
// Queries

float GroundDeformField::sample(const LocalVector<float> &p_field, float p_x, float p_z) const {
	const int n = resolution;
	if (int(p_field.size()) != n * n) {
		return 0.0f;
	}
	const float t = texel();
	const float fx = p_x / t - origin.x - 0.5f;
	const float fz = p_z / t - origin.y - 0.5f;
	if (fx < -0.5f || fz < -0.5f || fx > n - 0.5f || fz > n - 0.5f) {
		return 0.0f;
	}
	const float cx = CLAMP(fx, 0.0f, float(n - 1));
	const float cz = CLAMP(fz, 0.0f, float(n - 1));
	const int x0 = MIN(int(cx), n - 2);
	const int z0 = MIN(int(cz), n - 2);
	const float tx = cx - x0;
	const float tz = cz - z0;
	auto at = [&](int x, int z) { return p_field[z * n + x]; };
	return Math::lerp(Math::lerp(at(x0, z0), at(x0 + 1, z0), tx), Math::lerp(at(x0, z0 + 1), at(x0 + 1, z0 + 1), tx), tz);
}

Vector2 GroundDeformField::moved_volume() const {
	const float area = texel() * texel();
	double d = 0.0;
	double r = 0.0;
	for (uint32_t i = 0; i < dent.size(); i++) {
		d += dent[i];
		r += rim[i];
	}
	return Vector2(d * area, r * area);
}

// ---------------------------------------------------------------------------------------------------------
// Pressing

float GroundDeformField::bekker_sinkage(float p_pressure_kpa, float p_width, float p_n, float p_kc, float p_kphi) {
	if (p_pressure_kpa <= 0.0f) {
		return 0.0f;
	}
	const float k = p_kc / MAX(p_width, 0.02f) + p_kphi;
	return Math::pow(p_pressure_kpa / k, 1.0f / MAX(p_n, 0.1f));
}

float GroundDeformField::body_sinkage(const Body &p_body, float p_n, float p_kc, float p_kphi) {
	if (p_body.mass <= 0.0f) {
		return 0.0f;
	}
	const float load = p_body.mass * 9.81f / 1000.0f; // kN
	if (p_body.area > 0.0f) {
		return bekker_sinkage(load / p_body.area, p_body.width > 0.0f ? p_body.width : Math::sqrt(p_body.area), p_n, p_kc, p_kphi);
	}
	if (p_body.flat_area > 0.0f) {
		return bekker_sinkage(load / p_body.flat_area, p_body.width > 0.0f ? p_body.width : Math::sqrt(p_body.flat_area), p_n, p_kc, p_kphi);
	}
	// Round bodies and wheels: the patch grows with sinkage; a few fixed-point steps.
	const float rr = p_body.wheel ? p_body.half_height : p_body.radius;
	float z = 0.05f;
	for (int it = 0; it < 8; it++) {
		const float l = 2.0f * Math::sqrt(MAX(2.0f * rr * z - z * z, 1e-4f));
		const float patch = p_body.wheel ? l * p_body.width : Math::PI * 0.25f * l * l;
		const float b = p_body.wheel ? MIN(l, p_body.width) : l;
		z = 0.5f * z + 0.5f * bekker_sinkage(load / patch, b, p_n, p_kc, p_kphi);
	}
	return z;
}

// A broken crust (docs/ideas/snow.md, follow-up 3d). A crust on soft snow is a plate on an elastic
// foundation: loaded through, it cracks radially from the load and around it near its characteristic
// length l = (E h^3 / (12 (1 - nu^2) k))^(1/4) (Hertz 1884; Hetenyi 1946), and the pieces tip into the
// hole. With E ~ 50 MPa and k ~ 1e5 N/m3 (our estimates for a crust over soft snow) a 2 cm crust has
// l ~ 0.13 m, growing as h^(3/4).
static constexpr float CRACK_LENGTH = 0.13f; // m at 2 cm
static constexpr float PLATE = 0.12f; // m, the pieces heaped in the rim
static constexpr int CRACK_SECTORS = 7; // radial cracks around a break

float GroundDeformField::body_pressure(const Body &p_body, float p_sinkage) {
	if (p_body.mass <= 0.0f) {
		return 0.0f;
	}
	const float load = p_body.mass * 9.81f / 1000.0f; // kN
	if (p_body.area > 0.0f) {
		return load / p_body.area;
	}
	if (p_body.flat_area > 0.0f) {
		return load / p_body.flat_area;
	}
	const float rr = p_body.wheel ? p_body.half_height : p_body.radius;
	const float z = CLAMP(p_sinkage, 0.005f, rr);
	const float l = 2.0f * Math::sqrt(MAX(2.0f * rr * z - z * z, 1e-4f));
	return load / (p_body.wheel ? l * MAX(p_body.width, 0.01f) : Math::PI * 0.25f * l * l);
}

float GroundDeformField::press(const Columns &p_columns, Vector3 p_position, float p_radius, Vector2 p_half, float p_yaw, bool p_box, float p_bottom, float p_sink, bool p_round, float p_contact_tolerance) {
	if (!valid || p_radius <= 0.0f || (p_box && (p_half.x <= 0.0f || p_half.y <= 0.0f))) {
		return 0.0f;
	}
	const int n = resolution;
	const float t = texel();
	const float area = t * t;
	// The rim ring: half the footprint's equivalent radius wide.
	const float ring = 0.5f * (p_box ? Math::sqrt(p_half.x * p_half.y) : p_radius);
	const float crust = p_columns.column(p_position.x, p_position.z).crust;
	const float crack = crust > 0.0f ? CRACK_LENGTH * Math::pow(crust / 0.02f, 0.75f) : 0.0f;
	const float span = (p_box ? p_half.length() : p_radius) + MAX(ring, 1.6f * crack);
	const int x0 = MAX(int(Math::floor((p_position.x - span) / t)) - origin.x, 0);
	const int x1 = MIN(int(Math::ceil((p_position.x + span) / t)) - origin.x, n - 1);
	const int z0 = MAX(int(Math::floor((p_position.z - span) / t)) - origin.y, 0);
	const int z1 = MIN(int(Math::ceil((p_position.z + span) / t)) - origin.y, n - 1);
	if (x0 > x1 || z0 > z1) {
		return 0.0f;
	}
	const float r2 = p_radius * p_radius;
	const float cy = Math::cos(p_yaw);
	const float sy = Math::sin(p_yaw);
	// Footprint test: the distance outside the footprint (0 inside) and the squared distance from the centre.
	auto outside = [&](float wx, float wz, float &r_d2) {
		const float dx = wx - p_position.x;
		const float dz = wz - p_position.z;
		r_d2 = dx * dx + dz * dz;
		if (p_box) {
			// Node yaw turns local x toward -z: local = R(-yaw) applied to the offset.
			const float lx = cy * dx - sy * dz;
			const float lz = sy * dx + cy * dz;
			const float ox = MAX(Math::abs(lx) - p_half.x, 0.0f);
			const float oz = MAX(Math::abs(lz) - p_half.y, 0.0f);
			return Math::sqrt(ox * ox + oz * oz);
		}
		return MAX(Math::sqrt(r_d2) - p_radius, 0.0f);
	};
	// Coverage of the texel by the footprint, from the signed distance to its edge (negative inside) over
	// half a texel: the edge is drawn antialiased, so a turned box or a circle leaves a smoother wall on the
	// texel grid instead of a staircase. Half a texel, not one: a boot only two texels wide still sinks
	// to its full depth in the middle.
	auto coverage = [&](float wx, float wz, float &r_d2) {
		const float dx = wx - p_position.x;
		const float dz = wz - p_position.z;
		r_d2 = dx * dx + dz * dz;
		float sd;
		if (p_box) {
			const float lx = cy * dx - sy * dz;
			const float lz = sy * dx + cy * dz;
			const float qx = Math::abs(lx) - p_half.x;
			const float qz = Math::abs(lz) - p_half.y;
			sd = Math::sqrt(MAX(qx, 0.0f) * MAX(qx, 0.0f) + MAX(qz, 0.0f) * MAX(qz, 0.0f)) + MIN(MAX(qx, qz), 0.0f);
		} else {
			sd = Math::sqrt(r_d2) - p_radius;
		}
		return CLAMP(0.5f - 2.0f * sd / t, 0.0f, 1.0f);
	};
	auto profile = [&](float d2) {
		return p_round ? p_bottom + p_radius - Math::sqrt(MAX(r2 - d2, 0.0f)) : p_bottom;
	};
	// Contact: the underside reaches the current surface somewhere in the footprint.
	bool touching = false;
	for (int z = z0; z <= z1 && !touching; z++) {
		for (int x = x0; x <= x1; x++) {
			const float wx = (origin.x + x + 0.5f) * t;
			const float wz = (origin.y + z + 0.5f) * t;
			float d2;
			if (coverage(wx, wz, d2) <= 0.0f) {
				continue;
			}
			const Column c = p_columns.column(wx, wz);
			if (!c.deformable) {
				continue;
			}
			const int i = z * n + x;
			if (profile(d2) <= c.surface - dent[i] + rim[i] + p_contact_tolerance) {
				touching = true;
				break;
			}
		}
	}
	if (!touching) {
		return 0.0f;
	}
	float moved = 0.0f;
	float pushed = 0.0f;
	float deepest = 0.0f;
	for (int z = z0; z <= z1; z++) {
		for (int x = x0; x <= x1; x++) {
			const float wx = (origin.x + x + 0.5f) * t;
			const float wz = (origin.y + z + 0.5f) * t;
			float d2;
			const float cover = coverage(wx, wz, d2);
			if (cover <= 0.0f) {
				continue;
			}
			const Column c = p_columns.column(wx, wz);
			if (!c.deformable) {
				continue;
			}
			const int i = z * n + x;
			const float s = c.surface - dent[i] + rim[i];
			float target = profile(MIN(d2, r2));
			if (p_sink > 0.0f) {
				const float w = p_round ? Math::sqrt(MAX(1.0f - d2 / r2, 0.0f)) : 1.0f;
				target = MIN(target, c.surface - p_sink * w);
			}
			target = MAX(target, c.surface - c.max_dent);
			if (target >= s) {
				continue;
			}
			// An edge texel goes down by the share of it the footprint covers.
			const float delta = (s - target) * cover;
			deepest = MAX(deepest, c.surface - target);
			const float take = MIN(rim[i], delta);
			rim[i] -= take;
			dent[i] += delta - take;
			if (track_fresh) {
				fresh[i] = 1.0f;
			}
			moved += delta * area;
			pushed += delta * area * c.rim_share;
			_mark(x, z, press_settle);
		}
	}
	if (moved <= 0.0f) {
		return 0.0f;
	}
	if (crack > 0.0f && deepest > 0.0f) {
		// The broken crust: pieces out to a jagged crack line, one length per radial sector, tip into the
		// hole, their inner edges down by up to half its depth. The snow under them compacts (no rim).
		const uint32_t seed = hash_murmur3_one_32(uint32_t(int(Math::floor(p_position.x / 0.25f))) * 73856093u ^ uint32_t(int(Math::floor(p_position.z / 0.25f))) * 19349663u);
		const float drop_edge = MIN(0.5f * deepest, 0.5f * crack);
		for (int z = z0; z <= z1; z++) {
			for (int x = x0; x <= x1; x++) {
				const float wx = (origin.x + x + 0.5f) * t;
				const float wz = (origin.y + z + 0.5f) * t;
				float d2;
				const float o = outside(wx, wz, d2);
				if (o <= 0.0f) {
					continue;
				}
				const float a = Math::atan2(wz - p_position.z, wx - p_position.x) + Math::PI;
				const int sector = int(a / float(Math::TAU) * CRACK_SECTORS) % CRACK_SECTORS;
				const uint32_t hs = hash_murmur3_one_32(uint32_t(sector), seed);
				const float reach = crack * (0.5f + 1.1f * (hs & 0xffff) / 65535.0f);
				if (o >= reach) {
					continue;
				}
				const Column c = p_columns.column(wx, wz);
				if (!c.deformable || c.crust <= 0.0f) {
					continue;
				}
				const int i = z * n + x;
				const float target = MAX(c.surface - drop_edge * (1.0f - o / reach), c.surface - c.max_dent);
				const float s = c.surface - dent[i] + rim[i];
				if (target >= s) {
					continue;
				}
				const float delta = s - target;
				const float take = MIN(rim[i], delta);
				rim[i] -= take;
				dent[i] += delta - take;
				if (track_fresh) {
					fresh[i] = 1.0f;
				}
				_mark(x, z, press_settle);
			}
		}
	}
	// The rim: the pushed share of the moved material, spread over the ring, thickest at the edge.
	float wsum = 0.0f;
	for (int pass = 0; pass < 2 && pushed > 0.0f; pass++) {
		for (int z = z0; z <= z1; z++) {
			for (int x = x0; x <= x1; x++) {
				const float wx = (origin.x + x + 0.5f) * t;
				const float wz = (origin.y + z + 0.5f) * t;
				float d2;
				const float o = outside(wx, wz, d2);
				if (o <= 0.0f || o >= ring) {
					continue;
				}
				const Column c = p_columns.column(wx, wz);
				if (!c.deformable) {
					continue;
				}
				float w = 1.0f - o / ring;
				if (c.crust > 0.0f) {
					// A broken crust: the rim is heaped in plates, each a tilted slab of its own height.
					const int px = int(Math::floor(wx / PLATE));
					const int pz = int(Math::floor(wz / PLATE));
					const uint32_t hsh = hash_murmur3_one_32(uint32_t(px) * 73856093u ^ uint32_t(pz) * 19349663u);
					const float hgt = (hsh & 0xffff) / 65535.0f;
					const float tilt = ((hsh >> 16) & 0xff) / 127.5f - 1.0f;
					const float along = (wx - (px + 0.5f) * PLATE) * (((hsh >> 24) & 1) ? 1.0f : 0.0f) + (wz - (pz + 0.5f) * PLATE) * (((hsh >> 24) & 1) ? 0.0f : 1.0f);
					const float plate = MAX(0.1f + 1.8f * hgt + tilt * along / PLATE, 0.0f);
					w *= 1.0f + MIN(c.crust / 0.02f, 1.0f) * (plate - 1.0f);
				}
				if (pass == 0) {
					wsum += w;
				} else {
					const int i = z * n + x;
					rim[i] += pushed * w / (wsum * area);
					if (track_fresh) {
						fresh[i] = MAX(fresh[i], w);
					}
					_mark(x, z, press_settle);
				}
			}
		}
		if (wsum <= 0.0f) {
			break; // Nowhere to push: it all compacts in place.
		}
	}
	dirty = true;
	shape_dirty = true;
	return moved;
}

bool GroundDeformField::read_body(Node3D *p_node, float p_character_mass, float p_character_area, Body &r_body) {
	Body b;
	b.center = p_node->get_global_position();
	// Shape: the first enabled CollisionShape3D child, as the grass trample reads it.
	for (int c = 0; c < p_node->get_child_count(); c++) {
		CollisionShape3D *cs = Object::cast_to<CollisionShape3D>(p_node->get_child(c));
		if (!cs || cs->is_disabled() || cs->get_shape().is_null()) {
			continue;
		}
		const Ref<Shape3D> shape = cs->get_shape();
		const Basis basis = cs->get_global_basis();
		const float sc = basis.get_scale().x;
		b.center = cs->get_global_position();
		if (const SphereShape3D *sp = Object::cast_to<SphereShape3D>(shape.ptr())) {
			b.radius = b.half_height = sp->get_radius() * sc;
		} else if (const CapsuleShape3D *cp = Object::cast_to<CapsuleShape3D>(shape.ptr())) {
			b.radius = cp->get_radius() * sc;
			b.half_height = cp->get_height() * 0.5f * sc;
		} else if (const CylinderShape3D *cy = Object::cast_to<CylinderShape3D>(shape.ptr())) {
			const float up = Math::abs(basis.get_column(1).normalized().y);
			if (up > 0.7f) {
				b.radius = cy->get_radius() * sc;
				b.half_height = cy->get_height() * 0.5f * sc;
				b.round = false;
				b.flat_area = Math::PI * b.radius * b.radius;
			} else {
				// A wheel: a track as wide as the tyre, flat across.
				b.wheel = true;
				b.half_height = cy->get_radius() * sc;
				b.width = cy->get_height() * sc;
				b.radius = b.width * 0.5f;
				b.round = false;
			}
		} else if (const BoxShape3D *bx = Object::cast_to<BoxShape3D>(shape.ptr())) {
			const Vector3 s = bx->get_size() * 0.5f * sc;
			b.radius = MAX(s.x, s.z);
			b.half_height = s.y;
			b.round = false;
			b.flat_area = 4.0f * s.x * s.z;
			b.width = 2.0f * MIN(s.x, s.z);
			// A boot or a crate: an oriented print, turned with the shape's heading.
			b.box = true;
			b.box_half = Vector2(s.x, s.z);
			const Vector3 fx = basis.get_column(0);
			b.yaw = Math::atan2(-fx.z, fx.x);
		}
		break;
	}
	auto meta = [&](const char *p_ground, const char *p_snow, float &r_value) {
		if (p_node->has_meta(p_ground)) {
			r_value = p_node->get_meta(p_ground);
		} else if (p_node->has_meta(p_snow)) {
			r_value = p_node->get_meta(p_snow);
		}
	};
	meta("ground_radius", "snow_radius", b.radius);
	// Load: rigid bodies by mass, characters by character_mass, the rest only push to their underside.
	if (const RigidBody3D *rb = Object::cast_to<RigidBody3D>(p_node)) {
		b.mass = rb->get_mass();
	} else if (Object::cast_to<CharacterBody3D>(p_node)) {
		b.mass = p_character_mass;
		b.area = p_character_area;
		b.width = Math::sqrt(p_character_area / 3.0f); // a boot about three times as long as wide
	}
	meta("ground_mass", "snow_mass", b.mass);
	meta("ground_contact_area", "snow_contact_area", b.area);
	r_body = b;
	return true;
}

void GroundDeformField::press_bodies(SceneTree *p_tree, const PackedStringArray &p_groups, const Columns &p_columns, float p_character_mass, float p_character_area, float p_contact_tolerance, double p_delta) {
	frame++;
	if (!p_tree || !valid) {
		return;
	}
	HashSet<ObjectID> done;
	for (const String &group : p_groups) {
		const Vector<Node *> nodes = p_tree->get_nodes_in_group(StringName(group));
		for (Node *node : nodes) {
			Node3D *n3 = Object::cast_to<Node3D>(node);
			if (!n3 || !n3->is_visible_in_tree() || done.has(n3->get_instance_id())) {
				continue;
			}
			done.insert(n3->get_instance_id());
			Body b;
			read_body(n3, p_character_mass, p_character_area, b);
			const float sink = p_columns.sinkage(b);
			const Vector3 center = b.center;
			Mover *m = movers.getptr(n3->get_instance_id());
			Vector3 from = m ? m->last : center;
			if ((center - from).length() > 30.0f * MAX(float(p_delta), 0.001f) + 5.0f) {
				from = center; // teleported
			}
			const float dist = Vector2(center.x - from.x, center.z - from.z).length();
			const int steps = CLAMP(int(Math::ceil(dist / MAX(b.radius * 0.5f, 0.01f))), 1, 16);
			for (int k = 1; k <= steps; k++) {
				const Vector3 p = from.lerp(center, float(k) / steps);
				press(p_columns, p, b.radius, b.box ? b.box_half : Vector2(), b.yaw, b.box, p.y - b.half_height, sink, b.round, p_contact_tolerance);
			}
			movers[n3->get_instance_id()] = Mover{ center, frame };
		}
	}
	// Forget movers that left.
	LocalVector<ObjectID> gone;
	for (const KeyValue<ObjectID, Mover> &kv : movers) {
		if (kv.value.frame != frame) {
			gone.push_back(kv.key);
		}
	}
	for (const ObjectID &id : gone) {
		movers.erase(id);
	}
}

// ---------------------------------------------------------------------------------------------------------
// GPU

void GroundDeformField::upload(Ref<Image> &r_image, Ref<ImageTexture> &r_texture, const LocalVector<float> *p_b, const LocalVector<float> *p_a) {
	const int n = resolution;
	if (!valid || int(dent.size()) != n * n) {
		return;
	}
	const bool four = p_b || p_a;
	const int c = four ? 4 : 2;
	Vector<uint8_t> bytes;
	bytes.resize(n * n * c * 2);
	uint16_t *w = reinterpret_cast<uint16_t *>(bytes.ptrw());
	for (int i = 0; i < n * n; i++) {
		w[i * c + 0] = Math::make_half_float(dent[i]);
		w[i * c + 1] = Math::make_half_float(rim[i]);
		if (four) {
			w[i * c + 2] = Math::make_half_float(p_b ? (*p_b)[i] : 1.0f);
			w[i * c + 3] = Math::make_half_float(p_a ? (*p_a)[i] : 1.0f);
		}
	}
	const Image::Format format = four ? Image::FORMAT_RGBAH : Image::FORMAT_RGH;
	if (r_image.is_null() || r_image->get_width() != n || r_image->get_format() != format) {
		r_image = Image::create_from_data(n, n, false, format, bytes);
		r_texture = ImageTexture::create_from_image(r_image);
	} else {
		r_image->set_data(n, n, false, format, bytes);
		r_texture->update(r_image);
	}
	dirty = false;
}

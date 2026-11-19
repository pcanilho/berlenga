/**************************************************************************/
/*  snow_avalanche_3d.cpp                                                 */
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

#include "snow_avalanche_3d.h"

#include "snow_bodies_3d.h"
#include "snow_clumps_3d.h"
#include "snow_surface_3d.h"
#include "snow_cover_3d.h"

#include "core/config/engine.h"
#include "core/object/class_db.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/physics/rigid_body_3d.h"
#include "scene/main/viewport.h"

namespace {
const float GRAVITY = 9.81f;
const float FLOW_MIN = 1.0f; // kg/m2 (about 3 mm of debris): less is not flowing
const float STOP_SPEED = 0.5f; // m/s
const float ENTRAIN_SPEED = 5.0f; // m/s
const float WEAK_DENSITY = 100.0f; // kg/m3, the new snow's base or facets: the weak layer
const float BURIED_WEAK_DENSITY = 150.0f; // kg/m3, facets on a buried crust
const float RECENT_BONDED_HOURS = 168.0f; // a week: the new snow has bonded
const int MAX_RELEASE_CELLS = 4000;
const double FLUSH_INTERVAL = 0.2; // s between writes of the moving snow into the cover
} // namespace

float SnowAvalanche3D::weak_layer_strength(float p_density, float p_age_hours) {
	// Jamieson and Johnston (2001): shear strength 14.5 (rho / rho_ice)^1.73 kPa, growing as the layer sinters
	// (about double in two days, Jamieson 1995's rate of strength gain in new snow).
	const float base = 14.5e3f * Math::pow(CLAMP(p_density, 30.0f, 917.0f) / 917.0f, 1.73f);
	return base * (1.0f + MAX(p_age_hours, 0.0f) / 48.0f);
}

float SnowAvalanche3D::stability_index(float p_slab, float p_slab_density, float p_weak_density, float p_age_hours, float p_slope) {
	// Foehn (1987): S = tau_strength / tau_xz, the slab's weight sheared along the slope rho g h sin cos.
	const float load = p_slab_density * GRAVITY * p_slab * Math::sin(p_slope) * Math::cos(p_slope);
	if (load <= 1.0e-3f) {
		return 1.0e6f;
	}
	return weak_layer_strength(p_weak_density, p_age_hours) / load;
}

SnowCover3D *SnowAvalanche3D::_cover() const {
	return Object::cast_to<SnowCover3D>(get_parent());
}

int SnowAvalanche3D::_cover_index(const SnowCover3D *p_cover, int p_i) const {
	const int x = p_i % res + flow_origin.x - p_cover->origin_cell.x;
	const int z = p_i / res + flow_origin.y - p_cover->origin_cell.y;
	const int n = p_cover->resolution;
	return (x < 0 || z < 0 || x >= n || z >= n) ? -1 : z * n + x;
}

float SnowAvalanche3D::_slope_at(const SnowCover3D *p_cover, int p_x, int p_z) const {
	const int n = p_cover->resolution;
	const float cs = p_cover->_cell_size();
	const Vector<float> &g = p_cover->cell_ground;
	const int x0 = MAX(p_x - 1, 0), x1 = MIN(p_x + 1, n - 1);
	const int z0 = MAX(p_z - 1, 0), z1 = MIN(p_z + 1, n - 1);
	const float dx = (g[p_z * n + x1] - g[p_z * n + x0]) / (MAX(x1 - x0, 1) * cs);
	const float dz = (g[z1 * n + p_x] - g[z0 * n + p_x]) / (MAX(z1 - z0, 1) * cs);
	return Math::atan(Math::sqrt(dx * dx + dz * dz));
}

void SnowAvalanche3D::_track_recent(SnowCover3D *p_cover) {
	// The new snow since it last bonded, per cell: new snow adds to it (its age the mass-weighted mean), it ages
	// with the cover's hours, a week bonds it, and it is never more than the pack.
	const int n = p_cover->resolution;
	if (p_cover->pub.swe.size() != n * n || p_cover->cell_ground.size() != n * n) {
		return;
	}
	if (recent_res != n) {
		recent.resize(n * n);
		recent.fill(0.0f);
		recent_age.resize(n * n);
		recent_age.fill(0.0f);
		recent_res = n;
		recent_origin = p_cover->origin_cell;
	} else if (recent_origin != p_cover->origin_cell) {
		const Vector2i d = p_cover->origin_cell - recent_origin;
		Vector<float> r2, a2;
		r2.resize(n * n);
		r2.fill(0.0f);
		a2.resize(n * n);
		a2.fill(0.0f);
		for (int z = 0; z < n; z++) {
			for (int x = 0; x < n; x++) {
				const int ox = x + d.x, oz = z + d.y;
				if (ox >= 0 && oz >= 0 && ox < n && oz < n) {
					r2.write[z * n + x] = recent[oz * n + ox];
					a2.write[z * n + x] = recent_age[oz * n + ox];
				}
			}
		}
		recent = r2;
		recent_age = a2;
		recent_origin = p_cover->origin_cell;
	}
	const float hours = p_cover->last_step_hours;
	const bool has_new = p_cover->pub.new_depth.size() == n * n;
	for (int i = 0; i < n * n; i++) {
		const float add = has_new ? MAX(p_cover->pub.new_depth[i], 0.0f) : 0.0f;
		float r = recent[i];
		float a = recent_age[i] + hours;
		if (add > 0.0f) {
			a = (a * r) / (r + add);
			r += add;
		}
		const float rho = MAX(p_cover->pub.density[i], 30.0f);
		const float depth = p_cover->pub.swe[i] > 0.01f ? p_cover->pub.swe[i] / rho : 0.0f;
		r = MIN(r, depth);
		if (a > RECENT_BONDED_HOURS) {
			r = 0.0f;
			a = 0.0f;
		}
		recent.write[i] = r;
		recent_age.write[i] = a;
	}
}

bool SnowAvalanche3D::_check_release(SnowCover3D *p_cover) {
	const int n = p_cover->resolution;
	if (recent_res != n || p_cover->pub.swe.size() != n * n) {
		return false;
	}
	const float lo = Math::deg_to_rad(min_slope), hi = Math::deg_to_rad(max_slope);
	const bool crusts = p_cover->pub.crust.size() == n * n && p_cover->pub.crust_burial.size() == n * n;
	// The slab and its stability per cell (the weaker of the new snow and a buried crust's facets).
	Vector<float> stab, slab;
	stab.resize(n * n);
	slab.resize(n * n);
	int seed = -1;
	float seed_s = 1.0f;
	for (int z = 0; z < n; z++) {
		for (int x = 0; x < n; x++) {
			const int i = z * n + x;
			stab.write[i] = 1.0e6f;
			slab.write[i] = 0.0f;
			const float slope = _slope_at(p_cover, x, z);
			if (slope < lo || slope > hi) {
				continue;
			}
			const float rho = MAX(p_cover->pub.density[i], 30.0f);
			float best = 1.0e6f;
			float best_slab = 0.0f;
			if (recent[i] >= min_slab) {
				best = stability_index(recent[i], rho, WEAK_DENSITY, recent_age[i], slope);
				best_slab = recent[i];
			}
			if (crusts && p_cover->pub.crust[i] > 0.0f && p_cover->pub.crust_burial[i] >= min_slab) {
				const float s = stability_index(p_cover->pub.crust_burial[i], rho, BURIED_WEAK_DENSITY, 0.0f, slope);
				if (s < best) {
					best = s;
					best_slab = p_cover->pub.crust_burial[i];
				}
			}
			stab.write[i] = best;
			slab.write[i] = best_slab;
			if (best < seed_s) {
				seed_s = best;
				seed = i;
			}
		}
	}
	if (seed < 0) {
		return false;
	}
	// The fracture runs through the slab while it stays near failure (S under 1.2) on avalanche terrain.
	Vector<int> cells;
	Vector<float> amounts;
	Vector<uint8_t> seen;
	seen.resize(n * n);
	seen.fill(0);
	Vector<int> stack;
	stack.push_back(seed);
	seen.write[seed] = 1;
	while (!stack.is_empty() && cells.size() < MAX_RELEASE_CELLS) {
		const int i = stack[stack.size() - 1];
		stack.resize(stack.size() - 1);
		cells.push_back(i);
		amounts.push_back(slab[i]);
		const int x = i % n, z = i / n;
		const int nb[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
		for (const auto &o : nb) {
			const int xx = x + o[0], zz = z + o[1];
			if (xx < 0 || zz < 0 || xx >= n || zz >= n) {
				continue;
			}
			const int j = zz * n + xx;
			if (!seen[j] && stab[j] < 1.2f && slab[j] >= 0.5f * min_slab) {
				seen.write[j] = 1;
				stack.push_back(j);
			}
		}
	}
	_release(p_cover, cells, amounts);
	return true;
}

void SnowAvalanche3D::_release(SnowCover3D *p_cover, const Vector<int> &p_cells, const Vector<float> &p_slab) {
	p_cover->_finish_background(true);
	const int n = p_cover->resolution;
	res = n;
	cell = p_cover->_cell_size();
	flow_origin = p_cover->origin_cell;
	h.resize(n * n);
	h.fill(0.0f);
	hu.resize(n * n);
	hu.fill(0.0f);
	hv.resize(n * n);
	hv.fill(0.0f);
	written.resize(n * n);
	written.fill(0.0f);
	deposit.resize(n * n);
	deposit.fill(0.0f);
	released_volume = 0.0f;
	entrained_volume = 0.0f;
	deposited_volume = 0.0f;
	max_speed = 0.0f;
	run_out = 0.0f;
	flow_time = 0.0;
	flush_accum = 0.0;
	Vector3 c;
	float total = 0.0f;
	for (int k = 0; k < p_cells.size(); k++) {
		const int i = p_cells[k];
		const float rho = MAX(p_cover->cell_density[i], 30.0f);
		const float depth = p_cover->cell_swe[i] > 0.01f ? p_cover->cell_swe[i] / rho : 0.0f;
		const float s = MIN(p_slab[k], depth);
		if (s <= 0.0f) {
			continue;
		}
		// The slab's snow is already in the cover's cells: it starts moving where it lies.
		h.write[i] = s * rho;
		written.write[i] = s * rho;
		if (recent_res == n && recent_origin == flow_origin) {
			recent.write[i] = MAX(recent[i] - s, 0.0f);
		}
		released_volume += s * cell * cell;
		const Vector3 at((flow_origin.x + i % n + 0.5f) * cell, p_cover->cell_ground[i], (flow_origin.y + i / n + 0.5f) * cell);
		c += at * s;
		total += s;
	}
	if (total <= 0.0f) {
		return;
	}
	release_center = c / total;
	running = true;
	release_count++;
	emit_signal(SNAME("avalanche_released"), release_center, released_volume);
}

void SnowAvalanche3D::_flow_step(SnowCover3D *p_cover, float p_dt) {
	const int n = res;
	const int nn = n * n;
	// The snow surface the flow runs on: the ground, the cover's static snow and the flow itself.
	Vector<float> zs;
	zs.resize(nn);
	for (int i = 0; i < nn; i++) {
		const int ci = _cover_index(p_cover, i);
		if (ci < 0) {
			zs.write[i] = Math::NaN;
			continue;
		}
		const float rho = MAX(p_cover->cell_density[ci], 30.0f);
		const float stat = MAX(p_cover->cell_swe[ci] - written[i], 0.0f) / rho;
		zs.write[i] = p_cover->cell_ground[ci] + stat + h[i] / flow_density;
	}
	auto zat = [&](int x, int z, float fallback) {
		if (x < 0 || z < 0 || x >= n || z >= n) {
			return fallback;
		}
		const float v = zs[z * n + x];
		return Math::is_nan(v) ? fallback : v;
	};
	// Velocity: gravity down the surface, then Voellmy friction against the motion; deposition where the flow is
	// slow and its slope cannot overcome the dry friction; entrainment of the new snow it runs over.
	float vmax = 0.0f;
	for (int z = 0; z < n; z++) {
		for (int x = 0; x < n; x++) {
			const int i = z * n + x;
			if (h[i] < FLOW_MIN) {
				hu.write[i] = 0.0f;
				hv.write[i] = 0.0f;
				continue;
			}
			const int ci = _cover_index(p_cover, i);
			if (ci < 0) {
				continue;
			}
			const float z0 = zs[i];
			const float gx = (zat(x + 1, z, z0) - zat(x - 1, z, z0)) / (2.0f * cell);
			const float gz = (zat(x, z + 1, z0) - zat(x, z - 1, z0)) / (2.0f * cell);
			const float grad = Math::sqrt(gx * gx + gz * gz);
			const float cos_t = 1.0f / Math::sqrt(1.0f + grad * grad);
			const float depth = h[i] / flow_density;
			float u = hu[i] / h[i];
			float v = hv[i] / h[i];
			u -= GRAVITY * gx * cos_t * p_dt;
			v -= GRAVITY * gz * cos_t * p_dt;
			float speed = Math::sqrt(u * u + v * v);
			const float drag = (mu * GRAVITY * cos_t + GRAVITY * speed * speed / (xi * MAX(depth, 0.01f))) * p_dt;
			if (speed <= drag) {
				u = 0.0f;
				v = 0.0f;
				speed = 0.0f;
			} else {
				const float k = (speed - drag) / speed;
				u *= k;
				v *= k;
				speed -= drag;
			}
			if (speed < STOP_SPEED && GRAVITY * grad * cos_t <= mu * GRAVITY * cos_t) {
				// Deposited: the flow's snow stays in the cover as static snow, its surface broken into blocks near
				// the camera (avalanche debris: granules, Bartelt and McArdell 2009).
				deposited_volume += depth * cell * cell;
				_debris(Vector3((flow_origin.x + x + 0.5f) * cell, 0.0f, (flow_origin.y + z + 0.5f) * cell), depth * cell * cell);
				// The cover holds what was last written here; it may be less than the flow brought since.
				written.write[i] = written[i] - h[i];
				deposit.write[i] = deposit[i] + h[i];
				h.write[i] = 0.0f;
				hu.write[i] = 0.0f;
				hv.write[i] = 0.0f;
				continue;
			}
			if (speed > ENTRAIN_SPEED && recent_res == p_cover->resolution) {
				// recent is in the cover's cells now.
				const float take = MIN(recent[ci], entrainment * speed * p_dt);
				if (take > 0.0f) {
					const float rho = MAX(p_cover->cell_density[ci], 30.0f);
					recent.write[ci] = recent[ci] - take;
					h.write[i] = h[i] + take * rho;
					written.write[i] = written[i] + take * rho; // it was in the cover already
					entrained_volume += take * cell * cell;
				}
			}
			hu.write[i] = u * h[i];
			hv.write[i] = v * h[i];
			vmax = MAX(vmax, speed);
		}
	}
	max_speed = MAX(max_speed, vmax);
	// Donor-cell transport of mass and momentum to the four neighbours (CFL kept by the caller).
	Vector<float> h2 = h, hu2 = hu, hv2 = hv;
	const float k = p_dt / cell;
	for (int z = 0; z < n; z++) {
		for (int x = 0; x < n; x++) {
			const int i = z * n + x;
			if (h[i] < FLOW_MIN) {
				continue;
			}
			const float u = hu[i] / h[i];
			const float v = hv[i] / h[i];
			const float fx = Math::abs(u) * k, fz = Math::abs(v) * k;
			const float out = MIN(fx + fz, 0.9f);
			if (out <= 0.0f) {
				continue;
			}
			const float sx = fx / MAX(fx + fz, 1.0e-6f) * out, sz = out - sx;
			const int tx = x + (u > 0.0f ? 1 : -1), tz = z + (v > 0.0f ? 1 : -1);
			const float m = h[i], mu_ = hu[i], mv = hv[i];
			h2.write[i] -= m * out;
			hu2.write[i] -= mu_ * out;
			hv2.write[i] -= mv * out;
			if (tx >= 0 && tx < n && sx > 0.0f) {
				const int j = z * n + tx;
				h2.write[j] += m * sx;
				hu2.write[j] += mu_ * sx;
				hv2.write[j] += mv * sx;
			}
			if (tz >= 0 && tz < n && sz > 0.0f) {
				const int j = tz * n + x;
				h2.write[j] += m * sz;
				hu2.write[j] += mu_ * sz;
				hv2.write[j] += mv * sz;
			}
		}
	}
	h = h2;
	hu = hu2;
	hv = hv2;
	// How far the front got.
	for (int i = 0; i < nn; i++) {
		if (h[i] >= FLOW_MIN) {
			const Vector3 at((flow_origin.x + i % n + 0.5f) * cell, release_center.y, (flow_origin.y + i / n + 0.5f) * cell);
			run_out = MAX(run_out, Vector2(at.x - release_center.x, at.z - release_center.z).length());
		}
	}
}

void SnowAvalanche3D::_debris(const Vector3 &p_at, float p_volume) {
	SnowSurface3D *surface = SnowSurface3D::get_active();
	SnowClumps3D *clumps = surface ? surface->get_clumps_node() : nullptr;
	if (!clumps || !is_inside_tree()) {
		return;
	}
	const Camera3D *cam = get_viewport() ? get_viewport()->get_camera_3d() : nullptr;
	if (cam && Vector2(cam->get_global_position().x - p_at.x, cam->get_global_position().z - p_at.z).length() > debris_radius) {
		return;
	}
	// A share of the deposit's top as blocks, the cell's own size apart.
	clumps->drop(p_at, 0.5f * cell, p_volume * debris_share, 0.3f);
}

void SnowAvalanche3D::_flush(SnowCover3D *p_cover) {
	// The moving snow into the cover's cells, so the snow map, the getters and WorldSnow show it.
	bool any = false;
	for (int i = 0; i < res * res; i++) {
		const float d = h[i] - written[i];
		if (Math::abs(d) < 0.01f) {
			continue;
		}
		const int ci = _cover_index(p_cover, i);
		if (ci < 0) {
			written.write[i] = h[i];
			continue;
		}
		const float swe_old = MAX(p_cover->cell_swe[ci], 0.0f);
		const float rho_old = MAX(p_cover->cell_density[ci], 30.0f);
		const float swe_new = MAX(swe_old + d, 0.0f);
		if (d > 0.0f) {
			const float depth = (swe_old > 0.01f ? swe_old / rho_old : 0.0f) + d / flow_density;
			p_cover->cell_density.write[ci] = CLAMP(swe_new / MAX(depth, 1.0e-6f), 30.0f, 917.0f);
		}
		p_cover->cell_swe.write[ci] = swe_new;
		written.write[i] = h[i];
		any = true;
	}
	if (any) {
		p_cover->update_count++;
		p_cover->_upload();
	}
}

void SnowAvalanche3D::_hit_bodies(SnowCover3D *p_cover) {
	// Bodies in the flow take its dynamic pressure, 1/2 rho u^2, on about their width times the flow's depth.
	SnowBodies3D *bodies = p_cover->get_bodies();
	if (!bodies) {
		return;
	}
	const TypedArray<Node> list = bodies->get_tracked();
	for (int k = 0; k < list.size(); k++) {
		RigidBody3D *rb = Object::cast_to<RigidBody3D>(list[k]);
		if (!rb || !rb->is_inside_tree() || rb->is_freeze_enabled()) {
			continue;
		}
		const Vector3 p = rb->get_global_position();
		const int x = int(Math::floor(p.x / cell)) - flow_origin.x;
		const int z = int(Math::floor(p.z / cell)) - flow_origin.y;
		if (x < 0 || z < 0 || x >= res || z >= res) {
			continue;
		}
		const int i = z * res + x;
		if (h[i] < FLOW_MIN) {
			continue;
		}
		const Vector3 vel(hu[i] / h[i], 0.0f, hv[i] / h[i]);
		const float rel = vel.length();
		if (rel < 0.1f) {
			continue;
		}
		const float width = Math::sqrt(MAX(bodies->get_body_top_area(rb), 0.25f));
		const float area = width * MIN(h[i] / flow_density, 2.0f);
		rb->apply_central_force(vel / rel * 0.5f * flow_density * rel * rel * area);
	}
}

void SnowAvalanche3D::_finish(SnowCover3D *p_cover) {
	// What still moves stops where it is.
	for (int i = 0; i < res * res; i++) {
		if (h[i] > 0.0f) {
			deposited_volume += h[i] / flow_density * cell * cell;
			deposit.write[i] = deposit[i] + h[i];
		}
	}
	_flush(p_cover);
	// The donor-cell transport leaves the deposit uneven from cell to cell, which no debris field is at the grid's
	// scale (its blocks are the clumps): two mass-conserving smoothing passes, each cell giving an eighth of its
	// deposit to each neighbour.
	const int n = res;
	Vector<float> d = deposit;
	for (int pass = 0; pass < 2; pass++) {
		Vector<float> d2 = d;
		for (int z = 0; z < n; z++) {
			for (int x = 0; x < n; x++) {
				const int i = z * n + x;
				if (d[i] <= 0.0f) {
					continue;
				}
				const int nb[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
				for (const auto &o : nb) {
					const int xx = x + o[0], zz = z + o[1];
					if (xx < 0 || zz < 0 || xx >= n || zz >= n) {
						continue;
					}
					const float give = 0.125f * d[i];
					d2.write[i] -= give;
					d2.write[zz * n + xx] += give;
				}
			}
		}
		d = d2;
	}
	bool any = false;
	for (int i = 0; i < n * n; i++) {
		const float delta = d[i] - deposit[i];
		if (Math::abs(delta) < 0.01f) {
			continue;
		}
		const int ci = _cover_index(p_cover, i);
		if (ci < 0) {
			continue;
		}
		const float swe_old = MAX(p_cover->cell_swe[ci], 0.0f);
		const float rho_old = MAX(p_cover->cell_density[ci], 30.0f);
		const float swe_new = MAX(swe_old + delta, 0.0f);
		if (delta > 0.0f) {
			const float depth = (swe_old > 0.01f ? swe_old / rho_old : 0.0f) + delta / flow_density;
			p_cover->cell_density.write[ci] = CLAMP(swe_new / MAX(depth, 1.0e-6f), 30.0f, 917.0f);
		}
		p_cover->cell_swe.write[ci] = swe_new;
		any = true;
	}
	if (any) {
		p_cover->update_count++;
		p_cover->_upload();
	}
	written.fill(0.0f);
	h.fill(0.0f);
	hu.fill(0.0f);
	hv.fill(0.0f);
	running = false;
	emit_signal(SNAME("avalanche_stopped"), release_center, deposited_volume, run_out);
}

void SnowAvalanche3D::step(double p_delta) {
	SnowCover3D *cover = _cover();
	if (!cover || !running) {
		return;
	}
	cover->_finish_background(true);
	double left = p_delta;
	while (left > 1.0e-6 && running) {
		// CFL: a cell at most 0.4 of the way per substep.
		const float dt = float(MIN(left, double(0.4f * cell / MAX(max_speed + 2.0f, 1.0f))));
		_flow_step(cover, dt);
		left -= dt;
		flow_time += dt;
	}
	flush_accum += p_delta;
	const float moving = get_flowing_volume();
	if (flow_time > max_time || (flow_time > 1.0 && moving < 0.01f * MAX(released_volume, 1.0f))) {
		_finish(cover);
		return;
	}
	if (flush_accum >= FLUSH_INTERVAL) {
		flush_accum = 0.0;
		_flush(cover);
	}
}

bool SnowAvalanche3D::trigger(const Vector3 &p_at, float p_radius) {
	SnowCover3D *cover = _cover();
	ERR_FAIL_NULL_V(cover, false);
	if (running) {
		return false;
	}
	cover->_finish_background(true);
	_track_recent(cover);
	const int n = cover->resolution;
	if (recent_res != n || cover->cell_swe.size() != n * n) {
		return false;
	}
	// Whatever slab lies there goes, stable or not (the trigger supplies the rest), on any slope over the minimum.
	const float cs = cover->_cell_size();
	const float lo = Math::deg_to_rad(min_slope);
	Vector<int> cells;
	Vector<float> amounts;
	for (int z = 0; z < n; z++) {
		for (int x = 0; x < n; x++) {
			const Vector2 at((cover->origin_cell.x + x + 0.5f) * cs, (cover->origin_cell.y + z + 0.5f) * cs);
			if (at.distance_to(Vector2(p_at.x, p_at.z)) > p_radius) {
				continue;
			}
			const int i = z * n + x;
			if (_slope_at(cover, x, z) < lo) {
				continue;
			}
			const float s = recent[i] > 0.0f ? recent[i] : (cover->pub.crust.size() == n * n && cover->pub.crust[i] > 0.0f ? cover->pub.crust_burial[i] : 0.0f);
			if (s > 0.01f) {
				cells.push_back(i);
				amounts.push_back(s);
			}
		}
	}
	if (cells.is_empty()) {
		return false;
	}
	_release(cover, cells, amounts);
	return running;
}

float SnowAvalanche3D::get_flowing_volume() const {
	float v = 0.0f;
	for (int i = 0; i < h.size(); i++) {
		v += h[i];
	}
	return v / flow_density * cell * cell;
}

float SnowAvalanche3D::get_flow_height_at(const Vector3 &p_position) const {
	if (!running) {
		return 0.0f;
	}
	const int x = int(Math::floor(p_position.x / cell)) - flow_origin.x;
	const int z = int(Math::floor(p_position.z / cell)) - flow_origin.y;
	if (x < 0 || z < 0 || x >= res || z >= res) {
		return 0.0f;
	}
	return h[z * res + x] / flow_density;
}

Vector3 SnowAvalanche3D::get_flow_velocity_at(const Vector3 &p_position) const {
	if (!running) {
		return Vector3();
	}
	const int x = int(Math::floor(p_position.x / cell)) - flow_origin.x;
	const int z = int(Math::floor(p_position.z / cell)) - flow_origin.y;
	if (x < 0 || z < 0 || x >= res || z >= res) {
		return Vector3();
	}
	const int i = z * res + x;
	return h[i] >= FLOW_MIN ? Vector3(hu[i] / h[i], 0.0f, hv[i] / h[i]) : Vector3();
}

float SnowAvalanche3D::get_recent_snow_at(const Vector3 &p_position) const {
	const SnowCover3D *cover = _cover();
	if (!cover || recent_res != cover->resolution) {
		return 0.0f;
	}
	const float cs = cover->_cell_size();
	const int x = int(Math::floor(p_position.x / cs)) - recent_origin.x;
	const int z = int(Math::floor(p_position.z / cs)) - recent_origin.y;
	if (x < 0 || z < 0 || x >= recent_res || z >= recent_res) {
		return 0.0f;
	}
	return recent[z * recent_res + x];
}

void SnowAvalanche3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			if (!Engine::get_singleton()->is_editor_hint()) {
				set_physics_process_internal(true);
			}
		} break;
		case NOTIFICATION_INTERNAL_PHYSICS_PROCESS: {
			SnowCover3D *cover = _cover();
			if (!cover) {
				break;
			}
			if (running) {
				step(get_physics_process_delta_time());
				if (running) {
					_hit_bodies(cover);
				}
				break;
			}
			if (cover->update_count != seen_update && !cover->is_updating()) {
				seen_update = cover->update_count;
				_track_recent(cover);
				if (natural_release) {
					_check_release(cover);
				}
			}
		} break;
	}
}

void SnowAvalanche3D::_bind_methods() {
	ClassDB::bind_static_method("SnowAvalanche3D", D_METHOD("weak_layer_strength", "density", "age_hours"), &SnowAvalanche3D::weak_layer_strength);
	ClassDB::bind_static_method("SnowAvalanche3D", D_METHOD("stability_index", "slab", "slab_density", "weak_density", "age_hours", "slope"), &SnowAvalanche3D::stability_index);
	ClassDB::bind_method(D_METHOD("trigger", "at", "radius"), &SnowAvalanche3D::trigger);
	ClassDB::bind_method(D_METHOD("step", "delta"), &SnowAvalanche3D::step);
	ClassDB::bind_method(D_METHOD("is_running"), &SnowAvalanche3D::is_running);
	ClassDB::bind_method(D_METHOD("get_release_count"), &SnowAvalanche3D::get_release_count);
	ClassDB::bind_method(D_METHOD("get_released_volume"), &SnowAvalanche3D::get_released_volume);
	ClassDB::bind_method(D_METHOD("get_entrained_volume"), &SnowAvalanche3D::get_entrained_volume);
	ClassDB::bind_method(D_METHOD("get_deposited_volume"), &SnowAvalanche3D::get_deposited_volume);
	ClassDB::bind_method(D_METHOD("get_flowing_volume"), &SnowAvalanche3D::get_flowing_volume);
	ClassDB::bind_method(D_METHOD("get_max_speed"), &SnowAvalanche3D::get_max_speed);
	ClassDB::bind_method(D_METHOD("get_run_out"), &SnowAvalanche3D::get_run_out);
	ClassDB::bind_method(D_METHOD("get_flow_height_at", "position"), &SnowAvalanche3D::get_flow_height_at);
	ClassDB::bind_method(D_METHOD("get_flow_velocity_at", "position"), &SnowAvalanche3D::get_flow_velocity_at);
	ClassDB::bind_method(D_METHOD("get_recent_snow_at", "position"), &SnowAvalanche3D::get_recent_snow_at);
	ClassDB::bind_method(D_METHOD("set_mu", "mu"), &SnowAvalanche3D::set_mu);
	ClassDB::bind_method(D_METHOD("get_mu"), &SnowAvalanche3D::get_mu);
	ClassDB::bind_method(D_METHOD("set_xi", "xi"), &SnowAvalanche3D::set_xi);
	ClassDB::bind_method(D_METHOD("get_xi"), &SnowAvalanche3D::get_xi);
	ClassDB::bind_method(D_METHOD("set_min_slope", "degrees"), &SnowAvalanche3D::set_min_slope);
	ClassDB::bind_method(D_METHOD("get_min_slope"), &SnowAvalanche3D::get_min_slope);
	ClassDB::bind_method(D_METHOD("set_max_slope", "degrees"), &SnowAvalanche3D::set_max_slope);
	ClassDB::bind_method(D_METHOD("get_max_slope"), &SnowAvalanche3D::get_max_slope);
	ClassDB::bind_method(D_METHOD("set_min_slab", "depth"), &SnowAvalanche3D::set_min_slab);
	ClassDB::bind_method(D_METHOD("get_min_slab"), &SnowAvalanche3D::get_min_slab);
	ClassDB::bind_method(D_METHOD("set_entrainment", "rate"), &SnowAvalanche3D::set_entrainment);
	ClassDB::bind_method(D_METHOD("get_entrainment"), &SnowAvalanche3D::get_entrainment);
	ClassDB::bind_method(D_METHOD("set_natural_release", "enable"), &SnowAvalanche3D::set_natural_release);
	ClassDB::bind_method(D_METHOD("is_natural_release"), &SnowAvalanche3D::is_natural_release);

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "mu", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_mu", "get_mu");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "xi", PROPERTY_HINT_RANGE, "100,10000,1,suffix:m/s²"), "set_xi", "get_xi");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "min_slope", PROPERTY_HINT_RANGE, "0,90,0.1,degrees"), "set_min_slope", "get_min_slope");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "max_slope", PROPERTY_HINT_RANGE, "0,90,0.1,degrees"), "set_max_slope", "get_max_slope");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "min_slab", PROPERTY_HINT_RANGE, "0,3,0.01,suffix:m"), "set_min_slab", "get_min_slab");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "entrainment", PROPERTY_HINT_RANGE, "0,0.1,0.0001"), "set_entrainment", "get_entrainment");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "natural_release"), "set_natural_release", "is_natural_release");

	ADD_SIGNAL(MethodInfo("avalanche_released", PropertyInfo(Variant::VECTOR3, "position"), PropertyInfo(Variant::FLOAT, "volume")));
	ADD_SIGNAL(MethodInfo("avalanche_stopped", PropertyInfo(Variant::VECTOR3, "position"), PropertyInfo(Variant::FLOAT, "volume"), PropertyInfo(Variant::FLOAT, "run_out")));
}

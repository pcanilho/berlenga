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
#include "core/object/callable_mp.h"
#include "modules/ground/ground_deform_field.h"
#include "modules/world/world_contracts.h"
#include "core/object/class_db.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/physics/rigid_body_3d.h"
#include "scene/main/scene_tree.h"
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
struct SourceByRate {
	bool operator()(const SnowPowderCloud3D::Source &p_a, const SnowPowderCloud3D::Source &p_b) const { return p_a.rate > p_b.rate; }
};
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
	// with the cover's hours, a week bonds it, and it is never more than the pack. The cover calls this at the end
	// of every update (main thread, its cells finished), so no update's snow or hours are missed.
	const int n = p_cover->resolution;
	if (p_cover->cell_swe.size() != n * n || p_cover->cell_density.size() != n * n || p_cover->cell_ground.size() != n * n) {
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
	const bool has_new = p_cover->cell_new_depth.size() == n * n;
	for (int i = 0; i < n * n; i++) {
		const float add = has_new ? MAX(p_cover->cell_new_depth[i], 0.0f) : 0.0f;
		float r = recent[i];
		float a = recent_age[i] + hours;
		if (add > 0.0f) {
			a = (a * r) / (r + add);
			r += add;
		}
		const float rho = MAX(p_cover->cell_density[i], 30.0f);
		const float depth = p_cover->cell_swe[i] > 0.01f ? p_cover->cell_swe[i] / rho : 0.0f;
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
	peak.resize(n * n);
	peak.fill(0.0f);
	released_volume = 0.0f;
	entrained_volume = 0.0f;
	deposited_volume = 0.0f;
	max_speed = 0.0f;
	max_speed_terminal = 0.0f;
	powder_taken = 0.0f;
	powder_laid = 0.0f;
	powder_reach = 0.0f;
	run_out = 0.0f;
	flow_time = 0.0;
	flush_accum = 0.0;
	Vector3 c;
	float total = 0.0f;
	release_liquid = 0.0f;
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
		if (p_cover->cell_liquid.size() == p_cover->cell_swe.size()) {
			release_liquid += p_cover->cell_liquid[i] / MAX(p_cover->cell_swe[i], 1.0f) * s;
		}
		const Vector3 at((flow_origin.x + i % n + 0.5f) * cell, p_cover->cell_ground[i], (flow_origin.y + i / n + 0.5f) * cell);
		c += at * s;
		total += s;
	}
	release_liquid = total > 0.0f ? release_liquid / total : 0.0f; // the slab's liquid water share of its mass
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
	float vmax_depth = 0.0f;
	float vmax_cos = 1.0f;
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
				// Blocks only where the flow arrived fast (the run-out), not the thin remnants in the release zone.
				if (peak[i] > ENTRAIN_SPEED) {
					_debris(Vector3((flow_origin.x + x + 0.5f) * cell, 0.0f, (flow_origin.y + z + 0.5f) * cell), depth * cell * cell);
				}
				// The cover holds what was last written here; it may be less than the flow brought since.
				written.write[i] = written[i] - h[i];
				deposit.write[i] = deposit[i] + h[i];
				h.write[i] = 0.0f;
				hu.write[i] = 0.0f;
				hv.write[i] = 0.0f;
				continue;
			}
			if (powder_enabled && speed > powder_speed) {
				// The front of a fast dry flow blows snow into the air (the eruption current at the front of a powder
				// avalanche, Sovilla et al. 2015; a second, suspension layer fed by the dense core, Bartelt et al.
				// 2016): a share of its mass per 10 m it runs over the speed. Wet snow does not (no cloud on a wet
				// slide).
				const int dx = Math::abs(u) >= Math::abs(v) ? (u > 0.0f ? 1 : -1) : 0;
				const int dz = dx == 0 ? (v > 0.0f ? 1 : -1) : 0;
				const int fx = x + dx, fz = z + dz;
				const bool front = fx < 0 || fz < 0 || fx >= n || fz >= n || h[fz * n + fx] < h[i]; // the leading slope of the flow
				if (front && release_liquid < 0.001f) { // dry snow holds no liquid water
					const float take = MIN(0.5f * h[i], powder_share * h[i] * (speed - powder_speed) * 0.1f * p_dt);
					const float m = take * cell * cell;
					const Vector3 at((flow_origin.x + x + 0.5f) * cell, p_cover->cell_ground[ci] + MAX(p_cover->cell_swe[ci] - written[i], 0.0f) / MAX(p_cover->cell_density[ci], 30.0f) + depth, (flow_origin.y + z + 0.5f) * cell);
					Bin &b = bins[Vector2i(int(Math::floor(at.x / 8.0f)), int(Math::floor(at.z / 8.0f)))];
					b.mass += m;
					b.momentum += Vector3(u, 0.0f, v) * m;
					b.position += at * m;
					powder_taken += m;
					h.write[i] = h[i] - take;
				}
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
			if (speed > vmax) {
				vmax = speed;
				vmax_depth = depth;
				vmax_cos = cos_t;
			}
			peak.write[i] = MAX(peak[i], speed);
		}
	}
	if (vmax > max_speed) {
		max_speed = vmax;
		// Voellmy's steady speed at that depth and slope: sqrt(xi h (sin - mu cos)).
		const float sin_t = Math::sqrt(MAX(1.0f - vmax_cos * vmax_cos, 0.0f));
		max_speed_terminal = Math::sqrt(MAX(xi * vmax_depth * (sin_t - mu * vmax_cos), 0.0f));
	}
	_transport(p_dt);
	// How far the front got.
	for (int i = 0; i < nn; i++) {
		if (h[i] >= FLOW_MIN) {
			const Vector3 at((flow_origin.x + i % n + 0.5f) * cell, release_center.y, (flow_origin.y + i / n + 0.5f) * cell);
			run_out = MAX(run_out, Vector2(at.x - release_center.x, at.z - release_center.z).length());
		}
	}
}

namespace {
inline float minmod(float p_a, float p_b) {
	return (p_a * p_b <= 0.0f) ? 0.0f : (Math::abs(p_a) < Math::abs(p_b) ? p_a : p_b);
}
} // namespace

void SnowAvalanche3D::_fluxes(const Vector<float> &p_h, const Vector<float> &p_hu, const Vector<float> &p_hv, float p_dt, Vector<float> &r_dh, Vector<float> &r_dhu, Vector<float> &r_dhv) const {
	// Second-order finite volumes for the flow's advection (RAMMS solves Voellmy's equations this way, Christen et
	// al. 2010): MUSCL reconstruction with the minmod limiter of the mass and the velocities at each face, then the
	// HLL Riemann flux (Harten, Lax and van Leer 1983; Toro 2001 for shallow flows) with wave speeds u +- c, c the
	// gravity wave speed sqrt(g h). Gravity and the earth pressure stay in the source step (the gradient of the snow
	// surface with the flow on it), so snow at rest stays at rest. A cell outside the grid is a copy of its
	// neighbour: the flow leaves freely.
	const int n = res;
	const float k = p_dt / cell;
	r_dh.resize(n * n);
	r_dh.fill(0.0f);
	r_dhu.resize(n * n);
	r_dhu.fill(0.0f);
	r_dhv.resize(n * n);
	r_dhv.fill(0.0f);
	auto vel = [&](int i, float &r_u, float &r_v) {
		if (p_h[i] < FLOW_MIN) {
			r_u = 0.0f;
			r_v = 0.0f;
		} else {
			r_u = p_hu[i] / p_h[i];
			r_v = p_hv[i] / p_h[i];
		}
	};
	for (int axis = 0; axis < 2; axis++) {
		for (int a = 0; a < n; a++) {
			// One line of cells along the axis: x when axis is 0, z when 1. Faces b - 1/2 to b + 1/2.
			auto idx = [&](int b) {
				b = CLAMP(b, 0, n - 1);
				return axis == 0 ? a * n + b : b * n + a;
			};
			for (int b = -1; b < n; b++) {
				// The face between cells b and b + 1.
				const int im = idx(b - 1), il = idx(b), ir = idx(b + 1), ip = idx(b + 2);
				const float hm = p_h[im], hl = p_h[il], hr = p_h[ir], hp = p_h[ip];
				if (hl < FLOW_MIN && hr < FLOW_MIN) {
					continue;
				}
				float um, vm, ul, vl, ur, vr, up, vp;
				vel(im, um, vm);
				vel(il, ul, vl);
				vel(ir, ur, vr);
				vel(ip, up, vp);
				// Normal (n) and tangential (t) velocities along this axis.
				const float nm = axis == 0 ? um : vm, nl = axis == 0 ? ul : vl, nr = axis == 0 ? ur : vr, np = axis == 0 ? up : vp;
				const float tm = axis == 0 ? vm : um, tl = axis == 0 ? vl : ul, tr = axis == 0 ? vr : ur, tp = axis == 0 ? vp : up;
				const float HL = MAX(hl + 0.5f * minmod(hl - hm, hr - hl), 0.0f);
				const float HR = MAX(hr - 0.5f * minmod(hr - hl, hp - hr), 0.0f);
				const float NL = nl + 0.5f * minmod(nl - nm, nr - nl);
				const float NR = nr - 0.5f * minmod(nr - nl, np - nr);
				const float TL = tl + 0.5f * minmod(tl - tm, tr - tl);
				const float TR = tr - 0.5f * minmod(tr - tl, tp - tr);
				const float cl = Math::sqrt(GRAVITY * HL / flow_density), cr = Math::sqrt(GRAVITY * HR / flow_density);
				float sl, sr;
				if (HL < FLOW_MIN) {
					sl = NR - 2.0f * cr;
					sr = NR + cr;
				} else if (HR < FLOW_MIN) {
					sl = NL - cl;
					sr = NL + 2.0f * cl;
				} else {
					sl = MIN(NL - cl, NR - cr);
					sr = MAX(NL + cl, NR + cr);
				}
				// The fluxes of (h, h un, h ut) on each side.
				const float fl0 = HL * NL, fl1 = HL * NL * NL, fl2 = HL * NL * TL;
				const float fr0 = HR * NR, fr1 = HR * NR * NR, fr2 = HR * NR * TR;
				float f0, f1, f2;
				if (sl >= 0.0f) {
					f0 = fl0;
					f1 = fl1;
					f2 = fl2;
				} else if (sr <= 0.0f) {
					f0 = fr0;
					f1 = fr1;
					f2 = fr2;
				} else {
					const float d = 1.0f / (sr - sl);
					f0 = (sr * fl0 - sl * fr0 + sl * sr * (HR - HL)) * d;
					f1 = (sr * fl1 - sl * fr1 + sl * sr * (HR * NR - HL * NL)) * d;
					f2 = (sr * fl2 - sl * fr2 + sl * sr * (HR * TR - HL * TL)) * d;
				}
				// Out of cell b, into cell b + 1 (a cell beyond the edge is not updated: what crosses is gone).
				const float fu = axis == 0 ? f1 : f2, fv = axis == 0 ? f2 : f1;
				if (b >= 0) {
					r_dh.write[il] -= k * f0;
					r_dhu.write[il] -= k * fu;
					r_dhv.write[il] -= k * fv;
				}
				if (b + 1 < n) {
					r_dh.write[ir] += k * f0;
					r_dhu.write[ir] += k * fu;
					r_dhv.write[ir] += k * fv;
				}
			}
		}
	}
}

void SnowAvalanche3D::_transport(float p_dt) {
	// Heun's two stages (second order in time, total variation diminishing with the limiter at CFL under 0.5).
	const int nn = res * res;
	Vector<float> dh, dhu, dhv;
	_fluxes(h, hu, hv, p_dt, dh, dhu, dhv);
	Vector<float> h1 = h, hu1 = hu, hv1 = hv;
	for (int i = 0; i < nn; i++) {
		h1.write[i] = MAX(h[i] + dh[i], 0.0f);
		hu1.write[i] = hu[i] + dhu[i];
		hv1.write[i] = hv[i] + dhv[i];
	}
	Vector<float> dh2, dhu2, dhv2;
	_fluxes(h1, hu1, hv1, p_dt, dh2, dhu2, dhv2);
	for (int i = 0; i < nn; i++) {
		const float hn = 0.5f * (h[i] + h1[i] + dh2[i]);
		if (hn < 0.0f) {
			// Rounding at a drying edge: take nothing from a neighbour, give up the scrap (counted as lost).
			h.write[i] = 0.0f;
			hu.write[i] = 0.0f;
			hv.write[i] = 0.0f;
			continue;
		}
		h.write[i] = hn;
		hu.write[i] = 0.5f * (hu[i] + hu1[i] + dhu2[i]);
		hv.write[i] = 0.5f * (hv[i] + hv1[i] + dhv2[i]);
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
	// The grid-scale transport leaves the deposit uneven from cell to cell, which no debris field is at the grid's
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
	if (!cover) {
		return;
	}
	cover->_finish_background(true);
	if (!running) {
		_powder_step(cover, float(p_delta));
		return;
	}
	double left = p_delta;
	while (left > 1.0e-6 && running) {
		// CFL on the fastest wave, |u| + sqrt(g h), each way: 0.2 of a cell per substep (two axes at once).
		float wave = 1.0f;
		for (int i = 0; i < h.size(); i++) {
			if (h[i] >= FLOW_MIN) {
				const float sp = Math::sqrt(hu[i] * hu[i] + hv[i] * hv[i]) / h[i];
				wave = MAX(wave, sp + Math::sqrt(GRAVITY * h[i] / flow_density));
			}
		}
		const float dt = float(MIN(left, double(0.2f * cell / wave)));
		_flow_step(cover, dt);
		left -= dt;
		flow_time += dt;
	}
	flush_accum += p_delta;
	_powder_step(cover, float(p_delta));
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

// ---------------------------------------------------------------------------------------------------------
// Triggers (docs/ideas/snow.md, follow-up 21)

float SnowAvalanche3D::added_shear(float p_line_load, float p_depth, float p_slope) {
	// Foehn (1987), from Flamant's line load on an elastic half-space (the slope): the stress is radial,
	// 2 P sin(t + a) / (pi r) at r from the load and t below the surface, the vertical load a slope of a off the
	// surface's normal. Its shear on a plane parallel to the slope is that times sin(t) cos(t). At the weak layer,
	// D deep normal to the slope, r = D / sin(t): the peak over t. On the flat it is 0.207 P / D, Flamant's.
	const float depth = MAX(p_depth, 0.02f);
	float best = 0.0f;
	for (int k = 1; k < 128; k++) {
		const float t = Math::PI * k / 128.0f;
		const float st = Math::sin(t);
		best = MAX(best, Math::sin(t + p_slope) * st * st * Math::cos(t));
	}
	return 2.0f * MAX(p_line_load, 0.0f) * best / (Math::PI * depth);
}

float SnowAvalanche3D::_trigger_index(const SnowCover3D *p_cover, int p_x, int p_z, float p_line_load, float p_sink, float &r_slab, bool &r_crust) const {
	// Foehn's skier stability index: the weak layer's strength over the slab's own shear plus the load's. A load
	// sunk into the snow (a boot in powder) is that much nearer the weak layer.
	const int n = p_cover->resolution;
	const int i = p_z * n + p_x;
	r_slab = 0.0f;
	r_crust = false;
	if (recent_res != n || p_cover->pub.swe.size() != n * n) {
		return 1.0e6f;
	}
	const float slope = _slope_at(p_cover, p_x, p_z);
	const float rho = MAX(p_cover->pub.density[i], 30.0f);
	const float sc = Math::sin(slope) * Math::cos(slope);
	float best = 1.0e6f;
	auto test = [&](float p_slab, float p_strength, bool p_is_crust) {
		const float d = MAX(p_slab - p_sink, 0.02f) * Math::cos(slope);
		const float tau = rho * GRAVITY * p_slab * sc + added_shear(p_line_load, d, slope);
		const float sk = tau > 1.0e-3f ? p_strength / tau : 1.0e6f;
		if (sk < best) {
			best = sk;
			r_slab = p_slab;
			r_crust = p_is_crust;
		}
	};
	if (recent[i] >= min_slab) {
		test(recent[i], weak_layer_strength(WEAK_DENSITY, recent_age[i]), false);
	}
	const bool crusts = p_cover->pub.crust.size() == n * n && p_cover->pub.crust_burial.size() == n * n;
	if (crusts && p_cover->pub.crust[i] > 0.0f && p_cover->pub.crust_burial[i] >= min_slab && !(collapsed.size() == n * n && collapsed[i])) {
		test(p_cover->pub.crust_burial[i], weak_layer_strength(BURIED_WEAK_DENSITY, 0.0f), true);
	}
	return best;
}

float SnowAvalanche3D::get_trigger_index_at(const Vector3 &p_at, float p_line_load, float p_sink) const {
	const SnowCover3D *cover = _cover();
	ERR_FAIL_NULL_V(cover, 1.0e6f);
	const float cs = cover->_cell_size();
	const int x = int(Math::floor(p_at.x / cs)) - cover->origin_cell.x;
	const int z = int(Math::floor(p_at.z / cs)) - cover->origin_cell.y;
	if (x < 0 || z < 0 || x >= cover->resolution || z >= cover->resolution) {
		return 1.0e6f;
	}
	float slab;
	bool crust;
	return _trigger_index(cover, x, z, p_line_load, p_sink, slab, crust);
}

int SnowAvalanche3D::_load(SnowCover3D *p_cover, const Vector3 &p_at, float p_line_load, float p_sink, Node *p_body) {
	const int n = p_cover->resolution;
	const float cs = p_cover->_cell_size();
	const int x0 = int(Math::floor(p_at.x / cs)) - p_cover->origin_cell.x;
	const int z0 = int(Math::floor(p_at.z / cs)) - p_cover->origin_cell.y;
	if (running || x0 < 0 || z0 < 0 || x0 >= n || z0 >= n || recent_res != n) {
		return 0;
	}
	float slab = 0.0f;
	bool crust = false;
	if (_trigger_index(p_cover, x0, z0, p_line_load, p_sink, slab, crust) >= 1.0f) {
		return 0;
	}
	const float lo = Math::deg_to_rad(min_slope), hi = Math::deg_to_rad(max_slope);
	const int seed = z0 * n + x0;
	const float seed_slope = _slope_at(p_cover, x0, z0);
	// A fracture opened in a slab runs on through slab like it (a uniform storm slab goes as a whole), and through
	// any near failure on its own.
	const float spread = MAX(propagation_index, crust ? 0.0f : 1.2f * stability_index(slab, MAX(p_cover->pub.density[seed], 30.0f), WEAK_DENSITY, recent_age[seed], seed_slope));
	if (!crust && (seed_slope < lo || seed_slope > hi)) {
		return 0; // the new snow fails under the load, but on ground too flat to slide
	}
	// The fracture spreads from under the load. In new snow it runs through the slab on avalanche terrain while the
	// slab is near failure; along a buried crust's facets it runs anywhere the crust does, flat ground too, for up to
	// collapse_range (a whumpf, which releases the steep slopes it reaches: remote triggering; Johnson et al. 2004,
	// the propagation saw test of Gauthier and Jamieson 2008).
	if (crust && collapsed.size() != n * n) {
		collapsed.resize(n * n);
		collapsed.fill(0);
	}
	const bool crusts = p_cover->pub.crust.size() == n * n && p_cover->pub.crust_burial.size() == n * n;
	Vector<int> cells;
	Vector<float> amounts;
	Vector<uint8_t> seen;
	seen.resize(n * n);
	seen.fill(0);
	Vector<int> stack;
	stack.push_back(seed);
	seen.write[seed] = 1;
	int visited = 0;
	while (!stack.is_empty() && visited < MAX_RELEASE_CELLS) {
		const int i = stack[stack.size() - 1];
		stack.resize(stack.size() - 1);
		visited++;
		const int x = i % n, z = i / n;
		const float slope = _slope_at(p_cover, x, z);
		const float rho = MAX(p_cover->pub.density[i], 30.0f);
		const bool steep = slope >= lo && slope <= hi;
		if (crust) {
			collapsed.write[i] = 1;
			const float b = p_cover->pub.crust_burial[i];
			if (steep && (i == seed || stability_index(b, rho, BURIED_WEAK_DENSITY, 0.0f, slope) < propagation_index)) {
				cells.push_back(i);
				amounts.push_back(b);
			}
		} else {
			cells.push_back(i);
			amounts.push_back(i == seed ? slab : recent[i]);
		}
		const int nb[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
		for (const auto &o : nb) {
			const int xx = x + o[0], zz = z + o[1];
			if (xx < 0 || zz < 0 || xx >= n || zz >= n) {
				continue;
			}
			const int j = zz * n + xx;
			if (seen[j]) {
				continue;
			}
			bool go;
			if (crust) {
				go = crusts && p_cover->pub.crust[j] > 0.0f && p_cover->pub.crust_burial[j] >= 0.5f * min_slab && !collapsed[j] && Vector2(xx - x0, zz - z0).length() * cs <= collapse_range;
			} else {
				const float sj = _slope_at(p_cover, xx, zz);
				go = sj >= lo && sj <= hi && recent[j] >= 0.5f * min_slab && stability_index(recent[j], MAX(p_cover->pub.density[j], 30.0f), WEAK_DENSITY, recent_age[j], sj) < spread;
			}
			if (go) {
				seen.write[j] = 1;
				stack.push_back(j);
			}
		}
	}
	if (crust) {
		collapse_count++;
		emit_signal(SNAME("snow_collapsed"), p_at, float(visited) * cs * cs);
	}
	if (cells.is_empty()) {
		return crust ? 2 : 0;
	}
	_release(p_cover, cells, amounts);
	if (!running) {
		return crust ? 2 : 0;
	}
	trigger_count++;
	emit_signal(SNAME("avalanche_triggered"), p_at, p_body);
	return 1;
}

int SnowAvalanche3D::apply_load(const Vector3 &p_at, float p_line_load, float p_sink) {
	SnowCover3D *cover = _cover();
	ERR_FAIL_NULL_V(cover, 0);
	cover->_finish_background(true);
	return _load(cover, p_at, p_line_load, p_sink, nullptr);
}

void SnowAvalanche3D::_check_triggers(SnowCover3D *p_cover) {
	// Everything that presses the snow (the groups SnowSurface3D prints) loads the slab under it. Its weight is
	// spread along its footprint, a metre at least (Foehn's skier, 80 kg: 0.78 kN/m); what it has sunk in (the
	// print, read from WorldSnow) brings it nearer the weak layer.
	if (!is_inside_tree() || p_cover->is_updating()) {
		return;
	}
	SceneTree *tree = get_tree();
	WorldSnow *ws = WorldContracts::snow();
	HashSet<ObjectID> done;
	for (const String &group : trigger_groups) {
		const Vector<Node *> nodes = tree->get_nodes_in_group(StringName(group));
		for (Node *node : nodes) {
			Node3D *n3 = Object::cast_to<Node3D>(node);
			if (!n3 || !n3->is_visible_in_tree() || done.has(n3->get_instance_id())) {
				continue;
			}
			done.insert(n3->get_instance_id());
			GroundDeformField::Body b;
			GroundDeformField::read_body(n3, character_mass, 0.035f, b);
			if (b.mass <= 0.0f) {
				continue;
			}
			const float depth = p_cover->get_depth_at(b.center);
			if (depth < min_slab) {
				continue;
			}
			const float top = p_cover->get_ground_height_at(b.center) + depth;
			if (b.center.y - b.half_height > top + 0.25f) {
				continue; // in the air
			}
			float sink = 0.0f;
			const float surface = ws ? ws->surface_at(b.center) : -Math::INF;
			if (Math::is_finite(surface)) {
				sink = CLAMP(top - surface, 0.0f, depth);
			}
			const float extent = b.box ? 2.0f * MAX(b.box_half.x, b.box_half.y) : 2.0f * b.radius;
			const float load = b.mass * GRAVITY / MAX(extent, 1.0f);
			if (_load(p_cover, b.center, load, sink, n3) != 0 || running) {
				return;
			}
		}
	}
}

void SnowAvalanche3D::_on_cornice_failed(const Vector3 &p_position, const Vector3 &p_direction, float p_length, float p_mass) {
	// A failed cornice drops onto the lee slope below it: its weight along its length, doubled for the impact
	// (Vogel et al. 2012 measured cornice blocks triggering slabs on impact).
	SnowCover3D *cover = _cover();
	if (!cover || !triggers_enabled) {
		return;
	}
	const Vector3 at = p_position + p_direction.normalized() * MAX(2.0f * cover->_cell_size(), 3.0f);
	apply_load(at, 2.0f * p_mass * GRAVITY / MAX(p_length, 1.0f), 0.0f);
}

// ---------------------------------------------------------------------------------------------------------
// Powder cloud (docs/ideas/snow.md, follow-up 23)

float SnowAvalanche3D::get_suspended_mass() const {
	float m = 0.0f;
	for (const Parcel &p : parcels) {
		m += p.mass + p.owed;
	}
	for (const KeyValue<Vector2i, Bin> &kv : bins) {
		m += kv.value.mass;
	}
	return m;
}

float SnowAvalanche3D::get_powder_density_at(const Vector3 &p_position) const {
	// The parcels as cylinders of their radius and 0.6 of it high: 3-10 kg/m3 in a fresh cloud (Turnbull and
	// McElwaine 2008; Sovilla et al. 2015), thinning as it spreads and settles.
	float rho = 0.0f;
	for (const Parcel &p : parcels) {
		const Vector3 d = p_position - p.position;
		if (Vector2(d.x, d.z).length() > p.radius || d.y < -1.0f || d.y > 0.6f * p.radius) {
			continue;
		}
		rho += p.mass / (Math::PI * p.radius * p.radius * 0.6f * p.radius);
	}
	return MIN(rho, 10.0f);
}

void SnowAvalanche3D::_powder_lay(SnowCover3D *p_cover, const Parcel &p_parcel, float p_mass) {
	// Settled snow spreads over the parcel's disc on the cover's cells (kg/m2, a cone so the layer feathers out).
	const int n = p_cover->resolution;
	const float cs = p_cover->_cell_size();
	if (powder_pending.size() != n * n) {
		powder_pending.resize(n * n);
		powder_pending.fill(0.0f);
	}
	const float r = MAX(p_parcel.radius, cs);
	const int x0 = int(Math::floor((p_parcel.position.x - r) / cs)) - p_cover->origin_cell.x;
	const int x1 = int(Math::floor((p_parcel.position.x + r) / cs)) - p_cover->origin_cell.x;
	const int z0 = int(Math::floor((p_parcel.position.z - r) / cs)) - p_cover->origin_cell.y;
	const int z1 = int(Math::floor((p_parcel.position.z + r) / cs)) - p_cover->origin_cell.y;
	float total = 0.0f;
	for (int pass = 0; pass < 2; pass++) {
		for (int z = MAX(z0, 0); z <= MIN(z1, n - 1); z++) {
			for (int x = MAX(x0, 0); x <= MIN(x1, n - 1); x++) {
				const float cx = (p_cover->origin_cell.x + x + 0.5f) * cs - p_parcel.position.x;
				const float cz = (p_cover->origin_cell.y + z + 0.5f) * cs - p_parcel.position.z;
				const float w = MAX(1.0f - Math::sqrt(cx * cx + cz * cz) / r, 0.0f);
				if (pass == 0) {
					total += w;
				} else if (w > 0.0f) {
					powder_pending.write[z * n + x] += p_mass * w / (total * cs * cs);
				}
			}
		}
		if (total <= 0.0f) {
			return; // off the cover: gone
		}
	}
	powder_laid += p_mass;
	powder_reach = MAX(powder_reach, Vector2(p_parcel.position.x - release_center.x, p_parcel.position.z - release_center.z).length());
}

void SnowAvalanche3D::_powder_flush(SnowCover3D *p_cover) {
	const int n = p_cover->resolution;
	if (powder_pending.size() != n * n || p_cover->cell_swe.size() != n * n) {
		return;
	}
	bool any = false;
	for (int i = 0; i < n * n; i++) {
		const float add = powder_pending[i];
		if (add <= 0.0f) {
			continue;
		}
		const float swe_old = MAX(p_cover->cell_swe[i], 0.0f);
		const float depth = (swe_old > 0.01f ? swe_old / MAX(p_cover->cell_density[i], 30.0f) : 0.0f) + add / powder_deposit_density;
		p_cover->cell_swe.write[i] = swe_old + add;
		p_cover->cell_density.write[i] = CLAMP((swe_old + add) / MAX(depth, 1.0e-6f), 30.0f, 917.0f);
		if (p_cover->cell_albedo.size() == n * n) {
			p_cover->cell_albedo.write[i] = MAX(p_cover->cell_albedo[i], 0.85f); // fresh powder
		}
		powder_pending.write[i] = 0.0f;
		any = true;
	}
	if (any) {
		p_cover->update_count++;
		p_cover->_upload();
	}
}

void SnowAvalanche3D::_powder_step(SnowCover3D *p_cover, float p_dt) {
	if (p_dt <= 0.0f) {
		return;
	}
	// The front's intake every half second becomes parcels of cloud, and the strongest bins the emitters that draw it.
	powder_window += p_dt;
	if (powder_window >= 0.5 || (!running && !bins.is_empty())) {
		sources.clear();
		LocalVector<SnowPowderCloud3D::Source> all;
		for (const KeyValue<Vector2i, Bin> &kv : bins) {
			const Bin &b = kv.value;
			if (b.mass <= 0.0f) {
				continue;
			}
			Parcel pc;
			pc.position = b.position / b.mass;
			pc.velocity = b.momentum / b.mass;
			pc.mass = b.mass;
			parcels.push_back(pc);
			SnowPowderCloud3D::Source src;
			src.position = pc.position + Vector3(0.0f, 1.0f, 0.0f);
			src.velocity = pc.velocity;
			src.rate = b.mass / float(MAX(powder_window, 1.0e-3));
			src.radius = 5.0f; // overlapping the next 8 m bin
			all.push_back(src);
		}
		all.sort_custom<SourceByRate>();
		const int keep = cloud ? cloud->get_max_sources() : 24;
		for (uint32_t i = 0; i < all.size() && int(i) < keep; i++) {
			sources.push_back(all[i]);
		}
		bins.clear();
		powder_window = 0.0;
	}
	if (cloud) {
		cloud->feed(running ? sources : LocalVector<SnowPowderCloud3D::Source>());
	}
	if (!running) {
		sources.clear();
	}
	// Parcels run on with the front's speed, slowed by the air (8 s), spread (4 m/s) and settle (powder_settle).
	const float slow = Math::exp(-p_dt / 8.0f);
	const float settle = 1.0f - Math::exp(-p_dt / powder_settle);
	parcel_deposit += p_dt;
	const bool lay = parcel_deposit >= 0.2 || !running;
	for (uint32_t k = 0; k < parcels.size();) {
		Parcel &pc = parcels[k];
		pc.age += p_dt;
		pc.velocity *= slow;
		pc.position += pc.velocity * p_dt;
		pc.radius = MIN(4.0f + 4.0f * pc.age, 40.0f);
		const float out = pc.mass * settle;
		pc.mass -= out;
		pc.owed += out;
		const bool done = pc.mass < 0.5f;
		if (done) {
			pc.owed += pc.mass;
			pc.mass = 0.0f;
		}
		if (lay || done) {
			_powder_lay(p_cover, pc, pc.owed);
			pc.owed = 0.0f;
		}
		if (done) {
			parcels.remove_at_unordered(k);
			continue;
		}
		k++;
	}
	if (lay) {
		parcel_deposit = 0.0;
	}
	// The cloud's blast: free bodies inside it take 1/2 rho_c u^2 on their width times the cloud's height.
	if (!parcels.is_empty()) {
		if (SnowBodies3D *bodies = p_cover->get_bodies()) {
			const TypedArray<Node> list = bodies->get_tracked();
			for (int b = 0; b < list.size(); b++) {
				RigidBody3D *rb = Object::cast_to<RigidBody3D>(list[b]);
				if (!rb || !rb->is_inside_tree() || rb->is_freeze_enabled()) {
					continue;
				}
				const Vector3 at = rb->get_global_position();
				const float rho = get_powder_density_at(at);
				if (rho <= 0.0f) {
					continue;
				}
				Vector3 wind;
				float wsum = 0.0f;
				for (const Parcel &pc : parcels) {
					if (Vector2(at.x - pc.position.x, at.z - pc.position.z).length() <= pc.radius) {
						wind += pc.velocity * pc.mass;
						wsum += pc.mass;
					}
				}
				if (wsum <= 0.0f) {
					continue;
				}
				const Vector3 rel = wind / wsum - rb->get_linear_velocity();
				const float sp = rel.length();
				if (sp < 0.5f) {
					continue;
				}
				const float width = Math::sqrt(MAX(bodies->get_body_top_area(rb), 0.25f));
				rb->apply_central_force(rel / sp * 0.5f * rho * sp * sp * width * 2.0f);
			}
		}
	}
	powder_flush += p_dt;
	if ((powder_flush >= FLUSH_INTERVAL || parcels.is_empty()) && !p_cover->is_updating()) {
		powder_flush = 0.0;
		_powder_flush(p_cover);
	}
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
				SnowCover3D *cover = _cover();
				if (!cloud) {
					cloud = memnew(SnowPowderCloud3D);
					cloud->set_name("PowderCloud");
					add_child(cloud, false, INTERNAL_MODE_BACK);
					if (cover && !cover->get_height_source().is_empty()) {
						if (Node *src = cover->get_node_or_null(cover->get_height_source())) {
							cloud->set_terrain_path(cloud->get_path_to(src));
						}
					}
				}
				if (cover && !cover->is_connected(SNAME("cornice_failed"), callable_mp(this, &SnowAvalanche3D::_on_cornice_failed))) {
					cover->connect(SNAME("cornice_failed"), callable_mp(this, &SnowAvalanche3D::_on_cornice_failed));
				}
			}
		} break;
		case NOTIFICATION_INTERNAL_PHYSICS_PROCESS: {
			SnowCover3D *cover = _cover();
			if (!cover) {
				break;
			}
			if (running) {
				// The flow reads and writes the cover's cells: while a background update has them it waits, and
				// catches up after (the frame never waits for the update).
				pending_time += get_physics_process_delta_time();
				if (cover->is_updating()) {
					break;
				}
				step(MIN(pending_time, 0.25));
				pending_time = 0.0;
				if (running) {
					_hit_bodies(cover);
				}
				break;
			}
			pending_time = 0.0;
			if (!parcels.is_empty() || !bins.is_empty() || !sources.is_empty()) {
				if (!cover->is_updating()) {
					_powder_step(cover, float(get_physics_process_delta_time()));
				}
			}
			if (cover->update_count != seen_update && !cover->is_updating()) {
				seen_update = cover->update_count;
				if (natural_release) {
					_check_release(cover);
				}
			}
			if (triggers_enabled && !running && recent_res == cover->resolution) {
				_check_triggers(cover);
			}
		} break;
	}
}

void SnowAvalanche3D::_bind_methods() {
	ClassDB::bind_static_method("SnowAvalanche3D", D_METHOD("weak_layer_strength", "density", "age_hours"), &SnowAvalanche3D::weak_layer_strength);
	ClassDB::bind_static_method("SnowAvalanche3D", D_METHOD("stability_index", "slab", "slab_density", "weak_density", "age_hours", "slope"), &SnowAvalanche3D::stability_index);
	ClassDB::bind_static_method("SnowAvalanche3D", D_METHOD("added_shear", "line_load", "depth", "slope"), &SnowAvalanche3D::added_shear);
	ClassDB::bind_method(D_METHOD("apply_load", "at", "line_load", "sink"), &SnowAvalanche3D::apply_load, DEFVAL(0.0f));
	ClassDB::bind_method(D_METHOD("get_trigger_index_at", "at", "line_load", "sink"), &SnowAvalanche3D::get_trigger_index_at, DEFVAL(0.0f));
	ClassDB::bind_method(D_METHOD("get_trigger_count"), &SnowAvalanche3D::get_trigger_count);
	ClassDB::bind_method(D_METHOD("get_powder_taken"), &SnowAvalanche3D::get_powder_taken);
	ClassDB::bind_method(D_METHOD("get_powder_settled"), &SnowAvalanche3D::get_powder_settled);
	ClassDB::bind_method(D_METHOD("get_suspended_mass"), &SnowAvalanche3D::get_suspended_mass);
	ClassDB::bind_method(D_METHOD("get_powder_reach"), &SnowAvalanche3D::get_powder_reach);
	ClassDB::bind_method(D_METHOD("get_powder_density_at", "position"), &SnowAvalanche3D::get_powder_density_at);
	ClassDB::bind_method(D_METHOD("get_powder_cloud"), &SnowAvalanche3D::get_powder_cloud);
	ClassDB::bind_method(D_METHOD("set_powder_enabled", "enable"), &SnowAvalanche3D::set_powder_enabled);
	ClassDB::bind_method(D_METHOD("is_powder_enabled"), &SnowAvalanche3D::is_powder_enabled);
	ClassDB::bind_method(D_METHOD("set_powder_speed", "speed"), &SnowAvalanche3D::set_powder_speed);
	ClassDB::bind_method(D_METHOD("get_powder_speed"), &SnowAvalanche3D::get_powder_speed);
	ClassDB::bind_method(D_METHOD("set_powder_share", "share"), &SnowAvalanche3D::set_powder_share);
	ClassDB::bind_method(D_METHOD("get_powder_share"), &SnowAvalanche3D::get_powder_share);
	ClassDB::bind_method(D_METHOD("set_powder_settle", "seconds"), &SnowAvalanche3D::set_powder_settle);
	ClassDB::bind_method(D_METHOD("get_powder_settle"), &SnowAvalanche3D::get_powder_settle);
	ClassDB::bind_method(D_METHOD("get_collapse_count"), &SnowAvalanche3D::get_collapse_count);
	ClassDB::bind_method(D_METHOD("set_triggers_enabled", "enable"), &SnowAvalanche3D::set_triggers_enabled);
	ClassDB::bind_method(D_METHOD("is_triggers_enabled"), &SnowAvalanche3D::is_triggers_enabled);
	ClassDB::bind_method(D_METHOD("set_trigger_groups", "groups"), &SnowAvalanche3D::set_trigger_groups);
	ClassDB::bind_method(D_METHOD("get_trigger_groups"), &SnowAvalanche3D::get_trigger_groups);
	ClassDB::bind_method(D_METHOD("set_character_mass", "mass"), &SnowAvalanche3D::set_character_mass);
	ClassDB::bind_method(D_METHOD("get_character_mass"), &SnowAvalanche3D::get_character_mass);
	ClassDB::bind_method(D_METHOD("set_propagation_index", "index"), &SnowAvalanche3D::set_propagation_index);
	ClassDB::bind_method(D_METHOD("get_propagation_index"), &SnowAvalanche3D::get_propagation_index);
	ClassDB::bind_method(D_METHOD("set_collapse_range", "range"), &SnowAvalanche3D::set_collapse_range);
	ClassDB::bind_method(D_METHOD("get_collapse_range"), &SnowAvalanche3D::get_collapse_range);
	ClassDB::bind_method(D_METHOD("trigger", "at", "radius"), &SnowAvalanche3D::trigger);
	ClassDB::bind_method(D_METHOD("step", "delta"), &SnowAvalanche3D::step);
	ClassDB::bind_method(D_METHOD("is_running"), &SnowAvalanche3D::is_running);
	ClassDB::bind_method(D_METHOD("get_release_count"), &SnowAvalanche3D::get_release_count);
	ClassDB::bind_method(D_METHOD("get_released_volume"), &SnowAvalanche3D::get_released_volume);
	ClassDB::bind_method(D_METHOD("get_entrained_volume"), &SnowAvalanche3D::get_entrained_volume);
	ClassDB::bind_method(D_METHOD("get_deposited_volume"), &SnowAvalanche3D::get_deposited_volume);
	ClassDB::bind_method(D_METHOD("get_flowing_volume"), &SnowAvalanche3D::get_flowing_volume);
	ClassDB::bind_method(D_METHOD("get_max_speed"), &SnowAvalanche3D::get_max_speed);
	ClassDB::bind_method(D_METHOD("get_max_speed_terminal"), &SnowAvalanche3D::get_max_speed_terminal);
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
	ADD_GROUP("Powder cloud", "powder_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "powder_enabled"), "set_powder_enabled", "is_powder_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "powder_speed", PROPERTY_HINT_RANGE, "0,50,0.1,suffix:m/s"), "set_powder_speed", "get_powder_speed");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "powder_share", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_powder_share", "get_powder_share");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "powder_settle", PROPERTY_HINT_RANGE, "0.1,60,0.1,suffix:s"), "set_powder_settle", "get_powder_settle");
	ADD_GROUP("Triggers", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "triggers_enabled"), "set_triggers_enabled", "is_triggers_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_STRING_ARRAY, "trigger_groups"), "set_trigger_groups", "get_trigger_groups");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "character_mass", PROPERTY_HINT_RANGE, "0,500,0.5,suffix:kg"), "set_character_mass", "get_character_mass");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "propagation_index", PROPERTY_HINT_RANGE, "0,5,0.01"), "set_propagation_index", "get_propagation_index");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "collapse_range", PROPERTY_HINT_RANGE, "0,500,0.1,suffix:m"), "set_collapse_range", "get_collapse_range");

	ADD_SIGNAL(MethodInfo("avalanche_triggered", PropertyInfo(Variant::VECTOR3, "position"), PropertyInfo(Variant::OBJECT, "body", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT, "Node")));
	ADD_SIGNAL(MethodInfo("snow_collapsed", PropertyInfo(Variant::VECTOR3, "position"), PropertyInfo(Variant::FLOAT, "area")));
	ADD_SIGNAL(MethodInfo("avalanche_released", PropertyInfo(Variant::VECTOR3, "position"), PropertyInfo(Variant::FLOAT, "volume")));
	ADD_SIGNAL(MethodInfo("avalanche_stopped", PropertyInfo(Variant::VECTOR3, "position"), PropertyInfo(Variant::FLOAT, "volume"), PropertyInfo(Variant::FLOAT, "run_out")));
}

SnowAvalanche3D::SnowAvalanche3D() {
	trigger_groups.push_back("snow_deform");
	trigger_groups.push_back("grass_trample");
	trigger_groups.push_back("world_contact");
}

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
#include "snow_lake_ice_3d.h"

#include "core/config/engine.h"
#include "core/object/callable_mp.h"
#include "modules/ground/ground_deform_field.h"
#include "modules/terrain_deform/terrain_data_map_3d.h"
#include "modules/world/world_contracts.h"
#include "core/object/class_db.h"
#include "scene/3d/camera_3d.h"
#include "scene/resources/surface_tool.h"
#include "scene/3d/physics/rigid_body_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"

namespace {
const float GRAVITY = 9.81f;
const float FLOW_MIN = 1.0f; // kg/m2 (about 3 mm of debris): less is not flowing
const float STOP_SPEED = 0.5f; // m/s
const float BED_ALBEDO = 0.66f; // the old surface a slab leaves bare
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

// Smooth value noise in [0, 1] over world cells, about 3 cells across (integer hash, no sine: it holds far from the
// origin).
static float _patch_noise(int p_x, int p_z) {
	auto hash = [](int x, int z) {
		uint32_t h = uint32_t(x) * 1664525u + uint32_t(z) * 22695477u + 1013904223u;
		h ^= h >> 16;
		h *= 0x7feb352du;
		h ^= h >> 15;
		h *= 0x846ca68bu;
		h ^= h >> 16;
		return float(h >> 8) * (1.0f / 16777216.0f);
	};
	const float fx = float(p_x) / 3.0f, fz = float(p_z) / 3.0f;
	const int x0 = int(Math::floor(fx)), z0 = int(Math::floor(fz));
	float tx = fx - x0, tz = fz - z0;
	tx = tx * tx * (3.0f - 2.0f * tx);
	tz = tz * tz * (3.0f - 2.0f * tz);
	return Math::lerp(Math::lerp(hash(x0, z0), hash(x0 + 1, z0), tx), Math::lerp(hash(x0, z0 + 1), hash(x0 + 1, z0 + 1), tx), tz);
}

// The fall line (downhill, unit, xz) over the cells weighted by p_weight (cover cells), from the ground's mean
// gradient.
Vector2 SnowAvalanche3D::_fall_line(const SnowCover3D *p_cover, const Vector<float> &p_weight) {
	const int n = p_cover->resolution;
	const Vector<float> &g = p_cover->cell_ground;
	Vector2 d;
	for (int z = 1; z + 1 < n; z++) {
		for (int x = 1; x + 1 < n; x++) {
			const float w = p_weight[z * n + x];
			if (w > 0.0f) {
				d -= w * Vector2(g[z * n + x + 1] - g[z * n + x - 1], g[(z + 1) * n + x] - g[(z - 1) * n + x]);
			}
		}
	}
	return d.length_squared() > 1e-12f ? d.normalized() : Vector2(1, 0);
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
		recent_pack.resize(n * n);
		recent_pack.fill(0.0f);
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
		recent_pack.resize(n * n);
		recent_pack.fill(0.0f); // re-read below
		recent_origin = p_cover->origin_cell;
	}
	const float hours = p_cover->last_step_hours;
	const bool has_new = p_cover->cell_new_depth.size() == n * n;
	for (int i = 0; i < n * n; i++) {
		const float add = has_new ? MAX(p_cover->cell_new_depth[i], 0.0f) : 0.0f;
		float r = recent[i];
		float a = recent_age[i] + hours;
		const float rho = MAX(p_cover->cell_density[i], 30.0f);
		const float depth = p_cover->cell_swe[i] > 0.01f ? p_cover->cell_swe[i] / rho : 0.0f;
		// The new snow settles and melts with the pack (it is the part that settles fastest, so this is if
		// anything generous to it): scaled by how much the pack under the old reading shrank, not counting what
		// fell since. Without it a settled storm reads as the whole pack and a release scours to the ground.
		const float before = recent_pack[i];
		if (r > 0.0f && before > 1e-3f) {
			const float now = MAX(depth - add, 0.0f);
			if (now < before) {
				r *= now / before;
			}
		}
		if (add > 0.0f) {
			a = (a * r) / (r + add);
			r += add;
		}
		r = MIN(r, depth);
		recent_pack.write[i] = depth;
		if (a > RECENT_BONDED_HOURS) {
			r = 0.0f;
			a = 0.0f;
		}
		recent.write[i] = r;
		recent_age.write[i] = a;
	}
	_glide_advance(p_cover, hours);
	_bury_crowns(p_cover);
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
			best *= _hold(p_cover, i);
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
	// Breadth first: when the cap stops it, the release is a compact patch around the seed (depth first leaves
	// every other row behind in the stack).
	int head = 0;
	while (head < stack.size() && cells.size() < MAX_RELEASE_CELLS) {
		const int i = stack[head++];
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
	pressure.resize(n * n);
	pressure.fill(0.0f);
	detrained_volume = 0.0f;
	water_mass = 0.0f;
	water_total = 0.0f;
	water_waves = 0;
	wave_height = 0.0f;
	water_cells.clear();
	water_entered_now = false;
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
	Vector<float> released;
	released.resize(n * n);
	released.fill(0.0f);
	released_cells.resize(n * n);
	released_cells.fill(0);
	for (int k = 0; k < p_cells.size(); k++) {
		const int i = p_cells[k];
		const float rho = MAX(p_cover->cell_density[i], 30.0f);
		const float depth = p_cover->cell_swe[i] > 0.01f ? p_cover->cell_swe[i] / rho : 0.0f;
		const float s = MIN(p_slab[k], depth);
		if (s <= 0.0f) {
			continue;
		}
		released.write[i] = s;
		released_cells.write[i] = 1;
		// The slab's snow is already in the cover's cells: it starts moving where it lies.
		h.write[i] = s * rho;
		written.write[i] = s * rho;
		if (recent_res == n && recent_origin == flow_origin) {
			recent.write[i] = MAX(recent[i] - s, 0.0f);
			if (recent_pack.size() == n * n) {
				recent_pack.write[i] = MAX(recent_pack[i] - s, 0.0f); // gone with the slab, not settled
			}
		}
		released_volume += s * cell * cell;
		if (p_cover->cell_liquid.size() == p_cover->cell_swe.size()) {
			release_liquid += p_cover->cell_liquid[i] / MAX(p_cover->cell_swe[i], 1.0f) * s;
		}
		const Vector3 at((flow_origin.x + i % n + 0.5f) * cell, p_cover->cell_ground[i], (flow_origin.y + i / n + 0.5f) * cell);
		c += at * s;
		total += s;
	}
	// The bed the slab leaves is the old surface it slid on: the weak layer's faceted grains and the aged crust
	// under them, greyer than the new snow around (aged snow, albedo about 0.65-0.7; Gardner and Sharp 2010), and
	// streaked down the fall line where the slab's base scoured it.
	if (p_cover->cell_albedo.size() == n * n && total > 0.0f) {
		const Vector2 down = _fall_line(p_cover, released);
		for (int i = 0; i < n * n; i++) {
			if (released[i] < 0.05f) {
				continue;
			}
			const Vector2 wc(float(flow_origin.x + i % n), float(flow_origin.y + i / n));
			const float u = wc.dot(down), v = wc.dot(Vector2(-down.y, down.x));
			const float streak = _patch_noise(int(Math::floor(v * 3.0f)), int(Math::floor(u / 3.0f)));
			p_cover->cell_albedo.write[i] = MIN(p_cover->cell_albedo[i], BED_ALBEDO + 0.12f * (streak - 0.5f));
		}
	}
	release_liquid = total > 0.0f ? release_liquid / total : 0.0f; // the slab's liquid water share of its mass
	// A wet slab runs as a wet flow: denser, slower, with higher friction and cohesion (wet snow avalanches, Naaim
	// et al. 2013; Bartelt et al. 2015), by its liquid share of what the pack can hold.
	run_wet = wet_enabled ? CLAMP(release_liquid / MAX(p_cover->liquid_capacity, 0.005f), 0.0f, 1.0f) : 0.0f;
	mu_run = Math::lerp(mu, MAX(wet_mu, mu), run_wet);
	xi_run = Math::lerp(xi, wet_xi, run_wet);
	rho_run = Math::lerp(flow_density, wet_density, run_wet);
	cohesion_run = wet_cohesion * run_wet;
	if (total <= 0.0f) {
		return;
	}
	release_center = c / total;
	run_down = _fall_line(p_cover, released);
	if (crowns_enabled) {
		_build_crown(p_cover, released);
	}
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
		zs.write[i] = p_cover->cell_ground[ci] + stat + h[i] / rho_run;
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
			if (water_enabled && p_cover->cell_water.size() == p_cover->cell_swe.size() && p_cover->cell_water[ci] > -1e29f) {
				// Over a lake: ice that bears it carries the flow on, else (open water, or ice it breaks) the snow
				// plunges in and pushes a wave.
				float ice = p_cover->lake_ice_enabled && p_cover->pub.ice.size() == p_cover->cell_swe.size() ? p_cover->pub.ice[ci] : 0.0f;
				if (ice >= SnowCover3D::ICE_HOLD) {
					const float white = p_cover->pub.white_ice.size() == p_cover->cell_swe.size() ? p_cover->pub.white_ice[ci] / MAX(ice, 1e-4f) : 0.0f;
					const float air = WorldContracts::weather()->has_provider() ? WorldContracts::weather()->air_temperature_at(Vector3((flow_origin.x + x + 0.5f) * cell, 0.0f, (flow_origin.y + z + 0.5f) * cell)) : -5.0f;
					if (h[i] * cell * cell > SnowCover3D::ice_breaking_load(ice, white, air)) {
						SnowCover3D::IceBreak br;
						br.center = Vector2((flow_origin.x + x + 0.5f) * cell, (flow_origin.y + z + 0.5f) * cell);
						br.radius = 0.75f * cell;
						p_cover->ice_breaks.push_back(br);
						p_cover->_cut_ice(p_cover->pub.ice, p_cover->pub.white_ice);
						ice_broken++;
						ice = 0.0f;
					}
				}
				if (ice < SnowCover3D::ICE_HOLD) {
					const float u0 = hu[i] / h[i], v0 = hv[i] / h[i];
					float land_slope = 0.0f;
					int land = 0;
					const int nb[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
					for (const auto &o : nb) {
						const int xx = x + o[0], zz = z + o[1];
						const int cj = (xx < 0 || zz < 0 || xx >= n || zz >= n) ? -1 : _cover_index(p_cover, zz * n + xx);
						if (cj >= 0 && !(p_cover->cell_water[cj] > -1e29f)) {
							land_slope += _slope_at(p_cover, cj % p_cover->resolution, cj / p_cover->resolution);
							land++;
						}
					}
					_water_entry(p_cover, i, ci, Math::sqrt(u0 * u0 + v0 * v0), u0, v0, h[i] / rho_run, land > 0 ? land_slope / land : Math::deg_to_rad(30.0f));
					continue;
				}
			}
			const float z0 = zs[i];
			const float gx = (zat(x + 1, z, z0) - zat(x - 1, z, z0)) / (2.0f * cell);
			const float gz = (zat(x, z + 1, z0) - zat(x, z - 1, z0)) / (2.0f * cell);
			const float grad = Math::sqrt(gx * gx + gz * gz);
			const float cos_t = 1.0f / Math::sqrt(1.0f + grad * grad);
			const float depth = h[i] / rho_run;
			float u = hu[i] / h[i];
			float v = hv[i] / h[i];
			u -= GRAVITY * gx * cos_t * p_dt;
			v -= GRAVITY * gz * cos_t * p_dt;
			float speed = Math::sqrt(u * u + v * v);
			// A forest brakes it: the stems add friction and take snow out of the flow (detrainment, Feistl et al.
			// 2014: a rate K / u, K some tens to a hundred Pa by stand), which stays among the trees.
			const float forest = forests_enabled && p_cover->cell_canopy.size() == p_cover->cell_swe.size() ? CLAMP(p_cover->cell_canopy[ci], 0.0f, 1.0f) : 0.0f;
			// A wet flow's cohesion is a yield stress on its base (Bartelt et al. 2015: cohesion shortens the run-out and
			// narrows the flow); it stops thin margins first, which stand as levees beside the running core.
			const float coh = cohesion_run / (rho_run * MAX(depth, 0.05f));
			// Wet snow's friction varies with its water and grain from place to place: a front with patchy
			// friction breaks into lobes and fingers (fingering in granular fronts, Pouliquen et al. 1997; the
			// lobate snouts of wet snow avalanches, Bartelt et al. 2012). +-25% at full wetness, over about 3 cells.
			const float mu_here = mu_run * (1.0f + 0.25f * run_wet * (2.0f * _patch_noise(flow_origin.x + x, flow_origin.y + z) - 1.0f));
			const float drag = ((mu_here + forest_friction * forest) * GRAVITY * cos_t + coh + GRAVITY * speed * speed / (xi_run * MAX(depth, 0.01f))) * p_dt;
			if (forest > 0.01f) {
				const float det = MIN(h[i], forest_detrainment * forest / MAX(speed, 1.0f) * p_dt);
				if (det > 0.0f) {
					written.write[i] = written[i] - det;
					deposit.write[i] = deposit[i] + det;
					deposited_volume += det / rho_run * cell * cell;
					detrained_volume += det / rho_run * cell * cell;
					h.write[i] = h[i] - det;
					hu.write[i] = hu[i] * h[i] / (h[i] + det);
					hv.write[i] = hv[i] * h[i] / (h[i] + det);
				}
				if (h[i] < FLOW_MIN) {
					continue;
				}
			}
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
			if (speed < STOP_SPEED && GRAVITY * grad * cos_t <= mu_here * GRAVITY * cos_t + coh) {
				// Deposited: the flow's snow stays in the cover as static snow, its surface broken into blocks near
				// the camera (avalanche debris: granules, Bartelt and McArdell 2009).
				deposited_volume += depth * cell * cell;
				// The cover holds what was last written here; it may be less than the flow brought since.
				written.write[i] = written[i] - h[i];
				// It comes to rest over its neighbours too, half here and an eighth on each side (as the end of the
				// run smooths the deposit): the grid's 2 m is finer than a debris lobe, and a cell dropping its whole
				// load alone stands as a column among snow still creeping past. The neighbours' share is written
				// into the cover by the next flush (it reads as written below what is there).
				{
					float kept = h[i];
					const int nb4[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
					for (const auto &o : nb4) {
						const int xx = x + o[0], zz = z + o[1];
						if (xx < 0 || zz < 0 || xx >= n || zz >= n || _cover_index(p_cover, zz * n + xx) < 0) {
							continue;
						}
						const int j = zz * n + xx;
						const float share = 0.125f * h[i];
						deposit.write[j] = deposit[j] + share;
						written.write[j] = written[j] - share; // the flush adds it to the cover
						kept -= share;
					}
					// What leaves this cell is in its cover cell already: take it out there.
					const float moved = h[i] - kept;
					if (moved > 0.0f) {
						written.write[i] = written[i] + moved;
					}
					deposit.write[i] = deposit[i] + kept;
				}
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
				// The head: the leading slope of the flow, or the last fast cell before it. The head's very edge
				// ploughs the snow in front of it and slows, so its eruption current comes from the fast snow just
				// behind (the head is the fastest part of a dry avalanche; Sovilla et al. 2015).
				bool front = fx < 0 || fz < 0 || fx >= n || fz >= n || h[fz * n + fx] < h[i];
				if (!front) {
					const int f = fz * n + fx;
					front = h[f] < FLOW_MIN || (hu[f] * hu[f] + hv[f] * hv[f]) < powder_speed * powder_speed * h[f] * h[f];
				}
				if (front && release_liquid < 0.001f) { // dry snow holds no liquid water
					const float take = MIN(0.5f * h[i], powder_share * h[i] * (speed - powder_speed) * 0.1f * p_dt);
					const float m = take * cell * cell;
					const Vector3 at((flow_origin.x + x + 0.5f) * cell, p_cover->cell_ground[ci] + MAX(p_cover->cell_swe[ci] - written[i], 0.0f) / MAX(p_cover->cell_density[ci], 30.0f) + depth, (flow_origin.y + z + 0.5f) * cell);
					// By 8 m lanes across the run: each lane's intake rises from its leading edge, so the cloud
					// builds along the whole front as one mass (the slab's sides spill sideways and feed their
					// lanes, they do not stand as columns of their own).
					const Vector2 across(-run_down.y, run_down.x);
					Bin &b = bins[Vector2i(int(Math::floor(Vector2(at.x, at.z).dot(across) / 8.0f)), 0)];
					b.mass += m;
					b.momentum += Vector3(u, 0.0f, v) * m;
					b.position += at * m;
					const float along = Vector2(at.x, at.z).dot(run_down);
					if (along > b.lead_along) {
						b.lead_along = along;
						b.lead = at;
					}
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
					if (recent_pack.size() == recent.size()) {
						recent_pack.write[ci] = MAX(recent_pack[ci] - take, 0.0f);
					}
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
			pressure.write[i] = MAX(pressure[i], 0.5f * rho_run * speed * speed);
		}
	}
	if (vmax > max_speed) {
		max_speed = vmax;
		// Voellmy's steady speed at that depth and slope: sqrt(xi h (sin - mu cos)).
		const float sin_t = Math::sqrt(MAX(1.0f - vmax_cos * vmax_cos, 0.0f));
		max_speed_terminal = Math::sqrt(MAX(xi_run * vmax_depth * (sin_t - mu_run * vmax_cos), 0.0f));
	}
	// One wave for an entry: it ends when no snow has gone in for three seconds.
	water_quiet = water_entered_now ? 0.0f : water_quiet + p_dt;
	if (water_mass > 0.0f && water_quiet > 3.0f) {
		_emit_wave(p_cover);
	}
	water_entered_now = false;
	if (p_cover->lake_ice) {
		if (ice_broken != ice_broken_drawn) {
			ice_broken_drawn = ice_broken;
			p_cover->lake_ice->rebuild();
		}
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
				const float cl = Math::sqrt(GRAVITY * HL / rho_run), cr = Math::sqrt(GRAVITY * HR / rho_run);
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
	// A share of the deposit's top as granules over the cell: blocks, and snowballs when wet.
	clumps->drop(p_at, 0.7f * cell, p_volume * debris_share * (1.0f + 1.5f * run_wet), 0.3f + 0.7f * run_wet, 2.0f + 3.0f * run_wet);
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
			const float depth = (swe_old > 0.01f ? swe_old / rho_old : 0.0f) + d / rho_run;
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

void SnowAvalanche3D::_slump(SnowCover3D *p_cover, int p_passes) {
	// Debris that has stopped while the rest still runs slumps to its angle of repose (talus relaxation, as at the
	// end of the run): a cell that stops on a slow margin drops its whole load as a column, and no column of debris
	// stands above its neighbours, moving or not. Only stopped debris gives, even with moving snow over it; it may
	// land under moving snow.
	const int n = res;
	if (deposit.size() != n * n) {
		return;
	}
	const float repose = Math::tan(Math::deg_to_rad(Math::lerp(32.0f, 40.0f, run_wet))) * cell;
	Vector<float> top;
	top.resize(n * n);
	for (int i = 0; i < n * n; i++) {
		const int ci = _cover_index(p_cover, i);
		top.write[i] = ci < 0 ? Math::NaN : p_cover->cell_ground[ci] + MAX(p_cover->cell_swe[ci] - deposit[i] - written[i], 0.0f) / MAX(p_cover->cell_density[ci], 30.0f) + (deposit[i] + MAX(written[i], 0.0f)) / rho_run;
	}
	Vector<float> delta;
	delta.resize(n * n);
	delta.fill(0.0f);
	bool any = false;
	for (int pass = 0; pass < p_passes; pass++) {
		bool moved = false;
		for (int z = 0; z < n; z++) {
			for (int x = 0; x < n; x++) {
				const int i = z * n + x;
				const float have = deposit[i] + delta[i];
				if (have <= 0.0f || Math::is_nan(top[i])) {
					continue;
				}
				const int nb[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
				for (const auto &o : nb) {
					const int xx = x + o[0], zz = z + o[1];
					if (xx < 0 || zz < 0 || xx >= n || zz >= n) {
						continue;
					}
					const int j = zz * n + xx;
					if (Math::is_nan(top[j])) {
						continue;
					}
					const float step = top[i] - top[j] - repose;
					if (step > 0.01f) {
						const float give = MIN(0.25f * step * rho_run, 0.25f * (deposit[i] + delta[i]));
						delta.write[i] -= give;
						delta.write[j] += give;
						top.write[i] -= give / rho_run;
						top.write[j] += give / rho_run;
						moved = true;
					}
				}
			}
		}
		any = any || moved;
		if (!moved) {
			break;
		}
	}
	if (!any) {
		return;
	}
	for (int i = 0; i < n * n; i++) {
		const float d = delta[i];
		if (Math::abs(d) < 1e-4f) {
			continue;
		}
		const int ci = _cover_index(p_cover, i);
		if (ci < 0) {
			continue;
		}
		deposit.write[i] = MAX(deposit[i] + d, 0.0f);
		const float swe_old = MAX(p_cover->cell_swe[ci], 0.0f);
		const float swe_new = MAX(swe_old + d, 0.0f);
		if (d > 0.0f) {
			const float depth = (swe_old > 0.01f ? swe_old / MAX(p_cover->cell_density[ci], 30.0f) : 0.0f) + d / rho_run;
			p_cover->cell_density.write[ci] = CLAMP(swe_new / MAX(depth, 1.0e-6f), 30.0f, 917.0f);
		}
		p_cover->cell_swe.write[ci] = swe_new;
	}
	p_cover->update_count++;
	p_cover->_upload();
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
		const float area = width * MIN(h[i] / rho_run, 2.0f);
		rb->apply_central_force(vel / rel * 0.5f * rho_run * rel * rel * area);
	}
}

void SnowAvalanche3D::_finish(SnowCover3D *p_cover) {
	// What still moves stops where it is.
	for (int i = 0; i < res * res; i++) {
		if (h[i] > 0.0f) {
			deposited_volume += h[i] / rho_run * cell * cell;
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
	// The debris settles to its angle of repose: steeper steps between cells slump, half the excess each pass
	// (talus relaxation, the thermal erosion of Musgrave et al. 1989). Avalanche debris stands at about 30-35
	// degrees dry and steeper wet (cohesive), so a cell left standing alone as a spike slumps into a heap.
	{
		const float repose = Math::tan(Math::deg_to_rad(Math::lerp(32.0f, 40.0f, run_wet))) * cell;
		Vector<float> base;
		base.resize(n * n);
		for (int i = 0; i < n * n; i++) {
			const int ci = _cover_index(p_cover, i);
			base.write[i] = ci < 0 ? Math::NaN : p_cover->cell_ground[ci] + MAX(p_cover->cell_swe[ci] - deposit[i], 0.0f) / MAX(p_cover->cell_density[ci], 30.0f);
		}
		for (int pass = 0; pass < 16; pass++) {
			bool moved = false;
			Vector<float> d2 = d;
			for (int z = 0; z < n; z++) {
				for (int x = 0; x < n; x++) {
					const int i = z * n + x;
					if (d[i] <= 0.0f || Math::is_nan(base[i])) {
						continue;
					}
					const float hi = base[i] + d[i] / rho_run;
					const int nb[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
					for (const auto &o : nb) {
						const int xx = x + o[0], zz = z + o[1];
						if (xx < 0 || zz < 0 || xx >= n || zz >= n) {
							continue;
						}
						const int j = zz * n + xx;
						if (Math::is_nan(base[j])) {
							continue;
						}
						const float step = hi - (base[j] + d[j] / rho_run) - repose;
						if (step > 0.01f) {
							const float give = MIN(0.25f * step * rho_run, 0.25f * d[i]);
							d2.write[i] -= give;
							d2.write[j] += give;
							moved = true;
						}
					}
				}
			}
			d = d2;
			if (!moved) {
				break;
			}
		}
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
			const float depth = (swe_old > 0.01f ? swe_old / rho_old : 0.0f) + delta / rho_run;
			p_cover->cell_density.write[ci] = CLAMP(swe_new / MAX(depth, 1.0e-6f), 30.0f, 917.0f);
		}
		p_cover->cell_swe.write[ci] = swe_new;
		any = true;
	}
	if (any) {
		p_cover->update_count++;
		p_cover->_upload();
	}
	// Granules on the settled debris, once per cell.
	for (int i = 0; i < n * n; i++) {
		// Granules over the whole deposit (debris is rubble throughout, the slow wet snout too; not the thin
		// remnants in the release zone), densest at its margins and snout, where the blocks roll out and pile
		// (Jomelli and Bertran 2001; wet debris: Bartelt and McArdell 2009). A margin is a cell whose neighbours
		// hold much less debris; the snout is the far end of the run.
		const float dd = d[i] / rho_run;
		if (dd > 0.1f && peak[i] > 0.5f && !(released_cells.size() == n * n && released_cells[i])) {
			const int x = i % n, z = i / n;
			float thinnest = dd;
			const int nb[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
			for (const auto &o : nb) {
				const int xx = x + o[0], zz = z + o[1];
				thinnest = MIN(thinnest, (xx < 0 || zz < 0 || xx >= n || zz >= n) ? 0.0f : d[zz * n + xx] / rho_run);
			}
			const float margin = CLAMP(1.0f - thinnest / dd, 0.0f, 1.0f);
			const Vector2 at((flow_origin.x + x + 0.5f) * cell, (flow_origin.y + z + 0.5f) * cell);
			const float along = run_out > 1.0f ? CLAMP(at.distance_to(Vector2(release_center.x, release_center.z)) / run_out, 0.0f, 1.0f) : 0.0f;
			const float weight = 0.35f + 0.65f * MAX(margin, along * along * along);
			_debris(Vector3(at.x, 0.0f, at.y), dd * cell * cell * weight);
		}
	}
	written.fill(0.0f);
	h.fill(0.0f);
	hu.fill(0.0f);
	hv.fill(0.0f);
	if (water_mass > 0.0f) {
		_emit_wave(p_cover);
	}
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
				wave = MAX(wave, sp + Math::sqrt(GRAVITY * h[i] / rho_run));
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
		_slump(cover, 4);
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
	// A slab's outline, not the load's circle: a crown across the slope above, flanks down the fall line on either
	// side (drawing in a little toward the foot) and the stauchwall below (McClung and Schaerer 2006). The crown
	// and flanks run irregular lines: they wander by up to 12% of the radius over about 12 m (the slab's strength
	// and thickness vary along them).
	Vector<float> near;
	near.resize(n * n);
	near.fill(0.0f);
	for (int z = 0; z < n; z++) {
		for (int x = 0; x < n; x++) {
			const Vector2 at((cover->origin_cell.x + x + 0.5f) * cs, (cover->origin_cell.y + z + 0.5f) * cs);
			near.write[z * n + x] = at.distance_to(Vector2(p_at.x, p_at.z)) <= p_radius ? 1.0f : 0.0f;
		}
	}
	const Vector2 down = _fall_line(cover, near);
	const Vector2 across(-down.y, down.x);
	for (int z = 0; z < n; z++) {
		for (int x = 0; x < n; x++) {
			const Vector2 at((cover->origin_cell.x + x + 0.5f) * cs, (cover->origin_cell.y + z + 0.5f) * cs);
			const Vector2 rel = at - Vector2(p_at.x, p_at.z);
			const float u = rel.dot(down) / p_radius; // downslope
			const float v = rel.dot(across) / p_radius;
			const float wobble = 0.16f * (_patch_noise((cover->origin_cell.x + x) / 2, (cover->origin_cell.y + z) / 2) - 0.5f);
			// The crown zigzags across the slope in shallow steps: a run every third of a radius, each set up or
			// down the slope by up to 0.12 radii and slanting one way or the other (the fracture jumps between
			// weak spots). _patch_noise at whole multiples of 3 is its raw hash.
			const float vs = v * 3.0f + 17.0f;
			const int seg = int(Math::floor(vs));
			const float step = 0.24f * (_patch_noise(seg * 3, 7919) - 0.5f);
			const float slant = (_patch_noise(seg * 3, 104729) < 0.5f ? -0.1f : 0.1f) * (vs - seg - 0.5f);
			const float crown = -0.65f * (1.0f - 0.12f * v * v) + step + slant + wobble; // bowed a little downslope at its ends
			// The flanks wander by up to 0.2 radii over about a third of a radius, each its own way.
			const float side_wander = 0.4f * (_patch_noise(int(Math::floor(u * 8.0f)) + 50, v < 0.0f ? 311 : 733) - 0.5f);
			const float flank = 1.0f - 0.12f * MAX(u, 0.0f) + side_wander + wobble;
			if (u < crown || u > 0.8f + wobble || Math::abs(v) > flank) {
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
	const float anchor = _hold(p_cover, i);
	auto test = [&](float p_slab, float p_strength, bool p_is_crust) {
		const float d = MAX(p_slab - p_sink, 0.02f) * Math::cos(slope);
		const float tau = rho * GRAVITY * p_slab * sc + added_shear(p_line_load, d, slope);
		const float sk = tau > 1.0e-3f ? p_strength * anchor / tau : 1.0e6f;
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
	// Breadth first: when the cap stops it, the release is a compact patch around the seed (depth first leaves
	// every other row behind in the stack).
	int head = 0;
	while (head < stack.size() && visited < MAX_RELEASE_CELLS) {
		const int i = stack[head++];
		visited++;
		const int x = i % n, z = i / n;
		const float slope = _slope_at(p_cover, x, z);
		const float rho = MAX(p_cover->pub.density[i], 30.0f);
		const bool steep = slope >= lo && slope <= hi;
		if (crust) {
			collapsed.write[i] = 1;
			const float b = p_cover->pub.crust_burial[i];
			if (steep && (i == seed || stability_index(b, rho, BURIED_WEAK_DENSITY, 0.0f, slope) * _hold(p_cover, i) < propagation_index)) {
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
				go = sj >= lo && sj <= hi && recent[j] >= 0.5f * min_slab && stability_index(recent[j], MAX(p_cover->pub.density[j], 30.0f), WEAK_DENSITY, recent_age[j], sj) * _hold(p_cover, j) < spread;
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
		powder_windows++;
		// A powder avalanche is a gravity current: its head stands about twice the body's height (Simpson 1997) and
		// sheds Kelvin-Helmholtz billows of mixed sizes off its top, while the body thins back up the track toward
		// the release (Turnbull and McElwaine 2007, the Vallee de la Sionne radar profiles). So each lane lifts by how
		// strong it is next to the strongest, with a fresh random lift each half second for billows of varied height,
		// and sheds its air at half the front's speed while riding on with the front, so the cloud trails back up the
		// track as the head's wake. No lane draws more than twice the median, so no lane stands as a column of its own.
		LocalVector<float> rates;
		for (const KeyValue<Vector2i, Bin> &kv : bins) {
			if (kv.value.mass > 0.0f) {
				rates.push_back(kv.value.mass);
			}
		}
		float cap = 1e30f;
		float strongest = 0.0f;
		if (!rates.is_empty()) {
			rates.sort();
			cap = 2.0f * rates[rates.size() / 2];
			strongest = MIN(rates[rates.size() - 1], cap);
		}
		const float lift = cloud ? cloud->get_updraft() : 0.3f;
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
			const float mass = MIN(b.mass, cap);
			const float rate = mass / float(MAX(powder_window, 1.0e-3));
			const float share = strongest > 0.0f ? mass / strongest : 0.0f;
			const float billow = _patch_noise(kv.key.x * 7 + 13, int(powder_windows)); // 0..1, new every window
			const float speed = Vector2(pc.velocity.x, pc.velocity.z).length();
			SnowPowderCloud3D::Source head;
			head.position = b.lead + Vector3(0.0f, 1.0f + 3.0f * billow * share, 0.0f);
			head.velocity = Vector3(pc.velocity.x, 0.0f, pc.velocity.z) * 0.5f; // the air it sheds falls behind: the wake
			head.velocity.y = speed * lift * (0.6f + 2.4f * billow) * share; // on top of the cloud's own updraft
			head.rate = rate;
			head.radius = 3.0f + 3.0f * share;
			all.push_back(head);
		}
		all.sort_custom<SourceByRate>();
		const int keep = cloud ? cloud->get_max_sources() : 24;
		for (uint32_t i = 0; i < all.size() && int(i) < keep; i++) {
			sources.push_back(all[i]);
		}
		bins.clear();
		powder_window = 0.0;
		source_lift.resize(sources.size());
		for (uint32_t i = 0; i < sources.size(); i++) {
			const Vector3 &q = sources[i].position;
			source_lift[i] = q.y - p_cover->get_ground_height_at(q) - p_cover->get_surface_depth_at(q);
		}
	} else {
		// Between windows the emitters ride on with the air they feed, hugging the ground, so the cloud they draw
		// is one streak rather than a puff left every half second.
		for (uint32_t i = 0; i < sources.size(); i++) {
			SnowPowderCloud3D::Source &q = sources[i];
			q.position += Vector3(q.velocity.x, 0.0f, q.velocity.z) * (2.0f * p_dt); // the front's speed
			q.position.y = p_cover->get_ground_height_at(q.position) + p_cover->get_surface_depth_at(q.position) + source_lift[i];
		}
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

float SnowAvalanche3D::_anchoring(const SnowCover3D *p_cover, int p_ci) const {
	// Forest anchors the snow (Bebi et al. 2009; Teich et al. 2012): stems and crowns break the slab up and hold it,
	// and stands with crown cover over about half rarely release. The weak layer's strength counts five times under a
	// canopy of 0.5 and grows without bound toward closure 0.55; nothing under 0.2 helps.
	if (!forests_enabled || p_cover->cell_canopy.size() != p_cover->cell_swe.size() || p_ci < 0) {
		return 1.0f;
	}
	const float a = CLAMP((p_cover->cell_canopy[p_ci] - 0.2f) / 0.3f, 0.0f, 1.0f);
	return a >= 1.0f ? 1.0e6f : 1.0f + 4.0f * a / (1.0f - 0.9f * a);
}

float SnowAvalanche3D::_wet_factor(const SnowCover3D *p_cover, int p_ci) const {
	// Meltwater and rain weaken the pack (wet-snow instability): the weak layer's shear strength falls steeply with
	// its liquid water (Yamanoi and Endo 2002), and wet slabs release as the water nears what the layers hold and
	// ponds on them (Mitterer et al. 2011: the LWC index, the mean content over 3% by volume, the share it holds,
	// active from about 0.33). Here the index is the pack's liquid over what it holds (liquid_capacity), and the
	// strength is wet_weakening^index: 0.63 at a third, a quarter when it holds all it can, for the default 0.25.
	const int nn = p_cover->cell_swe.size();
	if (!wet_enabled || p_ci < 0 || p_cover->cell_liquid.size() != nn || p_cover->cell_swe[p_ci] <= 1.0f) {
		return 1.0f;
	}
	const float index = CLAMP(p_cover->cell_liquid[p_ci] / p_cover->cell_swe[p_ci] / MAX(p_cover->liquid_capacity, 0.005f), 0.0f, 1.0f);
	return Math::pow(wet_weakening, index);
}

void SnowAvalanche3D::_glide_advance(SnowCover3D *p_cover, float p_hours) {
	// Glide: on smooth ground (grass, slabs of rock) a wet base lets the whole pack slide slowly downhill; a glide
	// crack opens at the top, and the pack releases full depth when the glide speeds up (glide avalanches; In der
	// Gand and Zupancic 1966; Jones 2004; Ancey and Bain 2015). Water at the base is the condition: the pack drains
	// (runoff) with its liquid near what it holds. The rate follows the pack's depth and the slope over 15 degrees
	// (none under, McClung and Clarke 1987). Closed forest stops it (stems pin the pack). The pack refreezing or
	// drying stops it, and the crack stays.
	const int n = p_cover->resolution;
	const int nn = n * n;
	if (!glide_enabled || p_hours <= 0.0f || p_cover->cell_swe.size() != nn || p_cover->cell_liquid.size() != nn || p_cover->cell_runoff.size() != nn) {
		return;
	}
	if (glide.size() != nn || glide_origin != p_cover->origin_cell) {
		glide_origin = p_cover->origin_cell; // the map moved: the glide history starts over
		glide.resize(nn);
		glide.fill(0.0f);
		glide_open.resize(nn);
		glide_open.fill(0);
	}
	const float lo = Math::deg_to_rad(15.0f), ref = Math::deg_to_rad(35.0f);
	const float cap = MAX(p_cover->liquid_capacity, 0.005f);
	const bool canopy = p_cover->cell_canopy.size() == nn;
	for (int z = 0; z < n; z++) {
		for (int x = 0; x < n; x++) {
			const int i = z * n + x;
			const float swe = p_cover->cell_swe[i];
			if (swe <= 10.0f) {
				glide.write[i] = 0.0f;
				glide_open.write[i] = 0;
				continue;
			}
			const float wet = CLAMP((p_cover->cell_liquid[i] / swe / cap - 0.5f) * 2.0f, 0.0f, 1.0f);
			if (wet <= 0.0f || p_cover->cell_runoff[i] <= 0.0f || (canopy && p_cover->cell_canopy[i] > 0.5f)) {
				continue;
			}
			const float slope = _slope_at(p_cover, x, z);
			if (slope <= lo) {
				continue;
			}
			const float depth = swe / MAX(p_cover->cell_density[i], 30.0f);
			const float steep = (Math::sin(slope) - Math::sin(lo)) / (Math::sin(ref) - Math::sin(lo));
			glide.write[i] = glide[i] + glide_rate / 24.0f * p_hours * wet * steep * MIN(depth, 2.0f);
			if (!glide_open[i] && glide[i] >= 0.05f) {
				glide_open.write[i] = 1;
				glide_cracks++;
				emit_signal(SNAME("glide_crack_opened"), Vector3((p_cover->origin_cell.x + x + 0.5f) * p_cover->_cell_size(), p_cover->cell_ground[i] + depth, (p_cover->origin_cell.y + z + 0.5f) * p_cover->_cell_size()));
			}
		}
	}
}

bool SnowAvalanche3D::_check_glide(SnowCover3D *p_cover) {
	// The cell that glided furthest past glide_release takes the pack around it with it: the neighbours whose
	// glide is past half of it, full depth.
	const int n = p_cover->resolution;
	const int nn = n * n;
	if (!glide_enabled || glide.size() != nn || p_cover->cell_swe.size() != nn) {
		return false;
	}
	int seed = -1;
	float most = glide_release;
	for (int i = 0; i < nn; i++) {
		if (glide[i] >= most) {
			most = glide[i];
			seed = i;
		}
	}
	if (seed < 0) {
		return false;
	}
	Vector<int> cells;
	Vector<float> amounts;
	Vector<uint8_t> seen;
	seen.resize(nn);
	seen.fill(0);
	Vector<int> stack;
	stack.push_back(seed);
	seen.write[seed] = 1;
	// Breadth first: when the cap stops it, the release is a compact patch around the seed (depth first leaves
	// every other row behind in the stack).
	int head = 0;
	while (head < stack.size() && cells.size() < MAX_RELEASE_CELLS) {
		const int i = stack[head++];
		cells.push_back(i);
		amounts.push_back(p_cover->cell_swe[i] / MAX(p_cover->cell_density[i], 30.0f));
		glide.write[i] = 0.0f;
		glide_open.write[i] = 0;
		const int x = i % n, z = i / n;
		const int nb[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
		for (const auto &o : nb) {
			const int xx = x + o[0], zz = z + o[1];
			if (xx < 0 || zz < 0 || xx >= n || zz >= n) {
				continue;
			}
			const int j = zz * n + xx;
			if (!seen[j] && glide[j] >= 0.5f * glide_release) {
				seen.write[j] = 1;
				stack.push_back(j);
			}
		}
	}
	glide_count++;
	_release(p_cover, cells, amounts);
	return true;
}

void SnowAvalanche3D::_water_entry(SnowCover3D *p_cover, int p_i, int p_ci, float p_speed, float p_u, float p_v, float p_depth, float p_slope) {
	// The flow's snow in this cell goes into the water: it stays in the cover's cell, which melts snow on open water
	// into it (follow-up 15), and its mass, speed and thickness add to this entry's wave.
	const float m = h[p_i] * cell * cell;
	const Vector3 at((flow_origin.x + p_i % res + 0.5f) * cell, p_cover->cell_water[p_ci], (flow_origin.y + p_i / res + 0.5f) * cell);
	// The still water depth of the impact zone: the deepest within four cells ahead along the flow (the slide
	// plunges on from the shoreline, where the depth is nothing).
	float d = 0.0f;
	const TerrainDataMap3D *tm = p_cover->_terrain_map();
	if (tm) {
		const Vector3 dir = p_speed > 1.0e-3f ? Vector3(p_u, 0.0f, p_v) / p_speed : Vector3();
		for (int k = 0; k <= 4; k++) {
			d = MAX(d, tm->get_water_depth_at(at + dir * (float(k) * cell)));
		}
	}
	d = MAX(d, 0.5f);
	water_mass += m;
	water_total += m;
	water_momentum += Vector3(p_u, 0.0f, p_v) * m;
	water_position += at * m;
	water_thick += p_depth * m;
	water_depth += d * m;
	water_slope += p_slope * m;
	if (water_cells.size() != res * res) {
		water_cells.resize(res * res);
		water_cells.fill(0);
		water_cell_count = 0;
	}
	if (!water_cells[p_i]) {
		water_cells.write[p_i] = 1;
		water_cell_count++;
	}
	water_entered_now = true;
	written.write[p_i] = written[p_i] - h[p_i];
	deposit.write[p_i] = deposit[p_i] + h[p_i];
	deposited_volume += h[p_i] / rho_run * cell * cell;
	h.write[p_i] = 0.0f;
	hu.write[p_i] = 0.0f;
	hv.write[p_i] = 0.0f;
}

float SnowAvalanche3D::impulse_product(float p_speed, float p_thickness, float p_mass, float p_width, float p_water_depth, float p_slope) {
	// Heller and Hager (2010): the impulse product parameter P = F S^1/2 M^1/4 cos(6/7 alpha)^1/2, with the slide
	// Froude number F = V / sqrt(g h), relative thickness S = s / h, relative mass M = m / (rho_w b h^2), and the
	// impact (hill slope) angle alpha. It alone predicts the wave from a granular slide into a basin.
	const float hw = MAX(p_water_depth, 0.1f);
	const float F = p_speed / Math::sqrt(GRAVITY * hw);
	const float S = MAX(p_thickness, 0.0f) / hw;
	const float M = MAX(p_mass, 0.0f) / (1000.0f * MAX(p_width, 0.1f) * hw * hw);
	return F * Math::sqrt(S) * Math::pow(M, 0.25f) * Math::sqrt(MAX(Math::cos(6.0f / 7.0f * CLAMP(p_slope, 0.0f, Math::PI * 0.5f)), 0.0f));
}

float SnowAvalanche3D::impulse_wave_height(float p_product, float p_water_depth, float p_distance, float p_angle) {
	// The wave's height (crest to trough), m, at p_distance from the impact and p_angle off the slide's direction:
	// in the near field the 2D maximum H_M / h = 5/9 P^4/5 (Heller and Hager 2010), spreading in a basin as
	// H / h = 3/2 P^4/5 cos^2(2 gamma / 3) (r / h)^-2/3 (Heller et al. 2009, from Huber and Hager 1997), never
	// more than the near-field maximum nor breaking (H under 0.8 h).
	const float hw = MAX(p_water_depth, 0.1f);
	const float p45 = Math::pow(MAX(p_product, 0.0f), 0.8f);
	const float near = 5.0f / 9.0f * p45;
	const float c = Math::cos(2.0f / 3.0f * CLAMP(Math::abs(p_angle), 0.0f, Math::PI * 0.5f));
	const float far = 1.5f * p45 * c * c * Math::pow(MAX(p_distance / hw, 1.0e-3f), -2.0f / 3.0f);
	return MIN(MIN(near, far), 0.8f) * hw;
}

float SnowAvalanche3D::impulse_wave_period(float p_mass_ratio, float p_water_depth, float p_distance) {
	// Heller and Spinneken (2015), the leading wave in a basin: T (g / h)^1/2 = 5.5 M^0.05 (r / h)^0.36.
	const float hw = MAX(p_water_depth, 0.1f);
	return 5.5f * Math::pow(MAX(p_mass_ratio, 1.0e-4f), 0.05f) * Math::pow(MAX(p_distance / hw, 1.0f), 0.36f) * Math::sqrt(hw / GRAVITY);
}

void SnowAvalanche3D::_emit_wave(SnowCover3D *p_cover) {
	const float m = water_mass;
	const float V = water_momentum.length() / m;
	const float s = water_thick / m;
	const float hw = water_depth / m;
	const float alpha = water_slope / m;
	// The slide's width: the span of the cells it entered the water by, across its heading (counting the cells
	// over-counted the width by the length it ran on over the water).
	float b = cell;
	{
		const Vector3 mv = water_momentum;
		const Vector2 across = Vector2(mv.x, mv.z).length_squared() > 1e-6f ? Vector2(-mv.z, mv.x).normalized() : Vector2(0, 1);
		float lo = 1e30f, hi = -1e30f;
		for (int k = 0; k < water_cells.size(); k++) {
			if (water_cells[k]) {
				const float v = Vector2(k % res + 0.5f, k / res + 0.5f).dot(across) * cell;
				lo = MIN(lo, v);
				hi = MAX(hi, v);
			}
		}
		if (hi >= lo) {
			b = hi - lo + cell;
		}
	}
	const float product = impulse_product(V, s, m, b, hw, alpha);
	const float height = impulse_wave_height(product, hw, 0.0f, 0.0f);
	const Vector3 at = water_position / m;
	const Vector3 dir = V > 1.0e-3f ? water_momentum.normalized() : Vector3(1, 0, 0);
	if (height > wave_height || water_waves == 0) {
		// The getters answer for the run's biggest wave (the trickle after the main entry makes small ones).
		wave_position = at;
		wave_direction = dir;
		wave_depth = hw;
		wave_product = product;
		wave_mass_ratio = m / (1000.0f * b * hw * hw);
		wave_height = height;
		wave_amplitude = 0.8f * height; // a_M = 4/9 P^4/5 against H_M = 5/9 P^4/5
	}
	water_waves++;
	// The water carries it on (WorldWater.wave: metres and seconds; the ripple solver runs it out at sqrt(g h), water
	// without one makes a splash). Its period in the near field, a slide's width out from the entry.
	if (WorldWater *ww = WorldContracts::water()) {
		const Vector3 flat(dir.x, 0.0f, dir.z);
		ww->wave(at, flat.length_squared() > 1.0e-6f ? flat.normalized() : Vector3(1, 0, 0), height, impulse_wave_period(m / (1000.0f * b * hw * hw), hw, MAX(b, hw)), b);
	}
	emit_signal(SNAME("avalanche_entered_water"), at, dir, m, V, height);
	print_verbose(vformat("snow: avalanche into water, %.0f t at %.1f m/s, %.2f m thick, %.1f m wide, %.1f m deep, slope %.0f deg: P %.3f, H %.2f m", m / 1000.0f, V, s, b, hw, Math::rad_to_deg(alpha), product, height));
	water_mass = 0.0f;
	water_momentum = Vector3();
	water_position = Vector3();
	water_thick = 0.0f;
	water_depth = 0.0f;
	water_slope = 0.0f;
	water_cells.clear();
	water_cell_count = 0;
	(void)p_cover;
}

float SnowAvalanche3D::get_wave_height_at(const Vector3 &p_position) const {
	if (water_waves == 0) {
		return 0.0f;
	}
	const Vector3 d = Vector3(p_position.x - wave_position.x, 0.0f, p_position.z - wave_position.z);
	const float r = d.length();
	const float gamma = r > 1.0e-3f ? Math::acos(CLAMP(d.normalized().dot(wave_direction), -1.0f, 1.0f)) : 0.0f;
	return impulse_wave_height(wave_product, wave_depth, r, gamma);
}

float SnowAvalanche3D::get_wave_period_at(const Vector3 &p_position) const {
	if (water_waves == 0) {
		return 0.0f;
	}
	return impulse_wave_period(wave_mass_ratio, wave_depth, Vector2(p_position.x - wave_position.x, p_position.z - wave_position.z).length());
}

void SnowAvalanche3D::_build_crown(SnowCover3D *p_cover, const Vector<float> &p_released) {
	// The walls the slab leaves (McClung and Schaerer 2006): the crown, a tensile fracture standing the slab's
	// full depth across the top; the flanks, shear fractures down either side that lose height toward the foot;
	// and no wall at the foot (the stauchwall), where the slab rode out over the snow below. Every edge between a
	// released cell and one that kept its snow is chained into lines over the cells' corners (all of them, so
	// the lines run unbroken), rounded (Chaikin's corner cutting, three passes) and set wandering so the grid
	// does not show. Each point stands as high as the step there, times its fade: by its height within the
	// release (full in the upper half, nothing at the foot) and by which way it faces (a wall facing up the
	// slope, the foot's, has none). A cap runs from the wall's top back to the snow outside (the cover's smooth
	// surface rounds the step off over a cell; the cap keeps its edge sharp), and the wall's base shows the weak
	// layer the slab slid on, grey.
	const int n = p_cover->resolution;
	const float cs = p_cover->_cell_size();
	if (p_released.size() != n * n || !is_inside_tree()) {
		return;
	}
	const Vector2 down = _fall_line(p_cover, p_released);
	float g_lo = 1e30f, g_hi = -1e30f;
	for (int i = 0; i < n * n; i++) {
		if (p_released[i] >= 0.05f) {
			g_lo = MIN(g_lo, p_cover->cell_ground[i]);
			g_hi = MAX(g_hi, p_cover->cell_ground[i]);
		}
	}
	if (g_hi < g_lo) {
		return;
	}
	struct Seg {
		Vector2i a, b;
		float height;
		Vector2 normal; // toward the released side
		float albedo; // the snow outside's
	};
	LocalVector<Seg> segs;
	for (int z = 0; z < n; z++) {
		for (int x = 0; x < n; x++) {
			const int i = z * n + x;
			const float s = p_released[i];
			if (s < 0.05f) {
				continue;
			}
			const float up = g_hi - g_lo > 0.5f ? (p_cover->cell_ground[i] - g_lo) / (g_hi - g_lo) : 1.0f;
			const int nb[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
			for (const auto &o : nb) {
				const int xx = x + o[0], zz = z + o[1];
				if (xx < 0 || zz < 0 || xx >= n || zz >= n) {
					continue;
				}
				const int j = zz * n + xx;
				if (p_released[j] > 0.5f * s) {
					continue;
				}
				const Vector2 nrm(-o[0], -o[1]);
				const float facing = nrm.dot(down); // 1 at the crown, 0 on a flank, -1 at the foot
				const float dj = p_cover->cell_swe[j] > 0.01f ? p_cover->cell_swe[j] / MAX(p_cover->cell_density[j], 30.0f) : 0.0f;
				const float fade = Math::smoothstep(0.05f, 0.55f, up) * CLAMP((facing + 0.75f) / 0.6f, 0.0f, 1.0f);
				Seg sg;
				// The shared edge, in corner coordinates (cover cells, corner (x, z) at the cell's low corner).
				if (o[0] != 0) {
					const int ex = o[0] > 0 ? x + 1 : x;
					sg.a = Vector2i(ex, z);
					sg.b = Vector2i(ex, z + 1);
				} else {
					const int ez = o[1] > 0 ? z + 1 : z;
					sg.a = Vector2i(x, ez);
					sg.b = Vector2i(x + 1, ez);
				}
				sg.height = MIN(s - p_released[j], dj) * fade;
				sg.normal = nrm;
				sg.albedo = p_cover->cell_albedo.size() == n * n ? p_cover->cell_albedo[j] : 0.84f;
				segs.push_back(sg);
			}
		}
	}
	if (segs.is_empty()) {
		return;
	}
	// Chain the edges into lines.
	HashMap<Vector2i, LocalVector<uint32_t>> at_corner;
	for (uint32_t k = 0; k < segs.size(); k++) {
		at_corner[segs[k].a].push_back(k);
		at_corner[segs[k].b].push_back(k);
	}
	LocalVector<uint8_t> used;
	used.resize(segs.size());
	for (uint32_t k = 0; k < segs.size(); k++) {
		used[k] = 0;
	}
	struct P {
		Vector2 xz;
		float height;
		Vector2 normal;
		float albedo;
	};
	// Each corner's point is the mean of every edge meeting there (normal, height, albedo), so two lines that
	// meet at a corner (the walk breaks the outline where three or four edges meet) stand their ends on the
	// same spot: from each edge's own normal, the ends were set off the corner different ways and left a slit
	// between them, a dotted outline from afar.
	HashMap<Vector2i, P> corner_point;
	for (const KeyValue<Vector2i, LocalVector<uint32_t>> &kv : at_corner) {
		P cp = { Vector2(kv.key), 0.0f, Vector2(), 0.0f };
		for (uint32_t c : kv.value) {
			cp.height += segs[c].height;
			cp.normal += segs[c].normal;
			cp.albedo += segs[c].albedo;
		}
		const float inv = 1.0f / float(kv.value.size());
		cp.height *= inv;
		cp.albedo *= inv;
		// Opposite edges (a one-cell sliver) cancel: the first edge's way then.
		cp.normal = cp.normal.length_squared() > 1e-4f ? cp.normal.normalized() : segs[kv.value[0]].normal;
		corner_point[kv.key] = cp;
	}
	LocalVector<LocalVector<P>> lines;
	auto walk = [&](uint32_t p_start, Vector2i p_from) {
		LocalVector<P> line;
		Vector2i corner = p_from;
		uint32_t k = p_start;
		line.push_back(corner_point[corner]);
		while (true) {
			used[k] = 1;
			const Vector2i next = segs[k].a == corner ? segs[k].b : segs[k].a;
			line.push_back(corner_point[next]);
			corner = next;
			uint32_t found = UINT32_MAX;
			for (uint32_t c : at_corner[corner]) {
				if (!used[c]) {
					found = c;
					break;
				}
			}
			if (found == UINT32_MAX) {
				break;
			}
			k = found;
		}
		lines.push_back(std::move(line));
	};
	// Open lines first (from a corner with one edge), then what is left (loops).
	for (const KeyValue<Vector2i, LocalVector<uint32_t>> &kv : at_corner) {
		if (kv.value.size() == 1 && !used[kv.value[0]]) {
			walk(kv.value[0], kv.key);
		}
	}
	for (uint32_t k = 0; k < segs.size(); k++) {
		if (!used[k]) {
			walk(k, segs[k].a);
		}
	}
	Ref<SurfaceTool> st;
	st.instantiate();
	st->begin(Mesh::PRIMITIVE_TRIANGLES);
	float height_sum = 0.0f, length_sum = 0.0f;
	Vector3 center_sum;
	auto lerp_p = [](const P &p0, const P &p1, float t) -> P {
		return { p0.xz.lerp(p1.xz, t), Math::lerp(p0.height, p1.height, t), p0.normal.lerp(p1.normal, t), Math::lerp(p0.albedo, p1.albedo, t) };
	};
	for (LocalVector<P> &line : lines) {
		if (line.size() < 2) {
			continue;
		}
		// Chaikin: each pass replaces every edge by points at a quarter and three quarters (ends kept).
		for (int pass = 0; pass < 3; pass++) {
			LocalVector<P> out;
			out.push_back(line[0]);
			for (uint32_t q = 0; q + 1 < line.size(); q++) {
				out.push_back(lerp_p(line[q], line[q + 1], 0.25f));
				out.push_back(lerp_p(line[q], line[q + 1], 0.75f));
			}
			out.push_back(line[line.size() - 1]);
			line = std::move(out);
		}
		// To world space. The wall stands a little inside the released cell (a sixth to a half of a cell, wandering
		// over about 2 m), where the cover's rounded step has dropped most of its height, so the face shows.
		struct W {
			Vector3 top, band, bottom, outer;
			Vector3 normal;
			float albedo;
			float height;
		};
		LocalVector<W> ws;
		for (const P &p : line) {
			const Vector2 nrm = p.normal.length_squared() > 1e-6f ? p.normal.normalized() : Vector2(1, 0);
			const Vector2 wc = (p.xz + Vector2(p_cover->origin_cell)) * cs;
			const float wander = (0.15f + 0.35f * _patch_noise(int(Math::floor(wc.x * 1.5f)), int(Math::floor(wc.y * 1.5f)))) * cs;
			const Vector2 w = wc + nrm * wander;
			// The snow surface on the side that kept its snow, at the outside cell's middle.
			const Vector3 outside(wc.x - nrm.x * 0.5f * cs, 0.0f, wc.y - nrm.y * 0.5f * cs);
			const float surface = p_cover->get_ground_height_at(outside) + p_cover->get_surface_depth_at(outside);
			const float ragged = 0.04f * (_patch_noise(int(w.x * 8.0f), int(w.y * 8.0f)) - 0.5f);
			// The cap's top follows the slope from the outside cell's middle to the wall.
			const Vector3 at(w.x, 0.0f, w.y);
			const float rise = (p_cover->get_ground_height_at(at) - p_cover->get_ground_height_at(outside));
			const float top_y = surface + rise + ragged;
			const float hgt = MAX(p.height, 0.0f);
			W e;
			e.top = Vector3(w.x, top_y, w.y);
			e.band = Vector3(w.x, top_y - MAX(hgt - 0.12f, 0.6f * hgt), w.y);
			e.bottom = Vector3(w.x, top_y - hgt - 0.05f, w.y);
			e.outer = Vector3(outside.x, surface + 0.01f, outside.z);
			e.normal = Vector3(nrm.x, 0.0f, nrm.y);
			e.albedo = p.albedo;
			e.height = hgt;
			ws.push_back(e);
		}
		for (uint32_t q = 0; q + 1 < ws.size(); q++) {
			const W &e0 = ws[q];
			const W &e1 = ws[q + 1];
			if (MAX(e0.height, e1.height) < 0.04f) {
				continue;
			}
			const Vector3 nq = (e0.normal + e1.normal).normalized();
			const Color c0(e0.albedo, e0.albedo, e0.albedo), c1(e1.albedo, e1.albedo, e1.albedo);
			const Color weak(BED_ALBEDO - 0.04f, BED_ALBEDO - 0.04f, BED_ALBEDO - 0.03f);
			auto quad = [&](const Vector3 &a, const Vector3 &b, const Vector3 &c, const Vector3 &d, const Color &ca, const Color &cb, const Color &cc, const Color &cd, const Vector3 &nrm) {
				const Vector3 v[4] = { a, b, c, d };
				const Color col[4] = { ca, cb, cc, cd };
				// Wound to face the way its normal does (clockwise front, as Godot's): the path tracer takes the
				// winding's side as the front and leaves a normal behind it unlit.
				const bool flip = Plane(a, b, c).normal.dot(nrm) < 0.0f;
				const int tri_front[6] = { 0, 1, 2, 0, 2, 3 };
				const int tri_back[6] = { 0, 2, 1, 0, 3, 2 };
				for (int t : (flip ? tri_back : tri_front)) {
					st->set_normal(nrm);
					st->set_color(col[t]);
					st->add_vertex(v[t]);
				}
			};
			// The face: the slab's section, then the weak layer at its base.
			quad(e0.top, e1.top, e1.band, e0.band, c0, c1, c1, c0, nq);
			quad(e0.band, e1.band, e1.bottom, e0.bottom, c0, c1, weak, weak, nq);
			// The cap, from the wall's top back to the snow outside.
			Vector3 up = (e1.top - e0.top).cross(e0.outer - e0.top);
			up = up.y < 0.0f ? -up : up;
			up = up.length_squared() > 1e-8f ? up.normalized() : Vector3(0, 1, 0);
			quad(e0.outer, e1.outer, e1.top, e0.top, c0, c1, c1, c0, up);
			const float len = e0.top.distance_to(e1.top);
			length_sum += len;
			height_sum += e0.height * len;
			center_sum += e0.top * len;
		}
	}
	if (length_sum <= 0.0f) {
		return;
	}
	if (crown_material.is_null()) {
		crown_material.instantiate();
		crown_material->set_albedo(Color(1.0f, 1.0f, 1.0f));
		crown_material->set_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true); // linear: the cover's albedo
		crown_material->set_roughness(0.8f);
		crown_material->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
	}
	MeshInstance3D *mi = memnew(MeshInstance3D);
	mi->set_name("Crown");
	mi->set_as_top_level(true);
	mi->set_mesh(st->commit());
	mi->set_material_override(crown_material);
	add_child(mi, false, INTERNAL_MODE_BACK);
	mi->set_global_transform(Transform3D());
	Crown cr;
	cr.mesh = mi;
	cr.center = center_sum / length_sum;
	cr.height = height_sum / length_sum;
	if (crowns.size() >= 8) {
		crowns[0].mesh->queue_free();
		crowns.remove_at(0);
	}
	crowns.push_back(cr);
}

void SnowAvalanche3D::_bury_crowns(SnowCover3D *p_cover) {
	// New snow fills the step: the wall goes once it has had most of its height.
	for (uint32_t k = 0; k < crowns.size();) {
		crowns[k].buried += p_cover->get_new_snow_at(crowns[k].center);
		if (crowns[k].buried > 0.8f * crowns[k].height) {
			crowns[k].mesh->queue_free();
			crowns.remove_at(k);
		} else {
			k++;
		}
	}
}

float SnowAvalanche3D::get_glide_at(const Vector3 &p_position) const {
	const SnowCover3D *cover = _cover();
	if (!cover || glide.size() != cover->resolution * cover->resolution) {
		return 0.0f;
	}
	const float cs = cover->_cell_size();
	const int x = int(Math::floor(p_position.x / cs)) - cover->origin_cell.x;
	const int z = int(Math::floor(p_position.z / cs)) - cover->origin_cell.y;
	const int n = cover->resolution;
	return (x < 0 || z < 0 || x >= n || z >= n) ? 0.0f : glide[z * n + x];
}

float SnowAvalanche3D::get_peak_pressure_at(const Vector3 &p_position) const {
	const int x = int(Math::floor(p_position.x / cell)) - flow_origin.x;
	const int z = int(Math::floor(p_position.z / cell)) - flow_origin.y;
	if (x < 0 || z < 0 || x >= res || z >= res || pressure.size() != res * res) {
		return 0.0f;
	}
	return pressure[z * res + x];
}

float SnowAvalanche3D::get_pressure_at(const Vector3 &p_position) const {
	// The moving snow's dynamic pressure on what stands in it now (Pa): the dense flow's 1/2 rho u^2 up to its
	// height, else the powder cloud's.
	float pa = 0.0f;
	const int x = int(Math::floor(p_position.x / cell)) - flow_origin.x;
	const int z = int(Math::floor(p_position.z / cell)) - flow_origin.y;
	if (running && x >= 0 && z >= 0 && x < res && z < res) {
		const int i = z * res + x;
		if (h[i] >= FLOW_MIN) {
			const float u2 = (hu[i] * hu[i] + hv[i] * hv[i]) / (h[i] * h[i]);
			pa = 0.5f * rho_run * u2;
		}
	}
	const float rho = get_powder_density_at(p_position);
	if (rho > 0.0f) {
		Vector3 v;
		float w = 0.0f;
		for (const Parcel &pc : parcels) {
			if (Vector2(p_position.x - pc.position.x, p_position.z - pc.position.z).length() <= pc.radius) {
				v += pc.velocity * pc.mass;
				w += pc.mass;
			}
		}
		if (w > 0.0f) {
			pa = MAX(pa, 0.5f * rho * (v / w).length_squared());
		}
	}
	return pa;
}

float SnowAvalanche3D::get_flowing_volume() const {
	float v = 0.0f;
	for (int i = 0; i < h.size(); i++) {
		v += h[i];
	}
	return v / rho_run * cell * cell;
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
	return h[z * res + x] / rho_run;
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
				if (natural_release && !_check_release(cover)) {
					_check_glide(cover);
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
	ClassDB::bind_method(D_METHOD("get_pressure_at", "position"), &SnowAvalanche3D::get_pressure_at);
	ClassDB::bind_method(D_METHOD("get_peak_pressure_at", "position"), &SnowAvalanche3D::get_peak_pressure_at);
	ClassDB::bind_method(D_METHOD("get_detrained_volume"), &SnowAvalanche3D::get_detrained_volume);
	ClassDB::bind_method(D_METHOD("set_wet_enabled", "enable"), &SnowAvalanche3D::set_wet_enabled);
	ClassDB::bind_method(D_METHOD("is_wet_enabled"), &SnowAvalanche3D::is_wet_enabled);
	ClassDB::bind_method(D_METHOD("set_wet_mu", "mu"), &SnowAvalanche3D::set_wet_mu);
	ClassDB::bind_method(D_METHOD("get_wet_mu"), &SnowAvalanche3D::get_wet_mu);
	ClassDB::bind_method(D_METHOD("set_wet_xi", "xi"), &SnowAvalanche3D::set_wet_xi);
	ClassDB::bind_method(D_METHOD("get_wet_xi"), &SnowAvalanche3D::get_wet_xi);
	ClassDB::bind_method(D_METHOD("set_wet_cohesion", "pascals"), &SnowAvalanche3D::set_wet_cohesion);
	ClassDB::bind_method(D_METHOD("get_wet_cohesion"), &SnowAvalanche3D::get_wet_cohesion);
	ClassDB::bind_method(D_METHOD("set_wet_density", "density"), &SnowAvalanche3D::set_wet_density);
	ClassDB::bind_method(D_METHOD("get_wet_density"), &SnowAvalanche3D::get_wet_density);
	ClassDB::bind_method(D_METHOD("set_wet_weakening", "share"), &SnowAvalanche3D::set_wet_weakening);
	ClassDB::bind_method(D_METHOD("get_wet_weakening"), &SnowAvalanche3D::get_wet_weakening);
	ClassDB::bind_method(D_METHOD("set_glide_enabled", "enable"), &SnowAvalanche3D::set_glide_enabled);
	ClassDB::bind_method(D_METHOD("is_glide_enabled"), &SnowAvalanche3D::is_glide_enabled);
	ClassDB::bind_method(D_METHOD("set_glide_rate", "rate"), &SnowAvalanche3D::set_glide_rate);
	ClassDB::bind_method(D_METHOD("get_glide_rate"), &SnowAvalanche3D::get_glide_rate);
	ClassDB::bind_method(D_METHOD("set_glide_release", "glide"), &SnowAvalanche3D::set_glide_release);
	ClassDB::bind_method(D_METHOD("get_glide_release"), &SnowAvalanche3D::get_glide_release);
	ClassDB::bind_method(D_METHOD("get_glide_at", "position"), &SnowAvalanche3D::get_glide_at);
	ClassDB::bind_static_method("SnowAvalanche3D", D_METHOD("impulse_product", "speed", "thickness", "mass", "width", "water_depth", "slope"), &SnowAvalanche3D::impulse_product);
	ClassDB::bind_static_method("SnowAvalanche3D", D_METHOD("impulse_wave_height", "product", "water_depth", "distance", "angle"), &SnowAvalanche3D::impulse_wave_height);
	ClassDB::bind_static_method("SnowAvalanche3D", D_METHOD("impulse_wave_period", "mass_ratio", "water_depth", "distance"), &SnowAvalanche3D::impulse_wave_period);
	ClassDB::bind_method(D_METHOD("get_wave_height_at", "position"), &SnowAvalanche3D::get_wave_height_at);
	ClassDB::bind_method(D_METHOD("get_wave_period_at", "position"), &SnowAvalanche3D::get_wave_period_at);
	ClassDB::bind_method(D_METHOD("get_water_entry_mass"), &SnowAvalanche3D::get_water_entry_mass);
	ClassDB::bind_method(D_METHOD("get_wave_count"), &SnowAvalanche3D::get_wave_count);
	ClassDB::bind_method(D_METHOD("get_ice_broken"), &SnowAvalanche3D::get_ice_broken);
	ClassDB::bind_method(D_METHOD("set_water_enabled", "enable"), &SnowAvalanche3D::set_water_enabled);
	ClassDB::bind_method(D_METHOD("is_water_enabled"), &SnowAvalanche3D::is_water_enabled);
	ClassDB::bind_method(D_METHOD("get_glide_count"), &SnowAvalanche3D::get_glide_count);
	ClassDB::bind_method(D_METHOD("get_crown_count"), &SnowAvalanche3D::get_crown_count);
	ClassDB::bind_method(D_METHOD("set_crowns_enabled", "enable"), &SnowAvalanche3D::set_crowns_enabled);
	ClassDB::bind_method(D_METHOD("is_crowns_enabled"), &SnowAvalanche3D::is_crowns_enabled);
	ClassDB::bind_method(D_METHOD("get_glide_crack_count"), &SnowAvalanche3D::get_glide_crack_count);
	ClassDB::bind_method(D_METHOD("get_flow_wetness"), &SnowAvalanche3D::get_flow_wetness);
	ClassDB::bind_method(D_METHOD("set_forests_enabled", "enable"), &SnowAvalanche3D::set_forests_enabled);
	ClassDB::bind_method(D_METHOD("is_forests_enabled"), &SnowAvalanche3D::is_forests_enabled);
	ClassDB::bind_method(D_METHOD("set_forest_friction", "friction"), &SnowAvalanche3D::set_forest_friction);
	ClassDB::bind_method(D_METHOD("get_forest_friction"), &SnowAvalanche3D::get_forest_friction);
	ClassDB::bind_method(D_METHOD("set_forest_detrainment", "pascals"), &SnowAvalanche3D::set_forest_detrainment);
	ClassDB::bind_method(D_METHOD("get_forest_detrainment"), &SnowAvalanche3D::get_forest_detrainment);
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
	ADD_GROUP("Crowns", "crowns_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "crowns_enabled"), "set_crowns_enabled", "is_crowns_enabled");
	ADD_GROUP("Wet snow", "wet_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "wet_enabled"), "set_wet_enabled", "is_wet_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wet_mu", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_wet_mu", "get_wet_mu");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wet_xi", PROPERTY_HINT_RANGE, "100,10000,1,suffix:m/s²"), "set_wet_xi", "get_wet_xi");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wet_cohesion", PROPERTY_HINT_RANGE, "0,3000,1,suffix:Pa"), "set_wet_cohesion", "get_wet_cohesion");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wet_density", PROPERTY_HINT_RANGE, "100,800,1,suffix:kg/m³"), "set_wet_density", "get_wet_density");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wet_weakening", PROPERTY_HINT_RANGE, "0.01,1,0.01"), "set_wet_weakening", "get_wet_weakening");
	ADD_GROUP("Glide", "glide_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "glide_enabled"), "set_glide_enabled", "is_glide_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "glide_rate", PROPERTY_HINT_RANGE, "0,5,0.01,suffix:m/day"), "set_glide_rate", "get_glide_rate");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "glide_release", PROPERTY_HINT_RANGE, "0.01,10,0.01,suffix:m"), "set_glide_release", "get_glide_release");
	ADD_GROUP("Water", "water_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "water_enabled"), "set_water_enabled", "is_water_enabled");
	ADD_GROUP("Forests", "forest");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "forests_enabled"), "set_forests_enabled", "is_forests_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "forest_friction", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_forest_friction", "get_forest_friction");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "forest_detrainment", PROPERTY_HINT_RANGE, "0,1000,0.1,suffix:Pa"), "set_forest_detrainment", "get_forest_detrainment");
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
	ADD_SIGNAL(MethodInfo("avalanche_entered_water", PropertyInfo(Variant::VECTOR3, "position"), PropertyInfo(Variant::VECTOR3, "direction"), PropertyInfo(Variant::FLOAT, "mass"), PropertyInfo(Variant::FLOAT, "speed"), PropertyInfo(Variant::FLOAT, "wave_height")));
	ADD_SIGNAL(MethodInfo("glide_crack_opened", PropertyInfo(Variant::VECTOR3, "position")));
	ADD_SIGNAL(MethodInfo("avalanche_released", PropertyInfo(Variant::VECTOR3, "position"), PropertyInfo(Variant::FLOAT, "volume")));
	ADD_SIGNAL(MethodInfo("avalanche_stopped", PropertyInfo(Variant::VECTOR3, "position"), PropertyInfo(Variant::FLOAT, "volume"), PropertyInfo(Variant::FLOAT, "run_out")));
}

SnowAvalanche3D::SnowAvalanche3D() {
	trigger_groups.push_back("snow_deform");
	trigger_groups.push_back("grass_trample");
	trigger_groups.push_back("world_contact");
}

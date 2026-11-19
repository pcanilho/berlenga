/**************************************************************************/
/*  weather_runoff_debris.cpp                                             */
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

// Post-fire debris flows on the runoff's drainage (docs/ideas/rain.md, "Mud after fire"; agreed with PhysX and mud).
//
// After a fire, the bare, water-repellent slopes shed the first intense rain as sheet flow that gathers soil in the
// channels: most post-fire debris flows start within two years of the fire, in the first storms, by runoff, not
// landslides (Cannon et al. 2008). The USGS likelihood model M1 (Staley et al. 2017) gives a basin's chance of one
// from the storm's 15-minute rainfall accumulation R (mm) times three basin terms:
//   x = -3.63 + 0.41 X1 R + 0.67 X2 R + 0.70 X3 R,  P = 1 / (1 + e^-x)
//   X1: the share of the basin burnt at moderate or high severity on slopes of 23% or more,
//   X2: its mean burn severity (dNBR / 1000; WorldFire.burnt_at stands in), X3: its soil's erodibility (K factor).
// The volume follows Gartner et al. (2014): ln V = 4.22 + 0.13 sqrt(i15) + 0.36 ln(Bmh) + 0.39 sqrt(relief), with i15
// the peak 15-minute intensity (mm/h), Bmh the area burnt moderate-high (km2) and the basin's relief (m). Basins are
// the drainage's small catchments (0.01 to 10 km2, the range both models were fitted on), each read at its outlet
// (where it joins a larger stream, water or the map's edge). The event gives the channel below the outlet with its
// bankfull width (Castro and Jackson 2001: w = 2.37 A^0.38, Pacific Northwest), so the flow knows where to run.

#include "weather_runoff_3d.h"

#include "weather_map_3d.h"

#include "modules/world/world_contracts.h"

static constexpr float DF_MIN_AREA = 0.01e6f; // m2
static constexpr float DF_MAX_AREA = 10.0e6f; // m2
static constexpr float DF_MODERATE = 0.27f; // burnt_at (dNBR / 1000) from which the burn counts as moderate or high
static constexpr float DF_STEEP = 0.23f; // m/m
static constexpr double DF_REARM = 24.0 * 3600.0; // s of weather time before the same basin can run again
static constexpr int DF_LOG = 32;

float WeatherRunoff3D::debris_flow_likelihood(float p_rain15_mm, float p_burnt_steep_share, float p_burnt, float p_kf) {
	const float r = MAX(p_rain15_mm, 0.0f);
	const float x = -3.63f + 0.41f * p_burnt_steep_share * r + 0.67f * p_burnt * r + 0.70f * p_kf * r;
	return 1.0f / (1.0f + Math::exp(-x));
}

float WeatherRunoff3D::debris_flow_volume(float p_i15_mm_h, float p_burnt_km2, float p_relief_m) {
	return Math::exp(4.22f + 0.13f * Math::sqrt(MAX(p_i15_mm_h, 0.0f)) + 0.36f * Math::log(MAX(p_burnt_km2, 1e-3f)) + 0.39f * Math::sqrt(MAX(p_relief_m, 0.0f)));
}

float WeatherRunoff3D::bankfull_width(float p_area_km2) {
	return CLAMP(2.37f * Math::pow(MAX(p_area_km2, 1e-4f), 0.38f), 0.5f, 60.0f);
}

// The fire's marks per cell, faded with the burn's age (most debris flows come in the first year or two; by the third
// the ground has largely healed: Cannon et al. 2008, Staley et al. 2017), and the soil's erodibility there. Only what
// WorldFire reports as changed is read again.
void WeatherRunoff3D::_fire_cells(int p_x0, int p_z0, int p_x1, int p_z1) {
	const int n = baked_n;
	const WorldFire *wf = WorldContracts::fire();
	const WorldGround *wg = WorldContracts::ground();
	for (int z = MAX(p_z0, 0); z <= MIN(p_z1, n - 1); z++) {
		for (int x = MAX(p_x0, 0); x <= MIN(p_x1, n - 1); x++) {
			const int i = z * n + x;
			const Vector3 p(origin.x + (x + 0.5f) * cell, Math::is_finite(height[i]) ? height[i] : 0.0f, origin.y + (z + 0.5f) * cell);
			const float age = wf->burn_age_at(p);
			float b = 0.0f;
			if (age >= 0.0f) {
				b = CLAMP(wf->burnt_at(p), 0.0f, 1.0f) * (1.0f - Math::smoothstep(1.5f, 3.0f, age));
			}
			fire_burnt[i] = b;
			fire_kf[i] = 0.0f;
			if (b > 0.0f) {
				// No erodibility in the ground contract: soft soil about 0.3 (silt loams 0.3 to 0.45, firmer and more
				// cohesive soils less), firm ground and rock 0.05.
				const WorldGroundSoil soil = wg->soil_at(p);
				fire_kf[i] = soil.soft ? CLAMP(0.42f - 0.01f * soil.cohesion, 0.15f, 0.45f) : 0.05f;
			}
		}
	}
}

void WeatherRunoff3D::_fire_refresh() {
	const int n = baked_n;
	const WorldFire *wf = WorldContracts::fire();
	if (!wf->has_provider()) {
		if (fire_any) {
			for (uint32_t k = 0; k < fire_burnt.size(); k++) {
				fire_burnt[k] = 0.0f;
			}
			fire_any = false;
		}
		fire_generation = UINT64_MAX;
		return;
	}
	const uint64_t gen = wf->get_burn_generation();
	if (wf != fire_seen) { // another stub put in or taken out: its generations are its own
		fire_seen = wf;
		fire_generation = UINT64_MAX;
	}
	if (gen == fire_generation) {
		return;
	}
	Rect2 changed;
	if (fire_generation == UINT64_MAX || !wf->burn_changes_since(fire_generation, changed)) {
		_fire_cells(0, 0, n - 1, n - 1);
	} else if (changed.has_area()) {
		_fire_cells(int(Math::floor((changed.position.x - origin.x) / cell)), int(Math::floor((changed.position.y - origin.y) / cell)),
				int(Math::floor((changed.get_end().x - origin.x) / cell)), int(Math::floor((changed.get_end().y - origin.y) / cell)));
	}
	fire_generation = gen;
	fire_any = false;
	for (int i = 0; i < n * n && !fire_any; i++) {
		fire_any = fire_burnt[i] > 0.0f;
	}
}

void WeatherRunoff3D::_debris_flows() {
	const int n = baked_n;
	if (!fire_any || n <= 0) {
		return;
	}
	const int nn = n * n;
	const float ca = cell * cell;
	// Each basin's sums, carried down the drainage (upstream first): burnt moderate-high on steep ground (m2), burn
	// severity, erodibility and 15-minute rain times area, and its highest ground.
	LocalVector<float> b_mh, b_sev, b_kf, b_rain, b_top;
	b_mh.resize(nn);
	b_sev.resize(nn);
	b_kf.resize(nn);
	b_rain.resize(nn);
	b_top.resize(nn);
	for (int i = 0; i < nn; i++) {
		const float b = fire_burnt[i];
		b_mh[i] = (b >= DF_MODERATE && slope[i] >= DF_STEEP) ? ca : 0.0f;
		b_sev[i] = b * ca;
		b_kf[i] = fire_kf[i] * ca;
		b_rain[i] = rain_now[i] * 0.25f * ca; // mm in 15 minutes at the rate now
		b_top[i] = Math::is_finite(height[i]) ? height[i] : -1e9f;
	}
	for (int k = 0; k < nn; k++) {
		const int i = order[k];
		const int r = receiver[i];
		if (r >= 0) {
			b_mh[r] += b_mh[i];
			b_sev[r] += b_sev[i];
			b_kf[r] += b_kf[i];
			b_rain[r] += b_rain[i];
			b_top[r] = MAX(b_top[r], b_top[i]);
		}
	}
	const WorldWater *ww = WorldContracts::get_singleton() ? WorldContracts::get_singleton()->get_water().ptr() : nullptr;
	for (int i = 0; i < nn; i++) {
		const float a = area[i];
		if (a < DF_MIN_AREA || a > DF_MAX_AREA || b_sev[i] <= 0.0f || b_rain[i] <= 0.0f) {
			continue;
		}
		// The basin's outlet: where it joins a larger stream (its receiver gathers half again its area), or the map's edge.
		const int r = receiver[i];
		if (r >= 0 && area[r] < 1.5f * a && area[r] <= DF_MAX_AREA) {
			continue;
		}
		if (sim_time - df_fired_at[i] < DF_REARM) {
			continue;
		}
		const float rain15 = b_rain[i] / a;
		const float x1 = b_mh[i] / a;
		const float x2 = b_sev[i] / a;
		const float x3 = b_kf[i] / a;
		const float p = debris_flow_likelihood(rain15, x1, x2, x3);
		if (p < debris_flow_threshold) {
			continue;
		}
		df_fired_at[i] = sim_time;
		const float relief = b_top[i] - height[i];
		const float volume = debris_flow_volume(rain15 * 4.0f, b_mh[i] * 1e-6f, relief);
		// The channel it runs down: from the outlet along the drainage while it is steep enough to carry the flow
		// (about 2 degrees and up; it spreads into a fan below, a few cells on), stopping at open water.
		PackedVector3Array path;
		PackedFloat32Array widths;
		PackedFloat32Array severity;
		const WorldFire *wf = WorldContracts::fire();
		int c = i;
		int flat = 0;
		for (int step = 0; step < 512 && c >= 0; step++) {
			const Vector3 pc(origin.x + (c % n + 0.5f) * cell, Math::is_finite(height[c]) ? height[c] : 0.0f, origin.y + (c / n + 0.5f) * cell);
			path.push_back(pc);
			widths.push_back(bankfull_width(area[c] * 1e-6f));
			severity.push_back(CLAMP(wf->burnt_at(pc), 0.0f, 1.0f));
			if (ww != nullptr && ww->depth_at(Vector3(pc.x, pc.y - 0.05f, pc.z)) > 0.0f) {
				break;
			}
			flat = slope[c] < 0.035f ? flat + 1 : 0;
			if (flat > 4) {
				break;
			}
			c = receiver[c];
		}
		Dictionary ev;
		ev["id"] = df_next_id++;
		ev["time"] = sim_time;
		ev["position"] = path.is_empty() ? Vector3() : path[0];
		ev["probability"] = p;
		ev["volume"] = volume;
		ev["rain15"] = rain15;
		ev["i15"] = rain15 * 4.0f;
		ev["basin_area"] = a * 1e-6f;
		ev["burnt_share"] = x2;
		ev["burnt_steep_share"] = x1;
		ev["relief"] = relief;
		ev["path"] = path;
		ev["widths"] = widths;
		ev["severity"] = severity;
		df_log.push_back(ev);
		while (df_log.size() > DF_LOG) {
			df_log.remove_at(0);
		}
		emit_signal(SNAME("debris_flow"), ev);
		WorldContracts::ground()->emit_signal(SNAME("debris_flow"), ev);
	}
}

Array WeatherRunoff3D::get_debris_flows_since(int64_t p_id) const {
	Array out;
	for (int k = 0; k < df_log.size(); k++) {
		const Dictionary ev = df_log[k];
		if (int64_t(ev["id"]) > p_id) {
			out.push_back(ev);
		}
	}
	return out;
}

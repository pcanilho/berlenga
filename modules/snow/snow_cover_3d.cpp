/**************************************************************************/
/*  snow_cover_3d.cpp                                                     */
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


#include "snow_cover_3d.h"

#include "snow_avalanche_3d.h"
#include "snow_bodies_3d.h"
#include "snow_lake_ice_3d.h"
#include "snow_surface_3d.h"

#include "snow_climate.h"
#include "snow_fall_3d.h"

#include "modules/terrain_deform/terrain_data_map_3d.h"
#include "modules/terrain_deform/terrain_dunes.h"
#include "modules/world/world_contracts.h"
#include "modules/terrain_deform/terrain_deformer_3d.h"
#include "modules/weather/weather_map_3d.h"
#include "modules/world/world_contracts.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/templates/sort_array.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/physics/character_body_3d.h"
#include "scene/3d/physics/rigid_body_3d.h"
#include "scene/3d/physics/vehicle_body_3d.h"
#include "scene/3d/light_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "scene/resources/3d/world_3d.h"
#include "servers/physics_3d/physics_server_3d.h"
#include "servers/rendering/rendering_server.h"

SnowCover3D *SnowCover3D::active = nullptr;

namespace {

// Fresh snow albedo and where it ages to (CLASS, Verseghy 1991): dry snow toward 0.70, melting snow toward
// 0.50, e-folding in 100 hours. Snowfall of SNOW_REFRESH_MM water equivalent or more restores fresh snow.
constexpr float ALBEDO_FRESH = 0.84f;
constexpr float ALBEDO_OLD_DRY = 0.70f;
constexpr float ALBEDO_OLD_WET = 0.50f;
constexpr float SNOW_REFRESH_MM = 10.0f;
// Settling (CLASS): density relaxes toward a maximum that grows with depth, e-folding in 100 hours.
constexpr float SETTLE_RATE = 0.01f; // per hour
// Below this the cell is bare.
constexpr float SWE_MIN = 0.01f; // mm

struct SpindriftStronger {
	bool operator()(const SnowPowderCloud3D::Source &p_a, const SnowPowderCloud3D::Source &p_b) const { return p_a.rate > p_b.rate; }
};
constexpr float ICE_DENSITY = 917.0f; // kg/m3
constexpr float SALT_DEPRESSION = 0.0593f; // K per g of NaCl in a kg of water (dilute; 2 ions, Kf 1.86)
constexpr float SALT_EUTECTIC = 21.1f; // K: no brine stays liquid below -21.1 C
constexpr float SALT_FILM = 1.0f; // mm, the least brine film the salt dissolves into
constexpr float ICE_DECAY = 0.005f / 24.0f; // m per C hour above freezing, bare lake ice
constexpr float ICE_MAX = 1.5f; // m of lake ice at most
constexpr float ICE_CONDUCTIVITY = 2.24f; // W/(m K)
constexpr float ICE_LATENT = 3.34e5f; // J/kg
constexpr float ICE_AIR_TRANSFER = 20.0f; // W/(m2 K), the air over the ice (Ashton 1989: 10 to 30)
// Clear-sky beam at the ground, Meinel and Meinel (1976): 1353 W/m2 x 0.7^(AM^0.678), AM = 1 / sin(elevation).
constexpr float SOLAR_CONSTANT = 1353.0f;
// Diffuse sky light on a horizontal surface, a share of the beam.
constexpr float DIFFUSE_SHARE = 0.1f;
// Winstral Sx: a 30 degree window of upwind directions, every 5 degrees.
constexpr int SX_DIRECTIONS = 7;
constexpr float SX_STEP_DEGREES = 5.0f;
// Sx at which drift reaches full strength, degrees.
constexpr float SX_FULL = 15.0f;

} // namespace

// ---------------------------------------------------------------------------------------------------------
// Published relations

float SnowCover3D::snowfall_fraction(float p_celsius, float p_humidity) {
	// Jennings et al. (2018), the bivariate logistic model on air temperature and relative humidity (%),
	// with the coefficients given in Jennings and Molotch (2019): 1 / (1 + exp(-10.04 + 1.41 T + 0.09 RH)).
	// Used as the share of precipitation that falls as snow, so the change from snow to rain is gradual.
	// One formula for every system: the weather map's rain share, so snow and rain always sum to the
	// precipitation (mixed precipitation, sleet, is both falling at once).
	return 1.0f - WeatherMap3D::rain_fraction(p_celsius, p_humidity);
}

float SnowCover3D::snow_conductivity(float p_density) {
	// Sturm et al. (1997), the quadratic fit over 156 to 600 kg/m3: k = 0.138 - 1.01 rho + 3.233 rho^2,
	// rho in g/cm3.
	const float r = CLAMP(p_density, 100.0f, 600.0f) / 1000.0f;
	return 0.138f - 1.01f * r + 3.233f * r * r;
}

float SnowCover3D::lake_ice_growth(float p_ice, float p_air_celsius, float p_snow_depth, float p_snow_density, float p_hours) {
	// Stefan's law with insulation (Leppaeranta 1993; Ashton 1989): the latent heat of the water freezing at
	// the base is conducted up through the ice, the snow and the air's boundary layer,
	// rho_i L dh/dt = (0 - T_a) / (h / k_i + d_s / k_s + 1 / H). Integrated exactly over the step: with
	// R0 = d_s / k_s + 1 / H, (k_i R0 + h)^2 grows by 2 k_i^2 (-T_a) t / (rho_i L k_i), so a thin ice skin
	// starts at once and thick ice grows as the square root of the frost's degree-days.
	if (p_air_celsius >= 0.0f || p_hours <= 0.0f) {
		return p_ice;
	}
	const float r0 = MAX(p_snow_depth, 0.0f) / snow_conductivity(p_snow_density) + 1.0f / ICE_AIR_TRANSFER;
	const float a = ICE_CONDUCTIVITY * r0 + MAX(p_ice, 0.0f);
	const float grow = 2.0f * ICE_CONDUCTIVITY * -p_air_celsius * p_hours * 3600.0f / (ICE_DENSITY * ICE_LATENT);
	return MIN(Math::sqrt(a * a + grow) - ICE_CONDUCTIVITY * r0, ICE_MAX);
}

float SnowCover3D::new_snow_density(float p_celsius) {
	// Hedstrom and Pomeroy (1998): 67.92 + 51.25 exp(T / 2.59) kg/m3, cold snow light, warm snow wet and dense.
	return CLAMP(67.92f + 51.25f * Math::exp(MIN(p_celsius, 3.0f) / 2.59f), 50.0f, 250.0f);
}

float SnowCover3D::depth_hoar_step(float p_share, float p_gradient, float p_hours, float p_wet_share) {
	// Depth hoar (Akitaya 1974; Marbouty 1980; Colbeck 1983): over a temperature gradient above about 10 C/m vapour
	// runs up from the warm base and the grains there grow into large facets and cups over one to three weeks
	// (SNOWPACK's kinetic growth, Lehning et al. 2002, holds the same 10 C/m threshold). Here the base's share
	// grows at (G - 10) / 10 per two weeks (a share of 1 in two weeks at 20 C/m). It stays under a weaker gradient
	// (facets round only over months) and goes once melt water reaches the base (wet grains, a day).
	float f = CLAMP(p_share, 0.0f, 1.0f);
	if (p_hours <= 0.0f) {
		return f;
	}
	if (p_wet_share > 0.03f) {
		return f * Math::exp(-p_hours / 24.0f);
	}
	if (p_gradient > 10.0f) {
		f += (p_gradient - 10.0f) / 10.0f * p_hours / 336.0f;
	}
	return CLAMP(f, 0.0f, 1.0f);
}

float SnowCover3D::depth_hoar_strength(float p_share, float p_density) {
	// Jamieson and Johnston (2001): faceted grains and depth hoar hold 18.5 (rho / rho_ice)^2.11 kPa; settled
	// rounded snow 14.5 (rho / rho_ice)^1.73 kPa, here with weeks of sintering (x4, Jamieson 1995's gain in strength
	// levels off). The base between the two by its depth hoar share.
	const float r = CLAMP(p_density, 30.0f, 917.0f) / 917.0f;
	const float facets = 18.5e3f * Math::pow(r, 2.11f);
	const float rounded = 4.0f * 14.5e3f * Math::pow(r, 1.73f);
	return Math::lerp(rounded, facets, CLAMP(p_share, 0.0f, 1.0f));
}

Vector2 SnowCover3D::sastrugi_step(Vector2 p_relief, Vector2 p_wind_dir, float p_eroded, float p_filled, float p_depth, bool p_hard) {
	// Sastrugi (Filhol and Sturm 2015, JGR Earth Surface 120): erosional bedforms of a cohesive (wind-packed or
	// sintered) surface, ridges along the wind with steep prows facing it, 5-50 cm tall. Wind eroding a hard
	// surface carves them by about the depth it takes (the hard patches stay as ridges); the relief turns toward
	// the eroding wind as it grows. Snow laid on them (drift or fall) and melt fill them by their own depth. At
	// most 0.4 m and half the pack.
	Vector2 v = p_relief;
	if (p_hard && p_eroded > 0.0f && p_wind_dir.length_squared() > 0.0f) {
		v += p_eroded * p_wind_dir.normalized();
	}
	float a = v.length();
	if (a <= 0.0f) {
		return Vector2();
	}
	const float keep = CLAMP(MIN(a - MAX(p_filled, 0.0f), MIN(0.4f, 0.5f * MAX(p_depth, 0.0f))), 0.0f, a);
	return v * (keep / a);
}

float SnowCover3D::wind_snow_density(float p_celsius, float p_wind10) {
	// Crocus (Vionnet et al. 2012, GMD 5, after Pahaut 1976): rho = 109 + 6 (T - Tfus) + 26 U^0.5 kg/m3, at least
	// 50: snow falling in wind lands broken and packed. Calm, Hedstrom and Pomeroy's density stands (the higher).
	const float crocus = 109.0f + 6.0f * MIN(p_celsius, 0.0f) + 26.0f * Math::sqrt(MAX(p_wind10, 0.0f));
	return CLAMP(MAX(new_snow_density(p_celsius), crocus), 50.0f, 350.0f);
}

float SnowCover3D::drift_index(float p_wind10, float p_density, float p_fresh) {
	// Crocus (Vionnet et al. 2012; Guyomarc'h and Merindol 1998): mobility Mo = 0.34 (0.75 d - 0.5 s + 0.5) +
	// 0.66 F(rho), F = 1.25 - 0.0042 (max(rho, 50) - 50); driftability SI = -2.868 exp(-0.085 U) + 1 + Mo, the surface
	// drifting when SI > 0. Dendricity d and sphericity s from the snow's freshness (1 new: d 1, s 0; 0 old: d 0, s 1).
	const float f = CLAMP(p_fresh, 0.0f, 1.0f);
	const float mo = 0.34f * (0.75f * f - 0.5f * (1.0f - f) + 0.5f) + 0.66f * (1.25f - 0.0042f * (MAX(p_density, 50.0f) - 50.0f));
	return -2.868f * Math::exp(-0.085f * MAX(p_wind10, 0.0f)) + 1.0f + mo;
}

float SnowCover3D::wind_packing_step(float p_density, float p_wind10, float p_fresh, float p_hours) {
	// Crocus's wind-driven compaction: the top layers settle toward 350 kg/m3 with time constant 48 h / G, G = SI
	// exp(-z / 0.1 m), here the mean of exp(-z / 0.1) over the top 10 cm (0.632). Sommer et al. (2018, The Cryosphere 12)
	// saw it in the wind tunnel: packing only where the snow drifts.
	const float si = drift_index(p_wind10, p_density, p_fresh);
	if (si <= 0.0f || p_density >= 350.0f || p_hours <= 0.0f) {
		return p_density;
	}
	const float g = si * 0.632f;
	return p_density + (350.0f - p_density) * (1.0f - Math::exp(-p_hours * g / 48.0f));
}

float SnowCover3D::holding_depth(float p_slope_degrees, float p_min_mm, float p_decay) {
	// SnowSlide (Bernhardt and Schulz 2010): the snow a slope holds falls off exponentially with its angle,
	// down to a floor at 75 degrees and steeper.
	if (p_slope_degrees >= 75.0f) {
		return p_min_mm;
	}
	return p_min_mm * Math::exp(p_decay * (75.0f - p_slope_degrees));
}

// ---------------------------------------------------------------------------------------------------------
// Cells

Vector3 SnowCover3D::_follow_position() const {
	if (owner) {
		return owner->_follow_position();
	}
	if (!follow_target.is_empty()) {
		const Node3D *n = Object::cast_to<Node3D>(get_node_or_null(follow_target));
		if (n && n->is_inside_tree()) {
			return n->get_global_position();
		}
	}
	if (is_inside_tree() && get_viewport()) {
		const Camera3D *cam = get_viewport()->get_camera_3d();
		if (cam) {
			return cam->get_global_position();
		}
	}
	return is_inside_tree() ? get_global_position() : Vector3();
}

void SnowCover3D::_allocate() {
	_finish_background(true);
	_finish_sx(true);
	const int nn = resolution * resolution;
	cell_ground.resize(nn);
	cell_swe.resize(nn);
	cell_liquid.resize(nn);
	cell_density.resize(nn);
	cell_albedo.resize(nn);
	cell_runoff.resize(nn);
	cell_sx.resize(nn);
	cell_new_depth.resize(nn);
	cell_new_depth.fill(0.0f);
	cell_canopy.resize(nn);
	cell_canopy.fill(0.0f);
	cell_canopy_load.resize(nn);
	cell_canopy_load.fill(0.0f);
	for (Vector<float> *f : { &cell_crust, &cell_crust_cycles, &cell_crust_burial, &cell_crust_wet, &cell_hoar, &cell_hoar_layer, &cell_hoar_burial, &cell_hoar_age, &cell_cups, &cell_seal, &cell_slush, &cell_packed, &cell_glaze, &cell_wind_slab, &cell_slab_cover, &cell_depth_hoar, &cell_sas_x, &cell_sas_z, &cell_lap, &cell_lap_deep, &cell_lap_cover, &cell_lap_in }) {
		f->resize(nn);
		f->fill(0.0f);
	}
	catch_image = Image::create_empty(resolution, resolution, false, Image::FORMAT_RGBAF);
	catch_texture.unref();
	cups_image = Image::create_empty(resolution, resolution, false, Image::FORMAT_RGBAH);
	cups_texture.unref();
	sastrugi_image = Image::create_empty(resolution, resolution, false, Image::FORMAT_RGH);
	sastrugi_texture.unref();
	strata_image.unref();
	strata_texture.unref();
	cell_height_valid.resize(nn);
	cell_ground.fill(0.0f);
	cell_swe.fill(0.0f);
	cell_liquid.fill(0.0f);
	cell_density.fill(100.0f);
	cell_albedo.fill(ALBEDO_FRESH);
	cell_runoff.fill(0.0f);
	cell_sx.fill(0.0f);
	cell_height_valid.fill(0);
	cell_steep.resize(nn);
	cell_steep.fill(0);
	cell_water.resize(nn);
	cell_water.fill(-Math::INF);
	cell_ice.resize(nn);
	cell_ice.fill(0.0f);
	cell_white_ice.resize(nn);
	cell_white_ice.fill(0.0f);
	steep_dirty = true;
	cell_cornice_x.resize(cornices_enabled ? nn : 0);
	cell_cornice_z.resize(cornices_enabled ? nn : 0);
	cell_cornice_x.fill(0.0f);
	cell_cornice_z.fill(0.0f);
	cell_cornice_crack.resize(cornices_enabled ? nn : 0);
	cell_cornice_crack.fill(0.0f);
	cornice_failures.clear();
	_layers_alloc();
	sx_cache.clear();
	snow_image = Image::create_empty(resolution, resolution, false, Image::FORMAT_RGBAH);
	snow_texture.unref();
	globals_added = false;
	origin_valid = false;
	sx_valid = false;
	sx_dirty = false;
	_publish_cells();
}

Vector3 SnowCover3D::_cell_center(int p_x, int p_z) const {
	const float cs = _cell_size();
	const int i = p_z * resolution + p_x;
	return Vector3((origin_cell.x + p_x + 0.5f) * cs, cell_ground.size() > i ? cell_ground[i] : 0.0f, (origin_cell.y + p_z + 0.5f) * cs);
}

SnowCover3D::GroundQuery SnowCover3D::_ground_query() const {
	if (owner) {
		return owner->_ground_query();
	}
	return GroundHeightQuery::make(this, height_source, ground_collision_mask);
}

float SnowCover3D::_query_ground(const GroundQuery &p_q, Vector3 p_position) {
	return p_q.height(p_position);
}

float SnowCover3D::sample_ground_height(Vector3 p_position) const {
	return _query_ground(_ground_query(), p_position);
}

void SnowCover3D::sample_ground_heights(Vector2 p_origin, float p_spacing, int p_width, int p_height, float *r_heights) const {
	const GroundQuery q = _ground_query();
	for (int z = 0; z < p_height; z++) {
		for (int x = 0; x < p_width; x++) {
			r_heights[z * p_width + x] = _query_ground(q, Vector3(p_origin.x + x * p_spacing, 0.0f, p_origin.y + z * p_spacing));
		}
	}
}

void SnowCover3D::_sample_heights(int p_from_row, int p_to_row, const Vector<uint8_t> *p_only) {
	const int n = resolution;
	const GroundQuery q = _ground_query();
	const float cs = _cell_size();
	for (int z = p_from_row; z < p_to_row; z++) {
		for (int x = 0; x < n; x++) {
			const int i = z * n + x;
			if (p_only && !(*p_only)[i]) {
				continue;
			}
			const Vector3 at((origin_cell.x + x + 0.5f) * cs, 0.0f, (origin_cell.y + z + 0.5f) * cs);
			float h = _query_ground(q, at);
			// With a height source, roofs and other overhead colliders on shelter_collision_mask catch the snow
			// over the ground below them (the pack lies on top; materials under them stay bare).
			if (shelter_collision_mask && q.space && q.has_source()) {
				PhysicsDirectSpaceState3D::RayParameters rp;
				rp.from = Vector3(at.x, q.y0 + 10000.0f, at.z);
				rp.to = Vector3(at.x, q.y0 - 10000.0f, at.z);
				rp.collision_mask = shelter_collision_mask;
				PhysicsDirectSpaceState3D::RayResult rr;
				if (q.space->intersect_ray(rp, rr) && (!Math::is_finite(h) || rr.position.y > h + 0.3f)) {
					h = rr.position.y;
				}
			}
			cell_height_valid.write[i] = Math::is_finite(h) ? 1 : 0;
			cell_ground.write[i] = Math::is_finite(h) ? h : q.y0;
		}
	}
	steep_dirty = true;
	_refresh_cell_inputs(p_from_row, p_to_row, p_only);
}

void SnowCover3D::_follow(Vector3 p_center) {
	if (owner) {
		return; // a fixed square
	}
	const float cs = _cell_size();
	const Vector2i want(int(Math::floor(p_center.x / cs)) - resolution / 2, int(Math::floor(p_center.z / cs)) - resolution / 2);
	if (!origin_valid) {
		origin_cell = want;
		origin_valid = true;
		_finish_sx(true);
		_sample_heights(0, resolution);
		sx_valid = false;
		_seed_from_world(nullptr);
		_publish_cells();
		return;
	}
	if (want != origin_cell) {
		_shift(want);
	}
}

void SnowCover3D::_shift(Vector2i p_new_origin) {
	_finish_background(true);
	// A bake in flight reads the ground: let it land first (it is shifted with the rest).
	_finish_sx(true);
	// What the cells leaving the square did goes back to the world tier first.
	_feed_world();
	const int n = resolution;
	const Vector2i d = p_new_origin - origin_cell;
	// The pack is history: cells that stay keep theirs. Cells entering the square take the world tier's pack
	// where there is one, else the state of the nearest cell that was inside it.
	Vector<float> *fields[] = { &cell_ground, &cell_swe, &cell_liquid, &cell_density, &cell_albedo, &cell_runoff, &cell_sx, &cell_new_depth, &cell_canopy, &cell_canopy_load, &cell_crust, &cell_crust_cycles, &cell_crust_burial, &cell_crust_wet, &cell_hoar, &cell_hoar_layer, &cell_hoar_burial, &cell_hoar_age, &cell_cups, &cell_seal, &cell_slush, &cell_packed, &cell_glaze, &cell_wind_slab, &cell_slab_cover, &cell_depth_hoar, &cell_sas_x, &cell_sas_z, &cell_climate_precip, &cell_water, &cell_ice, &cell_white_ice, &cell_lap, &cell_lap_deep, &cell_lap_cover, &cell_lap_in, &cell_salt_pack, &cell_salt_seen };
	Vector<uint8_t> fresh;
	fresh.resize(n * n);
	for (Vector<float> *f : fields) {
		if (f->size() != n * n) {
			continue;
		}
		const Vector<float> old = *f;
		float *w = f->ptrw();
		for (int z = 0; z < n; z++) {
			for (int x = 0; x < n; x++) {
				const int sx = x + d.x;
				const int sz = z + d.y;
				const bool inside = sx >= 0 && sz >= 0 && sx < n && sz < n;
				w[z * n + x] = old[CLAMP(sz, 0, n - 1) * n + CLAMP(sx, 0, n - 1)];
				fresh.write[z * n + x] = inside ? 0 : 1;
			}
		}
	}
	if (layer_n.size() == n * n) {
		// The layers move with their cells; cells coming in start with none (the next update gives them the
		// bulk pack as one old layer).
		const int L = PACK_LAYERS;
		const Vector<float> os = layer_swe, orr = layer_rho, oa = layer_age, ol = layer_liq;
		const Vector<uint8_t> ok = layer_kind, on = layer_n;
		for (int z = 0; z < n; z++) {
			for (int x = 0; x < n; x++) {
				const int sx = x + d.x, sz = z + d.y;
				const int i = z * n + x;
				if (sx < 0 || sz < 0 || sx >= n || sz >= n) {
					layer_n.write[i] = 0;
					continue;
				}
				const int j = sz * n + sx;
				layer_n.write[i] = on[j];
				for (int k = 0; k < L; k++) {
					layer_swe.write[i * L + k] = os[j * L + k];
					layer_rho.write[i * L + k] = orr[j * L + k];
					layer_age.write[i * L + k] = oa[j * L + k];
					layer_liq.write[i * L + k] = ol[j * L + k];
					layer_kind.write[i * L + k] = ok[j * L + k];
				}
			}
		}
	}
	origin_cell = p_new_origin;
	// New cells need their own ground; Sx is rebaked in the background, the shifted one serves meanwhile.
	_sample_heights(0, n, &fresh);
	_seed_from_world(&fresh);
	if (world) {
		world->_upload();
	}
	_publish_cells(); // the getters read the moved square from now
	sx_dirty = sx_valid;
}

float SnowCover3D::_sx_at(int p_x, int p_z, Vector2 p_upwind) const {
	// Winstral, Elder and Davis (2002): the largest upward slope from the cell to the terrain upwind within
	// dmax, averaged over a 30 degree window of directions. Positive: sheltered, snow drifts in. Negative:
	// exposed, wind strips it.
	const int n = resolution;
	const float cs = _cell_size();
	// Rocks, walls, buildings and vehicles count as terrain (Winstral and Marks 2002 took vegetation and
	// structures into the surface the same way): the wind strips their tops and snow fills their lee.
	const float *ob = cell_obstacle.size() == n * n ? cell_obstacle.ptr() : nullptr;
	const float z0 = cell_ground[p_z * n + p_x] + (ob ? ob[p_z * n + p_x] : 0.0f);
	const int steps = MAX(1, int(drift_distance / cs));
	float sum = 0.0f;
	for (int k = 0; k < SX_DIRECTIONS; k++) {
		const float a = Math::deg_to_rad(SX_STEP_DEGREES * (k - SX_DIRECTIONS / 2));
		const Vector2 dir = p_upwind.rotated(a);
		float best = -Math::PI * 0.5f;
		bool any = false;
		for (int s = 1; s <= steps; s++) {
			const int x = p_x + int(Math::round(dir.x * s));
			const int z = p_z + int(Math::round(dir.y * s));
			if (x < 0 || z < 0 || x >= n || z >= n) {
				break;
			}
			const float dist = s * cs;
			best = MAX(best, Math::atan((cell_ground[z * n + x] + (ob ? ob[z * n + x] : 0.0f) - z0) / dist));
			any = true;
		}
		sum += any ? best : 0.0f;
	}
	return Math::rad_to_deg(sum / SX_DIRECTIONS);
}

void SnowCover3D::_sx_row(uint32_t p_row, void *p_unused) {
	const int n = resolution;
	const Vector2 upwind = -sx_next_direction;
	float *w = sx_next.ptrw() + p_row * n;
	for (int x = 0; x < n; x++) {
		w[x] = _sx_at(x, int(p_row), upwind);
	}
}

void SnowCover3D::_start_sx(Vector2 p_wind_dir) {
	// Rows in parallel, in the background: the drift keeps the last Sx until this one lands.
	_finish_sx(true);
	// The obstacles read last, for this bake's rows (no rows run now).
	if (obstacle_active && obstacle_next.size() == resolution * resolution && obstacle_origin == origin_cell) {
		cell_obstacle = obstacle_next;
	} else {
		cell_obstacle.clear();
	}
	sx_next.resize(resolution * resolution);
	sx_next.ptrw(); // unshare before the threads write
	sx_next_direction = p_wind_dir.normalized();
	sx_next_origin = origin_cell;
	sx_dirty = false;
	sx_task = WorkerThreadPool::get_singleton()->add_template_group_task(this, &SnowCover3D::_sx_row, (void *)nullptr, resolution, -1, false, String("SnowCover3D Sx"));
}

void SnowCover3D::_finish_sx(bool p_wait) {
	if (sx_task == WorkerThreadPool::INVALID_TASK_ID) {
		return;
	}
	if (!p_wait && !WorkerThreadPool::get_singleton()->is_group_task_completed(sx_task)) {
		return;
	}
	WorkerThreadPool::get_singleton()->wait_for_group_task_completion(sx_task);
	sx_task = WorkerThreadPool::INVALID_TASK_ID;
	if (sx_next_origin == origin_cell && sx_next.size() == cell_sx.size()) {
		cell_sx = sx_next;
		sx_direction = sx_next_direction;
		sx_valid = true;
	}
}

void SnowCover3D::_update_steep() {
	const int n = resolution;
	const float cs = _cell_size();
	const float tan_min = Math::tan(Math::deg_to_rad(slide_angle));
	for (int z = 0; z < n; z++) {
		for (int x = 0; x < n; x++) {
			const float h = cell_ground[z * n + x];
			float steepest = 0.0f;
			for (int dz = -1; dz <= 1; dz++) {
				for (int dx = -1; dx <= 1; dx++) {
					const int nx = x + dx;
					const int nz = z + dz;
					if ((dx == 0 && dz == 0) || nx < 0 || nz < 0 || nx >= n || nz >= n) {
						continue;
					}
					const float dist = cs * ((dx != 0 && dz != 0) ? Math::SQRT2 : 1.0f);
					steepest = MAX(steepest, (h - cell_ground[nz * n + nx]) / dist);
				}
			}
			cell_steep.write[z * n + x] = steepest >= tan_min ? 1 : 0;
		}
	}
	steep_dirty = false;
}

// ---------------------------------------------------------------------------------------------------------
// Update

void SnowCover3D::_prepare_inputs(double p_weather_dt, bool p_blocking) {
	Inputs &in = inputs;
	in.dt_h = float(p_weather_dt) / 3600.0f;
	// A climate bake drives its own weather.
	in.weather = driven ? nullptr : WeatherMap3D::get_active();
	const Vector3 center = _follow_position();
	if (driven) {
		in.ref_height = driven_ref_height;
		in.temperature = temperature;
		in.humidity = humidity;
	} else if (in.weather) {
		in.ref_height = in.weather->get_ground_height();
		in.temperature = in.weather->get_air_temperature();
		in.humidity = in.weather->get_air_humidity();
	} else {
		in.ref_height = is_inside_tree() ? get_global_position().y : 0.0f;
		in.temperature = temperature;
		in.humidity = humidity;
	}
	in.base_precip = precipitation_rate;
	// Under a warm layer aloft its melted share falls as rain, freezing where it lands on the cold pack.
	in.warm_nose_melt = in.weather ? in.weather->get_warm_nose_melt() : 0.0f;
	// The weather the cells read, as it is now: the update may run on a worker while the map moves on.
	in.wx = in.weather ? in.weather->get_snapshot() : WeatherMap3D::Snapshot();
	_gather_seal();
	in.climate_map = get_climate_map_node();
	in.climate_background = in.climate_map ? in.climate_map->get_climate_recipe()->get_precipitation() : 0.0f;

	// The sun: this node's light, else the weather map's (the world tier: its owner's; a bake: the date's).
	in.sun_dir = Vector3();
	in.direct_normal = 0.0f;
	const DirectionalLight3D *sun = nullptr;
	if (driven) {
		in.sun_dir = driven_sun;
		in.direct_normal = driven_sun.y > 0.01f ? driven_direct : 0.0f;
	} else if (owner) {
		in.sun_dir = owner->inputs.sun_dir;
		in.direct_normal = owner->inputs.direct_normal;
	} else if (is_inside_tree()) {
		if (!sun_light.is_empty()) {
			sun = Object::cast_to<DirectionalLight3D>(get_node_or_null(sun_light));
		} else if (in.weather && !in.weather->get_sun_light().is_empty()) {
			sun = Object::cast_to<DirectionalLight3D>(in.weather->get_node_or_null(in.weather->get_sun_light()));
		}
	}
	if (sun && sun->is_visible_in_tree()) {
		const Vector3 to_sun = sun->get_global_transform().basis.get_column(2).normalized();
		if (to_sun.y > 0.01f) {
			const float air_mass = 1.0f / to_sun.y;
			in.sun_dir = to_sun;
			in.direct_normal = SOLAR_CONSTANT * Math::pow(0.7f, Math::pow(air_mass, 0.678f));
		}
	}

	// Drift: only when the wind can lift snow (Li and Pomeroy 1997: about 7 m/s at 10 m for dry snow), full
	// at twice that.
	in.wind_factor = 0.0f;
	in.drift_norm = 1.0f;
	// The flat-ground wind: the cover applies its own terrain terms (Liston curvature, Winstral Sx) on it.
	last_wind = driven ? driven_wind : (in.weather ? in.weather->get_mean_wind_at(center, false) : Vector2());
	in.wind_speed = last_wind.length();
	if (drift_enabled && (in.weather || driven)) {
		const Vector2 wind = last_wind;
		const float u = wind.length();
		const float ut = MAX(drift_threshold_wind, 0.1f);
		in.wind_factor = CLAMP((u - ut) / ut, 0.0f, 1.0f);
		_finish_sx(p_blocking);
		if (in.wind_factor > 0.0f && driven) {
			// A bake turns the wind every day: Sx per 22.5 degrees, each baked once.
			const int bin = int(Math::round(Math::atan2(wind.y, wind.x) / Math::deg_to_rad(22.5f))) & 15;
			if (!sx_cache.has(bin)) {
				const float a = bin * Math::deg_to_rad(22.5f);
				_start_sx(Vector2(Math::cos(a), Math::sin(a)));
				_finish_sx(true);
				sx_cache.insert(bin, cell_sx);
			}
			cell_sx = sx_cache[bin];
			sx_valid = true;
		} else if (in.wind_factor > 0.0f) {
			const Vector2 dir = wind / u;
			const bool turned = !sx_valid || sx_direction.dot(dir) < Math::cos(Math::deg_to_rad(10.0f));
			const bool baking = sx_task != WorkerThreadPool::INVALID_TASK_ID;
			if ((turned && !(baking && sx_next_direction.dot(dir) >= Math::cos(Math::deg_to_rad(10.0f)))) || (sx_dirty && !baking)) {
				_start_sx(dir);
			}
			_finish_sx(p_blocking);
		}
		if (in.wind_factor > 0.0f && !sx_valid) {
			// No Sx yet for this map: no drift until the first bake lands.
			in.wind_factor = 0.0f;
		}
		if (in.wind_factor > 0.0f) {
			// Normalised so the map keeps the snow that fell: drift moves snow, it does not make it.
			double sum = 0.0;
			const int nn = resolution * resolution;
			for (int i = 0; i < nn; i++) {
				sum += MAX(0.0f, 1.0f + drift_strength * in.wind_factor * CLAMP(cell_sx[i] / SX_FULL, -1.0f, 1.0f));
			}
			in.drift_norm = sum > 0.0 ? float(nn / sum) : 1.0f;
		}
	}
}

void SnowCover3D::_update_row(uint32_t p_row, void *p_unused) {
	const Inputs &in = inputs;
	const int n = resolution;
	const float cs = _cell_size();
	const int z = int(p_row);
	for (int x = 0; x < n; x++) {
		const int i = z * n + x;
		const float h = cell_ground[i];
		const float t = in.temperature - lapse_rate * 0.001f * (h - in.ref_height);

		const Vector3 pos = _cell_center(x, z);
		// Precipitation, split into snow and rain. The weather as it was when the update started (its
		// snapshot: this may run on a worker while the map updates).
		const float rate = in.wx.valid ? in.wx.rain_at(pos) : in.base_precip;
		// More falls higher up and on windward slopes (orographic enhancement): the terrain climate's
		// precipitation field when there is one (read into cell_climate_precip on the main thread), else
		// precipitation_gradient per 100 m.
		const float oro = in.climate_map && in.climate_background > 0.0f && cell_climate_precip.size() == n * n
				? MAX(cell_climate_precip[i] / in.climate_background, 0.0f)
				: MAX(1.0f + precipitation_gradient * (h - in.ref_height) * 0.01f, 0.0f);
		const float p = MAX(rate, 0.0f) * oro * in.dt_h;
		// The weather's split when it runs (a warm layer aloft melts snow to rain, a deep cold layer refreezes it to
		// pellets: WeatherMap3D.liquid_share()), else the plain one.
		const float share = in.wx.valid ? 1.0f - in.wx.liquid_share_at(t, in.humidity, h) : MIN(snowfall_fraction(t, in.humidity), 1.0f - in.warm_nose_melt);
		float snow = p * share;
		float rain_extra = 0.0f;
		const float rain_base = p - snow;
		if (in.wind_factor > 0.0f) {
			snow *= in.drift_norm * MAX(0.0f, 1.0f + drift_strength * in.wind_factor * CLAMP(cell_sx[i] / SX_FULL, -1.0f, 1.0f));
		}
		// Trees hold snow in their crowns (Hedstrom and Pomeroy 1998): a canopy of closure C and leaf area
		// LAI (from C as the vegetation's Beer's law) takes I = (I* - I0)(1 - exp(-C P / I*)) of a fall P, up to I* = S (0.27 + 46 / rho_s) LAI with
		// S = 5.9 kg/m2 (spruce). It unloads onto the snow below as it warms toward thaw and in the wind
		// (Roesch et al. 2001, as in CLM5): I (T - 270.15 K) / 1.87e5 and I u / 1.56e5 per second.
		if (cell_canopy.size() == n * n && cell_canopy[i] > 0.01f) {
			const float closure = cell_canopy[i];
			float load = cell_canopy_load[i];
			if (snow > 0.0f) {
				const float cap = canopy_capacity(closure, new_snow_density(t));
				if (cap > load) {
					const float take = (cap - load) * (1.0f - Math::exp(-closure * snow / cap));
					load += take;
					snow -= take;
				}
			}
			if (load > 0.0f) {
				const float u = last_wind.length();
				const float unload = MAX(t + 273.15f - 270.15f, 0.0f) / 1.87e5f + u / 1.56e5f;
				const float drop = load * (1.0f - Math::exp(-unload * in.dt_h * 3600.0f));
				load -= drop;
				snow += drop;
				if (t > 0.0f) {
					// In a thaw the rest drips off as water.
					const float drip = MIN(load, melt_temperature_factor * t * in.dt_h);
					load -= drip;
					rain_extra += drip;
				}
			}
			cell_canopy_load.write[i] = load;
		}

		const float rain = rain_base + rain_extra;
		float swe = cell_swe[i];
		float liq = cell_liquid[i];
		float rho = cell_density[i];
		float alb = cell_albedo[i];
		float runoff = 0.0f;
		// Sea salt (WorldWeather.salt_at): it dissolves into the pack's liquid water as brine, whose freezing point
		// lies lower by about 0.059 K per gram of salt in a kilogram of water (NaCl, to the eutectic at -21 C); the
		// snow and ice then melt as if the air were that much warmer. The brine is the pack's liquid, at least what it
		// holds (liquid_capacity), at least a 1 mm film: thin snow salted by spray melts some degrees below 0 C, a deep
		// pack barely notices.
		// The salt is the pack's own (what fell on it since, cell_salt_pack), and the meltwater draining out takes
		// its share, so a salted pack melts until its brine has run out, not for ever. Bare ice reads the surface salt.
		float salt_dt = 0.0f, salt_dt_ice = 0.0f;
		const bool salted = cell_salt_pack.size() == n * n;
		float salt_pack = salted ? cell_salt_pack[i] : 0.0f;
		if (salt_pack > 0.0f) {
			const float brine = MAX(MAX(cell_liquid[i], liquid_capacity * MAX(cell_swe[i] - cell_liquid[i], 0.0f)), SALT_FILM);
			salt_dt = MIN(SALT_DEPRESSION * salt_pack / brine, SALT_EUTECTIC);
		}
		if (cell_salt.size() == n * n && cell_salt[i] > 0.0f) {
			salt_dt_ice = MIN(SALT_DEPRESSION * cell_salt[i] / SALT_FILM, SALT_EUTECTIC);
		}

		// Water (follow-up 15). Lake ice grows in frost under the snow on it (Stefan's law) and, once the
		// snow is gone, melts in a thaw at the melt's temperature rate. Snow on open water, or on ice too thin
		// to hold it, melts into the water (it arrives there as runoff).
		const bool on_water = cell_water.size() == n * n && cell_water[i] > -1e29f;
		float ice = 0.0f;
		if (on_water) {
			ice = lake_ice_enabled ? cell_ice[i] : 0.0f;
			if (lake_ice_enabled) {
				const float ds = swe > SWE_MIN ? swe / MAX(rho, 30.0f) : 0.0f;
				// Bare ice under salt melts as if warmer by its brine's depression; under snow the pack takes it.
				const float ti = ds > 0.0f ? t : t + salt_dt_ice;
				if (ti < 0.0f) {
					ice = lake_ice_growth(ice, ti, ds, rho, in.dt_h);
				} else if (ds <= 0.0f && ice > 0.0f) {
					// The thaw works down from the top, through the white snow ice first. Bare lake ice decays by
					// degree-days at about 0.5 cm per C day (Bilello 1980; Ashton 1983), sun and warm water
					// included, faster than snow's melt factor gives.
					const float m = MIN(ICE_DECAY * ti * in.dt_h, ice);
					ice -= m;
					cell_white_ice.write[i] = MAX(cell_white_ice[i] - m, 0.0f);
				}
				cell_ice.write[i] = ice;
				cell_white_ice.write[i] = MIN(cell_white_ice[i], ice);
			}
			if (ice < ICE_HOLD) {
				if (cell_lap.size() == n * n) {
					cell_lap.write[i] = cell_lap_deep.write[i] = cell_lap_cover.write[i] = 0.0f;
				}
				cell_swe.write[i] = 0.0f;
				cell_liquid.write[i] = 0.0f;
				cell_density.write[i] = 100.0f;
				cell_albedo.write[i] = ALBEDO_FRESH;
				cell_runoff.write[i] = in.dt_h > 0.0f ? (snow + rain + (swe > SWE_MIN ? swe : 0.0f)) / in.dt_h : 0.0f;
				cell_new_depth.write[i] = 0.0f;
				cell_crust.write[i] = 0.0f;
				cell_crust_cycles.write[i] = 0.0f;
				cell_crust_burial.write[i] = 0.0f;
				cell_crust_wet.write[i] = 0.0f;
				_clear_hoar(i);
				continue;
			}
		}

		float crust = cell_crust[i];
		float cycles = cell_crust_cycles[i];
		float burial = cell_crust_burial[i];
		float crust_wet = cell_crust_wet[i];
		// Snow falling in wind lands denser (Crocus); where it drifts as it lands it is wind slab (follow-up 45).
		const bool packing = wind_packing_enabled && cell_wind_slab.size() == n * n;
		const float rho_fall = packing ? wind_snow_density(t, in.wind_speed) : new_snow_density(t);
		float slab = packing && swe > SWE_MIN ? cell_wind_slab[i] : 0.0f;
		float slab_cover = packing && swe > SWE_MIN ? cell_slab_cover[i] : 0.0f;
		if (snow > 0.0f && crust > 0.0f) {
			burial += snow / rho_fall; // new snow buries the crust
		}

		cell_new_depth.write[i] = 0.0f;
		if (snow > 0.0f) {
			const float rho_new = rho_fall;
			const float depth_old = swe > SWE_MIN ? swe / rho : 0.0f;
			const float depth_new = snow / rho_new;
			if (packing && t < 0.0f && drift_index(in.wind_speed, rho_new, 1.0f) > 0.0f) {
				slab += depth_new;
				slab_cover = 0.0f;
			} else if (packing) {
				slab_cover += depth_new; // soft snow over the slab
			}
			cell_new_depth.write[i] = depth_new;
			if (hoar_enabled) {
				_layers_bury_hoar(i);
			}
			_layers_push(i, snow, rho_new, packing && t < 0.0f && drift_index(in.wind_speed, rho_new, 1.0f) > 0.0f ? LAYER_WIND : LAYER_NEW);
			swe = (swe > SWE_MIN ? swe : 0.0f) + snow;
			rho = swe / MAX(depth_old + depth_new, 1e-6f);
			alb += (ALBEDO_FRESH - alb) * MIN(snow / SNOW_REFRESH_MM, 1.0f);
		}

		// Impurities (light-absorbing particles): deposition lands on the surface; new snow buries what is there
		// (its depth over it); the melt brings buried particles back up, as they stay
		// behind while the snow melts around them (melt amplification, Doherty et al. 2013).
		const bool lap_on = cell_lap.size() == n * n;
		float lap = 0.0f, lap_deep = 0.0f, lap_cover = 0.0f;
		if (lap_on) {
			lap = cell_lap[i];
			lap_deep = cell_lap_deep[i];
			lap_cover = cell_lap_cover[i];
			const float d_new = cell_new_depth[i];
			if (d_new > 0.0f && lap + lap_deep > 0.0f) {
				// One buried layer: the surface's and the older buried particles under their new covers, at the
				// cover that shows as much of them as the two would (e-folding 2 cm in snow).
				const float total = lap + lap_deep;
				const float shown = lap * Math::exp(-d_new / 0.02f) + lap_deep * Math::exp(-(lap_cover + d_new) / 0.02f);
				lap_deep = total;
				lap = 0.0f;
				lap_cover = shown > total * 1e-6f ? -0.02f * Math::log(shown / total) : lap_cover + d_new;
			}
			if (impurities && cell_lap_in.size() == n * n) {
				lap += cell_lap_in[i] * in.dt_h;
			}
		}
		const float lap_drop = lap_on ? impurity_albedo_drop(lap + lap_deep * Math::exp(-lap_cover / 0.02f)) : 0.0f;

		if (swe <= SWE_MIN) {
			// Bare ground: rain passes through.
			if (lap_on) {
				cell_lap.write[i] = cell_lap_deep.write[i] = cell_lap_cover.write[i] = 0.0f;
			}
			cell_swe.write[i] = 0.0f;
			cell_liquid.write[i] = 0.0f;
			cell_density.write[i] = 100.0f;
			cell_albedo.write[i] = ALBEDO_FRESH;
			cell_runoff.write[i] = in.dt_h > 0.0f ? (rain + swe) / in.dt_h : 0.0f;
			cell_crust.write[i] = 0.0f;
			cell_crust_cycles.write[i] = 0.0f;
			cell_crust_burial.write[i] = 0.0f;
			cell_crust_wet.write[i] = 0.0f;
			if (cell_slush.size() == n * n && cell_slush[i] > 0.0f) {
				cell_runoff.write[i] += in.dt_h > 0.0f ? cell_slush[i] / in.dt_h : 0.0f;
				cell_slush.write[i] = 0.0f;
			}
			if (cell_packed.size() == n * n) {
				cell_packed.write[i] = 0.0f;
				cell_glaze.write[i] = 0.0f;
			}
			_clear_hoar(i);
			continue;
		}
		const float liq_before = liq;
		const float rho_before = rho;
		float refrozen = 0.0f;
		float surface_melt = 0.0f;
		float sun_melt = 0.0f, air_melt = 0.0f;

		// Rain soaks into the pack (and drains out below what it holds).
		liq += rain;
		swe += rain;

		// Melt (Pellicciotti et al. 2005): M = TF T + SRF (1 - albedo) G above the threshold temperature,
		// G the shortwave reaching the cell, its own slope facing the sun (Hock 1999).
		const float ts = t + salt_dt; // the air as the salted snow feels it
		const bool melting = ts > melt_threshold;
		if (melting) {
			float g = 0.0f;
			if (in.direct_normal > 0.0f) {
				const int xl = MAX(x - 1, 0), xr = MIN(x + 1, n - 1);
				const int zd = MAX(z - 1, 0), zu = MIN(z + 1, n - 1);
				const Vector3 normal = Vector3(-(cell_ground[z * n + xr] - cell_ground[z * n + xl]) / ((xr - xl) * cs), 1.0f,
						-(cell_ground[zu * n + x] - cell_ground[zd * n + x]) / ((zu - zd) * cs))
											   .normalized();
				// Through the clouds: the weather map's cloud shadow when it bakes one for a sun, else the cloud
				// cover by Kasten and Czeplak (1980), 1 - 0.75 N^3.4.
				float vis = 1.0f;
				if (in.wx.valid) {
					vis = in.wx.has_sun ? in.wx.sun_visibility_at(pos) : 1.0f - 0.75f * Math::pow(in.wx.cloud_cover_at(pos), 3.4f);
				}
				g = in.direct_normal * (MAX(normal.dot(in.sun_dir), 0.0f) * vis + DIFFUSE_SHARE * in.sun_dir.y);
			}
			const float m = (melt_temperature_factor * ts + melt_shortwave_factor * (1.0f - MAX(alb - lap_drop, 0.05f)) * g) * in.dt_h;
			surface_melt = CLAMP(m, 0.0f, MAX(swe - liq, 0.0f));
			liq += surface_melt;
			// The share the direct sun melts (it digs suncups) and the share the air does (it smooths them).
			if (m > 0.0f) {
				const float sun_part = melt_shortwave_factor * (1.0f - MAX(alb - lap_drop, 0.05f)) * MAX(g - in.direct_normal * DIFFUSE_SHARE * in.sun_dir.y, 0.0f) * in.dt_h;
				sun_melt = surface_melt * CLAMP(sun_part / m, 0.0f, 1.0f);
				air_melt = surface_melt - sun_melt;
			}
		}
		// Fire: the heat it puts into the pack melts snow whatever the air, its energy over the latent heat of
		// fusion (334 kJ/kg), M = H dt / L_f (mm water); the cold pack's own warming is small beside a fire's tens
		// of kW/m2 and left out.
		if (cell_fire.size() == n * n && cell_fire[i] > 0.0f) {
			const float m = cell_fire[i] * 3600.0f / 334000.0f * in.dt_h;
			const float fm = CLAMP(m, 0.0f, MAX(swe - liq, 0.0f));
			surface_melt += fm;
			liq += fm;
		}
		if (!melting && surface_melt <= 0.0f && ts < 0.0f && liq > 0.0f) {
			// Meltwater and rain refreeze in a cold pack, at the temperature factor's rate.
			refrozen = MIN(liq, melt_temperature_factor * -ts * in.dt_h);
			liq -= refrozen;
		}
		if (crust_enabled) {
			_update_crust(crust, cycles, burial, crust_wet, rain, surface_melt / MAX(rho, 30.0f), refrozen, liq_before, liq);
		}
		swe = MAX(swe, liq);

		// The pack holds liquid_capacity of its ice as liquid; the rest drains out. Over frozen ground it cannot
		// soak away: it ponds in the base as a saturated slush layer (cell_slush, mm: pore water, so it fills the
		// snow without adding to its depth or its SWE) and leaves only sideways, by Darcy flow down the slope
		// (slushflows; Onesti 1985, Hestnes 1998), up to the pores the snow has (the rest runs off over it). In
		// the cold it freezes into the pack's base (superimposed ice: denser, not deeper).
		float slush = cell_slush.size() == n * n ? cell_slush[i] : 0.0f;
		const float seal = slush_enabled && cell_seal.size() == n * n ? cell_seal[i] : 0.0f;
		const float hold = liquid_capacity * (swe - liq);
		if (liq > hold) {
			const float out = liq - hold;
			salt_pack *= hold / MAX(liq, 1e-6f); // the draining brine takes its salt
			swe -= out;
			liq = hold;
			const float ponded = slush_enabled ? out * seal : 0.0f;
			slush += ponded;
			runoff += out - ponded;
		}
		if (slush > 0.0f) {
			if (!slush_enabled || seal <= 0.0f || swe <= SWE_MIN) {
				runoff += slush; // thawed ground (or none left above it): it soaks away
				slush = 0.0f;
			} else {
				const int xl = MAX(x - 1, 0), xr = MIN(x + 1, n - 1);
				const int zd = MAX(z - 1, 0), zu = MIN(z + 1, n - 1);
				const float gx = (cell_ground[z * n + xr] - cell_ground[z * n + xl]) / ((xr - xl) * cs);
				const float gz = (cell_ground[zu * n + x] - cell_ground[zd * n + x]) / ((zu - zd) * cs);
				const float g2 = gx * gx + gz * gz;
				const float kept = slush * Math::exp(-slush_drain_rate(rho, Math::sqrt(g2 / (1.0f + g2)), MAX(cs, SLUSH_SLOPE_LENGTH)) * in.dt_h);
				runoff += slush - kept;
				slush = kept;
				// Pore room (mm of water): the pack's depth less its ice and the water it holds.
				const float room = MAX(swe / MAX(rho, 30.0f) * 1000.0f - (swe - liq) / 0.917f - liq, 0.0f);
				if (slush > room) {
					runoff += slush - room;
					slush = room;
				}
				if (!melting && ts < 0.0f) {
					const float frozen = MIN(slush, melt_temperature_factor * -ts * in.dt_h);
					const float depth = swe / MAX(rho, 30.0f);
					slush -= frozen;
					swe += frozen;
					rho = MIN(swe / MAX(depth, 1e-4f), 917.0f);
				}
			}
		}
		if (cell_slush.size() == n * n) {
			cell_slush.write[i] = slush;
		}
		// A traffic glaze melts as ice does in the warm (about 0.3 mm an hour per degree, a degree-day factor of
		// 7 mm a day for ice; Hock 2003) and under rain; packing and glaze go with the snow.
		if (cell_glaze.size() == n * n && cell_glaze[i] > 0.0f) {
			cell_glaze.write[i] = swe > SWE_MIN ? MAX(cell_glaze[i] - 0.3f * MAX(ts, 0.0f) * in.dt_h - 0.1f * rain, 0.0f) : 0.0f;
		}
		if (cell_packed.size() == n * n && swe <= SWE_MIN) {
			cell_packed.write[i] = 0.0f;
		}

		// Settling (CLASS): toward a maximum density that grows with depth, higher in wet snow.
		const bool wet = melting || liq > 0.01f;
		if (swe > SWE_MIN) {
			const float depth = swe / rho;
			const float rho_max = (wet ? 700.0f : 450.0f) - 204.7f / MAX(depth, 1e-3f) * (1.0f - Math::exp(-depth / 0.673f));
			const float k = Math::exp(-SETTLE_RATE * in.dt_h);
			if (rho < rho_max) {
				rho = (rho - rho_max) * k + rho_max;
			}
			const float alb_old = wet ? ALBEDO_OLD_WET : ALBEDO_OLD_DRY;
			if (alb > alb_old) {
				alb = (alb - alb_old) * k + alb_old;
			}
		}

		if (swe <= SWE_MIN) {
			runoff += swe;
			swe = 0.0f;
			liq = 0.0f;
			rho = 100.0f;
			alb = ALBEDO_FRESH;
		}
		if (swe <= SWE_MIN || !crust_enabled) {
			crust = cycles = burial = crust_wet = 0.0f;
		}
		if (on_water && swe > SWE_MIN) {
			// Snow heavier than the ice floats pushes it under: the freeboard F = ((1000 - 917) h - SWE) / 1000
			// turns negative, water floods the snow's base up to the water level and the slush freezes into
			// snow ice (white ice; Leppaeranta 1983, as in Saloranta 2000): the flooded layer, -F deep, becomes
			// ice and its snow leaves the pack. Each update floods what the freeboard says, so a heavy load
			// settles over a few updates.
			const float cap = (1000.0f - ICE_DENSITY) * ice;
			if (swe > cap) {
				const float depth = swe / MAX(rho, 30.0f);
				const float flood = MIN((swe - cap) / 1000.0f, depth);
				const float taken = flood * MAX(rho, 30.0f);
				liq *= (swe - taken) / swe;
				swe -= taken;
				const float grown = MIN(ice + flood, ICE_MAX) - ice;
				ice += grown;
				cell_ice.write[i] = ice;
				cell_white_ice.write[i] = MIN(cell_white_ice[i] + grown, ice);
			}
		}
		if (hoar_enabled && cell_hoar.size() == n * n) {
			_update_hoar(i, pos, t, snow, rain, surface_melt, rho_before, rho, swe, in);
		}
		if (cell_depth_hoar.size() == n * n) {
			// The gradient across the pack: its surface at the air's temperature (at most 0 C) over a base held
			// near 0 C by the ground under a seasonal pack.
			const float depth = swe > SWE_MIN ? swe / MAX(rho, 30.0f) : 0.0f;
			const float gradient = depth > 0.05f ? MAX(-MIN(t, 0.0f), 0.0f) / depth : 0.0f;
			cell_depth_hoar.write[i] = depth_hoar_enabled && depth > 0.0f ? depth_hoar_step(cell_depth_hoar[i], gradient, in.dt_h, swe > SWE_MIN ? liq / swe : 0.0f) : 0.0f;
		}
		if (cell_cups.size() == n * n) {
			cell_cups.write[i] = suncups_enabled && swe > SWE_MIN ? suncup_step(cell_cups[i], sun_melt, air_melt, rain, snow / rho_fall, lap_on ? lap : 0.0f, swe / MAX(rho, 30.0f)) : 0.0f;
		}
		cell_crust.write[i] = crust;
		cell_crust_cycles.write[i] = cycles;
		// The snow over the crust settles with the pack.
		cell_crust_burial.write[i] = swe > SWE_MIN ? MIN(burial * rho_before / MAX(rho, 30.0f), swe / MAX(rho, 30.0f)) : 0.0f;
		cell_crust_wet.write[i] = crust_wet;
		if (cell_sas_x.size() == n * n && cell_sas_z.size() == n * n) {
			const Vector2 v = sastrugi_enabled && swe > SWE_MIN ? sastrugi_step(Vector2(cell_sas_x[i], cell_sas_z[i]), Vector2(), 0.0f, cell_new_depth[i] + surface_melt / MAX(rho, 30.0f), swe / MAX(rho, 30.0f), false) : Vector2();
			cell_sas_x.write[i] = v.x;
			cell_sas_z.write[i] = v.y;
		}
		if (packing) {
			// The slab settles with the pack and goes with the melt from the top.
			const float depth_now = swe / MAX(rho, 30.0f);
			const float settle = rho_before / MAX(rho, 30.0f);
			const float melt_depth = surface_melt / MAX(rho, 30.0f);
			// Melt takes the soft cover first, then the slab.
			const float cover_now = slab_cover * settle - melt_depth;
			cell_wind_slab.write[i] = swe > SWE_MIN ? CLAMP(slab * settle + MIN(cover_now, 0.0f), 0.0f, depth_now) : 0.0f;
			cell_slab_cover.write[i] = swe > SWE_MIN ? CLAMP(cover_now, 0.0f, depth_now) : 0.0f;
		}
		cell_swe.write[i] = swe;
		cell_liquid.write[i] = liq;
		if (salted) {
			cell_salt_pack.write[i] = swe > SWE_MIN ? salt_pack : 0.0f; // the snow gone, its salt is the ground's
		}
		cell_density.write[i] = CLAMP(rho, 30.0f, 917.0f);
		cell_albedo.write[i] = alb;
		cell_runoff.write[i] = in.dt_h > 0.0f ? runoff / in.dt_h : 0.0f;
		if (lap_on) {
			if (swe <= SWE_MIN) {
				lap = lap_deep = lap_cover = 0.0f; // the snow is gone: the particles are on the ground
			} else if (surface_melt > 0.0f && lap_deep > 0.0f) {
				lap_cover -= surface_melt / MAX(rho, 30.0f);
				if (lap_cover <= 0.0f) {
					lap += lap_deep;
					lap_deep = 0.0f;
					lap_cover = 0.0f;
				}
			}
			cell_lap.write[i] = lap;
			cell_lap_deep.write[i] = lap_deep;
			cell_lap_cover.write[i] = lap_cover;
		}
	}
	// The row's layers, matched to what the cells came to.
	if (layers_enabled && !driven && layer_n.size() == n * n) {
		for (int x = 0; x < n; x++) {
			const int i = z * n + x;
			const float t = in.temperature - lapse_rate * 0.001f * (cell_ground[i] - in.ref_height);
			_layers_sync(i, in.dt_h, t);
		}
	}
}

// The pack's layers (follow-up 49). As Crocus (Vionnet et al. 2012) and SNOWPACK (Bartelt and Lehning 2002) keep
// the pack as layers, each snowfall and each wind deposit is a layer of its own with its density and age; a fall
// within 6 hours of the top one of its kind joins it, and past PACK_LAYERS the two thinnest adjacent layers under
// the top merge (Crocus merges similar thin layers). Each layer settles under the weight above it by Crocus's
// viscosity, eta = eta0 (rho / c_eta) exp(a_eta (Tfus - T) + b_eta rho) (eta0 7.62237e6 kg/m/s, a_eta 0.1 /K,
// b_eta 0.023 m3/kg, c_eta 250 kg/m3), the temperature going from the air's at the top to 0 C at the base. The
// bulk fields stay the pack's total: the layers' snow and depth are matched to them each update (melt, erosion,
// slides and releases come off the top), so the layers say how the bulk is split, not a second pack.
static constexpr float HOAR_MM_PER_KG = SnowCover3D::HOAR_MM_PER_KG_LAYER; // crystal height per kg/m2 deposited (a 0.07 kg/m2 night: 5 mm)
static constexpr float HOAR_MIN = 0.5f; // mm: smaller crystals do not make a layer
static constexpr float HOAR_KEEP_HOURS = 1000.0f; // a buried layer counts as weak for about six weeks
static constexpr float LAYER_MERGE_HOURS = 6.0f;
static constexpr float LAYER_BOND_HOURS = 168.0f; // a week: new snow is settled and rounded (as the avalanche's recent snow)

float SnowCover3D::layer_compaction(float p_density, float p_stress, float p_celsius, float p_hours) {
	const float rho = CLAMP(p_density, 30.0f, 917.0f);
	const float eta = 7.62237e6f * (rho / 250.0f) * Math::exp(0.1f * MAX(-p_celsius, 0.0f) + 0.023f * rho);
	const float strain = MAX(p_stress, 0.0f) / eta * MAX(p_hours, 0.0f) * 3600.0f;
	return MIN(rho * Math::exp(MIN(strain, 0.7f)), 917.0f);
}

void SnowCover3D::_layers_alloc() {
	const int nn = resolution * resolution;
	if (!layers_enabled) {
		layer_swe.clear();
		layer_rho.clear();
		layer_age.clear();
		layer_liq.clear();
		layer_kind.clear();
		layer_n.clear();
		return;
	}
	layer_swe.resize(nn * PACK_LAYERS);
	layer_rho.resize(nn * PACK_LAYERS);
	layer_age.resize(nn * PACK_LAYERS);
	layer_liq.resize(nn * PACK_LAYERS);
	layer_kind.resize(nn * PACK_LAYERS);
	layer_n.resize(nn);
	layer_swe.fill(0.0f);
	layer_rho.fill(100.0f);
	layer_age.fill(0.0f);
	layer_liq.fill(0.0f);
	layer_kind.fill(LAYER_OLD);
	layer_n.fill(0);
}

static inline bool layer_persistent(uint8_t p_kind) {
	// Thin layers that matter by what they are, not their mass: never merged away as traces.
	return p_kind == SnowCover3D::LAYER_CRUST || p_kind == SnowCover3D::LAYER_HOAR;
}

int SnowCover3D::_layers_room(int p_i) {
	const int L = PACK_LAYERS;
	const int b = p_i * L;
	float *ls = layer_swe.ptrw() + b;
	float *lr = layer_rho.ptrw() + b;
	float *la = layer_age.ptrw() + b;
	float *ll = layer_liq.ptrw() + b;
	uint8_t *lk = layer_kind.ptrw() + b;
	int k = layer_n[p_i];
	if (k < L) {
		return k;
	}
	// Merge the two thinnest adjacent layers under the top one, sparing crusts and buried hoar while any other
	// pair is left (the merged layer keeps the lower one's kind).
	int best = -1;
	for (int pass = 0; pass < 2 && best < 0; pass++) {
		for (int j = 0; j + 1 < L - 1; j++) {
			if (pass == 0 && (layer_persistent(lk[j]) || layer_persistent(lk[j + 1]))) {
				continue;
			}
			if (best < 0 || ls[j] + ls[j + 1] < ls[best] + ls[best + 1]) {
				best = j;
			}
		}
	}
	const float s = ls[best] + ls[best + 1];
	const float d = ls[best] / lr[best] + ls[best + 1] / lr[best + 1];
	ls[best] = s;
	lr[best] = s > 0.0f ? CLAMP(s / MAX(d, 1e-6f), 30.0f, 917.0f) : lr[best];
	la[best] = MAX(la[best], la[best + 1]);
	ll[best] += ll[best + 1];
	for (int j = best + 1; j + 1 < L; j++) {
		ls[j] = ls[j + 1];
		lr[j] = lr[j + 1];
		la[j] = la[j + 1];
		ll[j] = ll[j + 1];
		lk[j] = lk[j + 1];
	}
	layer_n.write[p_i] = uint8_t(L - 1);
	return L - 1;
}

void SnowCover3D::_layers_push(int p_i, float p_swe, float p_rho, uint8_t p_kind) {
	if (!layers_enabled || driven || p_swe <= 0.0f || layer_n.size() != resolution * resolution) {
		return;
	}
	const int L = PACK_LAYERS;
	const int b = p_i * L;
	float *ls = layer_swe.ptrw() + b;
	float *lr = layer_rho.ptrw() + b;
	float *la = layer_age.ptrw() + b;
	uint8_t *lk = layer_kind.ptrw() + b;
	int k = layer_n[p_i];
	const float rho = CLAMP(p_rho, 30.0f, 917.0f);
	if (k > 0 && lk[k - 1] == p_kind && la[k - 1] < LAYER_MERGE_HOURS) {
		const float s = ls[k - 1] + p_swe;
		const float d = ls[k - 1] / lr[k - 1] + p_swe / rho;
		la[k - 1] *= ls[k - 1] / s;
		ls[k - 1] = s;
		lr[k - 1] = CLAMP(s / d, 30.0f, 917.0f);
		return;
	}
	k = _layers_room(p_i);
	ls[k] = p_swe;
	lr[k] = rho;
	la[k] = 0.0f;
	layer_liq.write[b + k] = 0.0f;
	lk[k] = p_kind;
	layer_n.write[p_i] = uint8_t(k + 1);
}

void SnowCover3D::_layers_split_top(int p_i, float p_swe, float p_rho, uint8_t p_kind) {
	if (!layers_enabled || driven || p_swe <= 0.0f || layer_n.size() != resolution * resolution || layer_n[p_i] == 0) {
		return;
	}
	const int b = p_i * PACK_LAYERS;
	int k = layer_n[p_i];
	if (layer_swe[b + k - 1] <= p_swe * 1.2f) {
		// The top layer is no thicker than the part asked for: it is that layer.
		layer_kind.write[b + k - 1] = p_kind;
		layer_rho.write[b + k - 1] = MAX(layer_rho[b + k - 1], MIN(p_rho, 917.0f));
		return;
	}
	k = _layers_room(p_i);
	const int t = b + k - 1;
	const float share = p_swe / layer_swe[t];
	layer_swe.write[t] -= p_swe;
	layer_swe.write[t + 1] = p_swe;
	layer_rho.write[t + 1] = CLAMP(p_rho, 30.0f, 917.0f);
	layer_age.write[t + 1] = layer_age[t];
	layer_liq.write[t + 1] = layer_liq[t] * share;
	layer_liq.write[t] -= layer_liq[t + 1];
	layer_kind.write[t + 1] = p_kind;
	layer_n.write[p_i] = uint8_t(k + 1);
}

void SnowCover3D::_layers_bury_hoar(int p_i) {
	// The crystals (HOAR_MM_PER_KG mm per kg/m2 deposited) keep their mass as a layer; their size is read back
	// from it. Split off the top of the pack, so the layers' mass stays the bulk's.
	if (!layers_enabled || driven || cell_hoar.size() != resolution * resolution || cell_hoar[p_i] < HOAR_MIN) {
		return;
	}
	_layers_split_top(p_i, cell_hoar[p_i] / HOAR_MM_PER_KG, 30.0f, LAYER_HOAR);
}

void SnowCover3D::_layers_take(int p_i, float p_swe) {
	if (!layers_enabled || driven || p_swe <= 0.0f || layer_n.size() != resolution * resolution) {
		return;
	}
	const int b = p_i * PACK_LAYERS;
	float *ls = layer_swe.ptrw() + b;
	float *ll = layer_liq.ptrw() + b;
	int k = layer_n[p_i];
	float left = p_swe;
	while (k > 0 && left > 0.0f) {
		const float t = MIN(ls[k - 1], left);
		ll[k - 1] *= ls[k - 1] > 1e-6f ? (ls[k - 1] - t) / ls[k - 1] : 0.0f;
		ls[k - 1] -= t;
		left -= t;
		if (ls[k - 1] <= 1e-4f) {
			k--;
		}
	}
	layer_n.write[p_i] = uint8_t(k);
}

// Water in the layers (round 8, follow-up 51). Each layer holds liquid_capacity of its ice as liquid (the bulk's
// irreducible water); what is over that percolates down within the update (gravity flow fronts move some tens of
// centimetres to metres an hour through ripe snow; Colbeck 1972). Where fine grains lie over coarse ones (new or wind
// snow over old rounded snow, buried hoar or depth hoar) the water stops at the interface until the fine layer's
// suction is overcome: a capillary barrier, which ponds water in the few centimetres above it (Waldner, Schneebeli
// et al. 2004; Avanzi et al. 2016, 5-15% by volume); an ice crust under a layer ponds more and lets it through only
// by flow fingers. That ponding is where wet slab avalanches fail (Mitterer et al. 2011; Wever et al. 2016).
static constexpr float PONDING_ZONE = 0.03f; // m above the interface that fills
static constexpr float PONDING_CAPILLARY = 0.08f; // its liquid by volume over a capillary barrier
static constexpr float PONDING_ICE = 0.15f; // and over an ice crust
static constexpr float CRUST_LAYER_DENSITY = 450.0f; // kg/m3, a melt-freeze crust (Fierz et al. 2009: MFcr)

float SnowCover3D::layer_barrier(int p_above, int p_below) {
	const bool fine = p_above == LAYER_NEW || p_above == LAYER_WIND;
	if (p_below == LAYER_CRUST) {
		return PONDING_ICE;
	}
	if (fine && (p_below == LAYER_OLD || p_below == LAYER_HOAR || p_below == LAYER_DEPTH_HOAR)) {
		return PONDING_CAPILLARY;
	}
	return 0.0f;
}

void SnowCover3D::_layers_water(int p_i, float p_dt_h, float p_air) {
	const int b = p_i * PACK_LAYERS;
	float *ls = layer_swe.ptrw() + b;
	float *lr = layer_rho.ptrw() + b;
	float *ll = layer_liq.ptrw() + b;
	const uint8_t *lk = layer_kind.ptr() + b;
	const int k = layer_n[p_i];
	const float cap = MAX(liquid_capacity, 0.0f);
	// What each layer holds: its share, more just over a barrier, no more than its pores.
	float hold[PACK_LAYERS];
	for (int j = 0; j < k; j++) {
		const float ice = MAX(ls[j] - ll[j], 0.0f);
		const float depth = ls[j] / MAX(lr[j], 30.0f);
		const float room = MAX(depth * 1000.0f - ice / 0.917f, 0.0f); // mm of pore space
		float h = cap * ice;
		if (j > 0) {
			const float pond = layer_barrier(lk[j], lk[j - 1]);
			if (pond > 0.0f) {
				h += pond * MIN(depth, PONDING_ZONE) * 1000.0f;
			}
		}
		hold[j] = MIN(h, room);
	}
	// The bulk's liquid is the pack's: rain and melt come in at the top; in the cold the water refreezes from the
	// top down, in the warm it drains out from the bottom up.
	float have = 0.0f;
	for (int j = 0; j < k; j++) {
		have += ll[j];
	}
	const float want = cell_liquid[p_i];
	if (want > have) {
		ll[k - 1] += want - have;
	} else if (want < have) {
		float left = have - want;
		for (int s = 0; s < k && left > 0.0f; s++) {
			const int j = p_air < 0.0f ? k - 1 - s : s;
			const float t = MIN(ll[j], left);
			ll[j] -= t;
			left -= t;
		}
	}
	// Percolation, top down; the bottom layer keeps what reaches it (the bulk drains the pack).
	for (int j = k - 1; j > 0; j--) {
		if (ll[j] > hold[j]) {
			ll[j - 1] += ll[j] - hold[j];
			ll[j] = hold[j];
		}
	}
	for (int j = 0; j < k; j++) {
		ll[j] = MIN(MAX(ll[j], 0.0f), ls[j]);
	}
	(void)p_dt_h;
}

float SnowCover3D::pub_wettest_layer(int p_i, float &r_depth) const {
	r_depth = 0.0f;
	const int nn = resolution * resolution;
	if (pub.layer_n.size() != nn || pub.layer_liq.size() != nn * PACK_LAYERS || pub.layer_n[p_i] == 0) {
		return 0.0f;
	}
	const int b = p_i * PACK_LAYERS;
	const float cap = MAX(liquid_capacity, 0.005f);
	float best = 0.0f, depth = 0.0f;
	for (int j = pub.layer_n[p_i] - 1; j >= 0; j--) {
		depth += pub.layer_swe[b + j] / MAX(pub.layer_rho[b + j], 30.0f);
		const float ice = pub.layer_swe[b + j] - pub.layer_liq[b + j];
		const float index = ice > 0.5f ? pub.layer_liq[b + j] / (cap * ice) : 0.0f;
		if (index > best) {
			best = index;
			r_depth = depth;
		}
	}
	return best;
}

void SnowCover3D::_layers_sync(int p_i, float p_dt_h, float p_air) {
	if (!layers_enabled || layer_n.size() != resolution * resolution) {
		return;
	}
	const float swe = cell_swe[p_i];
	if (swe <= SWE_MIN) {
		layer_n.write[p_i] = 0;
		return;
	}
	const int L = PACK_LAYERS;
	const int b = p_i * L;
	float *ls = layer_swe.ptrw() + b;
	float *lr = layer_rho.ptrw() + b;
	float *la = layer_age.ptrw() + b;
	float *ll = layer_liq.ptrw() + b;
	uint8_t *lk = layer_kind.ptrw() + b;
	int k = layer_n[p_i];
	// The mass: what the bulk lost (melt, erosion, slides, releases) comes off the top; what it gained past the
	// layers (refrozen rain and slush) goes into the top one.
	float total = 0.0f;
	for (int j = 0; j < k; j++) {
		total += ls[j];
	}
	if (total > swe + 1e-4f) {
		_layers_take(p_i, total - swe);
		k = layer_n[p_i];
	} else if (total < swe - 1e-4f) {
		if (k == 0) {
			ls[0] = swe;
			lr[0] = cell_density[p_i];
			la[0] = LAYER_BOND_HOURS;
			ll[0] = 0.0f;
			lk[0] = LAYER_OLD;
			k = 1;
			layer_n.write[p_i] = 1;
		} else {
			const float add = swe - total;
			const float d = ls[k - 1] / lr[k - 1] + add / MAX(lr[k - 1], 30.0f);
			ls[k - 1] += add;
			lr[k - 1] = CLAMP(ls[k - 1] / MAX(d, 1e-6f), 30.0f, 917.0f);
		}
	}
	if (k == 0) {
		return;
	}
	// What the layers are. The top crust (cell_crust, at the surface) is the top's crust-thick part; a top crust
	// the bulk no longer has (it melted or broke up) is old snow again. Buried crusts stay crusts. The base is depth
	// hoar while the bulk's share says so (follow-up 47).
	const int n = resolution;
	if (crust_enabled && cell_crust.size() == n * n) {
		const bool top_crust = cell_crust[p_i] > 0.001f && cell_crust_burial[p_i] < 0.005f;
		if (top_crust && lk[k - 1] != LAYER_CRUST) {
			const float rho_c = MAX(CRUST_LAYER_DENSITY, lr[k - 1]);
			_layers_split_top(p_i, cell_crust[p_i] * rho_c, rho_c, LAYER_CRUST);
			k = layer_n[p_i];
		} else if (!top_crust && lk[k - 1] == LAYER_CRUST && (cell_crust[p_i] <= 0.0f || cell_crust_wet[p_i] > 0.5f)) {
			lk[k - 1] = LAYER_OLD;
		}
	}
	if (cell_depth_hoar.size() == n * n && lk[0] != LAYER_CRUST) {
		if (cell_depth_hoar[p_i] > 0.5f) {
			lk[0] = LAYER_DEPTH_HOAR;
		} else if (lk[0] == LAYER_DEPTH_HOAR && cell_depth_hoar[p_i] < 0.2f) {
			lk[0] = LAYER_OLD;
		}
	}
	_layers_water(p_i, p_dt_h, p_air);
	// Age and settle, from the top down: each layer under the weight above it and half its own. Wet new and wind
	// snow rounds within the day (wet metamorphism; Brun 1989); buried hoar wet through, or past HOAR_KEEP_HOURS,
	// is gone as a weak layer.
	const float cap = MAX(liquid_capacity, 0.005f);
	float above = 0.0f;
	float depth = 0.0f;
	for (int j = 0; j < k; j++) {
		depth += ls[j] / lr[j];
	}
	float z = 0.0f; // from the top
	for (int j = k - 1; j >= 0; j--) {
		la[j] += p_dt_h;
		const bool wet = ll[j] > 0.5f * cap * MAX(ls[j] - ll[j], 0.0f) && ll[j] > 0.01f;
		if ((lk[j] == LAYER_NEW && (la[j] > LAYER_BOND_HOURS || wet)) || (lk[j] == LAYER_WIND && wet) ||
				(lk[j] == LAYER_HOAR && (wet || la[j] > HOAR_KEEP_HOURS))) {
			lk[j] = LAYER_OLD;
		}
		const float dj = ls[j] / lr[j];
		const float mid = depth > 1e-4f ? (z + 0.5f * dj) / depth : 0.0f;
		const float celsius = MIN(p_air, 0.0f) * (1.0f - mid);
		if (lk[j] != LAYER_HOAR) { // the crystals' layer is read by its mass, not its depth
			lr[j] = layer_compaction(lr[j], 9.81f * (above + 0.5f * ls[j]), celsius, p_dt_h);
		}
		above += ls[j];
		z += dj;
	}
	// Traces (a layer of under half a millimetre of water) join the one under them, as does a layer of the same kind
	// and age (the wind's packing splits the top off in steps); crusts and buried hoar stay, thin as they are.
	int w = 0;
	for (int j = 0; j < k; j++) {
		if (w > 0 && !layer_persistent(lk[j]) && (ls[j] < 0.5f || (lk[j] == lk[w - 1] && Math::abs(la[j] - la[w - 1]) < LAYER_MERGE_HOURS))) {
			const float s = ls[w - 1] + ls[j];
			const float dd = ls[w - 1] / lr[w - 1] + ls[j] / lr[j];
			ls[w - 1] = s;
			lr[w - 1] = CLAMP(s / MAX(dd, 1e-6f), 30.0f, 917.0f);
			ll[w - 1] += ll[j];
			continue;
		}
		ls[w] = ls[j];
		lr[w] = lr[j];
		la[w] = la[j];
		ll[w] = ll[j];
		lk[w] = lk[j];
		w++;
	}
	k = w;
	layer_n.write[p_i] = uint8_t(k);
	// The depth: the bulk's settling (CLASS, with the wind's packing and the melt) is the pack's. The difference
	// goes to the layers by their pore space (d (1 - rho / rho_ice)): light new snow takes most of it, dense old
	// snow and ice little.
	float d = 0.0f, pore = 0.0f;
	for (int j = 0; j < k; j++) {
		const float dj = ls[j] / lr[j];
		d += dj;
		pore += dj * (1.0f - lr[j] / 917.0f);
	}
	const float target = swe / MAX(cell_density[p_i], 30.0f);
	if (d > 1e-6f && target > 1e-6f && pore > 1e-6f) {
		const float excess = d - target;
		for (int j = 0; j < k; j++) {
			const float dj = ls[j] / lr[j];
			const float dn = MAX(dj - excess * dj * (1.0f - lr[j] / 917.0f) / pore, ls[j] / 917.0f);
			lr[j] = CLAMP(ls[j] / MAX(dn, 1e-6f), 30.0f, 917.0f);
		}
	}
}

float SnowCover3D::_top_density(int p_i, float p_depth) const {
	const int nn = resolution * resolution;
	const float bulk = MAX(cell_density[p_i], 30.0f);
	if (!layers_enabled || layer_n.size() != nn || layer_swe.size() != nn * PACK_LAYERS || p_depth <= 0.0f || layer_n[p_i] == 0) {
		return bulk;
	}
	const int b = p_i * PACK_LAYERS;
	float mass = 0.0f, d = 0.0f;
	for (int j = layer_n[p_i] - 1; j >= 0 && d < p_depth; j--) {
		const float r = MAX(layer_rho[b + j], 30.0f);
		const float dj = MIN(layer_swe[b + j] / r, p_depth - d);
		mass += dj * r;
		d += dj;
	}
	return d > 1e-4f ? mass / d : bulk;
}

float SnowCover3D::pub_top_density(int p_i, float p_depth) const {
	const int nn = resolution * resolution;
	const float bulk = pub.density.size() == nn ? MAX(pub.density[p_i], 30.0f) : 100.0f;
	if (pub.layer_n.size() != nn || pub.layer_swe.size() != nn * PACK_LAYERS || p_depth <= 0.0f) {
		return bulk;
	}
	const int b = p_i * PACK_LAYERS;
	float mass = 0.0f, d = 0.0f;
	for (int j = pub.layer_n[p_i] - 1; j >= 0 && d < p_depth; j--) {
		const float r = MAX(pub.layer_rho[b + j], 30.0f);
		const float dj = MIN(pub.layer_swe[b + j] / r, p_depth - d);
		mass += dj * r;
		d += dj;
	}
	return d > 1e-4f ? mass / d : bulk;
}

Array SnowCover3D::get_layers_at(const Vector3 &p_position) const {
	Array out;
	const int n = resolution;
	if (!origin_valid || pub.layer_n.size() != n * n || pub.layer_swe.size() != n * n * PACK_LAYERS) {
		return out;
	}
	const float cs = _cell_size();
	const int x = CLAMP(int(Math::floor(p_position.x / cs)) - origin_cell.x, 0, n - 1);
	const int z = CLAMP(int(Math::floor(p_position.z / cs)) - origin_cell.y, 0, n - 1);
	const int i = z * n + x;
	static const char *KINDS[6] = { "new", "wind", "old", "crust", "hoar", "depth_hoar" };
	for (int j = 0; j < pub.layer_n[i]; j++) {
		const int k = i * PACK_LAYERS + j;
		Dictionary l;
		l["swe"] = pub.layer_swe[k];
		l["density"] = pub.layer_rho[k];
		l["depth"] = pub.layer_swe[k] / MAX(pub.layer_rho[k], 30.0f);
		l["age"] = pub.layer_age[k];
		l["kind"] = String(KINDS[MIN(int(pub.layer_kind[k]), 5)]);
		l["liquid"] = pub.layer_liq.size() == pub.layer_swe.size() ? pub.layer_liq[k] : 0.0f;
		if (pub.layer_kind[k] == LAYER_HOAR) {
			l["crystal_mm"] = pub.layer_swe[k] * HOAR_MM_PER_KG;
		}
		out.push_back(l);
	}
	return out;
}

// The bands a cut wall shows (follow-up 53), from the layers of one cell (base first): neighbours of one kind and
// wetness merge (so there are never more than PACK_LAYERS). Depths are scaled to the pack's p_depth. Each entry,
// top band first: (kind + 1 + 8 if wet) * 256 + the band's bottom under the surface in whole centimetres (to 2.55 m).
Vector<int> SnowCover3D::strata_bands(const float *p_swe, const float *p_rho, const float *p_liq, const uint8_t *p_kind, int p_count, float p_depth, float p_liquid_capacity) {
	Vector<int> out;
	if (p_count <= 0 || p_depth <= 0.001f) {
		return out;
	}
	LocalVector<int> codes; // top first
	LocalVector<float> thick;
	float total = 0.0f;
	for (int j = p_count - 1; j >= 0; j--) {
		const float t = p_swe[j] / MAX(p_rho[j], 30.0f);
		if (t <= 0.0f) {
			continue;
		}
		const bool wet = p_liq[j] > 0.3f * p_liquid_capacity * p_swe[j];
		const int code = MIN(int(p_kind[j]), 7) + 1 + (wet ? 8 : 0);
		if (!codes.is_empty() && codes[codes.size() - 1] == code) {
			thick[thick.size() - 1] += t;
		} else {
			codes.push_back(code);
			thick.push_back(t);
		}
		total += t;
	}
	if (codes.is_empty() || total <= 0.0f) {
		return out;
	}
	const float scale = p_depth / total;
	float bottom = 0.0f;
	for (uint32_t b = 0; b < codes.size(); b++) {
		bottom += thick[b] * scale;
		out.push_back(MIN(codes[b], 15) * 256 + CLAMP(int(Math::round(bottom * 100.0f)), 0, 255));
	}
	return out;
}

void SnowCover3D::set_layers_enabled(bool p_enabled) {
	_finish_background(true);
	layers_enabled = p_enabled;
	_layers_alloc();
	if (world) {
		world->set_layers_enabled(p_enabled);
	}
}

// Crusts (docs/ideas/snow.md, follow-up 3d). A melt-freeze crust is the surface layer that was wet and
// froze again: "at most a few centimetres thick", its "strength increases with number of melt-freeze
// cycles" (Fierz et al. 2009, MFcr); rain on snow freezes to a thin glaze the same way (IFrc). Only the
// top crust is kept: when the surface over a buried one gets wet, a new crust starts there.
static constexpr float CRUST_MAX = 0.05f; // m
static constexpr float CRUST_GAIN = 250.0f; // kg/m3: the density refrozen water adds to the layer it freezes in
static constexpr float CRUST_SURFACE = 0.01f; // m of snow over a crust that still counts as its surface

void SnowCover3D::_update_crust(float &r_crust, float &r_cycles, float &r_burial, float &r_wet, float p_rain, float p_melt_depth, float p_refrozen, float p_liquid_before, float p_liquid) const {
	// Melt takes the snow over the crust first.
	if (r_crust > 0.0f && p_melt_depth > 0.0f) {
		r_burial = MAX(r_burial - p_melt_depth, 0.0f);
	}
	if (p_melt_depth > 0.0f || p_rain > 0.0f) {
		if (r_crust > 0.0f && r_burial > CRUST_SURFACE) {
			// The wet layer is above the old crust: it starts the new top crust.
			r_crust = 0.0f;
			r_cycles = 0.0f;
			r_burial = 0.0f;
		}
		r_wet = 1.0f;
	}
	if (r_wet > 0.0f && p_refrozen > 0.0f) {
		r_crust = MIN(r_crust + p_refrozen / CRUST_GAIN, CRUST_MAX);
		if (p_liquid <= 1e-4f && p_liquid_before > 0.0f) {
			// Frozen through: one more melt-freeze cycle.
			r_cycles += 1.0f;
			r_wet = 0.0f;
		}
	}
}

// Surface hoar (docs/ideas/snow.md, round 5). On a clear night the snow radiates about 70 W/m2 more than the sky
// gives back (less under cloud, by 1 - 0.95 N^2: an overcast sky gives back nearly all); the air's heat (bulk transfer, rho cp C U) and the pack's
// (about 5 W/m2 K) hold its surface that much below the air, and the shortwave it absorbs by day warms it back. Below
// the air's frost point vapour deposits on it, E = rho C_E U (q_air - q_sat,ice(Ts)) (bulk transfer, C_E 0.0015 over
// snow); the crystals grow as feathery plates and cups, a few millimetres a night, up to centimetres over clear calm
// nights in a humid layer (Hachikubo and Akitaya 1997; Colbeck 1988). A light wind feeds them; above about 5 m/s at
// 10 m it breaks them, sun or rain that wets the surface destroys them, a drifting wind sweeps them off.
static constexpr float HOAR_LONGWAVE = 70.0f; // W/m2, clear-sky net loss of a snow surface at night
static constexpr float HOAR_PACK = 5.0f; // W/m2 K, the pack's heat toward its cooling surface
static constexpr float HOAR_CE = 0.0015f; // bulk transfer coefficient over snow (heat and vapour)
static constexpr float HOAR_MAX = 30.0f; // mm
static constexpr float HOAR_BREAK_WIND = 5.0f; // m/s at 10 m

static float _vapour_ice(float p_celsius) { // Pa, over ice (Buck 1981)
	return 611.15f * Math::exp(22.452f * p_celsius / (272.55f + p_celsius));
}
static float _vapour_water(float p_celsius) { // Pa, over water (Buck 1981)
	return 611.21f * Math::exp(17.502f * p_celsius / (240.97f + p_celsius));
}

float SnowCover3D::hoar_deposition(float p_air_celsius, float p_humidity, float p_wind, float p_cloud, float p_shortwave, float &r_surface_celsius) {
	const float u = MAX(0.75f * p_wind, 0.3f); // at 2 m, still air never quite still
	const float h_air = 1.29f * 1005.0f * HOAR_CE * u; // W/m2 K
	const float n = CLAMP(p_cloud, 0.0f, 1.0f);
	const float deficit = MAX((HOAR_LONGWAVE * (1.0f - 0.95f * n * n) - MAX(p_shortwave, 0.0f)) / (h_air + HOAR_PACK), 0.0f);
	r_surface_celsius = MIN(p_air_celsius - deficit, 0.0f);
	const float e_air = CLAMP(p_humidity, 0.0f, 1.0f) * _vapour_water(p_air_celsius);
	const float e_surf = _vapour_ice(r_surface_celsius);
	return 1.29f * HOAR_CE * u * 0.622f * (e_air - e_surf) / 101325.0f * 3600.0f; // kg/m2/h
}

float SnowCover3D::hoar_deposition_rate(float p_air_celsius, float p_humidity, float p_wind, float p_cloud, float p_shortwave) {
	float ts;
	return hoar_deposition(p_air_celsius, p_humidity, p_wind, p_cloud, p_shortwave, ts);
}

float SnowCover3D::buried_hoar_strength(float p_size_mm, float p_age_hours) {
	// Jamieson and Schweizer (2000): buried surface hoar holds about 0.3-0.8 kPa when buried, less the larger its
	// crystals, and gains strength slowly, some tens of Pa a day, staying weak for weeks.
	return (400.0f + 35.0f * MAX(p_age_hours, 0.0f) / 24.0f) * Math::sqrt(5.0f / MAX(p_size_mm, 1.0f));
}

void SnowCover3D::_clear_hoar(int p_i) {
	if (cell_hoar.size() > p_i) {
		cell_hoar.write[p_i] = 0.0f;
		cell_hoar_layer.write[p_i] = 0.0f;
		cell_hoar_burial.write[p_i] = 0.0f;
		cell_hoar_age.write[p_i] = 0.0f;
	}
}

// Suncups (docs/ideas/snow.md, follow-up 41): melt from direct sun deepens hollows that catch the light their
// walls reflect (Betterton 2000), growing from the surface's roughness to a cellular relief that is full grown
// after about 5 days of full sun (Mitchell and Tiedje 2010); melt by the air heats the surface evenly and
// smooths them, rain washes them down and new snow fills them. Thin dirt gathers on the ridges and deepens the
// hollows; a layer over about 3 mm insulates and stops them (Rhodes, Armstrong and Warren 1987; Driedger).
static constexpr float CUPS_MAX = 0.3f; // m, ridge to hollow, full grown in clean snow
static constexpr float CUPS_SEED = 0.01f; // m, the roughness they grow from
static constexpr float CUPS_RATE = 0.045f; // per mm of sun melt: 0.01 m to 90% in about 125 mm (5 sunny days)
static constexpr float CUPS_AIR_SMOOTH = 40.0f; // mm of air melt that smooths the relief by e
static constexpr float CUPS_RAIN_SMOOTH = 10.0f; // mm of rain that smooths it by e

float SnowCover3D::suncup_step(float p_relief, float p_sun_melt, float p_air_melt, float p_rain, float p_new_depth, float p_dirt, float p_depth) {
	float a = MAX(p_relief, 0.0f);
	const float deepen = 1.0f + 0.5f * Math::smoothstep(1.0e3f, 1.0e5f, p_dirt);
	const float block = 1.0f - Math::smoothstep(1.0e6f, 4.0e6f, p_dirt);
	const float top = MIN(CUPS_MAX * deepen * block, 0.5f * MAX(p_depth, 0.0f));
	if (p_sun_melt > 0.0f && top > 0.0f) {
		// In steps of at most 2 mm of melt, so a long update grows as a run of short ones would.
		const int steps = CLAMP(int(Math::ceil(p_sun_melt / 2.0f)), 1, 64);
		const float dm = p_sun_melt / steps;
		for (int k = 0; k < steps; k++) {
			a += CUPS_RATE * dm * (a + CUPS_SEED) * MAX(1.0f - a / top, 0.0f) * top / CUPS_MAX;
		}
	}
	a *= Math::exp(-MAX(p_air_melt, 0.0f) / CUPS_AIR_SMOOTH - MAX(p_rain, 0.0f) / CUPS_RAIN_SMOOTH);
	a = MAX(a - MAX(p_new_depth, 0.0f), 0.0f);
	return CLAMP(a, 0.0f, MAX(top, 0.0f));
}

float SnowCover3D::get_suncup_relief_at(Vector3 p_position) const {
	if (_in_world_only(p_position)) {
		return world->get_suncup_relief_at(p_position);
	}
	return pub.cups.is_empty() ? 0.0f : _sample(pub.cups, Vector2(p_position.x, p_position.z));
}

void SnowCover3D::_update_hoar(int p_i, const Vector3 &p_pos, float p_air, float p_snow, float p_rain, float p_melt, float p_rho_before, float p_rho, float p_swe, const Inputs &p_in) {
	float hoar = cell_hoar[p_i];
	float layer = cell_hoar_layer[p_i];
	float burial = cell_hoar_burial[p_i];
	float age = cell_hoar_age[p_i];
	if (p_swe <= SWE_MIN) {
		_clear_hoar(p_i);
		return;
	}
	// New snow buries the crystals: the newest layer, unless an older one lies shallow and has larger crystals.
	if (p_snow > 0.0f) {
		const float d = p_snow / new_snow_density(p_air);
		if (hoar >= HOAR_MIN && (layer <= 0.0f || hoar >= 0.5f * layer || burial > 1.0f)) {
			layer = hoar;
			burial = d;
			age = 0.0f;
		} else if (layer > 0.0f) {
			burial += d;
		}
		hoar = 0.0f;
	}
	if (layer > 0.0f) {
		age += p_in.dt_h;
		burial *= p_rho_before / MAX(p_rho, 30.0f); // settles with the pack
		burial -= p_melt / MAX(p_rho, 30.0f); // melt works down to it; wet through, it is gone
		if (burial <= 0.0f || age > HOAR_KEEP_HOURS) {
			layer = burial = age = 0.0f;
		}
	}
	// The surface: growth, sublimation, breaking.
	const float u10 = last_wind.length();
	if (p_rain > 0.0f || p_melt > 0.0f || p_air > 0.5f || p_in.wind_factor > 0.0f) {
		hoar = 0.0f; // wet, or swept off by drifting snow
	} else {
		float cloud = 0.0f;
		float sw = 0.0f;
		if (p_in.wx.valid) {
			cloud = p_in.wx.cloud_cover_at(p_pos);
		}
		if (p_in.direct_normal > 0.0f && p_in.sun_dir.y > 0.0f) {
			const float vis = p_in.wx.valid ? (p_in.wx.has_sun ? p_in.wx.sun_visibility_at(p_pos) : 1.0f - 0.75f * Math::pow(cloud, 3.4f)) : 1.0f;
			sw = (1.0f - cell_albedo[p_i]) * p_in.direct_normal * p_in.sun_dir.y * (vis + DIFFUSE_SHARE);
		}
		float ts;
		const float e = hoar_deposition(p_air, p_in.humidity, u10, cloud, sw, ts);
		if (e > 0.0f && u10 < HOAR_BREAK_WIND) {
			hoar += e * p_in.dt_h * HOAR_MM_PER_KG;
		} else if (e < 0.0f) {
			hoar += e * p_in.dt_h * HOAR_MM_PER_KG; // sublimates in the dry or the sun
		}
		if (u10 >= HOAR_BREAK_WIND) {
			hoar *= Math::exp(-p_in.dt_h / 0.5f);
		}
		hoar = CLAMP(hoar, 0.0f, HOAR_MAX);
	}
	cell_hoar.write[p_i] = hoar;
	cell_hoar_layer.write[p_i] = layer;
	cell_hoar_burial.write[p_i] = burial;
	cell_hoar_age.write[p_i] = age;
}

float SnowCover3D::crust_strength(float p_cycles, float p_thickness) {
	// The bearing pressure of a crust by its hand hardness (Fierz et al. 2009, Table 1.4): a layer of
	// hardness h lets the h-th test object in at the 10-15 N of the hand test but not the one before.
	// Taking 12 N over each object's face (fist 100, four fingers 20, one finger 2, pencil 0.5,
	// knife 0.1 cm2) gives bands whose geometric centres are F 0.5, 4F 2.7, 1F 19, P 120, K 540 kPa;
	// ice (I) is held at 1.2 MPa. A first-cycle crust is 1F, each further cycle half a step harder.
	// A crust thinner than 2 cm bears in proportion to its thickness (punching shear grows with it).
	if (p_cycles <= 0.0f || p_thickness <= 0.0f) {
		return 0.0f;
	}
	static const float BAND[6] = { 0.5f, 2.7f, 19.0f, 120.0f, 540.0f, 1200.0f };
	const float h = CLAMP(2.5f + 0.5f * p_cycles, 1.0f, 6.0f) - 1.0f;
	const int k = MIN(int(h), 4);
	const float f = h - k;
	const float sigma = Math::exp(Math::log(BAND[k]) * (1.0f - f) + Math::log(BAND[k + 1]) * f);
	return sigma * MIN(p_thickness / 0.02f, 1.0f);
}

void SnowCover3D::_slide() {
	// SnowSlide (Bernhardt and Schulz 2010): from the highest snow surface down, a cell steeper than
	// slide_angle holding more than its holding depth passes the excess to its lower neighbours, in
	// proportion to the drop to each. Mass is conserved; snow leaving the square is kept at the edge.
	const int n = resolution;
	const int nn = n * n;
	const float cs = _cell_size();
	LocalVector<float> surface;
	surface.resize(nn);
	struct Order {
		float z;
		int i;
		bool operator<(const Order &p_o) const { return z > p_o.z; }
	};
	LocalVector<Order> order;
	order.reserve(nn);
	if (steep_dirty) {
		_update_steep();
	}
	for (int i = 0; i < nn; i++) {
		const float d = cell_swe[i] > SWE_MIN ? cell_swe[i] / cell_density[i] : 0.0f;
		surface[i] = cell_ground[i] + d;
		if (cell_steep[i] && cell_swe[i] > slide_min_holding) {
			order.push_back({ surface[i], i });
		}
	}
	if (order.is_empty()) {
		return;
	}
	order.sort();
	const float tan_min = Math::tan(Math::deg_to_rad(slide_angle));
	static const int DX[8] = { -1, 0, 1, -1, 1, -1, 0, 1 };
	static const int DZ[8] = { -1, -1, -1, 0, 0, 1, 1, 1 };
	float *swe = cell_swe.ptrw();
	float *liq = cell_liquid.ptrw();
	float *rho = cell_density.ptrw();
	for (const Order &o : order) {
		const int i = o.i;
		const int x = i % n;
		const int z = i / n;
		// What a cell holds follows the slope of the ground under it; the snow goes down the snow surface.
		// (Bernhardt and Schulz take both from the snow surface on 30 m cells; on metre cells a deposit is
		// as deep as the cell is wide and would hide the slope it lies on.)
		float drops[8];
		float total = 0.0f;
		float steepest = 0.0f;
		for (int k = 0; k < 8; k++) {
			const int nx = x + DX[k];
			const int nz = z + DZ[k];
			drops[k] = 0.0f;
			if (nx < 0 || nz < 0 || nx >= n || nz >= n) {
				continue;
			}
			const int j = nz * n + nx;
			const float dist = cs * ((DX[k] != 0 && DZ[k] != 0) ? Math::SQRT2 : 1.0f);
			const float ground_drop = (cell_ground[i] - cell_ground[j]) / dist;
			steepest = MAX(steepest, ground_drop);
			// Only downhill: a deposit piled higher than this cell does not send snow back up the slope.
			const float drop = (surface[i] - surface[j]) / dist;
			if (ground_drop > 0.0f && drop > 0.0f) {
				drops[k] = drop;
				total += drop;
			}
		}
		if (steepest < tan_min) {
			continue;
		}
		if (total <= 0.0f) {
			// Buried below by its own deposit: the snow still goes down the ground's slope and piles there.
			for (int k = 0; k < 8; k++) {
				const int nx = x + DX[k];
				const int nz = z + DZ[k];
				if (nx < 0 || nz < 0 || nx >= n || nz >= n) {
					continue;
				}
				const float dist = cs * ((DX[k] != 0 && DZ[k] != 0) ? Math::SQRT2 : 1.0f);
				drops[k] = MAX((cell_ground[i] - cell_ground[nz * n + nx]) / dist, 0.0f);
				total += drops[k];
			}
			if (total <= 0.0f) {
				continue;
			}
		}
		const float slope = Math::rad_to_deg(Math::atan(steepest));
		const float keep = holding_depth(slope, slide_min_holding, slide_holding_decay);
		if (swe[i] <= keep) {
			continue;
		}
		const float out = swe[i] - keep;
		const float liq_share = liq[i] / swe[i];
		const float r = rho[i];
		for (int k = 0; k < 8; k++) {
			if (drops[k] <= 0.0f) {
				continue;
			}
			const int j = (z + DZ[k]) * n + (x + DX[k]);
			const float part = out * drops[k] / total;
			const float depth_j = swe[j] > SWE_MIN ? swe[j] / rho[j] : 0.0f;
			swe[j] = (swe[j] > SWE_MIN ? swe[j] : 0.0f) + part;
			liq[j] += part * liq_share;
			rho[j] = swe[j] / MAX(depth_j + part / r, 1e-6f);
			surface[j] += part / r;
			cell_albedo.write[j] = MIN(cell_albedo[j], cell_albedo[i]);
			if (cell_crust[j] > 0.0f) {
				cell_crust_burial.write[j] += part / r;
			}
		}
		swe[i] = keep;
		liq[i] *= keep / (keep + out);
		cell_crust_burial.write[i] = MIN(cell_crust_burial[i], keep / r);
		surface[i] = cell_ground[i] + keep / r;
	}
}

// What the cells read from other nodes that cannot be read from a worker: the canopy closure (the weather
// map's shelter) and the terrain climate's precipitation. Main thread, for rows [p_from, p_to). Both change
// rarely: each update refreshes a sixteenth of the rows, and the cells sampled anew (a shift) at once.
void SnowCover3D::_refresh_cell_inputs(int p_from_row, int p_to_row, const Vector<uint8_t> *p_only) {
	const int n = resolution;
	const int nn = n * n;
	const WorldWeather *wm = owner || !canopy_enabled || !WorldContracts::weather()->has_provider() ? nullptr : WorldContracts::weather();
	const TerrainDataMap3D *cm = get_climate_map_node();
	const bool canopy = cell_canopy.size() == nn;
	if (cm) {
		if (cell_climate_precip.size() != nn) {
			cell_climate_precip.resize(nn);
			cell_climate_precip.fill(0.0f);
			p_from_row = 0;
			p_to_row = n;
			p_only = nullptr;
		}
	} else {
		cell_climate_precip.clear();
	}
	float *c = canopy ? cell_canopy.ptrw() : nullptr;
	float *cp = cm ? cell_climate_precip.ptrw() : nullptr;
	// Water: the data map's surface where it is deeper than a puddle. The pack over it lies at the water
	// surface (on ice), so the cell's ground is lifted to it; materials on the bed below read it as covered
	// (the catch map) and stay bare.
	const TerrainDataMap3D *tm = _terrain_map();
	float *wl = cell_water.size() == nn ? cell_water.ptrw() : nullptr;
	float *g = cell_ground.size() == nn ? cell_ground.ptrw() : nullptr;
	for (int z = p_from_row; z < p_to_row; z++) {
		for (int x = 0; x < n; x++) {
			const int i = z * n + x;
			if (p_only && !(*p_only)[i]) {
				continue;
			}
			const Vector3 pos = _cell_center(x, z);
			if (c) {
				c[i] = wm ? CLAMP(wm->canopy_closure_at(pos), 0.0f, 1.0f) : 0.0f;
			}
			if (cp) {
				// The weather's precipitation already carries its own orographic factor where it applies one
				// (WorldWeather.orographic_factor_at): divided out here, so the climate field is not counted twice.
				cp[i] = cm->get_precipitation_at(pos) / MAX(WorldContracts::weather()->orographic_factor_at(pos), 0.05f);
			}
			if (wl) {
				const float depth = tm ? tm->get_water_depth_at(pos) : 0.0f;
				wl[i] = depth > 0.05f ? tm->get_height_at(pos) + depth : -Math::INF;
				if (g && wl[i] > g[i]) {
					g[i] = wl[i];
				}
			}
		}
	}
}

const TerrainDataMap3D *SnowCover3D::_terrain_map() const {
	const SnowCover3D *o = owner ? owner : this;
	if (o->climate_map.is_empty() || !o->is_inside_tree()) {
		return nullptr;
	}
	// Its water and heights only once they are baked (a map may carry the climate alone).
	const TerrainDataMap3D *m = Object::cast_to<TerrainDataMap3D>(o->get_node_or_null(o->climate_map));
	return m && m->is_baked() ? m : nullptr;
}

// Main thread: the inputs (weather, sun, wind, Sx) for one update.
void SnowCover3D::_update_begin(double p_weather_dt, bool p_blocking) {
	_prepare_inputs(p_weather_dt, p_blocking);
	inputs.weather_dt = p_weather_dt;
	const int band = MAX(resolution / 16, 1);
	const int from = int(update_count % uint64_t((resolution + band - 1) / band)) * band;
	_refresh_cell_inputs(from, MIN(from + band, resolution), nullptr);
	_refresh_fire();
	_refresh_salt();
	_refresh_lap();
	_apply_canopy_shakes();
	_refresh_obstacles();
	if (!ice_breaks.is_empty()) {
		_cut_ice(cell_ice, cell_white_ice);
		ice_breaks.clear(); // the cells carry the holes now (and grow ice in them again by Stefan's law)
	}
	if (!traffic_marks.is_empty()) {
		_apply_traffic(cell_packed, cell_glaze, traffic_marks, 0);
		traffic_marks.clear();
	}
}

// Floating ice's bearing capacity: Gold's (1971) P = A h^2, P in kg and h in cm, from his survey of loads on lake and
// river ice: about 3.5 for routine safe loads and failures from about 10 (USACE EM 1110-2-1612; Masterson 2009).
// White (snow) ice carries about half of clear ice, and ice at its melting point about half again.
float SnowCover3D::ice_breaking_load(float p_ice, float p_white_share, float p_air_celsius) {
	const float h = MAX(p_ice, 0.0f) * 100.0f * (1.0f - 0.5f * CLAMP(p_white_share, 0.0f, 1.0f));
	const float warm = Math::lerp(1.0f, 0.5f, Math::smoothstep(-2.0f, 1.0f, p_air_celsius));
	return 10.0f * h * h * warm;
}

void SnowCover3D::_cut_ice(Vector<float> &r_ice, Vector<float> &r_white) const {
	const int n = resolution;
	if (r_ice.size() != n * n || !origin_valid) {
		return;
	}
	const float cs = _cell_size();
	float *ice = r_ice.ptrw();
	float *white = r_white.size() == n * n ? r_white.ptrw() : nullptr;
	for (const IceBreak &b : ice_breaks) {
		const int x0 = MAX(int(Math::floor((b.center.x - b.radius) / cs)) - origin_cell.x, 0);
		const int x1 = MIN(int(Math::floor((b.center.x + b.radius) / cs)) - origin_cell.x, n - 1);
		const int z0 = MAX(int(Math::floor((b.center.y - b.radius) / cs)) - origin_cell.y, 0);
		const int z1 = MIN(int(Math::floor((b.center.y + b.radius) / cs)) - origin_cell.y, n - 1);
		for (int z = z0; z <= z1; z++) {
			for (int x = x0; x <= x1; x++) {
				const Vector2 c((origin_cell.x + x + 0.5f) * cs, (origin_cell.y + z + 0.5f) * cs);
				if (c.distance_to(b.center) <= b.radius + 0.5f * cs) {
					ice[z * n + x] = 0.0f;
					if (white) {
						white[z * n + x] = 0.0f;
					}
				}
			}
		}
	}
}

// Main thread, each physics frame: a body resting on the ice heavier than it bears breaks through. The hole is the
// body's size and a little more; it shows and stops bearing at once and freezes over again from open water.
void SnowCover3D::_ice_load_step() {
	if (driven || owner || !lake_ice_enabled || !is_inside_tree() || !origin_valid || pub.ice.size() != resolution * resolution) {
		return;
	}
	const float air = WorldContracts::weather()->has_provider() ? WorldContracts::weather()->air_temperature_at(_follow_position()) : -5.0f;
	bool broke = false;
	for (Node *n : get_tree()->get_nodes_in_group(SNAME("world_contact"))) {
		const Node3D *n3 = Object::cast_to<Node3D>(n);
		if (!n3 || !n3->is_visible_in_tree()) {
			continue;
		}
		const WorldContactBody b = WorldContracts::describe_body(n3);
		if (b.mass <= 0.0f) {
			continue;
		}
		const float ice = get_ice_thickness_at(b.center);
		const float water = get_water_level_at(b.center);
		if (ice < 0.003f || !(water > -1e29f)) {
			continue;
		}
		const float top = water + SnowLakeIce3D::freeboard(ice);
		if (b.center.y - b.half_height - top > 0.3f) {
			continue; // above the ice, not on it
		}
		const float white = get_white_ice_thickness_at(b.center) / MAX(ice, 1e-4f);
		if (b.mass > ice_breaking_load(ice, white, air)) {
			IceBreak br;
			br.center = Vector2(b.center.x, b.center.z);
			br.radius = b.radius + 0.3f;
			ice_breaks.push_back(br);
			ice_break_count++;
			broke = true;
		}
	}
	if (broke) {
		_cut_ice(pub.ice, pub.white_ice);
		if (lake_ice) {
			lake_ice->rebuild();
		}
	}
}

float SnowCover3D::impurity_albedo_drop(float p_mg_per_m2) {
	return 0.35f * (1.0f - Math::exp(-MAX(p_mg_per_m2, 0.0f) / 15.0f));
}

void SnowCover3D::add_impurity(Vector3 p_position, float p_radius, float p_mg_per_m2) {
	if (p_mg_per_m2 <= 0.0f || impurity_drops.size() >= 1024) {
		return;
	}
	ImpurityDrop d;
	d.center = Vector2(p_position.x, p_position.z);
	d.radius = MAX(p_radius, 0.0f);
	d.mg = p_mg_per_m2;
	impurity_drops.push_back(d);
}

// A crown shaken from outside the weather (trees: a snapped stem, a thrown or felled tree, a body brushing it): the
// share of the held snow within the radius falls onto the pack below, as it would unload, at once. The kg returned is
// read off the published load (what the trees saw); the cells give it up at the next update's start.
float SnowCover3D::shake_canopy(Vector3 p_position, float p_radius, float p_share) {
	const float share = CLAMP(p_share, 0.0f, 1.0f);
	const int n = resolution;
	if (share <= 0.0f || !canopy_enabled || pub.canopy_load.size() != n * n || canopy_shakes.size() >= 1024) {
		return 0.0f;
	}
	const Rect2 r = get_map_rect();
	const float cs = get_extent() / n;
	const float rad = MAX(p_radius, cs * 0.5f);
	const Vector2 at(p_position.x, p_position.z);
	const int x0 = MAX(int(Math::floor((at.x - rad - r.position.x) / cs)), 0);
	const int x1 = MIN(int(Math::floor((at.x + rad - r.position.x) / cs)), n - 1);
	const int z0 = MAX(int(Math::floor((at.y - rad - r.position.y) / cs)), 0);
	const int z1 = MIN(int(Math::floor((at.y + rad - r.position.y) / cs)), n - 1);
	float kg = 0.0f;
	for (int z = z0; z <= z1; z++) {
		for (int x = x0; x <= x1; x++) {
			const Vector2 c(r.position.x + (x + 0.5f) * cs, r.position.y + (z + 0.5f) * cs);
			if (c.distance_to(at) <= rad + cs * 0.5f) {
				// What earlier shakes queued on this cell since the last update has already left it.
				float left = pub.canopy_load[z * n + x];
				for (const CanopyShake &q : canopy_shakes) {
					if (c.distance_to(q.center) <= q.radius + cs * 0.5f) {
						left *= 1.0f - q.share;
					}
				}
				kg += left * share * cs * cs; // mm of water = kg/m2
			}
		}
	}
	if (kg <= 0.0f) {
		return 0.0f;
	}
	CanopyShake s;
	s.center = at;
	s.radius = rad;
	s.share = share;
	canopy_shakes.push_back(s);
	return kg;
}

void SnowCover3D::_apply_canopy_shakes() {
	const int n = resolution;
	if (canopy_shakes.is_empty()) {
		return;
	}
	if (cell_canopy_load.size() != n * n || cell_swe.size() != n * n) {
		canopy_shakes.clear();
		return;
	}
	const Rect2 r = get_map_rect();
	const float cs = get_extent() / n;
	for (const CanopyShake &s : canopy_shakes) {
		const int x0 = MAX(int(Math::floor((s.center.x - s.radius - r.position.x) / cs)), 0);
		const int x1 = MIN(int(Math::floor((s.center.x + s.radius - r.position.x) / cs)), n - 1);
		const int z0 = MAX(int(Math::floor((s.center.y - s.radius - r.position.y) / cs)), 0);
		const int z1 = MIN(int(Math::floor((s.center.y + s.radius - r.position.y) / cs)), n - 1);
		for (int z = z0; z <= z1; z++) {
			for (int x = x0; x <= x1; x++) {
				const Vector2 c(r.position.x + (x + 0.5f) * cs, r.position.y + (z + 0.5f) * cs);
				const int i = z * n + x;
				if (c.distance_to(s.center) > s.radius + cs * 0.5f || cell_canopy_load[i] <= 0.0f) {
					continue;
				}
				// Held snow lands as it was caught: settled crown snow of about 150 kg/m3 (Pomeroy and Gray 1995).
				const float drop = cell_canopy_load[i] * s.share;
				cell_canopy_load.write[i] -= drop;
				_deposit(i, drop, 150.0f);
				// It whitens the pack as new snow does (the step's own unloading lands as snowfall).
				float &alb = cell_albedo.write[i];
				alb += (ALBEDO_FRESH - alb) * MIN(drop / SNOW_REFRESH_MM, 1.0f);
			}
		}
	}
	canopy_shakes.clear();
}

float SnowCover3D::get_impurity_at(Vector3 p_position) const {
	if (world && !get_map_rect().has_point(Vector2(p_position.x, p_position.z))) {
		return world->get_impurity_at(p_position);
	}
	if (pub.lap.size() != resolution * resolution) {
		return 0.0f;
	}
	return MAX(_sample(pub.lap, Vector2(p_position.x, p_position.z)), 0.0f);
}

// Dirty snow (docs/ideas/world-links.md): the particles settling on the pack, per hour of weather time, from what the
// contracts report; read on the main thread every few seconds (smoke and burn scars change slowly). Smoke haze: dry
// deposition of its particulate at about 0.2 cm/s from the layer's 2 km (Seinfeld and Pandis), of which about a tenth
// is black carbon. Burnt forest: charred bark and debris falling from the burnt trees, about 2 mg/m2 a day of black
// carbon equivalent the first winter, fading over years since the burn (Gleason et al. 2013; Gleason and Nolin
// 2016), more under a closed canopy. Producers of dust add theirs through add_impurity (drops applied here).
void SnowCover3D::_refresh_lap() {
	const int n = resolution;
	if (cell_lap.size() != n * n || cell_ground.size() != n * n || driven) {
		return;
	}
	const float cs = get_extent() / n;
	if (!impurity_drops.is_empty()) {
		const Rect2 r = get_map_rect();
		float *lap = cell_lap.ptrw();
		for (const ImpurityDrop &d : impurity_drops) {
			const float rad = MAX(d.radius, cs * 0.5f);
			const int x0 = MAX(int(Math::floor((d.center.x - rad - r.position.x) / cs)), 0);
			const int x1 = MIN(int(Math::floor((d.center.x + rad - r.position.x) / cs)), n - 1);
			const int z0 = MAX(int(Math::floor((d.center.y - rad - r.position.y) / cs)), 0);
			const int z1 = MIN(int(Math::floor((d.center.y + rad - r.position.y) / cs)), n - 1);
			for (int z = z0; z <= z1; z++) {
				for (int x = x0; x <= x1; x++) {
					const Vector2 c(r.position.x + (x + 0.5f) * cs, r.position.y + (z + 0.5f) * cs);
					if (c.distance_to(d.center) <= rad + cs * 0.5f && cell_swe[z * n + x] > SWE_MIN) {
						lap[z * n + x] += d.mg;
					}
				}
			}
		}
		impurity_drops.clear();
	}
	const uint64_t now = OS::get_singleton()->get_ticks_usec();
	// Every few seconds, and every update that stands for a long stretch of weather time (advance, a climate run).
	if (lap_refresh_usec != 0 && now - lap_refresh_usec < 5000000 && inputs.weather_dt < 600.0) {
		return;
	}
	lap_refresh_usec = now;
	float *in = cell_lap_in.ptrw();
	const WorldWeather *wm = WorldContracts::weather();
	const WorldFire *fire = WorldContracts::fire();
	const bool smoke = impurities && wm && wm->has_provider();
	const bool burn = impurities && fire && fire->has_provider();
	for (int z = 0; z < n; z++) {
		for (int x = 0; x < n; x++) {
			const int i = z * n + x;
			float dep = 0.0f;
			if (smoke || burn) {
				Vector3 pos = _cell_center(x, z);
				pos.y = cell_ground[i];
				if (smoke) {
					// The smoke at the snow (g/m3: the layer's 2 km mix, or a cold pool's lid holding it low), times
					// 0.002 m/s, a tenth black carbon: mg/m2 per hour.
					dep += MAX(wm->smoke_concentration_at(pos), 0.0f) * 0.002f * 0.1f * 3.6e6f;
				}
				if (burn) {
					const float age = fire->burn_age_at(pos);
					if (age >= 0.0f && age < 15.0f) {
						const float canopy = cell_canopy.size() == n * n ? CLAMP(cell_canopy[i], 0.0f, 1.0f) : 0.0f;
						dep += 2.0f / 24.0f * Math::exp(-age / 4.0f) * (0.3f + 0.7f * canopy);
					}
				}
			}
			in[i] = dep;
		}
	}
}

// Fire over the snow (docs/ideas/world-links.md, fire → snow melt): the heat each cell takes from WorldFire, read on
// the main thread every update since a fire moves fast. None while a climate bake drives the cover.
void SnowCover3D::_refresh_fire() {
	const int n = resolution;
	WorldFire *fire = WorldContracts::fire();
	if (driven || !fire || !fire->has_provider() || cell_ground.size() != n * n) {
		cell_fire.clear();
		return;
	}
	cell_fire.resize(n * n);
	float *f = cell_fire.ptrw();
	bool any = false;
	for (int z = 0; z < n; z++) {
		for (int x = 0; x < n; x++) {
			Vector3 pos = _cell_center(x, z);
			pos.y = cell_ground[z * n + x];
			const float h = MAX(fire->heat_at(pos), 0.0f);
			f[z * n + x] = h;
			any = any || h > 0.0f;
		}
	}
	if (!any) {
		cell_fire.clear();
	}
}

// Sea salt over the snow (docs/ideas/world-links.md, salt melts shore snow and ice): the salt on surfaces from
// WorldWeather each update. A sparse probe first: with no salt anywhere on the square the cells are not read.
void SnowCover3D::_refresh_salt() {
	const int n = resolution;
	const WorldWeather *w = WorldContracts::weather();
	if (!salt_melt || driven || cell_ground.size() != n * n || !(w->has_provider() || w->get_script_instance())) {
		cell_salt.clear();
		return;
	}
	bool any = false;
	for (int z = 0; z < n && !any; z += 8) {
		for (int x = 0; x < n && !any; x += 8) {
			Vector3 pos = _cell_center(x, z);
			pos.y = cell_ground[z * n + x];
			any = w->salt_at(pos) > 0.0f;
		}
	}
	if (!any) {
		cell_salt.clear();
		cell_salt_seen.fill(0.0f);
		return;
	}
	cell_salt.resize(n * n);
	if (cell_salt_pack.size() != n * n) {
		cell_salt_pack.resize(n * n);
		cell_salt_pack.fill(0.0f);
		cell_salt_seen.resize(n * n);
		cell_salt_seen.fill(0.0f);
	}
	float *f = cell_salt.ptrw();
	float *pk = cell_salt_pack.ptrw();
	float *seen = cell_salt_seen.ptrw();
	for (int z = 0; z < n; z++) {
		for (int x = 0; x < n; x++) {
			const int i = z * n + x;
			Vector3 pos = _cell_center(x, z);
			pos.y = cell_ground[i];
			f[i] = MAX(w->salt_at(pos), 0.0f);
			// New salt settles into the pack where there is one (rain washing the surface's off adds none).
			if (f[i] > seen[i] && cell_swe[i] > SWE_MIN) {
				pk[i] += f[i] - seen[i];
			}
			seen[i] = f[i];
		}
	}
}

float SnowCover3D::get_salt_depression_at(Vector3 p_position) const {
	const int n = resolution;
	if (cell_salt_pack.size() != n * n || cell_swe.size() != n * n) {
		return 0.0f;
	}
	const float s = _sample(cell_salt_pack, Vector2(p_position.x, p_position.z));
	if (s <= 0.0f) {
		return 0.0f;
	}
	const float swe = _sample(cell_swe, Vector2(p_position.x, p_position.z));
	const float liq = _sample(cell_liquid, Vector2(p_position.x, p_position.z));
	const float brine = MAX(MAX(liq, liquid_capacity * MAX(swe - liq, 0.0f)), SALT_FILM);
	return MIN(SALT_DEPRESSION * s / brine, SALT_EUTECTIC);
}

// Wind obstacles over the snow (docs/ideas/world-links.md, lee drifts): their heights from WorldWeather, every few
// seconds and when the square has moved. A change bakes Sx again.
void SnowCover3D::_refresh_obstacles() {
	const int n = resolution;
	const WorldWeather *wm = WorldContracts::weather();
	if (!drift_obstacles || !drift_enabled || !wm || cell_ground.size() != n * n) {
		if (obstacle_active) {
			sx_dirty = sx_valid;
			sx_cache.clear();
		}
		obstacle_next.clear();
		cell_lee.clear();
		obstacle_active = false;
		return;
	}
	if (obstacle_origin != origin_cell || obstacle_next.size() != n * n || cell_lee.size() != n * n) {
		// A new square: start a sweep from scratch.
		obstacle_next.resize(n * n);
		obstacle_next.fill(0.0f);
		cell_lee.resize(n * n);
		cell_lee.fill(1.0f);
		obstacle_origin = origin_cell;
		obstacle_row = 0;
		obstacle_sweep_changed = obstacle_active;
		obstacle_sweep_any = false;
		obstacle_active = false;
	}
	// A band of rows each update (all of them for a long step), so the frame never waits on a whole square.
	const int band = inputs.weather_dt >= 600.0 ? n : MAX(n / 16, 1);
	const int end = MIN(obstacle_row + band, n);
	float *w = obstacle_next.ptrw();
	float *lw = cell_lee.ptrw();
	for (int z = obstacle_row; z < end; z++) {
		for (int x = 0; x < n; x++) {
			const int i = z * n + x;
			Vector3 pos = _cell_center(x, z);
			pos.y = cell_ground[i];
			const float h = MAX(wm->obstacle_height_at(pos), 0.0f);
			if (Math::abs(h - w[i]) > 0.1f) {
				obstacle_sweep_changed = true;
			}
			w[i] = h;
			// An obstacle's own top stands in the open wind. Elsewhere the lee at the snow's surface: it fades as
			// the drift fills it.
			const float depth = cell_swe[i] > SWE_MIN ? cell_swe[i] / MAX(cell_density[i], 30.0f) : 0.0f;
			const float lee = h > 0.0f ? 1.0f : wm->obstacle_lee_at(pos + Vector3(0.0f, depth, 0.0f));
			lw[i] = Math::is_finite(lee) ? CLAMP(lee, 0.0f, 1.0f) : 1.0f;
			obstacle_sweep_any = obstacle_sweep_any || h > 0.0f || lw[i] < 0.999f;
		}
	}
	obstacle_row = end;
	if (end >= n) {
		obstacle_row = 0;
		if (obstacle_sweep_changed || obstacle_active != obstacle_sweep_any) {
			// Even while an Sx bake runs (it started before this sweep): the next one takes the obstacles in.
			sx_dirty = true;
			sx_cache.clear();
		}
		obstacle_active = obstacle_sweep_any;
		obstacle_sweep_any = false;
		obstacle_sweep_changed = false;
	}
}

// The cells, slides, drift and cornices: any thread, nothing else touching the cells meanwhile.
void SnowCover3D::_update_cells(bool p_parallel) {
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	// The rows write the fields in parallel through write[]: make each one unique first, or a copy held
	// elsewhere (the published copies, the weather map's runoff) turns every write into a racing
	// copy-on-write.
	cell_swe.ptrw();
	cell_liquid.ptrw();
	cell_density.ptrw();
	cell_albedo.ptrw();
	cell_runoff.ptrw();
	cell_crust.ptrw();
	cell_crust_cycles.ptrw();
	cell_crust_burial.ptrw();
	cell_crust_wet.ptrw();
	cell_hoar.ptrw();
	cell_hoar_layer.ptrw();
	cell_hoar_burial.ptrw();
	cell_hoar_age.ptrw();
	cell_cups.ptrw(); // published as pub.cups
	cell_wind_slab.ptrw();
	cell_slab_cover.ptrw();
	cell_depth_hoar.ptrw();
	cell_sas_x.ptrw();
	cell_sas_z.ptrw();
	cell_slush.ptrw(); // published as pub.slush
	cell_glaze.ptrw(); // published as pub.glaze
	cell_packed.ptrw(); // published as pub.packed
	cell_new_depth.ptrw();
	cell_canopy_load.ptrw();
	if (layers_enabled && layer_n.size() != resolution * resolution) {
		_layers_alloc();
	}
	layer_swe.ptrw();
	layer_rho.ptrw();
	layer_age.ptrw();
	layer_liq.ptrw();
	layer_kind.ptrw();
	layer_n.ptrw();
	cell_cornice_x.ptrw();
	cell_cornice_z.ptrw();
	cell_cornice_crack.ptrw();
	cell_ice.ptrw();
	cell_white_ice.ptrw();
	if (p_parallel) {
		WorkerThreadPool::GroupID task = WorkerThreadPool::get_singleton()->add_template_group_task(this, &SnowCover3D::_update_row, (void *)nullptr, resolution, -1, true, String("SnowCover3D cells"));
		WorkerThreadPool::get_singleton()->wait_for_group_task_completion(task);
	} else {
		// On a worker already: the rows in turn (a pool thread waiting on its own group could starve it).
		for (int z = 0; z < resolution; z++) {
			_update_row(uint32_t(z), nullptr);
		}
	}
	// Sliding is slow next to a frame: a pass every slide_interval of weather time.
	slide_accum += inputs.weather_dt;
	if (slide_enabled && slide_accum >= slide_interval) {
		slide_accum = 0.0;
		_slide();
	}
	if (wind_transport_enabled) {
		_blow(inputs.dt_h, last_wind);
	} else {
		last_blown = 0.0f;
		last_blown_max = 0.0f;
	}
	if (cornices_enabled) {
		_grow_cornices(inputs.dt_h, last_wind);
	}
	_find_spindrift(last_wind);
	update_msec = double(OS::get_singleton()->get_ticks_usec() - t0) / 1000.0;
}

// What reaches the ground under the pack (rain through it and melt) wets surfaces and feeds WeatherRunoff3D's
// streams in place of the precipitation the weather map would put there. With a world tier, the source is the
// world's cells with the near square's averaged into those it covers, so melt on the whole mountain runs off
// (spring melt is the main flood water in mountains: Marks et al. 1998) and nothing is counted twice.
void SnowCover3D::_send_runoff() {
	WeatherMap3D *weather = owner ? nullptr : WeatherMap3D::get_active();
	if (!weather) {
		return;
	}
	const int n = resolution;
	const int m = world ? world->resolution : 0;
	if (!world || world->cell_runoff.size() != m * m || cell_runoff.size() != n * n || !world->origin_valid || !origin_valid) {
		weather->set_surface_water_source_cells(get_instance_id(), cell_runoff, n, get_map_rect());
		return;
	}
	const Rect2 wr = world->get_map_rect();
	const float wcs = wr.size.x / m;
	runoff_out = world->cell_runoff;
	LocalVector<float> sum, cnt;
	sum.resize(m * m);
	cnt.resize(m * m);
	for (int i = 0; i < m * m; i++) {
		sum[i] = 0.0f;
		cnt[i] = 0.0f;
	}
	const float cs = _cell_size();
	for (int z = 0; z < n; z++) {
		for (int x = 0; x < n; x++) {
			const Vector2 c((origin_cell.x + x + 0.5f) * cs, (origin_cell.y + z + 0.5f) * cs);
			const int wx = int(Math::floor((c.x - wr.position.x) / wcs));
			const int wz = int(Math::floor((c.y - wr.position.y) / wcs));
			if (wx < 0 || wz < 0 || wx >= m || wz >= m) {
				continue;
			}
			sum[wz * m + wx] += MAX(cell_runoff[z * n + x], 0.0f);
			cnt[wz * m + wx] += 1.0f;
		}
	}
	// A world cell part covered by the square takes the square's average over its covered share.
	const float full = (wcs / cs) * (wcs / cs);
	float *o = runoff_out.ptrw();
	for (int i = 0; i < m * m; i++) {
		if (cnt[i] > 0.0f) {
			const float f = MIN(cnt[i] / full, 1.0f);
			o[i] = Math::lerp(MAX(o[i], 0.0f), sum[i] / cnt[i], f);
		}
	}
	weather->set_surface_water_source_cells(get_instance_id(), runoff_out, m, wr);
}

// Main thread, after the cells.
void SnowCover3D::_update_end() {
	_send_runoff();
	last_step_hours = inputs.dt_h;
	last_drift = inputs.wind_factor;
	update_count++;
	if (avalanche_node) {
		avalanche_node->_track_recent(this); // every update's new snow and hours (follow-up 20)
	}
	_feed_spindrift();
}

void SnowCover3D::_update(double p_weather_dt, bool p_blocking) {
	_update_begin(p_weather_dt, p_blocking);
	_update_cells(true);
	_update_end();
}

void SnowCover3D::_publish_cells() {
	if (driven) {
		return; // a climate bake: read by nobody until it is done
	}
	pub.swe = cell_swe;
	pub.liquid = cell_liquid;
	pub.density = cell_density;
	pub.albedo = cell_albedo;
	if (cell_lap.size() == cell_albedo.size()) {
		// What shows: the clean pack's albedo less the particles at (or just under) the surface.
		pub.lap.resize(cell_lap.size());
		float *pl = pub.lap.ptrw();
		float *pa = pub.albedo.ptrw();
		for (int i = 0; i < cell_lap.size(); i++) {
			pl[i] = cell_lap[i] + cell_lap_deep[i] * Math::exp(-cell_lap_cover[i] / 0.02f);
			pa[i] = MAX(pa[i] - impurity_albedo_drop(pl[i]), 0.05f);
		}
	}
	pub.runoff = cell_runoff;
	pub.canopy_load = cell_canopy_load;
	pub.new_depth = cell_new_depth;
	pub.crust = cell_crust;
	pub.crust_cycles = cell_crust_cycles;
	pub.crust_burial = cell_crust_burial;
	pub.crust_wet = cell_crust_wet;
	pub.hoar = cell_hoar;
	pub.cups = cell_cups;
	pub.wind_slab = cell_wind_slab;
	pub.sas_x = cell_sas_x;
	pub.depth_hoar = cell_depth_hoar;
	pub.layer_swe = layer_swe;
	pub.layer_rho = layer_rho;
	pub.layer_age = layer_age;
	pub.layer_liq = layer_liq;
	pub.layer_kind = layer_kind;
	pub.layer_n = layer_n;
	pub.sas_z = cell_sas_z;
	pub.slush = cell_slush;
	pub.packed = cell_packed;
	pub.glaze = cell_glaze;
	if (!traffic_marks.is_empty()) {
		_apply_traffic(pub.packed, pub.glaze, traffic_marks, 0); // since this update started: the cells take them at the next one
	}
	pub.hoar_layer = cell_hoar_layer;
	pub.hoar_burial = cell_hoar_burial;
	pub.hoar_age = cell_hoar_age;
	pub.cornice_x = cell_cornice_x;
	pub.cornice_z = cell_cornice_z;
	pub.cornice_crack = cell_cornice_crack;
	pub.ice = cell_ice;
	pub.white_ice = cell_white_ice;
	if (!ice_breaks.is_empty()) {
		_cut_ice(pub.ice, pub.white_ice); // broken since this update started: the cells take them at the next one
	}
}

void SnowCover3D::_start_background(double p_weather_dt, bool p_world) {
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	_update_begin(p_weather_dt, false);
	cover_task_world = p_world && world;
	if (cover_task_world) {
		world->_update_begin(world_dt, false);
	}
	cover_task = WorkerThreadPool::get_singleton()->add_template_task(this, &SnowCover3D::_background_task, (void *)nullptr, false, String("SnowCover3D update"));
	main_usec = OS::get_singleton()->get_ticks_usec() - t0;
}

void SnowCover3D::_background_task(void *p_unused) {
	_update_cells(false);
	if (cover_task_world) {
		// The world tier's turn: the square's state goes up first, then the world cells step (a cornice
		// failing there drops its debris into the square too), then both maps' bytes.
		_feed_world();
		world->_update_cells(false);
		world->_build_upload();
	}
	_build_upload();
}

bool SnowCover3D::_finish_background(bool p_wait) {
	if (cover_task == WorkerThreadPool::INVALID_TASK_ID) {
		return false;
	}
	if (!p_wait && !WorkerThreadPool::get_singleton()->is_task_completed(cover_task)) {
		return false;
	}
	WorkerThreadPool::get_singleton()->wait_for_task_completion(cover_task);
	cover_task = WorkerThreadPool::INVALID_TASK_ID;
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	_update_end();
	if (cover_task_world) {
		world->_update_end();
		_emit_cornice_failures();
		world->_apply_upload();
		world->_publish_cells();
		_publish_world_globals();
	}
	_apply_upload();
	_publish_cells();
	main_usec += OS::get_singleton()->get_ticks_usec() - t0;
	return true;
}

void SnowCover3D::sync_update() {
	_finish_background(true);
}

void SnowCover3D::set_background_updates(bool p_enabled) {
	_finish_background(true);
	background_updates = p_enabled;
}

void SnowCover3D::_upload() {
	_build_upload();
	_apply_upload();
	_publish_cells();
}

// The textures' bytes: any thread.
void SnowCover3D::_build_upload() {
	const int n = resolution;
	upload_snow.unref();
	upload_catch.unref();
	if (snow_image.is_null() || snow_image->get_width() != n) {
		return;
	}
	Vector<uint8_t> bytes;
	bytes.resize(n * n * 8);
	uint16_t *w = reinterpret_cast<uint16_t *>(bytes.ptrw());
	for (int i = 0; i < n * n; i++) {
		const float swe = cell_swe[i];
		const float depth = swe > SWE_MIN ? swe / cell_density[i] : 0.0f;
		const float hold = liquid_capacity * (swe - cell_liquid[i]);
		float wet = hold > 1e-4f ? CLAMP(cell_liquid[i] / hold, 0.0f, 1.0f) : 0.0f;
		const float sl = cell_slush.size() == n * n ? cell_slush[i] : 0.0f;
		if (depth > 0.0f && sl > 0.5f) {
			// Over 1: the share of the pack's depth that is slush (saturated from the base up).
			wet = 1.0f + CLAMP(slush_height(swe, cell_liquid[i], cell_density[i], sl) / depth, 0.0f, 1.0f);
		}
		w[i * 4 + 0] = Math::make_half_float(depth);
		w[i * 4 + 1] = Math::make_half_float(pub.albedo.size() == n * n ? pub.albedo[i] : cell_albedo[i]);
		w[i * 4 + 2] = Math::make_half_float(wet);
		w[i * 4 + 3] = Math::make_half_float(cell_density[i] / 1000.0f);
	}
	upload_snow = Image::create_from_data(n, n, false, Image::FORMAT_RGBAH, bytes);
	upload_cups.unref();
	upload_sastrugi.unref();
	if (cell_cups.size() == n * n) {
		// The surface map: suncup relief (m), traffic packing (0..1), glaze (mm).
		const bool road = cell_packed.size() == n * n && cell_glaze.size() == n * n;
		Vector<uint8_t> cb;
		cb.resize(n * n * 8);
		uint16_t *cw = reinterpret_cast<uint16_t *>(cb.ptrw());
		for (int i = 0; i < n * n; i++) {
			cw[i * 4 + 0] = Math::make_half_float(cell_cups[i]);
			cw[i * 4 + 1] = Math::make_half_float(road ? cell_packed[i] : 0.0f);
			cw[i * 4 + 2] = Math::make_half_float(road ? cell_glaze[i] : 0.0f);
			cw[i * 4 + 3] = Math::make_half_float(0.0f);
		}
		upload_cups = Image::create_from_data(n, n, false, Image::FORMAT_RGBAH, cb);
	}
	if (cell_sas_x.size() == n * n && cell_sas_z.size() == n * n) {
		// Sastrugi: relief (m) times the carving wind's direction, so the filtered map blends directions too.
		Vector<uint8_t> sb;
		sb.resize(n * n * 4);
		uint16_t *sw = reinterpret_cast<uint16_t *>(sb.ptrw());
		for (int i = 0; i < n * n; i++) {
			sw[i * 2 + 0] = Math::make_half_float(cell_sas_x[i]);
			sw[i * 2 + 1] = Math::make_half_float(cell_sas_z[i]);
		}
		upload_sastrugi = Image::create_from_data(n, n, false, Image::FORMAT_RGH, sb);
	}
	// The strata map (round 8, follow-up 53): the pack's layers as up to eight bands, for the walls of ruts and
	// prints. Each channel holds two bands (see strata_bands), from the surface down: band * 4096 + the next band;
	// 0 ends the list.
	upload_strata.unref();
	// The live layers, not pub's (published after this runs): the same step as the depth map.
	if (layers_enabled && layer_n.size() == n * n && layer_swe.size() == n * n * PACK_LAYERS && layer_liq.size() == layer_swe.size()) {
		Vector<uint8_t> tb;
		tb.resize(n * n * 16);
		float *tw = reinterpret_cast<float *>(tb.ptrw());
		for (int i = 0; i < n * n; i++) {
			const float swe = cell_swe[i];
			const float depth = swe > SWE_MIN ? swe / cell_density[i] : 0.0f;
			const int k0 = i * PACK_LAYERS;
			const Vector<int> bands = strata_bands(layer_swe.ptr() + k0, layer_rho.ptr() + k0, layer_liq.ptr() + k0, layer_kind.ptr() + k0, layer_n[i], depth, liquid_capacity);
			for (int c = 0; c < 4; c++) {
				const int a = 2 * c < bands.size() ? bands[2 * c] : 0;
				const int b = 2 * c + 1 < bands.size() ? bands[2 * c + 1] : 0;
				tw[i * 4 + c] = float(a * 4096 + b); // two 12-bit bands, exact in a float
			}
		}
		upload_strata = Image::create_from_data(n, n, false, Image::FORMAT_RGBAF, tb);
	}
	// The catch map: the height of the surface the pack lies on, the canopy's load over its capacity, the
	// canopy closure and the surface hoar. Materials leave what lies under an overhang bare and give tree crowns their load.
	if (catch_image.is_valid() && catch_image->get_width() == n) {
		Vector<uint8_t> cb;
		cb.resize(n * n * 16);
		float *cw = reinterpret_cast<float *>(cb.ptrw());
		for (int i = 0; i < n * n; i++) {
			const float closure = cell_canopy.size() == n * n ? cell_canopy[i] : 0.0f;
			const float cap = closure > 0.01f ? canopy_capacity(closure, 100.0f) : 0.0f;
			cw[i * 4 + 0] = cell_ground[i];
			cw[i * 4 + 1] = cap > 0.0f ? CLAMP(cell_canopy_load[i] / cap, 0.0f, 1.0f) : 0.0f;
			cw[i * 4 + 2] = closure;
			// Alpha: 1 marks the map valid; above it, the surface hoar's crystals in centimetres (materials draw
			// their glints larger, round 5).
			cw[i * 4 + 3] = 1.0f + (cell_hoar.size() == n * n ? cell_hoar[i] * 0.1f : 0.0f);
		}
		upload_catch = Image::create_from_data(n, n, false, Image::FORMAT_RGBAF, cb);
	}
}

// Main thread: the bytes into the textures.
void SnowCover3D::_apply_upload() {
	const int n = resolution;
	if (upload_snow.is_null() || upload_snow->get_width() != n || snow_image.is_null() || snow_image->get_width() != n) {
		return;
	}
	snow_image = upload_snow;
	upload_snow.unref();
	if (upload_cups.is_valid()) {
		cups_image = upload_cups;
		upload_cups.unref();
		if (cups_texture.is_null() || cups_texture->get_width() != n) {
			cups_texture = ImageTexture::create_from_image(cups_image);
		} else {
			cups_texture->update(cups_image);
		}
	}
	if (upload_sastrugi.is_valid()) {
		sastrugi_image = upload_sastrugi;
		upload_sastrugi.unref();
		if (sastrugi_texture.is_null() || sastrugi_texture->get_width() != n) {
			sastrugi_texture = ImageTexture::create_from_image(sastrugi_image);
		} else {
			sastrugi_texture->update(sastrugi_image);
		}
	}
	if (upload_strata.is_valid()) {
		strata_image = upload_strata;
		upload_strata.unref();
		if (strata_texture.is_null() || strata_texture->get_width() != n) {
			strata_texture = ImageTexture::create_from_image(strata_image);
		} else {
			strata_texture->update(strata_image);
		}
	}
	if (upload_catch.is_valid() && catch_image.is_valid() && catch_image->get_width() == n) {
		catch_image = upload_catch;
		upload_catch.unref();
		if (catch_texture.is_null()) {
			catch_texture = ImageTexture::create_from_image(catch_image);
		} else {
			catch_texture->update(catch_image);
		}
	}
	if (snow_texture.is_null()) {
		snow_texture = ImageTexture::create_from_image(snow_image);
	} else {
		snow_texture->update(snow_image);
	}
	_publish_globals();
}

void SnowCover3D::_set_global(const StringName &p_name, const Variant &p_value, int p_type) {
	RenderingServer *rs = RenderingServer::get_singleton();
	static HashSet<StringName> added;
	if (!added.has(p_name) && !ProjectSettings::get_singleton()->has_setting("shader_globals/" + String(p_name))) {
		added.insert(p_name);
		rs->global_shader_parameter_add(p_name, RSE::GlobalShaderParameterType(p_type), p_value);
		return;
	}
	rs->global_shader_parameter_set(p_name, p_value);
}

void SnowCover3D::_publish_globals() {
	if (!publish_shader_globals || active != this || snow_texture.is_null()) {
		return;
	}
	if (!globals_added) {
		_set_global("snow_map", snow_texture->get_rid(), RSE::GLOBAL_VAR_TYPE_SAMPLER2D);
		if (catch_texture.is_valid()) {
			_set_global("snow_catch_map", catch_texture->get_rid(), RSE::GLOBAL_VAR_TYPE_SAMPLER2D);
		}
		globals_added = true;
	}
	const Rect2 r = get_map_rect();
	_set_global("snow_map_rect", Vector4(r.position.x, r.position.y, r.size.x, r.size.y), RSE::GLOBAL_VAR_TYPE_VEC4);
}

void SnowCover3D::advance(float p_seconds) {
	_finish_background(true);
	if (cell_swe.size() != resolution * resolution) {
		_allocate();
	}
	_follow(_follow_position());
	double left = MAX(p_seconds, 0.0f);
	while (left > 0.0) {
		const double step = MIN(left, 600.0);
		_update(step, true);
		left -= step;
	}
	_upload();
}

void SnowCover3D::advance_world(float p_seconds) {
	_finish_background(true);
	if (!world) {
		return;
	}
	double left = MAX(p_seconds, 0.0f);
	while (left > 0.0) {
		const double step = MIN(left, 3600.0);
		_feed_world();
		world->_update(step, true);
		left -= step;
		_emit_cornice_failures();
	}
	world->_upload();
	_publish_world_globals();
}

// ---------------------------------------------------------------------------------------------------------
// The world tier and the climate bake (stage 4)

float SnowCover3D::cornice_growth_share(float p_wind_ratio) {
	// Yu et al. (2023): cornices grow between the threshold wind and twice it, fastest about 40% above it;
	// below there is no transport, above it erosion wins.
	const float r = p_wind_ratio;
	if (r <= 1.0f || r >= 2.0f) {
		return 0.0f;
	}
	return r < 1.4f ? (r - 1.0f) / 0.4f : (2.0f - r) / 0.6f;
}

// Cornice failure (docs/ideas/snow.md, follow-up 6). The slab is wind-packed snow of 450 kg/m3 (stage 4); its
// liquid water adds weight.
static constexpr float CORNICE_DENSITY = 450.0f; // kg/m3
static constexpr float CORNICE_DEBRIS_DENSITY = 500.0f; // kg/m3, avalanche debris

float SnowCover3D::cornice_root_stress(float p_length, float p_crack, float p_liquid) {
	// The overhang as a cantilever thinning from its root (as thick as it is long, stage 4's profile) to the
	// lip: the root carries rho g h L^2 / 6, a bending stress of rho g L at the top of the section. The tension
	// crack cuts into the section; open to a tenth of the length (a tilt of about 6 degrees) it has taken 60%.
	if (p_length <= 0.0f) {
		return 0.0f;
	}
	const float s = MIN(MAX(p_crack, 0.0f) / (0.1f * p_length), 1.0f);
	const float k = 1.0f - 0.6f * s;
	const float rho = CORNICE_DENSITY * (1.0f + CLAMP(p_liquid, 0.0f, 0.5f));
	return rho * 9.81f * p_length / (k * k) * 0.001f;
}

float SnowCover3D::cornice_strength(float p_liquid) {
	// Sommerfeld (1971): the most snow of density rho holds in tension, log10 sigma = -4.3 + 3 log10 rho (sigma
	// in 100 Pa), 457 kPa at 450 kg/m3. A slab metres across holds far less than the small samples (the weakest
	// flaw decides; McClung 1979's large samples): a tenth, so a cold cornice falls at about 10 m, near the
	// longest field cornices. Melt water in the bonds weakens it further (our pick: a third at 10% water by
	// volume).
	const float rho = CORNICE_DENSITY;
	const float theta = CLAMP(p_liquid, 0.0f, 0.5f) * rho / 1000.0f;
	return 0.1f * 5.012e-3f * rho * rho * rho * 0.001f / (1.0f + 20.0f * theta);
}

float SnowCover3D::cornice_crack_rate(float p_length, float p_crack, float p_celsius, float p_liquid) {
	// The slab tilts about its root by viscous creep: strain rate sigma / eta with the snow viscosity of Crocus
	// (Vionnet et al. 2012), eta = eta0 rho / 250 exp(0.1 (0 C - T) + 0.023 rho) / (1 + 60 theta), the crack
	// opening that over the slab's length. A 4 m cornice at -5 C opens about 0.9 cm a day, at 0 C 1.4, wet 4,
	// close to the 0.4 to 2.8 cm a day measured at Gruvefjellet, rising to 8.6 before a failure (Vogel et
	// al. 2012).
	if (p_length <= 0.0f) {
		return 0.0f;
	}
	const float rho = CORNICE_DENSITY;
	const float theta = CLAMP(p_liquid, 0.0f, 0.5f) * rho / 1000.0f;
	const float tc = MIN(p_celsius, 0.0f);
	const double eta = 7.62237e6 * (rho / 250.0) * Math::exp(-0.1 * tc + 0.023 * rho) / (1.0 + 60.0 * theta);
	const double sigma = cornice_root_stress(p_length, p_crack, p_liquid) * 1000.0;
	return float(p_length * sigma / eta * 86400.0);
}

void SnowCover3D::_deposit(int p_index, float p_swe_mm, float p_density) {
	if (p_swe_mm <= 0.0f) {
		return;
	}
	float &swe = cell_swe.write[p_index];
	float &rho = cell_density.write[p_index];
	const float depth = (swe > SWE_MIN ? swe / rho : 0.0f) + p_swe_mm / p_density;
	swe += p_swe_mm;
	rho = swe / MAX(depth, 1e-6f);
	if (cell_crust.size() == cell_swe.size() && cell_crust[p_index] > 0.0f) {
		cell_crust_burial.write[p_index] += p_swe_mm / p_density;
	}
}

void SnowCover3D::_fail_cornice(int p_index, float p_length, Vector2 p_dir) {
	// The slab out from the crack falls (the drawn profile beyond it: 0.9 L^2 a metre of crest) and lands at
	// the foot of the face below as debris: the first cell downwind where the ground flattens under 30 degrees.
	const int n = resolution;
	const float cs = _cell_size();
	const int x0 = p_index % n;
	const int z0 = p_index / n;
	const float mass = 0.9f * p_length * p_length * cs * CORNICE_DENSITY;
	const float tan_foot = Math::tan(Math::deg_to_rad(30.0f));
	int foot = -1;
	float fx = x0 + 0.5f, fz = z0 + 0.5f;
	int prev = p_index;
	for (int k = 1; k <= 32; k++) {
		fx += p_dir.x;
		fz += p_dir.y;
		const int x = int(Math::floor(fx));
		const int z = int(Math::floor(fz));
		if (x < 0 || z < 0 || x >= n || z >= n) {
			break;
		}
		const int j = z * n + x;
		if (j == prev) {
			continue;
		}
		if (k > 1 && cell_ground[prev] - cell_ground[j] < tan_foot * cs) {
			foot = j;
			break;
		}
		prev = j;
	}
	if (foot < 0) {
		foot = prev;
	}
	const float swe = mass / (cs * cs); // kg/m2 = mm
	_deposit(foot, swe, CORNICE_DEBRIS_DENSITY);
	const Vector3 foot_pos = _cell_center(foot % n, foot / n);
	if (owner && owner->cell_swe.size() == owner->resolution * owner->resolution && owner->origin_valid) {
		// The near square too, over the cells under the world cell.
		const float ocs = owner->_cell_size();
		const int on = owner->resolution;
		const int ox0 = int(Math::floor((foot_pos.x - 0.5f * cs) / ocs)) - owner->origin_cell.x;
		const int oz0 = int(Math::floor((foot_pos.z - 0.5f * cs) / ocs)) - owner->origin_cell.y;
		const int span = MAX(int(Math::round(cs / ocs)), 1);
		for (int z = MAX(oz0, 0); z < MIN(oz0 + span, on); z++) {
			for (int x = MAX(ox0, 0); x < MIN(ox0 + span, on); x++) {
				owner->_deposit(z * on + x, swe, CORNICE_DEBRIS_DENSITY);
			}
		}
	}
	CorniceFailure f;
	const Vector3 c = _cell_center(x0, z0);
	f.position = Vector3(c.x, cell_ground[p_index] + cell_swe[p_index] / MAX(cell_density[p_index], 50.0f), c.z);
	f.direction = p_dir;
	f.length = p_length;
	f.mass = mass;
	cornice_failures.push_back(f);
	cornice_failure_count++;
}

void SnowCover3D::_emit_cornice_failures() {
	if (!world || world->cornice_failures.is_empty()) {
		return;
	}
	const Vector<CorniceFailure> list = world->cornice_failures;
	world->cornice_failures.clear();
	for (const CorniceFailure &f : list) {
		emit_signal(SNAME("cornice_failed"), f.position, Vector3(f.direction.x, 0.0f, f.direction.y), f.length, f.mass);
	}
}

uint64_t SnowCover3D::get_cornice_failure_count() const {
	return world ? world->cornice_failure_count : cornice_failure_count;
}

float SnowCover3D::get_cornice_crack_at(Vector3 p_position) const {
	const SnowCover3D *c = world ? world : (cornices_enabled ? this : nullptr);
	if (!c || c->pub.cornice_crack.size() != c->resolution * c->resolution) {
		return 0.0f;
	}
	const float cs = c->_cell_size();
	const int x = int(Math::floor(p_position.x / cs)) - c->origin_cell.x;
	const int z = int(Math::floor(p_position.z / cs)) - c->origin_cell.y;
	if (x < 0 || z < 0 || x >= c->resolution || z >= c->resolution) {
		return 0.0f;
	}
	return c->pub.cornice_crack[z * c->resolution + x];
}

void SnowCover3D::set_cornice_failure_enabled(bool p_enabled) {
	cornice_failure_enabled = p_enabled;
	if (world) {
		world->cornice_failure_enabled = p_enabled;
	}
}

void SnowCover3D::_setup_internal(SnowCover3D *p_owner, Rect2 p_rect, int p_resolution) {
	owner = p_owner;
	extent = MAX(p_rect.size.x, p_rect.size.y);
	resolution = MAX(p_resolution, 4);
	lapse_rate = owner->lapse_rate;
	melt_threshold = owner->melt_threshold;
	melt_temperature_factor = owner->melt_temperature_factor;
	melt_shortwave_factor = owner->melt_shortwave_factor;
	liquid_capacity = owner->liquid_capacity;
	crust_enabled = owner->crust_enabled;
	hoar_enabled = owner->hoar_enabled;
	slide_enabled = owner->slide_enabled;
	lake_ice_enabled = owner->lake_ice_enabled;
	slide_angle = owner->slide_angle;
	slide_min_holding = owner->slide_min_holding;
	slide_holding_decay = owner->slide_holding_decay;
	drift_enabled = owner->drift_enabled;
	drift_distance = owner->drift_distance;
	drift_strength = owner->drift_strength;
	drift_threshold_wind = owner->drift_threshold_wind;
	drift_relief = owner->drift_relief;
	wind_transport_enabled = owner->wind_transport_enabled;
	wind_curvature_length = owner->wind_curvature_length;
	cornice_failure_enabled = owner->cornice_failure_enabled;
	precipitation_gradient = owner->precipitation_gradient;
	publish_shader_globals = false;
	cornices_enabled = true;
	_allocate();
	const float cs = _cell_size();
	origin_cell = Vector2i(int(Math::floor(p_rect.position.x / cs)), int(Math::floor(p_rect.position.y / cs)));
	origin_valid = true;
	_sample_heights(0, resolution);
}

// ---------------------------------------------------------------------------------------------------------
// Wind moving lying snow (docs/ideas/snow.md, follow-up 4): SnowTran-3D (Liston and Sturm 1998).

static constexpr float Z0_SNOW = 0.0005f; // m, aerodynamic roughness of snow
static constexpr float RHO_AIR = 1.3f; // kg/m3
static constexpr float WIND_SNOW_DENSITY = 300.0f; // kg/m3, snow the wind lays down (wind-packed)
static constexpr float DRIFT_REPOSE = 38.0f; // degrees: the slip face of dry blown snow (dry snow's angle of repose, 35-45)
static constexpr float DRIFT_SATURATION = 2.0f; // m: the saturation length of snow saltation (the flux lags the shear)
static constexpr float DRIFT_SHEAR_A = 3.0f; // Kroy 2002's A and B, as the dune sea
static constexpr float DRIFT_SHEAR_B = 0.5f;
static constexpr float SASTRUGI_HARD = 250.0f; // kg/m3: snow dense enough to erode into sastrugi (wind-packed, sintered)
static constexpr float SASTRUGI_RATE = 0.02f; // share of the snow scoured off a hard surface that carves relief (ours: cm an hour in a gale)

static float _ustar(float p_wind10) {
	return 0.41f * MAX(p_wind10, 0.0f) / Math::log(10.0f / Z0_SNOW);
}

float SnowCover3D::transport_threshold(float p_celsius, float p_density) {
	// Two limits, the higher wins: the 10 m threshold of dry snow by temperature (Li and Pomeroy 1997), and
	// the friction velocity a pack of density rho needs, u*t = 0.10 exp(0.003 rho) (Liston et al. 2007).
	const float by_temperature = SnowFall3D::blowing_threshold(p_celsius);
	const float by_density = 0.10f * Math::exp(0.003f * p_density) * Math::log(10.0f / Z0_SNOW) / 0.41f;
	return MAX(by_temperature, by_density);
}

// Suspended flux at a 10 m wind, kg/(m s): the PBSM concentration profile (Pomeroy and Male 1992) times
// the log wind profile, from the top of the saltation layer to 5 m. Tabled once by wind speed.
static float _suspension(float p_wind10) {
	static float table[201];
	static bool built = false;
	if (!built) {
		for (int k = 0; k <= 200; k++) {
			const float u10 = k * 0.25f;
			const float us = _ustar(u10);
			float q = 0.0f;
			if (us > 0.01f) {
				const float zr = 0.05628f * Math::pow(us, 1.27f);
				const int steps = 48;
				const float lz0 = Math::log(zr), lz1 = Math::log(5.0f);
				for (int j = 0; j < steps; j++) {
					const float za = Math::exp(lz0 + (lz1 - lz0) * j / steps);
					const float zb = Math::exp(lz0 + (lz1 - lz0) * (j + 1) / steps);
					const float zm = 0.5f * (za + zb);
					q += SnowFall3D::blowing_concentration(u10, zm) * us / 0.41f * Math::log(zm / Z0_SNOW) * (zb - za);
				}
			}
			table[k] = q;
		}
		built = true;
	}
	const float f = CLAMP(p_wind10 / 0.25f, 0.0f, 199.999f);
	const int k = int(f);
	return Math::lerp(table[k], table[k + 1], f - k);
}

float SnowCover3D::transport_flux(float p_wind10, float p_celsius, float p_density) {
	if (p_celsius >= 0.0f) {
		return 0.0f; // wet snow does not blow
	}
	const float ut10 = transport_threshold(p_celsius, p_density);
	if (p_wind10 <= ut10) {
		return 0.0f;
	}
	const float us = _ustar(p_wind10);
	const float ust = _ustar(ut10);
	// Saltation (Pomeroy and Gray 1990): Qs = 0.68 rho_a u*t (u*^2 - u*t^2) / (g u*).
	const float qs = 0.68f * RHO_AIR * ust * (us * us - ust * ust) / (9.81f * us);
	// Suspension above it, faded in from the threshold so the flux has no step there (our smoothing).
	const float qt = _suspension(p_wind10) * (1.0f - (ust * ust) / (us * us));
	return qs + qt;
}

void SnowCover3D::_blow(float p_dt_h, Vector2 p_wind) {
	// SnowTran-3D's mass balance, dS/dt = -div(Qs + Qt): each cell sends its flux downwind (a donor-cell
	// scheme), so snow leaves where the wind speeds up and settles where it slows. The wind over the
	// ground is weighted by slope along it and curvature (MicroMet, Liston and Elder 2006):
	// W = 1 + 0.5 Os + 0.5 Oc, each scaled to +-0.5 over the map. Snow under a frozen crust, wet snow and
	// snow under trees stays.
	last_blown = 0.0f;
	last_blown_max = 0.0f;
	const float u = p_wind.length();
	if (p_dt_h <= 0.0f || u < 1.0f) {
		return;
	}
	const int n = resolution;
	const int nn = n * n;
	const float cs = _cell_size();
	const Vector2 dir = p_wind / u;
	auto cell = [n](int x, int z) { return CLAMP(z, 0, n - 1) * n + CLAMP(x, 0, n - 1); };
	// Terrain weights.
	LocalVector<float> slope, curve;
	slope.resize(nn);
	curve.resize(nn);
	const int eta = MAX(int(Math::round(MAX(wind_curvature_length, 2.0f * cs) / cs)), 1);
	// Floors keep flat ground flat (MicroMet scales by the map's own extremes; noise would be amplified).
	float smax = 0.1f, cmax = 0.002f;
	for (int z = 0; z < n; z++) {
		for (int x = 0; x < n; x++) {
			const int i = z * n + x;
			const float gx = (cell_ground[cell(x + 1, z)] - cell_ground[cell(x - 1, z)]) / (2.0f * cs);
			const float gz = (cell_ground[cell(x, z + 1)] - cell_ground[cell(x, z - 1)]) / (2.0f * cs);
			slope[i] = Math::atan(gx * dir.x + gz * dir.y); // rising along the wind: speeds up
			const float h = cell_ground[i];
			const float d = eta * cs;
			const float d2 = d * Math::SQRT2;
			curve[i] = 0.25f * ((h - 0.5f * (cell_ground[cell(x, z - eta)] + cell_ground[cell(x, z + eta)])) / (2.0f * d) + (h - 0.5f * (cell_ground[cell(x - eta, z)] + cell_ground[cell(x + eta, z)])) / (2.0f * d) + (h - 0.5f * (cell_ground[cell(x - eta, z - eta)] + cell_ground[cell(x + eta, z + eta)])) / (2.0f * d2) + (h - 0.5f * (cell_ground[cell(x - eta, z + eta)] + cell_ground[cell(x + eta, z - eta)])) / (2.0f * d2));
			smax = MAX(smax, Math::abs(slope[i]));
			cmax = MAX(cmax, Math::abs(curve[i]));
		}
	}
	// The wind over the snow's own relief (follow-up 48): drifts, banks and dunes change the shear on the snow as
	// hills do on sand, from linearised flow (DuneShear, shared with the dune sea: Kroy, Sauermann and Herrmann
	// 2002). The shear peaks upwind of a crest and drops past it, so a drift's stoss erodes, its lee fills and it
	// creeps downwind; smoothing over the saturation length keeps drifts shorter than that from steepening. The
	// wind speed goes as the root of the shear (u* ~ sqrt(tau)). Snow relief only: the ground has its own weights.
	// The relief changes slowly: the shear is kept for 5 minutes of weather, until the wind turns 5 degrees or the
	// square moves.
	std::vector<float> &relief_shear = relief_shear_cache;
	relief_shear_age += p_dt_h;
	const bool relief_due = relief_shear.size() != size_t(nn) || relief_shear_age >= 5.0f / 60.0f || relief_shear_origin != origin_cell || relief_shear_dir.dot(dir) < 0.996f;
	if (!drift_relief) {
		relief_shear.clear();
	} else if (relief_due) {
		relief_shear.clear();
		relief_shear_age = 0.0f;
		relief_shear_origin = origin_cell;
		relief_shear_dir = dir;
		std::vector<float> relief(nn), relief_slope;
		float relief_max = 0.0f;
		for (int i = 0; i < nn; i++) {
			relief[i] = cell_swe[i] > SWE_MIN ? cell_swe[i] / MAX(cell_density[i], 30.0f) / cs : 0.0f; // in cells
			relief_max = MAX(relief_max, relief[i]);
		}
		if (relief_max > 0.0f) {
			auto height = [&](double p_x, double p_z) {
				const double fx = Math::floor(p_x), fz = Math::floor(p_z);
				const int x0 = CLAMP(int(fx), 0, n - 1), z0 = CLAMP(int(fz), 0, n - 1);
				const int x1 = MIN(x0 + 1, n - 1), z1 = MIN(z0 + 1, n - 1);
				const double tx = CLAMP(p_x - fx, 0.0, 1.0), tz = CLAMP(p_z - fz, 0.0, 1.0);
				const double a = relief[z0 * n + x0] * (1.0 - tx) + relief[z0 * n + x1] * tx;
				const double b = relief[z1 * n + x0] * (1.0 - tx) + relief[z1 * n + x1] * tx;
				return a * (1.0 - tz) + b * tz;
			};
			const float ls = DRIFT_SATURATION / cs;
			const int passes = CLAMP(int(Math::round(2.0f * ls * ls)), 0, 8);
			DuneShear::slope_along(height, n, n, dir.x, dir.y, passes, false, relief_slope);
			relief_shear.resize(nn);
			DuneShear::rows(relief_slope.data(), n, n, dir.x, dir.y, DRIFT_SHEAR_A, DRIFT_SHEAR_B, MIN(32, n), false, 0, n, relief_shear.data());
		}
	}
	// Flux and what each cell can give (the loose snow over a crust; none under a frozen surface crust).
	LocalVector<float> flux, loose, uloc;
	flux.resize(nn);
	loose.resize(nn);
	uloc.resize(nn);
	float qmax = 0.0f;
	const Inputs &in = inputs;
	for (int i = 0; i < nn; i++) {
		flux[i] = 0.0f;
		loose[i] = 0.0f;
		uloc[i] = 0.0f;
		const float swe = cell_swe[i];
		if (swe <= SWE_MIN || cell_liquid[i] > 0.01f || (cell_canopy.size() == nn && cell_canopy[i] > 0.2f)) {
			continue;
		}
		float give = swe;
		if (crust_enabled && cell_crust[i] > 0.0f) {
			give = MIN(cell_crust_burial[i] * cell_density[i], swe);
		}
		const float t = in.temperature - lapse_rate * 0.001f * (cell_ground[i] - in.ref_height);
		float w = 1.0f + 0.5f * (0.5f * slope[i] / smax) + 0.5f * (0.5f * curve[i] / cmax);
		if (!relief_shear.empty()) {
			w *= Math::sqrt(relief_shear[i]);
		}
		// Behind rocks, walls, buildings and vehicles the wind slows below what lifts snow and the snow carried in
		// drops (SnowTran-3D's flux divergence, Liston and Sturm 1998): a lee drift.
		if (obstacle_active && cell_lee.size() == nn) {
			w *= cell_lee[i];
		}
		flux[i] = transport_flux(u * w, t, cell_density[i]);
		loose[i] = give;
		uloc[i] = t < 0.0f ? u * w : 0.0f;
		qmax = MAX(qmax, flux[i]);
	}
	last_blown_max = qmax;
	if (qmax <= 0.0f) {
		return;
	}
	// Substeps keep each one under 2 mm of snow water through a face.
	const float dt = p_dt_h * 3600.0f;
	const float ax = Math::abs(dir.x), az = Math::abs(dir.y);
	const float faces = ax + az;
	const int sub = CLAMP(int(Math::ceil(qmax * dt * faces / cs / 2.0f)), 1, 256);
	const float dts = dt / sub;
	const int sx = dir.x >= 0.0f ? 1 : -1;
	const int sz = dir.y >= 0.0f ? 1 : -1;
	float *swe = cell_swe.ptrw();
	float *rho = cell_density.ptrw();
	float *burial = cell_crust_burial.ptrw();
	float *slab = wind_packing_enabled && cell_wind_slab.size() == nn ? cell_wind_slab.ptrw() : nullptr;
	// Sastrugi relief as (x, z) pairs, written back after the substeps.
	LocalVector<float> sas_v;
	float *sas = nullptr;
	if (sastrugi_enabled && cell_sas_x.size() == nn && cell_sas_z.size() == nn) {
		sas_v.resize(nn * 2);
		for (int i = 0; i < nn; i++) {
			sas_v[i * 2] = cell_sas_x[i];
			sas_v[i * 2 + 1] = cell_sas_z[i];
		}
		sas = sas_v.ptr();
	}
	// Hard surfaces at the start (wind-packed with the slab on top, or dense sintered snow) and the snow the
	// saltation carried off each cell (its scour), whatever came in.
	LocalVector<uint8_t> hard;
	LocalVector<float> scour, swe_start;
	if (sas) {
		hard.resize(nn);
		scour.resize(nn);
		swe_start.resize(nn);
		for (int i = 0; i < nn; i++) {
			hard[i] = swe[i] > SWE_MIN && (rho[i] >= SASTRUGI_HARD || (slab && slab[i] > 0.005f && cell_slab_cover[i] < 0.005f)) ? 1 : 0;
			scour[i] = 0.0f;
			swe_start[i] = swe[i];
		}
	}
	LocalVector<float> gain;
	gain.resize(nn);
	double moved = 0.0;
	for (int k = 0; k < sub; k++) {
		for (int i = 0; i < nn; i++) {
			gain[i] = 0.0f;
		}
		for (int z = 0; z < n; z++) {
			for (int x = 0; x < n; x++) {
				const int i = z * n + x;
				if (flux[i] <= 0.0f) {
					continue;
				}
				// mm of snow water (kg/m2) through the downwind faces, at most what the cell can give.
				const float out = MIN(flux[i] * dts * faces / cs, loose[i]);
				if (sas) {
					scour[i] += out;
				}
				const float to_x = out * ax / faces;
				const float to_z = out * az / faces;
				gain[i] -= out;
				const int xn = x + sx, zn = z + sz;
				if (xn >= 0 && xn < n) {
					gain[z * n + xn] += to_x;
				}
				if (zn >= 0 && zn < n) {
					gain[zn * n + x] += to_z;
				}
				// The upwind edge: snow comes in from outside as from a cell like this one.
				if (x - sx < 0 || x - sx >= n) {
					gain[i] += to_x;
				}
				if (z - sz < 0 || z - sz >= n) {
					gain[i] += to_z;
				}
				moved += out;
			}
		}
		for (int i = 0; i < nn; i++) {
			const float g = gain[i];
			if (g == 0.0f) {
				continue;
			}
			const float old = swe[i] > SWE_MIN ? swe[i] : 0.0f;
			if (g > 0.0f) {
				// Laid down wind-packed (wind slab); it buries a crust.
				const float depth_old = old > 0.0f ? old / rho[i] : 0.0f;
				if (slab) {
					slab[i] = (old > 0.0f ? slab[i] : 0.0f) + g / WIND_SNOW_DENSITY;
					cell_slab_cover.write[i] = 0.0f;
				}
				if (cell_hoar_layer.size() == nn && cell_hoar_layer[i] > 0.0f) {
					cell_hoar_burial.write[i] += g / WIND_SNOW_DENSITY; // wind loading over buried surface hoar
				}
				if (sas) {
					const Vector2 v = sastrugi_step(Vector2(sas[i * 2], sas[i * 2 + 1]), Vector2(), 0.0f, g / WIND_SNOW_DENSITY, depth_old + g / WIND_SNOW_DENSITY, false);
					sas[i * 2] = v.x;
					sas[i * 2 + 1] = v.y;
				}
				swe[i] = old + g;
				rho[i] = swe[i] / MAX(depth_old + g / WIND_SNOW_DENSITY, 1e-6f);
				if (g > 0.05f) {
					_layers_push(i, g, WIND_SNOW_DENSITY, LAYER_WIND); // a trace is left to the next update's match
				}
				if (cell_crust[i] > 0.0f) {
					burial[i] += g / WIND_SNOW_DENSITY;
				}
				// A fresh deposit can blow on.
				if (flux[i] > 0.0f || cell_crust[i] <= 0.0f) {
					loose[i] += g;
				}
			} else {
				// Taken from the top: the pack below keeps its density. A slab under softer snow loses what the
				// wind takes past that snow.
				if (cell_hoar_layer.size() == nn && cell_hoar_layer[i] > 0.0f) {
					cell_hoar_burial.write[i] = MAX(cell_hoar_burial[i] + g / MAX(rho[i], 30.0f), 0.0f);
				}
				if (slab && old > 0.0f) {
					const float eroded = -g / MAX(rho[i], 30.0f);
					const float soft = cell_slab_cover[i];
					slab[i] = MAX(slab[i] - MAX(eroded - soft, 0.0f), 0.0f);
					cell_slab_cover.write[i] = MAX(soft - eroded, 0.0f);
				}
				swe[i] = MAX(old + g, 0.0f);
				_layers_take(i, MIN(-g, old));
				loose[i] = MAX(loose[i] + g, 0.0f);
				if (cell_crust[i] > 0.0f) {
					burial[i] = MAX(burial[i] + g / MAX(rho[i], 30.0f), 0.0f);
				}
				if (swe[i] <= SWE_MIN) {
					swe[i] = 0.0f;
					cell_liquid.write[i] = 0.0f;
					rho[i] = 100.0f;
				}
			}
		}
	}
	last_blown = float(moved / nn);
	if (drift_relief) {
		_repose(flux, 8);
	}
	if (sas) {
		// The saltating snow abrades a hard surface where it is undersaturated, carving it (Filhol and Sturm
		// 2015): a share SASTRUGI_RATE of the scoured snow's depth goes into the relief. Where the drift builds
		// up instead (a net gain) it buries them: no carving there.
		for (int i = 0; i < nn; i++) {
			if (hard[i] && scour[i] > 0.0f && swe[i] > SWE_MIN && swe[i] <= swe_start[i] + 0.05f) {
				const float r = MAX(rho[i], 30.0f);
				const Vector2 v = sastrugi_step(Vector2(sas[i * 2], sas[i * 2 + 1]), dir, SASTRUGI_RATE * scour[i] / r, 0.0f, swe[i] / r, true);
				sas[i * 2] = v.x;
				sas[i * 2 + 1] = v.y;
			}
		}
		float *sx = cell_sas_x.ptrw();
		float *sz = cell_sas_z.ptrw();
		for (int i = 0; i < nn; i++) {
			sx[i] = sas[i * 2];
			sz[i] = sas[i * 2 + 1];
		}
	}
	if (slab) {
		_wind_pack(p_dt_h, uloc);
	}
}

// The blown snow's angle of repose (follow-up 48): where the wind has steepened the snow's own relief past
// DRIFT_REPOSE the loose grains avalanche down it, as on a dune's slip face (the minimal dune model, Kroy,
// Sauermann and Herrmann 2002), so a drift's lee keeps a slip face instead of a spike. The snow's own relief only
// (snow on steep ground is SnowSlide's), and only where the wind moves snow (dry, loose, not under trees); mass is kept.
void SnowCover3D::_repose(const LocalVector<float> &p_flux, int p_passes) {
	const int n = resolution;
	const int nn = n * n;
	const float cs = _cell_size();
	const float rise = Math::tan(Math::deg_to_rad(DRIFT_REPOSE)) * cs;
	float *swe = cell_swe.ptrw();
	float *rho = cell_density.ptrw();
	LocalVector<float> depth, take, put_swe, put_depth;
	depth.resize(nn);
	take.resize(nn);
	put_swe.resize(nn);
	put_depth.resize(nn);
	static const int DX[4] = { 1, -1, 0, 0 };
	static const int DZ[4] = { 0, 0, 1, -1 };
	for (int pass = 0; pass < p_passes; pass++) {
		bool moved = false;
		for (int i = 0; i < nn; i++) {
			depth[i] = swe[i] > SWE_MIN ? swe[i] / MAX(rho[i], 30.0f) : 0.0f;
			take[i] = 0.0f;
			put_swe[i] = 0.0f;
			put_depth[i] = 0.0f;
		}
		for (int z = 0; z < n; z++) {
			for (int x = 0; x < n; x++) {
				const int i = z * n + x;
				if (p_flux[i] <= 0.0f || depth[i] <= 0.0f) {
					continue;
				}
				const float hi = depth[i];
				for (int k = 0; k < 4; k++) {
					const int xn = x + DX[k], zn = z + DZ[k];
					if (xn < 0 || zn < 0 || xn >= n || zn >= n) {
						continue;
					}
					const int j = zn * n + xn;
					const float excess = hi - depth[j] - rise;
					if (excess <= 0.0f) {
						continue;
					}
					// A fifth of the excess each pass (four neighbours can draw at once), at most what lies above.
					const float d = MIN(0.2f * excess, 0.25f * depth[i]);
					take[i] += d;
					put_swe[j] += d * rho[i];
					put_depth[j] += d;
					moved = true;
				}
			}
		}
		if (!moved) {
			break;
		}
		for (int i = 0; i < nn; i++) {
			if (take[i] <= 0.0f && put_swe[i] <= 0.0f) {
				continue;
			}
			const float d = MAX(depth[i] - take[i], 0.0f);
			const float w = MAX(swe[i] > SWE_MIN ? swe[i] - take[i] * rho[i] : 0.0f, 0.0f) + put_swe[i];
			const float dd = d + put_depth[i];
			if (w <= SWE_MIN || dd <= 0.0f) {
				swe[i] = 0.0f;
				rho[i] = 100.0f;
				continue;
			}
			if (depth[i] <= 0.0f) {
				cell_albedo.write[i] = ALBEDO_FRESH;
			}
			swe[i] = w;
			rho[i] = w / dd;
		}
	}
}

// Wind packing (follow-up 45): where the surface drifts (Crocus's SI > 0 at the cell's own wind) its top 10 cm
// settle toward 350 kg/m3 (wind_packing_step) and become wind slab. The pack is one layer, so the top takes the
// pack's density before it packs. Not under a frozen crust's surface, wet snow or trees (as the drift).
void SnowCover3D::_wind_pack(float p_dt_h, const LocalVector<float> &p_wind) {
	const int nn = resolution * resolution;
	float *swe = cell_swe.ptrw();
	float *rho = cell_density.ptrw();
	float *slab = cell_wind_slab.ptrw();
	const float span = ALBEDO_FRESH - ALBEDO_OLD_DRY;
	for (int i = 0; i < nn; i++) {
		const float uw = p_wind[i];
		if (uw <= 0.0f || swe[i] <= SWE_MIN || cell_liquid[i] > 0.01f || (cell_canopy.size() == nn && cell_canopy[i] > 0.2f)) {
			continue;
		}
		const float depth = swe[i] / MAX(rho[i], 30.0f);
		float top = MIN(depth, 0.1f);
		if (crust_enabled && cell_crust[i] > 0.0f) {
			top = MIN(top, cell_crust_burial[i]);
		}
		if (top <= 0.0f) {
			continue;
		}
		// The top's own density: its layers' (new snow over an old pack is light and drifts) or the bulk's.
		const float r = _top_density(i, top);
		const float fresh = CLAMP((cell_albedo[i] - ALBEDO_OLD_DRY) / span, 0.0f, 1.0f);
		const float packed = wind_packing_step(r, uw, fresh, p_dt_h);
		if (packed <= r) {
			continue;
		}
		const float top_after = top * r / packed;
		const float depth_after = depth - top + top_after;
		rho[i] = swe[i] / MAX(depth_after, 1e-6f);
		slab[i] = MIN(MAX(slab[i], top_after), depth_after);
		cell_slab_cover.write[i] = 0.0f; // the top is the slab now
		if (layers_enabled && layer_n.size() == resolution * resolution && layer_n[i] > 0) {
			// The packed top (its top metres, through as many layers as it takes) is wind slab: the layers in it
			// turn to wind slab and pack by the same factor, and one reaching deeper is split there.
			int k = layer_n[i];
			const int b = i * PACK_LAYERS;
			float left = top;
			for (int j = k - 1; j >= 0 && left > 0.01f; j--) {
				const float rj = MAX(layer_rho[b + j], 30.0f);
				const float dj = layer_swe[b + j] / rj;
				if (layer_kind[b + j] != LAYER_WIND && dj > 1.5f * left) {
					if (k >= PACK_LAYERS) {
						break; // no room to split: the layer stays as it is
					}
					// Split: the part above the packed depth goes on top of what is left of it.
					for (int m = k; m > j + 1; m--) {
						layer_swe.write[b + m] = layer_swe[b + m - 1];
						layer_rho.write[b + m] = layer_rho[b + m - 1];
						layer_age.write[b + m] = layer_age[b + m - 1];
						layer_liq.write[b + m] = layer_liq[b + m - 1];
						layer_kind.write[b + m] = layer_kind[b + m - 1];
					}
					const float part = left * rj;
					layer_liq.write[b + j + 1] = layer_liq[b + j] * part / MAX(layer_swe[b + j], 1e-6f);
					layer_liq.write[b + j] -= layer_liq[b + j + 1];
					layer_swe.write[b + j] -= part;
					layer_swe.write[b + j + 1] = part;
					layer_rho.write[b + j + 1] = MIN(rj * packed / r, 917.0f);
					layer_age.write[b + j + 1] = layer_age[b + j];
					layer_kind.write[b + j + 1] = LAYER_WIND;
					k++;
					layer_n.write[i] = uint8_t(k);
					break;
				}
				layer_kind.write[b + j] = LAYER_WIND;
				layer_rho.write[b + j] = MIN(rj * packed / r, 917.0f); // packed as the bulk's top was
				left -= dj;
			}
		}
		if (crust_enabled && cell_crust[i] > 0.0f) {
			cell_crust_burial.write[i] = MAX(cell_crust_burial[i] - (top - top_after), 0.0f);
		}
	}
}

// Spindrift (follow-up 42): snow blown over a sharp lee crest leaves the ground where the flow separates and
// streams out in a plume, the suspended part of the drift carried on by the wind while its grains settle (about
// 0.3-0.5 m/s; Schmidt 1982) and sublimate (Schmidt 1972; Thorpe and Mason 1966). The source per crest cell is the
// drift flux (Pomeroy and Male's saltation and suspension, transport_flux) at the crest's sped-up wind (Taylor and
// Lee 1984, as the cornices) times the cell's width; as SnowDrift3D (Schneiderbauer and Prokop 2011) the plume is
// then a scalar carried by the wind with a settling speed, here NVIDIA Flow's gas with a slight negative buoyancy.
// Loose dry snow only: none from wet snow, a bare crust or under the threshold wind.
void SnowCover3D::_find_spindrift(Vector2 p_wind) {
	spindrift_next.clear();
	spindrift_rate = 0.0f;
	const int n = resolution;
	const float u = p_wind.length();
	if (!spindrift_enabled || owner || driven || u < 1.0f || cell_swe.size() != n * n) {
		return;
	}
	const float cs = _cell_size();
	const Vector2 dir = p_wind / u;
	const float tan_lee = Math::tan(Math::deg_to_rad(30.0f));
	auto cell = [&](int p_x, int p_z) { return CLAMP(p_z, 0, n - 1) * n + CLAMP(p_x, 0, n - 1); };
	const int sx = int(Math::round(dir.x)), sz = int(Math::round(dir.y));
	const float step = cs * Vector2(sx, sz).length();
	if (step <= 0.0f) {
		return;
	}
	// Crest cells with their flux first, then binned (8 m, doubled until the plumes fit max_spindrift), so the
	// emitters spread along the whole ridge instead of crowding its strongest stretch.
	struct Lip {
		int x, z;
		Vector3 at;
		float q;
		float speedup;
	};
	LocalVector<Lip> lips;
	const int ring = MAX(4, int(Math::ceil(64.0f / cs)));
	for (int z = 1; z < n - 1; z++) {
		for (int x = 1; x < n - 1; x++) {
			const int i = z * n + x;
			const float swe = cell_swe[i];
			if (swe <= SWE_MIN || cell_liquid[i] > 0.01f || (cell_canopy.size() == n * n && cell_canopy[i] > 0.2f)) {
				continue;
			}
			if (crust_enabled && cell_crust[i] > 0.0f && cell_crust_burial[i] < 0.01f) {
				continue; // a bare crust: nothing loose to lift
			}
			const float h = cell_ground[i];
			const int iu = cell(x - sx, z - sz);
			const int id1 = cell(x + sx, z + sz);
			const int id2 = cell(x + 2 * sx, z + 2 * sz);
			if (!(cell_ground[iu] <= h + 0.25f * step && cell_ground[id1] <= h && (h - cell_ground[id2]) >= tan_lee * 2.0f * step)) {
				continue;
			}
			float around = 0.0f;
			for (int k = 0; k < 8; k++) {
				static const int RX[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };
				static const int RZ[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
				around += cell_ground[cell(x + RX[k] * ring, z + RZ[k] * ring)] / 8.0f;
			}
			const float speedup = 1.0f + CLAMP(1.6f * (h - around) / (ring * cs), 0.0f, 0.6f);
			const float t = inputs.temperature - lapse_rate * 0.001f * (h - inputs.ref_height);
			const float q = transport_flux(u * speedup, t, cell_density[i]) * cs; // kg/s over the cell's width
			if (q <= 0.0f) {
				continue;
			}
			spindrift_rate += q;
			const Vector3 c = _cell_center(x, z);
			const float top = h + swe / MAX(cell_density[i], 30.0f);
			// Where the plume leaves: half a cell past the lip and a metre up.
			lips.push_back({ x, z, Vector3(c.x + 0.5f * cs * dir.x, top + 1.0f, c.z + 0.5f * cs * dir.y), q, speedup });
		}
	}
	if (lips.is_empty()) {
		return;
	}
	int bin = MAX(1, int(Math::round(8.0f / cs)));
	while (true) {
		const int bn = (n + bin - 1) / bin;
		LocalVector<SnowPowderCloud3D::Source> bins;
		bins.resize(bn * bn);
		LocalVector<float> sp;
		sp.resize(bn * bn);
		for (const Lip &l : lips) {
			const int b = (l.z / bin) * bn + l.x / bin;
			SnowPowderCloud3D::Source &s = bins[b];
			s.position = (s.position * s.rate + l.at * l.q) / (s.rate + l.q);
			sp[b] = (sp[b] * s.rate + l.speedup * l.q) / (s.rate + l.q);
			s.rate += l.q;
		}
		spindrift_next.clear();
		for (uint32_t b = 0; b < bins.size(); b++) {
			SnowPowderCloud3D::Source s = bins[b];
			if (s.rate <= 0.0f) {
				continue;
			}
			// Near the surface the wind is some 60% of the 10 m wind (log profile at 1-2 m); the separating flow
			// lifts the drift off the lip (the plume rises over the lee eddy).
			s.velocity = Vector3(dir.x, 0.0f, dir.y) * (0.6f * u * sp[b]) + Vector3(0.0f, 0.1f * u, 0.0f);
			s.radius = CLAMP(0.5f * bin * cs, 1.0f, 6.0f);
			spindrift_next.push_back(s);
		}
		if ((int)spindrift_next.size() <= max_spindrift || bin >= n) {
			break;
		}
		bin *= 2;
	}
	spindrift_next.sort_custom<SpindriftStronger>();
	if ((int)spindrift_next.size() > max_spindrift) {
		spindrift_next.resize(max_spindrift);
	}
}

// Sublimation of the suspended grains: a 100 um grain in air at 70% humidity lasts about a minute (Schmidt
// 1972), faster in drier air, so the plume thins over tens of seconds to a few minutes.
float SnowCover3D::spindrift_fade(float p_humidity) {
	return 0.01f + 0.08f * CLAMP(1.0f - p_humidity, 0.0f, 1.0f);
}

void SnowCover3D::_feed_spindrift() {
	if (owner || driven) {
		return;
	}
	spindrift_sources = spindrift_next;
	if (!spindrift_enabled) {
		spindrift_sources.clear();
	}
	if (!spindrift_node && spindrift_sources.is_empty()) {
		return;
	}
	if (!spindrift_node) {
		spindrift_node = memnew(SnowPowderCloud3D);
		spindrift_node->set_name("Spindrift");
		// Thin and light: a few tens of grams of snow per cubic metre against the powder cloud's kilograms, settling
		// slowly, so the gas barely sinks and drifts far on the wind.
		spindrift_node->set_heaviness(0.05f);
		spindrift_node->set_updraft(0.0f);
		spindrift_node->set_smoke_per_kg(0.1f);
		spindrift_node->set_max_sources(max_spindrift);
		spindrift_node->set_divergence(0.0f);
		spindrift_node->set_grid(2.0f, 4096); // plumes hundreds of metres long: coarser than the powder cloud's 0.75 m
		add_child(spindrift_node, false, INTERNAL_MODE_FRONT);
	}
	spindrift_node->feed(spindrift_sources);
	if (Node3D *f = spindrift_node->get_flow()) {
		f->set("smoke_fade", spindrift_fade(inputs.humidity));
		f->set("smoke_density", 0.5f);
		f->set("vorticity", 0.5f); // low enough that the plume keeps the wind's speed and runs long (PhysX f09677c98a)
	}
}

void SnowCover3D::set_spindrift_enabled(bool p_enabled) {
	spindrift_enabled = p_enabled;
	if (!p_enabled && spindrift_node) {
		spindrift_sources.clear();
		spindrift_node->feed(spindrift_sources);
	}
}

Array SnowCover3D::get_spindrift_sources() const {
	Array out;
	for (const SnowPowderCloud3D::Source &s : spindrift_sources) {
		Dictionary d;
		d["position"] = s.position;
		d["velocity"] = s.velocity;
		d["rate"] = s.rate;
		out.push_back(d);
	}
	return out;
}

void SnowCover3D::_grow_cornices(float p_dt_h, Vector2 p_wind) {
	// A cornice builds where snow blown over a crest settles in the lee eddy and creeps out as a slab
	// (Naito and Kobayashi 1986; Yu et al. 2023). Per cell on a lee edge: the crest is not overtopped upwind,
	// the ground falls away at 30 degrees or more downwind, and the snow upwind is loose and dry. It grows
	// along the wind at up to 1.6 cm an hour (field rates of Hancock et al. 2020), shrinks when the wind is
	// past twice the threshold, and wastes back in thaws, 0.5 mm an hour per degree above the melt threshold
	// (a snow degree-day factor of 5 mm water a day per degree over a 450 kg/m3 slab).
	const int n = resolution;
	if (cell_cornice_x.size() != n * n) {
		return;
	}
	constexpr float RATE = 0.016f; // m/h at the best wind
	constexpr float MAX_LENGTH = 12.0f; // m
	const float cs = _cell_size();
	const float tan_lee = Math::tan(Math::deg_to_rad(30.0f));
	const float u = p_wind.length();
	const Vector2 dir = u > 0.1f ? p_wind / u : Vector2();
	float *cx = cell_cornice_x.ptrw();
	float *cz = cell_cornice_z.ptrw();
	if (cell_cornice_crack.size() != n * n) {
		cell_cornice_crack.resize(n * n);
		cell_cornice_crack.fill(0.0f);
	}
	float *crack = cell_cornice_crack.ptrw();
	auto cell = [&](int p_x, int p_z) { return CLAMP(p_z, 0, n - 1) * n + CLAMP(p_x, 0, n - 1); };
	for (int z = 1; z < n - 1; z++) {
		for (int x = 1; x < n - 1; x++) {
			const int i = z * n + x;
			if (cell_swe[i] <= SWE_MIN) {
				cx[i] = 0.0f;
				cz[i] = 0.0f;
				crack[i] = 0.0f;
				continue;
			}
			bool has = cx[i] != 0.0f || cz[i] != 0.0f;
			if (!has) {
				crack[i] = 0.0f;
			}
			const float h = cell_ground[i];
			const float t = inputs.temperature - lapse_rate * 0.001f * (h - inputs.ref_height);
			if (has && t > melt_threshold) {
				const float len = Vector2(cx[i], cz[i]).length();
				const float k = MAX(len - 0.0005f * (t - melt_threshold) * p_dt_h, 0.0f) / MAX(len, 1e-6f);
				cx[i] *= k;
				cz[i] *= k;
			}
			if (has && cornice_failure_enabled) {
				// The overhang creeps over its root: the tension crack behind it opens, faster when warm, wet
				// or long, and the slab falls once its root cannot hold it (follow-up 6).
				const float len = Vector2(cx[i], cz[i]).length();
				const float liquid = cell_liquid[i] / MAX(cell_swe[i], 1.0f);
				crack[i] += cornice_crack_rate(len, crack[i], t, liquid) * p_dt_h / 24.0f;
				if (len >= 0.25f && cornice_root_stress(len, crack[i], liquid) > cornice_strength(liquid)) {
					_fail_cornice(i, len, Vector2(cx[i], cz[i]) / len);
					cx[i] = 0.0f;
					cz[i] = 0.0f;
					crack[i] = 0.0f;
					continue;
				}
			}
			if (u < 0.1f || t >= 0.0f) {
				continue;
			}
			const int iu = cell(x - int(Math::round(dir.x)), z - int(Math::round(dir.y)));
			const int id1 = cell(x + int(Math::round(dir.x)), z + int(Math::round(dir.y)));
			const int id2 = cell(x + int(Math::round(2.0f * dir.x)), z + int(Math::round(2.0f * dir.y)));
			const float step = cs * Vector2(Math::round(dir.x), Math::round(dir.y)).length();
			const bool edge = cell_ground[iu] <= h + 0.25f * step && cell_ground[id1] <= h && (h - cell_ground[id2]) >= tan_lee * 2.0f * step;
			if (!edge) {
				continue;
			}
			// The wind speeds up over the crest: Taylor and Lee (1984), 1.6 H / L for a ridge of height H
			// over its half-width L, here the crest above the ground around it at a ring of 64 m or 4 cells.
			const int ring = MAX(4, int(Math::ceil(64.0f / cs)));
			float around = 0.0f;
			for (int k = 0; k < 8; k++) {
				static const int RX[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };
				static const int RZ[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
				around += cell_ground[cell(x + RX[k] * ring, z + RZ[k] * ring)] / 8.0f;
			}
			const float speedup = 1.0f + CLAMP(1.6f * (h - around) / (ring * cs), 0.0f, 0.6f); // at most 60%: steeper ridges separate
			const float r = u * speedup / SnowFall3D::blowing_threshold(t);
			if (r >= 2.0f && has) {
				// Scoured back.
				const float len = Vector2(cx[i], cz[i]).length();
				const float k = MAX(len - RATE * p_dt_h * MIN(r - 2.0f, 1.0f), 0.0f) / MAX(len, 1e-6f);
				cx[i] *= k;
				cz[i] *= k;
				continue;
			}
			const float g = cornice_growth_share(r);
			// Snow to carry: dry upwind, and loose (a light pack, or a fresh surface on a settled one).
			const bool loose = cell_density[iu] < 350.0f || cell_albedo[iu] > 0.78f;
			if (g <= 0.0f || cell_swe[iu] < 5.0f || !loose || cell_liquid[iu] > 0.05f * cell_swe[iu]) {
				continue;
			}
			Vector2 c = Vector2(cx[i], cz[i]) + dir * (g * RATE * p_dt_h);
			if (c.length() > MAX_LENGTH) {
				c = c.normalized() * MAX_LENGTH;
			}
			cx[i] = c.x;
			cz[i] = c.y;
			// Snow the wind lays on the cornice fills its crack and sinters over it (our pick: as fast as the
			// slab grows), so cracks open in calm, mild spells and mostly in spring (Vogel et al. 2012).
			crack[i] = MAX(crack[i] - g * RATE * p_dt_h, 0.0f);
		}
	}
}

void SnowCover3D::_feed_world() {
	// Two-way nesting (as nested weather models feed a fine grid back to its parent, WRF's feedback): each
	// world cell the square covers whole takes the square's state averaged over it, so what the finer
	// cells did (drifts, sheltered and sunny faces, slides, melt) is not lost when the square moves on
	// and the cells come back from the world tier. Water is averaged; density over the depth it makes
	// (the column's mass over its depth); albedo weighted by water.
	if (!world || owner || !origin_valid || cell_swe.size() != resolution * resolution) {
		return;
	}
	const float cs = _cell_size();
	const float wcs = world->_cell_size();
	if (wcs < cs * 1.5f) {
		return; // no coarser: nothing to average
	}
	const int n = resolution;
	const int wn = world->resolution;
	const Rect2 near = get_map_rect();
	const float *swe = cell_swe.ptr();
	const float *liq = cell_liquid.ptr();
	const float *rho = cell_density.ptr();
	const float *alb = cell_albedo.ptr();
	float *wswe = world->cell_swe.ptrw();
	float *wliq = world->cell_liquid.ptrw();
	float *wrho = world->cell_density.ptrw();
	float *walb = world->cell_albedo.ptrw();
	const int wx0 = MAX(int(Math::ceil(near.position.x / wcs)) - world->origin_cell.x, 0);
	const int wz0 = MAX(int(Math::ceil(near.position.y / wcs)) - world->origin_cell.y, 0);
	const int wx1 = MIN(int(Math::floor((near.position.x + near.size.x) / wcs)) - world->origin_cell.x, wn);
	const int wz1 = MIN(int(Math::floor((near.position.y + near.size.y) / wcs)) - world->origin_cell.y, wn);
	for (int wz = wz0; wz < wz1; wz++) {
		for (int wx = wx0; wx < wx1; wx++) {
			// The square's cells whose centres lie in this world cell.
			const float ax = (world->origin_cell.x + wx) * wcs;
			const float az = (world->origin_cell.y + wz) * wcs;
			const int x0 = MAX(int(Math::ceil(ax / cs - 0.5f)) - origin_cell.x, 0);
			const int z0 = MAX(int(Math::ceil(az / cs - 0.5f)) - origin_cell.y, 0);
			const int x1 = MIN(int(Math::ceil((ax + wcs) / cs - 0.5f)) - origin_cell.x, n);
			const int z1 = MIN(int(Math::ceil((az + wcs) / cs - 0.5f)) - origin_cell.y, n);
			double w_sum = 0.0;
			double l_sum = 0.0;
			double depth_sum = 0.0;
			double a_sum = 0.0;
			double ice_sum = 0.0;
			double white_sum = 0.0;
			int ice_n = 0;
			int count = 0;
			for (int z = z0; z < z1; z++) {
				for (int x = x0; x < x1; x++) {
					const int i = z * n + x;
					w_sum += swe[i];
					l_sum += liq[i];
					depth_sum += swe[i] / MAX(rho[i], 30.0f);
					a_sum += swe[i] * alb[i];
					if (cell_water.size() == n * n && cell_water[i] > -1e29f) {
						ice_sum += cell_ice[i];
						white_sum += cell_white_ice[i];
						ice_n++;
					}
					count++;
				}
			}
			if (count == 0) {
				continue;
			}
			const int wi = wz * wn + wx;
			if (ice_n > 0 && world->cell_ice.size() == wn * wn) {
				world->cell_ice.write[wi] = float(ice_sum / ice_n);
				world->cell_white_ice.write[wi] = float(white_sum / ice_n);
			}
			wswe[wi] = float(w_sum / count);
			wliq[wi] = float(l_sum / count);
			if (w_sum > 0.0 && depth_sum > 0.0) {
				wrho[wi] = CLAMP(float(w_sum / depth_sum), 30.0f, 917.0f);
				walb[wi] = float(a_sum / w_sum);
			}
		}
	}
}

void SnowCover3D::_seed_from_world(const Vector<uint8_t> *p_only) {
	// Cells arriving in the square start from the world tier's pack; the slide then sheds what the finer
	// slopes cannot hold (SnowSlide on the near cells), so cliffs come out bare and their feet loaded.
	if (!world || owner || !origin_valid) {
		return;
	}
	const Rect2 wr = world->get_map_rect();
	const int n = resolution;
	bool any = false;
	float *swe = cell_swe.ptrw();
	float *liq = cell_liquid.ptrw();
	float *rho = cell_density.ptrw();
	float *alb = cell_albedo.ptrw();
	for (int z = 0; z < n; z++) {
		for (int x = 0; x < n; x++) {
			const int i = z * n + x;
			if (p_only && !(*p_only)[i]) {
				continue;
			}
			const Vector3 c = _cell_center(x, z);
			const Vector2 xz(c.x, c.z);
			if (!wr.has_point(xz)) {
				continue;
			}
			const float w = world->_sample(world->cell_swe, xz);
			if (cell_ice.size() == n * n && world->cell_ice.size() == world->resolution * world->resolution) {
				cell_ice.write[i] = cell_water[i] > -1e29f ? world->_sample(world->cell_ice, xz) : 0.0f;
				cell_white_ice.write[i] = cell_water[i] > -1e29f ? MIN(world->_sample(world->cell_white_ice, xz), cell_ice[i]) : 0.0f;
			}
			if (w <= SWE_MIN) {
				swe[i] = 0.0f;
				liq[i] = 0.0f;
				rho[i] = 100.0f;
				alb[i] = ALBEDO_FRESH;
				cell_crust.write[i] = 0.0f;
				cell_crust_cycles.write[i] = 0.0f;
				cell_crust_burial.write[i] = 0.0f;
				cell_crust_wet.write[i] = 0.0f;
				continue;
			}
			swe[i] = w;
			if (world->cell_crust.size() == world->cell_swe.size()) {
				cell_crust.write[i] = world->_sample(world->cell_crust, xz);
				cell_crust_cycles.write[i] = Math::round(world->_sample(world->cell_crust_cycles, xz));
				cell_crust_burial.write[i] = MIN(world->_sample(world->cell_crust_burial, xz), w / rho[i]);
				cell_crust_wet.write[i] = world->_sample(world->cell_crust_wet, xz) > 0.5f ? 1.0f : 0.0f;
			}
			liq[i] = MIN(world->_sample(world->cell_liquid, xz), w);
			rho[i] = CLAMP(world->_sample(world->cell_density, xz), 30.0f, 917.0f);
			alb[i] = world->_sample(world->cell_albedo, xz);
			any = true;
		}
	}
	if (any && slide_enabled) {
		_slide();
	}
}

void SnowCover3D::_make_world() {
	_free_world();
	if (climate.is_null() || !climate->is_baked()) {
		return;
	}
	world = memnew(SnowCover3D);
	world->_setup_internal(this, climate->get_rect(), climate->get_resolution());
	const int nn = world->resolution * world->resolution;
	if (climate->get_swe().size() == nn) {
		world->cell_swe = climate->get_swe();
		world->cell_liquid = climate->get_liquid();
		world->cell_density = climate->get_density();
		world->cell_albedo = climate->get_albedo();
		if (climate->get_cornice_x().size() == nn) {
			world->cell_cornice_x = climate->get_cornice_x();
			world->cell_cornice_z = climate->get_cornice_z();
		}
		if (climate->get_ice().size() == nn && climate->get_white_ice().size() == nn) {
			world->cell_ice = climate->get_ice();
			world->cell_white_ice = climate->get_white_ice();
		}
	}
	world->_upload();
	_publish_world_globals();
}

void SnowCover3D::_free_world() {
	_finish_background(true);
	if (!world) {
		return;
	}
	world->_finish_sx(true);
	memdelete(world);
	world = nullptr;
	if (world_globals_added && active == this) {
		RenderingServer::get_singleton()->global_shader_parameter_set("snow_world_rect", Vector4());
	}
}

void SnowCover3D::_publish_world_globals() {
	if (!publish_shader_globals || active != this || !world || world->snow_texture.is_null()) {
		return;
	}
	if (!world_globals_added) {
		_set_global("snow_world_map", world->snow_texture->get_rid(), RSE::GLOBAL_VAR_TYPE_SAMPLER2D);
		world_globals_added = true;
	}
	const Rect2 r = world->get_map_rect();
	_set_global("snow_world_rect", Vector4(r.position.x, r.position.y, r.size.x, r.size.y), RSE::GLOBAL_VAR_TYPE_VEC4);
}

Error SnowCover3D::bake_climate(Rect2 p_rect, int p_resolution) {
	_finish_background(true);
	ERR_FAIL_COND_V_MSG(climate.is_null(), ERR_UNCONFIGURED, "SnowCover3D: set a climate to bake.");
	ERR_FAIL_COND_V(p_rect.size.x <= 0.0f || p_rect.size.y <= 0.0f || p_resolution < 4, ERR_INVALID_PARAMETER);
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	SnowCover3D *b = memnew(SnowCover3D);
	b->_setup_internal(this, p_rect, p_resolution);
	b->driven = true;
	b->driven_ref_height = climate->get_base_height();
	b->precipitation_gradient = climate->get_precipitation_gradient();
	b->slide_interval = 0.0f;
	climate->start_weather();
	const int step = CLAMP(climate->get_step_hours(), 1, 24);
	for (int d = 0; d < climate->get_days(); d++) {
		for (int hr = 0; hr < 24; hr += step) {
			const SnowClimate::Hour w = climate->weather_at(d, hr + 0.5f * step);
			b->lapse_rate = climate->seasonal_lapse_rate(float((climate->get_start_day() - 1 + d) % 365) + 1.0f, lapse_rate);
			b->temperature = w.temperature;
			b->humidity = w.humidity;
			b->precipitation_rate = w.precipitation;
			b->driven_wind = w.wind;
			b->driven_sun = w.to_sun;
			b->driven_direct = w.direct_normal;
			b->_update(step * 3600.0, true);
		}
	}
	climate->set_result(b->get_map_rect(), b->resolution, b->cell_swe, b->cell_liquid, b->cell_density, b->cell_albedo, b->cell_cornice_x, b->cell_cornice_z);
	climate->set_ice(b->cell_ice);
	climate->set_white_ice(b->cell_white_ice);
	memdelete(b);
	print_verbose(vformat("SnowCover3D: baked %d days of climate on %d x %d cells in %.1f s.", climate->get_days(), p_resolution, p_resolution, (OS::get_singleton()->get_ticks_usec() - t0) / 1e6));
	if (is_inside_tree()) {
		_make_world();
		_seed_from_world(nullptr);
		_upload();
	}
	return OK;
}

Rect2 SnowCover3D::get_world_rect() const {
	return world ? world->get_map_rect() : Rect2();
}

float SnowCover3D::canopy_capacity(float p_closure, float p_snow_density) {
	// Hedstrom and Pomeroy (1998). Leaf area from the closure by Beer's law with extinction 0.5, the
	// vegetation's own canopy model (closure 1 - exp(-0.5 LAI); 0.85 is LAI 3.8).
	const float lai = -2.0f * Math::log(1.0f - CLAMP(p_closure, 0.0f, 0.98f));
	return 5.9f * (0.27f + 46.0f / MAX(p_snow_density, 30.0f)) * lai;
}

float SnowCover3D::get_canopy_load_at(Vector3 p_position) const {
	return pub.canopy_load.size() == resolution * resolution ? MAX(_sample(pub.canopy_load, Vector2(p_position.x, p_position.z)), 0.0f) : 0.0f;
}

float SnowCover3D::get_canopy_capacity_at(Vector3 p_position) const {
	return cell_canopy.size() == resolution * resolution ? canopy_capacity(_sample(cell_canopy, Vector2(p_position.x, p_position.z)), 100.0f) : 0.0f;
}

float SnowCover3D::get_world_swe_at(Vector3 p_position) const {
	return world ? world->get_swe_at(p_position) : 0.0f;
}

float SnowCover3D::get_world_depth_at(Vector3 p_position) const {
	return world ? world->get_depth_at(p_position) : 0.0f;
}

Vector2 SnowCover3D::get_cornice_at(Vector3 p_position) const {
	const SnowCover3D *c = world ? world : (cornices_enabled ? this : nullptr);
	if (!c || c->pub.cornice_x.size() != c->resolution * c->resolution) {
		return Vector2();
	}
	const float cs = c->_cell_size();
	const int x = int(Math::floor(p_position.x / cs)) - c->origin_cell.x;
	const int z = int(Math::floor(p_position.z / cs)) - c->origin_cell.y;
	if (x < 0 || z < 0 || x >= c->resolution || z >= c->resolution) {
		return Vector2();
	}
	const int i = z * c->resolution + x;
	return Vector2(c->pub.cornice_x[i], c->pub.cornice_z[i]);
}

Ref<Texture2D> SnowCover3D::get_world_texture() const {
	return world ? Ref<Texture2D>(world->snow_texture) : Ref<Texture2D>();
}

void SnowCover3D::clear() {
	_finish_background(true);
	cell_swe.fill(0.0f);
	cell_liquid.fill(0.0f);
	cell_density.fill(100.0f);
	cell_albedo.fill(ALBEDO_FRESH);
	cell_runoff.fill(0.0f);
	for (Vector<float> *f : { &cell_crust, &cell_crust_cycles, &cell_crust_burial, &cell_crust_wet }) {
		f->fill(0.0f);
	}
	layer_n.fill(0);
	_upload();
}

void SnowCover3D::add_snow(float p_swe_mm, float p_density, bool p_world) {
	_finish_background(true);
	// A storm falls on the whole terrain: the world tier takes it too, so slopes outside the near square (where
	// get_depth_at reads the world tier) carry it.
	if (p_world && world && !owner) {
		world->add_snow(p_swe_mm, p_density, false);
	}
	if (cell_swe.size() != resolution * resolution) {
		_allocate();
		_follow(_follow_position());
	}
	const float rho_new = CLAMP(p_density, 30.0f, 917.0f);
	for (int i = 0; i < resolution * resolution; i++) {
		if (p_swe_mm > 0.0f && hoar_enabled) {
			_layers_bury_hoar(i);
		}
		const float depth_old = cell_swe[i] > SWE_MIN ? cell_swe[i] / cell_density[i] : 0.0f;
		const float swe = (cell_swe[i] > SWE_MIN ? cell_swe[i] : 0.0f) + MAX(p_swe_mm, 0.0f);
		cell_swe.write[i] = swe;
		cell_density.write[i] = swe / MAX(depth_old + MAX(p_swe_mm, 0.0f) / rho_new, 1e-6f);
		// Settled snow (200 kg/m3 and up) is an old layer; lighter is new snow.
		_layers_push(i, MAX(p_swe_mm, 0.0f), rho_new, rho_new >= 200.0f ? LAYER_OLD : LAYER_NEW);
		if (rho_new >= 200.0f && layer_n.size() == resolution * resolution && layer_n[i] > 0) {
			layer_age.write[i * PACK_LAYERS + layer_n[i] - 1] = LAYER_BOND_HOURS;
		}
		cell_albedo.write[i] = ALBEDO_FRESH;
		cell_new_depth.write[i] = MAX(p_swe_mm, 0.0f) / rho_new;
		if (cell_cups.size() == cell_swe.size()) {
			cell_cups.write[i] = MAX(cell_cups[i] - MAX(p_swe_mm, 0.0f) / rho_new, 0.0f); // new snow fills the hollows
		}
		if (cell_crust.size() == cell_swe.size() && cell_crust[i] > 0.0f) {
			cell_crust_burial.write[i] += MAX(p_swe_mm, 0.0f) / rho_new;
		}
		if (cell_hoar.size() == cell_swe.size() && p_swe_mm > 0.0f) {
			// The new snow buries the surface hoar (as an update's snowfall does).
			const float d = p_swe_mm / rho_new;
			if (cell_hoar[i] >= HOAR_MIN && (cell_hoar_layer[i] <= 0.0f || cell_hoar[i] >= 0.5f * cell_hoar_layer[i] || cell_hoar_burial[i] > 1.0f)) {
				cell_hoar_layer.write[i] = cell_hoar[i];
				cell_hoar_burial.write[i] = d;
				cell_hoar_age.write[i] = 0.0f;
			} else if (cell_hoar_layer[i] > 0.0f) {
				cell_hoar_burial.write[i] += d;
			}
			cell_hoar.write[i] = 0.0f;
		}
	}
	last_step_hours = 0.0f;
	last_drift = 0.0f;
	update_count++;
	if (avalanche_node) {
		avalanche_node->_track_recent(this);
	}
	_upload();
}

// A cone of wind-packed snow (a drift, a plough's bank, a pile), depth at its centre fading to none at the radius,
// on the near square.
void SnowCover3D::add_drift(const Vector3 &p_position, float p_radius, float p_depth, float p_density) {
	_finish_background(true);
	if (cell_swe.size() != resolution * resolution) {
		_allocate();
		_follow(_follow_position());
	}
	const float rho_new = CLAMP(p_density, 30.0f, 917.0f);
	const float r = MAX(p_radius, 0.01f);
	for (int z = 0; z < resolution; z++) {
		for (int x = 0; x < resolution; x++) {
			const Vector3 c = _cell_center(x, z);
			const float add = MAX(p_depth, 0.0f) * (1.0f - Vector2(c.x - p_position.x, c.z - p_position.z).length() / r);
			if (add <= 0.0f) {
				continue;
			}
			const int i = z * resolution + x;
			const float depth_old = cell_swe[i] > SWE_MIN ? cell_swe[i] / cell_density[i] : 0.0f;
			const float swe = (cell_swe[i] > SWE_MIN ? cell_swe[i] : 0.0f) + add * rho_new;
			cell_swe.write[i] = swe;
			cell_density.write[i] = swe / (depth_old + add);
			_layers_push(i, add * rho_new, rho_new, LAYER_WIND);
			if (cell_wind_slab.size() == cell_swe.size()) {
				cell_wind_slab.write[i] = (depth_old > 0.0f ? cell_wind_slab[i] : 0.0f) + add;
				cell_slab_cover.write[i] = 0.0f;
			}
			if (cell_crust.size() == cell_swe.size() && cell_crust[i] > 0.0f) {
				cell_crust_burial.write[i] += add;
			}
		}
	}
	last_step_hours = 0.0f;
	update_count++;
	_upload();
}

// A rounded heap of p_volume m3 of snow (debris that came to rest): a smooth (1 - (d/r)^2)^2 dome rather than
// add_drift's cone, kept off cells that lie under something higher than p_ceiling (a roof, a wall top) and tapered
// toward them over p_taper m, so a bank against a building meets the eave's bare strip without a cliff. The whole
// volume lands on the cells that take it.
void SnowCover3D::add_mound(const Vector3 &p_position, float p_radius, float p_volume, float p_density, float p_ceiling, float p_taper) {
	_finish_background(true);
	if (cell_swe.size() != resolution * resolution) {
		_allocate();
		_follow(_follow_position());
	}
	if (p_volume <= 0.0f) {
		return;
	}
	const int n = resolution;
	const float cs = _cell_size();
	// A heap only a cell or two across draws as a spike (the surface interpolates the cells): at least 2.5 cells
	// in radius, and the taper toward the bare strip at least two cells long.
	const float r = MAX(p_radius, 2.5f * cs);
	const float taper = MAX(p_taper, 2.0f * cs);
	const float rho_new = CLAMP(p_density, 30.0f, 917.0f);
	const bool masked = Math::is_finite(p_ceiling) && cell_ground.size() == n * n;
	const int cx = int(Math::floor(p_position.x / cs)) - origin_cell.x;
	const int cz = int(Math::floor(p_position.z / cs)) - origin_cell.y;
	const int reach = int(Math::ceil(r / cs)) + 1;
	const int treach = masked ? int(Math::ceil(taper / cs)) : 0;
	auto blocked = [&](int x, int z) {
		return masked && x >= 0 && z >= 0 && x < n && z < n && cell_ground[z * n + x] > p_ceiling;
	};
	LocalVector<int> idx;
	LocalVector<float> wt;
	double sum = 0.0;
	for (int z = MAX(cz - reach, 0); z <= MIN(cz + reach, n - 1); z++) {
		for (int x = MAX(cx - reach, 0); x <= MIN(cx + reach, n - 1); x++) {
			const Vector3 c = _cell_center(x, z);
			const float d2 = (Vector2(c.x - p_position.x, c.z - p_position.z) / r).length_squared();
			if (d2 >= 1.0f || blocked(x, z)) {
				continue;
			}
			float w = (1.0f - d2) * (1.0f - d2);
			if (treach > 0) {
				float near = 1e9f;
				for (int zz = z - treach; zz <= z + treach; zz++) {
					for (int xx = x - treach; xx <= x + treach; xx++) {
						if (blocked(xx, zz)) {
							near = MIN(near, Vector2(float(xx - x), float(zz - z)).length() * cs);
						}
					}
				}
				if (near < 1e8f) {
					const float t = CLAMP((near - 0.5f * cs) / taper, 0.0f, 1.0f);
					w *= t * t * (3.0f - 2.0f * t);
				}
			}
			if (w <= 0.0f) {
				continue;
			}
			idx.push_back(z * n + x);
			wt.push_back(w);
			sum += w;
		}
	}
	if (sum <= 0.0) {
		// Nowhere open within reach (or off the grid): fall back to a heap on the spot.
		add_drift(p_position, r, 3.0f * p_volume / (float(Math::PI) * r * r), p_density);
		return;
	}
	const float scale = float(p_volume / (sum * cs * cs));
	for (uint32_t k = 0; k < idx.size(); k++) {
		const int i = idx[k];
		const float add = wt[k] * scale;
		const float depth_old = cell_swe[i] > SWE_MIN ? cell_swe[i] / cell_density[i] : 0.0f;
		const float swe = (cell_swe[i] > SWE_MIN ? cell_swe[i] : 0.0f) + add * rho_new;
		cell_swe.write[i] = swe;
		cell_density.write[i] = swe / (depth_old + add);
		_layers_push(i, add * rho_new, rho_new, LAYER_WIND);
		if (cell_crust.size() == cell_swe.size() && cell_crust[i] > 0.0f) {
			cell_crust_burial.write[i] += add;
		}
	}
	last_step_hours = 0.0f;
	update_count++;
	_upload();
}

void SnowCover3D::refresh_heights() {
	_finish_background(true);
	if (cell_ground.size() == resolution * resolution && origin_valid) {
		_finish_sx(true);
		_sample_heights(0, resolution);
		sx_valid = false;
	}
}

// ---------------------------------------------------------------------------------------------------------
// Queries

Rect2 SnowCover3D::get_map_rect() const {
	const float cs = _cell_size();
	return Rect2(origin_cell.x * cs, origin_cell.y * cs, extent, extent);
}

float SnowCover3D::_sample(const Vector<float> &p_field, Vector2 p_xz) const {
	const int n = resolution;
	if (p_field.size() < n * n || !origin_valid) {
		return 0.0f;
	}
	const float cs = _cell_size();
	const float fx = CLAMP(p_xz.x / cs - origin_cell.x - 0.5f, 0.0f, float(n - 1));
	const float fz = CLAMP(p_xz.y / cs - origin_cell.y - 0.5f, 0.0f, float(n - 1));
	const int x0 = MIN(int(fx), n - 2);
	const int z0 = MIN(int(fz), n - 2);
	const float tx = fx - x0;
	const float tz = fz - z0;
	const float *f = p_field.ptr();
	auto at = [&](int x, int z) { return f[z * n + x]; };
	return Math::lerp(Math::lerp(at(x0, z0), at(x0 + 1, z0), tx), Math::lerp(at(x0, z0 + 1), at(x0 + 1, z0 + 1), tx), tz);
}

bool SnowCover3D::_in_world_only(Vector3 p_position) const {
	// Outside the near square, the pack is the world tier's (when there is one).
	return world && !get_map_rect().has_point(Vector2(p_position.x, p_position.z));
}

float SnowCover3D::get_depth_at(Vector3 p_position) const {
	if (_in_world_only(p_position)) {
		return world->get_depth_at(p_position);
	}
	const float swe = get_swe_at(p_position);
	return swe > SWE_MIN ? swe / MAX(get_density_at(p_position), 30.0f) : 0.0f;
}

float SnowCover3D::get_surface_depth_at(Vector3 p_position) const {
	// The drawn surface's depth (SnowSurface3D's snow_depth_smooth_grad): Catmull-Rom over the 4 x 4 cells around,
	// held within the four nearest cells' range. The snow texture holds the cell's swe / density.
	const int n = resolution;
	if (_in_world_only(p_position) || !origin_valid || pub.swe.size() != n * n || pub.density.size() != n * n) {
		return get_depth_at(p_position);
	}
	if (!get_map_rect().has_point(Vector2(p_position.x, p_position.z))) {
		return 0.0f;
	}
	const float cs = _cell_size();
	const float fx = p_position.x / cs - origin_cell.x - 0.5f;
	const float fz = p_position.z / cs - origin_cell.y - 0.5f;
	const int bx = int(Math::floor(fx)), bz = int(Math::floor(fz));
	const float tx = fx - bx, tz = fz - bz;
	auto weights = [](float t, float *w) {
		w[0] = t * (-0.5f + t * (1.0f - 0.5f * t));
		w[1] = 1.0f + t * t * (-2.5f + 1.5f * t);
		w[2] = t * (0.5f + t * (2.0f - 1.5f * t));
		w[3] = t * t * (-0.5f + 0.5f * t);
	};
	float wx[4], wz[4];
	weights(tx, wx);
	weights(tz, wz);
	float sum = 0.0f, lo = 1e9f, hi = -1e9f;
	for (int j = 0; j < 4; j++) {
		for (int i = 0; i < 4; i++) {
			const int x = CLAMP(bx + i - 1, 0, n - 1), z = CLAMP(bz + j - 1, 0, n - 1);
			const int c = z * n + x;
			const float swe = pub.swe[c];
			const float v = swe > SWE_MIN ? swe / MAX(pub.density[c], 30.0f) : 0.0f;
			sum += v * wx[i] * wz[j];
			if (i >= 1 && i <= 2 && j >= 1 && j <= 2) {
				lo = MIN(lo, v);
				hi = MAX(hi, v);
			}
		}
	}
	return CLAMP(sum, lo, hi);
}

float SnowCover3D::get_swe_at(Vector3 p_position) const {
	if (_in_world_only(p_position)) {
		return world->get_swe_at(p_position);
	}
	if (!get_map_rect().has_point(Vector2(p_position.x, p_position.z))) {
		return 0.0f; // no world tier: no snow known outside the square (as the materials read it), not its edge
	}
	return MAX(_sample(pub.swe, Vector2(p_position.x, p_position.z)), 0.0f);
}

float SnowCover3D::get_density_at(Vector3 p_position) const {
	if (_in_world_only(p_position)) {
		return world->get_density_at(p_position);
	}
	return _sample(pub.density, Vector2(p_position.x, p_position.z));
}

void SnowCover3D::add_crust(float p_thickness, float p_cycles) {
	_finish_background(true);
	if (cell_crust.size() != cell_swe.size()) {
		return;
	}
	for (int i = 0; i < cell_swe.size(); i++) {
		const bool on = cell_swe[i] > SWE_MIN && p_thickness > 0.0f && p_cycles > 0.0f;
		cell_crust.write[i] = on ? MIN(p_thickness, CRUST_MAX) : 0.0f;
		cell_crust_cycles.write[i] = on ? p_cycles : 0.0f;
		cell_crust_burial.write[i] = 0.0f;
		cell_crust_wet.write[i] = 0.0f;
	}
	update_count++;
	_publish_cells();
}

float SnowCover3D::get_hoar_at(Vector3 p_position) const {
	if (_in_world_only(p_position)) {
		return world->get_hoar_at(p_position);
	}
	return pub.hoar.is_empty() ? 0.0f : _sample(pub.hoar, Vector2(p_position.x, p_position.z));
}

float SnowCover3D::get_buried_hoar_at(Vector3 p_position) const {
	if (_in_world_only(p_position)) {
		return world->get_buried_hoar_at(p_position);
	}
	return pub.hoar_layer.is_empty() ? 0.0f : _sample(pub.hoar_layer, Vector2(p_position.x, p_position.z));
}

float SnowCover3D::get_buried_hoar_depth_at(Vector3 p_position) const {
	if (_in_world_only(p_position)) {
		return world->get_buried_hoar_depth_at(p_position);
	}
	return pub.hoar_burial.is_empty() ? 0.0f : _sample(pub.hoar_burial, Vector2(p_position.x, p_position.z));
}

float SnowCover3D::get_buried_hoar_age_at(Vector3 p_position) const {
	if (_in_world_only(p_position)) {
		return world->get_buried_hoar_age_at(p_position);
	}
	return pub.hoar_age.is_empty() ? 0.0f : _sample(pub.hoar_age, Vector2(p_position.x, p_position.z)) / 24.0f;
}

void SnowCover3D::add_hoar(float p_size_mm) {
	for (int i = 0; i < cell_hoar.size(); i++) {
		cell_hoar.write[i] = cell_swe[i] > SWE_MIN ? CLAMP(p_size_mm, 0.0f, HOAR_MAX) : 0.0f;
	}
	update_count++;
	_publish_cells();
}

void SnowCover3D::add_suncups(float p_relief) {
	_finish_background(true);
	for (int i = 0; i < cell_cups.size(); i++) {
		cell_cups.write[i] = cell_swe[i] > SWE_MIN ? CLAMP(p_relief, 0.0f, 0.5f * cell_swe[i] / MAX(cell_density[i], 30.0f)) : 0.0f;
	}
	update_count++;
	_upload();
}

void SnowCover3D::add_sastrugi(float p_relief, Vector2 p_wind_direction) {
	_finish_background(true);
	const Vector2 d = p_wind_direction.length_squared() > 0.0f ? p_wind_direction.normalized() : Vector2(1.0f, 0.0f);
	for (int i = 0; i < cell_sas_x.size(); i++) {
		const float a = cell_swe[i] > SWE_MIN ? CLAMP(p_relief, 0.0f, MIN(0.4f, 0.5f * cell_swe[i] / MAX(cell_density[i], 30.0f))) : 0.0f;
		cell_sas_x.write[i] = a * d.x;
		cell_sas_z.write[i] = a * d.y;
	}
	update_count++;
	_upload();
}

float SnowCover3D::get_crust_thickness_at(Vector3 p_position) const {
	if (_in_world_only(p_position)) {
		return world->get_crust_thickness_at(p_position);
	}
	return pub.crust.is_empty() ? 0.0f : _sample(pub.crust, Vector2(p_position.x, p_position.z));
}

float SnowCover3D::get_crust_depth_at(Vector3 p_position) const {
	if (_in_world_only(p_position)) {
		return world->get_crust_depth_at(p_position);
	}
	if (pub.crust.is_empty()) {
		return 0.0f;
	}
	return _sample(pub.crust_burial, Vector2(p_position.x, p_position.z));
}

float SnowCover3D::get_crust_cycles_at(Vector3 p_position) const {
	if (_in_world_only(p_position)) {
		return world->get_crust_cycles_at(p_position);
	}
	return pub.crust.is_empty() ? 0.0f : _sample(pub.crust_cycles, Vector2(p_position.x, p_position.z));
}

float SnowCover3D::get_crust_strength_at(Vector3 p_position) const {
	if (_in_world_only(p_position)) {
		return world->get_crust_strength_at(p_position);
	}
	if (pub.crust.is_empty()) {
		return 0.0f;
	}
	const Vector2 p(p_position.x, p_position.z);
	if (_sample(pub.crust_wet, p) > 0.5f) {
		return 0.0f; // wet: it is slush until it freezes again
	}
	return crust_strength(_sample(pub.crust_cycles, p), _sample(pub.crust, p));
}

float SnowCover3D::get_albedo_at(Vector3 p_position) const {
	if (_in_world_only(p_position)) {
		return world->get_albedo_at(p_position);
	}
	return _sample(pub.albedo, Vector2(p_position.x, p_position.z));
}

// Frozen ground seals the pack's base: concrete frost a few centimetres deep with no thawed layer over it holds
// melt water (Kane and Stein 1983: infiltration into frozen soil falls by orders of magnitude when its pores are
// ice-filled). A thawed top soaks some up and lets it through.
float SnowCover3D::ground_seal(Vector3 p_frost) {
	return Math::smoothstep(0.0f, 0.05f, p_frost.y) * (1.0f - Math::smoothstep(0.0f, 0.03f, p_frost.x));
}

// Darcy flow through the saturated base: hydraulic conductivity K = k rho_w g / mu with the intrinsic permeability
// of wet snow k = 0.077 d^2 exp(-7.8 rho / rho_w) (Shimizu 1970; Colbeck 1978), grains d 1 mm, mu 1.79e-3 Pa s
// at 0 C: K about 8e-3 m/s at 500 kg/m3. A cell gets from upslope about what it gives downslope, so the layer
// drains over the slope's length L (a linear reservoir, as hillslope models treat a saturated layer), at K s / L:
// hours on a steep slope, days on the flat (a slush pond; s at least 0.005, the ground's own unevenness).
float SnowCover3D::slush_drain_rate(float p_density, float p_slope_sine, float p_length) {
	const float k = 0.077f * 1e-6f * Math::exp(-7.8f * CLAMP(p_density, 30.0f, 917.0f) / 1000.0f);
	const float cond = k * 1000.0f * 9.81f / 1.79e-3f;
	return cond * MAX(p_slope_sine, 0.005f) / MAX(p_length, 0.1f) * 3600.0f;
}

void SnowCover3D::_gather_seal() {
	const int n = resolution;
	WorldGround *wg = WorldContracts::ground();
	if (!slush_enabled || driven || !wg || !wg->has_provider() || cell_liquid.size() != n * n || cell_ground.size() != n * n) {
		cell_seal.clear();
		return;
	}
	if (cell_seal.size() != n * n) {
		cell_seal.resize(n * n);
		cell_seal.fill(0.0f);
	}
	float *w = cell_seal.ptrw();
	// Only where there is water to hold: a dry pack does not ask (the frost changes over days, not seconds).
	for (int z = 0; z < n; z++) {
		for (int x = 0; x < n; x++) {
			const int i = z * n + x;
			if ((cell_liquid[i] <= 0.0f && (cell_slush.size() != n * n || cell_slush[i] <= 0.0f)) || cell_swe[i] <= SWE_MIN) {
				continue;
			}
			const Vector3 c = _cell_center(x, z);
			w[i] = ground_seal(wg->frost_at(Vector3(c.x, cell_ground[i], c.z)));
		}
	}
}

float SnowCover3D::get_slush_at(Vector3 p_position) const {
	if (_in_world_only(p_position)) {
		return world->get_slush_at(p_position);
	}
	if (pub.slush.is_empty()) {
		return 0.0f;
	}
	const Vector2 p(p_position.x, p_position.z);
	return slush_height(_sample(pub.swe, p), _sample(pub.liquid, p), _sample(pub.density, p), _sample(pub.slush, p));
}

// The slush water (mm) fills the pores (porosity 1 - ice share, less the water the grains hold) from the base up.
float SnowCover3D::slush_height(float p_swe, float p_liquid, float p_density, float p_slush) {
	if (p_swe <= SWE_MIN || p_slush <= 0.0f) {
		return 0.0f;
	}
	const float depth = p_swe / MAX(p_density, 30.0f);
	const float pores = MAX(1.0f - (p_swe - p_liquid) / 917.0f / MAX(depth, 1e-4f) - p_liquid / 1000.0f / MAX(depth, 1e-4f), 0.05f);
	return MIN(p_slush / 1000.0f / pores, depth);
}

// Traffic (follow-up 43). Wheels and feet pack the snow they pass over hard (a packed-snow road, 400-600 kg/m3), and a
// wheel that slips (spinning or braking) melts the surface by its friction, mu W v_slip, which refreezes in the cold as
// a polished glaze. Grip on each (Wallman and Astrom 2001, VTI): loose snow about 0.3, packed snow 0.2-0.3, ice
// 0.05-0.15 and wet ice lower.
void SnowCover3D::_traffic_step(double p_delta) {
	const int n = resolution;
	if (driven || owner || !traffic_enabled || !is_inside_tree() || !origin_valid || pub.swe.size() != n * n || p_delta <= 0.0) {
		return;
	}
	if (pub.packed.size() != n * n || pub.glaze.size() != n * n) {
		return;
	}
	const float dt = float(p_delta);
	const float cs = _cell_size();
	const float air = WorldContracts::weather()->has_provider() ? WorldContracts::weather()->air_temperature_at(_follow_position()) : inputs.temperature;
	const uint32_t from = traffic_marks.size();
	for (Node *nd : get_tree()->get_nodes_in_group(SNAME("world_contact"))) {
		const Node3D *n3 = Object::cast_to<Node3D>(nd);
		if (!n3 || !n3->is_visible_in_tree()) {
			continue;
		}
		const WorldContactBody b = WorldContracts::describe_body(n3);
		if (b.mass <= 0.0f) {
			continue;
		}
		const float depth = get_depth_at(b.center);
		if (depth < 0.01f) {
			continue;
		}
		const float top = get_ground_height_at(b.center) + depth;
		if (b.center.y - b.half_height - top > 0.15f) {
			continue; // not on the snow
		}
		Vector3 lin, ang;
		float slip = 0.0f;
		if (const RigidBody3D *rb = Object::cast_to<RigidBody3D>(n3)) {
			lin = rb->get_linear_velocity();
			ang = rb->get_angular_velocity();
			if (b.wheel) {
				// The contact point's own velocity: the body's plus its spin about its centre, r below it.
				const Vector3 vc = lin + ang.cross(Vector3(0.0f, -b.half_height, 0.0f));
				slip = Vector2(vc.x, vc.z).length();
			}
		} else if (const VehicleWheel3D *vw = Object::cast_to<VehicleWheel3D>(n3)) {
			if (const RigidBody3D *car = Object::cast_to<RigidBody3D>(vw->get_parent())) {
				lin = car->get_linear_velocity();
			}
			const float roll = Math::abs(vw->get_rpm()) * Math::TAU / 60.0f * vw->get_radius();
			slip = (1.0f - CLAMP(vw->get_skidinfo(), 0.0f, 1.0f)) * MAX(Vector2(lin.x, lin.z).length(), roll);
		} else if (const CharacterBody3D *cb = Object::cast_to<CharacterBody3D>(n3)) {
			lin = cb->get_velocity();
		} else {
			continue;
		}
		const float speed = Vector2(lin.x, lin.z).length();
		const float width = b.width > 0.0f ? b.width : 2.0f * b.radius;
		const float weight = b.mass * 9.81f;
		// How hard it presses: weight over a contact patch about 0.2 m long; a car tyre (75 kPa) packs fully in a
		// pass, a boot (15 kPa) about half as well.
		const float kpa = weight / MAX(width * 0.2f, 0.01f) / 1000.0f;
		TrafficMark m;
		m.center = Vector2(b.center.x, b.center.z);
		m.radius = 0.5f * width;
		m.pack = CLAMP(kpa / 75.0f, 0.2f, 1.0f) * MIN(width * speed * dt / (cs * cs), 1.0f);
		if (slip > 0.05f && air < 0.0f) {
			// Friction power mu W v_slip; about half goes into the snow, and that melt refreezes (917 kg/m3 of
			// ice, 334 kJ/kg) over the cell.
			const float joules = 0.25f * weight * slip * dt * 0.5f;
			m.glaze = joules / (917.0f * 334000.0f) * 1000.0f / (cs * cs);
		}
		if (m.pack > 0.0f || m.glaze > 0.0f) {
			traffic_marks.push_back(m);
		}
	}
	if (traffic_marks.size() > from) {
		_apply_traffic(pub.packed, pub.glaze, traffic_marks, from);
	}
	if (traffic_marks.size() > 65536) {
		traffic_marks.clear(); // not updating: the published copy keeps them
	}
}

void SnowCover3D::_apply_traffic(Vector<float> &r_packed, Vector<float> &r_glaze, const LocalVector<TrafficMark> &p_marks, uint32_t p_from) const {
	const int n = resolution;
	if (r_packed.size() != n * n || r_glaze.size() != n * n || !origin_valid) {
		return;
	}
	const float cs = _cell_size();
	float *pk = r_packed.ptrw();
	float *gl = r_glaze.ptrw();
	for (uint32_t k = p_from; k < p_marks.size(); k++) {
		const TrafficMark &m = p_marks[k];
		const int x = int(Math::floor(m.center.x / cs)) - origin_cell.x;
		const int z = int(Math::floor(m.center.y / cs)) - origin_cell.y;
		if (x < 0 || z < 0 || x >= n || z >= n) {
			continue;
		}
		const int i = z * n + x;
		pk[i] = pk[i] + (1.0f - pk[i]) * CLAMP(m.pack, 0.0f, 1.0f);
		gl[i] = MIN(gl[i] + m.glaze, 20.0f);
	}
}

void SnowCover3D::add_traffic(Vector3 p_position, float p_radius, float p_pack, float p_glaze_mm) {
	TrafficMark m;
	m.center = Vector2(p_position.x, p_position.z);
	m.radius = MAX(p_radius, 0.0f);
	m.pack = CLAMP(p_pack, 0.0f, 1.0f);
	m.glaze = MAX(p_glaze_mm, 0.0f);
	const int n = resolution;
	if (pub.packed.size() != n * n) {
		pub.packed.resize(n * n);
		pub.packed.fill(0.0f);
	}
	if (pub.glaze.size() != n * n) {
		pub.glaze.resize(n * n);
		pub.glaze.fill(0.0f);
	}
	traffic_marks.push_back(m);
	_apply_traffic(pub.packed, pub.glaze, traffic_marks, traffic_marks.size() - 1);
}

float SnowCover3D::get_packed_at(Vector3 p_position) const {
	if (_in_world_only(p_position) || pub.packed.is_empty()) {
		return 0.0f;
	}
	// The cell it lies in (not blended with its neighbours: a track packs the cell it runs through).
	const int n = resolution;
	const float cs = _cell_size();
	const int x = int(Math::floor(p_position.x / cs)) - origin_cell.x;
	const int z = int(Math::floor(p_position.z / cs)) - origin_cell.y;
	if (pub.packed.size() != n * n || x < 0 || z < 0 || x >= n || z >= n) {
		return 0.0f;
	}
	return CLAMP(pub.packed[z * n + x], 0.0f, 1.0f);
}

float SnowCover3D::get_glaze_at(Vector3 p_position) const {
	if (_in_world_only(p_position) || pub.glaze.is_empty()) {
		return 0.0f;
	}
	const int n = resolution;
	const float cs = _cell_size();
	const int x = int(Math::floor(p_position.x / cs)) - origin_cell.x;
	const int z = int(Math::floor(p_position.z / cs)) - origin_cell.y;
	if (pub.glaze.size() != n * n || x < 0 || z < 0 || x >= n || z >= n) {
		return 0.0f;
	}
	return MAX(pub.glaze[z * n + x], 0.0f);
}

// Ice is slipperiest near its melting point, where friction melts a water film (Hayhoe and Shapley 1989; Wallman and
// Astrom 2001: 0.05 on wet ice), and grips better as it gets colder (about 0.15 by -15 C).
float SnowCover3D::ice_grip(float p_celsius, float p_wetness) {
	const float cold = Math::smoothstep(-1.0f, -15.0f, p_celsius);
	return Math::lerp(0.05f + 0.1f * cold, 0.05f, Math::smoothstep(0.1f, 0.5f, p_wetness));
}

float SnowCover3D::get_grip_at(Vector3 p_position) const {
	if (get_depth_at(p_position) < 0.005f) {
		return -1.0f; // bare: the ground's own
	}
	const float air = WorldContracts::weather()->has_provider() ? WorldContracts::weather()->air_temperature_at(p_position) : inputs.temperature;
	const float wet = get_wetness_at(p_position);
	const float polish = 1.0f - Math::exp(-get_glaze_at(p_position) / 0.3f);
	float mu = Math::lerp(0.3f, 0.25f, get_packed_at(p_position));
	mu = Math::lerp(mu, ice_grip(air, wet), polish);
	// New snow over it: the tyre runs in that (loose snow) until it is packed in turn.
	return Math::lerp(mu, 0.3f, Math::smoothstep(0.0f, 0.03f, get_new_snow_at(p_position)));
}

float SnowCover3D::get_wetness_at(Vector3 p_position) const {
	if (_in_world_only(p_position)) {
		return world->get_wetness_at(p_position);
	}
	const Vector2 p(p_position.x, p_position.z);
	const float swe = _sample(pub.swe, p);
	const float liq = _sample(pub.liquid, p);
	if (!pub.slush.is_empty() && swe > SWE_MIN && _sample(pub.slush, p) > 0.5f) {
		return 1.0f; // saturated from the base
	}
	const float hold = liquid_capacity * (swe - liq);
	return hold > 1e-4f ? CLAMP(liq / hold, 0.0f, 1.0f) : 0.0f;
}

float SnowCover3D::get_depth_hoar_at(Vector3 p_position) const {
	if (_in_world_only(p_position)) {
		return world->get_depth_hoar_at(p_position);
	}
	return pub.depth_hoar.size() == resolution * resolution ? CLAMP(_sample(pub.depth_hoar, Vector2(p_position.x, p_position.z)), 0.0f, 1.0f) : 0.0f;
}

void SnowCover3D::add_depth_hoar(float p_share) {
	_finish_background(true);
	for (int i = 0; i < cell_depth_hoar.size(); i++) {
		cell_depth_hoar.write[i] = cell_swe[i] > SWE_MIN ? CLAMP(p_share, 0.0f, 1.0f) : 0.0f;
	}
	update_count++;
	_upload();
}

Vector2 SnowCover3D::get_sastrugi_at(Vector3 p_position) const {
	if (_in_world_only(p_position)) {
		return world->get_sastrugi_at(p_position);
	}
	const int nn = resolution * resolution;
	if (pub.sas_x.size() != nn || pub.sas_z.size() != nn) {
		return Vector2();
	}
	return Vector2(_sample(pub.sas_x, Vector2(p_position.x, p_position.z)), _sample(pub.sas_z, Vector2(p_position.x, p_position.z)));
}

float SnowCover3D::get_wind_slab_at(Vector3 p_position) const {
	if (_in_world_only(p_position)) {
		return world->get_wind_slab_at(p_position);
	}
	if (cell_wind_slab.size() != resolution * resolution || !origin_valid) {
		return 0.0f;
	}
	// The nearest cell's (as the packed snow): a slab is a cell's own layer, not a blend.
	const float cs = _cell_size();
	const int x = CLAMP(int(Math::floor(p_position.x / cs)) - origin_cell.x, 0, resolution - 1);
	const int z = CLAMP(int(Math::floor(p_position.z / cs)) - origin_cell.y, 0, resolution - 1);
	const int i = z * resolution + x;
	const float swe = pub.swe.size() == resolution * resolution ? pub.swe[i] : 0.0f;
	if (swe <= SWE_MIN || pub.wind_slab.size() != resolution * resolution) {
		return 0.0f;
	}
	return MIN(pub.wind_slab[i], swe / MAX(pub.density[i], 30.0f));
}

float SnowCover3D::get_runoff_at(Vector3 p_position) const {
	if (_in_world_only(p_position)) {
		return world->get_runoff_at(p_position);
	}
	return MAX(_sample(pub.runoff, Vector2(p_position.x, p_position.z)), 0.0f);
}

// The weather contract's air at the ground under the point, then the snow's own lapse rate up or down to it.
float SnowCover3D::get_air_temperature_at(Vector3 p_position) const {
	const WorldWeather *w = WorldContracts::weather();
	const bool live = w->has_provider();
	const float ref = live ? w->ground_height_at(p_position) : (is_inside_tree() ? get_global_position().y : 0.0f);
	const float t = live ? w->air_temperature_at(Vector3(p_position.x, ref, p_position.z)) : temperature;
	return t - lapse_rate * 0.001f * (p_position.y - ref);
}

float SnowCover3D::get_snowfall_rate_at(Vector3 p_position) const {
	const WorldWeather *w = WorldContracts::weather();
	const bool live = w->has_provider();
	const float ref = live ? w->ground_height_at(p_position) : (is_inside_tree() ? get_global_position().y : 0.0f);
	const float rate = live ? w->precipitation_at(p_position) : precipitation_rate;
	const float rh = live ? w->air_humidity_at(Vector3(p_position.x, ref, p_position.z)) : humidity;
	const float applied = live ? MAX(w->orographic_factor_at(p_position), 0.05f) : 1.0f; // already in precipitation_at
	// The weather's split (a warm layer aloft melts that share to rain), else the plain one.
	const float share = live ? 1.0f - w->rain_share_at(p_position) : snowfall_fraction(get_air_temperature_at(p_position), rh);
	return MAX(rate, 0.0f) * get_orographic_factor_at(p_position, ref) / applied * CLAMP(share, 0.0f, 1.0f);
}

float SnowCover3D::get_new_snow_at(Vector3 p_position) const {
	return _sample(pub.new_depth, Vector2(p_position.x, p_position.z));
}

float SnowCover3D::get_water_level_at(Vector3 p_position) const {
	if (_in_world_only(p_position)) {
		return world->get_water_level_at(p_position);
	}
	const int n = resolution;
	if (cell_water.size() != n * n || !origin_valid || !get_map_rect().has_point(Vector2(p_position.x, p_position.z))) {
		return -Math::INF;
	}
	// Nearest cell: a shore is a step, not a ramp into the lake.
	const float cs = _cell_size();
	const int x = CLAMP(int(Math::floor(p_position.x / cs)) - origin_cell.x, 0, n - 1);
	const int z = CLAMP(int(Math::floor(p_position.z / cs)) - origin_cell.y, 0, n - 1);
	return cell_water[z * n + x];
}

float SnowCover3D::get_ice_thickness_at(Vector3 p_position) const {
	if (_in_world_only(p_position)) {
		return world->get_ice_thickness_at(p_position);
	}
	const int n = resolution;
	if (pub.ice.size() != n * n || !origin_valid || !get_map_rect().has_point(Vector2(p_position.x, p_position.z))) {
		return 0.0f;
	}
	const float cs = _cell_size();
	const int x = CLAMP(int(Math::floor(p_position.x / cs)) - origin_cell.x, 0, n - 1);
	const int z = CLAMP(int(Math::floor(p_position.z / cs)) - origin_cell.y, 0, n - 1);
	return pub.ice[z * n + x];
}

float SnowCover3D::get_white_ice_thickness_at(Vector3 p_position) const {
	if (_in_world_only(p_position)) {
		return world->get_white_ice_thickness_at(p_position);
	}
	const int n = resolution;
	if (pub.white_ice.size() != n * n || !origin_valid || !get_map_rect().has_point(Vector2(p_position.x, p_position.z))) {
		return 0.0f;
	}
	const float cs = _cell_size();
	const int x = CLAMP(int(Math::floor(p_position.x / cs)) - origin_cell.x, 0, n - 1);
	const int z = CLAMP(int(Math::floor(p_position.z / cs)) - origin_cell.y, 0, n - 1);
	return pub.white_ice[z * n + x];
}

float SnowCover3D::contract_snow_density_at(const Vector3 &p_position) const {
	return get_depth_at(p_position) > 0.0f ? get_density_at(p_position) : 0.0f;
}

float SnowCover3D::contract_snow_surface_at(const Vector3 &p_position) const {
	// The deep snow near the camera has the prints in it; elsewhere the cover's ground plus its depth.
	const SnowSurface3D *surface = SnowSurface3D::get_active();
	if (surface && surface->get_deform_rect().has_point(Vector2(p_position.x, p_position.z))) {
		return surface->get_surface_height_at(p_position);
	}
	const float d = get_depth_at(p_position);
	return d > 0.0f ? get_ground_height_at(p_position) + d : -Math::INF;
}

float SnowCover3D::contract_snow_avalanche_pressure_at(const Vector3 &p_position) const {
	return avalanche_node ? avalanche_node->get_pressure_at(p_position) : 0.0f;
}

float SnowCover3D::contract_snow_avalanche_peak_pressure_at(const Vector3 &p_position) const {
	return avalanche_node ? avalanche_node->get_peak_pressure_at(p_position) : 0.0f;
}

Vector2 SnowCover3D::contract_snow_lake_ice_at(const Vector3 &p_position) const {
	const float ice = get_ice_thickness_at(p_position);
	return Vector2(ice, ice > 0.0f ? CLAMP(get_white_ice_thickness_at(p_position) / ice, 0.0f, 1.0f) : 0.0f);
}

void SnowCover3D::set_lake_ice_enabled(bool p_enabled) {
	_finish_background(true);
	lake_ice_enabled = p_enabled;
	if (world) {
		world->lake_ice_enabled = p_enabled;
	}
}

void SnowCover3D::set_avalanches_enabled(bool p_enabled) {
	avalanches_enabled = p_enabled;
	if (!p_enabled && avalanche_node) {
		avalanche_node->queue_free();
		avalanche_node = nullptr;
	}
}

void SnowCover3D::set_bodies_enabled(bool p_enabled) {
	bodies_enabled = p_enabled;
	if (!p_enabled && bodies_node) {
		bodies_node->queue_free();
		bodies_node = nullptr;
	}
}

float SnowCover3D::get_ground_height_at(Vector3 p_position) const {
	return _sample(cell_ground, Vector2(p_position.x, p_position.z));
}

float SnowCover3D::get_total_water() const {
	const float cs = _cell_size();
	double sum = 0.0;
	for (int i = 0; i < pub.swe.size(); i++) {
		sum += pub.swe[i];
	}
	for (int i = 0; i < pub.canopy_load.size(); i++) {
		sum += pub.canopy_load[i];
	}
	return float(sum * cs * cs);
}

// ---------------------------------------------------------------------------------------------------------
// Node

void SnowCover3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			active = this;
			if (!owner) {
				WorldContracts::register_snow(this);
			}
			if (cell_swe.size() != resolution * resolution) {
				_allocate();
			}
			if (!world && climate.is_valid() && climate->is_baked()) {
				_make_world();
			}
			if (!Engine::get_singleton()->is_editor_hint()) {
				set_physics_process_internal(true);
			}
		} break;
		case NOTIFICATION_READY: {
			if (!Engine::get_singleton()->is_editor_hint()) {
				_follow(_follow_position());
				_upload();
				// Lake ice draws itself (follow-up 15); a scene may hold its own SnowLakeIce3D instead.
				if (lake_ice_enabled && !owner && !lake_ice) {
					lake_ice = memnew(SnowLakeIce3D);
					add_child(lake_ice, false, INTERNAL_MODE_BACK);
				}
				// Moving things carry their own snow (follow-up 18).
				if (bodies_enabled && !owner && !bodies_node) {
					bodies_node = memnew(SnowBodies3D);
					add_child(bodies_node, false, INTERNAL_MODE_BACK);
				}
				// Slab avalanches (follow-up 20).
				if (avalanches_enabled && !owner && !avalanche_node) {
					avalanche_node = memnew(SnowAvalanche3D);
					add_child(avalanche_node, false, INTERNAL_MODE_BACK);
				}
			}
		} break;
		case NOTIFICATION_EXIT_TREE: {
			_finish_background(true);
			WorldContracts::unregister_snow(this);
			if (WeatherMap3D *weather = WeatherMap3D::get_active()) {
				weather->clear_surface_water_source(this);
			}
			_finish_sx(true);
			_free_world();
			if (active == this) {
				active = nullptr;
			}
			set_physics_process_internal(false);
		} break;
		case NOTIFICATION_INTERNAL_PHYSICS_PROCESS: {
			_ice_load_step();
			_traffic_step(get_physics_process_delta_time());
			const WeatherMap3D *weather = WeatherMap3D::get_active();
			const double scale = weather ? weather->get_time_scale() : time_scale;
			update_accum += get_physics_process_delta_time();
			if (background_updates) {
				// An update in flight swaps in once it is done; the square follows and the next one starts
				// only then (the step it takes is the time since the last one started, as before).
				_finish_background(false);
				if (is_updating()) {
					break;
				}
				_follow(_follow_position());
				if (update_accum >= 1.0 / MAX(update_rate, 0.01f)) {
					bool world_turn = false;
					if (world) {
						world_accum += update_accum * scale;
						if (++world_counter >= MAX(world_update_every, 1)) {
							world_counter = 0;
							world_dt = world_accum;
							world_accum = 0.0;
							world_turn = true;
						}
					}
					_start_background(update_accum * scale, world_turn);
					update_accum = 0.0;
				}
				break;
			}
			_follow(_follow_position());
			if (update_accum >= 1.0 / MAX(update_rate, 0.01f)) {
				const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
				_update(update_accum * scale, false);
				if (world) {
					world_accum += update_accum * scale;
					if (++world_counter >= MAX(world_update_every, 1)) {
						world_counter = 0;
						_feed_world();
						world->_update(world_accum, false);
						world_accum = 0.0;
						_emit_cornice_failures();
						world->_upload();
						_publish_world_globals();
					}
				}
				update_accum = 0.0;
				_upload();
				main_usec = OS::get_singleton()->get_ticks_usec() - t0;
			}
		} break;
	}
}

// ---------------------------------------------------------------------------------------------------------
// Settings

void SnowCover3D::set_extent(float p_extent) {
	extent = MAX(p_extent, 1.0f);
	_allocate();
}

void SnowCover3D::set_resolution(int p_resolution) {
	resolution = CLAMP(p_resolution, 4, 2048);
	_allocate();
}

void SnowCover3D::set_follow_target(const NodePath &p_path) {
	follow_target = p_path;
}

void SnowCover3D::set_update_rate(float p_rate) {
	update_rate = MAX(p_rate, 0.01f);
}

void SnowCover3D::set_height_source(const NodePath &p_path) {
	height_source = p_path;
	refresh_heights();
}

void SnowCover3D::set_ground_collision_mask(uint32_t p_mask) {
	ground_collision_mask = p_mask;
}

void SnowCover3D::set_sun_light(const NodePath &p_path) {
	sun_light = p_path;
}

void SnowCover3D::set_lapse_rate(float p_rate) {
	lapse_rate = p_rate;
}

void SnowCover3D::set_temperature(float p_celsius) {
	temperature = p_celsius;
}

void SnowCover3D::set_humidity(float p_humidity) {
	humidity = CLAMP(p_humidity, 0.0f, 1.0f);
}

void SnowCover3D::set_precipitation_rate(float p_rate) {
	precipitation_rate = MAX(p_rate, 0.0f);
}

void SnowCover3D::set_time_scale(float p_scale) {
	time_scale = MAX(p_scale, 0.0f);
}

void SnowCover3D::set_melt_threshold(float p_celsius) {
	melt_threshold = p_celsius;
}

void SnowCover3D::set_melt_temperature_factor(float p_factor) {
	melt_temperature_factor = MAX(p_factor, 0.0f);
}

void SnowCover3D::set_melt_shortwave_factor(float p_factor) {
	melt_shortwave_factor = MAX(p_factor, 0.0f);
}

void SnowCover3D::set_liquid_capacity(float p_share) {
	liquid_capacity = CLAMP(p_share, 0.0f, 0.5f);
}

void SnowCover3D::set_crust_enabled(bool p_enabled) {
	crust_enabled = p_enabled;
}

void SnowCover3D::set_hoar_enabled(bool p_enabled) {
	hoar_enabled = p_enabled;
}

void SnowCover3D::set_slide_enabled(bool p_enabled) {
	slide_enabled = p_enabled;
}

void SnowCover3D::set_slide_angle(float p_degrees) {
	slide_angle = CLAMP(p_degrees, 1.0f, 89.0f);
	steep_dirty = true;
}

void SnowCover3D::set_slide_interval(float p_seconds) {
	slide_interval = MAX(p_seconds, 0.0f);
}

void SnowCover3D::set_slide_min_holding(float p_mm) {
	slide_min_holding = MAX(p_mm, 0.0f);
}

void SnowCover3D::set_slide_holding_decay(float p_per_degree) {
	slide_holding_decay = MAX(p_per_degree, 0.0f);
}

void SnowCover3D::set_drift_obstacles(bool p_enabled) {
	drift_obstacles = p_enabled;
}

void SnowCover3D::set_drift_relief(bool p_enabled) {
	drift_relief = p_enabled;
	if (world) {
		world->drift_relief = p_enabled;
	}
}

void SnowCover3D::set_drift_enabled(bool p_enabled) {
	drift_enabled = p_enabled;
}

void SnowCover3D::set_drift_distance(float p_m) {
	drift_distance = MAX(p_m, 1.0f);
	sx_valid = false;
}

void SnowCover3D::set_drift_strength(float p_strength) {
	drift_strength = MAX(p_strength, 0.0f);
}

void SnowCover3D::set_wind_transport_enabled(bool p_enabled) {
	wind_transport_enabled = p_enabled;
}

void SnowCover3D::set_wind_curvature_length(float p_m) {
	wind_curvature_length = MAX(p_m, 0.0f);
}

void SnowCover3D::set_drift_threshold_wind(float p_speed) {
	drift_threshold_wind = MAX(p_speed, 0.0f);
}

void SnowCover3D::set_precipitation_gradient(float p_share) {
	precipitation_gradient = MAX(p_share, 0.0f);
}

void SnowCover3D::set_climate_map(const NodePath &p_path) {
	climate_map = p_path;
}

const TerrainDataMap3D *SnowCover3D::get_climate_map_node() const {
	const SnowCover3D *o = owner ? owner : this;
	if (o->climate_map.is_empty() || !o->is_inside_tree()) {
		return nullptr;
	}
	const TerrainDataMap3D *m = Object::cast_to<TerrainDataMap3D>(o->get_node_or_null(o->climate_map));
	return m && m->has_climate() && m->get_climate_recipe().is_valid() ? m : nullptr;
}

float SnowCover3D::get_orographic_factor_at(Vector3 p_position, float p_ref_height) const {
	const TerrainDataMap3D *m = get_climate_map_node();
	if (m && m->get_climate_recipe()->get_precipitation() > 0.0f) {
		return MAX(m->get_precipitation_at(p_position) / m->get_climate_recipe()->get_precipitation(), 0.0f);
	}
	return MAX(1.0f + precipitation_gradient * (p_position.y - p_ref_height) * 0.01f, 0.0f);
}

void SnowCover3D::set_climate(const Ref<SnowClimate> &p_climate) {
	_finish_background(true);
	climate = p_climate;
	if (is_inside_tree()) {
		_make_world();
		if (world) {
			_seed_from_world(nullptr);
			_upload();
		}
	}
}

void SnowCover3D::set_world_update_every(int p_updates) {
	world_update_every = MAX(p_updates, 1);
}

void SnowCover3D::set_publish_shader_globals(bool p_enabled) {
	publish_shader_globals = p_enabled;
	if (p_enabled) {
		_publish_globals();
	}
}

void SnowCover3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_extent", "extent"), &SnowCover3D::set_extent);
	ClassDB::bind_method(D_METHOD("get_extent"), &SnowCover3D::get_extent);
	ClassDB::bind_method(D_METHOD("set_resolution", "resolution"), &SnowCover3D::set_resolution);
	ClassDB::bind_method(D_METHOD("get_resolution"), &SnowCover3D::get_resolution);
	ClassDB::bind_method(D_METHOD("set_follow_target", "path"), &SnowCover3D::set_follow_target);
	ClassDB::bind_method(D_METHOD("get_follow_target"), &SnowCover3D::get_follow_target);
	ClassDB::bind_method(D_METHOD("set_update_rate", "rate"), &SnowCover3D::set_update_rate);
	ClassDB::bind_method(D_METHOD("get_update_rate"), &SnowCover3D::get_update_rate);
	ClassDB::bind_method(D_METHOD("set_height_source", "path"), &SnowCover3D::set_height_source);
	ClassDB::bind_method(D_METHOD("get_height_source"), &SnowCover3D::get_height_source);
	ClassDB::bind_method(D_METHOD("set_shelter_collision_mask", "mask"), &SnowCover3D::set_shelter_collision_mask);
	ClassDB::bind_method(D_METHOD("get_shelter_collision_mask"), &SnowCover3D::get_shelter_collision_mask);
	ClassDB::bind_method(D_METHOD("set_canopy_enabled", "enabled"), &SnowCover3D::set_canopy_enabled);
	ClassDB::bind_method(D_METHOD("is_canopy_enabled"), &SnowCover3D::is_canopy_enabled);
	ClassDB::bind_method(D_METHOD("get_canopy_load_at", "position"), &SnowCover3D::get_canopy_load_at);
	ClassDB::bind_method(D_METHOD("shake_canopy", "position", "radius", "share"), &SnowCover3D::shake_canopy);
	ClassDB::bind_method(D_METHOD("get_canopy_capacity_at", "position"), &SnowCover3D::get_canopy_capacity_at);
	ClassDB::bind_method(D_METHOD("get_catch_texture"), &SnowCover3D::get_catch_texture);
	ClassDB::bind_static_method("SnowCover3D", D_METHOD("canopy_capacity", "closure", "snow_density"), &SnowCover3D::canopy_capacity);
	ClassDB::bind_method(D_METHOD("set_ground_collision_mask", "mask"), &SnowCover3D::set_ground_collision_mask);
	ClassDB::bind_method(D_METHOD("get_ground_collision_mask"), &SnowCover3D::get_ground_collision_mask);
	ClassDB::bind_method(D_METHOD("set_sun_light", "path"), &SnowCover3D::set_sun_light);
	ClassDB::bind_method(D_METHOD("get_sun_light"), &SnowCover3D::get_sun_light);
	ClassDB::bind_method(D_METHOD("set_lapse_rate", "celsius_per_km"), &SnowCover3D::set_lapse_rate);
	ClassDB::bind_method(D_METHOD("get_lapse_rate"), &SnowCover3D::get_lapse_rate);
	ClassDB::bind_method(D_METHOD("set_temperature", "celsius"), &SnowCover3D::set_temperature);
	ClassDB::bind_method(D_METHOD("get_temperature"), &SnowCover3D::get_temperature);
	ClassDB::bind_method(D_METHOD("set_humidity", "humidity"), &SnowCover3D::set_humidity);
	ClassDB::bind_method(D_METHOD("get_humidity"), &SnowCover3D::get_humidity);
	ClassDB::bind_method(D_METHOD("set_precipitation_rate", "mm_per_hour"), &SnowCover3D::set_precipitation_rate);
	ClassDB::bind_method(D_METHOD("get_precipitation_rate"), &SnowCover3D::get_precipitation_rate);
	ClassDB::bind_method(D_METHOD("set_time_scale", "scale"), &SnowCover3D::set_time_scale);
	ClassDB::bind_method(D_METHOD("get_time_scale"), &SnowCover3D::get_time_scale);
	ClassDB::bind_method(D_METHOD("set_melt_threshold", "celsius"), &SnowCover3D::set_melt_threshold);
	ClassDB::bind_method(D_METHOD("get_melt_threshold"), &SnowCover3D::get_melt_threshold);
	ClassDB::bind_method(D_METHOD("set_melt_temperature_factor", "factor"), &SnowCover3D::set_melt_temperature_factor);
	ClassDB::bind_method(D_METHOD("get_melt_temperature_factor"), &SnowCover3D::get_melt_temperature_factor);
	ClassDB::bind_method(D_METHOD("set_melt_shortwave_factor", "factor"), &SnowCover3D::set_melt_shortwave_factor);
	ClassDB::bind_method(D_METHOD("get_melt_shortwave_factor"), &SnowCover3D::get_melt_shortwave_factor);
	ClassDB::bind_method(D_METHOD("set_liquid_capacity", "share"), &SnowCover3D::set_liquid_capacity);
	ClassDB::bind_method(D_METHOD("get_liquid_capacity"), &SnowCover3D::get_liquid_capacity);
	ClassDB::bind_method(D_METHOD("set_slide_enabled", "enabled"), &SnowCover3D::set_slide_enabled);
	ClassDB::bind_method(D_METHOD("is_slide_enabled"), &SnowCover3D::is_slide_enabled);
	ClassDB::bind_method(D_METHOD("set_slide_interval", "seconds"), &SnowCover3D::set_slide_interval);
	ClassDB::bind_method(D_METHOD("get_slide_interval"), &SnowCover3D::get_slide_interval);
	ClassDB::bind_method(D_METHOD("set_slide_angle", "degrees"), &SnowCover3D::set_slide_angle);
	ClassDB::bind_method(D_METHOD("get_slide_angle"), &SnowCover3D::get_slide_angle);
	ClassDB::bind_method(D_METHOD("set_slide_min_holding", "mm"), &SnowCover3D::set_slide_min_holding);
	ClassDB::bind_method(D_METHOD("get_slide_min_holding"), &SnowCover3D::get_slide_min_holding);
	ClassDB::bind_method(D_METHOD("set_slide_holding_decay", "per_degree"), &SnowCover3D::set_slide_holding_decay);
	ClassDB::bind_method(D_METHOD("get_slide_holding_decay"), &SnowCover3D::get_slide_holding_decay);
	ClassDB::bind_method(D_METHOD("set_drift_obstacles", "enabled"), &SnowCover3D::set_drift_obstacles);
	ClassDB::bind_method(D_METHOD("set_drift_relief", "enabled"), &SnowCover3D::set_drift_relief);
	ClassDB::bind_method(D_METHOD("get_layers_at", "position"), &SnowCover3D::get_layers_at);
	ClassDB::bind_method(D_METHOD("set_layers_enabled", "enabled"), &SnowCover3D::set_layers_enabled);
	ClassDB::bind_method(D_METHOD("is_layers_enabled"), &SnowCover3D::is_layers_enabled);
	ClassDB::bind_static_method("SnowCover3D", D_METHOD("layer_compaction", "density", "stress", "celsius", "hours"), &SnowCover3D::layer_compaction);
	ClassDB::bind_static_method("SnowCover3D", D_METHOD("layer_barrier", "kind_above", "kind_below"), &SnowCover3D::layer_barrier);
	ClassDB::bind_method(D_METHOD("is_drift_relief_enabled"), &SnowCover3D::is_drift_relief_enabled);
	ClassDB::bind_method(D_METHOD("is_drift_obstacles_enabled"), &SnowCover3D::is_drift_obstacles_enabled);
	ClassDB::bind_method(D_METHOD("set_drift_enabled", "enabled"), &SnowCover3D::set_drift_enabled);
	ClassDB::bind_method(D_METHOD("is_drift_enabled"), &SnowCover3D::is_drift_enabled);
	ClassDB::bind_method(D_METHOD("set_drift_distance", "distance"), &SnowCover3D::set_drift_distance);
	ClassDB::bind_method(D_METHOD("get_drift_distance"), &SnowCover3D::get_drift_distance);
	ClassDB::bind_method(D_METHOD("set_drift_strength", "strength"), &SnowCover3D::set_drift_strength);
	ClassDB::bind_method(D_METHOD("get_drift_strength"), &SnowCover3D::get_drift_strength);
	ClassDB::bind_method(D_METHOD("set_wind_transport_enabled", "enabled"), &SnowCover3D::set_wind_transport_enabled);
	ClassDB::bind_method(D_METHOD("is_wind_transport_enabled"), &SnowCover3D::is_wind_transport_enabled);
	ClassDB::bind_method(D_METHOD("set_wind_curvature_length", "length"), &SnowCover3D::set_wind_curvature_length);
	ClassDB::bind_method(D_METHOD("get_wind_curvature_length"), &SnowCover3D::get_wind_curvature_length);
	ClassDB::bind_static_method("SnowCover3D", D_METHOD("transport_flux", "wind10", "celsius", "density"), &SnowCover3D::transport_flux);
	ClassDB::bind_static_method("SnowCover3D", D_METHOD("transport_threshold", "celsius", "density"), &SnowCover3D::transport_threshold);
	ClassDB::bind_method(D_METHOD("get_blown_mean"), &SnowCover3D::get_blown_mean);
	ClassDB::bind_method(D_METHOD("get_blown_flux_max"), &SnowCover3D::get_blown_flux_max);
	ClassDB::bind_method(D_METHOD("set_drift_threshold_wind", "speed"), &SnowCover3D::set_drift_threshold_wind);
	ClassDB::bind_method(D_METHOD("get_drift_threshold_wind"), &SnowCover3D::get_drift_threshold_wind);
	ClassDB::bind_method(D_METHOD("set_publish_shader_globals", "enabled"), &SnowCover3D::set_publish_shader_globals);
	ClassDB::bind_method(D_METHOD("get_publish_shader_globals"), &SnowCover3D::get_publish_shader_globals);

	ClassDB::bind_method(D_METHOD("advance", "seconds"), &SnowCover3D::advance);
	ClassDB::bind_method(D_METHOD("clear"), &SnowCover3D::clear);
	ClassDB::bind_method(D_METHOD("add_snow", "swe_mm", "density", "world"), &SnowCover3D::add_snow, DEFVAL(100.0f), DEFVAL(true));
	ClassDB::bind_method(D_METHOD("add_drift", "position", "radius", "depth", "density"), &SnowCover3D::add_drift, DEFVAL(300.0f));
	ClassDB::bind_method(D_METHOD("add_mound", "position", "radius", "volume", "density", "ceiling", "taper"), &SnowCover3D::add_mound, DEFVAL(300.0f), DEFVAL(float(Math::INF)), DEFVAL(0.8f));
	ClassDB::bind_method(D_METHOD("refresh_heights"), &SnowCover3D::refresh_heights);
	ClassDB::bind_method(D_METHOD("get_map_rect"), &SnowCover3D::get_map_rect);
	ClassDB::bind_method(D_METHOD("get_depth_at", "position"), &SnowCover3D::get_depth_at);
	ClassDB::bind_method(D_METHOD("get_swe_at", "position"), &SnowCover3D::get_swe_at);
	ClassDB::bind_method(D_METHOD("get_density_at", "position"), &SnowCover3D::get_density_at);
	ClassDB::bind_method(D_METHOD("get_albedo_at", "position"), &SnowCover3D::get_albedo_at);
	ClassDB::bind_method(D_METHOD("get_wetness_at", "position"), &SnowCover3D::get_wetness_at);
	ClassDB::bind_method(D_METHOD("get_runoff_at", "position"), &SnowCover3D::get_runoff_at);
	ClassDB::bind_method(D_METHOD("get_ground_height_at", "position"), &SnowCover3D::get_ground_height_at);
	ClassDB::bind_method(D_METHOD("get_new_snow_at", "position"), &SnowCover3D::get_new_snow_at);
	ClassDB::bind_method(D_METHOD("get_crust_thickness_at", "position"), &SnowCover3D::get_crust_thickness_at);
	ClassDB::bind_method(D_METHOD("get_crust_depth_at", "position"), &SnowCover3D::get_crust_depth_at);
	ClassDB::bind_method(D_METHOD("get_crust_cycles_at", "position"), &SnowCover3D::get_crust_cycles_at);
	ClassDB::bind_method(D_METHOD("get_crust_strength_at", "position"), &SnowCover3D::get_crust_strength_at);
	ClassDB::bind_method(D_METHOD("get_hoar_at", "position"), &SnowCover3D::get_hoar_at);
	ClassDB::bind_method(D_METHOD("get_suncup_relief_at", "position"), &SnowCover3D::get_suncup_relief_at);
	ClassDB::bind_method(D_METHOD("get_cups_texture"), &SnowCover3D::get_cups_texture);
	ClassDB::bind_static_method("SnowCover3D", D_METHOD("suncup_step", "relief", "sun_melt", "air_melt", "rain", "new_depth", "dirt", "depth"), &SnowCover3D::suncup_step);
	ClassDB::bind_method(D_METHOD("set_suncups_enabled", "enabled"), &SnowCover3D::set_suncups_enabled);
	ClassDB::bind_method(D_METHOD("is_suncups_enabled"), &SnowCover3D::is_suncups_enabled);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "suncups_enabled"), "set_suncups_enabled", "is_suncups_enabled");
	ClassDB::bind_method(D_METHOD("set_depth_hoar_enabled", "enabled"), &SnowCover3D::set_depth_hoar_enabled);
	ClassDB::bind_method(D_METHOD("is_depth_hoar_enabled"), &SnowCover3D::is_depth_hoar_enabled);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "depth_hoar_enabled"), "set_depth_hoar_enabled", "is_depth_hoar_enabled");
	ClassDB::bind_method(D_METHOD("get_depth_hoar_at", "position"), &SnowCover3D::get_depth_hoar_at);
	ClassDB::bind_method(D_METHOD("add_depth_hoar", "share"), &SnowCover3D::add_depth_hoar);
	ClassDB::bind_static_method("SnowCover3D", D_METHOD("depth_hoar_step", "share", "gradient", "hours", "wet_share"), &SnowCover3D::depth_hoar_step);
	ClassDB::bind_static_method("SnowCover3D", D_METHOD("depth_hoar_strength", "share", "density"), &SnowCover3D::depth_hoar_strength);
	ClassDB::bind_method(D_METHOD("set_sastrugi_enabled", "enabled"), &SnowCover3D::set_sastrugi_enabled);
	ClassDB::bind_method(D_METHOD("is_sastrugi_enabled"), &SnowCover3D::is_sastrugi_enabled);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "sastrugi_enabled"), "set_sastrugi_enabled", "is_sastrugi_enabled");
	ClassDB::bind_method(D_METHOD("get_sastrugi_at", "position"), &SnowCover3D::get_sastrugi_at);
	ClassDB::bind_method(D_METHOD("get_sastrugi_texture"), &SnowCover3D::get_sastrugi_texture);
	ClassDB::bind_method(D_METHOD("get_strata_texture"), &SnowCover3D::get_strata_texture);
	ClassDB::bind_method(D_METHOD("add_sastrugi", "relief", "wind_direction"), &SnowCover3D::add_sastrugi);
	ClassDB::bind_static_method("SnowCover3D", D_METHOD("sastrugi_step", "relief", "wind_dir", "eroded", "filled", "depth", "hard"), &SnowCover3D::sastrugi_step);
	ClassDB::bind_method(D_METHOD("set_wind_packing_enabled", "enabled"), &SnowCover3D::set_wind_packing_enabled);
	ClassDB::bind_method(D_METHOD("is_wind_packing_enabled"), &SnowCover3D::is_wind_packing_enabled);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "wind_packing_enabled"), "set_wind_packing_enabled", "is_wind_packing_enabled");
	ClassDB::bind_method(D_METHOD("get_wind_slab_at", "position"), &SnowCover3D::get_wind_slab_at);
	ClassDB::bind_static_method("SnowCover3D", D_METHOD("wind_snow_density", "celsius", "wind10"), &SnowCover3D::wind_snow_density);
	ClassDB::bind_static_method("SnowCover3D", D_METHOD("drift_index", "wind10", "density", "fresh"), &SnowCover3D::drift_index);
	ClassDB::bind_static_method("SnowCover3D", D_METHOD("wind_packing_step", "density", "wind10", "fresh", "hours"), &SnowCover3D::wind_packing_step);
	ClassDB::bind_method(D_METHOD("set_slush_enabled", "enabled"), &SnowCover3D::set_slush_enabled);
	ClassDB::bind_method(D_METHOD("is_slush_enabled"), &SnowCover3D::is_slush_enabled);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "slush_enabled"), "set_slush_enabled", "is_slush_enabled");
	ClassDB::bind_method(D_METHOD("set_spindrift_enabled", "enabled"), &SnowCover3D::set_spindrift_enabled);
	ClassDB::bind_method(D_METHOD("is_spindrift_enabled"), &SnowCover3D::is_spindrift_enabled);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "spindrift_enabled"), "set_spindrift_enabled", "is_spindrift_enabled");
	ClassDB::bind_method(D_METHOD("set_traffic_enabled", "enabled"), &SnowCover3D::set_traffic_enabled);
	ClassDB::bind_method(D_METHOD("is_traffic_enabled"), &SnowCover3D::is_traffic_enabled);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "traffic_enabled"), "set_traffic_enabled", "is_traffic_enabled");
	ClassDB::bind_method(D_METHOD("get_packed_at", "position"), &SnowCover3D::get_packed_at);
	ClassDB::bind_method(D_METHOD("get_glaze_at", "position"), &SnowCover3D::get_glaze_at);
	ClassDB::bind_method(D_METHOD("get_grip_at", "position"), &SnowCover3D::get_grip_at);
	ClassDB::bind_method(D_METHOD("add_traffic", "position", "radius", "pack", "glaze_mm"), &SnowCover3D::add_traffic);
	ClassDB::bind_static_method("SnowCover3D", D_METHOD("ice_grip", "celsius", "wetness"), &SnowCover3D::ice_grip);
	ClassDB::bind_method(D_METHOD("set_max_spindrift", "count"), &SnowCover3D::set_max_spindrift);
	ClassDB::bind_method(D_METHOD("get_max_spindrift"), &SnowCover3D::get_max_spindrift);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_spindrift", PROPERTY_HINT_RANGE, "1,32,1"), "set_max_spindrift", "get_max_spindrift");
	ClassDB::bind_method(D_METHOD("get_spindrift_rate"), &SnowCover3D::get_spindrift_rate);
	ClassDB::bind_method(D_METHOD("get_spindrift_source_count"), &SnowCover3D::get_spindrift_source_count);
	ClassDB::bind_method(D_METHOD("get_spindrift_sources"), &SnowCover3D::get_spindrift_sources);
	ClassDB::bind_method(D_METHOD("get_spindrift"), &SnowCover3D::get_spindrift);
	ClassDB::bind_static_method("SnowCover3D", D_METHOD("spindrift_fade", "humidity"), &SnowCover3D::spindrift_fade);
	ClassDB::bind_method(D_METHOD("get_slush_at", "position"), &SnowCover3D::get_slush_at);
	ClassDB::bind_static_method("SnowCover3D", D_METHOD("ground_seal", "frost"), &SnowCover3D::ground_seal);
	ClassDB::bind_static_method("SnowCover3D", D_METHOD("slush_height", "swe", "liquid", "density", "slush"), &SnowCover3D::slush_height);
	ClassDB::bind_static_method("SnowCover3D", D_METHOD("slush_drain_rate", "density", "slope_sine", "length"), &SnowCover3D::slush_drain_rate);
	ClassDB::bind_method(D_METHOD("get_buried_hoar_at", "position"), &SnowCover3D::get_buried_hoar_at);
	ClassDB::bind_method(D_METHOD("get_buried_hoar_depth_at", "position"), &SnowCover3D::get_buried_hoar_depth_at);
	ClassDB::bind_method(D_METHOD("get_buried_hoar_age_at", "position"), &SnowCover3D::get_buried_hoar_age_at);
	ClassDB::bind_method(D_METHOD("add_hoar", "size_mm"), &SnowCover3D::add_hoar);
	ClassDB::bind_method(D_METHOD("add_suncups", "relief"), &SnowCover3D::add_suncups);
	ClassDB::bind_static_method("SnowCover3D", D_METHOD("buried_hoar_strength", "size_mm", "age_hours"), &SnowCover3D::buried_hoar_strength);
	ClassDB::bind_static_method("SnowCover3D", D_METHOD("hoar_deposition", "air_celsius", "humidity", "wind", "cloud", "shortwave"), &SnowCover3D::hoar_deposition_rate);
	ClassDB::bind_method(D_METHOD("set_hoar_enabled", "enabled"), &SnowCover3D::set_hoar_enabled);
	ClassDB::bind_method(D_METHOD("is_hoar_enabled"), &SnowCover3D::is_hoar_enabled);
	ClassDB::bind_static_method("SnowCover3D", D_METHOD("crust_strength", "cycles", "thickness"), &SnowCover3D::crust_strength);
	ClassDB::bind_method(D_METHOD("add_crust", "thickness", "cycles"), &SnowCover3D::add_crust);
	ClassDB::bind_method(D_METHOD("set_crust_enabled", "enabled"), &SnowCover3D::set_crust_enabled);
	ClassDB::bind_method(D_METHOD("is_crust_enabled"), &SnowCover3D::is_crust_enabled);
	ClassDB::bind_method(D_METHOD("get_air_temperature_at", "position"), &SnowCover3D::get_air_temperature_at);
	ClassDB::bind_method(D_METHOD("get_snowfall_rate_at", "position"), &SnowCover3D::get_snowfall_rate_at);
	ClassDB::bind_method(D_METHOD("sample_ground_height", "position"), &SnowCover3D::sample_ground_height);
	ClassDB::bind_method(D_METHOD("get_update_count"), &SnowCover3D::get_update_count);
	ClassDB::bind_method(D_METHOD("get_drift_factor"), &SnowCover3D::get_drift_factor);
	ClassDB::bind_method(D_METHOD("get_total_water"), &SnowCover3D::get_total_water);
	ClassDB::bind_method(D_METHOD("get_update_time_msec"), &SnowCover3D::get_update_time_msec);
	ClassDB::bind_method(D_METHOD("get_main_thread_msec"), &SnowCover3D::get_main_thread_msec);
	ClassDB::bind_method(D_METHOD("set_background_updates", "enabled"), &SnowCover3D::set_background_updates);
	ClassDB::bind_method(D_METHOD("is_background_updates"), &SnowCover3D::is_background_updates);
	ClassDB::bind_method(D_METHOD("is_updating"), &SnowCover3D::is_updating);
	ClassDB::bind_method(D_METHOD("sync_update"), &SnowCover3D::sync_update);
	ClassDB::bind_method(D_METHOD("get_water_level_at", "position"), &SnowCover3D::get_water_level_at);
	ClassDB::bind_method(D_METHOD("get_ice_thickness_at", "position"), &SnowCover3D::get_ice_thickness_at);
	ClassDB::bind_method(D_METHOD("get_ice_break_count"), &SnowCover3D::get_ice_break_count);
	ClassDB::bind_static_method("SnowCover3D", D_METHOD("ice_breaking_load", "ice", "white_share", "air_celsius"), &SnowCover3D::ice_breaking_load);
	ClassDB::bind_method(D_METHOD("get_white_ice_thickness_at", "position"), &SnowCover3D::get_white_ice_thickness_at);
	ClassDB::bind_method(D_METHOD("get_lake_ice"), &SnowCover3D::get_lake_ice);
	ClassDB::bind_method(D_METHOD("get_bodies"), &SnowCover3D::get_bodies);
	ClassDB::bind_method(D_METHOD("get_avalanches"), &SnowCover3D::get_avalanches);
	ClassDB::bind_method(D_METHOD("set_avalanches_enabled", "enabled"), &SnowCover3D::set_avalanches_enabled);
	ClassDB::bind_method(D_METHOD("is_avalanches_enabled"), &SnowCover3D::is_avalanches_enabled);
	ClassDB::bind_method(D_METHOD("set_bodies_enabled", "enabled"), &SnowCover3D::set_bodies_enabled);
	ClassDB::bind_method(D_METHOD("is_bodies_enabled"), &SnowCover3D::is_bodies_enabled);
	ClassDB::bind_method(D_METHOD("set_lake_ice_enabled", "enabled"), &SnowCover3D::set_lake_ice_enabled);
	ClassDB::bind_method(D_METHOD("set_salt_melt", "enabled"), &SnowCover3D::set_salt_melt);
	ClassDB::bind_method(D_METHOD("is_salt_melt_enabled"), &SnowCover3D::is_salt_melt_enabled);
	ClassDB::bind_method(D_METHOD("get_salt_depression_at", "position"), &SnowCover3D::get_salt_depression_at);
	ClassDB::bind_method(D_METHOD("set_impurities_enabled", "enabled"), &SnowCover3D::set_impurities_enabled);
	ClassDB::bind_method(D_METHOD("is_impurities_enabled"), &SnowCover3D::is_impurities_enabled);
	ClassDB::bind_method(D_METHOD("add_impurity", "position", "radius", "mg_per_m2"), &SnowCover3D::add_impurity);
	ClassDB::bind_method(D_METHOD("get_impurity_at", "position"), &SnowCover3D::get_impurity_at);
	ClassDB::bind_static_method("SnowCover3D", D_METHOD("impurity_albedo_drop", "mg_per_m2"), &SnowCover3D::impurity_albedo_drop);
	ClassDB::bind_method(D_METHOD("is_lake_ice_enabled"), &SnowCover3D::is_lake_ice_enabled);
	ClassDB::bind_method(D_METHOD("get_snow_texture"), &SnowCover3D::get_snow_texture);
	ClassDB::bind_method(D_METHOD("set_precipitation_gradient", "share"), &SnowCover3D::set_precipitation_gradient);
	ClassDB::bind_method(D_METHOD("get_precipitation_gradient"), &SnowCover3D::get_precipitation_gradient);
	ClassDB::bind_method(D_METHOD("set_climate_map", "path"), &SnowCover3D::set_climate_map);
	ClassDB::bind_method(D_METHOD("get_climate_map"), &SnowCover3D::get_climate_map);
	ClassDB::bind_method(D_METHOD("get_orographic_factor_at", "position", "reference_height"), &SnowCover3D::get_orographic_factor_at);
	ClassDB::bind_method(D_METHOD("set_climate", "climate"), &SnowCover3D::set_climate);
	ClassDB::bind_method(D_METHOD("get_climate"), &SnowCover3D::get_climate);
	ClassDB::bind_method(D_METHOD("set_world_update_every", "updates"), &SnowCover3D::set_world_update_every);
	ClassDB::bind_method(D_METHOD("get_world_update_every"), &SnowCover3D::get_world_update_every);
	ClassDB::bind_method(D_METHOD("bake_climate", "rect", "resolution"), &SnowCover3D::bake_climate);
	ClassDB::bind_method(D_METHOD("has_world"), &SnowCover3D::has_world);
	ClassDB::bind_method(D_METHOD("get_world_rect"), &SnowCover3D::get_world_rect);
	ClassDB::bind_method(D_METHOD("get_world_depth_at", "position"), &SnowCover3D::get_world_depth_at);
	ClassDB::bind_method(D_METHOD("get_world_swe_at", "position"), &SnowCover3D::get_world_swe_at);
	ClassDB::bind_method(D_METHOD("get_cornice_at", "position"), &SnowCover3D::get_cornice_at);
	ClassDB::bind_method(D_METHOD("get_world_texture"), &SnowCover3D::get_world_texture);
	ClassDB::bind_method(D_METHOD("advance_world", "seconds"), &SnowCover3D::advance_world);
	ClassDB::bind_static_method("SnowCover3D", D_METHOD("cornice_growth_share", "wind_ratio"), &SnowCover3D::cornice_growth_share);
	ClassDB::bind_static_method("SnowCover3D", D_METHOD("cornice_root_stress", "length", "crack", "liquid"), &SnowCover3D::cornice_root_stress);
	ClassDB::bind_static_method("SnowCover3D", D_METHOD("cornice_strength", "liquid"), &SnowCover3D::cornice_strength);
	ClassDB::bind_static_method("SnowCover3D", D_METHOD("cornice_crack_rate", "length", "crack", "celsius", "liquid"), &SnowCover3D::cornice_crack_rate);
	ClassDB::bind_method(D_METHOD("get_cornice_crack_at", "position"), &SnowCover3D::get_cornice_crack_at);
	ClassDB::bind_method(D_METHOD("set_cornice_failure_enabled", "enabled"), &SnowCover3D::set_cornice_failure_enabled);
	ClassDB::bind_method(D_METHOD("is_cornice_failure_enabled"), &SnowCover3D::is_cornice_failure_enabled);
	ClassDB::bind_method(D_METHOD("get_cornice_failure_count"), &SnowCover3D::get_cornice_failure_count);
	ADD_SIGNAL(MethodInfo("cornice_failed", PropertyInfo(Variant::VECTOR3, "position"), PropertyInfo(Variant::VECTOR3, "direction"), PropertyInfo(Variant::FLOAT, "length"), PropertyInfo(Variant::FLOAT, "mass")));
	ClassDB::bind_static_method("SnowCover3D", D_METHOD("snowfall_fraction", "celsius", "humidity"), &SnowCover3D::snowfall_fraction);
	ClassDB::bind_static_method("SnowCover3D", D_METHOD("new_snow_density", "celsius"), &SnowCover3D::new_snow_density);
	ClassDB::bind_static_method("SnowCover3D", D_METHOD("lake_ice_growth", "ice", "air_celsius", "snow_depth", "snow_density", "hours"), &SnowCover3D::lake_ice_growth);
	ClassDB::bind_static_method("SnowCover3D", D_METHOD("snow_conductivity", "density"), &SnowCover3D::snow_conductivity);
	ClassDB::bind_static_method("SnowCover3D", D_METHOD("holding_depth", "slope_degrees", "min_mm", "decay"), &SnowCover3D::holding_depth);

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "extent", PROPERTY_HINT_RANGE, "16,16384,1,or_greater,suffix:m"), "set_extent", "get_extent");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "resolution", PROPERTY_HINT_RANGE, "4,2048,1"), "set_resolution", "get_resolution");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "follow_target", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D"), "set_follow_target", "get_follow_target");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "update_rate", PROPERTY_HINT_RANGE, "0.01,60,0.01,suffix:Hz"), "set_update_rate", "get_update_rate");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "background_updates"), "set_background_updates", "is_background_updates");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "lake_ice_enabled"), "set_lake_ice_enabled", "is_lake_ice_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "salt_melt"), "set_salt_melt", "is_salt_melt_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "impurities_enabled"), "set_impurities_enabled", "is_impurities_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "bodies_enabled"), "set_bodies_enabled", "is_bodies_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "avalanches_enabled"), "set_avalanches_enabled", "is_avalanches_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "height_source"), "set_height_source", "get_height_source");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "ground_collision_mask", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_ground_collision_mask", "get_ground_collision_mask");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "shelter_collision_mask", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_shelter_collision_mask", "get_shelter_collision_mask");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "canopy_enabled"), "set_canopy_enabled", "is_canopy_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "sun_light", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "DirectionalLight3D"), "set_sun_light", "get_sun_light");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lapse_rate", PROPERTY_HINT_RANGE, "0,15,0.1,suffix:C/km"), "set_lapse_rate", "get_lapse_rate");
	ADD_GROUP("Without Weather", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "temperature", PROPERTY_HINT_RANGE, "-60,40,0.1,suffix:C"), "set_temperature", "get_temperature");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "humidity", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_humidity", "get_humidity");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "precipitation_rate", PROPERTY_HINT_RANGE, "0,100,0.1,or_greater,suffix:mm/h"), "set_precipitation_rate", "get_precipitation_rate");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "time_scale", PROPERTY_HINT_RANGE, "0,10000,0.1,or_greater"), "set_time_scale", "get_time_scale");
	ADD_GROUP("Melt", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "melt_threshold", PROPERTY_HINT_RANGE, "-5,5,0.1,suffix:C"), "set_melt_threshold", "get_melt_threshold");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "melt_temperature_factor", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_melt_temperature_factor", "get_melt_temperature_factor");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "melt_shortwave_factor", PROPERTY_HINT_RANGE, "0,0.1,0.0001"), "set_melt_shortwave_factor", "get_melt_shortwave_factor");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "liquid_capacity", PROPERTY_HINT_RANGE, "0,0.5,0.01"), "set_liquid_capacity", "get_liquid_capacity");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "crust_enabled"), "set_crust_enabled", "is_crust_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "hoar_enabled"), "set_hoar_enabled", "is_hoar_enabled");
	ADD_GROUP("Slide", "slide_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "slide_enabled"), "set_slide_enabled", "is_slide_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "slide_interval", PROPERTY_HINT_RANGE, "0,86400,1,suffix:s"), "set_slide_interval", "get_slide_interval");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "slide_angle", PROPERTY_HINT_RANGE, "1,89,0.1,degrees"), "set_slide_angle", "get_slide_angle");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "slide_min_holding", PROPERTY_HINT_RANGE, "0,1000,0.1,suffix:mm"), "set_slide_min_holding", "get_slide_min_holding");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "slide_holding_decay", PROPERTY_HINT_RANGE, "0,0.5,0.001"), "set_slide_holding_decay", "get_slide_holding_decay");
	ADD_GROUP("Drift", "drift_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "drift_enabled"), "set_drift_enabled", "is_drift_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "drift_distance", PROPERTY_HINT_RANGE, "1,1000,1,suffix:m"), "set_drift_distance", "get_drift_distance");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "drift_obstacles"), "set_drift_obstacles", "is_drift_obstacles_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "drift_relief"), "set_drift_relief", "is_drift_relief_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "layers_enabled"), "set_layers_enabled", "is_layers_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "drift_strength", PROPERTY_HINT_RANGE, "0,2,0.01"), "set_drift_strength", "get_drift_strength");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "drift_threshold_wind", PROPERTY_HINT_RANGE, "0,40,0.1,suffix:m/s"), "set_drift_threshold_wind", "get_drift_threshold_wind");
	ADD_GROUP("Wind Transport", "wind_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "wind_transport_enabled"), "set_wind_transport_enabled", "is_wind_transport_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wind_curvature_length", PROPERTY_HINT_RANGE, "0,500,1,suffix:m"), "set_wind_curvature_length", "get_wind_curvature_length");
	ADD_GROUP("World", "");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "climate", PROPERTY_HINT_RESOURCE_TYPE, "SnowClimate"), "set_climate", "get_climate");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "world_update_every", PROPERTY_HINT_RANGE, "1,120,1"), "set_world_update_every", "get_world_update_every");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "precipitation_gradient", PROPERTY_HINT_RANGE, "0,0.5,0.005"), "set_precipitation_gradient", "get_precipitation_gradient");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "climate_map", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "TerrainDataMap3D"), "set_climate_map", "get_climate_map");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "cornice_failure_enabled"), "set_cornice_failure_enabled", "is_cornice_failure_enabled");
	ADD_GROUP("", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "publish_shader_globals"), "set_publish_shader_globals", "get_publish_shader_globals");
}

SnowCover3D::SnowCover3D() {
	_allocate();
}

SnowCover3D::~SnowCover3D() {
	if (cover_task != WorkerThreadPool::INVALID_TASK_ID) {
		WorkerThreadPool::get_singleton()->wait_for_task_completion(cover_task);
		cover_task = WorkerThreadPool::INVALID_TASK_ID;
	}
	_finish_sx(true);
	_free_world();
	if (active == this) {
		active = nullptr;
	}
}

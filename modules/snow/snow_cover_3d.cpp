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
	for (Vector<float> *f : { &cell_crust, &cell_crust_cycles, &cell_crust_burial, &cell_crust_wet, &cell_hoar, &cell_hoar_layer, &cell_hoar_burial, &cell_hoar_age, &cell_cups, &cell_seal, &cell_slush, &cell_packed, &cell_glaze, &cell_lap, &cell_lap_deep, &cell_lap_cover, &cell_lap_in }) {
		f->resize(nn);
		f->fill(0.0f);
	}
	catch_image = Image::create_empty(resolution, resolution, false, Image::FORMAT_RGBAF);
	catch_texture.unref();
	cups_image = Image::create_empty(resolution, resolution, false, Image::FORMAT_RGBAH);
	cups_texture.unref();
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
	Vector<float> *fields[] = { &cell_ground, &cell_swe, &cell_liquid, &cell_density, &cell_albedo, &cell_runoff, &cell_sx, &cell_new_depth, &cell_canopy, &cell_canopy_load, &cell_crust, &cell_crust_cycles, &cell_crust_burial, &cell_crust_wet, &cell_hoar, &cell_hoar_layer, &cell_hoar_burial, &cell_hoar_age, &cell_cups, &cell_seal, &cell_slush, &cell_packed, &cell_glaze, &cell_climate_precip, &cell_water, &cell_ice, &cell_white_ice, &cell_lap, &cell_lap_deep, &cell_lap_cover, &cell_lap_in, &cell_salt_pack, &cell_salt_seen };
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
		const float share = snowfall_fraction(t, in.humidity);
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
		if (snow > 0.0f && crust > 0.0f) {
			burial += snow / new_snow_density(t); // new snow buries the crust
		}

		cell_new_depth.write[i] = 0.0f;
		if (snow > 0.0f) {
			const float rho_new = new_snow_density(t);
			const float depth_old = swe > SWE_MIN ? swe / rho : 0.0f;
			const float depth_new = snow / rho_new;
			cell_new_depth.write[i] = depth_new;
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
		if (cell_cups.size() == n * n) {
			cell_cups.write[i] = suncups_enabled && swe > SWE_MIN ? suncup_step(cell_cups[i], sun_melt, air_melt, rain, snow / new_snow_density(t), lap_on ? lap : 0.0f, swe / MAX(rho, 30.0f)) : 0.0f;
		}
		cell_crust.write[i] = crust;
		cell_crust_cycles.write[i] = cycles;
		// The snow over the crust settles with the pack.
		cell_crust_burial.write[i] = swe > SWE_MIN ? MIN(burial * rho_before / MAX(rho, 30.0f), swe / MAX(rho, 30.0f)) : 0.0f;
		cell_crust_wet.write[i] = crust_wet;
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
static constexpr float HOAR_MM_PER_KG = 70.0f; // crystal height per kg/m2 deposited (a 0.07 kg/m2 night: 5 mm)
static constexpr float HOAR_MAX = 30.0f; // mm
static constexpr float HOAR_BREAK_WIND = 5.0f; // m/s at 10 m
static constexpr float HOAR_MIN = 0.5f; // mm: smaller crystals do not make a layer
static constexpr float HOAR_KEEP_HOURS = 1000.0f; // a buried layer counts as weak for about six weeks

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
				cp[i] = cm->get_precipitation_at(pos);
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
	return Object::cast_to<TerrainDataMap3D>(o->get_node_or_null(o->climate_map));
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
					// g/m2 over 2000 m, times 0.002 m/s, a tenth black carbon: mg/m2 per hour.
					dep += MAX(wm->smoke_at(pos), 0.0f) / 2000.0f * 0.002f * 0.1f * 3.6e6f;
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
	cell_slush.ptrw(); // published as pub.slush
	cell_glaze.ptrw(); // published as pub.glaze
	cell_packed.ptrw(); // published as pub.packed
	cell_new_depth.ptrw();
	cell_canopy_load.ptrw();
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
	// Flux and what each cell can give (the loose snow over a crust; none under a frozen surface crust).
	LocalVector<float> flux, loose;
	flux.resize(nn);
	loose.resize(nn);
	float qmax = 0.0f;
	const Inputs &in = inputs;
	for (int i = 0; i < nn; i++) {
		flux[i] = 0.0f;
		loose[i] = 0.0f;
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
		// Behind rocks, walls, buildings and vehicles the wind slows below what lifts snow and the snow carried in
		// drops (SnowTran-3D's flux divergence, Liston and Sturm 1998): a lee drift.
		if (obstacle_active && cell_lee.size() == nn) {
			w *= cell_lee[i];
		}
		flux[i] = transport_flux(u * w, t, cell_density[i]);
		loose[i] = give;
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
				// Laid down wind-packed; it buries a crust.
				const float depth_old = old > 0.0f ? old / rho[i] : 0.0f;
				swe[i] = old + g;
				rho[i] = swe[i] / MAX(depth_old + g / WIND_SNOW_DENSITY, 1e-6f);
				if (cell_crust[i] > 0.0f) {
					burial[i] += g / WIND_SNOW_DENSITY;
				}
				// A fresh deposit can blow on.
				if (flux[i] > 0.0f || cell_crust[i] <= 0.0f) {
					loose[i] += g;
				}
			} else {
				// Taken from the top: the pack below keeps its density.
				swe[i] = MAX(old + g, 0.0f);
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
		f->set("vorticity", 2.0f);
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
		const float depth_old = cell_swe[i] > SWE_MIN ? cell_swe[i] / cell_density[i] : 0.0f;
		const float swe = (cell_swe[i] > SWE_MIN ? cell_swe[i] : 0.0f) + MAX(p_swe_mm, 0.0f);
		cell_swe.write[i] = swe;
		cell_density.write[i] = swe / MAX(depth_old + MAX(p_swe_mm, 0.0f) / rho_new, 1e-6f);
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
	return MAX(rate, 0.0f) * get_orographic_factor_at(p_position, ref) * snowfall_fraction(get_air_temperature_at(p_position), rh);
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

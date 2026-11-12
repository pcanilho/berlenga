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

#include "snow_climate.h"
#include "snow_fall_3d.h"

#include "modules/terrain_deform/terrain_data_map_3d.h"
#include "modules/terrain_deform/terrain_deformer_3d.h"
#include "modules/weather/weather_map_3d.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/templates/sort_array.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/light_3d.h"
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
	const float rh = CLAMP(p_humidity, 0.0f, 1.0f) * 100.0f;
	const float e = -10.04f + 1.41f * p_celsius + 0.09f * rh;
	return 1.0f / (1.0f + Math::exp(CLAMP(e, -60.0f, 60.0f)));
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
	for (Vector<float> *f : { &cell_crust, &cell_crust_cycles, &cell_crust_burial, &cell_crust_wet }) {
		f->resize(nn);
		f->fill(0.0f);
	}
	catch_image = Image::create_empty(resolution, resolution, false, Image::FORMAT_RGBAF);
	catch_texture.unref();
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
	Vector<float> *fields[] = { &cell_ground, &cell_swe, &cell_liquid, &cell_density, &cell_albedo, &cell_runoff, &cell_sx, &cell_new_depth, &cell_canopy, &cell_canopy_load, &cell_crust, &cell_crust_cycles, &cell_crust_burial, &cell_crust_wet, &cell_climate_precip };
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
	const float z0 = cell_ground[p_z * n + p_x];
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
			best = MAX(best, Math::atan((cell_ground[z * n + x] - z0) / dist));
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
		in.temperature = in.weather->get_temperature();
		in.humidity = in.weather->get_humidity();
	} else {
		in.ref_height = is_inside_tree() ? get_global_position().y : 0.0f;
		in.temperature = temperature;
		in.humidity = humidity;
	}
	in.base_precip = precipitation_rate;
	// The weather the cells read, as it is now: the update may run on a worker while the map moves on.
	in.wx = in.weather ? in.weather->get_snapshot() : WeatherMap3D::Snapshot();
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
	last_wind = driven ? driven_wind : (in.weather ? in.weather->get_mean_wind_at(center) : Vector2());
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

		if (swe <= SWE_MIN) {
			// Bare ground: rain passes through.
			cell_swe.write[i] = 0.0f;
			cell_liquid.write[i] = 0.0f;
			cell_density.write[i] = 100.0f;
			cell_albedo.write[i] = ALBEDO_FRESH;
			cell_runoff.write[i] = in.dt_h > 0.0f ? (rain + swe) / in.dt_h : 0.0f;
			cell_crust.write[i] = 0.0f;
			cell_crust_cycles.write[i] = 0.0f;
			cell_crust_burial.write[i] = 0.0f;
			cell_crust_wet.write[i] = 0.0f;
			continue;
		}
		const float liq_before = liq;
		const float rho_before = rho;
		float refrozen = 0.0f;
		float surface_melt = 0.0f;

		// Rain soaks into the pack (and drains out below what it holds).
		liq += rain;
		swe += rain;

		// Melt (Pellicciotti et al. 2005): M = TF T + SRF (1 - albedo) G above the threshold temperature,
		// G the shortwave reaching the cell, its own slope facing the sun (Hock 1999).
		const bool melting = t > melt_threshold;
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
			const float m = (melt_temperature_factor * t + melt_shortwave_factor * (1.0f - alb) * g) * in.dt_h;
			surface_melt = CLAMP(m, 0.0f, MAX(swe - liq, 0.0f));
			liq += surface_melt;
		} else if (t < 0.0f && liq > 0.0f) {
			// Meltwater and rain refreeze in a cold pack, at the temperature factor's rate.
			refrozen = MIN(liq, melt_temperature_factor * -t * in.dt_h);
			liq -= refrozen;
		}
		if (crust_enabled) {
			_update_crust(crust, cycles, burial, crust_wet, rain, surface_melt / MAX(rho, 30.0f), refrozen, liq_before, liq);
		}
		swe = MAX(swe, liq);

		// The pack holds liquid_capacity of its ice as liquid; the rest drains out.
		const float hold = liquid_capacity * (swe - liq);
		if (liq > hold) {
			const float out = liq - hold;
			runoff += out;
			swe -= out;
			liq = hold;
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
		cell_crust.write[i] = crust;
		cell_crust_cycles.write[i] = cycles;
		// The snow over the crust settles with the pack.
		cell_crust_burial.write[i] = swe > SWE_MIN ? MIN(burial * rho_before / MAX(rho, 30.0f), swe / MAX(rho, 30.0f)) : 0.0f;
		cell_crust_wet.write[i] = crust_wet;
		cell_swe.write[i] = swe;
		cell_liquid.write[i] = liq;
		cell_density.write[i] = CLAMP(rho, 30.0f, 917.0f);
		cell_albedo.write[i] = alb;
		cell_runoff.write[i] = in.dt_h > 0.0f ? runoff / in.dt_h : 0.0f;
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
	const WeatherMap3D *wm = owner || !canopy_enabled ? nullptr : WeatherMap3D::get_active();
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
	for (int z = p_from_row; z < p_to_row; z++) {
		for (int x = 0; x < n; x++) {
			const int i = z * n + x;
			if (p_only && !(*p_only)[i]) {
				continue;
			}
			const Vector3 pos = _cell_center(x, z);
			if (c) {
				c[i] = wm ? CLAMP(wm->get_canopy_closure_at(pos), 0.0f, 1.0f) : 0.0f;
			}
			if (cp) {
				cp[i] = cm->get_precipitation_at(pos);
			}
		}
	}
}

// Main thread: the inputs (weather, sun, wind, Sx) for one update.
void SnowCover3D::_update_begin(double p_weather_dt, bool p_blocking) {
	_prepare_inputs(p_weather_dt, p_blocking);
	inputs.weather_dt = p_weather_dt;
	const int band = MAX(resolution / 16, 1);
	const int from = int(update_count % uint64_t((resolution + band - 1) / band)) * band;
	_refresh_cell_inputs(from, MIN(from + band, resolution), nullptr);
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
	cell_new_depth.ptrw();
	cell_canopy_load.ptrw();
	cell_cornice_x.ptrw();
	cell_cornice_z.ptrw();
	cell_cornice_crack.ptrw();
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
	update_msec = double(OS::get_singleton()->get_ticks_usec() - t0) / 1000.0;
}

// Main thread, after the cells.
void SnowCover3D::_update_end() {
	// What reaches the ground under the pack (rain through it and melt) wets surfaces in place of the
	// precipitation the weather map would put there. (Not the world tier's: the near square stands for it.)
	if (WeatherMap3D *weather = owner ? nullptr : WeatherMap3D::get_active()) {
		weather->set_surface_water_source_cells(get_instance_id(), cell_runoff, resolution, get_map_rect());
	}
	last_step_hours = inputs.dt_h;
	last_drift = inputs.wind_factor;
	update_count++;
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
	pub.runoff = cell_runoff;
	pub.canopy_load = cell_canopy_load;
	pub.new_depth = cell_new_depth;
	pub.crust = cell_crust;
	pub.crust_cycles = cell_crust_cycles;
	pub.crust_burial = cell_crust_burial;
	pub.crust_wet = cell_crust_wet;
	pub.cornice_x = cell_cornice_x;
	pub.cornice_z = cell_cornice_z;
	pub.cornice_crack = cell_cornice_crack;
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
		const float wet = hold > 1e-4f ? CLAMP(cell_liquid[i] / hold, 0.0f, 1.0f) : 0.0f;
		w[i * 4 + 0] = Math::make_half_float(depth);
		w[i * 4 + 1] = Math::make_half_float(cell_albedo[i]);
		w[i * 4 + 2] = Math::make_half_float(wet);
		w[i * 4 + 3] = Math::make_half_float(cell_density[i] / 1000.0f);
	}
	upload_snow = Image::create_from_data(n, n, false, Image::FORMAT_RGBAH, bytes);
	// The catch map: the height of the surface the pack lies on, the canopy's load over its capacity, and
	// the canopy closure. Materials leave what lies under an overhang bare and give tree crowns their load.
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
			cw[i * 4 + 3] = 1.0f;
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
	slide_enabled = owner->slide_enabled;
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
		const float w = 1.0f + 0.5f * (0.5f * slope[i] / smax) + 0.5f * (0.5f * curve[i] / cmax);
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
			int count = 0;
			for (int z = z0; z < z1; z++) {
				for (int x = x0; x < x1; x++) {
					const int i = z * n + x;
					w_sum += swe[i];
					l_sum += liq[i];
					depth_sum += swe[i] / MAX(rho[i], 30.0f);
					a_sum += swe[i] * alb[i];
					count++;
				}
			}
			if (count == 0) {
				continue;
			}
			const int wi = wz * wn + wx;
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

void SnowCover3D::add_snow(float p_swe_mm, float p_density) {
	_finish_background(true);
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
		if (cell_crust.size() == cell_swe.size() && cell_crust[i] > 0.0f) {
			cell_crust_burial.write[i] += MAX(p_swe_mm, 0.0f) / rho_new;
		}
	}
	last_step_hours = 0.0f;
	last_drift = 0.0f;
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

float SnowCover3D::get_wetness_at(Vector3 p_position) const {
	if (_in_world_only(p_position)) {
		return world->get_wetness_at(p_position);
	}
	const Vector2 p(p_position.x, p_position.z);
	const float swe = _sample(pub.swe, p);
	const float liq = _sample(pub.liquid, p);
	const float hold = liquid_capacity * (swe - liq);
	return hold > 1e-4f ? CLAMP(liq / hold, 0.0f, 1.0f) : 0.0f;
}

float SnowCover3D::get_runoff_at(Vector3 p_position) const {
	return MAX(_sample(pub.runoff, Vector2(p_position.x, p_position.z)), 0.0f);
}

float SnowCover3D::get_air_temperature_at(Vector3 p_position) const {
	const WeatherMap3D *w = WeatherMap3D::get_active();
	const float ref = w ? w->get_ground_height() : (is_inside_tree() ? get_global_position().y : 0.0f);
	const float t = w ? w->get_temperature() : temperature;
	return t - lapse_rate * 0.001f * (p_position.y - ref);
}

float SnowCover3D::get_snowfall_rate_at(Vector3 p_position) const {
	const WeatherMap3D *w = WeatherMap3D::get_active();
	const float rate = w ? w->get_rain_at(p_position) : precipitation_rate;
	const float rh = w ? w->get_humidity() : humidity;
	const float ref = w ? w->get_ground_height() : (is_inside_tree() ? get_global_position().y : 0.0f);
	return MAX(rate, 0.0f) * get_orographic_factor_at(p_position, ref) * snowfall_fraction(get_air_temperature_at(p_position), rh);
}

float SnowCover3D::get_new_snow_at(Vector3 p_position) const {
	return _sample(pub.new_depth, Vector2(p_position.x, p_position.z));
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
			}
		} break;
		case NOTIFICATION_EXIT_TREE: {
			_finish_background(true);
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
	ClassDB::bind_method(D_METHOD("add_snow", "swe_mm", "density"), &SnowCover3D::add_snow, DEFVAL(100.0f));
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
	ClassDB::bind_static_method("SnowCover3D", D_METHOD("holding_depth", "slope_degrees", "min_mm", "decay"), &SnowCover3D::holding_depth);

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "extent", PROPERTY_HINT_RANGE, "16,16384,1,or_greater,suffix:m"), "set_extent", "get_extent");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "resolution", PROPERTY_HINT_RANGE, "4,2048,1"), "set_resolution", "get_resolution");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "follow_target", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D"), "set_follow_target", "get_follow_target");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "update_rate", PROPERTY_HINT_RANGE, "0.01,60,0.01,suffix:Hz"), "set_update_rate", "get_update_rate");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "background_updates"), "set_background_updates", "is_background_updates");
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
	ADD_GROUP("Slide", "slide_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "slide_enabled"), "set_slide_enabled", "is_slide_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "slide_interval", PROPERTY_HINT_RANGE, "0,86400,1,suffix:s"), "set_slide_interval", "get_slide_interval");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "slide_angle", PROPERTY_HINT_RANGE, "1,89,0.1,degrees"), "set_slide_angle", "get_slide_angle");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "slide_min_holding", PROPERTY_HINT_RANGE, "0,1000,0.1,suffix:mm"), "set_slide_min_holding", "get_slide_min_holding");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "slide_holding_decay", PROPERTY_HINT_RANGE, "0,0.5,0.001"), "set_slide_holding_decay", "get_slide_holding_decay");
	ADD_GROUP("Drift", "drift_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "drift_enabled"), "set_drift_enabled", "is_drift_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "drift_distance", PROPERTY_HINT_RANGE, "1,1000,1,suffix:m"), "set_drift_distance", "get_drift_distance");
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

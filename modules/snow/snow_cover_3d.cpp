/**************************************************************************/
/*  snow_cover_3d.cpp                                                     */
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


#include "snow_cover_3d.h"

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
	_finish_sx(true);
	const int nn = resolution * resolution;
	cell_ground.resize(nn);
	cell_swe.resize(nn);
	cell_liquid.resize(nn);
	cell_density.resize(nn);
	cell_albedo.resize(nn);
	cell_runoff.resize(nn);
	cell_sx.resize(nn);
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
	snow_image = Image::create_empty(resolution, resolution, false, Image::FORMAT_RGBAH);
	snow_texture.unref();
	globals_added = false;
	origin_valid = false;
	sx_valid = false;
	sx_dirty = false;
}

Vector3 SnowCover3D::_cell_center(int p_x, int p_z) const {
	const float cs = _cell_size();
	const int i = p_z * resolution + p_x;
	return Vector3((origin_cell.x + p_x + 0.5f) * cs, cell_ground.size() > i ? cell_ground[i] : 0.0f, (origin_cell.y + p_z + 0.5f) * cs);
}

void SnowCover3D::_sample_heights(int p_from_row, int p_to_row, const Vector<uint8_t> *p_only) {
	const int n = resolution;
	Node *src = height_source.is_empty() || !is_inside_tree() ? nullptr : get_node_or_null(height_source);
	TerrainDeformer3D *deformer = Object::cast_to<TerrainDeformer3D>(src);
	TerrainDataMap3D *data_map = Object::cast_to<TerrainDataMap3D>(src);
	Object *getter = nullptr;
	if (src && !deformer && !data_map) {
		if (src->has_method("get_height")) {
			getter = src;
		} else if (src->has_method("get_data")) {
			// Terrain3D, by name: Terrain3D.get_data().get_height(position).
			Object *data = src->call("get_data");
			if (data && data->has_method("get_height")) {
				getter = data;
			}
		}
	}
	PhysicsDirectSpaceState3D *space = nullptr;
	if (is_inside_tree() && get_world_3d().is_valid()) {
		space = get_world_3d()->get_direct_space_state();
	}
	const float y0 = is_inside_tree() ? get_global_position().y : 0.0f;
	const float cs = _cell_size();
	for (int z = p_from_row; z < p_to_row; z++) {
		for (int x = 0; x < n; x++) {
			const int i = z * n + x;
			if (p_only && !(*p_only)[i]) {
				continue;
			}
			const Vector3 p((origin_cell.x + x + 0.5f) * cs, y0, (origin_cell.y + z + 0.5f) * cs);
			float h = Math::NaN;
			if (deformer) {
				h = deformer->get_height(p);
			} else if (data_map && data_map->is_baked() && data_map->get_map_rect().has_point(Vector2(p.x, p.z))) {
				h = data_map->get_height_at(p);
			} else if (getter) {
				h = getter->call("get_height", p);
			}
			if (!Math::is_finite(h) && space) {
				PhysicsDirectSpaceState3D::RayParameters rp;
				rp.from = Vector3(p.x, y0 + 10000.0f, p.z);
				rp.to = Vector3(p.x, y0 - 10000.0f, p.z);
				rp.collision_mask = ground_collision_mask;
				PhysicsDirectSpaceState3D::RayResult rr;
				if (space->intersect_ray(rp, rr)) {
					h = rr.position.y;
				}
			}
			cell_height_valid.write[i] = Math::is_finite(h) ? 1 : 0;
			cell_ground.write[i] = Math::is_finite(h) ? h : y0;
		}
	}
	steep_dirty = true;
}

void SnowCover3D::_follow(Vector3 p_center) {
	const float cs = _cell_size();
	const Vector2i want(int(Math::floor(p_center.x / cs)) - resolution / 2, int(Math::floor(p_center.z / cs)) - resolution / 2);
	if (!origin_valid) {
		origin_cell = want;
		origin_valid = true;
		_finish_sx(true);
		_sample_heights(0, resolution);
		sx_valid = false;
		return;
	}
	if (want != origin_cell) {
		_shift(want);
	}
}

void SnowCover3D::_shift(Vector2i p_new_origin) {
	// A bake in flight reads the ground: let it land first (it is shifted with the rest).
	_finish_sx(true);
	const int n = resolution;
	const Vector2i d = p_new_origin - origin_cell;
	// The pack is history: cells that stay keep theirs. Cells entering the square take the state of the
	// nearest cell that was inside it (the snowpack of the world tier replaces this, docs/ideas/snow.md).
	Vector<float> *fields[] = { &cell_ground, &cell_swe, &cell_liquid, &cell_density, &cell_albedo, &cell_runoff, &cell_sx };
	Vector<uint8_t> fresh;
	fresh.resize(n * n);
	for (Vector<float> *f : fields) {
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
	in.weather = WeatherMap3D::get_active();
	const Vector3 center = _follow_position();
	if (in.weather) {
		in.ref_height = in.weather->get_ground_height();
		in.temperature = in.weather->get_temperature();
		in.humidity = in.weather->get_humidity();
	} else {
		in.ref_height = is_inside_tree() ? get_global_position().y : 0.0f;
		in.temperature = temperature;
		in.humidity = humidity;
	}
	in.base_precip = precipitation_rate;

	// The sun: this node's light, else the weather map's.
	in.sun_dir = Vector3();
	in.direct_normal = 0.0f;
	const DirectionalLight3D *sun = nullptr;
	if (is_inside_tree()) {
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
	if (drift_enabled && in.weather) {
		const Vector2 wind = in.weather->get_mean_wind_at(center);
		const float u = wind.length();
		const float ut = MAX(drift_threshold_wind, 0.1f);
		in.wind_factor = CLAMP((u - ut) / ut, 0.0f, 1.0f);
		_finish_sx(p_blocking);
		if (in.wind_factor > 0.0f) {
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
		const Vector3 pos = _cell_center(x, z);
		const float h = cell_ground[i];
		const float t = in.temperature - lapse_rate * 0.001f * (h - in.ref_height);

		// Precipitation, split into snow and rain.
		const float rate = in.weather ? in.weather->get_rain_at(pos) : in.base_precip;
		const float p = MAX(rate, 0.0f) * in.dt_h;
		const float share = snowfall_fraction(t, in.humidity);
		float snow = p * share;
		const float rain = p - snow;
		if (in.wind_factor > 0.0f) {
			snow *= in.drift_norm * MAX(0.0f, 1.0f + drift_strength * in.wind_factor * CLAMP(cell_sx[i] / SX_FULL, -1.0f, 1.0f));
		}

		float swe = cell_swe[i];
		float liq = cell_liquid[i];
		float rho = cell_density[i];
		float alb = cell_albedo[i];
		float runoff = 0.0f;

		if (snow > 0.0f) {
			const float rho_new = new_snow_density(t);
			const float depth_old = swe > SWE_MIN ? swe / rho : 0.0f;
			const float depth_new = snow / rho_new;
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
			continue;
		}

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
				if (in.weather) {
					if (in.weather->get_sun_light().is_empty()) {
						vis = 1.0f - 0.75f * Math::pow(in.weather->get_cloud_cover_at(pos), 3.4f);
					} else {
						vis = in.weather->get_sun_visibility_at(pos);
					}
				}
				g = in.direct_normal * (MAX(normal.dot(in.sun_dir), 0.0f) * vis + DIFFUSE_SHARE * in.sun_dir.y);
			}
			const float m = (melt_temperature_factor * t + melt_shortwave_factor * (1.0f - alb) * g) * in.dt_h;
			liq += CLAMP(m, 0.0f, MAX(swe - liq, 0.0f));
		} else if (t < 0.0f && liq > 0.0f) {
			// Meltwater and rain refreeze in a cold pack, at the temperature factor's rate.
			liq -= MIN(liq, melt_temperature_factor * -t * in.dt_h);
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
		cell_swe.write[i] = swe;
		cell_liquid.write[i] = liq;
		cell_density.write[i] = CLAMP(rho, 30.0f, 917.0f);
		cell_albedo.write[i] = alb;
		cell_runoff.write[i] = in.dt_h > 0.0f ? runoff / in.dt_h : 0.0f;
	}
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
		}
		swe[i] = keep;
		liq[i] *= keep / (keep + out);
		surface[i] = cell_ground[i] + keep / r;
	}
}

void SnowCover3D::_update(double p_weather_dt, bool p_blocking) {
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	_prepare_inputs(p_weather_dt, p_blocking);
	WorkerThreadPool::GroupID task = WorkerThreadPool::get_singleton()->add_template_group_task(this, &SnowCover3D::_update_row, (void *)nullptr, resolution, -1, true, String("SnowCover3D cells"));
	WorkerThreadPool::get_singleton()->wait_for_group_task_completion(task);
	// Sliding is slow next to a frame: a pass every slide_interval of weather time.
	slide_accum += p_weather_dt;
	if (slide_enabled && slide_accum >= slide_interval) {
		slide_accum = 0.0;
		_slide();
	}
	// What reaches the ground under the pack (rain through it and melt) wets surfaces in place of the
	// precipitation the weather map would put there.
	if (WeatherMap3D *weather = WeatherMap3D::get_active()) {
		weather->set_surface_water_source_cells(get_instance_id(), cell_runoff, resolution, get_map_rect());
	}
	update_msec = double(OS::get_singleton()->get_ticks_usec() - t0) / 1000.0;
}

void SnowCover3D::_upload() {
	const int n = resolution;
	if (snow_image.is_null() || snow_image->get_width() != n) {
		return;
	}
	Vector<uint8_t> data;
	data.resize(n * n * 8);
	uint16_t *w = reinterpret_cast<uint16_t *>(data.ptrw());
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
	snow_image->set_data(n, n, false, Image::FORMAT_RGBAH, data);
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
		globals_added = true;
	}
	const Rect2 r = get_map_rect();
	_set_global("snow_map_rect", Vector4(r.position.x, r.position.y, r.size.x, r.size.y), RSE::GLOBAL_VAR_TYPE_VEC4);
}

void SnowCover3D::advance(float p_seconds) {
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

void SnowCover3D::clear() {
	cell_swe.fill(0.0f);
	cell_liquid.fill(0.0f);
	cell_density.fill(100.0f);
	cell_albedo.fill(ALBEDO_FRESH);
	cell_runoff.fill(0.0f);
	_upload();
}

void SnowCover3D::add_snow(float p_swe_mm, float p_density) {
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
	}
	_upload();
}

void SnowCover3D::refresh_heights() {
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

float SnowCover3D::get_depth_at(Vector3 p_position) const {
	const float swe = get_swe_at(p_position);
	return swe > SWE_MIN ? swe / MAX(get_density_at(p_position), 30.0f) : 0.0f;
}

float SnowCover3D::get_swe_at(Vector3 p_position) const {
	return MAX(_sample(cell_swe, Vector2(p_position.x, p_position.z)), 0.0f);
}

float SnowCover3D::get_density_at(Vector3 p_position) const {
	return _sample(cell_density, Vector2(p_position.x, p_position.z));
}

float SnowCover3D::get_albedo_at(Vector3 p_position) const {
	return _sample(cell_albedo, Vector2(p_position.x, p_position.z));
}

float SnowCover3D::get_wetness_at(Vector3 p_position) const {
	const Vector2 p(p_position.x, p_position.z);
	const float swe = _sample(cell_swe, p);
	const float liq = _sample(cell_liquid, p);
	const float hold = liquid_capacity * (swe - liq);
	return hold > 1e-4f ? CLAMP(liq / hold, 0.0f, 1.0f) : 0.0f;
}

float SnowCover3D::get_runoff_at(Vector3 p_position) const {
	return MAX(_sample(cell_runoff, Vector2(p_position.x, p_position.z)), 0.0f);
}

float SnowCover3D::get_ground_height_at(Vector3 p_position) const {
	return _sample(cell_ground, Vector2(p_position.x, p_position.z));
}

float SnowCover3D::get_total_water() const {
	const float cs = _cell_size();
	double sum = 0.0;
	for (int i = 0; i < cell_swe.size(); i++) {
		sum += cell_swe[i];
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
			if (WeatherMap3D *weather = WeatherMap3D::get_active()) {
				weather->clear_surface_water_source(this);
			}
			_finish_sx(true);
			if (active == this) {
				active = nullptr;
			}
			set_physics_process_internal(false);
		} break;
		case NOTIFICATION_INTERNAL_PHYSICS_PROCESS: {
			const WeatherMap3D *weather = WeatherMap3D::get_active();
			const double scale = weather ? weather->get_time_scale() : time_scale;
			update_accum += get_physics_process_delta_time();
			_follow(_follow_position());
			if (update_accum >= 1.0 / MAX(update_rate, 0.01f)) {
				_update(update_accum * scale, false);
				update_accum = 0.0;
				_upload();
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

void SnowCover3D::set_drift_threshold_wind(float p_speed) {
	drift_threshold_wind = MAX(p_speed, 0.0f);
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
	ClassDB::bind_method(D_METHOD("get_total_water"), &SnowCover3D::get_total_water);
	ClassDB::bind_method(D_METHOD("get_update_time_msec"), &SnowCover3D::get_update_time_msec);
	ClassDB::bind_method(D_METHOD("get_snow_texture"), &SnowCover3D::get_snow_texture);
	ClassDB::bind_static_method("SnowCover3D", D_METHOD("snowfall_fraction", "celsius", "humidity"), &SnowCover3D::snowfall_fraction);
	ClassDB::bind_static_method("SnowCover3D", D_METHOD("new_snow_density", "celsius"), &SnowCover3D::new_snow_density);
	ClassDB::bind_static_method("SnowCover3D", D_METHOD("holding_depth", "slope_degrees", "min_mm", "decay"), &SnowCover3D::holding_depth);

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "extent", PROPERTY_HINT_RANGE, "16,16384,1,or_greater,suffix:m"), "set_extent", "get_extent");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "resolution", PROPERTY_HINT_RANGE, "4,2048,1"), "set_resolution", "get_resolution");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "follow_target", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D"), "set_follow_target", "get_follow_target");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "update_rate", PROPERTY_HINT_RANGE, "0.01,60,0.01,suffix:Hz"), "set_update_rate", "get_update_rate");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "height_source"), "set_height_source", "get_height_source");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "ground_collision_mask", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_ground_collision_mask", "get_ground_collision_mask");
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
	ADD_GROUP("", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "publish_shader_globals"), "set_publish_shader_globals", "get_publish_shader_globals");
}

SnowCover3D::SnowCover3D() {
	_allocate();
}

SnowCover3D::~SnowCover3D() {
	_finish_sx(true);
	if (active == this) {
		active = nullptr;
	}
}

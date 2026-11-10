/**************************************************************************/
/*  weather_map_3d.cpp                                                    */
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

#include "weather_map_3d.h"

#include "weather_sky_material.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/math/random_pcg.h"
#include "core/object/class_db.h"
#include "core/object/worker_thread_pool.h"
#include "core/templates/hash_set.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/visual_instance_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "scene/resources/3d/sky_material.h"
#include "scene/resources/3d/world_3d.h"
#include "scene/resources/environment.h"
#include "scene/resources/sky.h"
#include "servers/rendering/rendering_server.h"

WeatherMap3D *WeatherMap3D::active = nullptr;

namespace {

constexpr float KARMAN = 0.4f;
// sigma_u = 2.4 u* in the surface layer (Panofsky and Dutton), so sigma_u / U10 = 2.4 kappa / ln(10 / z0)
// = 0.96 / ln(10 / z0): the IEC turbulence intensity 1 / ln(z / z0) at 10 m.
constexpr float SIGMA_U_PER_USTAR = 2.4f;
// IEC 61400-1 Kaimal model below 60 m: Lambda = 0.7 z, integral lengths 8.1, 2.7 and 0.66 Lambda, and
// sigma_v = 0.8 sigma_u, sigma_w = 0.5 sigma_u. Taken at 10 m.
constexpr float KAIMAL_LAMBDA = 7.0f;
const float KAIMAL_L[3] = { 8.1f * KAIMAL_LAMBDA, 2.7f * KAIMAL_LAMBDA, 0.66f * KAIMAL_LAMBDA };
const float KAIMAL_SIGMA[3] = { 1.0f, 0.8f, 0.5f };
// The Oseguera-Bowles radial profile (R / r)(1 - exp(-(r / R)^2)) peaks at r = 1.121 R with 0.6382.
constexpr float DOWNBURST_PEAK = 0.6382f;

// Saturation vapour pressure over water (hPa), Magnus formula (Alduchov and Eskridge 1996).
float saturation_vapour_pressure(float p_celsius) {
	return 6.112f * Math::exp(17.62f * p_celsius / (243.12f + p_celsius));
}

// Evaporation from a wet surface (mm/h): Penman's (1948) open-water wind function,
// E = 0.26 (1 + 0.54 u) (e_s - e_a) mm/day, u the wind at 2 m (m/s), vapour pressures in hPa.
float evaporation_rate(float p_wind_2m, float p_celsius, float p_humidity) {
	const float es = saturation_vapour_pressure(p_celsius);
	const float deficit = MAX(es * (1.0f - CLAMP(p_humidity, 0.0f, 1.0f)), 0.0f);
	return 0.26f * (1.0f + 0.54f * p_wind_2m) * deficit / 24.0f;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------
// Gusts

// Kaimal turbulence, in wavenumber: under Taylor's frozen turbulence f = k U / 2 pi, so with n = f L / U the
// IEC spectrum f S(f) / sigma^2 = 4 n / (1 + 6 n)^(5/3) is S(n) = 4 / (1 + 6 n)^(5/3) per unit n, whatever
// the wind speed. Its variance between n1 and n2 is (1 + 6 n1)^(-2/3) - (1 + 6 n2)^(-2/3). Each axis gets
// GUSTS_PER_AXIS log-spaced bands; a band becomes one plane wave of that variance (amplitude sqrt(2 v)),
// mostly along the wind and turned a little across it, so gusts are longer along the wind than across it
// as measured (Davenport's coherence falls faster across the wind).
void WeatherMap3D::_build_gusts() {
	RandomPCG rng(0x5eed1e57u);
	const float n_min = 0.004f;
	const float n_max = 25.0f;
	for (int axis = 0; axis < 3; axis++) {
		for (int i = 0; i < GUSTS_PER_AXIS; i++) {
			const float t0 = float(i) / GUSTS_PER_AXIS;
			const float t1 = float(i + 1) / GUSTS_PER_AXIS;
			const float n0 = i == 0 ? 0.0f : n_min * Math::pow(n_max / n_min, t0);
			const float n1 = n_min * Math::pow(n_max / n_min, t1);
			const float var = Math::pow(1.0f + 6.0f * n0, -2.0f / 3.0f) - Math::pow(1.0f + 6.0f * n1, -2.0f / 3.0f);
			const float nc = i == 0 ? 0.5f * n1 : Math::sqrt(n0 * n1);
			const float k = float(Math::TAU) * nc / KAIMAL_L[axis];
			const float turn = (rng.randf() * 2.0f - 1.0f) * 0.6f;
			Gust &g = gusts[axis * GUSTS_PER_AXIS + i];
			g.k = Vector2(k, k * turn);
			g.amplitude = KAIMAL_SIGMA[axis] * Math::sqrt(2.0f * MAX(var, 0.0f));
			g.phase = rng.randf() * float(Math::TAU);
		}
	}
}

Vector3 WeatherMap3D::_gust_at(Vector3 p_position, Vector2 p_mean) const {
	const float speed = p_mean.length();
	if (speed < 1e-3f || gust_intensity <= 0.0f) {
		return Vector3();
	}
	const float z0 = MAX(roughness_length, 1e-5f);
	const float sigma = gust_intensity * SIGMA_U_PER_USTAR * KARMAN / Math::log(10.0f / z0) * speed;
	// Phases are in the base wind's frame, so the pattern is one field everywhere.
	const Vector2 ea = cur_wind_direction;
	const Vector2 ec(-ea.y, ea.x);
	const Vector2 p(p_position.x, p_position.z);
	const float a = p.dot(ea);
	const float c = p.dot(ec);
	float g[3] = { 0, 0, 0 };
	for (int i = 0; i < GUST_COUNT; i++) {
		const Gust &gu = gusts[i];
		g[i / GUSTS_PER_AXIS] += gu.amplitude * Math::cos(gu.k.x * a + gu.k.y * c + float(gu.phase));
	}
	const Vector2 along = p_mean / speed;
	const Vector2 across(-along.y, along.x);
	const Vector2 h = (along * g[0] + across * g[1]) * sigma;
	return Vector3(h.x, g[2] * sigma, h.y);
}

// Gust patches: faster and slower blobs of air of about gust_patch_size carried across the ground, as the
// coherent sweeps that make honami over crops (Inoue 1955; Finnigan 2000) and cat's paws on water. They
// travel at about 1.8 times the wind at the top of a crop (Raupach, Finnigan and Brunet 1996).
// They belong to the air near the ground and fade out above twice their size, where the Kaimal gusts carry
// the turbulence alone.
float WeatherMap3D::_patch_at(Vector2 p_xz, float p_height) const {
	const float fade = 1.0f - Math::smoothstep(0.5f * gust_patch_size, 2.0f * gust_patch_size, p_height);
	if (gust_patch_strength <= 0.0f || fade <= 0.0f) {
		return 1.0f;
	}
	const float n = WeatherPatches::noise((p_xz - patch_drift) / gust_patch_size);
	return MAX(1.0f + fade * gust_patch_strength * (n - 0.5f) / WeatherPatches::NOISE_STD, 0.05f);
}

void WeatherMap3D::_bake_shelter() {
	shelter.ground_height = ground_height;
	LocalVector<AABB> extra;
	if (shelter_enabled && is_inside_tree()) {
		const Vector<Node *> nodes = get_tree()->get_nodes_in_group(SNAME("weather_shelter"));
		for (Node *node : nodes) {
			VisualInstance3D *vi = Object::cast_to<VisualInstance3D>(node);
			if (vi != nullptr && vi->is_visible_in_tree()) {
				extra.push_back(vi->get_global_transform().xform(vi->get_aabb()));
			}
		}
	}
	const Vector3 c = _follow_position();
	if (shelter_enabled) {
		shelter.bake(Vector2(c.x, c.z), cur_wind_direction, extra);
	} else {
		// No shelter: an empty square, so shaders still find the texture.
		Vector<WeatherShelter::Canopy> keep_c = shelter.canopies;
		Vector<WeatherShelter::Obstacle> keep_o = shelter.obstacles;
		Ref<Image> keep_i = shelter.canopy_image;
		shelter.canopies.clear();
		shelter.obstacles.clear();
		shelter.canopy_image.unref();
		shelter.bake(Vector2(c.x, c.z), cur_wind_direction, LocalVector<AABB>());
		shelter.canopies = keep_c;
		shelter.obstacles = keep_o;
		shelter.canopy_image = keep_i;
	}
	_publish_globals();
}

// ---------------------------------------------------------------------------------------------------------
// Cells

void WeatherMap3D::_allocate() {
	const int n = resolution;
	cell_wind.resize(n * n * 2);
	cell_cover.resize(n * n);
	cell_type.resize(n * n);
	cell_rain.resize(n * n);
	cell_film.resize(n * n);
	cell_puddle.resize(n * n);
	cell_inflow.resize(n * n);
	cell_inflow.fill(-1.0f);
	cell_wind.fill(0.0f);
	cell_cover.fill(0.0f);
	cell_type.fill(0.0f);
	cell_rain.fill(0.0f);
	cell_film.fill(0.0f);
	cell_puddle.fill(0.0f);
	wind_image = Image::create_empty(n, n, false, Image::FORMAT_RGBAH);
	state_image = Image::create_empty(n, n, false, Image::FORMAT_RGBAH);
	gust_image = Image::create_empty(GUST_COUNT, 1, false, Image::FORMAT_RGBAF);
	wind_texture.unref();
	state_texture.unref();
	gust_texture.unref();
	globals_added = false;
	origin_valid = false;
}

float WeatherMap3D::_cloud_noise(Vector2 p_xz) const {
	return WeatherClouds::cover_noise(p_xz, cloud_drift, cloud_scale);
}

float WeatherMap3D::_storm_weight(const Storm &p_storm) const {
	if (p_storm.lifetime <= 0.0f) {
		return Math::smoothstep(0.0f, 600.0f, p_storm.age);
	}
	const float l = p_storm.lifetime;
	return Math::smoothstep(0.0f, 0.15f * l, p_storm.age) * (1.0f - Math::smoothstep(0.75f * l, l, p_storm.age));
}

void WeatherMap3D::_ease_base(double p_dt) {
	if (!eased_once) {
		_snap();
		return;
	}
	if (transition_time <= 0.0f) {
		_set_base_to_settings();
		return;
	}
	const float a = 1.0f - Math::exp(-float(p_dt) / transition_time);
	cur_wind_speed = Math::lerp(cur_wind_speed, wind_speed, a);
	const Vector2 target = wind_direction.length_squared() > 1e-8f ? wind_direction.normalized() : Vector2(1, 0);
	const float ang = cur_wind_direction.angle_to(target);
	cur_wind_direction = cur_wind_direction.rotated(ang * a).normalized();
	cur_cloud_cover = Math::lerp(cur_cloud_cover, cloud_cover, a);
	cur_cloud_type = Math::lerp(cur_cloud_type, cloud_type, a);
	cur_rain_rate = Math::lerp(cur_rain_rate, rain_rate, a);
	cur_temperature = Math::lerp(cur_temperature, temperature, a);
	cur_humidity = Math::lerp(cur_humidity, humidity, a);
}

void WeatherMap3D::snap_to_settings() {
	_snap();
	// Refresh the cells now, so what reads them (the sea's wind) sees the snapped state at once
	// rather than at the next update, by when the base may have eased on toward newer settings.
	if (is_inside_tree() && !Engine::get_singleton()->is_editor_hint()) {
		if (cell_film.size() != resolution * resolution) {
			_allocate();
		}
		_follow(_follow_position());
		_update_cells(0.0);
		_upload();
	}
}

void WeatherMap3D::_snap() {
	// Later updates ease from here instead of snapping again to whatever the settings are by then.
	eased_once = true;
	snap_serial++;
	snap_cells_serial = cells_serial;
	_set_base_to_settings();
}

void WeatherMap3D::_set_base_to_settings() {
	cur_wind_speed = wind_speed;
	cur_wind_direction = wind_direction.length_squared() > 1e-8f ? wind_direction.normalized() : Vector2(1, 0);
	cur_cloud_cover = cloud_cover;
	cur_cloud_type = cloud_type;
	cur_rain_rate = rain_rate;
	cur_temperature = temperature;
	cur_humidity = humidity;
}

Vector3 WeatherMap3D::_follow_position() const {
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

void WeatherMap3D::_shift(Vector2i p_new_origin) {
	const int n = resolution;
	const Vector2i d = p_new_origin - origin_cell;
	// The film and puddles are history; everything else is rebuilt from the weather each update.
	double film_sum = 0.0;
	double puddle_sum = 0.0;
	for (int i = 0; i < n * n; i++) {
		film_sum += cell_film[i];
		puddle_sum += cell_puddle[i];
	}
	const float film_mean = float(film_sum / (n * n));
	const float puddle_mean = float(puddle_sum / (n * n));
	Vector<float> film = cell_film;
	Vector<float> puddle = cell_puddle;
	for (int z = 0; z < n; z++) {
		for (int x = 0; x < n; x++) {
			const int sx = x + d.x;
			const int sz = z + d.y;
			const bool inside = sx >= 0 && sz >= 0 && sx < n && sz < n;
			cell_film.write[z * n + x] = inside ? film[sz * n + sx] : film_mean;
			cell_puddle.write[z * n + x] = inside ? puddle[sz * n + sx] : puddle_mean;
		}
	}
	origin_cell = p_new_origin;
}

void WeatherMap3D::_follow(Vector3 p_center) {
	const float cs = _cell_size();
	const Vector2i want(int(Math::floor(p_center.x / cs)) - resolution / 2, int(Math::floor(p_center.z / cs)) - resolution / 2);
	if (!origin_valid) {
		origin_cell = want;
		origin_valid = true;
		return;
	}
	if (want != origin_cell) {
		_shift(want);
	}
}

Rect2 WeatherMap3D::get_map_rect() const {
	const float cs = _cell_size();
	return Rect2(origin_cell.x * cs, origin_cell.y * cs, extent, extent);
}

void WeatherMap3D::_update_cells(double p_weather_dt) {
	CellPass &c = cell_pass;
	c.dt_h = float(p_weather_dt) / 3600.0f;
	const float z0 = MAX(roughness_length, 1e-5f);
	// Wind at 2 m for Penman, from 10 m on the log law.
	c.to_2m = Math::log(2.0f / z0) / Math::log(10.0f / z0);
	c.base_wind = cur_wind_direction * cur_wind_speed;
	c.cover_gate = MAX(Math::smoothstep(0.3f, 0.7f, cur_cloud_cover), 0.05f);
	c.painted = painted_image.is_valid() && painted_map_rect.size.x > 0.0f && painted_map_rect.size.y > 0.0f;
	c.wind = cell_wind.ptrw();
	c.cover = cell_cover.ptrw();
	c.type = cell_type.ptrw();
	c.rain = cell_rain.ptrw();
	cells_serial++;
	c.film = cell_film.ptrw();
	c.puddle = cell_puddle.ptrw();
	// Snow does not wet surfaces as it falls: only the rain share reaches the film.
	c.rain_fraction = rain_fraction(cur_temperature, cur_humidity);
	_gather_water_sources();
	c.inflow = cell_inflow.ptr();
	// Rows in parallel: each cell depends only on the weather and its own history.
	WorkerThreadPool::GroupID task = WorkerThreadPool::get_singleton()->add_template_group_task(this, &WeatherMap3D::_update_row, &cell_pass, resolution, -1, true, String("WeatherMap3D cells"));
	WorkerThreadPool::get_singleton()->wait_for_group_task_completion(task);
}

float WeatherMap3D::rain_fraction(float p_celsius, float p_humidity) {
	// Jennings et al. (2018), coefficients from Jennings and Molotch (2019): the snowfall share is
	// 1 / (1 + exp(-10.04 + 1.41 T + 0.09 RH)), RH in percent.
	const float e = -10.04f + 1.41f * p_celsius + 0.09f * CLAMP(p_humidity, 0.0f, 1.0f) * 100.0f;
	return 1.0f - 1.0f / (1.0f + Math::exp(CLAMP(e, -60.0f, 60.0f)));
}

void WeatherMap3D::_gather_water_sources() {
	const int n = resolution;
	if (cell_inflow.size() != n * n) {
		cell_inflow.resize(n * n);
	}
	cell_inflow.fill(-1.0f);
	if (water_sources.is_empty() || !origin_valid) {
		return;
	}
	const float cs = _cell_size();
	float *w = cell_inflow.ptrw();
	for (uint32_t s = 0; s < water_sources.size();) {
		const WaterSource &src = water_sources[s];
		if (ObjectDB::get_instance(src.owner) == nullptr) {
			water_sources.remove_at_unordered(s);
			continue;
		}
		s++;
		const int m = src.resolution;
		if (m < 2 || src.rates.size() != m * m || src.rect.size.x <= 0.0f || src.rect.size.y <= 0.0f) {
			continue;
		}
		const float *r = src.rates.ptr();
		for (int z = 0; z < n; z++) {
			for (int x = 0; x < n; x++) {
				const Vector2 p((origin_cell.x + x + 0.5f) * cs, (origin_cell.y + z + 0.5f) * cs);
				const Vector2 uv = (p - src.rect.position) / src.rect.size;
				if (uv.x < 0.0f || uv.y < 0.0f || uv.x >= 1.0f || uv.y >= 1.0f) {
					continue;
				}
				const float fx = CLAMP(uv.x * m - 0.5f, 0.0f, float(m - 1));
				const float fz = CLAMP(uv.y * m - 0.5f, 0.0f, float(m - 1));
				const int x0 = MIN(int(fx), m - 2);
				const int z0 = MIN(int(fz), m - 2);
				const float tx = fx - x0;
				const float tz = fz - z0;
				const float a = Math::lerp(r[z0 * m + x0], r[z0 * m + x0 + 1], tx);
				const float b = Math::lerp(r[(z0 + 1) * m + x0], r[(z0 + 1) * m + x0 + 1], tx);
				const int i = z * n + x;
				w[i] = MAX(w[i], 0.0f) + MAX(Math::lerp(a, b, tz), 0.0f);
			}
		}
	}
}

void WeatherMap3D::set_surface_water_source_cells(ObjectID p_owner, const Vector<float> &p_rates, int p_resolution, Rect2 p_rect) {
	for (WaterSource &src : water_sources) {
		if (src.owner == p_owner) {
			src.rates = p_rates;
			src.resolution = p_resolution;
			src.rect = p_rect;
			return;
		}
	}
	WaterSource src;
	src.owner = p_owner;
	src.rates = p_rates;
	src.resolution = p_resolution;
	src.rect = p_rect;
	water_sources.push_back(src);
}

void WeatherMap3D::set_surface_water_source(Object *p_owner, const PackedFloat32Array &p_rates, int p_resolution, Rect2 p_rect) {
	ERR_FAIL_NULL(p_owner);
	ERR_FAIL_COND_MSG(p_rates.size() != p_resolution * p_resolution, "rates must hold resolution * resolution values.");
	set_surface_water_source_cells(p_owner->get_instance_id(), p_rates, p_resolution, p_rect);
}

void WeatherMap3D::clear_surface_water_source(Object *p_owner) {
	ERR_FAIL_NULL(p_owner);
	for (uint32_t s = 0; s < water_sources.size(); s++) {
		if (water_sources[s].owner == p_owner->get_instance_id()) {
			water_sources.remove_at_unordered(s);
			return;
		}
	}
}

void WeatherMap3D::_update_row(uint32_t p_row, CellPass *p_pass) {
	const CellPass &c = *p_pass;
	const int n = resolution;
	const float cs = _cell_size();
	const int pw = c.painted ? painted_image->get_width() : 0;
	const int ph = c.painted ? painted_image->get_height() : 0;
	const int z = int(p_row);
	for (int x = 0; x < n; x++) {
		const int i = z * n + x;
		const Vector2 p((origin_cell.x + x + 0.5f) * cs, (origin_cell.y + z + 0.5f) * cs);

		float cover = cur_cloud_cover;
		float t = cur_cloud_type;
		float rain_scale = 1.0f;
		if (c.painted) {
			const Vector2 uv = (p - painted_map_rect.position) / painted_map_rect.size;
			if (uv.x >= 0.0f && uv.y >= 0.0f && uv.x < 1.0f && uv.y < 1.0f) {
				const Color pc = painted_image->get_pixel(CLAMP(int(uv.x * pw), 0, pw - 1), CLAMP(int(uv.y * ph), 0, ph - 1));
				cover = pc.r;
				rain_scale = pc.g;
				t = pc.b;
			}
		}
		// Broken cloud: the cover is the coverage threshold of a drifting noise field (as Nubis's
		// coverage remaps its noise), so a 0.4 sky is 40% clouds in patches, not a 40% grey.
		if (cloud_variation > 0.0f) {
			cover = CLAMP(cover + (_cloud_noise(p) - 0.5f) * 2.0f * cloud_variation, 0.0f, 1.0f);
		}
		// Rain falls from the thicker cloud: the base rate, spread where the cover is.
		float r = cur_rain_rate * rain_scale * CLAMP(Math::smoothstep(0.3f, 0.7f, cover) / c.cover_gate, 0.0f, 2.5f);
		Vector2 w = c.base_wind;

		for (const Storm &s : storms) {
			const float env = _storm_weight(s);
			if (env <= 0.0f) {
				continue;
			}
			const Vector2 d = p - s.position;
			const float rr = d.length();
			const float x2 = (rr / s.radius) * (rr / s.radius);
			r += s.rain_rate * Math::exp(-x2) * env;
			const float wide = Math::exp(-x2 / (1.6f * 1.6f)) * env;
			cover = 1.0f - (1.0f - cover) * (1.0f - s.cloud_cover * wide);
			t = MAX(t, 0.9f * wide); // cumulonimbus
			if (rr > 1e-3f && s.outflow > 0.0f) {
				// Downburst outflow (Oseguera and Bowles 1988): radial, zero at the core, peaking just
				// outside the rain shaft, falling as 1 / r beyond.
				const float vr = s.outflow * (s.radius / rr) * (1.0f - Math::exp(-x2)) / DOWNBURST_PEAK;
				w += d / rr * vr * env;
			}
		}

		c.wind[i * 2 + 0] = w.x;
		c.wind[i * 2 + 1] = w.y;
		c.cover[i] = cover;
		c.type[i] = t;
		c.rain[i] = r;

		// Water on surfaces: rain fills the film up to the surface's storage, the rest pools; puddles
		// drain, then evaporation takes the puddles and then the film (Penman; near-saturated air while
		// it rains).
		if (c.dt_h > 0.0f) {
			const float rh = r > 0.1f ? MAX(cur_humidity, 0.97f) : cur_humidity;
			const float evap = evaporation_rate(w.length() * c.to_2m, cur_temperature, rh) * c.dt_h;
			// Where a snowpack lies, what reaches the surface is its runoff (rain through it and melt).
			const float in = c.inflow[i] >= 0.0f ? c.inflow[i] : r * c.rain_fraction;
			float f = c.film[i] + in * c.dt_h;
			float pd = c.puddle[i];
			if (f > surface_storage) {
				pd += f - surface_storage;
				f = surface_storage;
			}
			pd = MAX(pd - puddle_drain_rate * c.dt_h, 0.0f);
			const float from_puddle = MIN(pd, evap);
			pd -= from_puddle;
			f = MAX(f - (evap - from_puddle), 0.0f);
			c.film[i] = f;
			c.puddle[i] = CLAMP(pd, 0.0f, puddle_max_depth);
		}
	}
}

float WeatherMap3D::_sample(const Vector<float> &p_field, Vector2 p_xz, int p_stride, int p_offset) const {
	const int n = resolution;
	if (p_field.size() < n * n * p_stride || !origin_valid) {
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
	auto at = [&](int x, int z) { return f[(z * n + x) * p_stride + p_offset]; };
	return Math::lerp(Math::lerp(at(x0, z0), at(x0 + 1, z0), tx), Math::lerp(at(x0, z0 + 1), at(x0 + 1, z0 + 1), tx), tz);
}

// ---------------------------------------------------------------------------------------------------------
// Time

void WeatherMap3D::_advance(double p_weather_dt, double p_real_dt) {
	weather_time += p_weather_dt;
	_ease_base(p_weather_dt);

	// Clouds and storms drift with the wind aloft: the log law up to about 1 km.
	const float z0 = MAX(roughness_length, 1e-5f);
	const float aloft = Math::log(1000.0f / z0) / Math::log(10.0f / z0);
	const Vector2 steer = cur_wind_direction * cur_wind_speed * aloft;
	cloud_drift += steer * float(p_weather_dt);
	for (int i = storms.size() - 1; i >= 0; i--) {
		Storm &s = storms.write[i];
		s.position += steer * float(p_weather_dt);
		s.age += float(p_weather_dt);
		if (s.lifetime > 0.0f && s.age >= s.lifetime) {
			storms.remove_at(i);
		}
	}

	// Gust patches ride the wind near the ground (1 m), wrapped to their noise's period.
	{
		const float uc = 1.8f * WeatherShelter::open_profile(1.0f, roughness_length) * cur_wind_speed;
		patch_drift += cur_wind_direction * uc * float(p_real_dt);
		const float period = WeatherPatches::PERIOD * gust_patch_size;
		patch_drift = Vector2(Math::fposmod(patch_drift.x, period), Math::fposmod(patch_drift.y, period));
	}

	// Gust phases advance in real time: turbulence is not sped up with the weather. The pattern moves with
	// the base wind (frozen turbulence), each wave at k_along U.
	for (int i = 0; i < GUST_COUNT; i++) {
		Gust &g = gusts[i];
		g.phase = Math::fmod(g.phase - double(g.k.x) * cur_wind_speed * p_real_dt, Math::TAU);
	}
}

void WeatherMap3D::advance(float p_seconds) {
	if (cell_film.size() != resolution * resolution) {
		_allocate();
	}
	_follow(_follow_position());
	// Large steps in pieces, so storms move and the base eases on the way.
	double left = MAX(p_seconds, 0.0f);
	while (left > 0.0) {
		const double step = MIN(left, 60.0);
		_advance(step, 0.0);
		_update_cells(step);
		left -= step;
	}
	advanced_time += MAX(p_seconds, 0.0f);
	_upload();
}

void WeatherMap3D::_fill_gust_image() {
	Vector<uint8_t> gd;
	gd.resize(GUST_COUNT * 16);
	float *g = reinterpret_cast<float *>(gd.ptrw());
	for (int i = 0; i < GUST_COUNT; i++) {
		g[i * 4 + 0] = gusts[i].k.x;
		g[i * 4 + 1] = gusts[i].k.y;
		g[i * 4 + 2] = gusts[i].amplitude;
		g[i * 4 + 3] = float(gusts[i].phase);
	}
	gust_image->set_data(GUST_COUNT, 1, false, Image::FORMAT_RGBAF, gd);
}

void WeatherMap3D::_upload() {
	const int n = resolution;
	if (wind_image.is_null() || wind_image->get_width() != n) {
		return;
	}
	{
		Vector<uint8_t> wd;
		wd.resize(n * n * 8);
		Vector<uint8_t> sd;
		sd.resize(n * n * 8);
		uint16_t *w = reinterpret_cast<uint16_t *>(wd.ptrw());
		uint16_t *s = reinterpret_cast<uint16_t *>(sd.ptrw());
		const float storage = MAX(surface_storage, 1e-3f);
		for (int i = 0; i < n * n; i++) {
			w[i * 4 + 0] = Math::make_half_float(cell_wind[i * 2 + 0]);
			w[i * 4 + 1] = Math::make_half_float(cell_wind[i * 2 + 1]);
			w[i * 4 + 2] = Math::make_half_float(cell_cover[i]);
			w[i * 4 + 3] = Math::make_half_float(cell_type[i]);
			const float wet = cell_puddle[i] > 0.0f ? 1.0f : MIN(cell_film[i] / storage, 1.0f);
			s[i * 4 + 0] = Math::make_half_float(cell_rain[i]);
			s[i * 4 + 1] = Math::make_half_float(wet);
			s[i * 4 + 2] = Math::make_half_float(cell_puddle[i]);
			s[i * 4 + 3] = Math::make_half_float(cell_film[i]);
		}
		wind_image->set_data(n, n, false, Image::FORMAT_RGBAH, wd);
		state_image->set_data(n, n, false, Image::FORMAT_RGBAH, sd);
	}
	_fill_gust_image();
	if (wind_texture.is_null()) {
		wind_texture = ImageTexture::create_from_image(wind_image);
		state_texture = ImageTexture::create_from_image(state_image);
		gust_texture = ImageTexture::create_from_image(gust_image);
	} else {
		wind_texture->update(wind_image);
		state_texture->update(state_image);
		gust_texture->update(gust_image);
	}
	_publish_globals();
}

void WeatherMap3D::_set_global(const StringName &p_name, const Variant &p_value, int p_type) {
	RenderingServer *rs = RenderingServer::get_singleton();
	// Declared in the project settings (so the editor knows them), or added here once, at run time.
	static HashSet<StringName> added;
	if (!added.has(p_name) && !ProjectSettings::get_singleton()->has_setting("shader_globals/" + String(p_name))) {
		added.insert(p_name);
		rs->global_shader_parameter_add(p_name, RSE::GlobalShaderParameterType(p_type), p_value);
		return;
	}
	rs->global_shader_parameter_set(p_name, p_value);
}

void WeatherMap3D::_publish_globals() {
	if (!publish_shader_globals || active != this || wind_texture.is_null()) {
		return;
	}
	const Rect2 r = get_map_rect();
	const float z0 = MAX(roughness_length, 1e-5f);
	gust_sigma_scale = gust_intensity * SIGMA_U_PER_USTAR * KARMAN / Math::log(10.0f / z0);
	if (!globals_added) {
		// Texture RIDs stay the same across updates, so they are set once.
		_set_global("weather_wind_map", wind_texture->get_rid(), RSE::GLOBAL_VAR_TYPE_SAMPLER2D);
		_set_global("weather_state_map", state_texture->get_rid(), RSE::GLOBAL_VAR_TYPE_SAMPLER2D);
		_set_global("weather_gusts", gust_texture->get_rid(), RSE::GLOBAL_VAR_TYPE_SAMPLER2D);
	}
	_set_global("weather_map_rect", Vector4(r.position.x, r.position.y, r.size.x, r.size.y), RSE::GLOBAL_VAR_TYPE_VEC4);
	_set_global("weather_wind_info", Vector4(cur_wind_direction.x, cur_wind_direction.y, gust_sigma_scale, z0), RSE::GLOBAL_VAR_TYPE_VEC4);
	_set_global("weather_ground_height", ground_height, RSE::GLOBAL_VAR_TYPE_FLOAT);
	if (shelter.texture.is_valid()) {
		if (!shelter_global_added) {
			_set_global("weather_shelter_map", shelter.texture->get_rid(), RSE::GLOBAL_VAR_TYPE_SAMPLER2D);
			shelter_global_added = true;
		}
		const Rect2 sr = shelter_enabled ? shelter.rect() : Rect2();
		_set_global("weather_shelter_rect", Vector4(sr.position.x, sr.position.y, sr.size.x, sr.size.y), RSE::GLOBAL_VAR_TYPE_VEC4);
	}
	_set_global("weather_patch_info", Vector4(patch_drift.x, patch_drift.y, 1.0f / gust_patch_size, gust_patch_strength / WeatherPatches::NOISE_STD), RSE::GLOBAL_VAR_TYPE_VEC4);
	globals_added = true;
}

void WeatherMap3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			active = this;
			if (cell_film.size() != resolution * resolution) {
				_allocate();
			}
			if (!Engine::get_singleton()->is_editor_hint()) {
				set_physics_process_internal(true);
			}
		} break;
		case NOTIFICATION_READY: {
			if (!Engine::get_singleton()->is_editor_hint()) {
				_follow(_follow_position());
				_ease_base(0.0);
				_update_cells(0.0);
				_bake_shelter();
				_upload();
				_push_sky();
				update_cloud_shadow();
			}
		} break;
		case NOTIFICATION_EXIT_TREE: {
			if (active == this) {
				active = nullptr;
			}
			set_physics_process_internal(false);
			_finish_shadow_bake(true);
			_release_shadow_light();
		} break;
		case NOTIFICATION_INTERNAL_PHYSICS_PROCESS: {
			const double dt = get_physics_process_delta_time();
			const double wdt = dt * time_scale;
			_advance(wdt, dt);
			update_accum += wdt;
			const double period = time_scale / MAX(update_rate, 0.01f);
			_follow(_follow_position());
			{
				const Vector3 c = _follow_position();
				if (shelter.needs_bake(Vector2(c.x, c.z), cur_wind_direction)) {
					_bake_shelter();
				}
			}
			// Cloud shadows bake in the background; between bakes they slide with the clouds.
			shadow_timer += dt;
			if (shadow_task != WorkerThreadPool::INVALID_TASK_ID && WorkerThreadPool::get_singleton()->is_group_task_completed(shadow_task)) {
				_finish_shadow_bake(false);
			} else if (shadow_task == WorkerThreadPool::INVALID_TASK_ID && shadow_timer >= cloud_shadow_interval) {
				_start_shadow_bake();
			}
			_apply_shadow();
			if (update_accum >= period) {
				_update_cells(update_accum);
				update_accum = 0.0;
				_upload();
				_push_sky();
			} else if (gust_texture.is_valid()) {
				// The gust phases move every tick; the cells only at update_rate.
				_fill_gust_image();
				gust_texture->update(gust_image);
				_publish_globals();
			}
		} break;
	}
}

// ---------------------------------------------------------------------------------------------------------
// Queries

Vector2 WeatherMap3D::get_mean_wind_at(Vector3 p_position) const {
	const Vector2 p(p_position.x, p_position.z);
	if (!origin_valid) {
		return cur_wind_direction * cur_wind_speed;
	}
	return Vector2(_sample(cell_wind, p, 2, 0), _sample(cell_wind, p, 2, 1));
}

Vector3 WeatherMap3D::get_wind_at(Vector3 p_position, float p_height_above_ground) const {
	const float z0 = MAX(roughness_length, 1e-5f);
	float h = p_height_above_ground >= 0.0f ? p_height_above_ground : p_position.y - ground_height;
	h = MAX(h, z0 * 1.5f);
	// Mean wind on the log law, u(z) = u10 ln(z / z0) / ln(10 / z0), sheltered by canopies and obstacles and
	// carried in gust patches. The turbulence under shelter drops more slowly than the mean (Finnigan 2000).
	const Vector2 m10 = get_mean_wind_at(p_position);
	const Vector2 xz(p_position.x, p_position.z);
	float f = 1.0f;
	if (shelter_enabled) {
		float cell[4];
		shelter.sample(xz, cell);
		f = WeatherShelter::factor(h, z0, cell);
	}
	const Vector2 m = m10 * (WeatherShelter::open_profile(h, z0) * f * _patch_at(xz, h));
	const Vector3 gust = _gust_at(p_position, m10) * Math::sqrt(f);
	return Vector3(m.x, 0.0f, m.y) + gust;
}

float WeatherMap3D::get_shelter_at(Vector3 p_position, float p_height_above_ground) const {
	if (!shelter_enabled) {
		return 1.0f;
	}
	const float h = p_height_above_ground >= 0.0f ? p_height_above_ground : p_position.y - ground_height;
	float cell[4];
	shelter.sample(Vector2(p_position.x, p_position.z), cell);
	return WeatherShelter::factor(MAX(h, 0.0f), roughness_length, cell);
}

float WeatherMap3D::get_canopy_height_at(Vector3 p_position) const {
	float cell[4];
	shelter.sample(Vector2(p_position.x, p_position.z), cell);
	return MAX(cell[0], 0.0f);
}

float WeatherMap3D::get_canopy_closure_at(Vector3 p_position) const {
	// Sky closed by the canopy, from Cionco's attenuation coefficient taken as a leaf area index (both run
	// about 1 for open orchards to 4 for dense stands) through Beer's law with an extinction of 0.5
	// (spherical leaf angles, Monsi and Saeki 1953): about 0.4 for an orchard, 0.7 for a forest.
	float cell[4];
	shelter.sample(Vector2(p_position.x, p_position.z), cell);
	if (cell[0] <= 0.0f) {
		return 0.0f;
	}
	return CLAMP(1.0f - Math::exp(-0.5f * MAX(cell[1], 0.0f)), 0.0f, 1.0f);
}

float WeatherMap3D::get_rain_at(Vector3 p_position) const {
	return MAX(_sample(cell_rain, Vector2(p_position.x, p_position.z)), 0.0f);
}

float WeatherMap3D::get_cloud_cover_at(Vector3 p_position) const {
	return CLAMP(_sample(cell_cover, Vector2(p_position.x, p_position.z)), 0.0f, 1.0f);
}

float WeatherMap3D::get_cloud_type_at(Vector3 p_position) const {
	return CLAMP(_sample(cell_type, Vector2(p_position.x, p_position.z)), 0.0f, 1.0f);
}

float WeatherMap3D::get_wetness_at(Vector3 p_position) const {
	const Vector2 p(p_position.x, p_position.z);
	if (_sample(cell_puddle, p) > 0.0f) {
		return 1.0f;
	}
	return CLAMP(_sample(cell_film, p) / MAX(surface_storage, 1e-3f), 0.0f, 1.0f);
}

float WeatherMap3D::get_puddle_at(Vector3 p_position) const {
	return MAX(_sample(cell_puddle, Vector2(p_position.x, p_position.z)), 0.0f);
}

// ---------------------------------------------------------------------------------------------------------
// Storms

int WeatherMap3D::add_storm(Vector2 p_position, float p_radius, float p_rain_rate, float p_outflow, float p_lifetime) {
	Storm s;
	s.id = next_storm_id++;
	s.position = p_position;
	s.radius = MAX(p_radius, 1.0f);
	s.rain_rate = MAX(p_rain_rate, 0.0f);
	s.outflow = MAX(p_outflow, 0.0f);
	s.lifetime = p_lifetime;
	storms.push_back(s);
	return s.id;
}

void WeatherMap3D::remove_storm(int p_id) {
	for (int i = 0; i < storms.size(); i++) {
		if (storms[i].id == p_id) {
			storms.remove_at(i);
			return;
		}
	}
}

void WeatherMap3D::clear_storms() {
	storms.clear();
}

// ---------------------------------------------------------------------------------------------------------
// Settings

void WeatherMap3D::set_extent(float p_extent) {
	extent = MAX(p_extent, 16.0f);
	origin_valid = false;
}

void WeatherMap3D::set_resolution(int p_resolution) {
	resolution = CLAMP(p_resolution, 8, 1024);
	_allocate();
}

void WeatherMap3D::set_follow_target(const NodePath &p_path) {
	follow_target = p_path;
}

void WeatherMap3D::set_update_rate(float p_rate) {
	update_rate = CLAMP(p_rate, 0.1f, 120.0f);
}

void WeatherMap3D::set_time_scale(float p_scale) {
	time_scale = MAX(p_scale, 0.0f);
}

void WeatherMap3D::set_transition_time(float p_time) {
	transition_time = MAX(p_time, 0.0f);
}

void WeatherMap3D::set_ground_height(float p_height) {
	ground_height = p_height;
}

void WeatherMap3D::set_wind_speed(float p_speed) {
	wind_speed = MAX(p_speed, 0.0f);
}

void WeatherMap3D::set_wind_direction(Vector2 p_direction) {
	wind_direction = p_direction;
}

void WeatherMap3D::set_roughness_length(float p_z0) {
	roughness_length = CLAMP(p_z0, 1e-5f, 3.0f);
}

void WeatherMap3D::set_gust_intensity(float p_scale) {
	gust_intensity = MAX(p_scale, 0.0f);
}

void WeatherMap3D::set_shelter_enabled(bool p_enabled) {
	shelter_enabled = p_enabled;
	shelter.dirty = true;
}

void WeatherMap3D::set_shelter_extent(float p_extent) {
	shelter.extent = MAX(p_extent, 16.0f);
	shelter.dirty = true;
}

void WeatherMap3D::set_shelter_resolution(int p_resolution) {
	shelter.resolution = CLAMP(p_resolution, 16, 2048);
	shelter.dirty = true;
}

void WeatherMap3D::set_canopy_map(const Ref<Texture2D> &p_map) {
	canopy_map = p_map;
	shelter.canopy_image = p_map.is_valid() ? p_map->get_image() : Ref<Image>();
	if (shelter.canopy_image.is_valid() && shelter.canopy_image->is_compressed()) {
		shelter.canopy_image = shelter.canopy_image->duplicate();
		shelter.canopy_image->decompress();
	}
	shelter.dirty = true;
}

void WeatherMap3D::set_canopy_map_rect(Rect2 p_rect) {
	shelter.canopy_rect = p_rect;
	shelter.dirty = true;
}

void WeatherMap3D::set_canopy_map_max_height(float p_height) {
	shelter.canopy_max_height = MAX(p_height, 0.0f);
	shelter.dirty = true;
}

void WeatherMap3D::set_gust_patch_size(float p_size) {
	gust_patch_size = MAX(p_size, 0.5f);
}

void WeatherMap3D::set_gust_patch_strength(float p_strength) {
	gust_patch_strength = CLAMP(p_strength, 0.0f, 2.0f);
}

int WeatherMap3D::add_canopy(Vector2 p_center, float p_radius, float p_height, float p_density) {
	WeatherShelter::Canopy c;
	c.id = shelter.next_id++;
	c.center = p_center;
	c.radius = MAX(p_radius, 0.1f);
	c.height = MAX(p_height, 0.0f);
	c.density = MAX(p_density, 0.0f);
	shelter.canopies.push_back(c);
	shelter.dirty = true;
	return c.id;
}

void WeatherMap3D::remove_canopy(int p_id) {
	for (int i = 0; i < shelter.canopies.size(); i++) {
		if (shelter.canopies[i].id == p_id) {
			shelter.canopies.remove_at(i);
			shelter.dirty = true;
			return;
		}
	}
}

int WeatherMap3D::add_shelter_box(const AABB &p_box) {
	WeatherShelter::Obstacle o;
	o.id = shelter.next_id++;
	o.box = p_box.abs();
	shelter.obstacles.push_back(o);
	shelter.dirty = true;
	return o.id;
}

void WeatherMap3D::remove_shelter_box(int p_id) {
	for (int i = 0; i < shelter.obstacles.size(); i++) {
		if (shelter.obstacles[i].id == p_id) {
			shelter.obstacles.remove_at(i);
			shelter.dirty = true;
			return;
		}
	}
}

void WeatherMap3D::clear_shelter() {
	shelter.canopies.clear();
	shelter.obstacles.clear();
	shelter.dirty = true;
}

void WeatherMap3D::rebake_shelter() {
	_bake_shelter();
}

void WeatherMap3D::set_cloud_cover(float p_cover) {
	cloud_cover = CLAMP(p_cover, 0.0f, 1.0f);
}

void WeatherMap3D::set_cloud_type(float p_type) {
	cloud_type = CLAMP(p_type, 0.0f, 1.0f);
}

void WeatherMap3D::set_cloud_variation(float p_variation) {
	cloud_variation = CLAMP(p_variation, 0.0f, 1.0f);
}

void WeatherMap3D::set_cloud_scale(float p_scale) {
	cloud_scale = MAX(p_scale, 10.0f);
}

void WeatherMap3D::set_rain_rate(float p_rate) {
	rain_rate = MAX(p_rate, 0.0f);
}

void WeatherMap3D::set_temperature(float p_celsius) {
	temperature = CLAMP(p_celsius, -60.0f, 60.0f);
}

void WeatherMap3D::set_humidity(float p_humidity) {
	humidity = CLAMP(p_humidity, 0.0f, 1.0f);
}

void WeatherMap3D::set_surface_storage(float p_mm) {
	surface_storage = MAX(p_mm, 0.01f);
}

void WeatherMap3D::set_puddle_max_depth(float p_mm) {
	puddle_max_depth = MAX(p_mm, 0.0f);
}

void WeatherMap3D::set_puddle_drain_rate(float p_mm_per_hour) {
	puddle_drain_rate = MAX(p_mm_per_hour, 0.0f);
}

void WeatherMap3D::set_painted_map(const Ref<Texture2D> &p_map) {
	painted_map = p_map;
	painted_image.unref();
	if (painted_map.is_valid()) {
		Ref<Image> img = painted_map->get_image();
		if (img.is_valid()) {
			img = img->duplicate();
			if (img->is_compressed()) {
				img->decompress();
			}
			img->convert(Image::FORMAT_RGBAF);
			painted_image = img;
		}
	}
}

void WeatherMap3D::set_painted_map_rect(Rect2 p_rect) {
	painted_map_rect = p_rect;
}

void WeatherMap3D::set_publish_shader_globals(bool p_enabled) {
	publish_shader_globals = p_enabled;
	if (p_enabled) {
		_publish_globals();
	}
}

void WeatherMap3D::set_cloud_base_height(float p_height) {
	cloud_base_height = p_height;
}

void WeatherMap3D::set_cloud_thickness(float p_thickness) {
	cloud_thickness = MAX(p_thickness, 1.0f);
}

void WeatherMap3D::set_cloud_density(float p_density) {
	cloud_density = MAX(p_density, 0.0f);
}

void WeatherMap3D::set_cloud_shape_scale(float p_scale) {
	cloud_shape_scale = MAX(p_scale, 1.0f);
}

void WeatherMap3D::set_sun_light(const NodePath &p_path) {
	if (sun_light == p_path) {
		return;
	}
	_release_shadow_light();
	sun_light = p_path;
	shadow_timer = cloud_shadow_interval; // bake for it on the next tick
}

void WeatherMap3D::set_cloud_shadow_extent(float p_extent) {
	cloud_shadow_extent = MAX(p_extent, 16.0f);
}

void WeatherMap3D::set_cloud_shadow_resolution(int p_resolution) {
	cloud_shadow_resolution = CLAMP(p_resolution, 16, 2048);
}

void WeatherMap3D::set_cloud_shadow_interval(float p_seconds) {
	cloud_shadow_interval = MAX(p_seconds, 0.0f);
}

// ---------------------------------------------------------------------------------------------------------
// Clouds

WeatherClouds::State WeatherMap3D::_cloud_state() const {
	WeatherClouds::State st;
	st.base_height = cloud_base_height;
	st.thickness = cloud_thickness;
	st.density = cloud_density;
	st.cover = cur_cloud_cover;
	st.type = cur_cloud_type;
	st.variation = cloud_variation;
	st.cover_scale = cloud_scale;
	st.shape_scale = cloud_shape_scale;
	st.drift = cloud_drift;
	for (const Storm &s : storms) {
		const float env = _storm_weight(s);
		if (env <= 0.0f || st.storm_count >= WeatherClouds::MAX_STORMS) {
			continue;
		}
		float *o = st.storms[st.storm_count++];
		o[0] = s.position.x;
		o[1] = s.position.y;
		o[2] = s.radius;
		o[3] = s.cloud_cover * env;
	}
	st.update_top_max();
	return st;
}

float WeatherMap3D::get_cloud_density_at(Vector3 p_position) const {
	return WeatherClouds::extinction_at(_cloud_state(), p_position);
}

// One row of the cloud shadow: for each texel, the line through it along the sun crosses the cloud
// layer, and the optical depth along it gives the sun's transmittance (Frostbite's cloud shadow
// texture), stored sRGB-encoded as projectors are sampled. Clouds hardly absorb: what they scatter
// mostly goes on forward, so the light under a cloud is its diffuse transmission in the two-stream
// approximation, 1 / (1 + 3/4 (1 - g) tau) with g = 0.85 (Bohren 1987), not exp(-tau).
void WeatherMap3D::ShadowBake::row(uint32_t p_row, void *) {
	const int n = resolution;
	const float b = corner.y + (int(p_row) + 0.5f) * texel;
	for (int x = 0; x < n; x++) {
		const float a = corner.x + (x + 0.5f) * texel;
		const Vector3 p0 = axis_u * a + axis_v * b;
		const float s0 = (state.base_height - p0.y) / to_sun.y;
		const float s1 = (state.top_max - p0.y) / to_sun.y;
		const float ds = (s1 - s0) / steps;
		float tau = 0.0f;
		for (int i = 0; i < steps; i++) {
			tau += WeatherClouds::extinction_at(state, p0 + to_sun * (s0 + (i + 0.5f) * ds)) * ds;
		}
		const float t = 1.0f / (1.0f + 0.75f * (1.0f - 0.85f) * tau);
		const int k = int(p_row) * n + x;
		transmittance_w[k] = t;
		const float srgb = t <= 0.0031308f ? 12.92f * t : 1.055f * Math::pow(t, 1.0f / 2.4f) - 0.055f;
		const uint8_t v = uint8_t(CLAMP(int(srgb * 255.0f + 0.5f), 0, 255));
		bytes_w[k * 4 + 0] = v;
		bytes_w[k * 4 + 1] = v;
		bytes_w[k * 4 + 2] = v;
		bytes_w[k * 4 + 3] = 255;
	}
}

void WeatherMap3D::_start_shadow_bake() {
	shadow_timer = 0.0;
	DirectionalLight3D *light = Object::cast_to<DirectionalLight3D>(get_node_or_null(sun_light));
	if (!light || !is_inside_tree()) {
		return;
	}
	const Basis basis = light->get_global_basis().orthonormalized();
	const Vector3 to_sun = basis.get_column(2);
	if (to_sun.y < 0.02f) {
		return; // the sun is down: nothing to shadow
	}
	WeatherClouds::shape_noise(); // made once, before the threads read it
	if (!shadow_bake) {
		shadow_bake = memnew(ShadowBake);
	}
	ShadowBake &sb = *shadow_bake;
	sb.state = _cloud_state();
	sb.axis_u = basis.get_column(0);
	sb.axis_v = -basis.get_column(1);
	sb.to_sun = to_sun;
	sb.resolution = cloud_shadow_resolution;
	sb.texel = cloud_shadow_extent / cloud_shadow_resolution;
	const Vector3 c = _follow_position();
	const Vector2 center(c.dot(sb.axis_u), c.dot(sb.axis_v));
	sb.corner = (Vector2(Math::floor(center.x / sb.texel), Math::floor(center.y / sb.texel)) - Vector2(sb.resolution / 2, sb.resolution / 2)) * sb.texel;
	sb.transmittance.resize(sb.resolution * sb.resolution);
	sb.bytes.resize(sb.resolution * sb.resolution * 4);
	sb.transmittance_w = sb.transmittance.ptrw();
	sb.bytes_w = sb.bytes.ptrw();
	shadow_task = WorkerThreadPool::get_singleton()->add_template_group_task(shadow_bake, &ShadowBake::row, (void *)nullptr, sb.resolution, -1, false, String("Cloud shadow"));
}

void WeatherMap3D::_finish_shadow_bake(bool p_discard) {
	if (shadow_task == WorkerThreadPool::INVALID_TASK_ID) {
		return;
	}
	WorkerThreadPool::get_singleton()->wait_for_group_task_completion(shadow_task);
	shadow_task = WorkerThreadPool::INVALID_TASK_ID;
	if (p_discard || !shadow_bake) {
		return;
	}
	const ShadowBake &sb = *shadow_bake;
	shadow_axis_u = sb.axis_u;
	shadow_axis_v = sb.axis_v;
	shadow_corner = sb.corner;
	shadow_drift = sb.state.drift;
	shadow_size = sb.texel * sb.resolution;
	shadow_res = sb.resolution;
	shadow_values = sb.transmittance;
	const bool same = shadow_image.is_valid() && shadow_image->get_width() == sb.resolution;
	shadow_image = Image::create_from_data(sb.resolution, sb.resolution, false, Image::FORMAT_RGBA8, sb.bytes);
	if (shadow_texture.is_null()) {
		shadow_texture = ImageTexture::create_from_image(shadow_image);
	} else if (same) {
		shadow_texture->update(shadow_image);
	} else {
		shadow_texture->set_image(shadow_image);
	}
	_apply_shadow();
}

void WeatherMap3D::update_cloud_shadow() {
	_finish_shadow_bake(true);
	_start_shadow_bake();
	_finish_shadow_bake(false);
}

void WeatherMap3D::_apply_shadow() {
	if (shadow_texture.is_null()) {
		return;
	}
	DirectionalLight3D *light = Object::cast_to<DirectionalLight3D>(get_node_or_null(sun_light));
	if (!light) {
		return;
	}
	if (light->get_projector() != shadow_texture) {
		light->set_projector(shadow_texture);
	}
	shadow_light = light->get_instance_id();
	// The clouds have drifted since the bake: slide the shadow with them.
	const Vector2 d = cloud_drift - shadow_drift;
	const Vector3 d3(d.x, 0.0f, d.y);
	light->set_projector_size(Vector2(shadow_size, shadow_size));
	light->set_projector_offset(shadow_corner + Vector2(d3.dot(shadow_axis_u), d3.dot(shadow_axis_v)));
}

void WeatherMap3D::_release_shadow_light() {
	DirectionalLight3D *light = Object::cast_to<DirectionalLight3D>(ObjectDB::get_instance(shadow_light));
	if (light && shadow_texture.is_valid() && light->get_projector() == shadow_texture) {
		light->set_projector(Ref<Texture2D>());
	}
	shadow_light = ObjectID();
}

float WeatherMap3D::get_sun_visibility_at(Vector3 p_position) const {
	if (shadow_values.size() != shadow_res * shadow_res || shadow_res < 2) {
		return 1.0f;
	}
	const Vector2 d = cloud_drift - shadow_drift;
	const Vector3 d3(d.x, 0.0f, d.y);
	const Vector2 corner = shadow_corner + Vector2(d3.dot(shadow_axis_u), d3.dot(shadow_axis_v));
	const float texel = shadow_size / shadow_res;
	const Vector2 f = (Vector2(p_position.dot(shadow_axis_u), p_position.dot(shadow_axis_v)) - corner) / texel - Vector2(0.5f, 0.5f);
	if (f.x < -0.5f || f.y < -0.5f || f.x > shadow_res - 0.5f || f.y > shadow_res - 0.5f) {
		return 1.0f;
	}
	const int x0 = CLAMP(int(Math::floor(f.x)), 0, shadow_res - 2);
	const int y0 = CLAMP(int(Math::floor(f.y)), 0, shadow_res - 2);
	const float tx = CLAMP(f.x - x0, 0.0f, 1.0f);
	const float ty = CLAMP(f.y - y0, 0.0f, 1.0f);
	const float *v = shadow_values.ptr();
	auto at = [&](int x, int y) { return v[y * shadow_res + x]; };
	return Math::lerp(Math::lerp(at(x0, y0), at(x0 + 1, y0), tx), Math::lerp(at(x0, y0 + 1), at(x0 + 1, y0 + 1), tx), ty);
}

// The sky material of the environment the camera sees, when it is a WeatherSkyMaterial.
void WeatherMap3D::_push_sky() {
	if (active != this || !is_inside_tree()) {
		return;
	}
	Ref<Environment> env;
	Viewport *vp = get_viewport();
	Camera3D *cam = vp ? vp->get_camera_3d() : nullptr;
	if (cam && cam->get_environment().is_valid()) {
		env = cam->get_environment();
	}
	Ref<World3D> world = get_world_3d();
	if (env.is_null() && world.is_valid()) {
		env = world->get_environment().is_valid() ? world->get_environment() : world->get_fallback_environment();
	}
	if (env.is_null() || env->get_sky().is_null()) {
		return;
	}
	Ref<WeatherSkyMaterial> sky_material = env->get_sky()->get_material();
	if (sky_material.is_valid()) {
		sky_material->set_cloud_state(_cloud_state());
	}
}

// The wind as get_wind_at() computes it, for shaders: the global uniforms, then weather_wind(world position,
// height above ground). Same numbers, same noise bits.
String WeatherMap3D::get_wind_shader_code() {
	String c;
	c += "// Wind from WeatherMap3D (WeatherMap3D.get_wind_shader_code()): weather_wind(world_pos, height_above_ground).\n";
	c += "global uniform sampler2D weather_wind_map;\n";
	c += "global uniform sampler2D weather_gusts;\n";
	c += "global uniform sampler2D weather_shelter_map;\n";
	c += "global uniform vec4 weather_map_rect;\n";
	c += "global uniform vec4 weather_wind_info;\n";
	c += "global uniform vec4 weather_shelter_rect;\n";
	c += "global uniform vec4 weather_patch_info;\n";
	c += "\n";
	c += "float weather_open_profile(float z, float z0) {\n";
	c += "	return log(max(z, 1.5 * z0) / z0) / log(10.0 / z0);\n";
	c += "}\n\n";
	c += "float weather_shelter_factor(float z, float z0, vec4 c) {\n";
	c += "	z = max(z, 0.01);\n";
	c += "	float f = 1.0;\n";
	c += "	if (c.x > 0.05 && c.y > 0.01) {\n";
	c += "		float zb = max(60.0, 4.0 * c.x);\n";
	c += "		if (z < zb) {\n";
	c += "			float d = 0.7 * c.x;\n";
	c += "			float z0c = 0.1 * c.x;\n";
	c += "			float ub = weather_open_profile(zb, z0);\n";
	c += "			float span = log((zb - d) / z0c);\n";
	c += "			float u = z >= c.x ? ub * log((z - d) / z0c) / span : ub * log((c.x - d) / z0c) / span * exp(c.y * (z / c.x - 1.0));\n";
	c += "			f = clamp(u / max(weather_open_profile(z, z0), 1e-4), 0.0, 1.0);\n";
	c += "		}\n";
	c += "	}\n";
	c += "	if (c.z < 1.0 && c.w > 0.05) {\n";
	c += "		f *= mix(c.z, 1.0, smoothstep(c.w, 2.0 * c.w, z));\n";
	c += "	}\n";
	c += "	return clamp(f, 0.0, 1.0);\n";
	c += "}\n\n";
	c += "float weather_patch_hash(int x, int y) {\n";
	c += vformat("	uint h = uint(x & %d) * 1597334677u ^ uint(y & %d) * 3812015801u;\n", WeatherPatches::PERIOD - 1, WeatherPatches::PERIOD - 1);
	c += "	h ^= h >> 16u;\n";
	c += "	h *= 2146121005u;\n";
	c += "	h ^= h >> 15u;\n";
	c += "	h *= 2221713035u;\n";
	c += "	h ^= h >> 16u;\n";
	c += "	return float(h >> 8u) * (1.0 / 16777216.0);\n";
	c += "}\n\n";
	c += "float weather_patch_value(vec2 p) {\n";
	c += "	vec2 f = floor(p);\n";
	c += "	ivec2 i = ivec2(f);\n";
	c += "	vec2 t = p - f;\n";
	c += "	vec2 u = t * t * (3.0 - 2.0 * t);\n";
	c += "	float a = weather_patch_hash(i.x, i.y);\n";
	c += "	float b = weather_patch_hash(i.x + 1, i.y);\n";
	c += "	float cc = weather_patch_hash(i.x, i.y + 1);\n";
	c += "	float d = weather_patch_hash(i.x + 1, i.y + 1);\n";
	c += "	return mix(mix(a, b, u.x), mix(cc, d, u.x), u.y);\n";
	c += "}\n\n";
	c += "// Gust patches: 1 is the mean wind.\n";
	c += "float weather_patch(vec2 xz, float z) {\n";
	c += "	float fade = 1.0 - smoothstep(0.5 / weather_patch_info.z, 2.0 / weather_patch_info.z, z);\n";
	c += "	if (weather_patch_info.w <= 0.0 || fade <= 0.0) {\n";
	c += "		return 1.0;\n";
	c += "	}\n";
	c += "	vec2 q = (xz - weather_patch_info.xy) * weather_patch_info.z;\n";
	c += "	float n = 0.65 * weather_patch_value(q) + 0.35 * weather_patch_value(q * 2.0 + vec2(17.0));\n";
	c += "	return max(1.0 + fade * weather_patch_info.w * (n - 0.5), 0.05);\n";
	c += "}\n\n";
	c += "vec3 weather_gust(vec3 wp, vec2 mean10) {\n";
	c += "	float speed = length(mean10);\n";
	c += "	if (speed < 1e-3 || weather_wind_info.z <= 0.0) {\n";
	c += "		return vec3(0.0);\n";
	c += "	}\n";
	c += "	float sigma = weather_wind_info.z * speed;\n";
	c += "	vec2 ea = weather_wind_info.xy;\n";
	c += "	vec2 ec = vec2(-ea.y, ea.x);\n";
	c += "	float a = dot(wp.xz, ea);\n";
	c += "	float cr = dot(wp.xz, ec);\n";
	c += "	vec3 g = vec3(0.0);\n";
	c += vformat("	for (int i = 0; i < %d; i++) {\n", GUST_COUNT);
	c += "		vec4 w = texelFetch(weather_gusts, ivec2(i, 0), 0);\n";
	c += "		float v = w.z * cos(w.x * a + w.y * cr + w.w);\n";
	c += vformat("		if (i < %d) {\n", GUSTS_PER_AXIS);
	c += "			g.x += v;\n";
	c += vformat("		} else if (i < %d) {\n", 2 * GUSTS_PER_AXIS);
	c += "			g.y += v;\n";
	c += "		} else {\n";
	c += "			g.z += v;\n";
	c += "		}\n";
	c += "	}\n";
	c += "	vec2 along = mean10 / speed;\n";
	c += "	vec2 across = vec2(-along.y, along.x);\n";
	c += "	vec2 hz = (along * g.x + across * g.y) * sigma;\n";
	c += "	return vec3(hz.x, g.z * sigma, hz.y);\n";
	c += "}\n\n";
	c += "// Air velocity (m/s) at a world position and a height above the ground.\n";
	c += "vec3 weather_wind(vec3 wp, float height) {\n";
	c += "	if (weather_map_rect.z <= 0.0 || weather_map_rect.w <= 0.0) {\n";
	c += "		return vec3(0.0); // no WeatherMap3D: no wind\n";
	c += "	}\n";
	c += "	float z0 = max(weather_wind_info.w, 1e-5);\n";
	c += "	vec2 m10 = textureLod(weather_wind_map, (wp.xz - weather_map_rect.xy) / weather_map_rect.zw, 0.0).rg;\n";
	c += "	vec4 sh = vec4(0.0, 0.0, 1.0, 0.0);\n";
	c += "	if (weather_shelter_rect.z > 0.0) {\n";
	c += "		vec2 su = (wp.xz - weather_shelter_rect.xy) / weather_shelter_rect.zw;\n";
	c += "		if (su.x >= 0.0 && su.y >= 0.0 && su.x <= 1.0 && su.y <= 1.0) {\n";
	c += "			sh = textureLod(weather_shelter_map, su, 0.0);\n";
	c += "		}\n";
	c += "	}\n";
	c += "	float z = max(height, 1.5 * z0);\n";
	c += "	float f = weather_shelter_factor(z, z0, sh);\n";
	c += "	vec2 m = m10 * (weather_open_profile(z, z0) * f * weather_patch(wp.xz, z));\n";
	c += "	return vec3(m.x, 0.0, m.y) + weather_gust(wp, m10) * sqrt(f);\n";
	c += "}\n";
	return c;
}

void WeatherMap3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_extent", "extent"), &WeatherMap3D::set_extent);
	ClassDB::bind_method(D_METHOD("get_extent"), &WeatherMap3D::get_extent);
	ClassDB::bind_method(D_METHOD("set_resolution", "resolution"), &WeatherMap3D::set_resolution);
	ClassDB::bind_method(D_METHOD("get_resolution"), &WeatherMap3D::get_resolution);
	ClassDB::bind_method(D_METHOD("set_follow_target", "path"), &WeatherMap3D::set_follow_target);
	ClassDB::bind_method(D_METHOD("get_follow_target"), &WeatherMap3D::get_follow_target);
	ClassDB::bind_method(D_METHOD("set_update_rate", "rate"), &WeatherMap3D::set_update_rate);
	ClassDB::bind_method(D_METHOD("get_update_rate"), &WeatherMap3D::get_update_rate);
	ClassDB::bind_method(D_METHOD("set_time_scale", "scale"), &WeatherMap3D::set_time_scale);
	ClassDB::bind_method(D_METHOD("get_time_scale"), &WeatherMap3D::get_time_scale);
	ClassDB::bind_method(D_METHOD("set_transition_time", "seconds"), &WeatherMap3D::set_transition_time);
	ClassDB::bind_method(D_METHOD("get_transition_time"), &WeatherMap3D::get_transition_time);
	ClassDB::bind_method(D_METHOD("set_ground_height", "height"), &WeatherMap3D::set_ground_height);
	ClassDB::bind_method(D_METHOD("get_ground_height"), &WeatherMap3D::get_ground_height);
	ClassDB::bind_method(D_METHOD("set_wind_speed", "speed"), &WeatherMap3D::set_wind_speed);
	ClassDB::bind_method(D_METHOD("get_wind_speed"), &WeatherMap3D::get_wind_speed);
	ClassDB::bind_method(D_METHOD("set_wind_direction", "direction"), &WeatherMap3D::set_wind_direction);
	ClassDB::bind_method(D_METHOD("get_wind_direction"), &WeatherMap3D::get_wind_direction);
	ClassDB::bind_method(D_METHOD("set_roughness_length", "z0"), &WeatherMap3D::set_roughness_length);
	ClassDB::bind_method(D_METHOD("get_roughness_length"), &WeatherMap3D::get_roughness_length);
	ClassDB::bind_method(D_METHOD("set_gust_intensity", "scale"), &WeatherMap3D::set_gust_intensity);
	ClassDB::bind_method(D_METHOD("get_gust_intensity"), &WeatherMap3D::get_gust_intensity);
	ClassDB::bind_method(D_METHOD("set_cloud_cover", "cover"), &WeatherMap3D::set_cloud_cover);
	ClassDB::bind_method(D_METHOD("get_cloud_cover"), &WeatherMap3D::get_cloud_cover);
	ClassDB::bind_method(D_METHOD("set_cloud_type", "type"), &WeatherMap3D::set_cloud_type);
	ClassDB::bind_method(D_METHOD("get_cloud_type"), &WeatherMap3D::get_cloud_type);
	ClassDB::bind_method(D_METHOD("set_cloud_variation", "variation"), &WeatherMap3D::set_cloud_variation);
	ClassDB::bind_method(D_METHOD("get_cloud_variation"), &WeatherMap3D::get_cloud_variation);
	ClassDB::bind_method(D_METHOD("set_cloud_scale", "scale"), &WeatherMap3D::set_cloud_scale);
	ClassDB::bind_method(D_METHOD("get_cloud_scale"), &WeatherMap3D::get_cloud_scale);
	ClassDB::bind_method(D_METHOD("set_rain_rate", "rate"), &WeatherMap3D::set_rain_rate);
	ClassDB::bind_method(D_METHOD("get_rain_rate"), &WeatherMap3D::get_rain_rate);
	ClassDB::bind_method(D_METHOD("set_temperature", "celsius"), &WeatherMap3D::set_temperature);
	ClassDB::bind_method(D_METHOD("get_temperature"), &WeatherMap3D::get_temperature);
	ClassDB::bind_method(D_METHOD("get_air_temperature"), &WeatherMap3D::get_air_temperature);
	ClassDB::bind_method(D_METHOD("get_air_humidity"), &WeatherMap3D::get_air_humidity);
	ClassDB::bind_static_method("WeatherMap3D", D_METHOD("rain_fraction", "celsius", "humidity"), &WeatherMap3D::rain_fraction);
	ClassDB::bind_method(D_METHOD("get_canopy_height_at", "position"), &WeatherMap3D::get_canopy_height_at);
	ClassDB::bind_method(D_METHOD("get_canopy_closure_at", "position"), &WeatherMap3D::get_canopy_closure_at);
	ClassDB::bind_method(D_METHOD("set_surface_water_source", "owner", "rates", "resolution", "rect"), &WeatherMap3D::set_surface_water_source);
	ClassDB::bind_method(D_METHOD("clear_surface_water_source", "owner"), &WeatherMap3D::clear_surface_water_source);
	ClassDB::bind_method(D_METHOD("set_humidity", "humidity"), &WeatherMap3D::set_humidity);
	ClassDB::bind_method(D_METHOD("get_humidity"), &WeatherMap3D::get_humidity);
	ClassDB::bind_method(D_METHOD("set_surface_storage", "mm"), &WeatherMap3D::set_surface_storage);
	ClassDB::bind_method(D_METHOD("get_surface_storage"), &WeatherMap3D::get_surface_storage);
	ClassDB::bind_method(D_METHOD("set_puddle_max_depth", "mm"), &WeatherMap3D::set_puddle_max_depth);
	ClassDB::bind_method(D_METHOD("get_puddle_max_depth"), &WeatherMap3D::get_puddle_max_depth);
	ClassDB::bind_method(D_METHOD("set_puddle_drain_rate", "mm_per_hour"), &WeatherMap3D::set_puddle_drain_rate);
	ClassDB::bind_method(D_METHOD("get_puddle_drain_rate"), &WeatherMap3D::get_puddle_drain_rate);
	ClassDB::bind_method(D_METHOD("set_painted_map", "map"), &WeatherMap3D::set_painted_map);
	ClassDB::bind_method(D_METHOD("get_painted_map"), &WeatherMap3D::get_painted_map);
	ClassDB::bind_method(D_METHOD("set_painted_map_rect", "rect"), &WeatherMap3D::set_painted_map_rect);
	ClassDB::bind_method(D_METHOD("get_painted_map_rect"), &WeatherMap3D::get_painted_map_rect);
	ClassDB::bind_method(D_METHOD("set_publish_shader_globals", "enabled"), &WeatherMap3D::set_publish_shader_globals);
	ClassDB::bind_method(D_METHOD("get_publish_shader_globals"), &WeatherMap3D::get_publish_shader_globals);

	ClassDB::bind_method(D_METHOD("snap_to_settings"), &WeatherMap3D::snap_to_settings);
	ClassDB::bind_method(D_METHOD("advance", "seconds"), &WeatherMap3D::advance);
	ClassDB::bind_method(D_METHOD("add_storm", "position", "radius", "rain_rate", "outflow", "lifetime"), &WeatherMap3D::add_storm, DEFVAL(1500.0f), DEFVAL(30.0f), DEFVAL(10.0f), DEFVAL(3600.0f));
	ClassDB::bind_method(D_METHOD("remove_storm", "id"), &WeatherMap3D::remove_storm);
	ClassDB::bind_method(D_METHOD("clear_storms"), &WeatherMap3D::clear_storms);
	ClassDB::bind_method(D_METHOD("get_storm_count"), &WeatherMap3D::get_storm_count);
	ClassDB::bind_method(D_METHOD("get_wind_at", "position", "height_above_ground"), &WeatherMap3D::get_wind_at, DEFVAL(-1.0f));
	ClassDB::bind_method(D_METHOD("set_shelter_enabled", "enabled"), &WeatherMap3D::set_shelter_enabled);
	ClassDB::bind_method(D_METHOD("get_shelter_enabled"), &WeatherMap3D::get_shelter_enabled);
	ClassDB::bind_method(D_METHOD("set_shelter_extent", "extent"), &WeatherMap3D::set_shelter_extent);
	ClassDB::bind_method(D_METHOD("get_shelter_extent"), &WeatherMap3D::get_shelter_extent);
	ClassDB::bind_method(D_METHOD("set_shelter_resolution", "resolution"), &WeatherMap3D::set_shelter_resolution);
	ClassDB::bind_method(D_METHOD("get_shelter_resolution"), &WeatherMap3D::get_shelter_resolution);
	ClassDB::bind_method(D_METHOD("set_canopy_map", "map"), &WeatherMap3D::set_canopy_map);
	ClassDB::bind_method(D_METHOD("get_canopy_map"), &WeatherMap3D::get_canopy_map);
	ClassDB::bind_method(D_METHOD("set_canopy_map_rect", "rect"), &WeatherMap3D::set_canopy_map_rect);
	ClassDB::bind_method(D_METHOD("get_canopy_map_rect"), &WeatherMap3D::get_canopy_map_rect);
	ClassDB::bind_method(D_METHOD("set_canopy_map_max_height", "height"), &WeatherMap3D::set_canopy_map_max_height);
	ClassDB::bind_method(D_METHOD("get_canopy_map_max_height"), &WeatherMap3D::get_canopy_map_max_height);
	ClassDB::bind_method(D_METHOD("set_gust_patch_size", "size"), &WeatherMap3D::set_gust_patch_size);
	ClassDB::bind_method(D_METHOD("get_gust_patch_size"), &WeatherMap3D::get_gust_patch_size);
	ClassDB::bind_method(D_METHOD("set_gust_patch_strength", "strength"), &WeatherMap3D::set_gust_patch_strength);
	ClassDB::bind_method(D_METHOD("get_gust_patch_strength"), &WeatherMap3D::get_gust_patch_strength);
	ClassDB::bind_method(D_METHOD("add_canopy", "center", "radius", "height", "density"), &WeatherMap3D::add_canopy, DEFVAL(2.5f));
	ClassDB::bind_method(D_METHOD("remove_canopy", "id"), &WeatherMap3D::remove_canopy);
	ClassDB::bind_method(D_METHOD("add_shelter_box", "box"), &WeatherMap3D::add_shelter_box);
	ClassDB::bind_method(D_METHOD("remove_shelter_box", "id"), &WeatherMap3D::remove_shelter_box);
	ClassDB::bind_method(D_METHOD("clear_shelter"), &WeatherMap3D::clear_shelter);
	ClassDB::bind_method(D_METHOD("rebake_shelter"), &WeatherMap3D::rebake_shelter);
	ClassDB::bind_method(D_METHOD("get_shelter_bake_time"), &WeatherMap3D::get_shelter_bake_time);
	ClassDB::bind_method(D_METHOD("get_shelter_at", "position", "height_above_ground"), &WeatherMap3D::get_shelter_at, DEFVAL(-1.0f));
	ClassDB::bind_method(D_METHOD("get_shelter_texture"), &WeatherMap3D::get_shelter_texture);
	ClassDB::bind_method(D_METHOD("get_shelter_rect"), &WeatherMap3D::get_shelter_rect);
	ClassDB::bind_static_method("WeatherMap3D", D_METHOD("get_wind_shader_code"), &WeatherMap3D::get_wind_shader_code);
	ClassDB::bind_method(D_METHOD("get_mean_wind_at", "position"), &WeatherMap3D::get_mean_wind_at);
	ClassDB::bind_method(D_METHOD("get_rain_at", "position"), &WeatherMap3D::get_rain_at);
	ClassDB::bind_method(D_METHOD("get_cloud_cover_at", "position"), &WeatherMap3D::get_cloud_cover_at);
	ClassDB::bind_method(D_METHOD("get_cloud_type_at", "position"), &WeatherMap3D::get_cloud_type_at);
	ClassDB::bind_method(D_METHOD("get_wetness_at", "position"), &WeatherMap3D::get_wetness_at);
	ClassDB::bind_method(D_METHOD("get_puddle_at", "position"), &WeatherMap3D::get_puddle_at);
	ClassDB::bind_method(D_METHOD("get_weather_time"), &WeatherMap3D::get_weather_time);
	ClassDB::bind_method(D_METHOD("get_wind_texture"), &WeatherMap3D::get_wind_texture);
	ClassDB::bind_method(D_METHOD("get_state_texture"), &WeatherMap3D::get_state_texture);
	ClassDB::bind_method(D_METHOD("get_gust_texture"), &WeatherMap3D::get_gust_texture);
	ClassDB::bind_method(D_METHOD("get_map_rect"), &WeatherMap3D::get_map_rect);
	ClassDB::bind_method(D_METHOD("set_cloud_base_height", "height"), &WeatherMap3D::set_cloud_base_height);
	ClassDB::bind_method(D_METHOD("get_cloud_base_height"), &WeatherMap3D::get_cloud_base_height);
	ClassDB::bind_method(D_METHOD("set_cloud_thickness", "thickness"), &WeatherMap3D::set_cloud_thickness);
	ClassDB::bind_method(D_METHOD("get_cloud_thickness"), &WeatherMap3D::get_cloud_thickness);
	ClassDB::bind_method(D_METHOD("set_cloud_density", "density"), &WeatherMap3D::set_cloud_density);
	ClassDB::bind_method(D_METHOD("get_cloud_density"), &WeatherMap3D::get_cloud_density);
	ClassDB::bind_method(D_METHOD("set_cloud_shape_scale", "scale"), &WeatherMap3D::set_cloud_shape_scale);
	ClassDB::bind_method(D_METHOD("get_cloud_shape_scale"), &WeatherMap3D::get_cloud_shape_scale);
	ClassDB::bind_method(D_METHOD("set_sun_light", "path"), &WeatherMap3D::set_sun_light);
	ClassDB::bind_method(D_METHOD("get_sun_light"), &WeatherMap3D::get_sun_light);
	ClassDB::bind_method(D_METHOD("set_cloud_shadow_extent", "extent"), &WeatherMap3D::set_cloud_shadow_extent);
	ClassDB::bind_method(D_METHOD("get_cloud_shadow_extent"), &WeatherMap3D::get_cloud_shadow_extent);
	ClassDB::bind_method(D_METHOD("set_cloud_shadow_resolution", "resolution"), &WeatherMap3D::set_cloud_shadow_resolution);
	ClassDB::bind_method(D_METHOD("get_cloud_shadow_resolution"), &WeatherMap3D::get_cloud_shadow_resolution);
	ClassDB::bind_method(D_METHOD("set_cloud_shadow_interval", "seconds"), &WeatherMap3D::set_cloud_shadow_interval);
	ClassDB::bind_method(D_METHOD("get_cloud_shadow_interval"), &WeatherMap3D::get_cloud_shadow_interval);
	ClassDB::bind_method(D_METHOD("get_cloud_density_at", "position"), &WeatherMap3D::get_cloud_density_at);
	ClassDB::bind_method(D_METHOD("get_sun_visibility_at", "position"), &WeatherMap3D::get_sun_visibility_at);
	ClassDB::bind_method(D_METHOD("update_cloud_shadow"), &WeatherMap3D::update_cloud_shadow);
	ClassDB::bind_method(D_METHOD("get_cloud_shadow_texture"), &WeatherMap3D::get_cloud_shadow_texture);

	ADD_GROUP("Map", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "extent", PROPERTY_HINT_RANGE, "16,100000,1,or_greater,suffix:m"), "set_extent", "get_extent");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "resolution", PROPERTY_HINT_RANGE, "8,1024,1"), "set_resolution", "get_resolution");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "follow_target", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D"), "set_follow_target", "get_follow_target");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "update_rate", PROPERTY_HINT_RANGE, "0.1,120,0.1,suffix:Hz"), "set_update_rate", "get_update_rate");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "time_scale", PROPERTY_HINT_RANGE, "0,3600,0.1,or_greater"), "set_time_scale", "get_time_scale");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "transition_time", PROPERTY_HINT_RANGE, "0,86400,1,or_greater,suffix:s"), "set_transition_time", "get_transition_time");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ground_height", PROPERTY_HINT_NONE, "suffix:m"), "set_ground_height", "get_ground_height");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "publish_shader_globals"), "set_publish_shader_globals", "get_publish_shader_globals");
	ADD_GROUP("Wind", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wind_speed", PROPERTY_HINT_RANGE, "0,60,0.1,or_greater,suffix:m/s"), "set_wind_speed", "get_wind_speed");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "wind_direction"), "set_wind_direction", "get_wind_direction");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "roughness_length", PROPERTY_HINT_RANGE, "0.00001,3,0.00001,exp,suffix:m"), "set_roughness_length", "get_roughness_length");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "gust_intensity", PROPERTY_HINT_RANGE, "0,3,0.01"), "set_gust_intensity", "get_gust_intensity");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "gust_patch_size", PROPERTY_HINT_RANGE, "0.5,50,0.1,or_greater,suffix:m"), "set_gust_patch_size", "get_gust_patch_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "gust_patch_strength", PROPERTY_HINT_RANGE, "0,2,0.01"), "set_gust_patch_strength", "get_gust_patch_strength");
	ADD_GROUP("Shelter", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "shelter_enabled"), "set_shelter_enabled", "get_shelter_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "shelter_extent", PROPERTY_HINT_RANGE, "16,4096,1,or_greater,suffix:m"), "set_shelter_extent", "get_shelter_extent");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "shelter_resolution", PROPERTY_HINT_RANGE, "16,2048,1"), "set_shelter_resolution", "get_shelter_resolution");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "canopy_map", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "set_canopy_map", "get_canopy_map");
	ADD_PROPERTY(PropertyInfo(Variant::RECT2, "canopy_map_rect", PROPERTY_HINT_NONE, "suffix:m"), "set_canopy_map_rect", "get_canopy_map_rect");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "canopy_map_max_height", PROPERTY_HINT_RANGE, "0,120,0.1,or_greater,suffix:m"), "set_canopy_map_max_height", "get_canopy_map_max_height");
	ADD_GROUP("Clouds and Rain", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cloud_cover", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_cloud_cover", "get_cloud_cover");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cloud_type", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_cloud_type", "get_cloud_type");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cloud_variation", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_cloud_variation", "get_cloud_variation");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cloud_scale", PROPERTY_HINT_RANGE, "10,100000,1,or_greater,suffix:m"), "set_cloud_scale", "get_cloud_scale");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "rain_rate", PROPERTY_HINT_RANGE, "0,200,0.1,or_greater,suffix:mm/h"), "set_rain_rate", "get_rain_rate");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "painted_map", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "set_painted_map", "get_painted_map");
	ADD_PROPERTY(PropertyInfo(Variant::RECT2, "painted_map_rect", PROPERTY_HINT_NONE, "suffix:m"), "set_painted_map_rect", "get_painted_map_rect");
	ADD_GROUP("Cloud Layer", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cloud_base_height", PROPERTY_HINT_RANGE, "0,10000,1,or_greater,suffix:m"), "set_cloud_base_height", "get_cloud_base_height");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cloud_thickness", PROPERTY_HINT_RANGE, "10,10000,1,or_greater,suffix:m"), "set_cloud_thickness", "get_cloud_thickness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cloud_density", PROPERTY_HINT_RANGE, "0,4,0.01,or_greater"), "set_cloud_density", "get_cloud_density");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cloud_shape_scale", PROPERTY_HINT_RANGE, "100,50000,1,or_greater,suffix:m"), "set_cloud_shape_scale", "get_cloud_shape_scale");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "sun_light", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "DirectionalLight3D"), "set_sun_light", "get_sun_light");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cloud_shadow_extent", PROPERTY_HINT_RANGE, "16,100000,1,or_greater,suffix:m"), "set_cloud_shadow_extent", "get_cloud_shadow_extent");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "cloud_shadow_resolution", PROPERTY_HINT_RANGE, "16,2048,1"), "set_cloud_shadow_resolution", "get_cloud_shadow_resolution");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cloud_shadow_interval", PROPERTY_HINT_RANGE, "0,60,0.01,suffix:s"), "set_cloud_shadow_interval", "get_cloud_shadow_interval");
	ADD_GROUP("Air and Surfaces", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "temperature", PROPERTY_HINT_RANGE, "-60,60,0.1,suffix:C"), "set_temperature", "get_temperature");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "humidity", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_humidity", "get_humidity");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "surface_storage", PROPERTY_HINT_RANGE, "0.01,10,0.01,suffix:mm"), "set_surface_storage", "get_surface_storage");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "puddle_max_depth", PROPERTY_HINT_RANGE, "0,200,0.1,suffix:mm"), "set_puddle_max_depth", "get_puddle_max_depth");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "puddle_drain_rate", PROPERTY_HINT_RANGE, "0,100,0.1,suffix:mm/h"), "set_puddle_drain_rate", "get_puddle_drain_rate");

	BIND_CONSTANT(GUSTS_PER_AXIS);
	BIND_CONSTANT(GUST_COUNT);
}

WeatherMap3D::WeatherMap3D() {
	_build_gusts();
	_allocate();
}

WeatherMap3D::~WeatherMap3D() {
	if (active == this) {
		active = nullptr;
	}
	if (shadow_task != WorkerThreadPool::INVALID_TASK_ID) {
		WorkerThreadPool::get_singleton()->wait_for_group_task_completion(shadow_task);
	}
	memdelete_notnull(shadow_bake);
}

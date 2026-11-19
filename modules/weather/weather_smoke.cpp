/**************************************************************************/
/*  weather_smoke.cpp                                                     */
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

// Smoke haze (docs/ideas/physx.md, "Smoke haze dims the sun"). Fires report their particulate (WorldFire.
// smoke_sources); the map keeps it as a layer, grams per square metre in the column over each cell, carried by the
// wind aloft (semi-Lagrangian, so the plume spreads as it drifts) and lost over a few days (dilution out of the
// layer), faster under rain and snow (smoke_washout). Wildfire smoke's mass extinction is about 4.5 m2/g at 550 nm
// (Reid et al. 2005), its single scattering albedo about 0.9, and its Angstrom exponent about 1.8, so blue is lost
// first: thick smoke (optical depth 2 to 5) turns the sun orange, then red, and dims it to nothing; under it the day
// stays cooler (Robock 1988). The sun's colour comes through SkyClock (get_smoke_transmittance), the haze through the
// map's aerosol, the cooler day through the daily range.

#include "weather_map_3d.h"

static constexpr float SMOKE_MASS_EXTINCTION = 4.5f; // m2/g at 550 nm
static constexpr float SMOKE_DIFFUSION = 300.0f; // m2/s, horizontal eddy diffusion
static constexpr float SMOKE_LAYER = 2000.0f; // m, the layer's depth from the ground (mixed through it)
// Optical depth per colour against 550 nm: (lambda / 550)^-1.8 at 610, 550 and 465 nm.
static const Vector3 SMOKE_SPECTRUM(0.83f, 1.0f, 1.35f);

void WeatherMap3D::_smoke_step(double p_weather_dt) {
	const int n = resolution;
	if (cell_smoke.size() != n * n) {
		cell_smoke.resize(n * n);
		cell_smoke.fill(0.0f);
	}
	if (p_weather_dt <= 0.0 || !origin_valid) {
		return;
	}
	const float cs = _cell_size();
	const float dt = float(p_weather_dt);
	// Carried by the wind in the layer (the log law up to about 1 km, as the clouds).
	const float z0 = MAX(roughness_length, 1e-5f);
	const Vector2 u = cur_wind_direction * cur_wind_speed * (Math::log(1000.0f / z0) / Math::log(10.0f / z0));
	bool any = false;
	for (int i = 0; i < n * n && !any; i++) {
		any = cell_smoke[i] > 1e-6f;
	}
	if (any) {
		const Vector2 back = u * dt / cs; // cells
		const float keep = Math::exp(-dt / MAX(smoke_lifetime, 60.0f));
		// Rain and snow wash the smoke out under them (below-cloud scavenging, WorldWeather::washout_rate): a
		// shower barely thins it, hours of heavy rain clear most of it. One rain/snow split for the map, at the
		// point it follows.
		const bool wash = smoke_washout && cell_rain.size() == n * n;
		const float snow_share = wash ? 1.0f - get_rain_share_at(_follow_position()) : 0.0f;
		const float *rain = wash ? cell_rain.ptr() : nullptr;
		Vector<float> old = cell_smoke;
		const float *o = old.ptr();
		float *w = cell_smoke.ptrw();
		for (int z = 0; z < n; z++) {
			for (int x = 0; x < n; x++) {
				const float fx = x - back.x, fz = z - back.y;
				const int x0 = int(Math::floor(fx)), z0i = int(Math::floor(fz));
				const float tx = fx - x0, tz = fz - z0i;
				auto at = [&](int xx, int zz) { return (xx < 0 || zz < 0 || xx >= n || zz >= n) ? 0.0f : o[zz * n + xx]; };
				const float v = Math::lerp(Math::lerp(at(x0, z0i), at(x0 + 1, z0i), tx), Math::lerp(at(x0, z0i + 1), at(x0 + 1, z0i + 1), tx), tz);
				float k = keep;
				if (rain != nullptr && rain[z * n + x] > 0.0f) {
					const float r = rain[z * n + x];
					k *= Math::exp(-dt * WorldWeather::washout_rate(r * (1.0f - snow_share), r * snow_share, false));
				}
				w[z * n + x] = v * k;
			}
		}
		// The plume widens as it drifts: eddy diffusion across the wind at about 300 m2/s (a plume some 2 km wide 10 km
		// downwind in a breeze), explicit steps of at most a fifth of a cell's spread.
		const float alpha = SMOKE_DIFFUSION * dt / (cs * cs);
		const int iters = MIN(int(Math::ceil(alpha / 0.2f)), 64);
		const float a = MIN(alpha / MAX(iters, 1), 0.2f);
		for (int it = 0; it < iters; it++) {
			old = cell_smoke;
			o = old.ptr();
			w = cell_smoke.ptrw();
			for (int z = 0; z < n; z++) {
				for (int x = 0; x < n; x++) {
					const int i = z * n + x;
					const float l = x > 0 ? o[i - 1] : 0.0f, r = x < n - 1 ? o[i + 1] : 0.0f;
					const float d = z > 0 ? o[i - n] : 0.0f, up = z < n - 1 ? o[i + n] : 0.0f;
					w[i] = o[i] + a * (l + r + d + up - 4.0f * o[i]);
				}
			}
		}
	}
	// The fires' smoke, kg per second of their own (real) time: over the real time this step stands for.
	const WorldFire *wf = WorldContracts::fire();
	if (smoke_haze && (wf->has_provider() || wf->get_script_instance())) {
		const float real = dt / MAX(time_scale, 1e-3f);
		// What left the fire during the step has drifted downwind of it by up to the wind's run: laid along that line,
		// so a long step does not drop the smoke in puffs.
		const float run = u.length() * dt;
		const int pieces = CLAMP(int(Math::ceil(run / cs)), 1, 64);
		for (const Vector4 &s : wf->smoke_sources()) {
			for (int k = 0; k < pieces; k++) {
				const Vector2 off = u * dt * ((k + 0.5f) / pieces);
				add_smoke(Vector3(s.x + off.x, s.y, s.z + off.y), s.w * real / pieces);
			}
		}
	}
}

void WeatherMap3D::add_smoke(Vector3 p_position, float p_kg) {
	const int n = resolution;
	if (p_kg <= 0.0f || !origin_valid) {
		return;
	}
	if (cell_smoke.size() != n * n) {
		cell_smoke.resize(n * n);
		cell_smoke.fill(0.0f);
	}
	const float cs = _cell_size();
	const int x = int(Math::floor(p_position.x / cs)) - origin_cell.x;
	const int z = int(Math::floor(p_position.z / cs)) - origin_cell.y;
	if (x < 0 || z < 0 || x >= n || z >= n) {
		return;
	}
	cell_smoke.write[z * n + x] += p_kg * 1000.0f / (cs * cs);
}

void WeatherMap3D::clear_smoke() {
	cell_smoke.fill(0.0f);
	applied_aerosol = aerosol_haze ? 0.0f : applied_aerosol;
}

void WeatherMap3D::_smoke_shift(Vector2i p_delta) {
	const int n = resolution;
	if (cell_smoke.size() != n * n) {
		return;
	}
	Vector<float> old = cell_smoke;
	for (int z = 0; z < n; z++) {
		for (int x = 0; x < n; x++) {
			const int sx = x + p_delta.x, sz = z + p_delta.y;
			cell_smoke.write[z * n + x] = (sx >= 0 && sz >= 0 && sx < n && sz < n) ? old[sz * n + sx] : 0.0f;
		}
	}
}

float WeatherMap3D::get_smoke_at(Vector3 p_position) const {
	const int n = resolution;
	if (cell_smoke.size() != n * n || !origin_valid) {
		return 0.0f;
	}
	const Rect2 r = get_map_rect();
	if (!r.has_point(Vector2(p_position.x, p_position.z))) {
		return 0.0f;
	}
	return MAX(_sample(cell_smoke, Vector2(p_position.x, p_position.z), 1, 0), 0.0f);
}

float WeatherMap3D::get_smoke_optical_depth_at(Vector3 p_position) const {
	return SMOKE_MASS_EXTINCTION * get_smoke_at(p_position);
}

Vector3 WeatherMap3D::get_smoke_transmittance(Vector3 p_position, Vector3 p_to_sun) const {
	const Vector3 d = p_to_sun.normalized();
	// The sun's ray crosses the layer's middle (half its depth) here, no further out than the map reaches (a low sun
	// reads the edge of the map's smoke, not empty air beyond it); the slant path through the layer is the column over
	// sin(elevation), with a Chapman-like floor so it stays smooth to the horizon (about ten air masses there).
	const float hd = Vector2(d.x, d.z).length();
	const float reach = hd > 1e-4f ? MIN(SMOKE_LAYER * 0.5f / MAX(d.y, 1e-3f), extent * 0.45f) : 0.0f;
	const Vector3 at = p_position + (hd > 1e-4f ? Vector3(d.x, 0.0f, d.z) / hd * reach : Vector3());
	const float tau = get_smoke_optical_depth_at(at) / Math::sqrt(d.y * MAX(d.y, 0.0f) + 0.01f);
	return Vector3(Math::exp(-tau * SMOKE_SPECTRUM.x), Math::exp(-tau * SMOKE_SPECTRUM.y), Math::exp(-tau * SMOKE_SPECTRUM.z));
}

float WeatherMap3D::_smoke_sigma() const {
	if (!smoke_haze) {
		return 0.0f;
	}
	return get_smoke_optical_depth_at(_follow_position()) / SMOKE_LAYER;
}

void WeatherMap3D::set_smoke_haze(bool p_enabled) {
	smoke_haze = p_enabled;
	applied_aerosol = aerosol_haze ? 0.0f : applied_aerosol;
}

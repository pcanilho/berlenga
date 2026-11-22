/**************************************************************************/
/*  weather_rain.cpp                                                      */
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

// Rain on the weather map (docs/ideas/rain.md): the precipitation's phase with height, and the water the canopy
// holds. The rings, falling rain, drips and runoff read these.

#include "weather_map_3d.h"
#include "weather_runoff_3d.h"

#include "core/object/class_db.h"
#include "servers/rendering/rendering_server.h"

namespace {

// Rutter et al. (1975): drainage Ds exp(b (C - S)), Ds = 0.0014 mm/min, b = 3.7 per mm.
constexpr float CANOPY_DS = 0.0014f * 60.0f; // mm/h
constexpr float CANOPY_B = 3.7f; // per mm
// A wet canopy evaporates several times faster than open water: its aerodynamic resistance is a few s/m
// against open water's hundred or so (Stewart 1977; Gash 1979 measured 0.2 to 0.4 mm/h from wet pine in rain).
constexpr float CANOPY_EVAPORATION_FACTOR = 4.0f;

float rain_saturation_vapour_pressure(float p_celsius) {
	return 6.112f * Math::exp(17.62f * p_celsius / (243.12f + p_celsius)); // hPa, Magnus
}

} // namespace

float WeatherMap3D::canopy_drainage(float p_held_mm, float p_storage_mm) {
	if (p_held_mm <= 0.0f) {
		return 0.0f;
	}
	return CANOPY_DS * Math::exp(MIN(CANOPY_B * (p_held_mm - p_storage_mm), 30.0f));
}

void WeatherMap3D::_rain_allocate() {
	const int n = resolution;
	cell_canopy.resize(n * n);
	cell_canopy.fill(0.0f);
	cell_drip.resize(n * n);
	cell_drip.fill(0.0f);
	cell_fog.resize(n * n);
	cell_fog.fill(0.0f);
	cell_fog_drip.resize(n * n);
	cell_fog_drip.fill(0.0f);
	cell_glaze.resize(n * n);
	cell_glaze.fill(0.0f);
	cell_rime_x.resize(n * n);
	cell_rime_x.fill(0.0f);
	cell_rime_z.resize(n * n);
	cell_rime_z.fill(0.0f);
	rain_image = Image::create_empty(n, n, false, Image::FORMAT_RGBAH);
	rain_texture.unref();
	rain_global_added = false;
}

void WeatherMap3D::_rain_shift(Vector2i p_delta) {
	// History, as the film: cells coming in take the map's mean.
	const int n = resolution;
	if (cell_canopy.size() != n * n) {
		return;
	}
	for (Vector<float> *v : { &cell_canopy, &cell_glaze, &cell_rime_x, &cell_rime_z }) {
		if (v->size() != n * n) {
			continue;
		}
		double sum = 0.0;
		for (int i = 0; i < n * n; i++) {
			sum += (*v)[i];
		}
		const float mean = float(sum / (n * n));
		const Vector<float> old = *v;
		for (int z = 0; z < n; z++) {
			for (int x = 0; x < n; x++) {
				const int sx = x + p_delta.x;
				const int sz = z + p_delta.y;
				const bool inside = sx >= 0 && sz >= 0 && sx < n && sz < n;
				v->write[z * n + x] = inside ? old[sz * n + sx] : mean;
			}
		}
	}
}

// Freezing rain. Snow falling through a warm layer aloft melts: wholly once the layer peaks above about +3 C at the
// usual depths, hardly below +1 C (Stewart and King 1987; Zerr 1997). Under sub-zero air at the ground the melted drops
// arrive supercooled and freeze on what they hit.
float WeatherMap3D::warm_nose_melt(float p_nose_celsius) {
	const float t = p_nose_celsius;
	if (t <= 1.0f) {
		return 0.0f;
	}
	const float x = MIN((t - 1.0f) / 2.0f, 1.0f);
	return x * x * (3.0f - 2.0f * x);
}

float WeatherMap3D::get_warm_nose_melt() const {
	return warm_nose_melt(warm_nose_temperature);
}

// Refreezing under the warm layer: Bourgouin's (2000) energy method as NCEP runs it (CALWXT_BOURG). A layer's area is
// g dz ln(T / 273.15) J/kg. The drops stay liquid where the cold layer's (negative) area is under 46 + 0.66 PA, freeze
// to ice pellets over 66 + 0.66 PA (between, NCEP draws at random; here the expected share). PA, the warm area, from the
// nose's peak with a linear profile over a 1 km deep layer (Stewart and King's noses are a few hundred metres to
// 1.5 km); NA from the air falling linearly from 0 C at the nose's base to the point's temperature.
float WeatherMap3D::liquid_share(float p_celsius, float p_humidity, float p_height, float p_nose_celsius, float p_nose_base, float *r_freezing) {
	const float plain = rain_fraction(p_celsius, p_humidity);
	const float melt = warm_nose_melt(p_nose_celsius);
	float freezing = 0.0f;
	float liquid = plain;
	if (melt > 0.0f) {
		if (p_height >= p_nose_base) {
			liquid = MAX(plain, melt); // in the warm air: rain
		} else {
			constexpr float G = 9.81f;
			constexpr float NOSE_DEPTH = 1000.0f;
			const float pa = G * NOSE_DEPTH * Math::log(1.0f + 0.5f * p_nose_celsius / 273.15f);
			const float na = p_celsius < 0.0f ? G * (p_nose_base - p_height) * -Math::log(1.0f + 0.5f * p_celsius / 273.15f) : 0.0f;
			const float stays = 46.0f + 0.66f * pa;
			const float pellets = 66.0f + 0.66f * pa;
			const float reaching = melt * (1.0f - CLAMP((na - stays) / (pellets - stays), 0.0f, 1.0f));
			liquid = MAX(plain, reaching);
			// Onto surfaces at or below 0 C (a kelvin's ramp round it, so nothing jumps).
			freezing = reaching * CLAMP(0.5f - p_celsius, 0.0f, 1.0f);
		}
	}
	if (r_freezing) {
		*r_freezing = freezing;
	}
	return liquid;
}

float WeatherMap3D::get_freezing_share_at(Vector3 p_position) const {
	float freezing = 0.0f;
	liquid_share(get_air_temperature_at(p_position), get_air_humidity_at(p_position), p_position.y - ground_height, warm_nose_temperature, warm_nose_base, &freezing);
	return freezing;
}

void WeatherMap3D::set_ice_storm_glaze(float p_mm) {
	ice_storm_glaze = CLAMP(p_mm, 0.0f, 60.0f);
	if (ice_storm_glaze > 0.0f) { // felt at once, not at the next update
		float *g = cell_glaze.ptrw();
		for (int i = 0; i < cell_glaze.size(); i++) {
			g[i] = MAX(g[i], ice_storm_glaze);
		}
	}
}

// Glaze: freezing rain accretes (Jones 1998); above freezing it melts at a degree-hour rate of 0.3 mm/h per K, the low end
// of clean ice's (Hock 2003: 6.6 to 20 mm/day/K); below, it stays (sublimation is a few hundredths of a mm a day).
// Its own pass, so it runs with the canopy water off too.
void WeatherMap3D::_glaze_update(float p_dt_h) {
	const int n = resolution;
	if (cell_glaze.size() != n * n || cell_rain.size() != n * n) {
		return;
	}
	float freezing = 0.0f;
	liquid_share(cur_air_temperature, cur_air_humidity, 0.0f, warm_nose_temperature, warm_nose_base, &freezing);
	float *g = cell_glaze.ptrw();
	const bool wind = cell_wind.size() == n * n * 2;
	for (int i = 0; i < n * n; i++) {
		if (freezing > 0.0f) {
			const float v = wind ? Vector2(cell_wind[i * 2], cell_wind[i * 2 + 1]).length() : 0.0f;
			g[i] += glaze_accretion_rate(MAX(cell_rain[i], 0.0f) * freezing, v) * p_dt_h;
		} else if (cur_air_temperature > 0.0f) {
			g[i] = MAX(g[i] - 0.3f * cur_air_temperature * p_dt_h, 0.0f);
		}
		g[i] = CLAMP(g[i], ice_storm_glaze, 60.0f);
	}
}

// Jones (1998), the simple ice accretion model (CRREL Report 98-1; ASCE 7's ice load): the radial ice a horizontal
// cylinder gains, mm/h, from freezing rain P (mm/h) in wind V (m/s), with the rain's liquid water content
// W = 0.067 P^0.846 g/m3 (Best 1949):  dR/dt = sqrt((P rho_w)^2 + (3.6 V W)^2) / (rho_i pi), rho_i 0.9 g/cm3.
float WeatherMap3D::glaze_accretion_rate(float p_precip_mm_h, float p_wind_m_s) {
	if (p_precip_mm_h <= 0.0f) {
		return 0.0f;
	}
	const float w = 0.067f * Math::pow(p_precip_mm_h, 0.846f);
	const float v = 3.6f * MAX(p_wind_m_s, 0.0f) * w;
	return Math::sqrt(p_precip_mm_h * p_precip_mm_h + v * v) / (0.9f * Math::PI);
}

void WeatherMap3D::_rain_update(double p_weather_dt) {
	const int n = resolution;
	if (cell_canopy.size() != n * n || cell_rain.size() != n * n || cell_fog.size() != n * n) {
		_rain_allocate();
	}
	float *c = cell_canopy.ptrw();
	float *d = cell_drip.ptrw();
	float *fg = cell_fog.ptrw();
	float *fd = cell_fog_drip.ptrw();
	_glaze_update(float(MAX(p_weather_dt, 0.0)) / 3600.0f);
	float *rx = rime && cell_rime_x.size() == n * n && cell_rime_z.size() == n * n ? cell_rime_x.ptrw() : nullptr;
	float *rz = rx ? cell_rime_z.ptrw() : nullptr;
	const bool heights = cell_height.size() == n * n;
	if (!canopy_water_enabled) {
		for (int i = 0; i < n * n; i++) {
			c[i] = 0.0f;
			d[i] = 0.0f;
			fg[i] = 0.0f;
			fd[i] = 0.0f;
		}
		return;
	}
	// The cloud water at each cell's canopy: the sky's own clouds where the ground stands in them (a ridge in a deck,
	// hill fog). LWC = 2 rho_w r_e beta / 3 with r_e 10 um (Stephens 1978): 6.7 g/m3 per 1/m of extinction.
	if (fog_drip && origin_valid) {
		const WeatherClouds::State st = _cloud_state();
		const float cs = _cell_size();
		// The valley fog sea too (radiation fog, 0.05 to 0.3 g/m3; Gultepe et al. 2007): its extinction under its
		// top, as its FogVolume draws it.
		const float *vf = valley_fog && cell_valley_fog.size() == n * n ? cell_valley_fog.ptr() : nullptr;
		for (int z = 0; z < n; z++) {
			for (int x = 0; x < n; x++) {
				Vector3 p((origin_cell.x + x + 0.5f) * cs, 0.0f, (origin_cell.y + z + 0.5f) * cs);
				const float g = get_terrain_ground_at(p);
				const float hc = get_canopy_height_at(p);
				const float above = MAX(0.7f * hc, 1.0f);
				p.y = (Math::is_finite(g) ? g : ground_height) + above;
				float beta = WeatherClouds::extinction_at(st, p);
				if (vf && vf[z * n + x] > 0.5f) {
					const float d = vf[z * n + x];
					beta = MAX(beta, valley_fog_density * Math::smoothstep(0.5f, 4.0f, d) * (1.0f - Math::smoothstep(d - 6.0f, d + 1.0f, above)));
				}
				fg[z * n + x] = MIN(6.7f * beta, 1.0f);
			}
		}
	} else {
		for (int i = 0; i < n * n; i++) {
			fg[i] = 0.0f;
		}
	}
	// Rutter's running water balance of the canopy, per cell:
	//   dC/dt = (1 - p) R - D(C) - E C / S (C below S; E at C >= S),
	// R the rain (the snow share is the snowpack's canopy load), p the free throughfall, E the wet canopy's
	// evaporation (Penman's open-water wind function times the canopy's aerodynamic gain). Explicit steps of at
	// most a minute: the drainage is stiff above storage.
	const float s = MAX(canopy_storage, 0.05f);
	const float p = canopy_free_throughfall;
	// The rain the leaves hold: the liquid share less what freezes on them (the glaze).
	float freezing_share = 0.0f;
	const float share = liquid_share(cur_air_temperature, cur_air_humidity, 0.0f, warm_nose_temperature, warm_nose_base, &freezing_share) - freezing_share;
	const float z0 = MAX(roughness_length, 1e-5f);
	const float to_2m = Math::log(2.0f / z0) / Math::log(10.0f / z0);
	const float deficit = MAX(rain_saturation_vapour_pressure(cur_air_temperature) * (1.0f - CLAMP(cur_air_humidity, 0.0f, 1.0f)), 0.0f);
	const float dt_h = float(MAX(p_weather_dt, 0.0)) / 3600.0f;
	const int steps = dt_h > 0.0f ? CLAMP(int(Math::ceil(dt_h * 60.0f)), 1, 240) : 0;
	const float h = steps > 0 ? dt_h / steps : 0.0f;
	const float cs = _cell_size();
	for (int i = 0; i < n * n; i++) {
		const float rain = MAX(cell_rain[i], 0.0f) * share;
		const Vector2 w(cell_wind.size() == n * n * 2 ? cell_wind[i * 2] : 0.0f, cell_wind.size() == n * n * 2 ? cell_wind[i * 2 + 1] : 0.0f);
		// Fog deposition, Lovett (1984): LWC x u x a capture efficiency, here 0.003 for short plants up to 0.018 under
		// a closed forest canopy (deposition velocities of about 1.5 to 9 cm/s in a 5 m/s wind; Gallagher et al. 1988,
		// Katata et al. 2008). 3.6 mm/h per g/m3 m/s. In the cloud the air is saturated: the canopy does not dry.
		float fog = 0.0f;
		if (fg[i] > 0.0f) {
			const Vector3 at((origin_cell.x + float(i % n) + 0.5f) * cs, 0.0f, (origin_cell.y + float(i / n) + 0.5f) * cs);
			fog = 3.6f * fg[i] * w.length() * (0.003f + 0.015f * get_canopy_closure_at(at));
		}
		// Penman's wind function, mm/h (WeatherMap3D's film uses the same, open water).
		const float e = fg[i] > 0.01f ? 0.0f : CANOPY_EVAPORATION_FACTOR * 0.26f * (1.0f + 0.54f * w.length() * to_2m) * deficit / 24.0f;
		float held = c[i];
		float drain = canopy_drainage(held, s);
		for (int k = 0; k < steps; k++) {
			drain = canopy_drainage(held, s);
			const float evap = e * MIN(held / s, 1.0f);
			held = MAX(held + ((1.0f - p) * rain + fog - drain - evap) * h, 0.0f);
			// The drainage cannot hold the canopy far above storage: drained to where it balances the input.
			if (held > s + 3.0f) {
				held = s + 3.0f;
			}
		}
		c[i] = held;
		d[i] = steps > 0 ? canopy_drainage(held, s) : drain;
		// Rime: the cloud's supercooled droplets freeze on the twigs and edges facing the wind, building into it
		// (feathers point upwind). In a thaw or strong sun it loosens and drops off in pieces (ice shedding: a share
		// an hour, half an hour's half-life at +2 C), faster in rain.
		if (rx != nullptr && dt_h > 0.0f) {
			float len = Math::sqrt(rx[i] * rx[i] + rz[i] * rz[i]);
			if (fg[i] > 0.0f || len > 0.0f) {
				const Vector3 at((origin_cell.x + float(i % n) + 0.5f) * cs, 0.0f, (origin_cell.y + float(i / n) + 0.5f) * cs);
				const float gy = heights && Math::is_finite(cell_height[i]) ? cell_height[i] : ground_height;
				const float t = get_air_temperature_at(Vector3(at.x, gy + 2.0f, at.z));
				const float u = w.length();
				const float grow = rime_accretion_rate(fg[i], u, t) * dt_h;
				if (grow > 0.0f && u > 0.1f) {
					rx[i] -= grow * w.x / u; // into the wind: against the way it blows
					rz[i] -= grow * w.y / u;
				}
				len = Math::sqrt(rx[i] * rx[i] + rz[i] * rz[i]);
				const float shed = 0.7f * MIN(MAX(t, 0.0f), 3.0f) + (sun_known ? 0.5f * cur_sun_sine * (1.0f - cur_cloud_cover) : 0.0f) + (t > 0.0f ? 0.2f * MAX(cell_rain[i], 0.0f) : 0.0f); // 1/h
				if (len > 0.0f) {
					float keep = Math::exp(-shed * dt_h);
					keep = len * keep < 0.05f ? 0.0f : MIN(len * keep, 200.0f) / len;
					rx[i] *= keep;
					rz[i] *= keep;
				}
			}
		}
		// The fog's share of what drips (the inputs mix on the leaves).
		const float in = (1.0f - p) * rain + fog;
		fd[i] = in > 1e-6f ? d[i] * fog / in : (fog > 0.0f ? d[i] : 0.0f);
	}
}

// Wetness from the runoff flowing over a cell: a sheet of half a millimetre wets the ground through (Lagarde's
// accumulated water starts there). 0 with no WeatherRunoff3D.
float WeatherMap3D::_runoff_wetness(int p_cell) const {
	const WeatherRunoff3D *ro = WeatherRunoff3D::get_active();
	if (ro == nullptr) {
		return 0.0f;
	}
	const int n = resolution;
	const float cs = _cell_size();
	const Rect2 r = get_map_rect();
	const Vector3 p(r.position.x + (float(p_cell % n) + 0.5f) * cs, 0.0f, r.position.y + (float(p_cell / n) + 0.5f) * cs);
	return CLAMP(ro->get_depth_at(p) / 0.5f, 0.0f, 1.0f);
}

void WeatherMap3D::_rain_upload() {
	const int n = resolution;
	if (rain_image.is_null() || rain_image->get_width() != n || cell_canopy.size() != n * n || cell_rain.size() != n * n) {
		return;
	}
	Vector<uint8_t> bytes;
	bytes.resize(n * n * 8);
	uint16_t *w = reinterpret_cast<uint16_t *>(bytes.ptrw());
	const float s = MAX(canopy_storage, 0.05f);
	for (int i = 0; i < n * n; i++) {
		w[i * 4 + 0] = Math::make_half_float(MAX(cell_rain[i], 0.0f));
		w[i * 4 + 1] = Math::make_half_float(MIN(cell_canopy[i] / s, 1.0f));
		w[i * 4 + 2] = Math::make_half_float(cell_drip[i]);
		w[i * 4 + 3] = Math::make_half_float(cell_glaze.size() == n * n ? cell_glaze[i] : 0.0f); // glaze, mm
	}
	rain_image->set_data(n, n, false, Image::FORMAT_RGBAH, bytes);
	if (rain_texture.is_null()) {
		rain_texture = ImageTexture::create_from_image(rain_image);
	} else {
		rain_texture->update(rain_image);
	}
	if (!publish_shader_globals || active != this) {
		return;
	}
	if (!rain_global_added) {
		_set_global("weather_rain_map", rain_texture->get_rid(), RSE::GLOBAL_VAR_TYPE_SAMPLER2D);
		rain_global_added = true;
	}
	const Rect2 r = get_map_rect();
	_set_global("weather_rain_rect", Vector4(r.position.x, r.position.y, r.size.x, r.size.y), RSE::GLOBAL_VAR_TYPE_VEC4);
	_set_global("weather_precip_phase", Vector4(cur_air_temperature, 0.0065f, cur_air_humidity, ground_height), RSE::GLOBAL_VAR_TYPE_VEC4);
}

float WeatherMap3D::get_rain_share_at(Vector3 p_position) const {
	// Under a warm layer aloft its melted share reaches the ground liquid, supercooled where the air is below freezing,
	// unless a deep cold layer refreezes it to pellets (liquid_share()).
	return liquid_share(get_air_temperature_at(p_position), get_air_humidity_at(p_position), p_position.y - ground_height, warm_nose_temperature, warm_nose_base);
}

// Rime accretion on a twig (Makkonen 2000; ISO 12494): dM/dt = a1 a2 a3 w v A, here per unit of the twig's face as
// radial growth: a1 the collision efficiency (about 0.5 for a centimetre twig in 10 um droplets, Finstad et al. 1988),
// a3 = 1 (dry growth well below freezing), deposited at Macklin's density (soft to hard rime, 300 kg/m3 here). mm/h.
// Near 0 C the droplets do not all freeze (wet growth, glaze): it fades out above -1 C.
float WeatherMap3D::rime_accretion_rate(float p_cloud_water, float p_wind_m_s, float p_celsius) {
	if (p_cloud_water <= 0.0f || p_wind_m_s <= 0.0f || p_celsius >= 0.0f) {
		return 0.0f;
	}
	const float flux = 0.3f * p_cloud_water * 1e-3f * p_wind_m_s * 3600.0f; // kg/m2/h
	return flux / 300.0f * 1000.0f * Math::smoothstep(0.0f, 1.0f, -p_celsius);
}

void WeatherMap3D::set_rime(bool p_enabled) {
	rime = p_enabled;
	if (!rime) {
		cell_rime_x.fill(0.0f);
		cell_rime_z.fill(0.0f);
	}
}

Vector3 WeatherMap3D::get_rime_at(Vector3 p_position) const {
	const int n = resolution;
	if (!rime || cell_rime_x.size() != n * n || cell_rime_z.size() != n * n) {
		return Vector3();
	}
	const Vector2 p(p_position.x, p_position.z);
	return Vector3(_sample(cell_rime_x, p), 0.0f, _sample(cell_rime_z, p));
}

float WeatherMap3D::get_glaze_at(Vector3 p_position) const {
	return cell_glaze.size() == resolution * resolution ? MAX(_sample(cell_glaze, Vector2(p_position.x, p_position.z)), 0.0f) : 0.0f;
}

float WeatherMap3D::get_rainfall_at(Vector3 p_position) const {
	return get_rain_at(p_position) * get_rain_share_at(p_position);
}

float WeatherMap3D::get_snowfall_at(Vector3 p_position) const {
	return get_rain_at(p_position) * (1.0f - get_rain_share_at(p_position));
}

float WeatherMap3D::get_canopy_water_at(Vector3 p_position) const {
	return MAX(_sample(cell_canopy, Vector2(p_position.x, p_position.z)), 0.0f);
}

float WeatherMap3D::get_canopy_wetness_at(Vector3 p_position) const {
	return CLAMP(get_canopy_water_at(p_position) / MAX(canopy_storage, 0.05f), 0.0f, 1.0f);
}

float WeatherMap3D::get_canopy_drip_at(Vector3 p_position) const {
	return MAX(_sample(cell_drip, Vector2(p_position.x, p_position.z)), 0.0f);
}

float WeatherMap3D::get_fog_water_at(Vector3 p_position) const {
	return cell_fog.size() == resolution * resolution ? MAX(_sample(cell_fog, Vector2(p_position.x, p_position.z)), 0.0f) : 0.0f;
}

float WeatherMap3D::get_fog_drip_at(Vector3 p_position) const {
	return cell_fog_drip.size() == resolution * resolution ? MAX(_sample(cell_fog_drip, Vector2(p_position.x, p_position.z)), 0.0f) : 0.0f;
}

float WeatherMap3D::get_ground_water_input_at(Vector3 p_position) const {
	const float fog = get_fog_drip_at(p_position) + get_sea_spray_at(p_position); // fog drip and sea spray
	const int n = resolution;
	if (origin_valid && cell_inflow.size() == n * n) {
		const float cs = _cell_size();
		const int x = int(Math::floor(p_position.x / cs)) - origin_cell.x;
		const int z = int(Math::floor(p_position.z / cs)) - origin_cell.y;
		if (x >= 0 && z >= 0 && x < n && z < n && cell_inflow[z * n + x] >= 0.0f) {
			return cell_inflow[z * n + x] + fog;
		}
	}
	return get_rainfall_at(p_position) + fog;
}

void WeatherMap3D::_bind_rain_methods() {
	ClassDB::bind_method(D_METHOD("get_rain_share_at", "position"), &WeatherMap3D::get_rain_share_at);
	ClassDB::bind_method(D_METHOD("get_rainfall_at", "position"), &WeatherMap3D::get_rainfall_at);
	ClassDB::bind_method(D_METHOD("get_snowfall_at", "position"), &WeatherMap3D::get_snowfall_at);
	ClassDB::bind_method(D_METHOD("set_canopy_water_enabled", "enabled"), &WeatherMap3D::set_canopy_water_enabled);
	ClassDB::bind_method(D_METHOD("is_canopy_water_enabled"), &WeatherMap3D::is_canopy_water_enabled);
	ClassDB::bind_method(D_METHOD("set_canopy_storage", "mm"), &WeatherMap3D::set_canopy_storage);
	ClassDB::bind_method(D_METHOD("get_canopy_storage"), &WeatherMap3D::get_canopy_storage);
	ClassDB::bind_method(D_METHOD("set_canopy_free_throughfall", "share"), &WeatherMap3D::set_canopy_free_throughfall);
	ClassDB::bind_method(D_METHOD("get_canopy_free_throughfall"), &WeatherMap3D::get_canopy_free_throughfall);
	ClassDB::bind_method(D_METHOD("get_canopy_water_at", "position"), &WeatherMap3D::get_canopy_water_at);
	ClassDB::bind_method(D_METHOD("get_canopy_wetness_at", "position"), &WeatherMap3D::get_canopy_wetness_at);
	ClassDB::bind_method(D_METHOD("get_canopy_drip_at", "position"), &WeatherMap3D::get_canopy_drip_at);
	ClassDB::bind_static_method("WeatherMap3D", D_METHOD("canopy_drainage", "held_mm", "storage_mm"), &WeatherMap3D::canopy_drainage);
	ClassDB::bind_method(D_METHOD("get_rain_texture"), &WeatherMap3D::get_rain_texture);
	ClassDB::bind_method(D_METHOD("get_ground_water_input_at", "position"), &WeatherMap3D::get_ground_water_input_at);
	ClassDB::bind_method(D_METHOD("get_fog_water_at", "position"), &WeatherMap3D::get_fog_water_at);
	ClassDB::bind_method(D_METHOD("get_fog_drip_at", "position"), &WeatherMap3D::get_fog_drip_at);
	ClassDB::bind_method(D_METHOD("set_warm_nose_temperature", "celsius"), &WeatherMap3D::set_warm_nose_temperature);
	ClassDB::bind_method(D_METHOD("get_warm_nose_temperature"), &WeatherMap3D::get_warm_nose_temperature);
	ClassDB::bind_method(D_METHOD("get_warm_nose_melt"), &WeatherMap3D::get_warm_nose_melt);
	ClassDB::bind_method(D_METHOD("get_glaze_at", "position"), &WeatherMap3D::get_glaze_at);
	ClassDB::bind_method(D_METHOD("get_rime_at", "position"), &WeatherMap3D::get_rime_at);
	ClassDB::bind_static_method("WeatherMap3D", D_METHOD("rime_accretion_rate", "cloud_water", "wind_m_s", "celsius"), &WeatherMap3D::rime_accretion_rate);
	ClassDB::bind_method(D_METHOD("set_rime", "enabled"), &WeatherMap3D::set_rime);
	ClassDB::bind_method(D_METHOD("is_rime_enabled"), &WeatherMap3D::is_rime_enabled);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "rime"), "set_rime", "is_rime_enabled");
	ClassDB::bind_static_method("WeatherMap3D", D_METHOD("glaze_accretion_rate", "precip_mm_h", "wind_m_s"), &WeatherMap3D::glaze_accretion_rate);
	ClassDB::bind_method(D_METHOD("set_warm_nose_base", "height"), &WeatherMap3D::set_warm_nose_base);
	ClassDB::bind_method(D_METHOD("get_warm_nose_base"), &WeatherMap3D::get_warm_nose_base);
	ClassDB::bind_method(D_METHOD("set_ice_storm_glaze", "mm"), &WeatherMap3D::set_ice_storm_glaze);
	ClassDB::bind_method(D_METHOD("get_ice_storm_glaze"), &WeatherMap3D::get_ice_storm_glaze);
	ClassDB::bind_method(D_METHOD("get_freezing_share_at", "position"), &WeatherMap3D::get_freezing_share_at);
	ClassDB::bind_method(D_METHOD("set_fog_drip", "enabled"), &WeatherMap3D::set_fog_drip);
	ClassDB::bind_method(D_METHOD("is_fog_drip_enabled"), &WeatherMap3D::is_fog_drip_enabled);

	ADD_GROUP("Rain", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "canopy_water_enabled"), "set_canopy_water_enabled", "is_canopy_water_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "canopy_storage", PROPERTY_HINT_RANGE, "0.05,10,0.01,suffix:mm"), "set_canopy_storage", "get_canopy_storage");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "fog_drip"), "set_fog_drip", "is_fog_drip_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "canopy_free_throughfall", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_canopy_free_throughfall", "get_canopy_free_throughfall");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "warm_nose_temperature", PROPERTY_HINT_RANGE, "0,15,0.1,suffix:C"), "set_warm_nose_temperature", "get_warm_nose_temperature");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "warm_nose_base", PROPERTY_HINT_RANGE, "0,5000,1,or_greater,suffix:m"), "set_warm_nose_base", "get_warm_nose_base");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ice_storm_glaze", PROPERTY_HINT_RANGE, "0,60,0.1,suffix:mm"), "set_ice_storm_glaze", "get_ice_storm_glaze");
}

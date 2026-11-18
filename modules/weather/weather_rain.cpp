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
	double sum = 0.0;
	for (int i = 0; i < n * n; i++) {
		sum += cell_canopy[i];
	}
	const float mean = float(sum / (n * n));
	const Vector<float> old = cell_canopy;
	for (int z = 0; z < n; z++) {
		for (int x = 0; x < n; x++) {
			const int sx = x + p_delta.x;
			const int sz = z + p_delta.y;
			const bool inside = sx >= 0 && sz >= 0 && sx < n && sz < n;
			cell_canopy.write[z * n + x] = inside ? old[sz * n + sx] : mean;
		}
	}
}

void WeatherMap3D::_rain_update(double p_weather_dt) {
	const int n = resolution;
	if (cell_canopy.size() != n * n || cell_rain.size() != n * n) {
		_rain_allocate();
	}
	float *c = cell_canopy.ptrw();
	float *d = cell_drip.ptrw();
	if (!canopy_water_enabled) {
		for (int i = 0; i < n * n; i++) {
			c[i] = 0.0f;
			d[i] = 0.0f;
		}
		return;
	}
	// Rutter's running water balance of the canopy, per cell:
	//   dC/dt = (1 - p) R - D(C) - E C / S (C below S; E at C >= S),
	// R the rain (the snow share is the snowpack's canopy load), p the free throughfall, E the wet canopy's
	// evaporation (Penman's open-water wind function times the canopy's aerodynamic gain). Explicit steps of at
	// most a minute: the drainage is stiff above storage.
	const float s = MAX(canopy_storage, 0.05f);
	const float p = canopy_free_throughfall;
	const float share = rain_fraction(cur_air_temperature, cur_air_humidity);
	const float z0 = MAX(roughness_length, 1e-5f);
	const float to_2m = Math::log(2.0f / z0) / Math::log(10.0f / z0);
	const float deficit = MAX(rain_saturation_vapour_pressure(cur_air_temperature) * (1.0f - CLAMP(cur_air_humidity, 0.0f, 1.0f)), 0.0f);
	const float dt_h = float(MAX(p_weather_dt, 0.0)) / 3600.0f;
	const int steps = dt_h > 0.0f ? CLAMP(int(Math::ceil(dt_h * 60.0f)), 1, 240) : 0;
	const float h = steps > 0 ? dt_h / steps : 0.0f;
	for (int i = 0; i < n * n; i++) {
		const float rain = MAX(cell_rain[i], 0.0f) * share;
		const Vector2 w(cell_wind.size() == n * n * 2 ? cell_wind[i * 2] : 0.0f, cell_wind.size() == n * n * 2 ? cell_wind[i * 2 + 1] : 0.0f);
		// Penman's wind function, mm/h (WeatherMap3D's film uses the same, open water).
		const float e = CANOPY_EVAPORATION_FACTOR * 0.26f * (1.0f + 0.54f * w.length() * to_2m) * deficit / 24.0f;
		float held = c[i];
		float drain = canopy_drainage(held, s);
		for (int k = 0; k < steps; k++) {
			drain = canopy_drainage(held, s);
			const float evap = e * MIN(held / s, 1.0f);
			held = MAX(held + ((1.0f - p) * rain - drain - evap) * h, 0.0f);
			// The drainage cannot hold the canopy far above storage: drained to where it balances the input.
			if (held > s + 3.0f) {
				held = s + 3.0f;
			}
		}
		c[i] = held;
		d[i] = steps > 0 ? canopy_drainage(held, s) : drain;
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
		w[i * 4 + 3] = Math::make_half_float(0.0f);
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
	return rain_fraction(get_air_temperature_at(p_position), get_air_humidity_at(p_position));
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

float WeatherMap3D::get_ground_water_input_at(Vector3 p_position) const {
	const int n = resolution;
	if (origin_valid && cell_inflow.size() == n * n) {
		const float cs = _cell_size();
		const int x = int(Math::floor(p_position.x / cs)) - origin_cell.x;
		const int z = int(Math::floor(p_position.z / cs)) - origin_cell.y;
		if (x >= 0 && z >= 0 && x < n && z < n && cell_inflow[z * n + x] >= 0.0f) {
			return cell_inflow[z * n + x];
		}
	}
	return get_rainfall_at(p_position);
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

	ADD_GROUP("Rain", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "canopy_water_enabled"), "set_canopy_water_enabled", "is_canopy_water_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "canopy_storage", PROPERTY_HINT_RANGE, "0.05,10,0.01,suffix:mm"), "set_canopy_storage", "get_canopy_storage");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "canopy_free_throughfall", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_canopy_free_throughfall", "get_canopy_free_throughfall");
}

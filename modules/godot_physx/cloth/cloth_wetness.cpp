/**************************************************************************/
/*  cloth_wetness.cpp                                                     */
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

#include "cloth_wetness.h"

#include "scene/3d/node_3d.h"
#include "scene/resources/3d/world_3d.h"
#include "servers/physics_3d/physics_server_3d.h"

#include "modules/modules_enabled.gen.h" // For weather.
#ifdef MODULE_WEATHER_ENABLED
#include "modules/weather/weather_map_3d.h"
#endif

void ClothWetness::rain_directions(const Vector3 &p_wind, Vector3 r_dirs[4], float &r_mean_dy) {
	const float sigma = 0.3f * Vector2(p_wind.x, p_wind.z).length() + 0.5f;
	r_dirs[0] = Vector3(p_wind.x + sigma, -DROP_SPEED, p_wind.z);
	r_dirs[1] = Vector3(p_wind.x - sigma, -DROP_SPEED, p_wind.z);
	r_dirs[2] = Vector3(p_wind.x, -DROP_SPEED, p_wind.z + sigma);
	r_dirs[3] = Vector3(p_wind.x, -DROP_SPEED, p_wind.z - sigma);
	r_mean_dy = 0.0f;
	for (int i = 0; i < 4; i++) {
		r_dirs[i].normalize();
		r_mean_dy += 0.25f * Math::abs(r_dirs[i].y);
	}
}

void ClothWetness::step(Node3D *p_node, const AABB &p_world_box, const Vector3 &p_wind, float p_dt, float p_capacity, bool p_weather, bool &r_update_catch) {
	r_update_catch = false;
	if (p_dt <= 0.0f) {
		return;
	}
	const float capacity = MAX(p_capacity, 1.0e-4f);
	float water = wetness * capacity;
	float air_t = 15.0f;
	float humidity = 0.7f;
	float rain = 0.0f;
#ifdef MODULE_WEATHER_ENABLED
	if (const WeatherMap3D *wm = WeatherMap3D::get_active()) {
		air_t = wm->get_air_temperature();
		humidity = wm->get_air_humidity();
		if (p_weather && p_node && p_node->is_inside_tree()) {
			const Vector3 at = p_world_box.get_center();
			rain = MAX(wm->get_rain_at(at), 0.0f);
			rain *= 1.0f - CLAMP(wm->get_canopy_closure_at(at), 0.0f, 1.0f);
			rain *= WeatherMap3D::rain_fraction(air_t, humidity); // snow does not soak in at once
			rain_timer -= p_dt;
			if (rain > 0.0f && rain_timer <= 0.0f) {
				rain_timer = 0.5f;
				r_update_catch = true;
				// A roof above it.
				rain_shelter = 1.0f;
				const Ref<World3D> w = p_node->get_world_3d();
				if (PhysicsDirectSpaceState3D *space = w.is_valid() ? w->get_direct_space_state() : nullptr) {
					PhysicsDirectSpaceState3D::RayParameters ray;
					ray.from = Vector3(at.x, p_world_box.position.y + p_world_box.size.y + 0.1f, at.z);
					ray.to = ray.from + Vector3(0.0f, 100.0f, 0.0f);
					PhysicsDirectSpaceState3D::RayResult hit;
					if (space->intersect_ray(ray, hit)) {
						rain_shelter = 0.0f;
					}
				}
			}
			rain *= rain_shelter * rain_catch;
		}
	}
#endif
	water += rain / 3600.0f * p_dt; // mm/h of rain = kg/m2/h
	const float es = 6.108f * Math::exp(17.27f * air_t / (air_t + 237.3f)); // hPa, saturation (Tetens)
	const float deficit = MAX(es * (1.0f - CLAMP(humidity, 0.0f, 1.0f)), 0.0f);
	const float u = Math::sqrt(p_wind.x * p_wind.x + p_wind.z * p_wind.z);
	float evap = 2.0f * 0.26f * (1.0f + 0.54f * u) * deficit / 86400.0f; // kg/m2/s, both faces
	evap *= CLAMP(wetness / 0.2f, 0.1f, 1.0f);
	water = CLAMP(water - evap * p_dt, 0.0f, capacity);
	wetness = water / capacity;
}

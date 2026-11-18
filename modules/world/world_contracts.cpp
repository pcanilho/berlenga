/**************************************************************************/
/*  world_contracts.cpp                                                   */
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

#include "world_contracts.h"

#include "core/object/class_db.h"

#include <type_traits>
#include "scene/3d/node_3d.h"
#include "scene/3d/physics/collision_shape_3d.h"
#include "scene/3d/physics/character_body_3d.h"
#include "scene/3d/physics/rigid_body_3d.h"
#include "scene/resources/3d/box_shape_3d.h"
#include "scene/resources/3d/capsule_shape_3d.h"
#include "scene/resources/3d/cylinder_shape_3d.h"
#include "scene/resources/3d/sphere_shape_3d.h"

// Weather.

#define WW_CALL(m_virtual, m_provider, m_default, ...) \
	{ \
		std::remove_cv_t<decltype(m_default)> r = m_default; \
		if (GDVIRTUAL_CALL(m_virtual, __VA_ARGS__, r)) { \
			return r; \
		} \
		return provider ? provider->m_provider : m_default; \
	}

float WorldWeather::air_temperature_at(const Vector3 &p_position) const WW_CALL(_air_temperature_at, contract_air_temperature_at(p_position), DEFAULT_AIR_TEMPERATURE, p_position)
float WorldWeather::air_humidity_at(const Vector3 &p_position) const WW_CALL(_air_humidity_at, contract_air_humidity_at(p_position), DEFAULT_AIR_HUMIDITY, p_position)
Vector3 WorldWeather::wind_at(const Vector3 &p_position, float p_height_above_ground) const WW_CALL(_wind_at, contract_wind_at(p_position, p_height_above_ground), Vector3(), p_position, p_height_above_ground)
float WorldWeather::precipitation_at(const Vector3 &p_position) const WW_CALL(_precipitation_at, contract_precipitation_at(p_position), 0.0f, p_position)
float WorldWeather::rain_share_at(const Vector3 &p_position) const WW_CALL(_rain_share_at, contract_rain_share_at(p_position), 1.0f, p_position)
float WorldWeather::surface_wetness_at(const Vector3 &p_position) const WW_CALL(_surface_wetness_at, contract_surface_wetness_at(p_position), 0.0f, p_position)
float WorldWeather::puddle_at(const Vector3 &p_position) const WW_CALL(_puddle_at, contract_puddle_at(p_position), 0.0f, p_position)
float WorldWeather::contact_wetness_at(const Vector3 &p_position) const WW_CALL(_contact_wetness_at, contract_contact_wetness_at(p_position), 0.0f, p_position)
float WorldWeather::sun_visibility_at(const Vector3 &p_position) const WW_CALL(_sun_visibility_at, contract_sun_visibility_at(p_position), 1.0f, p_position)
float WorldWeather::canopy_closure_at(const Vector3 &p_position) const WW_CALL(_canopy_closure_at, contract_canopy_closure_at(p_position), 0.0f, p_position)
float WorldWeather::frost_at(const Vector3 &p_position) const WW_CALL(_frost_at, contract_frost_at(p_position), 0.0f, p_position)
float WorldWeather::rainfall_at(const Vector3 &p_position) const WW_CALL(_rainfall_at, contract_rainfall_at(p_position), 0.0f, p_position)
float WorldWeather::snowfall_at(const Vector3 &p_position) const WW_CALL(_snowfall_at, contract_snowfall_at(p_position), 0.0f, p_position)
float WorldWeather::canopy_water_at(const Vector3 &p_position) const WW_CALL(_canopy_water_at, contract_canopy_water_at(p_position), 0.0f, p_position)
float WorldWeather::canopy_wetness_at(const Vector3 &p_position) const WW_CALL(_canopy_wetness_at, contract_canopy_wetness_at(p_position), 0.0f, p_position)
float WorldWeather::canopy_drip_at(const Vector3 &p_position) const WW_CALL(_canopy_drip_at, contract_canopy_drip_at(p_position), 0.0f, p_position)
float WorldWeather::ground_water_input_at(const Vector3 &p_position) const WW_CALL(_ground_water_input_at, contract_ground_water_input_at(p_position), 0.0f, p_position)

#undef WW_CALL

void WorldWeather::add_contact_wetness(const Vector3 &p_position, float p_radius, float p_amount) {
	if (GDVIRTUAL_CALL(_add_contact_wetness, p_position, p_radius, p_amount)) {
		return;
	}
	if (provider) {
		provider->contract_add_contact_wetness(p_position, p_radius, p_amount);
	}
}

float WorldWeather::dew_point_at(const Vector3 &p_position) const {
	// Magnus inverted (Alduchov and Eskridge 1996).
	const float t = air_temperature_at(p_position);
	const float g = Math::log(MAX(air_humidity_at(p_position), 0.01f)) + 17.62f * t / (243.12f + t);
	return 243.12f * g / (17.62f - g);
}

void WorldWeather::_bind_methods() {
	ClassDB::bind_method(D_METHOD("has_provider"), &WorldWeather::has_provider);
	ClassDB::bind_method(D_METHOD("air_temperature_at", "position"), &WorldWeather::air_temperature_at);
	ClassDB::bind_method(D_METHOD("air_humidity_at", "position"), &WorldWeather::air_humidity_at);
	ClassDB::bind_method(D_METHOD("dew_point_at", "position"), &WorldWeather::dew_point_at);
	ClassDB::bind_method(D_METHOD("wind_at", "position", "height_above_ground"), &WorldWeather::wind_at, DEFVAL(-1.0f));
	ClassDB::bind_method(D_METHOD("precipitation_at", "position"), &WorldWeather::precipitation_at);
	ClassDB::bind_method(D_METHOD("rain_share_at", "position"), &WorldWeather::rain_share_at);
	ClassDB::bind_method(D_METHOD("surface_wetness_at", "position"), &WorldWeather::surface_wetness_at);
	ClassDB::bind_method(D_METHOD("puddle_at", "position"), &WorldWeather::puddle_at);
	ClassDB::bind_method(D_METHOD("contact_wetness_at", "position"), &WorldWeather::contact_wetness_at);
	ClassDB::bind_method(D_METHOD("add_contact_wetness", "position", "radius", "amount"), &WorldWeather::add_contact_wetness, DEFVAL(1.0f));
	ClassDB::bind_method(D_METHOD("sun_visibility_at", "position"), &WorldWeather::sun_visibility_at);
	ClassDB::bind_method(D_METHOD("canopy_closure_at", "position"), &WorldWeather::canopy_closure_at);
	ClassDB::bind_method(D_METHOD("frost_at", "position"), &WorldWeather::frost_at);
	ClassDB::bind_method(D_METHOD("rainfall_at", "position"), &WorldWeather::rainfall_at);
	ClassDB::bind_method(D_METHOD("snowfall_at", "position"), &WorldWeather::snowfall_at);
	ClassDB::bind_method(D_METHOD("canopy_water_at", "position"), &WorldWeather::canopy_water_at);
	ClassDB::bind_method(D_METHOD("canopy_wetness_at", "position"), &WorldWeather::canopy_wetness_at);
	ClassDB::bind_method(D_METHOD("canopy_drip_at", "position"), &WorldWeather::canopy_drip_at);
	ClassDB::bind_method(D_METHOD("ground_water_input_at", "position"), &WorldWeather::ground_water_input_at);

	GDVIRTUAL_BIND(_air_temperature_at, "position");
	GDVIRTUAL_BIND(_air_humidity_at, "position");
	GDVIRTUAL_BIND(_wind_at, "position", "height_above_ground");
	GDVIRTUAL_BIND(_precipitation_at, "position");
	GDVIRTUAL_BIND(_rain_share_at, "position");
	GDVIRTUAL_BIND(_surface_wetness_at, "position");
	GDVIRTUAL_BIND(_puddle_at, "position");
	GDVIRTUAL_BIND(_contact_wetness_at, "position");
	GDVIRTUAL_BIND(_add_contact_wetness, "position", "radius", "amount");
	GDVIRTUAL_BIND(_sun_visibility_at, "position");
	GDVIRTUAL_BIND(_canopy_closure_at, "position");
	GDVIRTUAL_BIND(_frost_at, "position");
	GDVIRTUAL_BIND(_rainfall_at, "position");
	GDVIRTUAL_BIND(_snowfall_at, "position");
	GDVIRTUAL_BIND(_canopy_water_at, "position");
	GDVIRTUAL_BIND(_canopy_wetness_at, "position");
	GDVIRTUAL_BIND(_canopy_drip_at, "position");
	GDVIRTUAL_BIND(_ground_water_input_at, "position");
}

// Registry.

WorldContracts *WorldContracts::singleton = nullptr;

WorldWeather *WorldContracts::weather() {
	static Ref<WorldWeather> fallback; // before the singleton exists or after it goes
	if (singleton == nullptr) {
		if (fallback.is_null()) {
			fallback.instantiate();
		}
		return fallback.ptr();
	}
	if (singleton->weather_override.is_valid()) {
		return singleton->weather_override.ptr();
	}
	return singleton->builtin_weather.ptr();
}

void WorldContracts::register_weather(WorldWeatherProvider *p_provider) {
	if (singleton) {
		singleton->weather_provider = p_provider;
		singleton->builtin_weather->provider = p_provider;
		if (singleton->weather_override.is_valid()) {
			singleton->weather_override->provider = p_provider; // a partial stub falls back to the real one
		}
	}
}

void WorldContracts::unregister_weather(WorldWeatherProvider *p_provider) {
	if (singleton && singleton->weather_provider == p_provider) {
		register_weather(nullptr);
	}
}

void WorldContracts::set_weather(const Ref<WorldWeather> &p_stub) {
	weather_override = p_stub;
	if (weather_override.is_valid()) {
		weather_override->provider = weather_provider;
	}
}

Ref<WorldWeather> WorldContracts::get_weather() const {
	return weather_override.is_valid() ? weather_override : builtin_weather;
}

void WorldContracts::clear_overrides() {
	weather_override.unref();
}

// Contact bodies.

WorldContactBody WorldContracts::describe_body(const Node3D *p_node) {
	WorldContactBody b;
	if (p_node == nullptr) {
		return b;
	}
	b.center = p_node->get_global_position();
	bool sized = false;
	for (int c = 0; c < p_node->get_child_count(); c++) {
		const CollisionShape3D *cs = Object::cast_to<CollisionShape3D>(p_node->get_child(c));
		if (!cs || cs->is_disabled() || cs->get_shape().is_null()) {
			continue;
		}
		const Ref<Shape3D> shape = cs->get_shape();
		const Basis basis = cs->get_global_basis();
		const float sc = basis.get_scale().x;
		b.center = cs->get_global_position();
		if (const SphereShape3D *sp = Object::cast_to<SphereShape3D>(shape.ptr())) {
			b.radius = b.half_height = sp->get_radius() * sc;
		} else if (const CapsuleShape3D *cp = Object::cast_to<CapsuleShape3D>(shape.ptr())) {
			b.radius = cp->get_radius() * sc;
			b.half_height = cp->get_height() * 0.5f * sc;
		} else if (const CylinderShape3D *cy = Object::cast_to<CylinderShape3D>(shape.ptr())) {
			if (Math::abs(basis.get_column(1).normalized().y) > 0.7f) {
				b.radius = cy->get_radius() * sc;
				b.half_height = cy->get_height() * 0.5f * sc;
				b.round = false;
			} else {
				b.wheel = true;
				b.round = false;
				b.half_height = cy->get_radius() * sc;
				b.width = cy->get_height() * sc;
				b.radius = b.width * 0.5f;
			}
		} else if (const BoxShape3D *bx = Object::cast_to<BoxShape3D>(shape.ptr())) {
			const Vector3 s = bx->get_size() * 0.5f * sc;
			b.radius = MAX(s.x, s.z);
			b.half_height = s.y;
			b.round = false;
			b.box = true;
			b.width = 2.0f * MIN(s.x, s.z);
		}
		sized = true;
		break;
	}
	if (p_node->has_meta(SNAME("deform_wheel_radius"))) {
		b.center = p_node->get_global_position();
		b.wheel = true;
		b.round = false;
		b.box = false;
		b.half_height = p_node->get_meta(SNAME("deform_wheel_radius"));
		b.width = p_node->has_meta(SNAME("deform_wheel_width")) ? float(p_node->get_meta(SNAME("deform_wheel_width"))) : b.half_height * 0.6f;
		b.radius = b.width * 0.5f;
		sized = true;
	}
	if (p_node->has_meta(SNAME("trample_radius")) && !sized) {
		b.radius = b.half_height = p_node->get_meta(SNAME("trample_radius"));
	}
	if (const RigidBody3D *rb = Object::cast_to<RigidBody3D>(p_node)) {
		b.mass = rb->get_mass();
	}
	if (p_node->has_meta(SNAME("ground_mass"))) {
		b.mass = p_node->get_meta(SNAME("ground_mass"));
	}
	return b;
}

Dictionary WorldContracts::describe_body_bind(Node *p_node) const {
	const WorldContactBody b = describe_body(Object::cast_to<Node3D>(p_node));
	Dictionary d;
	d["center"] = b.center;
	d["radius"] = b.radius;
	d["half_height"] = b.half_height;
	d["round"] = b.round;
	d["wheel"] = b.wheel;
	d["box"] = b.box;
	d["width"] = b.width;
	d["mass"] = b.mass;
	return d;
}

void WorldContracts::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_weather", "stub"), &WorldContracts::set_weather);
	ClassDB::bind_method(D_METHOD("get_weather"), &WorldContracts::get_weather);
	ClassDB::bind_method(D_METHOD("clear_overrides"), &WorldContracts::clear_overrides);
	ClassDB::bind_method(D_METHOD("describe_body", "node"), &WorldContracts::describe_body_bind);
}

WorldContracts::WorldContracts() {
	singleton = this;
	builtin_weather.instantiate();
}

WorldContracts::~WorldContracts() {
	if (singleton == this) {
		singleton = nullptr;
	}
}

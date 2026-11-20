/**************************************************************************/
/*  snow_powder_cloud_3d.cpp                                              */
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


#include "snow_powder_cloud_3d.h"

#include "core/object/class_db.h"

void SnowPowderCloud3D::_build() {
	if (tried) {
		return;
	}
	tried = true;
	if (!ClassDB::class_exists("PhysXFlow3D") || !ClassDB::class_exists("PhysXFlowEmitter3D")) {
		return;
	}
	flow = Object::cast_to<Node3D>(ClassDB::instantiate("PhysXFlow3D"));
	if (!flow) {
		return;
	}
	// Coarse cells: the cloud is tens of metres across. Heavy (it hugs and runs down the slope), no fire, slow to
	// fade (it hangs for half a minute), bright and scattering like the snow it is.
	flow->set_name("PowderFlow");
	flow->set("cell_size", 0.75f);
	flow->set("max_blocks", 2048);
	flow->set("combustion", false);
	flow->set("buoyancy", 0.0f);
	flow->set("smoke_buoyancy", -heaviness);
	flow->set("smoke_fade", 0.04f);
	// Ice grains barely absorb (single-scattering albedo near 1), so a powder cloud is lit through by its many
	// scatterings and reads bright and soft: white, and smooth (a strongly curled gas shades its own clumps dark
	// against the snow and sky behind).
	flow->set("vorticity", 4.0f);
	flow->set("emission_strength", 0.0f);
	flow->set("smoke_density", 1.2f);
	flow->set("scattering_strength", 1.0f); // raster only; PT never sees it (parity)
	flow->set("scattering_albedo", 0.99f); // ice grains barely absorb (Warren 1982: above 0.999 in the visible)
	flow->set("ground_albedo", 0.8f); // the snow it rises from lights its underside (fresh snow, broadband; Warren 1982)
	flow->set("smoke_color", color);
	flow->set("collide_with_bodies", true);
	flow->set("collision_range", 30.0f);
	flow->set("weather_wind", true);
	flow->set("terrain_floor", true);
	if (!terrain_path.is_empty()) {
		flow->set("terrain_path", terrain_path);
	}
	add_child(flow, false, INTERNAL_MODE_FRONT);
	flow->set_as_top_level(true);
	flow->set_global_transform(Transform3D());
	for (int i = 0; i < max_sources; i++) {
		Node3D *e = Object::cast_to<Node3D>(ClassDB::instantiate("PhysXFlowEmitter3D"));
		if (!e) {
			break;
		}
		e->set_name(vformat("Front%d", i));
		e->set("enabled", false);
		flow->add_child(e);
		e->set("flow", e->get_path_to(flow));
		emitters.push_back(e);
	}
}

void SnowPowderCloud3D::set_terrain_path(const NodePath &p_path) {
	terrain_path = p_path;
	if (flow && !p_path.is_empty()) {
		flow->set("terrain_path", p_path);
	}
}

void SnowPowderCloud3D::set_heaviness(float p_value) {
	heaviness = MAX(p_value, 0.0f);
	if (flow) {
		flow->set("smoke_buoyancy", -heaviness);
	}
}

void SnowPowderCloud3D::set_color(const Color &p_color) {
	color = p_color;
	if (flow) {
		flow->set("smoke_color", p_color);
	}
}

void SnowPowderCloud3D::feed(const LocalVector<Source> &p_sources) {
	if (!enabled) {
		active = 0;
		for (Node3D *e : emitters) {
			e->set("enabled", false);
		}
		return;
	}
	if (!p_sources.is_empty()) {
		_build();
	}
	active = 0;
	// The front is one wall, not a row of puffs: every emitter overlaps its neighbours (8 m bins, 5 m spheres) and
	// the weaker parts of the front still feed it at least a third of the strongest (the cloud mixes along the front).
	float strongest = 0.0f;
	for (const Source &s : p_sources) {
		strongest = MAX(strongest, s.rate);
	}
	for (uint32_t i = 0; i < emitters.size(); i++) {
		Node3D *e = emitters[i];
		if (i >= p_sources.size()) {
			e->set("enabled", false);
			continue;
		}
		const Source &s = p_sources[i];
		// The front's own speed carries the cloud on ahead; the eruption current blows it up out of the front.
		Vector3 v = s.velocity;
		v.y = MAX(v.y, 0.0f) + updraft * Vector2(s.velocity.x, s.velocity.z).length();
		e->set("enabled", true);
		e->set("shape", 0); // sphere
		e->set("radius", CLAMP(s.radius, 0.5f, 6.0f));
		e->set_global_position(s.position);
		e->set("velocity", v);
		e->set("divergence", 3.0f);
		e->set("smoke", MIN(MAX(s.rate, strongest / 3.0f) * smoke_per_kg, 20.0f));
		e->set("temperature", 0.0f);
		e->set("couple_rate", 4.0f);
		active++;
	}
}

void SnowPowderCloud3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("has_flow"), &SnowPowderCloud3D::has_flow);
	ClassDB::bind_method(D_METHOD("get_flow"), &SnowPowderCloud3D::get_flow);
	ClassDB::bind_method(D_METHOD("get_active_sources"), &SnowPowderCloud3D::get_active_sources);
	ClassDB::bind_method(D_METHOD("set_terrain_path", "path"), &SnowPowderCloud3D::set_terrain_path);
	ClassDB::bind_method(D_METHOD("get_terrain_path"), &SnowPowderCloud3D::get_terrain_path);
	ClassDB::bind_method(D_METHOD("set_enabled", "enable"), &SnowPowderCloud3D::set_enabled);
	ClassDB::bind_method(D_METHOD("is_enabled"), &SnowPowderCloud3D::is_enabled);
	ClassDB::bind_method(D_METHOD("set_max_sources", "count"), &SnowPowderCloud3D::set_max_sources);
	ClassDB::bind_method(D_METHOD("get_max_sources"), &SnowPowderCloud3D::get_max_sources);
	ClassDB::bind_method(D_METHOD("set_smoke_per_kg", "value"), &SnowPowderCloud3D::set_smoke_per_kg);
	ClassDB::bind_method(D_METHOD("get_smoke_per_kg"), &SnowPowderCloud3D::get_smoke_per_kg);
	ClassDB::bind_method(D_METHOD("set_heaviness", "value"), &SnowPowderCloud3D::set_heaviness);
	ClassDB::bind_method(D_METHOD("get_heaviness"), &SnowPowderCloud3D::get_heaviness);
	ClassDB::bind_method(D_METHOD("set_updraft", "value"), &SnowPowderCloud3D::set_updraft);
	ClassDB::bind_method(D_METHOD("get_updraft"), &SnowPowderCloud3D::get_updraft);
	ClassDB::bind_method(D_METHOD("set_color", "color"), &SnowPowderCloud3D::set_color);
	ClassDB::bind_method(D_METHOD("get_color"), &SnowPowderCloud3D::get_color);

	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enabled"), "set_enabled", "is_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_sources", PROPERTY_HINT_RANGE, "1,32,1"), "set_max_sources", "get_max_sources");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "smoke_per_kg", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_smoke_per_kg", "get_smoke_per_kg");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "heaviness", PROPERTY_HINT_RANGE, "0,5,0.01"), "set_heaviness", "get_heaviness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "updraft", PROPERTY_HINT_RANGE, "0,2,0.01"), "set_updraft", "get_updraft");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "color"), "set_color", "get_color");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "terrain_path"), "set_terrain_path", "get_terrain_path");
}

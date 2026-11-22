/**************************************************************************/
/*  physx_dust_devils_3d.cpp                                                */
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

#include "physx_dust_devils_3d.h"

#include "physx_flow_3d.h"
#include "physx_flow_emitter_3d.h"

#include "core/object/class_db.h"
#include "modules/modules_enabled.gen.h" // MODULE_WEATHER_ENABLED
#include "modules/world/world_contracts.h"
#include "scene/3d/camera_3d.h"
#include "scene/main/viewport.h"

#ifdef MODULE_WEATHER_ENABLED
#include "modules/weather/weather_map_3d.h"
#endif

float PhysXDustDevils3D::_rand() {
	rng = rng * 1664525u + 1013904223u;
	return float(rng >> 8) * (1.0f / 16777216.0f);
}

Node3D *PhysXDustDevils3D::_target() const {
	if (!follow_target.is_empty()) {
		return Object::cast_to<Node3D>(get_node_or_null(follow_target));
	}
	return is_inside_tree() && get_viewport() ? get_viewport()->get_camera_3d() : nullptr;
}

float PhysXDustDevils3D::_ground_y(const Vector3 &p_at) const {
	const WorldGround *g = WorldContracts::ground();
	if (g && g->has_provider()) {
		const float h = g->height_at(p_at);
		if (Math::is_finite(h)) {
			return h;
		}
	}
	return p_at.y;
}

// The loose dust a vortex can lift there: the ground's dry, wind-moved sand share, or all of it with no ground
// producer (the bench's own plane).
float PhysXDustDevils3D::_dust_at(const Vector3 &p_at) const {
	const WorldGround *g = WorldContracts::ground();
	if (g && g->has_provider()) {
		return CLAMP(g->blown_sand_at(p_at), 0.0f, 1.0f);
	}
	return 1.0f;
}

// How favourable the air and the ground are, 0..1: a surface well over the air's temperature (the superadiabatic
// layer that feeds the vortex: none under about min_heating K, full some 12 K over it; Sinclair 1969), sunshine,
// light wind (Balme and Greeley 2006: few over about 8 m/s at 10 m, the shear tears the column), no rain, dry ground.
float PhysXDustDevils3D::get_favour_at(const Vector3 &p_at) const {
	const WorldWeather *w = WorldContracts::weather();
	if (!w || !w->has_provider()) {
		return 0.0f;
	}
	if (w->rainfall_at(p_at) > 0.05f) {
		return 0.0f;
	}
	const float heat = w->surface_temperature_at(p_at) - w->air_temperature_at(p_at);
	float f = Math::smoothstep(min_heating, min_heating + 12.0f, heat);
	// The 10 m mean wind: not wind_at, which carries the gusts and the devils' own swirl (WeatherMap3D add_vortex).
	const Vector3 wind = w->mean_wind_at(p_at);
	f *= 1.0f - Math::smoothstep(6.0f, 11.0f, Vector2(wind.x, wind.z).length());
	f *= 1.0f - Math::smoothstep(0.1f, 0.3f, w->surface_wetness_at(p_at));
	f *= CLAMP(w->sun_visibility_at(p_at), 0.0f, 1.0f);
	return f;
}

void PhysXDustDevils3D::set_terrain_path(const NodePath &p_path) {
	terrain_path = p_path;
	if (flow && is_inside_tree()) {
		Node *n = get_node_or_null(terrain_path);
		flow->set_terrain_path(n ? flow->get_path_to(n) : NodePath());
	}
}

void PhysXDustDevils3D::set_dust_color(const Color &p_c) {
	dust_color = p_c;
	if (flow) {
		flow->set_smoke_color(dust_color);
	}
}

void PhysXDustDevils3D::set_max_devils(int p_v) {
	max_devils = CLAMP(p_v, 0, 8);
	if (flow) {
		_pool();
	}
}

void PhysXDustDevils3D::_build() {
	if (flow) {
		return;
	}
	flow = memnew(PhysXFlow3D);
	flow->set_name("Flow");
	// Dust in warm rising air: no fire, the core's heat lifts it (buoyancy), the dust a little heavy in itself,
	// a slow fade (the fines hang for tens of seconds), strong vorticity confinement to keep the column's spin.
	flow->set_cell_size(flow_cell_size);
	flow->set_max_blocks(1024);
	flow->set_combustion(false);
	flow->set_buoyancy(2.0f);
	flow->set_cooling_rate(0.3f);
	flow->set_smoke_buoyancy(0.0f); // the fines ride the air
	flow->set_smoke_fade(0.06f); // the fines hang for minutes: the column is as tall as the updraft carries them
	flow->set_vorticity(3.0f);
	flow->set_emission_strength(0.0f);
	flow->set_smoke_density(0.8f);
	// A few kelvin of warm air, not a fire: no glow, no fire light or embers, and the shimmer of its few kelvin.
	flow->set_temperature_range(1.0f);
	flow->set_fire_light(false);
	flow->set_embers(false);
	flow->set_heat_shimmer_kelvin(4.0f);
	// The weather's wind layers pull the gas toward the level wind; a weak pull lets the column keep its own
	// updraft and spin and still lean and drift with the wind (the devil itself moves with it, see _step).
	flow->set_weather_wind_coupling(0.25f);
	// Fine mineral dust scatters strongly and forward (a bright, tan column against the sky, Sinclair 1969's photographs).
	flow->set_scattering_strength(2.0f);
	flow->set_smoke_color(dust_color);
	flow->set_terrain_floor(true);
	add_child(flow, false, INTERNAL_MODE_FRONT);
	flow->set_as_top_level(true);
	flow->set_global_transform(Transform3D());
	if (!terrain_path.is_empty()) {
		if (Node *n = get_node_or_null(terrain_path)) {
			flow->set_terrain_path(flow->get_path_to(n));
		}
	}
	_pool();
}

void PhysXDustDevils3D::_pool() {
	auto make = [&](const String &p_name) {
		PhysXFlowEmitter3D *e = memnew(PhysXFlowEmitter3D);
		e->set_regional_smoke(false); // its smoke is reported by its owner, or is not smoke
		e->set_name(p_name);
		e->set_shape(PhysXFlowEmitter3D::SHAPE_SPHERE);
		e->set_smoke(0.0f);
		e->set_temperature(0.0f);
		e->set_fuel(0.0f);
		e->set_enabled(false);
		flow->add_child(e);
		e->set_flow_path(e->get_path_to(flow));
		return e;
	};
	for (Devil &d : devils) {
		for (int k = 0; k < RING; k++) {
			if (d.foot[k]) {
				d.foot[k]->queue_free();
			}
			for (int l = 0; l < LEVELS; l++) {
				if (d.upper[l][k]) {
					d.upper[l][k]->queue_free();
				}
			}
		}
		if (d.core) {
			d.core->queue_free();
		}
	}
	LocalVector<Devil> old;
	for (const Devil &d : devils) {
		old.push_back(d);
	}
	devils.resize(max_devils);
	for (int i = 0; i < max_devils; i++) {
		Devil &d = devils[i];
		d = i < int(old.size()) ? old[i] : Devil();
		for (int k = 0; k < RING; k++) {
			d.foot[k] = make(vformat("Devil%dFoot%d", i, k));
			for (int l = 0; l < LEVELS; l++) {
				d.upper[l][k] = make(vformat("Devil%dRing%d_%d", i, l, k));
			}
		}
		d.core = make(vformat("Devil%dCore", i));
		d.core->set_shape(PhysXFlowEmitter3D::SHAPE_BOX);
	}
}

void PhysXDustDevils3D::_spawn(const Vector3 &p_at, float p_radius, float p_swirl) {
	for (Devil &d : devils) {
		if (d.alive) {
			continue;
		}
		d.alive = true;
		d.position = Vector3(p_at.x, _ground_y(p_at), p_at.z);
		// Most are small: 1.5 to 6 m across the swirl ring, 6 to 15 m/s on it (Sinclair 1969), a few minutes long.
		const float r = _rand();
		d.radius = p_radius > 0.0f ? p_radius : 1.5f + 4.5f * r * r;
		d.swirl = p_swirl > 0.0f ? p_swirl : Math::lerp(6.0f, 15.0f, _rand()) * (0.6f + 0.4f * MAX(favour, 0.5f));
		d.spin = _rand() < 0.5f ? -1.0f : 1.0f; // either sense, about evenly (Sinclair 1965)
		d.age = 0.0f;
		const float l = _rand();
		d.life = 60.0f + 300.0f * l * l;
		d.drift = Vector2();
		d.dust = _dust_at(d.position);
		spawned++;
		return;
	}
}

int PhysXDustDevils3D::spawn(const Vector3 &p_at, float p_radius, float p_swirl) {
	if (!flow) {
		_build();
	}
	const int before = get_devil_count();
	_spawn(p_at, p_radius, p_swirl);
	return get_devil_count() > before ? get_devil_count() - 1 : -1;
}

int PhysXDustDevils3D::get_devil_count() const {
	int n = 0;
	for (const Devil &d : devils) {
		n += d.alive ? 1 : 0;
	}
	return n;
}

static float _devil_strength(float p_age, float p_life) {
	return Math::smoothstep(0.0f, 15.0f, p_age) * (1.0f - Math::smoothstep(p_life - 25.0f, p_life, p_age));
}

Vector4 PhysXDustDevils3D::get_devil(int p_index) const {
	int n = 0;
	for (const Devil &d : devils) {
		if (!d.alive) {
			continue;
		}
		if (n++ == p_index) {
			return Vector4(d.position.x, d.position.z, d.radius, d.swirl * _devil_strength(d.age, d.life));
		}
	}
	return Vector4();
}

float PhysXDustDevils3D::get_devil_age(int p_index) const {
	int n = 0;
	for (const Devil &d : devils) {
		if (d.alive && n++ == p_index) {
			return d.age;
		}
	}
	return -1.0f;
}

void PhysXDustDevils3D::_step(float p_dt) {
	Node3D *t = _target();
	const Vector3 c = t ? t->get_global_position() : get_global_position();
	favour = get_favour_at(c);
	// Spawning: a Poisson process at frequency x favour over the disc round the target, each at a random spot with
	// loose dust, apart from the others.
	if (enabled && favour > 0.0f && get_devil_count() < max_devils) {
		const float per_hour = frequency * favour * Math::PI * spawn_range * spawn_range * 1e-6f;
		if (_rand() < per_hour * p_dt / 3600.0f) {
			const float a = _rand() * Math::TAU;
			const float r = spawn_range * Math::sqrt(Math::lerp(0.04f, 1.0f, _rand()));
			const Vector3 p = c + Vector3(Math::cos(a), 0.0f, Math::sin(a)) * r;
			bool clear = _dust_at(p) > 0.2f && get_favour_at(p) > 0.0f;
			for (const Devil &d : devils) {
				clear = clear && (!d.alive || Vector2(d.position.x - p.x, d.position.z - p.z).length() > 30.0f);
			}
			if (clear) {
				_spawn(p, -1.0f, -1.0f);
			}
		}
	}
	const WorldWeather *w = WorldContracts::weather();
	for (Devil &d : devils) {
		if (!d.alive) {
			_share_wind(d, 0.0f);
			_draw(d);
			continue;
		}
		d.age += p_dt;
		// They travel with the wind near the ground (about 0.7 of the 10 m wind) and wander a little.
		Vector2 wind;
		if (w && w->has_provider()) {
			const Vector3 wv = w->mean_wind_at(d.position); // not its own swirl
			wind = Vector2(wv.x, wv.z) * 0.7f;
		}
		d.drift = d.drift.lerp(Vector2(_rand() - 0.5f, _rand() - 0.5f) * 3.0f, MIN(p_dt * 0.2f, 1.0f));
		const Vector2 v = wind + d.drift;
		d.position.x += v.x * p_dt;
		d.position.z += v.y * p_dt;
		d.position.y = _ground_y(d.position);
		d.dust = Math::lerp(d.dust, _dust_at(d.position), MIN(p_dt, 1.0f));
		// Wet ground, a strong wind or the sun gone: it dies within half a minute; past twice the range, at once.
		if (!enabled || get_favour_at(d.position) <= 0.0f) {
			d.life = MIN(d.life, d.age + 25.0f);
		}
		if (d.age >= d.life || Vector2(d.position.x - c.x, d.position.z - c.z).length() > 2.0f * spawn_range) {
			d.alive = false;
		}
		_share_wind(d, d.alive ? _devil_strength(d.age, d.life) : 0.0f);
		_draw(d);
	}
}

// The column in Flow: a ring of spinning, inflowing, dust-lifting spheres at the foot, three spinning rings up to six
// radii up so the column keeps its spin as it rises, and a warm core that lifts it (Rankine's vortex: solid rotation
// inside the ring, the swirl falling off outside it, which Flow's own advection gives round the forced ring).
void PhysXDustDevils3D::_draw(Devil &p_d) {
	const float e = p_d.alive ? _devil_strength(p_d.age, p_d.life) : 0.0f;
	const bool on = e > 0.0f;
	const float R = p_d.radius;
	const float V = p_d.swirl * e;
	const float w = 0.5f * p_d.swirl * e;
	const float re = MAX(0.35f * R, 2.0f * flow_cell_size);
	const Vector3 up(0.0f, 1.0f, 0.0f);
	for (int k = 0; k < RING; k++) {
		PhysXFlowEmitter3D *foot = p_d.foot[k];
		if (!foot) {
			continue;
		}
		foot->set_enabled(on);
		for (int l = 0; l < LEVELS; l++) {
			if (p_d.upper[l][k]) {
				p_d.upper[l][k]->set_enabled(on);
			}
		}
		if (!on) {
			continue;
		}
		const float a = Math::TAU * k / RING;
		const Vector3 radial(Math::cos(a), 0.0f, Math::sin(a));
		const Vector3 tangent = Vector3(-radial.z, 0.0f, radial.x) * p_d.spin;
		foot->set_radius(re);
		foot->set_global_position(p_d.position + radial * R + up * (0.8f * re));
		foot->set_velocity(tangent * V + up * (0.8f * w) - radial * (0.3f * V));
		foot->set_smoke(0.5f * e * p_d.dust);
		// The superheated surface layer the vortex feeds on (Sinclair 1969): warm at the foot too, so the dust it
		// lifts rises with it instead of pooling on the ground.
		foot->set_temperature(0.6f * e);
		foot->set_couple_rate(8.0f);
		// Rings at 1.5, 3.5 and 6 radii keep the wall spinning up the column (the swirl holds the dust in the wall,
		// the inflow below feeds it), each a little wider and slower than the last, as the funnel flares.
		static const float level_h[LEVELS] = { 1.5f, 3.5f, 6.0f };
		for (int l = 0; l < LEVELS; l++) {
			PhysXFlowEmitter3D *upper = p_d.upper[l][k];
			if (!upper) {
				continue;
			}
			const float flare = 1.0f + 0.12f * level_h[l];
			// Rotated half a step a level, so the rings interleave.
			const float al = a + (l & 1 ? Math::PI / RING : 0.0f);
			const Vector3 rl(Math::cos(al), 0.0f, Math::sin(al));
			const Vector3 tl = Vector3(-rl.z, 0.0f, rl.x) * p_d.spin;
			upper->set_radius(re);
			upper->set_global_position(p_d.position + rl * (R * flare) + up * (level_h[l] * R));
			upper->set_velocity(tl * (V / flare) + up * w);
			upper->set_couple_rate(6.0f);
		}
	}
	if (p_d.core) {
		p_d.core->set_enabled(on);
		if (on) {
			// The rising core inside the wall, from the ground to the upper ring.
			const float cw = MAX(1.4f * R, 2.0f * flow_cell_size);
			p_d.core->set_size(Vector3(cw, 6.0f * R, cw));
			p_d.core->set_global_position(p_d.position + up * (3.0f * R + 0.5f * re));
			p_d.core->set_velocity(up * (1.2f * w));
			p_d.core->set_temperature(e);
			p_d.core->set_couple_rate(6.0f);
		}
	}
}

// The devil in the shared wind (docs/world-contracts.md, "Wind vortices"), so leaves, grass, dust, smoke and every
// other gas feel it: its swirl and an updraft of 0.3 of it (Tratt et al. 2003: 2 to 3 m/s in a 5 to 10 m/s devil).
// The Flow column forces its own, stronger updraft to carry the dust; this is the air the rest of the world reads.
void PhysXDustDevils3D::_share_wind(Devil &p_d, float p_strength) {
#ifdef MODULE_WEATHER_ENABLED
	WeatherMap3D *wm = WeatherMap3D::get_active();
	if (!wm || wm->get_instance_id() != vortex_map) {
		for (Devil &d : devils) {
			d.vortex_id = 0; // a new map (or none): the old ids mean nothing there
		}
		vortex_map = wm ? wm->get_instance_id() : ObjectID();
	}
	if (!wm) {
		return;
	}
	if (p_strength <= 0.0f) {
		if (p_d.vortex_id != 0) {
			wm->remove_vortex(p_d.vortex_id);
			p_d.vortex_id = 0;
		}
		return;
	}
	const float swirl = p_d.swirl * p_strength;
	if (p_d.vortex_id == 0) {
		p_d.vortex_id = wm->add_vortex(p_d.position, p_d.radius, swirl, 0.3f * swirl, p_d.spin);
	} else {
		wm->set_vortex(p_d.vortex_id, p_d.position, p_d.radius, swirl, 0.3f * swirl);
	}
#endif
}

void PhysXDustDevils3D::_clear_wind() {
#ifdef MODULE_WEATHER_ENABLED
	WeatherMap3D *wm = Object::cast_to<WeatherMap3D>(ObjectDB::get_instance(vortex_map));
	for (Devil &d : devils) {
		if (wm && d.vortex_id != 0) {
			wm->remove_vortex(d.vortex_id);
		}
		d.vortex_id = 0;
	}
#endif
}

void PhysXDustDevils3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_EXIT_TREE: {
			_clear_wind();
		} break;
		case NOTIFICATION_READY: {
			rng = seed * 2654435761u + 1u;
			_build();
			set_physics_process_internal(true);
		} break;
		case NOTIFICATION_INTERNAL_PHYSICS_PROCESS: {
			_step(float(get_physics_process_delta_time()));
		} break;
	}
}

void PhysXDustDevils3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_enabled", "enabled"), &PhysXDustDevils3D::set_enabled);
	ClassDB::bind_method(D_METHOD("is_enabled"), &PhysXDustDevils3D::is_enabled);
	ClassDB::bind_method(D_METHOD("set_follow_target", "path"), &PhysXDustDevils3D::set_follow_target);
	ClassDB::bind_method(D_METHOD("get_follow_target"), &PhysXDustDevils3D::get_follow_target);
	ClassDB::bind_method(D_METHOD("set_terrain_path", "path"), &PhysXDustDevils3D::set_terrain_path);
	ClassDB::bind_method(D_METHOD("get_terrain_path"), &PhysXDustDevils3D::get_terrain_path);
	ClassDB::bind_method(D_METHOD("set_spawn_range", "range"), &PhysXDustDevils3D::set_spawn_range);
	ClassDB::bind_method(D_METHOD("get_spawn_range"), &PhysXDustDevils3D::get_spawn_range);
	ClassDB::bind_method(D_METHOD("set_max_devils", "count"), &PhysXDustDevils3D::set_max_devils);
	ClassDB::bind_method(D_METHOD("get_max_devils"), &PhysXDustDevils3D::get_max_devils);
	ClassDB::bind_method(D_METHOD("set_frequency", "per_km2_hour"), &PhysXDustDevils3D::set_frequency);
	ClassDB::bind_method(D_METHOD("get_frequency"), &PhysXDustDevils3D::get_frequency);
	ClassDB::bind_method(D_METHOD("set_min_heating", "kelvin"), &PhysXDustDevils3D::set_min_heating);
	ClassDB::bind_method(D_METHOD("get_min_heating"), &PhysXDustDevils3D::get_min_heating);
	ClassDB::bind_method(D_METHOD("set_flow_cell_size", "size"), &PhysXDustDevils3D::set_flow_cell_size);
	ClassDB::bind_method(D_METHOD("get_flow_cell_size"), &PhysXDustDevils3D::get_flow_cell_size);
	ClassDB::bind_method(D_METHOD("set_dust_color", "color"), &PhysXDustDevils3D::set_dust_color);
	ClassDB::bind_method(D_METHOD("get_dust_color"), &PhysXDustDevils3D::get_dust_color);
	ClassDB::bind_method(D_METHOD("set_seed", "seed"), &PhysXDustDevils3D::set_seed);
	ClassDB::bind_method(D_METHOD("get_seed"), &PhysXDustDevils3D::get_seed);
	ClassDB::bind_method(D_METHOD("get_favour"), &PhysXDustDevils3D::get_favour);
	ClassDB::bind_method(D_METHOD("get_favour_at", "position"), &PhysXDustDevils3D::get_favour_at);
	ClassDB::bind_method(D_METHOD("spawn", "position", "radius", "swirl"), &PhysXDustDevils3D::spawn, DEFVAL(-1.0f), DEFVAL(-1.0f));
	ClassDB::bind_method(D_METHOD("get_devil_count"), &PhysXDustDevils3D::get_devil_count);
	ClassDB::bind_method(D_METHOD("get_devil", "index"), &PhysXDustDevils3D::get_devil);
	ClassDB::bind_method(D_METHOD("get_devil_age", "index"), &PhysXDustDevils3D::get_devil_age);
	ClassDB::bind_method(D_METHOD("get_spawned"), &PhysXDustDevils3D::get_spawned);
	ClassDB::bind_method(D_METHOD("get_flow"), &PhysXDustDevils3D::get_flow);

	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enabled"), "set_enabled", "is_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "follow_target", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D"), "set_follow_target", "get_follow_target");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "terrain_path"), "set_terrain_path", "get_terrain_path");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "spawn_range", PROPERTY_HINT_RANGE, "10,2000,1,suffix:m"), "set_spawn_range", "get_spawn_range");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_devils", PROPERTY_HINT_RANGE, "0,8,1"), "set_max_devils", "get_max_devils");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "frequency", PROPERTY_HINT_RANGE, "0,1000,0.1"), "set_frequency", "get_frequency");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "min_heating", PROPERTY_HINT_RANGE, "0,40,0.1,suffix:K"), "set_min_heating", "get_min_heating");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "flow_cell_size", PROPERTY_HINT_RANGE, "0.05,2,0.01,suffix:m"), "set_flow_cell_size", "get_flow_cell_size");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "dust_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_dust_color", "get_dust_color");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "seed"), "set_seed", "get_seed");
}

/**************************************************************************/
/*  weather_runoff_3d.cpp                                                 */
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

#include "weather_runoff_3d.h"

#include "weather_map_3d.h"

#include "core/config/engine.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/templates/sort_array.h"
#include "scene/resources/3d/world_3d.h"
#include "servers/physics_3d/physics_server_3d.h"

#include <queue>

namespace {
template <typename T, typename V>
void lv_fill(LocalVector<T> &r_v, V p_value) {
	for (uint32_t i = 0; i < r_v.size(); i++) {
		r_v[i] = T(p_value);
	}
}
} // namespace

WeatherRunoff3D *WeatherRunoff3D::active = nullptr;

float WeatherRunoff3D::scs_runoff(float p_rain_mm, float p_curve_number) {
	const float cn = CLAMP(p_curve_number, 30.0f, 100.0f);
	if (cn >= 99.99f) {
		return MAX(p_rain_mm, 0.0f);
	}
	const float s = 25400.0f / cn - 254.0f;
	const float ia = 0.2f * s;
	if (p_rain_mm <= ia) {
		return 0.0f;
	}
	return (p_rain_mm - ia) * (p_rain_mm - ia) / (p_rain_mm - ia + s);
}

float WeatherRunoff3D::shallow_flow_velocity(float p_slope) {
	return 4.918f * Math::sqrt(MAX(p_slope, 0.0f));
}

float WeatherRunoff3D::channel_velocity(float p_discharge) {
	return 0.63f * Math::pow(MAX(p_discharge, 1e-4f), 0.1f);
}

bool WeatherRunoff3D::_sample_heights() {
	const int n = resolution;
	cell = extent / n;
	const Vector3 c = is_inside_tree() ? get_global_position() : Vector3();
	origin = Vector2(c.x - extent * 0.5f, c.z - extent * 0.5f);
	height.resize(n * n);
	Node *src = is_inside_tree() && !height_source.is_empty() ? get_node_or_null(height_source) : nullptr;
	const bool by_method = src && src->has_method(SNAME("get_height_at"));
	PhysicsDirectSpaceState3D *space = !by_method && is_inside_tree() && get_world_3d().is_valid() ? get_world_3d()->get_direct_space_state() : nullptr;
	if (!by_method && !space) {
		return false;
	}
	for (int z = 0; z < n; z++) {
		for (int x = 0; x < n; x++) {
			const Vector3 p(origin.x + (x + 0.5f) * cell, c.y, origin.y + (z + 0.5f) * cell);
			float h = -Math::INF;
			if (by_method) {
				const Variant v = src->call(SNAME("get_height_at"), p);
				if (v.get_type() == Variant::FLOAT || v.get_type() == Variant::INT) {
					h = float(v);
				}
			} else {
				PhysicsDirectSpaceState3D::RayParameters rp;
				rp.from = p + Vector3(0, 10000.0f, 0);
				rp.to = p - Vector3(0, 10000.0f, 0);
				rp.collision_mask = collision_mask;
				PhysicsDirectSpaceState3D::RayResult rr;
				if (space->intersect_ray(rp, rr)) {
					h = rr.position.y;
				}
			}
			height[z * n + x] = Math::is_finite(h) ? h : -Math::INF;
		}
	}
	return true;
}

void WeatherRunoff3D::_build_network() {
	const int n = resolution;
	const int nn = n * n;
	// Priority-Flood with an epsilon (Barnes, Lehman and Mulla 2014, algorithm 3): from the edges (and the cells with no
	// ground) inward, each cell raised to just above the lowest spill path, so every cell has a way down to the edge.
	LocalVector<float> filled;
	filled.resize(nn);
	LocalVector<uint8_t> closed;
	closed.resize(nn);
	lv_fill(closed, 0);
	receiver.resize(nn);
	lv_fill(receiver, -1);
	struct Item {
		float z;
		int32_t i;
		bool operator<(const Item &o) const { return z > o.z; } // a min-heap on std::priority_queue
	};
	std::priority_queue<Item> open;
	for (int z = 0; z < n; z++) {
		for (int x = 0; x < n; x++) {
			const int i = z * n + x;
			if (x == 0 || z == 0 || x == n - 1 || z == n - 1 || !Math::is_finite(height[i])) {
				filled[i] = Math::is_finite(height[i]) ? height[i] : -1e9f;
				closed[i] = 1;
				open.push({ filled[i], i });
			}
		}
	}
	order.clear();
	order.reserve(nn);
	static const int DX[8] = { 1, -1, 0, 0, 1, 1, -1, -1 };
	static const int DZ[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
	while (!open.empty()) {
		const Item it = open.top();
		open.pop();
		order.push_back(it.i);
		const int x = it.i % n;
		const int z = it.i / n;
		for (int k = 0; k < 8; k++) {
			const int nx = x + DX[k];
			const int nz = z + DZ[k];
			if (nx < 0 || nz < 0 || nx >= n || nz >= n) {
				continue;
			}
			const int j = nz * n + nx;
			if (closed[j]) {
				continue;
			}
			closed[j] = 1;
			// Raised a hair above the cell it spills through, so flats drain toward their outlet.
			filled[j] = MAX(height[j], it.z + 1e-4f);
			// The cell it was reached from is its way out: on the filled surface that is downhill (or the spill path).
			receiver[j] = it.i;
			open.push({ filled[j], j });
		}
	}
	// The flood visits cells lowest first, each after the cell it drains to: reversed, upstream first.
	order.reverse();
	// A steeper way down than the flood's, where there is one on the filled surface (D8 steepest descent).
	slope.resize(nn);
	for (int i = 0; i < nn; i++) {
		const int x = i % n;
		const int z = i / n;
		int best = receiver[i];
		float best_s = best >= 0 ? (filled[i] - filled[best]) / (cell * Vector2(float(best % n - x), float(best / n - z)).length()) : 0.0f;
		for (int k = 0; k < 8; k++) {
			const int nx = x + DX[k];
			const int nz = z + DZ[k];
			if (nx < 0 || nz < 0 || nx >= n || nz >= n) {
				continue;
			}
			const int j = nz * n + nx;
			const float s = (filled[i] - filled[j]) / (cell * (k < 4 ? 1.0f : float(Math::SQRT2)));
			if (s > best_s && filled[j] < filled[i]) {
				best_s = s;
				best = j;
			}
		}
		if (best >= 0 && !(filled[best] < filled[i] || best == receiver[i])) {
			best = receiver[i];
		}
		receiver[i] = closed[i] && (i % n == 0 || i / n == 0 || i % n == n - 1 || i / n == n - 1) && best >= 0 && filled[best] >= filled[i] ? -1 : best;
		// The real ground's slope for the flow's speed (flats and filled pits crawl).
		const float h0 = Math::is_finite(height[i]) ? height[i] : filled[i];
		const float h1 = best >= 0 && Math::is_finite(height[best]) ? height[best] : h0;
		const float dist = best >= 0 ? cell * Vector2(float(best % n - x), float(best / n - z)).length() : cell;
		slope[i] = CLAMP((h0 - h1) / MAX(dist, 1e-3f), 0.001f, 2.0f);
	}
	// Upstream first means the receivers come later: a single pass carries the area down. Receivers are re-checked so
	// the order holds (a steeper receiver may have been visited before its donor).
	LocalVector<int32_t> rank;
	rank.resize(nn);
	for (int k = 0; k < nn; k++) {
		rank[order[k]] = k;
	}
	for (int i = 0; i < nn; i++) {
		if (receiver[i] >= 0 && rank[receiver[i]] <= rank[i]) {
			// Keep the flood's own receiver, which is always visited after its donor.
			int r = -1;
			const int x = i % n;
			const int z = i / n;
			for (int k = 0; k < 8 && r < 0; k++) {
				const int nx = x + DX[k];
				const int nz = z + DZ[k];
				if (nx >= 0 && nz >= 0 && nx < n && nz < n && rank[nz * n + nx] > rank[i] && filled[nz * n + nx] <= filled[i]) {
					r = nz * n + nx;
				}
			}
			receiver[i] = r;
		}
	}
	area.resize(nn);
	lv_fill(area, cell * cell);
	for (int k = 0; k < nn; k++) {
		const int i = order[k];
		if (receiver[i] >= 0) {
			area[receiver[i]] += area[i];
		}
	}
	storage.resize(nn);
	lv_fill(storage, 0.0f);
	event_p.resize(nn);
	lv_fill(event_p, 0.0f);
	event_q.resize(nn);
	lv_fill(event_q, 0.0f);
	outflow.resize(nn);
	lv_fill(outflow, 0.0f);
	inflow.resize(nn);
	lv_fill(inflow, 0.0f);
	load.resize(nn);
	lv_fill(load, Vector2());
	load_in.resize(nn);
	lv_fill(load_in, Vector2());
	rain_now.resize(nn);
	lv_fill(rain_now, 0.0f);
	fire_burnt.resize(nn);
	lv_fill(fire_burnt, 0.0f);
	fire_kf.resize(nn);
	lv_fill(fire_kf, 0.0f);
	df_fired_at.resize(nn);
	lv_fill(df_fired_at, -1e30f);
	fire_generation = UINT64_MAX;
	baked_n = n;
}

Error WeatherRunoff3D::bake() {
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	if (!_sample_heights()) {
		return ERR_UNCONFIGURED;
	}
	_build_network();
	bake_ms = (OS::get_singleton()->get_ticks_usec() - t0) / 1000.0;
	return OK;
}

void WeatherRunoff3D::_step(double p_dt) {
	const int n = baked_n;
	if (n <= 0 || p_dt <= 0.0) {
		return;
	}
	const WeatherMap3D *w = WeatherMap3D::get_active();
	const float dt = float(p_dt);
	const float dt_h = dt / 3600.0f;
	const float cell_area = cell * cell;
	const float relax = float(Math::exp(-p_dt / (3600.0 * antecedent_time)));
	const float channel_m2 = channel_area * 1e6f;
	WorldContracts *wc = WorldContracts::get_singleton();
	const Ref<WorldWater> water = wc ? wc->get_water() : Ref<WorldWater>();
	lv_fill(inflow, 0.0f);
	for (int k = 0; k < n * n; k++) {
		const int i = order[k];
		const Vector3 p(origin.x + (i % n + 0.5f) * cell, Math::is_finite(height[i]) ? height[i] : 0.0f, origin.y + (i / n + 0.5f) * cell);
		const float input = w ? w->get_ground_water_input_at(p) : 0.0f; // mm/h
		rain_now[i] = input;
		// The storm's running totals (curve number on the event, relaxing when dry).
		float excess = 0.0f;
		if (input > 0.05f) {
			event_p[i] += input * dt_h;
			const float q = scs_runoff(event_p[i], curve_number);
			excess = MAX(q - event_q[i], 0.0f);
			event_q[i] = q;
		} else {
			event_p[i] *= relax;
			event_q[i] = MIN(event_q[i] * relax, scs_runoff(event_p[i], curve_number));
		}
		// The cell's reservoir, emptying over its length at the flow's speed (implicit Euler).
		const float v = area[i] >= channel_m2 ? MAX(channel_velocity(outflow[i]), shallow_flow_velocity(slope[i])) : shallow_flow_velocity(slope[i]);
		const float k_res = cell / CLAMP(v, 0.02f, 6.0f);
		const float in = excess * 0.001f * cell_area + inflow[i] * dt;
		const float s = (storage[i] + in) / (1.0f + dt / k_res);
		storage[i] = s;
		outflow[i] = s / k_res;
		if (receiver[i] >= 0) {
			inflow[receiver[i]] += outflow[i];
		}
		// The load (washload: it moves with the water). The cell held s + outflow dt over the step and passed
		// outflow dt of it on, so that share of its load goes with it.
		Vector2 l = load[i] + load_in[i];
		load_in[i] = Vector2();
		if (l.x <= 0.0f && l.y <= 0.0f) {
			continue;
		}
		if (water.is_valid() && water->depth_at(Vector3(p.x, p.y - 0.05f, p.z)) > 0.0f) {
			// Open water over the cell: it takes the load (kg/s over the step).
			water->add_sediment(p, l.x / dt, l.y / dt);
			delivered += l;
			l = Vector2();
		} else {
			const float out = outflow[i] * dt;
			const float f = out > 0.0f ? out / (s + out) : 0.0f;
			const Vector2 moved = l * f;
			l -= moved;
			if (receiver[i] >= 0) {
				load_in[receiver[i]] += moved;
			}
		}
		load[i] = l;
	}
	_step_debris(p_dt);
}

void WeatherRunoff3D::_step_debris(double p_dt) {
	sim_time += p_dt;
	if (debris_flows) {
		_fire_refresh();
		_debris_flows();
	}
}

void WeatherRunoff3D::contract_runoff_add_load(const Vector3 &p_position, float p_mineral_kg, float p_ash_kg) {
	const int i = _cell_at(p_position);
	if (i < 0) {
		return;
	}
	load[i] += Vector2(MAX(p_mineral_kg, 0.0f), MAX(p_ash_kg, 0.0f));
}

Vector2 WeatherRunoff3D::get_load_at(const Vector3 &p_position) const {
	const int i = _cell_at(p_position);
	return i >= 0 ? load[i] : Vector2();
}

Vector2 WeatherRunoff3D::get_total_load() const {
	Vector2 t;
	for (uint32_t i = 0; i < load.size(); i++) {
		t += load[i] + load_in[i];
	}
	return t;
}

void WeatherRunoff3D::advance(float p_seconds) {
	if (baked_n <= 0) {
		bake();
	}
	_step(MAX(p_seconds, 0.0f));
}

int WeatherRunoff3D::_cell_at(const Vector3 &p_position) const {
	if (baked_n <= 0) {
		return -1;
	}
	const int x = int(Math::floor((p_position.x - origin.x) / cell));
	const int z = int(Math::floor((p_position.z - origin.y) / cell));
	if (x < 0 || z < 0 || x >= baked_n || z >= baked_n) {
		return -1;
	}
	return z * baked_n + x;
}

float WeatherRunoff3D::get_discharge_at(const Vector3 &p_position) const {
	const int i = _cell_at(p_position);
	return i >= 0 ? outflow[i] : 0.0f;
}

float WeatherRunoff3D::get_max_discharge_in(const Rect2 &p_rect) const {
	if (baked_n <= 0) {
		return 0.0f;
	}
	const int x0 = CLAMP(int(Math::floor((p_rect.position.x - origin.x) / cell)), 0, baked_n - 1);
	const int z0 = CLAMP(int(Math::floor((p_rect.position.y - origin.y) / cell)), 0, baked_n - 1);
	const int x1 = CLAMP(int(Math::floor((p_rect.get_end().x - origin.x) / cell)), 0, baked_n - 1);
	const int z1 = CLAMP(int(Math::floor((p_rect.get_end().y - origin.y) / cell)), 0, baked_n - 1);
	float q = 0.0f;
	for (int z = z0; z <= z1; z++) {
		for (int x = x0; x <= x1; x++) {
			q = MAX(q, outflow[z * baked_n + x]);
		}
	}
	return q;
}

float WeatherRunoff3D::get_depth_at(const Vector3 &p_position) const {
	const int i = _cell_at(p_position);
	return i >= 0 ? storage[i] / (cell * cell) * 1000.0f : 0.0f;
}

float WeatherRunoff3D::get_upstream_area_at(const Vector3 &p_position) const {
	const int i = _cell_at(p_position);
	return i >= 0 ? area[i] : 0.0f;
}

float WeatherRunoff3D::get_total_storage() const {
	double s = 0.0;
	for (uint32_t i = 0; i < storage.size(); i++) {
		s += storage[i];
	}
	return float(s);
}

void WeatherRunoff3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			active = this;
			if (!Engine::get_singleton()->is_editor_hint()) {
				set_physics_process_internal(true);
				WorldContracts::register_runoff(this);
			}
		} break;
		case NOTIFICATION_READY: {
			if (bake_on_ready && !Engine::get_singleton()->is_editor_hint()) {
				// After the terrain has built (its colliders and height queries are ready a frame later).
				callable_mp(this, &WeatherRunoff3D::bake).call_deferred();
			}
		} break;
		case NOTIFICATION_EXIT_TREE: {
			if (active == this) {
				active = nullptr;
			}
			WorldContracts::unregister_runoff(this);
			set_physics_process_internal(false);
		} break;
		case NOTIFICATION_INTERNAL_PHYSICS_PROCESS: {
			// Weather time: a storm run fast floods fast.
			const WeatherMap3D *w = WeatherMap3D::get_active();
			const double now = w ? w->get_weather_time() : 0.0;
			accum += get_physics_process_delta_time();
			if (accum >= 1.0 / update_rate && baked_n > 0) {
				accum = 0.0;
				if (last_weather_time >= 0.0 && now > last_weather_time) {
					_step(now - last_weather_time);
				}
				last_weather_time = now;
			}
		} break;
	}
}

void WeatherRunoff3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_extent", "extent"), &WeatherRunoff3D::set_extent);
	ClassDB::bind_method(D_METHOD("get_extent"), &WeatherRunoff3D::get_extent);
	ClassDB::bind_method(D_METHOD("set_resolution", "resolution"), &WeatherRunoff3D::set_resolution);
	ClassDB::bind_method(D_METHOD("get_resolution"), &WeatherRunoff3D::get_resolution);
	ClassDB::bind_method(D_METHOD("set_height_source", "path"), &WeatherRunoff3D::set_height_source);
	ClassDB::bind_method(D_METHOD("get_height_source"), &WeatherRunoff3D::get_height_source);
	ClassDB::bind_method(D_METHOD("set_collision_mask", "mask"), &WeatherRunoff3D::set_collision_mask);
	ClassDB::bind_method(D_METHOD("get_collision_mask"), &WeatherRunoff3D::get_collision_mask);
	ClassDB::bind_method(D_METHOD("set_curve_number", "cn"), &WeatherRunoff3D::set_curve_number);
	ClassDB::bind_method(D_METHOD("get_curve_number"), &WeatherRunoff3D::get_curve_number);
	ClassDB::bind_method(D_METHOD("set_antecedent_time", "hours"), &WeatherRunoff3D::set_antecedent_time);
	ClassDB::bind_method(D_METHOD("get_antecedent_time"), &WeatherRunoff3D::get_antecedent_time);
	ClassDB::bind_method(D_METHOD("set_channel_area", "km2"), &WeatherRunoff3D::set_channel_area);
	ClassDB::bind_method(D_METHOD("get_channel_area"), &WeatherRunoff3D::get_channel_area);
	ClassDB::bind_method(D_METHOD("set_update_rate", "hz"), &WeatherRunoff3D::set_update_rate);
	ClassDB::bind_method(D_METHOD("get_update_rate"), &WeatherRunoff3D::get_update_rate);
	ClassDB::bind_method(D_METHOD("set_bake_on_ready", "enabled"), &WeatherRunoff3D::set_bake_on_ready);
	ClassDB::bind_method(D_METHOD("is_bake_on_ready"), &WeatherRunoff3D::is_bake_on_ready);
	ClassDB::bind_method(D_METHOD("bake"), &WeatherRunoff3D::bake);
	ClassDB::bind_method(D_METHOD("is_baked"), &WeatherRunoff3D::is_baked);
	ClassDB::bind_method(D_METHOD("get_bake_time_msec"), &WeatherRunoff3D::get_bake_time_msec);
	ClassDB::bind_method(D_METHOD("advance", "seconds"), &WeatherRunoff3D::advance);
	ClassDB::bind_method(D_METHOD("get_map_rect"), &WeatherRunoff3D::get_map_rect);
	ClassDB::bind_method(D_METHOD("get_discharge_at", "position"), &WeatherRunoff3D::get_discharge_at);
	ClassDB::bind_method(D_METHOD("get_max_discharge_in", "rect"), &WeatherRunoff3D::get_max_discharge_in);
	ClassDB::bind_method(D_METHOD("get_depth_at", "position"), &WeatherRunoff3D::get_depth_at);
	ClassDB::bind_method(D_METHOD("get_upstream_area_at", "position"), &WeatherRunoff3D::get_upstream_area_at);
	ClassDB::bind_method(D_METHOD("get_total_storage"), &WeatherRunoff3D::get_total_storage);
	ClassDB::bind_method(D_METHOD("add_load", "position", "mineral_kg", "ash_kg"), &WeatherRunoff3D::contract_runoff_add_load);
	ClassDB::bind_method(D_METHOD("get_load_at", "position"), &WeatherRunoff3D::get_load_at);
	ClassDB::bind_method(D_METHOD("get_total_load"), &WeatherRunoff3D::get_total_load);
	ClassDB::bind_method(D_METHOD("get_delivered_load"), &WeatherRunoff3D::get_delivered_load);
	ClassDB::bind_method(D_METHOD("set_debris_flows", "enabled"), &WeatherRunoff3D::set_debris_flows);
	ClassDB::bind_method(D_METHOD("is_debris_flows"), &WeatherRunoff3D::is_debris_flows);
	ClassDB::bind_method(D_METHOD("set_debris_flow_threshold", "probability"), &WeatherRunoff3D::set_debris_flow_threshold);
	ClassDB::bind_method(D_METHOD("get_debris_flow_threshold"), &WeatherRunoff3D::get_debris_flow_threshold);
	ClassDB::bind_method(D_METHOD("get_debris_flows_since", "id"), &WeatherRunoff3D::get_debris_flows_since);
	ClassDB::bind_static_method("WeatherRunoff3D", D_METHOD("debris_flow_likelihood", "rain15_mm", "burnt_steep_share", "burnt", "kf"), &WeatherRunoff3D::debris_flow_likelihood);
	ClassDB::bind_static_method("WeatherRunoff3D", D_METHOD("debris_flow_volume", "i15_mm_h", "burnt_km2", "relief_m"), &WeatherRunoff3D::debris_flow_volume);
	ClassDB::bind_static_method("WeatherRunoff3D", D_METHOD("bankfull_width", "area_km2"), &WeatherRunoff3D::bankfull_width);
	ADD_SIGNAL(MethodInfo("debris_flow", PropertyInfo(Variant::DICTIONARY, "event")));
	ClassDB::bind_static_method("WeatherRunoff3D", D_METHOD("scs_runoff", "rain_mm", "curve_number"), &WeatherRunoff3D::scs_runoff);
	ClassDB::bind_static_method("WeatherRunoff3D", D_METHOD("shallow_flow_velocity", "slope"), &WeatherRunoff3D::shallow_flow_velocity);
	ClassDB::bind_static_method("WeatherRunoff3D", D_METHOD("channel_velocity", "discharge"), &WeatherRunoff3D::channel_velocity);

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "extent", PROPERTY_HINT_RANGE, "64,262144,1,suffix:m"), "set_extent", "get_extent");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "resolution", PROPERTY_HINT_RANGE, "8,2048,1"), "set_resolution", "get_resolution");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "height_source"), "set_height_source", "get_height_source");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_mask", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_collision_mask", "get_collision_mask");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "curve_number", PROPERTY_HINT_RANGE, "30,100,0.5"), "set_curve_number", "get_curve_number");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "antecedent_time", PROPERTY_HINT_RANGE, "0.1,720,0.1,suffix:h"), "set_antecedent_time", "get_antecedent_time");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "channel_area", PROPERTY_HINT_RANGE, "0,100,0.01,suffix:km²"), "set_channel_area", "get_channel_area");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "update_rate", PROPERTY_HINT_RANGE, "0.01,60,0.01,suffix:Hz"), "set_update_rate", "get_update_rate");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "bake_on_ready"), "set_bake_on_ready", "is_bake_on_ready");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "debris_flows"), "set_debris_flows", "is_debris_flows");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "debris_flow_threshold", PROPERTY_HINT_RANGE, "0.01,0.99,0.01"), "set_debris_flow_threshold", "get_debris_flow_threshold");
}

WeatherRunoff3D::WeatherRunoff3D() {
}

WeatherRunoff3D::~WeatherRunoff3D() {
	if (active == this) {
		active = nullptr;
	}
}

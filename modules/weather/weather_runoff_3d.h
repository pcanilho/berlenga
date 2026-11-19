/**************************************************************************/
/*  weather_runoff_3d.h                                                   */
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

#pragma once

#include "core/object/object_id.h"
#include "modules/world/world_contracts.h"
#include "scene/3d/node_3d.h"

// Rain running off the land at world scale (docs/ideas/rain.md, step 6), beyond the ground's and the live water's
// windows. A square of cells over the terrain, its heights from a height source (anything with get_height_at(), a
// TerrainDataMap3D or TerrainGenerator3D) or ray casts.
//
// - Routing network: the surface filled by Priority-Flood (Barnes, Lehman and Mulla 2014) so every cell drains, then
//   one steepest-descent receiver per cell (D8, O'Callaghan and Mark 1984) and the cells in upstream-first order.
// - Runoff: the SCS curve number method (USDA NRCS, National Engineering Handbook 630, ch. 10), on the storm's running
//   totals: S = 25400 / CN - 254 mm, initial abstraction 0.2 S, Q = (P - 0.2 S)^2 / (P + 0.8 S); the totals relax
//   over antecedent_time when it is dry, so the next storm meets drier soil. The water reaching the ground is the
//   weather map's (rain at its liquid share, or a snowpack's runoff where one lies), so snowmelt runs off too.
// - Travel: each cell a linear reservoir emptying over its length at the flow's speed: TR-55 shallow concentrated
//   flow on slopes (v = 4.918 sqrt(s) m/s unpaved; NRCS TR-55, 1986), Leopold and Maddock's (1953) downstream
//   hydraulic geometry in channels (v grows as Q^0.1, here 1 m/s at 100 m3/s). Solved implicitly, so any step is stable.
//
// Rivers (PhysXRiver3D) add the discharge reaching their upstream end to their base flow, so they rise and quicken;
// lakes take what flows into them; the weather map shows the water as wet ground along the drainage lines.
// - Load: the runoff contract's provider (WorldGround.add_runoff_load: ash off burnt ground, mud washed off a slope).
//   Each cell's load moves on with the share of its water that leaves it each step (washload travels with the water),
//   and where the flow reaches open water (WorldWater has a surface over the cell's ground) it is handed to that water
//   (WorldWater.add_sediment) as a rate over the step. Load put off the map, or carried off it, is lost.
// - Post-fire debris flows (weather_runoff_debris.cpp): over ground burnt in the last two years or so (WorldFire), each
//   small basin's likelihood of a debris flow in the storm's 15-minute rain by the USGS M1 model (Staley et al. 2017)
//   and its volume by Gartner et al. (2014); past debris_flow_threshold it raises one debris_flow event per basin per
//   storm (signal here and on WorldGround) with the channel it runs down, for the moving flow (mud, PhysX).
class WeatherRunoff3D : public Node3D, public WorldRunoffProvider {
	GDCLASS(WeatherRunoff3D, Node3D);

	float extent = 4096.0f;
	int resolution = 256;
	NodePath height_source;
	uint32_t collision_mask = 1;
	float curve_number = 75.0f;
	float antecedent_time = 48.0f; // h
	float channel_area = 0.5f; // km2 of catchment from which a cell is a channel
	float update_rate = 1.0f; // Hz, real
	bool bake_on_ready = true;

	int baked_n = 0;
	Vector2 origin; // world xz of the map's corner
	float cell = 16.0f;
	LocalVector<float> height; // the surface, m (-inf where none)
	LocalVector<int32_t> receiver; // the cell each drains to, -1 off the map
	LocalVector<int32_t> order; // upstream first
	LocalVector<float> slope; // to the receiver, m/m
	LocalVector<float> area; // contributing area, m2
	LocalVector<float> storage; // m3
	LocalVector<float> event_p; // mm, the storm's rain so far
	LocalVector<float> event_q; // mm, its runoff so far
	LocalVector<float> outflow; // m3/s
	LocalVector<float> inflow; // m3/s, scratch
	LocalVector<Vector2> load; // kg on the cell's water: x mineral, y ash
	LocalVector<Vector2> load_in; // kg arriving this step, scratch
	Vector2 delivered; // kg handed to open water so far
	// Post-fire debris flows.
	bool debris_flows = true;
	float debris_flow_threshold = 0.5f;
	LocalVector<float> rain_now; // mm/h reaching the ground in the last step
	LocalVector<float> fire_burnt; // 0..1 (dNBR / 1000 stand-in), faded by the burn's age
	LocalVector<float> fire_kf; // the soil's erodibility (USLE K factor)
	LocalVector<float> df_fired_at; // weather time of the basin's last event, -inf none
	uint64_t fire_generation = UINT64_MAX;
	const void *fire_seen = nullptr;
	bool fire_any = false;
	double sim_time = 0.0;
	int64_t df_next_id = 1;
	Array df_log; // the latest events (Dictionary), oldest first
	void _fire_refresh();
	void _fire_cells(int p_x0, int p_z0, int p_x1, int p_z1);
	void _debris_flows();
	void _step_debris(double p_dt);
	double accum = 0.0;
	double last_weather_time = -1.0;
	double bake_ms = 0.0;

	static WeatherRunoff3D *active;

	bool _sample_heights();
	void _build_network();
	void _step(double p_dt);
	int _cell_at(const Vector3 &p_position) const;

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	static WeatherRunoff3D *get_active() { return active; }

	void set_extent(float p_extent) { extent = CLAMP(p_extent, 64.0f, 262144.0f); }
	float get_extent() const { return extent; }
	void set_resolution(int p_resolution) { resolution = CLAMP(p_resolution, 8, 2048); }
	int get_resolution() const { return resolution; }
	void set_height_source(const NodePath &p_path) { height_source = p_path; }
	NodePath get_height_source() const { return height_source; }
	void set_collision_mask(uint32_t p_mask) { collision_mask = p_mask; }
	uint32_t get_collision_mask() const { return collision_mask; }
	void set_curve_number(float p_cn) { curve_number = CLAMP(p_cn, 30.0f, 100.0f); }
	float get_curve_number() const { return curve_number; }
	void set_antecedent_time(float p_hours) { antecedent_time = MAX(p_hours, 0.1f); }
	float get_antecedent_time() const { return antecedent_time; }
	void set_channel_area(float p_km2) { channel_area = MAX(p_km2, 0.0f); }
	float get_channel_area() const { return channel_area; }
	void set_update_rate(float p_hz) { update_rate = CLAMP(p_hz, 0.01f, 60.0f); }
	float get_update_rate() const { return update_rate; }
	void set_bake_on_ready(bool p_enabled) { bake_on_ready = p_enabled; }
	bool is_bake_on_ready() const { return bake_on_ready; }

	Error bake();
	bool is_baked() const { return baked_n > 0; }
	double get_bake_time_msec() const { return bake_ms; }
	// Advances the water by p_seconds of weather time at once (implicit, so large steps hold).
	void advance(float p_seconds);
	Rect2 get_map_rect() const { return Rect2(origin, Vector2(cell * baked_n, cell * baked_n)); }

	float get_discharge_at(const Vector3 &p_position) const; // m3/s leaving the cell
	float get_max_discharge_in(const Rect2 &p_rect) const; // m3/s, the largest in the rectangle (the flow through it)
	float get_depth_at(const Vector3 &p_position) const; // mm of water held over the cell
	float get_upstream_area_at(const Vector3 &p_position) const; // m2
	float get_total_storage() const; // m3 on the map
	Vector2 get_load_at(const Vector3 &p_position) const; // kg carried on the cell's water (mineral, ash)
	Vector2 get_total_load() const; // kg on the map's water
	Vector2 get_delivered_load() const { return delivered; } // kg handed to open water so far

	void set_debris_flows(bool p_enabled) { debris_flows = p_enabled; }
	bool is_debris_flows() const { return debris_flows; }
	void set_debris_flow_threshold(float p_p) { debris_flow_threshold = CLAMP(p_p, 0.01f, 0.99f); }
	float get_debris_flow_threshold() const { return debris_flow_threshold; }
	Array get_debris_flows_since(int64_t p_id) const; // the events with an id above p_id
	// The published relations.
	static float debris_flow_likelihood(float p_rain15_mm, float p_burnt_steep_share, float p_burnt, float p_kf); // M1
	static float debris_flow_volume(float p_i15_mm_h, float p_burnt_km2, float p_relief_m); // m3, Gartner 2014
	static float bankfull_width(float p_area_km2); // m

	// WorldRunoffProvider: loose material put on the runoff at a ground point.
	void contract_runoff_add_load(const Vector3 &p_position, float p_mineral_kg, float p_ash_kg) override;

	// The published relations.
	static float scs_runoff(float p_rain_mm, float p_curve_number); // mm
	static float shallow_flow_velocity(float p_slope); // m/s, TR-55 unpaved
	static float channel_velocity(float p_discharge); // m/s, Leopold and Maddock

	WeatherRunoff3D();
	~WeatherRunoff3D();
};

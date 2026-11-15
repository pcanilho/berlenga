/**************************************************************************/
/*  physx_live_water_3d.h                                                 */
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

#include "water_raster_reflections.h"

#include "core/templates/local_vector.h"
#include "scene/3d/node_3d.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"

class MeshInstance3D;
class PhysicsDirectSpaceState3D;

// Live water over any ground (docs/ideas/water.md "Live water on terrain"): a square of shallow water that
// flows, fills hollows and floods, solved with virtual pipes (O'Brien and Hodgins 1995; Mei, Decaudin and Hu
// 2007) on the CPU over the ground's heights (measured by ray casts, so terrain, rocks and buildings all
// count). Water comes from sources, rain and add_water(), and leaves over the open edges and by soaking in.
// Drawn with the river's surface (clear water, shadows, foam where it runs fast).
class PhysXLiveWater3D : public Node3D {
	GDCLASS(PhysXLiveWater3D, Node3D);

	Vector2 size = Vector2(64.0f, 64.0f);
	float cell_size = 0.5f;
	uint32_t ground_collision_mask = 0xFFFFFFFF;
	bool simulating = true;
	bool fill_enabled = false;
	float fill_height = 0.0f; // local y the area starts filled to (a pond, a reservoir behind a dam)
	Rect2 fill_rect; // local xz the fill is limited to; empty = the whole area
	PackedVector4Array sources; // local xyz, w = inflow (m^3/s)
	float source_radius = 1.0f;
	float rain_rate = 0.0f; // mm/h
	bool rain_from_weather = true;
	float infiltration_rate = 0.0f; // mm/h
	bool open_edges = true;
	float friction = 0.4f; // 1/s, on the pipes' flow
	float foam_speed = 2.5f; // m/s where white water starts
	bool raster_reflections = true;

	// Solver state, cell (x, z) at index z * nx + x. Heights are local y.
	int nx = 0;
	int nz = 0;
	LocalVector<float> ground;
	LocalVector<float> depth;
	LocalVector<float> flux; // 4 per cell: +x, -x, +z, -z outflow (m^3/s)
	LocalVector<float> vel; // 2 per cell: x, z (m/s)
	LocalVector<float> foam;
	bool built = false;
	double time_left = 0.0;
	double water_time = 0.0;
	float volume_out = 0.0f; // m^3 lost over the edges since reset (for tests)
	float cost_ms = 0.0f; // the last physics tick's solver and mesh time

	MeshInstance3D *mesh_instance = nullptr;
	Ref<ArrayMesh> mesh;
	Ref<ShaderMaterial> water_material;
	WaterRasterReflections raster_reflection_state;

	void _allocate();
	void _measure_ground(PhysicsDirectSpaceState3D *p_space, bool p_keep_water);
	bool ground_update_pending = false;
	void _step(float p_dt);
	void _update_mesh();
	void _wet_ground();
	LocalVector<float> wet_cells;
	float _rain_mm_per_hour() const;
	bool _cell_of(const Vector3 &p_global, int &r_x, int &r_z, float &r_fx, float &r_fz) const;

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_size(Vector2 p_size);
	Vector2 get_size() const { return size; }
	void set_cell_size(float p_size);
	float get_cell_size() const { return cell_size; }
	void set_ground_collision_mask(uint32_t p_mask);
	uint32_t get_ground_collision_mask() const { return ground_collision_mask; }
	void set_simulating(bool p_enabled) { simulating = p_enabled; }
	bool is_simulating() const { return simulating; }
	void set_fill_enabled(bool p_enabled);
	bool get_fill_enabled() const { return fill_enabled; }
	void set_fill_height(float p_height);
	float get_fill_height() const { return fill_height; }
	void set_fill_rect(const Rect2 &p_rect);
	Rect2 get_fill_rect() const { return fill_rect; }
	void set_sources(const PackedVector4Array &p_sources) { sources = p_sources; }
	PackedVector4Array get_sources() const { return sources; }
	void set_source_radius(float p_radius) { source_radius = MAX(p_radius, 0.01f); }
	float get_source_radius() const { return source_radius; }
	void set_rain_rate(float p_rate) { rain_rate = MAX(p_rate, 0.0f); }
	float get_rain_rate() const { return rain_rate; }
	void set_rain_from_weather(bool p_enabled) { rain_from_weather = p_enabled; }
	bool get_rain_from_weather() const { return rain_from_weather; }
	void set_infiltration_rate(float p_rate) { infiltration_rate = MAX(p_rate, 0.0f); }
	float get_infiltration_rate() const { return infiltration_rate; }
	void set_open_edges(bool p_open) { open_edges = p_open; }
	bool get_open_edges() const { return open_edges; }
	void set_friction(float p_friction) { friction = MAX(p_friction, 0.0f); }
	float get_friction() const { return friction; }
	void set_foam_speed(float p_speed) { foam_speed = MAX(p_speed, 0.1f); }
	float get_foam_speed() const { return foam_speed; }
	void set_raster_reflections(bool p_enabled) { raster_reflections = p_enabled; }
	bool get_raster_reflections() const { return raster_reflections; }

	// Measure the ground again (after it changed) and start over.
	void reset();
	// Measure the ground again at the next physics tick, keeping the water (a dam or a wall removed, a crater blown).
	void update_ground() { ground_update_pending = true; }
	// Pour p_volume m^3 over a disc of p_radius m round p_global (a burst pipe, a bucket, a wave over a wall).
	void add_water(const Vector3 &p_global, float p_volume, float p_radius = 1.0f);
	// Water depth (m) at a world point, 0 where dry or outside.
	float sample_depth(const Vector3 &p_global) const;
	// The water's surface height (world y) at a point, or the ground's where dry; NAN outside.
	float sample_height(const Vector3 &p_global) const;
	// Depth-averaged velocity (world, m/s) at a point.
	Vector3 sample_flow(const Vector3 &p_global) const;
	float get_total_volume() const;
	float get_volume_out() const { return volume_out; }
	float get_cost_ms() const { return cost_ms; }
	Ref<ShaderMaterial> get_water_material() const { return water_material; }

	PhysXLiveWater3D();
	~PhysXLiveWater3D();
};

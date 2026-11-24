/**************************************************************************/
/*  street_water_3d.h                                                     */
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

#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "scene/3d/node_3d.h"
#include "scene/resources/material.h"
#include "scene/resources/texture_rd.h"

// Street water (docs/ideas/street-water.md): shallow water over a road's relief, on the GPU, in a square window of
// cells following the view. Rain lands on the whole road and runs off it as a film into the relief's hollows (the same
// grid carries both, so the film costs nothing extra); sloshes and spills when bodies push through it (tyres, feet: submit_sphere()), soaks in
// and dries, and leaves the road damp where it stood. The road's material draws it from get_map() (see
// get_shader_include()), so raster and the path tracer read the same water.
class StreetWater3D : public Node3D {
	GDCLASS(StreetWater3D, Node3D);

	int resolution = 512;
	float cell = 0.025f; // m
	NodePath follow_path;
	NodePath frame_path;
	float prefill = 0.0f; // share of the relief's ponding cells entering the window start with
	float readback_rate = 8.0f; // Hz
	double last_readback = -1.0;
	// The map read back for get_depth_at().
	mutable Mutex readback_mutex;
	PackedByteArray rb_data;
	int rb_n = 0;
	Vector2i rb_origin;
	float rb_cell = 1.0f;
	void _rt_on_readback(const PackedByteArray &p_data, int p_n, Vector2i p_origin, float p_cell);
	Transform3D _frame_xform() const;
	TypedArray<NodePath> height_sources;
	StringName height_group; // every node in it is a height source too (streets streamed in and out)
	float rain_rate = 0.0f; // mm/h
	bool rain_from_weather = true;
	float soak_rate = 0.0f; // mm/h
	float evaporation_rate = 0.2f; // mm/h
	float drying_time = 1200.0f; // s
	float manning = 0.013f; // s/m^(1/3), asphalt
	float step_rate = 120.0f; // Hz
	int max_steps = 4;
	bool simulating = true;
	TypedArray<Material> materials;

	// Bodies (world centre, radius; the velocity from their last two places) and pours waiting for the next step.
	struct Body {
		Vector3 at;
		float radius = 0.0f;
		Vector3 velocity;
		double time = 0.0;
	};
	HashMap<int64_t, Body> spheres;
	struct Pour {
		Vector3 at;
		float radius = 0.1f;
		float volume = 0.0f;
	};
	LocalVector<Pour> pours;

	// The window: world cell of its corner, the height bed values are kept over.
	bool placed = false;
	Vector2i origin;
	real_t ref_y = 0.0;
	double time_left = 0.0;
	double clock = 0.0; // s the node has run
	float last_rain = 0.0f;
	float cost_ms = 0.0f;

	Ref<Texture2DRD> map;

	// Render thread.
	RID shader;
	RID pipeline;
	RID buf_state;
	RID buf_flux;
	RID buf_items;
	RID buf_load;
	RID tex_map;
	RID uset;
	int rt_n = 0;
	int rt_load_bytes = 0;
	bool rt_failed = false;
	void _rt_frame(int p_n, const PackedByteArray &p_records, const PackedFloat32Array &p_items, const PackedFloat32Array &p_loads, int p_readback, Vector2i p_origin, float p_cell);
	void _rt_free();

	Vector3 _follow_point() const;
	float _bed_at(const Vector3 &p_world, const LocalVector<Node *> &p_sources, float &r_pond, bool &r_inlet) const;
	void _frame(double p_delta);
	void _release();

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	static constexpr int MAX_BODIES = 64;
	static constexpr int MAX_POURS = 64;

	void set_resolution(int p_v);
	int get_resolution() const { return resolution; }
	void set_cell(float p_v);
	float get_cell() const { return cell; }
	void set_follow_path(const NodePath &p_v) { follow_path = p_v; }
	NodePath get_follow_path() const { return follow_path; }
	void set_frame_path(const NodePath &p_v) {
		frame_path = p_v;
		placed = false;
	}
	NodePath get_frame_path() const { return frame_path; }
	void set_prefill(float p_v) { prefill = CLAMP(p_v, 0.0f, 1.0f); }
	float get_prefill() const { return prefill; }
	void set_readback_rate(float p_v) { readback_rate = CLAMP(p_v, 0.0f, 60.0f); }
	float get_readback_rate() const { return readback_rate; }
	// The water's depth (m) at a world point from the last map read back (a frame or two old), 0 where dry or
	// outside the window.
	float get_depth_at(const Vector3 &p_world_position) const;
	void set_height_sources(const TypedArray<NodePath> &p_v);
	TypedArray<NodePath> get_height_sources() const { return height_sources; }
	void set_height_group(const StringName &p_v) { height_group = p_v; }
	StringName get_height_group() const { return height_group; }
	void set_rain_rate(float p_v) { rain_rate = MAX(p_v, 0.0f); }
	float get_rain_rate() const { return rain_rate; }
	void set_rain_from_weather(bool p_v) { rain_from_weather = p_v; }
	bool get_rain_from_weather() const { return rain_from_weather; }
	void set_soak_rate(float p_v) { soak_rate = MAX(p_v, 0.0f); }
	float get_soak_rate() const { return soak_rate; }
	void set_evaporation_rate(float p_v) { evaporation_rate = MAX(p_v, 0.0f); }
	float get_evaporation_rate() const { return evaporation_rate; }
	void set_drying_time(float p_v) { drying_time = MAX(p_v, 1.0f); }
	float get_drying_time() const { return drying_time; }
	void set_manning(float p_v) { manning = CLAMP(p_v, 0.005f, 0.1f); }
	float get_manning() const { return manning; }
	void set_step_rate(float p_v) { step_rate = CLAMP(p_v, 30.0f, 480.0f); }
	float get_step_rate() const { return step_rate; }
	void set_max_steps(int p_v) { max_steps = CLAMP(p_v, 1, 16); }
	int get_max_steps() const { return max_steps; }
	void set_simulating(bool p_v) { simulating = p_v; }
	bool is_simulating() const { return simulating; }
	void set_materials(const TypedArray<Material> &p_v) { materials = p_v; }
	TypedArray<Material> get_materials() const { return materials; }

	// A body pushing the water (a tyre: a sphere of its radius at the hub; a foot), kept until clear_sphere() or
	// moved by calling again.
	void submit_sphere(int64_t p_owner, const Vector3 &p_world_position, float p_radius);
	void clear_sphere(int64_t p_owner);
	// Pour p_volume m3 over a disc round a world point at the next step (a drip, a bucket).
	void add_water(const Vector3 &p_world_position, float p_volume, float p_radius = 0.05f);
	// Start over: measure the beds again, dry.
	void reset();

	Ref<Texture2DRD> get_map() const { return map; }
	// x, z: world position of the window's corner; then its size and cell (m).
	Vector4 get_water_window() const;
	float get_cost_ms() const { return cost_ms; }
	static String get_shader_include();

	StreetWater3D();
	~StreetWater3D();
};

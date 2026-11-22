/**************************************************************************/
/*  physx_haboob_front_3d.h                                                */
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

#include "scene/3d/node_3d.h"

class PhysXFlow3D;
class PhysXFlowEmitter3D;

// The near field of a haboob's front (docs/ideas/sand.md, "Haboob dust walls"): where WeatherMap3D's dust front (its
// dust layer's head share) passes within near_range of the follow target, a row of emitters rides the front's foot in
// a PhysXFlow3D of its own and lifts the dust into the density current's head: the cold outflow pushes forward along
// the ground, the air over the nose is thrown up and rolls back over it in billows (Kelvin-Helmholtz, Simpson 1997),
// so the wall's face boils near the camera while the cloud march draws it far away.
class PhysXHaboobFront3D : public Node3D {
	GDCLASS(PhysXHaboobFront3D, Node3D);

public:
	static constexpr int MAX_EMITTERS = 16;

private:
	bool enabled = true;
	NodePath follow_target; // empty: the viewport camera
	NodePath terrain_path;
	float near_range = 300.0f; // m
	int emitter_count = 10;
	float flow_cell_size = 2.0f;
	Color dust_color = Color(0.72, 0.52, 0.34);
	float scan_interval = 0.5f; // s
	float scan_clock = 0.0f;

	struct FrontPoint {
		Vector3 position; // on the ground
		Vector2 forward; // the front's travel, unit
		float speed = 0.0f; // m/s
		float load = 0.0f; // g/m2 of dust behind it
	};
	LocalVector<FrontPoint> front;
	PhysXFlow3D *flow = nullptr;
	PhysXFlowEmitter3D *emitters[MAX_EMITTERS] = {};

	Node3D *_target() const;
	void _build();
	void _scan();
	void _draw();

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_enabled(bool p_v) { enabled = p_v; }
	bool is_enabled() const { return enabled; }
	void set_follow_target(const NodePath &p_path) { follow_target = p_path; }
	NodePath get_follow_target() const { return follow_target; }
	void set_terrain_path(const NodePath &p_path);
	NodePath get_terrain_path() const { return terrain_path; }
	void set_near_range(float p_v) { near_range = CLAMP(p_v, 50.0f, 2000.0f); }
	float get_near_range() const { return near_range; }
	void set_emitter_count(int p_v) { emitter_count = CLAMP(p_v, 1, MAX_EMITTERS); }
	int get_emitter_count() const { return emitter_count; }
	void set_flow_cell_size(float p_v) { flow_cell_size = CLAMP(p_v, 0.25f, 8.0f); }
	float get_flow_cell_size() const { return flow_cell_size; }
	void set_dust_color(const Color &p_c);
	Color get_dust_color() const { return dust_color; }

	void scan_now() { _scan(); }
	int get_front_point_count() const { return int(front.size()); }
	Vector3 get_front_point(int p_index) const;
	Vector3 get_front_velocity(int p_index) const; // m/s, along the ground
	PhysXFlow3D *get_flow() const { return flow; }
};

/**************************************************************************/
/*  physx_dust_devils_3d.h                                                 */
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

// Dust devils (docs/ideas/sand.md, "Dust devils"): convective vortices that rise over strongly heated, dry ground on
// sunny, light-wind afternoons (Sinclair 1969; Balme and Greeley 2006), a few metres across, spinning at 5 to 15 m/s
// round a rising core, drifting with the wind for a few minutes and lifting the loose dust under them. The node
// spawns them round its follow target at a rate from the weather (WorldContracts: the surface's heating over the
// air, the 10 m wind, rain and wetness) and the ground's loose sand (WorldGround blown_sand_at), and draws each one
// in a PhysXFlow3D of its own: rings of air-only emitters spin the column (a Rankine-like vortex: swirl on the ring,
// a weak inflow at the foot), a warm core lifts it, and the foot ring lifts the dust. Flow carries the rest: the
// column leans and drifts with the weather's wind, the dust swirls up the walls and fans out on top.
class PhysXDustDevils3D : public Node3D {
	GDCLASS(PhysXDustDevils3D, Node3D);

public:
	static constexpr int RING = 8;

private:
	bool enabled = true;
	NodePath follow_target; // empty: the viewport camera
	NodePath terrain_path; // the Flow floor's terrain (PhysXFlow3D terrain_path)
	float spawn_range = 250.0f; // m round the target
	int max_devils = 2;
	float frequency = 10.0f; // devils per km2 per hour at full favour
	float min_heating = 8.0f; // K of surface over air below which none form
	float flow_cell_size = 0.25f;
	Color dust_color = Color(0.8, 0.7, 0.55);
	uint32_t seed = 1;

	struct Devil {
		bool alive = false;
		Vector3 position;
		Vector2 drift;
		float radius = 3.0f;
		float swirl = 10.0f; // m/s on the ring
		float spin = 1.0f; // +1 anticlockwise from above, -1 clockwise
		float age = 0.0f;
		float life = 180.0f;
		float dust = 1.0f;
		PhysXFlowEmitter3D *foot[RING] = {};
		PhysXFlowEmitter3D *upper[RING] = {};
		PhysXFlowEmitter3D *core = nullptr;
	};
	LocalVector<Devil> devils;
	PhysXFlow3D *flow = nullptr;
	uint32_t rng = 1;
	float favour = 0.0f;
	int spawned = 0;

	float _rand();
	Node3D *_target() const;
	float _ground_y(const Vector3 &p_at) const;
	float _dust_at(const Vector3 &p_at) const;
	void _build();
	void _pool();
	void _spawn(const Vector3 &p_at, float p_radius, float p_swirl);
	void _step(float p_dt);
	void _draw(Devil &p_d);

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
	void set_spawn_range(float p_v) { spawn_range = MAX(p_v, 10.0f); }
	float get_spawn_range() const { return spawn_range; }
	void set_max_devils(int p_v);
	int get_max_devils() const { return max_devils; }
	void set_frequency(float p_v) { frequency = MAX(p_v, 0.0f); }
	float get_frequency() const { return frequency; }
	void set_min_heating(float p_v) { min_heating = MAX(p_v, 0.0f); }
	float get_min_heating() const { return min_heating; }
	void set_flow_cell_size(float p_v) { flow_cell_size = CLAMP(p_v, 0.05f, 2.0f); }
	float get_flow_cell_size() const { return flow_cell_size; }
	void set_dust_color(const Color &p_c);
	Color get_dust_color() const { return dust_color; }
	void set_seed(int p_v) {
		seed = uint32_t(p_v);
		rng = seed * 2654435761u + 1u;
	}
	int get_seed() const { return int(seed); }

	float get_favour() const { return favour; }
	float get_favour_at(const Vector3 &p_at) const;
	int spawn(const Vector3 &p_at, float p_radius = -1.0f, float p_swirl = -1.0f);
	int get_devil_count() const;
	Vector4 get_devil(int p_index) const; // (x, z, radius, swirl x strength now); zero past the count
	float get_devil_age(int p_index) const;
	int get_spawned() const { return spawned; }
	PhysXFlow3D *get_flow() const { return flow; }
};

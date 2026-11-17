/**************************************************************************/
/*  physx_ground_dust_3d.h                                                */
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

#include "modules/modules_enabled.gen.h" // For ground.

#ifdef MODULE_GROUND_ENABLED

#include "scene/3d/node_3d.h"

class GroundSurface3D;
class PhysXFlow3D;
class PhysXFlowEmitter3D;

// Near-field drift dust in NVIDIA Flow (docs/ideas/sand.md, "Drifting on sand"). The GroundSurface3D's tyres
// lift dust (add_dust); this node claims the dust lifted within `near_range` of its target and feeds it into a
// PhysXFlow3D of its own, through a pool of sphere emitters that follow the dust sources (every sliding or
// spinning wheel). Flow then carries it: the vehicle body and other physics bodies are solids it curls round,
// the ground wind pushes it (a box emitter over the region that only moves air), it is heavier than the air
// (smoke_buoyancy below 0, so it sinks) and fades as it settles out. It casts a volumetric shadow on
// the ground by its density, and rests on the GroundSurface3D's deformed surface (Flow's terrain floor reads
// its get_height). Farther dust stays with GroundDust3D's grid.
class PhysXGroundDust3D : public Node3D {
	GDCLASS(PhysXGroundDust3D, Node3D);

	NodePath ground_surface; // empty: the active GroundSurface3D
	NodePath follow_target; // empty: the viewport camera
	bool enabled = true;
	float near_range = 40.0f;
	int max_sources = 8;
	float source_radius = 0.35f;
	float smoke_per_kg = 20.0f; // emitter smoke per kg/s lifted
	float dust_heaviness = 3.0f; // the dust's own weight in the air (PhysXFlow3D smoke_buoyancy = -this)
	float wind_coupling = 0.4f;
	float expansion = 1.5f; // the puff swells as it leaves the tyre (emitter divergence, 1/s)
	float turbulence = 2.5f; // m/s of gusting jet: the wake's eddies, so the plume rolls instead of streaming
	uint32_t gust_seed = 1;
	Color dust_color = Color(0.82, 0.72, 0.56);

	PhysXFlow3D *flow = nullptr;
	PhysXFlowEmitter3D *air = nullptr;
	struct Source {
		PhysXFlowEmitter3D *emitter = nullptr;
		Vector3 position;
		Vector3 velocity;
		float rate = 0.0f;
		int idle = 1000;
	};
	LocalVector<Source> sources;

	GroundSurface3D *_ground() const;
	Node3D *_target() const;
	void _build();
	void _step();

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_ground_surface(const NodePath &p_path) { ground_surface = p_path; }
	NodePath get_ground_surface() const { return ground_surface; }
	void set_follow_target(const NodePath &p_path) { follow_target = p_path; }
	NodePath get_follow_target() const { return follow_target; }
	void set_enabled(bool p_v) { enabled = p_v; }
	bool is_enabled() const { return enabled; }
	void set_near_range(float p_v) { near_range = MAX(p_v, 0.0f); }
	float get_near_range() const { return near_range; }
	void set_max_sources(int p_v);
	int get_max_sources() const { return max_sources; }
	void set_source_radius(float p_v) { source_radius = MAX(p_v, 0.05f); }
	float get_source_radius() const { return source_radius; }
	void set_smoke_per_kg(float p_v) { smoke_per_kg = MAX(p_v, 0.0f); }
	float get_smoke_per_kg() const { return smoke_per_kg; }
	void set_dust_heaviness(float p_v);
	float get_dust_heaviness() const { return dust_heaviness; }
	void set_wind_coupling(float p_v) { wind_coupling = MAX(p_v, 0.0f); }
	float get_wind_coupling() const { return wind_coupling; }
	void set_expansion(float p_v) { expansion = MAX(p_v, 0.0f); }
	float get_expansion() const { return expansion; }
	void set_turbulence(float p_v) { turbulence = MAX(p_v, 0.0f); }
	float get_turbulence() const { return turbulence; }
	void set_dust_color(const Color &p_c);
	Color get_dust_color() const { return dust_color; }
	PhysXFlow3D *get_flow() const { return flow; }
	int get_active_sources() const;
};

#endif // MODULE_GROUND_ENABLED

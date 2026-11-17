/**************************************************************************/
/*  physx_rock_dust_3d.h                                                  */
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

#include "modules/modules_enabled.gen.h" // For rocks.

#ifdef MODULE_ROCKS_ENABLED

#include "core/templates/local_vector.h"
#include "scene/3d/node_3d.h"

class RockField3D;
class PhysXFlow3D;
class PhysXFlowEmitter3D;

// Rockfall dust in NVIDIA Flow (docs/ideas/rocks.md, "Rockfall dust with PhysX Flow"). A RockField3D's falling
// and broken stones throw up fines where their pieces hit, where they break and where they slide (take_rockfall_dust);
// this node turns the dust within `near_range` of its target into short bursts through a pool of sphere emitters in a
// PhysXFlow3D of its own. The dust is heavier than the air (smoke_buoyancy below 0), swells out of the impact, runs
// on along the piece's path, rests on the terrain floor (the field's generator), curls round the moving pieces and
// drifts with the weather's wind; it fades over tens of seconds. The RockField3D makes one itself (rockfall_dust).
class PhysXRockDust3D : public Node3D {
	GDCLASS(PhysXRockDust3D, Node3D);

	NodePath rock_field; // empty: the parent, if it is a RockField3D
	NodePath follow_target; // empty: the viewport camera
	bool enabled = true;
	float near_range = 120.0f;
	int max_sources = 8;
	float burst_time = 1.0f; // seconds an impact's dust takes to come out
	float smoke_per_kg = 3.0f; // emitter smoke per kg/s of fines
	float dust_heaviness = 0.3f; // PhysXFlow3D smoke_buoyancy = -this
	float expansion = 18.0f; // the cloud swells out of the impact (emitter divergence, 1/s)
	float carry = 0.3f; // share of the piece's speed the cloud keeps along its path
	float turbulence = 5.0f;
	float wind_coupling = 0.4f;
	uint32_t gust_seed = 7;
	Color dust_color = Color(0.72, 0.69, 0.64);

	PhysXFlow3D *flow = nullptr;
	PhysXFlowEmitter3D *air = nullptr;
	struct Source {
		PhysXFlowEmitter3D *emitter = nullptr;
		Vector3 position;
		Vector3 velocity;
		float rate = 0.0f; // kg/s
		float radius = 0.3f;
		float left = 0.0f; // seconds of burst left
	};
	LocalVector<Source> sources;
	float total = 0.0f; // kg taken into Flow

	RockField3D *_field() const;
	Node3D *_target() const;
	void _build();
	void _step(float p_delta);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_rock_field(const NodePath &p_path) { rock_field = p_path; }
	NodePath get_rock_field() const { return rock_field; }
	void set_follow_target(const NodePath &p_path) { follow_target = p_path; }
	NodePath get_follow_target() const { return follow_target; }
	void set_enabled(bool p_v) { enabled = p_v; }
	bool is_enabled() const { return enabled; }
	void set_near_range(float p_v) { near_range = MAX(p_v, 0.0f); }
	float get_near_range() const { return near_range; }
	void set_max_sources(int p_v);
	int get_max_sources() const { return max_sources; }
	void set_burst_time(float p_v) { burst_time = MAX(p_v, 0.05f); }
	float get_burst_time() const { return burst_time; }
	void set_smoke_per_kg(float p_v) { smoke_per_kg = MAX(p_v, 0.0f); }
	float get_smoke_per_kg() const { return smoke_per_kg; }
	void set_dust_heaviness(float p_v);
	float get_dust_heaviness() const { return dust_heaviness; }
	void set_expansion(float p_v) { expansion = MAX(p_v, 0.0f); }
	float get_expansion() const { return expansion; }
	void set_carry(float p_v) { carry = CLAMP(p_v, 0.0f, 1.0f); }
	float get_carry() const { return carry; }
	void set_turbulence(float p_v) { turbulence = MAX(p_v, 0.0f); }
	float get_turbulence() const { return turbulence; }
	void set_wind_coupling(float p_v) { wind_coupling = MAX(p_v, 0.0f); }
	float get_wind_coupling() const { return wind_coupling; }
	void set_dust_color(const Color &p_c);
	Color get_dust_color() const { return dust_color; }

	PhysXFlow3D *get_flow() const { return flow; }
	int get_active_sources() const;
	float get_total_dust() const { return total; }
};

#endif // MODULE_ROCKS_ENABLED

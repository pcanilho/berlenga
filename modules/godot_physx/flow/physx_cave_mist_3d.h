/**************************************************************************/
/*  physx_cave_mist_3d.h                                                  */
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

#pragma once

#include "modules/modules_enabled.gen.h" // For rocks.

#ifdef MODULE_ROCKS_ENABLED

#include "core/templates/local_vector.h"
#include "scene/3d/node_3d.h"

class CaveNetwork3D;
class PhysXFlow3D;
class PhysXFlowEmitter3D;

// Cold mist at sea-cave mouths in NVIDIA Flow (docs/ideas/rocks.md, "Cave rimstone pools and sea-cave mist"). Each
// swell that runs into a sea cave compresses the air in it and pushes it back out through the mouth, loaded with
// spray and saturated, cooler than the air outside: a pale, heavy breath that sinks over the water and thins as the
// drops evaporate (the "blowing" of sea caves and blowholes). One emitter per sea cave mouth within near_range of
// the target, just inside the mouth over the sea, pulsing with the swell's period (the sea's shore_wave_period when
// the terrain has a sea node): out on each surge, a little drawn back between. The CaveNetwork3D makes one itself
// (sea_mist) when it has sea caves.
class PhysXCaveMist3D : public Node3D {
	GDCLASS(PhysXCaveMist3D, Node3D);

	NodePath cave_network; // empty: the parent, if it is a CaveNetwork3D
	NodePath follow_target; // empty: the viewport camera
	bool enabled = true;
	float near_range = 120.0f;
	int max_mouths = 4;
	float swell_period = 0.0f; // s; 0: the sea's shore_wave_period, else 10
	float mist_rate = 4.0f; // emitter smoke at the height of a surge
	float push_speed = 2.0f; // m/s out of the mouth at the height of a surge
	float heaviness = 0.4f; // PhysXFlow3D smoke_buoyancy = -this (cold, laden air sinks)
	float evaporation = 0.25f; // PhysXFlow3D smoke_fade
	float turbulence = 0.8f;
	Color mist_color = Color(0.86, 0.89, 0.92);
	uint32_t gust_seed = 11;
	double clock = 0.0;

	PhysXFlow3D *flow = nullptr;
	struct Mouth {
		PhysXFlowEmitter3D *emitter = nullptr;
		int cave = -1;
		float phase = 0.0f;
	};
	LocalVector<Mouth> mouths;

	CaveNetwork3D *_network() const;
	Node3D *_target() const;
	float _period() const;
	void _build();
	void _step(float p_delta);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_cave_network(const NodePath &p_path) { cave_network = p_path; }
	NodePath get_cave_network() const { return cave_network; }
	void set_follow_target(const NodePath &p_path) { follow_target = p_path; }
	NodePath get_follow_target() const { return follow_target; }
	void set_enabled(bool p_v) { enabled = p_v; }
	bool is_enabled() const { return enabled; }
	void set_near_range(float p_v) { near_range = MAX(p_v, 0.0f); }
	float get_near_range() const { return near_range; }
	void set_max_mouths(int p_v);
	int get_max_mouths() const { return max_mouths; }
	void set_swell_period(float p_v) { swell_period = MAX(p_v, 0.0f); }
	float get_swell_period() const { return swell_period; }
	void set_mist_rate(float p_v) { mist_rate = MAX(p_v, 0.0f); }
	float get_mist_rate() const { return mist_rate; }
	void set_push_speed(float p_v) { push_speed = MAX(p_v, 0.0f); }
	float get_push_speed() const { return push_speed; }
	void set_heaviness(float p_v);
	float get_heaviness() const { return heaviness; }
	void set_evaporation(float p_v);
	float get_evaporation() const { return evaporation; }
	void set_turbulence(float p_v) { turbulence = MAX(p_v, 0.0f); }
	float get_turbulence() const { return turbulence; }
	void set_mist_color(const Color &p_c);
	Color get_mist_color() const { return mist_color; }

	PhysXFlow3D *get_flow() const { return flow; }
	int get_active_mouths() const;
};

#endif // MODULE_ROCKS_ENABLED

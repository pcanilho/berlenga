/**************************************************************************/
/*  physx_debris_surge_3d.h                                               */
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

#include "core/templates/local_vector.h"
#include "scene/3d/node_3d.h"
#include "scene/resources/multimesh.h"

class MultiMeshInstance3D;
class RigidBody3D;

// The bouldery front of a post-fire debris flow (docs/ideas/physx.md, "Post-fire mudflows"): the weather's runoff
// fires an event (WeatherRunoff3D's debris_flow signal: an outlet, a volume and the channel path downhill); the
// mud body itself is the mud system's (GroundSurface3D). This draws what rides its front: boulders and cobbles
// gathered at the snout (Iverson 1997), carried by the flow's drag and buoyed by the dense slurry, then dropped where
// the front stops; mud thrown up as spray; a train of surges (Kean et al. 2011). The front runs down the path at
// Rickenmann's (1999) speed v = 2.1 Q^(1/3) S^(1/3) with peak discharge Q = 0.1 V^(5/6), or follows the mud
// system's front where one reports it.
class PhysXDebrisSurge3D : public Node3D {
	GDCLASS(PhysXDebrisSurge3D, Node3D);

	struct Boulder {
		ObjectID body;
		int surge = 0;
		float radius = 0.3f;
		int seg = 0; // nearest path segment, kept as it moves
	};
	struct Drop {
		Vector3 p;
		Vector3 v;
		float age = 0.0f;
		float life = 1.0f;
		float size = 0.05f;
	};
	struct Surge {
		int event_id = -1;
		PackedVector3Array path;
		PackedFloat32Array widths;
		LocalVector<float> along; // m from the outlet at each point
		float volume = 0.0f; // m3 in this surge
		float s = 0.0f; // m, the front's distance down the path
		float speed = 0.0f; // m/s
		float depth = 0.0f; // m
		float delay = 0.0f; // s before it starts
		float stopped_time = -1.0f; // s since it stopped
		bool done = false;
	};
	LocalVector<Surge> surges;
	LocalVector<Boulder> boulders;
	LocalVector<Drop> drops;
	float spray_carry = 0.0f;
	int last_event = -1;
	uint32_t rng = 1u;
	bool connected = false;
	MultiMeshInstance3D *spray_mmi = nullptr;
	Ref<MultiMesh> spray_mm;
	Ref<Mesh> rock_mesh;
	Ref<Material> rock_material;

	NodePath front_source; // an object with get_debris_front(event_id) -> Dictionary (the mud system)
	bool listen = true;
	int surge_count = 4; // the most surges per event
	float surge_interval = 60.0f; // s
	float boulder_density = 2650.0f; // kg/m3
	float slurry_density = 1900.0f; // kg/m3
	float max_boulder = 1.2f; // m across
	int max_boulders = 60;
	float boulder_rate = 0.08f; // boulders per m3 of surge (the coarse fraction gathered at the snout)
	float stop_slope = 0.07f; // the front stops on slopes below this (Ikeya 1981; fans at 3 to 6 degrees)
	float spray = 1.0f;
	Color mud_color = Color(0.13f, 0.1f, 0.08f);

	float _rand();
	void _connect();
	void _on_debris_flow(const Dictionary &p_event);
	void _point_at(const Surge &p_surge, float p_s, Vector3 &r_pos, Vector3 &r_dir, float &r_width, float &r_slope) const;
	void _spawn_boulders(int p_surge);
	void _step_surge(Surge &p_surge, float p_delta);
	void _push_boulders(float p_delta);
	void _step_spray(float p_delta);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	// Starts the surges of one event (the keys of WeatherRunoff3D's debris_flow: id, position, volume, path, widths).
	void start(const Dictionary &p_event);
	void clear();
	int get_surge_count() const { return surges.size(); }
	int get_active_surges() const;
	int get_boulder_count() const { return boulders.size(); }
	int get_spray_count() const { return drops.size(); }
	Vector3 get_front_position(int p_surge) const;
	float get_front_speed(int p_surge) const;
	float get_front_depth(int p_surge) const;
	float get_peak_discharge(float p_volume) const { return 0.1f * Math::pow(MAX(p_volume, 0.0f), 5.0f / 6.0f); }

	void set_front_source(const NodePath &p_path) { front_source = p_path; }
	NodePath get_front_source() const { return front_source; }
	void set_listen(bool p_v);
	bool is_listening() const { return listen; }
	void set_surge_count(int p_v) { surge_count = CLAMP(p_v, 1, 16); }
	int get_surge_count_max() const { return surge_count; }
	void set_surge_interval(float p_v) { surge_interval = MAX(p_v, 1.0f); }
	float get_surge_interval() const { return surge_interval; }
	void set_boulder_density(float p_v) { boulder_density = MAX(p_v, 500.0f); }
	float get_boulder_density() const { return boulder_density; }
	void set_slurry_density(float p_v) { slurry_density = MAX(p_v, 1000.0f); }
	float get_slurry_density() const { return slurry_density; }
	void set_max_boulder(float p_v) { max_boulder = MAX(p_v, 0.05f); }
	float get_max_boulder() const { return max_boulder; }
	void set_max_boulders(int p_v) { max_boulders = CLAMP(p_v, 0, 500); }
	int get_max_boulders() const { return max_boulders; }
	void set_boulder_rate(float p_v) { boulder_rate = MAX(p_v, 0.0f); }
	float get_boulder_rate() const { return boulder_rate; }
	void set_stop_slope(float p_v) { stop_slope = MAX(p_v, 0.0f); }
	float get_stop_slope() const { return stop_slope; }
	void set_spray(float p_v) { spray = MAX(p_v, 0.0f); }
	float get_spray() const { return spray; }
	void set_mud_color(const Color &p_c) { mud_color = p_c; }
	Color get_mud_color() const { return mud_color; }

	PhysXDebrisSurge3D();
};

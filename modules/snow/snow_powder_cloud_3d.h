/**************************************************************************/
/*  snow_powder_cloud_3d.h                                                */
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

#include "core/templates/local_vector.h"
#include "scene/3d/node_3d.h"

// The powder cloud's look (docs/ideas/snow.md, follow-up 23): a fast dry avalanche's front blows snow into the air
// (SnowAvalanche3D works out where and how much, and what settles back); this draws it with NVIDIA Flow as a heavy
// gas through a pool of sphere emitters on the front, in a PhysXFlow3D of its own that rests on the terrain and
// drifts with the weather's wind. It talks to the PhysX module by class and property names only, so the snow module
// builds and runs without it: then there is no cloud to see, and everything else is the same.
class SnowPowderCloud3D : public Node3D {
	GDCLASS(SnowPowderCloud3D, Node3D);

public:
	struct Source {
		Vector3 position;
		Vector3 velocity;
		float rate = 0.0f; // kg/s into the air
		float radius = 2.0f; // m
	};

private:
	bool enabled = true;
	int max_sources = 12;
	float smoke_per_kg = 0.05f; // emitter smoke per kg/s
	float heaviness = 0.8f; // Flow smoke_buoyancy = -this (3-10 kg/m3 of suspended snow against air's 1.2)
	float updraft = 0.3f; // share of the front's speed blown up out of it (the eruption current)
	Color color = Color(0.93, 0.95, 0.98);
	NodePath terrain_path;

	Node3D *flow = nullptr; // a PhysXFlow3D, when the PhysX module has Flow
	LocalVector<Node3D *> emitters;
	bool tried = false;
	int active = 0;

	void _build();

protected:
	static void _bind_methods();

public:
	// Called by the avalanche every physics frame with the front's sources (empty: the cloud just drifts and fades).
	void feed(const LocalVector<Source> &p_sources);
	bool has_flow() const { return flow != nullptr; }
	Node3D *get_flow() const { return flow; }
	int get_active_sources() const { return active; }
	void set_terrain_path(const NodePath &p_path);
	NodePath get_terrain_path() const { return terrain_path; }

	void set_enabled(bool p_enable) { enabled = p_enable; }
	bool is_enabled() const { return enabled; }
	void set_max_sources(int p_count) { max_sources = CLAMP(p_count, 1, 32); }
	int get_max_sources() const { return max_sources; }
	void set_smoke_per_kg(float p_value) { smoke_per_kg = MAX(p_value, 0.0f); }
	float get_smoke_per_kg() const { return smoke_per_kg; }
	void set_heaviness(float p_value);
	float get_heaviness() const { return heaviness; }
	void set_updraft(float p_value) { updraft = MAX(p_value, 0.0f); }
	float get_updraft() const { return updraft; }
	void set_color(const Color &p_color);
	Color get_color() const { return color; }
};

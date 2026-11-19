/**************************************************************************/
/*  snow_bodies_3d.h                                                      */
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

#include "core/templates/hash_map.h"
#include "scene/3d/node_3d.h"

class GeometryInstance3D;
class SnowCover3D;

// Snow that moving things carry (docs/ideas/snow.md, follow-up 18). Every rigid body, vehicle and cloth (and any
// node in the snow_catch group; snow_ignore opts out) keeps its own snow load: falling snow builds it where the sky
// is open above, the cover's depth seeds it on first sight, wind over the blowing threshold (Li and Pomeroy 1997,
// raised for wet and settled snow) strips it, hard shakes and jolts throw it off, warm air, rain and fire melt it.
// The load reaches the body's materials through BaseMaterial3D snow_on_body (instance uniform slot 12), the same
// in raster and path tracing. SnowCover3D adds one by itself while bodies_enabled.
class SnowBodies3D : public Node3D {
	GDCLASS(SnowBodies3D, Node3D);

	struct Body {
		ObjectID id;
		float depth = 0.0f; // m
		float density = 100.0f; // kg/m3
		float wet = 0.0f; // 0..1
		float albedo = 0.85f;
		float exposure = 1.0f; // 1 where the sky is open above
		Vector3 last_position;
		Vector3 last_velocity;
		bool seen = false;
		double ray_accum = 1.0e9;
		double geometry_accum = 1.0e9;
		Vector<ObjectID> geometry;
		Vector4 sent = Vector4(-1.0f, -1.0f, -1.0f, -1.0f);
	};

	HashMap<ObjectID, Body> bodies;
	float update_rate = 10.0f; // Hz
	float shake_threshold = 0.5f; // g
	float shake_shed_rate = 3.0f; // per second per g over the threshold
	float wind_shed_rate = 0.08f; // per second per m/s over the threshold
	float max_depth = 0.4f; // m
	bool auto_materials = true;
	bool auto_track = true;
	double accum = 0.0;

	void _consider(Node *p_node);
	void _scan(Node *p_node);
	void _on_node_added(Node *p_node);
	void _collect_geometry(Body &r_body, Node *p_node);
	void _prepare_geometry(GeometryInstance3D *p_geometry);
	void _step(double p_dt);
	float _exposure(Node3D *p_node);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	static bool is_carrier(const Node *p_node);

	void track(Node *p_node);
	void untrack(Node *p_node);
	bool is_tracked(Node *p_node) const;
	int get_tracked_count() const { return bodies.size(); }
	float get_body_depth(Node *p_node) const; // m
	float get_body_density(Node *p_node) const; // kg/m3
	float get_body_wetness(Node *p_node) const; // 0..1
	void set_body_depth(Node *p_node, float p_depth);
	void step(double p_dt); // one update now, for tests

	void set_update_rate(float p_rate);
	float get_update_rate() const { return update_rate; }
	void set_shake_threshold(float p_g);
	float get_shake_threshold() const { return shake_threshold; }
	void set_shake_shed_rate(float p_rate);
	float get_shake_shed_rate() const { return shake_shed_rate; }
	void set_wind_shed_rate(float p_rate);
	float get_wind_shed_rate() const { return wind_shed_rate; }
	void set_max_depth(float p_depth);
	float get_max_depth() const { return max_depth; }
	void set_auto_materials(bool p_enable) { auto_materials = p_enable; }
	bool is_auto_materials() const { return auto_materials; }
	void set_auto_track(bool p_enable) { auto_track = p_enable; }
	bool is_auto_track() const { return auto_track; }
};

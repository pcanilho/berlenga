/**************************************************************************/
/*  snow_cornice_3d.h                                                     */
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
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"

class MeshInstance3D;
class StaticBody3D;
class CollisionShape3D;

// Cornices (docs/ideas/snow.md, stage 4): the overhanging slabs the wind builds on lee crests. The world tier
// of SnowCover3D grows a length per cell along the wind that built it; this node draws them near the camera
// as a swept profile along each run of crest cells, with collision. The profile follows field and wind-tunnel
// cornices (Naito and Kobayashi 1986; Yu et al. 2023): a thin slab growing out from the crest top, thicker at
// the root, the tip sagging as it grows.
class SnowCornice3D : public Node3D {
	GDCLASS(SnowCornice3D, Node3D);

	float radius = 800.0f;
	float min_length = 0.5f;
	float rebuild_distance = 64.0f;
	bool collision_enabled = true;
	Ref<Material> material;
	NodePath follow_target;

	MeshInstance3D *mesh_instance = nullptr;
	StaticBody3D *body = nullptr;
	CollisionShape3D *shape = nullptr;
	Ref<StandardMaterial3D> default_material;
	Vector3 built_at;
	bool built = false;
	uint64_t built_update = 0;
	double check_accum = 0.0;
	int segment_count = 0;
	// Falling blocks of a failed cornice (follow-up 6).
	bool debris_enabled = true;
	float debris_lifetime = 30.0f;
	struct Debris {
		ObjectID node;
		double age = 0.0;
	};
	Vector<Debris> debris;
	ObjectID connected_cover;

	Vector3 _follow_position() const;
	void _ensure_children();
	void _connect_cover();
	void _on_cornice_failed(Vector3 p_position, Vector3 p_direction, float p_length, float p_mass);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void rebuild();
	int get_segment_count() const { return segment_count; }
	int get_debris_count() const { return debris.size(); }
	void set_debris_enabled(bool p_enabled);
	bool is_debris_enabled() const { return debris_enabled; }
	void set_debris_lifetime(float p_seconds);
	float get_debris_lifetime() const { return debris_lifetime; }

	void set_radius(float p_radius);
	float get_radius() const { return radius; }
	void set_min_length(float p_length);
	float get_min_length() const { return min_length; }
	void set_rebuild_distance(float p_distance);
	float get_rebuild_distance() const { return rebuild_distance; }
	void set_collision_enabled(bool p_enabled);
	bool is_collision_enabled() const { return collision_enabled; }
	void set_material(const Ref<Material> &p_material);
	Ref<Material> get_material() const { return material; }
	void set_follow_target(const NodePath &p_path);
	NodePath get_follow_target() const { return follow_target; }
};

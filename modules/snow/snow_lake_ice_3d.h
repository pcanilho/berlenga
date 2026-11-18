/**************************************************************************/
/*  snow_lake_ice_3d.h                                                    */
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

class MeshInstance3D;
class StaticBody3D;
class CollisionShape3D;
class SnowCover3D;

// Lake ice (docs/ideas/snow.md, follow-up 15): draws the ice SnowCover3D grows on open water, near square and
// world tier, as a sheet at the water level raised by its freeboard, with collision where it bears a walker.
// Clear (black) ice is dark, the water showing through it; snow ice from flooded snow is white (Bolsenga 1969:
// albedo about 0.1 and 0.46). Snow on the ice is the cover's, through the shared snow functions. SnowCover3D
// adds one by itself while lake_ice_enabled.
class SnowLakeIce3D : public Node3D {
	GDCLASS(SnowLakeIce3D, Node3D);

	float rebuild_interval = 2.0f;
	float collision_radius = 160.0f;
	bool collision_enabled = true;
	float crack_strength = 1.0f;
	Ref<Material> material;

	MeshInstance3D *mesh_instance = nullptr;
	StaticBody3D *body = nullptr;
	CollisionShape3D *shape = nullptr;
	Ref<ShaderMaterial> default_material;
	double accum = 0.0;
	uint64_t built_update = UINT64_MAX;
	uint64_t built_world_update = UINT64_MAX;
	uint32_t collision_key = 0;
	int quad_count = 0;
	int walkable_count = 0;

	void _ensure_children();
	static Ref<Shader> _shader();

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	static float freeboard(float p_ice);

	void rebuild();
	int get_quad_count() const { return quad_count; }
	int get_walkable_count() const { return walkable_count; }

	void set_rebuild_interval(float p_seconds);
	float get_rebuild_interval() const { return rebuild_interval; }
	void set_collision_radius(float p_radius);
	float get_collision_radius() const { return collision_radius; }
	void set_collision_enabled(bool p_enabled);
	bool is_collision_enabled() const { return collision_enabled; }
	void set_crack_strength(float p_strength);
	float get_crack_strength() const { return crack_strength; }
	void set_material(const Ref<Material> &p_material);
	Ref<Material> get_material() const { return material; }
};

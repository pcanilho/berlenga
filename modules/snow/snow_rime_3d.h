/**************************************************************************/
/*  snow_rime_3d.h                                                       */
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

#include "scene/3d/node_3d.h"
#include "scene/resources/material.h"

class MeshInstance3D;

// Rime (docs/ideas/snow.md, follow-up 40): supercooled fog droplets freezing on contact with what stands in the
// wind. The accreted mass per projected area is collision efficiency x liquid water content x wind speed x time
// (Makkonen 2000), the efficiency from the droplets' Stokes number on the collector (Finstad et al. 1988), the
// deposit's density from Macklin's (1962) parameter; it builds into the wind, thickest on thin parts and sharp
// edges (smaller collectors catch more), as a crust bristling with feathers pointing upwind.
class SnowRime3D : public Node3D {
	GDCLASS(SnowRime3D, Node3D);

	struct Target {
		ObjectID node;
		float diameter = 0.1f; // m, the collector
		Vector3 centre; // world, where the weather is sampled
		LocalVector<Vector3> points; // local to this node, welded
		LocalVector<Vector3> normals; // per welded point
		LocalVector<float> sharpness; // 0 flat .. 1 a sharp edge or corner
		LocalVector<float> thickness; // m
		LocalVector<Vector3> growth; // the way the deposit grew (local), weighted by thickness
		LocalVector<int> triangles; // welded point indices, three per triangle
	};
	LocalVector<Target> targets;
	TypedArray<NodePath> target_paths;
	String target_group = "rime";
	float droplet_diameter = 20.0f; // um, median volume
	float fog_water = -1.0f; // g/m3; < 0: the weather's
	float feather_spacing = 0.012f; // m
	int max_feathers = 60000;
	float max_thickness = 0.25f; // m
	int seed = 0;
	bool targets_dirty = true;
	MeshInstance3D *mesh_instance = nullptr;
	Ref<Material> material;
	Ref<StandardMaterial3D> default_material;
	float drawn_change = 0.0f;
	static Ref<Texture2D> _feather_texture();
	uint64_t seen_update = 0;
	float last_rate = 0.0f; // m/h on the most exposed point, last step
	float last_density = 0.0f;

	void _ensure_children();
	void _collect_targets();
	void _add_target(Node *p_node);
	void _build_mesh();

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	// Finstad et al. (1988): the share of the droplets in the swept volume that hit a cylinder of p_diameter (m) at
	// p_wind (m/s), for droplets of p_droplet_um median volume diameter.
	static float collision_efficiency(float p_wind, float p_droplet_um, float p_diameter);
	// Macklin (1962): rime density (kg/m3) from R = -d v / (2 Ts), d in um, v the impact speed, Ts the surface's C.
	static float rime_density(float p_droplet_um, float p_impact_speed, float p_surface_celsius);
	// Rime thickness growth (m/h) on a surface facing the wind, for a collector of p_diameter.
	static float accretion_rate(float p_wind, float p_fog_water, float p_celsius, float p_droplet_um, float p_diameter);

	void advance(float p_hours);
	// Grows (p_celsius < 0) or melts (> 0) for p_hours in a wind (local to the world, m/s) through fog of
	// p_fog_water g/m3.
	void grow(float p_hours, float p_celsius, const Vector3 &p_wind, float p_fog_water);
	void clear();
	void refresh_targets();

	int get_target_count() const { return targets.size(); }
	float get_target_thickness(int p_index) const; // m, the thickest point
	float get_target_diameter(int p_index) const { return p_index >= 0 && p_index < (int)targets.size() ? targets[p_index].diameter : 0.0f; }
	float get_thickness_at(const Vector3 &p_position) const; // m, the nearest rimed point within 0.5 m
	Vector3 get_growth_direction(int p_index) const; // world, the thickest point's
	int get_feather_count() const;
	float get_last_rate() const { return last_rate; }
	float get_last_density() const { return last_density; }

	void set_target_paths(const TypedArray<NodePath> &p_paths);
	TypedArray<NodePath> get_target_paths() const { return target_paths; }
	void set_target_group(const String &p_group);
	String get_target_group() const { return target_group; }
	void set_droplet_diameter(float p_um) { droplet_diameter = MAX(p_um, 1.0f); }
	float get_droplet_diameter() const { return droplet_diameter; }
	void set_fog_water(float p_value) { fog_water = p_value; }
	float get_fog_water() const { return fog_water; }
	void set_feather_spacing(float p_m);
	float get_feather_spacing() const { return feather_spacing; }
	void set_max_feathers(int p_count);
	int get_max_feathers() const { return max_feathers; }
	void set_max_thickness(float p_m) { max_thickness = MAX(p_m, 0.001f); }
	float get_max_thickness() const { return max_thickness; }
	void set_seed(int p_seed);
	int get_seed() const { return seed; }
	void set_material(const Ref<Material> &p_material);
	Ref<Material> get_material() const { return material; }
};

/**************************************************************************/
/*  snow_icicles_3d.h                                                     */
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

// Icicles (docs/ideas/snow.md, follow-up 6): where melt water from snow above runs off an edge into air below
// freezing. Each icicle grows by the heat its tip and wall lose to the air (Makkonen 1988; Szilder and Lozowski
// 1994): the tip lengthens as a tube of spongy ice around the pendant drop while water reaches it, the wall
// thickens with the film running down it, and once the wall freezes all the water the icicle only thickens.
class SnowIcicles3D : public Node3D {
	GDCLASS(SnowIcicles3D, Node3D);

	PackedVector3Array edge;
	float spacing = 0.15f;
	float catchment_length = 3.0f;
	float heat_loss = 0.0f;
	float max_length = 1.5f;
	int seed = 0;
	Ref<Material> material;

	struct Icicle {
		Vector3 position; // local, the root
		float share = 1.0f; // of the water along the edge
		float length = 0.0f; // m
		float radius = 0.0f; // m, at the root
	};
	Vector<Icicle> icicles;
	bool sites_dirty = true;
	MeshInstance3D *mesh_instance = nullptr;
	Ref<StandardMaterial3D> default_material;
	Ref<StandardMaterial3D> default_core_material;
	Ref<Material> core_material;
	bool hazy_core = true;
	uint64_t seen_update = 0;
	float drawn_change = 0.0f;
	float last_supply = 0.0f; // kg/h along the whole edge, last step
	float last_dripped = 0.0f; // kg/h

	void _place();
	void _ensure_children();
	void _build_mesh();

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	// Grows (or melts) the icicles over p_hours with the air at p_celsius and p_humidity, wind p_wind m/s and
	// p_supply kg/h of water reaching each metre of edge.
	void grow(float p_hours, float p_celsius, float p_wind, float p_supply, float p_humidity = 0.8f);
	// The same with the air, wind and water at the edge now: SnowCover3D's runoff over catchment_length plus the
	// roof's heat_loss melting the snow from below, its air temperature, WeatherMap3D's wind.
	void advance(float p_hours);
	void clear();

	int get_icicle_count() const { return icicles.size(); }
	float get_icicle_length(int p_index) const;
	float get_icicle_radius(int p_index) const;
	float get_longest() const;
	float get_ice_mass() const; // kg
	float get_last_supply() const { return last_supply; }
	float get_last_dripped() const { return last_dripped; }

	// The published relations (m/h): the tip's lengthening and the wall's thickening, at full water supply.
	static float tip_growth_rate(float p_celsius, float p_wind, float p_humidity = 0.8f);
	static float wall_growth_rate(float p_celsius, float p_wind, float p_radius, float p_length, float p_humidity = 0.8f);

	void set_edge(const PackedVector3Array &p_edge);
	PackedVector3Array get_edge() const { return edge; }
	void set_spacing(float p_spacing);
	float get_spacing() const { return spacing; }
	void set_catchment_length(float p_length);
	float get_catchment_length() const { return catchment_length; }
	void set_heat_loss(float p_watts);
	float get_heat_loss() const { return heat_loss; }
	void set_max_length(float p_length);
	float get_max_length() const { return max_length; }
	void set_seed(int p_seed);
	int get_seed() const { return seed; }
	void set_material(const Ref<Material> &p_material);
	Ref<Material> get_material() const { return material; }
	void set_core_material(const Ref<Material> &p_material);
	Ref<Material> get_core_material() const { return core_material; }
	void set_hazy_core(bool p_enabled);
	bool get_hazy_core() const { return hazy_core; }
};

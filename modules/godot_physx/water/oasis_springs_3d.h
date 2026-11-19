/**************************************************************************/
/*  oasis_springs_3d.h                                                    */
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

#include "modules/world/world_season.h"

class ArrayMesh;
class MeshInstance3D;
class PhysXWaterSurface3D;
class ShaderMaterial;

// Oasis spring pools: still water where a confined aquifer reaches the surface (Siwa's "ain", the Great Artesian
// Basin's springs). Each spring gets a PhysXWaterSurface3D sized by its water balance at steady state: the pool
// grows until evaporation and seepage take what the spring gives, A = Q / (E + I). E is open-water evaporation,
// from the climate (Hargreaves & Samani 1985 reference evapotranspiration, extraterrestrial radiation per FAO-56,
// open water Kc 1.05) or set by hand; I is seepage into the bed. The outline is a disc broken by a few
// low harmonics with per-spring random phases (no two pools alike), scaled to hold exactly that area, with the
// spring's vent set off its centre. At each vent a sand boil: the upwelling fluidises the bed sand into a small
// crater whose floor heaves and settles, and the surface over it lifts in a faint dome that sheds rings.
class OasisSprings3D : public Node3D {
	GDCLASS(OasisSprings3D, Node3D);

public:
	struct Spring {
		Vector3 position; // world: x, z the vent; y the pool's still-water level
		float discharge = 0.0f; // L/s
		float area = 0.0f; // m2
		float radius = 0.0f; // m, of a disc of that area
		Vector3 pool_centre; // world
		PackedVector2Array outline; // world xz, at even angles round pool_centre
		uint32_t seed = 0;
		PhysXWaterSurface3D *pool = nullptr;
		LocalVector<MeshInstance3D *> boils;
		LocalVector<Vector3> vents; // world, on the bed
		LocalVector<float> vent_phase;
		LocalVector<float> vent_scale; // of boil_radius: no two boils the same size
	};

private:
	PackedVector4Array springs; // x, still-water level y, z, w discharge L/s (0 = default_discharge)
	NodePath spring_source;
	float default_discharge = 0.05f; // L/s
	float evaporation = 0.0f; // m a year; 0 = from the climate
	float diurnal_range = 14.0f; // K, for the climate estimate (desert days)
	float seepage = 1.0f; // m a year
	float min_radius = 2.0f;
	float max_radius = 40.0f;
	float outline_noise = 0.22f; // relative radius swing
	float vent_offset = 0.3f; // of the radius
	float fallback_depth = 2.0f; // m, where no seabed is measured
	bool seabed_from_floor = true;
	uint32_t seabed_collision_mask = 1;
	bool sand_boils = true;
	float boil_radius = 0.45f; // m
	float boil_strength = 1.0f;
	Color sand_color = Color(0.78f, 0.69f, 0.52f);
	int seed = 0;

	LocalVector<Spring> built;
	bool dirty = true;
	bool vents_placed = false;
	double clock = 0.0;
	Ref<ShaderMaterial> boil_material;

	void _rebuild();
	void _clear();
	void _place_vents();
	void _pulse(double p_delta);
	PackedVector4Array _read_springs() const;
	Ref<ArrayMesh> _outline_mesh(Spring &r_spring) const;
	Ref<ShaderMaterial> _boil_shader_material();

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	// Pool area (m2) a spring of p_discharge L/s holds against p_evaporation + p_seepage (m a year).
	static float pool_area(float p_discharge, float p_evaporation, float p_seepage);
	// Open-water evaporation (m a year) over a year of the climate at height p_height (Hargreaves).
	static float climate_evaporation(const WeatherSeason::Climate &p_climate, float p_height, float p_diurnal_range);
	// The evaporation in use at a height: the set value, else the weather's climate, else 2.5 m.
	float evaporation_at(float p_height) const;

	void set_springs(const PackedVector4Array &p_springs);
	PackedVector4Array get_springs() const { return springs; }
	void set_spring_source(const NodePath &p_path);
	NodePath get_spring_source() const { return spring_source; }
	void set_default_discharge(float p_lps);
	float get_default_discharge() const { return default_discharge; }
	void set_evaporation(float p_m_yr);
	float get_evaporation() const { return evaporation; }
	void set_diurnal_range(float p_k);
	float get_diurnal_range() const { return diurnal_range; }
	void set_seepage(float p_m_yr);
	float get_seepage() const { return seepage; }
	void set_min_radius(float p_m);
	float get_min_radius() const { return min_radius; }
	void set_max_radius(float p_m);
	float get_max_radius() const { return max_radius; }
	void set_outline_noise(float p_amount);
	float get_outline_noise() const { return outline_noise; }
	void set_vent_offset(float p_share);
	float get_vent_offset() const { return vent_offset; }
	void set_fallback_depth(float p_m);
	float get_fallback_depth() const { return fallback_depth; }
	void set_seabed_from_floor(bool p_enabled);
	bool get_seabed_from_floor() const { return seabed_from_floor; }
	void set_seabed_collision_mask(uint32_t p_mask);
	uint32_t get_seabed_collision_mask() const { return seabed_collision_mask; }
	void set_sand_boils(bool p_enabled);
	bool get_sand_boils() const { return sand_boils; }
	void set_boil_radius(float p_m);
	float get_boil_radius() const { return boil_radius; }
	void set_boil_strength(float p_strength);
	float get_boil_strength() const { return boil_strength; }
	void set_sand_color(const Color &p_color);
	Color get_sand_color() const { return sand_color; }
	void set_seed(int p_seed);
	int get_seed() const { return seed; }

	// Builds the pools again from the springs (after the source changed).
	void rebuild();
	int get_pool_count() const { return (int)built.size(); }
	PhysXWaterSurface3D *get_pool(int p_index) const;
	float get_pool_area(int p_index) const;
	float get_pool_radius(int p_index) const;
	float get_pool_discharge(int p_index) const;
	PackedVector3Array get_vents(int p_index) const;
	// The pool's outline (world xz): the shore the ground's hollow should meet (terrain carves to it).
	PackedVector2Array get_outline(int p_index) const;
};

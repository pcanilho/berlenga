/**************************************************************************/
/*  physx_river_3d.h                                                      */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
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

#include "scene/3d/path_3d.h"
#include "scene/resources/material.h"

class MeshInstance3D;

// A river drawn as a path. Each curve point carries a width and optionally a
// depth; the river carries one discharge (m^3/s). The surface never runs
// uphill (a running minimum downstream, as Horizon's and Peytavie et al.'s
// river tools do); where it drops faster than max_slope it steps as a
// cascade. Depth where none is given is Manning's normal depth for the
// discharge, width, slope and roughness; speed follows by continuity, and
// across the channel each strip carries flow as depth^(5/3) (the divided
// channel method), so the middle runs fastest and the banks are still. White
// water rises with the Froude number and at the foot of every cascade and is
// carried downstream. The surface is a mesh of its own (the path tracer sees
// it as a water medium), and sample_height() / sample_flow() give buoyancy
// the same river.
class PhysXRiver3D : public Path3D {
	GDCLASS(PhysXRiver3D, Path3D);

public:
	struct Section {
		Vector3 centre; // local, at the surface
		Vector3 side; // local, horizontal unit vector to the left bank
		float s = 0.0f; // arc length, m
		float width = 0.0f;
		float surface = 0.0f; // local y
		float slope = 0.0f; // surface slope used for Manning (smoothed)
		float speed = 0.0f; // mean speed, m/s
		float foam = 0.0f; // 0..1 white water on the centre line
		bool cascade = false;
		float curvature = 0.0f; // 1/m, positive turning towards +side
		float carve_bed = 0.0f; // local y of the carved channel's floor
		float carve_top = 0.0f; // local y of the uncarved ground at the centre
		// Across: half_extent / cross_segments spacing, from -half to +half.
		LocalVector<float> depth; // water depth per cross sample (<= 0 dry)
		LocalVector<float> speed_at; // along-stream speed per cross sample
		LocalVector<float> across_at; // speed towards the left bank per cross sample
		LocalVector<float> foam_at; // white water per cross sample
	};

private:
	PackedFloat32Array widths;
	PackedFloat32Array depths;
	float default_width = 4.0f;
	float discharge = 2.0f; // m^3/s
	float roughness = 0.035f; // Manning n
	float max_slope = 0.08f; // steeper drops are cascades
	bool snap_to_ground = true;
	uint32_t ground_collision_mask = 1;
	uint32_t obstacle_collision_mask = 0; // rocks measured on top of the (base) ground
	float step_length = 1.0f; // m between cross-sections
	int cross_segments = 12;
	float bank_margin = 1.0f; // m of ribbon past each bank, for the waterline fade
	float foam_persistence = 4.0f; // s
	Ref<ShaderMaterial> water_material;
	bool carve = false;
	float carve_depth = 1.0f; // m below the uncarved ground at the centre
	float carve_bank_slope = 0.5f; // m of rise per m outside the banks
	float carve_falloff = 2.0f; // m past the banks over which the brush fades
	float carve_cell_size = 0.5f;
	NodePath base_height_source;

	MeshInstance3D *mesh_instance = nullptr;
	LocalVector<Section> sections;
	bool build_pending = false;
	double water_time = 0.0;

	float _point_value(const PackedFloat32Array &p_values, int p_index, float p_default) const;
	void _queue_build();
	void _build();
	void _build_mesh();
	void _on_curve_changed();
	void _solve_flow(float p_ds);
	bool _ground_at(PhysicsDirectSpaceState3D *p_space, const Vector3 &p_local, float &r_y, uint32_t p_mask) const;
	bool _base_ground_at(PhysicsDirectSpaceState3D *p_space, const Vector3 &p_local, float &r_y) const;
	float _carve_target(const Section &p_sec, float p_n, float &r_mask) const;
	bool _locate(const Vector3 &p_world, int &r_section, float &r_t, float &r_n) const;

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_widths(const PackedFloat32Array &p_widths);
	PackedFloat32Array get_widths() const { return widths; }
	void set_depths(const PackedFloat32Array &p_depths);
	PackedFloat32Array get_depths() const { return depths; }
	void set_default_width(float p_width);
	float get_default_width() const { return default_width; }
	void set_discharge(float p_discharge);
	float get_discharge() const { return discharge; }
	void set_roughness(float p_roughness);
	float get_roughness() const { return roughness; }
	void set_max_slope(float p_slope);
	float get_max_slope() const { return max_slope; }
	void set_snap_to_ground(bool p_snap);
	bool get_snap_to_ground() const { return snap_to_ground; }
	void set_ground_collision_mask(uint32_t p_mask);
	uint32_t get_ground_collision_mask() const { return ground_collision_mask; }
	void set_obstacle_collision_mask(uint32_t p_mask);
	uint32_t get_obstacle_collision_mask() const { return obstacle_collision_mask; }
	void set_step_length(float p_length);
	float get_step_length() const { return step_length; }
	void set_cross_segments(int p_segments);
	int get_cross_segments() const { return cross_segments; }
	void set_bank_margin(float p_margin);
	float get_bank_margin() const { return bank_margin; }
	void set_foam_persistence(float p_seconds);
	float get_foam_persistence() const { return foam_persistence; }
	void set_carve(bool p_carve);
	bool get_carve() const { return carve; }
	void set_carve_depth(float p_depth);
	float get_carve_depth() const { return carve_depth; }
	void set_carve_bank_slope(float p_slope);
	float get_carve_bank_slope() const { return carve_bank_slope; }
	void set_carve_falloff(float p_falloff);
	float get_carve_falloff() const { return carve_falloff; }
	void set_carve_cell_size(float p_size);
	float get_carve_cell_size() const { return carve_cell_size; }
	void set_base_height_source(const NodePath &p_path);
	NodePath get_base_height_source() const { return base_height_source; }
	// The channel as height brushes for a terrain deformer, one per square tile
	// of p_tile_size m (world XZ) the river touches.
	Array get_carve_brushes(float p_tile_size = 64.0f) const;
	void set_water_material(const Ref<ShaderMaterial> &p_material);
	Ref<ShaderMaterial> get_water_material() const { return water_material; }

	// Rebuild now (ground rays need the ground in the physics space).
	void rebuild();
	// Water surface height at a world position, -INF off the river or where it's dry.
	float sample_height(const Vector3 &p_world_pos) const;
	// Depth-averaged water velocity (m/s, world) at a world position; zero off the river.
	Vector3 sample_flow(const Vector3 &p_world_pos) const;
	// Water depth (m) under a world position, <= 0 off the river.
	float sample_depth(const Vector3 &p_world_pos) const;
	// White water amount 0..1 at a world position.
	float sample_foam(const Vector3 &p_world_pos) const;
	float get_length() const;
	int get_section_count() const { return (int)sections.size(); }
	// One cross-section, for tools and tests: centre (world), width, surface (world y), mean speed, foam, cascade.
	Dictionary get_section(int p_index) const;

	PackedStringArray get_configuration_warnings() const override;
};

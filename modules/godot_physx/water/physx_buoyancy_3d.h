/**************************************************************************/
/*  physx_buoyancy_3d.h                                                   */
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

#include "scene/3d/node_3d.h"

class PhysXRiver3D;
class PhysXWaterSurface3D;
class RigidBody3D;

// Floats its parent RigidBody3D on a PhysXWaterSurface3D or a PhysXRiver3D. Sample points on
// the hull each carry a share of the hull's footprint: the water pushes each
// one up by the weight of the water it displaces (density * g * area *
// depth, up to hull_height), so the body settles at its real draft and
// rights itself from the spread of the points. Submerged points drag
// against the water, and the body disturbs the ripple layer, more as it
// moves. Only applies RigidBody3D forces -- works with any physics engine.
class PhysXBuoyancy3D : public Node3D {
	GDCLASS(PhysXBuoyancy3D, Node3D);

public:
	enum VolumeMode {
		VOLUME_POINTS, // probe points sharing a footprint and a hull height
		VOLUME_SHAPES, // the collision shapes' volume: spheres as pontoons, the rest in cells
	};

private:
	VolumeMode volume_mode = VOLUME_POINTS;
	int max_cells = 32; // cells a body in VOLUME_SHAPES (spheres are one each)
	bool mass_from_densities = false; // body mass and centre of mass from the shapes' buoyancy_density
	float default_density = 500.0f; // kg/m^3 for shapes without buoyancy_density (with mass_from_densities)
	float drag_coefficient = 1.0f; // form drag, 0.5 rho Cd A |v| v on the wet part (VOLUME_SHAPES)
	float wave_damping = 0.2f; // heave, pitch and roll damping by the waves a floating body makes, share of critical
	struct Cell {
		Vector3 local; // body-local centre
		Vector3 axis[3]; // body-local half extents (a box cell's half axes)
		float volume = 0.0f; // m^3
		float radius = 0.0f; // > 0: a sphere (a pontoon), exact cap
	};
	LocalVector<Cell> cells;
	float total_volume = 0.0f;
	void _build_cells(RigidBody3D *p_body);
	void _shapes_step(RigidBody3D *p_body);

	NodePath water_surface_path;
	PackedVector3Array sample_points; // body-local; empty = generated from the collision shapes
	float hull_area = 0.0f; // m^2 footprint; 0 = from the collision shapes
	float hull_height = 0.0f; // m; 0 = from the collision shapes
	float water_density = 1000.0f;
	float linear_drag = 60.0f; // N per (m/s) per m^2 of submerged footprint
	float angular_drag = 40.0f; // N*m per (rad/s), times the submerged fraction
	bool disturb_water = true;
	float ripple_radius = 0.0f; // 0 = from the footprint
	float ripple_rest_strength = 0.15f;
	float ripple_reference_speed = 5.0f;

	// Resolved at runtime.
	ObjectID water_id;
	ObjectID river_id;
	ObjectID wake_id; // a sibling PhysXWaterWake3D: its own dip doesn't count
	PackedVector3Array points; // in use (explicit or generated)
	float area = 1.0f;
	float height = 0.5f;
	float radius = 1.0f;
	float submerged_fraction = 0.0f;
	PackedFloat32Array point_depths;

	void _resolve();
	void _find_wake();
	void _physics_step();
	bool _has_water() const;
	float _height_at(const Vector3 &p_world_pos) const;
	Vector3 _flow_at(const Vector3 &p_world_pos) const;

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	RigidBody3D *get_body() const;
	PhysXWaterSurface3D *get_water_surface() const;
	PhysXRiver3D *get_river() const;

	void set_volume_mode(VolumeMode p_mode);
	VolumeMode get_volume_mode() const { return volume_mode; }
	void set_max_cells(int p_cells);
	int get_max_cells() const { return max_cells; }
	void set_mass_from_densities(bool p_enabled);
	bool get_mass_from_densities() const { return mass_from_densities; }
	void set_default_density(float p_density);
	float get_default_density() const { return default_density; }
	void set_wave_damping(float p_ratio) { wave_damping = MAX(p_ratio, 0.0f); }
	float get_wave_damping() const { return wave_damping; }
	void set_drag_coefficient(float p_cd) { drag_coefficient = MAX(p_cd, 0.0f); }
	float get_drag_coefficient() const { return drag_coefficient; }
	// The volume in use in VOLUME_SHAPES (m^3).
	float get_total_volume() const { return total_volume; }

	void set_water_surface_path(const NodePath &p_path);
	NodePath get_water_surface_path() const { return water_surface_path; }
	void set_sample_points(const PackedVector3Array &p_points);
	PackedVector3Array get_sample_points() const { return sample_points; }
	void set_hull_area(float p_area);
	float get_hull_area() const { return hull_area; }
	void set_hull_height(float p_height);
	float get_hull_height() const { return hull_height; }
	void set_water_density(float p_density) { water_density = MAX(p_density, 0.0f); }
	float get_water_density() const { return water_density; }
	void set_linear_drag(float p_drag) { linear_drag = MAX(p_drag, 0.0f); }
	float get_linear_drag() const { return linear_drag; }
	void set_angular_drag(float p_drag) { angular_drag = MAX(p_drag, 0.0f); }
	float get_angular_drag() const { return angular_drag; }
	void set_disturb_water(bool p_enabled);
	bool get_disturb_water() const { return disturb_water; }
	void set_ripple_radius(float p_radius);
	float get_ripple_radius() const { return ripple_radius; }
	void set_ripple_rest_strength(float p_strength) { ripple_rest_strength = CLAMP(p_strength, 0.0f, 1.0f); }
	float get_ripple_rest_strength() const { return ripple_rest_strength; }
	void set_ripple_reference_speed(float p_speed) { ripple_reference_speed = MAX(p_speed, 0.01f); }
	float get_ripple_reference_speed() const { return ripple_reference_speed; }

	// Fraction of the hull's buoyancy in use this tick (0 = out of the water,
	// 1 = fully under), and each sample point's depth below the surface (m,
	// <= 0 above it).
	float get_submerged_fraction() const { return submerged_fraction; }
	PackedFloat32Array get_point_depths() const { return point_depths; }
	// The sample points in use (generated ones included), body-local.
	PackedVector3Array get_effective_sample_points() const { return points; }
	// Water surface height at a world position, -INF where there's none.
	float get_water_height(const Vector3 &p_world_pos) const;

	PackedStringArray get_configuration_warnings() const override;
};

VARIANT_ENUM_CAST(PhysXBuoyancy3D::VolumeMode);

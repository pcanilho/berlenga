/**************************************************************************/
/*  physx_cloth_3d.h                                                      */
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

#include "../cloth/xpbd_cloth_solver.h"

#include "scene/3d/visual_instance_3d.h"

#include "../cloth/cloth_wetness.h" // not Flow's: builds without flow_sdk need it too
#ifdef GODOT_PHYSX_FLOW
#include "../flow/physx_cloth_burn.h"
#endif
#include "scene/resources/mesh.h"

// A cloth patch simulated by the PhysX module. On an NVIDIA machine with GPU
// support it runs on a PhysX deformable surface; otherwise it falls back to a
// built-in XPBD solver on the CPU, so it works on any platform. Either way the
// result is drawn as an ArrayMesh with a standard material.
class PhysXCloth3D : public GeometryInstance3D {
	GDCLASS(PhysXCloth3D, GeometryInstance3D);

public:
	enum PinMode {
		PIN_NONE,
		PIN_TOP_EDGE,
		PIN_TOP_CORNERS,
		PIN_LEFT_EDGE,
		PIN_RIGHT_EDGE,
	};

	enum SimulationMode {
		SIM_AUTO, // GPU deformable surface if a CUDA device is present, else CPU
		SIM_GPU, // force GPU; falls back to CPU (with a warning) when unavailable
		SIM_CPU, // always the built-in CPU solver
	};

private:
	RID mesh;

	// Rest shape: a generated grid, or a supplied triangle mesh.
	Ref<Mesh> source_mesh;
	int grid_columns = 24;
	int grid_rows = 24;
	Vector2 grid_size = Vector2(1.0, 1.0);

	SimulationMode simulation_mode = SIM_AUTO;
	int substeps = 12;
	float stiffness = 0.9; // 0..1, maps to structural XPBD compliance
	float shear_stiffness = 0.4;
	float bend_stiffness = 0.1;
	float damping = 0.03;
	float density = 0.2;

	bool wind_enabled = true;
	NodePath wind_area; // optional Area3D whose wind settings drive the cloth
	Vector3 wind = Vector3(0, 0, 0); // constant base breeze, on top of the area
	float wind_turbulence = 0.4;
	bool weather_wind = true; // add the WeatherMap3D wind when there is one
	// A PhysXFlow3D whose gas blows the cloth (its velocity at each vertex), and which the cloth stirs back.
	NodePath flow_path;
	LocalVector<Vector3> flow_prev_positions; // world, last step, for the cloth's own velocity
	void _couple_flow(double p_delta);
	void _eddy_air(const Vector3 &p_wind); // wind_turbulence's eddies across the cloth, when no Flow gas drives it
	bool eddies_on = false;
	float drag = 1.0;
	float lift = 0.2;

	float tear_factor = 0.0; // 0 = off; else edges stretched past this many rest lengths break

	// Burning (with Flow): heat from a PhysXFireSpread3D's flames, Flow's hot gas and burning objects lights the
	// fabric; it chars, glows and burns through into holes (CPU solver).
	bool burnable = true;
	NodePath fire_path;
	float flame_spread = 0.02f;
	float burn_through_time = 3.0f;
#ifdef GODOT_PHYSX_FLOW
	PhysXClothBurn burn;
	RID burn_override;
	PackedColorArray burn_colors;
	void _step_burn(double p_delta);
#endif

	// Wet from the weather (docs/ideas/physx.md, "Rain soaks cloth"): rain soaks the fabric up to water_capacity times
	// its own weight; wet, it is heavier, limper, darker and clings to what it touches, then dries in the air.
	ClothWetness wet;
	bool weather_wetting = true;
	float water_capacity = 1.0f; // kg of water a kg of fabric holds soaked (cotton about 1, wool more, polyester less)
	float wet_applied = 0.0f; // the wetness the solver's masses and stiffness last took
	void _update_wetness(double p_delta, const Vector3 &p_wind);

	PinMode pin_mode = PIN_TOP_EDGE;
	PackedInt32Array pinned_vertices; // extra explicit pins, by vertex index
	NodePath anchor_path; // if set, pinned vertices follow this node's motion

	bool collision_enabled = true;
	uint32_t collision_mask = 1;
	float friction = 0.4; // 0..1, tangential velocity bled off on contact

	bool simulating = true;

	XPBDClothSolver solver; // CPU fallback
	RID gpu_cloth; // GodotPhysXCloth3D RID when the GPU (PxDeformableSurface) path is in use
	uint32_t gpu_mesh_version = 0;
	bool built = false;
	double wind_time = 0.0;
	Transform3D last_anchor_xform;
	// Where the cloth was built, and whether it has stepped since: a script move before the first step (placing a
	// spawned cloth) rebuilds it there. Once it runs, the cloth owns its world positions.
	Transform3D built_xform;
	bool stepped = false;
	LocalVector<int> pinned_index_cache; // resolved pin set (mode + explicit)

	PackedVector3Array render_verts;
	PackedVector3Array render_normals;
	PackedVector2Array render_uvs;
	PackedInt32Array render_indices;

	RID collision_sphere; // PhysicsServer sphere shape reused for the collision pass

	void _rebuild();
	void _destroy();
	void _apply_solver_settings();
	bool _try_build_gpu();
	void _resolve_pins();
	void _step(double p_delta);
	void _collide();
	LocalVector<Vector3> pre_step_positions; // world, before the solver step, for swept collision
	void _update_mesh(); // CPU: from the solver
	bool _fill_uvs(int p_count);
	void _update_gpu_mesh(); // GPU: from the server readback
	Vector3 _sample_wind(double p_delta);

	static bool _gpu_available();

protected:
	void _notification(int p_what);
	void _validate_property(PropertyInfo &p_property) const;
	static void _bind_methods();

public:
	void set_simulation_mode(SimulationMode p_mode);
	SimulationMode get_simulation_mode() const { return simulation_mode; }

	void set_source_mesh(const Ref<Mesh> &p_mesh);
	Ref<Mesh> get_source_mesh() const { return source_mesh; }
	void set_grid_columns(int p_v);
	int get_grid_columns() const { return grid_columns; }
	void set_grid_rows(int p_v);
	int get_grid_rows() const { return grid_rows; }
	void set_grid_size(const Vector2 &p_v);
	Vector2 get_grid_size() const { return grid_size; }

	void set_substeps(int p_v);
	int get_substeps() const { return substeps; }
	void set_stiffness(float p_v);
	float get_stiffness() const { return stiffness; }
	void set_shear_stiffness(float p_v);
	float get_shear_stiffness() const { return shear_stiffness; }
	void set_bend_stiffness(float p_v);
	float get_bend_stiffness() const { return bend_stiffness; }
	void set_damping(float p_v);
	float get_damping() const { return damping; }
	void set_density(float p_v);
	float get_density() const { return density; }

	void set_wind_enabled(bool p_v);
	bool is_wind_enabled() const { return wind_enabled; }
	void set_wind_area(const NodePath &p_v);
	NodePath get_wind_area() const { return wind_area; }
	void set_wind(const Vector3 &p_v);
	Vector3 get_wind() const { return wind; }
	void set_flow_path(const NodePath &p_path) { flow_path = p_path; }
	NodePath get_flow_path() const { return flow_path; }
	void set_weather_wind(bool p_v);
	bool get_weather_wind() const { return weather_wind; }
	void set_wind_turbulence(float p_v);
	float get_wind_turbulence() const { return wind_turbulence; }
	void set_drag(float p_v);
	float get_drag() const { return drag; }
	void set_lift(float p_v);
	float get_lift() const { return lift; }

	void set_pin_mode(PinMode p_v);
	PinMode get_pin_mode() const { return pin_mode; }
	void set_pinned_vertices(const PackedInt32Array &p_v);
	PackedInt32Array get_pinned_vertices() const { return pinned_vertices; }
	void set_anchor_path(const NodePath &p_v);
	NodePath get_anchor_path() const { return anchor_path; }

	void set_collision_enabled(bool p_v);
	bool is_collision_enabled() const { return collision_enabled; }
	void set_collision_mask(uint32_t p_v);
	uint32_t get_collision_mask() const { return collision_mask; }
	void set_friction(float p_v);
	float get_friction() const { return friction; }

	void set_tear_factor(float p_v);
	float get_tear_factor() const { return tear_factor; }
	// Strain tearing is on for this node and the project (physics/physx_3d/cloth/tearing).
	bool is_tearing() const;
	float get_max_strain() const { return solver.get_max_strain(); }

	void set_simulating(bool p_v);
	bool is_simulating() const { return simulating; }

	// Drop the cloth back to its rest shape (in the node's current transform).
	void reset();
	int get_vertex_count() const { return solver.vertex_count(); }
	// Local-space positions of the currently pinned vertices (for the editor gizmo).
	PackedVector3Array get_pinned_positions() const;
	int release_pins(const PackedInt32Array &p_vertices);
	void move_pins(const PackedInt32Array &p_vertices, const PackedVector3Array &p_positions);
	int tear(const Vector3 &p_from, const Vector3 &p_to);
	void set_wetness(float p_v);
	float get_wetness() const { return wet.effective(); }
	void set_weather_wetting(bool p_v) { weather_wetting = p_v; }
	bool is_weather_wetting() const { return weather_wetting; }
	void set_water_capacity(float p_v);
	float get_water_capacity() const { return water_capacity; }
	// Water held, kg/m2.
	float get_water_load() const { return wet.effective() * density * water_capacity; }

	void set_burnable(bool p_v);
	bool is_burnable() const { return burnable; }
	void set_fire_path(const NodePath &p_v) { fire_path = p_v; }
	NodePath get_fire_path() const { return fire_path; }
	void set_flame_spread(float p_v);
	float get_flame_spread() const { return flame_spread; }
	void set_burn_through_time(float p_v);
	float get_burn_through_time() const { return burn_through_time; }
	// Sets the fabric alight at the particle nearest a world position (CPU solver, Flow builds).
	void ignite(const Vector3 &p_position);
	int get_burnt_count() const;
	int get_burning_count() const;
	// True when the GPU (PhysX deformable-surface) path is in use.
	bool is_gpu_accelerated() const { return gpu_cloth.is_valid(); }

	AABB get_aabb() const override;
	PackedStringArray get_configuration_warnings() const override;

	PhysXCloth3D();
	~PhysXCloth3D();
};

VARIANT_ENUM_CAST(PhysXCloth3D::PinMode);
VARIANT_ENUM_CAST(PhysXCloth3D::SimulationMode);

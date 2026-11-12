/**************************************************************************/
/*  physx_river_3d.h                                                      */
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

#include "water_raster_reflections.h"
#include "water_solver.h"

#include "scene/3d/path_3d.h"
#include "scene/resources/material.h"

class MeshInstance3D;
class Texture2DRD;

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
		LocalVector<float> psi_at; // stream function (m^3/s from the right bank) per cross sample
		float discharge = 0.0f; // m^3/s here: the river's own plus tributaries joined upstream
		float psi_right = 0.0f; // psi on the right bank (lowered by tributaries joining from the right)
		float mouth_fade = 1.0f; // 1 on the river, fading to 0 past its mouth into the water it joins
		bool fall_drop = false; // the ribbon from here to the next section is under a waterfall's jet (hidden)
		float plunge = 0.0f; // 0..1 white water where a waterfall's jet lands
	};
	// A free overfall: the water leaves the lip and falls clear of the ground.
	struct Fall {
		int brink = 0; // last section above the lip
		int foot = 0; // first section at the bottom
		float lip_s = 0.0f; // arc length of the lip
		Vector3 lip; // local, centre of the lip on the bed
		Vector3 tangent; // local, horizontal downstream
		Vector3 side; // local, horizontal to the left bank
		float pool = 0.0f; // local y of the water the jet lands in
		float height = 0.0f; // lip surface to pool, m
		float q = 0.0f; // mean unit discharge at the lip, m^2/s
		float lip_speed = 0.0f; // m/s
		float impact_speed = 0.0f; // m/s
		float land_s = 0.0f; // arc length where the centre of the jet lands
		float breakup = 0.0f; // m of fall over which the sheet breaks up (Horeni)
		float width = 0.0f; // wet width at the lip
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

	// The live window: shallow-water simulation on a square round the camera.
	bool live_flow = false;
	bool raster_reflections = true;
	WaterRasterReflections raster_reflection_state;
	float live_domain_size = 48.0f; // m
	int live_grid_resolution = 192;
	float wall_friction = 1.5f; // 1/s, along rocks and banks
	float flow_foam = 0.25f; // a river runs fast everywhere: less than the sea's 1
	float flow_foam_persistence = 4.0f; // s
	float live_damping = 0.05f;
	NodePath live_follow; // empty: the viewport's camera

	WaterSolver live_solver;
	bool live_configured = false;
	bool live_bound = false;
	bool live_fields_dirty = false;
	Vector2 live_center;
	Ref<Texture2DRD> live_height_tex;
	Ref<Texture2DRD> live_flow_tex;
	Vector<float> live_height_cache;
	Vector<float> live_velocity_cache;
	PackedFloat32Array live_depth; // still-water depth per window cell, for the tilt fit
	Vector3 live_tilt; // the window's mean plane (h0, dh/dx, dh/dz), taken out
	int live_refresh = 0;

	// Joins: where the river ends (a lake, the sea, or another river), and the
	// rivers flowing into this one.
	NodePath mouth_target;
	float mouth_blend = 6.0f; // m past the shoreline over which the ribbon fades into the water it joins
	struct Tributary {
		ObjectID river;
		Vector3 at; // world, its mouth
		float s = 0.0f; // where it joins this river (found on each build)
		float discharge = 0.0f;
		bool from_right = false;
	};
	LocalVector<Tributary> tributaries;
	ObjectID mouth_id; // what this river last registered with
	float mouth_level_seen = -Math::INF; // the target river's surface at the join, last build

	// Waterfalls (see docs/ideas/water.md "Step 4f").
	float fall_min_height = 1.5f; // m; 0 turns falls off (every drop stays a cascade)
	PackedVector3Array fall_exclusions; // lip and foot pairs of faces drawn by another node (PhysXWaterfalls3D)
	bool fall_spray = true;
	bool fall_mist = true;
	float fall_mist_amount = 1.0f; // scales the mist each fall makes (its own grid, without a PhysXFlow3D)
	Ref<ShaderMaterial> fall_material;
	LocalVector<Fall> falls;
	LocalVector<Node3D *> fall_effects; // spray particles and mist emitters, rebuilt with the falls

	MeshInstance3D *mesh_instance = nullptr;
	MeshInstance3D *fall_instance = nullptr;
	LocalVector<Section> sections;
	bool build_pending = false;
	double water_time = 0.0;

	float _point_value(const PackedFloat32Array &p_values, int p_index, float p_default) const;
	void _queue_build();
	void _build();
	void _build_mesh();
	void _find_falls(PhysicsDirectSpaceState3D *p_space, const LocalVector<float> &p_bed, float p_ds);
	void _build_fall_mesh();
	void _build_fall_effects();
	void _on_curve_changed();
	void _solve_flow(float p_ds);
	bool _strip_at(const Vector3 &p_world, int &r_section, float &r_t, float &r_n) const;
	float _psi_at(const Vector3 &p_world, float &r_depth) const;
	void _live_fields(PackedFloat32Array &r_depth, PackedFloat32Array &r_flux) const;
	void _live_update(double p_delta);
	void _live_stop();
	void _live_material();
	void _live_fit_tilt();
	Node *_mouth_node() const;
	void _register_mouth();
	void _unregister_mouth();
	void _on_target_rebuilt();
	Vector2 _live_velocity_at(const Vector3 &p_world) const;
	float _live_height_at(const Vector3 &p_world) const;
	bool _ground_at(PhysicsDirectSpaceState3D *p_space, const Vector3 &p_local, float &r_y, uint32_t p_mask) const;
	bool _base_ground_at(PhysicsDirectSpaceState3D *p_space, const Vector3 &p_local, float &r_y) const;
	float _carve_target(const Section &p_sec, float p_n, float &r_mask) const;
	bool _locate(const Vector3 &p_world, int &r_section, float &r_t, float &r_n) const;

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	// The falling-sheet shader (streaks riding the water, aeration, break-up), shared with PhysXWaterfalls3D.
	static String get_fall_shader_code();
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
	// Raster only: the sky and the sun's glitter on open water by the Fresnel term (water_raster_reflections.h).
	void set_raster_reflections(bool p_enabled) { raster_reflections = p_enabled; }
	bool get_raster_reflections() const { return raster_reflections; }
	void set_live_flow(bool p_enabled);
	bool get_live_flow() const { return live_flow; }
	void set_live_domain_size(float p_size);
	float get_live_domain_size() const { return live_domain_size; }
	void set_live_grid_resolution(int p_n);
	int get_live_grid_resolution() const { return live_grid_resolution; }
	void set_wall_friction(float p_friction);
	float get_wall_friction() const { return wall_friction; }
	void set_flow_foam(float p_strength);
	float get_flow_foam() const { return flow_foam; }
	void set_flow_foam_persistence(float p_seconds);
	float get_flow_foam_persistence() const { return flow_foam_persistence; }
	void set_live_damping(float p_damping);
	float get_live_damping() const { return live_damping; }
	void set_live_follow(const NodePath &p_path);
	NodePath get_live_follow() const { return live_follow; }
	// Where the live window is (world xz centre), and whether it runs.
	Vector2 get_live_center() const { return live_center; }
	bool is_live_running() const { return live_configured && live_solver.is_available(); }
	// A floating body pushing the live water (as PhysXWaterSurface3D::submit_sphere()).
	void submit_sphere(int p_owner, const Vector3 &p_world_pos, float p_radius, float p_strength = 1.0f);
	void clear_sphere(int p_owner);
	void set_mouth_target(const NodePath &p_path);
	NodePath get_mouth_target() const { return mouth_target; }
	void set_mouth_blend(float p_blend);
	float get_mouth_blend() const { return mouth_blend; }
	// A river joining this one at p_world (its mouth) adds p_discharge downstream of there.
	void set_tributary(ObjectID p_river, const Vector3 &p_world, float p_discharge);
	void clear_tributary(ObjectID p_river);
	// Discharge (m^3/s) at a distance along the river, tributaries included.
	float get_discharge_at(float p_s) const;
	void set_fall_min_height(float p_height);
	float get_fall_min_height() const { return fall_min_height; }
	void set_fall_exclusions(const PackedVector3Array &p_points);
	PackedVector3Array get_fall_exclusions() const { return fall_exclusions; }
	void set_fall_spray(bool p_enabled);
	bool get_fall_spray() const { return fall_spray; }
	void set_fall_mist(bool p_enabled);
	bool get_fall_mist() const { return fall_mist; }
	void set_fall_mist_amount(float p_amount);
	float get_fall_mist_amount() const { return fall_mist_amount; }
	void set_fall_material(const Ref<ShaderMaterial> &p_material);
	Ref<ShaderMaterial> get_fall_material() const { return fall_material; }
	// Each waterfall: lip and impact (world), height, speeds, break-up length.
	Array get_waterfalls() const;
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

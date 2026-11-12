/**************************************************************************/
/*  physx_waterfalls_3d.h                                                 */
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

class GPUParticles3D;
class MeshInstance3D;

// Waterfalls from a terrain generator's fall records (TerrainGenerator3D.get_waterfalls(): lip points with their
// share of the flow, face profiles, ledges, form, impact speed). Falls whose water stays on the rock (cascade,
// horsetail, tiered: "contact falls") get a sheet that follows the face and turns white past the self-aeration
// inception point (Chanson 1993), spray where it strikes each ledge and at the foot, and mist off the face.
// docs/ideas/water.md "Step 6a: contact falls".
class PhysXWaterfalls3D : public Node3D {
	GDCLASS(PhysXWaterfalls3D, Node3D);

public:
	struct Column {
		PackedVector3Array points; // world, on the water's surface
		PackedFloat32Array speed; // m/s
		PackedFloat32Array air; // mean air concentration C, 0..1
		PackedFloat32Array travel; // s since the lip
		PackedFloat32Array dist; // m down the face from the lip
		PackedFloat32Array cover; // a free jet's unbroken share, 1..0 past the break-up length (empty on the rock)
		float q = 0.0f; // m^2/s
		float edge = 0.0f; // m inside the water's margin (< 0 on the wet rock beyond it)
	};
	struct Face {
		String id;
		String type;
		Vector3 top;
		Vector3 direction;
		float height = 0.0f;
		float discharge = 0.0f;
		float width = 0.0f;
		float impact_speed = 0.0f;
		float inception = 0.0f; // m down the face to the inception point (centre column)
		float air_at_foot = 0.0f;
		float air_peak = 0.0f;
		LocalVector<Column> columns;
		PackedVector3Array ledges; // world
		PackedFloat32Array ledge_speed; // m/s of the water striking each
		Vector3 foot;
		int plunge = -1; // a free jet's sheet: its index in plunges
		PackedFloat32Array sky_spec; // per row: share of the reflected sky the terrain leaves open (1 without heights)
	};
	// A tall free fall (plunge): the jet leaves the lip as a sheet, breaks up after L_b = 6 q^0.32 m (Horeni 1956)
	// and falls on as clumps and drops slowed by the air towards their terminal speeds. docs/ideas/water.md
	// "Step 6b".
	struct Plunge {
		String id;
		Vector3 top; // the lip, middle of the water
		Vector3 direction; // horizontal, downstream
		float height = 0.0f;
		float discharge = 0.0f;
		float width = 0.0f; // the water's, at the lip
		float lip_speed = 0.0f;
		float breakup = 0.0f; // m along the jet
		Vector3 zone_start; // the break-up zone, where clumps are launched (world)
		Vector3 zone_end;
		Vector3 zone_start_velocity;
		Vector3 zone_end_velocity;
		float floor = 0.0f; // the level the column lands on (pool, lake, ground)
		Vector3 foot; // where the column lands, mean
		float landing_speed = 0.0f;
		float fall_time = 0.0f; // s for the slowest clumps from the break-up to the floor
		float column_width = 0.0f; // across, at the floor (95% of the water)
		float drift = 0.0f; // m the wind carries the column's foot
		PackedFloat32Array face_out; // m out from the brink the cliff stands, at 16 heights from the floor up
		int streaks = 0;
		GPUParticles3D *column = nullptr;
		Ref<ShaderMaterial> column_material;
	};

private:
	NodePath source;
	NodePath surface; // a Landscape3D: the drawn rock the sheets lie on (get_cliff_point)
	Object *surface_obj = nullptr; // during rebuild()
	bool contact_falls = true;
	bool spray = true;
	bool mist = true;
	float mist_amount = 1.0f;
	float mist_min_discharge = 0.2f;
	float min_height = 5.0f;
	float rock_roughness = 0.15f; // k_s, m
	float friction_factor = 0.2f; // Darcy-Weisbach f of white water on rough rock (Chanson: 0.1 to 0.3 on steps)
	float row_spacing = 0.5f; // m along the face (a ledge's brink between rows 1 m apart cut through the sheet)
	float wet_margin = 0.6f; // m of wet rock either side of the water
	float rock_step = 0.8f; // m: height of the steps and ledges a thin flow runs over (nappe vs skimming flow)
	bool plunge_falls = true;
	float column_density = 1.0f; // scales the falling clumps drawn for a broken column
	float column_turbulence = 0.5f; // m/s: the air's eddies round a falling column, which widen it as it falls
	// Share of the open wind a falling column feels: it stands in the lee of its own cliff, and the air inside it is
	// dragged down with the water.
	float wind_exposure = 0.3f;

	LocalVector<Face> faces;
	LocalVector<Plunge> plunges;
	double wind_wait = 0.0;
	// Standing water from the source (sea level, lakes as rectangle and level): a sheet stops where it meets it.
	float sea_level = -1e30f;
	LocalVector<Rect2> lake_rects;
	LocalVector<float> lake_levels;
	float _water_level(const Vector3 &p_position, Object *p_source) const;
	MeshInstance3D *mesh_instance = nullptr;
	Ref<ShaderMaterial> material;
	LocalVector<Node3D *> effects;
	double water_time = 0.0;

	void _build_face(const Dictionary &p_fall, Object *p_source);
	void _build_plunge(const Dictionary &p_fall, Object *p_source);
	void _simulate_column(Plunge &r_plunge, const Vector3 &p_wind, Object *p_source) const;
	GPUParticles3D *_make_column(Plunge &r_plunge, const Transform3D &p_inv) const;
	Vector3 _wind_at(const Vector3 &p_position) const;
	void _exclude_from_rivers();
	void _build_mesh();
	void _build_effects();
	void _clear_effects();

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void rebuild();
	static String get_contact_shader_code();
	void set_wet_margin(float p_margin);
	float get_wet_margin() const;
	void set_rock_step(float p_step);
	float get_rock_step() const;
	// Share of a steep face a thin flow covers: falls step to step as threads (nappe flow) below the onset of
	// skimming flow, y_c / h > 1.2 - 0.325 h/l (Chanson 2001), and covers the rock above it.
	static float flow_cover(float p_q, float p_step, float p_tan_slope);
	static void _sky_spec(Face &r_face, int p_col, Object *p_source);
	Array get_contact_falls() const;
	Array get_plunge_falls() const;
	static String get_column_shader_code();
	static String get_streak_shader_code();
	void set_plunge_falls(bool p_enabled);
	bool get_plunge_falls_enabled() const { return plunge_falls; }
	void set_column_density(float p_density);
	float get_column_density() const { return column_density; }
	void set_wind_exposure(float p_share);
	float get_wind_exposure() const { return wind_exposure; }
	void set_column_turbulence(float p_speed);
	float get_column_turbulence() const { return column_turbulence; }

	void set_source(const NodePath &p_path);
	NodePath get_source() const { return source; }
	void set_surface(const NodePath &p_path);
	NodePath get_surface() const { return surface; }
	void set_contact_falls(bool p_enabled);
	bool get_contact_falls_enabled() const { return contact_falls; }
	void set_spray(bool p_enabled);
	bool get_spray() const { return spray; }
	void set_mist(bool p_enabled);
	bool get_mist() const { return mist; }
	void set_mist_amount(float p_amount);
	float get_mist_amount() const { return mist_amount; }
	void set_mist_min_discharge(float p_q);
	float get_mist_min_discharge() const { return mist_min_discharge; }
	void set_min_height(float p_height);
	float get_min_height() const { return min_height; }
	void set_rock_roughness(float p_ks);
	float get_rock_roughness() const { return rock_roughness; }
	void set_friction_factor(float p_f);
	float get_friction_factor() const { return friction_factor; }
	void set_row_spacing(float p_spacing);
	float get_row_spacing() const { return row_spacing; }

	// The self-aeration of a steep chute: inception length (m from the lip) and equilibrium mean air concentration.
	static float inception_length(float p_q, float p_sin_slope, float p_ks);
	static float equilibrium_air(float p_sin_slope);

	PhysXWaterfalls3D();
	~PhysXWaterfalls3D();
};

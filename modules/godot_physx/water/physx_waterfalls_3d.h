/**************************************************************************/
/*  physx_waterfalls_3d.h                                                 */
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
#include "scene/resources/material.h"

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
		float q = 0.0f; // m^2/s
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
		LocalVector<Column> columns;
		PackedVector3Array ledges; // world
		PackedFloat32Array ledge_speed; // m/s of the water striking each
		Vector3 foot;
	};

private:
	NodePath source;
	bool contact_falls = true;
	bool spray = true;
	bool mist = true;
	float mist_amount = 1.0f;
	float min_height = 5.0f;
	float rock_roughness = 0.15f; // k_s, m
	float manning_n = 0.05f; // rough rock
	float row_spacing = 1.0f; // m along the face

	LocalVector<Face> faces;
	MeshInstance3D *mesh_instance = nullptr;
	Ref<ShaderMaterial> material;
	LocalVector<Node3D *> effects;
	double water_time = 0.0;

	void _build_face(const Dictionary &p_fall);
	void _build_mesh();
	void _build_effects();
	void _clear_effects();

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void rebuild();
	Array get_contact_falls() const;

	void set_source(const NodePath &p_path);
	NodePath get_source() const { return source; }
	void set_contact_falls(bool p_enabled);
	bool get_contact_falls_enabled() const { return contact_falls; }
	void set_spray(bool p_enabled);
	bool get_spray() const { return spray; }
	void set_mist(bool p_enabled);
	bool get_mist() const { return mist; }
	void set_mist_amount(float p_amount);
	float get_mist_amount() const { return mist_amount; }
	void set_min_height(float p_height);
	float get_min_height() const { return min_height; }
	void set_rock_roughness(float p_ks);
	float get_rock_roughness() const { return rock_roughness; }
	void set_manning_n(float p_n);
	float get_manning_n() const { return manning_n; }
	void set_row_spacing(float p_spacing);
	float get_row_spacing() const { return row_spacing; }

	// The self-aeration of a steep chute: inception length (m from the lip) and equilibrium mean air concentration.
	static float inception_length(float p_q, float p_sin_slope, float p_ks);
	static float equilibrium_air(float p_sin_slope);

	PhysXWaterfalls3D();
	~PhysXWaterfalls3D();
};

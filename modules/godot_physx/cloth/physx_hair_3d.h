/**************************************************************************/
/*  physx_hair_3d.h                                                       */
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

#include "hair_strand_solver.h"

#include "scene/3d/node_3d.h"
#include "scene/resources/3d/hair_groom.h"

class MeshInstance3D;
class Shader;
class ShaderMaterial;
class Texture2DRD;

// Strand hair that moves: a HairGroom's strands simulated on the GPU (compute, any GPU) and drawn
// like HairMesh, with the path tracer's hair fibre shading. The node is the head: the groom is in
// its space and the roots follow it (put it under a BoneAttachment3D on the head bone). Strands
// fall under gravity, blow in the wind, keep their groomed shape and length, and stay out of the
// collision spheres. In the editor the groom is drawn still.
class PhysXHair3D : public Node3D {
	GDCLASS(PhysXHair3D, Node3D);

	Ref<HairGroom> groom;
	float root_width = 0.0004f;
	float tip_width = 0.0001f;
	float melanin = 0.5f;
	float melanin_redness = 0.0f;
	float roughness = 0.3f;
	float radial_roughness = 0.3f;
	Ref<ShaderMaterial> render_material;

	bool simulating = true;
	int substeps = 4;
	float damping = 0.5f;
	float root_follow = 0.3f;
	float tip_follow = 0.0f;
	float follow_range = 0.3f;
	float shape_stiffness = 0.5f;
	float length_damping = 0.9f;
	bool use_world_gravity = true;
	Vector3 fixed_gravity = Vector3(0, -9.8f, 0);
	Vector3 wind;
	float gust = 0.3f;
	PackedVector4Array collision_spheres; // this node's space: centre, radius

	HairStrandSolver *solver = nullptr;
	MeshInstance3D *render_instance = nullptr;
	Ref<ShaderMaterial> copy_material;
	Ref<Texture2DRD> pos_texture;
	bool built = false;
	bool build_pending = false;

	static Ref<Shader> render_shader;

	void _groom_changed();
	void _queue_build();
	void _build();
	void _clear();
	void _make_material();
	void _step(double p_delta);
	Vector3 _fibre_absorption() const;
	Color _mass_color() const;

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	static void free_shared();

	void set_groom(const Ref<HairGroom> &p_groom);
	Ref<HairGroom> get_groom() const;
	void set_root_width(float p_width);
	float get_root_width() const;
	void set_tip_width(float p_width);
	float get_tip_width() const;
	void set_melanin(float p_melanin);
	float get_melanin() const;
	void set_melanin_redness(float p_redness);
	float get_melanin_redness() const;
	void set_roughness(float p_roughness);
	float get_roughness() const;
	void set_radial_roughness(float p_roughness);
	float get_radial_roughness() const;
	void set_render_material(const Ref<ShaderMaterial> &p_material);
	Ref<ShaderMaterial> get_render_material() const;

	void set_simulating(bool p_simulating);
	bool is_simulating() const;
	void set_substeps(int p_substeps);
	int get_substeps() const;
	void set_damping(float p_damping);
	float get_damping() const;
	void set_root_follow(float p_follow);
	float get_root_follow() const;
	void set_tip_follow(float p_follow);
	float get_tip_follow() const;
	void set_follow_range(float p_range);
	float get_follow_range() const;
	void set_shape_stiffness(float p_stiffness);
	float get_shape_stiffness() const;
	void set_length_damping(float p_damping);
	float get_length_damping() const;
	void set_use_world_gravity(bool p_use);
	bool is_using_world_gravity() const;
	void set_fixed_gravity(const Vector3 &p_gravity);
	Vector3 get_fixed_gravity() const;
	void set_wind(const Vector3 &p_wind);
	Vector3 get_wind() const;
	void set_gust(float p_gust);
	float get_gust() const;
	void set_collision_spheres(const PackedVector4Array &p_spheres);
	PackedVector4Array get_collision_spheres() const;

	void reset_simulation();
	MeshInstance3D *get_render_instance() const;

	~PhysXHair3D();
};

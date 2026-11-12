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
	float simplify_tolerance = 0.5f; // share of root_width a drawn strand may stray from its points
	float tip_width = 0.0001f;
	float melanin = 0.5f;
	float melanin_redness = 0.0f;
	float roughness = 0.3f;
	float radial_roughness = 0.3f;
	bool hair_shading = true;
	bool lod_enabled = true;
	float lod_start_distance = 2.0f;
	float lod_end_distance = 12.0f;
	float lod_strand_ratio = 0.25f;
	float lod_width_scale = 1.0f; // last set on the material
	Ref<ShaderMaterial> render_material;

	bool simulating = true;
	int substeps = 4;
	float damping = 0.08f;
	float global_stiffness = 0.1f;
	float global_range = 0.3f;
	float local_stiffness = 0.9f;
	int local_iterations = 2;
	int length_iterations = 2;
	float vsp_coeff = 0.8f;
	float vsp_accel_threshold = 50.0f;
	float max_speed = 12.0f;
	bool use_world_gravity = true;
	Vector3 fixed_gravity = Vector3(0, -9.8f, 0);
	Vector3 wind;
	bool weather_wind = true; // add the WeatherMap3D wind when there is one
	float gust = 0.3f;
	float turbulence = 0.35f;
	float turbulence_size = 0.08f;
	float guide_ratio = 0.1f;
	PackedVector4Array collision_spheres; // this node's space: centre, radius
	NodePath collision_body;
	float collision_cell_size = 0.01f;
	bool deep_shadows = true;
	NodePath deep_shadow_light;
	ObjectID deep_shadow_light_cache; // the light found (deep_shadow_light, or the first shadowed DirectionalLight3D)
	uint64_t deep_shadow_search_frame = 0;
	int deep_shadow_local_lights = 3; // omni and spot lights with their own map, at most
	LocalVector<ObjectID> deep_shadow_local_cache; // shadowed omni and spot lights found by the search
	uint64_t deep_shadow_local_search_frame = 0;
	int deep_shadow_on = 0; // deep opacity maps last set on the material
	int draw_slot_points = 1;
	bool volume_enabled = true;
	float volume_friction = 0.2f;
	float volume_stiffness = 1.0f;
	bool strand_collision = true;
	float strand_collision_radius = 0.002f;
	float strand_collision_friction = 0.3f;
	// Wet hair (Ward et al. 2004; Gupta and Magnenat-Thalmann 2007): 0 dry, 1 soaked.
	float wetness = 0.0f;
	bool weather_wetting = true; // rain from the WeatherMap3D wets the hair; it dries without
	float water_capacity = 1.0f; // mm of rain that soak the hair
	float drying_time = 3600.0f; // s from soaked to dry
	float wet_clumping = 0.8f; // how far wet strands gather onto their guides (0..1)
	float wet_look = 0.0f; // the wetness last set on the material
	float rain_shelter = 1.0f; // share of the rain that reaches the hair (roofs above it)
	float rain_shelter_timer = 0.0f;
	// The collision body's mesh, cached: positions, triangle indices, skinning (4 bones each).
	struct BodyCache {
		ObjectID mesh_instance;
		RID mesh;
		PackedVector3Array verts;
		PackedInt32Array indices;
		PackedInt32Array bones;
		PackedFloat32Array weights;
		int per_vertex = 0;
	} body_cache;
	PackedVector3Array skinned;

	HairStrandSolver *solver = nullptr;
	MeshInstance3D *render_instance = nullptr;
	Ref<ShaderMaterial> copy_material;
	Ref<Texture2DRD> pos_texture;
	Ref<Texture2DRD> prev_pos_texture; // last frame's positions, for motion vectors
	Ref<Texture2DRD> deep_shadow_texture; // the deep opacity map
	bool built = false;
	bool build_pending = false;

	static Ref<Shader> render_shaders[2]; // Godot standard lighting, hair shading

	void _groom_changed();
	void _queue_build();
	void _build();
	void _clear();
	void _make_material();
	void _step(double p_delta);
	Vector3 _fibre_absorption() const;
	Vector2 _fibre_roughness() const;
	void _update_wetness(double p_delta);
	PackedFloat32Array _follow_data(const PackedVector3Array &p_points, const PackedInt32Array &p_counts) const;
	int _render_points(const PackedVector3Array &p_points, const PackedInt32Array &p_counts, PackedInt32Array &r_points) const;
	Color _mass_color() const;
	void _body_triangles(PackedVector3Array &r_tris);
	class DirectionalLight3D *_deep_shadow_light();
	void _deep_shadow_local_lights(const Vector3 &p_center, float p_radius, uint32_t p_layers, LocalVector<class Light3D *> &r_lights);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	static void free_shared();

	void set_groom(const Ref<HairGroom> &p_groom);
	Ref<HairGroom> get_groom() const;
	void set_root_width(float p_width);
	float get_root_width() const;
	void set_simplify_tolerance(float p_tolerance);
	float get_simplify_tolerance() const;
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
	void set_hair_shading(bool p_enabled);
	bool is_hair_shading() const;
	void set_lod_enabled(bool p_enabled);
	bool is_lod_enabled() const;
	void set_lod_start_distance(float p_distance);
	float get_lod_start_distance() const;
	void set_lod_end_distance(float p_distance);
	float get_lod_end_distance() const;
	void set_lod_strand_ratio(float p_ratio);
	float get_lod_strand_ratio() const;
	void set_render_material(const Ref<ShaderMaterial> &p_material);
	Ref<ShaderMaterial> get_render_material() const;

	void set_simulating(bool p_simulating);
	bool is_simulating() const;
	void set_substeps(int p_substeps);
	int get_substeps() const;
	void set_damping(float p_damping);
	float get_damping() const;
	void set_global_stiffness(float p_value);
	float get_global_stiffness() const;
	void set_global_range(float p_value);
	float get_global_range() const;
	void set_local_stiffness(float p_value);
	float get_local_stiffness() const;
	void set_local_iterations(int p_value);
	int get_local_iterations() const;
	void set_length_iterations(int p_value);
	int get_length_iterations() const;
	void set_vsp_coeff(float p_value);
	float get_vsp_coeff() const;
	void set_vsp_accel_threshold(float p_value);
	float get_vsp_accel_threshold() const;
	void set_max_speed(float p_value);
	float get_max_speed() const;
	void set_use_world_gravity(bool p_use);
	bool is_using_world_gravity() const;
	void set_fixed_gravity(const Vector3 &p_gravity);
	Vector3 get_fixed_gravity() const;
	void set_weather_wind(bool p_enabled);
	bool get_weather_wind() const;
	void set_wind(const Vector3 &p_wind);
	Vector3 get_wind() const;
	void set_gust(float p_gust);
	float get_gust() const;
	void set_guide_ratio(float p_ratio);
	float get_guide_ratio() const;
	void set_turbulence(float p_turbulence);
	float get_turbulence() const;
	void set_turbulence_size(float p_size);
	float get_turbulence_size() const;
	void set_collision_body(const NodePath &p_path);
	NodePath get_collision_body() const;
	void set_collision_cell_size(float p_size);
	float get_collision_cell_size() const;
	void set_deep_shadows(bool p_enabled);
	bool is_deep_shadows() const;
	void set_deep_shadow_light(const NodePath &p_path);
	NodePath get_deep_shadow_light() const;
	void set_deep_shadow_local_lights(int p_count);
	int get_deep_shadow_local_lights() const;
	void set_volume_enabled(bool p_enabled);
	bool is_volume_enabled() const;
	void set_volume_friction(float p_friction);
	float get_volume_friction() const;
	void set_volume_stiffness(float p_stiffness);
	void set_strand_collision(bool p_enabled);
	bool is_strand_collision() const;
	void set_strand_collision_radius(float p_radius);
	float get_strand_collision_radius() const;
	void set_strand_collision_friction(float p_friction);
	float get_strand_collision_friction() const;
	float get_volume_stiffness() const;
	void set_wetness(float p_wetness);
	float get_wetness() const;
	void set_weather_wetting(bool p_enabled);
	bool get_weather_wetting() const;
	void set_water_capacity(float p_mm);
	float get_water_capacity() const;
	void set_drying_time(float p_seconds);
	float get_drying_time() const;
	void set_wet_clumping(float p_clumping);
	float get_wet_clumping() const;
	void set_collision_spheres(const PackedVector4Array &p_spheres);
	PackedVector4Array get_collision_spheres() const;

	void reset_simulation();
	MeshInstance3D *get_render_instance() const;

	~PhysXHair3D();
};

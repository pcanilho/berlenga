/**************************************************************************/
/*  physx_hair_3d.cpp                                                     */
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

#include "physx_hair_3d.h"

#include "core/config/engine.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/resources/3d/world_3d.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"
#include "scene/resources/texture_rd.h"
#include "servers/physics_3d/physics_server_3d.h"

namespace {

// Drawn from PhysXHair3D's simulated points. Every vertex is one side of one of the two crossed strips
// around a strand point: CUSTOM0 = (point, previous point, next point, signed half width), CUSTOM1 =
// (the groomed strip axis, which strip). The strips are rebuilt around the simulated strand, as
// HairMesh lays them out (UV.x across, UV.y along), so the path tracer's hair_strands shading reads
// the fibre offset the same way. rt_vertex_dynamic: the path tracer recaptures it every frame.
const char *HAIR_RENDER_SHADER = R"(
shader_type spatial;
render_mode cull_disabled, rt_vertex_dynamic, hair_strands;

uniform sampler2D hair_positions : filter_nearest, repeat_disable;
uniform int texture_width = 1;
uniform vec4 hair_color : source_color = vec4(0.3, 0.2, 0.1, 1.0);
uniform vec3 fibre_absorption = vec3(0.4, 0.7, 1.4);
uniform vec2 fibre_roughness = vec2(0.3, 0.3);

vec3 hair_point(float p_index) {
	int i = int(p_index + 0.5);
	return texelFetch(hair_positions, ivec2(i % texture_width, i / texture_width), 0).xyz;
}

void vertex() {
	vec3 p = hair_point(CUSTOM0.x);
	vec3 t = hair_point(CUSTOM0.z) - hair_point(CUSTOM0.y);
	t = dot(t, t) > 1e-14 ? normalize(t) : vec3(0.0, 1.0, 0.0);
	vec3 a = CUSTOM1.xyz - t * dot(CUSTOM1.xyz, t);
	if (dot(a, a) < 1e-8) {
		a = cross(t, abs(t.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0));
	}
	a = normalize(a);
	vec3 across = CUSTOM1.w > 0.5 ? cross(t, a) : a;
	VERTEX = p + across * CUSTOM0.w;
	NORMAL = cross(across, t);
	TANGENT = across;
	BINORMAL = t;
}

void fragment() {
	ALBEDO = hair_color.rgb;
	ROUGHNESS = fibre_roughness.x;
	ANISOTROPY = 0.8;
	ANISOTROPY_FLOW = vec2(1.0, 0.0); // The highlight runs across the strands.
	HAIR = 1.0;
	HAIR_ABSORPTION = fibre_absorption;
	HAIR_ROUGHNESS = fibre_roughness;
}
)";

} // namespace

Ref<Shader> PhysXHair3D::render_shader;

void PhysXHair3D::free_shared() {
	render_shader.unref();
	HairStrandSolverGPU::free_shared();
}

// Melanin's absorption per fibre diameter, as BaseMaterial3D's Hair (Cycles' eumelanin and
// pheomelanin coefficients).
Vector3 PhysXHair3D::_fibre_absorption() const {
	const float m = -Math::log(MAX(1.0f - melanin, 1e-4f));
	const Vector3 eu(0.506f, 0.841f, 1.653f);
	const Vector3 pheo(0.343f, 0.733f, 1.924f);
	return (eu * (1.0f - melanin_redness) + pheo * melanin_redness) * m;
}

// The colour a mass of such fibres shows (Chiang et al. 2016, section 4.2), for the rasterizer.
Color PhysXHair3D::_mass_color() const {
	const float b = CLAMP(radial_roughness, 0.01f, 1.0f);
	const float d = 5.969f - 0.215f * b + 2.532f * b * b - 10.73f * b * b * b + 5.574f * b * b * b * b + 0.245f * b * b * b * b * b;
	const Vector3 s = _fibre_absorption();
	return Color(Math::exp(-d * Math::sqrt(s.x)), Math::exp(-d * Math::sqrt(s.y)), Math::exp(-d * Math::sqrt(s.z))).linear_to_srgb();
}

void PhysXHair3D::_make_material() {
	if (copy_material.is_valid()) {
		copy_material->set_shader_parameter("hair_positions", Variant());
	}
	if (render_material.is_valid() && render_material->get_shader().is_valid()) {
		copy_material = render_material->duplicate();
	} else {
		if (render_shader.is_null()) {
			render_shader.instantiate();
			render_shader->set_code(HAIR_RENDER_SHADER);
		}
		copy_material.instantiate();
		copy_material->set_shader(render_shader);
	}
	copy_material->set_shader_parameter("hair_color", _mass_color());
	copy_material->set_shader_parameter("fibre_absorption", _fibre_absorption());
	copy_material->set_shader_parameter("fibre_roughness", Vector2(roughness, radial_roughness));
	copy_material->set_shader_parameter("hair_positions", pos_texture);
	copy_material->set_shader_parameter("texture_width", solver ? solver->get_texture_width() : 1);
	if (render_instance) {
		render_instance->set_material_override(copy_material);
	}
}

void PhysXHair3D::_groom_changed() {
	_queue_build();
}

void PhysXHair3D::_queue_build() {
	if (is_inside_tree()) {
		build_pending = true;
	}
}

void PhysXHair3D::_clear() {
	if (render_instance) {
		render_instance->queue_free();
		render_instance = nullptr;
	}
	if (copy_material.is_valid()) {
		copy_material->set_shader_parameter("hair_positions", Variant());
		copy_material.unref();
	}
	if (pos_texture.is_valid()) {
		pos_texture->set_texture_rd_rid(RID());
		pos_texture.unref();
	}
	if (solver != nullptr) {
		memdelete(solver);
		solver = nullptr;
	}
	built = false;
}

void PhysXHair3D::_build() {
	build_pending = false;
	_clear();
	if (groom.is_null() || !groom->is_valid() || groom->get_point_count() == 0) {
		return;
	}
	const bool editor = Engine::get_singleton()->is_editor_hint();

	render_instance = memnew(MeshInstance3D);
	add_child(render_instance, false, INTERNAL_MODE_BACK);

	if (editor) {
		// The groom, still: a HairMesh with BaseMaterial3D's strand hair.
		Ref<HairMesh> preview;
		preview.instantiate();
		preview->set_groom(groom);
		preview->set_root_width(root_width);
		preview->set_tip_width(tip_width);
		Ref<StandardMaterial3D> m;
		m.instantiate();
		m->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
		m->set_feature(BaseMaterial3D::FEATURE_HAIR, true);
		m->set_hair_color_mode(BaseMaterial3D::HAIR_COLOR_MELANIN);
		m->set_hair_melanin(melanin);
		m->set_hair_melanin_redness(melanin_redness);
		m->set_hair_roughness(roughness);
		m->set_hair_radial_roughness(radial_roughness);
		m->set_hair_direction(BaseMaterial3D::HAIR_DIRECTION_STRANDS);
		preview->set_material(m);
		render_instance->set_mesh(preview);
		built = true;
		return;
	}

	const PackedVector3Array pts = groom->get_points();
	const PackedInt32Array counts = groom->get_strand_point_counts();

	// Solver input: rest points with their segment lengths, strand ranges.
	PackedFloat32Array rest;
	rest.resize(pts.size() * 4);
	PackedInt32Array strands;
	strands.resize(counts.size() * 2);
	{
		float *r = rest.ptrw();
		int *s = strands.ptrw();
		int start = 0;
		for (int i = 0; i < counts.size(); i++) {
			const int n = counts[i];
			s[i * 2] = start;
			s[i * 2 + 1] = n;
			for (int k = 0; k < n; k++) {
				const Vector3 p = pts[start + k];
				r[(start + k) * 4 + 0] = p.x;
				r[(start + k) * 4 + 1] = p.y;
				r[(start + k) * 4 + 2] = p.z;
				r[(start + k) * 4 + 3] = k > 0 ? p.distance_to(pts[start + k - 1]) : 0.0f;
			}
			start += n;
		}
	}

	// Render mesh: two crossed strips per point, as HairMesh, the positions rebuilt in vertex().
	PackedVector3Array verts;
	PackedVector3Array normals;
	PackedFloat32Array tangents;
	PackedVector2Array uvs;
	PackedFloat32Array custom0;
	PackedFloat32Array custom1;
	PackedInt32Array indices;
	AABB bounds(pts[0], Vector3());
	float longest = 0.0f;
	int start = 0;
	for (int c = 0; c < counts.size(); c++) {
		const int n = counts[c];
		if (n < 2) {
			start += MAX(n, 0);
			continue;
		}
		float length = 0.0f;
		for (int k = 1; k < n; k++) {
			length += pts[start + k].distance_to(pts[start + k - 1]);
		}
		longest = MAX(longest, length);
		Vector3 prev_t = (pts[start + 1] - pts[start]).normalized();
		Vector3 a = prev_t.cross(Math::abs(prev_t.y) < 0.9f ? Vector3(0, 1, 0) : Vector3(1, 0, 0)).normalized();
		const int base = verts.size();
		float along = 0.0f;
		for (int k = 0; k < n; k++) {
			const int ip = start + MAX(k - 1, 0);
			const int in = start + MIN(k + 1, n - 1);
			Vector3 t = pts[in] - pts[ip];
			t = t.length_squared() > 1e-16f ? t.normalized() : prev_t;
			const Vector3 axis = prev_t.cross(t);
			if (axis.length_squared() > 1e-12f) {
				a = a.rotated(axis.normalized(), prev_t.angle_to(t));
			}
			a = (a - t * a.dot(t)).normalized();
			prev_t = t;
			if (k > 0) {
				along += pts[start + k].distance_to(pts[start + k - 1]);
			}
			const float s = along / MAX(length, 1e-6f);
			const float half = 0.5f * Math::lerp(root_width, tip_width, s);
			bounds.expand_to(pts[start + k]);
			for (int strip = 0; strip < 2; strip++) {
				const Vector3 across = strip == 0 ? a : t.cross(a);
				for (int side = 0; side < 2; side++) {
					verts.push_back(pts[start + k] + across * (side == 0 ? -half : half));
					normals.push_back(across.cross(t));
					tangents.push_back(across.x);
					tangents.push_back(across.y);
					tangents.push_back(across.z);
					tangents.push_back(1.0f);
					uvs.push_back(Vector2(float(side), s));
					custom0.push_back(float(start + k));
					custom0.push_back(float(ip));
					custom0.push_back(float(in));
					custom0.push_back(side == 0 ? -half : half);
					custom1.push_back(a.x);
					custom1.push_back(a.y);
					custom1.push_back(a.z);
					custom1.push_back(float(strip));
				}
			}
		}
		for (int k = 0; k + 1 < n; k++) {
			for (int strip = 0; strip < 2; strip++) {
				const int v0 = base + k * 4 + strip * 2;
				const int v2 = v0 + 4;
				indices.push_back(v0);
				indices.push_back(v0 + 1);
				indices.push_back(v2);
				indices.push_back(v0 + 1);
				indices.push_back(v2 + 1);
				indices.push_back(v2);
			}
		}
		start += n;
	}
	if (verts.is_empty()) {
		render_instance->queue_free();
		render_instance = nullptr;
		return;
	}

	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = verts;
	arrays[Mesh::ARRAY_NORMAL] = normals;
	arrays[Mesh::ARRAY_TANGENT] = tangents;
	arrays[Mesh::ARRAY_TEX_UV] = uvs;
	arrays[Mesh::ARRAY_CUSTOM0] = custom0;
	arrays[Mesh::ARRAY_CUSTOM1] = custom1;
	arrays[Mesh::ARRAY_INDEX] = indices;
	const uint64_t format = ((uint64_t)Mesh::ARRAY_CUSTOM_RGBA_FLOAT << Mesh::ARRAY_FORMAT_CUSTOM0_SHIFT) |
			((uint64_t)Mesh::ARRAY_CUSTOM_RGBA_FLOAT << Mesh::ARRAY_FORMAT_CUSTOM1_SHIFT);
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays, Array(), Dictionary(), format);
	render_instance->set_mesh(mesh);
	// Strands swing up to their length away from where they were groomed.
	render_instance->set_custom_aabb(bounds.grow(longest + 0.05f));

	solver = memnew(HairStrandSolver);
	if (!solver->is_available()) {
		memdelete(solver);
		solver = nullptr;
		render_instance->queue_free();
		render_instance = nullptr;
		ERR_FAIL_MSG("PhysXHair3D: no RenderingDevice, the hair can't be simulated.");
	}
	solver->build(rest, strands);
	pos_texture.instantiate();
	pos_texture->set_texture_rd_rid(solver->get_position_texture_rd_rid());
	_make_material();
	built = true;
	_step(0.0);
}

void PhysXHair3D::_step(double p_delta) {
	if (solver == nullptr) {
		return;
	}
	HairStrandSolver::Settings s;
	s.damping = damping;
	s.root_follow = root_follow;
	s.tip_follow = tip_follow;
	s.shape_stiffness = shape_stiffness;
	s.length_damping = length_damping;
	s.gravity = fixed_gravity;
	if (use_world_gravity && is_inside_tree()) {
		const Ref<World3D> w = get_world_3d();
		if (w.is_valid()) {
			PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
			const float mag = ps->area_get_param(w->get_space(), PhysicsServer3D::AREA_PARAM_GRAVITY);
			const Vector3 dir = ps->area_get_param(w->get_space(), PhysicsServer3D::AREA_PARAM_GRAVITY_VECTOR);
			s.gravity = dir * mag;
		}
	}
	s.wind = wind;
	s.gust = gust;
	const Transform3D xform = get_global_transform();
	for (const Vector4 &sp : collision_spheres) {
		const Vector3 c = xform.xform(Vector3(sp.x, sp.y, sp.z));
		const float scale = xform.basis.get_scale().x;
		s.spheres.push_back(Vector4(c.x, c.y, c.z, sp.w * scale));
	}
	s.output_xform = render_instance ? render_instance->get_global_transform().affine_inverse() : xform.affine_inverse();
	solver->step(xform, p_delta, substeps, s);
}

void PhysXHair3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			set_process_internal(true);
			build_pending = true;
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			if (build_pending) {
				_build();
			}
			if (built && solver != nullptr && simulating) {
				_step(get_process_delta_time());
			}
		} break;
		case NOTIFICATION_EXIT_TREE: {
			_clear();
		} break;
	}
}

void PhysXHair3D::reset_simulation() {
	if (solver != nullptr) {
		solver->reset();
	}
}

MeshInstance3D *PhysXHair3D::get_render_instance() const {
	return render_instance;
}

PhysXHair3D::~PhysXHair3D() {
	_clear();
}

// Properties.

void PhysXHair3D::set_groom(const Ref<HairGroom> &p_groom) {
	if (groom == p_groom) {
		return;
	}
	if (groom.is_valid()) {
		groom->disconnect_changed(callable_mp(this, &PhysXHair3D::_groom_changed));
	}
	groom = p_groom;
	if (groom.is_valid()) {
		groom->connect_changed(callable_mp(this, &PhysXHair3D::_groom_changed));
	}
	_queue_build();
}

Ref<HairGroom> PhysXHair3D::get_groom() const {
	return groom;
}

void PhysXHair3D::set_root_width(float p_width) {
	root_width = MAX(p_width, 0.0f);
	_queue_build();
}

float PhysXHair3D::get_root_width() const {
	return root_width;
}

void PhysXHair3D::set_tip_width(float p_width) {
	tip_width = MAX(p_width, 0.0f);
	_queue_build();
}

float PhysXHair3D::get_tip_width() const {
	return tip_width;
}

void PhysXHair3D::set_melanin(float p_melanin) {
	melanin = CLAMP(p_melanin, 0.0f, 1.0f);
	if (built) {
		_make_material();
	}
}

float PhysXHair3D::get_melanin() const {
	return melanin;
}

void PhysXHair3D::set_melanin_redness(float p_redness) {
	melanin_redness = CLAMP(p_redness, 0.0f, 1.0f);
	if (built) {
		_make_material();
	}
}

float PhysXHair3D::get_melanin_redness() const {
	return melanin_redness;
}

void PhysXHair3D::set_roughness(float p_roughness) {
	roughness = CLAMP(p_roughness, 0.0f, 1.0f);
	if (built) {
		_make_material();
	}
}

float PhysXHair3D::get_roughness() const {
	return roughness;
}

void PhysXHair3D::set_radial_roughness(float p_roughness) {
	radial_roughness = CLAMP(p_roughness, 0.0f, 1.0f);
	if (built) {
		_make_material();
	}
}

float PhysXHair3D::get_radial_roughness() const {
	return radial_roughness;
}

void PhysXHair3D::set_render_material(const Ref<ShaderMaterial> &p_material) {
	render_material = p_material;
	if (built && solver != nullptr) {
		_make_material();
	}
}

Ref<ShaderMaterial> PhysXHair3D::get_render_material() const {
	return render_material;
}

void PhysXHair3D::set_simulating(bool p_simulating) {
	simulating = p_simulating;
}

bool PhysXHair3D::is_simulating() const {
	return simulating;
}

void PhysXHair3D::set_substeps(int p_substeps) {
	substeps = CLAMP(p_substeps, 1, 32);
}

int PhysXHair3D::get_substeps() const {
	return substeps;
}

void PhysXHair3D::set_damping(float p_damping) {
	damping = CLAMP(p_damping, 0.0f, 0.999f);
}

float PhysXHair3D::get_damping() const {
	return damping;
}

void PhysXHair3D::set_root_follow(float p_follow) {
	root_follow = CLAMP(p_follow, 0.0f, 1.0f);
}

float PhysXHair3D::get_root_follow() const {
	return root_follow;
}

void PhysXHair3D::set_tip_follow(float p_follow) {
	tip_follow = CLAMP(p_follow, 0.0f, 1.0f);
}

float PhysXHair3D::get_tip_follow() const {
	return tip_follow;
}

void PhysXHair3D::set_shape_stiffness(float p_stiffness) {
	shape_stiffness = CLAMP(p_stiffness, 0.0f, 1.0f);
}

float PhysXHair3D::get_shape_stiffness() const {
	return shape_stiffness;
}

void PhysXHair3D::set_length_damping(float p_damping) {
	length_damping = CLAMP(p_damping, 0.0f, 1.0f);
}

float PhysXHair3D::get_length_damping() const {
	return length_damping;
}

void PhysXHair3D::set_use_world_gravity(bool p_use) {
	use_world_gravity = p_use;
}

bool PhysXHair3D::is_using_world_gravity() const {
	return use_world_gravity;
}

void PhysXHair3D::set_fixed_gravity(const Vector3 &p_gravity) {
	fixed_gravity = p_gravity;
}

Vector3 PhysXHair3D::get_fixed_gravity() const {
	return fixed_gravity;
}

void PhysXHair3D::set_wind(const Vector3 &p_wind) {
	wind = p_wind;
}

Vector3 PhysXHair3D::get_wind() const {
	return wind;
}

void PhysXHair3D::set_gust(float p_gust) {
	gust = CLAMP(p_gust, 0.0f, 1.0f);
}

float PhysXHair3D::get_gust() const {
	return gust;
}

void PhysXHair3D::set_collision_spheres(const PackedVector4Array &p_spheres) {
	collision_spheres = p_spheres;
}

PackedVector4Array PhysXHair3D::get_collision_spheres() const {
	return collision_spheres;
}

void PhysXHair3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_groom", "groom"), &PhysXHair3D::set_groom);
	ClassDB::bind_method(D_METHOD("get_groom"), &PhysXHair3D::get_groom);
	ClassDB::bind_method(D_METHOD("set_root_width", "width"), &PhysXHair3D::set_root_width);
	ClassDB::bind_method(D_METHOD("get_root_width"), &PhysXHair3D::get_root_width);
	ClassDB::bind_method(D_METHOD("set_tip_width", "width"), &PhysXHair3D::set_tip_width);
	ClassDB::bind_method(D_METHOD("get_tip_width"), &PhysXHair3D::get_tip_width);
	ClassDB::bind_method(D_METHOD("set_melanin", "melanin"), &PhysXHair3D::set_melanin);
	ClassDB::bind_method(D_METHOD("get_melanin"), &PhysXHair3D::get_melanin);
	ClassDB::bind_method(D_METHOD("set_melanin_redness", "redness"), &PhysXHair3D::set_melanin_redness);
	ClassDB::bind_method(D_METHOD("get_melanin_redness"), &PhysXHair3D::get_melanin_redness);
	ClassDB::bind_method(D_METHOD("set_roughness", "roughness"), &PhysXHair3D::set_roughness);
	ClassDB::bind_method(D_METHOD("get_roughness"), &PhysXHair3D::get_roughness);
	ClassDB::bind_method(D_METHOD("set_radial_roughness", "roughness"), &PhysXHair3D::set_radial_roughness);
	ClassDB::bind_method(D_METHOD("get_radial_roughness"), &PhysXHair3D::get_radial_roughness);
	ClassDB::bind_method(D_METHOD("set_render_material", "material"), &PhysXHair3D::set_render_material);
	ClassDB::bind_method(D_METHOD("get_render_material"), &PhysXHair3D::get_render_material);
	ClassDB::bind_method(D_METHOD("set_simulating", "simulating"), &PhysXHair3D::set_simulating);
	ClassDB::bind_method(D_METHOD("is_simulating"), &PhysXHair3D::is_simulating);
	ClassDB::bind_method(D_METHOD("set_substeps", "substeps"), &PhysXHair3D::set_substeps);
	ClassDB::bind_method(D_METHOD("get_substeps"), &PhysXHair3D::get_substeps);
	ClassDB::bind_method(D_METHOD("set_damping", "damping"), &PhysXHair3D::set_damping);
	ClassDB::bind_method(D_METHOD("get_damping"), &PhysXHair3D::get_damping);
	ClassDB::bind_method(D_METHOD("set_root_follow", "follow"), &PhysXHair3D::set_root_follow);
	ClassDB::bind_method(D_METHOD("get_root_follow"), &PhysXHair3D::get_root_follow);
	ClassDB::bind_method(D_METHOD("set_tip_follow", "follow"), &PhysXHair3D::set_tip_follow);
	ClassDB::bind_method(D_METHOD("get_tip_follow"), &PhysXHair3D::get_tip_follow);
	ClassDB::bind_method(D_METHOD("set_shape_stiffness", "stiffness"), &PhysXHair3D::set_shape_stiffness);
	ClassDB::bind_method(D_METHOD("get_shape_stiffness"), &PhysXHair3D::get_shape_stiffness);
	ClassDB::bind_method(D_METHOD("set_length_damping", "damping"), &PhysXHair3D::set_length_damping);
	ClassDB::bind_method(D_METHOD("get_length_damping"), &PhysXHair3D::get_length_damping);
	ClassDB::bind_method(D_METHOD("set_use_world_gravity", "enabled"), &PhysXHair3D::set_use_world_gravity);
	ClassDB::bind_method(D_METHOD("is_using_world_gravity"), &PhysXHair3D::is_using_world_gravity);
	ClassDB::bind_method(D_METHOD("set_fixed_gravity", "gravity"), &PhysXHair3D::set_fixed_gravity);
	ClassDB::bind_method(D_METHOD("get_fixed_gravity"), &PhysXHair3D::get_fixed_gravity);
	ClassDB::bind_method(D_METHOD("set_wind", "wind"), &PhysXHair3D::set_wind);
	ClassDB::bind_method(D_METHOD("get_wind"), &PhysXHair3D::get_wind);
	ClassDB::bind_method(D_METHOD("set_gust", "gust"), &PhysXHair3D::set_gust);
	ClassDB::bind_method(D_METHOD("get_gust"), &PhysXHair3D::get_gust);
	ClassDB::bind_method(D_METHOD("set_collision_spheres", "spheres"), &PhysXHair3D::set_collision_spheres);
	ClassDB::bind_method(D_METHOD("get_collision_spheres"), &PhysXHair3D::get_collision_spheres);
	ClassDB::bind_method(D_METHOD("reset_simulation"), &PhysXHair3D::reset_simulation);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "groom", PROPERTY_HINT_RESOURCE_TYPE, HairGroom::get_class_static()), "set_groom", "get_groom");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "root_width", PROPERTY_HINT_RANGE, "0,0.01,0.00001,or_greater,suffix:m"), "set_root_width", "get_root_width");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "tip_width", PROPERTY_HINT_RANGE, "0,0.01,0.00001,or_greater,suffix:m"), "set_tip_width", "get_tip_width");
	ADD_GROUP("Look", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "melanin", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_melanin", "get_melanin");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "melanin_redness", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_melanin_redness", "get_melanin_redness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "roughness", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_roughness", "get_roughness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "radial_roughness", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_radial_roughness", "get_radial_roughness");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "render_material", PROPERTY_HINT_RESOURCE_TYPE, "ShaderMaterial"), "set_render_material", "get_render_material");
	ADD_GROUP("Simulation", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "simulating"), "set_simulating", "is_simulating");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "substeps", PROPERTY_HINT_RANGE, "1,32,1"), "set_substeps", "get_substeps");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "damping", PROPERTY_HINT_RANGE, "0,0.999,0.001"), "set_damping", "get_damping");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "root_follow", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_root_follow", "get_root_follow");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "tip_follow", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_tip_follow", "get_tip_follow");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "shape_stiffness", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_shape_stiffness", "get_shape_stiffness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "length_damping", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_length_damping", "get_length_damping");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "use_world_gravity"), "set_use_world_gravity", "is_using_world_gravity");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "fixed_gravity", PROPERTY_HINT_NONE, "suffix:m/s²"), "set_fixed_gravity", "get_fixed_gravity");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "wind", PROPERTY_HINT_NONE, "suffix:m/s²"), "set_wind", "get_wind");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "gust", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_gust", "get_gust");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_VECTOR4_ARRAY, "collision_spheres"), "set_collision_spheres", "get_collision_spheres");
}

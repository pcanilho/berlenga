/**************************************************************************/
/*  physx_buoyancy_3d.cpp                                                 */
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

#include "physx_buoyancy_3d.h"

#include "physx_river_3d.h"
#include "physx_water_surface_3d.h"
#include "physx_water_wake_3d.h"

#include "core/config/engine.h"
#include "core/object/class_db.h"
#include "scene/3d/physics/collision_shape_3d.h"
#include "scene/3d/physics/rigid_body_3d.h"
#include "scene/resources/3d/box_shape_3d.h"
#include "scene/resources/3d/capsule_shape_3d.h"
#include "scene/resources/3d/cylinder_shape_3d.h"
#include "scene/resources/3d/shape_3d.h"
#include "scene/resources/3d/sphere_shape_3d.h"
#include "scene/resources/mesh.h"

void PhysXBuoyancy3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_water_surface_path", "path"), &PhysXBuoyancy3D::set_water_surface_path);
	ClassDB::bind_method(D_METHOD("get_water_surface_path"), &PhysXBuoyancy3D::get_water_surface_path);
	ClassDB::bind_method(D_METHOD("set_sample_points", "points"), &PhysXBuoyancy3D::set_sample_points);
	ClassDB::bind_method(D_METHOD("get_sample_points"), &PhysXBuoyancy3D::get_sample_points);
	ClassDB::bind_method(D_METHOD("set_hull_area", "area"), &PhysXBuoyancy3D::set_hull_area);
	ClassDB::bind_method(D_METHOD("get_hull_area"), &PhysXBuoyancy3D::get_hull_area);
	ClassDB::bind_method(D_METHOD("set_hull_height", "height"), &PhysXBuoyancy3D::set_hull_height);
	ClassDB::bind_method(D_METHOD("get_hull_height"), &PhysXBuoyancy3D::get_hull_height);
	ClassDB::bind_method(D_METHOD("set_water_density", "density"), &PhysXBuoyancy3D::set_water_density);
	ClassDB::bind_method(D_METHOD("get_water_density"), &PhysXBuoyancy3D::get_water_density);
	ClassDB::bind_method(D_METHOD("set_linear_drag", "drag"), &PhysXBuoyancy3D::set_linear_drag);
	ClassDB::bind_method(D_METHOD("get_linear_drag"), &PhysXBuoyancy3D::get_linear_drag);
	ClassDB::bind_method(D_METHOD("set_angular_drag", "drag"), &PhysXBuoyancy3D::set_angular_drag);
	ClassDB::bind_method(D_METHOD("get_angular_drag"), &PhysXBuoyancy3D::get_angular_drag);
	ClassDB::bind_method(D_METHOD("set_disturb_water", "enabled"), &PhysXBuoyancy3D::set_disturb_water);
	ClassDB::bind_method(D_METHOD("get_disturb_water"), &PhysXBuoyancy3D::get_disturb_water);
	ClassDB::bind_method(D_METHOD("set_ripple_radius", "radius"), &PhysXBuoyancy3D::set_ripple_radius);
	ClassDB::bind_method(D_METHOD("get_ripple_radius"), &PhysXBuoyancy3D::get_ripple_radius);
	ClassDB::bind_method(D_METHOD("set_ripple_rest_strength", "strength"), &PhysXBuoyancy3D::set_ripple_rest_strength);
	ClassDB::bind_method(D_METHOD("get_ripple_rest_strength"), &PhysXBuoyancy3D::get_ripple_rest_strength);
	ClassDB::bind_method(D_METHOD("set_ripple_reference_speed", "speed"), &PhysXBuoyancy3D::set_ripple_reference_speed);
	ClassDB::bind_method(D_METHOD("get_ripple_reference_speed"), &PhysXBuoyancy3D::get_ripple_reference_speed);
	ClassDB::bind_method(D_METHOD("get_submerged_fraction"), &PhysXBuoyancy3D::get_submerged_fraction);
	ClassDB::bind_method(D_METHOD("get_point_depths"), &PhysXBuoyancy3D::get_point_depths);
	ClassDB::bind_method(D_METHOD("get_effective_sample_points"), &PhysXBuoyancy3D::get_effective_sample_points);
	ClassDB::bind_method(D_METHOD("get_water_height", "world_pos"), &PhysXBuoyancy3D::get_water_height);
	ClassDB::bind_method(D_METHOD("set_volume_mode", "mode"), &PhysXBuoyancy3D::set_volume_mode);
	ClassDB::bind_method(D_METHOD("get_volume_mode"), &PhysXBuoyancy3D::get_volume_mode);
	ClassDB::bind_method(D_METHOD("set_max_cells", "cells"), &PhysXBuoyancy3D::set_max_cells);
	ClassDB::bind_method(D_METHOD("get_max_cells"), &PhysXBuoyancy3D::get_max_cells);
	ClassDB::bind_method(D_METHOD("set_mass_from_densities", "enabled"), &PhysXBuoyancy3D::set_mass_from_densities);
	ClassDB::bind_method(D_METHOD("get_mass_from_densities"), &PhysXBuoyancy3D::get_mass_from_densities);
	ClassDB::bind_method(D_METHOD("set_default_density", "density"), &PhysXBuoyancy3D::set_default_density);
	ClassDB::bind_method(D_METHOD("get_default_density"), &PhysXBuoyancy3D::get_default_density);
	ClassDB::bind_method(D_METHOD("get_total_volume"), &PhysXBuoyancy3D::get_total_volume);
	ClassDB::bind_method(D_METHOD("set_wave_damping", "ratio"), &PhysXBuoyancy3D::set_wave_damping);
	ClassDB::bind_method(D_METHOD("get_wave_damping"), &PhysXBuoyancy3D::get_wave_damping);
	ClassDB::bind_method(D_METHOD("set_drag_coefficient", "cd"), &PhysXBuoyancy3D::set_drag_coefficient);
	ClassDB::bind_method(D_METHOD("get_drag_coefficient"), &PhysXBuoyancy3D::get_drag_coefficient);

	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "water_surface_path", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "PhysXWaterSurface3D,PhysXRiver3D"), "set_water_surface_path", "get_water_surface_path");
	ADD_GROUP("Volume", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "volume_mode", PROPERTY_HINT_ENUM, "Points,Shapes"), "set_volume_mode", "get_volume_mode");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_cells", PROPERTY_HINT_RANGE, "1,256,1"), "set_max_cells", "get_max_cells");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "mass_from_densities"), "set_mass_from_densities", "get_mass_from_densities");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "default_density", PROPERTY_HINT_RANGE, "0,20000,1,suffix:kg/m³"), "set_default_density", "get_default_density");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "drag_coefficient", PROPERTY_HINT_RANGE, "0,3,0.01"), "set_drag_coefficient", "get_drag_coefficient");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wave_damping", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_wave_damping", "get_wave_damping");
	BIND_ENUM_CONSTANT(VOLUME_POINTS);
	BIND_ENUM_CONSTANT(VOLUME_SHAPES);
	ADD_GROUP("Hull", "");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_VECTOR3_ARRAY, "sample_points"), "set_sample_points", "get_sample_points");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "hull_area", PROPERTY_HINT_RANGE, "0,100,0.01,or_greater,suffix:m²"), "set_hull_area", "get_hull_area");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "hull_height", PROPERTY_HINT_RANGE, "0,10,0.01,or_greater,suffix:m"), "set_hull_height", "get_hull_height");
	ADD_GROUP("Water", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "water_density", PROPERTY_HINT_RANGE, "1,2000,1,or_greater,suffix:kg/m³"), "set_water_density", "get_water_density");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "linear_drag", PROPERTY_HINT_RANGE, "0,1000,0.1,or_greater"), "set_linear_drag", "get_linear_drag");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "angular_drag", PROPERTY_HINT_RANGE, "0,1000,0.1,or_greater"), "set_angular_drag", "get_angular_drag");
	ADD_GROUP("Ripples", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "disturb_water"), "set_disturb_water", "get_disturb_water");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ripple_radius", PROPERTY_HINT_RANGE, "0,20,0.01,or_greater,suffix:m"), "set_ripple_radius", "get_ripple_radius");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ripple_rest_strength", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_ripple_rest_strength", "get_ripple_rest_strength");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ripple_reference_speed", PROPERTY_HINT_RANGE, "0.01,50,0.01,or_greater,suffix:m/s"), "set_ripple_reference_speed", "get_ripple_reference_speed");
}

RigidBody3D *PhysXBuoyancy3D::get_body() const {
	return Object::cast_to<RigidBody3D>(get_parent());
}

PhysXWaterSurface3D *PhysXBuoyancy3D::get_water_surface() const {
	return ObjectDB::get_instance<PhysXWaterSurface3D>(water_id);
}

PhysXRiver3D *PhysXBuoyancy3D::get_river() const {
	return ObjectDB::get_instance<PhysXRiver3D>(river_id);
}

bool PhysXBuoyancy3D::_has_water() const {
	return get_water_surface() != nullptr || get_river() != nullptr;
}

float PhysXBuoyancy3D::_height_at(const Vector3 &p_world_pos) const {
	if (PhysXWaterSurface3D *water = get_water_surface()) {
		return water->sample_height_excluding_wake(p_world_pos, wake_id);
	}
	if (PhysXRiver3D *river = get_river()) {
		return river->sample_height(p_world_pos);
	}
	return -Math::INF;
}

Vector3 PhysXBuoyancy3D::_flow_at(const Vector3 &p_world_pos) const {
	if (PhysXWaterSurface3D *water = get_water_surface()) {
		return water->sample_flow(p_world_pos);
	}
	if (PhysXRiver3D *river = get_river()) {
		return river->sample_flow(p_world_pos);
	}
	return Vector3();
}

float PhysXBuoyancy3D::get_water_height(const Vector3 &p_world_pos) const {
	return _height_at(p_world_pos);
}

void PhysXBuoyancy3D::_resolve() {
	PhysXWaterSurface3D *water = Object::cast_to<PhysXWaterSurface3D>(get_node_or_null(water_surface_path));
	water_id = water != nullptr ? water->get_instance_id() : ObjectID();
	PhysXRiver3D *river = Object::cast_to<PhysXRiver3D>(get_node_or_null(water_surface_path));
	river_id = river != nullptr ? river->get_instance_id() : ObjectID();
	_find_wake();

	// Hull bounds from the body's collision shapes, in the body's space.
	AABB bounds;
	bool have_bounds = false;
	RigidBody3D *body = get_body();
	if (body != nullptr) {
		for (int i = 0; i < body->get_child_count(); i++) {
			CollisionShape3D *cs = Object::cast_to<CollisionShape3D>(body->get_child(i));
			if (cs == nullptr || cs->is_disabled() || cs->get_shape().is_null()) {
				continue;
			}
			Ref<ArrayMesh> debug = cs->get_shape()->get_debug_mesh();
			if (debug.is_null()) {
				continue;
			}
			const AABB box = cs->get_transform().xform(debug->get_aabb());
			bounds = have_bounds ? bounds.merge(box) : box;
			have_bounds = true;
		}
	}
	if (!have_bounds) {
		bounds = AABB(Vector3(-0.5, -0.5, -0.5), Vector3(1, 1, 1));
	}

	// Default points: the bottom of the hull bounds, inset a little, three
	// along the long axis and two across -- enough to pitch and roll.
	points = sample_points;
	if (points.is_empty()) {
		const bool long_z = bounds.size.z >= bounds.size.x;
		const Vector3 inset = bounds.size * 0.15f;
		const float y = bounds.position.y;
		for (int l = 0; l < 3; l++) {
			for (int a = 0; a < 2; a++) {
				const float tl = l / 2.0f;
				const float ta = (float)a;
				const float x = long_z ? Math::lerp(bounds.position.x + inset.x, bounds.get_end().x - inset.x, ta) : Math::lerp(bounds.position.x + inset.x, bounds.get_end().x - inset.x, tl);
				const float z = long_z ? Math::lerp(bounds.position.z + inset.z, bounds.get_end().z - inset.z, tl) : Math::lerp(bounds.position.z + inset.z, bounds.get_end().z - inset.z, ta);
				points.push_back(Vector3(x, y, z));
			}
		}
	}
	// A hull isn't a box: about three quarters of its bounding footprint.
	area = hull_area > 0.0f ? hull_area : MAX(bounds.size.x * bounds.size.z * 0.75f, 0.01f);
	height = hull_height > 0.0f ? hull_height : MAX(bounds.size.y, 0.01f);
	radius = ripple_radius > 0.0f ? ripple_radius : MAX(Math::sqrt(bounds.size.x * bounds.size.z) * 0.5f, 0.1f);
	point_depths.resize(points.size());
	point_depths.fill(-Math::INF);
	cells.clear();
	total_volume = 0.0f;
	if (volume_mode == VOLUME_SHAPES && body != nullptr) {
		_build_cells(body);
		point_depths.resize(cells.size());
		point_depths.fill(-Math::INF);
	}
}

// The body's collision shapes as its volume (docs/ideas/water.md "Buoyancy from shapes with densities"):
// spheres are pontoons, boxes, cylinders and capsules are cut into cells (more along their long axis), other
// shapes are their bounding box at three quarters' fill. Each shape's density (buoyancy_density metadata on its
// CollisionShape3D, 0 for air) gives the body its mass with mass_from_densities.
void PhysXBuoyancy3D::_build_cells(RigidBody3D *p_body) {
	struct Part {
		Transform3D xf;
		Ref<Shape3D> shape;
		AABB aabb; // the shape's own bounds (for other shapes)
		float density = -1.0f;
	};
	LocalVector<Part> parts;
	for (int i = 0; i < p_body->get_child_count(); i++) {
		CollisionShape3D *cs = Object::cast_to<CollisionShape3D>(p_body->get_child(i));
		if (cs == nullptr || cs->is_disabled() || cs->get_shape().is_null()) {
			continue;
		}
		Part part;
		part.xf = cs->get_transform();
		part.shape = cs->get_shape();
		Ref<ArrayMesh> debug = part.shape->get_debug_mesh();
		part.aabb = debug.is_valid() ? debug->get_aabb() : AABB(Vector3(-0.5, -0.5, -0.5), Vector3(1, 1, 1));
		part.density = cs->has_meta(SNAME("buoyancy_density")) ? (float)cs->get_meta(SNAME("buoyancy_density")) : -1.0f;
		parts.push_back(part);
	}
	if (parts.is_empty()) {
		return;
	}
	const int spheres = [&]() {
		int n = 0;
		for (const Part &part : parts) {
			n += Object::cast_to<SphereShape3D>(part.shape.ptr()) != nullptr ? 1 : 0;
		}
		return n;
	}();
	const int budget = MAX(1, (max_cells - spheres) / MAX((int)parts.size() - spheres, 1));
	float mass = 0.0f;
	Vector3 moment;
	for (const Part &part : parts) {
		const Basis &b = part.xf.basis;
		const float scale = MAX(MAX(b.get_column(0).length(), b.get_column(1).length()), b.get_column(2).length());
		float shape_volume = 0.0f;
		const uint32_t first = cells.size();
		if (const SphereShape3D *sphere = Object::cast_to<SphereShape3D>(part.shape.ptr())) {
			Cell c;
			c.local = part.xf.origin;
			c.radius = sphere->get_radius() * scale;
			c.volume = 4.0f / 3.0f * (float)Math::PI * c.radius * c.radius * c.radius;
			cells.push_back(c);
			shape_volume = c.volume;
		} else {
			// The shape's box, its exact volume and an inside test for cells.
			Vector3 size = part.aabb.size;
			Vector3 centre = part.aabb.get_center();
			float exact = 0.0f;
			int kind = 0; // 0 box (all cells), 1 cylinder (round in xz), 2 capsule
			float radius = 0.0f;
			float half_h = 0.0f;
			if (const BoxShape3D *box = Object::cast_to<BoxShape3D>(part.shape.ptr())) {
				size = box->get_size();
				centre = Vector3();
				exact = size.x * size.y * size.z;
			} else if (const CylinderShape3D *cyl = Object::cast_to<CylinderShape3D>(part.shape.ptr())) {
				radius = cyl->get_radius();
				half_h = 0.5f * cyl->get_height();
				size = Vector3(2.0f * radius, 2.0f * half_h, 2.0f * radius);
				centre = Vector3();
				exact = (float)Math::PI * radius * radius * 2.0f * half_h;
				kind = 1;
			} else if (const CapsuleShape3D *cap = Object::cast_to<CapsuleShape3D>(part.shape.ptr())) {
				radius = cap->get_radius();
				half_h = 0.5f * MAX(cap->get_height(), 2.0f * radius);
				size = Vector3(2.0f * radius, 2.0f * half_h, 2.0f * radius);
				centre = Vector3();
				exact = (float)Math::PI * radius * radius * (2.0f * half_h - 2.0f * radius) + 4.0f / 3.0f * (float)Math::PI * radius * radius * radius;
				kind = 2;
			} else {
				exact = 0.75f * size.x * size.y * size.z;
			}
			// Cells about cube shaped: n along each axis in proportion to its length, n.x n.y n.z <= budget.
			const float cell = Math::pow(MAX(size.x * size.y * size.z, 1e-9f) / budget, 1.0f / 3.0f);
			int n[3];
			for (int a = 0; a < 3; a++) {
				n[a] = CLAMP((int)Math::round(size[a] / MAX(cell, 1e-4f)), 1, budget);
			}
			while (n[0] * n[1] * n[2] > budget) {
				const int a = (size[0] / n[0] <= size[1] / n[1] && size[0] / n[0] <= size[2] / n[2]) ? 0 : (size[1] / n[1] <= size[2] / n[2] ? 1 : 2);
				if (n[a] <= 1) {
					break;
				}
				n[a]--;
			}
			const Vector3 step(size.x / n[0], size.y / n[1], size.z / n[2]);
			for (int x = 0; x < n[0]; x++) {
				for (int y = 0; y < n[1]; y++) {
					for (int z = 0; z < n[2]; z++) {
						const Vector3 p = centre - 0.5f * size + Vector3((x + 0.5f) * step.x, (y + 0.5f) * step.y, (z + 0.5f) * step.z);
						bool inside = true;
						if (kind == 1) {
							inside = p.x * p.x + p.z * p.z <= radius * radius;
						} else if (kind == 2) {
							const float yy = MAX(Math::abs(p.y) - (half_h - radius), 0.0f);
							inside = p.x * p.x + p.z * p.z + yy * yy <= radius * radius;
						}
						if (!inside) {
							continue;
						}
						Cell c;
						c.local = part.xf.xform(p);
						c.axis[0] = b.get_column(0) * (0.5f * step.x);
						c.axis[1] = b.get_column(1) * (0.5f * step.y);
						c.axis[2] = b.get_column(2) * (0.5f * step.z);
						cells.push_back(c);
					}
				}
			}
			if (cells.size() == first) {
				// Too few cells to land inside (one cell on a round shape): the centre carries it.
				Cell c;
				c.local = part.xf.xform(centre);
				c.axis[0] = b.get_column(0) * (0.5f * size.x);
				c.axis[1] = b.get_column(1) * (0.5f * size.y);
				c.axis[2] = b.get_column(2) * (0.5f * size.z);
				cells.push_back(c);
			}
			// The exact volume, shared by the cells (scaled by the shape's own scale).
			const float det = Math::abs(b.determinant());
			shape_volume = exact * det;
			for (uint32_t k = first; k < cells.size(); k++) {
				cells[k].volume = shape_volume / (cells.size() - first);
			}
		}
		total_volume += shape_volume;
		const float rho = part.density >= 0.0f ? part.density : default_density;
		mass += rho * shape_volume;
		Vector3 centroid;
		for (uint32_t k = first; k < cells.size(); k++) {
			centroid += cells[k].local * cells[k].volume;
		}
		moment += rho * centroid;
	}
	if (mass_from_densities && mass > 0.0f) {
		p_body->set_mass(mass);
		p_body->set_center_of_mass_mode(RigidBody3D::CENTER_OF_MASS_MODE_CUSTOM);
		p_body->set_center_of_mass(moment / mass);
	}
}

// Each cell pushed up by the water it displaces at its own water height: a sphere by its submerged cap (at the
// cap's centroid), a cell by its depth into the water over its vertical extent (at the wet part's centre).
void PhysXBuoyancy3D::_shapes_step(RigidBody3D *p_body) {
	const Transform3D xf = p_body->get_global_transform();
	const float g = p_body->get_gravity().length();
	const Vector3 up = p_body->get_gravity().length_squared() > 0.0f ? -p_body->get_gravity().normalized() : Vector3(0, 1, 0);
	const Vector3 lin = p_body->get_linear_velocity();
	const Vector3 ang = p_body->get_angular_velocity();
	// Cells in one column share a water sample (a few per body instead of one a cell).
	struct Column {
		int64_t key;
		float height;
	};
	LocalVector<Column> columns;
	const float column_size = 0.25f;
	auto water_at = [&](const Vector3 &p_world) -> float {
		const int64_t key = ((int64_t)Math::floor(p_world.x / column_size) << 32) ^ (int64_t)(uint32_t)(int32_t)Math::floor(p_world.z / column_size);
		for (const Column &c : columns) {
			if (c.key == key) {
				return c.height;
			}
		}
		const float h = _height_at(p_world);
		columns.push_back({ key, h });
		return h;
	};
	float wet = 0.0f;
	struct Line {
		Vector3 at; // world, where the cell cuts the surface
		float area;
	};
	LocalVector<Line> line;
	float line_area = 0.0f;
	const float frontal_per_volume = total_volume > 0.0f ? Math::pow(total_volume, -1.0f / 3.0f) : 0.0f;
	for (uint32_t i = 0; i < cells.size(); i++) {
		const Cell &c = cells[i];
		const Vector3 wc = xf.xform(c.local);
		const float surface = water_at(wc);
		const float depth = surface - wc.y;
		point_depths.set(i, depth);
		if (!Math::is_finite(depth)) {
			continue;
		}
		float v_sub = 0.0f;
		Vector3 at = wc;
		float waterline = 0.0f; // m^2 of the water's surface this cell cuts
		if (c.radius > 0.0f) {
			const float r = c.radius;
			const float x = CLAMP(r + depth, 0.0f, 2.0f * r);
			v_sub = (float)Math::PI * x * x * (3.0f * r - x) / 3.0f;
			waterline = (float)Math::PI * x * (2.0f * r - x);
			if (x > 0.0f) {
				at = wc - up * (3.0f * (2.0f * r - x) * (2.0f * r - x) / (4.0f * (3.0f * r - x)));
			}
		} else {
			float e = 0.0f;
			for (int a = 0; a < 3; a++) {
				e += Math::abs(xf.basis.xform(c.axis[a]).dot(up));
			}
			e = MAX(e, 1e-4f);
			const float f = CLAMP((depth + e) / (2.0f * e), 0.0f, 1.0f);
			v_sub = f * c.volume;
			at = wc + up * (e * (f - 1.0f));
			waterline = (f > 0.0f && f < 1.0f) ? c.volume / (2.0f * e) : 0.0f;
		}
		if (waterline > 0.0f) {
			line.push_back({ wc + up * (depth), waterline });
			line_area += waterline;
		}
		if (v_sub <= 0.0f) {
			continue;
		}
		wet += v_sub;
		const Vector3 offset = at - xf.origin;
		const Vector3 v = lin + ang.cross(offset) - _flow_at(at);
		Vector3 force = up * (water_density * g * v_sub);
		// Drag on the wet part: the probe mode's linear drag (per wet volume over the hull's height: footprint
		// times its wet share), plus form drag 0.5 rho Cd A |v| v with the body's frontal area, about its
		// volume to the two thirds, shared by wet volume (a body that dives is stopped in a body length or two).
		force -= v * (linear_drag * v_sub / MAX(height, 0.01f));
		force -= v * (0.5f * water_density * drag_coefficient * v_sub * frontal_per_volume * v.length());
		p_body->apply_force(force, offset);
	}
	// Wave radiation: a body bobbing at the surface makes waves that carry its energy away, damping heave,
	// pitch and roll (a heave damping ratio of about 0.1 to 0.3 for barge-like hulls; Faltinsen 1990, Sea Loads
	// on Ships and Offshore Structures). As a share of the critical damping of the waterline's stiffness
	// rho g A_wp, spread over the waterline by area, against each point's vertical speed relative to the water.
	if (wave_damping > 0.0f && line_area > 0.0f) {
		const float k = water_density * g * line_area;
		const float c_total = 2.0f * wave_damping * Math::sqrt(k * MAX(p_body->get_mass(), 1e-3f));
		for (const Line &l : line) {
			const Vector3 offset = l.at - xf.origin;
			const float vy = (lin + ang.cross(offset) - _flow_at(l.at)).dot(up);
			p_body->apply_force(up * (-c_total * (l.area / line_area) * vy), offset);
		}
	}
	submerged_fraction = total_volume > 0.0f ? wet / total_volume : 0.0f;
	if (submerged_fraction > 0.0f) {
		p_body->apply_torque(-ang * (angular_drag * submerged_fraction));
	}
}

void PhysXBuoyancy3D::set_volume_mode(VolumeMode p_mode) {
	volume_mode = p_mode;
	if (is_inside_tree()) {
		_resolve();
	}
}

void PhysXBuoyancy3D::set_max_cells(int p_cells) {
	max_cells = CLAMP(p_cells, 1, 4096);
	if (is_inside_tree()) {
		_resolve();
	}
}

void PhysXBuoyancy3D::set_mass_from_densities(bool p_enabled) {
	mass_from_densities = p_enabled;
	if (is_inside_tree()) {
		_resolve();
	}
}

void PhysXBuoyancy3D::set_default_density(float p_density) {
	default_density = MAX(p_density, 0.0f);
	if (is_inside_tree()) {
		_resolve();
	}
}

// Every tick: a wake can be added or removed at any time.
void PhysXBuoyancy3D::_find_wake() {
	wake_id = ObjectID();
	const Node *parent = get_parent();
	if (parent == nullptr) {
		return;
	}
	for (int i = 0; i < parent->get_child_count(); i++) {
		if (Object::cast_to<PhysXWaterWake3D>(parent->get_child(i)) != nullptr) {
			wake_id = parent->get_child(i)->get_instance_id();
		}
	}
}

void PhysXBuoyancy3D::_physics_step() {
	_find_wake();
	RigidBody3D *body = get_body();
	PhysXWaterSurface3D *water = get_water_surface(); // null on a river
	PhysXRiver3D *river = get_river();
	if (body == nullptr || !_has_water() || points.is_empty()) {
		submerged_fraction = 0.0f;
		return;
	}
	const Transform3D xf = body->get_global_transform();
	const Vector3 lin = body->get_linear_velocity();
	if (volume_mode == VOLUME_SHAPES && !cells.is_empty()) {
		_shapes_step(body);
	} else {
		const float g = body->get_gravity().length();
		const Vector3 up = body->get_gravity().length_squared() > 0.0f ? -body->get_gravity().normalized() : Vector3(0, 1, 0);
		const Vector3 ang = body->get_angular_velocity();
		const float a = area / points.size();
		float used = 0.0f;
		for (int i = 0; i < points.size(); i++) {
			const Vector3 wp = xf.xform(points[i]);
			const float depth = _height_at(wp) - wp.y;
			point_depths.set(i, depth);
			if (!(depth > 0.0f)) {
				continue;
			}
			const float d = MIN(depth, height);
			used += d;
			const Vector3 offset = wp - xf.origin;
			// Relative to the water: in a current the drag carries the body along
			// (PhysXWaterSurface3D::current), so debris drifts downstream and
			// turns round rocks with the flow.
			const Vector3 v = lin + ang.cross(offset) - _flow_at(wp);
			// Archimedes for this point's share of the footprint, plus drag on
			// the part that's in the water.
			Vector3 force = up * (water_density * g * a * d);
			force -= v * (linear_drag * a * (d / height));
			body->apply_force(force, offset);
		}
		submerged_fraction = used / (points.size() * height);
		if (submerged_fraction > 0.0f) {
			body->apply_torque(-ang * (angular_drag * submerged_fraction));
		}
	}

	if (disturb_water && (water != nullptr || river != nullptr)) {
		// Only near the surface, harder the faster it moves (relative to the
		// water, on a river) -- a still floater only dimples the water.
		const int owner = (int)(uint64_t)get_instance_id();
		const float surface = water != nullptr ? water->sample_height_excluding_wake(xf.origin, wake_id) : river->sample_height(xf.origin);
		const float above = xf.origin.y - surface;
		const float proximity = Math::is_finite(surface) ? CLAMP(1.0f - above / radius, 0.0f, 1.0f) : 0.0f;
		if (proximity > 0.0f) {
			const Vector3 rel = water != nullptr ? lin : lin - river->sample_flow(xf.origin);
			const float speed_frac = rel.length() / ripple_reference_speed;
			const float strength = CLAMP(ripple_rest_strength + (1.0f - ripple_rest_strength) * speed_frac, ripple_rest_strength, 1.0f) * proximity;
			if (water != nullptr) {
				water->submit_sphere(owner, xf.origin, radius, strength);
			} else {
				river->submit_sphere(owner, xf.origin, radius, strength);
			}
		} else if (water != nullptr) {
			water->clear_sphere(owner);
		} else {
			river->clear_sphere(owner);
		}
	}
}

void PhysXBuoyancy3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			_resolve();
			if (!Engine::get_singleton()->is_editor_hint()) {
				set_physics_process_internal(true);
			}
		} break;
		case NOTIFICATION_INTERNAL_PHYSICS_PROCESS: {
			_physics_step();
		} break;
		case NOTIFICATION_EXIT_TREE: {
			PhysXWaterSurface3D *water = get_water_surface();
			if (water != nullptr) {
				water->clear_sphere((int)(uint64_t)get_instance_id());
			}
			if (PhysXRiver3D *river = get_river()) {
				river->clear_sphere((int)(uint64_t)get_instance_id());
			}
		} break;
	}
}

void PhysXBuoyancy3D::set_water_surface_path(const NodePath &p_path) {
	water_surface_path = p_path;
	if (is_inside_tree()) {
		_resolve();
	}
	update_configuration_warnings();
}

void PhysXBuoyancy3D::set_sample_points(const PackedVector3Array &p_points) {
	sample_points = p_points;
	if (is_inside_tree()) {
		_resolve();
	}
}

void PhysXBuoyancy3D::set_hull_area(float p_area) {
	hull_area = MAX(p_area, 0.0f);
	if (is_inside_tree()) {
		_resolve();
	}
}

void PhysXBuoyancy3D::set_hull_height(float p_height) {
	hull_height = MAX(p_height, 0.0f);
	if (is_inside_tree()) {
		_resolve();
	}
}

void PhysXBuoyancy3D::set_disturb_water(bool p_enabled) {
	disturb_water = p_enabled;
	PhysXWaterSurface3D *water = get_water_surface();
	if (!disturb_water && water != nullptr) {
		water->clear_sphere((int)(uint64_t)get_instance_id());
	}
	PhysXRiver3D *river = get_river();
	if (!disturb_water && river != nullptr) {
		river->clear_sphere((int)(uint64_t)get_instance_id());
	}
}

void PhysXBuoyancy3D::set_ripple_radius(float p_radius) {
	ripple_radius = MAX(p_radius, 0.0f);
	if (is_inside_tree()) {
		_resolve();
	}
}

PackedStringArray PhysXBuoyancy3D::get_configuration_warnings() const {
	PackedStringArray warnings = Node3D::get_configuration_warnings();
	if (Object::cast_to<RigidBody3D>(get_parent()) == nullptr) {
		warnings.push_back(RTR("PhysXBuoyancy3D only floats a RigidBody3D: make it a child of one."));
	}
	if (water_surface_path.is_empty()) {
		warnings.push_back(RTR("Set water_surface_path to the PhysXWaterSurface3D or PhysXRiver3D to float on."));
	}
	return warnings;
}

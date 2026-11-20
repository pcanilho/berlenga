/**************************************************************************/
/*  physx_water_mist_3d.h                                                 */
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

#include "physx_sea_cave_water_3d.h"

#include "physx_river_3d.h"
#include "water_raster_reflections.h"

#include "core/object/class_db.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/resources/material.h"

void PhysXSeaCaveWater3D::set_cave_network(const NodePath &p_path) {
	cave_network = p_path;
	_clear();
}

void PhysXSeaCaveWater3D::set_lanes(int p_lanes) {
	lanes = CLAMP(p_lanes, 2, 64);
}

void PhysXSeaCaveWater3D::_clear() {
	for (Ribbon &r : ribbons) {
		if (r.instance) {
			r.instance->queue_free();
		}
	}
	ribbons.clear();
}

// One ribbon per sea cave. The layout is the river's (PhysXLiveWater3D's build_live_surface): UV2 the current (m/s,
// world x and z), COLOR.r = (depth + 1) / 4, COLOR.g white water, COLOR.b = 1. Dry stations (the back of a cave at
// low water) sit just under the floor's level so the waterline falls between them and the wet ones.
void PhysXSeaCaveWater3D::_update() {
	Node *net = cave_network.is_empty() ? nullptr : get_node_or_null(cave_network);
	if (net == nullptr || !net->has_method(SNAME("get_sea_cave_water"))) {
		_clear();
		return;
	}
	if (material.is_null()) {
		material.instantiate();
		material->set_shader(water_shared_shader(physx_river_water_shader_src()));
	}
	const int caves = int(net->call(SNAME("get_cave_count")));
	LocalVector<int> want;
	for (int c = 0; c < caves; c++) {
		if (bool(net->call(SNAME("is_sea_cave"), c))) {
			want.push_back(c);
		}
	}
	// Keep the ribbons in step with the sea caves.
	if (want.size() != ribbons.size()) {
		_clear();
		for (int c : want) {
			Ribbon r;
			r.cave = c;
			r.mesh.instantiate();
			r.instance = memnew(MeshInstance3D);
			r.instance->set_mesh(r.mesh);
			r.instance->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
			add_child(r.instance, false, INTERNAL_MODE_BACK);
			ribbons.push_back(r);
		}
	}
	const Transform3D to_local = get_global_transform().affine_inverse();
	for (Ribbon &r : ribbons) {
		const Dictionary w = net->call(SNAME("get_sea_cave_water"), r.cave);
		const PackedVector3Array pts = w.get("points", PackedVector3Array());
		const PackedFloat32Array hw = w.get("half_widths", PackedFloat32Array());
		const PackedFloat32Array fl = w.get("flows", PackedFloat32Array());
		const PackedFloat32Array dp = w.get("depths", PackedFloat32Array());
		const int n = pts.size();
		r.mesh->clear_surfaces();
		if (n < 2 || hw.size() != n || fl.size() != n || dp.size() != n) {
			continue;
		}
		PackedVector3Array verts, normals;
		PackedColorArray colors;
		PackedVector2Array uv2;
		PackedInt32Array idx;
		verts.resize(n * lanes);
		normals.resize(n * lanes);
		colors.resize(n * lanes);
		uv2.resize(n * lanes);
		AABB box;
		for (int i = 0; i < n; i++) {
			// The passage's line, mouth first: along it into the cave; flows are positive into the cave.
			const Vector3 a = pts[MAX(i - 1, 0)];
			const Vector3 b = pts[MIN(i + 1, n - 1)];
			Vector3 along = Vector3(b.x - a.x, 0.0f, b.z - a.z);
			along = along.length_squared() > 1e-8f ? along.normalized() : Vector3(1, 0, 0);
			const Vector3 across(-along.z, 0.0f, along.x);
			const float half = hw[i] + wall_margin;
			const float depth = dp[i];
			const bool wet = depth > 0.01f;
			// The surface's slope along the passage (the wave's face), for the normal.
			const float slope = (b.y - a.y) / MAX(Vector2(b.x - a.x, b.z - a.z).length(), 0.01f);
			const Vector3 nrm = Vector3(-along.x * slope, 1.0f, -along.z * slope).normalized();
			const float u = fl[i];
			const float white = CLAMP((Math::abs(u) - white_water_speed) / white_water_speed, 0.0f, 1.0f);
			for (int l = 0; l < lanes; l++) {
				const float s = -1.0f + 2.0f * float(l) / float(lanes - 1);
				Vector3 p = pts[i] + across * (s * half);
				if (!wet) {
					p.y -= 0.03f;
				}
				const int k = i * lanes + l;
				verts.write[k] = to_local.xform(p);
				normals.write[k] = to_local.basis.xform(nrm).normalized();
				colors.write[k] = Color(CLAMP(((wet ? depth : -0.05f) + 1.0f) * 0.25f, 0.0f, 1.0f), white, 1.0f, 1.0f);
				uv2.write[k] = Vector2(along.x * u, along.z * u);
				if (i == 0 && l == 0) {
					box = AABB(verts[k], Vector3());
				} else {
					box.expand_to(verts[k]);
				}
			}
		}
		for (int i = 0; i < n - 1; i++) {
			const bool wet = dp[i] > 0.01f || dp[i + 1] > 0.01f;
			if (!wet) {
				continue;
			}
			for (int l = 0; l < lanes - 1; l++) {
				const int a = i * lanes + l;
				const int b = a + 1;
				const int c = a + lanes;
				const int e = c + 1;
				// Clockwise seen from above (Godot's front faces): the path tracer takes the top as entering the
				// water. The winding follows the ribbon's handedness (across is along turned left).
				idx.push_back(a);
				idx.push_back(c);
				idx.push_back(b);
				idx.push_back(b);
				idx.push_back(c);
				idx.push_back(e);
			}
		}
		if (idx.is_empty()) {
			continue;
		}
		Array arrays;
		arrays.resize(Mesh::ARRAY_MAX);
		arrays[Mesh::ARRAY_VERTEX] = verts;
		arrays[Mesh::ARRAY_NORMAL] = normals;
		arrays[Mesh::ARRAY_COLOR] = colors;
		arrays[Mesh::ARRAY_TEX_UV2] = uv2;
		arrays[Mesh::ARRAY_INDEX] = idx;
		r.mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
		r.mesh->surface_set_material(0, material);
		r.instance->set_custom_aabb(box.grow(1.0f));
	}
}

void PhysXSeaCaveWater3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			set_process_internal(true);
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			water_time += get_process_delta_time();
			if (material.is_valid()) {
				material->set_shader_parameter(SNAME("water_time"), (float)Math::fmod(water_time, 3600.0));
			}
			_update();
		} break;
		case NOTIFICATION_EXIT_TREE: {
			_clear();
		} break;
	}
}

void PhysXSeaCaveWater3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_cave_network", "path"), &PhysXSeaCaveWater3D::set_cave_network);
	ClassDB::bind_method(D_METHOD("get_cave_network"), &PhysXSeaCaveWater3D::get_cave_network);
	ClassDB::bind_method(D_METHOD("set_lanes", "lanes"), &PhysXSeaCaveWater3D::set_lanes);
	ClassDB::bind_method(D_METHOD("get_lanes"), &PhysXSeaCaveWater3D::get_lanes);
	ClassDB::bind_method(D_METHOD("set_wall_margin", "margin"), &PhysXSeaCaveWater3D::set_wall_margin);
	ClassDB::bind_method(D_METHOD("get_wall_margin"), &PhysXSeaCaveWater3D::get_wall_margin);
	ClassDB::bind_method(D_METHOD("set_white_water_speed", "speed"), &PhysXSeaCaveWater3D::set_white_water_speed);
	ClassDB::bind_method(D_METHOD("get_white_water_speed"), &PhysXSeaCaveWater3D::get_white_water_speed);
	ClassDB::bind_method(D_METHOD("get_ribbon_count"), &PhysXSeaCaveWater3D::get_ribbon_count);
	ClassDB::bind_method(D_METHOD("get_water_material"), &PhysXSeaCaveWater3D::get_water_material);
	ClassDB::bind_method(D_METHOD("update_now"), &PhysXSeaCaveWater3D::update_now);
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "cave_network", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D"), "set_cave_network", "get_cave_network");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "lanes", PROPERTY_HINT_RANGE, "2,64,1"), "set_lanes", "get_lanes");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wall_margin", PROPERTY_HINT_RANGE, "0,2,0.01,suffix:m"), "set_wall_margin", "get_wall_margin");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "white_water_speed", PROPERTY_HINT_RANGE, "0.1,10,0.1,suffix:m/s"), "set_white_water_speed", "get_white_water_speed");
}

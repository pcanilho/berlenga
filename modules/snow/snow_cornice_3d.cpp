/**************************************************************************/
/*  snow_cornice_3d.cpp                                                       */
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

#include "snow_cornice_3d.h"

#include "snow_cover_3d.h"

#include "core/config/engine.h"
#include "core/object/class_db.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/physics/collision_shape_3d.h"
#include "scene/3d/physics/static_body_3d.h"
#include "scene/main/viewport.h"
#include "scene/resources/3d/concave_polygon_shape_3d.h"

namespace {
// The cross-section, in lengths of the cornice: x out along the wind from the crest, y up from the snow top on
// the crest. A closed loop, top first (windward root buried in the crest's snow, out to the tip), back along
// the underside to a root buried in the lee face. As thick as it is long at the root, thinning to the lip
// (field cornices; the wind-tunnel slab of Yu et al. 2023 thickens more slowly than it lengthens).
constexpr int RING = 13;
const float PROFILE[RING][2] = {
	{ -0.60f, -0.30f },
	{ -0.30f, 0.00f },
	{ 0.00f, 0.06f },
	{ 0.35f, 0.10f },
	{ 0.70f, 0.06f },
	{ 0.92f, -0.04f },
	{ 1.00f, -0.18f },
	{ 0.94f, -0.32f },
	{ 0.75f, -0.42f },
	{ 0.50f, -0.56f },
	{ 0.30f, -0.80f },
	{ 0.12f, -1.15f },
	{ -0.30f, -1.50f },
};

struct Crest {
	Vector3 top; // the snow top on the crest
	float depth = 0.0f; // the pack's depth there
	Vector2 dir; // out over the lee
	float length = 0.0f;
	int links = 0;
};

Vector3 profile_point(const Crest &p_c, float p_scale, int p_k) {
	const float l = p_c.length * p_scale;
	const float d = p_c.depth * p_scale;
	float x = PROFILE[p_k][0] * l;
	float y = PROFILE[p_k][1] * l;
	// The buried roots reach down through the crest's pack to the ground, and the windward side ramps up
	// to the top as a drift, so the slab sits on the ground where no deep snow is drawn (far from a
	// SnowSurface3D) and on the snow where it is.
	if (p_k == 0) {
		x -= 3.0f * d;
		y -= d;
	} else if (p_k == 1) {
		x -= 1.5f * d;
	} else if (p_k >= RING - 2) {
		y -= d;
	}
	// The tip sags as the slab grows out (it creeps under its own weight).
	if (PROFILE[p_k][0] > 0.5f) {
		y -= 0.04f * l * l * (PROFILE[p_k][0] - 0.5f) * 2.0f;
	}
	return p_c.top + Vector3(p_c.dir.x * x, y, p_c.dir.y * x);
}
} // namespace

Vector3 SnowCornice3D::_follow_position() const {
	if (!follow_target.is_empty()) {
		const Node3D *n = Object::cast_to<Node3D>(get_node_or_null(follow_target));
		if (n && n->is_inside_tree()) {
			return n->get_global_position();
		}
	}
	if (is_inside_tree() && get_viewport()) {
		if (const Camera3D *cam = get_viewport()->get_camera_3d()) {
			return cam->get_global_position();
		}
	}
	return is_inside_tree() ? get_global_position() : Vector3();
}

void SnowCornice3D::_ensure_children() {
	if (!mesh_instance) {
		mesh_instance = memnew(MeshInstance3D);
		add_child(mesh_instance, false, INTERNAL_MODE_FRONT);
	}
	if (!body) {
		body = memnew(StaticBody3D);
		add_child(body, false, INTERNAL_MODE_FRONT);
		shape = memnew(CollisionShape3D);
		body->add_child(shape);
	}
	if (default_material.is_null()) {
		default_material.instantiate();
		default_material->set_albedo(Color(0.86f, 0.88f, 0.9f));
		default_material->set_roughness(0.75f);
		default_material->set_specular(0.22f);
	}
}

void SnowCornice3D::rebuild() {
	if (!is_inside_tree()) {
		return;
	}
	_ensure_children();
	set_global_transform(Transform3D());
	built_at = _follow_position();
	built = true;
	segment_count = 0;
	SnowCover3D *cover = SnowCover3D::get_active();
	const SnowCover3D *w = cover ? cover->world : nullptr;
	if (!w || w->cell_cornice_x.size() != w->resolution * w->resolution) {
		mesh_instance->set_mesh(Ref<Mesh>());
		shape->set_shape(Ref<Shape3D>());
		return;
	}
	built_update = w->update_count;
	const int n = w->resolution;
	const float cs = w->_cell_size();
	const Vector2i o = w->origin_cell;

	// The crest cells near the camera and the crest top in each, found on the fine ground along the wind.
	HashMap<int, Crest> crests;
	const int r = int(Math::ceil(radius / cs));
	const int cx0 = int(Math::floor(built_at.x / cs)) - o.x;
	const int cz0 = int(Math::floor(built_at.z / cs)) - o.y;
	for (int z = MAX(cz0 - r, 0); z <= MIN(cz0 + r, n - 1); z++) {
		for (int x = MAX(cx0 - r, 0); x <= MIN(cx0 + r, n - 1); x++) {
			const int i = z * n + x;
			const Vector2 c(w->cell_cornice_x[i], w->cell_cornice_z[i]);
			const float len = c.length();
			if (len < min_length) {
				continue;
			}
			Crest cr;
			cr.dir = c / len;
			cr.length = len;
			const Vector2 center((o.x + x + 0.5f) * cs, (o.y + z + 0.5f) * cs);
			float best = -1e30f;
			Vector2 at = center;
			for (int s = -4; s <= 4; s++) {
				const Vector2 p = center + cr.dir * (cs * 0.125f * s);
				float h = cover->sample_ground_height(Vector3(p.x, 0.0f, p.y));
				if (!Math::is_finite(h)) {
					h = w->_sample(w->cell_ground, p);
				}
				if (h > best) {
					best = h;
					at = p;
				}
			}
			const float depth = w->get_depth_at(Vector3(at.x, 0.0f, at.y));
			cr.top = Vector3(at.x, best + depth, at.y);
			cr.depth = depth;
			crests.insert(i, cr);
		}
	}

	Vector<Vector3> verts;
	Vector<Vector3> normals;
	Vector<int> indices;
	auto sweep = [&](const Crest &p_a, float p_sa, const Crest &p_b, float p_sb) {
		// One piece of the slab between two stations, normals from the ring and the run.
		const int base = verts.size();
		const Crest *st[2] = { &p_a, &p_b };
		const float sc[2] = { p_sa, p_sb };
		const Vector3 run = (p_b.top - p_a.top);
		for (int e = 0; e < 2; e++) {
			Vector3 ring[RING];
			Vector3 centroid;
			for (int k = 0; k < RING; k++) {
				ring[k] = profile_point(*st[e], sc[e], k);
				centroid += ring[k] / RING;
			}
			for (int k = 0; k < RING; k++) {
				const Vector3 t = ring[(k + 1) % RING] - ring[(k + RING - 1) % RING];
				Vector3 nrm = t.cross(run);
				if (nrm.length_squared() < 1e-12f) {
					nrm = Vector3(0, 1, 0);
				}
				nrm.normalize();
				if (nrm.dot(ring[k] - centroid) < 0.0f) {
					nrm = -nrm;
				}
				verts.push_back(ring[k]);
				normals.push_back(nrm);
			}
		}
		for (int k = 0; k < RING; k++) {
			const int k1 = (k + 1) % RING;
			const int a0 = base + k, a1 = base + k1, b0 = base + RING + k, b1 = base + RING + k1;
			// Wound so the face looks out (Godot: clockwise front faces).
			const Vector3 fn = (verts[b0] - verts[a0]).cross(verts[a1] - verts[a0]);
			const bool flip = fn.dot(normals[a0] + normals[a1]) > 0.0f;
			if (flip) {
				indices.push_back(a0);
				indices.push_back(a1);
				indices.push_back(b0);
				indices.push_back(a1);
				indices.push_back(b1);
				indices.push_back(b0);
			} else {
				indices.push_back(a0);
				indices.push_back(b0);
				indices.push_back(a1);
				indices.push_back(a1);
				indices.push_back(b0);
				indices.push_back(b1);
			}
		}
		segment_count++;
	};

	// Runs: neighbouring crest cells that lie across the wind are joined.
	static const int DX[4] = { 1, -1, 0, 1 };
	static const int DZ[4] = { 0, 1, 1, 1 };
	for (KeyValue<int, Crest> &kv : crests) {
		const int x = kv.key % n;
		const int z = kv.key / n;
		for (int k = 0; k < 4; k++) {
			const int nx = x + DX[k];
			const int nz = z + DZ[k];
			if (nx < 0 || nz < 0 || nx >= n || nz >= n) {
				continue;
			}
			Crest *b = crests.getptr(nz * n + nx);
			if (!b) {
				continue;
			}
			const Vector2 off = Vector2(b->top.x - kv.value.top.x, b->top.z - kv.value.top.z);
			const Vector2 d = (kv.value.dir + b->dir).normalized();
			if (off.length() < 1e-3f || Math::abs(off.normalized().dot(d)) > 0.6f) {
				continue;
			}
			sweep(kv.value, 1.0f, *b, 1.0f);
			kv.value.links++;
			b->links++;
		}
	}
	// Ends: a run tapers to nothing over most of a cell past its last crest.
	for (KeyValue<int, Crest> &kv : crests) {
		const Crest &c = kv.value;
		const Vector2 across(-c.dir.y, c.dir.x);
		Crest a = c, b = c;
		if (c.links == 0) {
			a.top -= Vector3(across.x, 0, across.y) * cs * 0.45f;
			b.top += Vector3(across.x, 0, across.y) * cs * 0.45f;
			sweep(a, 0.0f, c, 1.0f);
			sweep(c, 1.0f, b, 0.0f);
		}
	}

	Ref<ArrayMesh> mesh;
	if (!indices.is_empty()) {
		mesh.instantiate();
		Array arrays;
		arrays.resize(Mesh::ARRAY_MAX);
		arrays[Mesh::ARRAY_VERTEX] = verts;
		arrays[Mesh::ARRAY_NORMAL] = normals;
		arrays[Mesh::ARRAY_INDEX] = indices;
		mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
		mesh->surface_set_material(0, material.is_valid() ? material : Ref<Material>(default_material));
	}
	mesh_instance->set_mesh(mesh);
	if (collision_enabled && mesh.is_valid()) {
		Ref<ConcavePolygonShape3D> s;
		s.instantiate();
		Vector<Vector3> faces;
		faces.resize(indices.size());
		for (int i = 0; i < indices.size(); i++) {
			faces.write[i] = verts[indices[i]];
		}
		s->set_faces(faces);
		shape->set_shape(s);
	} else {
		shape->set_shape(Ref<Shape3D>());
	}
}

void SnowCornice3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			if (!Engine::get_singleton()->is_editor_hint()) {
				set_process_internal(true);
				rebuild();
			}
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			check_accum += get_process_delta_time();
			if (check_accum < 0.5) {
				break;
			}
			check_accum = 0.0;
			const SnowCover3D *cover = SnowCover3D::get_active();
			const SnowCover3D *w = cover ? cover->world : nullptr;
			const bool moved = !built || _follow_position().distance_to(built_at) > rebuild_distance;
			// The world tier changes slowly: catch up with it every few of its updates.
			const bool grown = w && w->update_count >= built_update + 8;
			if (moved || grown || (w && !built)) {
				rebuild();
			}
		} break;
	}
}

void SnowCornice3D::set_radius(float p_radius) {
	radius = MAX(p_radius, 1.0f);
}

void SnowCornice3D::set_min_length(float p_length) {
	min_length = MAX(p_length, 0.01f);
}

void SnowCornice3D::set_rebuild_distance(float p_distance) {
	rebuild_distance = MAX(p_distance, 1.0f);
}

void SnowCornice3D::set_collision_enabled(bool p_enabled) {
	collision_enabled = p_enabled;
}

void SnowCornice3D::set_material(const Ref<Material> &p_material) {
	material = p_material;
	if (mesh_instance && mesh_instance->get_mesh().is_valid()) {
		mesh_instance->get_mesh()->surface_set_material(0, material.is_valid() ? material : Ref<Material>(default_material));
	}
}

void SnowCornice3D::set_follow_target(const NodePath &p_path) {
	follow_target = p_path;
}

void SnowCornice3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("rebuild"), &SnowCornice3D::rebuild);
	ClassDB::bind_method(D_METHOD("get_segment_count"), &SnowCornice3D::get_segment_count);
	ClassDB::bind_method(D_METHOD("set_radius", "radius"), &SnowCornice3D::set_radius);
	ClassDB::bind_method(D_METHOD("get_radius"), &SnowCornice3D::get_radius);
	ClassDB::bind_method(D_METHOD("set_min_length", "length"), &SnowCornice3D::set_min_length);
	ClassDB::bind_method(D_METHOD("get_min_length"), &SnowCornice3D::get_min_length);
	ClassDB::bind_method(D_METHOD("set_rebuild_distance", "distance"), &SnowCornice3D::set_rebuild_distance);
	ClassDB::bind_method(D_METHOD("get_rebuild_distance"), &SnowCornice3D::get_rebuild_distance);
	ClassDB::bind_method(D_METHOD("set_collision_enabled", "enabled"), &SnowCornice3D::set_collision_enabled);
	ClassDB::bind_method(D_METHOD("is_collision_enabled"), &SnowCornice3D::is_collision_enabled);
	ClassDB::bind_method(D_METHOD("set_material", "material"), &SnowCornice3D::set_material);
	ClassDB::bind_method(D_METHOD("get_material"), &SnowCornice3D::get_material);
	ClassDB::bind_method(D_METHOD("set_follow_target", "path"), &SnowCornice3D::set_follow_target);
	ClassDB::bind_method(D_METHOD("get_follow_target"), &SnowCornice3D::get_follow_target);

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "radius", PROPERTY_HINT_RANGE, "10,10000,1,or_greater,suffix:m"), "set_radius", "get_radius");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "min_length", PROPERTY_HINT_RANGE, "0.01,10,0.01,suffix:m"), "set_min_length", "get_min_length");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "rebuild_distance", PROPERTY_HINT_RANGE, "1,1000,1,suffix:m"), "set_rebuild_distance", "get_rebuild_distance");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "collision_enabled"), "set_collision_enabled", "is_collision_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "material", PROPERTY_HINT_RESOURCE_TYPE, "BaseMaterial3D,ShaderMaterial"), "set_material", "get_material");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "follow_target", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D"), "set_follow_target", "get_follow_target");
}

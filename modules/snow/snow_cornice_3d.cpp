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
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/physics/collision_shape_3d.h"
#include "scene/3d/physics/rigid_body_3d.h"
#include "scene/resources/3d/convex_polygon_shape_3d.h"
#include "scene/3d/physics/static_body_3d.h"
#include "scene/main/viewport.h"
#include "scene/resources/3d/concave_polygon_shape_3d.h"

namespace {
// The cross-section, in lengths of the cornice: x out along the wind from the crest, y up from the snow top on
// the crest. A closed loop, top first (windward root buried in the crest's snow, out to the tip), back along
// the underside to a root buried in the lee face. As thick as it is long at the root, thinning to the lip
// (field cornices; the wind-tunnel slab of Yu et al. 2023 thickens more slowly than it lengthens). Points 2 to 4
// are the tension crack in the roof behind the crest (follow-up 6), closed until it opens: the windward lip, the
// crack's bottom and the lee lip; from the lee lip to the lee root's last point the slab tilts about a pivot
// low in its root as the crack opens (Vogel et al. 2012).
constexpr int RING = 16;
constexpr int CRACK_WINDWARD = 2;
constexpr int CRACK_BOTTOM = 3;
constexpr int CRACK_LEE = 4;
constexpr float CRACK_X = -0.15f;
const float PROFILE[RING][2] = {
	{ -0.60f, -0.30f },
	{ -0.30f, 0.00f },
	{ CRACK_X, 0.03f },
	{ CRACK_X, 0.03f },
	{ CRACK_X, 0.03f },
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
	float crack = 0.0f; // the tension crack's opening, m
	int links = 0;
	Vector3 link_sum; // offsets to the linked neighbours
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
	// The tension crack: the slab beyond it tilts out about a pivot low in its root, opening the roof by the
	// crack's width, cut down to the share of the section the failure model takes (60% when fully open).
	if (l > 0.0f && p_c.crack > 0.0f) {
		const float w = MIN(p_c.crack * p_scale, 0.1f * l);
		const float a = w / l;
		if (p_k == CRACK_BOTTOM) {
			x += 0.5f * w;
			y -= 0.6f * (w / (0.1f * l)) * l;
		} else if (p_k >= CRACK_LEE && p_k < RING - 1) {
			const float px = CRACK_X * l;
			const float py = -1.0f * l;
			const float dx = x - px;
			const float dy = y - py;
			x = px + dx * Math::cos(a) + dy * Math::sin(a);
			y = py - dx * Math::sin(a) + dy * Math::cos(a);
		}
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
		default_material->set_specular(0.335f); // ice, F0 0.018
		// The snow pass gives the sunlit top its sparkle. Light carries only millimetres into snow (Warren
		// 1982), below what a slab this size shows, so no subsurface scattering by default: under the path
		// tracer it only added noise. The mesh is closed, so a material with it works.
		default_material->set_feature(BaseMaterial3D::FEATURE_SNOW, true);
		default_material->set_snow_from_weather(false);
		default_material->set_snow_amount(1.0f);
		default_material->set_snow_max_slope(90.0f);
	}
}

void SnowCornice3D::rebuild() {
	if (!is_inside_tree()) {
		return;
	}
	_ensure_children();
	if (!Engine::get_singleton()->is_editor_hint()) {
		_connect_cover();
	}
	set_global_transform(Transform3D());
	built_at = _follow_position();
	built = true;
	segment_count = 0;
	SnowCover3D *cover = SnowCover3D::get_active();
	const SnowCover3D *w = cover ? cover->world : nullptr;
	if (!w || w->pub.cornice_x.size() != w->resolution * w->resolution) {
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
			const Vector2 c(w->pub.cornice_x[i], w->pub.cornice_z[i]);
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
			cr.crack = w->pub.cornice_crack.size() == n * n ? w->pub.cornice_crack[i] : 0.0f;
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
			kv.value.link_sum += b->top - kv.value.top;
			b->links++;
			b->link_sum += kv.value.top - b->top;
		}
	}
	// Ends: a run tapers to nothing over most of a cell past its last crest, which also closes the slab (the
	// path tracer's subsurface walk needs a closed mesh).
	for (KeyValue<int, Crest> &kv : crests) {
		const Crest &c = kv.value;
		const Vector2 across(-c.dir.y, c.dir.x);
		const Vector3 side = Vector3(across.x, 0, across.y) * cs * 0.45f;
		Crest a = c, b = c;
		if (c.links == 0) {
			a.top -= side;
			b.top += side;
			sweep(a, 0.0f, c, 1.0f);
			sweep(c, 1.0f, b, 0.0f);
		} else if (c.links == 1) {
			// Away from the one neighbour.
			b.top += c.link_sum.dot(side) > 0.0f ? -side : side;
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
				_connect_cover();
				rebuild();
			}
		} break;
		case NOTIFICATION_EXIT_TREE: {
			if (Object *o = ObjectDB::get_instance(connected_cover)) {
				if (o->is_connected(SNAME("cornice_failed"), callable_mp(this, &SnowCornice3D::_on_cornice_failed))) {
					o->disconnect(SNAME("cornice_failed"), callable_mp(this, &SnowCornice3D::_on_cornice_failed));
				}
			}
			connected_cover = ObjectID();
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			_connect_cover();
			for (int k = debris.size() - 1; k >= 0; k--) {
				debris.write[k].age += get_process_delta_time();
				if (debris[k].age > debris_lifetime) {
					if (Node *dn = ObjectDB::get_instance<Node>(debris[k].node)) {
						dn->queue_free();
					}
					debris.remove_at(k);
				}
			}
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

void SnowCornice3D::_connect_cover() {
	SnowCover3D *cover = SnowCover3D::get_active();
	const ObjectID id = cover ? cover->get_instance_id() : ObjectID();
	if (id == connected_cover) {
		return;
	}
	const Callable cb = callable_mp(this, &SnowCornice3D::_on_cornice_failed);
	if (Object *o = ObjectDB::get_instance(connected_cover)) {
		if (o->is_connected(SNAME("cornice_failed"), cb)) {
			o->disconnect(SNAME("cornice_failed"), cb);
		}
	}
	connected_cover = id;
	if (cover) {
		cover->connect(SNAME("cornice_failed"), cb);
	}
}

void SnowCornice3D::_on_cornice_failed(Vector3 p_position, Vector3 p_direction, float p_length, float p_mass) {
	// The slab breaks along the crack into blocks that tumble down the face (follow-up 6): a few pieces a few
	// tenths of its length across, along the run of crest the cell holds, then the mesh without it.
	if (!is_inside_tree()) {
		return;
	}
	if (debris_enabled && p_position.distance_to(_follow_position()) < radius) {
		_ensure_children();
		const SnowCover3D *cover = SnowCover3D::get_active();
		const float cs = cover && cover->world ? cover->world->_cell_size() : 16.0f;
		const Vector3 out = p_direction.normalized();
		const Vector3 across = Vector3(-out.z, 0.0f, out.x);
		// The crest top on the fine ground, as the drawn cornice finds it.
		Vector3 top = p_position;
		if (cover) {
			float best = -1e30f;
			for (int s = -4; s <= 4; s++) {
				const Vector3 p = p_position + out * (cs * 0.125f * s);
				const float h = cover->sample_ground_height(Vector3(p.x, 0.0f, p.z));
				if (Math::is_finite(h) && h > best) {
					best = h;
					top = Vector3(p.x, h + (cover->world ? cover->world->get_depth_at(p) : 0.0f), p.z);
				}
			}
		}
		const int count = CLAMP(int(Math::round(p_length * 3.0f)), 3, 16);
		const float piece = CLAMP(p_length * 0.35f, 0.2f, 1.6f);
		const Ref<Material> mat = material.is_valid() ? material : Ref<Material>(default_material);
		uint32_t seed = hash_murmur3_one_float(p_position.x, hash_murmur3_one_float(p_position.z));
		auto rnd = [&]() {
			seed = hash_murmur3_one_32(seed);
			return float(seed & 0xffffu) / 65535.0f;
		};
		for (int k = 0; k < count; k++) {
			const Vector3 size = Vector3(0.6f + 0.8f * rnd(), 0.5f + 0.6f * rnd(), 0.6f + 0.8f * rnd()) * piece;
			// A broken slab chunk: a coarse, squashed ball with its corners pushed in and out at random, so the
			// pieces are rounded and irregular, as snow blocks break.
			Vector<Vector3> pts;
			Vector<Vector3> nrm;
			Vector<int> idx;
			constexpr int SEG = 7;
			constexpr int RINGS = 5;
			for (int r = 0; r <= RINGS; r++) {
				const float v = float(r) / RINGS;
				const float phi = float(Math::PI) * v;
				for (int sgm = 0; sgm <= SEG; sgm++) {
					const float th = float(Math::TAU) * (sgm % SEG) / SEG;
					Vector3 d(Math::sin(phi) * Math::cos(th), Math::cos(phi), Math::sin(phi) * Math::sin(th));
					// One jitter per corner, shared by the seam's two copies.
					uint32_t hh = hash_murmur3_one_32(uint32_t(r * 31 + (sgm % SEG)), seed);
					const float j = (r == 0 || r == RINGS) ? 0.85f : 0.7f + 0.45f * float(hh & 0xffffu) / 65535.0f;
					pts.push_back(Vector3(d.x * size.x, d.y * size.y, d.z * size.z) * 0.5f * j);
					nrm.push_back(d);
				}
			}
			for (int r = 0; r < RINGS; r++) {
				for (int sgm = 0; sgm < SEG; sgm++) {
					const int a0 = r * (SEG + 1) + sgm, a1 = a0 + 1, b0 = a0 + SEG + 1, b1 = b0 + 1;
					idx.push_back(a0);
					idx.push_back(b0);
					idx.push_back(a1);
					idx.push_back(a1);
					idx.push_back(b0);
					idx.push_back(b1);
				}
			}
			Ref<ArrayMesh> chunk;
			chunk.instantiate();
			Array arr;
			arr.resize(Mesh::ARRAY_MAX);
			arr[Mesh::ARRAY_VERTEX] = pts;
			arr[Mesh::ARRAY_NORMAL] = nrm;
			arr[Mesh::ARRAY_INDEX] = idx;
			chunk->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arr);
			chunk->surface_set_material(0, mat);
			RigidBody3D *rb = memnew(RigidBody3D);
			rb->set_mass(MAX(size.x * size.y * size.z * 0.5f * 450.0f, 1.0f));
			CollisionShape3D *cs3 = memnew(CollisionShape3D);
			cs3->set_shape(chunk->create_convex_shape(true, true));
			rb->add_child(cs3);
			MeshInstance3D *mi = memnew(MeshInstance3D);
			mi->set_mesh(chunk);
			rb->add_child(mi);
			add_child(rb, false, INTERNAL_MODE_BACK);
			// Where the overhang was: out from the crest top, its top level with it.
			const Vector3 at = top + out * (p_length * (0.3f + 0.7f * rnd())) + across * (cs * (rnd() - 0.5f) * 0.9f) + Vector3(0.0f, -0.5f * size.y - 0.2f * piece * rnd(), 0.0f);
			rb->set_global_transform(Transform3D(Basis::from_euler(Vector3(rnd(), rnd(), rnd()) * 0.4f), at));
			rb->set_linear_velocity(out * (0.5f + rnd()) + Vector3(0.0f, -0.5f, 0.0f));
			rb->set_angular_velocity(across * (1.0f + 2.0f * rnd()));
			Debris d;
			d.node = rb->get_instance_id();
			debris.push_back(d);
		}
	}
	rebuild();
}

void SnowCornice3D::set_debris_enabled(bool p_enabled) {
	debris_enabled = p_enabled;
}

void SnowCornice3D::set_debris_lifetime(float p_seconds) {
	debris_lifetime = MAX(p_seconds, 0.0f);
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
	ClassDB::bind_method(D_METHOD("get_debris_count"), &SnowCornice3D::get_debris_count);
	ClassDB::bind_method(D_METHOD("set_debris_enabled", "enabled"), &SnowCornice3D::set_debris_enabled);
	ClassDB::bind_method(D_METHOD("is_debris_enabled"), &SnowCornice3D::is_debris_enabled);
	ClassDB::bind_method(D_METHOD("set_debris_lifetime", "seconds"), &SnowCornice3D::set_debris_lifetime);
	ClassDB::bind_method(D_METHOD("get_debris_lifetime"), &SnowCornice3D::get_debris_lifetime);
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
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "debris_enabled"), "set_debris_enabled", "is_debris_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "debris_lifetime", PROPERTY_HINT_RANGE, "0,600,0.1,suffix:s"), "set_debris_lifetime", "get_debris_lifetime");
}

/**************************************************************************/
/*  ground_clods_3d.cpp                                                   */
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


#include "ground_clods_3d.h"

#include "core/config/engine.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/templates/hash_map.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/multimesh_instance_3d.h"
#include "scene/main/viewport.h"
#include "servers/rendering/rendering_server.h"

namespace {

constexpr float RIM_STEP = 0.002f; // m of new rim on a scan cell before it counts
constexpr float CLOD_FILL = 0.32f; // a clod's volume over its size cubed
constexpr float MIN_SIZE = 0.008f; // m: smaller is the film and crumbs the ground already draws
constexpr float MAX_SIZE = 0.09f;
constexpr float SINK = 0.12f; // of its size, pressed into the ground it fell on
constexpr float STICKY = 0.3f; // soil this sticky holds together as clods (MudSpray's "mud")

float _hash01(uint32_t p_h) {
	p_h ^= p_h >> 16;
	p_h *= 0x7feb352du;
	p_h ^= p_h >> 15;
	p_h *= 0x846ca68bu;
	p_h ^= p_h >> 16;
	return float(p_h & 0xffffffu) / float(0x1000000);
}

float _lump(const Vector3 &p_dir, uint32_t p_seed) {
	const Vector3 p = p_dir * 2.6f;
	const Vector3 f = p.floor();
	const Vector3 t = p - f;
	const Vector3 s = t * t * (Vector3(3, 3, 3) - t * 2.0f);
	float v[8];
	for (int i = 0; i < 8; i++) {
		const int x = int(f.x) + (i & 1), y = int(f.y) + ((i >> 1) & 1), z = int(f.z) + ((i >> 2) & 1);
		v[i] = _hash01(uint32_t(x * 73856093) ^ uint32_t(y * 19349663) ^ uint32_t(z * 83492791) ^ (p_seed * 2654435761u));
	}
	const float x0 = Math::lerp(Math::lerp(v[0], v[1], s.x), Math::lerp(v[2], v[3], s.x), s.y);
	const float x1 = Math::lerp(Math::lerp(v[4], v[5], s.x), Math::lerp(v[6], v[7], s.x), s.y);
	return Math::lerp(x0, x1, s.z) * 2.0f - 1.0f;
}

// The clay's colour there: the mud map's, or the surface's mud colour where the map holds none (black).
Color _clay_at(const GroundSurface3D *p_surface, const Vector3 &p_at) {
	const Color c = p_surface->get_mud_color_at(p_at);
	return c.r + c.g + c.b > 0.01f ? c : p_surface->get_mud_color();
}

Vector3 _camera_or(const Node *p_node, const Vector3 &p_fallback) {
	const Viewport *vp = p_node->get_viewport();
	const Camera3D *cam = vp ? vp->get_camera_3d() : nullptr;
	return cam ? cam->get_global_position() : p_fallback;
}

} // namespace

// A clod of unit size: an icosphere (320 triangles) made lumpy, flattened, with two or three fracture faces where
// it tore from the rim or split on landing: clay breaks along planes (Dexter 1988), so a clod is angular.
Ref<ArrayMesh> GroundClods3D::make_shape(int p_seed) {
	LocalVector<Vector3> verts;
	LocalVector<int> tris;
	const float t = (1.0f + Math::sqrt(5.0f)) * 0.5f;
	const Vector3 base[12] = { Vector3(-1, t, 0), Vector3(1, t, 0), Vector3(-1, -t, 0), Vector3(1, -t, 0), Vector3(0, -1, t), Vector3(0, 1, t), Vector3(0, -1, -t), Vector3(0, 1, -t), Vector3(t, 0, -1), Vector3(t, 0, 1), Vector3(-t, 0, -1), Vector3(-t, 0, 1) };
	const int faces[60] = { 0, 11, 5, 0, 5, 1, 0, 1, 7, 0, 7, 10, 0, 10, 11, 1, 5, 9, 5, 11, 4, 11, 10, 2, 10, 7, 6, 7, 1, 8, 3, 9, 4, 3, 4, 2, 3, 2, 6, 3, 6, 8, 3, 8, 9, 4, 9, 5, 2, 4, 11, 6, 2, 10, 8, 6, 7, 9, 8, 1 };
	for (int i = 0; i < 12; i++) {
		verts.push_back(base[i].normalized());
	}
	for (int i = 0; i < 60; i++) {
		tris.push_back(faces[i]);
	}
	for (int level = 0; level < 2; level++) {
		HashMap<uint64_t, int> mid;
		auto midpoint = [&](int a, int b) {
			const uint64_t k = a < b ? (uint64_t(a) << 32 | uint32_t(b)) : (uint64_t(b) << 32 | uint32_t(a));
			if (const int *e = mid.getptr(k)) {
				return *e;
			}
			verts.push_back((verts[a] + verts[b]).normalized());
			mid[k] = int(verts.size()) - 1;
			return int(verts.size()) - 1;
		};
		LocalVector<int> next;
		for (uint32_t i = 0; i < tris.size(); i += 3) {
			const int a = tris[i], b = tris[i + 1], c = tris[i + 2];
			const int ab = midpoint(a, b), bc = midpoint(b, c), ca = midpoint(c, a);
			const int n[12] = { a, ab, ca, b, bc, ab, c, ca, bc, ab, bc, ca };
			for (int j = 0; j < 12; j++) {
				next.push_back(n[j]);
			}
		}
		tris = next;
	}
	RandomPCG r(uint64_t(p_seed) * 6151u + 7u);
	const int cuts = 2 + int(r.rand() % 2);
	Vector3 cut_n[3];
	float cut_d[3];
	for (int c = 0; c < cuts; c++) {
		const float az = r.randf() * float(Math::TAU);
		cut_n[c] = Vector3(Math::cos(az), r.random(-0.5f, 0.5f), Math::sin(az)).normalized();
		cut_d[c] = r.random(0.3f, 0.55f);
	}
	const Vector3 squash(r.random(0.85f, 1.15f), r.random(0.55f, 0.75f), r.random(0.85f, 1.15f));
	for (Vector3 &v : verts) {
		const Vector3 dir = v;
		const float rad = 1.0f + 0.25f * _lump(dir, uint32_t(p_seed)) + 0.08f * _lump(dir * 3.1f, uint32_t(p_seed) + 77u);
		Vector3 p = dir * rad;
		for (int c = 0; c < cuts; c++) {
			const float over = p.dot(cut_n[c]) - cut_d[c];
			if (over > 0.0f) {
				p -= cut_n[c] * over * 0.95f;
			}
		}
		v = p * squash * 0.5f;
	}
	LocalVector<Vector3> normals;
	normals.resize(verts.size());
	for (Vector3 &n : normals) {
		n = Vector3();
	}
	for (uint32_t i = 0; i < tris.size(); i += 3) {
		const Vector3 fn = (verts[tris[i + 1]] - verts[tris[i]]).cross(verts[tris[i + 2]] - verts[tris[i]]); // outward
		for (int j = 0; j < 3; j++) {
			normals[tris[i + j]] += fn;
		}
	}
	PackedVector3Array pv, pn;
	PackedInt32Array pi;
	pv.resize(verts.size());
	pn.resize(verts.size());
	for (uint32_t i = 0; i < verts.size(); i++) {
		pv.set(i, verts[i]);
		pn.set(i, normals[i].normalized());
	}
	pi.resize(tris.size());
	for (uint32_t i = 0; i < tris.size(); i += 3) {
		// Godot's front faces wind clockwise.
		pi.set(i, tris[i]);
		pi.set(i + 1, tris[i + 2]);
		pi.set(i + 2, tris[i + 1]);
	}
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = pv;
	arrays[Mesh::ARRAY_NORMAL] = pn;
	arrays[Mesh::ARRAY_INDEX] = pi;
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	return mesh;
}

GroundSurface3D *GroundClods3D::_surface() const {
	if (!surface_path.is_empty() && is_inside_tree()) {
		if (GroundSurface3D *s = Object::cast_to<GroundSurface3D>(get_node_or_null(surface_path))) {
			return s;
		}
	}
	return Object::cast_to<GroundSurface3D>(get_parent());
}

void GroundClods3D::_ensure_draw() {
	if (material.is_null()) {
		// Clay in its own colour per clod (the instance colour), matte as a broken face of clay is.
		Ref<StandardMaterial3D> m;
		m.instantiate();
		m->set_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
		m->set_albedo(Color(1, 1, 1));
		m->set_roughness(0.82f);
		material = m;
	}
	for (int i = 0; i < SHAPES; i++) {
		if (shapes[i].is_null()) {
			shapes[i] = make_shape(i + 1);
		}
	}
	lump_draw.ensure(this, shapes, SHAPES, true, material);
}

void GroundClods3D::_free_draw() {
	lump_draw.release();
}

// Thrown clay that landed: a landing holding a lump's volume stands up as one to three clods (a lump splits as it
// lands), the rest stays the film the ground drew.
void GroundClods3D::_land(GroundSurface3D *p_surface) {
	p_surface->take_clod_drops(drops);
	if (!drops.is_empty()) {
		_watch(p_surface);
	}
	for (const GroundSurface3D::ClodDrop &d : drops) {
		float vol = d.volume;
		const int pieces = 1 + int(rng.rand() % 3);
		for (int k = 0; k < pieces; k++) {
			const float share = k == pieces - 1 ? vol : vol * rng.random(0.3f, 0.7f);
			const float size = Math::pow(share / CLOD_FILL, 1.0f / 3.0f);
			if (size < MIN_SIZE) {
				break;
			}
			vol -= share;
			const float a = rng.randf() * float(Math::TAU);
			const Vector3 at = d.position + Vector3(Math::cos(a), 0.0f, Math::sin(a)) * rng.randf() * d.radius;
			_spawn(p_surface, at, Vector2(), MIN(size, MAX_SIZE), 0.0f, d.color.darkened(0.3f * CLAMP(d.wet, 0.0f, 1.0f)));
			landed_total++;
		}
	}
	drops.clear();
}

void GroundClods3D::_watch(GroundSurface3D *p_surface) {
	scan.main_field = &p_surface->get_deform_field();
	scan.fine_field = p_surface->get_near_deform_field();
	// Inside the near field's blend into the main one (GroundSurface3D::NEAR_BLEND, 1 m).
	scan.fine_rect = scan.fine_field ? p_surface->get_near_rect().grow(-1.0f) : Rect2();
}

void GroundClods3D::_scan(GroundSurface3D *p_surface) {
	const Rect2 field = p_surface->get_deform_rect();
	const float texel = p_surface->get_deform_extent() / MAX(p_surface->get_deform_resolution(), 1);
	const Vector3 cam = _camera_or(this, Vector3(field.get_center().x, 0.0f, field.get_center().y));
	scan.update(cam, radius, MAX(texel * 2.0f, 0.05f), RIM_STEP, grown);
	for (const GroundLumpScan::Grown &g : grown) {
		const Vector3 &c = g.center;
		// Plastic clay tears out as clods; sand crumbles, slurry flows (stickiness falls off for both).
		const float sticky = p_surface->get_soil(c).stickiness;
		if (sticky < STICKY) {
			continue;
		}
		float &owed = scan.owed[g.index];
		owed += g.grew * scan.cell * scan.cell * break_share * Math::smoothstep(STICKY, 0.7f, sticky);
		Color col;
		bool have_col = false;
		for (int k = 0; k < 6; k++) {
			// Log-normal aggregate sizes (Gardner 1956), median 2.5 cm.
			const float size = CLAMP(0.025f * Math::exp(0.55f * rng.randfn(0.0f, 1.0f)), MIN_SIZE, MAX_SIZE);
			const float vol = CLOD_FILL * size * size * size;
			if (owed < vol) {
				break;
			}
			owed -= vol;
			if (!have_col) {
				// A torn face is the moist inside of the clay: darker than the surface dried by the air.
				col = _clay_at(p_surface, c).darkened(0.08f);
				have_col = true;
			}
			const Vector3 at = c + Vector3(rng.random(-0.5f, 0.5f) * scan.cell, 0.0f, rng.random(-0.5f, 0.5f) * scan.cell);
			_spawn(p_surface, at, g.out, size, MIN(0.1f, 3.0f * size), col);
		}
	}
}

void GroundClods3D::_spawn(GroundSurface3D *p_surface, const Vector3 &p_at, const Vector2 &p_out, float p_size, float p_throw, const Color &p_color) {
	const float throw_len = rng.randf() * p_throw;
	const Vector2 side(-p_out.y, p_out.x);
	const Vector2 d = p_out * throw_len + side * rng.random(-0.3f, 0.3f) * throw_len;
	Clod c;
	c.size = p_size;
	c.shape = uint8_t(rng.rand() % SHAPES);
	const Vector3 pos(p_at.x + d.x, 0.0f, p_at.z + d.y);
	c.relief = scan.relief_at(pos);
	c.position = Vector3(pos.x, p_surface->get_surface_height_at(pos) + p_size * (0.32f - SINK), pos.z);
	const Basis yaw(Vector3(0, 1, 0), rng.randf() * float(Math::TAU));
	const Basis tilt(Vector3(rng.random(-1.0f, 1.0f), 0.0f, rng.random(-1.0f, 1.0f)).normalized(), rng.random(0.0f, 0.6f));
	// Each clod stretched its own way and its clay a shade of its own, so the shared shapes do not read as copies.
	const Basis stretch = Basis::from_scale(Vector3(rng.random(0.8f, 1.2f), rng.random(0.75f, 1.2f), rng.random(0.8f, 1.2f)));
	c.basis = tilt * yaw * stretch;
	const float shade = rng.random(0.88f, 1.08f);
	c.color = Color(CLAMP(p_color.r * shade, 0.0f, 1.0f), CLAMP(p_color.g * shade, 0.0f, 1.0f), CLAMP(p_color.b * shade, 0.0f, 1.0f));
	if (int(clods.size()) < max_clods) {
		clods.push_back(c);
	} else if (max_clods > 0) {
		clods[next_slot % clods.size()] = c;
		next_slot = (next_slot + 1) % clods.size();
	}
	spawned_total++;
	draw_dirty = true;
}

void GroundClods3D::_remove(uint32_t p_index) {
	clods[p_index] = clods[clods.size() - 1];
	clods.resize(clods.size() - 1);
	draw_dirty = true;
}

// A later print that presses a clod in or heaps over it crushes or hides it; water standing over it slakes it
// (dry clay aggregates fall apart when wetted, Emerson 1967); far behind the camera it is let go.
void GroundClods3D::_check(GroundSurface3D *p_surface, bool p_water) {
	const Rect2 keep = p_surface->get_deform_rect().grow(p_surface->get_deform_extent() * 0.5f);
	for (uint32_t i = 0; i < clods.size();) {
		const Clod &c = clods[i];
		bool gone = !keep.has_point(Vector2(c.position.x, c.position.z));
		if (!gone && scan.changed_at(c.position)) {
			// Only where the field moved since the last look: a print pressed it in or heaped over it.
			const float h = scan.relief_at(c.position);
			gone = h < c.relief - 0.5f * c.size || h > c.relief + 0.6f * c.size;
		}
		if (!gone && p_water) {
			gone = p_surface->get_standing_water_at(c.position) > 0.5f * c.size;
		}
		if (gone) {
			_remove(i);
		} else {
			i++;
		}
	}
	next_slot = clods.is_empty() ? 0 : next_slot % clods.size();
}

void GroundClods3D::_draw() {
	if (!is_inside_tree()) {
		return;
	}
	_ensure_draw();
	draw_dirty = false;
	for (const Clod &c : clods) {
		const Basis b = c.basis.scaled(Vector3(c.size, c.size, c.size));
		LocalVector<float> &out = lump_draw.buffers[c.shape % SHAPES];
		// Instance colours reach the shader as they are (linear); the clay's colour is sRGB, as albedos are given.
		const Color lin = c.color.srgb_to_linear();
		const float row[16] = { b.rows[0].x, b.rows[0].y, b.rows[0].z, c.position.x, b.rows[1].x, b.rows[1].y, b.rows[1].z, c.position.y, b.rows[2].x, b.rows[2].y, b.rows[2].z, c.position.z, lin.r, lin.g, lin.b, 1.0f };
		for (int k = 0; k < 16; k++) {
			out.push_back(row[k]);
		}
	}
	lump_draw.commit();
}

void GroundClods3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			rng.seed(uint64_t(seed) * 2654435761u + 3u);
			_ensure_draw();
			set_physics_process_internal(true);
		} break;
		case NOTIFICATION_EXIT_TREE: {
			set_physics_process_internal(false);
			_free_draw();
		} break;
		case NOTIFICATION_INTERNAL_PHYSICS_PROCESS: {
			if (!enabled) {
				break;
			}
			// Landings every physics frame (the surface keeps them only a couple of frames), the rest at update_rate.
			if (GroundSurface3D *surface = _surface()) {
				_land(surface);
			}
			since_scan += get_physics_process_delta_time();
			if (since_scan >= 1.0 / MAX(update_rate, 0.1f)) {
				since_scan = 0.0;
				update_now();
			}
		} break;
	}
}

void GroundClods3D::update_now() {
	if (!enabled || !is_inside_tree()) {
		return;
	}
	GroundSurface3D *surface = _surface();
	if (!surface) {
		return;
	}
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	_watch(surface);
	_land(surface);
	_scan(surface);
	if (!clods.is_empty()) {
		// Standing water changes slowly: looked at once a second.
		water_wait += 1;
		const bool water = surface->is_standing_water() && water_wait >= int(update_rate);
		if (water) {
			water_wait = 0;
		}
		_check(surface, water);
	}
	if (draw_dirty) {
		_draw();
	}
	const double ms = double(OS::get_singleton()->get_ticks_usec() - t0) / 1000.0;
	update_msec_total += ms;
	update_msec_worst = MAX(update_msec_worst, ms);
	update_runs++;
}

void GroundClods3D::clear() {
	clods.clear();
	next_slot = 0;
	scan.clear();
	draw_dirty = true;
	if (is_inside_tree()) {
		_draw();
	}
}

void GroundClods3D::set_surface(const NodePath &p_path) {
	surface_path = p_path;
	clear();
}

void GroundClods3D::set_enabled(bool p_enabled) {
	if (enabled == p_enabled) {
		return;
	}
	enabled = p_enabled;
	if (!enabled) {
		clear();
	}
}

void GroundClods3D::set_max_clods(int p_max) {
	max_clods = MAX(p_max, 0);
	if (int(clods.size()) > max_clods) {
		clods.resize(max_clods);
		next_slot = 0;
		draw_dirty = true;
	}
}

void GroundClods3D::set_material(const Ref<Material> &p_material) {
	material = p_material;
	lump_draw.set_material(material);
}

void GroundClods3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_surface", "path"), &GroundClods3D::set_surface);
	ClassDB::bind_method(D_METHOD("get_surface"), &GroundClods3D::get_surface);
	ClassDB::bind_method(D_METHOD("set_enabled", "enabled"), &GroundClods3D::set_enabled);
	ClassDB::bind_method(D_METHOD("is_enabled"), &GroundClods3D::is_enabled);
	ClassDB::bind_method(D_METHOD("set_break_share", "share"), &GroundClods3D::set_break_share);
	ClassDB::bind_method(D_METHOD("get_break_share"), &GroundClods3D::get_break_share);
	ClassDB::bind_method(D_METHOD("set_radius", "radius"), &GroundClods3D::set_radius);
	ClassDB::bind_method(D_METHOD("get_radius"), &GroundClods3D::get_radius);
	ClassDB::bind_method(D_METHOD("set_max_clods", "max"), &GroundClods3D::set_max_clods);
	ClassDB::bind_method(D_METHOD("get_max_clods"), &GroundClods3D::get_max_clods);
	ClassDB::bind_method(D_METHOD("set_seed", "seed"), &GroundClods3D::set_seed);
	ClassDB::bind_method(D_METHOD("get_seed"), &GroundClods3D::get_seed);
	ClassDB::bind_method(D_METHOD("set_material", "material"), &GroundClods3D::set_material);
	ClassDB::bind_method(D_METHOD("get_material"), &GroundClods3D::get_material);
	ClassDB::bind_method(D_METHOD("update_now"), &GroundClods3D::update_now);
	ClassDB::bind_method(D_METHOD("clear"), &GroundClods3D::clear);
	ClassDB::bind_method(D_METHOD("get_clod_count"), &GroundClods3D::get_clod_count);
	ClassDB::bind_method(D_METHOD("get_spawned_total"), &GroundClods3D::get_spawned_total);
	ClassDB::bind_method(D_METHOD("get_landed_total"), &GroundClods3D::get_landed_total);
	ClassDB::bind_method(D_METHOD("get_update_msec_mean"), &GroundClods3D::get_update_msec_mean);
	ClassDB::bind_method(D_METHOD("get_update_msec_worst"), &GroundClods3D::get_update_msec_worst);
	ClassDB::bind_method(D_METHOD("get_update_runs"), &GroundClods3D::get_update_runs);

	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "surface", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "GroundSurface3D"), "set_surface", "get_surface");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enabled"), "set_enabled", "is_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "break_share", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_break_share", "get_break_share");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "radius", PROPERTY_HINT_RANGE, "1,50,0.5,suffix:m"), "set_radius", "get_radius");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_clods", PROPERTY_HINT_RANGE, "0,200000,1"), "set_max_clods", "get_max_clods");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "seed"), "set_seed", "get_seed");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "material", PROPERTY_HINT_RESOURCE_TYPE, "BaseMaterial3D,ShaderMaterial"), "set_material", "get_material");
}

GroundClods3D::GroundClods3D() {
}

GroundClods3D::~GroundClods3D() {
}

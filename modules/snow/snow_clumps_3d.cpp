/**************************************************************************/
/*  snow_clumps_3d.cpp                                                    */
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

#include "snow_clumps_3d.h"

#include "snow_cover_3d.h"
#include "snow_surface_3d.h"

#include "core/object/class_db.h"
#include "core/os/os.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/multimesh_instance_3d.h"
#include "scene/main/viewport.h"
#include "servers/rendering/rendering_server.h"

namespace {

constexpr float RIM_STEP = 0.002f; // m of new rim on a scan cell before it counts (below: slumping noise)
constexpr float CLUMP_FILL = 0.3f; // a clump's volume over its size cubed (a flattened lumpy ball)
constexpr float MIN_SIZE = 0.01f; // m: smaller melts away or reads as the rim itself
constexpr float MAX_SIZE = 0.1f;
constexpr float SINK = 0.15f; // of its size, sat into the snow it fell on
constexpr float DEGREE_DAY = 0.004f; // m water a day per degree above 0 (snow, Hock 2003: 3-6 mm/d/C)
constexpr float CLUMP_DENSITY = 400.0f; // kg/m3, sintered lumps
constexpr float PARK_MARGIN = 4.0f; // m past the print field before a clump is kept away

float _hash01(uint32_t p_h) {
	p_h ^= p_h >> 16;
	p_h *= 0x7feb352du;
	p_h ^= p_h >> 15;
	p_h *= 0x846ca68bu;
	p_h ^= p_h >> 16;
	return float(p_h & 0xffffffu) / float(0x1000000);
}

// Value noise on the unit sphere's directions, smooth over about a quarter of the clump.
float _lump(const Vector3 &p_dir, uint32_t p_seed) {
	const Vector3 p = p_dir * 2.2f;
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

} // namespace

// A clump of unit size: an icosphere (320 triangles) made lumpy, flattened as a lump of snow lies, with one side
// a fracture face where it broke from the rim.
Ref<ArrayMesh> SnowClumps3D::make_shape(int p_seed) {
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
	RandomPCG r(uint64_t(p_seed) * 7919u + 13u);
	// The break: a plane a third of the way in on one side, tilted from the vertical.
	const float az = r.randf() * float(Math::TAU);
	const Vector3 cut_n = Vector3(Math::cos(az), r.random(-0.35f, 0.25f), Math::sin(az)).normalized();
	const float cut_d = r.random(0.25f, 0.45f);
	const Vector3 squash(r.random(0.85f, 1.1f), r.random(0.6f, 0.75f), r.random(0.85f, 1.1f));
	for (Vector3 &v : verts) {
		const Vector3 dir = v;
		float rad = 1.0f + 0.22f * _lump(dir, uint32_t(p_seed)) + 0.07f * _lump(dir * 2.7f, uint32_t(p_seed) + 101u);
		Vector3 p = dir * rad;
		const float over = p.dot(cut_n) - cut_d;
		if (over > 0.0f) {
			p -= cut_n * over * 0.92f; // a rough break, not a sawn face
		}
		v = p * squash * 0.5f; // unit size across
	}
	// Rest it on its flattest side: the lowest point at y = -0.5 x its height is fine, centred instead.
	LocalVector<Vector3> normals;
	normals.resize(verts.size());
	for (Vector3 &n : normals) {
		n = Vector3();
	}
	for (uint32_t i = 0; i < tris.size(); i += 3) {
		const Vector3 fn = (verts[tris[i + 1]] - verts[tris[i]]).cross(verts[tris[i + 2]] - verts[tris[i]]); // outward (the icosahedron winds counter-clockwise from outside)
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
	// Godot's front faces wind clockwise.
	pi.resize(tris.size());
	for (uint32_t i = 0; i < tris.size(); i += 3) {
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

// Whether pushed snow holds together as lumps, 0..1. Grains sinter into a skeleton as the pack settles (dense
// snow), and liquid water binds them (Colbeck 1982); cold dry new snow is cohesionless and sprays.
float SnowClumps3D::cohesion(float p_density, float p_wetness, float p_temperature) {
	const float settled = Math::smoothstep(120.0f, 320.0f, p_density);
	const float warm = Math::smoothstep(-14.0f, -3.0f, p_temperature);
	const float wet = Math::smoothstep(0.0f, 0.15f, p_wetness);
	return CLAMP(MAX(settled * MAX(warm, 0.3f), wet), 0.0f, 1.0f);
}

SnowSurface3D *SnowClumps3D::_surface() const {
	if (!surface_path.is_empty() && is_inside_tree()) {
		if (SnowSurface3D *s = Object::cast_to<SnowSurface3D>(get_node_or_null(surface_path))) {
			return s;
		}
	}
	return Object::cast_to<SnowSurface3D>(get_parent());
}

SnowCover3D *SnowClumps3D::_cover(SnowSurface3D *p_surface) const {
	if (p_surface && !p_surface->get_snow_cover().is_empty() && p_surface->is_inside_tree()) {
		if (SnowCover3D *c = Object::cast_to<SnowCover3D>(p_surface->get_node_or_null(p_surface->get_snow_cover()))) {
			return c;
		}
	}
	return SnowCover3D::get_active();
}

void SnowClumps3D::_ensure_draw() {
	if (material.is_null()) {
		// Pushed snow, sintered: as white as the pack, a little rougher where it broke.
		Ref<StandardMaterial3D> m;
		m.instantiate();
		m->set_albedo(Color(0.88f, 0.9f, 0.93f));
		m->set_roughness(0.72f);
		m->set_specular(0.335f); // ice, F0 0.018
		material = m;
	}
	for (int i = 0; i < SHAPES; i++) {
		if (shapes[i].is_null()) {
			shapes[i] = make_shape(i + 1);
		}
	}
	lump_draw.ensure(this, shapes, SHAPES, false, material);
}

void SnowClumps3D::_free_draw() {
	lump_draw.release();
}

static Vector3 _camera_or(const Node *p_node, const Vector3 &p_fallback) {
	const Viewport *vp = p_node->get_viewport();
	const Camera3D *cam = vp ? vp->get_camera_3d() : nullptr;
	return cam ? cam->get_global_position() : p_fallback;
}

void SnowClumps3D::_scan(SnowSurface3D *p_surface, SnowCover3D *p_cover) {
	const Rect2 field = p_surface->get_deform_rect();
	const float texel = p_surface->get_deform_extent() / MAX(p_surface->get_deform_resolution(), 1);
	const Vector3 cam = _camera_or(this, Vector3(field.get_center().x, 0.0f, field.get_center().y));
	scan.main_field = &p_surface->get_deform_field();
	scan.fine_field = nullptr;
	scan.update(cam, radius, texel * 2.0f, RIM_STEP, grown);
	for (const GroundLumpScan::Grown &g : grown) {
		const Vector3 &c = g.center;
		const float coh = p_cover ? cohesion(p_cover->get_density_at(c), p_cover->get_wetness_at(c), p_cover->get_air_temperature_at(c)) : 0.6f;
		if (coh <= 0.01f) {
			continue;
		}
		float &owed = scan.owed[g.index];
		owed += g.grew * scan.cell * scan.cell * break_share * coh;
		const float wet = p_cover ? p_cover->get_wetness_at(c) : 0.0f;
		for (int k = 0; k < 6; k++) {
			// Log-normal sizes (avalanche debris granules, Bartelt and McArdell 2009), larger in wet snow.
			const float median = 0.022f + 0.03f * wet;
			const float size = CLAMP(median * Math::exp(0.5f * rng.randfn(0.0f, 1.0f)), MIN_SIZE, MAX_SIZE);
			const float vol = CLUMP_FILL * size * size * size;
			if (owed < vol) {
				break;
			}
			owed -= vol;
			const Vector3 at = c + Vector3(rng.random(-0.5f, 0.5f) * scan.cell, 0.0f, rng.random(-0.5f, 0.5f) * scan.cell);
			_spawn(p_surface, p_cover, at, g.out, size);
		}
	}
}

void SnowClumps3D::_spawn(SnowSurface3D *p_surface, SnowCover3D *p_cover, const Vector3 &p_at, const Vector2 &p_out, float p_size) {
	// Thrown a little past the rim: up to a few of its own sizes and 12 cm.
	const float throw_len = rng.randf() * MIN(0.12f, 3.0f * p_size);
	const Vector2 side(-p_out.y, p_out.x);
	const Vector2 d = p_out * throw_len + side * rng.random(-0.3f, 0.3f) * throw_len;
	Clump c;
	c.size = p_size;
	c.shape = uint8_t(rng.rand() % SHAPES);
	const Vector3 pos(p_at.x + d.x, 0.0f, p_at.z + d.y);
	c.relief = scan.relief_at(pos);
	// Height of the shape is about 0.7 of its size: its centre a third of its size up, less what it sank.
	c.position = Vector3(pos.x, p_surface->get_surface_height_at(pos) + p_size * (0.34f - SINK), pos.z);
	const Basis yaw(Vector3(0, 1, 0), rng.randf() * float(Math::TAU));
	const Basis tilt(Vector3(rng.random(-1.0f, 1.0f), 0.0f, rng.random(-1.0f, 1.0f)).normalized(), rng.random(0.0f, 0.5f));
	// Each clump stretched its own way, so the few shared shapes do not read as copies.
	const Basis stretch = Basis::from_scale(Vector3(rng.random(0.8f, 1.2f), rng.random(0.75f, 1.15f), rng.random(0.8f, 1.2f)));
	c.basis = tilt * yaw * stretch;
	if (int(clumps.size()) < max_clumps) {
		clumps.push_back(c);
	} else if (max_clumps > 0) {
		clumps[next_slot % clumps.size()] = c;
		next_slot = (next_slot + 1) % clumps.size();
	}
	spawned_total++;
	draw_dirty = true;
}

void SnowClumps3D::_age(SnowSurface3D *p_surface, SnowCover3D *p_cover) {
	const float hours = p_cover ? p_cover->get_last_step_hours() : 0.0f;
	for (uint32_t i = 0; i < clumps.size();) {
		Clump &c = clumps[i];
		bool gone = false;
		if (p_cover && hours > 0.0f) {
			// A thaw takes it from the outside in (degree days on the lump's surface, both sides).
			const float t = p_cover->get_air_temperature_at(c.position);
			if (t > 0.0f) {
				c.size -= 2.0f * DEGREE_DAY * t * hours / 24.0f * 1000.0f / CLUMP_DENSITY;
			}
			c.buried += p_cover->get_new_snow_at(c.position);
			gone = c.size < MIN_SIZE * 0.5f || c.buried > 0.6f * c.size;
		}
		if (gone) {
			clumps[i] = clumps[clumps.size() - 1];
			clumps.resize(clumps.size() - 1);
			draw_dirty = true;
		} else {
			i++;
		}
	}
	next_slot = clumps.is_empty() ? 0 : next_slot % clumps.size();
	draw_dirty = true;
}

// A later print that presses a clump into the snow crushes it, and one that heaps over it hides it.
void SnowClumps3D::_crush(SnowSurface3D *p_surface) {
	// Past the field (and a margin, so it does not come and go): kept in the store while the camera is away.
	const Rect2 keep = p_surface->get_deform_rect().grow(PARK_MARGIN);
	for (uint32_t i = 0; i < clumps.size();) {
		const Clump &c = clumps[i];
		if (!keep.has_point(Vector2(c.position.x, c.position.z))) {
			store.park(c);
			clumps[i] = clumps[clumps.size() - 1];
			clumps.resize(clumps.size() - 1);
			draw_dirty = true;
			continue;
		}
		// Only where the print field moved since the last look (new snow buries it in _age()).
		const float h = scan.changed_at(c.position) ? scan.relief_at(c.position) : c.relief;
		if (h < c.relief - 0.5f * c.size || h > c.relief + 0.6f * c.size) {
			clumps[i] = clumps[clumps.size() - 1];
			clumps.resize(clumps.size() - 1);
			draw_dirty = true;
		} else {
			i++;
		}
	}
	next_slot = clumps.is_empty() ? 0 : next_slot % clumps.size();
}

// The clumps kept while the camera was away, back where the field is again: as they lay if the print under them is
// as it was (the field's cached tiles), gone if it changed or was let go; waited for while the field there is empty.
void SnowClumps3D::_restore(SnowSurface3D *p_surface) {
	back.clear();
	store.take(p_surface->get_deform_rect(), back);
	for (GroundLump &l : back) {
		const float h = scan.relief_at(l.position);
		if (h == 0.0f && Math::abs(l.relief) > 0.002f && l.tries < 30) {
			l.tries++;
			store.park(l);
			continue;
		}
		if (Math::abs(h - l.relief) > 0.5f * l.size || int(clumps.size()) >= max_clumps) {
			continue;
		}
		l.tries = 0;
		clumps.push_back(l);
		draw_dirty = true;
	}
}

void SnowClumps3D::_draw() {
	if (!is_inside_tree()) {
		return;
	}
	_ensure_draw();
	draw_dirty = false;
	for (const Clump &c : clumps) {
		const Basis b = c.basis.scaled(Vector3(c.size, c.size, c.size));
		LocalVector<float> &out = lump_draw.buffers[c.shape % SHAPES];
		const float row[12] = { b.rows[0].x, b.rows[0].y, b.rows[0].z, c.position.x, b.rows[1].x, b.rows[1].y, b.rows[1].z, c.position.y, b.rows[2].x, b.rows[2].y, b.rows[2].z, c.position.z };
		for (int k = 0; k < 12; k++) {
			out.push_back(row[k]);
		}
	}
	lump_draw.commit();
}

void SnowClumps3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			rng.seed(uint64_t(seed) * 2654435761u + 1u);
			_ensure_draw();
			set_process_internal(true);
		} break;
		case NOTIFICATION_EXIT_TREE: {
			set_process_internal(false);
			_free_draw();
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			since_scan += get_process_delta_time();
			if (since_scan >= 1.0 / MAX(update_rate, 0.1f)) {
				since_scan = 0.0;
				update_now();
			}
		} break;
	}
}

void SnowClumps3D::update_now() {
	if (!enabled || !is_inside_tree()) {
		return;
	}
	SnowSurface3D *surface = _surface();
	if (!surface || !surface->is_deform_enabled()) {
		return;
	}
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	SnowCover3D *cover = _cover(surface);
	_scan(surface, cover);
	if (!clumps.is_empty()) {
		_crush(surface);
	}
	_restore(surface);
	if (cover && cover->get_update_count() != seen_cover_update) {
		seen_cover_update = cover->get_update_count();
		_age(surface, cover);
	}
	if (draw_dirty) {
		_draw();
	}
	const double ms = double(OS::get_singleton()->get_ticks_usec() - t0) / 1000.0;
	update_msec_total += ms;
	update_msec_worst = MAX(update_msec_worst, ms);
	update_runs++;
}

void SnowClumps3D::clear() {
	clumps.clear();
	store.clear(false);
	next_slot = 0;
	scan.clear();
	draw_dirty = true;
	if (is_inside_tree()) {
		_draw();
	}
}

void SnowClumps3D::set_surface(const NodePath &p_path) {
	surface_path = p_path;
	clear();
}

void SnowClumps3D::set_enabled(bool p_enabled) {
	if (enabled == p_enabled) {
		return;
	}
	enabled = p_enabled;
	if (!enabled) {
		clear();
	}
}

void SnowClumps3D::set_max_clumps(int p_max) {
	max_clumps = MAX(p_max, 0);
	if (int(clumps.size()) > max_clumps) {
		clumps.resize(max_clumps);
		next_slot = 0;
		draw_dirty = true;
	}
}

void SnowClumps3D::set_material(const Ref<Material> &p_material) {
	material = p_material;
	lump_draw.set_material(material);
}

void SnowClumps3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_surface", "path"), &SnowClumps3D::set_surface);
	ClassDB::bind_method(D_METHOD("get_surface"), &SnowClumps3D::get_surface);
	ClassDB::bind_method(D_METHOD("set_enabled", "enabled"), &SnowClumps3D::set_enabled);
	ClassDB::bind_method(D_METHOD("is_enabled"), &SnowClumps3D::is_enabled);
	ClassDB::bind_method(D_METHOD("set_break_share", "share"), &SnowClumps3D::set_break_share);
	ClassDB::bind_method(D_METHOD("get_break_share"), &SnowClumps3D::get_break_share);
	ClassDB::bind_method(D_METHOD("set_radius", "radius"), &SnowClumps3D::set_radius);
	ClassDB::bind_method(D_METHOD("get_radius"), &SnowClumps3D::get_radius);
	ClassDB::bind_method(D_METHOD("set_max_clumps", "max"), &SnowClumps3D::set_max_clumps);
	ClassDB::bind_method(D_METHOD("get_max_clumps"), &SnowClumps3D::get_max_clumps);
	ClassDB::bind_method(D_METHOD("set_seed", "seed"), &SnowClumps3D::set_seed);
	ClassDB::bind_method(D_METHOD("get_seed"), &SnowClumps3D::get_seed);
	ClassDB::bind_method(D_METHOD("set_material", "material"), &SnowClumps3D::set_material);
	ClassDB::bind_method(D_METHOD("get_material"), &SnowClumps3D::get_material);
	ClassDB::bind_method(D_METHOD("update_now"), &SnowClumps3D::update_now);
	ClassDB::bind_method(D_METHOD("clear"), &SnowClumps3D::clear);
	ClassDB::bind_method(D_METHOD("get_clump_count"), &SnowClumps3D::get_clump_count);
	ClassDB::bind_method(D_METHOD("get_spawned_total"), &SnowClumps3D::get_spawned_total);
	ClassDB::bind_method(D_METHOD("get_parked_count"), &SnowClumps3D::get_parked_count);
	ClassDB::bind_method(D_METHOD("get_update_msec_mean"), &SnowClumps3D::get_update_msec_mean);
	ClassDB::bind_method(D_METHOD("get_update_msec_worst"), &SnowClumps3D::get_update_msec_worst);
	ClassDB::bind_method(D_METHOD("get_update_runs"), &SnowClumps3D::get_update_runs);
	ClassDB::bind_static_method("SnowClumps3D", D_METHOD("cohesion", "density", "wetness", "temperature"), &SnowClumps3D::cohesion);

	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "surface", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "SnowSurface3D"), "set_surface", "get_surface");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enabled"), "set_enabled", "is_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "break_share", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_break_share", "get_break_share");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "radius", PROPERTY_HINT_RANGE, "1,50,0.5,suffix:m"), "set_radius", "get_radius");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_clumps", PROPERTY_HINT_RANGE, "0,200000,1"), "set_max_clumps", "get_max_clumps");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "seed"), "set_seed", "get_seed");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "material", PROPERTY_HINT_RESOURCE_TYPE, "BaseMaterial3D,ShaderMaterial"), "set_material", "get_material");
}

SnowClumps3D::SnowClumps3D() {
}

SnowClumps3D::~SnowClumps3D() {
}

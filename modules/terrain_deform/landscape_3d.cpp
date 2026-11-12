/**************************************************************************/
/*  landscape_3d.cpp                                                      */
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




#include "landscape_3d.h"

#include "terrain_generator_3d.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/templates/hash_set.h"
#include "core/templates/sort_array.h"
#include "scene/3d/camera_3d.h"
#include "scene/main/viewport.h"
#include "scene/resources/3d/world_3d.h"
#include "scene/scene_string_names.h"
#include "servers/physics_3d/physics_server_3d.h"
#include "servers/rendering/rendering_server.h"

#ifdef TOOLS_ENABLED
#include "editor/editor_interface.h"
#endif

// Frames an unused mesh (and so its BLAS) stays, in case the camera comes back.
static const uint64_t MESH_TTL = 600;
// Error of the levels below COARSE in a chunk not read in full: never picked.
static const float UNREAD = 1e30f;
// rebuild() reads before the first frame the chunks whose 4 m grid would be off by more than this many times
// lod_error there; the rest the view needs come in over the next frames, nearest first.
static const real_t FIRST_VIEW_SLACK = 4.0;

float Landscape3D::_h(int p_x, int p_z) const {
	return heights[CLAMP(p_z, 0, gh - 1) * gw + CLAMP(p_x, 0, gw - 1)];
}

// Central differences at 1 m. Where a chunk is not read yet its points between the 4 m grid are interpolated, and
// there this gives exactly the 4 m central difference at the 4 m points (the only ones its meshes use).
Vector3 Landscape3D::_normal(int p_x, int p_z) const {
	return Vector3(_h(p_x - 1, p_z) - _h(p_x + 1, p_z), 2.0f * spacing, _h(p_x, p_z - 1) - _h(p_x, p_z + 1)).normalized();
}

// Indices of a chunk at p_level, in vertex rows of (n + 1), n = chunk_quads >> p_level. The grid is cut into 2 x 2
// cell blocks, each a fan of 8 triangles around its centre vertex; where p_mask flags a side whose neighbour is one
// level coarser (bit 0 -z, 1 +x, 2 +z, 3 -x), the blocks on that side skip its odd vertex and fan to the corners,
// which lie on the neighbour's coarser edge. Corner blocks with two flagged sides work the same way. All are made
// in rebuild(), so worker threads only read them.
const LocalVector<int> &Landscape3D::_indices(int p_level, int p_mask) {
	LocalVector<int> &r = indices[p_level][p_mask];
	if (!r.is_empty()) {
		return r;
	}
	const int n = chunk_quads >> p_level;
	const int row = n + 1;
	const int nb = n / 2;
	r.reserve(n * n * 6);
	for (int b = 0; b < nb; b++) {
		for (int a = 0; a < nb; a++) {
			const int x = 2 * a;
			const int z = 2 * b;
			const int c = (z + 1) * row + x + 1;
			int ring[8];
			int k = 0;
			ring[k++] = z * row + x;
			if (!(b == 0 && (p_mask & 1))) {
				ring[k++] = z * row + x + 1;
			}
			ring[k++] = z * row + x + 2;
			if (!(a == nb - 1 && (p_mask & 2))) {
				ring[k++] = (z + 1) * row + x + 2;
			}
			ring[k++] = (z + 2) * row + x + 2;
			if (!(b == nb - 1 && (p_mask & 4))) {
				ring[k++] = (z + 2) * row + x + 1;
			}
			ring[k++] = (z + 2) * row + x;
			if (!(a == 0 && (p_mask & 8))) {
				ring[k++] = (z + 1) * row + x;
			}
			for (int i = 0; i < k; i++) {
				r.push_back(ring[i]);
				r.push_back(ring[(i + 1) % k]);
				r.push_back(c);
			}
		}
	}
	return r;
}

// Height range and, per level, the largest gap between that level's triangles and the level 0 heights under them,
// for the (chunk_quads + 1)^2 heights at p_h, rows p_stride apart. Safe on worker threads.
void Landscape3D::_measure(const float *p_h, int p_stride, float *r_error, float &r_min, float &r_max) const {
	float lo = 1e30f, hi = -1e30f;
	for (int z = 0; z <= chunk_quads; z++) {
		for (int x = 0; x <= chunk_quads; x++) {
			const float h = p_h[z * p_stride + x];
			lo = MIN(lo, h);
			hi = MAX(hi, h);
		}
	}
	r_min = lo;
	r_max = hi;
	r_error[0] = 0.0f;
	for (int l = 1; l < levels; l++) {
		const int s = 1 << l;
		const int row = (chunk_quads >> l) + 1;
		const LocalVector<int> &idx = indices[l][0];
		float err = r_error[l - 1];
		for (uint32_t t = 0; t + 2 < idx.size(); t += 3) {
			int px[3], pz[3];
			float ph[3];
			for (int v = 0; v < 3; v++) {
				px[v] = (idx[t + v] % row) * s;
				pz[v] = (idx[t + v] / row) * s;
				ph[v] = p_h[pz[v] * p_stride + px[v]];
			}
			const float den = float((pz[1] - pz[2]) * (px[0] - px[2]) + (px[2] - px[1]) * (pz[0] - pz[2]));
			if (den == 0.0f) {
				continue;
			}
			const int bx0 = MIN(px[0], MIN(px[1], px[2])), bx1 = MAX(px[0], MAX(px[1], px[2]));
			const int bz0 = MIN(pz[0], MIN(pz[1], pz[2])), bz1 = MAX(pz[0], MAX(pz[1], pz[2]));
			for (int z = bz0; z <= bz1; z++) {
				for (int x = bx0; x <= bx1; x++) {
					const float wa = float((pz[1] - pz[2]) * (x - px[2]) + (px[2] - px[1]) * (z - pz[2])) / den;
					const float wb = float((pz[2] - pz[0]) * (x - px[2]) + (px[0] - px[2]) * (z - pz[2])) / den;
					const float wc = 1.0f - wa - wb;
					if (wa < -1e-5f || wb < -1e-5f || wc < -1e-5f) {
						continue;
					}
					err = MAX(err, Math::abs(wa * ph[0] + wb * ph[1] + wc * ph[2] - p_h[z * p_stride + x]));
				}
			}
		}
		r_error[l] = err;
	}
}

void Landscape3D::_measure_chunk(int p_cx, int p_cz) {
	Chunk &c = chunks[p_cz * cw + p_cx];
	_measure(heights.ptr() + size_t(p_cz) * chunk_quads * gw + size_t(p_cx) * chunk_quads, gw, c.error, c.min_h, c.max_h);
}

void Landscape3D::_measure_task(uint32_t p_index, void *p_userdata) {
	_measure_chunk(p_index % cw, p_index / cw);
}

// Mesh cache keys: the chunk's height generation, chunk index, level (3 bits), edge mask (4 bits).
uint64_t Landscape3D::_mesh_key(int p_chunk, int p_level, int p_mask) const {
	return (uint64_t(chunks[p_chunk].gen) << 32) | (uint64_t(p_chunk) << 7) | (uint64_t(p_level) << 4) | uint64_t(p_mask);
}

// Vertex data of one chunk mesh, ready for the renderer. Reads only the grid and the indices, so it runs on worker
// threads while the main thread leaves both alone.
void Landscape3D::_build_surface(MeshJob &r_job) const {
	const int cx = r_job.chunk % cw;
	const int cz = r_job.chunk / cw;
	const int s = 1 << r_job.level;
	const int n = chunk_quads >> r_job.level;
	PackedVector3Array verts;
	PackedVector3Array norms;
	verts.resize((n + 1) * (n + 1));
	norms.resize((n + 1) * (n + 1));
	Vector3 *vw = verts.ptrw();
	Vector3 *nw = norms.ptrw();
	for (int j = 0; j <= n; j++) {
		for (int i = 0; i <= n; i++) {
			const int x = cx * chunk_quads + i * s;
			const int z = cz * chunk_quads + j * s;
			vw[j * (n + 1) + i] = Vector3(i * s * spacing, _h(x, z), j * s * spacing);
			nw[j * (n + 1) + i] = _normal(x, z);
		}
	}
	const LocalVector<int> &idx = indices[r_job.level][r_job.mask];
	PackedInt32Array ia;
	ia.resize(idx.size());
	memcpy(ia.ptrw(), idx.ptr(), idx.size() * sizeof(int));
	Array arrays;
	arrays.resize(RSE::ARRAY_MAX);
	arrays[RSE::ARRAY_VERTEX] = verts;
	arrays[RSE::ARRAY_NORMAL] = norms;
	arrays[RSE::ARRAY_INDEX] = ia;
	RS::get_singleton()->mesh_create_surface_data_from_arrays(&r_job.surface, RSE::PRIMITIVE_TRIANGLES, arrays);
	r_job.triangles = int(idx.size() / 3);
}

void Landscape3D::_mesh_task(uint32_t p_index, void *p_userdata) {
	_build_surface(mesh_jobs[p_index]);
}

void Landscape3D::_set_chunk_mesh(int p_chunk, int p_level, int p_mask) {
	Chunk &c = chunks[p_chunk];
	const uint64_t key = _mesh_key(p_chunk, p_level, p_mask);
	if (key == c.mesh_key) {
		return;
	}
	MeshEntry *e = meshes.getptr(key);
	if (!e) {
		// Not built ahead (only when called outside a pick): build it here.
		MeshJob job;
		job.key = key;
		job.chunk = p_chunk;
		job.level = p_level;
		job.mask = p_mask;
		_build_surface(job);
		MeshEntry entry;
		entry.mesh = RS::get_singleton()->mesh_create();
		RS::get_singleton()->mesh_add_surface(entry.mesh, job.surface);
		entry.triangles = job.triangles;
		meshes.insert(key, entry);
		mesh_builds++;
		e = meshes.getptr(key);
	}
	if (c.mesh_key != UINT64_MAX) {
		MeshEntry *old = meshes.getptr(c.mesh_key);
		if (old) {
			old->users--;
			old->last_used = frame;
		}
		if (c.lod != p_level) {
			lod_switches++;
		}
	}
	e->users++;
	e->last_used = frame;
	c.mesh_key = key;
	c.lod = p_level;
	c.mask = p_mask;
	if (c.instance.is_valid()) {
		RS::get_singleton()->instance_set_base(c.instance, e->mesh);
	}
}

void Landscape3D::_evict(bool p_all) {
	LocalVector<uint64_t> gone;
	for (KeyValue<uint64_t, MeshEntry> &kv : meshes) {
		if (p_all || (kv.value.users <= 0 && frame - kv.value.last_used > MESH_TTL)) {
			gone.push_back(kv.key);
		}
	}
	for (uint64_t key : gone) {
		RS::get_singleton()->free_rid(meshes[key].mesh);
		meshes.erase(key);
	}
}

/* Collision */

// What the collision follows: collision_target, else the camera.
Node3D *Landscape3D::_collision_node() const {
	if (!is_inside_tree()) {
		return nullptr;
	}
	if (!collision_target.is_empty()) {
		return Object::cast_to<Node3D>(get_node_or_null(collision_target));
	}
	return _camera();
}

// One static heightfield per chunk within collision_radius of the target, at full detail (level 0, whatever level
// the chunk is drawn at), split like the drawn level 0 mesh. Tiles are freed past 1.25 x the radius. A chunk not read
// in full yet gets its tile once read; the target's own chunk and its neighbours are read at once.
void Landscape3D::_update_collision() {
	const bool on = collision_enabled && !chunks.is_empty() && is_inside_tree() && get_world_3d().is_valid() && !Engine::get_singleton()->is_editor_hint();
	Node3D *tn = on ? _collision_node() : nullptr;
	if (!tn) {
		if (collision_tiles > 0) {
			_free_all_collision();
		}
		return;
	}
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	const Vector3 t = get_global_transform().affine_inverse().xform(tn->get_global_position());
	const real_t size = chunk_quads * spacing;
	const real_t keep = collision_radius * 1.25f;
	auto dist = [&](int p_cx, int p_cz) {
		const real_t x0 = origin.x + p_cx * size, z0 = origin.y + p_cz * size;
		const Vector2 q(CLAMP(real_t(t.x), x0, x0 + size), CLAMP(real_t(t.z), z0, z0 + size));
		return q.distance_to(Vector2(t.x, t.z));
	};
	// Tiles out of reach go.
	for (uint32_t i = 0; i < chunks.size() && collision_tiles > 0; i++) {
		if (chunks[i].body.is_valid() && dist(i % cw, i / cw) > keep) {
			_free_collision(int(i));
		}
	}
	const int tx = int(Math::floor((t.x - origin.x) / size));
	const int tz = int(Math::floor((t.z - origin.y) / size));
	const int reach = int(Math::ceil(collision_radius / size)) + 1;
	// The ground right under the target cannot wait for the stream.
	LocalVector<int> urgent;
	for (int z = MAX(0, tz - 1); z <= MIN(ch - 1, tz + 1); z++) {
		for (int x = MAX(0, tx - 1); x <= MIN(cw - 1, tx + 1); x++) {
			const int i = z * cw + x;
			if (!chunks[i].fine && dist(x, z) <= collision_radius) {
				urgent.push_back(i);
			}
		}
	}
	if (!urgent.is_empty() && _stream_generator()) {
		_finish_meshes(true);
		_finish_heights(true);
		LocalVector<int> left;
		for (int i : urgent) {
			if (!chunks[i].fine) {
				left.push_back(i);
			}
		}
		_read_chunks(left);
	}
	for (int z = MAX(0, tz - reach); z <= MIN(ch - 1, tz + reach); z++) {
		for (int x = MAX(0, tx - reach); x <= MIN(cw - 1, tx + reach); x++) {
			const int i = z * cw + x;
			if (!chunks[i].body.is_valid() && chunks[i].fine && dist(x, z) <= collision_radius) {
				_collision_tile(i);
			}
		}
	}
	last_collision_usec = double(OS::get_singleton()->get_ticks_usec() - t0);
}

void Landscape3D::_collision_tile(int p_chunk) {
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	Chunk &c = chunks[p_chunk];
	const int cx = p_chunk % cw, cz = p_chunk / cw;
	const int row = chunk_quads + 1;
	PackedFloat32Array h;
	h.resize(row * row);
	float *w = h.ptrw();
	for (int j = 0; j < row; j++) {
		memcpy(w + j * row, heights.ptr() + size_t(cz * chunk_quads + j) * gw + cx * chunk_quads, row * sizeof(float));
	}
	Dictionary d;
	d["width"] = row;
	d["depth"] = row;
	d["heights"] = h;
	d["min_height"] = c.min_h;
	d["max_height"] = MAX(c.max_h, c.min_h + 0.001f);
	d["fan_blocks"] = true;
	c.shape = ps->heightmap_shape_create();
	ps->shape_set_data(c.shape, d);
	c.body = ps->body_create();
	ps->body_set_mode(c.body, PhysicsServer3D::BODY_MODE_STATIC);
	// The shape is centred on its middle sample and one unit per sample: scale it to the spacing.
	const real_t half = chunk_quads * spacing * 0.5f;
	const Vector3 centre(origin.x + cx * chunk_quads * spacing + half, 0.0, origin.y + cz * chunk_quads * spacing + half);
	ps->body_add_shape(c.body, c.shape, Transform3D(Basis().scaled(Vector3(spacing, 1.0, spacing)), centre));
	ps->body_set_collision_layer(c.body, collision_layer);
	ps->body_set_collision_mask(c.body, collision_mask);
	if (physics_material.is_valid()) {
		ps->body_set_param(c.body, PhysicsServer3D::BODY_PARAM_FRICTION, physics_material->computed_friction());
		ps->body_set_param(c.body, PhysicsServer3D::BODY_PARAM_BOUNCE, physics_material->computed_bounce());
	}
	ps->body_attach_object_instance_id(c.body, get_instance_id());
	ps->body_set_state(c.body, PhysicsServer3D::BODY_STATE_TRANSFORM, get_global_transform());
	ps->body_set_space(c.body, get_world_3d()->get_space());
	collision_tiles++;
}

void Landscape3D::_free_collision(int p_chunk) {
	Chunk &c = chunks[p_chunk];
	if (c.body.is_valid()) {
		PhysicsServer3D::get_singleton()->free_rid(c.body);
		c.body = RID();
		collision_tiles--;
	}
	if (c.shape.is_valid()) {
		PhysicsServer3D::get_singleton()->free_rid(c.shape);
		c.shape = RID();
	}
}

void Landscape3D::_free_all_collision() {
	for (uint32_t i = 0; i < chunks.size(); i++) {
		_free_collision(int(i));
	}
	collision_tiles = 0;
}

bool Landscape3D::has_chunk_collision(const Vector2i &p_chunk) const {
	ERR_FAIL_COND_V(p_chunk.x < 0 || p_chunk.y < 0 || p_chunk.x >= cw || p_chunk.y >= ch, false);
	return chunks[p_chunk.y * cw + p_chunk.x].body.is_valid();
}

void Landscape3D::set_collision_enabled(bool p_enable) {
	collision_enabled = p_enable;
	if (!p_enable) {
		_free_all_collision();
	}
}

void Landscape3D::set_collision_layer(uint32_t p_layer) {
	collision_layer = p_layer;
	for (Chunk &c : chunks) {
		if (c.body.is_valid()) {
			PhysicsServer3D::get_singleton()->body_set_collision_layer(c.body, p_layer);
		}
	}
}

void Landscape3D::set_collision_mask(uint32_t p_mask) {
	collision_mask = p_mask;
	for (Chunk &c : chunks) {
		if (c.body.is_valid()) {
			PhysicsServer3D::get_singleton()->body_set_collision_mask(c.body, p_mask);
		}
	}
}

void Landscape3D::set_physics_material(const Ref<PhysicsMaterial> &p_material) {
	physics_material = p_material;
	_free_all_collision(); // rebuilt with the new values on the next update
}

void Landscape3D::_free_instances() {
	for (Chunk &c : chunks) {
		if (c.instance.is_valid()) {
			RS::get_singleton()->free_rid(c.instance);
			c.instance = RID();
		}
	}
}

void Landscape3D::_create_instances() {
	if (!is_inside_tree() || get_world_3d().is_null()) {
		return;
	}
	RS *rs = RS::get_singleton();
	const RID scenario = get_world_3d()->get_scenario();
	for (uint32_t i = 0; i < chunks.size(); i++) {
		Chunk &c = chunks[i];
		if (c.instance.is_valid()) {
			continue;
		}
		c.instance = rs->instance_create();
		rs->instance_set_scenario(c.instance, scenario);
		rs->instance_set_visible(c.instance, is_visible_in_tree());
		if (c.mesh_key != UINT64_MAX) {
			rs->instance_set_base(c.instance, meshes[c.mesh_key].mesh);
		}
	}
	_update_transforms();
	_apply_material();
}

void Landscape3D::_update_transforms() {
	const Transform3D xf = is_inside_tree() ? get_global_transform() : Transform3D();
	for (uint32_t i = 0; i < chunks.size(); i++) {
		if (chunks[i].instance.is_valid()) {
			const Vector3 corner(origin.x + (i % cw) * chunk_quads * spacing, 0.0, origin.y + (i / cw) * chunk_quads * spacing);
			RS::get_singleton()->instance_set_transform(chunks[i].instance, xf * Transform3D(Basis(), corner));
		}
	}
}

void Landscape3D::_apply_material() {
	RS *rs = RS::get_singleton();
	const RID mat = material.is_valid() ? material->get_rid() : RID();
	for (Chunk &c : chunks) {
		if (c.instance.is_valid()) {
			rs->instance_geometry_set_material_override(c.instance, mat);
			rs->instance_geometry_set_cast_shadows_setting(c.instance, cast_shadows ? RSE::SHADOW_CASTING_SETTING_ON : RSE::SHADOW_CASTING_SETTING_OFF);
		}
	}
}

Camera3D *Landscape3D::_camera() const {
	if (!is_inside_tree()) {
		return nullptr;
	}
	if (!camera_path.is_empty()) {
		return Object::cast_to<Camera3D>(get_node_or_null(camera_path));
	}
#ifdef TOOLS_ENABLED
	if (Engine::get_singleton()->is_editor_hint() && EditorInterface::get_singleton()) {
		SubViewport *vp = EditorInterface::get_singleton()->get_editor_viewport_3d(0);
		return vp ? vp->get_camera_3d() : nullptr;
	}
#endif
	return get_viewport() ? get_viewport()->get_camera_3d() : nullptr;
}

// Picks a level and edge mask per chunk into target_lod and target_mask, and marks in need which unread chunks the
// view needs read in full. Returns true when every target mesh is already built.
bool Landscape3D::_pick_lods(const Transform3D &p_camera, const Projection &p_projection, real_t p_viewport_height) {
	const Transform3D inv = is_inside_tree() ? get_global_transform().affine_inverse() : Transform3D();
	const Vector3 eye = inv.xform(p_camera.origin);
	const bool ortho = p_projection.is_orthogonal();
	// Pixels per metre at distance 1 (perspective) or anywhere (orthogonal).
	const real_t k = Math::abs(p_projection.columns[1][1]) * p_viewport_height * 0.5;
	const real_t size = chunk_quads * spacing;
	const int n = int(chunks.size());
	LocalVector<int> &want = target_lod;
	want.resize(n);
	LocalVector<int> raw;
	raw.resize(n);
	LocalVector<int> floor_l;
	floor_l.resize(n);
	LocalVector<real_t> scale_of;
	scale_of.resize(n);
	for (int i = 0; i < n; i++) {
		const Chunk &c = chunks[i];
		const Vector3 lo(origin.x + (i % cw) * size, c.min_h, origin.y + (i / cw) * size);
		const Vector3 hi(lo.x + size, c.max_h, lo.z + size);
		const Vector3 d = eye.clamp(lo, hi) - eye;
		const real_t scale = ortho ? k : k / MAX(d.length(), real_t(0.001));
		scale_of[i] = scale;
		int loose = 0, strict = 0;
		for (int l = 1; l < levels; l++) {
			const real_t px = c.error[l] * scale;
			if (px <= lod_error) {
				loose = l;
			}
			if (px <= lod_error * 0.7) {
				strict = l;
			}
		}
		raw[i] = loose;
		floor_l[i] = c.fine ? 0 : COARSE;
		const int cur = c.mesh_key == UINT64_MAX ? loose : c.lod;
		want[i] = MAX(loose < cur ? loose : (strict > cur ? strict : cur), floor_l[i]);
	}
	// Chunks not read in full hold their neighbours at most one level finer.
	bool changed = true;
	while (changed) {
		changed = false;
		for (int z = 0; z < ch; z++) {
			for (int x = 0; x < cw; x++) {
				int &f = floor_l[z * cw + x];
				const int nb[4] = { z > 0 ? floor_l[(z - 1) * cw + x] : 0, x < cw - 1 ? floor_l[z * cw + x + 1] : 0, z < ch - 1 ? floor_l[(z + 1) * cw + x] : 0, x > 0 ? floor_l[z * cw + x - 1] : 0 };
				for (int s = 0; s < 4; s++) {
					if (f < nb[s] - 1) {
						f = nb[s] - 1;
						changed = true;
					}
				}
			}
		}
	}
	for (int i = 0; i < n; i++) {
		want[i] = MAX(want[i], floor_l[i]);
	}
	// Neighbours at most one level apart; the finer side wins (never below the floor: a neighbour sits at or above
	// its own floor, which is at most one under this one's).
	changed = true;
	while (changed) {
		changed = false;
		for (int z = 0; z < ch; z++) {
			for (int x = 0; x < cw; x++) {
				int &l = want[z * cw + x];
				const int nb[4] = { z > 0 ? want[(z - 1) * cw + x] : INT_MAX, x < cw - 1 ? want[z * cw + x + 1] : INT_MAX, z < ch - 1 ? want[(z + 1) * cw + x] : INT_MAX, x > 0 ? want[z * cw + x - 1] : INT_MAX };
				for (int s = 0; s < 4; s++) {
					if (nb[s] != INT_MAX && l > nb[s] + 1) {
						l = nb[s] + 1;
						changed = true;
					}
				}
			}
		}
	}
	// What the view needs read (2): an unread chunk it wants finer than COARSE, and one next to a chunk it wants at
	// level 0 (which could not sit next to it otherwise); 3 where the 4 m grid would be off by FIRST_VIEW_SLACK
	// times lod_error. The estimates are upper bounds, so reading never asks for more.
	need.resize(n);
	for (int i = 0; i < n; i++) {
		need[i] = chunks[i].fine ? 0 : (raw[i] < COARSE ? (chunks[i].error[COARSE] * scale_of[i] > lod_error * FIRST_VIEW_SLACK ? 3 : 2) : 1);
	}
	for (int z = 0; z < ch; z++) {
		for (int x = 0; x < cw; x++) {
			if (raw[z * cw + x] >= 1) {
				continue;
			}
			const int nb[4] = { z > 0 ? (z - 1) * cw + x : -1, x < cw - 1 ? z * cw + x + 1 : -1, z < ch - 1 ? (z + 1) * cw + x : -1, x > 0 ? z * cw + x - 1 : -1 };
			for (int s = 0; s < 4; s++) {
				if (nb[s] >= 0 && need[nb[s]] == 1) {
					need[nb[s]] = 2;
				}
			}
		}
	}
	target_mask.resize(n);
	bool ready = true;
	for (int z = 0; z < ch; z++) {
		for (int x = 0; x < cw; x++) {
			const int l = want[z * cw + x];
			int mask = 0;
			if (z > 0 && want[(z - 1) * cw + x] > l) {
				mask |= 1;
			}
			if (x < cw - 1 && want[z * cw + x + 1] > l) {
				mask |= 2;
			}
			if (z < ch - 1 && want[(z + 1) * cw + x] > l) {
				mask |= 4;
			}
			if (x > 0 && want[z * cw + x - 1] > l) {
				mask |= 8;
			}
			target_mask[z * cw + x] = mask;
			if (ready && !meshes.has(_mesh_key(z * cw + x, l, mask))) {
				ready = false;
			}
		}
	}
	return ready;
}

// Builds the meshes the target lacks on worker threads; with p_wait, waits and shows the target.
void Landscape3D::_launch_meshes(bool p_wait) {
	HashSet<uint64_t> queued;
	mesh_jobs.clear();
	for (uint32_t i = 0; i < chunks.size(); i++) {
		const uint64_t key = _mesh_key(i, target_lod[i], target_mask[i]);
		if (meshes.has(key) || queued.has(key)) {
			continue;
		}
		queued.insert(key);
		MeshJob job;
		job.key = key;
		job.chunk = int(i);
		job.level = target_lod[i];
		job.mask = target_mask[i];
		mesh_jobs.push_back(job);
	}
	if (mesh_jobs.is_empty()) {
		for (uint32_t i = 0; i < chunks.size(); i++) {
			_set_chunk_mesh(i, target_lod[i], target_mask[i]);
		}
		return;
	}
	mesh_group = WorkerThreadPool::get_singleton()->add_template_group_task(this, &Landscape3D::_mesh_task, (void *)nullptr, int(mesh_jobs.size()), -1, p_wait, SNAME("Landscape3D meshes"));
	if (p_wait) {
		_finish_meshes(true);
	}
}

// Hands the built meshes to the renderer and shows the whole target at once, so no frame has a level next to one
// it does not fit.
void Landscape3D::_finish_meshes(bool p_wait) {
	if (mesh_group < 0) {
		return;
	}
	if (!p_wait && !WorkerThreadPool::get_singleton()->is_group_task_completed(mesh_group)) {
		return;
	}
	WorkerThreadPool::get_singleton()->wait_for_group_task_completion(mesh_group);
	mesh_group = -1;
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	RS *rs = RS::get_singleton();
	for (MeshJob &job : mesh_jobs) {
		MeshEntry entry;
		entry.mesh = rs->mesh_create();
		rs->mesh_add_surface(entry.mesh, job.surface);
		entry.triangles = job.triangles;
		entry.last_used = frame;
		meshes.insert(job.key, entry);
		mesh_builds++;
	}
	mesh_jobs.clear();
	for (uint32_t i = 0; i < chunks.size(); i++) {
		_set_chunk_mesh(i, target_lod[i], target_mask[i]);
	}
	last_apply_usec = double(OS::get_singleton()->get_ticks_usec() - t0);
}

/* Streaming heights (docs/ideas/terrain.md, "Landscape3D: streaming and off-thread builds") */

const TerrainGenerator3D *Landscape3D::_stream_generator() const {
	return stream_source.is_valid() ? Object::cast_to<TerrainGenerator3D>(ObjectDB::get_instance(stream_source)) : nullptr;
}

// One row of the 4 m grid (every 4th point), past the valid points repeating the last row and column as the full
// read does.
void Landscape3D::_coarse_task(uint32_t p_row, void *p_userdata) {
	const TerrainGenerator3D *gen = static_cast<const TerrainGenerator3D *>(p_userdata);
	const int z = MIN(int(p_row) * 4, gh - 1);
	const real_t wz = origin.y + MIN(z, vh - 1) * spacing;
	for (int x = 0; x < gw; x += 4) {
		const float h = gen->get_height(Vector3(origin.x + MIN(x, vw - 1) * spacing, 0.0, wz));
		heights[size_t(z) * gw + x] = Math::is_nan(h) ? 0.0f : h;
	}
}

// One row of the grid between the 4 m points, bilinear from them.
void Landscape3D::_fill_task(uint32_t p_row, void *p_userdata) {
	const int z = int(p_row);
	const int z0 = z & ~3;
	const int z1 = MIN(z0 + 4, gh - 1);
	const float fz = float(z - z0) * 0.25f;
	float *row = heights.ptr() + size_t(z) * gw;
	const float *r0 = heights.ptr() + size_t(z0) * gw;
	const float *r1 = heights.ptr() + size_t(z1) * gw;
	for (int x = 0; x < gw; x++) {
		if ((x & 3) == 0 && z == z0) {
			continue;
		}
		const int x0 = x & ~3;
		const int x1 = MIN(x0 + 4, gw - 1);
		const float fx = float(x - x0) * 0.25f;
		const float a = Math::lerp(r0[x0], r0[x1], fx);
		const float b = Math::lerp(r1[x0], r1[x1], fx);
		row[x] = Math::lerp(a, b, fz);
	}
}

// One chunk in full into its own buffer, then its level errors; the main thread copies it in.
void Landscape3D::_height_task(uint32_t p_index, void *p_userdata) {
	const TerrainGenerator3D *gen = static_cast<const TerrainGenerator3D *>(p_userdata);
	HeightJob &job = height_jobs[p_index];
	const int row = chunk_quads + 1;
	const int x0 = (job.chunk % cw) * chunk_quads;
	const int z0 = (job.chunk / cw) * chunk_quads;
	job.h.resize(row * row);
	for (int j = 0; j < row; j++) {
		const real_t wz = origin.y + MIN(z0 + j, vh - 1) * spacing;
		for (int i = 0; i < row; i++) {
			const float h = gen->get_height(Vector3(origin.x + MIN(x0 + i, vw - 1) * spacing, 0.0, wz));
			job.h[j * row + i] = Math::is_nan(h) ? 0.0f : h;
		}
	}
	_measure(job.h.ptr(), row, job.error, job.min_h, job.max_h);
}

void Landscape3D::_start_heights(const LocalVector<int> &p_chunks, bool p_wait) {
	const TerrainGenerator3D *gen = _stream_generator();
	if (!gen || p_chunks.is_empty() || height_group >= 0) {
		return;
	}
	height_jobs.resize(p_chunks.size());
	for (uint32_t i = 0; i < p_chunks.size(); i++) {
		height_jobs[i].chunk = p_chunks[i];
		chunks[p_chunks[i]].loading = true;
	}
	height_group = WorkerThreadPool::get_singleton()->add_template_group_task(this, &Landscape3D::_height_task, (void *)const_cast<TerrainGenerator3D *>(gen), int(height_jobs.size()), -1, p_wait, SNAME("Landscape3D heights"));
}

// The next chunks to read: those the view needs first, then the rest, nearest the camera first.
void Landscape3D::_launch_heights() {
	if (coarse_chunks <= 0 || height_group >= 0 || !_stream_generator()) {
		return;
	}
	const Vector3 eye = (is_inside_tree() ? get_global_transform().affine_inverse() : Transform3D()).xform(last_camera.origin);
	const real_t size = chunk_quads * spacing;
	struct Candidate {
		float order = 0.0f;
		int chunk = 0;
		bool operator<(const Candidate &p_other) const { return order < p_other.order; }
	};
	LocalVector<Candidate> list;
	for (uint32_t i = 0; i < chunks.size(); i++) {
		const Chunk &c = chunks[i];
		if (c.fine || c.loading) {
			continue;
		}
		const Vector2 centre(origin.x + ((i % cw) + 0.5f) * size, origin.y + ((i / cw) + 0.5f) * size);
		Candidate cand;
		cand.chunk = int(i);
		cand.order = (have_camera ? centre.distance_to(Vector2(eye.x, eye.z)) : float(i)) + (i < need.size() ? float(3 - MIN(int(need[i]), 3)) * 1e9f : 3e9f);
		list.push_back(cand);
	}
	if (list.is_empty()) {
		return;
	}
	// Ground the collision target will stand on comes first of all.
	if (Node3D *tn = collision_enabled ? _collision_node() : nullptr) {
		const Vector3 t = get_global_transform().affine_inverse().xform(tn->get_global_position());
		for (Candidate &cand : list) {
			const int cx = cand.chunk % cw, cz = cand.chunk / cw;
			const Rect2 r(origin.x + cx * size, origin.y + cz * size, size, size);
			const Vector2 q(CLAMP(real_t(t.x), r.position.x, r.position.x + size), CLAMP(real_t(t.z), r.position.y, r.position.y + size));
			if (q.distance_to(Vector2(t.x, t.z)) <= collision_radius) {
				cand.order -= 4e9f;
			}
		}
	}
	list.sort();
	const int batch = MIN(int(list.size()), MAX(4, WorkerThreadPool::get_singleton()->get_thread_count() * 2));
	LocalVector<int> pick;
	for (int i = 0; i < batch; i++) {
		pick.push_back(list[i].chunk);
	}
	_start_heights(pick, false);
}

// Copies read chunks into the grid. Waits for any mesh batch first: those read the grid.
void Landscape3D::_finish_heights(bool p_wait) {
	if (height_group < 0) {
		return;
	}
	if (!p_wait && (mesh_group >= 0 || !WorkerThreadPool::get_singleton()->is_group_task_completed(height_group))) {
		return;
	}
	_finish_meshes(true);
	WorkerThreadPool::get_singleton()->wait_for_group_task_completion(height_group);
	height_group = -1;
	const int row = chunk_quads + 1;
	for (HeightJob &job : height_jobs) {
		Chunk &c = chunks[job.chunk];
		const int cx = job.chunk % cw;
		const int cz = job.chunk / cw;
		for (int j = 0; j < row; j++) {
			memcpy(heights.ptr() + size_t(cz * chunk_quads + j) * gw + cx * chunk_quads, job.h.ptr() + j * row, row * sizeof(float));
		}
		memcpy(c.error, job.error, sizeof(c.error));
		c.min_h = job.min_h;
		c.max_h = job.max_h;
		c.loading = false;
		if (!c.fine) {
			c.fine = true;
			coarse_chunks--;
			chunks_streamed++;
		}
		// Its own vertices and the normals along its neighbours' sides (and corners) change.
		for (int dz = -1; dz <= 1; dz++) {
			for (int dx = -1; dx <= 1; dx++) {
				if (cx + dx >= 0 && cx + dx < cw && cz + dz >= 0 && cz + dz < ch) {
					chunks[(cz + dz) * cw + cx + dx].gen++;
				}
			}
		}
		if (uint32_t(job.chunk) < need.size()) {
			need[job.chunk] = 0;
		}
	}
	height_jobs.clear();
	lods_dirty = true;
}

void Landscape3D::_read_chunks(const LocalVector<int> &p_chunks) {
	_finish_heights(true);
	_start_heights(p_chunks, true);
	_finish_heights(true);
}

void Landscape3D::_wait_all() {
	if (mesh_group >= 0) {
		WorkerThreadPool::get_singleton()->wait_for_group_task_completion(mesh_group);
		mesh_group = -1;
		mesh_jobs.clear();
		lods_dirty = true;
	}
	if (height_group >= 0) {
		WorkerThreadPool::get_singleton()->wait_for_group_task_completion(height_group);
		height_group = -1;
		for (HeightJob &job : height_jobs) {
			chunks[job.chunk].loading = false;
		}
		height_jobs.clear();
	}
}

// The generator is about to replace its result (or leave the tree): finish reading it and stop. Unread chunks
// keep their 4 m heights until the next rebuild().
void Landscape3D::_on_source_changing() {
	_wait_all();
	TerrainGenerator3D *gen = Object::cast_to<TerrainGenerator3D>(ObjectDB::get_instance(stream_source));
	if (gen) {
		const Callable cb = callable_mp(this, &Landscape3D::_on_source_changing);
		if (gen->is_connected(SNAME("changing"), cb)) {
			gen->disconnect(SNAME("changing"), cb);
		}
		if (gen->is_connected(SceneStringName(tree_exiting), cb)) {
			gen->disconnect(SceneStringName(tree_exiting), cb);
		}
	}
	stream_source = ObjectID();
}

void Landscape3D::load_all_heights() {
	if (!_stream_generator()) {
		return;
	}
	LocalVector<int> all;
	for (uint32_t i = 0; i < chunks.size(); i++) {
		if (!chunks[i].fine) {
			all.push_back(int(i));
		}
	}
	if (all.is_empty()) {
		return;
	}
	_read_chunks(all);
	if (have_camera) {
		update_lods(last_camera, last_projection, last_viewport_height);
	}
}

/* Levels */

// Synchronous: reads the chunks this view needs in full first (so the frame is exact), builds the missing meshes on
// worker threads and waits for them. The node itself does the same each frame without waiting.
void Landscape3D::update_lods(const Transform3D &p_camera, const Projection &p_projection, real_t p_viewport_height) {
	_update_lods(p_camera, p_projection, p_viewport_height, 2);
}

void Landscape3D::_update_lods(const Transform3D &p_camera, const Projection &p_projection, real_t p_viewport_height, int p_read) {
	if (chunks.is_empty()) {
		return;
	}
	_finish_meshes(true);
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	_pick_lods(p_camera, p_projection, p_viewport_height);
	for (int pass = 0; pass < 4 && coarse_chunks > 0 && _stream_generator(); pass++) {
		LocalVector<int> needed;
		for (uint32_t i = 0; i < need.size(); i++) {
			if (need[i] >= p_read) {
				needed.push_back(int(i));
			}
		}
		if (needed.is_empty()) {
			break;
		}
		_read_chunks(needed);
		_pick_lods(p_camera, p_projection, p_viewport_height);
	}
	last_update_usec = double(OS::get_singleton()->get_ticks_usec() - t0);
	last_camera = p_camera;
	last_projection = p_projection;
	last_viewport_height = p_viewport_height;
	have_camera = true;
	lods_dirty = false;
	_launch_meshes(true);
}

void Landscape3D::update_lods_from_camera() {
	Camera3D *cam = _camera();
	if (!cam || !cam->get_viewport()) {
		return;
	}
	update_lods(cam->get_global_transform(), cam->get_camera_projection(), cam->get_viewport()->get_visible_rect().size.y);
}

void Landscape3D::set_chunk_quads(int p_quads) {
	int q = 8;
	while (q < p_quads && q < 256) {
		q *= 2;
	}
	chunk_quads = q;
}

void Landscape3D::set_lod_error(real_t p_pixels) {
	lod_error = MAX(p_pixels, real_t(0.05));
	lods_dirty = true;
}

void Landscape3D::set_material(const Ref<Material> &p_material) {
	material = p_material;
	_apply_material();
}

void Landscape3D::set_cast_shadows(bool p_enable) {
	cast_shadows = p_enable;
	_apply_material();
}

void Landscape3D::clear() {
	_on_source_changing();
	_free_instances();
	_free_all_collision();
	_evict(true);
	chunks.clear();
	heights.clear();
	need.clear();
	target_lod.clear();
	target_mask.clear();
	for (int l = 0; l < 8; l++) {
		for (int m = 0; m < 16; m++) {
			indices[l][m].clear();
		}
	}
	gw = gh = vw = vh = cw = ch = 0;
	coarse_chunks = 0;
	detail_estimate = 0.0f;
	have_camera = false;
	lods_dirty = true;
}

bool Landscape3D::rebuild() {
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	clear();
	// Valid points from the source (vw x vh), then the grid rounded up to whole chunks, clamped at the far edges.
	Ref<Image> img;
	Node *src = source.is_empty() ? nullptr : get_node_or_null(source);
	if (src) {
		ERR_FAIL_COND_V_MSG(!src->has_method("sample_heights"), false, "Landscape3D: the source needs sample_heights(origin, spacing, size), as TerrainGenerator3D has.");
		if (src->has_method("get_world_rect")) {
			const Rect2 rect = src->call("get_world_rect");
			origin = rect.position;
			vw = int(Math::floor(rect.size.x / spacing)) + 1;
			vh = int(Math::floor(rect.size.y / spacing)) + 1;
		}
	} else if (height_image.is_valid() && !height_image->is_empty()) {
		img = height_image->duplicate();
		if (img->is_compressed()) {
			img->decompress();
		}
		img->convert(Image::FORMAT_RF);
		vw = img->get_width();
		vh = img->get_height();
	}
	ERR_FAIL_COND_V_MSG(vw < 2 || vh < 2, false, "Landscape3D: no heights (set source to a TerrainGenerator3D, or height_image).");
	cw = MAX(1, (vw - 2) / chunk_quads + 1);
	ch = MAX(1, (vh - 2) / chunk_quads + 1);
	gw = cw * chunk_quads + 1;
	gh = ch * chunk_quads + 1;
	ERR_FAIL_COND_V_MSG(int64_t(gw) * gh > 256 * 1024 * 1024, false, "Landscape3D: grid too large; raise spacing.");
	int max_levels = 1;
	while ((chunk_quads >> max_levels) >= 2 && max_levels < 8) {
		max_levels++;
	}
	levels = MIN(lod_levels, max_levels);
	for (int l = 0; l < levels; l++) {
		for (int m = 0; m < 16; m++) {
			_indices(l, m);
		}
	}
	chunks.resize(cw * ch);
	heights.resize(gw * gh);
	TerrainGenerator3D *gen = Object::cast_to<TerrainGenerator3D>(src);
	const bool streaming = stream_heights && gen && levels > COARSE + 1;
	WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
	if (streaming) {
		// The 4 m grid first, the rest interpolated until read.
		const int rows = (gh - 1) / 4 + 1;
		WorkerThreadPool::GroupID g = pool->add_template_group_task(this, &Landscape3D::_coarse_task, (void *)gen, rows, -1, true, SNAME("Landscape3D"));
		pool->wait_for_group_task_completion(g);
		g = pool->add_template_group_task(this, &Landscape3D::_fill_task, (void *)nullptr, gh, -1, true, SNAME("Landscape3D"));
		pool->wait_for_group_task_completion(g);
	} else {
		if (src) {
			const Ref<Image> got = src->call("sample_heights", origin, spacing, Vector2i(vw, vh));
			ERR_FAIL_COND_V(got.is_null() || got->get_width() != vw || got->get_height() != vh, false);
			img = got;
		}
		const float *in = reinterpret_cast<const float *>(img->get_data().ptr());
		for (int z = 0; z < gh; z++) {
			const int zz = MIN(z, vh - 1);
			for (int x = 0; x < gw; x++) {
				const float h = in[zz * vw + MIN(x, vw - 1)];
				heights[z * gw + x] = Math::is_nan(h) ? 0.0f : h;
			}
		}
	}
	const uint64_t t_src = OS::get_singleton()->get_ticks_usec();
	WorkerThreadPool::GroupID g = pool->add_template_group_task(this, &Landscape3D::_measure_task, (void *)nullptr, cw * ch, -1, true, SNAME("Landscape3D"));
	pool->wait_for_group_task_completion(g);
	if (streaming) {
		stream_source = gen->get_instance_id();
		const Callable cb = callable_mp(this, &Landscape3D::_on_source_changing);
		gen->connect(SNAME("changing"), cb);
		gen->connect(SceneStringName(tree_exiting), cb);
		for (Chunk &c : chunks) {
			c.fine = false;
			for (int l = 0; l < COARSE; l++) {
				c.error[l] = UNREAD;
			}
		}
		coarse_chunks = int(chunks.size());
		// How much the 4 m grid misses: read a sparse set of chunks in full and compare their errors.
		LocalVector<int> samples;
		for (int z = MIN(3, ch / 2); z < ch; z += 8) {
			for (int x = MIN(3, cw / 2); x < cw; x += 8) {
				samples.push_back(z * cw + x);
			}
		}
		LocalVector<float> before;
		for (int i : samples) {
			for (int l = COARSE; l < levels; l++) {
				before.push_back(chunks[i].error[l]);
			}
		}
		_read_chunks(samples);
		// Per level: the most a read chunk's error grew over its 4 m estimate, with a margin.
		float miss[8] = {};
		for (uint32_t s = 0; s < samples.size(); s++) {
			for (int l = COARSE; l < levels; l++) {
				miss[l] = MAX(miss[l], chunks[samples[s]].error[l] - before[s * (levels - COARSE) + l - COARSE]);
			}
		}
		detail_estimate = 0.0f;
		for (int l = COARSE; l < levels; l++) {
			miss[l] *= 1.25f;
			detail_estimate = MAX(detail_estimate, miss[l]);
		}
		for (Chunk &c : chunks) {
			if (!c.fine) {
				for (int l = COARSE; l < levels; l++) {
					c.error[l] += miss[l];
				}
				for (int l = COARSE + 1; l < levels; l++) {
					c.error[l] = MAX(c.error[l], c.error[l - 1]);
				}
			}
		}
	}
	const uint64_t t_measure = OS::get_singleton()->get_ticks_usec();
	_create_instances();
	Camera3D *cam = _camera();
	if (cam && cam->get_viewport()) {
		// Reads what the first view needs before showing it, so the first frame is exact.
		const Transform3D xf = cam->get_global_transform();
		const Projection pr = cam->get_camera_projection();
		const real_t vp_h = cam->get_viewport()->get_visible_rect().size.y;
		_update_lods(xf, pr, vp_h, 3);
	} else {
		// No camera yet: the coarsest level everywhere (cheap); the first camera refines it.
		for (uint32_t i = 0; i < chunks.size(); i++) {
			_set_chunk_mesh(i, levels - 1, 0);
		}
	}
	_update_collision();
	update_gizmos();
	const uint64_t t_end = OS::get_singleton()->get_ticks_usec();
	last_build_msec = double(t_end - t0) / 1000.0;
	last_source_msec = double(t_src - t0) / 1000.0;
	last_measure_msec = double(t_measure - t_src) / 1000.0;
	return true;
}

int Landscape3D::get_chunk_lod(const Vector2i &p_chunk) const {
	ERR_FAIL_COND_V(p_chunk.x < 0 || p_chunk.y < 0 || p_chunk.x >= cw || p_chunk.y >= ch, -1);
	return chunks[p_chunk.y * cw + p_chunk.x].lod;
}

int Landscape3D::get_chunk_edge_mask(const Vector2i &p_chunk) const {
	ERR_FAIL_COND_V(p_chunk.x < 0 || p_chunk.y < 0 || p_chunk.x >= cw || p_chunk.y >= ch, -1);
	return chunks[p_chunk.y * cw + p_chunk.x].mask;
}

bool Landscape3D::is_chunk_loaded(const Vector2i &p_chunk) const {
	ERR_FAIL_COND_V(p_chunk.x < 0 || p_chunk.y < 0 || p_chunk.x >= cw || p_chunk.y >= ch, false);
	return chunks[p_chunk.y * cw + p_chunk.x].fine;
}

real_t Landscape3D::get_chunk_lod_error(const Vector2i &p_chunk, int p_level) const {
	ERR_FAIL_COND_V(p_chunk.x < 0 || p_chunk.y < 0 || p_chunk.x >= cw || p_chunk.y >= ch, -1.0);
	ERR_FAIL_INDEX_V(p_level, levels, -1.0);
	return chunks[p_chunk.y * cw + p_chunk.x].error[p_level];
}

// The drawn triangles of a chunk in global space, three points each.
PackedVector3Array Landscape3D::get_chunk_triangles(const Vector2i &p_chunk) const {
	PackedVector3Array out;
	ERR_FAIL_COND_V(p_chunk.x < 0 || p_chunk.y < 0 || p_chunk.x >= cw || p_chunk.y >= ch, out);
	const Chunk &c = chunks[p_chunk.y * cw + p_chunk.x];
	const LocalVector<int> &idx = indices[c.lod][c.mask];
	ERR_FAIL_COND_V(idx.is_empty(), out);
	const Transform3D xf = is_inside_tree() ? get_global_transform() : Transform3D();
	const int s = 1 << c.lod;
	const int row = (chunk_quads >> c.lod) + 1;
	out.resize(idx.size());
	Vector3 *w = out.ptrw();
	for (uint32_t i = 0; i < idx.size(); i++) {
		const int x = p_chunk.x * chunk_quads + (idx[i] % row) * s;
		const int z = p_chunk.y * chunk_quads + (idx[i] / row) * s;
		w[i] = xf.xform(Vector3(origin.x + x * spacing, _h(x, z), origin.y + z * spacing));
	}
	return out;
}

// Level 0 ground under a global point, bilinear between the grid heights.
real_t Landscape3D::get_height(const Vector3 &p_position) const {
	if (heights.is_empty()) {
		return 0.0;
	}
	const Vector3 p = (is_inside_tree() ? get_global_transform().affine_inverse() : Transform3D()).xform(p_position);
	const real_t fx = (p.x - origin.x) / spacing;
	const real_t fz = (p.z - origin.y) / spacing;
	const int x = int(Math::floor(fx));
	const int z = int(Math::floor(fz));
	const real_t u = fx - x, v = fz - z;
	return Math::lerp(Math::lerp(_h(x, z), _h(x + 1, z), u), Math::lerp(_h(x, z + 1), _h(x + 1, z + 1), u), v);
}

AABB Landscape3D::get_aabb() const {
	if (chunks.is_empty()) {
		return AABB();
	}
	float lo = 1e30f, hi = -1e30f;
	for (const Chunk &c : chunks) {
		lo = MIN(lo, c.min_h);
		hi = MAX(hi, c.max_h);
	}
	return AABB(Vector3(origin.x, lo, origin.y), Vector3((gw - 1) * spacing, hi - lo, (gh - 1) * spacing));
}

Dictionary Landscape3D::get_stats() const {
	Dictionary d;
	int tris = 0;
	PackedInt32Array per_level;
	per_level.resize(levels);
	per_level.fill(0);
	for (const Chunk &c : chunks) {
		if (c.mesh_key != UINT64_MAX) {
			tris += meshes[c.mesh_key].triangles;
			per_level.set(c.lod, per_level[c.lod] + 1);
		}
	}
	d["chunks"] = int(chunks.size());
	d["triangles"] = tris;
	d["chunks_per_level"] = per_level;
	d["meshes"] = int(meshes.size());
	d["mesh_builds"] = int64_t(mesh_builds);
	d["lod_switches"] = int64_t(lod_switches);
	d["build_msec"] = last_build_msec;
	d["source_msec"] = last_source_msec;
	d["measure_msec"] = last_measure_msec;
	d["update_usec"] = last_update_usec;
	d["apply_usec"] = last_apply_usec;
	d["unread_chunks"] = coarse_chunks;
	d["chunks_streamed"] = int64_t(chunks_streamed);
	d["detail_estimate"] = detail_estimate;
	d["busy"] = mesh_group >= 0 || height_group >= 0;
	d["collision_tiles"] = collision_tiles;
	d["collision_usec"] = last_collision_usec;
	return d;
}


void Landscape3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_WORLD: {
			_create_instances();
			set_process_internal(true);
		} break;
		case NOTIFICATION_EXIT_WORLD: {
			_wait_all();
			_free_instances();
			_free_all_collision();
			set_process_internal(false);
		} break;
		case NOTIFICATION_TRANSFORM_CHANGED: {
			_update_transforms();
			if (is_inside_tree()) {
				for (Chunk &c : chunks) {
					if (c.body.is_valid()) {
						PhysicsServer3D::get_singleton()->body_set_state(c.body, PhysicsServer3D::BODY_STATE_TRANSFORM, get_global_transform());
					}
				}
			}
			lods_dirty = true;
		} break;
		case NOTIFICATION_VISIBILITY_CHANGED: {
			for (Chunk &c : chunks) {
				if (c.instance.is_valid()) {
					RS::get_singleton()->instance_set_visible(c.instance, is_visible_in_tree());
				}
			}
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			frame++;
			if (chunks.is_empty()) {
				break;
			}
			// Built meshes in, read chunks in (both on the main thread, between worker batches), then a new pick
			// when the view or the heights changed, then the next chunks to read.
			_finish_meshes(false);
			_finish_heights(false);
			Camera3D *cam = _camera();
			if (mesh_group < 0 && cam && cam->get_viewport()) {
				const Transform3D xf = cam->get_global_transform();
				const Projection pr = cam->get_camera_projection();
				const real_t vp_h = cam->get_viewport()->get_visible_rect().size.y;
				if (lods_dirty || !have_camera || xf != last_camera || pr != last_projection || vp_h != last_viewport_height) {
					const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
					_pick_lods(xf, pr, vp_h);
					last_update_usec = double(OS::get_singleton()->get_ticks_usec() - t0);
					last_camera = xf;
					last_projection = pr;
					last_viewport_height = vp_h;
					have_camera = true;
					lods_dirty = false;
					_launch_meshes(false);
				}
			}
			_update_collision();
			_launch_heights();
			if (frame % 60 == 0) {
				_evict(false);
			}
		} break;
	}
}

Landscape3D::Landscape3D() {
	set_notify_transform(true);
}

Landscape3D::~Landscape3D() {
	clear();
}

void Landscape3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_source", "path"), &Landscape3D::set_source);
	ClassDB::bind_method(D_METHOD("get_source"), &Landscape3D::get_source);
	ClassDB::bind_method(D_METHOD("set_height_image", "image"), &Landscape3D::set_height_image);
	ClassDB::bind_method(D_METHOD("get_height_image"), &Landscape3D::get_height_image);
	ClassDB::bind_method(D_METHOD("set_origin", "origin"), &Landscape3D::set_origin);
	ClassDB::bind_method(D_METHOD("get_origin"), &Landscape3D::get_origin);
	ClassDB::bind_method(D_METHOD("set_spacing", "spacing"), &Landscape3D::set_spacing);
	ClassDB::bind_method(D_METHOD("get_spacing"), &Landscape3D::get_spacing);
	ClassDB::bind_method(D_METHOD("set_chunk_quads", "quads"), &Landscape3D::set_chunk_quads);
	ClassDB::bind_method(D_METHOD("get_chunk_quads"), &Landscape3D::get_chunk_quads);
	ClassDB::bind_method(D_METHOD("set_lod_levels", "levels"), &Landscape3D::set_lod_levels);
	ClassDB::bind_method(D_METHOD("get_lod_levels"), &Landscape3D::get_lod_levels);
	ClassDB::bind_method(D_METHOD("set_lod_error", "pixels"), &Landscape3D::set_lod_error);
	ClassDB::bind_method(D_METHOD("get_lod_error"), &Landscape3D::get_lod_error);
	ClassDB::bind_method(D_METHOD("set_material", "material"), &Landscape3D::set_material);
	ClassDB::bind_method(D_METHOD("get_material"), &Landscape3D::get_material);
	ClassDB::bind_method(D_METHOD("set_cast_shadows", "enable"), &Landscape3D::set_cast_shadows);
	ClassDB::bind_method(D_METHOD("is_casting_shadows"), &Landscape3D::is_casting_shadows);
	ClassDB::bind_method(D_METHOD("set_camera", "path"), &Landscape3D::set_camera);
	ClassDB::bind_method(D_METHOD("get_camera"), &Landscape3D::get_camera);
	ClassDB::bind_method(D_METHOD("set_stream_heights", "enable"), &Landscape3D::set_stream_heights);
	ClassDB::bind_method(D_METHOD("is_streaming_heights"), &Landscape3D::is_streaming_heights);

	ClassDB::bind_method(D_METHOD("set_collision_enabled", "enable"), &Landscape3D::set_collision_enabled);
	ClassDB::bind_method(D_METHOD("is_collision_enabled"), &Landscape3D::is_collision_enabled);
	ClassDB::bind_method(D_METHOD("set_collision_radius", "radius"), &Landscape3D::set_collision_radius);
	ClassDB::bind_method(D_METHOD("get_collision_radius"), &Landscape3D::get_collision_radius);
	ClassDB::bind_method(D_METHOD("set_collision_target", "path"), &Landscape3D::set_collision_target);
	ClassDB::bind_method(D_METHOD("get_collision_target"), &Landscape3D::get_collision_target);
	ClassDB::bind_method(D_METHOD("set_collision_layer", "layer"), &Landscape3D::set_collision_layer);
	ClassDB::bind_method(D_METHOD("get_collision_layer"), &Landscape3D::get_collision_layer);
	ClassDB::bind_method(D_METHOD("set_collision_mask", "mask"), &Landscape3D::set_collision_mask);
	ClassDB::bind_method(D_METHOD("get_collision_mask"), &Landscape3D::get_collision_mask);
	ClassDB::bind_method(D_METHOD("set_physics_material", "material"), &Landscape3D::set_physics_material);
	ClassDB::bind_method(D_METHOD("get_physics_material"), &Landscape3D::get_physics_material);
	ClassDB::bind_method(D_METHOD("update_collision"), &Landscape3D::update_collision);
	ClassDB::bind_method(D_METHOD("has_chunk_collision", "chunk"), &Landscape3D::has_chunk_collision);
	ClassDB::bind_method(D_METHOD("rebuild"), &Landscape3D::rebuild);
	ClassDB::bind_method(D_METHOD("clear"), &Landscape3D::clear);
	ClassDB::bind_method(D_METHOD("update_lods", "camera_transform", "projection", "viewport_height"), &Landscape3D::update_lods);
	ClassDB::bind_method(D_METHOD("update_lods_from_camera"), &Landscape3D::update_lods_from_camera);
	ClassDB::bind_method(D_METHOD("load_all_heights"), &Landscape3D::load_all_heights);
	ClassDB::bind_method(D_METHOD("is_fully_loaded"), &Landscape3D::is_fully_loaded);
	ClassDB::bind_method(D_METHOD("is_chunk_loaded", "chunk"), &Landscape3D::is_chunk_loaded);
	ClassDB::bind_method(D_METHOD("get_chunk_grid"), &Landscape3D::get_chunk_grid);
	ClassDB::bind_method(D_METHOD("get_lod_count"), &Landscape3D::get_lod_count);
	ClassDB::bind_method(D_METHOD("get_chunk_lod", "chunk"), &Landscape3D::get_chunk_lod);
	ClassDB::bind_method(D_METHOD("get_chunk_edge_mask", "chunk"), &Landscape3D::get_chunk_edge_mask);
	ClassDB::bind_method(D_METHOD("get_chunk_lod_error", "chunk", "level"), &Landscape3D::get_chunk_lod_error);
	ClassDB::bind_method(D_METHOD("get_chunk_triangles", "chunk"), &Landscape3D::get_chunk_triangles);
	ClassDB::bind_method(D_METHOD("get_height", "global_position"), &Landscape3D::get_height);
	ClassDB::bind_method(D_METHOD("get_aabb"), &Landscape3D::get_aabb);
	ClassDB::bind_method(D_METHOD("get_stats"), &Landscape3D::get_stats);

	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "source", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "TerrainGenerator3D"), "set_source", "get_source");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "height_image", PROPERTY_HINT_RESOURCE_TYPE, "Image"), "set_height_image", "get_height_image");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "origin", PROPERTY_HINT_NONE, "suffix:m"), "set_origin", "get_origin");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "spacing", PROPERTY_HINT_RANGE, "0.05,16,0.05,or_greater,suffix:m"), "set_spacing", "get_spacing");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "chunk_quads", PROPERTY_HINT_ENUM, "8:8,16:16,32:32,64:64,128:128,256:256"), "set_chunk_quads", "get_chunk_quads");
	ADD_GROUP("LOD", "lod_");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "lod_levels", PROPERTY_HINT_RANGE, "1,8,1"), "set_lod_levels", "get_lod_levels");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lod_error", PROPERTY_HINT_RANGE, "0.05,16,0.05,suffix:px"), "set_lod_error", "get_lod_error");
	ADD_GROUP("", "");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "material", PROPERTY_HINT_RESOURCE_TYPE, "BaseMaterial3D,ShaderMaterial"), "set_material", "get_material");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "cast_shadows"), "set_cast_shadows", "is_casting_shadows");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "stream_heights"), "set_stream_heights", "is_streaming_heights");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "camera", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Camera3D"), "set_camera", "get_camera");
	ADD_GROUP("Collision", "collision_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "collision_enabled"), "set_collision_enabled", "is_collision_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "collision_radius", PROPERTY_HINT_RANGE, "0,2048,1,or_greater,suffix:m"), "set_collision_radius", "get_collision_radius");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "collision_target", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D"), "set_collision_target", "get_collision_target");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_layer", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_collision_layer", "get_collision_layer");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_mask", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_collision_mask", "get_collision_mask");
	ADD_GROUP("", "");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "physics_material", PROPERTY_HINT_RESOURCE_TYPE, "PhysicsMaterial"), "set_physics_material", "get_physics_material");
}

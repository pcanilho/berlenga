/**************************************************************************/
/*  landscape_3d.cpp                                                      */
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


#include "landscape_3d.h"

#include "core/object/class_db.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/os.h"
#include "scene/3d/camera_3d.h"
#include "scene/main/viewport.h"
#include "scene/resources/3d/world_3d.h"
#include "servers/rendering/rendering_server.h"

#ifdef TOOLS_ENABLED
#include "editor/editor_interface.h"
#endif

// Mesh cache keys: chunk index, level (3 bits), edge mask (4 bits).
static inline uint32_t _key(int p_chunk, int p_level, int p_mask) {
	return (uint32_t(p_chunk) << 7) | (uint32_t(p_level) << 4) | uint32_t(p_mask);
}

// Frames an unused mesh (and so its BLAS) stays, in case the camera comes back.
static const uint64_t MESH_TTL = 600;

float Landscape3D::_h(int p_x, int p_z) const {
	return heights[CLAMP(p_z, 0, gh - 1) * gw + CLAMP(p_x, 0, gw - 1)];
}

Vector3 Landscape3D::_normal(int p_x, int p_z) const {
	return Vector3(_h(p_x - 1, p_z) - _h(p_x + 1, p_z), 2.0f * spacing, _h(p_x, p_z - 1) - _h(p_x, p_z + 1)).normalized();
}

// Indices of a chunk at p_level, in vertex rows of (n + 1), n = chunk_quads >> p_level. The grid is cut into 2 x 2
// cell blocks, each a fan of 8 triangles around its centre vertex; where p_mask flags a side whose neighbour is one
// level coarser (bit 0 -z, 1 +x, 2 +z, 3 -x), the blocks on that side skip its odd vertex and fan to the corners,
// which lie on the neighbour's coarser edge. Corner blocks with two flagged sides work the same way.
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

// Height range and, per level, the largest gap between that level's triangles and the level 0 heights under them.
void Landscape3D::_measure_chunk(int p_cx, int p_cz) {
	Chunk &c = chunks[p_cz * cw + p_cx];
	const int x0 = p_cx * chunk_quads;
	const int z0 = p_cz * chunk_quads;
	float lo = 1e30f, hi = -1e30f;
	for (int z = 0; z <= chunk_quads; z++) {
		for (int x = 0; x <= chunk_quads; x++) {
			const float h = _h(x0 + x, z0 + z);
			lo = MIN(lo, h);
			hi = MAX(hi, h);
		}
	}
	c.min_h = lo;
	c.max_h = hi;
	c.error[0] = 0.0f;
	for (int l = 1; l < levels; l++) {
		const int s = 1 << l;
		const int row = (chunk_quads >> l) + 1;
		const LocalVector<int> &idx = indices[l][0];
		float err = c.error[l - 1];
		for (uint32_t t = 0; t + 2 < idx.size(); t += 3) {
			int px[3], pz[3];
			float ph[3];
			for (int v = 0; v < 3; v++) {
				px[v] = (idx[t + v] % row) * s;
				pz[v] = (idx[t + v] / row) * s;
				ph[v] = _h(x0 + px[v], z0 + pz[v]);
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
					err = MAX(err, Math::abs(wa * ph[0] + wb * ph[1] + wc * ph[2] - _h(x0 + x, z0 + z)));
				}
			}
		}
		c.error[l] = err;
	}
}

RID Landscape3D::_mesh(int p_chunk, int p_level, int p_mask) {
	const uint32_t key = _key(p_chunk, p_level, p_mask);
	MeshEntry *e = meshes.getptr(key);
	if (e) {
		return e->mesh;
	}
	const int cx = p_chunk % cw;
	const int cz = p_chunk / cw;
	const int s = 1 << p_level;
	const int n = chunk_quads >> p_level;
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
	const LocalVector<int> &idx = _indices(p_level, p_mask);
	PackedInt32Array ia;
	ia.resize(idx.size());
	memcpy(ia.ptrw(), idx.ptr(), idx.size() * sizeof(int));
	Array arrays;
	arrays.resize(RSE::ARRAY_MAX);
	arrays[RSE::ARRAY_VERTEX] = verts;
	arrays[RSE::ARRAY_NORMAL] = norms;
	arrays[RSE::ARRAY_INDEX] = ia;
	MeshEntry entry;
	entry.mesh = RS::get_singleton()->mesh_create();
	RS::get_singleton()->mesh_add_surface_from_arrays(entry.mesh, RSE::PRIMITIVE_TRIANGLES, arrays);
	entry.triangles = int(idx.size() / 3);
	entry.last_used = frame;
	meshes.insert(key, entry);
	mesh_builds++;
	return entry.mesh;
}

void Landscape3D::_set_chunk_mesh(int p_chunk, int p_level, int p_mask) {
	Chunk &c = chunks[p_chunk];
	const uint32_t key = _key(p_chunk, p_level, p_mask);
	if (key == c.mesh_key) {
		return;
	}
	const RID mesh = _mesh(p_chunk, p_level, p_mask);
	if (c.mesh_key != UINT32_MAX) {
		MeshEntry *old = meshes.getptr(c.mesh_key);
		if (old) {
			old->users--;
			old->last_used = frame;
		}
		if (c.lod != p_level) {
			lod_switches++;
		}
	}
	MeshEntry *e = meshes.getptr(key);
	e->users++;
	e->last_used = frame;
	c.mesh_key = key;
	c.lod = p_level;
	c.mask = p_mask;
	if (c.instance.is_valid()) {
		RS::get_singleton()->instance_set_base(c.instance, mesh);
	}
}

void Landscape3D::_evict(bool p_all) {
	LocalVector<uint32_t> gone;
	for (KeyValue<uint32_t, MeshEntry> &kv : meshes) {
		if (p_all || (kv.value.users <= 0 && frame - kv.value.last_used > MESH_TTL)) {
			gone.push_back(kv.key);
		}
	}
	for (uint32_t key : gone) {
		RS::get_singleton()->free_rid(meshes[key].mesh);
		meshes.erase(key);
	}
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
		if (c.mesh_key != UINT32_MAX) {
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

void Landscape3D::_pick_lods(const Transform3D &p_camera, const Projection &p_projection, real_t p_viewport_height) {
	const Transform3D inv = is_inside_tree() ? get_global_transform().affine_inverse() : Transform3D();
	const Vector3 eye = inv.xform(p_camera.origin);
	const bool ortho = p_projection.is_orthogonal();
	// Pixels per metre at distance 1 (perspective) or anywhere (orthogonal).
	const real_t k = Math::abs(p_projection.columns[1][1]) * p_viewport_height * 0.5;
	const real_t size = chunk_quads * spacing;
	LocalVector<int> want;
	want.resize(chunks.size());
	for (uint32_t i = 0; i < chunks.size(); i++) {
		const Chunk &c = chunks[i];
		const Vector3 lo(origin.x + (i % cw) * size, c.min_h, origin.y + (i / cw) * size);
		const Vector3 hi(lo.x + size, c.max_h, lo.z + size);
		const Vector3 d = eye.clamp(lo, hi) - eye;
		const real_t scale = ortho ? k : k / MAX(d.length(), real_t(0.001));
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
		const int cur = c.mesh_key == UINT32_MAX ? loose : c.lod;
		want[i] = loose < cur ? loose : (strict > cur ? strict : cur);
	}
	// Neighbours at most one level apart; the finer side wins.
	bool changed = true;
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
			_set_chunk_mesh(z * cw + x, l, mask);
		}
	}
}

void Landscape3D::update_lods(const Transform3D &p_camera, const Projection &p_projection, real_t p_viewport_height) {
	if (chunks.is_empty()) {
		return;
	}
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	_pick_lods(p_camera, p_projection, p_viewport_height);
	last_camera = p_camera;
	last_projection = p_projection;
	have_camera = true;
	lods_dirty = false;
	last_update_usec = double(OS::get_singleton()->get_ticks_usec() - t0);
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
	_free_instances();
	_evict(true);
	chunks.clear();
	heights.clear();
	for (int l = 0; l < 8; l++) {
		for (int m = 0; m < 16; m++) {
			indices[l][m].clear();
		}
	}
	gw = gh = cw = ch = 0;
	have_camera = false;
	lods_dirty = true;
}

bool Landscape3D::rebuild() {
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	clear();
	// Valid points from the source (vw x vh), then the grid rounded up to whole chunks, clamped at the far edges.
	int vw = 0, vh = 0;
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
	if (src) {
		const Ref<Image> got = src->call("sample_heights", origin, spacing, Vector2i(vw, vh));
		ERR_FAIL_COND_V(got.is_null() || got->get_width() != vw || got->get_height() != vh, false);
		img = got;
	}
	heights.resize(gw * gh);
	const float *in = reinterpret_cast<const float *>(img->get_data().ptr());
	for (int z = 0; z < gh; z++) {
		const int zz = MIN(z, vh - 1);
		for (int x = 0; x < gw; x++) {
			const float h = in[zz * vw + MIN(x, vw - 1)];
			heights[z * gw + x] = Math::is_nan(h) ? 0.0f : h;
		}
	}
	int max_levels = 1;
	while ((chunk_quads >> max_levels) >= 2 && max_levels < 8) {
		max_levels++;
	}
	levels = MIN(lod_levels, max_levels);
	for (int l = 0; l < levels; l++) {
		_indices(l, 0);
	}
	chunks.resize(cw * ch);
	WorkerThreadPool::GroupID g = WorkerThreadPool::get_singleton()->add_template_group_task(this, &Landscape3D::_measure_task, (void *)nullptr, cw * ch, -1, true, SNAME("Landscape3D"));
	WorkerThreadPool::get_singleton()->wait_for_group_task_completion(g);
	_create_instances();
	Camera3D *cam = _camera();
	if (cam && cam->get_viewport()) {
		update_lods_from_camera();
	} else {
		// No camera yet: the coarsest level everywhere (cheap); the first camera refines it.
		for (uint32_t i = 0; i < chunks.size(); i++) {
			_set_chunk_mesh(i, levels - 1, 0);
		}
	}
	update_gizmos();
	last_build_msec = double(OS::get_singleton()->get_ticks_usec() - t0) / 1000.0;
	return true;
}

void Landscape3D::_measure_task(uint32_t p_index, void *p_userdata) {
	_measure_chunk(p_index % cw, p_index / cw);
}

int Landscape3D::get_chunk_lod(const Vector2i &p_chunk) const {
	ERR_FAIL_COND_V(p_chunk.x < 0 || p_chunk.y < 0 || p_chunk.x >= cw || p_chunk.y >= ch, -1);
	return chunks[p_chunk.y * cw + p_chunk.x].lod;
}

int Landscape3D::get_chunk_edge_mask(const Vector2i &p_chunk) const {
	ERR_FAIL_COND_V(p_chunk.x < 0 || p_chunk.y < 0 || p_chunk.x >= cw || p_chunk.y >= ch, -1);
	return chunks[p_chunk.y * cw + p_chunk.x].mask;
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
		if (c.mesh_key != UINT32_MAX) {
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
	d["update_usec"] = last_update_usec;
	return d;
}

void Landscape3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_WORLD: {
			_create_instances();
			set_process_internal(true);
		} break;
		case NOTIFICATION_EXIT_WORLD: {
			_free_instances();
			set_process_internal(false);
		} break;
		case NOTIFICATION_TRANSFORM_CHANGED: {
			_update_transforms();
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
			Camera3D *cam = _camera();
			if (!chunks.is_empty() && cam && cam->get_viewport()) {
				const Transform3D xf = cam->get_global_transform();
				const Projection pr = cam->get_camera_projection();
				if (lods_dirty || !have_camera || xf != last_camera || pr != last_projection) {
					update_lods(xf, pr, cam->get_viewport()->get_visible_rect().size.y);
				}
			}
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

	ClassDB::bind_method(D_METHOD("rebuild"), &Landscape3D::rebuild);
	ClassDB::bind_method(D_METHOD("clear"), &Landscape3D::clear);
	ClassDB::bind_method(D_METHOD("update_lods", "camera_transform", "projection", "viewport_height"), &Landscape3D::update_lods);
	ClassDB::bind_method(D_METHOD("update_lods_from_camera"), &Landscape3D::update_lods_from_camera);
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
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "camera", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Camera3D"), "set_camera", "get_camera");
}

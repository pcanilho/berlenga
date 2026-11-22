/**************************************************************************/
/*  terrain_deformer_3d.cpp                                               */
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

#include "terrain_deformer_3d.h"

#include "core/config/engine.h"
#include "core/io/image.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/physics/collision_shape_3d.h"
#include "scene/resources/3d/height_map_shape_3d.h"
#include "scene/resources/mesh.h"
#include "scene/resources/surface_tool.h"

// Non-destructive height brushes over a heightfield ground: the edit-layer model of Unreal's Landscape (layers
// composited in order over the base heights, Landmass brush blend types Min / Max / Alpha Blend / Additive).
// Terrain3D is called by name only (no build or load dependency); a HeightMapShape3D (with an optional generated
// mesh) is the plain backend. Design and sources: docs/ideas/terrain.md, "Deformation layer".

/* TerrainCarveBrush */

bool TerrainCarveBrush::from_dictionary(const Dictionary &p_dict, TerrainCarveBrush &r_brush, String &r_error) {
	if (!p_dict.has("rect") || !p_dict.has("heights")) {
		r_error = "needs \"rect\" and \"heights\"";
		return false;
	}
	r_brush.rect = p_dict["rect"];
	r_brush.cell_size = p_dict.get("cell_size", 1.0);
	if (!(r_brush.cell_size > 0.0)) {
		r_error = "\"cell_size\" must be positive";
		return false;
	}
	const PackedFloat32Array heights = p_dict["heights"];
	r_brush.heights = heights;
	if (p_dict.has("size")) {
		r_brush.size = p_dict["size"];
	} else {
		r_brush.size = Vector2i(int(Math::round(r_brush.rect.size.x / r_brush.cell_size)), int(Math::round(r_brush.rect.size.y / r_brush.cell_size)));
	}
	if (r_brush.size.x < 2 || r_brush.size.y < 2 || r_brush.heights.size() != r_brush.size.x * r_brush.size.y) {
		r_error = vformat("\"heights\" has %d values, expected %d x %d", r_brush.heights.size(), r_brush.size.x, r_brush.size.y);
		return false;
	}
	if (p_dict.has("mask")) {
		const PackedFloat32Array mask = p_dict["mask"];
		r_brush.mask = mask;
		if (!r_brush.mask.is_empty() && r_brush.mask.size() != r_brush.heights.size()) {
			r_error = "\"mask\" must match \"heights\"";
			return false;
		}
	}
	const Variant mode = p_dict.get("mode", "lower");
	if (mode.get_type() == Variant::INT) {
		r_brush.mode = Mode(CLAMP(int(mode), 0, int(MODE_ADD)));
	} else {
		const String m = String(mode).to_lower();
		if (m == "lower" || m == "min") {
			r_brush.mode = MODE_LOWER;
		} else if (m == "raise" || m == "max") {
			r_brush.mode = MODE_RAISE;
		} else if (m == "flatten" || m == "set") {
			r_brush.mode = MODE_FLATTEN;
		} else if (m == "add") {
			r_brush.mode = MODE_ADD;
		} else {
			r_error = "unknown \"mode\" " + m;
			return false;
		}
	}
	return true;
}

bool TerrainCarveBrush::sample(real_t p_x, real_t p_z, float &r_height, float &r_mask) const {
	// Samples sit at cell centres: sample (i, j) is at rect.position + ((i, j) + 0.5) * cell, and the rect spans
	// size * cell (PhysXRiver3D's convention). Between the outer centres and the rect's edge the edge value holds.
	if (!(p_x >= rect.position.x && p_z >= rect.position.y && p_x <= rect.position.x + rect.size.x && p_z <= rect.position.y + rect.size.y)) {
		return false;
	}
	const real_t gx = CLAMP((p_x - rect.position.x) / rect.size.x * size.x - 0.5, real_t(0.0), real_t(size.x - 1));
	const real_t gz = CLAMP((p_z - rect.position.y) / rect.size.y * size.y - 0.5, real_t(0.0), real_t(size.y - 1));
	const int x0 = MIN(int(gx), size.x - 2);
	const int z0 = MIN(int(gz), size.y - 2);
	const float fx = float(gx - x0);
	const float fz = float(gz - z0);
	const int i00 = z0 * size.x + x0;
	const int i10 = i00 + 1;
	const int i01 = i00 + size.x;
	const int i11 = i01 + 1;
	const float *h = heights.ptr();
	const float w[4] = { (1.0f - fx) * (1.0f - fz), fx * (1.0f - fz), (1.0f - fx) * fz, fx * fz };
	const int idx[4] = { i00, i10, i01, i11 };
	float hs = 0.0f;
	float ws = 0.0f;
	float ms = 0.0f;
	for (int i = 0; i < 4; i++) {
		if (w[i] <= 0.0f) {
			continue;
		}
		if (Math::is_nan(h[idx[i]])) {
			return false; // a hole in the brush: leaves the ground alone
		}
		hs += h[idx[i]] * w[i];
		ms += (mask.is_empty() ? 1.0f : mask[idx[i]]) * w[i];
		ws += w[i];
	}
	if (ws <= 0.0f) {
		return false;
	}
	r_height = hs / ws;
	r_mask = CLAMP(ms / ws, 0.0f, 1.0f);
	return true;
}

float TerrainCarveBrush::blend(Mode p_mode, float p_ground, float p_target, float p_mask) {
	switch (p_mode) {
		case MODE_LOWER:
			return Math::lerp(p_ground, MIN(p_ground, p_target), p_mask);
		case MODE_RAISE:
			return Math::lerp(p_ground, MAX(p_ground, p_target), p_mask);
		case MODE_FLATTEN:
			return Math::lerp(p_ground, p_target, p_mask);
		case MODE_ADD:
			return p_ground + p_target * p_mask;
	}
	return p_ground;
}

/* TerrainDeformer3D: backends */

static inline int _floor_div(int p_a, int p_b) {
	const int q = p_a / p_b;
	return (p_a % p_b != 0 && ((p_a < 0) != (p_b < 0))) ? q - 1 : q;
}

static bool _is_unrotated(const Basis &p_basis) {
	const Vector3 x = p_basis.get_column(0).normalized();
	const Vector3 y = p_basis.get_column(1).normalized();
	const Vector3 z = p_basis.get_column(2).normalized();
	return x.is_equal_approx(Vector3(1, 0, 0)) && y.is_equal_approx(Vector3(0, 1, 0)) && z.is_equal_approx(Vector3(0, 0, 1));
}

bool TerrainDeformer3D::_open_grid(Grid &r_grid) const {
	r_grid = Grid();
	if (!is_inside_tree()) {
		return false;
	}
	return open_grid(get_node_or_null(terrain_path), r_grid);
}

bool TerrainDeformer3D::open_grid(Node *node, Grid &r_grid) {
	r_grid = Grid();
	if (!node || !node->is_inside_tree()) {
		return false;
	}
	if (node->is_class("Terrain3D")) {
		Object *t3d_data = node->call("get_data");
		if (!t3d_data) {
			return false;
		}
		r_grid.backend = BACKEND_TERRAIN3D;
		r_grid.terrain = node;
		r_grid.data = t3d_data;
		r_grid.region_size = node->call("get_region_size");
		r_grid.spacing = node->call("get_vertex_spacing");
		r_grid.origin = Vector2();
		return r_grid.region_size > 0 && r_grid.spacing > 0.0;
	}
	CollisionShape3D *cs = Object::cast_to<CollisionShape3D>(node);
	if (cs) {
		Ref<HeightMapShape3D> shape = cs->get_shape();
		if (shape.is_null() || !cs->is_inside_tree()) {
			return false;
		}
		const Transform3D xf = cs->get_global_transform();
		ERR_FAIL_COND_V_MSG(!_is_unrotated(xf.basis), false, "TerrainDeformer3D: the HeightMapShape3D's CollisionShape3D must not be rotated.");
		const Vector3 scale = xf.basis.get_scale();
		ERR_FAIL_COND_V_MSG(!Math::is_equal_approx(scale.x, scale.z), false, "TerrainDeformer3D: the HeightMapShape3D needs the same scale on X and Z.");
		r_grid.backend = BACKEND_HEIGHT_MAP;
		r_grid.shape = shape;
		r_grid.map = shape->get_map_data();
		r_grid.width = shape->get_map_width();
		r_grid.depth = shape->get_map_depth();
		r_grid.spacing = scale.x;
		r_grid.height_scale = scale.y;
		r_grid.height_offset = xf.origin.y;
		r_grid.origin = Vector2(xf.origin.x - (r_grid.width - 1) * 0.5 * scale.x, xf.origin.z - (r_grid.depth - 1) * 0.5 * scale.z);
		return r_grid.width >= 2 && r_grid.depth >= 2;
	}
	return false;
}

bool TerrainDeformer3D::grid_get(Grid &r_grid, const Vector2i &p_cell, float &r_height) {
	if (r_grid.backend == BACKEND_TERRAIN3D) {
		const Vector2i region(_floor_div(p_cell.x, r_grid.region_size), _floor_div(p_cell.y, r_grid.region_size));
		Ref<Image> *cached = r_grid.region_maps.getptr(region);
		Ref<Image> map;
		if (cached) {
			map = *cached;
		} else {
			Object *region_obj = r_grid.data->call("get_region", region);
			if (region_obj && !bool(region_obj->call("is_deleted"))) {
				map = region_obj->call("get_height_map");
			}
			r_grid.region_maps.insert(region, map);
		}
		if (map.is_null()) {
			return false;
		}
		const Vector2i px(Math::posmod(p_cell.x, r_grid.region_size), Math::posmod(p_cell.y, r_grid.region_size));
		r_height = map->get_pixel(px.x, px.y).r;
		return !Math::is_nan(r_height);
	}
	if (r_grid.backend == BACKEND_HEIGHT_MAP) {
		if (p_cell.x < 0 || p_cell.y < 0 || p_cell.x >= r_grid.width || p_cell.y >= r_grid.depth) {
			return false;
		}
		const real_t h = r_grid.map[p_cell.y * r_grid.width + p_cell.x];
		r_height = float(h * r_grid.height_scale + r_grid.height_offset);
		return !Math::is_nan(h);
	}
	return false;
}

void TerrainDeformer3D::grid_set(Grid &r_grid, const Vector2i &p_cell, float p_height) {
	if (r_grid.backend == BACKEND_TERRAIN3D) {
		const Vector2i region(_floor_div(p_cell.x, r_grid.region_size), _floor_div(p_cell.y, r_grid.region_size));
		const Ref<Image> *map = r_grid.region_maps.getptr(region);
		if (!map || map->is_null()) {
			return;
		}
		const Vector2i px(Math::posmod(p_cell.x, r_grid.region_size), Math::posmod(p_cell.y, r_grid.region_size));
		(*map)->set_pixel(px.x, px.y, Color(p_height, 0.0, 0.0, 1.0));
		r_grid.touched_regions.insert(region);
	} else if (r_grid.backend == BACKEND_HEIGHT_MAP) {
		if (p_cell.x < 0 || p_cell.y < 0 || p_cell.x >= r_grid.width || p_cell.y >= r_grid.depth) {
			return;
		}
		r_grid.map.write[p_cell.y * r_grid.width + p_cell.x] = (p_height - r_grid.height_offset) / r_grid.height_scale;
		r_grid.map_changed = true;
	}
}

void TerrainDeformer3D::_commit_grid(Grid &r_grid) {
	commit_grid(r_grid, Object::cast_to<MeshInstance3D>(get_node_or_null(ground_mesh_path)));
}

void TerrainDeformer3D::commit_grid(Grid &r_grid, MeshInstance3D *p_ground_mesh) {
	if (r_grid.backend == BACKEND_TERRAIN3D && !r_grid.touched_regions.is_empty()) {
		// Terrain3D's own editor flags a region "edited" so update_maps() re-uploads only that layer of the
		// height texture array, then clears the flag; "modified" makes it save with the terrain.
		Vector<Object *> regions;
		for (const Vector2i &loc : r_grid.touched_regions) {
			Object *region = r_grid.data->call("get_region", loc);
			if (region) {
				region->call("set_modified", true);
				region->call("set_edited", true);
				region->call("calc_height_range");
				regions.push_back(region);
			}
		}
		r_grid.data->call("update_maps", 0 /* TYPE_HEIGHT */, false, false);
		for (Object *region : regions) {
			region->call("set_edited", false);
		}
		r_grid.data->call("calc_height_range", false);
		Object *collision = r_grid.terrain->call("get_collision");
		if (collision) {
			collision->call("update", Vector2i(INT32_MAX, INT32_MAX), true);
		}
	} else if (r_grid.backend == BACKEND_HEIGHT_MAP && r_grid.map_changed) {
		r_grid.shape->set_map_data(r_grid.map);
		rebuild_ground_mesh(r_grid, p_ground_mesh);
	}
}

void TerrainDeformer3D::rebuild_ground_mesh(const Grid &p_grid, MeshInstance3D *mi) {
	if (!mi || p_grid.backend != BACKEND_HEIGHT_MAP) {
		return;
	}
	// The mesh is the heightfield itself, in the mesh instance's space, so it draws (and traces) exactly what the
	// collision is. Triangles split like PhysX's and Godot's height fields: (x, z) (x+1, z) (x, z+1) and the rest.
	const int w = p_grid.width;
	const int d = p_grid.depth;
	const Transform3D to_local = mi->get_global_transform().affine_inverse();
	PackedVector3Array verts;
	PackedVector3Array normals;
	PackedVector2Array uvs;
	PackedInt32Array indices;
	verts.resize(w * d);
	normals.resize(w * d);
	uvs.resize(w * d);
	Vector3 *vp = verts.ptrw();
	Vector2 *uvp = uvs.ptrw();
	auto world_at = [&](int x, int z) -> Vector3 {
		const real_t h = p_grid.map[z * w + x];
		return Vector3(p_grid.origin.x + x * p_grid.spacing, (Math::is_nan(h) ? 0.0 : h) * p_grid.height_scale + p_grid.height_offset, p_grid.origin.y + z * p_grid.spacing);
	};
	for (int z = 0; z < d; z++) {
		for (int x = 0; x < w; x++) {
			const Vector3 c = world_at(x, z);
			const Vector3 dx = world_at(MIN(x + 1, w - 1), z) - world_at(MAX(x - 1, 0), z);
			const Vector3 dz = world_at(x, MIN(z + 1, d - 1)) - world_at(x, MAX(z - 1, 0));
			vp[z * w + x] = to_local.xform(c);
			normals.write[z * w + x] = to_local.basis.xform(dz.cross(dx)).normalized();
			uvp[z * w + x] = Vector2(c.x, c.z); // world-space UV, metres
		}
	}
	for (int z = 0; z < d - 1; z++) {
		for (int x = 0; x < w - 1; x++) {
			const int i00 = z * w + x;
			const int i10 = i00 + 1;
			const int i01 = i00 + w;
			const int i11 = i01 + 1;
			const bool hole0 = Math::is_nan(p_grid.map[i00]) || Math::is_nan(p_grid.map[i10]) || Math::is_nan(p_grid.map[i01]);
			const bool hole1 = Math::is_nan(p_grid.map[i11]) || Math::is_nan(p_grid.map[i10]) || Math::is_nan(p_grid.map[i01]);
			// Godot winds front faces clockwise.
			if (!hole0) {
				indices.push_back(i00);
				indices.push_back(i10);
				indices.push_back(i01);
			}
			if (!hole1) {
				indices.push_back(i10);
				indices.push_back(i11);
				indices.push_back(i01);
			}
		}
	}
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = verts;
	arrays[Mesh::ARRAY_NORMAL] = normals;
	arrays[Mesh::ARRAY_TEX_UV] = uvs;
	arrays[Mesh::ARRAY_INDEX] = indices;
	Ref<ArrayMesh> mesh = mi->get_mesh();
	Ref<Material> material;
	if (mesh.is_valid() && mesh->get_surface_count() > 0) {
		material = mesh->surface_get_material(0);
	}
	if (mesh.is_null()) {
		mesh.instantiate();
	} else {
		mesh->clear_surfaces();
	}
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	if (material.is_valid()) {
		mesh->surface_set_material(0, material);
	}
	mi->set_mesh(mesh);
}

/* TerrainDeformer3D: brushes */

Vector<Object *> TerrainDeformer3D::_get_brush_objects() const {
	Vector<Object *> result;
	if (!is_inside_tree()) {
		return result;
	}
	for (int i = 0; i < brush_paths.size(); i++) {
		Node *n = get_node_or_null(brush_paths[i]);
		if (n && (n->has_method(SNAME("get_carve_brush")) || n->has_method(SNAME("get_carve_brushes")))) {
			result.push_back(n);
		}
	}
	return result;
}

void TerrainDeformer3D::_connect_brushes() {
	const Callable cb = callable_mp(this, &TerrainDeformer3D::_on_brush_changed);
	for (Object *obj : _get_brush_objects()) {
		if (obj->has_signal(SNAME("carve_changed")) && !obj->is_connected(SNAME("carve_changed"), cb)) {
			obj->connect(SNAME("carve_changed"), cb);
			connected_brushes.insert(obj->get_instance_id());
		}
	}
}

void TerrainDeformer3D::_on_brush_changed() {
	if (auto_apply) {
		queue_apply();
	}
}

void TerrainDeformer3D::queue_apply() {
	if (apply_queued) {
		return;
	}
	apply_queued = true;
	callable_mp(this, &TerrainDeformer3D::_deferred_apply).call_deferred();
}

void TerrainDeformer3D::_deferred_apply() {
	apply_queued = false;
	if (is_inside_tree()) {
		apply();
	}
}

void TerrainDeformer3D::_on_terrain_ready() {
	if (waiting_for_terrain) {
		waiting_for_terrain = false;
		queue_apply();
	}
}

void TerrainDeformer3D::add_brush(Node *p_brush) {
	ERR_FAIL_NULL(p_brush);
	ERR_FAIL_COND_MSG(!is_inside_tree() || !p_brush->is_inside_tree(), "TerrainDeformer3D: the deformer and the brush must be in the tree.");
	const NodePath path = get_path_to(p_brush);
	if (!brush_paths.has(path)) {
		brush_paths.push_back(path);
	}
	_connect_brushes();
	if (auto_apply) {
		queue_apply();
	}
}

void TerrainDeformer3D::remove_brush(Node *p_brush) {
	ERR_FAIL_NULL(p_brush);
	const NodePath path = get_path_to(p_brush);
	brush_paths.erase(path);
	const Callable cb = callable_mp(this, &TerrainDeformer3D::_on_brush_changed);
	if (p_brush->is_connected(SNAME("carve_changed"), cb)) {
		p_brush->disconnect(SNAME("carve_changed"), cb);
	}
	connected_brushes.erase(p_brush->get_instance_id());
	if (auto_apply) {
		queue_apply();
	}
}

/* TerrainDeformer3D: apply */

TerrainDeformer3D::Backend TerrainDeformer3D::get_backend() const {
	Grid grid;
	return _open_grid(grid) ? grid.backend : BACKEND_NONE;
}

int TerrainDeformer3D::apply() {
	Grid grid;
	if (!_open_grid(grid)) {
		return 0;
	}
	if (base_spacing > 0.0 && !Math::is_equal_approx(base_spacing, grid.spacing) && !base_heights.is_empty()) {
		WARN_PRINT(vformat("TerrainDeformer3D: the terrain's vertex spacing changed (%f to %f); forgetting %d base heights.", base_spacing, grid.spacing, base_heights.size()));
		base_heights.clear();
	}
	base_spacing = grid.spacing;
	_connect_brushes();

	// Brushes in stack order.
	LocalVector<TerrainCarveBrush> brushes;
	for (Object *obj : _get_brush_objects()) {
		// One brush (get_carve_brush) or a list of tiles (get_carve_brushes, as PhysXRiver3D gives).
		Array list;
		if (obj->has_method(SNAME("get_carve_brushes"))) {
			list = obj->call(SNAME("get_carve_brushes"), brush_tile_size);
		} else {
			list.push_back(obj->call(SNAME("get_carve_brush")));
		}
		for (int i = 0; i < list.size(); i++) {
			const Dictionary d = list[i];
			if (d.is_empty()) {
				continue; // nothing to carve right now
			}
			TerrainCarveBrush b;
			String error;
			if (!TerrainCarveBrush::from_dictionary(d, b, error)) {
				Node *n = Object::cast_to<Node>(obj);
				ERR_PRINT(vformat("TerrainDeformer3D: brush %s: %s.", n ? String(n->get_path()) : String("?"), error));
				continue;
			}
			brushes.push_back(b);
		}
	}

	// Every grid vertex a brush covers, with its base height. Vertices first seen now take the ground as it is.
	HashMap<int64_t, float> result;
	for (const TerrainCarveBrush &b : brushes) {
		const int x0 = int(Math::ceil((b.rect.position.x - grid.origin.x) / grid.spacing - CMP_EPSILON));
		const int z0 = int(Math::ceil((b.rect.position.y - grid.origin.y) / grid.spacing - CMP_EPSILON));
		const int x1 = int(Math::floor((b.rect.position.x + b.rect.size.x - grid.origin.x) / grid.spacing + CMP_EPSILON));
		const int z1 = int(Math::floor((b.rect.position.y + b.rect.size.y - grid.origin.y) / grid.spacing + CMP_EPSILON));
		for (int z = z0; z <= z1; z++) {
			for (int x = x0; x <= x1; x++) {
				const Vector2i cell(x, z);
				const int64_t key = pack_cell(cell);
				if (result.has(key)) {
					continue;
				}
				const float *base = base_heights.getptr(key);
				if (base) {
					result.insert(key, *base);
				} else {
					float h;
					if (grid_get(grid, cell, h)) {
						base_heights.insert(key, h);
						result.insert(key, h);
					}
				}
			}
		}
	}

	// Composite in order.
	for (const TerrainCarveBrush &b : brushes) {
		const int x0 = int(Math::ceil((b.rect.position.x - grid.origin.x) / grid.spacing - CMP_EPSILON));
		const int z0 = int(Math::ceil((b.rect.position.y - grid.origin.y) / grid.spacing - CMP_EPSILON));
		const int x1 = int(Math::floor((b.rect.position.x + b.rect.size.x - grid.origin.x) / grid.spacing + CMP_EPSILON));
		const int z1 = int(Math::floor((b.rect.position.y + b.rect.size.y - grid.origin.y) / grid.spacing + CMP_EPSILON));
		for (int z = z0; z <= z1; z++) {
			for (int x = x0; x <= x1; x++) {
				float *h = result.getptr(pack_cell(Vector2i(x, z)));
				if (!h) {
					continue;
				}
				float target, mask;
				if (b.sample(grid.origin.x + x * grid.spacing, grid.origin.y + z * grid.spacing, target, mask) && mask > 0.0f) {
					*h = TerrainCarveBrush::blend(b.mode, *h, target, mask);
				}
			}
		}
	}

	// Write. Vertices no brush covers any more go back to their base height and are forgotten.
	int written = 0;
	LocalVector<int64_t> released;
	for (const KeyValue<int64_t, float> &kv : base_heights) {
		if (!result.has(kv.key)) {
			released.push_back(kv.key);
		}
	}
	for (int64_t key : released) {
		const Vector2i cell = unpack_cell(key);
		float now;
		const float base = base_heights[key];
		if (grid_get(grid, cell, now) && now != base) {
			grid_set(grid, cell, base);
			written++;
		}
		base_heights.erase(key);
	}
	for (const KeyValue<int64_t, float> &kv : result) {
		const Vector2i cell = unpack_cell(kv.key);
		float now;
		if (grid_get(grid, cell, now) && now != kv.value) {
			grid_set(grid, cell, kv.value);
			written++;
		}
	}
	_commit_grid(grid);
	if (written > 0) {
		emit_signal(SNAME("applied"), written);
	}
	return written;
}

void TerrainDeformer3D::clear() {
	Grid grid;
	if (!_open_grid(grid)) {
		base_heights.clear();
		return;
	}
	for (const KeyValue<int64_t, float> &kv : base_heights) {
		float now;
		const Vector2i cell = unpack_cell(kv.key);
		if (grid_get(grid, cell, now) && now != kv.value) {
			grid_set(grid, cell, kv.value);
		}
	}
	base_heights.clear();
	_commit_grid(grid);
}

real_t TerrainDeformer3D::get_base_height(const Vector3 &p_global_position) const {
	Grid grid;
	if (!_open_grid(grid)) {
		return Math::NaN;
	}
	const real_t gx = (p_global_position.x - grid.origin.x) / grid.spacing;
	const real_t gz = (p_global_position.z - grid.origin.y) / grid.spacing;
	const int x0 = int(Math::floor(gx));
	const int z0 = int(Math::floor(gz));
	const real_t fx = gx - x0;
	const real_t fz = gz - z0;
	real_t h[4];
	const Vector2i cells[4] = { Vector2i(x0, z0), Vector2i(x0 + 1, z0), Vector2i(x0, z0 + 1), Vector2i(x0 + 1, z0 + 1) };
	for (int i = 0; i < 4; i++) {
		const float *base = base_heights.getptr(pack_cell(cells[i]));
		float v;
		if (base) {
			v = *base;
		} else if (!grid_get(grid, cells[i], v)) {
			return Math::NaN;
		}
		h[i] = v;
	}
	// Split along the same diagonal as the generated ground mesh and Godot's height map shape.
	if (fx + fz <= 1.0) {
		return h[0] + (h[1] - h[0]) * fx + (h[2] - h[0]) * fz;
	}
	return h[3] + (h[2] - h[3]) * (1.0 - fx) + (h[1] - h[3]) * (1.0 - fz);
}

real_t TerrainDeformer3D::base_height_at(const Vector2 &p_world_xz) const {
	return get_base_height(Vector3(p_world_xz.x, 0.0, p_world_xz.y));
}

real_t TerrainDeformer3D::get_height(const Vector3 &p_global_position) const {
	Grid grid;
	if (!_open_grid(grid)) {
		return Math::NaN;
	}
	const real_t gx = (p_global_position.x - grid.origin.x) / grid.spacing;
	const real_t gz = (p_global_position.z - grid.origin.y) / grid.spacing;
	const int x0 = int(Math::floor(gx));
	const int z0 = int(Math::floor(gz));
	const real_t fx = gx - x0;
	const real_t fz = gz - z0;
	float h[4];
	const Vector2i cells[4] = { Vector2i(x0, z0), Vector2i(x0 + 1, z0), Vector2i(x0, z0 + 1), Vector2i(x0 + 1, z0 + 1) };
	for (int i = 0; i < 4; i++) {
		if (!grid_get(grid, cells[i], h[i])) {
			return Math::NaN;
		}
	}
	if (fx + fz <= 1.0) {
		return h[0] + (h[1] - h[0]) * fx + (h[2] - h[0]) * fz;
	}
	return h[3] + (h[2] - h[3]) * (1.0 - fx) + (h[1] - h[3]) * (1.0 - fz);
}

/* TerrainDeformer3D: properties */

void TerrainDeformer3D::set_terrain(const NodePath &p_path) {
	if (terrain_path == p_path) {
		return;
	}
	// A different ground: put the old one back first, then carve the new one.
	if (is_inside_tree() && !base_heights.is_empty()) {
		clear();
	}
	terrain_path = p_path;
	if (is_inside_tree() && auto_apply) {
		queue_apply();
	}
	update_configuration_warnings();
}

void TerrainDeformer3D::set_ground_mesh(const NodePath &p_path) {
	ground_mesh_path = p_path;
	if (is_inside_tree() && auto_apply) {
		queue_apply();
	}
}

void TerrainDeformer3D::set_brushes(const TypedArray<NodePath> &p_brushes) {
	brush_paths = p_brushes;
	if (is_inside_tree()) {
		_connect_brushes();
		if (auto_apply) {
			queue_apply();
		}
	}
}

void TerrainDeformer3D::_set_base_keys(const PackedInt64Array &p_keys) {
	pending_keys = p_keys;
}

PackedInt64Array TerrainDeformer3D::_get_base_keys() const {
	PackedInt64Array keys;
	keys.resize(base_heights.size());
	int i = 0;
	for (const KeyValue<int64_t, float> &kv : base_heights) {
		keys.write[i++] = kv.key;
	}
	return keys;
}

void TerrainDeformer3D::_set_base_values(const PackedFloat32Array &p_values) {
	ERR_FAIL_COND_MSG(p_values.size() != pending_keys.size(), "TerrainDeformer3D: base heights and keys differ in size; ignoring them.");
	base_heights.clear();
	base_heights.reserve(p_values.size());
	for (int i = 0; i < p_values.size(); i++) {
		base_heights.insert(pending_keys[i], p_values[i]);
	}
	pending_keys.clear();
}

PackedFloat32Array TerrainDeformer3D::_get_base_values() const {
	// HashMap iterates in insertion order, so values line up with _get_base_keys().
	PackedFloat32Array values;
	values.resize(base_heights.size());
	int i = 0;
	for (const KeyValue<int64_t, float> &kv : base_heights) {
		values.write[i++] = kv.value;
	}
	return values;
}

PackedStringArray TerrainDeformer3D::get_configuration_warnings() const {
	PackedStringArray warnings = Node3D::get_configuration_warnings();
	if (!is_inside_tree()) {
		return warnings;
	}
	Node *node = get_node_or_null(terrain_path);
	if (!node) {
		warnings.push_back(RTR("Set \"terrain\" to a Terrain3D node or to a CollisionShape3D with a HeightMapShape3D."));
	} else if (!node->is_class("Terrain3D")) {
		CollisionShape3D *cs = Object::cast_to<CollisionShape3D>(node);
		if (!cs || Ref<HeightMapShape3D>(cs->get_shape()).is_null()) {
			warnings.push_back(RTR("\"terrain\" must be a Terrain3D node or a CollisionShape3D with a HeightMapShape3D."));
		}
	}
	return warnings;
}

void TerrainDeformer3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			_connect_brushes();
			if (!auto_apply) {
				break;
			}
			// Terrain3D loads its regions when its data directory is set, which can come after this node is ready.
			Node *node = get_node_or_null(terrain_path);
			if (node && node->is_class("Terrain3D")) {
				Object *t3d_data = node->call("get_data");
				const int regions = t3d_data ? int(t3d_data->call("get_region_count")) : 0;
				if (regions == 0 && t3d_data && t3d_data->has_signal("region_map_changed")) {
					waiting_for_terrain = true;
					t3d_data->connect("region_map_changed", callable_mp(this, &TerrainDeformer3D::_on_terrain_ready), CONNECT_ONE_SHOT);
					break;
				}
			}
			queue_apply();
		} break;
	}
}

void TerrainDeformer3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_terrain", "path"), &TerrainDeformer3D::set_terrain);
	ClassDB::bind_method(D_METHOD("get_terrain"), &TerrainDeformer3D::get_terrain);
	ClassDB::bind_method(D_METHOD("set_ground_mesh", "path"), &TerrainDeformer3D::set_ground_mesh);
	ClassDB::bind_method(D_METHOD("get_ground_mesh"), &TerrainDeformer3D::get_ground_mesh);
	ClassDB::bind_method(D_METHOD("set_brushes", "brushes"), &TerrainDeformer3D::set_brushes);
	ClassDB::bind_method(D_METHOD("get_brushes"), &TerrainDeformer3D::get_brushes);
	ClassDB::bind_method(D_METHOD("set_auto_apply", "enable"), &TerrainDeformer3D::set_auto_apply);
	ClassDB::bind_method(D_METHOD("is_auto_apply"), &TerrainDeformer3D::is_auto_apply);
	ClassDB::bind_method(D_METHOD("set_base_spacing", "spacing"), &TerrainDeformer3D::set_base_spacing);
	ClassDB::bind_method(D_METHOD("get_base_spacing"), &TerrainDeformer3D::get_base_spacing);
	ClassDB::bind_method(D_METHOD("_set_base_keys", "keys"), &TerrainDeformer3D::_set_base_keys);
	ClassDB::bind_method(D_METHOD("_get_base_keys"), &TerrainDeformer3D::_get_base_keys);
	ClassDB::bind_method(D_METHOD("_set_base_values", "values"), &TerrainDeformer3D::_set_base_values);
	ClassDB::bind_method(D_METHOD("_get_base_values"), &TerrainDeformer3D::_get_base_values);

	ClassDB::bind_method(D_METHOD("add_brush", "brush"), &TerrainDeformer3D::add_brush);
	ClassDB::bind_method(D_METHOD("remove_brush", "brush"), &TerrainDeformer3D::remove_brush);
	ClassDB::bind_method(D_METHOD("get_backend"), &TerrainDeformer3D::get_backend);
	ClassDB::bind_method(D_METHOD("apply"), &TerrainDeformer3D::apply);
	ClassDB::bind_method(D_METHOD("queue_apply"), &TerrainDeformer3D::queue_apply);
	ClassDB::bind_method(D_METHOD("clear"), &TerrainDeformer3D::clear);
	ClassDB::bind_method(D_METHOD("get_base_height", "global_position"), &TerrainDeformer3D::get_base_height);
	ClassDB::bind_method(D_METHOD("base_height_at", "world_xz"), &TerrainDeformer3D::base_height_at);
	ClassDB::bind_method(D_METHOD("get_height", "global_position"), &TerrainDeformer3D::get_height);
	ClassDB::bind_method(D_METHOD("set_brush_tile_size", "size"), &TerrainDeformer3D::set_brush_tile_size);
	ClassDB::bind_method(D_METHOD("get_brush_tile_size"), &TerrainDeformer3D::get_brush_tile_size);
	ClassDB::bind_method(D_METHOD("get_carved_vertex_count"), &TerrainDeformer3D::get_carved_vertex_count);

	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "terrain"), "set_terrain", "get_terrain");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "ground_mesh", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "MeshInstance3D"), "set_ground_mesh", "get_ground_mesh");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "brushes", PROPERTY_HINT_ARRAY_TYPE, "NodePath"), "set_brushes", "get_brushes");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "auto_apply"), "set_auto_apply", "is_auto_apply");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "brush_tile_size", PROPERTY_HINT_RANGE, "8,512,1,or_greater,suffix:m"), "set_brush_tile_size", "get_brush_tile_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "base_spacing", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_base_spacing", "get_base_spacing");
	// Keys before values: the setter of the second pairs them up.
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT64_ARRAY, "base_keys", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "_set_base_keys", "_get_base_keys");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_FLOAT32_ARRAY, "base_values", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "_set_base_values", "_get_base_values");

	ADD_SIGNAL(MethodInfo("applied", PropertyInfo(Variant::INT, "vertices")));

	BIND_ENUM_CONSTANT(BACKEND_NONE);
	BIND_ENUM_CONSTANT(BACKEND_TERRAIN3D);
	BIND_ENUM_CONSTANT(BACKEND_HEIGHT_MAP);
}

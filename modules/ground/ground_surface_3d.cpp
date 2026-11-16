/**************************************************************************/
/*  ground_surface_3d.cpp                                                 */
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

#include "ground_surface_3d.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "modules/terrain_deform/terrain_generator_3d.h"
#include "modules/weather/weather_map_3d.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/physics/rigid_body_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "scene/main/window.h"
#include "servers/rendering/rendering_server.h"

GroundSurface3D *GroundSurface3D::active = nullptr;

// ---------------------------------------------------------------------------------------------------------
// The rules

// Bekker values: Wong, "Theory of Ground Vehicles", table of terrain values (dry sand, Land Locomotion
// Lab; sandy loam at 15% moisture). Damp sand is firmer (capillary bridges, Hornbaker et al. 1997) and
// its walls stand far steeper; soft mud is fitted to 5 to 10 cm under a boot, since Wong's clays are
// trafficable soils. Repose of dry sand 33 degrees (Al-Hashemi and Al-Amoudi 2018).
GroundSurface3D::Cell GroundSurface3D::material_cell(float p_sand, float p_moisture, float p_litter) {
	const float m = CLAMP(p_moisture, 0.0f, 1.0f);
	const float cohesion = Math::smoothstep(0.0f, 0.5f, m); // damp sand gets all its strength early
	auto lerp_log = [](float a, float b, float t) { return Math::exp(Math::lerp(Math::log(a), Math::log(b), t)); };
	Cell sand;
	sand.n = Math::lerp(1.1f, 1.0f, m);
	sand.kc = Math::lerp(0.99f, 0.5f, m);
	sand.kphi = lerp_log(1528.43f, 6000.0f, cohesion);
	sand.rim_share = Math::lerp(0.8f, 0.7f, m);
	sand.tan_repose = Math::tan(Math::deg_to_rad(Math::lerp(33.0f, 65.0f, cohesion)));
	sand.rate = lerp_log(4.0f, 0.02f, cohesion);
	sand.max_dent = 0.3f;
	sand.darken = Math::lerp(0.3f, 0.12f, m);
	sand.gloss = Math::lerp(1.0f, 0.85f, m);
	// Dry sand (Land Locomotion Lab): c 1.04 kPa, phi 28 deg, loose (K 2.5 cm); damp sand as packed LETE sand
	// (c 1.3 kPa, phi 31.1 deg) and firm in shear (K 1 cm).
	sand.cohesion = Math::lerp(1.04f, 1.3f, cohesion);
	sand.tan_phi = Math::tan(Math::deg_to_rad(Math::lerp(28.0f, 31.1f, cohesion)));
	sand.shear_k = Math::lerp(0.025f, 0.01f, cohesion);
	sand.unit_weight = Math::lerp(15.7f, 17.5f, m);
	// Under a tyre damp sand bears as packed sand does: LETE sand (Wong table 2.2: n 0.79, kc 102, kphi 5301);
	// dry, the Land Locomotion Lab's dry sand, as the boot.
	const float packed = Math::smoothstep(0.0f, 0.35f, m);
	sand.wheel_n = Math::lerp(1.1f, 0.79f, packed);
	sand.wheel_kc = lerp_log(0.99f, 102.0f, packed);
	sand.wheel_kphi = lerp_log(1528.43f, 5301.0f, packed);
	Cell soil;
	soil.n = Math::lerp(0.7f, 1.0f, m);
	soil.kc = Math::lerp(5.27f, 3.5f, m);
	soil.kphi = lerp_log(1515.04f, 230.0f, m);
	soil.rim_share = Math::lerp(0.4f, 0.95f, m);
	soil.tan_repose = Math::tan(Math::deg_to_rad(Math::lerp(40.0f, 25.0f, m)));
	soil.rate = lerp_log(0.5f, 1.0f / 120.0f, m); // crumbling edges dry, a slow creep as mud (Coussot 1997)
	soil.max_dent = Math::lerp(0.08f, 0.3f, m);
	soil.darken = Math::lerp(0.3f, 0.15f, m);
	soil.gloss = Math::lerp(1.0f, 0.6f, m);
	// Sandy loam at 15% (c 1.72 kPa, phi 29 deg) to clayey soil at 38% (c 4.14 kPa, phi 13 deg), Wong table 2.2.
	soil.cohesion = Math::lerp(1.72f, 4.14f, m);
	soil.tan_phi = Math::tan(Math::deg_to_rad(Math::lerp(29.0f, 13.0f, m)));
	soil.shear_k = 0.025f;
	soil.unit_weight = Math::lerp(15.0f, 17.0f, m);
	soil.wheel_n = soil.n;
	soil.wheel_kc = soil.kc;
	soil.wheel_kphi = soil.kphi;
	const float s = CLAMP(p_sand, 0.0f, 1.0f);
	Cell c;
	c.n = Math::lerp(soil.n, sand.n, s);
	c.kc = Math::lerp(soil.kc, sand.kc, s);
	c.kphi = lerp_log(soil.kphi, sand.kphi, s);
	c.rim_share = Math::lerp(soil.rim_share, sand.rim_share, s);
	c.tan_repose = Math::lerp(soil.tan_repose, sand.tan_repose, s);
	c.rate = lerp_log(soil.rate, sand.rate, s);
	c.max_dent = Math::lerp(soil.max_dent, sand.max_dent, s);
	c.darken = Math::lerp(soil.darken, sand.darken, s);
	c.gloss = Math::lerp(soil.gloss, sand.gloss, s);
	c.cohesion = Math::lerp(soil.cohesion, sand.cohesion, s);
	c.tan_phi = Math::lerp(soil.tan_phi, sand.tan_phi, s);
	c.shear_k = Math::lerp(soil.shear_k, sand.shear_k, s);
	c.unit_weight = Math::lerp(soil.unit_weight, sand.unit_weight, s);
	c.wheel_n = Math::lerp(soil.wheel_n, sand.wheel_n, s);
	c.wheel_kc = lerp_log(soil.wheel_kc, sand.wheel_kc, s);
	c.wheel_kphi = lerp_log(soil.wheel_kphi, sand.wheel_kphi, s);
	c.sand = s;
	// Leaf litter on top: compresses to a third under a boot, pushes little aside, and shows the darker
	// soil where it is pressed through.
	c.litter = MAX(p_litter, 0.0f);
	if (c.litter > 0.0f) {
		const float lw = MIN(c.litter / 0.02f, 1.0f);
		c.max_dent += 0.7f * c.litter;
		c.rim_share = Math::lerp(c.rim_share, 0.1f, lw);
		c.darken = MAX(c.darken, 0.45f * lw);
		c.tan_repose = MAX(c.tan_repose, Math::lerp(c.tan_repose, Math::tan(Math::deg_to_rad(60.0f)), lw));
	}
	return c;
}

GroundDeformField::Column GroundSurface3D::Columns::column(float p_x, float p_z) const {
	GroundDeformField::Column col;
	const Cell &c = surface->_cell_at(p_x, p_z);
	// Only where the terrain is fully lowered under the patch.
	if (!c.deformable || surface->_edge_distance(p_x, p_z) < surface->terrain_margin + surface->skirt_fade) {
		return col;
	}
	col.deformable = true;
	col.surface = surface->_ground_at(p_x, p_z);
	col.max_dent = c.max_dent;
	col.rim_share = c.rim_share;
	return col;
}

float GroundSurface3D::Columns::sinkage(const GroundDeformField::Body &p_body) const {
	const Cell &c = surface->_cell_at(p_body.center.x, p_body.center.z);
	if (!c.deformable) {
		return 0.0f;
	}
	float sink = GroundDeformField::body_sinkage(p_body, c.n, c.kc, c.kphi);
	if (c.litter > 0.0f && p_body.mass > 0.0f) {
		const float area = p_body.area > 0.0f ? p_body.area : (p_body.flat_area > 0.0f ? p_body.flat_area : Math::PI * p_body.radius * p_body.radius * 0.25f);
		const float kpa = p_body.mass * 9.81f / 1000.0f / MAX(area, 1e-3f);
		sink += 0.7f * c.litter * MIN(kpa / 5.0f, 1.0f);
	}
	return MIN(sink, c.max_dent);
}

// ---------------------------------------------------------------------------------------------------------
// Lattice and cells

float GroundSurface3D::_ground_at(float p_x, float p_z) const {
	const int n = ground_n;
	if (!ground_valid || n < 2) {
		return 0.0f;
	}
	const float fx = CLAMP(p_x / ground_spacing - ground_origin.x, 0.0f, float(n - 1));
	const float fz = CLAMP(p_z / ground_spacing - ground_origin.y, 0.0f, float(n - 1));
	const int x0 = MIN(int(fx), n - 2);
	const int z0 = MIN(int(fz), n - 2);
	const float tx = fx - x0;
	const float tz = fz - z0;
	auto at = [&](int x, int z) { return ground[z * n + x]; };
	return Math::lerp(Math::lerp(at(x0, z0), at(x0 + 1, z0), tx), Math::lerp(at(x0, z0 + 1), at(x0 + 1, z0 + 1), tx), tz);
}

const GroundSurface3D::Cell &GroundSurface3D::_cell_at(float p_x, float p_z) const {
	static const Cell none = [] { Cell c; c.deformable = false; return c; }();
	const int n = ground_n;
	if (!ground_valid || int(cells.size()) != n * n) {
		return none;
	}
	const int x = int(Math::round(p_x / ground_spacing)) - ground_origin.x;
	const int z = int(Math::round(p_z / ground_spacing)) - ground_origin.y;
	if (x < 0 || z < 0 || x >= n || z >= n) {
		return none;
	}
	return cells[z * n + x];
}

float GroundSurface3D::_edge_distance(float p_x, float p_z) const {
	if (!field.valid) {
		return -1.0f;
	}
	const Rect2 r = field.rect();
	return MIN(MIN(p_x - r.position.x, r.position.x + r.size.x - p_x), MIN(p_z - r.position.y, r.position.y + r.size.y - p_z));
}

void GroundSurface3D::_sample_ground(Vector2i p_origin, int p_n) {
	const GroundHeightQuery q = GroundHeightQuery::make(this, height_source, ground_collision_mask);
	LocalVector<float> next;
	next.resize(p_n * p_n);
	const bool keep = ground_valid && ground_n == p_n;
	const Vector2i d = p_origin - ground_origin;
	const float fallback = q.y0;
	for (int z = 0; z < p_n; z++) {
		for (int x = 0; x < p_n; x++) {
			const int sx = x + d.x;
			const int sz = z + d.y;
			float h;
			if (keep && sx >= 0 && sz >= 0 && sx < p_n && sz < p_n) {
				h = ground[sz * p_n + sx];
			} else {
				h = q.height(Vector3((p_origin.x + x) * ground_spacing, 0.0f, (p_origin.y + z) * ground_spacing));
				if (!Math::is_finite(h)) {
					h = fallback;
				}
			}
			next[z * p_n + x] = h;
		}
	}
	ground = next;
	ground_n = p_n;
	ground_origin = p_origin;
	ground_valid = true;
	gpu_ground_dirty = true;
	ground_min = 1e20f;
	ground_max = -1e20f;
	for (float h : ground) {
		ground_min = MIN(ground_min, h);
		ground_max = MAX(ground_max, h);
	}
	Vector<uint8_t> bytes;
	bytes.resize(p_n * p_n * 4);
	memcpy(bytes.ptrw(), ground.ptr(), p_n * p_n * 4);
	if (height_image.is_null() || height_image->get_width() != p_n) {
		height_image = Image::create_from_data(p_n, p_n, false, Image::FORMAT_RF, bytes);
		height_texture = ImageTexture::create_from_image(height_image);
		globals_published = false;
	} else {
		height_image->set_data(p_n, p_n, false, Image::FORMAT_RF, bytes);
		height_texture->update(height_image);
	}
	_update_cells();
}

void GroundSurface3D::_update_cells() {
	const int n = ground_n;
	if (!ground_valid || n <= 0) {
		return;
	}
	cells.resize(n * n);
	gpu_cells_dirty = true;
	const WeatherMap3D *wm = WeatherMap3D::get_active();
	Node *hs = height_source.is_empty() || !is_inside_tree() ? nullptr : get_node_or_null(height_source);
	const TerrainGenerator3D *gen = rock_from_geology ? Object::cast_to<TerrainGenerator3D>(hs) : nullptr;
	Node *snow = snow_cover.is_empty() || !is_inside_tree() ? nullptr : get_node_or_null(snow_cover);
	if (snow && !snow->has_method("get_depth_at")) {
		snow = nullptr;
	}
	const float base_sand = ground_type == GROUND_SAND ? 1.0f : 0.0f;
	const bool map = type_map.is_valid() && !type_map->is_empty() && type_map_rect.size.x > 0.0f && type_map_rect.size.y > 0.0f;
	for (int z = 0; z < n; z++) {
		for (int x = 0; x < n; x++) {
			const int i = z * n + x;
			const Vector3 p((ground_origin.x + x) * ground_spacing, ground[i], (ground_origin.y + z) * ground_spacing);
			float sand = base_sand;
			bool rock = false;
			if (map) {
				const Vector2 uv = (Vector2(p.x, p.z) - type_map_rect.position) / type_map_rect.size;
				if (uv.x >= 0.0f && uv.y >= 0.0f && uv.x <= 1.0f && uv.y <= 1.0f) {
					const Color c = type_map->get_pixel(CLAMP(int(uv.x * type_map->get_width()), 0, type_map->get_width() - 1), CLAMP(int(uv.y * type_map->get_height()), 0, type_map->get_height() - 1));
					sand = c.r;
					rock = c.g > 0.5f;
				}
			}
			if (gen && gen->get_rock_exposure(p) > 0.5f) {
				rock = true;
			}
			const float m = moisture >= 0.0f ? moisture : (wm ? wm->get_wetness_at(p) : 0.0f);
			const float closure = wm ? wm->get_canopy_closure_at(p) : litter_cover;
			Cell c = material_cell(sand, m, litter_depth * CLAMP(closure, 0.0f, 1.0f));
			// Under more than a few centimetres of snow the snow surface takes the print.
			if (rock || (snow && float(snow->call("get_depth_at", p)) > 0.03f)) {
				c.deformable = false;
			}
			cells[i] = c;
		}
	}
}

// ---------------------------------------------------------------------------------------------------------
// Following and drawing

Vector3 GroundSurface3D::_follow_position() const {
	if (!follow_target.is_empty()) {
		if (const Node3D *t = Object::cast_to<Node3D>(get_node_or_null(follow_target))) {
			return t->get_global_position();
		}
	}
	if (is_inside_tree() && get_viewport()) {
		if (const Camera3D *cam = get_viewport()->get_camera_3d()) {
			return cam->get_global_position();
		}
	}
	return is_inside_tree() ? get_global_position() : Vector3();
}

void GroundSurface3D::_build_mesh() {
	// A tensor grid over the field: detail_spacing over its middle half, outer_spacing beyond.
	const float half = deform_extent * 0.5f;
	const float inner = deform_extent * 0.25f;
	LocalVector<float> pos;
	for (float x = detail_spacing; x < inner - 1e-4f; x += detail_spacing) {
		pos.push_back(x);
	}
	float x = pos.size() ? pos[pos.size() - 1] : 0.0f;
	while (x < half - 1e-4f) {
		x = MIN(x + outer_spacing, half);
		pos.push_back(x);
	}
	LocalVector<float> c;
	for (int i = int(pos.size()) - 1; i >= 0; i--) {
		c.push_back(-pos[i]);
	}
	c.push_back(0.0f);
	for (float p : pos) {
		c.push_back(p);
	}
	const int n = c.size();
	PackedVector3Array verts;
	PackedVector3Array normals;
	PackedFloat32Array tangents;
	PackedVector2Array uvs;
	verts.resize(n * n);
	normals.resize(n * n);
	tangents.resize(n * n * 4);
	uvs.resize(n * n);
	for (int z = 0; z < n; z++) {
		for (int xi = 0; xi < n; xi++) {
			const int i = z * n + xi;
			verts.set(i, Vector3(c[xi], 0.0f, c[z]));
			normals.set(i, Vector3(0, 1, 0));
			uvs.set(i, Vector2(c[xi], c[z]));
			tangents.set(i * 4 + 0, 1.0f);
			tangents.set(i * 4 + 1, 0.0f);
			tangents.set(i * 4 + 2, 0.0f);
			tangents.set(i * 4 + 3, 1.0f);
		}
	}
	PackedInt32Array idx;
	idx.resize((n - 1) * (n - 1) * 6);
	int k = 0;
	for (int z = 0; z < n - 1; z++) {
		for (int xi = 0; xi < n - 1; xi++) {
			const int a = z * n + xi;
			const int b = a + 1;
			const int d = a + n;
			const int e = d + 1;
			idx.set(k++, a);
			idx.set(k++, b);
			idx.set(k++, d);
			idx.set(k++, b);
			idx.set(k++, e);
			idx.set(k++, d);
		}
	}
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = verts;
	arrays[Mesh::ARRAY_NORMAL] = normals;
	arrays[Mesh::ARRAY_TANGENT] = tangents;
	arrays[Mesh::ARRAY_TEX_UV] = uvs;
	arrays[Mesh::ARRAY_INDEX] = idx;
	mesh.instantiate();
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	if (mesh_instance) {
		mesh_instance->set_mesh(mesh);
	}
	mesh_dirty = false;
}

void GroundSurface3D::_update_patch_material() {
	Ref<Material> src = material;
	if (src.is_null()) {
		Ref<StandardMaterial3D> m;
		m.instantiate();
		m->set_albedo(Color(0.42f, 0.35f, 0.27f));
		m->set_roughness(0.95f);
		m->set_feature(BaseMaterial3D::FEATURE_GROUND_DEFORM, true);
		src = m;
	}
	patch_material = src->duplicate();
	if (BaseMaterial3D *bm = Object::cast_to<BaseMaterial3D>(patch_material.ptr())) {
		bm->set_feature(BaseMaterial3D::FEATURE_GROUND_DEFORM, true);
		bm->set_ground_deform_patch(true);
		patch_uv_offset = bm->get_uv1_offset();
	} else if (ShaderMaterial *sm = Object::cast_to<ShaderMaterial>(patch_material.ptr())) {
		sm->set_shader_parameter("ground_deform_patch", true);
	}
	if (mesh_instance) {
		mesh_instance->set_material_override(patch_material);
	}
}

void GroundSurface3D::_ensure_children() {
	if (!mesh_instance) {
		mesh_instance = memnew(MeshInstance3D);
		mesh_instance->set_as_top_level(true);
		add_child(mesh_instance, false, INTERNAL_MODE_FRONT);
		_update_patch_material();
	}
	if (mesh_dirty) {
		_build_mesh();
	}
}

void GroundSurface3D::_follow(Vector3 p_center) {
	field.extent = deform_extent;
	field.resolution = deform_resolution;
	// On the GPU path the tile cache is the GPU's; the CPU copy keeps none.
	field.max_cached_tiles = _gpu_on() ? 0 : max_cached_tiles;
	field.track_fresh = true;
	const Vector2 xz(p_center.x, p_center.z);
	const Vector2i want = field.origin_for(xz);
	const bool moved = !field.valid || want != field.origin;
	if (moved) {
		if (_gpu_on()) {
			gpu_host.shift_to(field, want, deform_resolution, max_cached_tiles);
		}
		field.shift(want);
	}
	const Rect2 r = field.rect();
	const int n = int(Math::ceil(deform_extent / ground_spacing)) + 3;
	const Vector2i go(int(Math::floor(r.position.x / ground_spacing)) - 1, int(Math::floor(r.position.y / ground_spacing)) - 1);
	if (!ground_valid || go != ground_origin || n != ground_n) {
		_sample_ground(go, n);
		globals_published = false;
	}
	const Vector2 c = r.position + r.size * 0.5f;
	if (moved || c != center || (mesh_instance && mesh_instance->get_global_position() != Vector3(c.x, 0, c.y))) {
		center = c;
		if (mesh_instance) {
			mesh_instance->set_global_transform(Transform3D(Basis(), Vector3(c.x, 0.0f, c.y)));
			mesh_instance->set_custom_aabb(AABB(Vector3(-r.size.x * 0.5f, ground_min - skirt - 1.0f, -r.size.y * 0.5f), Vector3(r.size.x, ground_max - ground_min + skirt + 2.0f, r.size.y)));
		}
		// Mesh UVs are metres from the patch centre: keep the material's world mapping.
		if (BaseMaterial3D *bm = Object::cast_to<BaseMaterial3D>(patch_material.ptr())) {
			const Vector3 s = bm->get_uv1_scale();
			bm->set_uv1_offset(patch_uv_offset + Vector3(Math::fposmod(c.x * s.x, 1.0f), Math::fposmod(c.y * s.y, 1.0f), 0.0f));
		}
		globals_published = false;
	}
}

void GroundSurface3D::_set_global(const StringName &p_name, const Variant &p_value, int p_type) {
	RenderingServer *rs = RenderingServer::get_singleton();
	static HashSet<StringName> added;
	if (!added.has(p_name) && !ProjectSettings::get_singleton()->has_setting("shader_globals/" + String(p_name))) {
		added.insert(p_name);
		rs->global_shader_parameter_add(p_name, RSE::GlobalShaderParameterType(p_type), p_value);
		return;
	}
	rs->global_shader_parameter_set(p_name, p_value);
}

void GroundSurface3D::_publish() {
	const bool gpu_tex = _gpu_on() && gpu_host.texture.is_valid() && gpu_host.texture->get_texture_rd_rid().is_valid();
	if (active != this || globals_published || (!gpu_tex && deform_texture.is_null()) || height_texture.is_null()) {
		return;
	}
	const Rect2 r = field.rect();
	_set_global("ground_deform_map", gpu_tex ? gpu_host.texture->get_rid() : deform_texture->get_rid(), RSE::GLOBAL_VAR_TYPE_SAMPLER2D);
	_set_global("ground_deform_height", height_texture->get_rid(), RSE::GLOBAL_VAR_TYPE_SAMPLER2D);
	_set_global("ground_deform_rect", Vector4(r.position.x, r.position.y, r.size.x, field.texel()), RSE::GLOBAL_VAR_TYPE_VEC4);
	_set_global("ground_deform_height_rect", Vector4((ground_origin.x - 0.5f) * ground_spacing, (ground_origin.y - 0.5f) * ground_spacing, ground_n * ground_spacing, ground_n * ground_spacing), RSE::GLOBAL_VAR_TYPE_VEC4);
	_set_global("ground_deform_skirt", Vector4(skirt, terrain_margin, skirt_fade, 0.0f), RSE::GLOBAL_VAR_TYPE_VEC4);
	globals_published = true;
}

void GroundSurface3D::_upload() {
	const int n = field.resolution;
	if (!field.valid) {
		return;
	}
	tint_albedo.resize(n * n);
	tint_roughness.resize(n * n);
	const int nt = field.tiles();
	const float t = field.texel();
	const int T = GroundDeformField::TILE;
	for (int tz = 0; tz < nt; tz++) {
		for (int tx = 0; tx < nt; tx++) {
			const bool on = field.tile_active[tz * nt + tx];
			for (int z = tz * T; z < (tz + 1) * T; z++) {
				for (int x = tx * T; x < (tx + 1) * T; x++) {
					const int i = z * n + x;
					const float f = on ? field.fresh[i] : 0.0f;
					if (f <= 0.0f) {
						tint_albedo[i] = 1.0f;
						tint_roughness[i] = 1.0f;
						continue;
					}
					const Cell &c = _cell_at((field.origin.x + x + 0.5f) * t, (field.origin.y + z + 0.5f) * t);
					tint_albedo[i] = 1.0f - f * c.darken;
					tint_roughness[i] = Math::lerp(1.0f, c.gloss, f);
				}
			}
		}
	}
	const RID before = deform_texture.is_valid() ? deform_texture->get_rid() : RID();
	field.upload(deform_image, deform_texture, &tint_albedo, &tint_roughness);
	if (deform_texture.is_valid() && deform_texture->get_rid() != before) {
		globals_published = false;
	}
}

// Every second: turned ground dries, wind fills dry sand prints, rain softens soil prints.
void GroundSurface3D::_age(double p_delta) {
	if (!field.valid) {
		return;
	}
	const float dt = float(p_delta);
	const float dt_h = dt / 3600.0f;
	const WeatherMap3D *wm = WeatherMap3D::get_active();
	const Vector3 at(center.x, _ground_at(center.x, center.y), center.y);
	float wind = 0.0f;
	float rain = 0.0f;
	if (wm) {
		wind = wm->get_wind_at(at, 10.0f).length();
		rain = wm->get_rain_at(at);
	}
	// Sand moves above about 5 m/s at 10 m (Bagnold's threshold for fine dry sand).
	const float drift = sand_drift_rate * MAX(wind - 5.0f, 0.0f) / 5.0f;
	const float soften = rain_smoothing * rain;
	const float dry = Math::exp(-dt / drying_time);
	const int n = field.resolution;
	const int nt = field.tiles();
	const float t = field.texel();
	const int T = GroundDeformField::TILE;
	for (int tz = 0; tz < nt; tz++) {
		for (int tx = 0; tx < nt; tx++) {
			if (!field.tile_active[tz * nt + tx]) {
				continue;
			}
			bool any = false;
			for (int z = tz * T; z < (tz + 1) * T; z++) {
				for (int x = tx * T; x < (tx + 1) * T; x++) {
					const int i = z * n + x;
					float &dn = field.dent[i];
					float &rm = field.rim[i];
					if (dn <= 0.0f && rm <= 0.0f && field.fresh[i] <= 0.0f) {
						continue;
					}
					if (drift > 0.0f || soften > 0.0f) {
						const Cell &c = _cell_at((field.origin.x + x + 0.5f) * t, (field.origin.y + z + 0.5f) * t);
						const float keep = Math::exp(-(drift * c.sand * (1.0f - Math::smoothstep(0.1f, 0.4f, 1.0f - c.gloss)) + soften) * dt_h);
						dn *= keep;
						rm *= keep;
					}
					field.fresh[i] *= dry;
					if (dn < 1e-4f) {
						dn = 0.0f;
					}
					if (rm < 1e-4f) {
						rm = 0.0f;
					}
					if (field.fresh[i] < 0.01f) {
						field.fresh[i] = 0.0f;
					}
					any = any || dn > 0.0f || rm > 0.0f || field.fresh[i] > 0.0f;
				}
			}
			field.tile_active[tz * nt + tx] = any ? 1 : 0;
		}
	}
	field.dirty = true;
}

void GroundSurface3D::settle(float p_seconds, float p_step) {
	const float step = MAX(p_step, 1e-3f);
	if (_gpu_on() && field.valid && is_inside_tree()) {
		float left_gpu = p_seconds;
		double age_gpu = 0.0;
		float sync_accum = 0.0f;
		while (left_gpu > 0.0f) {
			const float dt = MIN(step, left_gpu);
			age_gpu += dt;
			const bool age_now = age_gpu >= 1.0;
			// A blocking read back every simulated second keeps slumping going while texels still owe some.
			sync_accum += dt;
			const bool sync_now = sync_accum >= 1.0f;
			_gpu_submit(dt, age_now ? age_gpu : 0.0, false, sync_now);
			if (sync_now) {
				sync_accum = 0.0f;
			}
			if (age_now) {
				age_gpu = 0.0;
			}
			left_gpu -= dt;
		}
		if (age_gpu > 0.0) {
			_gpu_submit(0.0, age_gpu, false, false);
		}
		sync_deformation();
		return;
	}
	float left = p_seconds;
	auto ground_fn = [this](float x, float z) { return _ground_at(x, z); };
	auto params_fn = [this](float x, float z, float &r_tan, float &r_rate, float &r_max) {
		const Cell &c = _cell_at(x, z);
		if (!c.deformable) {
			return false;
		}
		r_tan = c.tan_repose;
		r_rate = c.rate;
		r_max = c.max_dent;
		return true;
	};
	double age = 0.0;
	while (left > 0.0f) {
		const float dt = MIN(step, left);
		field.slump(dt, ground_fn, params_fn);
		age += dt;
		if (age >= 1.0) {
			_age(age);
			age = 0.0;
		}
		left -= dt;
	}
	if (age > 0.0) {
		_age(age);
	}
	if (field.dirty) {
		_upload();
	}
}

void GroundSurface3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			if (!active) {
				active = this;
			}
			_ensure_children();
			if (!Engine::get_singleton()->is_editor_hint()) {
				set_physics_process_internal(true);
			}
		} break;
		case NOTIFICATION_READY: {
			_follow(_follow_position());
			_upload();
			_publish();
		} break;
		case NOTIFICATION_EXIT_TREE: {
			set_physics_process_internal(false);
			_gpu_release();
			if (active == this) {
				_set_global("ground_deform_rect", Vector4(), RSE::GLOBAL_VAR_TYPE_VEC4);
				active = nullptr;
			}
			globals_published = false;
		} break;
		case NOTIFICATION_INTERNAL_PHYSICS_PROCESS: {
			const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
			const double delta = get_physics_process_delta_time();
			if (!active) {
				active = this;
			}
			_follow(_follow_position());
			cells_accum += delta;
			if (cells_accum >= 2.0) {
				cells_accum = 0.0;
				_update_cells();
			}
			const bool on_gpu = _gpu_on();
			if (!on_gpu && gpu_host.gpu.is_valid()) {
				_gpu_release();
				field.invalidate();
				_follow(_follow_position());
			}
			if (on_gpu && field.valid) {
				_gpu_take();
				Columns cols;
				cols.surface = this;
				const Cell &c = _cell_at(center.x, center.y);
				field.press_settle = c.deformable ? CLAMP(3.0f / MAX(c.rate, 1e-3f), 1.0f, 600.0f) : 1.0f;
				// The bodies press as stamps for the GPU field (the same maths).
				field.stamp_sink = &gpu_sink;
				field.press_bodies(get_tree(), deform_groups, cols, character_mass, character_contact_area, contact_tolerance, delta);
				field.stamp_sink = nullptr;
				_plough_bodies();
				age_accum += delta;
				double age = 0.0;
				if (age_accum >= 1.0) {
					age = age_accum;
					age_accum = 0.0;
				}
				gpu_readback_accum += delta;
				bool readback = false;
				if (gpu_readback_accum >= 1.0 / 15.0) {
					gpu_readback_accum = 0.0;
					readback = true;
				}
				_gpu_submit(delta, age, readback, false);
			} else if (field.valid) {
				Columns cols;
				cols.surface = this;
				const Cell &c = _cell_at(center.x, center.y);
				field.press_settle = c.deformable ? CLAMP(3.0f / MAX(c.rate, 1e-3f), 1.0f, 600.0f) : 1.0f;
				field.press_bodies(get_tree(), deform_groups, cols, character_mass, character_contact_area, contact_tolerance, delta);
				_plough_bodies();
				auto ground_fn = [this](float x, float z) { return _ground_at(x, z); };
				auto params_fn = [this](float x, float z, float &r_tan, float &r_rate, float &r_max) {
					const Cell &cl = _cell_at(x, z);
					if (!cl.deformable) {
						return false;
					}
					r_tan = cl.tan_repose;
					r_rate = cl.rate;
					r_max = cl.max_dent;
					return true;
				};
				field.slump(delta, ground_fn, params_fn);
				age_accum += delta;
				if (age_accum >= 1.0) {
					_age(age_accum);
					age_accum = 0.0;
				}
			}
			if (field.dirty && !on_gpu) {
				_upload();
			}
			_publish();
			step_msec = double(OS::get_singleton()->get_ticks_usec() - t0) / 1000.0;
		} break;
	}
}

// ---------------------------------------------------------------------------------------------------------
// Soft soil (docs/ideas/sand.md, stage 1)

GroundSoil GroundSurface3D::get_soil(Vector3 p_position) const {
	GroundSoil soil;
	const Cell &c = _cell_at(p_position.x, p_position.z);
	if (!c.deformable || !field.valid || _edge_distance(p_position.x, p_position.z) < terrain_margin + skirt_fade) {
		return soil;
	}
	soil.soft = true;
	soil.n = c.wheel_n;
	soil.kc = c.wheel_kc;
	soil.kphi = c.wheel_kphi;
	soil.cohesion = c.cohesion;
	soil.tan_phi = c.tan_phi;
	soil.shear_k = c.shear_k;
	soil.unit_weight = c.unit_weight;
	soil.tan_repose = c.tan_repose;
	soil.max_sinkage = c.max_dent;
	return soil;
}

GroundWheelSoil GroundSurface3D::wheel_soil(Vector3 p_position, float p_load, float p_radius, float p_width, float p_inflation, float p_slip, float p_slip_angle) const {
	const GroundSoil soil = get_soil(p_position);
	const float already = MAX(field.dent_at(p_position.x, p_position.z) - field.rim_at(p_position.x, p_position.z), 0.0f);
	return GroundTerramechanics::wheel(soil, p_load, p_radius, p_width, p_inflation, p_slip, p_slip_angle, already);
}

Dictionary GroundSurface3D::get_wheel_soil(Vector3 p_position, float p_load, float p_radius, float p_width, float p_inflation, float p_slip, float p_slip_angle) const {
	const GroundWheelSoil w = wheel_soil(p_position, p_load, p_radius, p_width, p_inflation, p_slip, p_slip_angle);
	Dictionary d;
	d["soft"] = w.soft;
	d["static_sinkage"] = w.static_sinkage;
	d["sinkage"] = w.sinkage;
	d["contact_length"] = w.contact_length;
	d["compaction"] = w.compaction;
	d["bulldozing"] = w.bulldozing;
	d["resistance"] = w.compaction + w.bulldozing;
	d["shear_limit"] = w.shear_limit;
	d["traction"] = w.traction;
	d["lateral"] = w.lateral;
	return d;
}

float GroundSurface3D::press_wheel(Vector3 p_position, Vector2 p_size, float p_yaw, float p_sinkage, Vector2 p_velocity) {
	if (!field.valid || p_sinkage <= 0.0f || p_size.x <= 0.0f || p_size.y <= 0.0f) {
		return 0.0f;
	}
	Columns cols;
	cols.surface = this;
	const Cell &c = _cell_at(p_position.x, p_position.z);
	field.press_settle = c.deformable ? CLAMP(3.0f / MAX(c.rate, 1e-3f), 1.0f, 600.0f) : 1.0f;
	const Vector2 half = p_size * 0.5f;
	const float radius = 0.5f * MAX(p_size.x, p_size.y);
	const float bottom = _ground_at(p_position.x, p_position.z) - p_sinkage;
	if (_gpu_on()) {
		// The bodies' path: only the GPU field takes it (the CPU copy follows at the read back).
		gpu_sink.stamp(cols, p_position, radius, half, p_yaw, true, bottom, p_sinkage, false, contact_tolerance, p_velocity);
		return 0.0f;
	}
	return field.press(cols, p_position, radius, half, p_yaw, true, bottom, p_sinkage, false, contact_tolerance, p_velocity);
}

void GroundSurface3D::_plough_bodies() {
	// A rigid body sliding or rolling through soft ground (a crate, a rock, a barrel) works the soil it moves
	// into as a wheel does: compaction of its print's new depth and bulldozing of the soil before it (Bekker,
	// Wong 2008), at the body's width, against its travel. It still rests on the undisturbed collider.
	if (!field.valid || !ploughing) {
		return;
	}
	Columns cols;
	cols.surface = this;
	HashSet<ObjectID> done;
	for (const String &group : deform_groups) {
		for (Node *node : get_tree()->get_nodes_in_group(StringName(group))) {
			RigidBody3D *rb = Object::cast_to<RigidBody3D>(node);
			if (!rb || rb->is_freeze_enabled() || rb->is_sleeping() || done.has(rb->get_instance_id())) {
				continue;
			}
			done.insert(rb->get_instance_id());
			const Vector3 v = rb->get_linear_velocity();
			const Vector3 vh(v.x, 0.0f, v.z);
			const float speed = vh.length();
			if (speed < 1e-3f) {
				continue;
			}
			GroundDeformField::Body b;
			field.read_body(rb, character_mass, character_contact_area, b);
			const Cell &c = _cell_at(b.center.x, b.center.z);
			if (!c.deformable) {
				continue;
			}
			// Only while it touches the ground: its underside within the sinkage of the surface.
			const float sink = cols.sinkage(b);
			const float under = b.center.y - b.half_height - _ground_at(b.center.x, b.center.z);
			if (sink <= 0.0f || under > contact_tolerance + sink) {
				continue;
			}
			GroundSoil soil = get_soil(b.center);
			if (!soil.soft) {
				continue;
			}
			// A plate's pressure-sinkage, as its print.
			soil.n = c.n;
			soil.kc = c.kc;
			soil.kphi = c.kphi;
			const Vector3 dir = vh / speed;
			const Vector3 front = b.center + dir * (b.radius + 0.05f);
			const float already = MAX(field.dent_at(front.x, front.z) - field.rim_at(front.x, front.z), 0.0f);
			const float width = b.width > 0.0f ? b.width : 2.0f * b.radius;
			const float resist = GroundTerramechanics::compaction_resistance(soil, width, sink, already) + GroundTerramechanics::bulldozing_resistance(soil, width, sink, already);
			// Eased in over the first 0.3 m/s, and never more than stops it within the step.
			const float step = (float)get_physics_process_delta_time();
			const float stop = rb->get_mass() * speed / MAX(step, 1e-4f);
			const float f = MIN(resist * MIN(speed / 0.3f, 1.0f), stop);
			rb->apply_central_force(-dir * f);
		}
	}
}

Dictionary GroundSurface3D::get_footing_at(Vector3 p_position, float p_mass, float p_area) const {
	Dictionary d;
	const GroundSoil soil = get_soil(p_position);
	float sink = 0.0f;
	Vector3 slide;
	if (soil.soft) {
		Columns cols;
		cols.surface = this;
		GroundDeformField::Body b;
		b.center = p_position;
		b.mass = p_mass;
		b.area = p_area;
		sink = cols.sinkage(b);
		// The surface's slope over a boot length; past what the disturbed material holds (its repose, less the
		// few degrees between the angle at which a granular slope starts and stops flowing; Pouliquen 1999,
		// "Scaling laws in granular flows down rough inclined planes"), the footing slides with the grains:
		// Coulomb friction on the slope, a = g (sin t - cos t tan(repose - 3 deg)).
		const float h = 0.15f;
		const float gx = (get_surface_height_at(p_position + Vector3(h, 0, 0)) - get_surface_height_at(p_position - Vector3(h, 0, 0))) / (2.0f * h);
		const float gz = (get_surface_height_at(p_position + Vector3(0, 0, h)) - get_surface_height_at(p_position - Vector3(0, 0, h))) / (2.0f * h);
		const float grad = Math::sqrt(gx * gx + gz * gz);
		const float theta = Math::atan(grad);
		const float hold = MAX(Math::atan(soil.tan_repose) - Math::deg_to_rad(3.0f), 0.0f);
		if (grad > 1e-4f && theta > hold) {
			const float a = 9.81f * (Math::sin(theta) - Math::cos(theta) * Math::tan(hold));
			// Down the slope, along the surface.
			const Vector3 down = Vector3(-gx, -grad * grad, -gz).normalized();
			slide = down * a;
		}
	}
	// Lejeune, Willems and Heglund 1998, "Mechanics and energetics of human locomotion on sand" (J. Exp. Biol.
	// 201): 1.6 to 2.5 times the work of firm ground. Fitted to the print depth: 2 cm (dry sand) costs 2.1
	// times, damp sand (0.6 cm) 1.3, deep mud at most 2.65; at the same effort the stride slows by the same
	// factor.
	const float cost = 1.0f + 1.1f * MIN(sink / 0.02f, 1.5f);
	d["soft"] = soil.soft;
	d["sinkage"] = sink;
	d["effort"] = cost;
	d["speed_scale"] = 1.0f / cost;
	d["slide"] = slide;
	return d;
}

// ---------------------------------------------------------------------------------------------------------
// The field on the GPU (docs/ideas/sand.md, stage 0)

bool GroundSurface3D::_gpu_on() const {
	return gpu_simulation && GroundDeformGPU::is_available();
}

void GroundSurface3D::set_gpu_simulation(bool p_enabled) {
	if (gpu_simulation == p_enabled) {
		return;
	}
	gpu_simulation = p_enabled;
	// Each path keeps its own field: start the new one empty.
	_gpu_release();
	field.invalidate();
	field.clear();
	globals_published = false;
	if (is_inside_tree()) {
		_follow(_follow_position());
		_upload();
		_publish();
	}
}

void GroundSurface3D::_gpu_release() {
	gpu_host.release();
	gpu_ground_dirty = true;
	gpu_cells_dirty = true;
	globals_published = false;
}

void GroundSurface3D::GPUSink::stamp(const GroundDeformField::Columns &p_columns, Vector3 p_position, float p_radius, Vector2 p_half, float p_yaw, bool p_box, float p_bottom, float p_sink, bool p_round, float p_contact_tolerance, Vector2 p_velocity) {
	GroundDeformField &f = surface->field;
	if (!f.valid || p_radius <= 0.0f || (p_box && (p_half.x <= 0.0f || p_half.y <= 0.0f))) {
		return;
	}
	// The footprint's reach, as GroundDeformField::press works it out (no crusts in the ground).
	const float ring = 0.5f * (p_box ? Math::sqrt(p_half.x * p_half.y) : p_radius);
	const float bow = CLAMP(p_velocity.length() / GroundDeformField::BOW_SPEED, 0.0f, 1.0f);
	const float span = (p_box ? p_half.length() : p_radius) + ring * (1.0f + bow * GroundDeformField::BOW_REACH);
	int x0, x1, z0, z1;
	if (!GroundDeformGPUHost::stamp_bounds(f, p_position, span, x0, x1, z0, z1)) {
		return;
	}
	const uint32_t flags = (p_box ? 1u : 0u) | (p_round ? 2u : 0u);
	const float v[12] = {
		p_position.x, p_position.y, p_position.z, p_radius,
		p_half.x, p_half.y, p_yaw, p_bottom,
		p_sink, p_contact_tolerance, p_velocity.x, p_velocity.y
	};
	const float settle = MAX(f.press_settle, 0.0f);
	const float d[4] = { 0.0f, float(flags), 0.0f, settle };
	surface->gpu_host.add_stamp(f, v, d, x0, x1, z0, z1);
	surface->gpu_host.slump_until = MAX(surface->gpu_host.slump_until, surface->gpu_host.time + settle + 0.1);
}

PackedByteArray GroundSurface3D::_gpu_cells() const {
	PackedByteArray out;
	const int n = ground_n;
	if (!ground_valid || int(cells.size()) != n * n) {
		return out;
	}
	out.resize(n * n * 32);
	float *w = reinterpret_cast<float *>(out.ptrw());
	for (int i = 0; i < n * n; i++) {
		const Cell &c = cells[i];
		w[i * 8 + 0] = c.rim_share;
		w[i * 8 + 1] = c.tan_repose;
		w[i * 8 + 2] = c.rate;
		w[i * 8 + 3] = c.max_dent;
		w[i * 8 + 4] = c.darken;
		w[i * 8 + 5] = c.gloss;
		w[i * 8 + 6] = c.sand;
		w[i * 8 + 7] = c.deformable ? 1.0f : 0.0f;
	}
	return out;
}

PackedByteArray GroundSurface3D::_gpu_params(double p_delta, double p_age, double p_slow) const {
	PackedByteArray b;
	b.resize(GroundDeformGPU::PARAMS_BYTES);
	uint8_t *w = b.ptrw();
	memset(w, 0, GroundDeformGPU::PARAMS_BYTES);
	const int n = deform_resolution;
	const int32_t res[4] = { n, n / GroundDeformField::TILE, ground_n, ground_n };
	memcpy(w, res, 16);
	float *f = reinterpret_cast<float *>(w + 16);
	auto put = [&](int p_vec, float a, float bb, float c, float d) {
		f[p_vec * 4 + 0] = a;
		f[p_vec * 4 + 1] = bb;
		f[p_vec * 4 + 2] = c;
		f[p_vec * 4 + 3] = d;
	};
	const Rect2 r = field.rect();
	put(0, float(field.origin.x), float(field.origin.y), field.texel(), float(p_delta));
	put(1, float(ground_origin.x), float(ground_origin.y), ground_spacing, 1.0f);
	put(5, r.position.x, r.position.y, r.size.x, r.size.y);
	put(8, GroundDeformField::BOW_SPEED, GroundDeformField::BOW_GAIN, GroundDeformField::BOW_REACH, 0.0f);
	put(9, GroundDeformField::CRACK_LENGTH, GroundDeformField::PLATE, float(GroundDeformField::CRACK_SECTORS), 0.0f);
	// Ageing (_age): the same numbers over p_age seconds.
	float drift = 0.0f;
	float soften = 0.0f;
	if (p_age > 0.0) {
		const WeatherMap3D *wm = WeatherMap3D::get_active();
		if (wm) {
			const Vector3 at(center.x, _ground_at(center.x, center.y), center.y);
			const float wind = wm->get_wind_at(at, 10.0f).length();
			drift = sand_drift_rate * MAX(wind - 5.0f, 0.0f) / 5.0f;
			soften = rain_smoothing * wm->get_rain_at(at);
		}
	}
	const float dry = p_age > 0.0 ? Math::exp(-float(p_age) / drying_time) : 1.0f;
	put(11, 1.0f, float(p_slow), dry, drift);
	put(12, soften, float(p_age) / 3600.0f, terrain_margin + skirt_fade, 0.0f);
	return b;
}

void GroundSurface3D::_gpu_submit(double p_delta, double p_age, bool p_readback, bool p_sync) {
	if (gpu_host.gpu.is_null()) {
		gpu_host.gpu.instantiate();
		gpu_host.reset = true;
		gpu_ground_dirty = true;
		gpu_cells_dirty = true;
	}
	if (gpu_host.texture.is_null()) {
		gpu_host.texture.instantiate();
	}
	gpu_host.time += p_delta;
	int flags = 0;
	PackedByteArray ground_bytes;
	if (gpu_ground_dirty && ground_valid) {
		ground_bytes.resize(ground_n * ground_n * 4);
		memcpy(ground_bytes.ptrw(), ground.ptr(), ground_n * ground_n * 4);
		flags |= GroundDeformGPU::STEP_GROUND;
		gpu_ground_dirty = false;
	}
	PackedByteArray cell_bytes;
	if (gpu_cells_dirty && ground_valid) {
		cell_bytes = _gpu_cells();
		if (cell_bytes.size() > 0) {
			flags |= GroundDeformGPU::STEP_COVER;
			gpu_cells_dirty = false;
		}
	}
	if (gpu_host.reset) {
		flags |= GroundDeformGPU::STEP_RESET;
	}
	if (gpu_host.shift.size() > 0 || gpu_host.stores.size() > 0) {
		flags |= GroundDeformGPU::STEP_SHIFT;
	}
	if (gpu_host.stamps.size() > 0) {
		flags |= GroundDeformGPU::STEP_STAMPS;
	}
	// Slow material (mud) moves every tenth of a second, by the seconds saved up (the CPU's tile wait).
	double slow = 0.0;
	if (gpu_host.time <= gpu_host.slump_until && p_delta > 0.0) {
		flags |= GroundDeformGPU::STEP_SLUMP;
		gpu_slow_accum += p_delta;
		if (gpu_slow_accum >= 0.1) {
			slow = gpu_slow_accum;
			gpu_slow_accum = 0.0;
		}
	} else {
		gpu_slow_accum = 0.0;
	}
	if (p_age > 0.0) {
		flags |= GroundDeformGPU::STEP_RELAX;
	}
	if (flags & (GroundDeformGPU::STEP_RESET | GroundDeformGPU::STEP_SHIFT | GroundDeformGPU::STEP_STAMPS | GroundDeformGPU::STEP_SLUMP | GroundDeformGPU::STEP_RELAX | GroundDeformGPU::STEP_COVER)) {
		flags |= GroundDeformGPU::STEP_BLIT;
		gpu_host.changed = true;
	}
	if ((p_readback && gpu_host.changed) || p_sync) {
		flags |= GroundDeformGPU::STEP_READBACK;
		gpu_host.changed = false;
	}
	if (p_sync) {
		flags |= GroundDeformGPU::STEP_SYNC;
	}
	GroundDeformGPU::Dims d;
	d.n = deform_resolution;
	d.gn = ground_n;
	d.cn = ground_n;
	d.slots = MAX(max_cached_tiles, 1);
	d.coll_w = 0;
	d.stride = 2;
	gpu_host.gpu->step(d, flags, _gpu_params(p_delta, p_age, slow), ground_bytes, cell_bytes, gpu_host.stamps, gpu_host.stores, gpu_host.shift, field.origin, Vector2());
	gpu_host.stamps.clear();
	gpu_host.stores.clear();
	gpu_host.shift.clear();
	gpu_host.reset = false;
	if (p_sync) {
		_gpu_take();
	}
}

void GroundSurface3D::_gpu_take() {
	if (gpu_host.gpu.is_null()) {
		return;
	}
	if (gpu_host.take_texture()) {
		globals_published = false;
	}
	// Slumping goes on while any texel still owes some (neighbours of a texel that moved are marked again,
	// as on the CPU).
	float owed = 0.0f;
	if (gpu_host.take_field(field, 2, &owed) && owed > 0.0f) {
		gpu_host.slump_until = MAX(gpu_host.slump_until, gpu_host.time + owed + 0.1);
	}
}

void GroundSurface3D::sync_deformation() {
	if (!_gpu_on() || !field.valid || !is_inside_tree()) {
		return;
	}
	_gpu_submit(0.0, 0.0, true, true);
	_publish();
}

// ---------------------------------------------------------------------------------------------------------
// Pressing and queries

float GroundSurface3D::press(Vector3 p_position, float p_radius, float p_bottom, float p_sink, bool p_round) {
	Columns cols;
	cols.surface = this;
	const Cell &c = _cell_at(p_position.x, p_position.z);
	field.press_settle = c.deformable ? CLAMP(3.0f / MAX(c.rate, 1e-3f), 1.0f, 600.0f) : 1.0f;
	// On the GPU path the CPU copy takes the press too, so the volume returned and queries made before the
	// next read back see it; the GPU field takes it as a stamp.
	const float moved = field.press(cols, p_position, p_radius, Vector2(), 0.0f, false, p_bottom, p_sink, p_round, contact_tolerance);
	if (_gpu_on() && field.valid) {
		gpu_sink.stamp(cols, p_position, p_radius, Vector2(), 0.0f, false, p_bottom, p_sink, p_round, contact_tolerance, Vector2());
	}
	return moved;
}

float GroundSurface3D::press_box(Vector3 p_position, Vector2 p_size, float p_yaw, float p_bottom, float p_sink) {
	Columns cols;
	cols.surface = this;
	const Cell &c = _cell_at(p_position.x, p_position.z);
	field.press_settle = c.deformable ? CLAMP(3.0f / MAX(c.rate, 1e-3f), 1.0f, 600.0f) : 1.0f;
	const float moved = field.press(cols, p_position, 0.5f * MAX(p_size.x, p_size.y), p_size * 0.5f, p_yaw, true, p_bottom, p_sink, false, contact_tolerance);
	if (_gpu_on() && field.valid) {
		gpu_sink.stamp(cols, p_position, 0.5f * MAX(p_size.x, p_size.y), p_size * 0.5f, p_yaw, true, p_bottom, p_sink, false, contact_tolerance, Vector2());
	}
	return moved;
}

float GroundSurface3D::get_sinkage_at(Vector3 p_position, float p_mass, float p_area, float p_width) const {
	Columns cols;
	cols.surface = this;
	GroundDeformField::Body b;
	b.center = p_position;
	b.mass = p_mass;
	b.area = p_area;
	b.width = p_width;
	return cols.sinkage(b);
}

void GroundSurface3D::clear_deformation() {
	field.clear();
	gpu_host.reset = true;
	gpu_host.clear_cache();
	gpu_host.stamps.clear();
	gpu_host.slump_until = -1.0;
	if (is_inside_tree()) {
		_upload();
	}
}

void GroundSurface3D::refresh_ground() {
	ground_valid = false;
	if (is_inside_tree()) {
		_follow(_follow_position());
		_publish();
	}
}

float GroundSurface3D::get_ground_height_at(Vector3 p_position) const {
	return _ground_at(p_position.x, p_position.z);
}

float GroundSurface3D::get_surface_height_at(Vector3 p_position) const {
	return _ground_at(p_position.x, p_position.z) + field.rim_at(p_position.x, p_position.z) - field.dent_at(p_position.x, p_position.z);
}

float GroundSurface3D::get_dent_at(Vector3 p_position) const {
	return field.dent_at(p_position.x, p_position.z);
}

float GroundSurface3D::get_rim_at(Vector3 p_position) const {
	return field.rim_at(p_position.x, p_position.z);
}

float GroundSurface3D::get_fresh_at(Vector3 p_position) const {
	return field.valid ? field.sample(field.fresh, p_position.x, p_position.z) : 0.0f;
}

Dictionary GroundSurface3D::get_material_at(Vector3 p_position) const {
	const Cell &c = _cell_at(p_position.x, p_position.z);
	Dictionary d;
	d["n"] = c.n;
	d["kc"] = c.kc;
	d["kphi"] = c.kphi;
	d["rim_share"] = c.rim_share;
	d["repose"] = Math::rad_to_deg(Math::atan(c.tan_repose));
	d["rate"] = c.rate;
	d["max_dent"] = c.max_dent;
	d["litter"] = c.litter;
	d["sand"] = c.sand;
	d["deformable"] = c.deformable;
	return d;
}

Vector2 GroundSurface3D::get_moved_volume() const {
	return field.moved_volume();
}

Rect2 GroundSurface3D::get_deform_rect() const {
	return field.valid ? field.rect() : Rect2();
}

PackedStringArray GroundSurface3D::get_configuration_warnings() const {
	PackedStringArray warnings = Node3D::get_configuration_warnings();
	const BaseMaterial3D *bm = Object::cast_to<BaseMaterial3D>(material.ptr());
	if (material.is_null()) {
		warnings.push_back(RTR("No material: the patch is drawn plain. Give it the terrain's material, with Ground Deform on, so the terrain is lowered under it."));
	} else if (bm && !bm->get_feature(BaseMaterial3D::FEATURE_GROUND_DEFORM)) {
		warnings.push_back(RTR("The material's Ground Deform is off: the terrain drawn with it will not be lowered and will hide the prints."));
	}
	return warnings;
}

// ---------------------------------------------------------------------------------------------------------
// Settings

#define GROUND_REBUILD           \
	ground_valid = false;        \
	field.invalidate();          \
	field.clear();               \
	mesh_dirty = true;           \
	if (is_inside_tree()) {      \
		_ensure_children();      \
		_follow(_follow_position()); \
		_upload();               \
		_publish();              \
	}

void GroundSurface3D::set_height_source(const NodePath &p_path) {
	height_source = p_path;
	ground_valid = false;
	if (is_inside_tree()) {
		_follow(_follow_position());
	}
}

void GroundSurface3D::set_ground_type(GroundType p_type) {
	ground_type = p_type;
	_update_cells();
}

void GroundSurface3D::set_moisture(float p_moisture) {
	moisture = p_moisture < 0.0f ? -1.0f : MIN(p_moisture, 1.0f);
	_update_cells();
}

void GroundSurface3D::set_litter_depth(float p_m) {
	litter_depth = CLAMP(p_m, 0.0f, 0.5f);
	_update_cells();
}

void GroundSurface3D::set_litter_cover(float p_cover) {
	litter_cover = CLAMP(p_cover, 0.0f, 1.0f);
	_update_cells();
}

void GroundSurface3D::set_rock_from_geology(bool p_enable) {
	rock_from_geology = p_enable;
	_update_cells();
}

void GroundSurface3D::set_type_map(const Ref<Image> &p_map) {
	type_map = p_map;
	if (type_map.is_valid() && type_map->is_compressed()) {
		type_map = type_map->duplicate();
		type_map->decompress();
	}
	_update_cells();
}

void GroundSurface3D::set_type_map_rect(const Rect2 &p_rect) {
	type_map_rect = p_rect;
	_update_cells();
}

void GroundSurface3D::set_deform_extent(float p_extent) {
	deform_extent = CLAMP(p_extent, 8.0f, 256.0f);
	GROUND_REBUILD
}

void GroundSurface3D::set_deform_resolution(int p_resolution) {
	const int T = GroundDeformField::TILE;
	deform_resolution = CLAMP((p_resolution + T - 1) / T * T, T * 2, 4096);
	GROUND_REBUILD
}

void GroundSurface3D::set_ground_spacing(float p_spacing) {
	ground_spacing = CLAMP(p_spacing, 0.05f, 4.0f);
	GROUND_REBUILD
}

void GroundSurface3D::set_detail_spacing(float p_spacing) {
	detail_spacing = CLAMP(p_spacing, 0.01f, 1.0f);
	mesh_dirty = true;
	if (is_inside_tree()) {
		_ensure_children();
	}
}

void GroundSurface3D::set_outer_spacing(float p_spacing) {
	outer_spacing = CLAMP(p_spacing, 0.01f, 4.0f);
	mesh_dirty = true;
	if (is_inside_tree()) {
		_ensure_children();
	}
}

void GroundSurface3D::set_material(const Ref<Material> &p_material) {
	material = p_material;
	_update_patch_material();
	if (is_inside_tree()) {
		center = Vector2(Math::INF, Math::INF); // re-apply the UV offset
		_follow(_follow_position());
	}
	update_configuration_warnings();
}

void GroundSurface3D::set_skirt(float p_m) {
	skirt = CLAMP(p_m, 0.0f, 10.0f);
	globals_published = false;
}

void GroundSurface3D::set_terrain_margin(float p_m) {
	terrain_margin = CLAMP(p_m, 0.0f, 32.0f);
	globals_published = false;
}

void GroundSurface3D::set_skirt_fade(float p_m) {
	skirt_fade = CLAMP(p_m, 0.01f, 32.0f);
	globals_published = false;
}

void GroundSurface3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_follow_target", "path"), &GroundSurface3D::set_follow_target);
	ClassDB::bind_method(D_METHOD("get_follow_target"), &GroundSurface3D::get_follow_target);
	ClassDB::bind_method(D_METHOD("set_height_source", "path"), &GroundSurface3D::set_height_source);
	ClassDB::bind_method(D_METHOD("get_height_source"), &GroundSurface3D::get_height_source);
	ClassDB::bind_method(D_METHOD("set_snow_cover", "path"), &GroundSurface3D::set_snow_cover);
	ClassDB::bind_method(D_METHOD("get_snow_cover"), &GroundSurface3D::get_snow_cover);
	ClassDB::bind_method(D_METHOD("set_ground_collision_mask", "mask"), &GroundSurface3D::set_ground_collision_mask);
	ClassDB::bind_method(D_METHOD("get_ground_collision_mask"), &GroundSurface3D::get_ground_collision_mask);
	ClassDB::bind_method(D_METHOD("set_ground_type", "type"), &GroundSurface3D::set_ground_type);
	ClassDB::bind_method(D_METHOD("get_ground_type"), &GroundSurface3D::get_ground_type);
	ClassDB::bind_method(D_METHOD("set_moisture", "moisture"), &GroundSurface3D::set_moisture);
	ClassDB::bind_method(D_METHOD("get_moisture"), &GroundSurface3D::get_moisture);
	ClassDB::bind_method(D_METHOD("set_litter_depth", "depth"), &GroundSurface3D::set_litter_depth);
	ClassDB::bind_method(D_METHOD("get_litter_depth"), &GroundSurface3D::get_litter_depth);
	ClassDB::bind_method(D_METHOD("set_litter_cover", "cover"), &GroundSurface3D::set_litter_cover);
	ClassDB::bind_method(D_METHOD("get_litter_cover"), &GroundSurface3D::get_litter_cover);
	ClassDB::bind_method(D_METHOD("set_rock_from_geology", "enable"), &GroundSurface3D::set_rock_from_geology);
	ClassDB::bind_method(D_METHOD("is_rock_from_geology"), &GroundSurface3D::is_rock_from_geology);
	ClassDB::bind_method(D_METHOD("set_type_map", "map"), &GroundSurface3D::set_type_map);
	ClassDB::bind_method(D_METHOD("get_type_map"), &GroundSurface3D::get_type_map);
	ClassDB::bind_method(D_METHOD("set_type_map_rect", "rect"), &GroundSurface3D::set_type_map_rect);
	ClassDB::bind_method(D_METHOD("get_type_map_rect"), &GroundSurface3D::get_type_map_rect);
	ClassDB::bind_method(D_METHOD("set_deform_extent", "extent"), &GroundSurface3D::set_deform_extent);
	ClassDB::bind_method(D_METHOD("get_deform_extent"), &GroundSurface3D::get_deform_extent);
	ClassDB::bind_method(D_METHOD("set_deform_resolution", "resolution"), &GroundSurface3D::set_deform_resolution);
	ClassDB::bind_method(D_METHOD("get_deform_resolution"), &GroundSurface3D::get_deform_resolution);
	ClassDB::bind_method(D_METHOD("set_ground_spacing", "spacing"), &GroundSurface3D::set_ground_spacing);
	ClassDB::bind_method(D_METHOD("get_ground_spacing"), &GroundSurface3D::get_ground_spacing);
	ClassDB::bind_method(D_METHOD("set_detail_spacing", "spacing"), &GroundSurface3D::set_detail_spacing);
	ClassDB::bind_method(D_METHOD("get_detail_spacing"), &GroundSurface3D::get_detail_spacing);
	ClassDB::bind_method(D_METHOD("set_outer_spacing", "spacing"), &GroundSurface3D::set_outer_spacing);
	ClassDB::bind_method(D_METHOD("get_outer_spacing"), &GroundSurface3D::get_outer_spacing);
	ClassDB::bind_method(D_METHOD("set_deform_groups", "groups"), &GroundSurface3D::set_deform_groups);
	ClassDB::bind_method(D_METHOD("get_deform_groups"), &GroundSurface3D::get_deform_groups);
	ClassDB::bind_method(D_METHOD("set_contact_tolerance", "distance"), &GroundSurface3D::set_contact_tolerance);
	ClassDB::bind_method(D_METHOD("get_contact_tolerance"), &GroundSurface3D::get_contact_tolerance);
	ClassDB::bind_method(D_METHOD("set_character_mass", "mass"), &GroundSurface3D::set_character_mass);
	ClassDB::bind_method(D_METHOD("get_character_mass"), &GroundSurface3D::get_character_mass);
	ClassDB::bind_method(D_METHOD("set_character_contact_area", "area"), &GroundSurface3D::set_character_contact_area);
	ClassDB::bind_method(D_METHOD("get_character_contact_area"), &GroundSurface3D::get_character_contact_area);
	ClassDB::bind_method(D_METHOD("set_max_cached_tiles", "tiles"), &GroundSurface3D::set_max_cached_tiles);
	ClassDB::bind_method(D_METHOD("get_max_cached_tiles"), &GroundSurface3D::get_max_cached_tiles);
	ClassDB::bind_method(D_METHOD("set_material", "material"), &GroundSurface3D::set_material);
	ClassDB::bind_method(D_METHOD("get_material"), &GroundSurface3D::get_material);
	ClassDB::bind_method(D_METHOD("set_skirt", "depth"), &GroundSurface3D::set_skirt);
	ClassDB::bind_method(D_METHOD("get_skirt"), &GroundSurface3D::get_skirt);
	ClassDB::bind_method(D_METHOD("set_terrain_margin", "distance"), &GroundSurface3D::set_terrain_margin);
	ClassDB::bind_method(D_METHOD("get_terrain_margin"), &GroundSurface3D::get_terrain_margin);
	ClassDB::bind_method(D_METHOD("set_skirt_fade", "distance"), &GroundSurface3D::set_skirt_fade);
	ClassDB::bind_method(D_METHOD("get_skirt_fade"), &GroundSurface3D::get_skirt_fade);
	ClassDB::bind_method(D_METHOD("set_drying_time", "seconds"), &GroundSurface3D::set_drying_time);
	ClassDB::bind_method(D_METHOD("get_drying_time"), &GroundSurface3D::get_drying_time);
	ClassDB::bind_method(D_METHOD("set_sand_drift_rate", "rate"), &GroundSurface3D::set_sand_drift_rate);
	ClassDB::bind_method(D_METHOD("get_sand_drift_rate"), &GroundSurface3D::get_sand_drift_rate);
	ClassDB::bind_method(D_METHOD("set_rain_smoothing", "rate"), &GroundSurface3D::set_rain_smoothing);
	ClassDB::bind_method(D_METHOD("get_rain_smoothing"), &GroundSurface3D::get_rain_smoothing);

	ClassDB::bind_method(D_METHOD("get_wheel_soil", "position", "load", "radius", "width", "inflation", "slip", "slip_angle"), &GroundSurface3D::get_wheel_soil, DEFVAL(200.0f), DEFVAL(0.0f), DEFVAL(0.0f));
	ClassDB::bind_method(D_METHOD("press_wheel", "position", "size", "yaw", "sinkage", "velocity"), &GroundSurface3D::press_wheel, DEFVAL(Vector2()));
	ClassDB::bind_method(D_METHOD("get_footing_at", "position", "mass", "area"), &GroundSurface3D::get_footing_at, DEFVAL(75.0f), DEFVAL(0.035f));
	ClassDB::bind_method(D_METHOD("set_ploughing", "enabled"), &GroundSurface3D::set_ploughing);
	ClassDB::bind_method(D_METHOD("is_ploughing"), &GroundSurface3D::is_ploughing);
	ClassDB::bind_method(D_METHOD("set_gpu_simulation", "enabled"), &GroundSurface3D::set_gpu_simulation);
	ClassDB::bind_method(D_METHOD("is_gpu_simulation"), &GroundSurface3D::is_gpu_simulation);
	ClassDB::bind_method(D_METHOD("is_gpu_active"), &GroundSurface3D::is_gpu_active);
	ClassDB::bind_method(D_METHOD("sync_deformation"), &GroundSurface3D::sync_deformation);
	ClassDB::bind_method(D_METHOD("press", "position", "radius", "bottom", "sink", "round"), &GroundSurface3D::press, DEFVAL(0.0f), DEFVAL(true));
	ClassDB::bind_method(D_METHOD("press_box", "position", "size", "yaw", "bottom", "sink"), &GroundSurface3D::press_box, DEFVAL(0.0f));
	ClassDB::bind_method(D_METHOD("get_sinkage_at", "position", "mass", "area", "width"), &GroundSurface3D::get_sinkage_at, DEFVAL(0.0f));
	ClassDB::bind_method(D_METHOD("settle", "seconds", "step"), &GroundSurface3D::settle, DEFVAL(1.0f / 60.0f));
	ClassDB::bind_method(D_METHOD("clear_deformation"), &GroundSurface3D::clear_deformation);
	ClassDB::bind_method(D_METHOD("refresh_ground"), &GroundSurface3D::refresh_ground);
	ClassDB::bind_method(D_METHOD("get_surface_height_at", "position"), &GroundSurface3D::get_surface_height_at);
	ClassDB::bind_method(D_METHOD("get_ground_height_at", "position"), &GroundSurface3D::get_ground_height_at);
	ClassDB::bind_method(D_METHOD("get_dent_at", "position"), &GroundSurface3D::get_dent_at);
	ClassDB::bind_method(D_METHOD("get_rim_at", "position"), &GroundSurface3D::get_rim_at);
	ClassDB::bind_method(D_METHOD("get_fresh_at", "position"), &GroundSurface3D::get_fresh_at);
	ClassDB::bind_method(D_METHOD("get_material_at", "position"), &GroundSurface3D::get_material_at);
	ClassDB::bind_method(D_METHOD("get_moved_volume"), &GroundSurface3D::get_moved_volume);
	ClassDB::bind_method(D_METHOD("get_deform_rect"), &GroundSurface3D::get_deform_rect);
	ClassDB::bind_method(D_METHOD("get_cached_tile_count"), &GroundSurface3D::get_cached_tile_count);
	ClassDB::bind_method(D_METHOD("get_step_time_msec"), &GroundSurface3D::get_step_time_msec);
	ClassDB::bind_method(D_METHOD("get_patch_material"), &GroundSurface3D::get_patch_material);

	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "follow_target", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D"), "set_follow_target", "get_follow_target");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "height_source"), "set_height_source", "get_height_source");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "snow_cover"), "set_snow_cover", "get_snow_cover");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "ground_collision_mask", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_ground_collision_mask", "get_ground_collision_mask");
	ADD_GROUP("Material", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "ground_type", PROPERTY_HINT_ENUM, "Sand,Soil"), "set_ground_type", "get_ground_type");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "moisture", PROPERTY_HINT_RANGE, "-1,1,0.01"), "set_moisture", "get_moisture");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "litter_depth", PROPERTY_HINT_RANGE, "0,0.5,0.005,suffix:m"), "set_litter_depth", "get_litter_depth");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "litter_cover", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_litter_cover", "get_litter_cover");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "rock_from_geology"), "set_rock_from_geology", "is_rock_from_geology");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "type_map", PROPERTY_HINT_RESOURCE_TYPE, "Image"), "set_type_map", "get_type_map");
	ADD_PROPERTY(PropertyInfo(Variant::RECT2, "type_map_rect", PROPERTY_HINT_NONE, "suffix:m"), "set_type_map_rect", "get_type_map_rect");
	ADD_GROUP("Deformation", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "deform_extent", PROPERTY_HINT_RANGE, "8,256,1,suffix:m"), "set_deform_extent", "get_deform_extent");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "deform_resolution", PROPERTY_HINT_RANGE, "128,4096,64"), "set_deform_resolution", "get_deform_resolution");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ground_spacing", PROPERTY_HINT_RANGE, "0.05,4,0.05,suffix:m"), "set_ground_spacing", "get_ground_spacing");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_STRING_ARRAY, "deform_groups"), "set_deform_groups", "get_deform_groups");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "ploughing"), "set_ploughing", "is_ploughing");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "contact_tolerance", PROPERTY_HINT_RANGE, "0,0.2,0.001,suffix:m"), "set_contact_tolerance", "get_contact_tolerance");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "character_mass", PROPERTY_HINT_RANGE, "0,500,0.5,suffix:kg"), "set_character_mass", "get_character_mass");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "character_contact_area", PROPERTY_HINT_RANGE, "0.001,1,0.001,suffix:m²"), "set_character_contact_area", "get_character_contact_area");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_cached_tiles", PROPERTY_HINT_RANGE, "0,65536,1"), "set_max_cached_tiles", "get_max_cached_tiles");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "gpu_simulation"), "set_gpu_simulation", "is_gpu_simulation");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "drying_time", PROPERTY_HINT_RANGE, "1,86400,1,suffix:s"), "set_drying_time", "get_drying_time");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "sand_drift_rate", PROPERTY_HINT_RANGE, "0,100,0.1"), "set_sand_drift_rate", "get_sand_drift_rate");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "rain_smoothing", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_rain_smoothing", "get_rain_smoothing");
	ADD_GROUP("Drawing", "");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "material", PROPERTY_HINT_RESOURCE_TYPE, "BaseMaterial3D,ShaderMaterial"), "set_material", "get_material");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "detail_spacing", PROPERTY_HINT_RANGE, "0.01,1,0.005,suffix:m"), "set_detail_spacing", "get_detail_spacing");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "outer_spacing", PROPERTY_HINT_RANGE, "0.01,4,0.005,suffix:m"), "set_outer_spacing", "get_outer_spacing");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "skirt", PROPERTY_HINT_RANGE, "0,10,0.01,suffix:m"), "set_skirt", "get_skirt");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "terrain_margin", PROPERTY_HINT_RANGE, "0,32,0.1,suffix:m"), "set_terrain_margin", "get_terrain_margin");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "skirt_fade", PROPERTY_HINT_RANGE, "0.01,32,0.1,suffix:m"), "set_skirt_fade", "get_skirt_fade");

	BIND_ENUM_CONSTANT(GROUND_SAND);
	BIND_ENUM_CONSTANT(GROUND_SOIL);
}

GroundSurface3D::GroundSurface3D() {
	gpu_sink.surface = this;
	deform_groups.push_back("ground_deform");
	deform_groups.push_back("snow_deform");
	deform_groups.push_back("grass_trample");
}

GroundSurface3D::~GroundSurface3D() {
	if (active == this) {
		active = nullptr;
	}
}

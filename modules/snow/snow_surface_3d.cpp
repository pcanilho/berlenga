/**************************************************************************/
/*  snow_surface_3d.cpp                                                       */
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



#include "snow_surface_3d.h"

#include "snow_cover_3d.h"

#include "core/config/engine.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/templates/hash_set.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/physics/character_body_3d.h"
#include "scene/3d/physics/collision_shape_3d.h"
#include "scene/3d/physics/rigid_body_3d.h"
#include "scene/3d/physics/static_body_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "scene/resources/3d/box_shape_3d.h"
#include "scene/resources/3d/capsule_shape_3d.h"
#include "scene/resources/3d/cylinder_shape_3d.h"
#include "scene/resources/3d/height_map_shape_3d.h"
#include "scene/resources/3d/sphere_shape_3d.h"
#include "scene/resources/mesh.h"
#include "scene/resources/material.h"

// The surface shader: the same height function in vertex() (geometry, traced from the capture)
// and fragment() (a finer normal from the deformation texels, plus a few millimetres of wind
// ripple), so trails keep their edges at any mesh density. Snow optics as BaseMaterial3D's snow
// (ice F0 0.018, roughness 0.7 dry to 0.35 wet), compacted snow a little darker and smoother.
// Loose, disturbed snow's angle of repose: about 45 degrees for dry snow pushed aside (Abe 2004 measured
// heaps of disaggregated snow layers; debris and plough berms stand at 35 to 50 degrees).
static constexpr float LOOSE_SNOW_TAN_REPOSE = 1.0f;

static const char *SNOW_SURFACE_SHADER = R"(
shader_type spatial;
render_mode cull_back, depth_draw_opaque;

uniform sampler2D ground_tex : filter_linear, repeat_disable;
uniform vec4 ground_rect;
uniform sampler2D snow_tex : filter_linear, repeat_disable;
uniform vec4 snow_rect;
uniform sampler2D catch_tex : filter_linear, repeat_disable;
uniform bool has_catch = false;
uniform sampler2D deform_tex : filter_linear, repeat_disable;
uniform vec4 deform_rect;
uniform vec4 mesh_rect;
uniform float min_depth = 0.03;
uniform float edge_fade = 8.0;
uniform float detail_step = 0.0625;
uniform float vertex_step = 0.0625;
uniform float coarse_step = 0.5;
uniform float micro_relief = 1.0;
uniform vec3 tint = vec3(1.0);
global uniform bool snow_exact_glints;

varying vec3 w_pos;
varying vec3 w_n;
varying vec3 w_t;
varying vec3 w_b;

bool snow_inside(vec2 uv) {
	return uv.x >= 0.0 && uv.y >= 0.0 && uv.x <= 1.0 && uv.y <= 1.0;
}
float snow_ground(vec2 p) {
	return textureLod(ground_tex, (p - ground_rect.xy) / ground_rect.zw, 0.0).r;
}
vec4 snow_pack(vec2 p) {
	vec2 uv = (p - snow_rect.xy) / max(snow_rect.zw, vec2(1e-4));
	if (!snow_inside(uv)) {
		return vec4(0.0, 0.84, 0.0, 0.1);
	}
	vec4 v = textureLod(snow_tex, uv, 0.0);
	// Under a roof the pack lies on the roof (the cover's catch map): none on the ground here.
	if (has_catch && textureLod(catch_tex, uv, 0.0).r > snow_ground(p) + 0.5) {
		v.r = 0.0;
	}
	return v;
}
vec2 snow_deform(vec2 p) {
	if (deform_rect.z <= 0.0) {
		return vec2(0.0);
	}
	vec2 uv = (p - deform_rect.xy) / deform_rect.zw;
	return snow_inside(uv) ? textureLod(deform_tex, uv, 0.0).rg : vec2(0.0);
}
float snow_fade(vec2 p) {
	vec2 d = min(p - mesh_rect.xy, mesh_rect.xy + mesh_rect.zw - p);
	return smoothstep(0.0, edge_fade, min(d.x, d.y));
}
// The deformation averaged over a vertex's spacing where the triangles are coarser than the texels (a
// displacement prefiltered to the mesh's sampling rate, as geometry clipmaps filter each level): without
// it, vertices land on and off a print's wall and a turned edge zigzags. The fragment's normal keeps the
// texels' own detail.
vec2 snow_deform_vertex(vec2 p) {
	float r = 0.5 * max(vertex_step - detail_step, 0.0);
	if (r <= 0.0) {
		return snow_deform(p);
	}
	return 0.25 * (snow_deform(p + vec2(-r, -r)) + snow_deform(p + vec2(r, -r)) + snow_deform(p + vec2(-r, r)) + snow_deform(p + vec2(r, r)));
}
float snow_height_with(vec2 p, vec2 dr) {
	float d = snow_pack(p).r * snow_fade(p) - dr.x + dr.y;
	// Under min_depth the snow is the ground material's; the surface tucks 5 cm under the ground.
	float lift = d >= min_depth ? d : mix(-0.05, min_depth, max(d, 0.0) / min_depth);
	return snow_ground(p) + lift;
}
float snow_height(vec2 p) {
	return snow_height_with(p, snow_deform(p));
}
float snow_height_vertex(vec2 p) {
	return snow_height_with(p, snow_deform_vertex(p));
}
float snow_hash(vec2 p) {
	return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}
float snow_noise(vec2 p) {
	vec2 i = floor(p);
	vec2 f = fract(p);
	f = f * f * (3.0 - 2.0 * f);
	return mix(mix(snow_hash(i), snow_hash(i + vec2(1.0, 0.0)), f.x), mix(snow_hash(i + vec2(0.0, 1.0)), snow_hash(i + vec2(1.0, 1.0)), f.x), f.y);
}
// Snow sparkle (docs/ideas/snow.md, stage 5): the same function as BaseMaterial3D's snow pass, there with
// its comments. A facet normal and its weight (0: none) for a footprint in metres.
vec4 snow_glint(vec3 p_pos, vec3 p_n, float p_footprint) {
	const float grain = 0.002;
	const float share = 0.25;
	float level = floor(max(log2(max(p_footprint, grain) / grain), 0.0));
	if (level > 5.0) {
		return vec4(p_n, 0.0);
	}
	float cell = grain * exp2(level);
	ivec3 q = ivec3(floor(p_pos / cell));
	uint h = (uint(q.x) * 73856093u) ^ (uint(q.y) * 19349663u) ^ (uint(q.z) * 83492791u) ^ (uint(level) * 2654435761u);
	h ^= h >> 16u;
	h *= 0x7feb352du;
	h ^= h >> 15u;
	h *= 0x846ca68bu;
	h ^= h >> 16u;
	float r0 = float(h & 0x3ffu) / 1023.0;
	float r1 = float((h >> 10u) & 0x3ffu) / 1024.0;
	float r2 = float((h >> 20u) & 0x3ffu) / 1023.0;
	float grains = exp2(2.0 * level);
	float chance = min(share * grains, 1.0);
	if (r0 >= chance) {
		return vec4(p_n, 0.0);
	}
	float tan2 = 0.16 * r1 / max(1.0 - r1, 1e-3);
	float ct = inversesqrt(1.0 + tan2);
	float st = sqrt(max(1.0 - ct * ct, 0.0));
	vec3 t = normalize(cross(p_n, abs(p_n.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
	vec3 b = cross(p_n, t);
	float phi = 6.2831853 * r2;
	vec3 f = normalize(p_n * ct + (t * cos(phi) + b * sin(phi)) * st);
	return vec4(f, share / chance / sqrt(max(share * grains, 1.0)));
}

// Exact sparkle under the path tracer (docs/ideas/snow.md, follow-up 5): the facet of the grain at a point,
// the same function as BaseMaterial3D's snow pass and the path tracer's, there with its comments.
vec3 snow_grain_facet(vec3 p_pos, vec3 p_n) {
	const float grain = 0.002;
	ivec3 q = ivec3(floor(p_pos / grain));
	uint h = (uint(q.x) * 73856093u) ^ (uint(q.y) * 19349663u) ^ (uint(q.z) * 83492791u);
	h ^= h >> 16u;
	h *= 0x7feb352du;
	h ^= h >> 15u;
	h *= 0x846ca68bu;
	h ^= h >> 16u;
	float r1 = float(h & 0xffffu) / 65536.0;
	float r2 = float(h >> 16u) / 65536.0;
	float tan2 = 0.16 * r1 / max(1.0 - r1, 1e-4);
	float ct = inversesqrt(1.0 + tan2);
	float st = sqrt(max(1.0 - ct * ct, 0.0));
	vec3 t = normalize(cross(p_n, abs(p_n.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
	vec3 b = cross(p_n, t);
	float phi = 6.2831853 * r2;
	return normalize(p_n * ct + (t * cos(phi) + b * sin(phi)) * st);
}

float snow_ripple(vec2 p) {
	// Wind ripples and grain clumps: 4 mm at 35 cm, 1.5 mm at 9 cm.
	return micro_relief * (0.004 * snow_noise(p / 0.35) + 0.0015 * snow_noise(p / 0.09 + 17.0));
}

void vertex() {
	vec2 p = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xz;
	vec2 duv = (p - deform_rect.xy) / max(deform_rect.zw, vec2(1e-4));
	float e = (deform_rect.z > 0.0 && snow_inside(duv)) ? max(detail_step * 2.0, vertex_step) : coarse_step;
	float h = snow_height_vertex(p);
	float hx = snow_height_vertex(p + vec2(e, 0.0)) - snow_height_vertex(p - vec2(e, 0.0));
	float hz = snow_height_vertex(p + vec2(0.0, e)) - snow_height_vertex(p - vec2(0.0, e));
	vec3 n = normalize(vec3(-hx, 2.0 * e, -hz));
	vec3 t = normalize(vec3(2.0 * e, hx, 0.0));
	vec3 b = normalize(cross(n, t));
	VERTEX.y = h - MODEL_MATRIX[3].y;
	NORMAL = n;
	TANGENT = t;
	BINORMAL = b;
	w_pos = vec3(p.x, h, p.y);
	w_n = n;
	w_t = t;
	w_b = b;
}

void fragment() {
	vec2 p = w_pos.xz;
	vec4 pack = snow_pack(p);
	vec2 dr = snow_deform(p);
	float compact = clamp(dr.x / max(pack.r, 0.01), 0.0, 1.0);
	float e = detail_step;
	float hx = snow_height(p + vec2(e, 0.0)) - snow_height(p - vec2(e, 0.0));
	float hz = snow_height(p + vec2(0.0, e)) - snow_height(p - vec2(0.0, e));
	float em = 0.02;
	hx += (snow_ripple(p + vec2(em, 0.0)) - snow_ripple(p - vec2(em, 0.0))) * e / em;
	hz += (snow_ripple(p + vec2(0.0, em)) - snow_ripple(p - vec2(0.0, em))) * e / em;
	vec3 nf = normalize(vec3(-hx, 2.0 * e, -hz));
	vec3 tn = vec3(dot(nf, normalize(w_t)), dot(nf, normalize(w_b)), dot(nf, normalize(w_n)));
	ALBEDO = tint * pack.g * (1.0 - 0.08 * compact);
	ROUGHNESS = clamp(mix(0.7, 0.35, pack.b) - 0.15 * compact, 0.05, 1.0);
	SPECULAR = 0.335; // ice, F0 0.018
	METALLIC = 0.0;
	NORMAL_MAP = tn * 0.5 + 0.5;
	// Sparkle on dry snow: the facet is the coat's near-mirror lobe on its own normal (NORMAL before
	// NORMAL_MAP), weighted by the share of the pixel it covers, with NORMAL_MAP solved to land back on the
	// snow's normal (as BaseMaterial3D's snow pass). Packed snow keeps fewer whole grains; melt water smooths
	// them away.
	float pix = 2.0 / (abs(PROJECTION_MATRIX[1][1]) * VIEWPORT_SIZE.y);
	float fp = PROJECTION_MATRIX[3][3] != 0.0 ? pix : distance(w_pos, INV_VIEW_MATRIX[3].xyz) * pix;
	if (IN_PATH_TRACER && snow_exact_glints) {
		// Under the path tracer: the ice specular lies on the facet of the grain hit, for every grain (the
		// samples of a pixel integrate them), at the facet's own roughness; the diffuse keeps the snow's
		// normal. Trodden snow keeps fewer whole grains, the rest stays rough.
		float ew = (1.0 - smoothstep(0.05, 0.3, pack.b)) * (1.0 - 0.7 * compact);
		if (ew > 0.0) {
			SPECULAR_NORMAL = normalize((VIEW_MATRIX * vec4(snow_grain_facet(w_pos, nf), 0.0)).xyz);
			ROUGHNESS = mix(ROUGHNESS, 0.22, ew);
		}
	} else {
		vec4 g = snow_glint(w_pos, nf, fp);
		float gw = g.w * (1.0 - smoothstep(0.05, 0.3, pack.b)) * (1.0 - 0.7 * compact);
		vec3 f = normalize((VIEW_MATRIX * vec4(g.xyz, 0.0)).xyz);
		mat3 tbf = mat3(TANGENT, BINORMAL, f);
		if (gw > 0.0 && abs(determinant(tbf)) > 1e-3) {
			vec3 k = inverse(tbf) * normalize((VIEW_MATRIX * vec4(nf, 0.0)).xyz);
			if (k.z > 0.0) {
				k /= length(k);
				NORMAL = f;
				NORMAL_MAP = vec3(k.xy * 0.5 + 0.5, 1.0);
				CLEARCOAT = 0.45 * gw;
				CLEARCOAT_ROUGHNESS = 0.5;
			}
		}
	}
}
)";

// ---------------------------------------------------------------------------------------------------------
// Relations

float SnowSurface3D::bekker_sinkage(float p_pressure_kpa, float p_width, float p_n, float p_kc, float p_kphi) {
	return GroundDeformField::bekker_sinkage(p_pressure_kpa, p_width, p_n, p_kc, p_kphi);
}

float SnowSurface3D::_dent_max(float p_depth, float p_density) const {
	// A column of depth D at density rho lowered by x keeps (1 - f) of the lost mass as compaction:
	// D - x >= (D rho - f x rho) / rho_c, so x <= D (1 - rho / rho_c) / (1 - f rho / rho_c).
	const float r = p_density / compacted_density;
	if (r >= 1.0f || p_depth <= 0.0f) {
		return 0.0f;
	}
	return p_depth * (1.0f - r) / MAX(1.0f - rim_share * r, 1e-3f);
}

// ---------------------------------------------------------------------------------------------------------
// Setup

SnowCover3D *SnowSurface3D::_cover() const {
	if (!snow_cover.is_empty() && is_inside_tree()) {
		SnowCover3D *c = Object::cast_to<SnowCover3D>(get_node_or_null(snow_cover));
		if (c) {
			return c;
		}
	}
	return SnowCover3D::get_active();
}

Vector3 SnowSurface3D::_follow_position() const {
	if (!follow_target.is_empty()) {
		const Node3D *n = Object::cast_to<Node3D>(get_node_or_null(follow_target));
		if (n && n->is_inside_tree()) {
			return n->get_global_position();
		}
	}
	if (is_inside_tree() && get_viewport()) {
		const Camera3D *cam = get_viewport()->get_camera_3d();
		if (cam) {
			return cam->get_global_position();
		}
	}
	return is_inside_tree() ? get_global_position() : Vector3();
}

void SnowSurface3D::_build_mesh() {
	// A tensor grid: detail_spacing over the deformation field, then spacing growing by a fifth per
	// step up to max_spacing. No T-junctions, so no cracks.
	LocalVector<float> half;
	const float dense = deform_extent * 0.5f;
	const float outer = MAX(extent * 0.5f, dense);
	for (float x = detail_spacing; x < dense - 1e-4f; x += detail_spacing) {
		half.push_back(x);
	}
	half.push_back(dense);
	float s = detail_spacing;
	float x = dense;
	while (x < outer - 1e-3f) {
		s = MIN(s * 1.2f, max_spacing);
		x = MIN(x + s, outer);
		half.push_back(x);
	}
	LocalVector<float> c;
	for (int i = int(half.size()) - 1; i >= 0; i--) {
		c.push_back(-half[i]);
	}
	c.push_back(0.0f);
	for (float h : half) {
		c.push_back(h);
	}
	const int n = c.size();
	PackedVector3Array verts;
	PackedVector3Array normals;
	PackedFloat32Array tangents;
	verts.resize(n * n);
	normals.resize(n * n);
	tangents.resize(n * n * 4);
	for (int z = 0; z < n; z++) {
		for (int xi = 0; xi < n; xi++) {
			const int i = z * n + xi;
			verts.set(i, Vector3(c[xi], 0.0f, c[z]));
			normals.set(i, Vector3(0, 1, 0));
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
	arrays[Mesh::ARRAY_INDEX] = idx;
	mesh.instantiate();
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	mesh->surface_set_material(0, material);
	if (mesh_instance) {
		mesh_instance->set_mesh(mesh);
	}
	mesh_dirty = false;
}

void SnowSurface3D::_ensure_children() {
	if (!mesh_instance) {
		mesh_instance = memnew(MeshInstance3D);
		mesh_instance->set_as_top_level(true);
		add_child(mesh_instance, false, INTERNAL_MODE_FRONT);
	}
	if (mesh_dirty) {
		_build_mesh();
	}
	if (collision_enabled && !body) {
		body = memnew(StaticBody3D);
		body->set_as_top_level(true);
		body->set_collision_layer(collision_layer);
		body->set_collision_mask(collision_mask);
		collision_node = memnew(CollisionShape3D);
		collision_shape.instantiate();
		collision_node->set_shape(collision_shape);
		body->add_child(collision_node);
		add_child(body, false, INTERNAL_MODE_FRONT);
		collision_dirty = true;
	} else if (!collision_enabled && body) {
		body->queue_free();
		body = nullptr;
		collision_node = nullptr;
		collision_shape.unref();
	}
}

// ---------------------------------------------------------------------------------------------------------
// Following

void SnowSurface3D::_follow(Vector3 p_center) {
	field.extent = deform_extent;
	field.resolution = deform_resolution;
	field.max_cached_tiles = max_cached_tiles;
	const Vector2i want_deform = field.origin_for(Vector2(p_center.x, p_center.z));
	const int gh = int(Math::round(extent * 0.5f / ground_spacing));
	const Vector2 center = field.center_for(Vector2(p_center.x, p_center.z));
	const Vector2i want_ground(int(Math::round(center.x / ground_spacing)) - gh, int(Math::round(center.y / ground_spacing)) - gh);
	if (!ground_valid || want_ground != ground_origin || ground_n != 2 * gh + 1) {
		_shift_ground(want_ground);
	}
	if (deform_enabled && (!field.valid || want_deform != field.origin)) {
		_shift_deform(want_deform);
	}
	if (center != mesh_center || !mesh_instance->is_inside_tree() || mesh_instance->get_global_position() != Vector3(center.x, 0, center.y)) {
		mesh_center = center;
		mesh_instance->set_global_transform(Transform3D(Basis(), Vector3(center.x, 0.0f, center.y)));
		if (body) {
			body->set_global_transform(Transform3D(Basis(), Vector3(center.x, 0.0f, center.y)));
			collision_dirty = true;
		}
		_update_material();
	}
}

void SnowSurface3D::_shift_ground(Vector2i p_origin) {
	const int gh = int(Math::round(extent * 0.5f / ground_spacing));
	const int n = 2 * gh + 1;
	LocalVector<float> next;
	next.resize(n * n);
	LocalVector<uint8_t> missing;
	missing.resize(n * n);
	const bool keep = ground_valid && ground_n == n;
	const Vector2i d = p_origin - ground_origin;
	for (int z = 0; z < n; z++) {
		for (int x = 0; x < n; x++) {
			const int sx = x + d.x;
			const int sz = z + d.y;
			if (keep && sx >= 0 && sz >= 0 && sx < n && sz < n) {
				next[z * n + x] = ground[sz * n + sx];
				missing[z * n + x] = 0;
			} else {
				missing[z * n + x] = 1;
			}
		}
	}
	const SnowCover3D *cover = _cover();
	LocalVector<float> row;
	row.resize(n);
	for (int z = 0; z < n; z++) {
		int x = 0;
		while (x < n) {
			if (!missing[z * n + x]) {
				x++;
				continue;
			}
			int e = x;
			while (e < n && missing[z * n + e]) {
				e++;
			}
			const Vector2 o((p_origin.x + x) * ground_spacing, (p_origin.y + z) * ground_spacing);
			if (cover) {
				cover->sample_ground_heights(o, ground_spacing, e - x, 1, row.ptr());
			}
			for (int i = x; i < e; i++) {
				float h = cover ? row[i - x] : 0.0f;
				if (!Math::is_finite(h)) {
					h = cover ? cover->get_ground_height_at(Vector3((p_origin.x + i) * ground_spacing, 0, (p_origin.y + z) * ground_spacing)) : 0.0f;
				}
				next[z * n + i] = h;
			}
			x = e;
		}
	}
	ground = next;
	ground_n = n;
	ground_origin = p_origin;
	ground_valid = true;
	ground_min = 1e20f;
	ground_max = -1e20f;
	for (float h : ground) {
		ground_min = MIN(ground_min, h);
		ground_max = MAX(ground_max, h);
	}
	collision_dirty = true;
	_upload_ground();
}

void SnowSurface3D::_shift_deform(Vector2i p_origin) {
	field.shift(p_origin);
	collision_dirty = true;
	_update_material();
}

// ---------------------------------------------------------------------------------------------------------
// Heights

float SnowSurface3D::_ground_at(float p_x, float p_z) const {
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

float SnowSurface3D::_surface_at(float p_x, float p_z, const SnowCover3D *p_cover) const {
	const float g = _ground_at(p_x, p_z);
	if (!p_cover) {
		return g;
	}
	const float half = extent * 0.5f;
	const float edge = MIN(MIN(p_x - (mesh_center.x - half), mesh_center.x + half - p_x), MIN(p_z - (mesh_center.y - half), mesh_center.y + half - p_z));
	const float fade = edge_fade > 0.0f ? Math::smoothstep(0.0f, edge_fade, edge) : (edge >= 0.0f ? 1.0f : 0.0f);
	// Under a roof the pack lies on the roof (the cover's ground there is the roof): none on the ground.
	float d = p_cover->get_ground_height_at(Vector3(p_x, 0, p_z)) > g + 0.5f ? 0.0f : p_cover->get_depth_at(Vector3(p_x, 0, p_z)) * fade;
	if (deform_enabled && field.valid) {
		d += field.rim_at(p_x, p_z) - field.dent_at(p_x, p_z);
	}
	return d >= min_depth ? g + d : g;
}

// ---------------------------------------------------------------------------------------------------------
// Deformation

float SnowSurface3D::press(Vector3 p_position, float p_radius, float p_bottom, float p_sink, bool p_round) {
	return _press(p_position, p_radius, Vector2(), 0.0f, false, p_bottom, p_sink, p_round);
}

float SnowSurface3D::press_box(Vector3 p_position, Vector2 p_size, float p_yaw, float p_bottom, float p_sink) {
	return _press(p_position, 0.5f * MAX(p_size.x, p_size.y), p_size * 0.5f, p_yaw, true, p_bottom, p_sink, false);
}

GroundDeformField::Column SnowSurface3D::SnowColumns::column(float p_x, float p_z) const {
	GroundDeformField::Column c;
	const Vector3 wp(p_x, 0, p_z);
	const float depth = cover->get_depth_at(wp);
	if (depth < surface->min_depth) {
		return c;
	}
	c.deformable = true;
	c.surface = surface->_ground_at(p_x, p_z) + depth;
	const float density = cover->get_density_at(wp);
	c.max_dent = surface->_dent_max(depth, density);
	c.rim_share = surface->rim_share;
	if (held || broke) {
		const float thickness = cover->get_crust_thickness_at(wp);
		const float over = cover->get_crust_depth_at(wp);
		if (thickness > 0.0f && over < depth) {
			if (held) {
				// Standing on the crust: only the snow over it compacts.
				c.max_dent = MIN(c.max_dent, surface->_dent_max(over, density));
			} else if (over < 0.05f) {
				c.crust = thickness;
			}
		}
	}
	return c;
}

float SnowSurface3D::SnowColumns::sinkage(const GroundDeformField::Body &p_body) const {
	held = false;
	broke = false;
	const float z = GroundDeformField::body_sinkage(p_body, surface->bekker_n, surface->bekker_kc, surface->bekker_kphi);
	const Vector3 wp(p_body.center.x, 0, p_body.center.z);
	const float strength = cover->get_crust_strength_at(wp);
	if (strength <= 0.0f) {
		return z;
	}
	if (p_body.mass <= 0.0f) {
		broke = true; // driven through at a set depth: it breaks whatever is there
		return z;
	}
	const float over = cover->get_crust_depth_at(wp);
	const float depth = cover->get_depth_at(wp);
	if (over >= depth) {
		return z;
	}
	// Already broken here (the hole goes past the crust): it goes on breaking at the hole's edge.
	const float above = surface->_dent_max(over, cover->get_density_at(wp));
	if (surface->field.dent_at(wp.x, wp.z) > above + 0.5f * cover->get_crust_thickness_at(wp) + 0.005f) {
		broke = true;
		return z;
	}
	if (GroundDeformField::body_pressure(p_body, MIN(z, MAX(above, 0.005f))) < strength) {
		held = true;
		return MIN(z, above);
	}
	broke = true;
	return z;
}

float SnowSurface3D::_press(Vector3 p_position, float p_radius, Vector2 p_half, float p_yaw, bool p_box, float p_bottom, float p_sink, bool p_round) {
	SnowCover3D *cover = _cover();
	if (!deform_enabled || !cover) {
		return 0.0f;
	}
	SnowColumns cols;
	cols.surface = this;
	cols.cover = cover;
	const float moved = field.press(cols, p_position, p_radius, p_half, p_yaw, p_box, p_bottom, p_sink, p_round, contact_tolerance);
	if (moved > 0.0f) {
		collision_dirty = true;
	}
	return moved;
}

void SnowSurface3D::_press_movers(double p_delta) {
	SnowCover3D *cover = _cover();
	if (!is_inside_tree() || !cover) {
		return;
	}
	SnowColumns cols;
	cols.surface = this;
	cols.cover = cover;
	field.press_bodies(get_tree(), deform_groups, cols, character_mass, character_contact_area, contact_tolerance, p_delta);
	if (field.shape_dirty) {
		collision_dirty = true;
	}
}

void SnowSurface3D::_relax() {
	SnowCover3D *cover = _cover();
	if (!cover || !field.valid || cover->get_update_count() == seen_cover_update) {
		return;
	}
	seen_cover_update = cover->get_update_count();
	const int n = field.resolution;
	const int tiles = n / TILE;
	const float t = field.texel();
	LocalVector<float> &dent = field.dent;
	LocalVector<float> &rim = field.rim;
	LocalVector<uint8_t> &tile_active = field.tile_active;
	const Vector2i deform_origin = field.origin;
	const float keep = Math::exp(-wind_fill_rate * cover->get_drift_factor() * cover->get_last_step_hours());
	for (int tz = 0; tz < tiles; tz++) {
		for (int tx = 0; tx < tiles; tx++) {
			if (!tile_active[tz * tiles + tx]) {
				continue;
			}
			bool any = false;
			for (int z = tz * TILE; z < (tz + 1) * TILE; z++) {
				for (int x = tx * TILE; x < (tx + 1) * TILE; x++) {
					const int i = z * n + x;
					float dn = dent[i];
					float rm = rim[i];
					if (dn <= 0.0f && rm <= 0.0f) {
						continue;
					}
					const Vector3 wp((deform_origin.x + x + 0.5f) * t, 0, (deform_origin.y + z + 0.5f) * t);
					const float depth = cover->get_depth_at(wp);
					// New snow slides into hollows; wind drifts over the trail and planes the rim.
					dn = MAX(dn - cover->get_new_snow_at(wp) * refill_share, 0.0f) * keep;
					rm *= keep;
					// The pack around settles toward the compacted density: the step shrinks.
					dn = MIN(dn, _dent_max(depth, cover->get_density_at(wp)));
					rm = MIN(rm, depth);
					dent[i] = dn > 1e-4f ? dn : 0.0f;
					rim[i] = rm > 1e-4f ? rm : 0.0f;
					any = any || dent[i] > 0.0f || rim[i] > 0.0f;
				}
			}
			tile_active[tz * tiles + tx] = any ? 1 : 0;
		}
	}
	field.dirty = true;
	collision_dirty = true;
}

void SnowSurface3D::clear_deformation() {
	field.clear();
	collision_dirty = true;
}

void SnowSurface3D::refresh_ground() {
	ground_valid = false;
	if (is_inside_tree()) {
		_follow(_follow_position());
	}
}

// ---------------------------------------------------------------------------------------------------------
// GPU and physics

void SnowSurface3D::_upload_ground() {
	const int n = ground_n;
	Vector<uint8_t> bytes;
	bytes.resize(n * n * 4);
	memcpy(bytes.ptrw(), ground.ptr(), n * n * 4);
	if (ground_image.is_null() || ground_image->get_width() != n) {
		ground_image = Image::create_from_data(n, n, false, Image::FORMAT_RF, bytes);
		ground_texture = ImageTexture::create_from_image(ground_image);
	} else {
		ground_image->set_data(n, n, false, Image::FORMAT_RF, bytes);
		ground_texture->update(ground_image);
	}
	if (mesh_instance) {
		const float half = extent * 0.5f;
		mesh_instance->set_custom_aabb(AABB(Vector3(-half, ground_min - 1.0f, -half), Vector3(extent, ground_max - ground_min + 12.0f, extent)));
	}
	_update_material();
}

void SnowSurface3D::_upload_deform() {
	const RID before = deform_texture.is_valid() ? deform_texture->get_rid() : RID();
	field.upload(deform_image, deform_texture);
	if (deform_texture.is_valid() && deform_texture->get_rid() != before) {
		_update_material();
	}
}

void SnowSurface3D::_update_material() {
	if (material.is_null()) {
		return;
	}
	const SnowCover3D *cover = _cover();
	material->set_shader_parameter("ground_tex", ground_texture);
	material->set_shader_parameter("ground_rect", Vector4((ground_origin.x - 0.5f) * ground_spacing, (ground_origin.y - 0.5f) * ground_spacing, ground_n * ground_spacing, ground_n * ground_spacing));
	if (cover && cover->get_snow_texture().is_valid()) {
		const Rect2 r = cover->get_map_rect();
		material->set_shader_parameter("snow_tex", cover->get_snow_texture());
		material->set_shader_parameter("snow_rect", Vector4(r.position.x, r.position.y, r.size.x, r.size.y));
		material->set_shader_parameter("catch_tex", cover->get_catch_texture());
		material->set_shader_parameter("has_catch", cover->get_catch_texture().is_valid());
	} else {
		material->set_shader_parameter("snow_rect", Vector4());
	}
	if (deform_enabled && field.valid && deform_texture.is_valid()) {
		const Rect2 r = field.rect();
		material->set_shader_parameter("deform_tex", deform_texture);
		material->set_shader_parameter("deform_rect", Vector4(r.position.x, r.position.y, r.size.x, r.size.y));
	} else {
		material->set_shader_parameter("deform_rect", Vector4());
	}
	const float half = extent * 0.5f;
	material->set_shader_parameter("mesh_rect", Vector4(mesh_center.x - half, mesh_center.y - half, extent, extent));
	material->set_shader_parameter("min_depth", min_depth);
	material->set_shader_parameter("edge_fade", edge_fade);
	material->set_shader_parameter("detail_step", _texel());
	material->set_shader_parameter("vertex_step", detail_spacing);
	material->set_shader_parameter("coarse_step", MAX(ground_spacing, max_spacing * 0.5f));
	material->set_shader_parameter("micro_relief", micro_relief);
	material->set_shader_parameter("tint", Vector3(tint.r, tint.g, tint.b));
}

void SnowSurface3D::_update_collision() {
	if (!body || collision_shape.is_null() || !collision_dirty) {
		return;
	}
	const SnowCover3D *cover = _cover();
	const float sp = _texel() * 2.0f;
	const int cells = MAX(int(Math::round(collision_extent / sp)), 2);
	const int w = cells + 1;
	PackedFloat32Array heights;
	heights.resize(w * w);
	float *hw = heights.ptrw();
	for (int z = 0; z < w; z++) {
		for (int x = 0; x < w; x++) {
			const float wx = mesh_center.x + (x - cells * 0.5f) * sp;
			const float wz = mesh_center.y + (z - cells * 0.5f) * sp;
			hw[z * w + x] = _surface_at(wx, wz, cover) / sp;
		}
	}
	collision_node->set_scale(Vector3(sp, sp, sp));
	if (collision_shape->get_map_width() != w || collision_shape->get_map_depth() != w) {
		collision_shape->set_map_width(w);
		collision_shape->set_map_depth(w);
	}
	collision_shape->set_map_data(heights);
	collision_dirty = false;
}

void SnowSurface3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			_ensure_children();
			if (!Engine::get_singleton()->is_editor_hint()) {
				set_physics_process_internal(true);
			}
		} break;
		case NOTIFICATION_READY: {
			_follow(_follow_position());
			_upload_deform();
			_update_collision();
		} break;
		case NOTIFICATION_EXIT_TREE: {
			set_physics_process_internal(false);
		} break;
		case NOTIFICATION_INTERNAL_PHYSICS_PROCESS: {
			const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
			const double delta = get_physics_process_delta_time();
			_follow(_follow_position());
			if (deform_enabled && field.valid) {
				// Pressed tiles owe a second of slumping: the snow a body pushes aside is loose and slides off
				// where it lies steeper than its repose (Sumner's erosion step, as the granular ground): into
				// the print, or off a thin wall left between two overlapping steps. Only that rim moves: the
				// compacted walls stand.
				field.press_settle = 1.0f;
				_press_movers(delta);
				auto ground_fn = [this](float x, float z) { return _ground_at(x, z); };
				const SnowCover3D *slump_cover = _cover();
				auto params_fn = [slump_cover](float x, float z, float &r_tan, float &r_rate, float &r_max) {
					// Plates of a broken crust are slabs, not loose snow: they stay as heaped.
					if (!slump_cover || slump_cover->get_crust_thickness_at(Vector3(x, 0, z)) > 0.0f) {
						return false;
					}
					r_tan = LOOSE_SNOW_TAN_REPOSE;
					r_rate = 1e3f;
					r_max = 0.0f;
					return true;
				};
				field.slump(delta, ground_fn, params_fn);
				_relax();
			}
			collision_accum += delta;
			if (collision_accum >= 1.0 / collision_rate) {
				collision_accum = 0.0;
				_update_collision();
			}
			if (field.dirty) {
				_upload_deform();
			}
			// The cover's texture and rect move on its own schedule.
			const SnowCover3D *cover = _cover();
			if (cover && cover->get_snow_texture().is_valid() && material.is_valid() && material->get_shader_parameter("snow_tex") != Variant(cover->get_snow_texture())) {
				_update_material();
			} else if (cover && material.is_valid()) {
				const Rect2 r = cover->get_map_rect();
				const Vector4 want(r.position.x, r.position.y, r.size.x, r.size.y);
				if (Vector4(material->get_shader_parameter("snow_rect")) != want) {
					_update_material();
				}
			}
			if (cover && cover->get_update_count() != seen_cover_update) {
				collision_dirty = true;
			}
			step_msec = double(OS::get_singleton()->get_ticks_usec() - t0) / 1000.0;
		} break;
	}
}

// ---------------------------------------------------------------------------------------------------------
// Queries

float SnowSurface3D::get_surface_height_at(Vector3 p_position) const {
	return _surface_at(p_position.x, p_position.z, _cover());
}

float SnowSurface3D::get_dent_at(Vector3 p_position) const {
	return field.dent_at(p_position.x, p_position.z);
}

float SnowSurface3D::get_rim_at(Vector3 p_position) const {
	return field.rim_at(p_position.x, p_position.z);
}

Vector2 SnowSurface3D::get_moved_volume() const {
	return field.moved_volume();
}

Rect2 SnowSurface3D::get_deform_rect() const {
	return field.valid ? field.rect() : Rect2(0, 0, deform_extent, deform_extent);
}

// ---------------------------------------------------------------------------------------------------------
// Settings

#define SNOW_SURFACE_REBUILD \
	mesh_dirty = true;       \
	ground_valid = false;    \
	if (is_inside_tree()) {  \
		_ensure_children();  \
		_follow(_follow_position()); \
	}

void SnowSurface3D::set_snow_cover(const NodePath &p_path) {
	snow_cover = p_path;
	if (is_inside_tree()) {
		ground_valid = false;
		_follow(_follow_position());
	}
}
void SnowSurface3D::set_extent(float p_extent) {
	extent = MAX(p_extent, deform_extent);
	SNOW_SURFACE_REBUILD
}
void SnowSurface3D::set_detail_spacing(float p_spacing) {
	detail_spacing = CLAMP(p_spacing, 0.02f, 4.0f);
	SNOW_SURFACE_REBUILD
}
void SnowSurface3D::set_max_spacing(float p_spacing) {
	max_spacing = CLAMP(p_spacing, detail_spacing, 16.0f);
	SNOW_SURFACE_REBUILD
}
void SnowSurface3D::set_ground_spacing(float p_spacing) {
	ground_spacing = CLAMP(p_spacing, 0.05f, 8.0f);
	SNOW_SURFACE_REBUILD
}
void SnowSurface3D::set_min_depth(float p_depth) {
	min_depth = MAX(p_depth, 0.001f);
	_update_material();
	collision_dirty = true;
}
void SnowSurface3D::set_edge_fade(float p_m) {
	edge_fade = MAX(p_m, 0.0f);
	_update_material();
}
void SnowSurface3D::set_micro_relief(float p_scale) {
	micro_relief = MAX(p_scale, 0.0f);
	_update_material();
}
void SnowSurface3D::set_tint(const Color &p_tint) {
	tint = p_tint;
	_update_material();
}
void SnowSurface3D::set_deform_enabled(bool p_enabled) {
	deform_enabled = p_enabled;
	field.invalidate();
	if (is_inside_tree()) {
		_follow(_follow_position());
	}
	_update_material();
	collision_dirty = true;
}
void SnowSurface3D::set_deform_extent(float p_extent) {
	deform_extent = CLAMP(p_extent, 4.0f, 512.0f);
	extent = MAX(extent, deform_extent);
	field.invalidate();
	field.clear();
	SNOW_SURFACE_REBUILD
}
void SnowSurface3D::set_deform_resolution(int p_resolution) {
	deform_resolution = CLAMP((p_resolution + TILE - 1) / TILE * TILE, TILE * 2, 4096);
	field.invalidate();
	field.clear();
	SNOW_SURFACE_REBUILD
}
void SnowSurface3D::set_collision_enabled(bool p_enabled) {
	collision_enabled = p_enabled;
	if (is_inside_tree()) {
		_ensure_children();
		_follow(_follow_position());
	}
}
void SnowSurface3D::set_collision_extent(float p_extent) {
	collision_extent = CLAMP(p_extent, 1.0f, deform_extent);
	collision_dirty = true;
}
void SnowSurface3D::set_collision_layer(uint32_t p_layer) {
	collision_layer = p_layer;
	if (body) {
		body->set_collision_layer(p_layer);
	}
}
void SnowSurface3D::set_collision_mask(uint32_t p_mask) {
	collision_mask = p_mask;
	if (body) {
		body->set_collision_mask(p_mask);
	}
}

void SnowSurface3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_snow_cover", "path"), &SnowSurface3D::set_snow_cover);
	ClassDB::bind_method(D_METHOD("get_snow_cover"), &SnowSurface3D::get_snow_cover);
	ClassDB::bind_method(D_METHOD("set_follow_target", "path"), &SnowSurface3D::set_follow_target);
	ClassDB::bind_method(D_METHOD("get_follow_target"), &SnowSurface3D::get_follow_target);
	ClassDB::bind_method(D_METHOD("set_extent", "extent"), &SnowSurface3D::set_extent);
	ClassDB::bind_method(D_METHOD("get_extent"), &SnowSurface3D::get_extent);
	ClassDB::bind_method(D_METHOD("set_detail_spacing", "spacing"), &SnowSurface3D::set_detail_spacing);
	ClassDB::bind_method(D_METHOD("get_detail_spacing"), &SnowSurface3D::get_detail_spacing);
	ClassDB::bind_method(D_METHOD("set_max_spacing", "spacing"), &SnowSurface3D::set_max_spacing);
	ClassDB::bind_method(D_METHOD("get_max_spacing"), &SnowSurface3D::get_max_spacing);
	ClassDB::bind_method(D_METHOD("set_ground_spacing", "spacing"), &SnowSurface3D::set_ground_spacing);
	ClassDB::bind_method(D_METHOD("get_ground_spacing"), &SnowSurface3D::get_ground_spacing);
	ClassDB::bind_method(D_METHOD("set_min_depth", "depth"), &SnowSurface3D::set_min_depth);
	ClassDB::bind_method(D_METHOD("get_min_depth"), &SnowSurface3D::get_min_depth);
	ClassDB::bind_method(D_METHOD("set_edge_fade", "distance"), &SnowSurface3D::set_edge_fade);
	ClassDB::bind_method(D_METHOD("get_edge_fade"), &SnowSurface3D::get_edge_fade);
	ClassDB::bind_method(D_METHOD("set_micro_relief", "scale"), &SnowSurface3D::set_micro_relief);
	ClassDB::bind_method(D_METHOD("get_micro_relief"), &SnowSurface3D::get_micro_relief);
	ClassDB::bind_method(D_METHOD("set_tint", "tint"), &SnowSurface3D::set_tint);
	ClassDB::bind_method(D_METHOD("get_tint"), &SnowSurface3D::get_tint);
	ClassDB::bind_method(D_METHOD("set_deform_enabled", "enabled"), &SnowSurface3D::set_deform_enabled);
	ClassDB::bind_method(D_METHOD("is_deform_enabled"), &SnowSurface3D::is_deform_enabled);
	ClassDB::bind_method(D_METHOD("set_deform_extent", "extent"), &SnowSurface3D::set_deform_extent);
	ClassDB::bind_method(D_METHOD("get_deform_extent"), &SnowSurface3D::get_deform_extent);
	ClassDB::bind_method(D_METHOD("set_deform_resolution", "resolution"), &SnowSurface3D::set_deform_resolution);
	ClassDB::bind_method(D_METHOD("get_deform_resolution"), &SnowSurface3D::get_deform_resolution);
	ClassDB::bind_method(D_METHOD("set_deform_groups", "groups"), &SnowSurface3D::set_deform_groups);
	ClassDB::bind_method(D_METHOD("get_deform_groups"), &SnowSurface3D::get_deform_groups);
	ClassDB::bind_method(D_METHOD("set_compacted_density", "density"), &SnowSurface3D::set_compacted_density);
	ClassDB::bind_method(D_METHOD("get_compacted_density"), &SnowSurface3D::get_compacted_density);
	ClassDB::bind_method(D_METHOD("set_rim_share", "share"), &SnowSurface3D::set_rim_share);
	ClassDB::bind_method(D_METHOD("get_rim_share"), &SnowSurface3D::get_rim_share);
	ClassDB::bind_method(D_METHOD("set_contact_tolerance", "distance"), &SnowSurface3D::set_contact_tolerance);
	ClassDB::bind_method(D_METHOD("get_contact_tolerance"), &SnowSurface3D::get_contact_tolerance);
	ClassDB::bind_method(D_METHOD("set_bekker_n", "n"), &SnowSurface3D::set_bekker_n);
	ClassDB::bind_method(D_METHOD("get_bekker_n"), &SnowSurface3D::get_bekker_n);
	ClassDB::bind_method(D_METHOD("set_bekker_kc", "kc"), &SnowSurface3D::set_bekker_kc);
	ClassDB::bind_method(D_METHOD("get_bekker_kc"), &SnowSurface3D::get_bekker_kc);
	ClassDB::bind_method(D_METHOD("set_bekker_kphi", "kphi"), &SnowSurface3D::set_bekker_kphi);
	ClassDB::bind_method(D_METHOD("get_bekker_kphi"), &SnowSurface3D::get_bekker_kphi);
	ClassDB::bind_method(D_METHOD("set_character_mass", "mass"), &SnowSurface3D::set_character_mass);
	ClassDB::bind_method(D_METHOD("get_character_mass"), &SnowSurface3D::get_character_mass);
	ClassDB::bind_method(D_METHOD("set_character_contact_area", "area"), &SnowSurface3D::set_character_contact_area);
	ClassDB::bind_method(D_METHOD("get_character_contact_area"), &SnowSurface3D::get_character_contact_area);
	ClassDB::bind_method(D_METHOD("set_refill_share", "share"), &SnowSurface3D::set_refill_share);
	ClassDB::bind_method(D_METHOD("get_refill_share"), &SnowSurface3D::get_refill_share);
	ClassDB::bind_method(D_METHOD("set_wind_fill_rate", "rate"), &SnowSurface3D::set_wind_fill_rate);
	ClassDB::bind_method(D_METHOD("get_wind_fill_rate"), &SnowSurface3D::get_wind_fill_rate);
	ClassDB::bind_method(D_METHOD("set_max_cached_tiles", "tiles"), &SnowSurface3D::set_max_cached_tiles);
	ClassDB::bind_method(D_METHOD("get_max_cached_tiles"), &SnowSurface3D::get_max_cached_tiles);
	ClassDB::bind_method(D_METHOD("set_collision_enabled", "enabled"), &SnowSurface3D::set_collision_enabled);
	ClassDB::bind_method(D_METHOD("is_collision_enabled"), &SnowSurface3D::is_collision_enabled);
	ClassDB::bind_method(D_METHOD("set_collision_extent", "extent"), &SnowSurface3D::set_collision_extent);
	ClassDB::bind_method(D_METHOD("get_collision_extent"), &SnowSurface3D::get_collision_extent);
	ClassDB::bind_method(D_METHOD("set_collision_layer", "layer"), &SnowSurface3D::set_collision_layer);
	ClassDB::bind_method(D_METHOD("get_collision_layer"), &SnowSurface3D::get_collision_layer);
	ClassDB::bind_method(D_METHOD("set_collision_mask", "mask"), &SnowSurface3D::set_collision_mask);
	ClassDB::bind_method(D_METHOD("get_collision_mask"), &SnowSurface3D::get_collision_mask);
	ClassDB::bind_method(D_METHOD("set_collision_rate", "rate"), &SnowSurface3D::set_collision_rate);
	ClassDB::bind_method(D_METHOD("get_collision_rate"), &SnowSurface3D::get_collision_rate);

	ClassDB::bind_method(D_METHOD("press", "position", "radius", "bottom", "sink", "round"), &SnowSurface3D::press, DEFVAL(0.0f), DEFVAL(true));
	ClassDB::bind_method(D_METHOD("press_box", "position", "size", "yaw", "bottom", "sink"), &SnowSurface3D::press_box, DEFVAL(0.0f));
	ClassDB::bind_static_method("SnowSurface3D", D_METHOD("bekker_sinkage", "pressure_kpa", "width", "n", "kc", "kphi"), &SnowSurface3D::bekker_sinkage);
	ClassDB::bind_method(D_METHOD("clear_deformation"), &SnowSurface3D::clear_deformation);
	ClassDB::bind_method(D_METHOD("refresh_ground"), &SnowSurface3D::refresh_ground);
	ClassDB::bind_method(D_METHOD("get_surface_height_at", "position"), &SnowSurface3D::get_surface_height_at);
	ClassDB::bind_method(D_METHOD("get_dent_at", "position"), &SnowSurface3D::get_dent_at);
	ClassDB::bind_method(D_METHOD("get_rim_at", "position"), &SnowSurface3D::get_rim_at);
	ClassDB::bind_method(D_METHOD("get_moved_volume"), &SnowSurface3D::get_moved_volume);
	ClassDB::bind_method(D_METHOD("get_cached_tile_count"), &SnowSurface3D::get_cached_tile_count);
	ClassDB::bind_method(D_METHOD("get_deform_rect"), &SnowSurface3D::get_deform_rect);
	ClassDB::bind_method(D_METHOD("get_step_time_msec"), &SnowSurface3D::get_step_time_msec);
	ClassDB::bind_method(D_METHOD("get_surface_material"), &SnowSurface3D::get_surface_material);

	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "snow_cover", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "SnowCover3D"), "set_snow_cover", "get_snow_cover");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "follow_target", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D"), "set_follow_target", "get_follow_target");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "extent", PROPERTY_HINT_RANGE, "8,1024,1,suffix:m"), "set_extent", "get_extent");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "detail_spacing", PROPERTY_HINT_RANGE, "0.02,4,0.005,suffix:m"), "set_detail_spacing", "get_detail_spacing");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "max_spacing", PROPERTY_HINT_RANGE, "0.1,16,0.05,suffix:m"), "set_max_spacing", "get_max_spacing");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ground_spacing", PROPERTY_HINT_RANGE, "0.05,8,0.05,suffix:m"), "set_ground_spacing", "get_ground_spacing");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "min_depth", PROPERTY_HINT_RANGE, "0.001,0.5,0.001,suffix:m"), "set_min_depth", "get_min_depth");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "edge_fade", PROPERTY_HINT_RANGE, "0,64,0.5,suffix:m"), "set_edge_fade", "get_edge_fade");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "micro_relief", PROPERTY_HINT_RANGE, "0,4,0.01"), "set_micro_relief", "get_micro_relief");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "tint", PROPERTY_HINT_COLOR_NO_ALPHA), "set_tint", "get_tint");
	ADD_GROUP("Deformation", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "deform_enabled"), "set_deform_enabled", "is_deform_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "deform_extent", PROPERTY_HINT_RANGE, "4,512,1,suffix:m"), "set_deform_extent", "get_deform_extent");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "deform_resolution", PROPERTY_HINT_RANGE, "128,4096,64"), "set_deform_resolution", "get_deform_resolution");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_STRING_ARRAY, "deform_groups"), "set_deform_groups", "get_deform_groups");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "compacted_density", PROPERTY_HINT_RANGE, "100,917,1,suffix:kg/m3"), "set_compacted_density", "get_compacted_density");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "rim_share", PROPERTY_HINT_RANGE, "0,0.9,0.01"), "set_rim_share", "get_rim_share");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "contact_tolerance", PROPERTY_HINT_RANGE, "0,0.5,0.005,suffix:m"), "set_contact_tolerance", "get_contact_tolerance");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "bekker_n", PROPERTY_HINT_RANGE, "0.3,3,0.01"), "set_bekker_n", "get_bekker_n");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "bekker_kc", PROPERTY_HINT_RANGE, "0,100,0.01"), "set_bekker_kc", "get_bekker_kc");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "bekker_kphi", PROPERTY_HINT_RANGE, "0.001,2000,0.01"), "set_bekker_kphi", "get_bekker_kphi");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "character_mass", PROPERTY_HINT_RANGE, "0,500,0.5,suffix:kg"), "set_character_mass", "get_character_mass");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "character_contact_area", PROPERTY_HINT_RANGE, "0.001,1,0.001,suffix:m2"), "set_character_contact_area", "get_character_contact_area");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "refill_share", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_refill_share", "get_refill_share");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wind_fill_rate", PROPERTY_HINT_RANGE, "0,10,0.01"), "set_wind_fill_rate", "get_wind_fill_rate");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_cached_tiles", PROPERTY_HINT_RANGE, "0,65536,1"), "set_max_cached_tiles", "get_max_cached_tiles");
	ADD_GROUP("Collision", "collision_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "collision_enabled"), "set_collision_enabled", "is_collision_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "collision_extent", PROPERTY_HINT_RANGE, "1,512,0.5,suffix:m"), "set_collision_extent", "get_collision_extent");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_layer", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_collision_layer", "get_collision_layer");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_mask", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_collision_mask", "get_collision_mask");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "collision_rate", PROPERTY_HINT_RANGE, "0.5,120,0.5,suffix:Hz"), "set_collision_rate", "get_collision_rate");
}

SnowSurface3D::SnowSurface3D() {
	deform_groups.push_back("snow_deform");
	deform_groups.push_back("grass_trample");
	shader.instantiate();
	shader->set_code(SNOW_SURFACE_SHADER);
	material.instantiate();
	material->set_shader(shader);
}

SnowSurface3D::~SnowSurface3D() {
}

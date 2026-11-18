/**************************************************************************/
/*  snow_lake_ice_3d.cpp                                                  */
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

#include "snow_lake_ice_3d.h"

#include "snow_cover_3d.h"

#include "core/config/engine.h"
#include "core/object/class_db.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/physics/collision_shape_3d.h"
#include "scene/3d/physics/static_body_3d.h"
#include "scene/resources/3d/concave_polygon_shape_3d.h"
#include "scene/resources/mesh.h"

namespace {
// Ice thinner than this is not drawn (a skim of a few millimetres shows as the water itself).
constexpr float ICE_SHOW = 0.003f;

const char *ICE_SHADER_HEAD = R"(
shader_type spatial;
render_mode cull_back;
)";

const char *ICE_SHADER_BODY = R"(
uniform float snow = 1.0;
uniform float snow_max_slope = 55.0;
uniform float crack_strength = 1.0;
varying vec3 v_world;
varying vec3 v_normal;

// Integer lattice hash (pcg2d, Jarzynski and Olano 2020): the crack network never repeats.
vec2 ice_hash2(ivec2 c) {
	uvec2 v = uvec2(c) * 1664525u + 1013904223u;
	v.x += v.y * 1664525u;
	v.y += v.x * 1664525u;
	v ^= v >> 16u;
	v.x += v.y * 1664525u;
	v.y += v.x * 1664525u;
	v ^= v >> 16u;
	return vec2(v >> 8u) * (1.0 / 16777216.0);
}

// Smooth value noise on the same hash, for bending the crack lines.
vec2 ice_noise2(vec2 p) {
	ivec2 c = ivec2(floor(p));
	vec2 f = p - floor(p);
	f = f * f * (3.0 - 2.0 * f);
	return mix(mix(ice_hash2(c), ice_hash2(c + ivec2(1, 0)), f.x), mix(ice_hash2(c + ivec2(0, 1)), ice_hash2(c + ivec2(1, 1)), f.x), f.y) * 2.0 - 1.0;
}

// Distance to the nearest crack: the walls of a jittered Voronoi pattern (the second nearest site minus the
// nearest, halved), bent by noise, with only some walls cracked (a hash of the pair of plates decides), so
// the network is long, branching runs and not a honeycomb.
float ice_crack_distance(vec2 p, float p_keep) {
	p += 0.35 * ice_noise2(p * 1.7 + 5.0);
	ivec2 c = ivec2(floor(p));
	vec2 f = p - floor(p);
	float d1 = 8.0;
	float d2 = 8.0;
	ivec2 k1 = c;
	ivec2 k2 = c;
	for (int j = -1; j <= 1; j++) {
		for (int i = -1; i <= 1; i++) {
			ivec2 k = c + ivec2(i, j);
			vec2 o = vec2(float(i), float(j)) + ice_hash2(k);
			float d = length(o - f);
			if (d < d1) {
				d2 = d1;
				k2 = k1;
				d1 = d;
				k1 = k;
			} else if (d < d2) {
				d2 = d;
				k2 = k;
			}
		}
	}
	// The same value from either side of the wall: order the pair.
	ivec2 lo = (k1.x < k2.x || (k1.x == k2.x && k1.y < k2.y)) ? k1 : k2;
	ivec2 hi = all(equal(lo, k1)) ? k2 : k1;
	float wall = ice_hash2(lo * 7 + hi * 13 + ivec2(311, 97)).x;
	return wall < p_keep ? 0.5 * (d2 - d1) : 8.0;
}

void vertex() {
	v_world = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	v_normal = normalize((MODEL_MATRIX * vec4(NORMAL, 0.0)).xyz);
}

void fragment() {
	// Thickness (m) and the share that is snow ice, from the vertex colour.
	float h = COLOR.r * COLOR.r * 1.5;
	float white = COLOR.g;
	// Clear ice over deep water reflects about a tenth of the light (Bolsenga 1969), new thin ice less: the
	// dark water shows through. Snow ice, full of bubbles, scatters: albedo about 0.46.
	vec3 clear = mix(vec3(0.02, 0.03, 0.038), vec3(0.06, 0.08, 0.095), smoothstep(0.02, 0.3, h));
	vec3 col = mix(clear, vec3(0.5, 0.54, 0.58), white);
	float rough = mix(0.03, 0.35, white);
	// Cracks: thermal cracks split the sheet into plates of several metres, a few splitting again. Frozen
	// bubbles line them, so they show pale in the clear ice; most are hairlines. Each line is at least about
	// one and a half pixels wide (the pixel footprint from the projection and viewport, the same in raster and
	// path tracing), so it neither aliases in raster nor averages away under path tracing, and the network
	// fades out before the plates get smaller than a few pixels.
	float dist = distance(v_world, INV_VIEW_MATRIX[3].xyz);
	float pixel = dist * 2.0 / max(abs(PROJECTION_MATRIX[1][1]) * VIEWPORT_SIZE.y, 1.0);
	float wide = max(0.004, 0.75 * pixel);
	vec2 q = mat2(vec2(0.8, 0.6), vec2(-0.6, 0.8)) * v_world.xz;
	float big = 1.0 - smoothstep(0.0, wide / 9.0, ice_crack_distance(q / 9.0 + 23.0, 0.55));
	float fine = (1.0 - smoothstep(0.0, wide / 2.5, ice_crack_distance(q / 2.5 + 71.0, 0.2))) * smoothstep(0.08, 0.2, h);
	float crack = max(big, 0.5 * fine) * smoothstep(0.02, 0.06, h) * (1.0 - smoothstep(25.0, 90.0, dist));
	// A crack is a fracture plane full of tiny bubbles: it scatters like snow ice, so it reads in the
	// diffuse albedo (which path tracing keeps at any sample count), not only as a roughness change.
	// Far away a pixel-wide line stands for a hairline much thinner than the pixel, so its contrast falls with
	// distance (about its true coverage) and it does not break into bright dashes at grazing angles.
	float crack_far = 1.0 - 0.7 * smoothstep(12.0, 45.0, dist);
	col = mix(col, vec3(0.6, 0.64, 0.68), crack * crack_strength * (1.0 - white) * 0.8 * crack_far);
	rough = mix(rough, 0.2, crack * crack_strength * crack_far);
	float metallic = 0.0;
	float specular = 0.335; // ice, F0 0.018
	vec4 snow_s = snow_cover_at(v_world);
	float sw = snow_cover_weight(snow_s.x * snow, snow_max_slope, v_normal, v_normal);
	snow_cover_apply(sw, snow_s.y, snow_s.z, col, rough, metallic, specular);
	ALBEDO = col;
	ROUGHNESS = rough;
	METALLIC = metallic;
	SPECULAR = specular;
}
)";

struct Tier {
	const SnowCover3D *cover = nullptr;
	Rect2 skip; // cells centred in here belong to a finer tier
};
} // namespace

float SnowLakeIce3D::freeboard(float p_ice) {
	// Floating ice stands out of the water by (1 - 917 / 1000) of its thickness.
	return p_ice > 0.0f ? MAX((1.0f - 917.0f / 1000.0f) * p_ice, 0.004f) : 0.0f;
}

Ref<Shader> SnowLakeIce3D::_shader() {
	Ref<Shader> shader;
	shader.instantiate();
	shader->set_code(String(ICE_SHADER_HEAD) + BaseMaterial3D::get_snow_shader_code() + String(ICE_SHADER_BODY));
	return shader;
}

void SnowLakeIce3D::_ensure_children() {
	if (!mesh_instance) {
		mesh_instance = memnew(MeshInstance3D);
		add_child(mesh_instance, false, INTERNAL_MODE_FRONT);
	}
	if (!body) {
		body = memnew(StaticBody3D);
		add_child(body, false, INTERNAL_MODE_FRONT);
		shape = memnew(CollisionShape3D);
		body->add_child(shape, false, INTERNAL_MODE_FRONT);
	}
	if (default_material.is_null()) {
		default_material.instantiate();
		default_material->set_shader(_shader());
	}
	default_material->set_shader_parameter("crack_strength", crack_strength);
}

void SnowLakeIce3D::rebuild() {
	_ensure_children();
	const SnowCover3D *cover = SnowCover3D::get_active();
	quad_count = 0;
	walkable_count = 0;
	if (!cover || !cover->lake_ice_enabled) {
		mesh_instance->set_mesh(Ref<Mesh>());
		shape->set_shape(Ref<Shape3D>());
		collision_key = 0;
		return;
	}
	built_update = cover->update_count;
	built_world_update = cover->world ? cover->world->update_count : 0;

	Tier tiers[2];
	int tier_count = 0;
	tiers[tier_count++].cover = cover;
	if (cover->world) {
		tiers[tier_count].cover = cover->world;
		tiers[tier_count].skip = cover->origin_valid ? cover->get_map_rect() : Rect2();
		tier_count++;
	}

	const Vector3 follow = cover->_follow_position();
	PackedVector3Array verts;
	PackedVector3Array normals;
	PackedColorArray colors;
	PackedInt32Array indices;
	Vector<Vector3> faces;
	uint32_t key = 0x9e3779b9u;

	for (int t = 0; t < tier_count; t++) {
		const SnowCover3D *c = tiers[t].cover;
		const int n = c->resolution;
		const int nn = n * n;
		if (!c->origin_valid || c->cell_water.size() != nn || c->pub.ice.size() != nn || c->pub.white_ice.size() != nn) {
			continue;
		}
		const float cs = c->_cell_size();
		const float *water = c->cell_water.ptr();
		const float *ice = c->pub.ice.ptr();
		const float *white = c->pub.white_ice.ptr();
		const bool skip = tiers[t].skip.has_area();

		// Each drawn cell: its ice (or, on the one-cell margin round it, its iced neighbour's) at its level.
		// The margin carries the sheet under the shore, where the ground hides it, so the edge is where the
		// terrain meets the ice and not the cells' steps.
		Vector<float> top;
		Vector<float> thick;
		Vector<float> share;
		top.resize(nn);
		thick.resize(nn);
		share.resize(nn);
		Vector<uint8_t> kind; // 0 none, 1 ice, 2 margin
		kind.resize(nn);
		kind.fill(0);
		float *tp = top.ptrw();
		float *th = thick.ptrw();
		float *sh = share.ptrw();
		uint8_t *kd = kind.ptrw();
		bool any = false;
		for (int i = 0; i < nn; i++) {
			if (water[i] > -1e29f && ice[i] >= ICE_SHOW) {
				any = true;
				kd[i] = 1;
				th[i] = ice[i];
				sh[i] = CLAMP(white[i] / MAX(ice[i], 1e-4f), 0.0f, 1.0f);
				tp[i] = water[i] + freeboard(ice[i]);
			}
		}
		if (!any) {
			continue;
		}
		for (int z = 0; z < n; z++) {
			for (int x = 0; x < n; x++) {
				const int i = z * n + x;
				if (kd[i] == 1 || water[i] > -1e29f) {
					continue;
				}
				float best = -1.0f;
				for (int dz = -1; dz <= 1; dz++) {
					for (int dx = -1; dx <= 1; dx++) {
						const int xx = x + dx, zz = z + dz;
						if (xx < 0 || zz < 0 || xx >= n || zz >= n) {
							continue;
						}
						const int j = zz * n + xx;
						if (kd[j] == 1 && th[j] > best) {
							best = th[j];
							tp[i] = tp[j];
							th[i] = th[j];
							sh[i] = sh[j];
						}
					}
				}
				if (best >= 0.0f) {
					kd[i] = 2;
				}
			}
		}

		// Shared corners, each at the mean of the drawn cells round it, so the sheet is one smooth surface.
		const int m = n + 1;
		Vector<float> csum;
		Vector<float> cth;
		Vector<float> csh;
		Vector<int> ccount;
		Vector<int> cindex;
		csum.resize(m * m);
		cth.resize(m * m);
		csh.resize(m * m);
		ccount.resize(m * m);
		cindex.resize(m * m);
		csum.fill(0.0f);
		cth.fill(0.0f);
		csh.fill(0.0f);
		ccount.fill(0);
		cindex.fill(-1);
		Vector<uint8_t> draw;
		draw.resize(nn);
		draw.fill(0);
		for (int z = 0; z < n; z++) {
			for (int x = 0; x < n; x++) {
				const int i = z * n + x;
				if (!kd[i]) {
					continue;
				}
				if (skip) {
					const Vector3 cc = c->_cell_center(x, z);
					if (tiers[t].skip.has_point(Vector2(cc.x, cc.z))) {
						continue;
					}
				}
				draw.write[i] = 1;
				const int corners[4] = { z * m + x, z * m + x + 1, (z + 1) * m + x, (z + 1) * m + x + 1 };
				for (int k : corners) {
					csum.write[k] += tp[i];
					cth.write[k] += th[i];
					csh.write[k] += sh[i];
					ccount.write[k]++;
				}
			}
		}
		const Vector2 o = Vector2(c->origin_cell) * cs;
		const bool near_tier = t == 0;
		for (int z = 0; z < n; z++) {
			for (int x = 0; x < n; x++) {
				const int i = z * n + x;
				if (!draw[i]) {
					continue;
				}
				const int corners[4] = { z * m + x, z * m + x + 1, (z + 1) * m + x + 1, (z + 1) * m + x };
				int vi[4];
				Vector3 p[4];
				for (int k = 0; k < 4; k++) {
					const int ci = corners[k];
					const int cx = ci % m, cz = ci / m;
					p[k] = Vector3(o.x + cx * cs, csum[ci] / ccount[ci], o.y + cz * cs);
					if (cindex[ci] < 0) {
						cindex.write[ci] = verts.size();
						verts.push_back(p[k]);
						normals.push_back(Vector3(0, 1, 0));
						const float h = cth[ci] / ccount[ci];
						colors.push_back(Color(Math::sqrt(CLAMP(h / 1.5f, 0.0f, 1.0f)), csh[ci] / ccount[ci], 0.0f, 1.0f));
					}
					vi[k] = cindex[ci];
				}
				// Clockwise seen from above (Godot's front faces).
				indices.push_back(vi[0]);
				indices.push_back(vi[1]);
				indices.push_back(vi[2]);
				indices.push_back(vi[0]);
				indices.push_back(vi[2]);
				indices.push_back(vi[3]);
				quad_count++;
				// Collision: ice that bears a walker, near the follow point.
				if (collision_enabled && near_tier && th[i] >= SnowCover3D::ICE_WALK) {
					const Vector3 cc = c->_cell_center(x, z);
					if (Vector2(cc.x - follow.x, cc.z - follow.z).length() <= collision_radius) {
						faces.push_back(p[0]);
						faces.push_back(p[1]);
						faces.push_back(p[2]);
						faces.push_back(p[0]);
						faces.push_back(p[2]);
						faces.push_back(p[3]);
						walkable_count++;
						key = (key ^ uint32_t(i + 1)) * 16777619u;
						key = (key ^ uint32_t(int(p[0].y * 100.0f))) * 16777619u;
					}
				}
			}
		}
		key = (key ^ uint32_t(c->origin_cell.x * 73856093 ^ c->origin_cell.y * 19349663)) * 16777619u;
	}

	Ref<ArrayMesh> mesh;
	if (!indices.is_empty()) {
		mesh.instantiate();
		Array arrays;
		arrays.resize(Mesh::ARRAY_MAX);
		arrays[Mesh::ARRAY_VERTEX] = verts;
		arrays[Mesh::ARRAY_NORMAL] = normals;
		arrays[Mesh::ARRAY_COLOR] = colors;
		arrays[Mesh::ARRAY_INDEX] = indices;
		mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
		mesh->surface_set_material(0, material.is_valid() ? material : Ref<Material>(default_material));
	}
	mesh_instance->set_mesh(mesh);
	// The collision changes only when the walkable cells do (centimetre levels), not with every update.
	if (faces.is_empty()) {
		shape->set_shape(Ref<Shape3D>());
		collision_key = 0;
	} else if (key != collision_key) {
		Ref<ConcavePolygonShape3D> s;
		s.instantiate();
		s->set_faces(faces);
		shape->set_shape(s);
		collision_key = key;
	}
}

void SnowLakeIce3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			set_as_top_level(true);
			set_global_transform(Transform3D());
			if (!Engine::get_singleton()->is_editor_hint()) {
				set_process_internal(true);
				rebuild();
			}
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			accum += get_process_delta_time();
			if (accum < rebuild_interval) {
				break;
			}
			const SnowCover3D *cover = SnowCover3D::get_active();
			const uint64_t u = cover ? cover->update_count : 0;
			const uint64_t wu = cover && cover->world ? cover->world->update_count : 0;
			if (u != built_update || wu != built_world_update) {
				accum = 0.0;
				rebuild();
			}
		} break;
	}
}

void SnowLakeIce3D::set_rebuild_interval(float p_seconds) {
	rebuild_interval = MAX(p_seconds, 0.0f);
}

void SnowLakeIce3D::set_collision_radius(float p_radius) {
	collision_radius = MAX(p_radius, 0.0f);
}

void SnowLakeIce3D::set_collision_enabled(bool p_enabled) {
	collision_enabled = p_enabled;
}

void SnowLakeIce3D::set_crack_strength(float p_strength) {
	crack_strength = CLAMP(p_strength, 0.0f, 1.0f);
	if (default_material.is_valid()) {
		default_material->set_shader_parameter("crack_strength", crack_strength);
	}
}

void SnowLakeIce3D::set_material(const Ref<Material> &p_material) {
	material = p_material;
}

void SnowLakeIce3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("rebuild"), &SnowLakeIce3D::rebuild);
	ClassDB::bind_method(D_METHOD("get_quad_count"), &SnowLakeIce3D::get_quad_count);
	ClassDB::bind_method(D_METHOD("get_walkable_count"), &SnowLakeIce3D::get_walkable_count);
	ClassDB::bind_static_method("SnowLakeIce3D", D_METHOD("freeboard", "ice"), &SnowLakeIce3D::freeboard);
	ClassDB::bind_method(D_METHOD("set_rebuild_interval", "seconds"), &SnowLakeIce3D::set_rebuild_interval);
	ClassDB::bind_method(D_METHOD("get_rebuild_interval"), &SnowLakeIce3D::get_rebuild_interval);
	ClassDB::bind_method(D_METHOD("set_collision_radius", "radius"), &SnowLakeIce3D::set_collision_radius);
	ClassDB::bind_method(D_METHOD("get_collision_radius"), &SnowLakeIce3D::get_collision_radius);
	ClassDB::bind_method(D_METHOD("set_collision_enabled", "enabled"), &SnowLakeIce3D::set_collision_enabled);
	ClassDB::bind_method(D_METHOD("is_collision_enabled"), &SnowLakeIce3D::is_collision_enabled);
	ClassDB::bind_method(D_METHOD("set_crack_strength", "strength"), &SnowLakeIce3D::set_crack_strength);
	ClassDB::bind_method(D_METHOD("get_crack_strength"), &SnowLakeIce3D::get_crack_strength);
	ClassDB::bind_method(D_METHOD("set_material", "material"), &SnowLakeIce3D::set_material);
	ClassDB::bind_method(D_METHOD("get_material"), &SnowLakeIce3D::get_material);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "rebuild_interval", PROPERTY_HINT_RANGE, "0,30,0.1,suffix:s"), "set_rebuild_interval", "get_rebuild_interval");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "collision_enabled"), "set_collision_enabled", "is_collision_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "collision_radius", PROPERTY_HINT_RANGE, "0,2000,1,suffix:m"), "set_collision_radius", "get_collision_radius");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "crack_strength", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_crack_strength", "get_crack_strength");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "material", PROPERTY_HINT_RESOURCE_TYPE, "BaseMaterial3D,ShaderMaterial"), "set_material", "get_material");
}

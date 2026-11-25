/**************************************************************************/
/*  grass_field_3d.cpp                                                    */
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

#include "grass_field_3d.h"

#include "core/templates/hash_set.h"
#include "modules/world/world_contracts.h"
#include "tree_generator.h"
#include "vegetation_voxel_lod.h"
#include "deer_beds.h"
#include "fairy_rings.h"
#include "vegetation_fire.h"
#include "vegetation_light.h"

#include "vegetation_field_3d.h"

#include "core/config/engine.h"
#include "core/math/geometry_2d.h"
#include "core/config/project_settings.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/mutex.h"
#include "core/os/os.h"
#include "core/templates/hashfuncs.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/light_3d.h"
#include "scene/main/window.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/world_environment.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/3d/sky_material.h"
#include "scene/resources/environment.h"
#include "scene/resources/sky.h"
#include "scene/resources/material.h"
#include "scene/3d/multimesh_instance_3d.h"
#include "plant_generator.h"
#include "scene/3d/physics/collision_shape_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "scene/resources/3d/box_shape_3d.h"
#include "scene/resources/3d/capsule_shape_3d.h"
#include "scene/resources/3d/cylinder_shape_3d.h"
#include "scene/resources/3d/sphere_shape_3d.h"
#include "scene/resources/multimesh.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_device_binds.h"
#include "servers/rendering/rendering_server.h"

#include "grass_fill.glsl.gen.h"

#include "modules/terrain_deform/terrain_data_map_3d.h"
#include "modules/weather/weather_map_3d.h"

namespace {

uint32_t cell_hash(int p_x, int p_z, uint32_t p_salt) {
	uint32_t h = hash_murmur3_one_32(uint32_t(p_x), p_salt);
	h = hash_murmur3_one_32(uint32_t(p_z), h);
	return hash_fmix32(h);
}

float unit(uint32_t p_h, int p_shift) {
	return float((p_h >> p_shift) & 0xFFu) / 255.0f;
}

// The clump a blade belongs to: the nearest of jittered seed points, one per cell (a Voronoi
// cell, as Ghost of Tsushima's clumps), so clumps are irregular patches, not a square grid that
// reads as blocks of lighter and darker grass. grass_fill.glsl has the same.
uint32_t clump_hash(float p_x, float p_z, float p_size) {
	const int cx = int(Math::floor(p_x / p_size));
	const int cz = int(Math::floor(p_z / p_size));
	float best = 1e30f;
	uint32_t best_h = 0;
	for (int dz = -1; dz <= 1; dz++) {
		for (int dx = -1; dx <= 1; dx++) {
			const uint32_t h = cell_hash(cx + dx, cz + dz, 0xC2B2AE35u);
			const float sx = (cx + dx + unit(h, 0) * 0.5f + unit(h, 8) * 0.5f) * p_size - p_x;
			const float sz = (cz + dz + unit(h, 16) * 0.5f + unit(h, 24) * 0.5f) * p_size - p_z;
			const float d2 = sx * sx + sz * sz;
			if (d2 < best) {
				best = d2;
				best_h = h;
			}
		}
	}
	return hash_fmix32(best_h ^ 0x27D4EB2Fu);
}

// Blue-noise rank mask (void and cluster, Ulichney 1993): 64 x 64 cells ranked so that every
// threshold keeps an even spread, with no clumps and gaps and no lattice rows. Built once, by
// putting each next point in the largest void (lowest Gaussian energy, wrapping round).
constexpr int BLUE_SIZE = 64;

const uint16_t *build_blue_ranks() {
	static uint16_t ranks[BLUE_SIZE * BLUE_SIZE];
	const int n = BLUE_SIZE * BLUE_SIZE;
	float kernel[BLUE_SIZE * BLUE_SIZE];
	for (int y = 0; y < BLUE_SIZE; y++) {
		for (int x = 0; x < BLUE_SIZE; x++) {
			const int dx = MIN(x, BLUE_SIZE - x);
			const int dy = MIN(y, BLUE_SIZE - y);
			kernel[y * BLUE_SIZE + x] = Math::exp(-float(dx * dx + dy * dy) / (2.0f * 1.9f * 1.9f));
		}
	}
	LocalVector<float> energy;
	energy.resize(n);
	for (int i = 0; i < n; i++) {
		energy[i] = float(hash_fmix32(uint32_t(i) * 0x9E3779B9u + 7u) & 0xFFFFu) * 1e-9f; // Breaks ties.
	}
	for (int r = 0; r < n; r++) {
		int best = 0;
		for (int i = 1; i < n; i++) {
			if (energy[i] < energy[best]) {
				best = i;
			}
		}
		ranks[best] = uint16_t(r);
		energy[best] = 1e30f;
		const int bx = best % BLUE_SIZE;
		const int by = best / BLUE_SIZE;
		for (int y = 0; y < BLUE_SIZE; y++) {
			const int ky = ((y - by) & (BLUE_SIZE - 1)) * BLUE_SIZE;
			for (int x = 0; x < BLUE_SIZE; x++) {
				energy[y * BLUE_SIZE + x] += kernel[ky + ((x - bx) & (BLUE_SIZE - 1))];
			}
		}
	}
	return ranks;
}

// Built once, on first use (thread-safe static initialisation): no lock per call, which cost more
// than the rest of a blade's placement.
const uint16_t *blue_ranks() {
	static const uint16_t *ranks = build_blue_ranks();
	return ranks;
}

// A cell's rank in [0, 1): the mask, turned or mirrored per 64-cell block so it does not repeat.
float blue_rank(int p_x, int p_z) {
	const uint16_t *ranks = blue_ranks();
	const uint32_t sym = cell_hash(p_x >> 6, p_z >> 6, 0x27D4EB2Fu) & 7u;
	int x = p_x & (BLUE_SIZE - 1);
	int z = p_z & (BLUE_SIZE - 1);
	if (sym & 1u) {
		x = BLUE_SIZE - 1 - x;
	}
	if (sym & 2u) {
		z = BLUE_SIZE - 1 - z;
	}
	if (sym & 4u) {
		SWAP(x, z);
	}
	return (float(ranks[z * BLUE_SIZE + x]) + 0.5f) / float(BLUE_SIZE * BLUE_SIZE);
}

// GPU tile fill (docs/ideas/grass.md, stage 3a): one compute pipeline for every field, built on
// the render thread the first time a tile is filled.
struct GrassFillGPU {
	RID shader;
	RID pipeline;
	bool tried = false;
	bool ok = false;
};
GrassFillGPU grass_fill_gpu;

constexpr int FILL_HEADER = 48; // grass_fill.glsl's H_* floats.
// Worn paths: walkers a year on a grid this fine (a path's tread is about a metre wide).
constexpr float FILL_WEAR_STEP = 0.25f;
constexpr float FILL_SUIT_STEP = 0.5f;

// A value on a 1 m grid of p_n x p_n over a tile, at tile-local metres.
float grid_bilinear(const LocalVector<float> &p_grid, int p_n, float p_x, float p_z) {
	const float lx = CLAMP(p_x, 0.0f, float(p_n - 1));
	const float lz = CLAMP(p_z, 0.0f, float(p_n - 1));
	const int gx = MIN(int(lx), p_n - 2);
	const int gz = MIN(int(lz), p_n - 2);
	return Math::lerp(Math::lerp(p_grid[gz * p_n + gx], p_grid[gz * p_n + gx + 1], lx - gx), Math::lerp(p_grid[(gz + 1) * p_n + gx], p_grid[(gz + 1) * p_n + gx + 1], lx - gx), lz - gz);
}

void grass_fill_rt(RID p_multimesh, const PackedFloat32Array &p_inputs, int p_count) {
	RenderingServer *rs = RenderingServer::get_singleton();
	RenderingDevice *rd = rs ? rs->get_rendering_device() : nullptr;
	if (!rd) {
		return;
	}
	if (!grass_fill_gpu.tried) {
		grass_fill_gpu.tried = true;
		Ref<RDShaderFile> sf;
		sf.instantiate();
		if (sf->parse_versions_from_text(grass_fill_shader_glsl) == OK) {
			grass_fill_gpu.shader = rd->shader_create_from_spirv(sf->get_spirv_stages());
			if (grass_fill_gpu.shader.is_valid()) {
				grass_fill_gpu.pipeline = rd->compute_pipeline_create(grass_fill_gpu.shader);
			}
		}
		grass_fill_gpu.ok = grass_fill_gpu.pipeline.is_valid();
		ERR_FAIL_COND_MSG(!grass_fill_gpu.ok, "GrassField3D: the tile fill compute shader failed to build.");
	}
	if (!grass_fill_gpu.ok) {
		return;
	}
	const RID records = rs->multimesh_get_buffer_rd_rid(p_multimesh);
	if (!records.is_valid()) {
		return;
	}
	const Vector<uint8_t> bytes = p_inputs.to_byte_array();
	const RID inputs = rd->storage_buffer_create(bytes.size(), bytes);
	Vector<RD::Uniform> uniforms;
	RD::Uniform u0(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, records);
	RD::Uniform u1(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, inputs);
	uniforms.push_back(u0);
	uniforms.push_back(u1);
	const RID set = rd->uniform_set_create(uniforms, grass_fill_gpu.shader, 0);
	const RD::ComputeListID cl = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(cl, grass_fill_gpu.pipeline);
	rd->compute_list_bind_uniform_set(cl, set, 0);
	rd->compute_list_dispatch(cl, (p_count + 63) / 64, 1, 1);
	rd->compute_list_end();
	rd->free_rid(set);
	rd->free_rid(inputs);
	rs->multimesh_mark_buffer_rd_changed(p_multimesh);
}

} // namespace

static void grass_fill_free_rt() {
	RenderingServer *rs = RenderingServer::get_singleton();
	RenderingDevice *rd = rs ? rs->get_rendering_device() : nullptr;
	if (rd && grass_fill_gpu.shader.is_valid()) {
		rd->free_rid(grass_fill_gpu.shader); // Frees the pipeline with it.
	}
	grass_fill_gpu = GrassFillGPU();
}

void GrassField3D::free_gpu_fill() {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs && grass_fill_gpu.shader.is_valid()) {
		rs->call_on_render_thread(callable_mp_static(&grass_fill_free_rt));
	}
}

bool GrassField3D::is_gpu_fill_available() {
	RenderingServer *rs = RenderingServer::get_singleton();
	return rs && rs->get_rendering_device() != nullptr && !(grass_fill_gpu.tried && !grass_fill_gpu.ok);
}

// Shared by the blades and the ground under them (get_ground_shader_code()), so both drift to the
// same colours at the same places: a gap between blades then shows the base of the same sward.
// Soft edges (docs/ideas/grass.md, "Colour without blotches"): where the rules thin the sward the blades left grow
// shorter too, half height at the edge, so a patch or a fade to other ground tapers instead of ending in a wall of
// full-height blades. grass_fill.glsl does the same.
static inline float _edge_height(float p_suit) {
	// In sixteenths, so the GPU fill (its suitability from a 0.5 m grid) gives the same heights as this.
	return Math::round(Math::lerp(0.5f, 1.0f, Math::smoothstep(0.0f, 0.7f, p_suit)) * 16.0f) / 16.0f;
}

static const char *GRASS_COLOR_CODE = R"(
float grass_hash(vec2 p) {
	return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float grass_noise(vec2 p) {
	vec2 i = floor(p);
	vec2 f = fract(p);
	f = f * f * (3.0 - 2.0 * f);
	return mix(mix(grass_hash(i), grass_hash(i + vec2(1.0, 0.0)), f.x), mix(grass_hash(i + vec2(0.0, 1.0)), grass_hash(i + vec2(1.0, 1.0)), f.x), f.y);
}

// Colour variation (docs/ideas/grass.md, "Colour without blotches"): a meadow's colour drifts slowly over tens of
// metres (moisture, soil, the species mix) and varies strongly only from one blade or tuft to the next. Strong hue
// patches a few metres across read as camouflage blotches, so the hue is gentle and large-scale: two rotated, domain-
// warped octaves at about 70 m and 25 m (no square-lattice blobs), soft at their extremes (no flat-tinted plateaus).
// The blade's own value carries the fine variation, and the ground under the blades takes the same field.
vec2 grass_rot(vec2 p, float a) {
	float s = sin(a), c = cos(a);
	return vec2(c * p.x - s * p.y, s * p.x + c * p.y);
}

vec3 grass_patch_tint(vec2 xz, float blade, float value, float variation) {
	vec2 warp = vec2(grass_noise(grass_rot(xz, 0.6) * 0.021 + 5.2), grass_noise(grass_rot(xz, 2.1) * 0.021 - 7.9)) - 0.5;
	vec2 q = xz + warp * 30.0;
	float drift = grass_noise(grass_rot(q, 0.37) * 0.0143) * 0.65 + grass_noise(grass_rot(q, 1.91) * 0.04 + 3.7) * 0.35;
	// Soft response: tanh-like, so the extremes ease off instead of saturating to one flat tint.
	float d = (drift - 0.5) * 2.2;
	d = d / (1.0 + abs(d));
	float hue = d * 0.55 + (blade - 0.5) * 0.6;
	vec3 warm = vec3(1.14, 1.06, 0.7); // yellower, drier
	vec3 cool = vec3(0.86, 0.97, 1.02); // bluer, lusher
	vec3 tint = hue > 0.0 ? mix(vec3(1.0), warm, min(hue, 1.0)) : mix(vec3(1.0), cool, min(-hue, 1.0));
	tint *= 0.8 + 0.4 * value;
	return mix(vec3(1.0), tint, variation);
}
)";

// The path tracer merges a MultiMesh into one BLAS only under a triangle cap; above it every blade becomes its own
// TLAS record, far slower to build and trace (grass.md, stage 3). A full tile over the cap: the tiles shrink to fit
// (the same blades, only cut into more tiles), to the half metre under the fit.
void GrassField3D::_fit_tile_size(int p_cap, int p_tris) {
	const float fit = Math::sqrt(float(p_cap) / MAX(density * float(p_tris), 1e-3f));
	const float fitted = MAX(Math::floor(fit * 2.0f) * 0.5f, 1.0f);
	if (fitted < tile_size) {
		print_verbose(vformat("GrassField3D \"%s\": a full %.1f m tile holds about %d triangles, over the path tracer's merged BLAS cap (%d); tiles of %.1f m instead.", get_name(), tile_size, int(density * tile_size * tile_size * p_tris), p_cap, fitted));
		tile_size = fitted;
		// Tiles cut at the old size would overlap the new ones: dropped, rebuilt nearest first.
		for (KeyValue<Vector2i, Tile> &kv : tiles) {
			_free_tile(kv.value);
		}
		tiles.clear();
	}
}

void GrassField3D::_ensure_resources() {
	if (blade_mesh.is_null() && crop != CROP_NONE) {
		blade_mesh = _make_crop_mesh(false);
		crop_mesh_far = _make_crop_mesh(true);
		const int tris = blade_mesh->surface_get_array_index_len(0) / 3;
		const int cap = int(GLOBAL_GET("rendering/pathtracing/multimesh_merged_blas_max_triangles"));
		const float tile_tris = density * tile_size * tile_size * tris;
		if (cap > 0 && tile_tris > cap) {
			_fit_tile_size(cap, tris);
		}
	}
	if (blade_mesh.is_null()) {
		// A strip of blade_segments quads and a tip: VERTEX.x is the side (-0.5..0.5), VERTEX.y
		// the parameter along the blade; the vertex shader builds the real blade from them.
		PackedVector3Array verts;
		PackedVector2Array uvs;
		PackedInt32Array idx;
		for (int i = 0; i < blade_segments; i++) {
			const float t = float(i) / blade_segments;
			verts.push_back(Vector3(-0.5f, t, 0.0f));
			verts.push_back(Vector3(0.5f, t, 0.0f));
			uvs.push_back(Vector2(0.0f, t));
			uvs.push_back(Vector2(1.0f, t));
		}
		verts.push_back(Vector3(0.0f, 1.0f, 0.0f));
		uvs.push_back(Vector2(0.5f, 1.0f));
		for (int i = 0; i < blade_segments - 1; i++) {
			const int a = i * 2;
			idx.append_array(PackedInt32Array({ a, a + 2, a + 1, a + 1, a + 2, a + 3 }));
		}
		const int last = (blade_segments - 1) * 2;
		idx.append_array(PackedInt32Array({ last, last + 2, last + 1 }));
		PackedVector3Array normals;
		normals.resize(verts.size());
		normals.fill(Vector3(0, 0, 1));
		Array arrays;
		arrays.resize(Mesh::ARRAY_MAX);
		arrays[Mesh::ARRAY_VERTEX] = verts;
		arrays[Mesh::ARRAY_NORMAL] = normals;
		arrays[Mesh::ARRAY_TEX_UV] = uvs;
		arrays[Mesh::ARRAY_INDEX] = idx;
		blade_mesh.instantiate();
		blade_mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
		// Bent and scaled in the shader: bounds for the tallest blade bent any way.
		const float h = MAX(blade_height.x, blade_height.y) * 1.1f;
		blade_mesh->set_custom_aabb(AABB(Vector3(-h, -0.05f, -h), Vector3(2.0f * h, h + 0.1f, 2.0f * h)));
		// The path tracer merges a MultiMesh into one BLAS only under a triangle cap; above it every
		// blade becomes its own TLAS record, far slower to build and trace (grass.md, stage 3).
		const int tris = 2 * (blade_segments - 1) + 1;
		const int cap = int(GLOBAL_GET("rendering/pathtracing/multimesh_merged_blas_max_triangles"));
		const float tile_tris = density * tile_size * tile_size * tris;
		if (cap > 0 && tile_tris > cap) {
			_fit_tile_size(cap, tris);
		}
	}
	if (material.is_null()) {
		_build_material();
	}
}

// The drawn ground's height over the heightfield at p's xz (Landscape3D's bank and cliff relief, WorldGround.relief_at):
// 0 with no relief producer or out of its reach.
// p_steps false: without the terracettes' steps, for blades, whose shader adds them at each root (wc_terracette_step).
static float _relief_y(const Vector3 &p_at, bool p_steps = true) {
	const float r = WorldContracts::ground()->relief_at(p_at, p_steps).x;
	return Math::is_finite(r) ? r : 0.0f;
}

String GrassField3D::add_camera_cutout(const String &p_code) {
	String code = p_code;
	int at = code.find("\nvoid ");
	if (at < 0) {
		return code;
	}
	code = code.insert(at + 1, BaseMaterial3D::get_camera_cutout_shader_code() + "\n");
	const String head = "void fragment() {";
	const String cut = "\n\t// The camera cut-out (camera_cutout global): first, so nothing else runs where the surface is cut.\n\tif (!IN_SHADOW_PASS && camera_cutout.w > 0.0 && camera_cutout_discard((INV_VIEW_MATRIX * vec4(VERTEX, 1.0)).xyz, INV_VIEW_MATRIX, PROJECTION_MATRIX, FRAGCOORD.xy)) {\n\t\tdiscard;\n\t}";
	int from = 0;
	while ((at = code.find(head, from)) >= 0) {
		code = code.insert(at + head.length(), cut);
		from = at + head.length() + cut.length();
	}
	return code;
}

String GrassField3D::get_ground_opened_shader_code() {
	return R"(
global uniform sampler2D ground_deform_map;
global uniform vec4 ground_deform_rect;
global uniform sampler2D ground_deform_near_map;
global uniform vec4 ground_deform_near_rect;
global uniform sampler2D ground_wet_map;
#ifndef GROUND_WEAR_CODE
#define GROUND_WEAR_CODE
global uniform sampler2D ground_wear_map : filter_linear, repeat_disable;
global uniform vec4 ground_wear_rect;
// Worn-in tracks (mud round 14.2): the sward's cover left by the wheel passes GroundSurface3D counted at p_xz
// (ground_wear_map). A wheel bears and scuffs as some twenty boots do, so the passes count twenty times against the
// sward's p_resistance (GrassField3D.trample_resistance, foot passes; Cole 1995's cover, as _trample_cover()).
float grass_ground_worn(vec2 p_xz, float p_resistance) {
	if (ground_wear_rect.w <= 0.0 || ground_wear_rect.z <= 0.0) {
		return 1.0;
	}
	vec2 uv = (p_xz - ground_wear_rect.xy) / ground_wear_rect.z;
	if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
		return 1.0;
	}
	float passes = textureLod(ground_wear_map, uv, 0.0).r;
	float x = 20.0 * passes / max(p_resistance, 1.0);
	return x > 0.0 ? (1.0 - smoothstep(2.5, 5.0, x)) / (1.0 + x * x) : 1.0;
}
#endif
// Mud thrown or splashed onto the sward and the plants at p_xz (mud round 7): the share of their faces spattered and
// the clay's linear colour, from the ground's wet map (its block tagged 6.5; GroundSurface3D::_wet_publish()).
vec4 grass_ground_mud(vec2 p_xz) {
	ivec2 ts = textureSize(ground_wet_map, 0);
	if (ts.x < 5 || ts.y < 2) {
		return vec4(0.0);
	}
	vec4 r = texelFetch(ground_wet_map, ivec2(0, ts.y - 1), 0);
	if (abs(r.w - 2.5) > 0.01 || abs(texelFetch(ground_wet_map, ivec2(4, ts.y - 1), 0).w - 6.5) > 0.01 || r.z <= 0.0) {
		return vec4(0.0);
	}
	vec2 uv = (p_xz - r.xy) / r.z;
	if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
		return vec4(0.0);
	}
	int blocks = 1;
	for (int k = 1; k < 7; k++) {
		blocks += abs(texelFetch(ground_wet_map, ivec2(k, ts.y - 1), 0).w - (2.5 + float(k))) < 0.01 ? 1 : 0;
	}
	// The plant mud block, before a fresh deposit's (texel 5's tag 7.5) and a salt crust's (texel 6's 8.5) when there
	// are any.
	int at = blocks - 1 - (ts.x > 5 && abs(texelFetch(ground_wet_map, ivec2(5, ts.y - 1), 0).w - 7.5) < 0.01 ? 1 : 0)
			- (ts.x > 6 && abs(texelFetch(ground_wet_map, ivec2(6, ts.y - 1), 0).w - 8.5) < 0.01 ? 1 : 0);
	float rows = float((ts.y - 1) / blocks);
	float v = clamp(uv.y * rows, 0.5, rows - 0.5) + float(at) * rows;
	return textureLod(ground_wet_map, vec2(uv.x, v / float(ts.y)), 0.0);
}
// What a rut, dig or puddle does to the sward at p_xz: x lays it flat (a print, a tyre's light pass, a mud film), y takes
// it away (a rut or dig past a centimetre or so, standing water), 0 to 1 each; z how far
// the ground went down there (m, negative on a rim), so the blades left stand on it.
vec3 grass_ground_opened(vec2 p_xz) {
	if (ground_deform_rect.z <= 0.0) {
		float worn = grass_ground_worn(p_xz, 300.0);
		return vec3(smoothstep(1.0, 0.6, worn), 1.0 - worn, 0.0);
	}
	vec2 uv = (p_xz - ground_deform_rect.xy) / ground_deform_rect.z;
	if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
		float worn = grass_ground_worn(p_xz, 300.0);
		return vec3(smoothstep(1.0, 0.6, worn), 1.0 - worn, 0.0);
	}
	vec4 d = textureLod(ground_deform_map, uv, 0.0);
	if (ground_deform_near_rect.z > 0.0) {
		vec2 e = min(p_xz - ground_deform_near_rect.xy, ground_deform_near_rect.xy + ground_deform_near_rect.z - p_xz);
		float nw = smoothstep(0.0, 1.0, min(e.x, e.y));
		if (nw > 0.0) {
			d = mix(d, textureLod(ground_deform_near_map, (p_xz - ground_deform_near_rect.xy) / ground_deform_near_rect.z, 0.0), nw);
		}
	}
	// Any fresh print or film lays the sward over (the ground's fresh tint, B); a light pass leaves it there, flattened,
	// as on a meadow a tyre has crossed once. Pressed in past a centimetre or so, or under water, it is torn out.
	// Grown back over since (mud round 6: GroundSurface3D's kept ruts age; A just over 1): torn blades return, flattened
	// ones stand up four times sooner.
	float regrown = clamp((d.a - 1.0) / 0.02, 0.0, 1.0);
	// A film of clay thrown onto the sward (mud round 7: it spattered the blades, grass_ground_mud()) came from the air:
	// it lays nothing over.
	float thrown = grass_ground_mud(p_xz).x;
	float flat_ = max(smoothstep(0.002, 0.008, abs(d.r)) * (1.0 - min(4.0 * regrown, 1.0)), smoothstep(0.03, 0.15, abs(1.0 - d.b)) * (1.0 - thrown));
	float relief = smoothstep(0.01, 0.04, abs(d.r)) * (1.0 - regrown);
	float water = d.a < 0.0 ? 1.0 : smoothstep(5e-4, 3e-3, d.g);
	// A worn-in track (mud round 14.2): what the passes left lies flat, the rest is gone.
	float worn = grass_ground_worn(p_xz, 300.0);
	if (worn < 1.0) {
		flat_ = max(flat_, smoothstep(1.0, 0.6, worn));
		water = max(water, 1.0 - worn);
	}
	return vec3(flat_, max(relief, water), d.r);
}
// Spatter on a face p_up metres above the ground (thrown clay lands thickest low down, splashes reach 30 to 50 cm), at
// world point p_w: specks that join into a coat as the share grows, the clay's colour a little darkened. Applied to
// the albedo; returns the share covered (the caller takes its roughness to the clay's matte 0.85).
float grass_mud_apply(vec4 p_mud, vec3 p_w, float p_up, inout vec3 r_albedo) {
	if (p_mud.x <= 0.0) {
		return 0.0;
	}
	float reach = mix(0.12, 0.45, p_mud.x);
	float m = p_mud.x * (1.0 - smoothstep(0.3 * reach, reach, p_up));
	float speck = fract(sin(dot(floor(p_w * 160.0), vec3(12.9898, 78.233, 37.719))) * 43758.5453);
	float cover = smoothstep(speck - 0.15, speck + 0.15, 1.3 * m);
	r_albedo = mix(r_albedo, p_mud.yzw * 0.8, cover);
	return cover;
}
)";
}

void GrassField3D::_build_material() {
	String code = "shader_type spatial;\nrender_mode cull_disabled;\n\n";
	code += "// GrassField3D blades (docs/ideas/grass.md): quadratic Bezier bent by the wind, after Ghost of Tsushima.\n";
	code += "uniform vec3 base_color : source_color;\nuniform vec3 tip_color : source_color;\nuniform vec3 dry_color : source_color;\n";
	code += "uniform vec2 fallback_wind;\nuniform float wind_response;\nuniform float view_facing;\nuniform vec4 fade_band;\n";
	// The far rings (docs/ideas/grass.md, "Far grass"): their start (where every ring blade is kept), end, end band
	// and the most a blade widens.
	code += "uniform vec4 ring_band;\n";
	// The ground's normal for the far blades (docs/ideas/vegetation.md, "Next round", 2): the data map's heights
	// (TerrainDataMap3D.get_data_texture(), r: height) over ground_data_rect (z 0: none, the blades ease to up).
	code += "uniform sampler2D ground_data_map : filter_linear, repeat_disable, hint_default_black;\nuniform vec4 ground_data_rect;\n";
	code += "uniform float dormant; // Share of blades dead to the crown in a drought or hard winter (season).\n";
	code += "uniform float season_height = 1.0; // The sward's standing height over its summer height (GrassField3D.season_height_for).\n";
	// Storm lodging (lodging): r, g the direction the blades lie (* 0.5 + 0.5), b the lodged share; rect z 0: none.
	code += "uniform sampler2D lodge_map : filter_linear, repeat_disable, hint_default_black;\nuniform vec4 lodge_rect;\n";
	// Meadow fairy rings (fairy_rings.h): 1 on, 0 off.
	code += "uniform float fairy_rings = 1.0;\n";
	// After the snow (GrassField3D::_update_snowmelt()): per height band (melt_band0.x the first band's height, y the
	// step), x the days the last lasting snow lay, y the days since it melted out (< 0 none); snowmelt 1 on, 0 off.
	code += "uniform float snowmelt = 1.0;\nuniform vec2 melt_band0 = vec2(0.0, 50.0);\nuniform vec2 melt_bands[16];\n";
	// The last flood's mark (GrassField3D::_flood_step()): r the peak surface (m), g days since it, b the flow's angle
	// then (> 50 unknown), a 1 where known; flood_rect z 0: none.
	code += "uniform sampler2D flood_map : filter_linear, repeat_disable, hint_default_black;\nuniform vec4 flood_rect;\n";
	code += FairyRings::GLSL;
	// Terracettes on steep slopes (1 on, 0 off).
	code += "uniform float terracettes = 1.0;\n";
	// Deer beds (deer_beds.h): 1 on, 0 off; deer_bed_day the season's day (the beds' ages).
	code += "uniform float deer_beds = 1.0;\nuniform float deer_bed_day = 180.0;\n";
	code += DeerBeds::GLSL;
	code += "uniform sampler2D trample_map : filter_linear, repeat_disable;\nuniform vec4 trample_rect;\n";
	// Regrowth flush on burnt ground (burn_recovery): 1 on a fresh burn, 0 by a year and a half; rect z 0: none.
	code += "uniform sampler2D burn_flush_map : filter_linear, repeat_disable, hint_default_black;\nuniform vec4 burn_flush_rect;\n";
	// Deer lawns (graze_height): r = how far the sward is grazed down, 0 none to 1 to graze_height; rect z 0: none.
	code += "uniform sampler2D graze_map : filter_linear, repeat_disable, hint_default_black;\nuniform vec4 graze_rect;\nuniform float graze_height = 0.04;\n";
	// Hay meadows (hay_meadows): r = the meadow (index + 1, 0 none) at each texel; mow_state a texel a meadow (r days since mown, below 0 not; g the rows' angle; b mm of rain on the cut grass); rect z 0: none.
	code += "uniform sampler2D mow_map : filter_nearest, repeat_disable, hint_default_black;\nuniform sampler2D mow_state : filter_nearest, repeat_disable, hint_default_black;\nuniform vec4 mow_rect;\n";
	// Bare soil by the climate's dryness (WorldContracts::bare_patch, the terrain material's patches): no sward there.
	// The terrain material module registers these globals.
	code += "uniform bool climate_bare = true;\nglobal uniform sampler2D terrain_climate_map : hint_default_black, filter_linear;\nglobal uniform vec4 terrain_climate_rect;\nglobal uniform vec4 weather_climate;\n";
	code += WorldContracts::climate_bare_shader_code();
	code += WorldContracts::terracette_shader_code();
	// The ground's terracette steps (WorldContracts::terracette_step): Landscape3D's share in terrain_bank_relief.y, gated
	// to open soil by the generator's geology (terrain_deform and terrain_material register these globals).
	code += R"(global uniform vec4 terrain_bank_relief;
global uniform sampler2D terrain_geology_map;
global uniform vec4 terrain_geology_rect;
// The steps' height over the smooth ground at a blade's root (m), as the ground draws them: the heights the blades are
// placed on leave the steps out (a 3 m pattern on metre texels aliases), so each root adds its own.
float grass_terracette_lift(vec3 root) {
	if (terrain_bank_relief.y <= 0.0 || ground_data_rect.z <= 0.0) {
		return 0.0;
	}
	vec2 ts = vec2(textureSize(ground_data_map, 0));
	vec2 uv = (root.xz - ground_data_rect.xy) / ground_data_rect.zw;
	vec2 du = vec2(1.0 / ts.x, 0.0);
	vec2 dv = vec2(0.0, 1.0 / ts.y);
	vec2 cell = 2.0 * ground_data_rect.zw / ts;
	vec2 grad = vec2(textureLod(ground_data_map, uv + du, 0.0).r - textureLod(ground_data_map, uv - du, 0.0).r, textureLod(ground_data_map, uv + dv, 0.0).r - textureLod(ground_data_map, uv - dv, 0.0).r) / cell;
	// The heightfield's own height, as Landscape3D phases the steps (its relief left out).
	vec3 p = vec3(root.x, textureLod(ground_data_map, uv, 0.0).r, root.z);
	float share = terrain_bank_relief.y;
	if (terrain_geology_rect.z > 0.0) {
		vec2 gs = vec2(textureSize(terrain_geology_map, 0));
		vec2 g = clamp((root.xz - terrain_geology_rect.xy) / terrain_geology_rect.zw, 0.0, 1.0);
		share *= wc_terracette_soil(textureLod(terrain_geology_map, (g * (gs - 1.0) + 0.5) / gs, 0.0));
	}
	// Filtered over half a metre, as Landscape3D draws the near ground.
	return wc_terracette_step(p, grad, 0.5, share).x;
}
)";
	code += "global uniform sampler2D weather_state_map;\nglobal uniform sampler2D weather_contact_map;\nglobal uniform vec4 weather_contact_rect;\nglobal uniform sampler2D weather_frost_map;\nglobal uniform sampler2D weather_spray_map;\nglobal uniform vec4 weather_spray_rect;\n";
	// The weather module declares its globals, so this compiles with or without a WeatherMap3D.
	// Sea salt on the blades (WeatherMap3D sea_spray, G: 1 - exp(-salt / 10 g/m2)): salt spray burns leaf tips brown
	// on the windward shore (Boyce 1954; foliar chloride necrosis), from a couple of grams per square metre.
	code += R"(
float grass_salt_burn(vec2 p_xz) {
	if (weather_spray_rect.z <= 0.0) {
		return 0.0;
	}
	vec2 uv = (p_xz - weather_spray_rect.xy) / weather_spray_rect.zw;
	vec2 e = min(uv, 1.0 - uv);
	float edge = smoothstep(0.0, 0.05, min(e.x, e.y));
	return edge > 0.0 ? smoothstep(0.15, 0.85, textureLod(weather_spray_map, uv, 0.0).g) * edge : 0.0;
}
)";
	code += VegetationFire::WIND_CODE;
	code += WeatherMap3D::get_wind_shader_code();
	code += vformat("#define WEATHER_GUST_COUNT %d\n#define WEATHER_GUSTS_PER_AXIS %d\n", WeatherMap3D::GUST_COUNT, WeatherMap3D::GUSTS_PER_AXIS);
	// The ground's ruts, digs and puddles (GroundSurface3D; material.cpp ground_deform_exposed()), mud round 5.
	code += get_ground_opened_shader_code();
	// The snowpack (SnowCover3D globals; material.cpp snow_pack_at(), snow_press()): it presses the sward flat.
	code += BaseMaterial3D::get_snow_functions_code(true);
	code += get_snow_sward_shader_code();
	code += R"(
// weather_wind() as felt by a blade: a damped oscillator of angular frequency omega0 and damping
// zeta (Py, de Langre and Moulia 2006). The mean wind and the gust patches are slow and pass as
// they are; each Kaimal gust wave goes through the oscillator's transfer function (gain capped at
// the quasi-static 1, with its lag), as TreeWind3D does for branches. On top, the resonant sway
// Davenport's resonant part gives, locked in across the sward: it travels downwind at the canopy's
// convection speed with wavelength Uc / f0, the honami. jitter (0..1) shifts a clump's phase a
// little so the wave is not one sheet.
vec3 grass_weather_wind(vec3 wp, float height, float omega0, float zeta, float jitter, float time) {
	if (weather_map_rect.z <= 0.0 || weather_map_rect.w <= 0.0) {
		return vec3(0.0);
	}
	float z0 = max(weather_wind_info.w, 1e-5);
	vec2 m10 = textureLod(weather_wind_map, (wp.xz - weather_map_rect.xy) / weather_map_rect.zw, 0.0).rg;
	vec4 sh = vec4(0.0, 0.0, 1.0, 0.0);
	if (weather_shelter_rect.z > 0.0) {
		vec2 su = (wp.xz - weather_shelter_rect.xy) / weather_shelter_rect.zw;
		if (su.x >= 0.0 && su.y >= 0.0 && su.x <= 1.0 && su.y <= 1.0) {
			sh = textureLod(weather_shelter_map, su, 0.0);
		}
	}
	float z = max(height, 1.5 * z0);
	float f = weather_shelter_factor(z, z0, sh);
	float profile = weather_open_profile(z, z0);
	vec2 m = m10 * (profile * f * weather_patch(wp.xz, z));
	vec3 v = vec3(m.x, 0.0, m.y);
	float speed = length(m10);
	if (speed < 1e-3 || weather_wind_info.z <= 0.0) {
		return v;
	}
	float sigma = weather_wind_info.z * speed * sqrt(f);
	vec2 ea = weather_wind_info.xy;
	vec2 ec = vec2(-ea.y, ea.x);
	float a = dot(wp.xz, ea);
	float cr = dot(wp.xz, ec);
	vec3 g = vec3(0.0);
	for (int i = 0; i < WEATHER_GUST_COUNT; i++) {
		vec4 w = texelFetch(weather_gusts, ivec2(i, 0), 0);
		float r = abs(w.x) * speed / omega0;
		float ph = w.x * a + w.y * cr + w.w;
		float c;
		if (r < 0.25) {
			// Well below resonance: gain within 7% of 1, lag under 3 degrees.
			c = w.z * cos(ph);
		} else if (r > 4.0) {
			// Filtered out: gain under 1/15.
			c = 0.0;
		} else {
			float re = 1.0 - r * r;
			float im = 2.0 * zeta * r;
			float mag = max(sqrt(re * re + im * im), 1e-4);
			c = w.z * min(1.0 / mag, 1.0) / mag * (cos(ph) * re + sin(ph) * im);
		}
		if (i < WEATHER_GUSTS_PER_AXIS) {
			g.x += c;
		} else if (i < 2 * WEATHER_GUSTS_PER_AXIS) {
			g.y += c;
		} else {
			g.z += c;
		}
	}
	vec2 along = m10 / speed;
	vec2 across = vec2(-along.y, along.x);
	// Resonance: Kaimal along-wind density at f0 (integral length 8.1 x 7 m at 10 m, as the gust
	// waves), over the oscillator's band pi f0 / (4 zeta), as two waves at omega0 (1 -+ zeta) for
	// the beating envelope, both travelling at the canopy's convection speed (1.8 U(h), Raupach
	// et al. 1996).
	float f0 = omega0 / 6.2832;
	float lu = 56.7 / speed;
	float density = sigma * sigma * 4.0 * lu / pow(1.0 + 6.0 * f0 * lu, 5.0 / 3.0);
	float amp = sqrt(density * 3.1416 * f0 / (4.0 * zeta));
	float uc = max(1.8 * profile * f * speed, 0.3);
	float wa = omega0 * (1.0 - zeta);
	float wb = omega0 * (1.0 + zeta);
	// The fronts are not straight lines a sward wide: coherent structures in a canopy are a few
	// heights across and break up (Finnigan 2000; Py et al. 2006 measured honami patches of a few
	// metres), so the front is bent by a slow crosswind warp and the wave's strength comes and goes
	// in patches. Straight, even fronts read as stripes, and in perspective as concentric arcs
	// (docs 20 Nov).
	float across_pos = dot(wp.xz, across);
	float along_pos = dot(wp.xz, along);
	along_pos += 1.6 * sin(across_pos * 0.43 + 1.3 * sin(along_pos * 0.21)) + 0.9 * sin(across_pos * 1.07 - along_pos * 0.37 + 2.0);
	float patchy = 0.55 + 0.45 * sin(across_pos * 0.29 + along_pos * 0.17 - time * 0.13) * sin(across_pos * 0.61 - along_pos * 0.23 + 1.7);
	float res = amp * patchy * (cos(wa * (time - along_pos / uc) + jitter * 2.0) + cos(wb * (time - along_pos / uc) + 2.1 + jitter * 2.0));
	vec2 hz = (along * g.x + across * g.y) * sigma + along * res;
	return v + vec3(hz.x, g.z * sigma, hz.y);
}

// The weather map's wind; without one, fallback_wind with gusts rolling downwind. A fire's indraft adds to either.
vec3 grass_wind_ambient(vec3 wp, float h, float time, float omega0, float jitter) {
	if (weather_map_rect.z > 0.0) {
		return grass_weather_wind(wp, h, omega0, 0.1, jitter, time);
	}
	float s = length(fallback_wind);
	if (s < 1e-4) {
		return vec3(0.0);
	}
	vec2 d = fallback_wind / s;
	// Gusts as bent, patchy fronts, not straight bands (see grass_weather_wind()).
	float al = dot(wp.xz, d);
	float cr = dot(wp.xz, vec2(-d.y, d.x));
	al += 1.6 * sin(cr * 0.43 + 1.3 * sin(al * 0.21)) + 0.9 * sin(cr * 1.07 - al * 0.37 + 2.0);
	float phase = al * 0.12 - time * s * 0.12;
	float g = 1.0 + 0.45 * sin(phase) * (0.6 + 0.4 * sin(cr * 0.23 + al * 0.11 - time * 0.07)) + 0.2 * sin(phase * 2.7 + cr * 0.2) + 0.12 * (jitter - 0.5);
	return vec3(fallback_wind.x, 0.0, fallback_wind.y) * g;
}
vec3 grass_wind(vec3 wp, float h, float time, float omega0, float jitter) {
	return grass_wind_ambient(wp, h, time, omega0, jitter) + fire_wind_at(wp.xz);
}
)";
	if (crop != CROP_NONE) {
		code += R"(
// Crop tillers (farmland stage 1): _make_crop_mesh() parts built here from the stage uniforms.
varying float v_t;
varying float v_part;
varying float v_ripe;
varying float v_dead;
varying float v_wet;
varying float v_frost;
varying float v_salt;
varying float v_snow; // under the snow surface: 1
varying vec3 v_tint;
varying float v_light;
varying float v_hf;
varying float v_rnd;

uniform float color_variation;
// The growth stage (BBCH) of each plot: 0 the node's crop_stage, 1 on its fields in order.
// A texel a plot (not a uniform array: 64 floats pad to 1 KB in std140, over the path tracer's
// material slot).
uniform sampler2D crop_plot_stage : filter_nearest, repeat_disable;
uniform float crop_nod; // How far the ripe ear bends over (0 upright, 1 hanging).
uniform vec4 crop_ear; // Ear length, half width, awn length, panicle branches.
uniform vec4 crop_leaf; // Leaf length, half width, stem radius.
uniform vec3 crop_green : source_color;
uniform vec3 crop_ripe : source_color;
uniform vec3 crop_straw : source_color;
uniform float crop_lai = 3.0;
)";
		code += GRASS_COLOR_CODE;
		code += VegetationLight::SHADER_CODE;
		code += VegetationFire::SHADER_CODE;
		code += VegetationFire::ASH_CODE;
		code += R"(
float crop_ramp(float x, float a, float b) {
	return clamp((x - a) / (b - a), 0.0, 1.0);
}

// The look at a growth stage (BBCH decimal code, Zadoks et al. 1974; Lancashire et al. 1991):
// leaves unfold from 10, the stem elongates from 30, the ear emerges from 49 to 59; from late milk
// (75) the ear and then the straw turn from green to gold while the leaves die from the bottom up,
// and the ripe ear nods; at 99 the crop is cut to stubble. growth: stem share of its ripe height,
// leaf scale, ear emerged (0..1), nod (0..1); age: ear ripeness, straw ripeness, dead leaf share, cut
// height (m; 0 standing).
void crop_look(float s, out vec4 growth, out vec4 age) {
	float stem = s < 30.0 ? 0.0 : (s < 39.0 ? 0.55 * crop_ramp(s, 30.0, 39.0) : (s < 51.0 ? mix(0.55, 0.9, crop_ramp(s, 39.0, 51.0)) : mix(0.9, 1.0, crop_ramp(s, 51.0, 65.0))));
	float leaf = s < 21.0 ? mix(0.25, 0.6, crop_ramp(s, 10.0, 21.0)) : (s < 30.0 ? mix(0.6, 0.7, crop_ramp(s, 21.0, 30.0)) : mix(0.7, 1.0, crop_ramp(s, 30.0, 39.0)));
	float ear = crop_ramp(s, 49.0, 59.0);
	float nod = smoothstep(0.0, 1.0, crop_ramp(s, 73.0, 89.0)) * crop_nod;
	float dead = s < 55.0 ? 0.0 : (s < 75.0 ? 0.5 * crop_ramp(s, 55.0, 75.0) : mix(0.5, 1.0, crop_ramp(s, 75.0, 85.0)));
	if (s >= 99.0) {
		growth = vec4(stem, leaf, 0.0, nod);
		age = vec4(1.0, 1.0, 1.0, 0.15);
	} else {
		growth = vec4(stem, leaf, ear, nod);
		age = vec4(crop_ramp(s, 75.0, 89.0), crop_ramp(s, 77.0, 91.0), dead, 0.0);
	}
}

vec3 crop_bez(vec3 a, vec3 b, vec3 c, float t) {
	return mix(mix(a, b, t), mix(b, c, t), t);
}
vec3 crop_bez_d(vec3 a, vec3 b, vec3 c, float t) {
	return 2.0 * mix(b - a, c - b, t) + vec3(0.0, 1e-5, 0.0);
}

// Spikelet k of n on a spike (axis top, e1, e2): its base on the rachis, its direction, length and
// the side it grows to. Two alternate rows on the ear's wide face (b1), angled out and up,
// overlapping like tiles, smaller at both ends of the ear.
void crop_spikelet(float k, float n, vec3 top, vec3 e1, vec3 e2, float ear_len, float half_w, float rnd, out vec3 r_base, out vec3 r_dir, out float r_len, out vec3 r_out, out float r_prof) {
	float tk = (k + 0.5) / n * 0.94 + 0.02;
	vec3 c = crop_bez(top, e1, e2, tk);
	vec3 tg = normalize(crop_bez_d(top, e1, e2, tk));
	vec3 b1 = normalize(cross(tg, abs(tg.y) < 0.95 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
	vec3 b2 = cross(b1, tg);
	float side = mod(k, 2.0) < 0.5 ? 1.0 : -1.0;
	r_out = normalize(b1 * side + b2 * (fract(k * 0.37 + rnd) - 0.5) * 0.35);
	r_prof = mix(0.65, 1.0, smoothstep(0.0, 0.2, tk)) * (1.0 - 0.45 * smoothstep(0.7, 1.0, tk));
	r_len = ear_len / n * 2.5 * r_prof;
	r_dir = normalize(tg + r_out * 0.42);
	r_base = c + r_out * half_w * 0.2;
}

void vertex() {
	float H = INSTANCE_CUSTOM.x;
	float light_q = floor(INSTANCE_CUSTOM.z / 4.0);
	float rest_lean = INSTANCE_CUSTOM.z - light_q * 4.0;
	v_light = light_q / 100.0 * 1.5;
	float rnd = fract(INSTANCE_CUSTOM.w);
	v_rnd = rnd;
	// y: the plot in its integer part over 4, the bulk (thicker where fewer remain) below 4.
	float plot = floor(INSTANCE_CUSTOM.y / 4.0);
	float bulk = INSTANCE_CUSTOM.y - plot * 4.0;
	// Plots from 64 up: loose straw the combine left lying in its swath, not a standing tiller.
	bool straw = plot > 63.5;
	plot -= straw ? 64.0 : 0.0;
	float stage = texelFetch(crop_plot_stage, ivec2(clamp(int(plot), 0, 63), 0), 0).r;
	vec4 crop_growth;
	vec4 crop_age;
	crop_look(stage, crop_growth, crop_age);
	int part = int(VERTEX.z + 0.5);
	float a = VERTEX.x;
	float t = VERTEX.y;
	float idx = UV2.x;
	float cnt = max(UV2.y, 1.0);
	vec3 base_w = (MODEL_MATRIX * vec4(0.0, 0.0, 0.0, 1.0)).xyz;
	mat3 to_local = inverse(mat3(MODEL_MATRIX));
	vec3 cam_l = to_local * (MAIN_CAM_INV_VIEW_MATRIX[3].xyz - base_w);
	float cut = crop_age.w;
	// Late tillers lag behind: a ripening field keeps some greener ears and stems.
	float lag = (fract(rnd * 5.17) - 0.5) * 0.3;
	float h = cut > 0.0 ? min(H, cut * (0.8 + 0.4 * fract(rnd * 3.7))) : H * crop_growth.x;
	h = straw ? H : h;
	// Fire (vegetation_fire.h): a crop burns to stubble a hand high, its leaves and ears gone.
	float fire_gone = smoothstep(0.0, 0.85, fire_state_at(base_w.xz).x);
	h = mix(h, min(h, 0.06 + 0.06 * fract(rnd * 29.3)), fire_gone);

	// Wind: the tiller sways as a cantilever with the ear's mass on top, 1 to 2 Hz (Berry et al.
	// 2003, wheat stems); the same oscillator and honami as the grass. Stubble barely moves.
	float omega0 = 6.2832 * (1.1 + 0.4 * rnd) * (cut > 0.0 ? 3.0 : 1.0);
	vec3 wind_l = to_local * grass_wind(base_w, max(h * 0.8, 0.08), TIME, omega0, rnd);
	vec2 wl = wind_l.xz;
	float ws = length(wl);
	vec2 off = vec2(0.0, rest_lean) + (ws > 1e-4 ? wl / ws : vec2(0.0)) * clamp(ws * wind_response * (cut > 0.0 ? 0.1 : 0.16) * (0.5 + fract(rnd * 3.3)), 0.0, 0.32);
	if (trample_rect.z > 0.0) {
		vec2 tuv = (base_w.xz - trample_rect.xy) / trample_rect.zw;
		if (all(greaterThan(tuv, vec2(0.0))) && all(lessThan(tuv, vec2(1.0)))) {
			vec2 b = textureLod(trample_map, tuv, 0.0).rg;
			float bl = length(b);
			if (bl > 1e-3) {
				vec2 bd = (to_local * vec3(b.x, 0.0, b.y)).xz;
				off = mix(off, normalize(bd) * 1.3, smoothstep(0.0, 1.0, min(bl, 1.0)));
			}
		}
	}
	// Snow (docs/ideas/world-links.md): a settled pack's weight presses the sward flat, each blade its own way (the
	// snow press, Koerner 2003); it stands again as the pack melts.
	vec2 snow_pk = snow_pack_at(base_w);
	float snow_p = snow_press(snow_pk, 1.5);
	if (snow_p > 0.0) {
		float sa = fract(rnd * 61.7) * 6.2832;
		off = mix(off, vec2(cos(sa), sin(sa)) * 1.45, snow_p);
		// Laid flat under a pack deeper than it lies: buried, the snow surface shows over it.
		h *= 1.0 - smoothstep(0.06, 0.12, snow_pk.x) * smoothstep(0.7, 1.0, snow_p);
	}
	// Ruts and puddles (mud round 5): pressed blades lie flat, each its own way; where the soil is opened they are gone.
	vec3 go = grass_ground_opened(base_w.xz);
	{
		if (go.x + go.y > 0.0) {
			float ga = fract(rnd * 41.3) * 6.2832;
			off = mix(off, vec2(cos(ga), sin(ga)) * 1.45, go.x);
			h *= 1.0 - go.y * step(fract(rnd * 23.9), 0.97 * go.y + 0.02);
		}
	}
	float ang = min(length(off), 1.4);
	vec2 dir = length(off) > 1e-4 ? normalize(off) : vec2(0.0, 1.0);
	// Straw lies along its record's +z, a little up or down through the swath, bowed a little.
	ang = straw ? 1.3 + 0.4 * fract(rnd * 5.9) : ang;
	dir = straw ? vec2(0.0, 1.0) : dir;
	vec3 dir3 = vec3(dir.x, 0.0, dir.y);
	vec3 top = vec3(dir.x * sin(ang), cos(ang), dir.y * sin(ang)) * h;
	vec3 mid = vec3(0.0, h * 0.55 * cos(ang * 0.3), 0.0) + dir3 * h * 0.12 * sin(ang);
	mid = straw ? top * 0.5 + vec3(0.0, h * 0.04 * (fract(rnd * 2.3) - 0.3), 0.0) : mid;
	vec3 top_tg = normalize(crop_bez_d(vec3(0.0), mid, top, 1.0));

	// The ear's axis: on from the top of the stem, bending over towards the lean by the nod.
	float ear_len = crop_ear.x * crop_growth.z * (0.9 + 0.2 * fract(rnd * 9.1));
	float nod = clamp(crop_growth.w * (0.55 + 0.9 * fract(rnd * 7.7)), 0.0, 1.0);
	vec3 nod_dir = normalize(dir3 * 0.55 + vec3(0.0, -1.0, 0.0));
	vec3 e1 = top + top_tg * ear_len * 0.5;
	vec3 e2 = e1 + normalize(mix(top_tg, nod_dir, nod) + vec3(0.0, 1e-4, 0.0)) * ear_len * 0.5;

	vec3 p = vec3(0.0);
	vec3 n = vec3(0.0, 1.0, 0.0);
	vec3 axis = vec3(0.0, 1.0, 0.0);
	float view_w = 0.0; // Half width across the view, for thin round parts.
	bool hide = false;
	float ripe = 0.0;
	float dead = 0.0;
	if (part == 0) {
		// The culm, round: a strip turned to the view.
		hide = h < 0.003;
		p = crop_bez(vec3(0.0), mid, top, t);
		axis = normalize(crop_bez_d(vec3(0.0), mid, top, t));
		view_w = crop_leaf.z * sqrt(bulk) * (1.0 - 0.3 * t);
		ripe = clamp(crop_age.y + lag, 0.0, 1.0);
	} else if (part == 1) {
		// A leaf from its node: the lamina rises at its insertion angle and arches over, widest near
		// the base and tapering to a fine tip, alternate sides up the stem; the lowest die first,
		// curl and hang.
		hide = idx + 0.5 > clamp(1.0 + (stage - 10.0) / 1.5, 1.0, 4.0) || (cut > 0.0 && idx > 0.5);
		float sn = pow(idx / 3.0, 1.15) * 0.8;
		vec3 node = crop_bez(vec3(0.0), mid, top, sn);
		vec3 ntg = normalize(crop_bez_d(vec3(0.0), mid, top, sn));
		float L = crop_leaf.x * crop_growth.y * (0.75 + 0.35 * sin(3.1416 * (idx + 1.0) / 5.0)) * (0.85 + 0.3 * fract(rnd * (idx + 3.0) * 7.1));
		dead = clamp(crop_age.z * 4.0 - idx + lag, 0.0, 1.0);
		if (cut > 0.0) {
			dead = 1.0;
			L *= 0.6;
		}
		float yaw = idx * 3.1416 + (fract(rnd * 11.3 + idx * 0.37) - 0.5) * 0.9;
		vec3 out_d = vec3(sin(yaw), 0.0, cos(yaw));
		float ins = 0.3 + 0.3 * fract(rnd * 5.3 + idx * 0.71) + 0.5 * dead;
		float droop = 0.5 + 0.4 * fract(rnd * 2.9 + idx * 0.43) + 1.3 * dead;
		vec3 l1 = node + normalize(ntg * cos(ins) + out_d * sin(ins)) * L * 0.5;
		vec3 l2 = l1 + normalize(out_d * 0.85 + vec3(0.0, 0.35 - droop, 0.0)) * L * 0.5;
		l2 += vec3(wl.x, 0.0, wl.y) * 0.004 * L * (1.0 + sin(TIME * 5.0 + rnd * 20.0 + idx));
		p = crop_bez(node, l1, l2, t);
		vec3 tg = normalize(crop_bez_d(node, l1, l2, t));
		vec3 acr = normalize(vec3(-out_d.z, 0.0, out_d.x));
		float tw = t * 1.6 * (fract(rnd * 3.1 + idx * 0.29) - 0.5);
		acr = normalize(acr * cos(tw) + cross(tg, acr) * sin(tw));
		float lw = crop_leaf.y * sqrt(bulk) * (0.55 + 0.45 * smoothstep(0.0, 0.15, t)) * (1.0 - pow(t, 1.6)) * (1.0 - 0.5 * dead);
		p += acr * a * 2.0 * lw;
		n = normalize(cross(acr, tg));
		n = normalize(n + acr * a * 0.8);
		ripe = clamp(crop_age.y + lag, 0.0, 1.0);
	} else if (part == 2) {
		// The ear: two rows of spikelets, flattened, tapering to the tip (oats: the panicle's thin
		// axis).
		hide = ear_len < 0.002 || cut > 0.0;
		p = crop_bez(top, e1, e2, t);
		vec3 tg = normalize(crop_bez_d(top, e1, e2, t));
		vec3 b1 = normalize(cross(tg, abs(tg.y) < 0.95 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
		vec3 b2 = cross(b1, tg);
		float th = a * 6.2832;
		float prof = crop_ear.w > 0.5 ? 1.0 : mix(0.55, 1.0, smoothstep(0.0, 0.15, t)) * (1.0 - 0.6 * smoothstep(0.7, 1.0, t));
		// A spike's own spikelets give it its girth: the core is only the rachis and grain inside.
		float r = t > 0.999 ? 0.0 : crop_ear.y * prof * mix(1.0, sqrt(bulk), 0.6) * (crop_ear.w > 0.5 ? 1.0 : 0.55);
		p += (b1 * cos(th) + b2 * sin(th) * 0.72) * r;
		n = normalize(b1 * cos(th) + b2 * sin(th) * 1.3);
		ripe = clamp(crop_age.x + lag, 0.0, 1.0);
	} else if (part == 3) {
		// An awn from the tip of each spikelet, near parallel to the ear, longest at the top.
		hide = ear_len < 0.002 || cut > 0.0;
		vec3 sb;
		vec3 sd;
		float sl;
		vec3 so;
		float sp;
		crop_spikelet(idx, cnt, top, e1, e2, ear_len, crop_ear.y, rnd, sb, sd, sl, so, sp);
		vec3 tg = normalize(crop_bez_d(top, e1, e2, (idx + 0.5) / cnt));
		vec3 ad = normalize(tg + so * (0.12 + 0.1 * fract(idx * 0.618 + rnd)));
		float al = crop_ear.z * crop_growth.z * (0.7 + 0.3 * (idx + 0.5) / cnt);
		p = sb + sd * sl + ad * al * t + vec3(0.0, -1.0, 0.0) * al * 0.06 * t * t;
		axis = ad;
		view_w = 0.0007 * (1.0 - 0.8 * t);
		ripe = clamp(crop_age.x + lag + 0.05, 0.0, 1.0);
	} else if (part == 6) {
		// A spikelet: a plump lozenge from the rachis, out and up, turned to the view.
		hide = ear_len < 0.002 || cut > 0.0;
		vec3 sb;
		vec3 sd;
		float sl;
		vec3 so;
		float sp;
		crop_spikelet(idx, cnt, top, e1, e2, ear_len, crop_ear.y, rnd, sb, sd, sl, so, sp);
		p = sb + sd * sl * t + so * crop_ear.y * 0.35 * sin(3.1416 * t);
		axis = sd;
		view_w = crop_ear.y * 0.62 * sp * mix(1.0, sqrt(bulk), 0.5);
		ripe = clamp(crop_age.x + lag + 0.04 * (t - 0.5), 0.0, 1.0);
	} else {
		// Oat panicle: branches in whorls (golden angle round the axis) leaving the axis and
		// drooping, a spikelet hanging from each end.
		hide = ear_len < 0.002 || cut > 0.0;
		// Five whorls of three, the lowest branches longest (a pyramid of drooping spikelets).
		float whorl = floor(idx / 3.0);
		// The top whorl at the axis' tip: no bare axis standing above the panicle.
		float tk = (whorl + 0.3) / max(ceil(cnt / 3.0) - 0.7, 0.3) * 0.97;
		vec3 c = crop_bez(top, e1, e2, tk);
		vec3 tg = normalize(crop_bez_d(top, e1, e2, tk));
		vec3 b1 = normalize(cross(tg, abs(tg.y) < 0.95 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
		vec3 b2 = cross(b1, tg);
		float phi = mod(idx, 3.0) * 2.0944 + whorl * 0.9 + rnd * 6.2832;
		vec3 radial = b1 * cos(phi) + b2 * sin(phi);
		float bl = (0.11 - 0.075 * tk) * (0.8 + 0.4 * fract(idx * 0.618 + rnd)) * crop_growth.z;
		// Branches leave the axis at 30 to 50 degrees, then arch out and down under the spikelets.
		vec3 bd = normalize(radial * 0.65 + tg * 0.8 + vec3(0.0, -0.1 - 0.3 * nod, 0.0));
		if (part == 4) {
			// A capillary branch arching over under its spikelet's weight.
			p = c + bd * bl * t + vec3(0.0, -1.0, 0.0) * bl * 0.55 * t * t;
			axis = normalize(bd + vec3(0.0, -1.1 * t, 0.0));
			view_w = 0.0005;
			ripe = clamp(crop_age.y + lag, 0.0, 1.0);
		} else {
			// The spikelet: its glumes a pendant about 2.2 cm long.
			vec3 hang = normalize(vec3(0.0, -1.0, 0.0) + bd * 0.25 + vec3(wl.x, 0.0, wl.y) * 0.02);
			// Hanging tip down: the two glumes a spindle, widest a third from its attachment.
			p = c + bd * bl + vec3(0.0, -1.0, 0.0) * bl * 0.55 + hang * 0.022 * crop_growth.z * t;
			axis = hang;
			view_w = 0.0018 * pow(sin(3.1416 * clamp(1.0 - pow(1.0 - t, 1.6), 0.0, 1.0)), 0.8) * sqrt(bulk);
			// Papery glumes: pale long before the grain is ripe.
			ripe = clamp(crop_age.x + lag + 0.3 * smoothstep(55.0, 70.0, stage), 0.0, 1.0);
		}
	}
	if (view_w > 0.0) {
		vec3 to_cam = cam_l - p;
		vec3 acr = cross(axis, to_cam);
		float acl = length(acr);
		acr = acl > 1e-6 ? acr / acl : vec3(1.0, 0.0, 0.0);
		p += acr * a * 2.0 * view_w;
		vec3 fwd = normalize(cross(acr, axis));
		fwd = dot(fwd, to_cam) < 0.0 ? -fwd : fwd;
		n = normalize(fwd + acr * a * 1.6);
	}
	if (fade_band.w > 0.0) {
		float keep_b = clamp((fade_band.z - length(base_w.xz - fade_band.xy)) / fade_band.w, 0.0, 1.0);
		float order = floor(INSTANCE_CUSTOM.w) / 256.0;
		p *= smoothstep(order, order + 0.15, keep_b * 1.15);
	}
	hide = hide || (straw && part != 0) || (fire_gone > 0.6 && part != 0);
	// Lying straw shows its upper side, lit from the sky, not a strip turned edge-on to a low view.
	n = straw ? normalize(n + vec3(0.0, 2.0, 0.0)) : n;
	// Not yet emerged (a plot sown, below 10): nothing shows.
	p -= to_local * vec3(0.0, go.z, 0.0);
	// On the ground's terracette steps (WorldContracts::terracette_step), which the placed heights leave out.
	p += to_local * vec3(0.0, grass_terracette_lift(base_w), 0.0);
	VERTEX = hide || stage < 10.0 ? vec3(0.0) : p;
	NORMAL = n;
	v_t = t;
	v_part = float(part);
	v_ripe = ripe;
	v_dead = dead;
	v_hf = clamp(p.y / max(h + ear_len, 0.05), 0.0, 1.0);
	v_tint = grass_patch_tint(base_w.xz, fract(rnd * 7.31 + 0.13), fract(rnd * 13.7 + 0.51), color_variation);
	v_wet = 0.0;
	v_frost = 0.0;
	v_salt = grass_salt_burn(base_w.xz);
	v_snow = max(snow_pk.x > 0.01 ? clamp((snow_pk.x - p.y) / 0.01 + 0.5, 0.0, 1.0) : 0.0, grass_snow_coat(snow_pk, rnd, t, h + ear_len));
	if (weather_map_rect.z > 0.0) {
		vec2 half_texel = 0.5 / vec2(textureSize(weather_state_map, 0));
		vec2 uv = clamp((base_w.xz - weather_map_rect.xy) / weather_map_rect.zw, half_texel, 1.0 - half_texel);
		v_wet = textureLod(weather_state_map, uv, 0.0).g;
		// Hoarfrost (WeatherMap3D dew_and_frost): a few tenths of a millimetre frost the sward over.
		v_frost = smoothstep(0.0, 0.3, textureLod(weather_frost_map, uv, 0.0).r);
	}
}

void fragment() {
	int part = int(v_part + 0.5);
	vec3 green = crop_green * v_tint;
	float x = abs(UV.x - 0.5) * 2.0;
	float fw = max(fwidth(UV.x), 1e-4);
	float detail = 1.0 - smoothstep(0.08, 0.25, fw);
	vec3 c;
	float rough;
	float spec;
	float back;
	if (part == 1) {
		// Leaf: waxy glaucous green, yellowing with the straw; dead leaves a pale grey-tan.
		c = mix(green, crop_straw * 0.9, v_ripe * 0.8);
		c = mix(c, crop_straw * vec3(0.8, 0.74, 0.66) * (0.8 + 0.3 * fract(v_rnd * 17.0)), v_dead);
		float rib = 1.0 - smoothstep(0.0, 0.1 + fw, x);
		float veins = 0.5 + 0.5 * cos(x * 6.2832 * 4.5);
		c *= 1.0 + detail * (0.12 * rib - 0.06 * veins * (1.0 - rib));
		rough = mix(0.45, 0.75, max(v_dead, v_ripe));
		spec = mix(0.42, 0.25, max(v_dead, v_ripe));
		back = mix(0.65, 0.3, v_dead);
	} else if (part == 0 || part == 4) {
		// Culm: a paler green, then glossy straw.
		c = mix(green * vec3(1.15, 1.12, 1.0), crop_straw, v_ripe);
		rough = mix(0.45, 0.5, v_ripe);
		spec = 0.45;
		back = 0.35;
	} else {
		// Ear, awns, spikelets: green, through yellow, to the ripe colour, ripening from the top.
		float r = clamp(v_ripe + (part == 2 ? (v_t - 0.5) * 0.2 : 0.0), 0.0, 1.0);
		vec3 turning = mix(green, crop_ripe, 0.5) * vec3(1.15, 1.1, 0.75);
		c = r < 0.5 ? mix(green * 1.1, turning, r * 2.0) : mix(turning, crop_ripe, r * 2.0 - 1.0);
		c *= 0.9 + 0.2 * fract(v_rnd * 23.0);
		if (part == 2 && crop_ear.w < 0.5) {
			// Spikelets up the ear in two alternate rows: dark creases between the glumes.
			float sk = fract(v_t * 9.0 + step(0.5, fract(UV.x * 2.0)) * 0.5);
			float crease = smoothstep(0.0, 0.2, sk) * smoothstep(1.0, 0.75, sk);
			c *= mix(1.0, 0.7 + 0.3 * crease, 1.0 - smoothstep(0.1, 0.4, fwidth(v_t * 9.0)));
		} else if (part == 6) {
			// A spikelet's glumes: rounded, darker where they tuck under the next one, a pale keel.
			float keel = 1.0 - smoothstep(0.0, 0.12 + fw, x);
			c *= (0.7 + 0.3 * smoothstep(0.0, 0.55, v_t)) * (1.0 - 0.22 * x * x) * (1.0 + 0.08 * keel * detail);
		}
		rough = 0.6;
		spec = 0.3;
		back = 0.45;
	}
	c = vegetation_light_tint(c, v_light);
	// Specular antialiasing (Kaplanyan et al. 2016): the normals' spread over the pixel widens the
	// lobe; past a few metres leaves are under a pixel and their derivatives say nothing, so the
	// distance widens it too. Without it the waxy leaves sparkle as white speckles across the field.
	// fwidth(), not dFdx()/dFdy(): the path tracer's shader translation provides only fwidth().
	vec3 dn = fwidth(NORMAL);
	float spread = min(0.25 * dot(dn, dn), 0.18);
	float far_t = smoothstep(4.0, 18.0, length(VERTEX));
	rough = sqrt(clamp(rough * rough + spread, 0.0, 1.0));
	rough = mix(rough, max(rough, 0.85), far_t);
	spec *= mix(1.0, 0.6, far_t);
	// Wet (Lagarde 2013, low porosity): a little darker and glossier.
	float wet_factor = mix(1.0, 0.2, 0.25);
	// Fire: charred stubble with ash flecks, its top glowing in the burning band. Read at this point (the path
	// tracer reruns vertex() for a merged MultiMesh with the field's transform).
	vec2 fire_xz = (INV_VIEW_MATRIX * vec4(VERTEX, 1.0)).xz;
	vec4 fire_f = fire_state_at(fire_xz);
	if (fire_f.x > 0.0) {
		// Ash flecks as many as the ground's ash share (grass_ash: a dry burn's white ash, a damp one's char).
		float fleck = step(1.0 - mix(0.02, 0.5, fire_ash_at(fire_xz)), fract(v_rnd * 41.0)) * smoothstep(0.6, 1.0, fire_f.x);
		vec3 ch = mix(vec3(0.035, 0.03, 0.027), vec3(0.35, 0.34, 0.33), fleck);
		c = mix(c, ch, smoothstep(0.0, 0.5, fire_f.x));
		rough = mix(rough, 0.95, smoothstep(0.0, 0.5, fire_f.x));
		back *= 1.0 - smoothstep(0.0, 0.5, fire_f.x);
		EMISSION = fire_glow_color(fire_f.y) * 4.0 * smoothstep(0.4, 1.0, v_hf);
	}
	// Mud thrown or splashed onto it (mud round 7).
	vec3 mud_w = (INV_VIEW_MATRIX * vec4(VERTEX, 1.0)).xyz;
	float mud_c = grass_mud_apply(grass_ground_mud(mud_w.xz), mud_w, v_hf * 0.8, c);
	rough = mix(rough, 0.85, mud_c);
	// Salt burn: the tips first, then down the blade.
	c = mix(c, vec3(0.3, 0.22, 0.1) * v_tint, v_salt * smoothstep(1.0 - v_salt, 1.2 - 0.5 * v_salt, v_hf));
	// Hoarfrost rimes the blades, the tips most (they see the most sky); frozen, they do not look wet.
	float frost_c = v_frost * mix(0.4, 1.0, v_hf);
	c = mix(c, vec3(0.78, 0.8, 0.84), 0.6 * frost_c);
	rough = mix(rough, 0.85, frost_c);
	// Under the snow surface the blade is the snow's (the pack's albedo stands for it).
	c = mix(c, vec3(0.85), v_snow);
	rough = mix(rough, 0.7, v_snow);
	ALBEDO = c * mix(1.0, wet_factor, v_wet * (1.0 - frost_c));
	ROUGHNESS = rough * mix(1.0, 0.4, v_wet * (1.0 - frost_c));
	SPECULAR = spec;
	BACKLIGHT = ALBEDO * back;
	// Sky light inside the canopy (Monsi and Saeki 1953), as the grass: raster ambient only.
	float scatter = clamp(dot(ALBEDO, vec3(0.3, 0.59, 0.11)) * 1.7, 0.0, 0.95);
	AO = exp(-0.7 * sqrt(1.0 - scatter) * crop_lai * (1.0 - v_hf));
}
)";
	} else {
		code += R"(
varying float v_t;
varying float v_dry;
varying float v_flush; // Regrowth on burnt ground (burn_flush_map).
varying float v_graze; // Grazed down to a lawn (graze_map).
varying float v_hay; // Cut hay lying in the meadow: days since mown; below 0 none.
// A mown meadow (one varying: the shader is at the 32 limit): x the sward's leaf area left over this blade's ground
// (1 uncut; the stubble less), y the cut stub's share of a stubble blade's height now, z how fresh the cut (1 just cut).
varying vec3 v_cut;
varying float v_fairy_dead; // A blade browned in a fairy ring's dead band.
varying vec3 v_melt; // After the snow: x pale and matted, y snow mould, z days since the melt.
varying vec2 v_wash; // Under the last flood's mark: x combed flat, y silted.
varying float v_soft; // Distance softening, 0 near, 1 from 35 m.
varying float v_wet;
varying float v_frost;
varying float v_salt;
varying float v_snow; // under the snow surface: 1
varying vec3 v_tint;
varying float v_light;
varying float v_trod; // Trodden flat on a worn path (its rest lean past any sward's own).

uniform float color_variation;
uniform float fibre_shading = 0.8;
)";
		code += GRASS_COLOR_CODE;
		code += VegetationLight::SHADER_CODE;
		code += VegetationFire::SHADER_CODE;
		code += VegetationFire::ASH_CODE;
		code += R"(

void vertex() {
	// The season's standing height (GrassField3D.season_height_for): short in winter, regrown by early summer.
	float h = INSTANCE_CUSTOM.x * season_height;
	float w = INSTANCE_CUSTOM.y;
	float light_q = floor(INSTANCE_CUSTOM.z / 4.0);
	float rest_lean = INSTANCE_CUSTOM.z - light_q * 4.0;
	v_trod = smoothstep(1.1, 2.2, rest_lean);
	v_light = light_q / 100.0 * 1.5;
	float rnd = fract(INSTANCE_CUSTOM.w);
	float t = VERTEX.y;
	float side = VERTEX.x;
	vec3 base_w = (MODEL_MATRIX * vec4(0.0, 0.0, 0.0, 1.0)).xyz;
	mat3 to_local = inverse(mat3(MODEL_MATRIX));
	// Deer lawns (Putman 1986; Kuijper et al. 2009): where deer graze hard the blades are bitten down to about
	// graze_height, a few new leaves a little over it; the blade keeps its width, so the turf reads dense and even.
	v_graze = 0.0;
	if (graze_rect.z > 0.0) {
		vec2 guv = (base_w.xz - graze_rect.xy) / graze_rect.zw;
		if (all(greaterThanEqual(guv, vec2(0.0))) && all(lessThanEqual(guv, vec2(1.0)))) {
			vec2 gr = textureLod(graze_map, guv, 0.0).rg;
			v_graze = gr.r;
			// Boar rooting (g: the share torn out): those blades are gone, the rest stand short and thin on the
			// turned soil.
			if (fract(rnd * 7.77) < gr.g) {
				h = 0.0;
				w = 0.0;
			}
			h *= 1.0 - 0.5 * gr.g;
			// Not one even height: grazers bite patchily (patch grazing; Adler, Raff and Lauenroth 2001), keeping some
			// patches close-cropped and leaving others, by dung or unpalatable plants, as taller roughs, a mosaic at a
			// metre and a few metres; within it each blade stands at its own bite, a few just regrown (skewed tall).
			float mosaic = grass_noise(base_w.xz * 1.3 + 3.1) * 0.55 + grass_noise(base_w.xz * 0.31 + 17.0) * 0.45;
			float patch_h = mix(0.65, 1.5, smoothstep(0.3, 0.75, mosaic)) * (1.0 + 1.2 * smoothstep(0.72, 0.86, mosaic));
			float bite = fract(rnd * 5.31);
			float crop_h = graze_height * patch_h * (0.5 + 0.8 * bite * bite + (fract(rnd * 3.17) < 0.12 ? 0.6 : 0.0));
			h = mix(h, min(h, crop_h), v_graze);
			// Regrowth after a bite is fine young leaf: narrower blades keep the short turf from reading as teeth. The
			// turf ground under the sward (get_ground_shader_code()) closes it between them.
			w *= 1.0 - 0.35 * v_graze * (0.4 + fract(rnd * 9.13));
		}
	}
	// Drylands (climate_bare, WorldContracts::bare_patch): the sward grows in the clumps of cover, none on the bare soil
	// between them, which the terrain material draws from the same pattern. The patch's edge is a few centimetres of
	// the noise, so its blades thin out across it rather than stop on a line.
	if (climate_bare) {
		float cb = wc_climate_bare(base_w);
		if (cb > 0.0 && fract(rnd * 23.17) < wc_bare_patch(base_w, cb, 0.0)) {
			h = 0.0;
			w = 0.0;
		}
	}
	// Hay meadows (docs/ideas/vegetation.md, "Mowing and hay rows"): mown to a stubble of 7.5 to 10 cm (UNH Extension,
	// "Haymaking": 3 to 4 inches for quick regrowth), which grows back over about six weeks (an estimate). Most blades
	// are the cut grass: on the day, lying in the mower's swath (passes 3 m apart, the swath gathered to 1.6 m: FAO,
	// Grassland Fodder Conservation); then tedded, spread flat over the stubble; on the third day raked into
	// windrows (two swaths to a row, 6 m apart, an estimate) and baled (hay takes about three days of good weather to
	// cure: UNH; rarely under three in the UK: FAO, Grassland Fodder Conservation); then carted. The gathering moves each
	// blade across its row, so a windrow holds the grass of its whole strip.
	v_hay = -1.0;
	v_cut = vec3(1.0, 0.0, 0.0);
	vec3 hay_shift = vec3(0.0);
	float hay_lift = 0.0;
	vec2 hay_lay = vec2(0.0);
	if (mow_rect.z > 0.0) {
		vec2 muv = (base_w.xz - mow_rect.xy) / mow_rect.zw;
		if (all(greaterThanEqual(muv, vec2(0.0))) && all(lessThan(muv, vec2(1.0)))) {
			int mi = int(textureLod(mow_map, muv, 0.0).r * 255.0 + 0.5);
			if (mi > 0) {
				vec4 ms = texelFetch(mow_state, ivec2(mi - 1, 0), 0);
				float md = ms.r;
				if (md >= 0.0) {
					float stub = 0.075 + 0.025 * fract(rnd * 29.3);
					float regrown = min(h, stub + max(h - stub, 0.0) * smoothstep(4.0, 42.0, md));
					// The canopy over the ground is the stubble's now, not the sward's: the sky reaches down into it,
					// so the ambient darkening (AO below) takes the leaf area left, about the height left (the
					// regrowth's leaves fill out its height).
					v_cut.x = clamp(regrown / max(h, 1e-3), 0.0, 1.0);
					if (fract(rnd * 13.7) >= 0.6 || md >= 3.5) {
						h = regrown;
						// The cut stub keeps the pale of the stem bases and sheaths the cut lays open (turf cut low "turns
						// yellow or brown", "the stem tissue is exposed": UC IPM, Scalping; K-State, Scalping Turf), under
						// the green leaves growing back from the base; the stub itself greens a little in the light over
						// a couple of weeks (an estimate).
						v_cut.yz = vec2(clamp(stub / max(regrown, 1e-3), 0.0, 1.0), 1.0 - 0.6 * smoothstep(5.0, 21.0, md));
					} else {
						vec2 rd = vec2(cos(ms.g), sin(ms.g));
						vec2 rn = vec2(-rd.y, rd.x);
						float pitch = md < 1.0 ? 3.0 : (md < 2.5 ? 0.0 : 6.0);
						float half_w = md < 1.0 ? 0.8 : 0.6;
						float heap = md < 1.0 ? 0.1 : (md < 2.5 ? 0.035 : 0.3);
						float across = 0.0;
						float la = fract(rnd * 4.3) * 6.2832; // Tedded: every way.
						if (pitch > 0.0) {
							float u = dot(base_w.xz, rn);
							float row = floor(u / pitch);
							float uc = (row + 0.5) * pitch + 0.2 * sin(dot(base_w.xz, rd) * 0.06 + row * 1.7) + 0.08 * sin(dot(base_w.xz, rd) * 0.27 + row * 0.4);
							float rel = clamp(u - uc, -0.5 * pitch, 0.5 * pitch);
							float nrel = rel * half_w / (0.5 * pitch);
							hay_shift = vec3(rn.x, 0.0, rn.y) * (nrel - rel);
							across = nrel / half_w;
							// Along the row either way, some crossways.
							la = atan(rd.y, rd.x) + (fract(rnd * 4.3) < 0.5 ? 0.0 : 3.1416) + (fract(rnd * 7.9) - 0.5) * 1.3;
						}
						// Heaped to the row's middle, each blade at its own depth in the heap.
						hay_lift = heap * (1.0 - across * across) * fract(rnd * 3.31);
						vec2 lw = vec2(cos(la), sin(la));
						vec2 ll = (to_local * vec3(lw.x, 0.0, lw.y)).xz;
						hay_lay = length(ll) > 1e-4 ? normalize(ll) : vec2(0.0, 1.0);
						w *= pitch > 0.0 ? 1.0 : 1.4;
						v_hay = md;
						// Rain on it while it lay (mow_state b, mm): v_cut.z carries the dulling (no stub on a lying blade).
						v_cut.z = smoothstep(2.0, 25.0, ms.b);
					}
				}
			}
		}
	}
	// Meadow fairy rings (fairy_rings.h; Shantz and Piemeisel 1917): the dark, lush front stands taller; behind it the
	// dead band, its blades browned, shorter and some gone to bare soil; then a weaker lush band inside.
	v_fairy_dead = 0.0;
	vec3 fairy = vec3(0.0);
	if (fairy_rings > 0.0) {
		fairy = fairy_ring_bands(base_w.xz);
		h *= 1.0 + 0.4 * (fairy.x + fairy.z);
		if (fairy.y > 0.0) {
			if (fract(rnd * 27.3) < 0.3 * fairy.y) {
				h = 0.0;
				w = 0.0;
			}
			h *= 1.0 - 0.35 * fairy.y;
			v_fairy_dead = step(fract(rnd * 19.1), 0.9 * fairy.y);
		}
	}
	// Terracettes (docs/ideas/vegetation.md, "Terracettes"): on steep grazed hillsides the stock walk the slope across,
	// along the contours, and their paths settle into regularly spaced steps; they walk the flat treads and graze the
	// risers between (Seleb, Chatterjee and Bhamla 2025: bands parallel to the contours from about 30 degrees, regular
	// by 40, about 3.4 m apart in their model). The steps themselves are the ground's; here the sward: a worn, short,
	// thinned band on each tread, combed along it, and the grass on the risers left a little taller. Bands follow the
	// height with a wander of a few decimetres, so they keep to the contours and drift as real ones do.
	if (terracettes > 0.0 && ground_data_rect.z > 0.0 && v_hay < 0.0) {
		vec2 tts = vec2(textureSize(ground_data_map, 0));
		vec2 tuv = (base_w.xz - ground_data_rect.xy) / ground_data_rect.zw;
		vec2 tdu = vec2(1.0 / tts.x, 0.0);
		vec2 tdv = vec2(0.0, 1.0 / tts.y);
		vec2 tcell = 2.0 * ground_data_rect.zw / tts;
		vec2 grad = vec2(textureLod(ground_data_map, tuv + tdu, 0.0).r - textureLod(ground_data_map, tuv - tdu, 0.0).r, textureLod(ground_data_map, tuv + tdv, 0.0).r - textureLod(ground_data_map, tuv - tdv, 0.0).r) / tcell;
		vec3 terr = wc_terracette(base_w, grad); // WorldContracts::terracette_at: the ground's steps read the same
		float form = terr.z;
		if (form > 0.0) {
			float tread = terr.x;
			if (tread > 0.0) {
				// Trodden: most tillers gone, the rest short turf (an estimate, as on a worn path).
				if (fract(rnd * 33.1) < 0.7 * tread) {
					h = 0.0;
					w = 0.0;
				}
				h *= mix(1.0, 0.3, tread);
			} else {
				h *= 1.0 + 0.15 * form;
			}
		}
	}
	// A far ring blade (negative width; GrassField3D::_build_ring_tile()): one of a set thinned with the square of
	// the distance from the field's centre, each blade dropping at its own rank (w's integer part: -log2 of it, in
	// 1/1024ths) and the ones left widened, so coverage holds and no ring shows (Ghost of Tsushima, Wohllaib GDC
	// 2021). They come in over the near blades' outer band, which they mirror, and go over the rings' own end
	// band, where the voxel shell takes over. From the field's centre, not the camera: the shadow pass agrees.
	bool ring_blade = w < 0.0;
	float ring_s = 1.0;
	if (ring_blade) {
		w = -w;
		float rd = max(length(base_w.xz - fade_band.xy), 1e-3);
		float rk = min(ring_band.x * ring_band.x / (rd * rd), 1.0);
		w *= min(inversesqrt(rk), ring_band.w);
		float near_keep = fade_band.w > 0.0 ? clamp((fade_band.z - rd) / fade_band.w, 0.0, 1.0) : 0.0;
		float far_keep = ring_band.z > 0.0 ? clamp((ring_band.y - rd) / ring_band.z, 0.0, 1.0) : 1.0;
		float rank = exp2(-floor(INSTANCE_CUSTOM.w) / 1024.0);
		ring_s = smoothstep(1.0, 1.4, rk * (1.0 - near_keep) * far_keep / rank);
	}

	// Wind at two thirds of the blade, in the blade's frame; flutter on top.
	// A blade's own frequency: about 2 Hz at 0.4 m, slower when taller and softer (de Langre 2008).
	float omega0 = 6.2832 * clamp(2.0 * sqrt(0.4 / max(h, 0.05)), 1.0, 4.0);
	vec3 wind_l = to_local * grass_wind(base_w, max(h * 0.66, 0.05), TIME, omega0, rnd);
	vec2 wl = wind_l.xz;
	float ws = length(wl);
	float flutter = sin(TIME * (2.5 + 2.0 * rnd) + rnd * 6.2832 + dot(base_w.xz, vec2(0.7, 0.5))) * 0.25 * clamp(ws * 0.15, 0.0, 1.0);
	vec2 off = vec2(0.0, rest_lean) + (ws > 1e-4 ? wl / ws : vec2(0.0)) * clamp(ws * wind_response, 0.0, 1.3) * (1.0 + flutter);
	// Trampled: the trample field's bend replaces the wind, laying the blade down along it.
	if (trample_rect.z > 0.0) {
		vec2 tuv = (base_w.xz - trample_rect.xy) / trample_rect.zw;
		if (all(greaterThan(tuv, vec2(0.0))) && all(lessThan(tuv, vec2(1.0)))) {
			vec2 b = textureLod(trample_map, tuv, 0.0).rg;
			float bl = length(b);
			if (bl > 1e-3) {
				vec2 bd = (to_local * vec3(b.x, 0.0, b.y)).xz;
				off = mix(off, normalize(bd) * 1.4, smoothstep(0.0, 1.0, min(bl, 1.0)));
			}
		}
	}
	// Snow (docs/ideas/world-links.md): a settled pack's weight presses the sward flat, each blade its own way (the
	// snow press, Koerner 2003); it stands again as the pack melts.
	vec2 snow_pk = snow_pack_at(base_w);
	float snow_p = snow_press(snow_pk, 1.0);
	if (snow_p > 0.0) {
		float sa = fract(rnd * 61.7) * 6.2832;
		off = mix(off, vec2(cos(sa), sin(sa)) * 1.45, snow_p);
		// Laid flat under a pack deeper than it lies: buried, the snow surface shows over it.
		h *= 1.0 - smoothstep(0.06, 0.12, snow_pk.x) * smoothstep(0.7, 1.0, snow_p);
	}
	// After the snow (docs/ideas/vegetation.md, "Snowmelt sward"): where snow lay for weeks the sward comes out matted
	// flat in swirls, pale from the dark, and where it lay 45 to 60 days and more, with patches of grey snow mould
	// (Typhula; UMass Extension, Penn State turf lab): grey lint-like mycelium at the melt, then straw to grey-brown,
	// brittle, matted patches. It stands and greens again over about three weeks. Drifts and hollows lie longer.
	v_melt = vec3(0.0);
	if (snowmelt > 0.0) {
		float fi = clamp((base_w.y - melt_band0.x) / melt_band0.y, 0.0, 14.999);
		int bi = int(fi);
		vec2 lb = mix(melt_bands[bi], melt_bands[bi + 1], fract(fi));
		float drift = (grass_noise(base_w.xz * 0.09 + 41.0) - 0.5) * 16.0 + (grass_noise(base_w.xz * 0.6 + 5.0) - 0.5) * 6.0;
		float lie = lb.x + drift;
		float since = lb.y - max(drift, 0.0) * 0.5;
		if (lb.y >= 0.0 && since >= 0.0 && lie > 20.0) {
			float mat = smoothstep(20.0, 60.0, lie) * (1.0 - smoothstep(4.0, 24.0, since));
			float swirl = grass_noise(base_w.xz * 0.45 + 13.0) * 12.566 + (fract(rnd * 37.1) - 0.5) * 0.9;
			off = mix(off, vec2(cos(swirl), sin(swirl)) * 1.35, mat);
			float spot = smoothstep(0.6, 0.68, grass_noise(base_w.xz * 2.2 + 9.1)) * smoothstep(0.3, 0.5, grass_noise(base_w.xz * 0.15 + 77.0));
			float mould = spot * smoothstep(45.0, 75.0, lie) * (1.0 - smoothstep(12.0, 40.0, since));
			v_melt = vec3(mat, mould, since);
		}
	}
	// Ruts and puddles (mud round 5): pressed blades lie flat, each its own way; where the soil is opened they are gone.
	vec3 go = grass_ground_opened(base_w.xz);
	{
		if (go.x + go.y > 0.0) {
			float ga = fract(rnd * 41.3) * 6.2832;
			off = mix(off, vec2(cos(ga), sin(ga)) * 1.45, go.x);
			h *= 1.0 - go.y * step(fract(rnd * 23.9), 0.97 * go.y + 0.02);
		}
	}

	// Blades dead to the crown in winter or a drought (dormant) lose their turgor and lodge: they lie
	// over, each its own way, a little shorter, in the mat rather than standing up as straw spikes.
	if (rnd > 1.0 - dormant * 0.85) {
		float la = fract(rnd * 53.7) * 6.2832;
		off = off * 0.4 + vec2(cos(la), sin(la)) * (0.9 + 0.4 * fract(rnd * 17.3));
		h *= 0.8;
	}
	// Storm lodging (docs/ideas/vegetation.md, "Storm lodging"): where a storm's gusts on wet sward passed the stems'
	// base strength, a share of the blades lie down along the storm's wind, kinked at the base (Baker et al. 1998), the
	// rest lean after them; the swirls are in the map's direction. A grazed lawn is too short to lodge.
	if (lodge_rect.z > 0.0) {
		vec2 luv = (base_w.xz - lodge_rect.xy) / lodge_rect.zw;
		if (all(greaterThan(luv, vec2(0.0))) && all(lessThan(luv, vec2(1.0)))) {
			vec3 lg = textureLod(lodge_map, luv, 0.0).rgb;
			float la = lg.b * (1.0 - v_graze);
			float lr = fract(rnd * 61.7);
			if (la > 0.004 && lr < la * 1.6) {
				vec2 ld = lg.rg * 2.0 - 1.0;
				float lj = (fract(rnd * 13.9) - 0.5) * 0.6;
				ld = mat2(vec2(cos(lj), sin(lj)), vec2(-sin(lj), cos(lj))) * ld;
				vec2 ldl = (to_local * vec3(ld.x, 0.0, ld.y)).xz;
				ldl = length(ldl) > 1e-4 ? normalize(ldl) : vec2(0.0, 1.0);
				// Down (the lodged share), else leaning on its neighbours at the patch's edge.
				float down = lr < la * 1.05 ? 1.0 : 0.45;
				off = mix(off, ldl * (1.15 + 0.3 * fract(rnd * 7.3)) * down, mix(0.7, 0.95, down));
				h *= mix(1.0, 0.93, down);
			}
		}
	}
	// Deer beds (deer_beds.h): where a deer lay up, the tall grass lies flat in an oval, swirled the way it turned
	// before lying and pressed out from the middle, standing up again over about ten days. Not in a mown meadow or a
	// lawn too short to show it.
	float bed_face_up = 0.0;
	if (deer_beds > 0.0 && v_hay < 0.0 && h > 0.12) {
		vec3 bed = deer_bed(base_w.xz, deer_bed_day);
		if (bed.x > 0.0) {
			float bj = (fract(rnd * 23.9) - 0.5) * 0.5;
			vec2 bd = mat2(vec2(cos(bj), sin(bj)), vec2(-sin(bj), cos(bj))) * bed.yz;
			vec2 bdl = (to_local * vec3(bd.x, 0.0, bd.y)).xz;
			bdl = length(bdl) > 1e-4 ? normalize(bdl) : vec2(0.0, 1.0);
			// Most blades flat under the body; the rest, and all of them as it recovers, half up.
			float flat_k = fract(rnd * 41.3) < 0.8 ? bed.x : 0.5 * bed.x;
			off = mix(off, bdl * (1.2 + 0.25 * fract(rnd * 3.7)), flat_k);
			h *= mix(1.0, 0.85, bed.x);
			// Each drawn blade stands for several tillers: standing, they hide the soil seen from the side; lying, only
			// their own width covers it, so the pressed ones widen to the mat the tillers make together.
			w *= mix(1.0, 2.8, flat_k);
			// A body pressed them flat: they lie face up in a mat over the soil, not on edge.
			bed_face_up = step(0.5, flat_k);
		}
	}
	// Under the last flood's mark (WorldWater.high_water_at): the current combed the blades flat downstream and left
	// them grey with silt, with a sharp line at the crest (USGS, Koenig et al. 2016: folded-grass wash lines); they
	// stand again over weeks, the silt washes off over months.
	v_wash = vec2(0.0);
	if (flood_rect.z > 0.0) {
		vec2 fuv = (base_w.xz - flood_rect.xy) / flood_rect.zw;
		if (all(greaterThan(fuv, vec2(0.0))) && all(lessThan(fuv, vec2(1.0)))) {
			vec4 fl = textureLod(flood_map, fuv, 0.0);
			float line = fl.r + (grass_noise(base_w.xz * 1.7 + 3.0) - 0.5) * 0.06;
			float under = fl.a > 0.5 && fl.g >= 0.0 ? 1.0 - smoothstep(line - 0.04, line, base_w.y) : 0.0;
			if (under > 0.0) {
				float comb = under * (1.0 - smoothstep(7.0, 35.0, fl.g));
				vec2 fd = fl.b < 50.0 ? vec2(cos(fl.b), sin(fl.b)) : vec2(cos(rnd * 6.2832), sin(rnd * 6.2832));
				float fj = (fract(rnd * 19.7) - 0.5) * 0.5;
				fd = mat2(vec2(cos(fj), sin(fj)), vec2(-sin(fj), cos(fj))) * fd;
				vec2 fdl = (to_local * vec3(fd.x, 0.0, fd.y)).xz;
				fdl = length(fdl) > 1e-4 ? normalize(fdl) : vec2(0.0, 1.0);
				off = mix(off, fdl * (1.3 + 0.2 * fract(rnd * 5.3)), comb * 0.95);
				// Pressed lowest in the strand line's band just under the crest, where the wrack lies on them.
				float strand = smoothstep(line - 0.12, line - 0.07, base_w.y);
				h *= mix(1.0, mix(0.9, 0.4, strand), comb);
				v_wash = vec2(comb, under * (1.0 - smoothstep(20.0, 120.0, fl.g)));
			}
		}
	}
	// Fire (vegetation_fire.h): the front burns a blade from the top down as its fuel goes, to
	// stubble 3-6 cm tall, thinner and upright (a grass fire's burnt ground).
	vec4 fire_s = fire_state_at(base_w.xz);
	if (fire_s.x > 0.0) {
		float gone = smoothstep(0.0, 0.85, fire_s.x);
		float stub = clamp((0.03 + 0.03 * fract(rnd * 29.3)) / max(h, 0.01), 0.0, 1.0);
		h *= mix(1.0, stub, gone);
		w *= mix(1.0, 0.6, gone);
		off *= 1.0 - 0.8 * gone;
	}
	if (v_hay >= 0.0) {
		off = hay_lay * (1.42 + 0.06 * fract(rnd * 9.7)); // Cut: lying, not standing.
	}
	// Length-preserving bend (Jahrmann and Wimmer 2017): the tip turns by the bend angle.
	float ang = min(length(off), 1.45);
	vec2 dir = length(off) > 1e-4 ? normalize(off) : vec2(0.0, 1.0);
	vec3 tip = vec3(dir.x * sin(ang), cos(ang), dir.y * sin(ang)) * h;
	vec3 mid = vec3(0.0, h * 0.62 * cos(ang * 0.35), 0.0) + vec3(dir.x, 0.0, dir.y) * h * 0.18 * sin(ang);
	vec3 p = 2.0 * t * (1.0 - t) * mid + t * t * tip;
	vec3 tangent = normalize(2.0 * (1.0 - t) * mid + 2.0 * t * (tip - mid) + vec3(0.0, 1e-4, 0.0));
	vec3 across = bed_face_up > 0.5 ? vec3(dir.y, 0.0, -dir.x) : vec3(1.0, 0.0, 0.0);
	// Nearly parallel edges, then a short taper to the tip, as a real leaf blade.
	float taper = 1.0 - 0.9 * t * t;
	p += across * side * w * taper;
	// Seen edge-on a blade is a line: widen it across the view instead (Ghost of Tsushima's
	// view-space thickening), so the field stays full from every side. Always towards the main
	// camera: in a shadow pass the light's camera would widen every blade across the sun and the
	// sward would cast about twice the shadow it has.
	vec3 cam_l = to_local * (MAIN_CAM_INV_VIEW_MATRIX[3].xyz - base_w);
	vec2 to_cam = cam_l.xz - p.xz;
	float cam_d = length(to_cam);
	if (cam_d > 1e-3) {
		to_cam /= cam_d;
		float edge = 1.0 - abs(to_cam.y);
		p.xz += vec2(-to_cam.y, to_cam.x) * sign(to_cam.x + 1e-6) * side * w * taper * edge * view_facing;
	}
	// Rounded across its width, as a real blade.
	vec3 n = normalize(cross(across, tangent));
	// Outer band (Ghost of Tsushima, Horizon Zero Dawn): blades drop out one at a time in an even
	// dither order and shrink to nothing before the radius, so the field has no edge; the ground under
	// them takes the blades' colour (get_ground_shader_code()). fade_band: centre xz, the distance
	// where the last blade is gone, the width of the band.
	if (ring_blade) {
		p *= ring_s;
	} else if (fade_band.w > 0.0) {
		float keep_b = clamp((fade_band.z - length(base_w.xz - fade_band.xy)) / fade_band.w, 0.0, 1.0);
		float order = floor(INSTANCE_CUSTOM.w) / 256.0;
		p *= smoothstep(order, order + 0.15, keep_b * 1.15);
	}
	p -= to_local * vec3(0.0, go.z, 0.0);
	if (v_hay >= 0.0) {
		p += to_local * (hay_shift + vec3(0.0, hay_lift, 0.0));
	}
	// On the ground's terracette steps (WorldContracts::terracette_step), which the placed heights leave out.
	p += to_local * vec3(0.0, grass_terracette_lift(base_w), 0.0);
	VERTEX = p;
	NORMAL = normalize(n + across * side * 0.9);

	v_t = t;
	v_dry = rnd;
	v_wet = 0.0;
	v_frost = 0.0;
	v_salt = grass_salt_burn(base_w.xz);
	v_snow = max(snow_pk.x > 0.01 ? clamp((snow_pk.x - p.y) / 0.01 + 0.5, 0.0, 1.0) : 0.0, grass_snow_coat(snow_pk, rnd, t, h));
	v_tint = grass_patch_tint(base_w.xz, fract(rnd * 7.31 + 0.13), fract(rnd * 13.7 + 0.51), color_variation);
	v_soft = 0.0;
	// Softness with distance (docs/ideas/grass.md, "Colour without blotches"; Ghost of Tsushima, GDC 2021: normals
	// blended towards a common normal with distance): past a few metres each blade's own facing and value flicker
	// into speckle, so they ease towards the sward's: the normal towards the ground's up, the tint towards the large-
	// scale drift. The meadow then reads as a mass of fine fibres (Planet Tech v5), not grit. Measured from the
	// fade band's centre, so the shadow pass and the path tracer agree.
	if (fade_band.w > 0.0) {
		float soft_d = length(base_w.xz - fade_band.xy);
		float soft = smoothstep(6.0, 35.0, soft_d);
		v_soft = soft;
		// Towards the ground's own normal, not straight up (Ghost of Tsushima; Pesce 2024, "Making a greener grass"),
		// so a far hillside's blades take the slope's light as the ground and the far shell under them do, and further
		// over the far rings, where a blade is a pixel or less and its own facing only flickers.
		vec3 ground_n = vec3(0.0, 1.0, 0.0);
		if (ground_data_rect.z > 0.0) {
			vec2 ts = vec2(textureSize(ground_data_map, 0));
			vec2 uv = (base_w.xz - ground_data_rect.xy) / ground_data_rect.zw;
			vec2 du = vec2(1.0 / ts.x, 0.0);
			vec2 dv = vec2(0.0, 1.0 / ts.y);
			float hx = textureLod(ground_data_map, uv + du, 0.0).r - textureLod(ground_data_map, uv - du, 0.0).r;
			float hz = textureLod(ground_data_map, uv + dv, 0.0).r - textureLod(ground_data_map, uv - dv, 0.0).r;
			vec2 cell = 2.0 * ground_data_rect.zw / ts;
			ground_n = normalize(vec3(-hx / cell.x, 1.0, -hz / cell.y));
		}
		float soft_mix = 0.55 * soft;
		if (ring_blade) {
			soft_mix = mix(0.55, 0.85, smoothstep(ring_band.x, mix(ring_band.x, ring_band.y, 0.5), soft_d));
		}
		NORMAL = normalize(mix(NORMAL, normalize(to_local * ground_n), soft_mix));
		v_tint = mix(v_tint, grass_patch_tint(base_w.xz, 0.5, 0.5, color_variation), 0.5 * soft);
	}
	// Fibre shading (docs/ideas/grass.md): the blade's normals spread across its width, not along it, so its
	// specular lobe widens across the strand (ANISOTROPY along TANGENT) and the sward's highlight runs as a sheen
	// across the blades, as on hair. Tangent across the blade, square to the normal, so raster and the path
	// tracer (which squares it) agree.
	vec3 fib_t = across - dot(across, NORMAL) * NORMAL;
	TANGENT = dot(fib_t, fib_t) > 1e-8 ? normalize(fib_t) : normalize(cross(NORMAL, tangent));
	BINORMAL = normalize(cross(NORMAL, TANGENT));
	// Regrowth on burnt ground: the new blades come up a lighter, fresher yellow-green than the sward around (the
	// post-fire green flush), fading over a year and a half; the plants' shader does the same.
	v_flush = 0.0;
	if (burn_flush_rect.z > 0.0) {
		vec2 fuv = (base_w.xz - burn_flush_rect.xy) / burn_flush_rect.zw;
		if (all(greaterThanEqual(fuv, vec2(0.0))) && all(lessThanEqual(fuv, vec2(1.0)))) {
			v_flush = textureLod(burn_flush_map, fuv, 0.0).r;
			v_tint *= mix(vec3(1.0), vec3(1.10, 1.22, 0.84), v_flush);
		}
	}
	// Grazed turf is all young leaf: a brighter, fresher green than the standing sward.
	v_tint *= mix(vec3(1.0), vec3(1.06, 1.16, 0.88), v_graze);
	// A fairy ring's lush bands: a deeper, bluer green (nitrogen).
	v_tint *= mix(vec3(1.0), vec3(0.6, 0.86, 0.5), clamp(fairy.x + fairy.z, 0.0, 1.0));
	if (weather_map_rect.z > 0.0) {
		vec2 half_texel = 0.5 / vec2(textureSize(weather_state_map, 0));
		vec2 uv = clamp((base_w.xz - weather_map_rect.xy) / weather_map_rect.zw, half_texel, 1.0 - half_texel);
		v_wet = textureLod(weather_state_map, uv, 0.0).g;
		// Hoarfrost (WeatherMap3D dew_and_frost): a few tenths of a millimetre frost the sward over.
		v_frost = smoothstep(0.0, 0.3, textureLod(weather_frost_map, uv, 0.0).r);
	}
	if (weather_contact_rect.z > 0.0) {
		// Wet where water has been (WeatherMap3D contact wetness), easing out at its square's edge.
		vec2 cuv = (base_w.xz - weather_contact_rect.xy) / weather_contact_rect.zw;
		vec2 ce = min(cuv, 1.0 - cuv);
		float edge = smoothstep(0.0, 0.1, min(ce.x, ce.y));
		if (edge > 0.0) {
			v_wet = max(v_wet, textureLod(weather_contact_map, cuv, 0.0).r * edge);
		}
	}
}

uniform float dryness;
uniform float sward_lai = 1.0;

void fragment() {
	float t = smoothstep(0.0, 1.0, v_t);
	// Shade keeps grass green; a sunny slope dries it first.
	float dry_share = clamp(dryness * (1.0 - 0.7 * (1.0 - clamp(v_light, 0.0, 1.0))) + 0.25 * smoothstep(1.0, 1.3, v_light), 0.0, 1.0);
	// The fire took the old growth: regrowth carries no dry tips or thatch yet (a burn reads greener than the sward
	// around it through its first season).
	dry_share *= 1.0 - 0.85 * v_flush;
	// Grazed turf is mostly fresh regrowth: few dry tips survive the bites.
	dry_share *= 1.0 - 0.7 * v_graze;
	vec3 tipc = mix(tip_color, dry_color, step(1.0 - dry_share, v_dry) * smoothstep(0.3, 1.0, v_t));
	vec3 c = mix(base_color, tipc, t) * v_tint;
	// The bitten ends: torn, not cut (deer have no upper incisors), they dry pale over a few days.
	c = mix(c, mix(dry_color, vec3(0.62, 0.6, 0.45), 0.4), v_graze * smoothstep(0.82, 1.0, v_t) * step(0.3, fract(v_dry * 17.3)) * 0.7);
	// Some blades of the dry fraction are dead from the root (thatch), straw that greys with age.
	float dead = step(1.0 - max(dry_share * 0.3, dormant * 0.85 * (1.0 - v_flush)), v_dry);
	// Dead leaves bleach to a pale grey buff, not the warm straw of a dry tip (a saturated straw read
	// as orange specks on a dark winter sward).
	vec3 straw = mix(dry_color, vec3(dot(dry_color, vec3(0.3, 0.59, 0.11))) * vec3(1.0, 0.96, 0.88), 0.5) * 0.85;
	c = mix(c, straw * mix(0.7, 1.0, t) * (0.75 + 0.5 * fract(v_dry * 31.0)), dead);
	// Browned in a fairy ring's dead band: a warmer, darker brown than winter straw, the base still a little green.
	c = mix(c, mix(dry_color, straw, 0.4) * vec3(0.85, 0.72, 0.55) * mix(0.75, 1.0, t), v_fairy_dead * mix(0.7, 1.0, t));
	// Out from under the snow: pale, yellowed leaves (grown in the dark), then snow mould: grey-white lint at the melt,
	// straw to grey-brown once it dries.
	if (v_melt.x + v_melt.y > 0.0) {
		vec3 pale = mix(c, vec3(dot(c, vec3(0.3, 0.59, 0.11))) * vec3(1.15, 1.12, 0.8) + vec3(0.04, 0.04, 0.0), 0.65);
		c = mix(c, pale, v_melt.x);
		vec3 lint = vec3(0.72, 0.71, 0.69);
		vec3 crust = mix(straw, vec3(0.6, 0.57, 0.52), 0.6);
		c = mix(c, mix(lint, crust, smoothstep(1.0, 5.0, v_melt.z)), v_melt.y * mix(0.8, 1.0, t));
	}
	// Flood-washed: yellowed where it lay flat, a grey-brown film of silt that pales as it dries.
	if (v_wash.x + v_wash.y > 0.0) {
		c = mix(c, mix(c, straw, 0.5), v_wash.x * 0.5);
		c = mix(c, vec3(0.34, 0.3, 0.23) * mix(0.8, 1.0, t), v_wash.y * 0.55);
	}
	// Cut grass curing: it wilts duller within the day, then cures to a bright green-gold hay (well-made hay "looks
	// bright"; bleached hay is weathered: FAO), each blade a little its own shade; the cut ends pale.
	// Fresh stubble: below the cut the stem bases and sheaths are pale yellow-green (grown in the sward's shade), the
	// cut ends paler and straw-tinged; the regrowth above the stub keeps the blade's own green.
	if (v_cut.y > 0.0) {
		// Yellow-green, not grey: the sheaths hold less chlorophyll, not none (a first try, desaturated to the
		// blade's brightness, read as a grey-white field).
		vec3 sheath = mix(c * vec3(1.3, 1.15, 0.55), straw, 0.2);
		float below = 1.0 - smoothstep(v_cut.y - 0.04, v_cut.y + 0.01, v_t);
		float cut_end = smoothstep(v_cut.y - 0.12, v_cut.y - 0.02, v_t) * below;
		c = mix(c, mix(sheath, straw * 1.15, 0.5 * cut_end), below * v_cut.z * 0.65);
	}
	if (v_hay >= 0.0) {
		vec3 wilted = mix(c, vec3(dot(c, vec3(0.3, 0.59, 0.11))) * vec3(0.92, 1.06, 0.78), 0.3);
		vec3 hay_c = vec3(0.42, 0.4, 0.17) * (0.8 + 0.4 * fract(v_dry * 23.0)); // Green-gold, not grey (PC pair 23 Nov).
		c = mix(wilted, hay_c, smoothstep(0.3, 3.0, v_hay) * 0.85);
		// Rained on while curing: leached and bleached, the green gone to a dull brown-tan (a lighter grey first read as white).
		c = mix(c, vec3(dot(c, vec3(0.3, 0.59, 0.11))) * vec3(0.95, 0.85, 0.62), v_cut.z * 0.75);
	}
	// Prefiltered with distance (Bruneton and Neyret 2012, non-linear pre-filtering): once blades are a pixel or
	// two wide, each blade's own base-to-tip ramp and its dry or dead odd one out alias into grit; their mean over
	// the sward is what a pixel should see. Past 6 m the blade's colour eases towards that mean (the soft, slightly
	// diffused read of a far meadow), and below its roughness rises (the sheen of many blades averages out).
	{
		vec3 tip_mean = mix(tip_color, dry_color, dry_share * 0.6);
		vec3 mean_c = mix(base_color, tip_mean, 0.6) * v_tint;
		float dead_share = max(dry_share * 0.3, dormant * 0.85 * (1.0 - v_flush));
		mean_c = mix(mean_c, straw * 0.85, dead_share);
		c = mix(c, mean_c, 0.5 * v_soft);
	}
	// Midrib and parallel veins (monocot leaves), faded out once a blade is under a few pixels wide.
	float x = abs(UV.x - 0.5) * 2.0;
	float fw = max(fwidth(UV.x), 1e-4);
	float detail = 1.0 - smoothstep(0.08, 0.25, fw);
	float rib = 1.0 - smoothstep(0.0, 0.12 + fw, x);
	float veins = 0.5 + 0.5 * cos(x * 6.2832 * 3.5);
	c *= 1.0 + detail * (0.18 * rib - 0.07 * veins * (1.0 - rib) - 0.12 * x * x);
	// Folded at the mid vein (Pesce 2024, "Making a greener grass"; keeled and V-folded grass leaves): each half of the
	// blade is flat and tilted to its side, not rounded, so the strand sheen breaks into a thin bright line along one
	// half. The vertex normal leans 0.45 across at the edges and interpolates linearly; this bends each half to a
	// constant 0.375, with fibre_shading, and goes as the blade narrows to a few pixels or softens with distance.
	{
		float s = (UV.x - 0.5) * 2.0;
		float half_sign = s / (abs(s) + 2.0 * fw);
		float fold = fibre_shading * detail * (1.0 - v_soft) * (half_sign * 0.375 - s * 0.45);
		fold *= FRONT_FACING ? 1.0 : -1.0; // A back face's normal is flipped across its tangent too.
		NORMAL_MAP = vec3(0.5 + 0.5 * clamp(fold, -1.0, 1.0), 0.5, 1.0);
	}
	c = vegetation_light_tint(c, v_light);
	// Wet blades (Lagarde 2013, low porosity): a little darker and glossier.
	float wet_factor = mix(1.0, 0.2, 0.25);
	// Trodden flat: bruised and soiled, and its waxy face no longer catches the sky at a grazing
	// angle (lying blades read frosted white on a worn path's edge, docs 17 Nov).
	c *= mix(1.0, 0.72, v_trod);
	// Read here at the blade's own point, not passed from vertex(): the path tracer reruns vertex() for a merged
	// MultiMesh with the field's transform, not the blade's.
	vec2 fire_xz = (INV_VIEW_MATRIX * vec4(VERTEX, 1.0)).xz;
	vec4 fire_f = fire_state_at(fire_xz);
	float v_burn = fire_f.x;
	float v_glow = fire_f.y;
	if (v_burn > 0.0) {
		// Charred stubble, with flecks of white ash where blades burnt out, as many as the ground's ash share
		// (grass_ash: a dry burn's white ash, a damp one's char).
		float fleck = step(1.0 - mix(0.02, 0.5, fire_ash_at(fire_xz)), fract(v_dry * 41.0)) * smoothstep(0.6, 1.0, v_burn);
		vec3 ch = mix(vec3(0.035, 0.03, 0.027), vec3(0.35, 0.34, 0.33), fleck);
		c = mix(c, ch, smoothstep(0.0, 0.5, v_burn));
	}
	// Mud thrown or splashed onto it (mud round 7): the blade's height as a 30 cm sward's.
	vec3 mud_w = (INV_VIEW_MATRIX * vec4(VERTEX, 1.0)).xyz;
	float mud_c = grass_mud_apply(grass_ground_mud(mud_w.xz), mud_w, v_t * 0.3, c);
	// Salt burn: the tips first, then down the blade.
	c = mix(c, vec3(0.3, 0.22, 0.1) * v_tint, v_salt * smoothstep(1.0 - v_salt, 1.2 - 0.5 * v_salt, v_t));
	// Hoarfrost rimes the stubs (frozen, they do not look wet).
	float frost_c = v_frost * mix(0.4, 1.0, v_t);
	c = mix(c, vec3(0.78, 0.8, 0.84), 0.6 * frost_c);
	float wet_c = v_wet * (1.0 - frost_c);
	// Under the snow surface the blade is the snow's (the pack's albedo stands for it).
	c = mix(c, vec3(0.85), v_snow);
	ALBEDO = c * mix(1.0, wet_factor, wet_c);
	ROUGHNESS = mix(mix(mix(mix(0.55, 0.75, v_trod) * mix(1.0, wet_factor, 0.5 * wet_c) * mix(1.0, 0.4, wet_c), 0.85, mud_c), 0.95, smoothstep(0.0, 0.5, v_burn)), 0.85, frost_c);
	ROUGHNESS = mix(ROUGHNESS, 0.7, v_snow);
	// Cut hay lying face up: dry and matt, so it does not mirror the sky into a grey-white sheet (the windrows of the
	// 23 Nov PC pair read pale grey in both raster and path tracing).
	float hay_dry = v_hay >= 0.0 ? smoothstep(0.3, 2.0, v_hay) : 0.0;
	ROUGHNESS = mix(ROUGHNESS, max(ROUGHNESS, 0.8), hay_dry);
	// Fibre shading: the strand sheen; wet, frosted, muddy or snow-covered blades lose it (a film or a crust over the fibres).
	float fib = fibre_shading * (1.0 - max(wet_c, max(frost_c, max(mud_c, v_snow))));
	fib *= 1.0 - 0.6 * hay_dry;
	// Prefiltered, as the path tracer's many samples a pixel do: once a blade is under a few pixels wide its strand
	// highlight averages back to the round lobe (full from about 3 pixels wide, gone by 1.25: fwidth(UV.x)), and the lobe
	// takes the normal's spread across the pixel as extra roughness (Tokuyoshi and Kaplanyan 2019, "Improved Geometric
	// Specular Antialiasing": alpha^2 += min(2 variance, 0.18)). Without them a single lit pixel of a thin blade
	// flashes white.
	fib *= 1.0 - smoothstep(0.3, 0.8, fw);
	ROUGHNESS = mix(ROUGHNESS, max(ROUGHNESS, 0.8), v_soft);
	{
		// fwidth(), not dFdx()/dFdy(): the path tracer's translation has only fwidth() (about zero there, where the
		// samples do this). (|dx| + |dy|)^2 / 4 stands for the variance's 2 (dx^2 + dy^2) / 4.
		vec3 nw = fwidth(NORMAL);
		float kernel = min(0.25 * dot(nw, nw), 0.18);
		float a = ROUGHNESS * ROUGHNESS;
		ROUGHNESS = sqrt(sqrt(a * a + kernel));
	}
	// The burning band: the stubs' tips glow as the ground's embers do (the char decal's glow energy).
	EMISSION = fire_glow_color(v_glow) * 4.0 * smoothstep(0.4, 1.0, v_t) * step(1e-3, v_burn);
	SPECULAR = mix(mix(0.35, 0.2, v_trod), 0.2, hay_dry);
	// The highlight drawn out along the blade (ANISOTROPY_FLOW onto BINORMAL), so lit blades carry it as streaks down
	// their length, the fibre look; across the blade it only dulled the sward's sheen (docs/ideas/grass.md). Only the
	// lobe's shape changes, not its energy: raster and the path tracer agree on that, where a smoother, shinier blade
	// brightened the path-traced sward far more than the raster one.
	ANISOTROPY = fib;
	ANISOTROPY_FLOW = vec2(0.0, 1.0);
	BACKLIGHT = ALBEDO * 0.7;
	// Sky light inside the sward (Monsi and Saeki 1953): diffuse light falls off as
	// exp(-k L) with the leaf area L above, k about 0.8 for upright blades, and light the blades
	// scatter on slows that to k sqrt(1 - scattering) (Goudriaan 1977). In AO, so only the
	// raster's ambient takes it: its shadow maps already darken the sun there, and the path
	// tracer sees the blades themselves (it used to take this darkening in the albedo too).
	float scatter = clamp(dot(ALBEDO, vec3(0.3, 0.59, 0.11)) * 1.7, 0.0, 0.95);
	AO = exp(-0.8 * sqrt(1.0 - scatter) * sward_lai * v_cut.x * (1.0 - v_t));
}

)";
	}
	// A rebuild keeps the compiled shader when its code is unchanged (most settings are uniforms):
	// only the material is new, so no pipeline recompiles on a settings change.
	if (blade_shader.is_null() || blade_shader->get_code() != code) {
		blade_shader.instantiate();
		blade_shader->set_code(add_camera_cutout(code));
	}
	const Ref<Shader> shader = blade_shader;
	material.instantiate();
	material->set_shader(shader);
	lodge_dirty = true; // A new material: the lodging map goes to it at the next sweep.
	material->set_shader_parameter("fairy_rings", fairy_rings && crop == CROP_NONE ? 1.0f : 0.0f);
	material->set_shader_parameter("deer_beds", deer_beds && crop == CROP_NONE ? 1.0f : 0.0f);
	material->set_shader_parameter("deer_bed_day", deer_bed_day);
	material->set_shader_parameter("terracettes", terracettes && crop == CROP_NONE ? 1.0f : 0.0f);
	// A sown crop covers its field whatever the patches of wild cover round it.
	material->set_shader_parameter("climate_bare", climate_bare && crop == CROP_NONE);
	melt_day = -2; // A new material: the snowmelt bands again on the next season step.
	flood_usec = 0; // And the flood map.
	material->set_shader_parameter("base_color", base_color);
	material->set_shader_parameter("tip_color", tip_color);
	material->set_shader_parameter("dry_color", dry_color);
	material->set_shader_parameter("dryness", _dryness());
	material->set_shader_parameter("dormant", _dormant());
	material->set_shader_parameter("season_height", season_height);
	material->set_shader_parameter("color_variation", color_variation);
	material->set_shader_parameter("fallback_wind", fallback_wind);
	material->set_shader_parameter("wind_response", wind_response);
	material->set_shader_parameter("trample_rect", Vector4());
	_publish_burn_flush();
	_publish_graze();
	_publish_mow();
	material->set_shader_parameter("view_facing", view_facing);
	material->set_shader_parameter("fibre_shading", fibre_shading);
	material->set_shader_parameter("fade_band", fade_band);
	material->set_shader_parameter("ring_band", ring_band);
	material->set_shader_parameter("ground_data_map", ground_data_tex);
	material->set_shader_parameter("ground_data_rect", ground_data_rect);
	// One-sided leaf area per ground area: blades per m2 x mean height x width x 0.7 (the taper).
	material->set_shader_parameter("sward_lai", density * 0.5f * (blade_height.x + blade_height.y) * blade_width * 0.7f);
	if (blade_mesh.is_valid()) {
		blade_mesh->surface_set_material(0, material);
	}
	if (crop != CROP_NONE) {
		_crop_uniforms(material);
		if (crop_mesh_far.is_valid()) {
			crop_mesh_far->surface_set_material(0, material);
		}
	}
}

int GrassField3D::_lod_for(float p_distance) const {
	// Full density over the inner half, then a half and a quarter (Ghost of Tsushima holds its blades' density far
	// out and thins late; Wohllaib GDC 2021). The far rings carry on from the quarter.
	const float f = p_distance / MAX(radius, 0.001f);
	return f < 0.5f ? 0 : (f < 0.75f ? 1 : 2);
}

// The cereals (vegetation.md, farmland plan): modern straw lengths to the ear's base, ear and awn
// sizes and leaf blades from agronomy texts (AHDB wheat and barley growth guides; Kirby and
// Appleyard, "Cereal development guide"). Colours: glaucous leaf green, the ripe ear and straw.
GrassField3D::CropShape GrassField3D::_crop_shape(Crop p_crop) {
	switch (p_crop) {
		case CROP_BARLEY:
			// Two-row barley: a slim ear with long awns that bends right over when ripe.
			return { 0.68f, 0.075f, 0.0048f, 0.13f, 14, 0, 0.19f, 0.0065f, 1.0f, Color(0.21f, 0.35f, 0.14f), Color(0.80f, 0.71f, 0.50f), Color(0.80f, 0.72f, 0.53f) };
		case CROP_OATS:
			// An open panicle of drooping spikelets on whorled branches.
			return { 0.85f, 0.22f, 0.0012f, 0.0f, 0, 15, 0.24f, 0.0072f, 0.35f, Color(0.19f, 0.34f, 0.15f), Color(0.80f, 0.76f, 0.58f), Color(0.79f, 0.72f, 0.50f) };
		case CROP_RYE:
			// The tallest: a long slender ear with short awns, blue-grey leaves, nodding a little.
			return { 1.25f, 0.11f, 0.0042f, 0.035f, 18, 0, 0.22f, 0.006f, 0.45f, Color(0.23f, 0.34f, 0.22f), Color(0.68f, 0.61f, 0.46f), Color(0.74f, 0.66f, 0.49f) };
		case CROP_WHEAT:
		default:
			// Bread wheat: an awnless, flattened ear of two rows of spikelets, held upright.
			return { 0.80f, 0.09f, 0.0055f, 0.0f, 0, 0, 0.22f, 0.007f, 0.3f, Color(0.20f, 0.33f, 0.14f), Color(0.75f, 0.62f, 0.39f), Color(0.78f, 0.69f, 0.49f) };
	}
}

// After harvest the combine leaves its straw in a swath behind it, down the middle of each pass:
// passes a third of the tramline spacing (an 8 m header on 24 m tramlines), the swath about 1.4 m
// wide.
bool GrassField3D::_crop_swath(const CropPlot &p_pl, float &r_pitch, float &r_half) {
	if (p_pl.current < 99.0f) {
		return false;
	}
	r_pitch = p_pl.tramline_spacing > 0.0f ? p_pl.tramline_spacing / 3.0f : 7.5f;
	r_half = 0.7f;
	return true;
}

namespace {
float seg_distance(const Vector2 &p_q, const Vector2 &p_a, const Vector2 &p_b) {
	const Vector2 ab = p_b - p_a;
	const float t = CLAMP((p_q - p_a).dot(ab) / MAX(ab.length_squared(), 1e-8f), 0.0f, 1.0f);
	return p_q.distance_to(p_a + ab * t);
}

float polygon_edge_distance(const Vector2 &p_q, const PackedVector2Array &p_poly, int *r_edge = nullptr) {
	float best = 1e30f;
	const int n = p_poly.size();
	for (int i = 0; i < n; i++) {
		const float dd = seg_distance(p_q, p_poly[i], p_poly[(i + 1) % n]);
		if (dd < best) {
			best = dd;
			if (r_edge) {
				*r_edge = i;
			}
		}
	}
	return best;
}
} // namespace

// The stage on a day from the sowing day: days after sowing to the BBCH code for a crop in a mild
// temperate climate (AHDB "Wheat growth guide" and "Barley growth guide"; HGCA timings for England):
// autumn sowing (day 200 on) over-winters in tillering; spring sowing runs through in five months.
// After harvest the stubble stands until the next sowing.
float GrassField3D::crop_calendar_stage(int p_sown_day, int p_harvest_day, int p_day) {
	static const float WINTER[][2] = { { 0, 0 }, { 14, 10 }, { 30, 13 }, { 60, 21 }, { 150, 25 }, { 175, 30 }, { 210, 37 }, { 220, 39 }, { 235, 51 }, { 246, 59 }, { 255, 65 }, { 265, 71 }, { 272, 75 }, { 286, 85 }, { 291, 87 }, { 302, 92 } };
	static const float SPRING[][2] = { { 0, 0 }, { 14, 10 }, { 30, 13 }, { 45, 21 }, { 55, 30 }, { 75, 39 }, { 85, 51 }, { 92, 59 }, { 98, 65 }, { 110, 71 }, { 118, 75 }, { 130, 85 }, { 138, 92 } };
	const bool winter = p_sown_day >= 200;
	const float(*table)[2] = winter ? WINTER : SPRING;
	const int count = winter ? int(sizeof(WINTER) / sizeof(WINTER[0])) : int(sizeof(SPRING) / sizeof(SPRING[0]));
	const int since = ((p_day - p_sown_day) % 365 + 365) % 365;
	int harvest = p_harvest_day >= 0 ? ((p_harvest_day - p_sown_day) % 365 + 365) % 365 : int(table[count - 1][0]) + 10;
	if (since >= harvest) {
		return 99.0f;
	}
	for (int i = 1; i < count; i++) {
		if (since < table[i][0]) {
			const float t = (since - table[i - 1][0]) / (table[i][0] - table[i - 1][0]);
			return Math::lerp(table[i - 1][1], table[i][1], t);
		}
	}
	return table[count - 1][1];
}

void GrassField3D::_update_plot_stages(int p_day) {
	for (CropPlot &pl : plots) {
		pl.current = pl.sown_day >= 0 && p_day >= 0 ? crop_calendar_stage(pl.sown_day, pl.harvest_day, p_day) : (pl.stage >= 0.0f ? pl.stage : crop_stage);
	}
	if (material.is_valid() && crop != CROP_NONE) {
		_crop_uniforms(material);
	}
	crop_plots_dirty = true;
}

void GrassField3D::set_fields(const Array &p_fields) {
	fields = p_fields;
	plots.clear();
	for (int i = 0; i < fields.size(); i++) {
		const Dictionary f = fields[i];
		CropPlot pl;
		pl.polygon = f.get("polygon", PackedVector2Array());
		if (pl.polygon.size() < 3) {
			continue;
		}
		pl.bounds = Rect2(pl.polygon[0], Vector2());
		for (const Vector2 &v : pl.polygon) {
			pl.bounds.expand_to(v);
		}
		pl.clockwise = Geometry2D::is_polygon_clockwise(pl.polygon);
		pl.drill_angle = f.get("drill_angle", drill_angle);
		pl.row_spacing = MAX(float(f.get("row_spacing", row_spacing)), 0.03f);
		pl.stage = f.get("stage", -1.0f);
		pl.sown_day = f.get("sown_day", -1);
		pl.harvest_day = f.get("harvest_day", -1);
		pl.tramline_spacing = MAX(float(f.get("tramline_spacing", 24.0f)), 0.0f);
		pl.tramline_gauge = f.get("tramline_gauge", 1.8f);
		pl.headland = MAX(float(f.get("headland", 12.0f)), 0.0f);
		plots.push_back(pl);
		if (plots.size() >= uint32_t(MAX_PLOTS)) {
			WARN_PRINT(vformat("GrassField3D \"%s\": only the first %d fields are grown.", get_name(), MAX_PLOTS));
			break;
		}
	}
	_update_plot_stages(crop_day);
	_crop_fields_changed_notify();
	_queue_hedges();
	rebuild();
}

void GrassField3D::set_hay_meadows(const Array &p_meadows) {
	hay_meadows = p_meadows;
	const bool had = !meadows.is_empty();
	meadows.clear();
	for (int i = 0; i < hay_meadows.size(); i++) {
		const Dictionary f = hay_meadows[i];
		HayMeadow m;
		m.polygon = f.get("polygon", PackedVector2Array());
		if (m.polygon.size() < 3) {
			continue;
		}
		m.bounds = Rect2(m.polygon[0], Vector2());
		for (const Vector2 &v : m.polygon) {
			m.bounds.expand_to(v);
		}
		m.row_angle = f.get("row_angle", 0.0f);
		m.mow_day = f.get("mow_day", -1);
		m.days = f.get("days", -1.0f);
		m.rain_fixed = f.get("rain", -1.0f);
		m.rain = MAX(m.rain_fixed, 0.0f);
		meadows.push_back(m);
		if (meadows.size() >= uint32_t(MAX_HAY_MEADOWS)) {
			WARN_PRINT(vformat("GrassField3D \"%s\": only the first %d hay meadows are mown.", get_name(), MAX_HAY_MEADOWS));
			break;
		}
	}
	_build_mow_map();
	_update_mow_state(hay_day);
	hay_generation++;
	_hay_register();
	if (had != !meadows.is_empty()) {
		rebuild(); // The tiles' culling boxes take the rows' reach.
	}
}

// Rain on the cut grass while it lies curing (swaths, tedded, windrows: up to the carting on day 4): the weather's
// rainfall over each meadow's middle, added up in the weather's own time. Rain leaches the cut grass and, with the sun,
// bleaches its green ("prolonged exposure to sun and rain reduces green color": Manitoba Agriculture, How Rain Affects
// Hay Quality); the shaders dull the hay by it.
void GrassField3D::_hay_rain_step() {
	const uint64_t now = OS::get_singleton()->get_ticks_usec();
	const double real_s = hay_rain_usec > 0 ? double(now - hay_rain_usec) * 1e-6 : 0.0;
	hay_rain_usec = now;
	const WorldWeather *wm = WorldContracts::weather();
	if (real_s <= 0.0 || real_s > 30.0 || !wm->has_provider()) {
		return;
	}
	const double hours = real_s * wm->time_scale() / 3600.0;
	for (HayMeadow &m : meadows) {
		if (m.rain_fixed >= 0.0f || m.current < 0.0f || m.current >= 4.0f) {
			continue;
		}
		const Vector2 c = m.bounds.get_center();
		m.rain += float(MAX(wm->rainfall_at(Vector3(c.x, get_global_position().y, c.y)), 0.0f) * hours); // Polygons are world xz.
	}
}

// Days since a meadow was mown on a day of the year (the year wraps), or its fixed days; -1 not mown this year.
static float hay_days_since(int p_mow_day, float p_days, int p_day) {
	if (p_days >= 0.0f) {
		return p_days;
	}
	if (p_mow_day < 0 || p_day < 0) {
		return -1.0f;
	}
	const int d = p_day - p_mow_day;
	return d >= 0 ? float(d) : -1.0f;
}

void GrassField3D::_update_mow_state(int p_day) {
	hay_day = p_day;
	if (meadows.is_empty()) {
		mow_state_texture.unref();
		_publish_mow();
		_free_bales();
		return;
	}
	Ref<Image> img = Image::create_empty(MAX_HAY_MEADOWS, 1, false, Image::FORMAT_RGBAF);
	for (uint32_t i = 0; i < meadows.size(); i++) {
		const float was = meadows[i].current;
		meadows[i].current = hay_days_since(meadows[i].mow_day, meadows[i].days, p_day);
		if (was != meadows[i].current && hay_nodes.has(this)) {
			hay_generation++;
		}
		if (meadows[i].rain_fixed < 0.0f && (meadows[i].current < 0.0f || meadows[i].current < was)) {
			meadows[i].rain = 0.0f; // Not lying cut, or mown again: no rain on it yet.
		}
		meadows[i].rain_sent = meadows[i].rain;
		img->set_pixel(int(i), 0, Color(meadows[i].current, meadows[i].row_angle, meadows[i].rain, 1.0f));
	}
	if (mow_state_texture.is_valid()) {
		mow_state_texture->update(img);
	} else {
		mow_state_texture = ImageTexture::create_from_image(img);
	}
	_publish_mow();
	_queue_bales();
}

// The meadows' index map: 0.5 m texels over their bounds (coarser past 1024 a side), the first meadow over a point wins.
void GrassField3D::_build_mow_map() {
	if (meadows.is_empty()) {
		mow_texture.unref();
		mow_rect = Rect2();
		return;
	}
	Rect2 r = meadows[0].bounds;
	for (const HayMeadow &m : meadows) {
		r = r.merge(m.bounds);
	}
	r = r.grow(0.5f);
	const float cell = MAX(0.5f, MAX(r.size.x, r.size.y) / 1024.0f);
	const int nx = CLAMP(int(Math::ceil(r.size.x / cell)), 1, 1024);
	const int ny = CLAMP(int(Math::ceil(r.size.y / cell)), 1, 1024);
	Vector<uint8_t> data;
	data.resize(nx * ny);
	uint8_t *w = data.ptrw();
	for (int z = 0; z < ny; z++) {
		for (int x = 0; x < nx; x++) {
			const Vector2 q = r.position + Vector2((x + 0.5f) * cell, (z + 0.5f) * cell);
			uint8_t v = 0;
			for (uint32_t i = 0; i < meadows.size(); i++) {
				if (meadows[i].bounds.has_point(q) && Geometry2D::is_point_in_polygon(q, meadows[i].polygon)) {
					v = uint8_t(i + 1);
					break;
				}
			}
			w[z * nx + x] = v;
		}
	}
	mow_texture = ImageTexture::create_from_image(Image::create_from_data(nx, ny, false, Image::FORMAT_R8, data));
	mow_rect = Rect2(r.position, Vector2(nx * cell, ny * cell));
}

void GrassField3D::_publish_mow() {
	if (material.is_null()) {
		return;
	}
	if (meadows.is_empty() || mow_texture.is_null() || mow_state_texture.is_null()) {
		material->set_shader_parameter("mow_rect", Vector4());
		if (mow_published) {
			RenderingServer::get_singleton()->global_shader_parameter_set("grass_mow_rect", Vector4());
			mow_published = false;
		}
		return;
	}
	material->set_shader_parameter("mow_map", mow_texture);
	material->set_shader_parameter("mow_state", mow_state_texture);
	material->set_shader_parameter("mow_rect", Vector4(mow_rect.position.x, mow_rect.position.y, mow_rect.size.x, mow_rect.size.y));
	if (ground_tint_enabled && is_inside_tree()) {
		// The field that colours the ground hands the meadows to grass_ground() too, for the far rows.
		RenderingServer *rs = RenderingServer::get_singleton();
		rs->global_shader_parameter_set("grass_mow_map", mow_texture->get_rid());
		rs->global_shader_parameter_set("grass_mow_state", mow_state_texture->get_rid());
		rs->global_shader_parameter_set("grass_mow_rect", Vector4(mow_rect.position.x, mow_rect.position.y, mow_rect.size.x, mow_rect.size.y));
		mow_published = true;
	}
}

void GrassField3D::_hay_register() {
	const bool want = is_inside_tree() && !meadows.is_empty();
	if (want == hay_nodes.has(this)) {
		return;
	}
	if (want) {
		hay_nodes.push_back(this);
	} else {
		hay_nodes.erase(this);
	}
	hay_generation++;
}

void GrassField3D::_queue_bales() {
	if (bales_queued || !is_inside_tree()) {
		return;
	}
	bales_queued = true;
	callable_mp(this, &GrassField3D::_build_bales).call_deferred();
}

void GrassField3D::_free_bales() {
	for (MultiMeshInstance3D *n : bale_nodes) {
		if (n) {
			n->queue_free();
		}
	}
	bale_nodes.clear();
	bale_count = 0;
}

// Baled: a round baler picks up the windrow and drops a bale each time its chamber is full, so the bales lie along
// the rows a bale's hay apart (its mass over the row's hay a metre: HAY_YIELD times the pitch), each row's first
// at its own point. 4 x 4 ft bales hold about 230 kg, 5 x 5 ft 450 kg (UF/IFAS EDIS AN326: 1,000 lb at 5 x 5, a
// 4 x 4 half of it); 4 x 5 ft between. Ends to the sides of the row, settled on the ground's slope.
void GrassField3D::_build_bales() {
	bales_queued = false;
	_free_bales();
	if (meadows.is_empty() || !is_inside_tree()) {
		return;
	}
	TerrainDataMap3D *map = Object::cast_to<TerrainDataMap3D>(get_node_or_null(data_map));
	if (!map) {
		return;
	}
	if (bale_meshes.is_empty()) {
		for (int i = 0; i < 6; i++) {
			bale_meshes.push_back(PlantGenerator::make_hay_bale(13100 + i, true));
		}
	}
	const int nv = int(bale_meshes.size());
	LocalVector<LocalVector<Transform3D>> xf;
	xf.resize(nv);
	const Transform3D to_local = get_global_transform().affine_inverse();
	auto ground = [&](const Vector2 &q) {
		const Vector3 p(q.x, 0.0f, q.y);
		return map->get_height_at(p) + _relief_y(p);
	};
	for (uint32_t mi = 0; mi < meadows.size(); mi++) {
		const HayMeadow &m = meadows[mi];
		if (m.current < 3.5f || m.current >= 5.0f) {
			continue;
		}
		// The same meadow on an earlier sward layer: its bales are that node's.
		bool other = false;
		for (const GrassField3D *g : hay_nodes) {
			if (g == this) {
				break;
			}
			for (const HayMeadow &o : g->meadows) {
				other = other || (o.polygon == m.polygon && o.row_angle == m.row_angle);
			}
		}
		if (other) {
			continue;
		}
		const Vector2 rd(Math::cos(m.row_angle), Math::sin(m.row_angle));
		const Vector2 rn(-rd.y, rd.x);
		float umin = 1e30f, umax = -1e30f, vmin = 1e30f, vmax = -1e30f;
		for (const Vector2 &v : m.polygon) {
			umin = MIN(umin, v.dot(rn));
			umax = MAX(umax, v.dot(rn));
			vmin = MIN(vmin, v.dot(rd));
			vmax = MAX(vmax, v.dot(rd));
		}
		const float per_m = HAY_YIELD * HAY_ROW_PITCH;
		for (int row = int(Math::floor(umin / HAY_ROW_PITCH)); row <= int(Math::ceil(umax / HAY_ROW_PITCH)); row++) {
			uint32_t h = hash_murmur3_one_32(uint32_t(row), hash_murmur3_one_32(mi * 7919u + 17u));
			auto rnd = [&]() {
				h = hash_murmur3_one_32(h, 0x9e3779b9u);
				return float(h & 0xFFFFFF) / float(0x1000000);
			};
			float v = vmin + rnd() * 40.0f; // The row's first bale.
			while (v < vmax) {
				const int variant = MIN(int(rnd() * nv), nv - 1);
				const AABB ab = bale_meshes[variant]->get_aabb();
				const float mass = ab.size.x > 1.4f ? 450.0f : (ab.size.y > 1.4f ? 360.0f : 230.0f);
				// On the windrow's centre line as the shader lays it (its wander included).
				const float sway = 0.2f * Math::sin(v * 0.06f + row * 1.7f) + 0.08f * Math::sin(v * 0.27f + row * 0.4f);
				const float uc = (row + 0.5f) * HAY_ROW_PITCH + sway;
				const Vector2 q = rn * uc + rd * v;
				const WorldWater *water = WorldContracts::water();
				const float gy = ground(q);
				const float wy = water ? water->height_at(Vector3(q.x, gy, q.y)) : -Math::INF;
				// Not in a pond or a ditch the meadow holds: a baler goes round water.
				const bool wet = (Math::is_finite(wy) && wy > gy - 0.05f) || (map && map->get_water_depth_at(Vector3(q.x, gy, q.y)) > 0.0f);
				if (!wet && m.bounds.has_point(q) && Geometry2D::is_point_in_polygon(q, m.polygon)) {
					const float e = 0.6f;
					const float hx = ground(q + rd * e) - ground(q - rd * e);
					const float hz = ground(q + rn * e) - ground(q - rn * e);
					const Vector3 up = Vector3(-hx * rd.x - hz * rn.x, 2.0f * e, -hx * rd.y - hz * rn.y).normalized();
					// Mesh +X (the axis) across the row, a little askew as it rolled out.
					const float yaw = Math::atan2(-rn.y, rn.x) + (rnd() - 0.5f) * 0.35f;
					Basis b(Vector3(0, 1, 0), yaw);
					const Vector3 axis = Vector3(0, 1, 0).cross(up);
					if (axis.length_squared() > 1e-10f) {
						b = Basis(axis.normalized(), Math::acos(CLAMP(up.y, -1.0f, 1.0f))) * b;
					}
					xf[variant].push_back(to_local * Transform3D(b, Vector3(q.x, ground(q) - 0.02f, q.y)));
				}
				v += mass / per_m * (0.85f + 0.3f * rnd());
			}
		}
	}
	for (int i = 0; i < nv; i++) {
		if (xf[i].is_empty()) {
			continue;
		}
		Ref<MultiMesh> mm;
		mm.instantiate();
		mm->set_transform_format(MultiMesh::TRANSFORM_3D);
		mm->set_mesh(bale_meshes[i]);
		mm->set_instance_count(xf[i].size());
		for (uint32_t k = 0; k < xf[i].size(); k++) {
			mm->set_instance_transform(k, xf[i][k]);
		}
		MultiMeshInstance3D *mmi = memnew(MultiMeshInstance3D);
		mmi->set_multimesh(mm);
		mmi->set_name(vformat("HayBales_%d", i));
		add_child(mmi, false, INTERNAL_MODE_BACK);
		bale_nodes.push_back(mmi);
		bale_count += int(xf[i].size());
	}
}

float GrassField3D::mown_days_at(const Vector3 &p_position) {
	float d = -1.0f;
	for (const GrassField3D *g : hay_nodes) {
		d = MAX(d, g->get_mown_days_at(p_position));
	}
	return d;
}

float GrassField3D::get_mown_days_at(const Vector3 &p_position) const {
	const Vector2 q(p_position.x, p_position.z);
	for (const HayMeadow &m : meadows) {
		if (m.bounds.has_point(q) && Geometry2D::is_point_in_polygon(q, m.polygon)) {
			return m.current;
		}
	}
	return -1.0f;
}

void GrassField3D::set_hedges(bool p_enabled) {
	if (hedges == p_enabled) {
		return;
	}
	hedges = p_enabled;
	_queue_hedges();
}

Ref<ArrayMesh> GrassField3D::make_voxel_shell(const Ref<Mesh> &p_mesh, float p_voxel) {
	return VegetationVoxelLod::make_shell(p_mesh, p_voxel);
}

void GrassField3D::set_hedge_voxel_lods(bool p_enabled) {
	if (hedge_voxel_lods == p_enabled) {
		return;
	}
	hedge_voxel_lods = p_enabled;
	_queue_hedges();
}

void GrassField3D::set_hedge_offset(float p_offset) {
	hedge_offset = MAX(p_offset, 0.0f);
	_queue_hedges();
}

int GrassField3D::get_hedge_segment_count() const {
	int n = 0;
	for (const Node3D *h : hedge_nodes) {
		for (int i = 0; i < h->get_child_count(); i++) {
			const MultiMeshInstance3D *mmi = Object::cast_to<MultiMeshInstance3D>(h->get_child(i));
			if (mmi && mmi->get_multimesh().is_valid() && String(mmi->get_name()).begins_with("Hedge") && String(mmi->get_name()).ends_with("lod0")) {
				n += mmi->get_multimesh()->get_instance_count();
			}
		}
	}
	return n;
}

void GrassField3D::_queue_hedges() {
	if (hedges_queued || !is_inside_tree()) {
		return;
	}
	hedges_queued = true;
	callable_mp(this, &GrassField3D::_build_hedges).call_deferred();
}

void GrassField3D::_free_hedges() {
	for (Node3D *h : hedge_nodes) {
		if (h) {
			h->queue_free();
		}
	}
	hedge_nodes.clear();
}

// Hedgerows (docs/ideas/vegetation.md, "Farmland", stage 3): a hawthorn hedge with blackthorn,
// hazel, field maple and dog rose through it (the commonest mixed hedge of lowland England and
// northern France; Rackham 1986, Pollard, Hooper and Moore 1974), managed as a flailed A or box
// (TreeGenerator SHRUB_HEDGE, 2 m segments). Each field's edge gets a line past the verge
// (hedge_offset), the species in runs of a few segments; a shared boundary gets one hedge (the
// lower-numbered field's); one gateway gap per field on its longest side.

void GrassField3D::_build_hedges() {
	hedges_queued = false;
	_free_hedges();
	const bool had_runs = !hedge_runs.is_empty();
	hedge_runs.clear();
	if (!hedges || crop == CROP_NONE || plots.is_empty() || !is_inside_tree()) {
		if (had_runs) {
			_crop_fields_changed_notify();
		}
		return;
	}
	Dictionary season;
	// season_source set: the season from the weather contract at the field.
	if (_follows_season() && WorldContracts::weather()->has_provider()) {
		season = WorldContracts::weather()->season_at(is_inside_tree() ? get_global_position() : Vector3());
	} else if (crop_day >= 0) {
		season["day_of_year"] = crop_day;
	} else {
		season["day_of_year"] = 200; // No calendar: midsummer leaf, as the crops' default stages.
	}
	hedge_day = int(season.get("day_of_year", -1));
	struct Mix {
		TreeGenerator::Preset preset;
		float share;
	};
	const Mix mix[] = { { TreeGenerator::PRESET_HAWTHORN, 0.55f }, { TreeGenerator::PRESET_BLACKTHORN, 0.15f }, { TreeGenerator::PRESET_HAZEL, 0.12f }, { TreeGenerator::PRESET_FIELD_MAPLE, 0.1f }, { TreeGenerator::PRESET_DOG_ROSE, 0.08f } };
	const int kinds = int(std_size(mix));
	const int VARIANTS = 3;
	Ref<PlantSpecies> species[5];
	LocalVector<Transform3D> xf[5][VARIANTS];
	// Hedgerow standards: trees let grow up out of the hedge every few tens of metres (field
	// maple and hawthorn grown on; Rackham 1986: most hedges hold some), so a hedge is not one
	// even line of bushes from afar.
	// Oak is the commonest hedgerow tree of lowland England, and the tallest here.
	const TreeGenerator::Preset std_presets[3] = { TreeGenerator::PRESET_FIELD_MAPLE, TreeGenerator::PRESET_HAWTHORN, TreeGenerator::PRESET_OAK };
	LocalVector<Transform3D> std_xf[3][VARIANTS];
	TerrainDataMap3D *map = Object::cast_to<TerrainDataMap3D>(get_node_or_null(data_map));
	const Transform3D to_local = get_global_transform().affine_inverse();
	const float base_y = get_global_position().y;
	const float seg = 2.0f;
	// Every crop node's fields: a hedge already round a neighbour's field is not doubled.
	const CropFieldMask all = get_crop_field_mask();
	int own_first = 0;
	for (const GrassField3D *g : crop_nodes) {
		if (g == this) {
			break;
		}
		own_first += g->plots.size();
	}
	for (int fi = 0; fi < int(plots.size()); fi++) {
		const PackedVector2Array &poly = plots[fi].polygon;
		const int n = poly.size();
		// Outward is to the right of each edge on an anticlockwise ring (x, z as x, y), left on a
		// clockwise one.
		const float out_sign = plots[fi].clockwise ? -1.0f : 1.0f;
		int gate_edge = 0;
		float longest = 0.0f;
		for (int i = 0; i < n; i++) {
			const float l = poly[i].distance_to(poly[(i + 1) % n]);
			if (l > longest) {
				longest = l;
				gate_edge = i;
			}
		}
		for (int i = 0; i < n; i++) {
			const Vector2 a = poly[i];
			const Vector2 b = poly[(i + 1) % n];
			const float len = a.distance_to(b);
			if (len < 1.0f) {
				continue;
			}
			const Vector2 dir = (b - a) / len;
			const Vector2 out = Vector2(dir.y, -dir.x) * out_sign;
			// Run a little past each end so the lines meet round a corner.
			const float s0 = -hedge_offset * 0.8f;
			const float s1 = len + hedge_offset * 0.8f;
			const int count = int(Math::floor((s1 - s0) / seg));
			const float start = s0 + 0.5f * ((s1 - s0) - count * seg);
			const float gate = len * 0.25f;
			float next_std = 10.0f + 30.0f * float(hash_murmur3_one_32(uint32_t(i), uint32_t(fi) * 7919u + 17u) & 0xFF) / 255.0f;
			float run_from = -1e9f, run_to = -1e9f;
			auto close_run = [&]() {
				if (run_to > run_from) {
					CropFieldMask::Hedge h;
					h.a = a + dir * run_from + out * hedge_offset;
					h.b = a + dir * run_to + out * hedge_offset;
					h.bounds = Rect2(h.a, Vector2()).expand(h.b).grow(h.half);
					hedge_runs.push_back(h);
				}
				run_from = run_to = -1e9f;
			};
			for (int k = 0; k < count; k++) {
				const float s = start + k * seg;
				if (run_to > -1e8f && Math::abs(s - run_to) > 0.01f) {
					close_run();
				}
				if (i == gate_edge && s + seg > gate - 3.0f && s < gate + 3.0f) {
					continue; // The gateway.
				}
				const Vector2 mid = a + dir * (s + 0.5f * seg) + out * hedge_offset;
				// Not inside any field, and on a shared boundary only once.
				bool skip = false;
				for (int j = 0; j < int(all.items.size()) && !skip; j++) {
					if (j == own_first + fi) {
						continue;
					}
					const CropFieldMask::Item &it = all.items[j];
					if (it.bounds.grow(hedge_offset * 2.0f + 1.0f).has_point(mid)) {
						CropFieldMask one;
						one.items.push_back(it);
						const float dd = one.edge_distance(mid, 100.0f);
						skip = dd < 0.0f || (dd < hedge_offset + 1.0f && j < own_first + fi);
					}
				}
				if (skip) {
					continue;
				}
				// The species in runs of about three segments.
				const uint32_t run = hash_murmur3_one_32(uint32_t(int(Math::floor((s + 1000.0f) / (seg * 3.0f)))), hash_murmur3_one_32(uint32_t(i), hash_murmur3_one_32(uint32_t(fi), 0x51ED270Bu)));
				float u = float(run & 0xFFFF) / 65535.0f;
				int kind = 0;
				while (kind < kinds - 1 && u > mix[kind].share) {
					u -= mix[kind].share;
					kind++;
				}
				const uint32_t hv = hash_murmur3_one_32(uint32_t(k), run);
				const int variant = int(hv % VARIANTS);
				auto h01 = [&](uint32_t p_salt) { return float(hash_murmur3_one_32(p_salt, hv) & 0xFFFF) / 65535.0f; };
				// Segment meshes are centred on their origin, 2 m along local +X. Each sits a little
				// off the line, and is a little longer than its step so neighbours grow into each
				// other (no row of separate round bushes from afar).
				const float side = (h01(1) - 0.5f) * 0.35f;
				const Vector2 mid_j = mid + out * side;
				Vector3 at(mid_j.x, base_y, mid_j.y);
				if (map) {
					const Vector3 pm(mid_j.x, 0.0f, mid_j.y);
					at.y = map->get_height_at(pm) - 0.05f;
				}
				// The segment runs along its local +X: turn that onto the edge, either way round.
				const float yaw = Math::atan2(-dir.y, dir.x) + (h01(2) < 0.5f ? 0.0f : float(Math::PI));
				// A hedge's height wanders along it over tens of metres (cut one year, grown out
				// another, gappy where a shrub died), plus each shrub's own.
				const float along = s + float(i) * 37.0f + float(fi) * 101.0f;
				const float tall = CLAMP(1.0f + 0.2f * Math::sin(along * 0.083f + float(fi)) + 0.1f * Math::sin(along * 0.29f + 1.3f) + 0.16f * (h01(3) - 0.5f), 0.7f, 1.4f);
				const float wide = 0.9f + 0.25f * h01(4) + 0.15f * (tall - 1.0f);
				const float lng = 1.12f + 0.12f * h01(5);
				const Basis bs = Basis(Vector3(0, 1, 0), yaw) * Basis::from_scale(Vector3(lng, tall, wide));
				xf[kind][variant].push_back(to_local * Transform3D(bs, at));
				// A standard every 35 to 80 m along the line.
				// Not at a field's corner: the gateways and turning space are there, and a corner
				// tree would shade two fields.
				if (s >= next_std && s > 8.0f && s < len - 8.0f) {
					const float pick = h01(6);
					const int sk = pick < 0.35f ? 0 : (pick < 0.6f ? 1 : 2);
					const float sc = sk == 0 ? 0.45f + 0.2f * h01(7) : (sk == 1 ? 0.7f + 0.2f * h01(7) : 0.55f + 0.2f * h01(7)); // Oak: 10 to 14 m of a 19 m open-grown crown.
					// Each its own: a crown wider one way than the other, taller or squatter, and a
					// lean of up to 8 degrees, most often away from the field (light from the open side).
					const float sx = sc * (0.85f + 0.3f * h01(11));
					const float sy = sc * (0.85f + 0.35f * h01(12));
					const float sz = sc * (0.85f + 0.3f * h01(13));
					const float lean = Math::deg_to_rad(8.0f) * h01(14);
					const Vector3 lean_axis = Vector3(dir.x, 0.0f, dir.y).rotated(Vector3(0, 1, 0), (h01(15) - 0.5f) * 1.2f);
					const Basis sb = Basis(lean_axis.normalized(), lean) * Basis(Vector3(0, 1, 0), h01(8) * float(Math::TAU)) * Basis::from_scale(Vector3(sx, sy, sz));
					std_xf[sk][int(hash_murmur3_one_32(9, hv) % VARIANTS)].push_back(to_local * Transform3D(sb, at));
					next_std = s + 35.0f + 45.0f * h01(10);
				}
				if (run_from < -1e8f) {
					run_from = s;
				}
				run_to = s + seg;
			}
			close_run();
		}
	}
	// The meadow and the plants keep out of the hedges' cores.
	_crop_fields_changed_notify();
	// One holder node, so code walking the tiles (MultiMeshInstance3D children) never meets them.
	Node3D *holder = memnew(Node3D);
	holder->set_name("Hedges");
	add_child(holder, false, INTERNAL_MODE_BACK);
	hedge_nodes.push_back(holder);
	// The real mesh near, voxel shells further out (docs/ideas/vegetation.md, "Far hedges as voxel
	// bricks"), each level a MultiMesh per 8 m cell: a visibility range is measured to the
	// instance's AABB centre, so one MultiMesh for a whole field would switch all its hedges
	// together.
	auto add_levels = [&](const Ref<Mesh> &p_full, const LocalVector<Transform3D> &p_xf, const String &p_key, const float *p_voxels, const float *p_ranges, const String &p_name, int p_levels) {
		for (int lod = 0; lod < p_levels; lod++) {
			Ref<Mesh> lm = p_full;
			if (lod > 0) {
				const String key = vformat("%s_%d_%d", p_key, lod, hedge_day);
				if (!hedge_shell_cache.has(key)) {
					hedge_shell_cache[key] = VegetationVoxelLod::make_shell(p_full, p_voxels[lod], 7 + lod * 13 + uint32_t(p_key.hash() & 0xFF));
				}
				lm = hedge_shell_cache[key];
			}
			if (lm.is_null() || lm->get_surface_count() == 0) {
				continue;
			}
			if (!hedge_voxel_lods && lod > 0) {
				continue;
			}
			HashMap<Vector2i, LocalVector<Transform3D>> cells;
			for (const Transform3D &t : p_xf) {
				const Vector2i c(int(Math::floor(t.origin.x / 8.0f)), int(Math::floor(t.origin.z / 8.0f)));
				cells[hedge_voxel_lods ? c : Vector2i()].push_back(t);
			}
			for (const KeyValue<Vector2i, LocalVector<Transform3D>> &cell : cells) {
				Ref<MultiMesh> mm;
				mm.instantiate();
				mm->set_transform_format(MultiMesh::TRANSFORM_3D);
				// Each shrub its own tone (Paulo, 18 Nov: no visible repeats): a little lighter or
				// darker and yellower or bluer, from its position, the same at every level.
				mm->set_use_colors(true);
				mm->set_mesh(lm);
				mm->set_instance_count(cell.value.size());
				for (uint32_t t = 0; t < cell.value.size(); t++) {
					mm->set_instance_transform(t, cell.value[t]);
					const Vector3 o = cell.value[t].origin;
					const uint32_t hh = hash_murmur3_one_32(uint32_t(int(Math::floor(o.x * 10.0f))), hash_murmur3_one_32(uint32_t(int(Math::floor(o.z * 10.0f))), 0x7A3Bu));
					const float lv = 0.92f + 0.16f * float(hh & 0xFF) / 255.0f;
					const float yl = 0.1f * (float((hh >> 8) & 0xFF) / 255.0f - 0.5f);
					mm->set_instance_color(t, Color(lv * (1.0f + yl), lv * (1.0f + 0.3f * yl), lv * (1.0f - yl), 1.0f));
				}
				MultiMeshInstance3D *mmi = memnew(MultiMeshInstance3D);
				mmi->set_name(vformat("%s_%d_%d_lod%d", p_name, cell.key.x, cell.key.y, lod));
				mmi->set_multimesh(mm);
				if (hedge_voxel_lods) {
					mmi->set_visibility_range_begin(p_ranges[lod]);
					mmi->set_visibility_range_end(p_ranges[lod + 1]);
					mmi->set_visibility_range_begin_margin(lod > 0 ? 3.0f : 0.0f);
					mmi->set_visibility_range_end_margin(lod < p_levels - 1 ? 3.0f : 0.0f);
				}
				holder->add_child(mmi);
			}
		}
	};
	// Segments: 5 cm voxels from 30 m, 10 cm from 45 m, 20 cm from 90 m, 40 cm from 180 m, where a
	// voxel is about two pixels across at 1080p and a 75 degree view (plus the cell's reach). A
	// flailed segment is over a million triangles; past 30 m its leaves are under a pixel (from 20 m the bench's near hedges read as blocks).
	const float seg_ranges[6] = { 0.0f, 30.0f, 45.0f, 90.0f, 180.0f, 0.0f };
	const float seg_voxels[5] = { 0.0f, 0.05f, 0.1f, 0.2f, 0.4f };
	for (int k = 0; k < kinds; k++) {
		bool any = false;
		for (int v = 0; v < VARIANTS; v++) {
			any = any || !xf[k][v].is_empty();
		}
		if (!any) {
			continue;
		}
		species[k] = TreeGenerator::make_shrub_species(mix[k].preset, TreeGenerator::SHRUB_HEDGE, VARIANTS, season);
		if (species[k].is_null()) {
			continue;
		}
		const TypedArray<Mesh> meshes = TreeGenerator::get_living_meshes(species[k]);
		for (int v = 0; v < VARIANTS; v++) {
			if (xf[k][v].is_empty() || meshes.is_empty()) {
				continue;
			}
			add_levels(meshes[v % meshes.size()], xf[k][v], vformat("hedge_%d_%d", int(mix[k].preset), v), seg_voxels, seg_ranges, vformat("Hedge%d_%d", k, v), 5);
		}
	}
	// Standards: crowns of 5 to 10 m, coarser voxels and further ranges.
	const float std_ranges[5] = { 0.0f, 70.0f, 140.0f, 280.0f, 0.0f };
	const float std_voxels[4] = { 0.0f, 0.15f, 0.3f, 0.6f };
	for (int k = 0; k < 3; k++) {
		bool any = false;
		for (int v = 0; v < VARIANTS; v++) {
			any = any || !std_xf[k][v].is_empty();
		}
		if (!any) {
			continue;
		}
		Ref<PlantSpecies> sp;
		if (std_presets[k] == TreeGenerator::PRESET_OAK) {
			// A tree preset: an open-grown tree (a full crown, low branches) from its generator.
			Ref<TreeGenerator> tg;
			tg.instantiate();
			tg->set_preset(std_presets[k]);
			tg->set_season(season);
			// The hedgerow's own trees do not follow fires (yet): no burnt band meshes to build.
			tg->set_fire_variants(false);
			sp = tg->make_species(false, 2); // Two: a tree is heavy, and few stand in view.
		} else {
			sp = TreeGenerator::make_shrub_species(std_presets[k], TreeGenerator::SHRUB_MATURE, VARIANTS, season);
		}
		if (sp.is_null()) {
			continue;
		}
		const TypedArray<Mesh> meshes = TreeGenerator::get_living_meshes(sp);
		for (int v = 0; v < VARIANTS; v++) {
			if (std_xf[k][v].is_empty() || meshes.is_empty()) {
				continue;
			}
			add_levels(meshes[v % meshes.size()], std_xf[k][v], vformat("standard_%d_%d", int(std_presets[k]), v), std_voxels, std_ranges, vformat("Standard%d_%d", k, v), 4);
		}
	}
}

float GrassField3D::get_field_stage(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, int(plots.size()), -1.0f);
	return plots[p_index].current;
}

// The crop as seen from above at a stage (rgb, linear) and how much of the soil it covers (a), for
// the ground past the tillers' band: from the sparse green rows of tillering to a closed canopy,
// through the ears' turn to gold, to the pale stubble after harvest. The canopy's own shade
// darkens its leaves' colour seen from afar (the far bake measured grass about 0.8 of its blades).
Color GrassField3D::crop_canopy_look(Crop p_crop, float p_stage) {
	const CropShape cs = _crop_shape(p_crop);
	const float s = p_stage;
	if (s < 10.0f) {
		return Color(0, 0, 0, 0);
	}
	if (s >= 99.0f) {
		// Stubble from afar: the cut stems and the fallen straw shade each other and the soil
		// between them as a ripe stand does (0.55, pushed from its grey), a dull gold, not the
		// pale straw of one stem (the first fields frame read as snow).
		const float g = (cs.straw.r + cs.straw.g + cs.straw.b) / 3.0f;
		const Color st = Color(g, g, g).lerp(cs.straw, 1.5f) * 0.62f;
		return Color(MAX(st.r, 0.0f), MAX(st.g, 0.0f), MAX(st.b, 0.0f), 0.6f);
	}
	auto ramp = [](float p_x, float p_a, float p_b) { return CLAMP((p_x - p_a) / (p_b - p_a), 0.0f, 1.0f); };
	const float cover = s < 30.0f ? Math::lerp(0.08f, 0.45f, ramp(s, 10.0f, 30.0f)) : Math::lerp(0.45f, 0.97f, ramp(s, 30.0f, 39.0f)); // Closed by flag leaf (39).
	Color c = cs.green.lerp(cs.green * 1.12f, ramp(s, 51.0f, 65.0f));
	c = c.lerp(cs.ripe.lerp(cs.straw, 0.4f), Math::smoothstep(0.0f, 1.0f, ramp(s, 71.0f, 87.0f)));
	// Seen from above a stand is darker than its parts: the gaps between ears and leaves are in
	// their shade. Matched to the instanced tillers' mean over the same ground (grass bench,
	// CROP_FAR_ONLY, 450 tillers per m2).
	// Shade between the leaves deepens the colour too: pushed out from its grey.
	const float grey = (c.r + c.g + c.b) / 3.0f;
	const float ripe = ramp(s, 71.0f, 87.0f);
	// A young open stand has little self-shade: its leaves keep nearer their own colour.
	const float closed = ramp(cover, 0.3f, 0.95f);
	// (Ripe: matched with the sun ahead and behind, the far ground turning by the tillers' bake.)
	c = Color(grey, grey, grey).lerp(c, Math::lerp(Math::lerp(1.2f, 1.6f, closed), 1.15f, ripe));
	c = c * Math::lerp(Math::lerp(0.8f, 0.44f, closed), 0.63f, ripe);
	c.r = MAX(c.r, 0.0f);
	c.g = MAX(c.g, 0.0f);
	c.b = MAX(c.b, 0.0f);
	c.a = cover;
	return c;
}

// The canopy's top over the ground at a stage (m), for the far fields' relief: leaves only while
// tillering, the stem extending to flag leaf and heading, the ears on top; stubble after harvest.
float GrassField3D::crop_canopy_height(Crop p_crop, float p_stage) {
	const CropShape cs = _crop_shape(p_crop);
	const float s = p_stage;
	if (s < 10.0f) {
		return 0.0f;
	}
	if (s >= 99.0f) {
		return 0.15f;
	}
	auto ramp = [](float p_x, float p_a, float p_b) { return CLAMP((p_x - p_a) / (p_b - p_a), 0.0f, 1.0f); };
	const float full = cs.height + cs.ear_length * 0.8f;
	if (s < 30.0f) {
		return Math::lerp(0.05f, 0.22f, ramp(s, 10.0f, 30.0f)) * full / 0.9f;
	}
	return Math::lerp(0.22f, 1.0f, ramp(s, 30.0f, 59.0f)) * full;
}

LocalVector<GrassField3D *> GrassField3D::crop_nodes;
LocalVector<GrassField3D *> GrassField3D::hay_nodes;
uint64_t GrassField3D::hay_generation = 1;
bool GrassField3D::crop_ground_dirty = true;
bool GrassField3D::crop_plots_dirty = true;
Ref<ImageTexture> GrassField3D::crop_ground_texture;
Ref<ImageTexture> GrassField3D::crop_plots_texture;

GrassField3D::CropFieldMask GrassField3D::get_crop_field_mask() {
	CropFieldMask m;
	for (const GrassField3D *g : crop_nodes) {
		for (const CropPlot &pl : g->plots) {
			m.items.push_back({ pl.bounds, pl.polygon });
		}
		for (const CropFieldMask::Hedge &h : g->hedge_runs) {
			m.hedges.push_back(h);
		}
	}
	return m;
}

// The fields changed (a crop node came or went, or its fields): the shared ground is baked again
// and the meadow grass and plants rebuild clear of them.
void GrassField3D::_crop_fields_changed_notify() {
	crop_ground_dirty = true;
	if (is_inside_tree()) {
		get_tree()->call_group_flags(SceneTree::GROUP_CALL_DEFERRED, SNAME("vegetation_crop_aware"), SNAME("_on_crop_fields_changed"));
	}
}

void GrassField3D::_on_crop_fields_changed() {
	if (crop == CROP_NONE) {
		crop_mask = get_crop_field_mask();
		rebuild();
	}
}

// The fields' ground (crop_ground()), shared by every crop node: the first one in the tree bakes the
// map of all their fields (a slot each, up to 255) and the table of their looks.
void GrassField3D::_crop_ground_step() {
	if (!crop_nodes.has(this)) {
		crop_nodes.push_back(this);
		_crop_fields_changed_notify();
	}
	if (crop_nodes[0] != this) {
		return;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	if (crop_ground_dirty) {
		crop_ground_dirty = false;
		crop_plots_dirty = true;
		Rect2 bounds;
		bool any = false;
		for (const GrassField3D *g : crop_nodes) {
			for (const CropPlot &pl : g->plots) {
				bounds = any ? bounds.merge(pl.bounds) : pl.bounds;
				any = true;
			}
		}
		if (!any) {
			rs->global_shader_parameter_set("crop_ground_rect", Vector4());
		} else {
			bounds = bounds.grow(1.0f);
			const float texel = MAX(0.5f, MAX(bounds.size.x, bounds.size.y) / 2048.0f);
			const int w = MAX(int(Math::ceil(bounds.size.x / texel)), 1);
			const int h = MAX(int(Math::ceil(bounds.size.y / texel)), 1);
			Vector<uint8_t> data;
			data.resize(w * h * 2);
			memset(data.ptrw(), 0, data.size());
			uint8_t *d = data.ptrw();
			int slot = 1;
			for (const GrassField3D *g : crop_nodes) {
				for (const CropPlot &pl : g->plots) {
					if (slot > 255) {
						break;
					}
					const Vector2 dd(Math::cos(pl.drill_angle), Math::sin(pl.drill_angle));
					const Vector2 n(-dd.y, dd.x);
					const int x0 = MAX(int((pl.bounds.position.x - bounds.position.x) / texel), 0);
					const int y0 = MAX(int((pl.bounds.position.y - bounds.position.y) / texel), 0);
					const int x1 = MIN(int(Math::ceil((pl.bounds.get_end().x - bounds.position.x) / texel)), w - 1);
					const int y1 = MIN(int(Math::ceil((pl.bounds.get_end().y - bounds.position.y) / texel)), h - 1);
					for (int y = y0; y <= y1; y++) {
						for (int x = x0; x <= x1; x++) {
							const Vector2 q = bounds.position + Vector2(x + 0.5f, y + 0.5f) * texel;
							if (!Geometry2D::is_point_in_polygon(q, pl.polygon)) {
								continue;
							}
							uint8_t *o = d + (y * w + x) * 2;
							o[0] = uint8_t(slot);
							if (pl.tramline_spacing > 0.0f && (pl.headland <= 0.0f || polygon_edge_distance(q, pl.polygon) >= pl.headland)) {
								const float wv = Math::fposmod(float(q.dot(n)), pl.tramline_spacing) - 0.5f * pl.tramline_spacing;
								const float off = Math::abs(Math::abs(wv) - 0.5f * pl.tramline_gauge);
								o[1] = uint8_t(CLAMP(1.0f - (off - 0.15f) / (0.5f * texel + 0.1f), 0.0f, 1.0f) * 255.0f);
							}
						}
					}
					slot++;
				}
			}
			crop_ground_texture = ImageTexture::create_from_image(Image::create_from_data(w, h, false, Image::FORMAT_RG8, data));
			rs->global_shader_parameter_set("crop_ground_map", crop_ground_texture->get_rid());
			rs->global_shader_parameter_set("crop_ground_rect", Vector4(bounds.position.x, bounds.position.y, w * texel, h * texel));
		}
	}
	if (crop_plots_dirty) {
		crop_plots_dirty = false;
		Ref<Image> img = Image::create_empty(256, 4, false, Image::FORMAT_RGBAF);
		int slot = 1;
		for (const GrassField3D *g : crop_nodes) {
			for (const CropPlot &pl : g->plots) {
				if (slot > 255) {
					break;
				}
				const Color look = crop_canopy_look(g->crop, pl.current);
				Color lin = look.srgb_to_linear(); // The crop's colours are sRGB (source_color).
				lin.a = look.a;
				img->set_pixel(slot, 0, lin);
				img->set_pixel(slot, 1, Color(pl.drill_angle, pl.row_spacing, g->radius * 0.97f, g->radius * 0.37f));
				float pitch = 0.0f;
				float half = 0.0f;
				const bool swath = _crop_swath(pl, pitch, half);
				img->set_pixel(slot, 2, Color(pitch, half, swath ? 1.0f : 0.0f, crop_canopy_height(g->crop, pl.current)));
				img->set_pixel(slot, 3, crop_slot_view.has(slot) ? crop_slot_view[slot] : Color(1, 1, 1, 1));
				slot++;
			}
		}
		if (crop_plots_texture.is_valid() && crop_plots_texture->get_size() == Vector2(256, 4)) {
			crop_plots_texture->update(img);
		} else {
			crop_plots_texture = ImageTexture::create_from_image(img);
		}
		rs->global_shader_parameter_set("crop_ground_plots", crop_plots_texture->get_rid());
	}
}

// The tillers of a drilled crop over p_rect: without fields, rows everywhere along drill_angle;
// with fields, inside each polygon along its own drill angle, with tramlines (paired unsown wheel
// tracks every tramline_spacing metres) and a headland of rows run round the edge, drilled last
// where the drill turned. Plants spaced along each row from the density, each one to three tillers
// fanning from its crown. p_place(x, z, rank, hash, yaw, splay, plot): rank orders the thinning
// (whole plants drop out; a golden-ratio sequence along each row keeps the spacing even at every
// level), yaw turns local +z away from the crown, splay is the tiller's rest lean, plot the
// stage slot (0 the node's, 1 on the fields).
template <typename F>
void GrassField3D::_crop_rows(const Rect2 &p_rect, float p_keep, F &&p_place, bool p_fields) const {
	const float plants = MAX(density, 1.0f) / 2.0f; // About 2 tillers survive to ear a plant.
	const Vector2 end = p_rect.get_end();
	// One plant on row r at step k (row frame: n across, d along), offset across by p_u.
	auto plant = [&](float p_u, const Vector2 &p_n, const Vector2 &p_d, float p_v, int p_r, int p_k, uint32_t p_salt, float p_phase, int p_plot, const CropPlot *p_pl, bool p_headland) {
		const uint32_t hp = cell_hash(p_r, p_k, 0x27D4EB2Fu ^ p_salt);
		if (unit(hp, 0) < 0.07f) {
			return; // Seeds that did not establish.
		}
		const float wobble = 0.02f * Math::sin(p_v * 0.23f + p_phase) + 0.006f * Math::sin(p_v * 1.7f + p_phase * 3.0f);
		const float u = p_u + wobble + (unit(hp, 16) - 0.5f) * 0.025f;
		const Vector2 crown = p_n * u + p_d * p_v;
		if (p_pl) {
			if (!p_pl->bounds.grow(0.05f).has_point(crown) || !Geometry2D::is_point_in_polygon(crown, p_pl->polygon)) {
				return;
			}
			if (p_pl->headland > 0.0f && !p_headland && polygon_edge_distance(crown, p_pl->polygon) < p_pl->headland) {
				return;
			}
		}
		const float rank = Math::fmod(p_k * 0.618034f + unit(cell_hash(p_r, 0, 0x165667B1u ^ p_salt), 8) + 1000.0f, 1.0f);
		const int tillers = 1 + MIN(int(unit(hp, 24) * 3.0f), 2);
		const float a0 = unit(cell_hash(p_r, p_k, 0x61C88647u ^ p_salt), 0) * Math::TAU;
		for (int t = 0; t < tillers; t++) {
			const uint32_t ht = cell_hash(p_r * 4 + t, p_k, 0x9E3779B9u ^ p_salt);
			const float a = a0 + t * Math::TAU / tillers + (unit(ht, 24) - 0.5f) * 0.8f;
			const Vector2 dir(Math::cos(a), Math::sin(a));
			const Vector2 q = crown + dir * (tillers > 1 ? 0.006f + 0.006f * unit(ht, 0) : 0.0f);
			if (q.x < p_rect.position.x || q.y < p_rect.position.y || q.x >= end.x || q.y >= end.y) {
				continue;
			}
			const float splay = tillers > 1 ? 0.05f + 0.14f * unit(ht, 8) : 0.02f + 0.08f * unit(ht, 8);
			p_place(q.x, q.y, rank, ht, Math::atan2(dir.x, dir.y), splay, p_plot);
		}
	};
	// The main body: rows along the drill angle.
	auto drill = [&](const Rect2 &p_area, float p_angle, float p_rs, int p_plot, const CropPlot *p_pl) {
		const Vector2 d(Math::cos(p_angle), Math::sin(p_angle));
		const Vector2 n(-d.y, d.x);
		const float s = 1.0f / (plants * p_rs);
		float umin = 1e30f, umax = -1e30f, vmin = 1e30f, vmax = -1e30f;
		for (int i = 0; i < 4; i++) {
			const Vector2 c = p_area.position + Vector2((i & 1) ? p_area.size.x : 0.0f, (i & 2) ? p_area.size.y : 0.0f);
			umin = MIN(umin, c.dot(n));
			umax = MAX(umax, c.dot(n));
			vmin = MIN(vmin, c.dot(d));
			vmax = MAX(vmax, c.dot(d));
		}
		const uint32_t salt = uint32_t(p_plot) * 0x9E3779B1u;
		const int r0 = int(Math::floor(umin / p_rs)) - 1;
		const int r1 = int(Math::ceil(umax / p_rs)) + 1;
		const int k0 = int(Math::floor(vmin / s)) - 2;
		const int k1 = int(Math::ceil(vmax / s)) + 2;
		for (int r = r0; r <= r1; r++) {
			const float u = r * p_rs;
			if (p_pl && p_pl->tramline_spacing > 0.0f) {
				// Tramlines: the drill shuts the coulters over the sprayer's wheel tracks.
				const float w = Math::fposmod(u, p_pl->tramline_spacing) - 0.5f * p_pl->tramline_spacing;
				if (Math::abs(Math::abs(w) - 0.5f * p_pl->tramline_gauge) < 0.2f) {
					continue;
				}
			}
			// A drill's coulters (24 rows, 3 m) wander together; the next bout its own way, which is
			// what makes the bouts readable across a field.
			const int bout = int(Math::floor(r / 24.0f));
			const float phase = unit(cell_hash(bout, 0, 0xD3A2646Cu ^ salt), 0) * Math::TAU;
			for (int k = k0; k <= k1; k++) {
				const float rank = Math::fmod(k * 0.618034f + unit(cell_hash(r, 0, 0x165667B1u ^ salt), 8) + 1000.0f, 1.0f);
				if (rank >= p_keep) {
					continue;
				}
				const uint32_t hp = cell_hash(r, k, 0x27D4EB2Fu ^ salt);
				plant(u, n, d, (k + (unit(hp, 8) - 0.5f) * 0.9f) * s, r, k, salt, phase, p_plot, p_pl, false);
			}
		}
	};
	if (plots.is_empty() || !p_fields) {
		drill(p_rect, drill_angle, MAX(row_spacing, 0.03f), 0, nullptr);
		return;
	}
	const Rect2 reach = p_rect.grow(0.05f);
	for (uint32_t pi = 0; pi < plots.size(); pi++) {
		const CropPlot &pl = plots[pi];
		if (!pl.bounds.intersects(reach) || (pl.sown_day < 0 && pl.current < 10.0f)) {
			continue;
		}
		const int slot = int(pi) + 1;
		drill(p_rect, pl.drill_angle, pl.row_spacing, slot, &pl);
		if (pl.headland <= 0.0f) {
			continue;
		}
		// The headland: rows parallel to each edge, out to the headland's width; each point grows on
		// the rows of its nearest edge, so the corners mitre.
		const int ne = pl.polygon.size();
		const float s = 1.0f / (plants * pl.row_spacing);
		const int rows = int(Math::ceil(pl.headland / pl.row_spacing));
		for (int e = 0; e < ne; e++) {
			const Vector2 a = pl.polygon[e];
			const Vector2 b = pl.polygon[(e + 1) % ne];
			const float len = a.distance_to(b);
			if (len < 1e-3f) {
				continue;
			}
			const Vector2 d = (b - a) / len;
			const Vector2 n = pl.clockwise ? Vector2(d.y, -d.x) : Vector2(-d.y, d.x); // Inward.
			// The stretch of this edge's rows over the tile.
			float vmin = 1e30f, vmax = -1e30f, umin = 1e30f, umax = -1e30f;
			for (int i = 0; i < 4; i++) {
				const Vector2 c = p_rect.position + Vector2((i & 1) ? p_rect.size.x : 0.0f, (i & 2) ? p_rect.size.y : 0.0f) - a;
				vmin = MIN(vmin, c.dot(d));
				vmax = MAX(vmax, c.dot(d));
				umin = MIN(umin, c.dot(n));
				umax = MAX(umax, c.dot(n));
			}
			if (umax < 0.0f || umin > pl.headland || vmax < -pl.headland || vmin > len + pl.headland) {
				continue;
			}
			const uint32_t salt = uint32_t(slot) * 0x9E3779B1u ^ uint32_t(e + 1) * 0x85EBCA77u;
			const float phase = unit(cell_hash(e, slot, 0xD3A2646Cu), 0) * Math::TAU;
			const int k0 = int(Math::floor(MAX(vmin, -pl.headland) / s)) - 2;
			const int k1 = int(Math::ceil(MIN(vmax, len + pl.headland) / s)) + 2;
			for (int r = MAX(int(Math::floor(umin / pl.row_spacing)) - 1, 0); r < MIN(rows, int(Math::ceil(umax / pl.row_spacing)) + 2); r++) {
				const float u = (r + 0.5f) * pl.row_spacing;
				for (int k = k0; k <= k1; k++) {
					const float rank = Math::fmod(k * 0.618034f + unit(cell_hash(r, 0, 0x165667B1u ^ salt), 8) + 1000.0f, 1.0f);
					if (rank >= p_keep) {
						continue;
					}
					const uint32_t hp = cell_hash(r, k, 0x27D4EB2Fu ^ salt);
					const float v = (k + (unit(hp, 8) - 0.5f) * 0.9f) * s;
					// Its own edge's rows only: the nearest edge to the row line at this point.
					const Vector2 at = a + d * v + n * u;
					int nearest = -1;
					if (polygon_edge_distance(at, pl.polygon, &nearest) >= pl.headland || nearest != e) {
						continue;
					}
					// The row frame is the edge's, shifted to start at a.
					plant(u + a.dot(n), n, d, v + a.dot(d), r, k, salt, phase, slot, &pl, true);
				}
			}
		}
	}
}

// A tiller's parts as one mesh: VERTEX.z is the part (0 stem, 1 leaf, 2 ear or panicle axis, 3
// awn, 4 panicle branch, 5 oat spikelet, 6 spikelet of a spike), VERTEX.x the side (-0.5..0.5) or the angle round (0..1),
// VERTEX.y the parameter along it; UV2 the part's index and count. The vertex shader builds the
// real tiller from them (stage, wind, ripeness). p_far: fewer segments, half the leaves and awns.
Ref<ArrayMesh> GrassField3D::_make_crop_mesh(bool p_far) const {
	const CropShape cs = _crop_shape(crop);
	PackedVector3Array verts;
	PackedVector2Array uvs;
	PackedVector2Array uv2s;
	PackedInt32Array idx;
	auto strip = [&](int p_part, int p_index, int p_count, int p_segs, bool p_tip) {
		const int b = verts.size();
		const int rows = p_tip ? p_segs : p_segs + 1;
		for (int i = 0; i < rows; i++) {
			const float t = float(i) / p_segs;
			verts.push_back(Vector3(-0.5f, t, p_part));
			verts.push_back(Vector3(0.5f, t, p_part));
			uvs.push_back(Vector2(0.0f, t));
			uvs.push_back(Vector2(1.0f, t));
			uv2s.push_back(Vector2(p_index, p_count));
			uv2s.push_back(Vector2(p_index, p_count));
		}
		for (int i = 0; i < rows - 1; i++) {
			const int a = b + i * 2;
			idx.append_array(PackedInt32Array({ a, a + 2, a + 1, a + 1, a + 2, a + 3 }));
		}
		if (p_tip) {
			verts.push_back(Vector3(0.0f, 1.0f, p_part));
			uvs.push_back(Vector2(0.5f, 1.0f));
			uv2s.push_back(Vector2(p_index, p_count));
			const int a = b + (rows - 1) * 2;
			idx.append_array(PackedInt32Array({ a, a + 2, a + 1 }));
		}
	};
	auto ring = [&](int p_part, int p_sides, int p_segs) {
		const int b = verts.size();
		for (int i = 0; i < p_segs; i++) {
			const float t = float(i) / p_segs;
			for (int j = 0; j <= p_sides; j++) {
				verts.push_back(Vector3(float(j) / p_sides, t, p_part));
				uvs.push_back(Vector2(float(j) / p_sides, t));
				uv2s.push_back(Vector2(0, 1));
			}
		}
		const int tip = verts.size();
		verts.push_back(Vector3(0.0f, 1.0f, p_part));
		uvs.push_back(Vector2(0.5f, 1.0f));
		uv2s.push_back(Vector2(0, 1));
		const int w = p_sides + 1;
		for (int i = 0; i < p_segs - 1; i++) {
			for (int j = 0; j < p_sides; j++) {
				const int a = b + i * w + j;
				idx.append_array(PackedInt32Array({ a, a + w, a + 1, a + 1, a + w, a + w + 1 }));
			}
		}
		for (int j = 0; j < p_sides; j++) {
			const int a = b + (p_segs - 1) * w + j;
			idx.append_array(PackedInt32Array({ a, tip, a + 1 }));
		}
	};
	strip(0, 0, 1, p_far ? 2 : 5, false);
	constexpr int LEAVES = 4;
	for (int i = p_far ? 1 : 0; i < LEAVES; i += p_far ? 2 : 1) {
		strip(1, i, LEAVES, p_far ? 2 : 4, true);
	}
	if (cs.branches > 0) {
		ring(2, 3, p_far ? 2 : 3);
		for (int k = 0; k < cs.branches; k += p_far ? 2 : 1) {
			strip(4, k, cs.branches, p_far ? 1 : 3, true); // Arched: three segments near.
			strip(5, k, cs.branches, 2, true);
		}
	} else if (p_far) {
		ring(2, 3, 2);
	} else {
		// The rachis, and on it the spikelets in two alternate rows, each a lozenge (base, two
		// flanks, tip) turned to the view: the ear's grain rows as real geometry.
		ring(2, 3, 3);
		const int n = cs.awns > 0 ? cs.awns : 18;
		for (int k = 0; k < n; k++) {
			const int b = verts.size();
			const Vector3 vv[4] = { Vector3(0.0f, 0.0f, 6), Vector3(-0.5f, 0.5f, 6), Vector3(0.5f, 0.5f, 6), Vector3(0.0f, 1.0f, 6) };
			const Vector2 uu[4] = { Vector2(0.5f, 0.0f), Vector2(0.0f, 0.5f), Vector2(1.0f, 0.5f), Vector2(0.5f, 1.0f) };
			for (int i = 0; i < 4; i++) {
				verts.push_back(vv[i]);
				uvs.push_back(uu[i]);
				uv2s.push_back(Vector2(k, n));
			}
			idx.append_array(PackedInt32Array({ b, b + 1, b + 3, b, b + 3, b + 2 }));
		}
	}
	for (int k = 0; k < cs.awns; k += p_far ? 2 : 1) {
		strip(3, k, cs.awns, 1, true);
	}
	PackedVector3Array normals;
	normals.resize(verts.size());
	normals.fill(Vector3(0, 0, 1));
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = verts;
	arrays[Mesh::ARRAY_NORMAL] = normals;
	arrays[Mesh::ARRAY_TEX_UV] = uvs;
	arrays[Mesh::ARRAY_TEX_UV2] = uv2s;
	arrays[Mesh::ARRAY_INDEX] = idx;
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	const float h = cs.height * 1.25f + cs.ear_length + cs.awn_length + 0.1f;
	mesh->set_custom_aabb(AABB(Vector3(-h, -0.05f, -h), Vector3(2.0f * h, h + 0.1f, 2.0f * h)));
	return mesh;
}

// The crop's shape and each plot's growth stage as shader uniforms; the shader's crop_look() turns a
// stage into the look.
void GrassField3D::_crop_uniforms(const Ref<ShaderMaterial> &p_material) const {
	const CropShape cs = _crop_shape(crop);
	Ref<Image> img = Image::create_empty(64, 1, false, Image::FORMAT_RF);
	for (int i = 0; i < 64; i++) {
		img->set_pixel(i, 0, Color(i >= 1 && uint32_t(i - 1) < plots.size() && i <= MAX_PLOTS ? plots[i - 1].current : crop_stage, 0, 0));
	}
	if (crop_stage_texture.is_valid()) {
		crop_stage_texture->update(img);
	} else {
		crop_stage_texture = ImageTexture::create_from_image(img);
	}
	p_material->set_shader_parameter("crop_plot_stage", crop_stage_texture);
	p_material->set_shader_parameter("crop_nod", cs.nod);
	p_material->set_shader_parameter("crop_ear", Vector4(cs.ear_length, cs.ear_width, cs.awn_length, float(cs.branches)));
	p_material->set_shader_parameter("crop_leaf", Vector4(cs.leaf_length, cs.leaf_width, 0.0022f, 4.0f));
	p_material->set_shader_parameter("crop_green", cs.green);
	p_material->set_shader_parameter("crop_ripe", cs.ripe);
	p_material->set_shader_parameter("crop_straw", cs.straw);
	// Leaf area per ground area, for the light inside the canopy: tillers x leaves x length x width
	// (at the node's stage).
	const float leaf_scale = crop_stage < 30.0f ? CLAMP(0.25f + (crop_stage - 10.0f) * 0.02f, 0.25f, 0.7f) : CLAMP(0.7f + (crop_stage - 30.0f) / 30.0f, 0.7f, 1.0f);
	p_material->set_shader_parameter("crop_lai", density * 4.0f * cs.leaf_length * leaf_scale * 2.0f * cs.leaf_width * 0.75f);
}

void GrassField3D::_build_tile(const Vector2i &p_tile, int p_lod, TerrainDataMap3D *p_map) {
	const float keep = 1.0f / float(1 << p_lod);
	const float cell = 1.0f / Math::sqrt(MAX(density, 0.01f));
	const float x0 = p_tile.x * tile_size;
	const float z0 = p_tile.y * tile_size;
	const int ix0 = int(Math::floor(x0 / cell));
	const int iz0 = int(Math::floor(z0 / cell));
	const int ix1 = int(Math::floor((x0 + tile_size) / cell));
	const int iz1 = int(Math::floor((z0 + tile_size) / cell));
	const float ground_y = get_global_position().y;
	const Transform3D to_local = get_global_transform().affine_inverse();
	Ref<PlantSpecies> r = rules;
	if (r.is_null()) {
		r.instantiate(); // Defaults: dry land only.
	}
	const VegetationField3D *canopy = _get_canopy(r.ptr());
	// The light the blades grew in (VegetationLight), on a 1 m grid over the tile: the crowns'
	// shade from the canopy field (even when the rules ignore shade) and the slope's sun.
	const VegetationField3D *crowns = canopy_field.is_empty() || !is_inside_tree() ? nullptr : Object::cast_to<VegetationField3D>(get_node_or_null(canopy_field));
	const int lg = int(Math::ceil(tile_size)) + 1;
	LocalVector<float> light_grid;
	light_grid.resize(lg * lg);
	for (int gz = 0; gz < lg; gz++) {
		for (int gx = 0; gx < lg; gx++) {
			const Vector3 q(x0 + gx, 0.0f, z0 + gz);
			const float sh = crowns ? crowns->get_shade_at(q) : 0.0f;
			light_grid[gz * lg + gx] = (1.0f - sh) * (p_map ? VegetationLight::exposure(p_map, sun_mean, q.x, q.z) : 1.0f);
		}
	}
	// The undergrowth's shade on the sward (bracken, bramble), on the same grid: the blades thin
	// out under their stands.
	const VegetationField3D *under = _get_undergrowth();
	LocalVector<float> sward_grid;
	if (under) {
		sward_grid.resize(lg * lg);
		for (int gz = 0; gz < lg; gz++) {
			for (int gx = 0; gx < lg; gx++) {
				sward_grid[gz * lg + gx] = under->get_sward_shade_at(Vector3(x0 + gx, 0.0f, z0 + gz));
			}
		}
	}
	// Worn paths: the walkers a year each blade gets (GPU fill: on a fine grid), which thin the
	// blades and shorten and lay over the ones left.
	const bool trodden = !path_grid.is_empty() && trample_resistance > 0.0f && path_grid.any_in(Rect2(x0, z0, tile_size, tile_size).grow(path_grid.max_reach));
	LocalVector<float> wear_grid;
	if (trodden) {
		const int wn = int(Math::ceil(tile_size / FILL_WEAR_STEP)) + 1;
		wear_grid.resize(wn * wn);
		for (int j = 0; j < wn; j++) {
			for (int i = 0; i < wn; i++) {
				wear_grid[j * wn + i] = path_grid.passes_at(Vector2(x0 + i * FILL_WEAR_STEP, z0 + j * FILL_WEAR_STEP));
			}
		}
	}
	// Wider blades where fewer remain, so the field keeps its coverage at a distance.
	const float width = blade_width / Math::sqrt(keep);
	// Crops are CPU-built for now (farmland stage 1).
	if (gpu_fill && crop == CROP_NONE && is_gpu_fill_available()) {
		_build_tile_gpu(p_tile, p_lod, p_map, r.ptr(), canopy, light_grid, sward_grid, wear_grid, lg, width);
		return;
	}
	LocalVector<Transform3D> xforms;
	LocalVector<Color> customs;
	const bool rocks = _tile_has_obstacles(x0, z0);
	const bool holed = !holes.is_empty() && holes.any_in(Rect2(x0, z0, tile_size, tile_size).grow(OBSTACLE_MARGIN));
	// Whether a blade (or crop tiller) grows at (x, z): the rules, paths and light decide; rank orders
	// the thinning (coarser levels keep the same ones).
	struct Spot {
		float y = 0.0f;
		float light = 1.0f;
		float shade_t = 0.0f;
		float sun_t = 0.0f;
		float passes = 0.0f;
		float edge = 1.0f; // Height at the rules' edges (soft edges: the thinning blades also shorten).
	};
	const bool fielded = crop == CROP_NONE && crop_mask.any_in(Rect2(x0, z0, tile_size, tile_size));
	auto accept = [&](float x, float z, float rank, Spot &r_spot) -> bool {
		if (rocks && obstacle_grid.blocks(Vector2(x, z), OBSTACLE_MARGIN)) {
			return false;
		}
		if (fielded && crop_mask.contains(Vector2(x, z))) {
			return false; // A crop field: the meadow stops at its edge.
		}
		if (holed && _blade_in_hole(p_map, x, z, ground_y)) {
			return false;
		}
		const float passes = trodden ? path_grid.passes_at(Vector2(x, z)) : 0.0f;
		if (passes > 0.0f && rank >= keep * _trample_cover(passes)) {
			return false;
		}
		const Vector3 p(x, ground_y, z);
		float y = ground_y;
		if (p_map) {
			float suit = r->get_suitability(p_map, p);
			if (canopy && suit > 0.0f) {
				suit *= r->rule_weight(PlantSpecies::RULE_SHADE, canopy->get_stand_shade_at(p));
			}
			if (suit > 0.0f) {
				suit *= _burn_cover(r.ptr(), x, z);
			}
			if (!sward_grid.is_empty() && suit > 0.0f) {
				suit *= 1.0f - grid_bilinear(sward_grid, lg, x - x0, z - z0);
			}
			if (passes > 0.0f && suit > 0.0f) {
				suit *= _trample_cover(passes);
			}
			if (rank >= keep * suit) {
				return false;
			}
			r_spot.edge = _edge_height(suit / MAX(passes > 0.0f ? _trample_cover(passes) : 1.0f, 1e-4f));
			y = p_map->get_height_at(p) + _relief_y(p, false);
		}
		const float lx = CLAMP(x - x0, 0.0f, float(lg - 1));
		const float lz = CLAMP(z - z0, 0.0f, float(lg - 1));
		const int gx = MIN(int(lx), lg - 2);
		const int gz = MIN(int(lz), lg - 2);
		r_spot.light = Math::lerp(Math::lerp(light_grid[gz * lg + gx], light_grid[gz * lg + gx + 1], lx - gx), Math::lerp(light_grid[(gz + 1) * lg + gx], light_grid[(gz + 1) * lg + gx + 1], lx - gx), lz - gz);
		r_spot.y = y;
		r_spot.passes = passes;
		// Shade avoidance (Franklin 2008): grass in shade grows longer and laxer; in full sun on a
		// sunny slope, shorter.
		r_spot.shade_t = 1.0f - CLAMP(r_spot.light, 0.0f, 1.0f);
		r_spot.sun_t = Math::smoothstep(1.0f, 1.3f, r_spot.light);
		return true;
	};
	if (crop == CROP_NONE) {
		for (int iz = iz0; iz < iz1; iz++) {
			for (int ix = ix0; ix < ix1; ix++) {
				// The rank decides which blades a coarser level keeps: the same ones, so nothing
				// pops. A blue-noise mask instead of a hash: every threshold keeps an even spread,
				// so thinned grass has no clumps and gaps.
				const float rank = blue_rank(ix, iz);
				if (rank >= keep) {
					continue;
				}
				const uint32_t hb = cell_hash(ix, iz, 0x9E3779B9u);
				const uint32_t hj = cell_hash(ix, iz, 0x85EBCA6Bu);
				const float x = (ix + unit(hj, 0)) * cell;
				const float z = (iz + unit(hj, 8)) * cell;
				Spot sp;
				if (!accept(x, z, rank, sp)) {
					continue;
				}
				// Clumps (Ghost of Tsushima): blades of a clump share height, facing and dryness.
				const uint32_t hc = clump_hash(x, z, clump_size);
				const float clump_h = unit(hc, 0);
				const float clump_yaw = unit(hc, 8) * Math::TAU;
				const float rnd = unit(hb, 16);
				const float height = Math::lerp(float(blade_height.x), float(blade_height.y), 0.65f * clump_h + 0.35f * unit(hj, 16)) * (1.0f + 0.35f * sp.shade_t - 0.15f * sp.sun_t) * _trample_scale(sp.passes) * sp.edge;
				const float yaw = clump_yaw + (unit(hj, 24) - 0.5f) * 1.6f;
				const Transform3D world(Basis(Vector3(0, 1, 0), yaw), Vector3(x, sp.y, z));
				xforms.push_back(to_local * world);
				// w: the outer band's drop order in the integer part (the rank within the coarsest
				// level, so the last blades left are evenly spread too), the blade's random value in
				// the fraction.
				const float order = Math::floor(Math::fmod(rank * 8.0f, 1.0f) * 255.0f);
				// z: the light in its integer part over 4 (VegetationLight::pack), the rest lean
				// (under 4). Trodden blades lie over (wear 0.45 at the most-walked: flat).
				const float flat = CLAMP((1.0f - _trample_scale(sp.passes)) / 0.55f, 0.0f, 1.0f);
				const float blade_lean = MIN(lean * (0.4f + 1.2f * unit(hc, 16)) * (1.0f + 0.8f * sp.shade_t) + 2.6f * Math::sqrt(flat) * (0.7f + 0.6f * Math::fmod(rnd * 7.31f, 1.0f)), 3.99f);
				customs.push_back(Color(height, width, VegetationLight::pack(sp.light) * 4.0f + blade_lean, order + 0.999f * Math::fmod(rnd + 0.37f * unit(hc, 24), 1.0f)));
			}
		}
	} else if (crop_stage >= 10.0f || !plots.is_empty()) {
		// A tiller: x its ripe stem height, y its bulk (thicker where fewer remain), z the light and
		// rest lean as the blades, w the drop order and its random value. Local +z points away from
		// its plant's crown, the way it leans.
		const float bulk = 1.0f / Math::sqrt(keep);
		const float crop_h = _crop_shape(crop).height;
		_crop_rows(Rect2(x0, z0, tile_size, tile_size), keep, [&](float x, float z, float rank, uint32_t hb, float yaw, float splay, int plot) {
			Spot sp;
			if (!accept(x, z, rank, sp)) {
				return;
			}
			const float rnd = unit(hb, 16);
			// Shaded crop grows taller and thinner (shade avoidance), a sunny slope a little shorter.
			const float height = crop_h * (0.9f + 0.2f * unit(hb, 0)) * (1.0f + 0.15f * sp.shade_t - 0.05f * sp.sun_t) * _trample_scale(sp.passes);
			xforms.push_back(to_local * Transform3D(Basis(Vector3(0, 1, 0), yaw), Vector3(x, sp.y, z)));
			const float order = Math::floor(Math::fmod(rank * 8.0f, 1.0f) * 255.0f);
			const float flat = CLAMP((1.0f - _trample_scale(sp.passes)) / 0.55f, 0.0f, 1.0f);
			const float tiller_lean = MIN(splay * (1.0f + 0.5f * sp.shade_t) + 1.3f * Math::sqrt(flat) * (0.7f + 0.6f * Math::fmod(rnd * 7.31f, 1.0f)), 3.99f);
			customs.push_back(Color(height, plot * 4.0f + MIN(bulk * (0.85f + 0.3f * unit(hb, 8)), 3.99f), VegetationLight::pack(sp.light) * 4.0f + tiller_lean, order + 0.999f * rnd));
		});
		// Harvested plots: the straw lying in each pass's swath, mostly along the pass, heaped in the
		// middle (about 600 pieces a square metre there, thinning to its edges).
		const Rect2 trect(x0, z0, tile_size, tile_size);
		for (uint32_t pi = 0; pi < plots.size(); pi++) {
			const CropPlot &pl = plots[pi];
			float pitch = 0.0f;
			float half = 0.0f;
			if (!_crop_swath(pl, pitch, half) || !pl.bounds.intersects(trect)) {
				continue;
			}
			const Vector2 d(Math::cos(pl.drill_angle), Math::sin(pl.drill_angle));
			const Vector2 nn(-d.y, d.x);
			float umin = 1e30f, umax = -1e30f, vmin = 1e30f, vmax = -1e30f;
			for (int i = 0; i < 4; i++) {
				const Vector2 c = trect.position + Vector2((i & 1) ? tile_size : 0.0f, (i & 2) ? tile_size : 0.0f);
				umin = MIN(umin, c.dot(nn));
				umax = MAX(umax, c.dot(nn));
				vmin = MIN(vmin, c.dot(d));
				vmax = MAX(vmax, c.dot(d));
			}
			const float cell = 1.0f / Math::sqrt(600.0f);
			const uint32_t salt = uint32_t(pi + 1) * 0x7FEB352Du;
			for (int j = int(Math::floor((umin - half) / pitch)); j <= int(Math::ceil((umax + half) / pitch)); j++) {
				const float uc = (j + 0.5f) * pitch;
				// The swath wanders a little as the combine steers.
				const int i0 = int(Math::floor(-half / cell));
				const int i1 = int(Math::ceil(half / cell));
				for (int k = int(Math::floor(vmin / cell)) - 1; k <= int(Math::ceil(vmax / cell)) + 1; k++) {
					const float v_k = k * cell;
					const float sway = 0.25f * Math::sin(v_k * 0.05f + j * 1.7f) + 0.1f * Math::sin(v_k * 0.21f + j * 0.3f);
					for (int i = i0; i <= i1; i++) {
						const uint32_t h = cell_hash(j * 4096 + i, k, salt);
						const float rank = unit(h, 0);
						if (rank >= keep) {
							continue;
						}
						const float du = (i + unit(h, 8)) * cell;
						const float across = du / half;
						if (unit(h, 16) > 1.0f - across * across) {
							continue; // Thinner to the swath's edges.
						}
						const Vector2 q = nn * (uc + du + sway) + d * (v_k + unit(h, 24) * cell);
						if (!trect.has_point(q) || !Geometry2D::is_point_in_polygon(q, pl.polygon)) {
							continue;
						}
						Spot sp;
						if (!accept(q.x, q.y, rank, sp)) {
							continue;
						}
						const uint32_t h2 = cell_hash(k, j * 4096 + i, salt ^ 0x5bd1e995u);
						const float rnd = unit(h2, 0);
						// Along the pass either way, some crossways; heaped to the middle.
						const float lie = pl.drill_angle + (unit(h2, 8) < 0.5f ? 0.0f : Math::PI) + (unit(h2, 16) - 0.5f) * 1.2f;
						const Vector2 ld(Math::cos(lie), Math::sin(lie));
						const float heap = 0.12f * (1.0f - across * across) * unit(h2, 24);
						xforms.push_back(to_local * Transform3D(Basis(Vector3(0, 1, 0), Math::atan2(ld.x, ld.y)), Vector3(q.x, sp.y + heap, q.y)));
						const float order = Math::floor(Math::fmod(rank * 8.0f, 1.0f) * 255.0f);
						customs.push_back(Color(crop_h * (0.35f + 0.45f * unit(h, 4)), (pi + 1 + 64) * 4.0f + MIN(bulk, 3.99f), VegetationLight::pack(sp.light) * 4.0f, order + 0.999f * rnd));
					}
				}
			}
		}
	}
	Tile tile;
	tile.lod = p_lod;
	tile.generation = generation;
	if (!xforms.is_empty()) {
		Ref<MultiMesh> mm;
		mm.instantiate();
		mm->set_transform_format(MultiMesh::TRANSFORM_3D);
		mm->set_use_custom_data(true);
		mm->set_mesh(crop != CROP_NONE && p_lod >= 2 && crop_mesh_far.is_valid() ? crop_mesh_far : blade_mesh);
		mm->set_instance_count(xforms.size());
		for (uint32_t i = 0; i < xforms.size(); i++) {
			mm->set_instance_transform(i, xforms[i]);
			mm->set_instance_custom_data(i, customs[i]);
		}
		tile.mmi = memnew(MultiMeshInstance3D);
		tile.mmi->set_multimesh(mm);
		tile.mmi->set_name(vformat("Grass_%d_%d", p_tile.x, p_tile.y));
		add_child(tile.mmi, false, INTERNAL_MODE_BACK);
		blade_count += xforms.size();
	}
	tiles.insert(p_tile, tile);
}

// The tile's blades on the GPU (stage 3a). The CPU lists the candidate cells (those the rank keeps at
// this level, in a fixed order), bakes the rules into a 0.5 m suitability grid and hands over the
// light grid and the patch of map heights under the tile; the compute shader makes each blade from
// the same hashes as the CPU loop and writes the records in place. Rejected candidates are written
// as zero (degenerate) records.
void GrassField3D::_build_tile_gpu(const Vector2i &p_tile, int p_lod, TerrainDataMap3D *p_map, const PlantSpecies *p_rules, const VegetationField3D *p_canopy, const LocalVector<float> &p_light, const LocalVector<float> &p_sward, const LocalVector<float> &p_wear, int p_lg, float p_width) {
	const float keep = 1.0f / float(1 << p_lod);
	const float cell = 1.0f / Math::sqrt(MAX(density, 0.01f));
	const float x0 = p_tile.x * tile_size;
	const float z0 = p_tile.y * tile_size;
	const int ix0 = int(Math::floor(x0 / cell));
	const int iz0 = int(Math::floor(z0 / cell));
	const int ix1 = int(Math::floor((x0 + tile_size) / cell));
	const int iz1 = int(Math::floor((z0 + tile_size) / cell));
	const float ground_y = get_global_position().y;
	const Transform3D to_local = get_global_transform().affine_inverse();

	LocalVector<float> cand;
	const bool rocks = _tile_has_obstacles(x0, z0);
	const bool holed = !holes.is_empty() && holes.any_in(Rect2(x0, z0, tile_size, tile_size).grow(OBSTACLE_MARGIN));
	for (int iz = iz0; iz < iz1; iz++) {
		for (int ix = ix0; ix < ix1; ix++) {
			const float rank = blue_rank(ix, iz);
			if (rank >= keep) {
				continue;
			}
			if (rocks) {
				// The blade's root as the shader places it (the same hash), kept out of the rocks.
				const uint32_t hj = cell_hash(ix, iz, 0x85EBCA6Bu);
				if (obstacle_grid.blocks(Vector2((ix + unit(hj, 0)) * cell, (iz + unit(hj, 8)) * cell), OBSTACLE_MARGIN)) {
					continue;
				}
			}
			if (holed) {
				const uint32_t hj = cell_hash(ix, iz, 0x85EBCA6Bu);
				if (_blade_in_hole(p_map, (ix + unit(hj, 0)) * cell, (iz + unit(hj, 8)) * cell, ground_y)) {
					continue;
				}
			}
			union {
				int32_t i;
				float f;
			} bx, bz;
			bx.i = ix;
			bz.i = iz;
			cand.push_back(bx.f);
			cand.push_back(bz.f);
			cand.push_back(rank);
		}
	}
	const int count = int(cand.size() / 3);
	Tile tile;
	tile.lod = p_lod;
	tile.generation = generation;
	if (count == 0) {
		tiles.insert(p_tile, tile);
		return;
	}

	// Rules baked on a 0.5 m grid (the CPU path evaluates them per blade).
	const int sg_n = int(Math::ceil(tile_size / FILL_SUIT_STEP)) + 1;
	LocalVector<float> suit;
	// Heights: the map texels under the tile, so the shader samples exactly as get_height_at() does.
	int hp_x0 = 0, hp_z0 = 0, hp_w = 1, hp_h = 1;
	LocalVector<float> heights;
	Rect2 map_rect;
	int map_n = 1;
	float y_min = ground_y, y_max = ground_y;
	if (p_map) {
		suit.resize(sg_n * sg_n);
		for (int j = 0; j < sg_n; j++) {
			for (int i = 0; i < sg_n; i++) {
				const Vector3 q(x0 + i * FILL_SUIT_STEP, ground_y, z0 + j * FILL_SUIT_STEP);
				float v = crop_mask.contains(Vector2(q.x, q.z)) ? 0.0f : p_rules->get_suitability(p_map, q);
				if (p_canopy && v > 0.0f) {
					v *= p_rules->rule_weight(PlantSpecies::RULE_SHADE, p_canopy->get_stand_shade_at(q));
				}
				if (v > 0.0f) {
					v *= _burn_cover(p_rules, q.x, q.z);
				}
				if (!p_sward.is_empty() && v > 0.0f) {
					v *= 1.0f - grid_bilinear(p_sward, p_lg, q.x - x0, q.z - z0);
				}
				suit[j * sg_n + i] = v;
			}
		}
		map_rect = p_map->get_map_rect();
		if (height_image.is_null() || height_rect != map_rect) {
			height_image = p_map->get_channel_image(TerrainDataMap3D::CHANNEL_HEIGHT);
			height_rect = map_rect;
		}
		map_n = height_image.is_valid() ? height_image->get_width() : 0;
		if (map_n > 0) {
			const float mc = map_rect.size.x / map_n;
			hp_x0 = CLAMP(int(Math::floor((x0 - map_rect.position.x) / mc - 0.5f)), 0, map_n - 1);
			hp_z0 = CLAMP(int(Math::floor((z0 - map_rect.position.y) / mc - 0.5f)), 0, map_n - 1);
			const int hp_x1 = CLAMP(int(Math::floor((x0 + tile_size - map_rect.position.x) / mc - 0.5f)) + 1, 0, map_n - 1);
			const int hp_z1 = CLAMP(int(Math::floor((z0 + tile_size - map_rect.position.y) / mc - 0.5f)) + 1, 0, map_n - 1);
			hp_w = hp_x1 - hp_x0 + 1;
			hp_h = hp_z1 - hp_z0 + 1;
			const float *hd = reinterpret_cast<const float *>(height_image->get_data().ptr());
			heights.resize(hp_w * hp_h);
			y_min = 1e30f;
			y_max = -1e30f;
			for (int j = 0; j < hp_h; j++) {
				for (int i = 0; i < hp_w; i++) {
					// The drawn ground's bank and cliff relief over the heightfield (WorldGround.relief_at), at the texel's
					// centre, so the blades stand on the surface drawn, not the heightfield under it.
					const float h = hd[(hp_z0 + j) * map_n + hp_x0 + i] + _relief_y(Vector3(map_rect.position.x + (hp_x0 + i + 0.5f) * mc, 0.0f, map_rect.position.y + (hp_z0 + j + 0.5f) * mc), false);
					heights[j * hp_w + i] = h;
					y_min = MIN(y_min, h);
					y_max = MAX(y_max, h);
				}
			}
		}
	}
	if (suit.is_empty()) {
		suit.push_back(1.0f);
	}
	if (heights.is_empty()) {
		heights.push_back(ground_y);
	}

	PackedFloat32Array in;
	in.resize(FILL_HEADER);
	float *h = in.ptrw();
	for (int k = 0; k < FILL_HEADER; k++) {
		h[k] = 0.0f;
	}
	h[0] = x0;
	h[1] = z0;
	h[2] = cell;
	h[3] = keep;
	h[4] = p_width;
	h[5] = blade_height.x;
	h[6] = blade_height.y;
	h[7] = lean;
	h[8] = clump_size;
	h[9] = ground_y;
	h[10] = (p_map && map_n > 0) ? 1.0f : 0.0f;
	h[11] = FILL_SUIT_STEP;
	h[12] = float(sg_n);
	h[13] = float(p_lg);
	h[14] = float(hp_x0);
	h[15] = float(hp_z0);
	h[16] = float(hp_w);
	h[17] = float(hp_h);
	h[18] = map_rect.position.x;
	h[19] = map_rect.position.y;
	h[20] = map_n > 0 ? map_rect.size.x / map_n : 1.0f;
	h[21] = float(map_n);
	for (int r = 0; r < 3; r++) {
		for (int c = 0; c < 3; c++) {
			h[22 + r * 3 + c] = to_local.basis.rows[r][c];
		}
		h[31 + r] = to_local.origin[r];
	}
	h[34] = float(count);
	const int off_cand = FILL_HEADER;
	const int off_suit = off_cand + int(cand.size());
	const int off_light = off_suit + int(suit.size());
	const int off_height = off_light + int(p_light.size());
	h[35] = float(off_cand);
	h[36] = float(off_suit);
	h[37] = float(off_light);
	h[38] = float(off_height);
	const int off_wear = off_height + int(heights.size());
	h[39] = p_wear.is_empty() ? 0.0f : float(off_wear); // Worn paths: walkers a year on a fine grid.
	h[40] = float(int(Math::ceil(tile_size / FILL_WEAR_STEP)) + 1);
	h[41] = FILL_WEAR_STEP;
	h[42] = trample_resistance;
	h[43] = 0.0f; // A near tile.
	h[44] = 1.0f; // The light grid: 1 m.
	in.resize(off_wear + int(p_wear.size()));
	float *w = in.ptrw();
	memcpy(w + off_cand, cand.ptr(), cand.size() * sizeof(float));
	memcpy(w + off_suit, suit.ptr(), suit.size() * sizeof(float));
	memcpy(w + off_light, p_light.ptr(), p_light.size() * sizeof(float));
	memcpy(w + off_height, heights.ptr(), heights.size() * sizeof(float));
	if (!p_wear.is_empty()) {
		memcpy(w + off_wear, p_wear.ptr(), p_wear.size() * sizeof(float));
	}

	Ref<MultiMesh> mm;
	mm.instantiate();
	mm->set_transform_format(MultiMesh::TRANSFORM_3D);
	mm->set_use_custom_data(true);
	mm->set_mesh(blade_mesh);
	mm->set_instance_count(count);
	// The records never reach the CPU before the first frame: give culling the tile's box.
	const float top = blade_height.y * 1.5f + 0.2f;
	const float reach = 1.0f + _hay_reach(); // Hay is gathered across its rows in the shader.
	const AABB world_box(Vector3(x0 - reach, y_min - 0.2f, z0 - reach), Vector3(tile_size + 2.0f * reach, y_max - y_min + top + 0.2f, tile_size + 2.0f * reach));
	mm->set_custom_aabb(to_local.xform(world_box));
	RenderingServer::get_singleton()->call_on_render_thread(callable_mp_static(&grass_fill_rt).bind(mm->get_rid(), in, count));
	tile.mmi = memnew(MultiMeshInstance3D);
	tile.mmi->set_multimesh(mm);
	tile.mmi->set_name(vformat("Grass_%d_%d", p_tile.x, p_tile.y));
	add_child(tile.mmi, false, INTERNAL_MODE_BACK);
	blade_count += count;
	tiles.insert(p_tile, tile);
}

void GrassField3D::_free_tile(Tile &p_tile) {
	if (p_tile.mmi) {
		Ref<MultiMesh> mm = p_tile.mmi->get_multimesh();
		if (mm.is_valid()) {
			blade_count -= mm->get_instance_count();
		}
		remove_child(p_tile.mmi);
		memdelete(p_tile.mmi);
		p_tile.mmi = nullptr;
	}
}

// Far rings (docs/ideas/grass.md, "Far grass"): the meadow's blades past the near tiles, out to far_ring_range.
// One set of blades on nested grids: a cell at level L (cells 2^L times the base cell) carries one blade, picked
// down a chain of hashed children to a base cell, and the same blade carries its parent's cell for as many levels
// up as the chain continues. That count gives the blade's rank (4^-(levels + 1) to 4^-levels), so the shader can
// keep blades whose rank is under (start / distance)^2 and the set thins evenly and smoothly with no level showing.
// A tile at level L holds the blades of rank under 4^-L, enough for any point at 2^L times the start or farther.
namespace {
constexpr float RING_TILE = 16.0f; // m, a level-0 tile; level L is 2^L times wider and holds as many blades.
constexpr int RING_MAX_LEVEL = 8;
constexpr float RING_WIDEN_MAX = 16.0f;
constexpr float RING_BASE_SHARE = 4.0f; // The rings' density at their start: the near tiles' coarsest level.
constexpr float RING_BUDGET_MSEC = 4.0f; // Ring tiles built a frame, past the first.

uint32_t ring_child(int p_x, int p_z, int p_level) {
	return cell_hash(p_x, p_z, 0xA511E9B3u + uint32_t(p_level) * 0x9E3779B9u) & 3u;
}

int floor_half(int p_v) {
	return p_v >= 0 ? p_v / 2 : -((-p_v + 1) / 2);
}
} // namespace

bool GrassField3D::_rings_active() const {
	return far_rings && crop == CROP_NONE && is_inside_tree() && fade_owner == this && radius > 0.0f && far_ring_range > radius;
}

Ref<ArrayMesh> GrassField3D::_make_ring_mesh(int p_segments) const {
	PackedVector3Array verts;
	PackedVector2Array uvs;
	PackedInt32Array idx;
	for (int i = 0; i < p_segments; i++) {
		const float t = float(i) / p_segments;
		verts.push_back(Vector3(-0.5f, t, 0.0f));
		verts.push_back(Vector3(0.5f, t, 0.0f));
		uvs.push_back(Vector2(0.0f, t));
		uvs.push_back(Vector2(1.0f, t));
	}
	verts.push_back(Vector3(0.0f, 1.0f, 0.0f));
	uvs.push_back(Vector2(0.5f, 1.0f));
	for (int i = 0; i < p_segments - 1; i++) {
		const int a = i * 2;
		idx.append_array(PackedInt32Array({ a, a + 2, a + 1, a + 1, a + 2, a + 3 }));
	}
	const int last = (p_segments - 1) * 2;
	idx.append_array(PackedInt32Array({ last, last + 2, last + 1 }));
	PackedVector3Array normals;
	normals.resize(verts.size());
	normals.fill(Vector3(0, 0, 1));
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = verts;
	arrays[Mesh::ARRAY_NORMAL] = normals;
	arrays[Mesh::ARRAY_TEX_UV] = uvs;
	arrays[Mesh::ARRAY_INDEX] = idx;
	Ref<ArrayMesh> m;
	m.instantiate();
	m->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	// Bent and widened in the shader: bounds for the tallest blade at the widest.
	const float h = MAX(blade_height.x, blade_height.y) * 1.1f;
	const float wd = blade_width * Math::sqrt(RING_BASE_SHARE) * RING_WIDEN_MAX * 1.5f + h;
	m->set_custom_aabb(AABB(Vector3(-wd, -0.05f, -wd), Vector3(2.0f * wd, h + 0.1f, 2.0f * wd)));
	m->surface_set_material(0, material);
	return m;
}

void GrassField3D::_free_ring_tile(RingTile &p_tile) {
	if (p_tile.mmi) {
		Ref<MultiMesh> mm = p_tile.mmi->get_multimesh();
		if (mm.is_valid()) {
			ring_blade_count -= mm->get_instance_count();
		}
		remove_child(p_tile.mmi);
		memdelete(p_tile.mmi);
		p_tile.mmi = nullptr;
	}
}

void GrassField3D::_free_rings() {
	for (KeyValue<Vector3i, RingTile> &kv : ring_tiles) {
		_free_ring_tile(kv.value);
	}
	ring_tiles.clear();
	ring_blade_count = 0;
}

void GrassField3D::_update_rings(const Vector2 &p_center, TerrainDataMap3D *p_map) {
	// The far blades' ground normal (the shader's ground_data_map).
	const Ref<Texture2D> ground_tex = p_map ? p_map->get_data_texture() : Ref<Texture2D>();
	const Rect2 ground_rect = ground_tex.is_valid() ? p_map->get_map_rect() : Rect2();
	const Vector4 ground_v(ground_rect.position.x, ground_rect.position.y, ground_rect.size.x, ground_rect.size.y);
	if (material.is_valid() && (ground_tex != ground_data_tex || !ground_v.is_equal_approx(ground_data_rect))) {
		ground_data_tex = ground_tex;
		ground_data_rect = ground_v;
		material->set_shader_parameter("ground_data_map", ground_tex);
		material->set_shader_parameter("ground_data_rect", ground_v);
	}
	if (!_rings_active()) {
		if (!ring_tiles.is_empty()) {
			_free_rings();
			_free_shell();
		}
		return;
	}
	// The rings start where the near blades start to thin (their outer band) and keep every blade there.
	const float start = MAX(fade_band.z - fade_band.w, 1.0f);
	const Vector4 band(start, far_ring_range, far_ring_range * 0.2f, RING_WIDEN_MAX);
	if (!band.is_equal_approx(ring_band)) {
		ring_band = band;
		material->set_shader_parameter("ring_band", ring_band);
	}
	if (ring_mesh_near.is_null() || ring_mesh_near->surface_get_material(0) != material) {
		ring_mesh_near = _make_ring_mesh(2);
		ring_mesh_far = _make_ring_mesh(1);
	}
	// The wanted tiles: a quadtree from tiles covering the whole reach, split while a tile is nearer than its level
	// keeps enough blades for.
	int top = 0;
	while (top < RING_MAX_LEVEL && start * float(1 << top) < far_ring_range) {
		top++;
	}
	struct Want {
		Vector3i key;
		float d;
		bool operator<(const Want &o) const { return d < o.d; }
	};
	LocalVector<Want> want;
	HashSet<Vector3i> wanted;
	const float top_size = RING_TILE * float(1 << top);
	const Vector2i lo(int(Math::floor((p_center.x - far_ring_range) / top_size)), int(Math::floor((p_center.y - far_ring_range) / top_size)));
	const Vector2i hi(int(Math::floor((p_center.x + far_ring_range) / top_size)), int(Math::floor((p_center.y + far_ring_range) / top_size)));
	LocalVector<Vector3i> stack;
	for (int z = lo.y; z <= hi.y; z++) {
		for (int x = lo.x; x <= hi.x; x++) {
			stack.push_back(Vector3i(x, z, top));
		}
	}
	while (!stack.is_empty()) {
		const Vector3i k = stack[stack.size() - 1];
		stack.remove_at(stack.size() - 1);
		const float size = RING_TILE * float(1 << k.z);
		const Rect2 rect(k.x * size, k.y * size, size, size);
		const Vector2 nearest(CLAMP(p_center.x, rect.position.x, rect.position.x + size), CLAMP(p_center.y, rect.position.y, rect.position.y + size));
		const float d_near = nearest.distance_to(p_center);
		const float d_far = Vector2(MAX(Math::abs(p_center.x - rect.position.x), Math::abs(p_center.x - rect.position.x - size)), MAX(Math::abs(p_center.y - rect.position.y), Math::abs(p_center.y - rect.position.y - size))).length();
		if (d_near > far_ring_range || d_far < start) {
			continue; // Past the rings, or all inside the near blades.
		}
		if (k.z > 0 && d_near < start * float(1 << k.z)) {
			for (int c = 0; c < 4; c++) {
				stack.push_back(Vector3i(k.x * 2 + (c & 1), k.y * 2 + (c >> 1), k.z - 1));
			}
			continue;
		}
		wanted.insert(k);
		const RingTile *existing = ring_tiles.getptr(k);
		if (!existing || existing->generation != generation || d_near < existing->built_near) {
			want.push_back({ k, d_near });
		}
	}
	want.sort();
	last_built += int(want.size());
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	// The first fill all at once (a scene's first frame), then within a budget a frame.
	const bool first = ring_tiles.is_empty();
	for (const Want &w : want) {
		if (!first && (OS::get_singleton()->get_ticks_usec() - t0) > uint64_t(RING_BUDGET_MSEC * 1000.0f)) {
			break;
		}
		RingTile *existing = ring_tiles.getptr(w.key);
		if (existing) {
			_free_ring_tile(*existing);
			ring_tiles.erase(w.key);
		}
		RingTile tile;
		const float ring_size = RING_TILE * float(1 << w.key.z);
		_rocks_for(Rect2(w.key.x * ring_size, w.key.y * ring_size, ring_size, ring_size));
		// Built for a quarter of the tile nearer than now, so walking does not rebuild it every step.
		_build_ring_tile(w.key, MAX(w.d - 0.25f * RING_TILE * float(1 << w.key.z), 0.0f), p_map, tile);
		ring_tiles.insert(w.key, tile);
	}
	const uint64_t t1 = OS::get_singleton()->get_ticks_usec();
	if (!want.is_empty()) {
		ring_build_msec = double(t1 - t0) / 1000.0;
	}
	// Tiles no longer wanted go once every wanted one is built, so a split or a merge never leaves a hole.
	bool complete = true;
	for (const Vector3i &k : wanted) {
		const RingTile *t = ring_tiles.getptr(k);
		if (!t || t->generation != generation) {
			complete = false;
			break;
		}
	}
	if (complete) {
		LocalVector<Vector3i> drop;
		for (const KeyValue<Vector3i, RingTile> &kv : ring_tiles) {
			if (!wanted.has(kv.key)) {
				drop.push_back(kv.key);
			}
		}
		for (const Vector3i &k : drop) {
			_free_ring_tile(ring_tiles[k]);
			ring_tiles.erase(k);
		}
	}
}

void GrassField3D::_build_ring_tile(const Vector3i &p_key, float p_near, TerrainDataMap3D *p_map, RingTile &r_tile) {
	r_tile.generation = generation;
	r_tile.built_near = p_near;
	// Only the blades the shader keeps somewhere in the tile: rank under (start / nearest distance)^2.
	const float ring_start = MAX(ring_band.x, 1.0f);
	const float min_neg_log_rank = p_near > ring_start ? 2.0f * Math::log2(p_near / ring_start) : 0.0f;
	const int level = p_key.z;
	// The base density: the near tiles' coarsest level (a quarter, RING_BASE_SHARE), its cell fitted to the tile.
	const float base_density = MAX(density, 0.01f) / RING_BASE_SHARE;
	const int n = MAX(int(Math::ceil(RING_TILE * Math::sqrt(base_density))), 1);
	const float cell0 = RING_TILE / float(n);
	const float width0 = blade_width * Math::sqrt(MAX(density, 0.01f) / base_density);
	const float ground_y = get_global_position().y;
	const Transform3D to_local = get_global_transform().affine_inverse();
	Ref<PlantSpecies> r = rules;
	if (r.is_null()) {
		r.instantiate();
	}
	const VegetationField3D *canopy = _get_canopy(r.ptr());
	const VegetationField3D *crowns = canopy_field.is_empty() ? nullptr : Object::cast_to<VegetationField3D>(get_node_or_null(canopy_field));
	const VegetationField3D *under = _get_undergrowth();
	const float size = RING_TILE * float(1 << level);
	const Rect2 rect(p_key.x * size, p_key.y * size, size, size);
	const bool rocks = !obstacle_grid.is_empty();
	const bool holed = !holes.is_empty() && holes.any_in(rect.grow(OBSTACLE_MARGIN));
	const bool fielded = crop_mask.any_in(rect);
	const bool trodden = !path_grid.is_empty() && trample_resistance > 0.0f && path_grid.any_in(rect.grow(path_grid.max_reach));
	// Only over the data map: past it there are no rules or heights.
	const Rect2 map_rect = p_map ? p_map->get_map_rect() : Rect2();
	if (p_map && !map_rect.intersects(rect)) {
		return;
	}
	// GPU-built far blades (docs/ideas/vegetation.md, "Next round", 1): the rules, shade and light on a grid of 33 x 33
	// over the tile (0.5 m on a level-0 tile, as the near fill) instead of per blade, and the blades placed by
	// grass_fill.glsl straight into the MultiMesh's buffer, so no copy of them stays on the CPU. A worn path's fine
	// wear stays on the CPU.
	const bool use_gpu = gpu_fill && !trodden && is_gpu_fill_available();
	float sg_step = 1.0f;
	int sg_n = 1;
	LocalVector<float> suit_grid;
	LocalVector<float> light_grid;
	LocalVector<float> cand;
	if (use_gpu) {
		sg_step = MAX(size / 32.0f, FILL_SUIT_STEP);
		sg_n = int(Math::ceil(size / sg_step)) + 1;
		suit_grid.resize(sg_n * sg_n);
		light_grid.resize(sg_n * sg_n);
		for (int j = 0; j < sg_n; j++) {
			for (int i = 0; i < sg_n; i++) {
				const float x = rect.position.x + i * sg_step;
				const float z = rect.position.y + j * sg_step;
				const Vector3 p(x, ground_y, z);
				float v = 1.0f;
				if (p_map) {
					v = r->get_suitability(p_map, p);
					if (canopy && v > 0.0f) {
						v *= r->rule_weight(PlantSpecies::RULE_SHADE, canopy->get_stand_shade_at(p));
					}
					if (v > 0.0f) {
						v *= _burn_cover(r.ptr(), x, z);
					}
					if (under && v > 0.0f) {
						v *= 1.0f - under->get_sward_shade_at(p);
					}
				}
				suit_grid[j * sg_n + i] = v;
				light_grid[j * sg_n + i] = (1.0f - (crowns ? crowns->get_shade_at(p) : 0.0f)) * (p_map ? VegetationLight::exposure(p_map, sun_mean, x, z) : 1.0f);
			}
		}
		cand.reserve(n * n * 3 / 2);
	}
	LocalVector<Transform3D> xforms;
	LocalVector<Color> customs;
	if (!use_gpu) {
		xforms.reserve(n * n);
		customs.reserve(n * n);
	}
	for (int j = 0; j < n; j++) {
		for (int i = 0; i < n; i++) {
			const int ci = p_key.x * n + i;
			const int cj = p_key.y * n + j;
			// Down the chain to the base cell that carries this cell's blade.
			int bx = ci;
			int bz = cj;
			for (int l = level; l > 0; l--) {
				const uint32_t ch = ring_child(bx, bz, l);
				bx = bx * 2 + int(ch & 1u);
				bz = bz * 2 + int(ch >> 1);
			}
			// Up: how many more levels it carries.
			int keepers = level;
			int ux = ci;
			int uz = cj;
			while (keepers < 24) {
				const int px = floor_half(ux);
				const int pz = floor_half(uz);
				if (ring_child(px, pz, keepers + 1) != (uint32_t(ux & 1) | (uint32_t(uz & 1) << 1))) {
					break;
				}
				ux = px;
				uz = pz;
				keepers++;
			}
			const uint32_t hj = cell_hash(bx, bz, 0x6C8E9CF5u);
			const uint32_t hb = cell_hash(bx, bz, 0x1B873593u);
			// -log2 of the rank: 2 (keepers + 1) - log2(1 + 3 u).
			const float u = float(hb & 0xFFFFu) / 65536.0f;
			const float neg_log_rank = 2.0f * float(keepers + 1) - Math::log2(1.0f + 3.0f * u);
			if (neg_log_rank < min_neg_log_rank) {
				continue; // Dropped everywhere in the tile.
			}
			const float x = (bx + unit(hj, 0)) * cell0;
			const float z = (bz + unit(hj, 8)) * cell0;
			const Vector2 q(x, z);
			if (p_map && !map_rect.has_point(q)) {
				continue;
			}
			if (rocks && obstacle_grid.blocks(q, OBSTACLE_MARGIN)) {
				continue;
			}
			if (fielded && crop_mask.contains(q)) {
				continue;
			}
			if (holed && _blade_in_hole(p_map, x, z, ground_y)) {
				continue;
			}
			if (use_gpu) {
				if (p_map) {
					// As grass_fill.glsl's suitability() and its pick, so the buffer holds only blades that grow (the
					// path tracer keeps a copy of every record): a cell with a corner where nothing grows stays empty.
					const float gu = CLAMP((x - rect.position.x) / sg_step, 0.0f, float(sg_n - 1));
					const float gv = CLAMP((z - rect.position.y) / sg_step, 0.0f, float(sg_n - 1));
					const int gi = MIN(int(gu), sg_n - 2);
					const int gj = MIN(int(gv), sg_n - 2);
					const float *g = suit_grid.ptr() + gj * sg_n + gi;
					if (MIN(MIN(g[0], g[1]), MIN(g[sg_n], g[sg_n + 1])) <= 0.0f) {
						continue;
					}
					const float fx = gu - gi;
					const float suit = Math::lerp(Math::lerp(g[0], g[1], fx), Math::lerp(g[sg_n], g[sg_n + 1], fx), gv - gj);
					if (unit(hb, 16) + unit(hb, 24) / 256.0f >= suit * 1.0001f) {
						continue;
					}
				}
				union {
					int32_t i;
					float f;
				} ux_bits, uz_bits;
				ux_bits.i = bx;
				uz_bits.i = bz;
				cand.push_back(ux_bits.f);
				cand.push_back(uz_bits.f);
				cand.push_back(neg_log_rank);
				continue;
			}
			const float passes = trodden ? path_grid.passes_at(q) : 0.0f;
			const Vector3 p(x, ground_y, z);
			float y = ground_y;
			const float pick = unit(hb, 16) + unit(hb, 24) / 256.0f;
			float suit = 1.0f;
			float edge = 1.0f;
			if (p_map) {
				suit = r->get_suitability(p_map, p);
				if (canopy && suit > 0.0f) {
					suit *= r->rule_weight(PlantSpecies::RULE_SHADE, canopy->get_stand_shade_at(p));
				}
				if (suit > 0.0f) {
					suit *= _burn_cover(r.ptr(), x, z);
				}
				if (under && suit > 0.0f) {
					suit *= 1.0f - under->get_sward_shade_at(p);
				}
				if (passes > 0.0f && suit > 0.0f) {
					suit *= _trample_cover(passes);
				}
				if (pick >= suit) {
					continue;
				}
				edge = _edge_height(suit / MAX(passes > 0.0f ? _trample_cover(passes) : 1.0f, 1e-4f));
				y = p_map->get_height_at(p) + _relief_y(p, false);
			}
			const float light = (1.0f - (crowns ? crowns->get_shade_at(p) : 0.0f)) * (p_map ? VegetationLight::exposure(p_map, sun_mean, x, z) : 1.0f);
			const float shade_t = 1.0f - CLAMP(light, 0.0f, 1.0f);
			const float sun_t = Math::smoothstep(1.0f, 1.3f, light);
			const uint32_t hc = clump_hash(x, z, clump_size);
			const float height = Math::lerp(float(blade_height.x), float(blade_height.y), 0.65f * unit(hc, 0) + 0.35f * unit(hj, 16)) * (1.0f + 0.35f * shade_t - 0.15f * sun_t) * _trample_scale(passes) * edge;
			const float yaw = unit(hc, 8) * Math::TAU + (unit(hj, 24) - 0.5f) * 1.6f;
			xforms.push_back(to_local * Transform3D(Basis(Vector3(0, 1, 0), yaw), Vector3(x, y, z)));
			const float rnd = unit(hash_fmix32(hb ^ 0x5BD1E995u), 0);
			const float blade_lean = MIN(lean * (0.4f + 1.2f * unit(hc, 16)) * (1.0f + 0.8f * shade_t), 3.99f);
			customs.push_back(Color(height, -width0, VegetationLight::pack(light) * 4.0f + blade_lean, Math::floor(MIN(neg_log_rank, 63.0f) * 1024.0f) + 0.999f * Math::fmod(rnd + 0.37f * unit(hc, 24), 1.0f)));
		}
	}
	if (use_gpu) {
		_fill_ring_tile_gpu(p_key, rect, p_map, cell0, width0, sg_step, sg_n, suit_grid, light_grid, cand, r_tile);
		return;
	}
	if (xforms.is_empty()) {
		return;
	}
	Ref<MultiMesh> mm;
	mm.instantiate();
	mm->set_transform_format(MultiMesh::TRANSFORM_3D);
	mm->set_use_custom_data(true);
	mm->set_mesh(level == 0 ? ring_mesh_near : ring_mesh_far);
	mm->set_instance_count(xforms.size());
	for (uint32_t i = 0; i < xforms.size(); i++) {
		mm->set_instance_transform(i, xforms[i]);
		mm->set_instance_custom_data(i, customs[i]);
	}
	r_tile.mmi = memnew(MultiMeshInstance3D);
	r_tile.mmi->set_multimesh(mm);
	// Shadows from the nearer rings only: past a hundred metres or so a blade's shadow is under a pixel (the near
	// tiles' self shade in their light term carries the sward's darkening).
	r_tile.mmi->set_cast_shadows_setting(level <= 1 ? GeometryInstance3D::SHADOW_CASTING_SETTING_ON : GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
	r_tile.mmi->set_name(vformat("GrassRing_%d_%d_%d", p_key.x, p_key.y, level));
	add_child(r_tile.mmi, false, INTERNAL_MODE_BACK);
	ring_blade_count += xforms.size();
}

void GrassField3D::_fill_ring_tile_gpu(const Vector3i &p_key, const Rect2 &p_rect, TerrainDataMap3D *p_map, float p_cell, float p_width, float p_step, int p_n, const LocalVector<float> &p_suit, const LocalVector<float> &p_light, const LocalVector<float> &p_cand, RingTile &r_tile) {
	const int count = int(p_cand.size() / 3);
	if (count == 0) {
		return;
	}
	const float ground_y = get_global_position().y;
	const Transform3D to_local = get_global_transform().affine_inverse();
	// Heights: the map texels under the tile, so the shader samples exactly as get_height_at() does.
	int hp_x0 = 0, hp_z0 = 0, hp_w = 1, hp_h = 1;
	LocalVector<float> heights;
	Rect2 map_rect;
	int map_n = 0;
	float y_min = ground_y, y_max = ground_y;
	if (p_map) {
		map_rect = p_map->get_map_rect();
		if (height_image.is_null() || height_rect != map_rect) {
			height_image = p_map->get_channel_image(TerrainDataMap3D::CHANNEL_HEIGHT);
			height_rect = map_rect;
		}
		map_n = height_image.is_valid() ? height_image->get_width() : 0;
		if (map_n > 0) {
			const float mc = map_rect.size.x / map_n;
			hp_x0 = CLAMP(int(Math::floor((p_rect.position.x - map_rect.position.x) / mc - 0.5f)), 0, map_n - 1);
			hp_z0 = CLAMP(int(Math::floor((p_rect.position.y - map_rect.position.y) / mc - 0.5f)), 0, map_n - 1);
			const int hp_x1 = CLAMP(int(Math::floor((p_rect.position.x + p_rect.size.x - map_rect.position.x) / mc - 0.5f)) + 1, 0, map_n - 1);
			const int hp_z1 = CLAMP(int(Math::floor((p_rect.position.y + p_rect.size.y - map_rect.position.y) / mc - 0.5f)) + 1, 0, map_n - 1);
			hp_w = hp_x1 - hp_x0 + 1;
			hp_h = hp_z1 - hp_z0 + 1;
			const float *hd = reinterpret_cast<const float *>(height_image->get_data().ptr());
			heights.resize(hp_w * hp_h);
			y_min = 1e30f;
			y_max = -1e30f;
			for (int j = 0; j < hp_h; j++) {
				for (int i = 0; i < hp_w; i++) {
					const float hv = hd[(hp_z0 + j) * map_n + hp_x0 + i] + _relief_y(Vector3(map_rect.position.x + (hp_x0 + i + 0.5f) * mc, 0.0f, map_rect.position.y + (hp_z0 + j + 0.5f) * mc), false);
					heights[j * hp_w + i] = hv;
					y_min = MIN(y_min, hv);
					y_max = MAX(y_max, hv);
				}
			}
		}
	}
	if (heights.is_empty()) {
		heights.push_back(ground_y);
	}
	PackedFloat32Array in;
	in.resize(FILL_HEADER);
	float *h = in.ptrw();
	for (int k = 0; k < FILL_HEADER; k++) {
		h[k] = 0.0f;
	}
	h[0] = p_rect.position.x;
	h[1] = p_rect.position.y;
	h[2] = p_cell;
	h[4] = p_width;
	h[5] = blade_height.x;
	h[6] = blade_height.y;
	h[7] = lean;
	h[8] = clump_size;
	h[9] = ground_y;
	h[10] = (p_map && map_n > 0) ? 1.0f : 0.0f;
	h[11] = p_step;
	h[12] = float(p_n);
	h[13] = float(p_n);
	h[14] = float(hp_x0);
	h[15] = float(hp_z0);
	h[16] = float(hp_w);
	h[17] = float(hp_h);
	h[18] = map_rect.position.x;
	h[19] = map_rect.position.y;
	h[20] = map_n > 0 ? map_rect.size.x / map_n : 1.0f;
	h[21] = float(MAX(map_n, 1));
	for (int r = 0; r < 3; r++) {
		for (int c = 0; c < 3; c++) {
			h[22 + r * 3 + c] = to_local.basis.rows[r][c];
		}
		h[31 + r] = to_local.origin[r];
	}
	h[34] = float(count);
	const int off_cand = FILL_HEADER;
	const int off_suit = off_cand + int(p_cand.size());
	const int off_light = off_suit + int(p_suit.size());
	const int off_height = off_light + int(p_light.size());
	h[35] = float(off_cand);
	h[36] = float(off_suit);
	h[37] = float(off_light);
	h[38] = float(off_height);
	h[39] = 0.0f; // No wear: worn tiles are built on the CPU.
	h[43] = 1.0f; // A ring tile.
	h[44] = p_step;
	in.resize(off_height + int(heights.size()));
	float *w = in.ptrw();
	memcpy(w + off_cand, p_cand.ptr(), p_cand.size() * sizeof(float));
	memcpy(w + off_suit, p_suit.ptr(), p_suit.size() * sizeof(float));
	memcpy(w + off_light, p_light.ptr(), p_light.size() * sizeof(float));
	memcpy(w + off_height, heights.ptr(), heights.size() * sizeof(float));

	const int level = p_key.z;
	Ref<MultiMesh> mm;
	mm.instantiate();
	mm->set_transform_format(MultiMesh::TRANSFORM_3D);
	mm->set_use_custom_data(true);
	mm->set_mesh(level == 0 ? ring_mesh_near : ring_mesh_far);
	mm->set_instance_count(count);
	// The records never reach the CPU before the first frame: give culling the tile's box, grown by the widest blade.
	const float top = MAX(blade_height.x, blade_height.y) * 1.5f + 0.2f;
	const float grow = blade_width * Math::sqrt(RING_BASE_SHARE) * RING_WIDEN_MAX * 1.5f + top + _hay_reach();
	const AABB world_box(Vector3(p_rect.position.x - grow, y_min - 0.2f, p_rect.position.y - grow), Vector3(p_rect.size.x + 2.0f * grow, y_max - y_min + top + 0.2f, p_rect.size.y + 2.0f * grow));
	mm->set_custom_aabb(to_local.xform(world_box));
	RenderingServer::get_singleton()->call_on_render_thread(callable_mp_static(&grass_fill_rt).bind(mm->get_rid(), in, count));
	r_tile.mmi = memnew(MultiMeshInstance3D);
	r_tile.mmi->set_multimesh(mm);
	r_tile.mmi->set_cast_shadows_setting(level <= 1 ? GeometryInstance3D::SHADOW_CASTING_SETTING_ON : GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
	r_tile.mmi->set_name(vformat("GrassRing_%d_%d_%d", p_key.x, p_key.y, level));
	add_child(r_tile.mmi, false, INTERNAL_MODE_BACK);
	ring_blade_count += count;
}

void GrassField3D::set_far_rings(bool p_enabled) {
	far_rings = p_enabled;
	if (!far_rings) {
		_free_rings();
	}
}

void GrassField3D::set_far_ring_range(float p_range) {
	far_ring_range = MAX(p_range, 0.0f);
	generation++;
}

// Far shell (docs/ideas/grass.md, "Far grass"): past the rings the sward as columns of voxels standing on the
// ground, as the hedges' far shells and VegetationField3D.far_volume (VegetationVoxelLod; Unreal 5.7's Nanite
// Foliage voxels; Loubet and Neyret 2017): real geometry, so raster, shadows and the path tracer see one shape.
// Each column's top follows the ground, its height the sward's (the rules' cover and clump-sized patches), its
// corners jittered by a hash shared with its neighbours so the shell is closed and lumpy, not boxy. It takes the
// far ground's own colour (grass_ground()), so it matches the ground the rings stood on; its sides darken into the
// sward. It rises out of the ground over the rings' end band and sinks back by far_shell_range (the shader, from
// the field's centre).
namespace {
constexpr float SHELL_TILE = 64.0f;
constexpr int BURN_FOLLOW_MSEC = 500; // GrassField3D._follow_burns(): at most twice a second.
} // namespace

float GrassField3D::_shell_column(int p_x, int p_z, float p_cell, const PlantSpecies *p_rules, TerrainDataMap3D *p_map) const {
	const float x = (p_x + 0.5f) * p_cell;
	const float z = (p_z + 0.5f) * p_cell;
	const Vector2 q(x, z);
	float suit = 1.0f;
	if (p_map) {
		if (!p_map->get_map_rect().has_point(q)) {
			return 0.0f;
		}
		suit = p_rules->get_suitability(p_map, Vector3(x, get_global_position().y, z)) * _burn_cover(p_rules, x, z);
	}
	if ((!crop_mask.is_empty() || !crop_mask.hedges.is_empty()) && crop_mask.contains(q)) {
		suit = 0.0f;
	}
	if (suit < 0.15f) {
		return 0.0f;
	}
	// Patches a few metres across (the blades' clumps seen together) and each column a little different.
	const uint32_t hp = clump_hash(x, z, 4.0f);
	const uint32_t hc = cell_hash(p_x, p_z, 0x3C6EF372u ^ uint32_t(p_cell * 16.0f));
	const float mean = 0.5f * (blade_height.x + blade_height.y) * 0.75f; // Leaning blades stand lower than their length.
	return mean * (0.55f + 0.6f * unit(hp, 0)) * (0.85f + 0.3f * unit(hc, 0)) * MIN(suit * 1.25f, 1.0f);
}

void GrassField3D::_free_shell_tile(ShellTile &p_tile) {
	if (p_tile.mi) {
		remove_child(p_tile.mi);
		memdelete(p_tile.mi);
		p_tile.mi = nullptr;
	}
}

void GrassField3D::_free_shell() {
	for (KeyValue<Vector2i, ShellTile> &kv : shell_tiles) {
		_free_shell_tile(kv.value);
	}
	shell_tiles.clear();
	shell_column_count = 0;
}

void GrassField3D::_update_shell(const Vector2 &p_center, TerrainDataMap3D *p_map) {
	if (!far_shell || crop != CROP_NONE || !is_inside_tree() || fade_owner != this || radius <= 0.0f) {
		if (!shell_tiles.is_empty()) {
			_free_shell();
		}
		return;
	}
	const bool rings = _rings_active();
	const float in_end = rings ? far_ring_range : fade_band.z;
	const float in_band = rings ? far_ring_range * 0.2f : fade_band.w;
	const float out_end = MAX(far_shell_range, in_end + 1.0f);
	const float out_band = MIN(out_end * 0.2f, out_end - in_end);
	if (shell_material.is_null()) {
		Ref<Shader> sh;
		sh.instantiate();
		String code = "shader_type spatial;\nrender_mode cull_disabled;\n\n";
		code += "// GrassField3D far shell (docs/ideas/grass.md, \"Far grass\"): the sward as voxel columns on the ground.\n";
		code += "uniform float sward_height = 0.3; // The blades' mean height (m) this season: how thin snow covers the sward.\n";
		code += "uniform float season_height = 1.0; // The columns' height over their summer height (GrassField3D.season_height_for).\n";
		code += "uniform vec2 shell_center;\nuniform vec4 shell_band; // x: where it is fully up, y: the band it rises over, z: where it is gone, w: the band it sinks over.\n";
		// The climate code first: the turf (get_ground_shader_code()) cures by its wc_climate_cured.
		code += "uniform bool climate_bare = true;\nglobal uniform sampler2D terrain_climate_map : hint_default_black, filter_linear;\nglobal uniform vec4 terrain_climate_rect;\nglobal uniform vec4 weather_climate;\n";
		code += WorldContracts::climate_bare_shader_code();
		code += get_ground_shader_code();
		// Thin snow over the far sward, as on the blades (get_snow_sward_shader_code()).
		code += BaseMaterial3D::get_snow_functions_code(true);
		code += get_snow_sward_shader_code();
		code += R"(
varying vec3 v_wp;
void vertex() {
	vec3 wp = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	float d = length(wp.xz - shell_center);
	float grow_in = shell_band.y > 0.0 ? 1.0 - clamp((shell_band.x - d) / shell_band.y, 0.0, 1.0) : 1.0;
	float grow_out = shell_band.w > 0.0 ? clamp((shell_band.z - d) / shell_band.w, 0.0, 1.0) : 1.0;
	float g = grow_in * grow_out;
	// UV2.x: the vertex's height over the ground; down under it where the shell is gone.
	VERTEX.y += UV2.x * (g * season_height - 1.0) - 0.15 * (1.0 - g);
	v_wp = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
}
void fragment() {
	// The far ground's colour here (the blades seen from afar, with their patches and the sun's lobes), darker down
	// the columns' sides into the sward.
	// Drylands: no sward on the bare soil between the clumps (WorldContracts::bare_patch, as the blades).
	if (climate_bare) {
		float cb = wc_climate_bare(v_wp);
		if (cb > 0.0 && wc_bare_patch(v_wp, cb, 0.0) > 0.5) {
			discard;
		}
	}
	ALBEDO = grass_ground(v_wp, vec3(0.2, 0.16, 0.1)) * COLOR.r;
	ROUGHNESS = 0.95;
	// Snow on the sward: its covered share whitens the columns (the snow's albedo, the blades' tips the rest).
	float sw = grass_snow_cover(snow_pack_at(v_wp), sward_height);
	if (sw > 0.0) {
		ALBEDO = mix(ALBEDO, vec3(snow_cover_at(v_wp).y), sw);
		ROUGHNESS = mix(ROUGHNESS, 0.7, sw);
	}
	// The ground's own sheen under the sward (little): a plain 0.5 lit the columns' tops grey with the sky at grazing.
	SPECULAR = grass_ground_specular(v_wp, 0.3);
}
)";
		sh->set_code(add_camera_cutout(code));
		shell_material.instantiate();
		shell_material->set_shader(sh);
		shell_material->set_shader_parameter("climate_bare", climate_bare);
	}
	shell_material->set_shader_parameter("sward_height", 0.5f * (blade_height.x + blade_height.y) * season_height);
	shell_material->set_shader_parameter("season_height", season_height);
	shell_material->set_shader_parameter("shell_center", p_center);
	shell_material->set_shader_parameter("shell_band", Vector4(in_end, in_band, out_end, out_band));
	const float start = in_end - in_band;
	HashSet<Vector2i> wanted;
	struct Want {
		Vector2i key;
		int cells;
		float d;
		bool operator<(const Want &o) const { return d < o.d; }
	};
	LocalVector<Want> want;
	const Vector2i lo(int(Math::floor((p_center.x - out_end) / SHELL_TILE)), int(Math::floor((p_center.y - out_end) / SHELL_TILE)));
	const Vector2i hi(int(Math::floor((p_center.x + out_end) / SHELL_TILE)), int(Math::floor((p_center.y + out_end) / SHELL_TILE)));
	for (int z = lo.y; z <= hi.y; z++) {
		for (int x = lo.x; x <= hi.x; x++) {
			const Rect2 rect(x * SHELL_TILE, z * SHELL_TILE, SHELL_TILE, SHELL_TILE);
			const Vector2 nearest(CLAMP(p_center.x, rect.position.x, rect.position.x + SHELL_TILE), CLAMP(p_center.y, rect.position.y, rect.position.y + SHELL_TILE));
			const float d_near = nearest.distance_to(p_center);
			const float d_far = Vector2(MAX(Math::abs(p_center.x - rect.position.x), Math::abs(p_center.x - rect.position.x - SHELL_TILE)), MAX(Math::abs(p_center.y - rect.position.y), Math::abs(p_center.y - rect.position.y - SHELL_TILE))).length();
			if (d_near > out_end || d_far < start) {
				continue;
			}
			// Columns about 1.5 m wide, 3 m past 300 m (a few pixels at 1080p either way).
			const int cells = d_near < 300.0f ? 42 : 21;
			const Vector2i k(x, z);
			wanted.insert(k);
			const ShellTile *existing = shell_tiles.getptr(k);
			if (!existing || existing->generation != generation || existing->cells != cells) {
				want.push_back({ k, cells, d_near });
			}
		}
	}
	want.sort();
	last_built += int(want.size());
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	const bool first = shell_tiles.is_empty();
	for (const Want &w : want) {
		if (!first && (OS::get_singleton()->get_ticks_usec() - t0) > uint64_t(RING_BUDGET_MSEC * 1000.0f)) {
			break;
		}
		ShellTile *existing = shell_tiles.getptr(w.key);
		if (existing) {
			_free_shell_tile(*existing);
			shell_tiles.erase(w.key);
		}
		ShellTile tile;
		_build_shell_tile(w.key, w.cells, p_map, tile);
		shell_tiles.insert(w.key, tile);
	}
	LocalVector<Vector2i> drop;
	for (const KeyValue<Vector2i, ShellTile> &kv : shell_tiles) {
		if (!wanted.has(kv.key)) {
			drop.push_back(kv.key);
		}
	}
	for (const Vector2i &k : drop) {
		_free_shell_tile(shell_tiles[k]);
		shell_tiles.erase(k);
	}
}

void GrassField3D::_build_shell_tile(const Vector2i &p_key, int p_cells, TerrainDataMap3D *p_map, ShellTile &r_tile) {
	r_tile.generation = generation;
	r_tile.cells = p_cells;
	Ref<PlantSpecies> r = rules;
	if (r.is_null()) {
		r.instantiate();
	}
	const float cell = SHELL_TILE / float(p_cells);
	const int bx = p_key.x * p_cells;
	const int bz = p_key.y * p_cells;
	const int n = p_cells + 2;
	// Column heights with a ring of neighbours, so the tiles' walls agree.
	LocalVector<float> hgt;
	hgt.resize(n * n);
	int columns = 0;
	for (int j = 0; j < n; j++) {
		for (int i = 0; i < n; i++) {
			hgt[j * n + i] = _shell_column(bx + i - 1, bz + j - 1, cell, r.ptr(), p_map);
		}
	}
	// Corners: jittered by a hash shared with the neighbours, on the ground.
	const int m = p_cells + 1;
	LocalVector<Vector3> corner;
	corner.resize(m * m);
	const uint32_t salt = 0x9E3779B9u ^ uint32_t(p_cells);
	const float gy = get_global_position().y;
	for (int j = 0; j < m; j++) {
		for (int i = 0; i < m; i++) {
			const uint32_t h = cell_hash(bx + i, bz + j, salt);
			const float x = (bx + i + (unit(h, 0) - 0.5f) * 0.6f) * cell;
			const float z = (bz + j + (unit(h, 8) - 0.5f) * 0.6f) * cell;
			const float y = p_map ? p_map->get_height_at(Vector3(x, gy, z)) : gy;
			corner[j * m + i] = Vector3(x, y, z);
		}
	}
	PackedVector3Array verts;
	PackedVector3Array normals;
	PackedColorArray colors;
	PackedVector2Array uv2s;
	PackedInt32Array idx;
	const Transform3D to_local = get_global_transform().affine_inverse();
	auto quad = [&](const Vector3 p_v[4], const float p_h[4], const float p_shade[4], const Vector3 &p_out) {
		Vector3 nn = (p_v[1] - p_v[0]).cross(p_v[3] - p_v[0]);
		if (nn.dot(p_out) < 0.0f) {
			nn = -nn;
		}
		nn = nn.normalized();
		const int first_v = verts.size();
		for (int c = 0; c < 4; c++) {
			verts.push_back(to_local.xform(p_v[c]));
			normals.push_back(to_local.basis.xform(nn).normalized());
			colors.push_back(Color(p_shade[c], p_shade[c], p_shade[c], 1.0f));
			uv2s.push_back(Vector2(p_h[c], 0.0f));
		}
		idx.append_array(PackedInt32Array({ first_v, first_v + 1, first_v + 2, first_v, first_v + 2, first_v + 3 }));
	};
	// The four sides: neighbour offset, and the two corners (in the corner grid) of the shared edge.
	const int side_d[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
	const int side_c[4][4] = { { 1, 0, 1, 1 }, { 0, 1, 0, 0 }, { 1, 1, 0, 1 }, { 0, 0, 1, 0 } };
	for (int j = 0; j < p_cells; j++) {
		for (int i = 0; i < p_cells; i++) {
			const float h = hgt[(j + 1) * n + i + 1];
			if (h <= 0.0f) {
				continue;
			}
			columns++;
			const Vector3 c00 = corner[j * m + i];
			const Vector3 c10 = corner[j * m + i + 1];
			const Vector3 c11 = corner[(j + 1) * m + i + 1];
			const Vector3 c01 = corner[(j + 1) * m + i];
			{
				const Vector3 v[4] = { c00 + Vector3(0, h, 0), c10 + Vector3(0, h, 0), c11 + Vector3(0, h, 0), c01 + Vector3(0, h, 0) };
				const float hh[4] = { h, h, h, h };
				const float sh[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
				quad(v, hh, sh, Vector3(0, 1, 0));
			}
			for (int s = 0; s < 4; s++) {
				const float hn = hgt[(j + 1 + side_d[s][1]) * n + i + 1 + side_d[s][0]];
				if (h <= hn + 0.02f) {
					continue;
				}
				const Vector3 a = corner[(j + side_c[s][1]) * m + i + side_c[s][0]];
				const Vector3 b = corner[(j + side_c[s][3]) * m + i + side_c[s][2]];
				// Down the side into the sward: its lower part in the blades' shade.
				const float low = hn > 0.0f ? 0.8f : 0.6f;
				const Vector3 v[4] = { a + Vector3(0, hn, 0), b + Vector3(0, hn, 0), b + Vector3(0, h, 0), a + Vector3(0, h, 0) };
				const float hh[4] = { hn, hn, h, h };
				const float sh[4] = { low, low, 0.9f, 0.9f };
				quad(v, hh, sh, Vector3(float(side_d[s][0]), 0.0f, float(side_d[s][1])));
			}
		}
	}
	if (verts.is_empty()) {
		return;
	}
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = verts;
	arrays[Mesh::ARRAY_NORMAL] = normals;
	arrays[Mesh::ARRAY_COLOR] = colors;
	arrays[Mesh::ARRAY_TEX_UV2] = uv2s;
	arrays[Mesh::ARRAY_INDEX] = idx;
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	mesh->surface_set_material(0, shell_material);
	r_tile.mi = memnew(MeshInstance3D);
	r_tile.mi->set_mesh(mesh);
	// Its shadows are under a pixel this far out; the path tracer traces them anyway.
	r_tile.mi->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
	r_tile.mi->set_name(vformat("GrassShell_%d_%d", p_key.x, p_key.y));
	add_child(r_tile.mi, false, INTERNAL_MODE_BACK);
	shell_column_count += columns;
}

void GrassField3D::set_far_shell(bool p_enabled) {
	far_shell = p_enabled;
	if (!far_shell) {
		_free_shell();
	}
}

// Burn ages changed somewhere (WorldFire.burn_changed): sample the grid again there and rebuild what stands on it.
void GrassField3D::_follow_burns(TerrainDataMap3D *p_map) {
	const WorldFire *fire = WorldContracts::fire();
	if (!fire || !p_map || crop != CROP_NONE || !burn_read || fire->get_burn_generation() == burn_seen) {
		return;
	}
	const uint64_t now = OS::get_singleton()->get_ticks_usec();
	if (now - burn_follow_usec < uint64_t(BURN_FOLLOW_MSEC) * 1000) {
		return;
	}
	burn_follow_usec = now;
	if (burn_recovery <= 0.0f && (rules.is_null() || rules->get_burn_recovery() <= 0.0f)) {
		burn_seen = fire->get_burn_generation();
		return;
	}
	Rect2 area;
	const bool partial = fire->burn_changes_since(burn_seen, area);
	const Rect2 map_rect = p_map->get_map_rect();
	if (!partial || burn_ages.is_empty()) {
		const bool had = !burn_ages.is_empty();
		_read_burn_age(p_map);
		if (!partial) {
			area = map_rect;
		} else if (!had && burn_ages.is_empty()) {
			return; // Still nothing burnt on this map.
		}
	} else {
		burn_seen = fire->get_burn_generation();
		const float cx = burn_rect.size.x / (burn_nx - 1);
		const float cz = burn_rect.size.y / (burn_ny - 1);
		const int x0 = CLAMP(int(Math::floor((area.position.x - burn_rect.position.x) / cx)) - 1, 0, burn_nx - 1);
		const int x1 = CLAMP(int(Math::ceil((area.get_end().x - burn_rect.position.x) / cx)) + 1, 0, burn_nx - 1);
		const int z0 = CLAMP(int(Math::floor((area.position.y - burn_rect.position.y) / cz)) - 1, 0, burn_ny - 1);
		const int z1 = CLAMP(int(Math::ceil((area.get_end().y - burn_rect.position.y) / cz)) + 1, 0, burn_ny - 1);
		for (int z = z0; z <= z1; z++) {
			for (int x = x0; x <= x1; x++) {
				const Vector2 xz = burn_rect.position + Vector2(x * cx, z * cz);
				burn_ages[z * burn_nx + x] = fire->burn_age_at(Vector3(xz.x, 0.0f, xz.y));
			}
		}
	}
	if (area.intersects(map_rect, true)) {
		_burn_dirty(area);
		_publish_burn_flush();
	}
}

// Deer lawns: the browse pressure over the map as how far the sward is grazed down, smoothstep(0.3, 0.8, pressure)
// (a light pressure leaves the sward standing; Putman 1986), on a grid like the burn ages'.
void GrassField3D::_read_graze(TerrainDataMap3D *p_map) {
	const WorldStand *stand = WorldContracts::stand();
	graze_any = false;
	if (stand) {
		graze_seen = stand->get_stand_generation();
	}
	if (!stand || !p_map || crop != CROP_NONE || graze_height <= 0.0f) {
		return;
	}
	const Rect2 rect = p_map->get_map_rect();
	if (!rect.has_area()) {
		return;
	}
	const float cell = MAX(2.0f, MAX(rect.size.x, rect.size.y) / 511.0f);
	const int nx = CLAMP(int(Math::ceil(rect.size.x / cell)) + 1, 2, 512);
	const int ny = CLAMP(int(Math::ceil(rect.size.y / cell)) + 1, 2, 512);
	// R: grazed to a lawn; G: the share of blades boar rooting tore out (all of them fresh, back over two years with the terrain's freshness 1 - smoothstep(0.25, 2, years):
	// Bueno et al. 2013), which the sods (PlantGenerator.make_rooting) cover meanwhile.
	Vector<uint8_t> data;
	data.resize(nx * ny * 2);
	uint8_t *w = data.ptrw();
	for (int z = 0; z < ny; z++) {
		for (int x = 0; x < nx; x++) {
			const Vector2 xz = rect.position + rect.size * Vector2(float(x) / (nx - 1), float(z) / (ny - 1));
			const Vector3 p(xz.x, 0.0f, xz.y);
			const float g = Math::smoothstep(0.3f, 0.8f, stand->browse_at(p));
			const float ra = stand->rooting_age_at(p);
			const float torn = ra < 0.0f ? 0.0f : 1.0f - Math::smoothstep(0.25f, 2.0f, ra); // The terrain's freshness.
			const int i = (z * nx + x) * 2;
			w[i] = uint8_t(CLAMP(g, 0.0f, 1.0f) * 255.0f + 0.5f);
			w[i + 1] = uint8_t(CLAMP(torn, 0.0f, 1.0f) * 255.0f + 0.5f);
			graze_any = graze_any || w[i] > 0 || w[i + 1] > 0;
		}
	}
	if (!graze_any) {
		graze_texture.unref();
		return;
	}
	const Ref<Image> img = Image::create_from_data(nx, ny, false, Image::FORMAT_RG8, data);
	if (graze_texture.is_valid() && graze_nx == nx && graze_ny == ny) {
		graze_texture->update(img);
	} else {
		graze_texture = ImageTexture::create_from_image(img);
	}
	graze_nx = nx;
	graze_ny = ny;
	graze_rect = rect;
}

void GrassField3D::_publish_graze() {
	if (material.is_null()) {
		return;
	}
	// The field that colours the ground (ground_tint_enabled) also hands the map to grass_ground(): under a deer
	// lawn the turf closes over the soil.
	RenderingServer *rs = RenderingServer::get_singleton();
	const bool ground = ground_tint_enabled && is_inside_tree();
	if (!graze_any || graze_texture.is_null() || graze_height <= 0.0f) {
		material->set_shader_parameter("graze_rect", Vector4());
		if (ground && graze_published) {
			rs->global_shader_parameter_set("grass_graze_rect", Vector4());
			graze_published = false;
		}
		return;
	}
	const Vector2 cell(graze_rect.size.x / (graze_nx - 1), graze_rect.size.y / (graze_ny - 1));
	const Vector4 rect(graze_rect.position.x - cell.x * 0.5f, graze_rect.position.y - cell.y * 0.5f, graze_rect.size.x + cell.x, graze_rect.size.y + cell.y);
	material->set_shader_parameter("graze_map", graze_texture);
	material->set_shader_parameter("graze_rect", rect);
	material->set_shader_parameter("graze_height", graze_height);
	if (ground) {
		rs->global_shader_parameter_set("grass_graze_map", graze_texture->get_rid());
		rs->global_shader_parameter_set("grass_graze_rect", rect);
		graze_published = true;
	}
}

void GrassField3D::set_climate_bare(bool p_on) {
	climate_bare = p_on;
	if (material.is_valid()) {
		material->set_shader_parameter("climate_bare", climate_bare && crop == CROP_NONE);
	}
	if (shell_material.is_valid()) {
		shell_material->set_shader_parameter("climate_bare", climate_bare);
	}
}

void GrassField3D::set_graze_height(float p_m) {
	graze_height = MAX(p_m, 0.0f);
	if (graze_height > 0.0f && graze_texture.is_null()) {
		graze_seen = 0; // Read it on the next process.
	}
	_publish_graze();
}

// Storm lodging (docs/ideas/vegetation.md, "Storm lodging"). A smooth value noise in world space, so a cell keeps its
// strength and swirl when the grid moves with the camera.
static float _lodge_noise(float p_x, float p_z, uint32_t p_seed) {
	const float fx = Math::floor(p_x);
	const float fz = Math::floor(p_z);
	const int ix = int(fx);
	const int iz = int(fz);
	auto corner = [&](int p_dx, int p_dz) {
		uint32_t h = hash_murmur3_one_32(uint32_t(ix + p_dx), p_seed);
		h = hash_fmix32(hash_murmur3_one_32(uint32_t(iz + p_dz), h));
		return float(h & 0xffffff) / float(0xffffff);
	};
	float tx = p_x - fx;
	float tz = p_z - fz;
	tx = tx * tx * (3.0f - 2.0f * tx);
	tz = tz * tz * (3.0f - 2.0f * tz);
	return Math::lerp(Math::lerp(corner(0, 0), corner(1, 0), tx), Math::lerp(corner(0, 1), corner(1, 1), tx), tz);
}

float GrassField3D::_lodge_height() const {
	return crop != CROP_NONE ? _crop_shape(crop).height : MAX(blade_height.x, blade_height.y);
}

// The grid follows the camera: once it is a quarter of the grid from the centre, the cells move under it and the
// overlap keeps its state.
void GrassField3D::_lodge_place(const Vector3 &p_center) {
	const Vector2i want(int(Math::floor(p_center.x / LODGE_CELL)) - LODGE_N / 2, int(Math::floor(p_center.z / LODGE_CELL)) - LODGE_N / 2);
	if (lodge_placed && Math::abs(want.x - lodge_origin.x) < LODGE_N / 4 && Math::abs(want.y - lodge_origin.y) < LODGE_N / 4) {
		return;
	}
	LocalVector<float> amt;
	LocalVector<Vector2> dir;
	amt.resize(LODGE_N * LODGE_N);
	dir.resize(LODGE_N * LODGE_N);
	for (int i = 0; i < LODGE_N * LODGE_N; i++) {
		amt[i] = 0.0f;
		dir[i] = Vector2(1.0f, 0.0f);
	}
	if (lodge_placed) {
		for (int y = 0; y < LODGE_N; y++) {
			const int oy = y + want.y - lodge_origin.y;
			if (oy < 0 || oy >= LODGE_N) {
				continue;
			}
			for (int x = 0; x < LODGE_N; x++) {
				const int ox = x + want.x - lodge_origin.x;
				if (ox >= 0 && ox < LODGE_N) {
					amt[y * LODGE_N + x] = lodge_amt[oy * LODGE_N + ox];
					dir[y * LODGE_N + x] = lodge_dir[oy * LODGE_N + ox];
				}
			}
		}
	} else {
		lodge_row_time.resize(LODGE_N);
		for (int r = 0; r < LODGE_N; r++) {
			lodge_row_time[r] = lodge_clock;
		}
	}
	lodge_amt = amt;
	lodge_dir = dir;
	lodge_origin = want;
	lodge_placed = true;
	lodge_dirty = true;
}

// One cell over p_dt weather seconds. The lodging risk follows the cereal lodging model's moment balance (Baker et al.
// 1998; Berry et al. 2004): the wind's moment on the stem goes with the gust speed squared and the shoot's height, the
// rain held on the leaves adds weight, and wet soil gives the base less anchorage. The critical 2 m gust is 10 m/s for
// a 0.6 m sward (a wet summer gale lays meadows down; a dry one needs a storm), higher for shorter grass. Patches come
// from the stems' strength varying over tens of metres; the swirl from gusts turning in the lee of the patch.
void GrassField3D::_lodge_step_cell(int p_i, const Vector3 &p_pos, float p_dt, const WorldWeather *p_weather, float p_ucrit, bool p_crop) {
	float &a = lodge_amt[p_i];
	const Vector3 w = p_weather->wind_at(p_pos, 2.0f);
	const Vector2 u(w.x, w.z);
	const float speed = u.length();
	const float wet = CLAMP(p_weather->surface_wetness_at(p_pos), 0.0f, 1.0f);
	float target = 0.0f;
	if (speed > p_ucrit * 0.45f) {
		const float rain = MAX(p_weather->rainfall_at(p_pos), 0.0f);
		// Sturdy (0.3) to weak (1.7) stands over tens of metres (fertility, density): the patches.
		const float n = 0.65f * _lodge_noise(p_pos.x / 41.0f, p_pos.z / 41.0f, 0x10d9e5u) + 0.35f * _lodge_noise(p_pos.x / 13.0f, p_pos.z / 13.0f, 0x51a7u);
		const float ns = CLAMP((n - 0.2f) / 0.6f, 0.0f, 1.0f);
		const float weakness = 0.3f + 1.4f * ns * ns * (3.0f - 2.0f * ns);
		const float risk = (speed / p_ucrit) * (speed / p_ucrit) * (1.0f + 0.6f * wet) * (1.0f + MIN(rain / 40.0f, 0.5f)) / (1.0f - 0.35f * wet) * weakness;
		const float t = CLAMP((risk - 1.0f) / 3.0f, 0.0f, 1.0f);
		target = t * t * (3.0f - 2.0f * t);
	}
	if (target > a) {
		// Down within a couple of minutes of strong gusts, along the wind turned by the patch's swirl.
		const float k = MIN(1.0f, p_dt / 90.0f);
		const float swirl = (_lodge_noise(p_pos.x / 23.0f, p_pos.z / 23.0f, 0x5717u) - 0.5f) * 1.4f;
		const Vector2 to = (u / speed).rotated(swirl);
		lodge_dir[p_i] = a < 0.02f ? to : lodge_dir[p_i].lerp(to, k * (target - a) / MAX(target, 1e-3f)).normalized();
		a += (target - a) * k;
	} else if (a > 0.0f) {
		// Stems lift again at the nodes (gravitropism) over days as they dry; ripe cereals hardly do.
		const float tau = (p_crop ? 40.0f : 2.5f) * 86400.0f * (1.0f + 2.0f * wet);
		a *= Math::exp(-p_dt / tau);
		if (a < 0.03f) { // Under one blade in twenty leaning: standing.
			a = 0.0f;
		}
	}
	if (a > 0.0f) {
		lodge_counting++;
	}
}

void GrassField3D::_lodge_rows(int p_rows, const Vector3 &p_center) {
	const WorldWeather *wm = WorldContracts::weather();
	const float h = _lodge_height();
	const float ucrit = 10.0f * Math::sqrt(0.6f / MAX(h, 0.15f));
	const bool crop_field = crop != CROP_NONE;
	for (int r = 0; r < p_rows; r++) {
		const int row = lodge_row;
		const float dt = float(lodge_clock - lodge_row_time[row]);
		lodge_row_time[row] = lodge_clock;
		if (row == 0) {
			// Nothing lodged and the wind far from lodging (or the sward too short): skip the sweep.
			const Vector3 w = wm->wind_at(p_center, 2.0f);
			lodge_calm = lodge_any == 0 && (h < 0.15f || Vector2(w.x, w.z).length() < ucrit * 0.4f);
			lodge_counting = 0;
		}
		if (!lodge_calm && dt > 0.0f) {
			for (int x = 0; x < LODGE_N; x++) {
				const int i = row * LODGE_N + x;
				const float before = lodge_amt[i];
				const Vector3 pos((lodge_origin.x + x + 0.5f) * LODGE_CELL, p_center.y, (lodge_origin.y + row + 0.5f) * LODGE_CELL);
				_lodge_step_cell(i, pos, dt, wm, ucrit, crop_field);
				if (lodge_amt[i] != before) {
					lodge_dirty = true;
				}
			}
		}
		lodge_row = (row + 1) % LODGE_N;
		if (lodge_row == 0) {
			lodge_any = lodge_calm ? 0 : lodge_counting;
			if (lodge_dirty) {
				_lodge_upload();
			}
		}
	}
}

void GrassField3D::_lodge_upload() {
	lodge_dirty = false;
	if (material.is_null()) {
		return;
	}
	if (!lodging || !lodge_placed || lodge_any == 0) {
		material->set_shader_parameter("lodge_rect", Vector4());
		return;
	}
	Ref<Image> img = Image::create_empty(LODGE_N, LODGE_N, false, Image::FORMAT_RGBA8);
	for (int y = 0; y < LODGE_N; y++) {
		for (int x = 0; x < LODGE_N; x++) {
			const int i = y * LODGE_N + x;
			const Vector2 d = lodge_dir[i];
			img->set_pixel(x, y, Color(d.x * 0.5f + 0.5f, d.y * 0.5f + 0.5f, lodge_amt[i], 1.0f));
		}
	}
	if (lodge_texture.is_valid()) {
		lodge_texture->update(img);
	} else {
		lodge_texture = ImageTexture::create_from_image(img);
	}
	material->set_shader_parameter("lodge_map", lodge_texture);
	material->set_shader_parameter("lodge_rect", Vector4(lodge_origin.x * LODGE_CELL, lodge_origin.y * LODGE_CELL, LODGE_N * LODGE_CELL, LODGE_N * LODGE_CELL));
}

void GrassField3D::set_lodging(bool p_on) {
	lodging = p_on;
	if (!p_on) {
		lodge_placed = false;
		lodge_any = 0;
		lodge_amt.clear();
		lodge_dir.clear();
	}
	_lodge_upload();
}

Vector2 GrassField3D::snow_lie(const WeatherSeason::Climate &p_climate, float p_height, int p_day) {
	// The degree-day pack of WeatherSeason (snow_water_at), day by day from 1 August, keeping the last lie of 10 days
	// or more over 3 mm of water.
	const int n = WeatherSeason::days_since(p_day, 213);
	float swe = 0.0f;
	int lie_start = -1;
	float last_lie = 0.0f;
	int melted_at = -1;
	for (int k = 0; k <= n; k++) {
		const float d = float((213 + k - 1) % 365 + 1);
		const float t = WeatherSeason::day_temperature(p_climate, p_height, d);
		swe += WeatherSeason::day_precipitation(p_climate, d) * WeatherSeason::snow_share(p_climate, t);
		swe = MAX(swe - p_climate.melt_factor * MAX(t, 0.0f), 0.0f);
		if (swe > 3.0f) {
			if (lie_start < 0) {
				lie_start = k;
			}
		} else if (lie_start >= 0) {
			if (k - lie_start >= 10) {
				last_lie = float(k - lie_start);
				melted_at = k;
			}
			lie_start = -1;
		}
	}
	if (lie_start >= 0 && n - lie_start >= 10) {
		return Vector2(float(n - lie_start), -1.0f); // still under it
	}
	return melted_at >= 0 ? Vector2(last_lie, float(n - melted_at)) : Vector2(0.0f, -1.0f);
}

// Once a day of the season: the lie and melt per 50 m band round the field's height, for the blades.
void GrassField3D::_update_snowmelt(const Dictionary &p_season) {
	if (!material.is_valid()) {
		return;
	}
	const int day = int(p_season.get("day_of_year", -1));
	WeatherSeason::Climate climate;
	if (!snowmelt || crop != CROP_NONE || day < 1 || !WorldContracts::weather()->season_climate(climate)) {
		material->set_shader_parameter("snowmelt", 0.0f);
		melt_day = -2;
		return;
	}
	if (day == melt_day) {
		return;
	}
	melt_day = day;
	melt_h0 = Math::floor((is_inside_tree() ? get_global_position().y : 0.0f) / 50.0f) * 50.0f - 400.0f;
	PackedVector2Array bands;
	bands.resize(16);
	for (int i = 0; i < 16; i++) {
		bands.write[i] = snow_lie(climate, melt_h0 + 50.0f * i, day);
	}
	material->set_shader_parameter("melt_bands", bands);
	material->set_shader_parameter("melt_band0", Vector2(melt_h0, 50.0f));
	material->set_shader_parameter("snowmelt", 1.0f);
}

Vector3 GrassField3D::get_snowmelt_at(const Vector3 &p_position, int p_day) const {
	WeatherSeason::Climate climate;
	if (!WorldContracts::weather()->season_climate(climate)) {
		return Vector3(0.0f, -1.0f, 0.0f);
	}
	int day = p_day;
	if (day < 1) {
		day = int(WorldContracts::weather()->season_at(p_position).get("day_of_year", 1));
	}
	const Vector2 l = snow_lie(climate, p_position.y, day);
	const float mat = l.y >= 0.0f && l.x > 20.0f ? Math::smoothstep(20.0f, 60.0f, float(l.x)) * (1.0f - Math::smoothstep(4.0f, 24.0f, float(l.y))) : 0.0f;
	return Vector3(l.x, l.y, mat);
}

// The last flood's mark round the camera (WorldWater.high_water_at) on a FLOOD_N grid of FLOOD_CELL m, read again
// every 5 s and when the camera has moved a quarter of the grid. Cells where no producer knows a flood take their
// known neighbours' peak (so the line interpolates cleanly to the grid's known edge) but stay unknown for the blades
// (alpha 0) unless a known neighbour is close.
void GrassField3D::_flood_step(const Vector3 &p_center, bool p_force) {
	if (!material.is_valid()) {
		return;
	}
	const WorldWater *water = WorldContracts::water();
	if (!flood_wash || crop != CROP_NONE || !water->has_provider()) {
		if (flood_any) {
			material->set_shader_parameter("flood_rect", Vector4());
			flood_any = false;
		}
		return;
	}
	const Vector2i want(int(Math::floor(p_center.x / FLOOD_CELL)) - FLOOD_N / 2, int(Math::floor(p_center.z / FLOOD_CELL)) - FLOOD_N / 2);
	const uint64_t now = OS::get_singleton()->get_ticks_usec();
	const bool moved = Math::abs(want.x - flood_origin.x) >= FLOOD_N / 4 || Math::abs(want.y - flood_origin.y) >= FLOOD_N / 4;
	if (!p_force && flood_usec != 0 && !moved && now - flood_usec < 5000000) {
		return;
	}
	flood_usec = MAX(now, uint64_t(1));
	flood_origin = want;
	LocalVector<Color> cells;
	cells.resize(FLOOD_N * FLOOD_N);
	bool any = false;
	for (int y = 0; y < FLOOD_N; y++) {
		for (int x = 0; x < FLOOD_N; x++) {
			const Vector3 pos((want.x + x + 0.5f) * FLOOD_CELL, p_center.y, (want.y + y + 0.5f) * FLOOD_CELL);
			const Vector4 hw = water->high_water_at(pos);
			Color &c = cells[y * FLOOD_N + x];
			if (hw.x > -1e20f && hw.y >= 0.0f) {
				const Vector2 d(hw.z, hw.w);
				c = Color(hw.x, hw.y, d.length_squared() > 1e-6f ? Math::atan2(d.y, d.x) : 100.0f, 1.0f);
				any = true;
			} else {
				c = Color(0.0f, -1.0f, 100.0f, 0.0f);
			}
		}
	}
	flood_any = any;
	if (!any) {
		material->set_shader_parameter("flood_rect", Vector4());
		return;
	}
	// Spread the known peaks two cells into the unknown ones (values only, not the known mask): the bilinear line
	// stays at the true peak up to the last known cell.
	for (int pass = 0; pass < 2; pass++) {
		LocalVector<Color> next;
		next.resize(cells.size());
		for (uint32_t k = 0; k < cells.size(); k++) {
			next[k] = cells[k];
		}
		for (int y = 0; y < FLOOD_N; y++) {
			for (int x = 0; x < FLOOD_N; x++) {
				Color &c = next[y * FLOOD_N + x];
				if (c.g >= 0.0f) {
					continue;
				}
				float sum_h = 0.0f, sum_d = 0.0f, n = 0.0f;
				float dir = 100.0f;
				for (int dy = -1; dy <= 1; dy++) {
					for (int dx = -1; dx <= 1; dx++) {
						const int xx = x + dx, yy = y + dy;
						if (xx < 0 || yy < 0 || xx >= FLOOD_N || yy >= FLOOD_N) {
							continue;
						}
						const Color &o = cells[yy * FLOOD_N + xx];
						if (o.g >= 0.0f) {
							sum_h += o.r;
							sum_d += o.g;
							dir = o.b;
							n += 1.0f;
						}
					}
				}
				if (n > 0.0f) {
					c = Color(sum_h / n, sum_d / n, dir, c.a);
				}
			}
		}
		for (uint32_t k = 0; k < cells.size(); k++) {
			cells[k] = next[k];
		}
	}
	Ref<Image> img = Image::create_empty(FLOOD_N, FLOOD_N, false, Image::FORMAT_RGBAF);
	for (int y = 0; y < FLOOD_N; y++) {
		for (int x = 0; x < FLOOD_N; x++) {
			img->set_pixel(x, y, cells[y * FLOOD_N + x]);
		}
	}
	if (flood_texture.is_valid() && flood_texture->get_width() == FLOOD_N) {
		flood_texture->update(img);
	} else {
		flood_texture = ImageTexture::create_from_image(img);
	}
	material->set_shader_parameter("flood_map", flood_texture);
	material->set_shader_parameter("flood_rect", Vector4(want.x * FLOOD_CELL, want.y * FLOOD_CELL, FLOOD_N * FLOOD_CELL, FLOOD_N * FLOOD_CELL));
}

Vector3 GrassField3D::get_flood_wash_at(const Vector3 &p_position) const {
	const Vector4 hw = WorldContracts::water()->high_water_at(p_position);
	if (!(hw.x > -1e20f && hw.y >= 0.0f) || p_position.y >= hw.x) {
		return Vector3(0.0f, 0.0f, -1.0f);
	}
	return Vector3(1.0f - Math::smoothstep(7.0f, 35.0f, float(hw.y)), 1.0f - Math::smoothstep(20.0f, 120.0f, float(hw.y)), hw.y);
}

void GrassField3D::set_flood_wash(bool p_on) {
	flood_wash = p_on;
	flood_usec = 0;
	if (material.is_valid() && !p_on) {
		material->set_shader_parameter("flood_rect", Vector4());
		flood_any = false;
	}
}

void GrassField3D::set_snowmelt(bool p_on) {
	snowmelt = p_on;
	melt_day = -2;
	if (material.is_valid() && !snowmelt) {
		material->set_shader_parameter("snowmelt", 0.0f);
	}
}

Vector3 GrassField3D::get_terracette_at(const Vector3 &p_global_position, const Vector2 &p_gradient) {
	return WorldContracts::terracette_at(p_global_position, p_gradient);
}

void GrassField3D::set_terracettes(bool p_on) {
	terracettes = p_on;
	if (material.is_valid()) {
		material->set_shader_parameter("terracettes", terracettes && crop == CROP_NONE ? 1.0f : 0.0f);
	}
}

void GrassField3D::set_deer_beds(bool p_on) {
	deer_beds = p_on;
	if (material.is_valid()) {
		material->set_shader_parameter("deer_beds", deer_beds && crop == CROP_NONE ? 1.0f : 0.0f);
	}
}

void GrassField3D::set_deer_bed_day(float p_day) {
	deer_bed_day = p_day;
	if (material.is_valid()) {
		material->set_shader_parameter("deer_bed_day", deer_bed_day);
	}
}

Vector4 GrassField3D::get_deer_bed_at(const Vector3 &p_global_position, float p_day) {
	const DeerBeds::Hit hit = DeerBeds::at(Vector2(p_global_position.x, p_global_position.z), p_day);
	return Vector4(hit.press, hit.lie.x, hit.lie.y, hit.length);
}

void GrassField3D::set_fairy_rings(bool p_on) {
	fairy_rings = p_on;
	if (material.is_valid()) {
		material->set_shader_parameter("fairy_rings", fairy_rings && crop == CROP_NONE ? 1.0f : 0.0f);
	}
}

Vector3 GrassField3D::get_fairy_ring_at(const Vector3 &p_global_position) {
	return FairyRings::bands(FairyRings::at(Vector2(p_global_position.x, p_global_position.z)));
}

Vector3 GrassField3D::get_lodging_at(const Vector3 &p_global_position) const {
	if (!lodge_placed) {
		return Vector3();
	}
	const int x = int(Math::floor(p_global_position.x / LODGE_CELL)) - lodge_origin.x;
	const int y = int(Math::floor(p_global_position.z / LODGE_CELL)) - lodge_origin.y;
	if (x < 0 || y < 0 || x >= LODGE_N || y >= LODGE_N) {
		return Vector3();
	}
	const int i = y * LODGE_N + x;
	return Vector3(lodge_amt[i], lodge_dir[i].x, lodge_dir[i].y);
}

void GrassField3D::advance_lodging(double p_weather_seconds) {
	if (!lodging || p_weather_seconds <= 0.0) {
		return;
	}
	Vector3 center = get_global_position();
	if (is_inside_tree() && get_viewport() && get_viewport()->get_camera_3d()) {
		center = get_viewport()->get_camera_3d()->get_global_position();
	}
	_lodge_place(center);
	lodge_clock += p_weather_seconds;
	// One whole sweep from row 0, each row stepped over its full time since it was last stepped.
	const int left = (LODGE_N - lodge_row) % LODGE_N;
	_lodge_rows(left, center);
	_lodge_rows(LODGE_N, center);
	_lodge_upload();
}

// The regrowth flush (shader's burn_flush_map) from the burn grid: 1 - smoothstep(0, 1.5 yr, age), 0 unburnt.
void GrassField3D::_publish_burn_flush() {
	if (material.is_null()) {
		return;
	}
	if (burn_ages.is_empty()) {
		material->set_shader_parameter("burn_flush_rect", Vector4());
		burn_flush_texture.unref();
		return;
	}
	Vector<uint8_t> data;
	data.resize(burn_nx * burn_ny);
	uint8_t *w = data.ptrw();
	for (int i = 0; i < burn_nx * burn_ny; i++) {
		const float a = burn_ages[i];
		w[i] = uint8_t(CLAMP(a < 0.0f ? 0.0f : 1.0f - Math::smoothstep(0.0f, 1.5f, a), 0.0f, 1.0f) * 255.0f + 0.5f);
	}
	const Ref<Image> img = Image::create_from_data(burn_nx, burn_ny, false, Image::FORMAT_R8, data);
	if (burn_flush_texture.is_valid() && burn_flush_texture->get_width() == burn_nx && burn_flush_texture->get_height() == burn_ny) {
		burn_flush_texture->update(img);
	} else {
		burn_flush_texture = ImageTexture::create_from_image(img);
	}
	// Texel centres on the grid's nodes.
	const Vector2 cell(burn_rect.size.x / (burn_nx - 1), burn_rect.size.y / (burn_ny - 1));
	material->set_shader_parameter("burn_flush_map", burn_flush_texture);
	material->set_shader_parameter("burn_flush_rect", Vector4(burn_rect.position.x - cell.x * 0.5f, burn_rect.position.y - cell.y * 0.5f, burn_rect.size.x + cell.x, burn_rect.size.y + cell.y));
}

// What stands on the area builds again (stale generation: each is replaced once its successor is built).
void GrassField3D::_burn_dirty(const Rect2 &p_area) {
	const float margin = burn_ages.is_empty() ? 4.0f : burn_rect.size.x / (burn_nx - 1) + 0.5f;
	const Rect2 a = p_area.grow(margin);
	for (KeyValue<Vector2i, Tile> &kv : tiles) {
		if (a.intersects(Rect2(Vector2(kv.key) * tile_size, Vector2(tile_size, tile_size)), true)) {
			kv.value.generation = generation - 1;
		}
	}
	for (KeyValue<Vector3i, RingTile> &kv : ring_tiles) {
		const float w = RING_TILE * float(1 << kv.key.z);
		if (a.intersects(Rect2(kv.key.x * w, kv.key.y * w, w, w), true)) {
			kv.value.generation = generation - 1;
		}
	}
	for (KeyValue<Vector2i, ShellTile> &kv : shell_tiles) {
		if (a.intersects(Rect2(Vector2(kv.key) * SHELL_TILE, Vector2(SHELL_TILE, SHELL_TILE)), true)) {
			kv.value.generation = generation - 1;
		}
	}
	ground_baked = false; // The ground tint (grass_ground_map) over the burn.
}

void GrassField3D::set_burn_recovery(float p_years) {
	burn_recovery = MAX(p_years, 0.0f);
	rebuild();
}

void GrassField3D::_read_sand_sea(TerrainDataMap3D *p_map) {
	sea_depth.clear();
	sea_nx = 0;
	sea_ny = 0;
	springs.clear();
	const TerrainGenerator3D *gen = p_map ? VegetationHoles::source_of(p_map) : nullptr;
	if (!gen || crop != CROP_NONE || !gen->get_sand_sea_rect().has_area()) {
		return;
	}
	const Rect2 rect = gen->get_sand_sea_rect();
	const float cell = MAX(MAX(float(gen->get_sand_sea_scale()), 2.0f), MAX(rect.size.x, rect.size.y) / 1023.0f);
	const int nx = CLAMP(int(Math::ceil(rect.size.x / cell)) + 1, 2, 1024);
	const int ny = CLAMP(int(Math::ceil(rect.size.y / cell)) + 1, 2, 1024);
	LocalVector<float> depth;
	depth.resize(nx * ny);
	bool any = false;
	for (int z = 0; z < ny; z++) {
		for (int x = 0; x < nx; x++) {
			const float d = float(gen->get_sand_sea_height(Vector3(rect.position.x + rect.size.x * x / (nx - 1), 0.0f, rect.position.y + rect.size.y * z / (ny - 1))));
			depth[z * nx + x] = d;
			any = any || d > 0.05f;
		}
	}
	if (any) {
		sea_depth = depth;
		sea_rect = rect;
		sea_nx = nx;
		sea_ny = ny;
		springs.build(gen);
	}
}

void GrassField3D::_read_burn_age(TerrainDataMap3D *p_map) {
	burn_ages.clear();
	burn_nx = 0;
	burn_ny = 0;
	const WorldFire *fire = WorldContracts::fire();
	if (fire) {
		burn_seen = fire->get_burn_generation();
	}
	if (!p_map || crop != CROP_NONE || !fire || !fire->has_provider()) {
		return;
	}
	if (burn_recovery <= 0.0f && (rules.is_null() || rules->get_burn_recovery() <= 0.0f)) {
		return;
	}
	const Rect2 rect = p_map->get_map_rect();
	if (!rect.has_area()) {
		return;
	}
	const float cell = MAX(2.0f, MAX(rect.size.x, rect.size.y) / 511.0f);
	const int nx = CLAMP(int(Math::ceil(rect.size.x / cell)) + 1, 2, 512);
	const int ny = CLAMP(int(Math::ceil(rect.size.y / cell)) + 1, 2, 512);
	LocalVector<float> ages;
	ages.resize(nx * ny);
	bool any = false;
	for (int z = 0; z < ny; z++) {
		for (int x = 0; x < nx; x++) {
			const Vector2 xz = rect.position + rect.size * Vector2(float(x) / (nx - 1), float(z) / (ny - 1));
			const float a = fire->burn_age_at(Vector3(xz.x, 0.0f, xz.y));
			ages[z * nx + x] = a;
			any = any || a >= 0.0f;
		}
	}
	if (any) {
		burn_ages = ages;
		burn_nx = nx;
		burn_ny = ny;
		burn_rect = rect;
	}
}

// The sward's cover on burnt ground: 0 on a fresh burn, 1 once burn_recovery years have passed
// (PlantSpecies.burn_cover: slow at first, then closing in). Bilinear between the grid's ages,
// with unburnt cells counted as long recovered, so the scar's edge grows in over a cell.
float GrassField3D::_burn_cover(const PlantSpecies *p_rules, float p_x, float p_z) const {
	float sand = 1.0f;
	if (!sea_depth.is_empty() && !(p_rules && (p_rules->uses_dune() || p_rules->uses_coast() || p_rules->uses_sand_mobility()))) {
		const float gx = (p_x - sea_rect.position.x) / sea_rect.size.x * (sea_nx - 1);
		const float gz = (p_z - sea_rect.position.y) / sea_rect.size.y * (sea_ny - 1);
		if (gx >= 0.0f && gz >= 0.0f && gx <= sea_nx - 1 && gz <= sea_ny - 1) {
			const int ix = MIN(int(gx), sea_nx - 2), iz = MIN(int(gz), sea_ny - 2);
			const float fx = gx - ix, fz = gz - iz;
			const float *r0 = &sea_depth[iz * sea_nx + ix];
			const float *r1 = r0 + sea_nx;
			const float d = Math::lerp(Math::lerp(r0[0], r0[1], fx), Math::lerp(r1[0], r1[1], fx), fz);
			sand = 1.0f - Math::smoothstep(0.05f, 0.3f, d);
			if (p_rules && p_rules->uses_spring()) {
				// An oasis sward (rules with PlantSpecies.spring_range): only round the springs, by its own range.
				sand = p_rules->rule_weight(PlantSpecies::RULE_SPRING, springs.at(Vector2(p_x, p_z))) * (1.0f - Math::smoothstep(1.0f, 3.0f, d));
			} else if (sand < 1.0f && !springs.is_empty()) {
				// An oasis's damp ground holds a sward on the sand.
				// (Damp ground in the corridor, not up the dunes standing in it: thinning out from 1 to 3 m of sand.)
				sand = MAX(sand, Math::smoothstep(0.5f, 0.85f, springs.at(Vector2(p_x, p_z))) * (1.0f - Math::smoothstep(1.0f, 3.0f, d)));
			}
		}
	}
	if (p_rules && p_rules->uses_spring() && sea_depth.is_empty()) {
		sand = 0.0f; // An oasis sward with no sand sea to hold springs.
	}
	if (burn_ages.is_empty() || sand <= 0.0f) {
		return sand;
	}
	const float rec = (p_rules && p_rules->get_burn_recovery() > 0.0f) ? p_rules->get_burn_recovery() : burn_recovery;
	if (rec <= 0.0f) {
		return sand;
	}
	const float gx = (p_x - burn_rect.position.x) / burn_rect.size.x * (burn_nx - 1);
	const float gz = (p_z - burn_rect.position.y) / burn_rect.size.y * (burn_ny - 1);
	if (gx < 0.0f || gz < 0.0f || gx > burn_nx - 1 || gz > burn_ny - 1) {
		return sand;
	}
	const int x0 = MIN(int(gx), burn_nx - 2);
	const int z0 = MIN(int(gz), burn_ny - 2);
	const float fx = gx - x0;
	const float fz = gz - z0;
	auto c = [&](int x, int z) {
		const float a = burn_ages[z * burn_nx + x];
		return a < 0.0f ? 1.0f : Math::smoothstep(0.0f, rec, a);
	};
	return sand * Math::lerp(Math::lerp(c(x0, z0), c(x0 + 1, z0), fx), Math::lerp(c(x0, z0 + 1), c(x0 + 1, z0 + 1), fx), fz);
}

float GrassField3D::get_burn_cover_at(const Vector3 &p_global_position) const {
	return _burn_cover(rules.ptr(), p_global_position.x, p_global_position.z);
}

void GrassField3D::set_far_shell_range(float p_range) {
	far_shell_range = MAX(p_range, 0.0f);
	generation++;
}

// The outer band: blades thin out from 60% of the radius and are all gone just inside it (the
// blade shader's fade_band), and the ground under the grass turns to the blades' colour over the
// same band (grass_ground_fade, grass_ground_far), so the field ends without an edge.
GrassField3D *GrassField3D::fade_owner = nullptr;
GrassField3D *GrassField3D::under_owner = nullptr;

void GrassField3D::_update_fade(const Vector2 &p_center) {
	const Vector4 band(p_center.x, p_center.y, radius * 0.97f, radius * 0.37f);
	if (band.is_equal_approx(fade_band)) {
		return;
	}
	fade_band = band;
	if (material.is_valid()) {
		material->set_shader_parameter("fade_band", band);
	}
	if (crop != CROP_NONE) {
		// A crop's ground is its own (crop_ground()): the meadow keeps the grass ground's band.
		if (is_inside_tree()) {
			crop_view_published = Vector4(); // Published with the eye's height in update_at().
			crop_plots_dirty = true;
		}
		return;
	}
	// The ground's band is global: the widest field publishes it, so a near undercoat field does
	// not pull the ground's blade colour in to its own short radius.
	if (fade_owner && fade_owner != this && fade_owner->radius > radius) {
		// A narrower field with its own ground tint (an undercoat): the ground past its band.
		if (ground_tint_enabled && is_inside_tree() && (!under_owner || under_owner == this || under_owner->radius <= radius)) {
			under_owner = this;
			_publish_under();
		}
		return;
	}
	if (under_owner == this) {
		_drop_under(); // Now the widest: its band is the main one.
	}
	// The widest field even without a ground tint of its own: its blades are the ones seen last.
	if (is_inside_tree()) {
		GrassField3D *prev = fade_owner;
		fade_owner = this;
		// Taking over from a narrower field with a ground tint: it is the undercoat now.
		if (prev && prev != this && prev->ground_tint_enabled && prev->is_inside_tree() && (!under_owner || under_owner->radius <= prev->radius)) {
			under_owner = prev;
		}
		RenderingServer *rs = RenderingServer::get_singleton();
		rs->global_shader_parameter_set("grass_ground_fade", band);
		_publish_far();
	}
}

// The blades seen from a distance (linear), as the far ground's albedo: measured from a render of
// the blades once that has run (_far_bake_start()), the hand-matched blend before it and without a
// renderer.
Color GrassField3D::get_far_color() const {
	return far_baked ? far_color_baked : _far_color_hand();
}

// Mostly the upper blade, some dry, darkened by the shade the sward casts on itself (matched to the
// field's mean colour in the grass bench).
Color GrassField3D::_far_color_hand() const {
	const Color b = base_color.srgb_to_linear();
	const Color t = tip_color.srgb_to_linear();
	const Color d = dry_color.srgb_to_linear();
	// A dormant sward (dryness towards 1: drought or winter) is dead blades over its few green ones, so the dry share
	// rises to 0.9 there; a living sward's dryness is only its dry tips.
	const float dry = _dryness();
	const Color far = b.lerp(t, 0.65f).lerp(d, MIN(dry * 0.35f + 0.55f * Math::smoothstep(0.4f, 1.0f, dry), 0.9f)) * 0.355f;
	return Color(far.r, far.g, far.b, 1.0f);
}

// Far colour bake. Impostor baking (Brucks 2018) for a whole sward: what the raster draws, so shape,
// lean, colour along the blade, shadows inside the sward and the ground between blades all count.
// A canopy-reflectance model tracing the blades on the CPU (Raytran, Govaerts and Verstraete 1998)
// would have to copy the raster's lighting and drift when the blade shader changes.
namespace {
constexpr float FAR_BAKE_VIEW_DEG = 7.0f; // The view a far meadow is seen in (eye 1.7 m, 14 m out).
constexpr float FAR_BAKE_SUN_DEG = 30.0f; // The reference elevation (get_far_color(), the cover map).
// The elevations the sward is baked at (the reference among them): the far colour follows the sun
// between them (bench: a 63 degree sun lit the far ground 1.55 times as much against the blades as a
// 30 degree one).
constexpr float FAR_BAKE_ELEVATIONS_DEG[4] = { 12.0f, 30.0f, 50.0f, 72.0f };
constexpr float FAR_BAKE_ORTHO = 2.0f; // Metres across the image.
constexpr float FAR_BAKE_CAM_DIST = 20.0f;
constexpr int FAR_BAKE_SIZE = 96;
constexpr int FAR_BAKE_AZIMUTHS = 4; // Sun behind, beside and facing the view: their mean.
constexpr int FAR_BAKE_FRAMES = 3; // Frames each view renders before it is read (shadows, sky).
} // namespace

void GrassField3D::set_far_bake(bool p_enabled) {
	if (far_bake == p_enabled) {
		return;
	}
	far_bake = p_enabled;
	if (!far_bake) {
		_far_bake_free(); // Stays queued: turned on again, it bakes.
	}
}

void GrassField3D::_queue_far_bake() {
	far_bake_queued = true;
	_far_bake_free(); // A bake under way is of the old look: start again.
}

void GrassField3D::_far_bake_free() {
	if (far_bake_vp) {
		far_bake_vp->queue_free();
	}
	far_bake_vp = nullptr;
	far_bake_sun = nullptr;
	far_bake_blades = nullptr;
	far_bake_ground_mat.unref();
	far_bake_state = -1;
}

void GrassField3D::_far_bake_start() {
	far_bake_queued = false;
	// The raster renderer only: headless and the compatibility renderer keep the hand-matched colour.
	if (!is_inside_tree() || !RenderingServer::get_singleton()->get_rendering_device()) {
		if (fade_owner == this && is_inside_tree()) {
			_publish_far(); // The hand-matched colour, for the look as it is now.
		}
		return;
	}
	_ensure_resources();
	const float e = Math::deg_to_rad(FAR_BAKE_VIEW_DEG);
	const float hmax = crop != CROP_NONE ? _crop_shape(crop).height * 1.15f + _crop_shape(crop).ear_length : MAX(blade_height.x, blade_height.y) * 1.1f;
	// The patch every view ray crosses from the top of the sward to the ground (view along -z).
	const float half_w = FAR_BAKE_ORTHO * 0.5f + hmax + 0.5f;
	const float z_lo = -FAR_BAKE_ORTHO * 0.5f / Math::sin(e) - hmax - 0.5f;
	const float z_hi = FAR_BAKE_ORTHO * 0.5f / Math::sin(e) + hmax / Math::tan(e) + hmax + 0.5f;

	// The blades as _build_tile() makes them, on open flat ground (light 1), every one kept.
	const float cell = 1.0f / Math::sqrt(MAX(density, 0.01f));
	const int ix0 = int(Math::floor(-half_w / cell));
	const int ix1 = int(Math::ceil(half_w / cell));
	const int iz0 = int(Math::floor(z_lo / cell));
	const int iz1 = int(Math::ceil(z_hi / cell));
	LocalVector<Transform3D> xforms;
	LocalVector<Color> customs;
	if (crop != CROP_NONE) {
		// The tillers as _build_tile() makes them.
		const float crop_h = _crop_shape(crop).height;
		_crop_rows(Rect2(-half_w, z_lo, 2.0f * half_w, z_hi - z_lo), 1.0f, [&](float x, float z, float rank, uint32_t hb, float yaw, float splay, int plot) {
			xforms.push_back(Transform3D(Basis(Vector3(0, 1, 0), yaw), Vector3(x, 0.0f, z)));
			const float order = Math::floor(Math::fmod(rank * 8.0f, 1.0f) * 255.0f);
			customs.push_back(Color(crop_h * (0.9f + 0.2f * unit(hb, 0)), plot * 4.0f + 0.85f + 0.3f * unit(hb, 8), VegetationLight::pack(1.0f) * 4.0f + splay, order + 0.999f * unit(hb, 16)));
		}, false);
	}
	for (int iz = iz0; iz < (crop != CROP_NONE ? iz0 : iz1); iz++) {
		for (int ix = ix0; ix < ix1; ix++) {
			const uint32_t hb = cell_hash(ix, iz, 0x9E3779B9u);
			const float rank = blue_rank(ix, iz);
			const uint32_t hj = cell_hash(ix, iz, 0x85EBCA6Bu);
			const float x = (ix + unit(hj, 0)) * cell;
			const float z = (iz + unit(hj, 8)) * cell;
			const uint32_t hc = clump_hash(x, z, clump_size);
			const float rnd = unit(hb, 16);
			const float height = Math::lerp(float(blade_height.x), float(blade_height.y), 0.65f * unit(hc, 0) + 0.35f * unit(hj, 16));
			const float yaw = unit(hc, 8) * Math::TAU + (unit(hj, 24) - 0.5f) * 1.6f;
			xforms.push_back(Transform3D(Basis(Vector3(0, 1, 0), yaw), Vector3(x, 0.0f, z)));
			const float order = Math::floor(Math::fmod(rank * 8.0f, 1.0f) * 255.0f);
			const float blade_lean = MIN(lean * (0.4f + 1.2f * unit(hc, 16)), 3.99f);
			customs.push_back(Color(height, blade_width, VegetationLight::pack(1.0f) * 4.0f + blade_lean, order + 0.999f * Math::fmod(rnd + 0.37f * unit(hc, 24), 1.0f)));
		}
	}
	Ref<MultiMesh> mm;
	mm.instantiate();
	mm->set_transform_format(MultiMesh::TRANSFORM_3D);
	mm->set_use_custom_data(true);
	mm->set_mesh(blade_mesh);
	mm->set_instance_count(xforms.size());
	for (uint32_t i = 0; i < xforms.size(); i++) {
		mm->set_instance_transform(i, xforms[i]);
		mm->set_instance_custom_data(i, customs[i]);
	}
	// The field's own material, without the outer band or trample.
	Ref<ShaderMaterial> mat = material->duplicate();
	mat->set_shader_parameter("fade_band", Vector4());
	mat->set_shader_parameter("trample_rect", Vector4());

	SubViewport *vp = memnew(SubViewport);
	vp->set_name("FarColourBake");
	vp->set_size(Size2i(FAR_BAKE_SIZE, FAR_BAKE_SIZE));
	vp->set_use_own_world_3d(true);
	vp->set_use_hdr_2d(true); // Linear and unclamped.
	vp->set_update_mode(SubViewport::UPDATE_ALWAYS);
	add_child(vp, false, INTERNAL_MODE_BACK);

	// The scene's own sky light and sun strength: the blades shadow each other from the sun but not
	// from the sky (the raster has no ambient occlusion inside the sward), so the colour they need
	// on the far ground depends on how the light splits between the two. A stock sky under a
	// brighter sun and a weather sky made the far ground too light (a lime band where the blades
	// end). The sky material is shared, so the bake's sky follows the bake's own sun.
	Ref<Environment> scene_env;
	if (get_world_3d().is_valid()) {
		scene_env = get_world_3d()->get_environment();
		if (scene_env.is_null()) {
			scene_env = get_world_3d()->get_fallback_environment();
		}
	}
	Ref<Environment> env;
	env.instantiate();
	Ref<Sky> sky;
	sky.instantiate();
	Ref<Material> sky_mat;
	if (scene_env.is_valid() && scene_env->get_sky().is_valid()) {
		sky_mat = scene_env->get_sky()->get_material();
		sky->set_radiance_size(scene_env->get_sky()->get_radiance_size());
	}
	if (sky_mat.is_null()) {
		sky_mat = Ref<Material>(memnew(ProceduralSkyMaterial));
	}
	sky->set_material(sky_mat);
	sky->set_process_mode(Sky::PROCESS_MODE_QUALITY);
	env->set_background(Environment::BG_SKY);
	env->set_sky(sky);
	env->set_ambient_source(Environment::AMBIENT_SOURCE_SKY);
	if (scene_env.is_valid()) {
		env->set_sky_rotation(scene_env->get_sky_rotation());
		env->set_ambient_source(scene_env->get_ambient_source());
		env->set_ambient_light_color(scene_env->get_ambient_light_color());
		env->set_ambient_light_energy(scene_env->get_ambient_light_energy());
		env->set_ambient_light_sky_contribution(scene_env->get_ambient_light_sky_contribution());
		if (scene_env->get_background() != Environment::BG_SKY) {
			env->set_background(scene_env->get_background());
			env->set_bg_color(scene_env->get_bg_color());
			env->set_bg_energy_multiplier(scene_env->get_bg_energy_multiplier());
		}
	}
	env->set_tonemapper(Environment::TONE_MAPPER_LINEAR);
	WorldEnvironment *we = memnew(WorldEnvironment);
	we->set_environment(env);
	vp->add_child(we);

	MultiMeshInstance3D *mmi = memnew(MultiMeshInstance3D);
	mmi->set_multimesh(mm);
	mmi->set_material_override(mat);
	vp->add_child(mmi);

	far_bake_ground_mat.instantiate();
	MeshInstance3D *ground = memnew(MeshInstance3D);
	Ref<PlaneMesh> plane;
	plane.instantiate();
	plane->set_size(Vector2(2.0f * half_w + 4.0f, z_hi - z_lo + 4.0f));
	plane->set_material(far_bake_ground_mat);
	ground->set_mesh(plane);
	ground->set_position(Vector3(0.0f, 0.0f, 0.5f * (z_lo + z_hi)));
	vp->add_child(ground);

	DirectionalLight3D *sun = memnew(DirectionalLight3D);
	far_bake_sun_energy = 1.0f;
	if (DirectionalLight3D *scene_sun = _get_sun()) {
		far_bake_sun_energy = MAX(scene_sun->get_param(Light3D::PARAM_ENERGY), 0.01f);
		sun->set_param(Light3D::PARAM_ENERGY, far_bake_sun_energy);
		sun->set_color(scene_sun->get_color());
	}
	sun->set_shadow(true);
	sun->set_shadow_mode(DirectionalLight3D::SHADOW_ORTHOGONAL);
	sun->set_param(Light3D::PARAM_SHADOW_MAX_DISTANCE, FAR_BAKE_CAM_DIST + z_hi);
	vp->add_child(sun);

	Camera3D *cam = memnew(Camera3D);
	const Vector3 target(0.0f, 0.5f * hmax, 0.0f);
	const Vector3 dir(0.0f, -Math::sin(e), -Math::cos(e));
	const Vector3 eye = target - dir * FAR_BAKE_CAM_DIST;
	cam->set_transform(Transform3D(Basis::looking_at(dir), eye));
	cam->set_orthogonal(FAR_BAKE_ORTHO, 0.5f, 2.0f * FAR_BAKE_CAM_DIST + z_hi);
	vp->add_child(cam);
	cam->set_current(true);

	far_bake_vp = vp;
	far_bake_sun = sun;
	far_bake_blades = mmi;
	far_bake_state = 0;
	far_bake_wait = 0;
	_far_bake_step(); // Sets up the first view.
}

// One process frame of the bake. Views 0 .. FAR_BAKE_AZIMUTHS - 1: the sward with the sun turned
// round the view; then the bare far ground's material (roughness 0.95, specular 0.1, as
// grass_ground_specular() leaves it) in white, then in black at each azimuth. The far ground shows
// its albedo times the white's diffuse light plus the black's sheen, so the albedo that matches the
// blades is (blades - black) / (white - black): at the low angles a far meadow is seen at, the
// sheen is up to half of what the blades send, and it turns with the sun as the blades do. Each
// view renders FAR_BAKE_FRAMES frames, then is read.
void GrassField3D::_far_bake_step() {
	constexpr int WHITE = FAR_BAKE_AZIMUTHS;
	constexpr int PER_ELEVATION = 2 * FAR_BAKE_AZIMUTHS + 1;
	constexpr int VIEWS = PER_ELEVATION * FAR_ELEVATIONS;
	static_assert(sizeof(FAR_BAKE_ELEVATIONS_DEG) / sizeof(float) == FAR_ELEVATIONS);
	if (far_bake_wait == 0) {
		const int sub = far_bake_state % PER_ELEVATION;
		const float elev = FAR_BAKE_ELEVATIONS_DEG[far_bake_state / PER_ELEVATION];
		const bool bare = sub >= WHITE;
		const int azimuth = sub < WHITE ? sub : (sub == WHITE ? 0 : sub - WHITE - 1);
		const Color gt = ground_tint_enabled ? Color(ground_tint.r, ground_tint.g, ground_tint.b) : Color(0.11f, 0.12f, 0.06f);
		far_bake_ground_mat->set_albedo(bare ? (sub == WHITE ? Color(1, 1, 1) : Color(0, 0, 0)) : gt);
		far_bake_ground_mat->set_roughness(0.95f);
		far_bake_ground_mat->set_specular(0.1f);
		far_bake_blades->set_visible(!bare);
		far_bake_sun->set_rotation_degrees(Vector3(-elev, 360.0f / FAR_BAKE_AZIMUTHS * azimuth, 0.0f));
	}
	if (++far_bake_wait <= FAR_BAKE_FRAMES + (far_bake_state == 0 ? 2 : 0)) {
		return;
	}
	far_bake_wait = 0;
	Ref<Image> img = far_bake_vp->get_texture()->get_image();
	if (img.is_null() || img->is_empty()) {
		_far_bake_free();
		return;
	}
	if (img->get_format() == Image::FORMAT_RGBA8 || img->get_format() == Image::FORMAT_RGB8) {
		img->srgb_to_linear(); // An sRGB target (no HDR 2D): back to linear.
	}
	Color sum(0, 0, 0, 0);
	for (int y = 0; y < img->get_height(); y++) {
		for (int x = 0; x < img->get_width(); x++) {
			sum += img->get_pixel(x, y);
		}
	}
	far_bake_view[far_bake_state] = sum / float(img->get_width() * img->get_height());
	if (++far_bake_state < VIEWS) {
		return;
	}
	_far_bake_free();
	// Per elevation: the albedo each azimuth needs. The white's diffuse light does not turn with the
	// sun (the ground is Lambertian under its sheen), so one white view serves all four. The sheen
	// never takes more than three quarters of the blades' light (a dark sward seen very low).
	int baked = 0;
	for (int e = 0; e < FAR_ELEVATIONS; e++) {
		const Color *views = far_bake_view + e * PER_ELEVATION;
		const Color diffuse = views[WHITE] - views[WHITE + 1];
		if (diffuse.r <= 1e-4f || diffuse.g <= 1e-4f || diffuse.b <= 1e-4f) {
			break;
		}
		Color need[FAR_BAKE_AZIMUTHS];
		Color mean(0, 0, 0, 0);
		for (int k = 0; k < FAR_BAKE_AZIMUTHS; k++) {
			const Color &b = views[k];
			const Color &black = views[WHITE + 1 + k];
			for (int c = 0; c < 3; c++) {
				need[k][c] = MAX(b[c] - black[c], 0.25f * b[c]) / diffuse[c];
			}
			mean += need[k] / float(FAR_BAKE_AZIMUTHS);
		}
		if (!Math::is_finite(mean.r) || !Math::is_finite(mean.g) || !Math::is_finite(mean.b) || mean.r <= 0.0f || mean.g <= 0.0f || mean.b <= 0.0f) {
			break;
		}
		// The azimuths are 0 (sun behind the view, the hot spot), 90, 180 (into the sun) and 270,
		// so c(phi) = a0 + a1 cos(phi) + a2 cos(2 phi) through them: a1 = (front - back) / 2,
		// a2 = (front + back - sides) / 4. Bounded so the factor stays between 0.1 and 1.9.
		Color l1c, l2c;
		for (int c = 0; c < 3; c++) {
			float l1 = 0.5f * (need[0][c] - need[2][c]) / mean[c];
			float l2 = 0.25f * (need[0][c] + need[2][c] - need[1][c] - need[3][c]) / mean[c];
			const float total = Math::abs(l1) + Math::abs(l2);
			if (total > 0.9f) {
				l1 *= 0.9f / total;
				l2 *= 0.9f / total;
			}
			l1c[c] = l1;
			l2c[c] = l2;
		}
		l1c.a = l2c.a = 0.0f;
		far_elev_color[e] = Color(CLAMP(mean.r, 0.0f, 1.0f), CLAMP(mean.g, 0.0f, 1.0f), CLAMP(mean.b, 0.0f, 1.0f), 1.0f);
		far_elev_lobe1[e] = l1c;
		far_elev_lobe2[e] = l2c;
		baked++;
	}
	if (baked < FAR_ELEVATIONS) {
		return;
	}
	far_elev_count = baked;
	far_color_baked = far_elev_color[FAR_REF_ELEVATION];
	far_lobe1 = far_elev_lobe1[FAR_REF_ELEVATION];
	far_lobe2 = far_elev_lobe2[FAR_REF_ELEVATION];
	published_far_elevation = -1000.0f;
	far_baked = true;
	print_verbose(vformat("GrassField3D \"%s\": far colour %s (hand-matched %s), sun azimuth lobes %s %s; by sun elevation 12/30/50/72 degrees %s %s %s %s.", get_name(), far_color_baked, _far_color_hand(), far_lobe1, far_lobe2, far_elev_color[0], far_elev_color[1], far_elev_color[2], far_elev_color[3]));
	if (fade_owner == this || _is_crop_view()) {
		if (fade_owner == this) {
			_publish_far();
		}
		far_view_round = 0;
		far_view_misses = 0;
		_far_view_start(20);
	} else if (under_owner == this) {
		_publish_under();
	}
	emit_signal(SNAME("far_color_changed"));
}

// The fields' far ground turns with the sun as the tillers do: the first crop node's bake of its
// tillers gives the azimuth lobes (hot spot down-sun, shaded sides into it) at the sun's elevation,
// applied to every field's canopy colour (the canopy's shape changes less with the stage than its
// colour). Without a bake, or with the sun down, none.
void GrassField3D::_publish_crop_sun() {
	Vector4 sun_v;
	Color l1, l2;
	DirectionalLight3D *sun = _get_sun();
	if (far_baked && sun && sun->is_visible_in_tree() && sun->get_param(Light3D::PARAM_ENERGY) > 0.0f) {
		const Vector3 d = -sun->get_global_transform().basis.get_column(Vector3::AXIS_Z).normalized();
		const float h = Vector2(d.x, d.z).length();
		const float up = -d.y;
		if (h > 1e-4f) {
			const float horizon = CLAMP((up + 0.05f) / 0.1f, 0.0f, 1.0f);
			const float strength = far_elev_count == FAR_ELEVATIONS ? horizon : MIN(h / Math::cos(Math::deg_to_rad(FAR_BAKE_SUN_DEG)), 1.15f) * horizon;
			sun_v = Vector4(d.x / h, d.z / h, strength, 0.0f);
			Color col;
			_far_at_elevation(Math::rad_to_deg(Math::asin(CLAMP(up, -1.0f, 1.0f))), col, l1, l2);
		}
	}
	if (sun_v.is_equal_approx(crop_sun_published) && l1.is_equal_approx(crop_lobe1_published) && l2.is_equal_approx(crop_lobe2_published)) {
		return;
	}
	crop_sun_published = sun_v;
	crop_lobe1_published = l1;
	crop_lobe2_published = l2;
	RenderingServer *rs = RenderingServer::get_singleton();
	rs->global_shader_parameter_set("crop_ground_sun", sun_v);
	rs->global_shader_parameter_set("crop_ground_lobe1", Vector4(l1.r, l1.g, l1.b, 0.0f));
	rs->global_shader_parameter_set("crop_ground_lobe2", Vector4(l2.r, l2.g, l2.b, 0.0f));
}

// The sward over the ground for WorldGround.albedo_at (the light it sends up into gas and haze): the blades' colour
// seen from a distance (get_far_color, as the far ground draws it) over the share the ground map gives the sward there
// (its rules and worn paths), bilinear over the map's texels; -1 past the map.
Color GrassField3D::contract_plant_albedo_at(const Vector3 &p_position) const {
	if (ground_image.is_null() || ground_image_rect.z <= 0.0f || ground_image_rect.w <= 0.0f || !ground_tint_enabled) {
		return Color(0, 0, 0, -1.0f);
	}
	const Vector2 uv((p_position.x - ground_image_rect.x) / ground_image_rect.z, (p_position.z - ground_image_rect.y) / ground_image_rect.w);
	if (uv.x < 0.0f || uv.y < 0.0f || uv.x > 1.0f || uv.y > 1.0f) {
		return Color(0, 0, 0, -1.0f);
	}
	const int w = ground_image->get_width();
	const int h = ground_image->get_height();
	const float fx = CLAMP(uv.x * w - 0.5f, 0.0f, float(w - 1));
	const float fz = CLAMP(uv.y * h - 0.5f, 0.0f, float(h - 1));
	const int x0 = int(fx);
	const int z0 = int(fz);
	const int x1 = MIN(x0 + 1, w - 1);
	const int z1 = MIN(z0 + 1, h - 1);
	const float tx = fx - x0;
	const float tz = fz - z0;
	const float cover = Math::lerp(Math::lerp(ground_image->get_pixel(x0, z0).r, ground_image->get_pixel(x1, z0).r, tx), Math::lerp(ground_image->get_pixel(x0, z1).r, ground_image->get_pixel(x1, z1).r, tx), tz);
	Color c = get_far_color();
	c.a = CLAMP(cover, 0.0f, 1.0f);
	return c;
}

// The far colour and its azimuth lobes, for grass_ground(). The fade owner's only.
// At the sun's elevation, between the baked ones (the lowest and highest held beyond them).
void GrassField3D::_far_at_elevation(float p_deg, Color &r_color, Color &r_lobe1, Color &r_lobe2) const {
	if (far_elev_count < FAR_ELEVATIONS) {
		r_color = far_color_baked;
		r_lobe1 = far_lobe1;
		r_lobe2 = far_lobe2;
		return;
	}
	int i = 0;
	while (i < FAR_ELEVATIONS - 2 && p_deg > FAR_BAKE_ELEVATIONS_DEG[i + 1]) {
		i++;
	}
	const float t = CLAMP((p_deg - FAR_BAKE_ELEVATIONS_DEG[i]) / (FAR_BAKE_ELEVATIONS_DEG[i + 1] - FAR_BAKE_ELEVATIONS_DEG[i]), 0.0f, 1.0f);
	r_color = far_elev_color[i].lerp(far_elev_color[i + 1], t);
	r_lobe1 = far_elev_lobe1[i].lerp(far_elev_lobe1[i + 1], t);
	r_lobe2 = far_elev_lobe2[i].lerp(far_elev_lobe2[i + 1], t);
}

// The far colour at the sun's elevation, its azimuth lobes and its ratio to get_far_color() (the
// cover map's, at 30 degrees: grass_ground_far_scale, for the terrain's far cover). The fade owner's
// only.
void GrassField3D::_publish_far() {
	RenderingServer *rs = RenderingServer::get_singleton();
	Color far = get_far_color();
	Color l1, l2, scale(1, 1, 1, 1);
	if (far_baked) {
		_far_at_elevation(sun_elevation_deg, far, l1, l2);
		const Color ref = far_color_baked;
		scale = Color(ref.r > 0.0f ? far.r / ref.r : 1.0f, ref.g > 0.0f ? far.g / ref.g : 1.0f, ref.b > 0.0f ? far.b / ref.b : 1.0f, 1.0f);
	}
	// The live-view match, on both: the cover map's colour reaches the terrain through the scale.
	far *= far_view_scale;
	scale *= far_view_scale;
	far.a = 1.0f;
	scale.a = 1.0f;
	published_far_elevation = sun_elevation_deg;
	// a: the share of the ground this sward covers (its patches), the rest the undercoat's.
	rs->global_shader_parameter_set("grass_ground_far", Vector4(far.r, far.g, far.b, _cover_share()));
	rs->global_shader_parameter_set("grass_ground_far_lobe1", Vector4(l1.r, l1.g, l1.b, 0.0f));
	rs->global_shader_parameter_set("grass_ground_far_lobe2", Vector4(l2.r, l2.g, l2.b, 0.0f));
	rs->global_shader_parameter_set("grass_ground_far_scale", Vector4(scale.r, scale.g, scale.b, 1.0f));
	if (under_owner && under_owner != this) {
		under_owner->_publish_under(); // At the same sun elevation.
	}
}

// The share of the ground the blades stand on: the patch cover when the rules grow it in patches.
float GrassField3D::_cover_share() const {
	if (rules.is_valid() && rules->get_patch_scale() > 0.0f) {
		return CLAMP(rules->get_patch_cover(), 0.0f, 1.0f);
	}
	return 1.0f;
}

// The undercoat's band and far colour (at the main field's sun elevation), for grass_ground().
void GrassField3D::_publish_under() {
	RenderingServer *rs = RenderingServer::get_singleton();
	Color far = get_far_color();
	if (far_baked) {
		Color l1, l2;
		_far_at_elevation(fade_owner ? fade_owner->sun_elevation_deg : sun_elevation_deg, far, l1, l2);
	}
	if (fade_owner) {
		far *= fade_owner->far_view_scale; // The main field's live-view match holds for its light.
		far.a = 1.0f;
	}
	rs->global_shader_parameter_set("grass_ground_under", fade_band);
	rs->global_shader_parameter_set("grass_ground_under_far", Vector4(far.r, far.g, far.b, 1.0f));
}

void GrassField3D::_drop_under() {
	if (under_owner != this) {
		return;
	}
	under_owner = nullptr;
	if (RenderingServer::get_singleton()) {
		RenderingServer::get_singleton()->global_shader_parameter_set("grass_ground_under", Vector4());
		RenderingServer::get_singleton()->global_shader_parameter_set("grass_ground_under_far", Vector4());
	}
}

Dictionary GrassField3D::get_far_published() const {
	Dictionary d;
	Color far = get_far_color();
	Color l1, l2, scale(1, 1, 1, 1);
	if (far_baked) {
		_far_at_elevation(sun_elevation_deg, far, l1, l2);
		const Color ref = far_color_baked;
		scale = Color(ref.r > 0.0f ? far.r / ref.r : 1.0f, ref.g > 0.0f ? far.g / ref.g : 1.0f, ref.b > 0.0f ? far.b / ref.b : 1.0f, 1.0f);
	}
	d["far"] = far;
	d["lobe1"] = l1;
	d["lobe2"] = l2;
	d["scale"] = scale;
	d["sun"] = published_sun;
	d["sun_elevation"] = sun_elevation_deg;
	d["cover_share"] = _cover_share();
	d["baked"] = far_baked;
	d["view_scale"] = far_view_scale;
	d["view_ratio"] = far_view_ratio;
	d["view_pairs"] = far_view_pairs;
	d["view_round"] = far_view_round;
	d["view_state"] = far_view_state;
	d["view_msec"] = far_view_msec;
	return d;
}

// Live-view match (docs/ideas/vegetation-lod-streaming.md, "Live-view match"). The bake matches the
// blades on a plain plane under the scene's sky and sun; a scene can still light its terrain's far
// cover differently from the blades (seen as a lighter ring past the band), for reasons the bake
// does not model. This compares the two where the player sees them: pairs of ground points in the
// same direction from the camera, one where the blades stand full and one just past the band, read
// from the frame (asynchronously: the copy lands a frame or two later, without stalling the GPU),
// and scales the published far colour until they agree. A few rounds, again when the sun has moved
// far or the camera has gone a field's width.
void GrassField3D::set_far_match_view(bool p_enabled) {
	if (far_match_view == p_enabled) {
		return;
	}
	far_match_view = p_enabled;
	if (!p_enabled) {
		far_view_scale = Color(1, 1, 1, 1);
		far_view_round = 0;
		if (far_view_state == 1) {
			far_view_state = 0;
		}
	}
	if (fade_owner == this && is_inside_tree()) {
		_publish_far();
		if (p_enabled && far_baked) {
			_far_view_start(20);
		}
	}
}

void GrassField3D::_far_view_start(int p_wait) {
	if (!far_match_view || far_view_state == 2) {
		return;
	}
	far_view_state = 1;
	far_view_wait = p_wait;
}

void GrassField3D::_far_view_step(Camera3D *p_cam) {
	if (!far_match_view || !far_baked || far_bake_vp || far_view_state == 2) {
		return;
	}
	if (far_view_state == 0) {
		// Again once the sun has moved well away or the camera has gone a field's width.
		if (far_view_elevation > -999.0f && (Math::abs(sun_elevation_deg - far_view_elevation) > 8.0f || p_cam->get_global_position().distance_to(far_view_camera) > radius)) {
			far_view_round = 0;
			far_view_misses = 0;
			_far_view_start(20);
		}
		return;
	}
	if (--far_view_wait > 0) {
		return;
	}
	Viewport *vp = get_viewport();
	if (!vp || vp->get_texture().is_null()) {
		return;
	}
	const Vector2 size = vp->get_visible_rect().size;
	TerrainDataMap3D *map = Object::cast_to<TerrainDataMap3D>(get_node_or_null(data_map));
	const float base_y = get_global_position().y;
	const bool crop_view = fade_owner != this;
	const Vector3 eye = p_cam->get_global_position();
	const Vector2 c = crop_view ? Vector2(eye.x, eye.z) : Vector2(fade_band.x, fade_band.y);
	// A crop's tillers thin from 15% of the radius (coarser levels, their ground in the canopy's
	// colour) and are gone from 60% to 97%: compare at half the radius against just past it.
	const float r_full = crop_view ? radius * 0.5f : (fade_band.z - fade_band.w) * 0.92f; // Blades still all there.
	const float r_out = crop_view ? radius * 0.97f + MAX(3.0f, radius * 0.08f) : fade_band.z + MAX(3.0f, fade_band.z * 0.08f); // Only the far ground.
	const CropFieldMask fields = crop_view ? get_crop_field_mask() : CropFieldMask();
	const Vector3 fwd = -p_cam->get_global_transform().basis.get_column(Vector3::AXIS_Z);
	Vector2 f2(fwd.x, fwd.z);
	if (f2.length() < 0.05f) {
		const Vector3 up = p_cam->get_global_transform().basis.get_column(Vector3::AXIS_Y);
		f2 = Vector2(up.x, up.z); // Looking straight down: the top of the frame.
	}
	const float a0 = Math::atan2(f2.y, f2.x);
	far_view_px.clear();
	far_view_pair_slot.clear();
	// The field slot (crop_ground_plots' texel) a point is in, 0 for none.
	auto slot_of = [&](const Vector2 &p_xz) -> int {
		int slot = 1;
		for (const GrassField3D *g : crop_nodes) {
			for (const CropPlot &pl : g->plots) {
				if (Geometry2D::is_point_in_polygon(p_xz, pl.polygon)) {
					return slot;
				}
				slot++;
			}
		}
		return 0;
	};
	const int dirs = 24;
	for (int i = 0; i < dirs; i++) {
		const float a = a0 + (float(i) / float(dirs - 1) - 0.5f) * 1.6f;
		const Vector2 d(Math::cos(a), Math::sin(a));
		for (int k = 0; k < 2; k++) {
			Vector2 px[2];
			bool ok = true;
			for (int j = 0; j < 2 && ok; j++) {
				const float r = j == 0 ? r_full * (1.0f - 0.15f * float(k)) : r_out + 0.06f * fade_band.z * float(k);
				const Vector2 xz = c + d * r;
				Vector3 p(xz.x, base_y, xz.y);
				if (map) {
					p.y = map->get_height_at(p);
				}
				if (p_cam->is_position_behind(p)) {
					ok = false;
					break;
				}
				px[j] = p_cam->unproject_position(p);
				ok = px[j].x >= 4.0f && px[j].y >= 4.0f && px[j].x < size.x - 4.0f && px[j].y < size.y - 4.0f;
			}
			if (ok && crop_view) {
				// Both in the same field, so the canopy is compared with itself.
				const Vector2 q0 = c + d * (r_full * (1.0f - 0.15f * float(k)));
				const Vector2 q1 = c + d * (r_out + 0.06f * radius * float(k));
				bool same = false;
				for (const CropFieldMask::Item &it : fields.items) {
					if (it.bounds.has_point(q0) && it.bounds.has_point(q1) && Geometry2D::is_point_in_polygon(q0, it.polygon) && Geometry2D::is_point_in_polygon(q1, it.polygon)) {
						same = true;
						break;
					}
				}
				ok = same;
			}
			if (ok && px[0].distance_to(px[1]) > 6.0f) {
				far_view_px.push_back(px[0]);
				far_view_px.push_back(px[1]);
				far_view_pair_slot.push_back(crop_view ? slot_of(c + d * (r_out + 0.06f * radius * float(k))) : 0);
			}
		}
	}
	far_view_camera = p_cam->get_global_position();
	far_view_size = size;
	if (far_view_px.size() < 24) {
		_far_view_apply(Color(1, 1, 1, 1), far_view_px.size() / 2); // Not enough of the band in view.
		return;
	}
	far_view_state = 2;
	RenderingServer::get_singleton()->call_on_render_thread(callable_mp(this, &GrassField3D::_far_view_issue).bind(vp->get_texture()->get_rid()));
}

// On the render thread: copy the frame as it stands (the last one drawn) and hand it back later.
void GrassField3D::_far_view_issue(RID p_viewport_texture) {
	RenderingServer *rs = RenderingServer::get_singleton();
	RenderingDevice *rd = rs->get_rendering_device();
	const RID tex = rd ? rs->texture_get_rd_texture(p_viewport_texture) : RID();
	if (!tex.is_valid() || !rd->texture_is_valid(tex)) {
		callable_mp(this, &GrassField3D::_far_view_apply).call_deferred(Color(1, 1, 1, 1), 0);
		return;
	}
	const RD::TextureFormat tf = rd->texture_get_format(tex);
	const Error err = rd->texture_get_data_async(tex, 0, callable_mp(this, &GrassField3D::_far_view_on_data).bind(int(tf.width), int(tf.height), int(tf.format)));
	if (err != OK) {
		callable_mp(this, &GrassField3D::_far_view_apply).call_deferred(Color(1, 1, 1, 1), 0);
	}
}

// When the copy lands: the median over the pairs of (blades)/(far ground), per channel, in linear.
void GrassField3D::_far_view_on_data(const PackedByteArray &p_data, int p_width, int p_height, int p_format) {
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	int bpp = 0;
	bool half = false, bgr = false, srgb = true;
	switch (p_format) {
		case RD::DATA_FORMAT_R8G8B8A8_UNORM:
		case RD::DATA_FORMAT_R8G8B8A8_SRGB:
			bpp = 4;
			break;
		case RD::DATA_FORMAT_B8G8R8A8_UNORM:
		case RD::DATA_FORMAT_B8G8R8A8_SRGB:
			bpp = 4;
			bgr = true;
			break;
		case RD::DATA_FORMAT_R16G16B16A16_SFLOAT:
			bpp = 8;
			half = true;
			srgb = false; // HDR 2D: linear already.
			break;
		default:
			break;
	}
	if (bpp == 0 || p_width <= 0 || p_height <= 0 || p_data.size() < int64_t(p_width) * p_height * bpp || far_view_size.x <= 0.0f) {
		callable_mp(this, &GrassField3D::_far_view_apply).call_deferred(Color(1, 1, 1, 1), 0);
		return;
	}
	const uint8_t *data = p_data.ptr();
	const Vector2 to_px(float(p_width) / far_view_size.x, float(p_height) / far_view_size.y);
	const int half_box = MAX(2, p_height / 120); // About 1/60 of the frame high, three times as wide: many blades.
	auto box = [&](const Vector2 &p_at) -> Color {
		const int cx = int(p_at.x * to_px.x), cy = int(p_at.y * to_px.y);
		Color sum(0, 0, 0, 0);
		int n = 0;
		for (int y = MAX(cy - half_box, 0); y <= MIN(cy + half_box, p_height - 1); y++) {
			for (int x = MAX(cx - 3 * half_box, 0); x <= MIN(cx + 3 * half_box, p_width - 1); x++) {
				const uint8_t *px = data + (int64_t(y) * p_width + x) * bpp;
				Color col;
				if (half) {
					const uint16_t *h = reinterpret_cast<const uint16_t *>(px);
					col = Color(Math::half_to_float(h[0]), Math::half_to_float(h[1]), Math::half_to_float(h[2]), 1.0f);
				} else {
					col = Color(px[bgr ? 2 : 0] / 255.0f, px[1] / 255.0f, px[bgr ? 0 : 2] / 255.0f, 1.0f);
				}
				if (srgb) {
					col = col.srgb_to_linear();
				}
				sum += col;
				n++;
			}
		}
		return n ? sum / float(n) : Color();
	};
	// The ratio of the sums over the pairs kept (both sides share the same headings, so the sun's
	// azimuth cancels); a single box of blades varies too much with clumps and gaps to use alone.
	Color sum_in(0, 0, 0, 0), sum_out(0, 0, 0, 0);
	LocalVector<float> lum_ratios;
	HashMap<int, Color> slot_in, slot_out;
	HashMap<int, int> slot_n;
	const bool debug = OS::get_singleton()->get_environment("GRASS_VIEW_DEBUG") != ""; // Print each pair.
	for (int i = 0; i + 1 < far_view_px.size(); i += 2) {
		const Color in = box(far_view_px[i]);
		const Color out = box(far_view_px[i + 1]);
		const float lin = in.r + in.g + in.b, lout = out.r + out.g + out.b;
		if (debug) {
			print_line(vformat("VIEWPAIR in %s %s out %s %s", far_view_px[i], in, far_view_px[i + 1], out));
		}
		if (lin < 1e-4f || lout < 1e-4f || lin / lout < 0.4f || lin / lout > 2.5f) {
			continue; // Something standing in front of one of them.
		}
		if (in.b > 0.9f * in.g || out.b > 0.9f * out.g) {
			continue; // Sky, haze or water: grass and soil are always less blue than green.
		}
		sum_in += in;
		sum_out += out;
		lum_ratios.push_back(lin / lout);
		const int slot = i / 2 < int(far_view_pair_slot.size()) ? far_view_pair_slot[i / 2] : 0;
		if (slot > 0) {
			slot_in[slot] = (slot_in.has(slot) ? slot_in[slot] : Color(0, 0, 0, 0)) + in;
			slot_out[slot] = (slot_out.has(slot) ? slot_out[slot] : Color(0, 0, 0, 0)) + out;
			slot_n[slot] = (slot_n.has(slot) ? slot_n[slot] : 0) + 1;
		}
	}
	// Each field with enough pairs of its own gets its own ratio.
	far_view_slot_ratio.clear();
	for (const KeyValue<int, int> &e : slot_n) {
		if (e.value >= 6) {
			const Color a = slot_in[e.key], b = slot_out[e.key];
			far_view_slot_ratio[e.key] = Color(a.r / MAX(b.r, 1e-5f), a.g / MAX(b.g, 1e-5f), a.b / MAX(b.b, 1e-5f), 1.0f);
		}
	}
	int pairs = lum_ratios.size();
	Color med(1, 1, 1, 1);
	if (pairs > 0) {
		// Brightness only: the bake's hue holds, and the blue channel is too small to read well.
		const float yin = sum_in.r * 0.2126f + sum_in.g * 0.7152f + sum_in.b * 0.0722f;
		const float yout = sum_out.r * 0.2126f + sum_out.g * 0.7152f + sum_out.b * 0.0722f;
		const float k = yin / MAX(yout, 1e-5f);
		med = Color(k, k, k, 1.0f);
		if (fade_owner != this) {
			// A crop: per channel (the far canopy's hue is set by hand, the tillers' by the renderer).
			med = Color(sum_in.r / MAX(sum_out.r, 1e-5f), sum_in.g / MAX(sum_out.g, 1e-5f), sum_in.b / MAX(sum_out.b, 1e-5f), 1.0f);
		}
		lum_ratios.sort();
		const float spread = (lum_ratios[(pairs * 3) / 4] - lum_ratios[pairs / 4]) / MAX(lum_ratios[pairs / 2], 1e-4f);
		if (spread > 0.8f && far_view_slot_ratio.size() < 2) {
			pairs = 0; // The pairs disagree too much to trust (a mixed view): look again later.
		}
	}
	far_view_msec = double(OS::get_singleton()->get_ticks_usec() - t0) / 1000.0;
	callable_mp(this, &GrassField3D::_far_view_apply).call_deferred(med, pairs);
}

void GrassField3D::_far_view_apply(const Color &p_ratio, int p_pairs) {
	far_view_state = 0;
	far_view_pairs = p_pairs;
	if (p_pairs < 12) {
		// Too little of the band in view: look again in a while, then wait for the camera to move.
		if (++far_view_misses < 10) {
			_far_view_start(60);
		} else {
			far_view_elevation = sun_elevation_deg;
		}
		return;
	}
	far_view_misses = 0;
	far_view_ratio = p_ratio;
	far_view_round++;
	const bool close = Math::abs(p_ratio.r - 1.0f) < 0.03f && Math::abs(p_ratio.g - 1.0f) < 0.03f && Math::abs(p_ratio.b - 1.0f) < 0.03f;
	if (!close && fade_owner != this) {
		// A crop's far canopy (crop_ground()).
		crop_view_scale.r = CLAMP(crop_view_scale.r * CLAMP(p_ratio.r, 0.5f, 2.0f), 0.3f, 3.0f);
		crop_view_scale.g = CLAMP(crop_view_scale.g * CLAMP(p_ratio.g, 0.5f, 2.0f), 0.3f, 3.0f);
		crop_view_scale.b = CLAMP(crop_view_scale.b * CLAMP(p_ratio.b, 0.5f, 2.0f), 0.3f, 3.0f);
		RenderingServer::get_singleton()->global_shader_parameter_set("crop_ground_view", Vector4(crop_view_scale.r, crop_view_scale.g, crop_view_scale.b, 1.0f));
	}
	if (fade_owner != this && !far_view_slot_ratio.is_empty()) {
		// Each measured field: its own ratio over the global step just taken.
		for (const KeyValue<int, Color> &e : far_view_slot_ratio) {
			Color sv = crop_slot_view.has(e.key) ? crop_slot_view[e.key] : Color(1, 1, 1, 1);
			for (int ch = 0; ch < 3; ch++) {
				const float rel = e.value[ch] / MAX(p_ratio[ch], 1e-4f);
				sv[ch] = CLAMP(sv[ch] * CLAMP(rel, 0.5f, 2.0f), 0.4f, 2.5f);
			}
			crop_slot_view[e.key] = sv;
		}
		crop_plots_dirty = true; // _crop_ground_step() writes them into the plots' row 3.
	}
	if (!close && fade_owner == this) {
		// The frame is tone mapped, so the step is not exact: take it, then look again.
		far_view_scale.r = CLAMP(far_view_scale.r * CLAMP(p_ratio.r, 0.8f, 1.25f), 0.5f, 1.5f);
		far_view_scale.g = CLAMP(far_view_scale.g * CLAMP(p_ratio.g, 0.8f, 1.25f), 0.5f, 1.5f);
		far_view_scale.b = CLAMP(far_view_scale.b * CLAMP(p_ratio.b, 0.8f, 1.25f), 0.5f, 1.5f);
		if (fade_owner == this) {
			_publish_far();
		}
	}
	far_view_elevation = sun_elevation_deg;
	print_verbose(vformat("GrassField3D \"%s\": live-view far match round %d: blades/far %s over %d pairs, scale now %s (%.2f ms).", get_name(), far_view_round, p_ratio, p_pairs, fade_owner == this ? far_view_scale : crop_view_scale, far_view_msec));
	if (!close && far_view_round < 5) {
		_far_view_start(fade_owner == this ? 20 : 10); // A crop's own ground only: no bake to wait on.
	} else {
		far_view_round = 0;
	}
}

PackedColorArray GrassField3D::get_far_color_lobes() const {
	PackedColorArray r;
	r.push_back(far_baked ? far_lobe1 : Color(0, 0, 0, 0));
	r.push_back(far_baked ? far_lobe2 : Color(0, 0, 0, 0));
	return r;
}

void GrassField3D::set_sun_light(const NodePath &p_path) {
	sun_light = p_path;
	sun_cache = ObjectID();
	sun_search_wait = 0;
}

// The sun for the far colour's azimuth: sun_light, else the first visible DirectionalLight3D in
// this field's world (looked for again about once a second while there is none).
DirectionalLight3D *GrassField3D::_get_sun() {
	if (!sun_light.is_empty()) {
		return Object::cast_to<DirectionalLight3D>(get_node_or_null(sun_light));
	}
	DirectionalLight3D *sun = Object::cast_to<DirectionalLight3D>(ObjectDB::get_instance(sun_cache));
	if (sun && sun->is_inside_tree() && sun->is_visible_in_tree()) {
		return sun;
	}
	sun_cache = ObjectID();
	if (sun_search_wait-- > 0) {
		return nullptr;
	}
	sun_search_wait = 60;
	const Ref<World3D> world = get_world_3d();
	TypedArray<Node> lights = get_tree()->get_root()->find_children("*", "DirectionalLight3D", true, false);
	for (int i = 0; i < lights.size(); i++) {
		DirectionalLight3D *l = Object::cast_to<DirectionalLight3D>(lights[i]);
		if (l && l->is_visible_in_tree() && l->get_world_3d() == world) {
			sun_cache = l->get_instance_id();
			return l;
		}
	}
	return nullptr;
}

// The sun's heading and the camera, so grass_ground() can turn the far colour with the azimuth
// between them, and the sun's elevation for the far colour itself. Without the elevation bake the
// lobes are the 30 degree ones, and the azimuth matters less as the sun climbs (none at the
// zenith); with it the lobes are those of the sun's own elevation. Either way it goes with the sun
// below the horizon.
void GrassField3D::_publish_sun_view(const Vector3 &p_camera) {
	Vector4 sun_v;
	DirectionalLight3D *sun = _get_sun();
	if (sun && sun->is_visible_in_tree() && sun->get_param(Light3D::PARAM_ENERGY) > 0.0f) {
		const Vector3 d = -sun->get_global_transform().basis.get_column(Vector3::AXIS_Z).normalized(); // Where the light travels.
		const float h = Vector2(d.x, d.z).length();
		const float up = -d.y; // Sine of the sun's elevation.
		if (h > 1e-4f) {
			const float horizon = CLAMP((up + 0.05f) / 0.1f, 0.0f, 1.0f);
			const float strength = far_elev_count == FAR_ELEVATIONS ? horizon : MIN(h / Math::cos(Math::deg_to_rad(FAR_BAKE_SUN_DEG)), 1.15f) * horizon;
			sun_v = Vector4(d.x / h, d.z / h, strength, 0.0f);
		}
		sun_elevation_deg = Math::rad_to_deg(Math::asin(CLAMP(up, -1.0f, 1.0f)));
		// The far colour depends on how the light splits between sun and sky: bake it again once
		// the sun's strength has moved well away from the one it was baked under.
		const float ratio = sun->get_param(Light3D::PARAM_ENERGY) / far_bake_sun_energy;
		if (far_baked && !far_bake_vp && !far_bake_queued && (ratio > 1.3f || ratio < 1.0f / 1.3f)) {
			_queue_far_bake();
		}
	}
	if (far_baked && Math::abs(sun_elevation_deg - published_far_elevation) > 0.5f) {
		_publish_far(); // The sun has climbed or sunk: the far colour that matches the blades now.
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	if (!sun_v.is_equal_approx(published_sun)) {
		published_sun = sun_v;
		rs->global_shader_parameter_set("grass_ground_sun", sun_v);
	}
	const Vector4 view(p_camera.x, p_camera.y, p_camera.z, 1.0f);
	if (!view.is_equal_approx(published_view)) {
		published_view = view;
		rs->global_shader_parameter_set("grass_ground_view", view);
	}
}

void GrassField3D::_update_tiles(const Vector3 &p_center) {
	_ensure_resources();
	TerrainDataMap3D *map = Object::cast_to<TerrainDataMap3D>(get_node_or_null(data_map));
	if (map && !map->is_baked()) {
		return; // Wait for the map.
	}
	if (map && hole_source.is_null()) {
		_follow_holes(map);
	}
	if (!ground_baked) {
		_bake_ground(map);
	} else if (ground_job.active) {
		_ground_bake_rows(map, GROUND_BAKE_BUDGET_USEC);
	}
	_follow_burns(map);
	{
		// Game trails published or changed: the sward and its ground are worn along them.
		const WorldStand *stand = WorldContracts::stand();
		const uint64_t now = OS::get_singleton()->get_ticks_usec();
		if (stand && stand->get_stand_generation() != trails_seen && now - trails_follow_usec >= uint64_t(BURN_FOLLOW_MSEC) * 1000) {
			trails_follow_usec = now;
			if (_read_world_trails()) {
				// Between frames, as a setter would: the rings and tiles below still use this frame's resources.
				callable_mp(this, &GrassField3D::rebuild).call_deferred();
			}
		}
	}
	if (crop == CROP_NONE) {
		const uint64_t now = OS::get_singleton()->get_ticks_usec();
		if (now - mounds_follow_usec >= uint64_t(BURN_FOLLOW_MSEC) * 1000) {
			mounds_follow_usec = now;
			_follow_mounds();
		}
	}
	if (burn_read && graze_height > 0.0f && crop == CROP_NONE) {
		// Browse discs added, moved or cleared: the pressure grid is read again (cheap; the texture only).
		const WorldStand *stand = WorldContracts::stand();
		if (stand && stand->get_stand_generation() != graze_seen) {
			const uint64_t now = OS::get_singleton()->get_ticks_usec();
			if (now - burn_follow_usec >= uint64_t(BURN_FOLLOW_MSEC) * 1000) {
				burn_follow_usec = now;
				_read_graze(map);
				_publish_graze();
			}
		}
	}
	if (crop != CROP_NONE && is_inside_tree()) {
		_crop_ground_step();
	}
	if (radius <= 0.0f) {
		// radius 0: no blades at all (the near tiles, the far rings and shell); the ground tint keeps its own setting.
		for (KeyValue<Vector2i, Tile> &kv : tiles) {
			_free_tile(kv.value);
		}
		tiles.clear();
		_free_rings();
		_free_shell();
		return;
	}
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	const Vector2 c(p_center.x, p_center.z);
	const float reach = radius + tile_size * 0.75f;
	_update_fade(c);
	if (crop != CROP_NONE && is_inside_tree()) {
		// The fields' ground also needs the eye's height: seen low, the canopy hides the soil.
		const Vector4 cv(c.x, c.y, p_center.y, 1.0f);
		if (!cv.is_equal_approx(crop_view_published)) {
			crop_view_published = cv;
			RenderingServer::get_singleton()->global_shader_parameter_set("crop_ground_fade", cv);
		}
		if (!crop_nodes.is_empty() && crop_nodes[0] == this) {
			_publish_crop_sun();
		}
	}
	// Drop tiles out of range or at the wrong level.
	LocalVector<Vector2i> drop;
	for (KeyValue<Vector2i, Tile> &kv : tiles) {
		const Vector2 tc = (Vector2(kv.key) + Vector2(0.5f, 0.5f)) * tile_size;
		const float d = tc.distance_to(c);
		if (d > reach || _lod_for(d) != kv.value.lod) {
			drop.push_back(kv.key);
		}
	}
	// Wanted tiles, nearest first.
	struct Want {
		Vector2i tile;
		float d;
		bool operator<(const Want &o) const { return d < o.d; }
	};
	LocalVector<Want> want;
	const int r = int(Math::ceil(reach / tile_size));
	const Vector2i ct(int(Math::floor(c.x / tile_size)), int(Math::floor(c.y / tile_size)));
	for (int dz = -r; dz <= r; dz++) {
		for (int dx = -r; dx <= r; dx++) {
			const Vector2i t = ct + Vector2i(dx, dz);
			const Vector2 tc = (Vector2(t) + Vector2(0.5f, 0.5f)) * tile_size;
			const float d = tc.distance_to(c);
			if (d > radius + tile_size * 0.5f) {
				continue;
			}
			const Tile *existing = tiles.getptr(t);
			if (!existing || existing->lod != _lod_for(d) || existing->generation != generation) {
				want.push_back({ t, d });
			}
		}
	}
	want.sort();
	// Replace a tile only once its successor is ready, so no hole flashes; limit the work a frame.
	int built = 0;
	for (const Want &w : want) {
		if (max_tiles_per_frame > 0 && built >= max_tiles_per_frame) {
			break;
		}
		Tile *existing = tiles.getptr(w.tile);
		if (existing) {
			_free_tile(*existing);
			tiles.erase(w.tile);
		}
		_rocks_for(Rect2(w.tile.x * tile_size, w.tile.y * tile_size, tile_size, tile_size));
		_build_tile(w.tile, _lod_for(w.d), map);
		built++;
	}
	for (const Vector2i &t : drop) {
		Tile *existing = tiles.getptr(t);
		if (!existing) {
			continue;
		}
		const Vector2 tc = (Vector2(t) + Vector2(0.5f, 0.5f)) * tile_size;
		if (tc.distance_to(c) > reach) {
			_free_tile(*existing);
			tiles.erase(t);
		}
	}
	if (built > 0) {
		build_msec = (OS::get_singleton()->get_ticks_usec() - t0) / 1000.0;
	}
	last_built = int(want.size()); // Anything wanted (built now or left for later frames), with the far rings' and shell's.
	_update_rings(c, map);
	_update_shell(c, map);
}

// Ground tint: where grass grows the ground under it takes the grass's colour, so the gaps
// between blades read as more grass (thatch and shade) instead of bare soil. Coverage is the
// rules' suitability on the data map's grid, published as grass_ground_* globals for the
// terrain's shader (get_ground_shader_code()).
void GrassField3D::_bake_ground(TerrainDataMap3D *p_map) {
	ground_baked = true;
	ground_job.active = false;
	sun_mean = VegetationLight::sun_mean(p_map);
	fade_band = Vector4(); // Send the band and the far colour again.
	if (!burn_read) {
		_read_burn_age(p_map);
		_read_sand_sea(p_map);
		burn_read = true;
		_publish_burn_flush();
		_read_graze(p_map);
		_publish_graze();
	}
	if (crop != CROP_NONE) {
		crop_ground_dirty = true; // The fields' own ground (crop_ground()).
		return;
	}
	if (is_inside_tree()) {
		// The fallback wind for the other plants (PlantGenerator's shader), so they sway with the grass.
		RenderingServer::get_singleton()->global_shader_parameter_set("grass_wind", Vector4(fallback_wind.x, fallback_wind.y, wind_response, 0.0f));
	}
	if (!ground_tint_enabled || !is_inside_tree()) {
		return;
	}
	if (!p_map) {
		// No map: grass everywhere.
		Vector<uint8_t> data;
		data.push_back(255);
		data.push_back(0);
		_ground_bake_publish(Vector4(-1e6f, -1e6f, 2e6f, 2e6f), Image::create_from_data(1, 1, false, Image::FORMAT_RG8, data), Ref<Image>());
		return;
	}
	GroundJob &j = ground_job;
	j.res = MIN(p_map->get_resolution(), 1024);
	const float size = p_map->get_size();
	const Vector3 c = p_map->get_global_position();
	j.rect = Vector4(c.x - size * 0.5f, c.z - size * 0.5f, size, size);
	j.row = 0;
	j.any_litter = false;
	j.data.resize(j.res * j.res * 2); // r: the sward's cover, g: the tread of worn paths.
	j.ldata.resize(_get_undergrowth() ? j.res * j.res * 4 : 0);
	if (j.ldata.size() > 0) {
		memset(j.ldata.ptrw(), 0, j.ldata.size());
	}
	j.active = true;
	// The first bake whole, so the ground is right from the first frame; later ones a few rows a
	// frame (_update_tiles), the previous map shown meanwhile.
	_ground_bake_rows(p_map, ground_published ? GROUND_BAKE_BUDGET_USEC : 0);
}

// Bakes rows of the ground tint's map until the time budget runs out (0: all of them), and
// publishes the map when the last row is done.
void GrassField3D::_ground_bake_rows(TerrainDataMap3D *p_map, uint64_t p_budget_usec) {
	GroundJob &j = ground_job;
	if (!j.active || !p_map) {
		return;
	}
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	Ref<PlantSpecies> r = rules;
	if (r.is_null()) {
		r.instantiate();
	}
	const VegetationField3D *canopy = _get_canopy(r.ptr());
	const VegetationField3D *under = _get_undergrowth();
	const int res = j.res;
	const float size = j.rect.z;
	uint8_t *w = j.data.ptrw();
	// The undergrowth's litter where its stands have shaded the sward out (bracken's dead
	// fronds), as premultiplied colour stored as its square root (the shader squares it back).
	uint8_t *lw = (under && j.ldata.size() == res * res * 4) ? j.ldata.ptrw() : nullptr;
	while (j.row < res) {
		const int z = j.row;
		for (int x = 0; x < res; x++) {
			const Vector3 p(j.rect.x + (x + 0.5f) * size / res, 0.0f, j.rect.y + (z + 0.5f) * size / res);
			float suit = crop_mask.contains(Vector2(p.x, p.z)) ? 0.0f : r->get_suitability(p_map, p);
			if (canopy && suit > 0.0f) {
				suit *= r->rule_weight(PlantSpecies::RULE_SHADE, canopy->get_stand_shade_at(p));
			}
			if (suit > 0.0f) {
				suit *= _burn_cover(r.ptr(), p.x, p.z);
			}
			if (lw) {
				Color lc;
				const float sh = under->sward_at(p, &lc);
				suit *= 1.0f - sh;
				uint8_t *o = lw + (z * res + x) * 4;
				o[0] = uint8_t(CLAMP(Math::sqrt(lc.r * lc.a), 0.0f, 1.0f) * 255.0f + 0.5f);
				o[1] = uint8_t(CLAMP(Math::sqrt(lc.g * lc.a), 0.0f, 1.0f) * 255.0f + 0.5f);
				o[2] = uint8_t(CLAMP(Math::sqrt(lc.b * lc.a), 0.0f, 1.0f) * 255.0f + 0.5f);
				o[3] = uint8_t(CLAMP(lc.a, 0.0f, 1.0f) * 255.0f + 0.5f);
				j.any_litter = j.any_litter || o[3] > 0;
			}
			float tread = 0.0f;
			if (!path_grid.is_empty()) {
				// Worn paths: the trodden line shows the bare, compacted soil the walkers have
				// opened (grass_ground_tread()), and the sward is thinned there.
				const float cover = _trample_cover(path_grid.passes_at(Vector2(p.x, p.z)));
				suit *= cover;
				tread = 1.0f - cover;
			}
			w[(z * res + x) * 2] = uint8_t(CLAMP(suit, 0.0f, 1.0f) * 255.0f + 0.5f);
			w[(z * res + x) * 2 + 1] = uint8_t(CLAMP(tread, 0.0f, 1.0f) * 255.0f + 0.5f);
		}
		j.row++;
		if (p_budget_usec > 0 && OS::get_singleton()->get_ticks_usec() - t0 >= p_budget_usec) {
			return;
		}
	}
	j.active = false;
	const Ref<Image> litter = j.any_litter ? Image::create_from_data(res, res, false, Image::FORMAT_RGBA8, j.ldata) : Ref<Image>();
	_ground_bake_publish(j.rect, Image::create_from_data(res, res, false, Image::FORMAT_RG8, j.data), litter);
	j.data.clear();
	j.ldata.clear();
}

void GrassField3D::_ground_bake_publish(const Vector4 &p_rect, const Ref<Image> &p_img, const Ref<Image> &p_litter) {
	Ref<Image> litter_img = p_litter;
	if (litter_img.is_null()) {
		// No undergrowth litter: one clear texel.
		Vector<uint8_t> none;
		none.resize(4);
		memset(none.ptrw(), 0, 4);
		litter_img = Image::create_from_data(1, 1, false, Image::FORMAT_RGBA8, none);
	}
	ground_texture = ImageTexture::create_from_image(p_img);
	ground_litter_texture = ImageTexture::create_from_image(litter_img);
	ground_image = p_img;
	ground_image_rect = p_rect;
	if (!plant_color_registered && is_inside_tree()) {
		WorldContracts::register_plant_color(this); // The ground field: its sward's colour for WorldGround.albedo_at.
		plant_color_registered = true;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	rs->global_shader_parameter_set("grass_ground_map", ground_texture->get_rid());
	rs->global_shader_parameter_set("grass_ground_litter", ground_litter_texture->get_rid());
	rs->global_shader_parameter_set("grass_ground_rect", p_rect);
	rs->global_shader_parameter_set("grass_ground_color", Vector4(ground_tint.r, ground_tint.g, ground_tint.b, ground_tint.a));
	rs->global_shader_parameter_set("grass_ground_dry", Vector4(dry_color.r, dry_color.g, dry_color.b, _dryness()));
	ground_published = true;
	rs->global_shader_parameter_set("grass_ground_params", Vector4(color_variation, trample_resistance, 0.0f, 0.0f));
}

// Thin snow on the sward (docs/ideas/vegetation.md, "Thin snow on the sward"). Snow falls onto the sward's canopy, not
// under it: the blades catch the flakes and bow under them, so a centimetre or two whitens a short sward with only the
// tips showing, long before the pack is deep enough to bury the blades. The share covered seen from above is Niu and
// Yang's (2007) snow-covered fraction, tanh(d / (2.5 z0 (rho / rho_new))), with the sward's own roughness, z0 = 0.1 h
// (the bare ground's is 0.004 m in snow_cover_at()): fresh snow (100 kg/m3) 1.6 cm deep covers 86% of a 5 cm sward,
// 31% of a 20 cm one, 13% of a 50 cm one; settled, denser snow covers less for its depth. Each blade is laden or not
// by that share, its lower 70% white and its tip clear; deeper, the pack buries the blades from the ground up (v_snow's
// burial term).
String GrassField3D::get_snow_sward_shader_code() {
	return R"(
// The share of a sward p_height (m) tall under snow seen from above, for a pack of depth p_pack.x (m) and density
// p_pack.y (kg/m3). The sward's roughness length is a tenth of its height (the usual rule for grass, z0 = 0.1 h).
float grass_snow_cover(vec2 p_pack, float p_height) {
	if (p_pack.x <= 0.002) {
		return 0.0;
	}
	float z0 = max(0.1 * p_height, 0.004);
	return tanh(p_pack.x / (2.5 * z0 * max(p_pack.y / 100.0, 0.5)));
}
// How white a blade p_height (m) tall is at p_t along it (0 base, 1 tip) under that snow: the laden blades (p_rnd under
// the share).
float grass_snow_coat(vec2 p_pack, float p_rnd, float p_t, float p_height) {
	float f = grass_snow_cover(p_pack, p_height);
	return f > 0.0 ? step(fract(p_rnd * 29.37 + 0.71), f) * (1.0 - smoothstep(0.7, 0.97, p_t)) : 0.0;
}
)";
}

String GrassField3D::get_ground_shader_code() {
	String code = R"(
// GrassField3D turf under the grass, for terrain shaders. Where grass grows the ground becomes the
// base of the sward rather than soil (Serious Sam 4, GDC 2019: grass "albedo colored from the
// floor", floor and blades matched; Ghost of Tsushima and Star Citizen colour the terrain from the
// grass on it): the blades' own root colour with the same colour patches as the blades, fine
// streaks of out-of-focus blades, dead thatch, and the dark of the roots. A gap in the carpet then
// shows more grass, not ground.
global uniform sampler2D grass_ground_map : filter_linear; // r: the sward's cover, g: worn paths' tread.
global uniform sampler2D grass_ground_litter : hint_default_black, filter_linear; // sqrt of premultiplied rgb, a: cover.
global uniform vec4 grass_ground_rect;
global uniform vec4 grass_ground_color; // rgb: the sward's root colour; a: how much it covers the soil.
global uniform vec4 grass_ground_dry; // rgb: thatch colour; a: the grass's dryness.
global uniform vec4 grass_ground_params; // x: colour variation, y: the sward's trample_resistance.
global uniform vec4 grass_ground_fade; // xy: centre of the blades, z: where the last is gone, w: the band they thin over.
global uniform vec4 grass_ground_far; // rgb: the blades' colour seen from a distance.
global uniform vec4 grass_ground_far_lobe1; // rgb: its change with cos of the sun's azimuth from the view.
global uniform vec4 grass_ground_far_lobe2; // rgb: and with cos of twice that azimuth.
global uniform vec4 grass_ground_under; // xy: centre, z: radius, w: band of the undercoat field (0: none).
global uniform vec4 grass_ground_under_far; // rgb: the undercoat's far colour; a: 1 when set.
global uniform vec4 grass_ground_far_scale; // rgb: the far colour at the sun's elevation over the cover map's (30 degrees).
global uniform vec4 grass_ground_sun; // xy: the sun light's heading (xz); z: how much the azimuth counts.
global uniform vec4 grass_ground_view; // xyz: the camera.
global uniform sampler2D grass_graze_map : hint_default_black, filter_linear, repeat_disable; // R: grazed to a lawn, G: torn by rooting.
global uniform vec4 grass_graze_rect; // z 0: none.
global uniform sampler2D grass_mow_map : hint_default_black, filter_nearest, repeat_disable; // Hay meadows: R the meadow (index + 1, 0 none).
global uniform sampler2D grass_mow_state : hint_default_black, filter_nearest, repeat_disable; // A texel a meadow: r days since mown (below 0 not), g the rows' angle, b mm of rain on the cut grass.
global uniform vec4 grass_mow_rect; // z 0: none.
global uniform sampler2D crop_ground_map : filter_linear; // GrassField3D.fields: r the field's slot, g the wheelings.
global uniform sampler2D crop_ground_plots : filter_nearest; // A texel a field (crop_ground()).
global uniform vec4 crop_ground_rect;
global uniform vec4 crop_ground_sun; // xy: where the sun's light heads, z: the lobes' strength (crop tillers' bake).
global uniform vec4 crop_ground_lobe1; // rgb: the far canopy's change with cos of the sun's azimuth from the view.
global uniform vec4 crop_ground_lobe2; // rgb: and with cos of twice that azimuth.
global uniform vec4 crop_ground_view; // rgb: the live-view match of the far canopy to the tillers on screen (1: none yet).
global uniform vec4 crop_ground_fade; // xy: the centre of the tillers' band (the eye), z: the eye's height, w: 1 (-1: no tillers, the far canopy everywhere).
)";
	code += GRASS_COLOR_CODE;
	code += VegetationField3D::get_floor_shader_code();
	code += R"(
#ifndef GROUND_WEAR_CODE
#define GROUND_WEAR_CODE
global uniform sampler2D ground_wear_map : filter_linear, repeat_disable;
global uniform vec4 ground_wear_rect;
// Worn-in tracks (mud round 14.2): the sward's cover left by the wheel passes GroundSurface3D counted at p_xz
// (ground_wear_map). A wheel bears and scuffs as some twenty boots do, so the passes count twenty times against the
// sward's p_resistance (GrassField3D.trample_resistance, foot passes; Cole 1995's cover, as _trample_cover()).
float grass_ground_worn(vec2 p_xz, float p_resistance) {
	if (ground_wear_rect.w <= 0.0 || ground_wear_rect.z <= 0.0) {
		return 1.0;
	}
	vec2 uv = (p_xz - ground_wear_rect.xy) / ground_wear_rect.z;
	if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
		return 1.0;
	}
	float passes = textureLod(ground_wear_map, uv, 0.0).r;
	float x = 20.0 * passes / max(p_resistance, 1.0);
	return x > 0.0 ? (1.0 - smoothstep(2.5, 5.0, x)) / (1.0 + x * x) : 1.0;
}
#endif

float grass_ground_cover(vec3 world_pos) {
	if (grass_ground_rect.z <= 0.0) {
		return 0.0;
	}
	vec2 uv = (world_pos.xz - grass_ground_rect.xy) / grass_ground_rect.zw;
	vec2 half_texel = 0.5 / vec2(textureSize(grass_ground_map, 0));
	// Nothing outside the map: fade out over its outer texel instead of smearing the edge.
	vec2 outside = max(half_texel - uv, uv - (1.0 - half_texel));
	float inside = 1.0 - clamp(max(outside.x, outside.y) / half_texel.x, 0.0, 1.0);
	return textureLod(grass_ground_map, clamp(uv, half_texel, 1.0 - half_texel), 0.0).r * inside * grass_ground_worn(world_pos.xz, grass_ground_params.y > 0.0 ? grass_ground_params.y : 300.0);
}

// The tread of worn paths (GrassField3D.paths): 1 where walkers have worn the sward away.
float grass_ground_tread(vec3 world_pos) {
	if (grass_ground_rect.z <= 0.0) {
		return 0.0;
	}
	vec2 uv = (world_pos.xz - grass_ground_rect.xy) / grass_ground_rect.zw;
	if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
		return 0.0;
	}
	return textureLod(grass_ground_map, uv, 0.0).g;
}

// Noise streaked along a direction that turns slowly over the ground (matted blades, litter). Turning world xz itself
// by a varying angle draws arcs round the world's origin (a small turn swings far points a long way: concentric swirl
// stripes, 20 Nov), so each 2 m cell turns the offsets from its own centre and the four nearest cells blend (texture
// bombing, Glanville 2004, GPU Gems ch. 20).
float grass_streaks(vec2 xz, float p_turn_scale, float p_turn_offset, vec2 p_s1, vec2 p_s2, float p_o2) {
	vec2 g = xz * 0.5;
	vec2 i = floor(g);
	vec2 f = smoothstep(vec2(0.0), vec2(1.0), fract(g));
	float s = 0.0;
	for (int k = 0; k < 4; k++) {
		vec2 o = vec2(float(k % 2), float(k / 2));
		vec2 c = (i + o) * 2.0;
		float ang = grass_noise(c * p_turn_scale + p_turn_offset) * 6.2832;
		vec2 dir = vec2(cos(ang), sin(ang));
		vec2 d = xz - c;
		vec2 q = vec2(dot(d, dir), dot(d, vec2(-dir.y, dir.x))) + c * 1.618;
		float v = grass_noise(q * p_s1) * 0.6 + grass_noise(q * p_s2 + p_o2) * 0.4;
		vec2 wv = mix(1.0 - f, f, o);
		s += v * wv.x * wv.y;
	}
	// Blending four lowers the contrast mid-cell; most of it back.
	return clamp(0.5 + (s - 0.5) * 1.3, 0.0, 1.0);
}

// Deer lawns (GrassField3D graze_height): R how far the sward is grazed down, G the share rooting tore out.
vec2 grass_graze_at(vec3 world_pos) {
	if (grass_graze_rect.z <= 0.0) {
		return vec2(0.0);
	}
	vec2 uv = (world_pos.xz - grass_graze_rect.xy) / grass_graze_rect.zw;
	if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
		return vec2(0.0);
	}
	return textureLod(grass_graze_map, uv, 0.0).rg;
}

// Hay meadows past the blades (GrassField3D.hay_meadows; the blade shader does them near): the stubble paler and
// yellower than the standing sward (the cut stems' pale bases) until it grows back, and the cut grass where it lies:
// the swaths and windrows as stripes along the rows, as the blades lay them (same pitches, widths and wander), a
// tedded field all over, curing from the sward's green to green-gold hay.
vec3 grass_mow_ground(vec3 world_pos, vec3 turf) {
	if (grass_mow_rect.z <= 0.0) {
		return turf;
	}
	vec2 uv = (world_pos.xz - grass_mow_rect.xy) / grass_mow_rect.zw;
	if (any(lessThan(uv, vec2(0.0))) || any(greaterThanEqual(uv, vec2(1.0)))) {
		return turf;
	}
	int mi = int(textureLod(grass_mow_map, uv, 0.0).r * 255.0 + 0.5);
	if (mi <= 0) {
		return turf;
	}
	vec4 ms = texelFetch(grass_mow_state, ivec2(mi - 1, 0), 0);
	float md = ms.r;
	if (md < 0.0) {
		return turf;
	}
	float stub = 1.0 - smoothstep(4.0, 42.0, md);
	turf = mix(turf, turf * vec3(1.14, 1.1, 0.8), stub);
	if (md < 3.5) {
		vec2 rd = vec2(cos(ms.g), sin(ms.g));
		vec2 rn = vec2(-rd.y, rd.x);
		float pitch = md < 1.0 ? 3.0 : (md < 2.5 ? 0.0 : 6.0);
		float half_w = md < 1.0 ? 0.8 : 0.6;
		float lie = 0.6; // Tedded: a loose layer over it all.
		if (pitch > 0.0) {
			float u = dot(world_pos.xz, rn);
			float row = floor(u / pitch);
			float v = dot(world_pos.xz, rd);
			float uc = (row + 0.5) * pitch + 0.2 * sin(v * 0.06 + row * 1.7) + 0.08 * sin(v * 0.27 + row * 0.4);
			float across = abs(u - uc) / half_w;
			lie = 1.0 - smoothstep(0.7, 1.05, across);
		}
		vec3 cured = vec3(0.42, 0.4, 0.17) * (0.9 + 0.2 * grass_noise(world_pos.xz * 2.1));
		vec3 hay = mix(turf * vec3(0.95, 1.02, 0.8), cured, smoothstep(0.3, 3.0, md) * 0.85);
		hay = mix(hay, vec3(dot(hay, vec3(0.3, 0.59, 0.11))) * vec3(0.95, 0.85, 0.62), smoothstep(2.0, 25.0, ms.b) * 0.75); // Rained on (as the blades)
		turf = mix(turf, hay, lie * 0.9);
	}
	return turf;
}

// The living turf's colour at a point: blades' root colour, patch tint, streaks, thatch.
vec3 grass_turf_green(vec3 world_pos) {
	vec2 xz = world_pos.xz;
	// Streaks: fine noise stretched along a direction that turns slowly, like matted blades.
	float streak = grass_streaks(xz, 0.6, 0.0, vec2(9.0, 70.0), vec2(4.0, 31.0), 7.1);
	vec3 c = grass_ground_color.rgb * grass_patch_tint(xz, grass_noise(xz * 3.0), streak, grass_ground_params.x);
	c *= 0.7 + 0.6 * streak;
	// Dead thatch where the grass is dry, in matted patches.
	float thatch = grass_streaks(xz, 0.6, 0.0, vec2(5.0, 40.0), vec2(5.0, 40.0), 13.0) * 0.5 + grass_noise(xz * 1.3) * 0.5;
	// Shaded at the base of the sward, so darker than the dry tips above it. A deer lawn has little: the deer eat
	// the dead leaves with the green and the new leaves are fresh (the blades' own dry share drops the same way).
	float lawn = grass_graze_at(world_pos).r;
	c = mix(c, grass_ground_dry.rgb * (0.25 + 0.2 * streak), 0.8 * (1.0 - 0.7 * lawn) * smoothstep(0.8 - grass_ground_dry.a * 0.5, 0.95 - grass_ground_dry.a * 0.5, thatch));
	// The lawn's young leaves, brighter and yellower than a lax sward's (the blades' tint).
	c *= mix(vec3(1.0), vec3(1.06, 1.16, 0.88), lawn);
	return c;
}

// Cured sward (docs/ideas/vegetation.md, "Cured turf"): the share of the turf gone to straw. The field's own season
// dryness (follow_weather: the blades' dryness, so turf and blades cure together) and, where the shared climate code
// is declared before this, the climate's dry-season cured share (the WorldContracts cured contract, wc_climate_cured:
// the terrain's cured soil cover) while the season is dry, the larger of the two. r_climate: the climate's share
// alone (the far bake already carries the season's).
float grass_turf_cured(vec3 world_pos, out float r_climate) {
	r_climate = 0.0;
#ifdef WC_CLIMATE_CODE
	// In the dry season only: the field's season dryness says when (a semi-arid sward is green after the rains).
	r_climate = wc_climate_cured(world_pos) * smoothstep(0.3, 0.7, grass_ground_dry.a);
#endif
	return max(smoothstep(0.45, 0.95, grass_ground_dry.a), r_climate);
}

// The cured turf: the blades' dry colour, shaded at the sward's base, in drifts. A deer lawn keeps more green.
vec3 grass_turf_cure(vec3 world_pos, vec3 c, float cured) {
	vec2 xz = world_pos.xz;
	// grass_ground_dry carries the sRGB dry_color (the blades take it as source_color): to linear.
	vec3 straw = pow(grass_ground_dry.rgb, vec3(2.2)) * (0.65 + 0.25 * grass_noise(xz * 1.7 + 5.1)) * (0.9 + 0.2 * grass_noise(xz * 0.7 + 3.3));
	return mix(c, straw, cured * 0.9 * (1.0 - 0.6 * grass_graze_at(world_pos).r));
}

// The turf colour at a point, cured as the season and the climate say.
vec3 grass_turf(vec3 world_pos) {
	float climate;
	return grass_turf_cure(world_pos, grass_turf_green(world_pos), grass_turf_cured(world_pos, climate));
}

// Within the blades, partial cover (a rule's fade: scree, rock, shade) as patches of turf and gaps
// of bare ground, and the soil showing between the tufts close up, not as a smooth even tint (on a
// half-covered talus the turf read as smooth streaks with no blades on them, scene 16 Nov). Past
// the blades (gone 1) the cover stays as it is: there it stands for the whole far sward.
float grass_ground_patchy(vec3 world_pos, float cover, float gone) {
	vec2 xz = world_pos.xz;
	float clump = grass_noise(xz * 0.9) * 0.6 + grass_noise(xz * 3.7 + 2.3) * 0.4;
	float patchy = smoothstep(0.0, 0.25, cover - 0.75 * (1.0 - clump));
	float tufts = mix(0.55, 1.0, smoothstep(0.3, 0.5, grass_noise(xz * 18.0 + 4.1)));
	// Full cover keeps the even turf it always had: only thinning swards break up.
	return mix(patchy * mix(1.0, tufts, 1.0 - cover), cover, gone);
}

// How much of the ground the sward stands for at a point: its cover, full where the blades have
// thinned away (the outer band), as grass_ground() weighs the turf.
float grass_ground_weight(vec3 world_pos) {
	float cover = grass_ground_cover(world_pos);
	if (cover <= 0.0) {
		return 0.0;
	}
	float gone = 0.0;
	if (grass_ground_fade.w > 0.0) {
		gone = 1.0 - clamp((grass_ground_fade.z - length(world_pos.xz - grass_ground_fade.xy)) / grass_ground_fade.w, 0.0, 1.0);
	}
	if (grass_ground_under.w > 0.0 && grass_ground_under_far.a > 0.0) {
		gone = max(gone, 1.0 - clamp((grass_ground_under.z - length(world_pos.xz - grass_ground_under.xy)) / grass_ground_under.w, 0.0, 1.0));
	}
	return grass_ground_patchy(world_pos, cover, gone) * mix(grass_ground_color.a, 1.0, gone);
}

// The ground's specular under the sward: the blades hide the soil's sheen at the grazing angles a
// field is seen at, so a meadow from afar is near-Lambertian. Without this the far meadow beyond
// the blades turned pale grey against them (grass bench --far-match: the sheen was most of the
// step at the grass edge; 0.1 matches the blades).

// The undergrowth's litter where its stands shade the sward out (PlantSpecies.litter_color):
// bracken's dead fronds as a mat of rachises and pinnae lying every way in drifts, a little
// darker in the gaps between them, so the ground under a stand is litter, not bare soil.
vec3 grass_ground_undergrowth(vec3 world_pos, vec3 albedo) {
	if (grass_ground_rect.z <= 0.0) {
		return albedo;
	}
	vec2 uv = (world_pos.xz - grass_ground_rect.xy) / grass_ground_rect.zw;
	if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
		return albedo;
	}
	vec4 l = textureLod(grass_ground_litter, uv, 0.0);
	if (l.a <= 0.0) {
		return albedo;
	}
	vec3 c = l.rgb * l.rgb / l.a;
	vec2 xz = world_pos.xz;
	float strands = grass_streaks(xz, 0.9, 3.7, vec2(6.0, 38.0), vec2(2.5, 15.0), 5.3);
	c *= 0.7 + 0.55 * strands + 0.15 * (grass_noise(xz * 0.8) - 0.5);
	return mix(albedo, c, l.a);
}

// Trodden soil: compacted, so darker and browner than loose ground (bulk density up, pores and
// the pale dry crumb gone; Liddle 1997, ch. 2), damp in the hollows feet press into it, the fines
// washed and kicked off to leave grit and small stones standing in it, and a few crushed stems
// at the edges.
vec3 grass_path_soil(vec3 world_pos, vec3 albedo, float tread) {
	vec2 xz = world_pos.xz;
	vec3 soil = mix(albedo, vec3(0.085, 0.062, 0.042), 0.65) * (0.85 + 0.3 * grass_noise(xz * 2.3 + 9.1));
	// Damp patches in the hollows: darker (water fills the pores; Lagarde 2013).
	float damp = smoothstep(0.55, 0.75, grass_noise(xz * 0.7 + 4.4) * 0.7 + grass_noise(xz * 3.1) * 0.3) * smoothstep(0.5, 0.9, tread);
	soil *= mix(1.0, 0.62, damp);
	// Small stones the feet have bared: one in half the 10 cm cells, 1.5 to 6 cm across, each its
	// own size, shade and shape (rounded, a little elongated);
	// between them only a faint fine grit.
	float bare = smoothstep(0.35, 0.85, tread);
	vec2 cell = floor(xz * 10.0);
	float st = 0.0;
	float st_c = 0.0;
	for (int j = -1; j <= 1; j++) {
		for (int i = -1; i <= 1; i++) {
			vec2 c = cell + vec2(float(i), float(j));
			float h0 = grass_hash(c * 1.31 + 7.7);
			if (h0 > 0.45) {
				continue; // Half the cells have none: fewer, larger stones.
			}
			vec2 o = vec2(grass_hash(c + 3.1), grass_hash(c + 9.4));
			vec2 d = xz * 10.0 - (c + o);
			float ang = grass_hash(c + 5.9) * 3.1416;
			d = vec2(dot(d, vec2(cos(ang), sin(ang))), dot(d, vec2(-sin(ang), cos(ang))));
			d.y *= 1.0 + 0.8 * grass_hash(c + 2.2);
			float r = mix(0.08, 0.32, pow(grass_hash(c + 4.6), 1.5)); // 1.5 to 6 cm across, mostly small.
			float k = length(d) / r;
			float in_s = 1.0 - smoothstep(0.85, 1.0, k);
			if (in_s > st) {
				st = in_s;
				st_c = grass_hash(c + 8.3);
			}
		}
	}
	vec3 stone_c = vec3(0.21, 0.19, 0.16) * (0.6 + 0.6 * st_c) * (0.85 + 0.3 * grass_noise(xz * 140.0));
	soil = mix(soil, stone_c, st * bare);
	soil *= 0.92 + 0.08 * grass_noise(xz * 90.0 + 5.3); // Fine grit.
	return mix(albedo, soil, smoothstep(0.0, 0.6, tread));
}

// Arable fields (GrassField3D.fields): the drilled soil under the crop, darker in the tramline
// wheelings, and past the tillers' band the crop's own colour and cover at its stage, so fields read
// to the horizon as a patchwork with their tramlines (Serious Sam 4's "4 million acres": the ground
// matched to the instanced crop). crop_ground_map: r the field (its slot), g the wheelings;
// crop_ground_plots, a texel a field: row 0 its colour from above and cover, row 1 its drill angle,
// row spacing and tillers' radius and band.
float crop_ground_field_slot(vec3 world_pos) {
	if (crop_ground_rect.z <= 0.0) {
		return 0.0;
	}
	vec2 uv = (world_pos.xz - crop_ground_rect.xy) / crop_ground_rect.zw;
	if (any(lessThan(uv, vec2(0.0))) || any(greaterThanEqual(uv, vec2(1.0)))) {
		return 0.0;
	}
	ivec2 size = textureSize(crop_ground_map, 0);
	return floor(texelFetch(crop_ground_map, ivec2(uv * vec2(size)), 0).r * 255.0 + 0.5);
}

float crop_ground_field(vec3 world_pos, out vec4 r_look, out vec4 r_drill, out float r_wheel) {
	r_look = vec4(0.0);
	r_drill = vec4(0.0);
	r_wheel = 0.0;
	if (crop_ground_rect.z <= 0.0) {
		return 0.0;
	}
	vec2 uv = (world_pos.xz - crop_ground_rect.xy) / crop_ground_rect.zw;
	if (any(lessThan(uv, vec2(0.0))) || any(greaterThanEqual(uv, vec2(1.0)))) {
		return 0.0;
	}
	ivec2 size = textureSize(crop_ground_map, 0);
	vec4 m = texelFetch(crop_ground_map, ivec2(uv * vec2(size)), 0);
	int slot = int(m.r * 255.0 + 0.5);
	if (slot == 0) {
		return 0.0;
	}
	r_look = texelFetch(crop_ground_plots, ivec2(slot, 0), 0);
	r_drill = texelFetch(crop_ground_plots, ivec2(slot, 1), 0);
	r_wheel = textureLod(crop_ground_map, uv, 0.0).g;
	return 1.0;
}

// The far canopy's height field (crop_ground()'s relief): the stand's top, lower between the rows
// while it is open, in clumps of tillers, with the ears' tops speckled where a pixel resolves them,
// and cut down to the ground in the tramline lanes.
float crop_canopy_h(vec2 q, vec2 nrow, float row_spacing, float H, float closed, float ear_f) {
	float u = dot(q, nrow) / max(row_spacing, 0.03);
	float rows = mix(0.55 + 0.45 * (0.5 + 0.5 * cos(u * 6.2832)), 1.0, closed * closed);
	float clump = grass_noise(q * 1.8 + 5.3);
	float ears = mix(0.5, grass_noise(q * 14.0 + 9.1), ear_f);
	vec2 uv = (q - crop_ground_rect.xy) / crop_ground_rect.zw;
	float lane = textureLod(crop_ground_map, uv, 0.0).g;
	return H * (0.74 + 0.16 * clump + 0.16 * ears) * rows * (1.0 - 0.95 * lane);
}

vec3 crop_ground(vec3 world_pos, vec3 albedo) {
	vec4 look;
	vec4 drill;
	float wheel;
	if (crop_ground_field(world_pos, look, drill, wheel) <= 0.0) {
		return albedo;
	}
	vec2 xz = world_pos.xz;
	// Tilled soil: a fine crumb, the drill's slots as faint lines along the rows.
	vec2 d = vec2(cos(drill.x), sin(drill.x));
	float u = dot(xz, vec2(-d.y, d.x)) / max(drill.y, 0.03);
	float slot_line = smoothstep(0.35, 0.0, abs(fract(u) - 0.5) - 0.3);
	vec3 soil = albedo * (0.9 + 0.2 * grass_noise(xz * 6.0)) * mix(1.0, 0.85, slot_line);
	// Wheelings: compacted, darker, a little smoother.
	soil = mix(soil, albedo * vec3(0.85, 0.82, 0.8), wheel * 0.6);
	// Close up the tillers stand on it and shade it. Further out the coarser levels keep fewer of
	// them (a half, a quarter, an eighth) and the ground takes the colour of the ones dropped, as the
	// grass ground does under thinned blades; past their band the crop's colour covers it all.
	float dist = length(xz - crop_ground_fade.xy);
	float gone = 0.0;
	float dropped = 0.0;
	if (drill.w > 0.0) {
		gone = crop_ground_fade.w < 0.0 ? 1.0 : 1.0 - clamp((drill.z - dist) / drill.w, 0.0, 1.0);
		float radius = drill.z / 0.97;
		dropped = smoothstep(0.15, 0.85, dist / radius);
	}
	// The crop as a textured canopy, not a flat tint: patches of uneven growth and the wind's sheen
	// at metres, fine speckle of heads and leaves, and the drill rows as stripes with soil between
	// while the canopy is open; each faded to its mean as it drops under a pixel.
	float patchy = grass_noise(xz * 0.12 + 1.7) * 0.6 + grass_noise(xz * 0.45 + 8.1) * 0.4;
	float speck = mix(grass_noise(xz * 7.0 + 2.2), 0.5, smoothstep(0.3, 1.0, length(fwidth(xz * 7.0))));
	float row_show = (1.0 - smoothstep(0.25, 0.6, fwidth(u))) * (1.0 - 0.8 * smoothstep(0.5, 0.95, look.a));
	float rows = mix(1.0, smoothstep(0.1, 0.6, 0.5 + 0.5 * cos(u * 6.2832)), row_show);
	// Clumps of ears and lodged patches at about half a metre, still resolved some way out.
	float clump = mix(grass_noise(xz * 1.8 + 5.3), 0.5, smoothstep(0.4, 1.0, length(fwidth(xz * 1.8))));
	// A closed, ripe stand is even from afar (the ears' layer hides the stems): its patches and
	// clumps are faint; an open young crop shows its uneven growth more.
	float even = clamp((look.a - 0.6) / 0.35, 0.0, 1.0);
	vec3 canopy = look.rgb * (1.0 + (0.44 * patchy - 0.22) * mix(1.0, 0.25, even)) * (0.8 + 0.4 * speck) * (1.0 + (0.4 * clump - 0.2) * mix(1.0, 0.5, even));
	// Down-sun the lit faces of ears and leaves, into the sun their shaded sides (the tillers' bake).
	vec2 to_far = xz - crop_ground_fade.xy;
	float to_far_len = length(to_far);
	if (crop_ground_sun.z > 0.0 && to_far_len > 1e-3) {
		float c1 = dot(to_far / to_far_len, crop_ground_sun.xy);
		canopy *= max(1.0 + crop_ground_sun.z * (crop_ground_lobe1.rgb * c1 + crop_ground_lobe2.rgb * (2.0 * c1 * c1 - 1.0)), vec3(0.1));
	}
	// Wheelings: the crop is missing there, soil shows as narrow lines through it.
	float cover = look.a * (1.0 - wheel * 0.6);
	// Seen at a slant the soil's gaps close: the gap fraction falls as exp(-k LAI / cos(view
	// zenith)) (Nilson 1971), so (1 - cover) goes to the power 1 / cos.
	float mu = 1.0;
	if (crop_ground_fade.w != 0.0) {
		vec3 to_eye = vec3(crop_ground_fade.x, crop_ground_fade.z, crop_ground_fade.y) - world_pos;
		mu = clamp(to_eye.y / max(length(to_eye), 1e-3), 0.05, 1.0);
		cover = 1.0 - pow(1.0 - clamp(cover, 0.0, 0.999), 1.0 / mu);
	}
	vec3 shaded = soil * mix(1.0, 0.55, look.a);
	vec3 near = mix(shaded, canopy, cover * dropped);
	// The rows' soil stripes close up too as the view slants along the ground.
	vec3 far = mix(shaded, canopy, cover * mix(rows, 1.0, max(look.a * look.a, 1.0 - mu * mu)));
	// Tramlines in a standing crop are lanes between walls of it: soil in their shade, darker
	// than the canopy round them, not pale stripes over it.
	far = mix(far, mix(canopy * 0.55, soil * 0.75, 0.6), wheel * look.a * mu * (1.0 - smoothstep(0.1, 0.4, length(fwidth(xz)))));
	vec4 sw = texelFetch(crop_ground_plots, ivec2(int(crop_ground_field_slot(world_pos)), 2), 0);
	// Past the tillers' band the stand as a relief: the view ray marched back from the ground
	// through a height field of the canopy (parallax occlusion mapping, Tatarchuk 2006; relief
	// mapping, Policarpo et al. 2005), so a far field keeps its height: the ears' tops lit, the
	// stems below them in their shade, rows, clumps and tramline lanes cut into it. The ground is
	// the base, the canopy rises above it: the first point along the ray (from the eye) under the
	// canopy's top is what the pixel sees.
	float H = sw.w;
	if (gone > 0.0 && H > 0.05 && crop_ground_fade.w != 0.0) {
		vec3 eye = vec3(crop_ground_fade.x, crop_ground_fade.z, crop_ground_fade.y);
		vec3 v = world_pos - eye;
		float vl = length(v);
		v /= max(vl, 1e-3);
		float dv = -v.y;
		if (dv > 0.004) {
			vec2 nrow = vec2(-d.y, d.x);
			float s_max = min(H / dv, min(30.0, vl * 0.9));
			float px_m = length(fwidth(xz)); // The pixel's footprint (m), outside any branch.
			// Across the view the footprint is the distance times the pixel's angle; along it the
			// slant stretches it. The grain follows the narrow side, or a far field turns blotchy.
			float px_lat = vl * length(fwidth(v)) * 0.7;
			// Across a tramline (it runs along the rows): seen side on, a lane's width lies along
			// the view's slant, where the footprint is long.
			float px_lane = fwidth(dot(xz, vec2(-d.y, d.x)));
			float ear_f = 1.0 - smoothstep(0.03, 0.12, px_m);
			float closed = clamp((look.a - 0.3) / 0.65, 0.0, 1.0);
			const int STEPS = 24;
			float hit_s = -1.0;
			float prev_s = s_max;
			float prev_d = -H;
			for (int i = 0; i <= STEPS; i++) {
				float st = s_max * (1.0 - float(i) / float(STEPS));
				float dd = crop_canopy_h(xz - v.xz * st, nrow, drill.y, H, closed, ear_f) - st * dv;
				if (dd >= 0.0) {
					hit_s = mix(prev_s, st, prev_d / min(prev_d - dd, -1e-5));
					break;
				}
				prev_s = st;
				prev_d = dd;
			}
			// Down a lane to its soil, part in its walls' shade, while a wheel track (half a metre)
			// spans a few pixels; past that the pixel holds the lane's walls and tops as well, and
			// its mean is near the canopy's own (a far tramline is a faint line, not a black one).
			float lane_res = 1.0 - smoothstep(0.08, 0.3, px_lane);
			vec3 relief = mix(canopy * 0.8, mix(soil, canopy, 0.3) * mix(1.0, 0.75, look.a), lane_res); // Lit from the sky down the lane, and the straw's bounce.
			if (hit_s >= 0.0) {
				vec2 q = xz - v.xz * hit_s;
				float depth = clamp(hit_s * dv / H, 0.0, 1.0);
				// The ears' grain at the pixel's own scale: octaves of noise from the ears' size
				// (14 per metre) down, each kept only below its Nyquist limit and only while no finer
				// one is resolved (band-limited procedural noise; Lagae et al. 2010), down to 3.5 per metre and no coarser, so a far field
				// keeps the near stalks' speckle contrast instead of fading to a smooth blur (the
				// first fields frame's seam where the tillers end).
				float grain = 0.0;
				float fq = 14.0;
				float finer = 0.0;
				for (int k = 0; k < 3; k++) {
					float a = 1.0 - smoothstep(0.35, 0.7, px_lat * fq);
					grain += a * (1.0 - finer) * (grass_noise(q * fq + float(k) * 3.7 + 9.1) - 0.5);
					finer = max(finer, a);
					fq *= 0.5;
				}
				// Lit at the top, in the stand's own shade lower down (its light falls about as
				// exp(-LAI) to the base).
				relief = canopy * max(1.0 + 1.3 * grain, 0.3) * mix(mix(0.42, 0.7, 1.0 - lane_res), 1.3, depth * depth); // Deep hits (a lane's walls) lifted to the pixel's mean where the lane is unresolved.
			}
			far = mix(far, relief * crop_ground_view.rgb * texelFetch(crop_ground_plots, ivec2(int(crop_ground_field_slot(world_pos)), 3), 0).rgb, gone);
		}
	}
	// Stubble: the combine's swaths of straw down each pass, paler and fuller than the stubble.
	if (sw.z > 0.0) {
		float um = dot(xz, vec2(-d.y, d.x));
		float vm = dot(xz, d);
		float sway = 0.25 * sin(vm * 0.05 + floor(um / sw.x) * 1.7) + 0.1 * sin(vm * 0.21 + floor(um / sw.x) * 0.3);
		float across = abs(mod(um - sway, sw.x) - 0.5 * sw.x) / sw.y;
		float heap = clamp(1.0 - across * across, 0.0, 1.0) * (0.75 + 0.5 * speck);
		vec3 straw = look.rgb * 1.3 * (0.8 + 0.4 * patchy);
		far = mix(far, straw, smoothstep(0.0, 1.0, heap) * 0.5);
		// Harvest stripes: the combine goes up one pass and back down the next, laying the stubble
		// and the chaff over each way, so alternate passes catch the light differently.
		float pass_side = smoothstep(0.35, 0.65, abs(fract(um / (2.0 * sw.x)) * 2.0 - 1.0));
		far *= mix(0.86, 1.1, pass_side);
		near = mix(near, straw * 0.55, smoothstep(0.0, 0.5, heap) * (1.0 - dropped));
	}
	return mix(near, far, gone);
}

float grass_ground_specular(vec3 world_pos, float specular) {
	// Trodden soil: a dull, fine crumb (the bench's pale grey tread was its sheen).
	specular = mix(specular, min(specular, 0.2), grass_ground_tread(world_pos));
	// A crop's canopy scatters its leaves' sheen every way: no sheen off the ground plane under it.
	vec4 look;
	vec4 drill;
	float wheel;
	if (crop_ground_field(world_pos, look, drill, wheel) > 0.0) {
		specular = mix(specular, min(specular, 0.1), look.a * (1.0 - wheel * 0.6));
	}
	return mix(specular, min(specular, 0.1), grass_ground_weight(world_pos));
}

// The ground's albedo under the vegetation: litter where the canopy closes (forest_floor()), the
// undergrowth's litter under its stands, then the sward's turf where grass grows.
vec3 grass_ground(vec3 world_pos, vec3 albedo) {
	albedo = forest_floor(world_pos, albedo);
	albedo = crop_ground(world_pos, albedo);
	albedo = grass_ground_undergrowth(world_pos, albedo);
	float tread = grass_ground_tread(world_pos);
	if (tread > 0.0) {
		albedo = grass_path_soil(world_pos, albedo, tread);
	}
	float cover = grass_ground_cover(world_pos);
	if (cover <= 0.0) {
		return albedo;
	}
	// Where the blades have thinned away (the blade shader's outer band) the turf becomes the
	// blades' own colour with their patch tint, fully covering, so the field reads to the horizon.
	float gone = 0.0;
	if (grass_ground_fade.w > 0.0) {
		gone = 1.0 - clamp((grass_ground_fade.z - length(world_pos.xz - grass_ground_fade.xy)) / grass_ground_fade.w, 0.0, 1.0);
	}
	// Past the undercoat's own band (a narrower field under a patchy tall sward) its blades are
	// gone too: the ground between the tall patches becomes the undercoat's far colour.
	float gone_u = 0.0;
	if (grass_ground_under.w > 0.0 && grass_ground_under_far.a > 0.0) {
		gone_u = 1.0 - clamp((grass_ground_under.z - length(world_pos.xz - grass_ground_under.xy)) / grass_ground_under.w, 0.0, 1.0);
	}
	vec3 turf_green = grass_turf_green(world_pos);
	float cured_climate;
	float cured = grass_turf_cured(world_pos, cured_climate);
	vec3 turf = grass_turf_cure(world_pos, turf_green, cured);
	if (gone > 0.0 || gone_u > 0.0) {
		vec2 xz = world_pos.xz;
		vec3 tint = grass_patch_tint(xz, grass_noise(xz * 3.0), 0.5, grass_ground_params.x);
		// A sward looked at down-sun shows its lit faces (the hot spot), into the sun its shadowed
		// sides: the bake's azimuth lobes, by the angle between the view and the sun's light.
		vec3 lobe = vec3(1.0);
		vec2 to = xz - grass_ground_view.xz;
		float to_len = length(to);
		if (grass_ground_sun.z > 0.0 && to_len > 1e-3) {
			float c1 = dot(to / to_len, grass_ground_sun.xy);
			lobe = max(1.0 + grass_ground_sun.z * (grass_ground_far_lobe1.rgb * c1 + grass_ground_far_lobe2.rgb * (2.0 * c1 * c1 - 1.0)), vec3(0.1));
		}
		// Keep the turf's streaks and thatch as brightness, so the far sward is not flat.
		float lum = clamp(dot(turf_green, vec3(0.3, 0.59, 0.11)) / max(dot(grass_ground_color.rgb, vec3(0.3, 0.59, 0.11)), 1e-3), 0.5, 1.5);
		vec3 under = mix(grass_ground_under_far.rgb, pow(grass_ground_dry.rgb, vec3(2.2)) * 0.8, cured_climate * 0.9) * tint * lobe;
		if (gone_u > 0.0) {
			turf = mix(turf, under * lum, gone_u);
		}
		if (gone > 0.0) {
			vec3 far = grass_ground_far.rgb * tint * lobe;
			if (grass_ground_under_far.a > 0.0) {
				far = mix(under, far, grass_ground_far.a); // The tall patches over the undercoat.
			}
			// The bake carries the season's dryness; the climate's cured share over it, as on the turf.
			far = mix(far, pow(grass_ground_dry.rgb, vec3(2.2)) * 0.8 * tint * lobe, cured_climate * 0.9);
			turf = mix(turf, far * lum, gone);
		}
		// Hay meadows: what the far blades would show of the stubble and the rows.
		turf = mix(turf, grass_mow_ground(world_pos, turf), max(gone, gone_u));
	}
	gone = max(gone, gone_u);
	// A deer lawn is a closed carpet: bitten short, the grasses tiller densely (20,000 to 40,000 tillers per m2 under
	// close grazing against 5,000 to 10,000 in a lax sward; Hodgson 1990, Grant et al. 1983), so between the short
	// blades the ground is the turf's own, not soil or litter. Rooting (G) tears it back to the soil.
	vec2 lawn = grass_graze_at(world_pos);
	float closed = lawn.r * (1.0 - lawn.g);
	float w = grass_ground_patchy(world_pos, cover, max(gone, closed)) * mix(grass_ground_color.a, 1.0, max(gone, closed));
	return mix(albedo, turf, w * (1.0 - lawn.g));
}
)";
	return code;
}

void GrassField3D::set_blade_segments(int p_segments) {
	blade_segments = CLAMP(p_segments, 1, 8);
	rebuild();
	_queue_far_bake();
}

void GrassField3D::set_view_facing(float p_facing) {
	view_facing = CLAMP(p_facing, 0.0f, 1.0f);
	_queue_far_bake();
	if (material.is_valid()) {
		material->set_shader_parameter("view_facing", view_facing);
	}
}

void GrassField3D::set_still_shot(bool p_enabled) {
	still_shot = p_enabled;
	still_frozen = false;
	still_quiet = 0;
}

void GrassField3D::set_fibre_shading(float p_amount) {
	fibre_shading = CLAMP(p_amount, 0.0f, 1.0f);
	if (material.is_valid()) {
		material->set_shader_parameter("fibre_shading", fibre_shading);
	}
}

void GrassField3D::set_ground_tint(const Color &p_color) {
	ground_tint = p_color;
	ground_baked = false;
	_queue_far_bake();
}

void GrassField3D::set_ground_tint_enabled(bool p_enabled) {
	ground_tint_enabled = p_enabled;
	ground_baked = false;
	_queue_far_bake();
	if (!p_enabled) {
		_drop_under();
	}
	if (!p_enabled && is_inside_tree()) {
		RenderingServer::get_singleton()->global_shader_parameter_set("grass_ground_rect", Vector4());
	}
}

// Trample field. Texels are world aligned, so a moving centre scrolls the field without smearing it.

void GrassField3D::_trample_recenter(const Vector3 &p_center) {
	const int res = trample_resolution;
	const float texel = trample_size / res;
	const Vector2i origin(int(Math::floor(p_center.x / texel)) - res / 2, int(Math::floor(p_center.z / texel)) - res / 2);
	if ((int)trample.size() != res * res) {
		trample.resize(res * res);
		for (Vector2 &b : trample) {
			b = Vector2();
		}
		trample_origin = origin;
		return;
	}
	if (origin == trample_origin) {
		return;
	}
	const Vector2i shift = origin - trample_origin;
	LocalVector<Vector2> moved;
	moved.resize(res * res);
	for (int z = 0; z < res; z++) {
		for (int x = 0; x < res; x++) {
			const int sx = x + shift.x;
			const int sz = z + shift.y;
			moved[z * res + x] = (sx >= 0 && sx < res && sz >= 0 && sz < res) ? trample[sz * res + sx] : Vector2();
		}
	}
	trample = moved;
	trample_origin = origin;
}

void GrassField3D::add_trample(const Vector3 &p_position, float p_radius, const Vector3 &p_velocity, float p_strength) {
	const int res = trample_resolution;
	if ((int)trample.size() != res * res || p_radius <= 0.0f) {
		return;
	}
	const float texel = trample_size / res;
	const float reach = p_radius * 1.25f;
	const Vector2 c(p_position.x, p_position.z);
	const Vector2 v(p_velocity.x, p_velocity.z);
	const float speed = v.length();
	const Vector2 vd = speed > 1e-3f ? v / speed : Vector2();
	const int x0 = MAX(int(Math::floor((c.x - reach) / texel)) - trample_origin.x, 0);
	const int x1 = MIN(int(Math::floor((c.x + reach) / texel)) - trample_origin.x, res - 1);
	const int z0 = MAX(int(Math::floor((c.y - reach) / texel)) - trample_origin.y, 0);
	const int z1 = MIN(int(Math::floor((c.y + reach) / texel)) - trample_origin.y, res - 1);
	for (int z = z0; z <= z1; z++) {
		for (int x = x0; x <= x1; x++) {
			const Vector2 q((trample_origin.x + x + 0.5f) * texel, (trample_origin.y + z + 0.5f) * texel);
			const Vector2 rel = q - c;
			const float d = rel.length();
			if (d >= reach) {
				continue;
			}
			// Pressed flat under the mover, fading over its rim.
			const float f = 1.0f - Math::smoothstep(p_radius * 0.6f, reach, d);
			// Blades fall away from the mover and forward along its motion (Jahrmann and Wimmer).
			Vector2 dir = (d > 1e-3f ? rel / d : Vector2()) * 0.6f + vd * MIN(speed, 1.0f);
			if (dir.length() < 1e-3f) {
				dir = Vector2(1.0f, 0.0f);
			}
			const Vector2 target = dir.normalized() * f * CLAMP(p_strength, 0.0f, 1.0f);
			Vector2 &b = trample[z * res + x];
			if (target.length_squared() > b.length_squared()) {
				b = target;
				trample_active = true;
			}
		}
	}
}

Vector2 GrassField3D::get_trample_at(const Vector3 &p_position) const {
	const int res = trample_resolution;
	if ((int)trample.size() != res * res) {
		return Vector2();
	}
	const float texel = trample_size / res;
	const int x = int(Math::floor(p_position.x / texel)) - trample_origin.x;
	const int z = int(Math::floor(p_position.z / texel)) - trample_origin.y;
	if (x < 0 || x >= res || z < 0 || z >= res) {
		return Vector2();
	}
	return trample[z * res + x];
}

void GrassField3D::clear_trample() {
	for (Vector2 &b : trample) {
		b = Vector2();
	}
	trample_active = true; // Upload the cleared field once.
}

Vector2 GrassField3D::_mover_extent(Node3D *p_node) const {
	return WorldContracts::body_extent(p_node, trample_radius);
}

void GrassField3D::_trample_step(double p_delta) {
	TerrainDataMap3D *map = Object::cast_to<TerrainDataMap3D>(get_node_or_null(data_map));
	if (map && !map->is_baked()) {
		map = nullptr;
	}
	// Recovery: every texel relaxes towards upright with the given half-life.
	if (trample_active) {
		const float keep = trample_recovery > 0.0f ? Math::pow(0.5f, float(p_delta) / trample_recovery) : 0.0f;
		float largest = 0.0f;
		for (Vector2 &b : trample) {
			b *= keep;
			largest = MAX(largest, b.length_squared());
		}
		if (largest < 1e-6f) {
			for (Vector2 &b : trample) {
				b = Vector2();
			}
			trample_active = false;
			_trample_upload(); // Upload the relaxed field once.
		}
	}
	// Movers: any Node3D in the group whose underside is within the grass.
	const float top = MAX(blade_height.x, blade_height.y);
	const float ground_y = get_global_position().y;
	HashMap<ObjectID, Vector3> seen;
	if (is_inside_tree()) {
		// The trample group and world_contact, every body any system should feel (docs/ideas/world-links.md).
		Vector<Node *> movers = get_tree()->get_nodes_in_group(trample_group);
		movers.append_array(get_tree()->get_nodes_in_group(SNAME("world_contact")));
		for (Node *n : movers) {
			Node3D *n3 = Object::cast_to<Node3D>(n);
			if (!n3 || !n3->is_visible_in_tree() || seen.has(n3->get_instance_id())) {
				continue;
			}
			const Vector3 pos = n3->get_global_position();
			const Vector3 *last = trample_last.getptr(n3->get_instance_id());
			Vector3 vel = (last && p_delta > 0.0) ? (pos - *last) / float(p_delta) : Vector3();
			if (vel.length_squared() > 30.0f * 30.0f) {
				vel = Vector3(); // Teleported, not moving.
			}
			seen.insert(n3->get_instance_id(), pos);
			const Vector2 ext = _mover_extent(n3);
			const float ground = map ? map->get_height_at(pos) : ground_y;
			if (pos.y - ext.y - ground > top) {
				continue; // Above the grass.
			}
			add_trample(pos, ext.x, vel);
		}
	}
	trample_last = seen;
	// The current in any water over the sward lays it downstream (WaterCurrentBend, Luhar and Nepf 2011).
	if ((int)trample.size() == trample_resolution * trample_resolution) {
		water_current.update(trample_origin, trample_resolution, trample_size / trample_resolution, p_delta, [&](const Vector3 &p) { return map ? map->get_height_at(p) : ground_y; });
		trample_active = water_current.apply(trample, trample_resolution) || trample_active;
	}
	if (trample_active) {
		_trample_upload();
	}
}

void GrassField3D::_trample_upload() {
	if (material.is_null()) {
		return;
	}
	const int res = trample_resolution;
	RenderingServer *rs = RenderingServer::get_singleton();
	if (!trample_enabled || (int)trample.size() != res * res) {
		material->set_shader_parameter("trample_rect", Vector4());
		rs->global_shader_parameter_set("grass_trample_rect", Vector4());
		return;
	}
	Vector<uint8_t> data;
	data.resize(res * res * 4);
	uint16_t *w = (uint16_t *)data.ptrw();
	for (int i = 0; i < res * res; i++) {
		w[i * 2 + 0] = Math::make_half_float(trample[i].x);
		w[i * 2 + 1] = Math::make_half_float(trample[i].y);
	}
	if (trample_image.is_valid() && trample_image->get_width() == res) {
		trample_image->set_data(res, res, false, Image::FORMAT_RGH, data);
	} else {
		trample_image = Image::create_from_data(res, res, false, Image::FORMAT_RGH, data);
	}
	if (trample_texture.is_valid() && trample_texture->get_width() == res) {
		trample_texture->update(trample_image);
	} else {
		trample_texture = ImageTexture::create_from_image(trample_image);
	}
	if (material->get_shader_parameter("trample_map") != Variant(trample_texture)) {
		material->set_shader_parameter("trample_map", trample_texture);
	}
	const float texel = trample_size / res;
	const Vector4 trect(trample_origin.x * texel, trample_origin.y * texel, trample_size, trample_size);
	material->set_shader_parameter("trample_rect", trect);
	// The same field for every other plant (PlantGenerator's shader): one trample for the ground cover.
	rs->global_shader_parameter_set("grass_trample_map", trample_texture->get_rid());
	rs->global_shader_parameter_set("grass_trample_rect", trect);
}

void GrassField3D::set_trample_enabled(bool p_enabled) {
	trample_enabled = p_enabled;
	if (!trample_enabled) {
		trample.clear();
		trample_last.clear();
		trample_active = false;
	}
	_trample_upload();
}

void GrassField3D::set_trample_size(float p_size) {
	trample_size = MAX(p_size, 1.0f);
	trample.clear();
}

void GrassField3D::set_trample_resolution(int p_resolution) {
	trample_resolution = CLAMP(p_resolution, 8, 2048);
	trample.clear();
}

void GrassField3D::update_at(const Vector3 &p_center) {
	if (still_shot && still_frozen) {
		if (p_center.distance_to(still_at) < 0.5f && generation == still_generation) {
			return;
		}
		still_frozen = false;
		still_quiet = 0;
	}
	const uint32_t gen_before = generation;
	_update_tiles(p_center);
	if (still_shot) {
		// Complete once a few frames in a row built nothing.
		still_quiet = (last_built == 0 && generation == gen_before) ? still_quiet + 1 : 0;
		if (still_quiet >= 10) {
			still_frozen = true;
			still_at = p_center;
			still_generation = generation;
		}
	}
	if (trample_enabled) {
		const Vector2i before = trample_origin;
		const bool fresh = (int)trample.size() != trample_resolution * trample_resolution || material->get_shader_parameter("trample_map").get_type() == Variant::NIL;
		_trample_recenter(p_center);
		if (fresh || before != trample_origin) {
			_trample_upload();
		}
	}
}

// A setting changed: new blades, material and ground tint. The old tiles stay drawn (they hold
// the old mesh and material) and are replaced nearest first within max_tiles_per_frame, each
// once its successor is built, so a change never empties the field or stalls a frame on a full
// rebuild.
void GrassField3D::rebuild() {
	generation++;
	blade_mesh.unref();
	crop_mesh_far.unref();
	material.unref();
	ground_baked = false;
	burn_read = false;
}

void GrassField3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			if (!Engine::get_singleton()->is_editor_hint()) {
				set_process_internal(true);
				_season_step(); // The season's look from the first frame, not a frame later.
				_queue_far_bake();
			}
		} break;
		case NOTIFICATION_ENTER_TREE: {
			add_to_group(SNAME("grass_trample_field")); // keeps the trample field (VegetationField3D stands in without one)
			_hay_register();
			if (!meadows.is_empty()) {
				_queue_bales();
			}
			if (crop != CROP_NONE) {
				if (!crop_nodes.has(this)) {
					crop_nodes.push_back(this);
					_crop_fields_changed_notify();
				}
				if (hedge_nodes.is_empty()) {
					_queue_hedges();
				}
			} else {
				add_to_group(SNAME("vegetation_crop_aware"));
				crop_mask = get_crop_field_mask();
			}
		} break;
		case NOTIFICATION_EXIT_TREE: {
			if (mow_published) {
				RenderingServer::get_singleton()->global_shader_parameter_set("grass_mow_rect", Vector4());
				mow_published = false;
			}
			if (hay_nodes.has(this)) {
				hay_nodes.erase(this);
				hay_generation++;
				if (hay_nodes.is_empty()) {
					bale_meshes.clear(); // With the tree, not at exit after the rendering server.
				}
			}
			if (plant_color_registered) {
				WorldContracts::unregister_plant_color(this);
				plant_color_registered = false;
			}
			if (crop_nodes.has(this)) {
				crop_nodes.erase(this);
				_crop_fields_changed_notify();
				if (crop_nodes.is_empty()) {
					hedge_shell_cache.clear();
					crop_slot_view.clear();
					// The last crop node: no fields' ground left, and the shared textures go with the
					// tree (not at exit, after the rendering server).
					RenderingServer *rs = RenderingServer::get_singleton();
					if (rs) {
						rs->global_shader_parameter_set("crop_ground_rect", Vector4());
					}
					crop_ground_texture.unref();
					crop_plots_texture.unref();
					crop_plots_dirty = true;
				}
			}
		} break;
		case NOTIFICATION_PREDELETE: {
			for (KeyValue<Vector2i, Tile> &kv : tiles) {
				_free_tile(kv.value);
			}
			tiles.clear();
			_free_rings();
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			Viewport *vp = get_viewport();
			Camera3D *cam = vp ? vp->get_camera_3d() : nullptr;
			if (cam) {
				update_at(cam->get_global_position());
				if (fade_owner == this) {
					_publish_sun_view(cam->get_global_position());
					_far_view_step(cam);
				} else if (_is_crop_view()) {
					_far_view_step(cam);
				}
			}
			if (trample_enabled && !trample.is_empty() && !still_frozen) {
				_trample_step(get_process_delta_time());
			}
			if (lodging && cam) {
				const WorldWeather *wm = WorldContracts::weather();
				if (wm->has_provider()) {
					lodge_clock += get_process_delta_time() * wm->time_scale();
					_lodge_place(cam->get_global_position());
					_lodge_rows(LODGE_ROWS_PER_FRAME, cam->get_global_position());
				}
			}
			if (flood_wash && cam) {
				_flood_step(cam->get_global_position(), false);
			}
			_season_step();
			if (far_bake_vp) {
				_far_bake_step();
			} else if (far_bake_queued && far_bake) {
				_far_bake_start();
			}
		} break;
	}
}

GrassField3D::~GrassField3D() {
	_drop_under();
	hay_nodes.erase(this);
	if (crop_nodes.has(this)) {
		crop_nodes.erase(this);
		crop_ground_dirty = true;
	}
	if (fade_owner == this) {
		fade_owner = nullptr;
		if (RenderingServer::get_singleton()) {
			RenderingServer::get_singleton()->global_shader_parameter_set("grass_ground_fade", Vector4());
			RenderingServer::get_singleton()->global_shader_parameter_set("grass_ground_sun", Vector4());
		}
	}
	// Tile instances are internal children: Node frees them before this runs.
}

#define GRASS_SET_REBUILD(m_name, m_value) \
	m_name = m_value; \
	rebuild();

void GrassField3D::set_canopy_field(const NodePath &p_path) {
	GRASS_SET_REBUILD(canopy_field, p_path)
}

void GrassField3D::set_undergrowth_field(const NodePath &p_path) {
	GRASS_SET_REBUILD(undergrowth_field, p_path)
}

// The generator's holes, followed once found through the data map: the tiles over a changed hole are
// built again (nearest first, the old ones drawn until then).
void GrassField3D::_follow_holes(TerrainDataMap3D *p_map) {
	TerrainGenerator3D *gen = VegetationHoles::source_of(p_map);
	if (!gen || gen->get_instance_id() == hole_source) {
		return;
	}
	const Callable on_holes = callable_mp(this, &GrassField3D::_on_holes_changed);
	if (Object *old = ObjectDB::get_instance(hole_source)) {
		if (old->is_connected(SNAME("holes_changed"), on_holes)) {
			old->disconnect(SNAME("holes_changed"), on_holes);
		}
	}
	hole_source = gen->get_instance_id();
	gen->connect(SNAME("holes_changed"), on_holes);
	holes.read(gen);
	if (!holes.is_empty()) {
		generation++; // Holes cut before this field found them.
	}
}

void GrassField3D::_on_holes_changed(const Rect2 &p_rect) {
	holes.read(Object::cast_to<TerrainGenerator3D>(ObjectDB::get_instance(hole_source)));
	const Rect2 near = p_rect.grow(OBSTACLE_MARGIN);
	for (KeyValue<Vector2i, Tile> &kv : tiles) {
		if (Rect2(Vector2(kv.key) * tile_size, Vector2(tile_size, tile_size)).intersects(near)) {
			kv.value.generation = generation - 1;
		}
	}
}

// A blade's root in an opening of the ground or at its edge.
bool GrassField3D::_blade_in_hole(const TerrainDataMap3D *p_map, float p_x, float p_z, float p_ground_y) const {
	const float y = p_map ? p_map->get_height_at(Vector3(p_x, 0.0f, p_z)) : p_ground_y;
	return holes.at(Vector3(p_x, y, p_z), OBSTACLE_MARGIN);
}

// Whether any rock reaches into a tile (so its blades need testing one by one).
bool GrassField3D::_tile_has_obstacles(float p_x0, float p_z0) const {
	if (obstacle_grid.is_empty()) {
		return false;
	}
	const float half = tile_size * 0.5f;
	const float reach = half * float(Math::SQRT2) + OBSTACLE_MARGIN;
	return obstacle_grid.clearance(Vector2(p_x0 + half, p_z0 + half), reach) <= reach;
}

// Trampling of the sward (Cole 1995), as PlantSpecies.trample_cover() and trample_scale().
float GrassField3D::_trample_cover(float p_passes) const {
	if (trample_resistance <= 0.0f || p_passes <= 0.0f) {
		return 1.0f;
	}
	const float x = p_passes / trample_resistance;
	// Past about four times its resistance nothing stands: the tread is bare.
	return (1.0f - Math::smoothstep(2.5f, 5.0f, x)) / (1.0f + x * x);
}

float GrassField3D::_trample_scale(float p_passes) const {
	if (trample_resistance <= 0.0f || p_passes <= 0.0f) {
		return 1.0f;
	}
	return Math::lerp(1.0f, 0.45f, p_passes / (p_passes + trample_resistance));
}

// Game trails (WorldStand.set_trails, from VegetationField3D.make_game_trails): worn like the field's own paths.
// True when they changed.
bool GrassField3D::_read_world_trails() {
	const WorldStand *stand = WorldContracts::stand();
	if (!stand) {
		return false;
	}
	trails_seen = stand->get_stand_generation();
	const Array now = stand->get_trails();
	if (now == world_trails) {
		return false;
	}
	world_trails = now.duplicate(true);
	path_grid.build(VegetationPaths::merged(paths, world_trails));
	return true;
}

void GrassField3D::set_paths(const Array &p_paths) {
	paths = p_paths;
	path_grid.build(VegetationPaths::merged(paths, world_trails));
	ground_baked = false; // The ground tint opens on the paths too.
	rebuild();
}

void GrassField3D::set_trample_resistance(float p_passes) {
	trample_resistance = MAX(p_passes, 0.0f);
	rebuild();
}

void GrassField3D::set_obstacles(const PackedVector4Array &p_obstacles) {
	obstacles = p_obstacles;
	_rebuild_obstacle_grid();
	rebuild();
}

void GrassField3D::_rebuild_obstacle_grid() {
	PackedVector4Array all = obstacles;
	all.append_array(mound_obstacles);
	rock_cache.gather_all(all);
	obstacle_grid.build(all);
}

// The rock field's stones are fetched again from scratch, as the tiles are built again.
void GrassField3D::set_rock_field(const NodePath &p_path) {
	rock_field = p_path;
	rock_cache.clear();
	_rebuild_obstacle_grid();
	rebuild();
}

void GrassField3D::set_rock_obstacle_min(float p_size) {
	rock_obstacle_min = MAX(p_size, 0.0f);
	set_rock_field(rock_field);
}

void GrassField3D::set_rock_footprint_max(float p_radius) {
	rock_footprint_max = MAX(p_radius, 0.0f);
}

void GrassField3D::_rocks_for(const Rect2 &p_rect) {
	if (rock_field.is_empty() || !is_inside_tree()) {
		return;
	}
	Node *rf = get_node_or_null(rock_field);
	if (!rf || !rf->has_method(SNAME("get_obstacles"))) {
		return;
	}
	// A blade tests the rocks within OBSTACLE_MARGIN of its root (and a tile, within its half diagonal of its centre):
	// every stone whose centre is within the widest footprint of the rect.
	PackedVector4Array fresh;
	if (rock_cache.fetch(rf, p_rect.grow(rock_footprint_max + OBSTACLE_MARGIN + 1.0f), rock_obstacle_min, &fresh)) {
		if (rock_cache.max_radius > rock_footprint_max) {
			WARN_PRINT_ONCE(vformat("GrassField3D: a stone %.1f m wide at its foot, wider than rock_footprint_max (%.1f m): raise it.", rock_cache.max_radius, rock_footprint_max));
		}
		obstacle_grid.add(fresh); // Main thread: the tiles are built here.
	}
}

// The mounds the fields have placed near the blades, as obstacle discs (x, y, z, radius): each species with the meta
// "sward_mound" (its radius at scale 1) at its smallest scale, so the turf round a mound meets its foot. Polled like
// the trails; only the tiles over mounds that came or went are built again.
bool GrassField3D::_follow_mounds() {
	PackedVector4Array now;
	if (is_inside_tree()) {
		const VegetationField3D *fields[2] = { _get_undergrowth(), canopy_field.is_empty() ? nullptr : Object::cast_to<VegetationField3D>(get_node_or_null(canopy_field)) };
		for (int f = 0; f < 2; f++) {
			const VegetationField3D *field = fields[f];
			if (!field || (f == 1 && field == fields[0]) || field->get_biome().is_null()) {
				continue;
			}
			const TypedArray<PlantSpecies> list = field->get_biome()->get_species();
			for (int i = 0; i < list.size(); i++) {
				const Ref<PlantSpecies> sp = list[i];
				if (sp.is_null() || !sp->has_meta("sward_mound")) {
					continue;
				}
				const float r = float(sp->get_meta("sward_mound")) * sp->get_scale_range().x * 0.9f;
				const PackedVector3Array at = field->get_placements(i);
				for (const Vector3 &q : at) {
					now.push_back(Vector4(q.x, q.y, q.z, r));
				}
			}
		}
	}
	if (now == mound_obstacles) {
		return false;
	}
	// The discs that came or went (a set difference by position to the centimetre and radius).
	auto key = [](const Vector4 &p_d) { return Vector3i(int(Math::round(p_d.x * 100.0f)), int(Math::round(p_d.z * 100.0f)), int(Math::round(p_d.w * 100.0f))); };
	HashSet<Vector3i> before;
	for (const Vector4 &d : mound_obstacles) {
		before.insert(key(d));
	}
	HashSet<Vector3i> after;
	PackedVector4Array changed;
	for (const Vector4 &d : now) {
		after.insert(key(d));
		if (!before.has(key(d))) {
			changed.push_back(d);
		}
	}
	for (const Vector4 &d : mound_obstacles) {
		if (!after.has(key(d))) {
			changed.push_back(d);
		}
	}
	mound_obstacles = now;
	_rebuild_obstacle_grid();
	_stale_tiles_over(changed);
	return true;
}

// Marks the blade tiles and far ring tiles under the given discs as out of date: they are built again, nearest first,
// while the old ones stay drawn.
void GrassField3D::_stale_tiles_over(const PackedVector4Array &p_discs) {
	if (p_discs.is_empty()) {
		return;
	}
	for (KeyValue<Vector2i, Tile> &kv : tiles) {
		const Rect2 rect(kv.key.x * tile_size, kv.key.y * tile_size, tile_size, tile_size);
		for (const Vector4 &d : p_discs) {
			if (rect.grow(d.w + OBSTACLE_MARGIN).has_point(Vector2(d.x, d.z))) {
				kv.value.generation = generation - 1;
				break;
			}
		}
	}
	for (KeyValue<Vector3i, RingTile> &kv : ring_tiles) {
		const float size = RING_TILE * float(1 << kv.key.z);
		const Rect2 rect(kv.key.x * size, kv.key.y * size, size, size);
		for (const Vector4 &d : p_discs) {
			if (rect.grow(d.w + OBSTACLE_MARGIN).has_point(Vector2(d.x, d.z))) {
				kv.value.generation = generation - 1;
				break;
			}
		}
	}
}

VegetationField3D *GrassField3D::_get_undergrowth() const {
	if (undergrowth_field.is_empty() || !is_inside_tree()) {
		return nullptr;
	}
	return Object::cast_to<VegetationField3D>(get_node_or_null(undergrowth_field));
}

VegetationField3D *GrassField3D::_get_canopy(const PlantSpecies *p_rules) const {
	if (!p_rules->uses_shade() || canopy_field.is_empty() || !is_inside_tree()) {
		return nullptr;
	}
	return Object::cast_to<VegetationField3D>(get_node_or_null(canopy_field));
}

void GrassField3D::set_data_map(const NodePath &p_path) {
	GRASS_SET_REBUILD(data_map, p_path)
}
void GrassField3D::set_rules(const Ref<PlantSpecies> &p_rules) {
	GRASS_SET_REBUILD(rules, p_rules)
}
void GrassField3D::set_crop(Crop p_crop) {
	_queue_far_bake();
	if (p_crop == CROP_NONE && crop_nodes.has(this)) {
		crop_nodes.erase(this);
		_crop_fields_changed_notify();
	}
	GRASS_SET_REBUILD(crop, p_crop)
}
void GrassField3D::set_crop_stage(float p_stage) {
	const bool emerged = crop_stage >= 10.0f;
	crop_stage = CLAMP(p_stage, 0.0f, 99.0f);
	_queue_far_bake();
	_update_plot_stages(crop_day);
	// The stage is all uniforms, but below 10 nothing has emerged: the tiles rebuild across that line.
	if (emerged != (crop_stage >= 10.0f)) {
		rebuild();
	}
}
void GrassField3D::set_row_spacing(float p_spacing) {
	_queue_far_bake();
	GRASS_SET_REBUILD(row_spacing, CLAMP(p_spacing, 0.03f, 1.0f))
}
void GrassField3D::set_drill_angle(float p_angle) {
	GRASS_SET_REBUILD(drill_angle, p_angle)
}
void GrassField3D::set_density(float p_density) {
	_queue_far_bake();
	GRASS_SET_REBUILD(density, CLAMP(p_density, 0.1f, 1000.0f))
}
void GrassField3D::set_blade_height(const Vector2 &p_height) {
	_queue_far_bake();
	GRASS_SET_REBUILD(blade_height, Vector2(MAX(p_height.x, 0.01f), MAX(p_height.y, 0.01f)))
}
void GrassField3D::set_blade_width(float p_width) {
	_queue_far_bake();
	GRASS_SET_REBUILD(blade_width, MAX(p_width, 0.001f))
}
void GrassField3D::set_clump_size(float p_size) {
	_queue_far_bake();
	GRASS_SET_REBUILD(clump_size, MAX(p_size, 0.05f))
}
void GrassField3D::set_lean(float p_lean) {
	_queue_far_bake();
	GRASS_SET_REBUILD(lean, CLAMP(p_lean, 0.0f, 1.4f))
}
void GrassField3D::set_radius(float p_radius) {
	GRASS_SET_REBUILD(radius, MAX(p_radius, 0.0f))
}
void GrassField3D::set_tile_size(float p_size) {
	GRASS_SET_REBUILD(tile_size, MAX(p_size, 1.0f))
}

void GrassField3D::set_base_color(const Color &p_color) {
	base_color = p_color;
	_queue_far_bake();
	if (material.is_valid()) {
		material->set_shader_parameter("base_color", base_color);
	}
}
void GrassField3D::set_tip_color(const Color &p_color) {
	tip_color = p_color;
	_queue_far_bake();
	if (material.is_valid()) {
		material->set_shader_parameter("tip_color", tip_color);
	}
}
void GrassField3D::set_dry_color(const Color &p_color) {
	dry_color = p_color;
	ground_baked = false;
	_queue_far_bake();
	if (material.is_valid()) {
		material->set_shader_parameter("dry_color", dry_color);
	}
}
void GrassField3D::set_color_variation(float p_amount) {
	color_variation = CLAMP(p_amount, 0.0f, 1.0f);
	ground_baked = false;
	_queue_far_bake();
	if (material.is_valid()) {
		material->set_shader_parameter("color_variation", color_variation);
	}
}

void GrassField3D::set_dryness(float p_dryness) {
	dryness = CLAMP(p_dryness, 0.0f, 1.0f);
	ground_baked = false;
	_queue_far_bake();
	if (material.is_valid()) {
		material->set_shader_parameter("dryness", _dryness());
		material->set_shader_parameter("dormant", _dormant());
	}
}

void GrassField3D::set_season_source(const NodePath &p_path) {
	season_source = p_path;
	season_wait = 0;
	if (!_follows_season()) {
		season_green = 1.0f;
		_set_season_height(1.0f);
		set_dryness(dryness);
	}
}

void GrassField3D::set_follow_weather(bool p_on) {
	follow_weather = p_on;
	season_wait = 0;
	if (!_follows_season()) {
		season_green = 1.0f;
		_set_season_height(1.0f);
		set_dryness(dryness);
	}
}

void GrassField3D::set_season_response(float p_response) {
	season_response = CLAMP(p_response, 0.0f, 1.0f);
	set_dryness(dryness);
}

// The sward's standing height over its summer height for a season state (docs/ideas/vegetation.md, "Seasonal sward
// height"). Grass stands short through the winter: the summer's growth dies back and lodges under the autumn's rain and
// wind, and what stands is the short new tillers; growth restarts in spring (the agronomists' T-sum of 200 degree days
// from 1 January, about our forcing of 40 over 5 C) and a meadow is at full height by heading, about forcing 550 (late
// May to June at 46 N). In autumn it declines with the senescence sum as the leaves die. The winter height, a third of
// the summer's, is an estimate. 1 with no season.
float GrassField3D::season_height_for(const Dictionary &p_season) {
	if (p_season.is_empty() || !p_season.has("forcing")) {
		return 1.0f;
	}
	const float winter = 0.35f;
	const float forcing = float(p_season.get("forcing", 0.0f));
	float g = Math::lerp(winter, 1.0f, Math::smoothstep(40.0f, 550.0f, forcing));
	if (int(p_season.get("day_of_year", 180)) > 172) {
		g = MIN(g, Math::lerp(1.0f, winter, Math::smoothstep(150.0f, 900.0f, float(p_season.get("senescence", 0.0f)))));
	}
	return g;
}

void GrassField3D::_set_season_height(float p_height) {
	if (Math::abs(p_height - season_height) < 0.01f) {
		return;
	}
	season_height = p_height;
	if (material.is_valid()) {
		material->set_shader_parameter("season_height", season_height);
	}
	if (shell_material.is_valid()) {
		shell_material->set_shader_parameter("season_height", season_height);
		shell_material->set_shader_parameter("sward_height", 0.5f * (blade_height.x + blade_height.y) * season_height);
	}
}

// How green the sward is for a season state (WeatherMap3D.get_season()), 0 to 1. Grassland
// greenness follows soil water and temperature (PhenoGrass, Hufkens et al. 2016; the Growing
// Season Index of Jolly et al. 2005 without its day-length term, since a temperate sward stays
// green through short days): drought browns it as the soil dries below about 40% of capacity,
// and a hard winter (daily means well below freezing) leaves it tan, while a mild maritime
// winter keeps it green.
float GrassField3D::season_greenness(const Dictionary &p_season) {
	if (p_season.is_empty()) {
		return 1.0f;
	}
	// The shared rule (WorldContracts::greenness_for_season): the terrain's cured cover follows the same.
	return WorldContracts::greenness_for_season(float(p_season.get("soil_moisture", 1.0f)), float(p_season.get("temperature", 10.0f)));
}

// Once a second: the season's greenness into the blades' and the ground's dryness, and a new far
// colour bake when the look has moved enough to show.
void GrassField3D::_season_step() {
	if (!_follows_season() || season_wait-- > 0) {
		return;
	}
	season_wait = 60;
	const WorldWeather *wm = WorldContracts::weather();
	if (!wm->has_provider()) {
		return;
	}
	const Dictionary season_now = wm->season_at(get_global_position());
	_update_snowmelt(season_now);
	if (material.is_valid() && season_now.has("day_of_year")) {
		// The deer beds' ages (deer_beds.h) by the season's day; the hour in, so they recover through the day.
		deer_bed_day = float(int(season_now.get("day_of_year", 180))) + float(season_now.get("hour", 12.0f)) / 24.0f;
		material->set_shader_parameter("deer_bed_day", deer_bed_day);
	}
	if (!meadows.is_empty()) {
		const int day = int(season_now.get("day_of_year", -1));
		_hay_rain_step();
		bool rained = false;
		for (const HayMeadow &m : meadows) {
			rained = rained || Math::abs(m.rain - m.rain_sent) > 0.25f;
		}
		if (day != hay_day || rained) {
			_update_mow_state(day);
		}
	}
	if (crop != CROP_NONE && !plots.is_empty()) {
		// Fields with a sowing day grow through the year.
		const int day = int(season_now.get("day_of_year", -1));
		if (day != crop_day) {
			crop_day = day;
			_update_plot_stages(day);
			_queue_far_bake();
		}
	}
	_set_season_height(season_height_for(season_now));
	const float g = season_greenness(season_now);
	if (Math::abs(g - season_green) < 0.005f) {
		return;
	}
	season_green = g;
	const float d = _dryness();
	if (material.is_valid()) {
		material->set_shader_parameter("dryness", d);
		material->set_shader_parameter("dormant", _dormant());
	}
	if (ground_published && is_inside_tree()) {
		RenderingServer::get_singleton()->global_shader_parameter_set("grass_ground_dry", Vector4(dry_color.r, dry_color.g, dry_color.b, d));
	}
	if (season_baked_dryness < 0.0f || Math::abs(d - season_baked_dryness) > 0.05f) {
		season_baked_dryness = d;
		_queue_far_bake();
		if (!far_bake && is_inside_tree()) {
			emit_signal(SNAME("far_color_changed")); // No bake to wait for: the hand-matched colour and the cover map follow now.
		}
	}
}
void GrassField3D::set_fallback_wind(const Vector2 &p_wind) {
	fallback_wind = p_wind;
	if (material.is_valid()) {
		material->set_shader_parameter("fallback_wind", fallback_wind);
	}
	ground_baked = false; // Republishes grass_wind for the other plants.
}
void GrassField3D::set_wind_response(float p_response) {
	wind_response = MAX(p_response, 0.0f);
	if (material.is_valid()) {
		material->set_shader_parameter("wind_response", wind_response);
	}
}

void GrassField3D::set_gpu_fill(bool p_enabled) {
	if (gpu_fill == p_enabled) {
		return;
	}
	gpu_fill = p_enabled;
	// The same blades either way: only the tiles are rebuilt (nearest first, the old ones drawn
	// until then), not the mesh or the material.
	generation++;
}

void GrassField3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_gpu_fill", "enabled"), &GrassField3D::set_gpu_fill);
	ClassDB::bind_method(D_METHOD("is_gpu_fill"), &GrassField3D::is_gpu_fill);
	ClassDB::bind_static_method("GrassField3D", D_METHOD("is_gpu_fill_available"), &GrassField3D::is_gpu_fill_available);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "gpu_fill"), "set_gpu_fill", "is_gpu_fill");
#define GRASS_BIND(m_set, m_get, m_arg) \
	ClassDB::bind_method(D_METHOD(#m_set, m_arg), &GrassField3D::m_set); \
	ClassDB::bind_method(D_METHOD(#m_get), &GrassField3D::m_get);
	GRASS_BIND(set_data_map, get_data_map, "path")
	GRASS_BIND(set_rules, get_rules, "rules")
	ClassDB::bind_method(D_METHOD("get_far_color"), &GrassField3D::get_far_color);
	ClassDB::bind_method(D_METHOD("is_far_color_baked"), &GrassField3D::is_far_color_baked);
	ClassDB::bind_method(D_METHOD("set_far_bake", "enabled"), &GrassField3D::set_far_bake);
	ClassDB::bind_method(D_METHOD("is_far_bake"), &GrassField3D::is_far_bake);
	ClassDB::bind_method(D_METHOD("get_far_color_lobes"), &GrassField3D::get_far_color_lobes);
	ClassDB::bind_method(D_METHOD("get_far_published"), &GrassField3D::get_far_published);
	ClassDB::bind_method(D_METHOD("set_far_match_view", "enabled"), &GrassField3D::set_far_match_view);
	ClassDB::bind_method(D_METHOD("is_far_match_view"), &GrassField3D::is_far_match_view);
	GRASS_BIND(set_sun_light, get_sun_light, "path")
	ADD_SIGNAL(MethodInfo("far_color_changed"));
	GRASS_BIND(set_canopy_field, get_canopy_field, "path")
	GRASS_BIND(set_undergrowth_field, get_undergrowth_field, "path")
	GRASS_BIND(set_obstacles, get_obstacles, "obstacles")
	GRASS_BIND(set_rock_field, get_rock_field, "path")
	GRASS_BIND(set_rock_obstacle_min, get_rock_obstacle_min, "size")
	GRASS_BIND(set_rock_footprint_max, get_rock_footprint_max, "radius")
	ClassDB::bind_method(D_METHOD("get_rock_chunk_count"), &GrassField3D::get_rock_chunk_count);
	GRASS_BIND(set_paths, get_paths, "paths")
	GRASS_BIND(set_trample_resistance, get_trample_resistance, "passes")
	GRASS_BIND(set_density, get_density, "density")
	GRASS_BIND(set_crop, get_crop, "crop")
	GRASS_BIND(set_crop_stage, get_crop_stage, "stage")
	GRASS_BIND(set_row_spacing, get_row_spacing, "spacing")
	GRASS_BIND(set_drill_angle, get_drill_angle, "angle")
	GRASS_BIND(set_fields, get_fields, "fields")
	ClassDB::bind_method(D_METHOD("set_hay_meadows", "meadows"), &GrassField3D::set_hay_meadows);
	ClassDB::bind_method(D_METHOD("get_hay_meadows"), &GrassField3D::get_hay_meadows);
	ClassDB::bind_method(D_METHOD("get_mown_days_at", "position"), &GrassField3D::get_mown_days_at);
	ClassDB::bind_static_method("GrassField3D", D_METHOD("mown_days_at", "position"), &GrassField3D::mown_days_at);
	ClassDB::bind_method(D_METHOD("get_hay_bale_count"), &GrassField3D::get_hay_bale_count);
	ClassDB::bind_method(D_METHOD("get_field_stage", "index"), &GrassField3D::get_field_stage);
	ClassDB::bind_method(D_METHOD("_on_crop_fields_changed"), &GrassField3D::_on_crop_fields_changed);
	ClassDB::bind_static_method("GrassField3D", D_METHOD("crop_calendar_stage", "sown_day", "harvest_day", "day"), &GrassField3D::crop_calendar_stage);
	ClassDB::bind_static_method("GrassField3D", D_METHOD("crop_canopy_look", "crop", "stage"), &GrassField3D::crop_canopy_look);
	GRASS_BIND(set_blade_height, get_blade_height, "height")
	GRASS_BIND(set_blade_width, get_blade_width, "width")
	GRASS_BIND(set_clump_size, get_clump_size, "size")
	ClassDB::bind_method(D_METHOD("set_hedges", "enabled"), &GrassField3D::set_hedges);
	ClassDB::bind_method(D_METHOD("get_hedges"), &GrassField3D::get_hedges);
	ClassDB::bind_method(D_METHOD("set_hedge_offset", "offset"), &GrassField3D::set_hedge_offset);
	ClassDB::bind_method(D_METHOD("get_hedge_offset"), &GrassField3D::get_hedge_offset);
	ClassDB::bind_method(D_METHOD("set_hedge_voxel_lods", "enabled"), &GrassField3D::set_hedge_voxel_lods);
	ClassDB::bind_method(D_METHOD("get_hedge_voxel_lods"), &GrassField3D::get_hedge_voxel_lods);
	ClassDB::bind_static_method("GrassField3D", D_METHOD("make_voxel_shell", "mesh", "voxel"), &GrassField3D::make_voxel_shell);
	ClassDB::bind_method(D_METHOD("get_hedge_segment_count"), &GrassField3D::get_hedge_segment_count);
	GRASS_BIND(set_lean, get_lean, "lean")
	GRASS_BIND(set_base_color, get_base_color, "color")
	GRASS_BIND(set_tip_color, get_tip_color, "color")
	GRASS_BIND(set_dry_color, get_dry_color, "color")
	GRASS_BIND(set_dryness, get_dryness, "dryness")
	GRASS_BIND(set_season_source, get_season_source, "path")
	ClassDB::bind_method(D_METHOD("set_follow_weather", "enabled"), &GrassField3D::set_follow_weather);
	ClassDB::bind_method(D_METHOD("is_follow_weather"), &GrassField3D::is_follow_weather);
	GRASS_BIND(set_season_response, get_season_response, "response")
	ClassDB::bind_method(D_METHOD("get_season_green"), &GrassField3D::get_season_green);
	ClassDB::bind_method(D_METHOD("get_effective_dryness"), &GrassField3D::get_effective_dryness);
	ClassDB::bind_static_method("GrassField3D", D_METHOD("season_greenness", "season"), &GrassField3D::season_greenness);
	ClassDB::bind_static_method("GrassField3D", D_METHOD("season_height_for", "season"), &GrassField3D::season_height_for);
	GRASS_BIND(set_color_variation, get_color_variation, "amount")
	GRASS_BIND(set_radius, get_radius, "radius")
	GRASS_BIND(set_tile_size, get_tile_size, "size")
	GRASS_BIND(set_fallback_wind, get_fallback_wind, "wind")
	GRASS_BIND(set_wind_response, get_wind_response, "response")
	ClassDB::bind_method(D_METHOD("set_trample_enabled", "enabled"), &GrassField3D::set_trample_enabled);
	ClassDB::bind_method(D_METHOD("is_trample_enabled"), &GrassField3D::is_trample_enabled);
	GRASS_BIND(set_blade_segments, get_blade_segments, "segments")
	GRASS_BIND(set_view_facing, get_view_facing, "facing")
	GRASS_BIND(set_fibre_shading, get_fibre_shading, "amount")
	ClassDB::bind_method(D_METHOD("set_still_shot", "enabled"), &GrassField3D::set_still_shot);
	ClassDB::bind_method(D_METHOD("is_still_shot"), &GrassField3D::is_still_shot);
	ClassDB::bind_method(D_METHOD("is_still_frozen"), &GrassField3D::is_still_frozen);
	GRASS_BIND(set_ground_tint, get_ground_tint, "color")
	ClassDB::bind_method(D_METHOD("set_ground_tint_enabled", "enabled"), &GrassField3D::set_ground_tint_enabled);
	ClassDB::bind_method(D_METHOD("is_ground_tint_enabled"), &GrassField3D::is_ground_tint_enabled);
	ClassDB::bind_static_method("GrassField3D", D_METHOD("get_ground_shader_code"), &GrassField3D::get_ground_shader_code);
	GRASS_BIND(set_trample_group, get_trample_group, "group")
	GRASS_BIND(set_trample_size, get_trample_size, "size")
	GRASS_BIND(set_trample_resolution, get_trample_resolution, "resolution")
	GRASS_BIND(set_trample_radius, get_trample_radius, "radius")
	GRASS_BIND(set_trample_recovery, get_trample_recovery, "seconds")
	ClassDB::bind_method(D_METHOD("add_trample", "position", "radius", "velocity", "strength"), &GrassField3D::add_trample, DEFVAL(Vector3()), DEFVAL(1.0f));
	ClassDB::bind_method(D_METHOD("get_trample_at", "position"), &GrassField3D::get_trample_at);
	ClassDB::bind_method(D_METHOD("clear_trample"), &GrassField3D::clear_trample);
#undef GRASS_BIND
	ClassDB::bind_method(D_METHOD("rebuild"), &GrassField3D::rebuild);
	ClassDB::bind_method(D_METHOD("update_at", "center"), &GrassField3D::update_at);
	ClassDB::bind_method(D_METHOD("get_blade_count"), &GrassField3D::get_blade_count);
	ClassDB::bind_method(D_METHOD("get_tile_count"), &GrassField3D::get_tile_count);
	ClassDB::bind_method(D_METHOD("set_far_rings", "enabled"), &GrassField3D::set_far_rings);
	ClassDB::bind_method(D_METHOD("is_far_rings"), &GrassField3D::is_far_rings);
	ClassDB::bind_method(D_METHOD("set_far_ring_range", "range"), &GrassField3D::set_far_ring_range);
	ClassDB::bind_method(D_METHOD("get_far_ring_range"), &GrassField3D::get_far_ring_range);
	ClassDB::bind_method(D_METHOD("get_ring_blade_count"), &GrassField3D::get_ring_blade_count);
	ClassDB::bind_method(D_METHOD("get_ring_tile_count"), &GrassField3D::get_ring_tile_count);
	ClassDB::bind_method(D_METHOD("get_ring_build_msec"), &GrassField3D::get_ring_build_msec);
	ClassDB::bind_method(D_METHOD("set_far_shell", "enabled"), &GrassField3D::set_far_shell);
	ClassDB::bind_method(D_METHOD("is_far_shell"), &GrassField3D::is_far_shell);
	ClassDB::bind_method(D_METHOD("set_far_shell_range", "range"), &GrassField3D::set_far_shell_range);
	ClassDB::bind_method(D_METHOD("get_far_shell_range"), &GrassField3D::get_far_shell_range);
	ClassDB::bind_method(D_METHOD("set_burn_recovery", "years"), &GrassField3D::set_burn_recovery);
	ClassDB::bind_method(D_METHOD("set_graze_height", "height"), &GrassField3D::set_graze_height);
	ClassDB::bind_method(D_METHOD("set_climate_bare", "enabled"), &GrassField3D::set_climate_bare);
	ClassDB::bind_method(D_METHOD("is_climate_bare"), &GrassField3D::is_climate_bare);
	ClassDB::bind_method(D_METHOD("get_graze_height"), &GrassField3D::get_graze_height);
	ClassDB::bind_method(D_METHOD("set_lodging", "enabled"), &GrassField3D::set_lodging);
	ClassDB::bind_method(D_METHOD("is_lodging"), &GrassField3D::is_lodging);
	ClassDB::bind_method(D_METHOD("get_lodging_at", "global_position"), &GrassField3D::get_lodging_at);
	ClassDB::bind_method(D_METHOD("advance_lodging", "weather_seconds"), &GrassField3D::advance_lodging);
	ClassDB::bind_method(D_METHOD("set_snowmelt", "enabled"), &GrassField3D::set_snowmelt);
	ClassDB::bind_method(D_METHOD("is_snowmelt"), &GrassField3D::is_snowmelt);
	ClassDB::bind_method(D_METHOD("get_snowmelt_at", "position", "day"), &GrassField3D::get_snowmelt_at, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("set_flood_wash", "enabled"), &GrassField3D::set_flood_wash);
	ClassDB::bind_method(D_METHOD("is_flood_wash"), &GrassField3D::is_flood_wash);
	ClassDB::bind_method(D_METHOD("get_flood_wash_at", "position"), &GrassField3D::get_flood_wash_at);
	ClassDB::bind_method(D_METHOD("update_flood_map", "center"), &GrassField3D::update_flood_map);
	ClassDB::bind_static_method("GrassField3D", D_METHOD("get_terracette_at", "global_position", "gradient"), &GrassField3D::get_terracette_at);
	ClassDB::bind_method(D_METHOD("set_terracettes", "enabled"), &GrassField3D::set_terracettes);
	ClassDB::bind_method(D_METHOD("is_terracettes"), &GrassField3D::is_terracettes);
	ClassDB::bind_method(D_METHOD("set_deer_beds", "enabled"), &GrassField3D::set_deer_beds);
	ClassDB::bind_method(D_METHOD("is_deer_beds"), &GrassField3D::is_deer_beds);
	ClassDB::bind_method(D_METHOD("set_deer_bed_day", "day"), &GrassField3D::set_deer_bed_day);
	ClassDB::bind_method(D_METHOD("get_deer_bed_day"), &GrassField3D::get_deer_bed_day);
	ClassDB::bind_static_method("GrassField3D", D_METHOD("get_deer_bed_at", "global_position", "day"), &GrassField3D::get_deer_bed_at);
	ClassDB::bind_method(D_METHOD("set_fairy_rings", "enabled"), &GrassField3D::set_fairy_rings);
	ClassDB::bind_method(D_METHOD("is_fairy_rings"), &GrassField3D::is_fairy_rings);
	ClassDB::bind_static_method("GrassField3D", D_METHOD("get_fairy_ring_at", "global_position"), &GrassField3D::get_fairy_ring_at);
	ClassDB::bind_method(D_METHOD("get_burn_recovery"), &GrassField3D::get_burn_recovery);
	ClassDB::bind_method(D_METHOD("get_burn_cover_at", "global_position"), &GrassField3D::get_burn_cover_at);
	ClassDB::bind_method(D_METHOD("get_shell_column_count"), &GrassField3D::get_shell_column_count);
	ClassDB::bind_method(D_METHOD("get_shell_tile_count"), &GrassField3D::get_shell_tile_count);
	ClassDB::bind_method(D_METHOD("get_build_time_msec"), &GrassField3D::get_build_time_msec);
	ClassDB::bind_method(D_METHOD("get_material"), &GrassField3D::get_material);

	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "data_map", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "TerrainDataMap3D"), "set_data_map", "get_data_map");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "rules", PROPERTY_HINT_RESOURCE_TYPE, "PlantSpecies"), "set_rules", "get_rules");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "canopy_field", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "VegetationField3D"), "set_canopy_field", "get_canopy_field");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "undergrowth_field", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "VegetationField3D"), "set_undergrowth_field", "get_undergrowth_field");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_VECTOR4_ARRAY, "obstacles"), "set_obstacles", "get_obstacles");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "rock_field", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "RockField3D"), "set_rock_field", "get_rock_field");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "rock_obstacle_min", PROPERTY_HINT_RANGE, "0,4,0.05,suffix:m"), "set_rock_obstacle_min", "get_rock_obstacle_min");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "rock_footprint_max", PROPERTY_HINT_RANGE, "0,128,0.5,suffix:m"), "set_rock_footprint_max", "get_rock_footprint_max");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "paths", PROPERTY_HINT_ARRAY_TYPE, "Dictionary"), "set_paths", "get_paths");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "trample_resistance", PROPERTY_HINT_RANGE, "0,5000,1,or_greater,suffix:passes/yr"), "set_trample_resistance", "get_trample_resistance");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "sun_light", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "DirectionalLight3D"), "set_sun_light", "get_sun_light");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "density", PROPERTY_HINT_RANGE, "0.1,400,0.1,or_greater,suffix:/m²"), "set_density", "get_density");
	ADD_GROUP("Crop", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "crop", PROPERTY_HINT_ENUM, "None,Wheat,Barley,Oats,Rye"), "set_crop", "get_crop");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "crop_stage", PROPERTY_HINT_RANGE, "0,99,0.5"), "set_crop_stage", "get_crop_stage");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "row_spacing", PROPERTY_HINT_RANGE, "0.03,1,0.005,suffix:m"), "set_row_spacing", "get_row_spacing");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "drill_angle", PROPERTY_HINT_RANGE, "-180,180,0.1,radians_as_degrees"), "set_drill_angle", "get_drill_angle");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "fields", PROPERTY_HINT_ARRAY_TYPE, "Dictionary"), "set_fields", "get_fields");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "hay_meadows", PROPERTY_HINT_ARRAY_TYPE, "Dictionary"), "set_hay_meadows", "get_hay_meadows");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "hedges"), "set_hedges", "get_hedges");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "hedge_offset", PROPERTY_HINT_RANGE, "0,20,0.1,suffix:m"), "set_hedge_offset", "get_hedge_offset");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "hedge_voxel_lods"), "set_hedge_voxel_lods", "get_hedge_voxel_lods");
	BIND_ENUM_CONSTANT(CROP_NONE);
	BIND_ENUM_CONSTANT(CROP_WHEAT);
	BIND_ENUM_CONSTANT(CROP_BARLEY);
	BIND_ENUM_CONSTANT(CROP_OATS);
	BIND_ENUM_CONSTANT(CROP_RYE);
	ADD_GROUP("Blades", "");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "blade_height", PROPERTY_HINT_NONE, "suffix:m"), "set_blade_height", "get_blade_height");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "blade_width", PROPERTY_HINT_RANGE, "0.001,0.2,0.001,suffix:m"), "set_blade_width", "get_blade_width");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "clump_size", PROPERTY_HINT_RANGE, "0.05,10,0.05,suffix:m"), "set_clump_size", "get_clump_size");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "blade_segments", PROPERTY_HINT_RANGE, "1,8,1"), "set_blade_segments", "get_blade_segments");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "view_facing", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_view_facing", "get_view_facing");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "still_shot"), "set_still_shot", "is_still_shot");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "fibre_shading", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_fibre_shading", "get_fibre_shading");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lean", PROPERTY_HINT_RANGE, "0,1.4,0.01,radians_as_degrees"), "set_lean", "get_lean");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "base_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_base_color", "get_base_color");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "tip_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_tip_color", "get_tip_color");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "dry_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_dry_color", "get_dry_color");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "dryness", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_dryness", "get_dryness");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "season_source", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "WeatherMap3D"), "set_season_source", "get_season_source");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "follow_weather"), "set_follow_weather", "is_follow_weather");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "season_response", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_season_response", "get_season_response");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "color_variation", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_color_variation", "get_color_variation");
	ADD_GROUP("Ground", "ground_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "ground_tint_enabled"), "set_ground_tint_enabled", "is_ground_tint_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "ground_tint"), "set_ground_tint", "get_ground_tint");
	ADD_GROUP("Range", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "radius", PROPERTY_HINT_RANGE, "0,500,0.5,or_greater,suffix:m"), "set_radius", "get_radius");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "far_rings"), "set_far_rings", "is_far_rings");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "far_ring_range", PROPERTY_HINT_RANGE, "0,2000,1,or_greater,suffix:m"), "set_far_ring_range", "get_far_ring_range");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "far_shell"), "set_far_shell", "is_far_shell");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "far_bake"), "set_far_bake", "is_far_bake");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "far_shell_range", PROPERTY_HINT_RANGE, "0,5000,1,or_greater,suffix:m"), "set_far_shell_range", "get_far_shell_range");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "burn_recovery", PROPERTY_HINT_RANGE, "0,10,0.05,or_greater,suffix:yr"), "set_burn_recovery", "get_burn_recovery");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "graze_height", PROPERTY_HINT_RANGE, "0,0.2,0.005,suffix:m"), "set_graze_height", "get_graze_height");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "climate_bare"), "set_climate_bare", "is_climate_bare");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "lodging"), "set_lodging", "is_lodging");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "fairy_rings"), "set_fairy_rings", "is_fairy_rings");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "deer_beds"), "set_deer_beds", "is_deer_beds");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "terracettes"), "set_terracettes", "is_terracettes");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "deer_bed_day", PROPERTY_HINT_RANGE, "0,366,0.01"), "set_deer_bed_day", "get_deer_bed_day");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "snowmelt"), "set_snowmelt", "is_snowmelt");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "flood_wash"), "set_flood_wash", "is_flood_wash");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "far_match_view"), "set_far_match_view", "is_far_match_view");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "tile_size", PROPERTY_HINT_RANGE, "1,64,0.5,suffix:m"), "set_tile_size", "get_tile_size");
	ADD_GROUP("Wind", "");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "fallback_wind", PROPERTY_HINT_NONE, "suffix:m/s"), "set_fallback_wind", "get_fallback_wind");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wind_response", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_wind_response", "get_wind_response");
	ADD_GROUP("Trample", "trample_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "trample_enabled"), "set_trample_enabled", "is_trample_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "trample_group"), "set_trample_group", "get_trample_group");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "trample_size", PROPERTY_HINT_RANGE, "4,256,0.5,suffix:m"), "set_trample_size", "get_trample_size");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "trample_resolution", PROPERTY_HINT_RANGE, "8,1024,1"), "set_trample_resolution", "get_trample_resolution");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "trample_radius", PROPERTY_HINT_RANGE, "0.01,5,0.01,suffix:m"), "set_trample_radius", "get_trample_radius");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "trample_recovery", PROPERTY_HINT_RANGE, "0,120,0.1,suffix:s"), "set_trample_recovery", "get_trample_recovery");
}

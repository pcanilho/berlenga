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
#include "scene/3d/multimesh_instance_3d.h"
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

constexpr int FILL_HEADER = 44;
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

// Colour variation at three scales (patches of soil and water, clumps, single blades), as in
// Ghost of Tsushima and Horizon: a field of one flat green reads as carpet, not grass.
vec3 grass_patch_tint(vec2 xz, float blade, float value, float variation) {
	float patches = grass_noise(xz * 0.11) * 0.65 + grass_noise(xz * 0.47 + 3.7) * 0.35;
	float hue = (patches - 0.5) * 1.6 + (blade - 0.5) * 0.7;
	vec3 warm = vec3(1.18, 1.08, 0.62); // yellower, drier
	vec3 cool = vec3(0.82, 0.96, 1.02); // bluer, lusher
	vec3 tint = hue > 0.0 ? mix(vec3(1.0), warm, min(hue, 1.0)) : mix(vec3(1.0), cool, min(-hue, 1.0));
	tint *= 0.82 + 0.36 * value;
	return mix(vec3(1.0), tint, variation);
}
)";

void GrassField3D::_ensure_resources() {
	if (blade_mesh.is_null() && crop != CROP_NONE) {
		blade_mesh = _make_crop_mesh(false);
		crop_mesh_far = _make_crop_mesh(true);
		const int tris = blade_mesh->surface_get_array_index_len(0) / 3;
		const int cap = int(GLOBAL_GET("rendering/pathtracing/multimesh_merged_blas_max_triangles"));
		const float tile_tris = density * tile_size * tile_size * tris;
		if (cap > 0 && tile_tris > cap) {
			WARN_PRINT(vformat("GrassField3D \"%s\": a full crop tile holds about %d triangles, over the path tracer's merged BLAS cap (%d); lower tile_size to %.1f m or less.", get_name(), int(tile_tris), cap, Math::sqrt(float(cap) / (density * tris))));
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
			WARN_PRINT(vformat("GrassField3D \"%s\": a full tile holds about %d triangles, over the path tracer's merged BLAS cap (%d); lower tile_size to %.1f m or less.", get_name(), int(tile_tris), cap, Math::sqrt(float(cap) / (density * tris))));
		}
	}
	if (material.is_null()) {
		_build_material();
	}
}

void GrassField3D::_build_material() {
	String code = "shader_type spatial;\nrender_mode cull_disabled;\n\n";
	code += "// GrassField3D blades (docs/ideas/grass.md): quadratic Bezier bent by the wind, after Ghost of Tsushima.\n";
	code += "uniform vec3 base_color : source_color;\nuniform vec3 tip_color : source_color;\nuniform vec3 dry_color : source_color;\n";
	code += "uniform vec2 fallback_wind;\nuniform float wind_response;\nuniform float view_facing;\nuniform vec4 fade_band;\n";
	code += "uniform float dormant; // Share of blades dead to the crown in a drought or hard winter (season).\n";
	code += "uniform sampler2D trample_map : filter_linear, repeat_disable;\nuniform vec4 trample_rect;\n";
	code += "global uniform sampler2D weather_state_map;\nglobal uniform sampler2D weather_contact_map;\nglobal uniform vec4 weather_contact_rect;\n";
	// The weather module declares its globals, so this compiles with or without a WeatherMap3D.
	code += WeatherMap3D::get_wind_shader_code();
	code += vformat("#define WEATHER_GUST_COUNT %d\n#define WEATHER_GUSTS_PER_AXIS %d\n", WeatherMap3D::GUST_COUNT, WeatherMap3D::GUSTS_PER_AXIS);
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
	float along_pos = dot(wp.xz, along);
	float res = amp * (cos(wa * (time - along_pos / uc) + jitter * 1.2) + cos(wb * (time - along_pos / uc) + 2.1 + jitter * 1.2));
	vec2 hz = (along * g.x + across * g.y) * sigma + along * res;
	return v + vec3(hz.x, g.z * sigma, hz.y);
}

// The weather map's wind; without one, fallback_wind with gusts rolling downwind.
vec3 grass_wind(vec3 wp, float h, float time, float omega0, float jitter) {
	if (weather_map_rect.z > 0.0) {
		return grass_weather_wind(wp, h, omega0, 0.1, jitter, time);
	}
	float s = length(fallback_wind);
	if (s < 1e-4) {
		return vec3(0.0);
	}
	vec2 d = fallback_wind / s;
	float phase = dot(wp.xz, d) * 0.12 - time * s * 0.12;
	float g = 1.0 + 0.45 * sin(phase) + 0.2 * sin(phase * 2.7 + dot(wp.xz, vec2(-d.y, d.x)) * 0.2);
	return vec3(fallback_wind.x, 0.0, fallback_wind.y) * g;
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
	VERTEX = hide || stage < 10.0 ? vec3(0.0) : p;
	NORMAL = n;
	v_t = t;
	v_part = float(part);
	v_ripe = ripe;
	v_dead = dead;
	v_hf = clamp(p.y / max(h + ear_len, 0.05), 0.0, 1.0);
	v_tint = grass_patch_tint(base_w.xz, fract(rnd * 7.31 + 0.13), fract(rnd * 13.7 + 0.51), color_variation);
	v_wet = 0.0;
	if (weather_map_rect.z > 0.0) {
		vec2 half_texel = 0.5 / vec2(textureSize(weather_state_map, 0));
		vec2 uv = clamp((base_w.xz - weather_map_rect.xy) / weather_map_rect.zw, half_texel, 1.0 - half_texel);
		v_wet = textureLod(weather_state_map, uv, 0.0).g;
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
	vec4 fire_f = fire_state_at((INV_VIEW_MATRIX * vec4(VERTEX, 1.0)).xz);
	if (fire_f.x > 0.0) {
		vec3 ch = mix(vec3(0.035, 0.03, 0.027), vec3(0.35, 0.34, 0.33), step(0.85, fract(v_rnd * 41.0)) * smoothstep(0.6, 1.0, fire_f.x));
		c = mix(c, ch, smoothstep(0.0, 0.5, fire_f.x));
		rough = mix(rough, 0.95, smoothstep(0.0, 0.5, fire_f.x));
		back *= 1.0 - smoothstep(0.0, 0.5, fire_f.x);
		EMISSION = fire_glow_color(fire_f.y) * 4.0 * smoothstep(0.4, 1.0, v_hf);
	}
	ALBEDO = c * mix(1.0, wet_factor, v_wet);
	ROUGHNESS = rough * mix(1.0, 0.4, v_wet);
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
varying float v_wet;
varying vec3 v_tint;
varying float v_light;
varying float v_trod; // Trodden flat on a worn path (its rest lean past any sward's own).

uniform float color_variation;
)";
		code += GRASS_COLOR_CODE;
		code += VegetationLight::SHADER_CODE;
		code += VegetationFire::SHADER_CODE;
		code += R"(

void vertex() {
	float h = INSTANCE_CUSTOM.x;
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

	// Blades dead to the crown in winter or a drought (dormant) lose their turgor and lodge: they lie
	// over, each its own way, a little shorter, in the mat rather than standing up as straw spikes.
	if (rnd > 1.0 - dormant * 0.85) {
		float la = fract(rnd * 53.7) * 6.2832;
		off = off * 0.4 + vec2(cos(la), sin(la)) * (0.9 + 0.4 * fract(rnd * 17.3));
		h *= 0.8;
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
	// Length-preserving bend (Jahrmann and Wimmer 2017): the tip turns by the bend angle.
	float ang = min(length(off), 1.45);
	vec2 dir = length(off) > 1e-4 ? normalize(off) : vec2(0.0, 1.0);
	vec3 tip = vec3(dir.x * sin(ang), cos(ang), dir.y * sin(ang)) * h;
	vec3 mid = vec3(0.0, h * 0.62 * cos(ang * 0.35), 0.0) + vec3(dir.x, 0.0, dir.y) * h * 0.18 * sin(ang);
	vec3 p = 2.0 * t * (1.0 - t) * mid + t * t * tip;
	vec3 tangent = normalize(2.0 * (1.0 - t) * mid + 2.0 * t * (tip - mid) + vec3(0.0, 1e-4, 0.0));
	vec3 across = vec3(1.0, 0.0, 0.0);
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
	if (fade_band.w > 0.0) {
		float keep_b = clamp((fade_band.z - length(base_w.xz - fade_band.xy)) / fade_band.w, 0.0, 1.0);
		float order = floor(INSTANCE_CUSTOM.w) / 256.0;
		p *= smoothstep(order, order + 0.15, keep_b * 1.15);
	}
	VERTEX = p;
	NORMAL = normalize(n + across * side * 0.9);

	v_t = t;
	v_dry = rnd;
	v_wet = 0.0;
	v_tint = grass_patch_tint(base_w.xz, fract(rnd * 7.31 + 0.13), fract(rnd * 13.7 + 0.51), color_variation);
	if (weather_map_rect.z > 0.0) {
		vec2 half_texel = 0.5 / vec2(textureSize(weather_state_map, 0));
		vec2 uv = clamp((base_w.xz - weather_map_rect.xy) / weather_map_rect.zw, half_texel, 1.0 - half_texel);
		v_wet = textureLod(weather_state_map, uv, 0.0).g;
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
	vec3 tipc = mix(tip_color, dry_color, step(1.0 - dry_share, v_dry) * smoothstep(0.3, 1.0, v_t));
	vec3 c = mix(base_color, tipc, t) * v_tint;
	// Some blades of the dry fraction are dead from the root (thatch), straw that greys with age.
	float dead = step(1.0 - max(dry_share * 0.3, dormant * 0.85), v_dry);
	// Dead leaves bleach to a pale grey buff, not the warm straw of a dry tip (a saturated straw read
	// as orange specks on a dark winter sward).
	vec3 straw = mix(dry_color, vec3(dot(dry_color, vec3(0.3, 0.59, 0.11))) * vec3(1.0, 0.96, 0.88), 0.5) * 0.85;
	c = mix(c, straw * mix(0.7, 1.0, t) * (0.75 + 0.5 * fract(v_dry * 31.0)), dead);
	// Midrib and parallel veins (monocot leaves), faded out once a blade is under a few pixels wide.
	float x = abs(UV.x - 0.5) * 2.0;
	float fw = max(fwidth(UV.x), 1e-4);
	float detail = 1.0 - smoothstep(0.08, 0.25, fw);
	float rib = 1.0 - smoothstep(0.0, 0.12 + fw, x);
	float veins = 0.5 + 0.5 * cos(x * 6.2832 * 3.5);
	c *= 1.0 + detail * (0.18 * rib - 0.07 * veins * (1.0 - rib) - 0.12 * x * x);
	c = vegetation_light_tint(c, v_light);
	// Wet blades (Lagarde 2013, low porosity): a little darker and glossier.
	float wet_factor = mix(1.0, 0.2, 0.25);
	// Trodden flat: bruised and soiled, and its waxy face no longer catches the sky at a grazing
	// angle (lying blades read frosted white on a worn path's edge, docs 17 Nov).
	c *= mix(1.0, 0.72, v_trod);
	// Read here at the blade's own point, not passed from vertex(): the path tracer reruns vertex() for a merged
	// MultiMesh with the field's transform, not the blade's.
	vec4 fire_f = fire_state_at((INV_VIEW_MATRIX * vec4(VERTEX, 1.0)).xz);
	float v_burn = fire_f.x;
	float v_glow = fire_f.y;
	if (v_burn > 0.0) {
		// Charred stubble, with flecks of white ash where blades burnt out.
		vec3 ch = mix(vec3(0.035, 0.03, 0.027), vec3(0.35, 0.34, 0.33), step(0.85, fract(v_dry * 41.0)) * smoothstep(0.6, 1.0, v_burn));
		c = mix(c, ch, smoothstep(0.0, 0.5, v_burn));
	}
	ALBEDO = c * mix(1.0, wet_factor, v_wet);
	ROUGHNESS = mix(mix(0.55, 0.75, v_trod) * mix(1.0, wet_factor, 0.5 * v_wet) * mix(1.0, 0.4, v_wet), 0.95, smoothstep(0.0, 0.5, v_burn));
	// The burning band: the stubs' tips glow as the ground's embers do (the char decal's glow energy).
	EMISSION = fire_glow_color(v_glow) * 4.0 * smoothstep(0.4, 1.0, v_t) * step(1e-3, v_burn);
	SPECULAR = mix(0.35, 0.2, v_trod);
	BACKLIGHT = ALBEDO * 0.7;
	// Sky light inside the sward (Monsi and Saeki 1953): diffuse light falls off as
	// exp(-k L) with the leaf area L above, k about 0.8 for upright blades, and light the blades
	// scatter on slows that to k sqrt(1 - scattering) (Goudriaan 1977). In AO, so only the
	// raster's ambient takes it: its shadow maps already darken the sun there, and the path
	// tracer sees the blades themselves (it used to take this darkening in the albedo too).
	float scatter = clamp(dot(ALBEDO, vec3(0.3, 0.59, 0.11)) * 1.7, 0.0, 0.95);
	AO = exp(-0.8 * sqrt(1.0 - scatter) * sward_lai * (1.0 - v_t));
}

)";
	}
	// A rebuild keeps the compiled shader when its code is unchanged (most settings are uniforms):
	// only the material is new, so no pipeline recompiles on a settings change.
	if (blade_shader.is_null() || blade_shader->get_code() != code) {
		blade_shader.instantiate();
		blade_shader->set_code(code);
	}
	const Ref<Shader> shader = blade_shader;
	material.instantiate();
	material->set_shader(shader);
	material->set_shader_parameter("base_color", base_color);
	material->set_shader_parameter("tip_color", tip_color);
	material->set_shader_parameter("dry_color", dry_color);
	material->set_shader_parameter("dryness", _dryness());
	material->set_shader_parameter("dormant", _dormant());
	material->set_shader_parameter("color_variation", color_variation);
	material->set_shader_parameter("fallback_wind", fallback_wind);
	material->set_shader_parameter("wind_response", wind_response);
	material->set_shader_parameter("trample_rect", Vector4());
	material->set_shader_parameter("view_facing", view_facing);
	material->set_shader_parameter("fade_band", fade_band);
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
	const float f = p_distance / MAX(radius, 0.001f);
	return f < 0.25f ? 0 : (f < 0.5f ? 1 : (f < 0.75f ? 2 : 3));
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
	rebuild();
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
		return Color(cs.straw.r * 0.85f, cs.straw.g * 0.85f, cs.straw.b * 0.85f, 0.45f);
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
	c = Color(grey, grey, grey).lerp(c, Math::lerp(Math::lerp(1.2f, 1.6f, closed), 0.85f, ripe));
	c = c * Math::lerp(Math::lerp(0.8f, 0.44f, closed), 0.68f, ripe);
	c.r = MAX(c.r, 0.0f);
	c.g = MAX(c.g, 0.0f);
	c.b = MAX(c.b, 0.0f);
	c.a = cover;
	return c;
}

LocalVector<GrassField3D *> GrassField3D::crop_nodes;
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
								const float wv = Math::fposmod(q.dot(n), pl.tramline_spacing) - 0.5f * pl.tramline_spacing;
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
		Ref<Image> img = Image::create_empty(256, 3, false, Image::FORMAT_RGBAF);
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
				if (_crop_swath(pl, pitch, half)) {
					img->set_pixel(slot, 2, Color(pitch, half, 1.0f, 0.0f));
				}
				slot++;
			}
		}
		if (crop_plots_texture.is_valid() && crop_plots_texture->get_size() == Vector2(256, 3)) {
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
			if (!sward_grid.is_empty() && suit > 0.0f) {
				suit *= 1.0f - grid_bilinear(sward_grid, lg, x - x0, z - z0);
			}
			if (passes > 0.0f && suit > 0.0f) {
				suit *= _trample_cover(passes);
			}
			if (rank >= keep * suit) {
				return false;
			}
			y = p_map->get_height_at(p);
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
				const int cx = int(Math::floor(x / clump_size));
				const int cz = int(Math::floor(z / clump_size));
				const uint32_t hc = cell_hash(cx, cz, 0xC2B2AE35u);
				const float clump_h = unit(hc, 0);
				const float clump_yaw = unit(hc, 8) * Math::TAU;
				const float rnd = unit(hb, 16);
				const float height = Math::lerp(blade_height.x, blade_height.y, 0.65f * clump_h + 0.35f * unit(hj, 16)) * (1.0f + 0.35f * sp.shade_t - 0.15f * sp.sun_t) * _trample_scale(sp.passes);
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
					const float h = hd[(hp_z0 + j) * map_n + hp_x0 + i];
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
	const AABB world_box(Vector3(x0 - 1.0f, y_min - 0.2f, z0 - 1.0f), Vector3(tile_size + 2.0f, y_max - y_min + top + 0.2f, tile_size + 2.0f));
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
	const Color far = b.lerp(t, 0.65f).lerp(d, _dryness() * 0.35f) * 0.355f;
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
			const int cx = int(Math::floor(x / clump_size));
			const int cz = int(Math::floor(z / clump_size));
			const uint32_t hc = cell_hash(cx, cz, 0xC2B2AE35u);
			const float rnd = unit(hb, 16);
			const float height = Math::lerp(blade_height.x, blade_height.y, 0.65f * unit(hc, 0) + 0.35f * unit(hj, 16));
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
	if (fade_owner == this) {
		_publish_far();
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
	const Vector2 c(fade_band.x, fade_band.y);
	const float r_full = (fade_band.z - fade_band.w) * 0.92f; // Blades still all there.
	const float r_out = fade_band.z + MAX(3.0f, fade_band.z * 0.08f); // Only the far ground.
	const Vector3 fwd = -p_cam->get_global_transform().basis.get_column(Vector3::AXIS_Z);
	Vector2 f2(fwd.x, fwd.z);
	if (f2.length() < 0.05f) {
		const Vector3 up = p_cam->get_global_transform().basis.get_column(Vector3::AXIS_Y);
		f2 = Vector2(up.x, up.z); // Looking straight down: the top of the frame.
	}
	const float a0 = Math::atan2(f2.y, f2.x);
	far_view_px.clear();
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
			if (ok && px[0].distance_to(px[1]) > 6.0f) {
				far_view_px.push_back(px[0]);
				far_view_px.push_back(px[1]);
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
	}
	int pairs = lum_ratios.size();
	Color med(1, 1, 1, 1);
	if (pairs > 0) {
		// Brightness only: the bake's hue holds, and the blue channel is too small to read well.
		const float yin = sum_in.r * 0.2126f + sum_in.g * 0.7152f + sum_in.b * 0.0722f;
		const float yout = sum_out.r * 0.2126f + sum_out.g * 0.7152f + sum_out.b * 0.0722f;
		const float k = yin / MAX(yout, 1e-5f);
		med = Color(k, k, k, 1.0f);
		lum_ratios.sort();
		const float spread = (lum_ratios[(pairs * 3) / 4] - lum_ratios[pairs / 4]) / MAX(lum_ratios[pairs / 2], 1e-4f);
		if (spread > 0.8f) {
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
	if (!close) {
		// The frame is tone mapped, so the step is not exact: take it, then look again.
		far_view_scale.r = CLAMP(far_view_scale.r * CLAMP(p_ratio.r, 0.8f, 1.25f), 0.5f, 1.5f);
		far_view_scale.g = CLAMP(far_view_scale.g * CLAMP(p_ratio.g, 0.8f, 1.25f), 0.5f, 1.5f);
		far_view_scale.b = CLAMP(far_view_scale.b * CLAMP(p_ratio.b, 0.8f, 1.25f), 0.5f, 1.5f);
		if (fade_owner == this) {
			_publish_far();
		}
	}
	far_view_elevation = sun_elevation_deg;
	print_verbose(vformat("GrassField3D \"%s\": live-view far match round %d: blades/far %s over %d pairs, scale now %s (%.2f ms).", get_name(), far_view_round, p_ratio, p_pairs, far_view_scale, far_view_msec));
	if (!close && far_view_round < 5) {
		_far_view_start(20);
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
	if (crop != CROP_NONE && is_inside_tree()) {
		_crop_ground_step();
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
	RenderingServer *rs = RenderingServer::get_singleton();
	rs->global_shader_parameter_set("grass_ground_map", ground_texture->get_rid());
	rs->global_shader_parameter_set("grass_ground_litter", ground_litter_texture->get_rid());
	rs->global_shader_parameter_set("grass_ground_rect", p_rect);
	rs->global_shader_parameter_set("grass_ground_color", Vector4(ground_tint.r, ground_tint.g, ground_tint.b, ground_tint.a));
	rs->global_shader_parameter_set("grass_ground_dry", Vector4(dry_color.r, dry_color.g, dry_color.b, _dryness()));
	ground_published = true;
	rs->global_shader_parameter_set("grass_ground_params", Vector4(color_variation, 0.0f, 0.0f, 0.0f));
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
global uniform vec4 grass_ground_params; // x: colour variation.
global uniform vec4 grass_ground_fade; // xy: centre of the blades, z: where the last is gone, w: the band they thin over.
global uniform vec4 grass_ground_far; // rgb: the blades' colour seen from a distance.
global uniform vec4 grass_ground_far_lobe1; // rgb: its change with cos of the sun's azimuth from the view.
global uniform vec4 grass_ground_far_lobe2; // rgb: and with cos of twice that azimuth.
global uniform vec4 grass_ground_under; // xy: centre, z: radius, w: band of the undercoat field (0: none).
global uniform vec4 grass_ground_under_far; // rgb: the undercoat's far colour; a: 1 when set.
global uniform vec4 grass_ground_far_scale; // rgb: the far colour at the sun's elevation over the cover map's (30 degrees).
global uniform vec4 grass_ground_sun; // xy: the sun light's heading (xz); z: how much the azimuth counts.
global uniform vec4 grass_ground_view; // xyz: the camera.
global uniform sampler2D crop_ground_map : filter_linear; // GrassField3D.fields: r the field's slot, g the wheelings.
global uniform sampler2D crop_ground_plots : filter_nearest; // A texel a field (crop_ground()).
global uniform vec4 crop_ground_rect;
global uniform vec4 crop_ground_sun; // xy: where the sun's light heads, z: the lobes' strength (crop tillers' bake).
global uniform vec4 crop_ground_lobe1; // rgb: the far canopy's change with cos of the sun's azimuth from the view.
global uniform vec4 crop_ground_lobe2; // rgb: and with cos of twice that azimuth.
global uniform vec4 crop_ground_fade; // xy: the centre of the tillers' band (the eye), z: the eye's height, w: 1 (-1: no tillers, the far canopy everywhere).
)";
	code += GRASS_COLOR_CODE;
	code += VegetationField3D::get_floor_shader_code();
	code += R"(
float grass_ground_cover(vec3 world_pos) {
	if (grass_ground_rect.z <= 0.0) {
		return 0.0;
	}
	vec2 uv = (world_pos.xz - grass_ground_rect.xy) / grass_ground_rect.zw;
	vec2 half_texel = 0.5 / vec2(textureSize(grass_ground_map, 0));
	// Nothing outside the map: fade out over its outer texel instead of smearing the edge.
	vec2 outside = max(half_texel - uv, uv - (1.0 - half_texel));
	float inside = 1.0 - clamp(max(outside.x, outside.y) / half_texel.x, 0.0, 1.0);
	return textureLod(grass_ground_map, clamp(uv, half_texel, 1.0 - half_texel), 0.0).r * inside;
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

// The turf colour at a point: blades' root colour, patch tint, streaks, thatch.
vec3 grass_turf(vec3 world_pos) {
	vec2 xz = world_pos.xz;
	// Streaks: fine noise stretched along a direction that turns slowly, like matted blades.
	float ang = grass_noise(xz * 0.6) * 6.2832;
	vec2 dir = vec2(cos(ang), sin(ang));
	vec2 q = vec2(dot(xz, dir), dot(xz, vec2(-dir.y, dir.x)));
	float streak = grass_noise(q * vec2(9.0, 70.0)) * 0.6 + grass_noise(q * vec2(4.0, 31.0) + 7.1) * 0.4;
	vec3 c = grass_ground_color.rgb * grass_patch_tint(xz, grass_noise(xz * 3.0), streak, grass_ground_params.x);
	c *= 0.7 + 0.6 * streak;
	// Dead thatch where the grass is dry, in matted patches.
	float thatch = grass_noise(q * vec2(5.0, 40.0) + 13.0) * 0.5 + grass_noise(xz * 1.3) * 0.5;
	// Shaded at the base of the sward, so darker than the dry tips above it.
	c = mix(c, grass_ground_dry.rgb * (0.25 + 0.2 * streak), 0.8 * smoothstep(0.8 - grass_ground_dry.a * 0.5, 0.95 - grass_ground_dry.a * 0.5, thatch));
	return c;
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
	float ang = grass_noise(xz * 0.9 + 3.7) * 6.2832;
	vec2 dir = vec2(cos(ang), sin(ang));
	vec2 q = vec2(dot(xz, dir), dot(xz, vec2(-dir.y, dir.x)));
	float strands = grass_noise(q * vec2(6.0, 38.0)) * 0.6 + grass_noise(q * vec2(2.5, 15.0) + 5.3) * 0.4;
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
	vec3 canopy = look.rgb * (0.6 + 0.8 * patchy) * (0.75 + 0.5 * speck) * (0.8 + 0.4 * clump);
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
	far = mix(far, mix(canopy * 0.45, soil * 0.3, 0.5), wheel * look.a * mu);
	// Stubble: the combine's swaths of straw down each pass, paler and fuller than the stubble.
	vec4 sw = texelFetch(crop_ground_plots, ivec2(textureSize(crop_ground_plots, 0).y > 2 ? int(crop_ground_field_slot(world_pos)) : 0, 2), 0);
	if (sw.z > 0.0) {
		float um = dot(xz, vec2(-d.y, d.x));
		float vm = dot(xz, d);
		float sway = 0.25 * sin(vm * 0.05 + floor(um / sw.x) * 1.7) + 0.1 * sin(vm * 0.21 + floor(um / sw.x) * 0.3);
		float across = abs(mod(um - sway, sw.x) - 0.5 * sw.x) / sw.y;
		float heap = clamp(1.0 - across * across, 0.0, 1.0) * (0.75 + 0.5 * speck);
		vec3 straw = look.rgb / 0.85 * 0.8 * (0.8 + 0.4 * patchy);
		far = mix(far, straw, smoothstep(0.0, 1.0, heap) * 0.5);
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
	vec3 turf = grass_turf(world_pos);
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
		float lum = clamp(dot(turf, vec3(0.3, 0.59, 0.11)) / max(dot(grass_ground_color.rgb, vec3(0.3, 0.59, 0.11)), 1e-3), 0.5, 1.5);
		vec3 under = grass_ground_under_far.rgb * tint * lobe;
		if (gone_u > 0.0) {
			turf = mix(turf, under * lum, gone_u);
		}
		if (gone > 0.0) {
			vec3 far = grass_ground_far.rgb * tint * lobe;
			if (grass_ground_under_far.a > 0.0) {
				far = mix(under, far, grass_ground_far.a); // The tall patches over the undercoat.
			}
			turf = mix(turf, far * lum, gone);
		}
	}
	gone = max(gone, gone_u);
	return mix(albedo, turf, grass_ground_patchy(world_pos, cover, gone) * mix(grass_ground_color.a, 1.0, gone));
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
	if (p_node->has_meta("trample_radius")) {
		const float r = p_node->get_meta("trample_radius");
		return Vector2(r, r);
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		CollisionShape3D *cs = Object::cast_to<CollisionShape3D>(p_node->get_child(i));
		if (!cs || cs->is_disabled() || cs->get_shape().is_null()) {
			continue;
		}
		const Ref<Shape3D> shape = cs->get_shape();
		const float sc = cs->get_global_basis().get_scale().x;
		if (const SphereShape3D *sp = Object::cast_to<SphereShape3D>(shape.ptr())) {
			return Vector2(sp->get_radius(), sp->get_radius()) * sc;
		}
		if (const CapsuleShape3D *cp = Object::cast_to<CapsuleShape3D>(shape.ptr())) {
			return Vector2(cp->get_radius(), cp->get_height() * 0.5f) * sc;
		}
		if (const CylinderShape3D *cy = Object::cast_to<CylinderShape3D>(shape.ptr())) {
			return Vector2(cy->get_radius(), cy->get_height() * 0.5f) * sc;
		}
		if (const BoxShape3D *bx = Object::cast_to<BoxShape3D>(shape.ptr())) {
			const Vector3 s = bx->get_size() * 0.5f * sc;
			return Vector2(MAX(s.x, s.z), s.y);
		}
	}
	return Vector2(trample_radius, trample_radius);
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
		const Vector<Node *> movers = get_tree()->get_nodes_in_group(trample_group);
		for (Node *n : movers) {
			Node3D *n3 = Object::cast_to<Node3D>(n);
			if (!n3 || !n3->is_visible_in_tree()) {
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
	_update_tiles(p_center);
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
}

void GrassField3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			if (!Engine::get_singleton()->is_editor_hint()) {
				set_process_internal(true);
				_queue_far_bake();
			}
		} break;
		case NOTIFICATION_ENTER_TREE: {
			if (crop != CROP_NONE) {
				if (!crop_nodes.has(this)) {
					crop_nodes.push_back(this);
					_crop_fields_changed_notify();
				}
			} else {
				add_to_group(SNAME("vegetation_crop_aware"));
				crop_mask = get_crop_field_mask();
			}
		} break;
		case NOTIFICATION_EXIT_TREE: {
			if (crop_nodes.has(this)) {
				crop_nodes.erase(this);
				_crop_fields_changed_notify();
				if (crop_nodes.is_empty()) {
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
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			Viewport *vp = get_viewport();
			Camera3D *cam = vp ? vp->get_camera_3d() : nullptr;
			if (cam) {
				update_at(cam->get_global_position());
				if (fade_owner == this) {
					_publish_sun_view(cam->get_global_position());
					_far_view_step(cam);
				}
			}
			if (trample_enabled && !trample.is_empty()) {
				_trample_step(get_process_delta_time());
			}
			_season_step();
			if (far_bake_vp) {
				_far_bake_step();
			} else if (far_bake_queued) {
				_far_bake_start();
			}
		} break;
	}
}

GrassField3D::~GrassField3D() {
	_drop_under();
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

void GrassField3D::set_paths(const Array &p_paths) {
	paths = p_paths;
	path_grid.build(paths);
	ground_baked = false; // The ground tint opens on the paths too.
	rebuild();
}

void GrassField3D::set_trample_resistance(float p_passes) {
	trample_resistance = MAX(p_passes, 0.0f);
	rebuild();
}

void GrassField3D::set_obstacles(const PackedVector4Array &p_obstacles) {
	obstacles = p_obstacles;
	obstacle_grid.build(obstacles);
	rebuild();
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
	GRASS_SET_REBUILD(radius, MAX(p_radius, 1.0f))
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
	if (p_path.is_empty()) {
		season_green = 1.0f;
		set_dryness(dryness);
	}
}

void GrassField3D::set_season_response(float p_response) {
	season_response = CLAMP(p_response, 0.0f, 1.0f);
	set_dryness(dryness);
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
	const float moisture = float(p_season.get("soil_moisture", 1.0f));
	const float temperature = float(p_season.get("temperature", 10.0f));
	const float wet = Math::smoothstep(0.1f, 0.4f, moisture);
	const float warm = Math::smoothstep(-8.0f, 2.0f, temperature);
	return CLAMP(wet * warm, 0.0f, 1.0f);
}

// Once a second: the season's greenness into the blades' and the ground's dryness, and a new far
// colour bake when the look has moved enough to show.
void GrassField3D::_season_step() {
	if (season_source.is_empty() || season_wait-- > 0) {
		return;
	}
	season_wait = 60;
	WeatherMap3D *wm = Object::cast_to<WeatherMap3D>(get_node_or_null(season_source));
	if (!wm) {
		return;
	}
	if (crop != CROP_NONE && !plots.is_empty()) {
		// Fields with a sowing day grow through the year.
		const int day = int(wm->get_season().get("day_of_year", -1));
		if (day != crop_day) {
			crop_day = day;
			_update_plot_stages(day);
			_queue_far_bake();
		}
	}
	const float g = season_greenness(wm->get_season());
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
	ClassDB::bind_method(D_METHOD("get_far_color_lobes"), &GrassField3D::get_far_color_lobes);
	ClassDB::bind_method(D_METHOD("get_far_published"), &GrassField3D::get_far_published);
	ClassDB::bind_method(D_METHOD("set_far_match_view", "enabled"), &GrassField3D::set_far_match_view);
	ClassDB::bind_method(D_METHOD("is_far_match_view"), &GrassField3D::is_far_match_view);
	GRASS_BIND(set_sun_light, get_sun_light, "path")
	ADD_SIGNAL(MethodInfo("far_color_changed"));
	GRASS_BIND(set_canopy_field, get_canopy_field, "path")
	GRASS_BIND(set_undergrowth_field, get_undergrowth_field, "path")
	GRASS_BIND(set_obstacles, get_obstacles, "obstacles")
	GRASS_BIND(set_paths, get_paths, "paths")
	GRASS_BIND(set_trample_resistance, get_trample_resistance, "passes")
	GRASS_BIND(set_density, get_density, "density")
	GRASS_BIND(set_crop, get_crop, "crop")
	GRASS_BIND(set_crop_stage, get_crop_stage, "stage")
	GRASS_BIND(set_row_spacing, get_row_spacing, "spacing")
	GRASS_BIND(set_drill_angle, get_drill_angle, "angle")
	GRASS_BIND(set_fields, get_fields, "fields")
	ClassDB::bind_method(D_METHOD("get_field_stage", "index"), &GrassField3D::get_field_stage);
	ClassDB::bind_method(D_METHOD("_on_crop_fields_changed"), &GrassField3D::_on_crop_fields_changed);
	ClassDB::bind_static_method("GrassField3D", D_METHOD("crop_calendar_stage", "sown_day", "harvest_day", "day"), &GrassField3D::crop_calendar_stage);
	ClassDB::bind_static_method("GrassField3D", D_METHOD("crop_canopy_look", "crop", "stage"), &GrassField3D::crop_canopy_look);
	GRASS_BIND(set_blade_height, get_blade_height, "height")
	GRASS_BIND(set_blade_width, get_blade_width, "width")
	GRASS_BIND(set_clump_size, get_clump_size, "size")
	GRASS_BIND(set_lean, get_lean, "lean")
	GRASS_BIND(set_base_color, get_base_color, "color")
	GRASS_BIND(set_tip_color, get_tip_color, "color")
	GRASS_BIND(set_dry_color, get_dry_color, "color")
	GRASS_BIND(set_dryness, get_dryness, "dryness")
	GRASS_BIND(set_season_source, get_season_source, "path")
	GRASS_BIND(set_season_response, get_season_response, "response")
	ClassDB::bind_method(D_METHOD("get_season_green"), &GrassField3D::get_season_green);
	ClassDB::bind_method(D_METHOD("get_effective_dryness"), &GrassField3D::get_effective_dryness);
	ClassDB::bind_static_method("GrassField3D", D_METHOD("season_greenness", "season"), &GrassField3D::season_greenness);
	GRASS_BIND(set_color_variation, get_color_variation, "amount")
	GRASS_BIND(set_radius, get_radius, "radius")
	GRASS_BIND(set_tile_size, get_tile_size, "size")
	GRASS_BIND(set_fallback_wind, get_fallback_wind, "wind")
	GRASS_BIND(set_wind_response, get_wind_response, "response")
	ClassDB::bind_method(D_METHOD("set_trample_enabled", "enabled"), &GrassField3D::set_trample_enabled);
	ClassDB::bind_method(D_METHOD("is_trample_enabled"), &GrassField3D::is_trample_enabled);
	GRASS_BIND(set_blade_segments, get_blade_segments, "segments")
	GRASS_BIND(set_view_facing, get_view_facing, "facing")
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
	ClassDB::bind_method(D_METHOD("get_build_time_msec"), &GrassField3D::get_build_time_msec);
	ClassDB::bind_method(D_METHOD("get_material"), &GrassField3D::get_material);

	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "data_map", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "TerrainDataMap3D"), "set_data_map", "get_data_map");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "rules", PROPERTY_HINT_RESOURCE_TYPE, "PlantSpecies"), "set_rules", "get_rules");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "canopy_field", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "VegetationField3D"), "set_canopy_field", "get_canopy_field");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "undergrowth_field", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "VegetationField3D"), "set_undergrowth_field", "get_undergrowth_field");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_VECTOR4_ARRAY, "obstacles"), "set_obstacles", "get_obstacles");
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
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lean", PROPERTY_HINT_RANGE, "0,1.4,0.01,radians_as_degrees"), "set_lean", "get_lean");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "base_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_base_color", "get_base_color");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "tip_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_tip_color", "get_tip_color");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "dry_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_dry_color", "get_dry_color");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "dryness", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_dryness", "get_dryness");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "season_source", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "WeatherMap3D"), "set_season_source", "get_season_source");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "season_response", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_season_response", "get_season_response");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "color_variation", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_color_variation", "get_color_variation");
	ADD_GROUP("Ground", "ground_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "ground_tint_enabled"), "set_ground_tint_enabled", "is_ground_tint_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "ground_tint"), "set_ground_tint", "get_ground_tint");
	ADD_GROUP("Range", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "radius", PROPERTY_HINT_RANGE, "1,500,0.5,or_greater,suffix:m"), "set_radius", "get_radius");
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

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
#include "vegetation_light.h"

#include "vegetation_field_3d.h"

#include "core/config/engine.h"
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

constexpr int FILL_HEADER = 40;
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
	code += "uniform sampler2D trample_map : filter_linear, repeat_disable;\nuniform vec4 trample_rect;\n";
	code += "global uniform sampler2D weather_state_map;\n";
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
	code += R"(
varying float v_t;
varying float v_dry;
varying float v_wet;
varying vec3 v_tint;
varying float v_light;

uniform float color_variation;
)";
	code += GRASS_COLOR_CODE;
	code += VegetationLight::SHADER_CODE;
	code += R"(

void vertex() {
	float h = INSTANCE_CUSTOM.x;
	float w = INSTANCE_CUSTOM.y;
	float light_q = floor(INSTANCE_CUSTOM.z / 4.0);
	float rest_lean = INSTANCE_CUSTOM.z - light_q * 4.0;
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
}

uniform float dryness;
uniform float dormant; // Share of blades dead to the crown in a drought or hard winter (season).
uniform float sward_lai = 1.0;

void fragment() {
	float t = smoothstep(0.0, 1.0, v_t);
	// Shade keeps grass green; a sunny slope dries it first.
	float dry_share = clamp(dryness * (1.0 - 0.7 * (1.0 - clamp(v_light, 0.0, 1.0))) + 0.25 * smoothstep(1.0, 1.3, v_light), 0.0, 1.0);
	vec3 tipc = mix(tip_color, dry_color, step(1.0 - dry_share, v_dry) * smoothstep(0.3, 1.0, v_t));
	vec3 c = mix(base_color, tipc, t) * v_tint;
	// Some blades of the dry fraction are dead from the root (thatch), straw that greys with age.
	float dead = step(1.0 - max(dry_share * 0.3, dormant * 0.85), v_dry);
	c = mix(c, dry_color * mix(0.7, 1.0, t) * (0.75 + 0.5 * fract(v_dry * 31.0)), dead);
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
	ALBEDO = c * mix(1.0, wet_factor, v_wet);
	ROUGHNESS = 0.55 * mix(1.0, wet_factor, 0.5 * v_wet) * mix(1.0, 0.4, v_wet);
	SPECULAR = 0.35;
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
	Ref<Shader> shader;
	shader.instantiate();
	shader->set_code(code);
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
}

int GrassField3D::_lod_for(float p_distance) const {
	const float f = p_distance / MAX(radius, 0.001f);
	return f < 0.25f ? 0 : (f < 0.5f ? 1 : (f < 0.75f ? 2 : 3));
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
	// Wider blades where fewer remain, so the field keeps its coverage at a distance.
	const float width = blade_width / Math::sqrt(keep);
	if (gpu_fill && is_gpu_fill_available()) {
		_build_tile_gpu(p_tile, p_lod, p_map, r.ptr(), canopy, light_grid, sward_grid, lg, width);
		return;
	}
	LocalVector<Transform3D> xforms;
	LocalVector<Color> customs;
	const bool rocks = _tile_has_obstacles(x0, z0);
	for (int iz = iz0; iz < iz1; iz++) {
		for (int ix = ix0; ix < ix1; ix++) {
			const uint32_t hb = cell_hash(ix, iz, 0x9E3779B9u);
			// The rank decides which blades a coarser level keeps: the same ones, so nothing pops.
			// A blue-noise mask instead of a hash: every threshold keeps an even spread, so thinned
			// grass has no clumps and gaps.
			const float rank = blue_rank(ix, iz);
			if (rank >= keep) {
				continue;
			}
			const uint32_t hj = cell_hash(ix, iz, 0x85EBCA6Bu);
			const float x = (ix + unit(hj, 0)) * cell;
			const float z = (iz + unit(hj, 8)) * cell;
			if (rocks && obstacle_grid.blocks(Vector2(x, z), OBSTACLE_MARGIN)) {
				continue;
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
				if (rank >= keep * suit) {
					continue;
				}
				y = p_map->get_height_at(p);
			}
			// Clumps (Ghost of Tsushima): blades of a clump share height, facing and dryness.
			const int cx = int(Math::floor(x / clump_size));
			const int cz = int(Math::floor(z / clump_size));
			const uint32_t hc = cell_hash(cx, cz, 0xC2B2AE35u);
			const float clump_h = unit(hc, 0);
			const float clump_yaw = unit(hc, 8) * Math::TAU;
			const float rnd = unit(hb, 16);
			const float lx = CLAMP(x - x0, 0.0f, float(lg - 1));
			const float lz = CLAMP(z - z0, 0.0f, float(lg - 1));
			const int gx = MIN(int(lx), lg - 2);
			const int gz = MIN(int(lz), lg - 2);
			const float light = Math::lerp(Math::lerp(light_grid[gz * lg + gx], light_grid[gz * lg + gx + 1], lx - gx), Math::lerp(light_grid[(gz + 1) * lg + gx], light_grid[(gz + 1) * lg + gx + 1], lx - gx), lz - gz);
			// Shade avoidance (Franklin 2008): grass in shade grows longer and laxer; in full sun on a
			// sunny slope, shorter.
			const float shade_t = 1.0f - CLAMP(light, 0.0f, 1.0f);
			const float sun_t = Math::smoothstep(1.0f, 1.3f, light);
			const float height = Math::lerp(blade_height.x, blade_height.y, 0.65f * clump_h + 0.35f * unit(hj, 16)) * (1.0f + 0.35f * shade_t - 0.15f * sun_t);
			const float yaw = clump_yaw + (unit(hj, 24) - 0.5f) * 1.6f;
			const Transform3D world(Basis(Vector3(0, 1, 0), yaw), Vector3(x, y, z));
			xforms.push_back(to_local * world);
			// w: the outer band's drop order in the integer part (the rank within the coarsest level,
			// so the last blades left are evenly spread too), the blade's random value in the fraction.
			const float order = Math::floor(Math::fmod(rank * 8.0f, 1.0f) * 255.0f);
			// z: the light in its integer part over 4 (VegetationLight::pack), the rest lean (under 4).
			const float blade_lean = MIN(lean * (0.4f + 1.2f * unit(hc, 16)) * (1.0f + 0.8f * shade_t), 3.99f);
			customs.push_back(Color(height, width, VegetationLight::pack(light) * 4.0f + blade_lean, order + 0.999f * Math::fmod(rnd + 0.37f * unit(hc, 24), 1.0f)));
		}
	}
	Tile tile;
	tile.lod = p_lod;
	if (!xforms.is_empty()) {
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
void GrassField3D::_build_tile_gpu(const Vector2i &p_tile, int p_lod, TerrainDataMap3D *p_map, const PlantSpecies *p_rules, const VegetationField3D *p_canopy, const LocalVector<float> &p_light, const LocalVector<float> &p_sward, int p_lg, float p_width) {
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
				float v = p_rules->get_suitability(p_map, q);
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
	in.resize(off_height + int(heights.size()));
	float *w = in.ptrw();
	memcpy(w + off_cand, cand.ptr(), cand.size() * sizeof(float));
	memcpy(w + off_suit, suit.ptr(), suit.size() * sizeof(float));
	memcpy(w + off_light, p_light.ptr(), p_light.size() * sizeof(float));
	memcpy(w + off_height, heights.ptr(), heights.size() * sizeof(float));

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

void GrassField3D::_update_fade(const Vector2 &p_center) {
	const Vector4 band(p_center.x, p_center.y, radius * 0.97f, radius * 0.37f);
	if (band.is_equal_approx(fade_band)) {
		return;
	}
	fade_band = band;
	if (material.is_valid()) {
		material->set_shader_parameter("fade_band", band);
	}
	// The ground's band is global: the widest field publishes it, so a near undercoat field does
	// not pull the ground's blade colour in to its own short radius.
	if (fade_owner && fade_owner != this && fade_owner->radius > radius) {
		return;
	}
	// The widest field even without a ground tint of its own: its blades are the ones seen last.
	if (is_inside_tree()) {
		fade_owner = this;
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
constexpr float FAR_BAKE_SUN_DEG = 30.0f;
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
	const float hmax = MAX(blade_height.x, blade_height.y) * 1.1f;
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
	for (int iz = iz0; iz < iz1; iz++) {
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

	Ref<Environment> env;
	env.instantiate();
	Ref<Sky> sky;
	sky.instantiate();
	sky->set_material(memnew(ProceduralSkyMaterial));
	sky->set_process_mode(Sky::PROCESS_MODE_QUALITY);
	env->set_background(Environment::BG_SKY);
	env->set_sky(sky);
	env->set_ambient_source(Environment::AMBIENT_SOURCE_SKY);
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
	constexpr int VIEWS = 2 * FAR_BAKE_AZIMUTHS + 1;
	if (far_bake_wait == 0) {
		const bool bare = far_bake_state >= WHITE;
		const int azimuth = far_bake_state < WHITE ? far_bake_state : (far_bake_state == WHITE ? 0 : far_bake_state - WHITE - 1);
		const Color gt = ground_tint_enabled ? Color(ground_tint.r, ground_tint.g, ground_tint.b) : Color(0.11f, 0.12f, 0.06f);
		far_bake_ground_mat->set_albedo(bare ? (far_bake_state == WHITE ? Color(1, 1, 1) : Color(0, 0, 0)) : gt);
		far_bake_ground_mat->set_roughness(0.95f);
		far_bake_ground_mat->set_specular(0.1f);
		far_bake_blades->set_visible(!bare);
		far_bake_sun->set_rotation_degrees(Vector3(-FAR_BAKE_SUN_DEG, 360.0f / FAR_BAKE_AZIMUTHS * azimuth, 0.0f));
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
	// The albedo each azimuth needs. The white's diffuse light does not turn with the sun (the
	// ground is Lambertian under its sheen), so one white view serves all four. The sheen never
	// takes more than three quarters of the blades' light (a dark sward seen very low).
	const Color diffuse = far_bake_view[WHITE] - far_bake_view[WHITE + 1];
	if (diffuse.r <= 1e-4f || diffuse.g <= 1e-4f || diffuse.b <= 1e-4f) {
		return;
	}
	Color need[FAR_BAKE_AZIMUTHS];
	Color mean(0, 0, 0, 0);
	for (int k = 0; k < FAR_BAKE_AZIMUTHS; k++) {
		const Color &b = far_bake_view[k];
		const Color &black = far_bake_view[WHITE + 1 + k];
		for (int c = 0; c < 3; c++) {
			need[k][c] = MAX(b[c] - black[c], 0.25f * b[c]) / diffuse[c];
		}
		mean += need[k] / float(FAR_BAKE_AZIMUTHS);
	}
	if (!Math::is_finite(mean.r) || !Math::is_finite(mean.g) || !Math::is_finite(mean.b) || mean.r <= 0.0f || mean.g <= 0.0f || mean.b <= 0.0f) {
		return;
	}
	// The azimuths are 0 (sun behind the view, the hot spot), 90, 180 (into the sun) and 270, so
	// c(phi) = a0 + a1 cos(phi) + a2 cos(2 phi) through them: a1 = (front - back) / 2,
	// a2 = (front + back - sides) / 4. Bounded so the factor stays between 0.1 and 1.9.
	for (int c = 0; c < 3; c++) {
		float l1 = 0.5f * (need[0][c] - need[2][c]) / mean[c];
		float l2 = 0.25f * (need[0][c] + need[2][c] - need[1][c] - need[3][c]) / mean[c];
		const float total = Math::abs(l1) + Math::abs(l2);
		if (total > 0.9f) {
			l1 *= 0.9f / total;
			l2 *= 0.9f / total;
		}
		far_lobe1[c] = l1;
		far_lobe2[c] = l2;
	}
	far_lobe1.a = far_lobe2.a = 0.0f;
	far_color_baked = Color(CLAMP(mean.r, 0.0f, 1.0f), CLAMP(mean.g, 0.0f, 1.0f), CLAMP(mean.b, 0.0f, 1.0f), 1.0f);
	far_baked = true;
	print_verbose(vformat("GrassField3D \"%s\": far colour %s (hand-matched %s), sun azimuth lobes %s %s.", get_name(), far_color_baked, _far_color_hand(), far_lobe1, far_lobe2));
	if (fade_owner == this) {
		_publish_far();
	}
	emit_signal(SNAME("far_color_changed"));
}

// The far colour and its azimuth lobes, for grass_ground(). The fade owner's only.
void GrassField3D::_publish_far() {
	RenderingServer *rs = RenderingServer::get_singleton();
	const Color far = get_far_color();
	rs->global_shader_parameter_set("grass_ground_far", Vector4(far.r, far.g, far.b, 1.0f));
	const Color l1 = far_baked ? far_lobe1 : Color(0, 0, 0, 0);
	const Color l2 = far_baked ? far_lobe2 : Color(0, 0, 0, 0);
	rs->global_shader_parameter_set("grass_ground_far_lobe1", Vector4(l1.r, l1.g, l1.b, 0.0f));
	rs->global_shader_parameter_set("grass_ground_far_lobe2", Vector4(l2.r, l2.g, l2.b, 0.0f));
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
// between them. The lobes were baked with the sun 30 degrees up; the azimuth matters less as the
// sun climbs (none at the zenith) and goes with the sun below the horizon.
void GrassField3D::_publish_sun_view(const Vector3 &p_camera) {
	Vector4 sun_v;
	DirectionalLight3D *sun = _get_sun();
	if (sun && sun->is_visible_in_tree() && sun->get_param(Light3D::PARAM_ENERGY) > 0.0f) {
		const Vector3 d = -sun->get_global_transform().basis.get_column(Vector3::AXIS_Z).normalized(); // Where the light travels.
		const float h = Vector2(d.x, d.z).length();
		const float up = -d.y; // Sine of the sun's elevation.
		if (h > 1e-4f) {
			const float strength = MIN(h / Math::cos(Math::deg_to_rad(FAR_BAKE_SUN_DEG)), 1.15f) * CLAMP((up + 0.05f) / 0.1f, 0.0f, 1.0f);
			sun_v = Vector4(d.x / h, d.z / h, strength, 0.0f);
		}
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
	if (!ground_baked) {
		_bake_ground(map);
	}
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	const Vector2 c(p_center.x, p_center.z);
	const float reach = radius + tile_size * 0.75f;
	_update_fade(c);
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
			if (!existing || existing->lod != _lod_for(d)) {
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
	sun_mean = VegetationLight::sun_mean(p_map);
	fade_band = Vector4(); // Send the band and the far colour again.
	if (is_inside_tree()) {
		// The fallback wind for the other plants (PlantGenerator's shader), so they sway with the grass.
		RenderingServer::get_singleton()->global_shader_parameter_set("grass_wind", Vector4(fallback_wind.x, fallback_wind.y, wind_response, 0.0f));
	}
	if (!ground_tint_enabled || !is_inside_tree()) {
		return;
	}
	Vector4 rect;
	Ref<Image> img;
	Ref<Image> litter_img;
	if (p_map) {
		const int res = MIN(p_map->get_resolution(), 1024);
		const float size = p_map->get_size();
		const Vector3 c = p_map->get_global_position();
		rect = Vector4(c.x - size * 0.5f, c.z - size * 0.5f, size, size);
		Ref<PlantSpecies> r = rules;
		if (r.is_null()) {
			r.instantiate();
		}
		const VegetationField3D *canopy = _get_canopy(r.ptr());
		const VegetationField3D *under = _get_undergrowth();
		Vector<uint8_t> data;
		data.resize(res * res);
		uint8_t *w = data.ptrw();
		// The undergrowth's litter where its stands have shaded the sward out (bracken's dead
		// fronds), as premultiplied colour stored as its square root (the shader squares it back).
		Vector<uint8_t> ldata;
		uint8_t *lw = nullptr;
		bool any_litter = false;
		if (under) {
			ldata.resize(res * res * 4);
			lw = ldata.ptrw();
		}
		for (int z = 0; z < res; z++) {
			for (int x = 0; x < res; x++) {
				const Vector3 p(rect.x + (x + 0.5f) * size / res, 0.0f, rect.y + (z + 0.5f) * size / res);
				float suit = r->get_suitability(p_map, p);
				if (canopy && suit > 0.0f) {
					suit *= r->rule_weight(PlantSpecies::RULE_SHADE, canopy->get_stand_shade_at(p));
				}
				if (under) {
					Color lc;
					const float sh = under->sward_at(p, &lc);
					suit *= 1.0f - sh;
					uint8_t *o = lw + (z * res + x) * 4;
					o[0] = uint8_t(CLAMP(Math::sqrt(lc.r * lc.a), 0.0f, 1.0f) * 255.0f + 0.5f);
					o[1] = uint8_t(CLAMP(Math::sqrt(lc.g * lc.a), 0.0f, 1.0f) * 255.0f + 0.5f);
					o[2] = uint8_t(CLAMP(Math::sqrt(lc.b * lc.a), 0.0f, 1.0f) * 255.0f + 0.5f);
					o[3] = uint8_t(CLAMP(lc.a, 0.0f, 1.0f) * 255.0f + 0.5f);
					any_litter = any_litter || o[3] > 0;
				}
				w[z * res + x] = uint8_t(CLAMP(suit, 0.0f, 1.0f) * 255.0f + 0.5f);
			}
		}
		img = Image::create_from_data(res, res, false, Image::FORMAT_L8, data);
		if (any_litter) {
			litter_img = Image::create_from_data(res, res, false, Image::FORMAT_RGBA8, ldata);
		}
	} else {
		// No map: grass everywhere.
		rect = Vector4(-1e6f, -1e6f, 2e6f, 2e6f);
		Vector<uint8_t> data;
		data.push_back(255);
		img = Image::create_from_data(1, 1, false, Image::FORMAT_L8, data);
	}
	if (litter_img.is_null()) {
		// No undergrowth litter: one clear texel.
		Vector<uint8_t> none;
		none.resize(4);
		memset(none.ptrw(), 0, 4);
		litter_img = Image::create_from_data(1, 1, false, Image::FORMAT_RGBA8, none);
	}
	ground_texture = ImageTexture::create_from_image(img);
	ground_litter_texture = ImageTexture::create_from_image(litter_img);
	RenderingServer *rs = RenderingServer::get_singleton();
	rs->global_shader_parameter_set("grass_ground_map", ground_texture->get_rid());
	rs->global_shader_parameter_set("grass_ground_litter", ground_litter_texture->get_rid());
	rs->global_shader_parameter_set("grass_ground_rect", rect);
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
global uniform sampler2D grass_ground_map : filter_linear;
global uniform sampler2D grass_ground_litter : hint_default_black, filter_linear; // sqrt of premultiplied rgb, a: cover.
global uniform vec4 grass_ground_rect;
global uniform vec4 grass_ground_color; // rgb: the sward's root colour; a: how much it covers the soil.
global uniform vec4 grass_ground_dry; // rgb: thatch colour; a: the grass's dryness.
global uniform vec4 grass_ground_params; // x: colour variation.
global uniform vec4 grass_ground_fade; // xy: centre of the blades, z: where the last is gone, w: the band they thin over.
global uniform vec4 grass_ground_far; // rgb: the blades' colour seen from a distance.
global uniform vec4 grass_ground_far_lobe1; // rgb: its change with cos of the sun's azimuth from the view.
global uniform vec4 grass_ground_far_lobe2; // rgb: and with cos of twice that azimuth.
global uniform vec4 grass_ground_sun; // xy: the sun light's heading (xz); z: how much the azimuth counts.
global uniform vec4 grass_ground_view; // xyz: the camera.
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
	return cover * mix(grass_ground_color.a, 1.0, gone);
}

// The ground's specular under the sward: the blades hide the soil's sheen at the grazing angles a
// field is seen at, so a meadow from afar is near-Lambertian. Without this the far meadow beyond
// the blades turned pale grey against them (grass bench --far-match: the sheen was most of the
// step at the grass edge; 0.1 matches the blades).
float grass_ground_specular(vec3 world_pos, float specular) {
	return mix(specular, min(specular, 0.1), grass_ground_weight(world_pos));
}

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

// The ground's albedo under the vegetation: litter where the canopy closes (forest_floor()), the
// undergrowth's litter under its stands, then the sward's turf where grass grows.
vec3 grass_ground(vec3 world_pos, vec3 albedo) {
	albedo = forest_floor(world_pos, albedo);
	albedo = grass_ground_undergrowth(world_pos, albedo);
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
	vec3 turf = grass_turf(world_pos);
	if (gone > 0.0) {
		vec2 xz = world_pos.xz;
		vec3 far = grass_ground_far.rgb * grass_patch_tint(xz, grass_noise(xz * 3.0), 0.5, grass_ground_params.x);
		// A sward looked at down-sun shows its lit faces (the hot spot), into the sun its shadowed
		// sides: the bake's azimuth lobes, by the angle between the view and the sun's light.
		vec2 to = xz - grass_ground_view.xz;
		float to_len = length(to);
		if (grass_ground_sun.z > 0.0 && to_len > 1e-3) {
			float c1 = dot(to / to_len, grass_ground_sun.xy);
			far *= max(1.0 + grass_ground_sun.z * (grass_ground_far_lobe1.rgb * c1 + grass_ground_far_lobe2.rgb * (2.0 * c1 * c1 - 1.0)), vec3(0.1));
		}
		// Keep the turf's streaks and thatch as brightness, so the far sward is not flat.
		float lum = dot(turf, vec3(0.3, 0.59, 0.11)) / max(dot(grass_ground_color.rgb, vec3(0.3, 0.59, 0.11)), 1e-3);
		turf = mix(turf, far * clamp(lum, 0.5, 1.5), gone);
	}
	return mix(albedo, turf, cover * mix(grass_ground_color.a, 1.0, gone));
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

void GrassField3D::rebuild() {
	for (KeyValue<Vector2i, Tile> &kv : tiles) {
		_free_tile(kv.value);
	}
	tiles.clear();
	blade_count = 0;
	blade_mesh.unref();
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

// Whether any rock reaches into a tile (so its blades need testing one by one).
bool GrassField3D::_tile_has_obstacles(float p_x0, float p_z0) const {
	if (obstacle_grid.is_empty()) {
		return false;
	}
	const float half = tile_size * 0.5f;
	const float reach = half * float(Math::SQRT2) + OBSTACLE_MARGIN;
	return obstacle_grid.clearance(Vector2(p_x0 + half, p_z0 + half), reach) <= reach;
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
	// The same blades either way: only the tiles are rebuilt, not the mesh or the material.
	for (KeyValue<Vector2i, Tile> &kv : tiles) {
		_free_tile(kv.value);
	}
	tiles.clear();
	blade_count = 0;
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
	GRASS_BIND(set_sun_light, get_sun_light, "path")
	ADD_SIGNAL(MethodInfo("far_color_changed"));
	GRASS_BIND(set_canopy_field, get_canopy_field, "path")
	GRASS_BIND(set_undergrowth_field, get_undergrowth_field, "path")
	GRASS_BIND(set_obstacles, get_obstacles, "obstacles")
	GRASS_BIND(set_density, get_density, "density")
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
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "sun_light", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "DirectionalLight3D"), "set_sun_light", "get_sun_light");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "density", PROPERTY_HINT_RANGE, "0.1,400,0.1,or_greater,suffix:/m²"), "set_density", "get_density");
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

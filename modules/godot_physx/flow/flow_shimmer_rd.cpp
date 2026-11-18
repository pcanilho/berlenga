/**************************************************************************/
/*  flow_shimmer_rd.cpp                                                   */
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

#include "flow_shimmer_rd.h"

#include "core/math/rect2i.h"
#include "core/math/vector4.h"
#include "servers/rendering/rendering_device_binds.h"

namespace {

// The sub-grid turbulence and the refractive index, shared in spirit with rt_volume_bend
// (raytracing_volume_inc.glsl): keep the constants the same in both.
const char *SHIMMER_SHADER = R"(
#[compute]
#version 450

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler3D density_tex; // r: temperature, a: smoke
layout(set = 0, binding = 1, std430) restrict readonly buffer Table {
	uint table_data[];
};
layout(set = 0, binding = 2) uniform sampler2D depth_tex;
layout(set = 0, binding = 3) uniform sampler2D source_tex;
layout(set = 0, binding = 4, rgba16f) uniform restrict writeonly image2D color_img;
layout(set = 0, binding = 5, std140) uniform Params {
	mat4 inv_view_projection; // clip -> world meters
	mat4 view_projection; // world meters -> clip
	vec4 camera; // xyz meters; w: units per meter
	uvec4 level[10]; // NvFlowSparseLevelParams
	vec4 block_size; // Flow units
	ivec4 info; // x: layerAndLevel, yz: frame size, w: max steps
	ivec4 rect; // xy: first pixel, zw: size
	vec4 world_min; // Flow units
	vec4 world_max;
	vec4 shimmer; // x: kelvin per Flow temperature unit, y: air kelvin, z: turbulence, w: time
} p;

#define BLOCK_DIM_BITS (p.level[1].xyz)
#define TABLE_DIM_LESS_ONE (p.level[2].xyz)
#define TABLE_DIM_BITS_X (p.level[3].x)
#define TABLE_DIM_BITS_XY (p.level[3].y)
#define LOCATION_OFFSET (p.level[3].w)
#define BLOCK_LEVEL_OFFSET_GLOBAL (p.level[4].z)
#define DIM_INV (uintBitsToFloat(p.level[7].xyz))
const uint NO_BLOCK = 0xFFFFFFFFu;

uint location_to_block(ivec4 loc) {
	uvec3 bucket = uvec3(loc.xyz) & TABLE_DIM_LESS_ONE;
	uint bucket1d = (bucket.z << TABLE_DIM_BITS_XY) | (bucket.y << TABLE_DIM_BITS_X) | bucket.x;
	uint range_begin = table_data[(bucket1d << 1u) + 0u];
	uint range_end = table_data[(bucket1d << 1u) + 1u];
	for (uint b = range_begin; b < range_end; b++) {
		uint i = (b << 2u) + LOCATION_OFFSET;
		if (int(table_data[i + 0u]) == loc.x && int(table_data[i + 1u]) == loc.y && int(table_data[i + 2u]) == loc.z && int(table_data[i + 3u]) == loc.w) {
			return b;
		}
	}
	return NO_BLOCK;
}

struct Accessor {
	ivec4 location;
	ivec3 offset;
	bool valid;
};

void accessor_update(inout Accessor a, ivec4 loc) {
	if (loc == a.location) {
		return;
	}
	a.location = loc;
	uint b = location_to_block(loc);
	a.valid = false;
	if (b != NO_BLOCK) {
		uint v = table_data[b + BLOCK_LEVEL_OFFSET_GLOBAL];
		a.valid = (v >> 31u) != 0u;
		ivec3 base = ivec3(int((v << 1u) | 1u) & 0xFFF, int((v >> 10u) | 1u) & 0x7FF, int((v >> 20u) | 1u) & 0x7FF);
		a.offset = base - (loc.xyz << ivec3(BLOCK_DIM_BITS));
	}
}

// Temperature at a point (Flow units); false where no block is allocated.
bool temperature_at(vec3 pos, vec3 cell_inv, inout Accessor a, out float r_t) {
	vec3 vidxf = pos * cell_inv;
	ivec4 loc = ivec4(ivec3(floor(vidxf)) >> ivec3(BLOCK_DIM_BITS), p.info.x);
	accessor_update(a, loc);
	if (!a.valid) {
		r_t = 0.0;
		return false;
	}
	r_t = textureLod(density_tex, (vidxf + vec3(a.offset)) * DIM_INV, 0.0).r;
	return true;
}

bool intersect_box(vec3 ro, vec3 rd_inv, vec3 bmin, vec3 bmax, out float t0, out float t1) {
	vec3 a = (bmin - ro) * rd_inv;
	vec3 b = (bmax - ro) * rd_inv;
	vec3 lo = min(a, b);
	vec3 hi = max(a, b);
	t0 = max(max(lo.x, lo.y), lo.z);
	t1 = min(min(hi.x, hi.y), hi.z);
	return t1 > max(t0, 0.0);
}

// Air's refractive index minus one at sea level and 15 C (visible light), scaling with density, so as 1 / T
// at constant pressure (Gladstone-Dale).
const float N0 = 2.9e-4;
// Sub-grid turbulence: correlation length (m), the plume's rise speed carrying it (m/s, Morton, Taylor and
// Turner 1956 for a small fire), and the value noise's own rms (measured) to normalise it.
const float EDDY = 0.04;
const float RISE = 1.5;
const float NOISE_RMS = 0.40;

float hash3(vec3 q) {
	q = fract(q * 0.3183099 + 0.1) * 17.0;
	return fract(q.x * q.y * q.z * (q.x + q.y + q.z)) * 2.0 - 1.0;
}

// Value noise's gradient, analytic (Quilez, "Value noise derivatives"), per lattice unit.
vec3 noise_grad(vec3 x) {
	vec3 i = floor(x);
	vec3 f = x - i;
	vec3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
	vec3 du = 30.0 * f * f * (f * (f - 2.0) + 1.0);
	float a = hash3(i);
	float b = hash3(i + vec3(1, 0, 0));
	float c = hash3(i + vec3(0, 1, 0));
	float d = hash3(i + vec3(1, 1, 0));
	float e = hash3(i + vec3(0, 0, 1));
	float f1 = hash3(i + vec3(1, 0, 1));
	float g = hash3(i + vec3(0, 1, 1));
	float h = hash3(i + vec3(1, 1, 1));
	float k1 = b - a;
	float k2 = c - a;
	float k3 = e - a;
	float k4 = a - b - c + d;
	float k5 = a - c - e + g;
	float k6 = a - b - e + f1;
	float k7 = -a + b + c - d + e - f1 - g + h;
	return du * vec3(k1 + k4 * u.y + k6 * u.z + k7 * u.y * u.z, k2 + k5 * u.z + k4 * u.x + k7 * u.z * u.x, k3 + k6 * u.x + k5 * u.y + k7 * u.x * u.y);
}

// The turbulent temperature field's gradient at a point (meters), per unit rms: eddies carried up at the
// plume's rise and renewed about once per their turnover (two layers cross-faded, variance kept).
vec3 eddy_grad(vec3 pos_m, float time) {
	float tau = time * RISE / (3.0 * EDDY);
	float k = floor(tau);
	float w = tau - k;
	vec3 q = pos_m / EDDY - vec3(0.0, time * RISE / EDDY, 0.0);
	vec3 g0 = noise_grad(q + vec3(17.31, 3.7, 9.13) * k);
	vec3 g1 = noise_grad(q + vec3(17.31, 3.7, 9.13) * (k + 1.0));
	return (g0 * (1.0 - w) + g1 * w) / (sqrt(w * w + (1.0 - w) * (1.0 - w)) * NOISE_RMS * EDDY);
}

float scene_distance(vec2 uv, float depth, vec3 cam) {
	if (depth <= 1e-7) {
		return 1e6;
	}
	vec4 h = p.inv_view_projection * vec4(uv * 2.0 - 1.0, depth, 1.0);
	return length(h.xyz / h.w - cam);
}

void main() {
	ivec2 local = ivec2(gl_GlobalInvocationID.xy);
	if (local.x >= p.rect.z || local.y >= p.rect.w) {
		return;
	}
	ivec2 px = p.rect.xy + local;
	vec2 size = vec2(p.info.yz);
	vec2 uv = (vec2(px) + 0.5) / size;
	float depth = texelFetch(depth_tex, px, 0).r;
	float upm = p.camera.w;
	vec3 cam_m = p.camera.xyz;

	vec4 near_h = p.inv_view_projection * vec4(uv * 2.0 - 1.0, 1.0, 1.0);
	vec4 far_h = p.inv_view_projection * vec4(uv * 2.0 - 1.0, max(depth, 1e-7), 1.0);
	vec3 dir = normalize(far_h.xyz / far_h.w - near_h.xyz / near_h.w);
	float scene_t = scene_distance(uv, depth, cam_m); // meters

	vec3 cam = cam_m * upm;
	float t0;
	float t1;
	if (!intersect_box(cam, 1.0 / dir, p.world_min.xyz, p.world_max.xyz, t0, t1)) {
		return;
	}
	t0 = max(t0, 0.0) / upm;
	t1 = min(t1 / upm, scene_t);
	if (t1 <= t0) {
		return;
	}

	vec3 cell = p.block_size.xyz / vec3(p.level[0].xyz + 1u);
	vec3 cell_inv = 1.0 / cell;
	vec3 cell_m = cell / upm;
	float h = min(min(cell_m.x, cell_m.y), cell_m.z);
	float kelvin = p.shimmer.x;
	float air = p.shimmer.y;
	float turbulence = p.shimmer.z;
	float kick = sqrt(EDDY / h); // sub-grid kicks: correlated over EDDY, not the whole step

	Accessor acc;
	acc.location = ivec4(0x40000000);
	acc.valid = false;
	vec3 bend = vec3(0.0);
	float w_sum = 0.0;
	float t_sum = 0.0;
	// A fixed offset per pixel (interleaved gradient noise, Jimenez 2014): no step bands.
	float t = t0 + h * fract(52.9829189 * fract(dot(vec2(px), vec2(0.06711056, 0.00583715))));
	for (int i = 0; i < p.info.w && t < t1; i++) {
		vec3 pos = cam + dir * (t * upm);
		float temp;
		if (!temperature_at(pos, cell_inv, acc, temp)) {
			// An empty block: jump to its far side.
			vec3 lo = floor(pos / p.block_size.xyz) * p.block_size.xyz;
			vec3 hi = lo + p.block_size.xyz;
			vec3 inv = 1.0 / dir;
			vec3 te = max((lo - pos) * inv, (hi - pos) * inv);
			t += max(min(min(te.x, te.y), te.z), 0.0) / upm + 1e-4;
			continue;
		}
		float excess = max(temp, 0.0) * kelvin;
		if (excess > 0.5) {
			Accessor nb = acc;
			float tx0, tx1, ty0, ty1, tz0, tz1;
			temperature_at(pos - vec3(cell.x, 0, 0), cell_inv, nb, tx0);
			temperature_at(pos + vec3(cell.x, 0, 0), cell_inv, nb, tx1);
			temperature_at(pos - vec3(0, cell.y, 0), cell_inv, nb, ty0);
			temperature_at(pos + vec3(0, cell.y, 0), cell_inv, nb, ty1);
			temperature_at(pos - vec3(0, 0, cell.z), cell_inv, nb, tz0);
			temperature_at(pos + vec3(0, 0, cell.z), cell_inv, nb, tz1);
			vec3 grad_t = vec3(tx1 - tx0, ty1 - ty0, tz1 - tz0) / (2.0 * cell_m) * kelvin; // K/m
			float temp_k = air + excess;
			float dn_dt = -N0 * 288.15 / (temp_k * temp_k); // per kelvin
			vec3 g = dn_dt * grad_t * h;
			if (turbulence > 0.0) {
				g += dn_dt * (turbulence * excess) * eddy_grad(pos / upm, p.shimmer.w) * (h * kick);
			}
			vec3 perp = g - dir * dot(g, dir);
			bend += perp;
			float w = length(perp);
			w_sum += w;
			t_sum += w * t;
		}
		t += h;
	}
	if (w_sum <= 0.0) {
		return;
	}
	// The bent ray leaves the deflection's centroid in its new direction; the scene point it lands on is taken
	// at the straight ray's scene distance.
	float tc = t_sum / w_sum;
	float d_scene = min(scene_t, 1e4);
	if (d_scene <= tc) {
		return;
	}
	vec3 bent = normalize(dir + bend);
	vec3 land = cam_m + dir * tc + bent * (d_scene - tc);
	vec4 clip = p.view_projection * vec4(land, 1.0);
	if (clip.w <= 0.0) {
		return;
	}
	vec2 uv2 = clip.xy / clip.w * 0.5 + 0.5;
	if (any(lessThan(uv2, vec2(0.0))) || any(greaterThan(uv2, vec2(1.0)))) {
		return;
	}
	// What the landing pixel shows lies in front of the hot air (a post, a hand): keep this pixel as it is.
	float depth2 = textureLod(depth_tex, uv2, 0.0).r;
	if (scene_distance(uv2, depth2, cam_m) < tc) {
		return;
	}
	imageStore(color_img, px, textureLod(source_tex, uv2, 0.0));
}
)";

constexpr uint32_t PARAMS_WORDS = 16 + 16 + 4 + 40 + 4 + 4 + 4 + 4 + 4 + 4;

} // namespace

bool FlowShimmerRD::_ensure(RenderingDevice *p_rd) {
	if (pipeline.is_valid()) {
		return true;
	}
	if (failed) {
		return false;
	}
	failed = true;
	Ref<RDShaderFile> file;
	file.instantiate();
	ERR_FAIL_COND_V_MSG(file->parse_versions_from_text(SHIMMER_SHADER) != OK, false, "PhysXFlow3D: heat shimmer shader failed to compile.");
	Ref<RDShaderSPIRV> spirv = file->get_spirv();
	ERR_FAIL_COND_V(spirv.is_null(), false);
	const String err = spirv->get_stage_compile_error(RD::SHADER_STAGE_COMPUTE);
	ERR_FAIL_COND_V_MSG(!err.is_empty(), false, "PhysXFlow3D: heat shimmer shader: " + err);
	shader = p_rd->shader_create_from_spirv(file->get_spirv_stages());
	ERR_FAIL_COND_V(!shader.is_valid(), false);
	pipeline = p_rd->compute_pipeline_create(shader);
	ERR_FAIL_COND_V(!pipeline.is_valid(), false);
	RD::SamplerState linear;
	linear.mag_filter = RD::SAMPLER_FILTER_LINEAR;
	linear.min_filter = RD::SAMPLER_FILTER_LINEAR;
	linear.repeat_u = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	linear.repeat_v = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	linear.repeat_w = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	linear_sampler = p_rd->sampler_create(linear);
	RD::SamplerState point;
	point.repeat_u = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	point.repeat_v = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	point_sampler = p_rd->sampler_create(point);
	params_buffer = p_rd->uniform_buffer_create(PARAMS_WORDS * 4);
	failed = false;
	return true;
}

RID FlowShimmerRD::get_source_copy(RenderingDevice *p_rd, const Size2i &p_size) {
	if (!source_copy.is_valid() || source_size != p_size) {
		if (source_copy.is_valid()) {
			p_rd->free_rid(source_copy);
		}
		RD::TextureFormat tf;
		tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		tf.width = p_size.x;
		tf.height = p_size.y;
		tf.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT;
		source_copy = p_rd->texture_create(tf, RD::TextureView());
		source_size = p_size;
	}
	return source_copy;
}

void FlowShimmerRD::run(RenderingDevice *p_rd, const FlowBackend::ScatterSource &p_source, const Params &p_params, const RID &p_depth, const RID &p_color, const Size2i &p_size) {
	if (!p_source.valid || p_size.x <= 0 || p_size.y <= 0 || !source_copy.is_valid() || source_size != p_size || !_ensure(p_rd)) {
		return;
	}
	// The gas's screen rectangle: its bounds' corners projected (all of the screen when one is behind the eye).
	const Projection view_projection = p_params.inv_view_projection.inverse();
	const real_t inv_upm = 1.0 / FlowBackend::UNITS_PER_METER;
	Vector2 lo(1e30, 1e30);
	Vector2 hi(-1e30, -1e30);
	bool full = false;
	for (int c = 0; c < 8 && !full; c++) {
		const Vector3 corner = Vector3((c & 1) ? p_source.world_max[0] : p_source.world_min[0], (c & 2) ? p_source.world_max[1] : p_source.world_min[1], (c & 4) ? p_source.world_max[2] : p_source.world_min[2]) * inv_upm;
		const Vector4 h = view_projection.xform(Vector4(corner.x, corner.y, corner.z, 1.0));
		if (h.w <= 1e-4) {
			full = true;
			break;
		}
		const Vector2 uv = Vector2(h.x / h.w, h.y / h.w) * 0.5 + Vector2(0.5, 0.5);
		lo = lo.min(uv);
		hi = hi.max(uv);
	}
	Rect2i rect(0, 0, p_size.x, p_size.y);
	if (!full) {
		// A few pixels of margin: the bent rays land a little outside.
		const Vector2i a = Vector2i(Math::floor(lo.x * p_size.x) - 4, Math::floor(lo.y * p_size.y) - 4);
		const Vector2i b = Vector2i(Math::ceil(hi.x * p_size.x) + 4, Math::ceil(hi.y * p_size.y) + 4);
		rect = Rect2i(a, b - a).intersection(Rect2i(0, 0, p_size.x, p_size.y));
	}
	if (rect.size.x <= 0 || rect.size.y <= 0) {
		return;
	}

	uint32_t words[PARAMS_WORDS] = {};
	float *f = reinterpret_cast<float *>(words);
	int32_t *iw = reinterpret_cast<int32_t *>(words);
	uint32_t w = 0;
	for (const Projection *m : { &p_params.inv_view_projection, &view_projection }) {
		for (int c = 0; c < 4; c++) {
			for (int r = 0; r < 4; r++) {
				f[w++] = (float)m->columns[c][r];
			}
		}
	}
	f[w++] = (float)p_params.camera_position.x;
	f[w++] = (float)p_params.camera_position.y;
	f[w++] = (float)p_params.camera_position.z;
	f[w++] = FlowBackend::UNITS_PER_METER;
	for (int i = 0; i < 40; i++) {
		words[w++] = p_source.level[i];
	}
	f[w++] = p_source.block_size_world[0];
	f[w++] = p_source.block_size_world[1];
	f[w++] = p_source.block_size_world[2];
	w++;
	iw[w++] = p_source.layer_and_level;
	iw[w++] = p_size.x;
	iw[w++] = p_size.y;
	iw[w++] = 256;
	iw[w++] = rect.position.x;
	iw[w++] = rect.position.y;
	iw[w++] = rect.size.x;
	iw[w++] = rect.size.y;
	for (int i = 0; i < 3; i++) {
		f[w++] = p_source.world_min[i];
	}
	w++;
	for (int i = 0; i < 3; i++) {
		f[w++] = p_source.world_max[i];
	}
	w++;
	f[w++] = p_params.kelvin / MAX(p_params.temperature_range, 1e-4f);
	f[w++] = MAX(p_params.air_kelvin, 150.0f);
	f[w++] = MAX(p_params.turbulence, 0.0f);
	f[w++] = p_params.time;
	ERR_FAIL_COND(w != PARAMS_WORDS);
	p_rd->buffer_update(params_buffer, 0, sizeof(words), words);

	LocalVector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ linear_sampler, RID::from_uint64(p_source.density_rid) })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, RID::from_uint64(p_source.table_rid)));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ point_sampler, p_depth })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, Vector<RID>({ linear_sampler, source_copy })));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 4, p_color));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 5, params_buffer));
	const RID set = p_rd->uniform_set_create(uniforms, shader, 0);
	ERR_FAIL_COND(!set.is_valid());
	const RD::ComputeListID list = p_rd->compute_list_begin();
	p_rd->compute_list_bind_compute_pipeline(list, pipeline);
	p_rd->compute_list_bind_uniform_set(list, set, 0);
	p_rd->compute_list_dispatch(list, (rect.size.x + 7) / 8, (rect.size.y + 7) / 8, 1);
	p_rd->compute_list_end();
	p_rd->free_rid(set);
}

void FlowShimmerRD::free(RenderingDevice *p_rd) {
	if (p_rd == nullptr) {
		return;
	}
	for (RID *r : { &pipeline, &shader, &params_buffer, &linear_sampler, &point_sampler, &source_copy }) {
		if (r->is_valid()) {
			p_rd->free_rid(*r);
			*r = RID();
		}
	}
}

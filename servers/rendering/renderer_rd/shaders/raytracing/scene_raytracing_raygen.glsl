#[raygen]

#version 460

#extension GL_EXT_control_flow_attributes : enable

#VERSION_DEFINES

#ifdef RT_TIMING_ENABLED
#extension GL_EXT_shader_realtime_clock : require
layout(set = 0, binding = 34, r32f) uniform writeonly image2D timing_image;

void store_path_timing(ivec2 pixel, uvec2 start_clock) {
	uvec2 end_clock = clockRealtime2x32EXT();
	// Subtract before converting to float, including the low-word borrow.
	// Device clocks remain coherent across trace calls and shader execution reordering.
	uvec2 elapsed = uvec2(end_clock.x - start_clock.x,
			end_clock.y - start_clock.y - uint(end_clock.x < start_clock.x));
	float ticks = float(elapsed.y) * 4294967296.0 + float(elapsed.x);
	imageStore(timing_image, pixel, vec4(ticks));
}
#endif

// clang-format off
#include "raytracing_inc.glsl"
#include "../scene_data_inc.glsl"
// clang-format on

#pragma shader_stage(raygen)
#extension GL_EXT_ray_tracing : enable
#ifdef USE_SER
#extension GL_EXT_shader_invocation_reorder : enable
#endif

#define GLSL 1
#define RT_STAGE_RAYGEN 1
#include "raytracing_common_inc.glsl"
#include "raytracing_samplers_inc.glsl"

layout(set = 0, binding = 0, rgba16f) uniform image2D image; // RB_TEX_RAYTRACING is R16G16B16A16_SFLOAT.
layout(set = 0, binding = 1) uniform accelerationStructureEXT tlas;
layout(set = 0, binding = 31) uniform accelerationStructureEXT tlas_transparent;
layout(set = 0, binding = 29) uniform texture2D prepass_depth_texture;
layout(set = 0, binding = 30) uniform texture2D prepass_color_texture;

layout(location = 0) rayPayloadEXT PathPayload payload;

// Materials, for the medium a path travels in (water scattering).
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_ARB_gpu_shader_int64 : require
#include "raytracing_data_inc.glsl"
// clang-format off
layout(set = 0, binding = 3, std430) readonly buffer GeometryBuffer {
	GeometryData geometries[];
};
layout(set = 0, binding = 5, std430) readonly buffer MaterialBuffer {
	MaterialData materials[];
};
// clang-format on
#include "raytracing_entry_material_inc.glsl"

// The light API for the transparent pass's light cache (plight_update).
#extension GL_EXT_nonuniform_qualifier : require
layout(set = 1, binding = 0) uniform texture2D bindless_textures[];
#include "raytracing_hair_inc.glsl"
#include "brdf_inc.glsl"
#define RT_LIGHTS_TRACE_SHADOWS_ONLY
#include "raytracing_lights_inc.glsl"

struct ContinuationReservoir {
	PathState selected;
	float selected_weight;
	float total_weight;
	bool has_selection;
};

ContinuationReservoir continuation_reservoir_initial() {
	ContinuationReservoir r;
	r.selected_weight = 0.0;
	r.total_weight = 0.0;
	r.has_selection = false;
	return r;
}

void continuation_reservoir_add(inout ContinuationReservoir r, PathState candidate, inout uint rng_state) {
	if (is_path_terminated(candidate.packed_bounces_flags)) {
		return;
	}
	float weight = max(candidate.throughput.x, max(candidate.throughput.y, candidate.throughput.z));
	if (weight <= 1e-6) {
		return;
	}
	r.total_weight += weight;
	if (!r.has_selection || rand(rng_state) < weight / r.total_weight) {
		r.selected = candidate;
		r.selected_weight = weight;
		r.has_selection = true;
	}
}

// Russian roulette thresholds on the strongest throughput channel, per bounce; the last entry covers
// deeper bounces. The first bounce carries most of the visible indirect light, so it is cut least.
const float RT_RUSSIAN_ROULETTE_THRESHOLDS[2] = float[](0.1, 0.25);

// p_next_bounce is the segment about to be traced, 1 for the first bounce. Survivors are scaled back
// up to the threshold, never above it, so they cannot become fireflies.
bool russian_roulette_survives(uint p_next_bounce, inout PathState ps) {
	uint threshold_idx = min(p_next_bounce, uint(RT_RUSSIAN_ROULETTE_THRESHOLDS.length())) - 1u;
	float survival = max(ps.throughput.x, max(ps.throughput.y, ps.throughput.z)) / RT_RUSSIAN_ROULETTE_THRESHOLDS[threshold_idx];
	if (survival >= 1.0) {
		return true;
	}
	if (rand(ps.rng_state) >= survival) {
		return false;
	}
	ps.throughput /= survival;
	return true;
}

#ifdef DLSS_RR_ENABLED
// Maximum number of mirrors followed, including the primary one.
#define DLSS_RR_REPLACEMENT_MAX_MIRRORS 2u

// Fallback primary surface replacement, used when the path's own next hit couldn't provide it.
// Follows the mirror chain and writes the motion and albedo guides of the surface seen through it.
// The primary mirror isn't retraced; its normal is read back from the guides.
void dlss_rr_replace_primary_surface(vec3 p_origin, vec3 p_dir, float p_hit_t) {
	vec3 own_normal = imageLoad(dlss_rr_normal_roughness, ivec2(RT_LAUNCH_ID.xy)).xyz;
	mat3 image_xform = dlss_rr_householder(own_normal); // World to mirror-image space, linear part.
	float scene_length = p_hit_t;
	vec3 mirror_throughput = vec3(1.0); // Specular albedos of the mirrors after the primary one.
	vec3 ray_dir = reflect(p_dir, own_normal);
	vec3 ray_origin = offset_ray_origin(p_origin + p_dir * p_hit_t, ray_dir, p_hit_t);

	PathState gs;
	gs.bsdf_pdf = 0.0;
	gs.radiance = vec3(0.0);
	gs.throughput = vec3(0.0);
	gs.rng_state = 0u;
	gs.offset_normal = vec3(0.0, 0.0, 1.0);
	gs.next_ray_dir = vec3(0.0, 0.0, 1.0);
	for (uint mirrors = 1u; mirrors <= DLSS_RR_REPLACEMENT_MAX_MIRRORS; mirrors++) {
		// One bounce and no SAMPLE_ZERO keep the primary depth/velocity/guide writes away.
		gs.packed_bounces_flags = inc_total_bounce(0u) | GUIDE_RAY_FLAG |
				(mirrors == DLSS_RR_REPLACEMENT_MAX_MIRRORS ? GUIDE_LAST_FLAG : 0u);
		gs.hit_t = -1.0;
		path_pack(payload, gs);
		traceRayEXT(tlas, RT_RAY_FLAGS, RT_VIS_MASK, 0, 0, 0, ray_origin, 0.001, ray_dir, 10000.0, 0);
		if (payload.hit_t < 0.0 || (payload.packed_bounces_flags & GUIDE_RESULT_FLAG) == 0u) {
			return;
		}
		PathState result = path_unpack(payload);
		scene_length += result.hit_t;
		vec3 hit_diffuse = unpackUnorm4x8(payload.packed_rt[2]).rgb;
		vec3 hit_specular = unpackUnorm4x8(payload.rng_state).rgb;
		if ((result.packed_bounces_flags & GUIDE_MIRROR_FLAG) != 0u) {
			vec3 mirror_axis = ray_dir - result.next_ray_dir;
			if (dot(mirror_axis, mirror_axis) < 1e-10) {
				return;
			}
			mirror_throughput *= hit_specular;
			image_xform = image_xform * dlss_rr_householder(normalize(mirror_axis));
			ray_origin = offset_ray_origin(ray_origin + ray_dir * result.hit_t, result.offset_normal, result.hit_t);
			ray_dir = result.next_ray_dir;
			continue;
		}

		vec3 motion = vec3(unpackHalf2x16(payload.packed_rt[0]), unpackHalf2x16(payload.packed_rt[1]).x);
		dlss_rr_write_replaced_guides(p_origin + p_dir * scene_length, image_xform * motion, mirror_throughput, hit_diffuse, hit_specular);
		return;
	}
}
#endif // DLSS_RR_ENABLED

// The traced light cache of the transparent pass (#72). Transparents and particles are
// rasterised after the trace (and after Ray Reconstruction); under path tracing they read
// their light from this camera-aligned froxel grid instead of the raster light loop and
// ambient. A cell holds what a point in the air there receives: the lights of the light
// grid through a shadow ray, the sky and one bounce off the traced scene. As the froxel
// radiance cache RTX Remix lights its particles with (Wronski 2014, Hillaire 2015 for the
// grid; Talbot et al. 2005 for the resampled light choice).
//
// The grid is the internal size / 8 across (a coarser tile past 240 cells) and PLIGHT_SLICES slices deep, log-spaced from
// the near to the far distance; every pixel's thread refreshes one cell: the 8x8 block's
// pixels the block's slices. Two images a frame, read back by the next one (ping-pong):
// [0..1] frame A, [2..3] frame B; each pair rgb = the irradiance sum (normal incidence,
// the light buffer's units), and xyz = its luminance-weighted direction sum.
#define PLIGHT_SLICES 64u
#define PLIGHT_CANDIDATES 8u
layout(set = 0, binding = 43, rgba16f) uniform image3D plight_images[4];

float plight_luma(vec3 c) {
	return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

// A positional light's irradiance at a point facing it, unshadowed (as the raster light
// loop: the inverse-power attenuation, the range window and the spot cone).
vec3 plight_positional(RTLightData p_light, vec3 p_x, out vec3 r_to_light, out float r_dist) {
	vec3 to_l = p_light.position - p_x;
	float d2 = dot(to_l, to_l);
	r_dist = sqrt(d2);
	r_to_light = to_l / max(r_dist, 1e-4);
	if (p_light.max_range_squared > 0.0 && d2 > p_light.max_range_squared) {
		return vec3(0.0);
	}
	float atten = pow(max(r_dist, 0.0001), -p_light.attenuation);
	if (p_light.inv_max_range >= 0.0) {
		float nd = r_dist * p_light.inv_max_range;
		nd *= nd;
		nd *= nd;
		nd = max(1.0 - nd, 0.0);
		atten *= nd * nd;
	}
	if (p_light.type == RT_LIGHT_TYPE_SPOT) {
		float scos = dot(-r_to_light, lights_rotate(p_light.orientation, vec3(0.0, 0.0, -1.0)));
		float cos_spot = 1.0 - p_light.one_minus_cos_spot;
		if (scos <= cos_spot) {
			return vec3(0.0);
		}
		float spot_rim = max(1e-4, (1.0 - scos) / max(p_light.one_minus_cos_spot, 1e-4));
		atten *= 1.0 - pow(spot_rim, p_light.inv_spot_attenuation);
	}
	return p_light.emission * atten;
}

// The view-space point at screen uv and view distance p_z (along -Z).
vec3 plight_view_point(vec2 p_uv, float p_z, bool p_ortho) {
	vec2 d = p_uv * 2.0 - 1.0;
	vec4 t = scene_data_block.data.inv_projection_matrix * vec4(d.x, d.y, 1.0, 1.0);
	vec3 v = t.xyz / t.w;
	if (p_ortho) {
		return vec3(v.xy, -p_z);
	}
	return v * (p_z / max(-v.z, 1e-6));
}

void plight_update(uvec2 p_pixel, mat4 p_inv_view, uint p_frame) {
	uint state = uint(get_rt_param(RT_PARAM_PLIGHT_STATE));
	uint write = state & 1u;
	bool history = (state & 2u) != 0u;
	uint tile = uint(get_rt_param(RT_PARAM_PLIGHT_TILE));
	uvec2 grid_xy = max(uvec2(RT_LAUNCH_SIZE.xy) / tile, uvec2(1u));
	uvec2 in_tile = p_pixel % tile;
	// the tile's first 8x8 pixels refresh its 64 slices; any others idle
	if (any(greaterThanEqual(in_tile, uvec2(8u)))) {
		return;
	}
	uvec3 cell = uvec3(p_pixel / tile, in_tile.x + 8u * in_tile.y);
	if (any(greaterThanEqual(cell.xy, grid_xy))) {
		return;
	}
	float z_near = get_rt_param(RT_PARAM_PLIGHT_NEAR);
	float z_far = get_rt_param(RT_PARAM_PLIGHT_FAR);
	float log_ratio = log(z_far / z_near);
	bool ortho = abs(scene_data_block.data.projection_matrix[2][3]) < 1e-6;

	uint rng = init_rng(p_pixel, p_frame, 1231u);
	vec3 j = vec3(rand2(rng), rand(rng));
	vec2 uv = (vec2(cell.xy) + j.xy) / vec2(grid_xy);
	float z = z_near * exp(log_ratio * (float(cell.z) + j.z) / float(PLIGHT_SLICES));
	vec3 x = (p_inv_view * vec4(plight_view_point(uv, z, ortho), 1.0)).xyz;

	vec3 e_sum = vec3(0.0);
	vec3 dir_sum = vec3(0.0);
	uint light_count = uint(get_rt_param(RT_PARAM_LIGHT_COUNT));

	// the directional lights lead the light buffer: one direction in each one's disc
	uint dir_count = lights_directional_count(light_count);
	[[dont_unroll]] for (uint li = 0u; li < dir_count; li++) {
		RTLightData light = lights_get(li);
		vec3 axis = -normalize(light.position);
		vec2 u = rand2(rng);
		float cos_max = cos(light.radius);
		float cos_t = 1.0 - u.x * (1.0 - cos_max);
		float sin_t = sqrt(max(0.0, 1.0 - cos_t * cos_t));
		float phi = 2.0 * PI * u.y;
		vec3 tangent = normalize(abs(axis.y) < 0.999 ? cross(axis, vec3(0.0, 1.0, 0.0)) : cross(axis, vec3(1.0, 0.0, 0.0)));
		vec3 bitangent = cross(axis, tangent);
		vec3 L = normalize(tangent * (sin_t * cos(phi)) + bitangent * (sin_t * sin(phi)) + axis * cos_t);
		vec3 e = light.emission * lights_shadow_transmittance(light, x, L, 10000.0, 0xFFu, rng);
		e_sum += e;
		dir_sum += plight_luma(e) * L;
	}

	// the positional lights that can reach x (the light grid's lists): PLIGHT_CANDIDATES
	// drawn uniformly, one kept in proportion to its unshadowed irradiance, and its shadow ray
	{
		uint g_start, g_count, c_start, c_count;
		lights_grid_lists(x, g_start, g_count, c_start, c_count);
		uint n = g_count + c_count;
		if (n > 0u) {
			uint m = min(n, PLIGHT_CANDIDATES);
			float w_sum = 0.0;
			float w_sel = 0.0;
			vec3 e_sel = vec3(0.0);
			vec3 l_sel = vec3(0.0, 1.0, 0.0);
			float d_sel = 0.0;
			uint li_sel = 0u;
			for (uint k = 0u; k < m; k++) {
				// all of them when they fit, else drawn at random
				uint pick = (n <= PLIGHT_CANDIDATES) ? k : min(uint(rand(rng) * float(n)), n - 1u);
				uint li = lights_grid_light(pick, g_start, g_count, c_start);
				RTLightData light = lights_get(li);
				if (light.type == RT_LIGHT_TYPE_DIRECTIONAL) {
					continue;
				}
				vec3 to_l;
				float dist;
				vec3 e = plight_positional(light, x, to_l, dist);
				// the candidate's weight: its target over its source pdf (m/n of the lights)
				float w = plight_luma(e) * (n <= PLIGHT_CANDIDATES ? 1.0 : float(n) / float(m));
				if (!(w > 0.0) || isinf(w)) {
					continue;
				}
				w_sum += w;
				if (rand(rng) * w_sum < w) {
					w_sel = plight_luma(e);
					e_sel = e;
					l_sel = to_l;
					d_sel = dist;
					li_sel = li;
				}
			}
			if (w_sel > 0.0) {
				vec3 e = e_sel * (w_sum / w_sel) * lights_shadow_transmittance(lights_get(li_sel), x, l_sel, max(d_sel - 0.01, 0.002), 0xFFu, rng);
				e_sum += e;
				dir_sum += plight_luma(e) * l_sel;
			}
		}
	}

	// the sky and one bounce off the traced scene: a uniform direction, the path's first
	// segment (the hit's emission and its own light sampling, or the sky on a miss)
	if (get_rt_param(RT_PARAM_PLIGHT_BOUNCE) > 0.5) {
		vec2 u = rand2(rng);
		float cz = 1.0 - 2.0 * u.x;
		float sz = sqrt(max(0.0, 1.0 - cz * cz));
		float phi = 2.0 * PI * u.y;
		vec3 dir = vec3(sz * cos(phi), sz * sin(phi), cz);
		PathState ps;
		ps.bsdf_pdf = 0.0;
		ps.radiance = vec3(0.0);
		ps.throughput = vec3(1.0);
		ps.packed_bounces_flags = 0u;
		ps.offset_normal = vec3(0.0, 0.0, 1.0);
		ps.next_ray_dir = dir;
		ps.rng_state = init_rng(p_pixel, p_frame, 1237u);
		ps.hit_t = -1.0;
		path_pack(payload, ps);
		traceRayEXT(tlas, RT_RAY_FLAGS, RT_BOUNCE_MASK, 0, 0, 0, x, 0.001, dir, 10000.0, 0);
		ps = path_unpack(payload);
		// a uniform sphere sample of radiance, as irradiance: L * 4 pi
		vec3 e = clamp(ps.radiance, vec3(0.0), vec3(1000.0)) * (4.0 * PI);
		e_sum += e;
		dir_sum += plight_luma(e) * dir;
	}

	// blend into last frame's cell where this point was (an exponential moving average)
	vec4 prev_a = vec4(0.0);
	vec4 prev_b = vec4(0.0);
	float alpha = 1.0;
	if (history) {
		vec3 xc = (p_inv_view * vec4(plight_view_point((vec2(cell.xy) + 0.5) / vec2(grid_xy), z_near * exp(log_ratio * (float(cell.z) + 0.5) / float(PLIGHT_SLICES)), ortho), 1.0)).xyz;
		vec4 prev_clip = prev_vp_unjittered * vec4(xc, 1.0);
		vec2 motion = project_uv(xc, prev_vp_unjittered) - project_uv(xc, curr_vp_unjittered);
		vec2 prev_uv = (vec2(cell.xy) + 0.5) / vec2(grid_xy) + motion;
		float prev_z = ortho ? z_near * exp(log_ratio * (float(cell.z) + 0.5) / float(PLIGHT_SLICES)) : prev_clip.w;
		if (prev_clip.w > 0.0 && all(greaterThanEqual(prev_uv, vec2(0.0))) && all(lessThan(prev_uv, vec2(1.0))) && prev_z > z_near) {
			float prev_s = log(prev_z / z_near) / log_ratio * float(PLIGHT_SLICES);
			if (prev_s < float(PLIGHT_SLICES)) {
				ivec3 pc = ivec3(ivec2(prev_uv * vec2(grid_xy)), int(prev_s));
				uint read = 1u - write;
				prev_a = imageLoad(plight_images[read * 2u], pc);
				prev_b = imageLoad(plight_images[read * 2u + 1u], pc);
				alpha = 0.1;
			}
		}
	}
	vec4 a = mix(prev_a, vec4(e_sum, 1.0), alpha);
	vec4 b = mix(prev_b, vec4(dir_sum, 1.0), alpha);
	imageStore(plight_images[write * 2u], ivec3(cell), a);
	imageStore(plight_images[write * 2u + 1u], ivec3(cell), b);
}

// Light reaching a scattering point in simulated gas and scattered along -p_dir (the ray arrived
// travelling along p_dir): every directional light, and one positional light chosen among the light
// grid's candidates in proportion to its unshadowed contribution (as plight_update), each through a
// shadow ray whose transmittance includes the gas (ratio tracking), weighted by the
// Henyey-Greenstein phase function of anisotropy g (the gas's, or a water medium's). With p_cloud_beam
// (the open air's haze), the sun's light is its direct beam through the clouds over x.
vec3 rt_volume_direct(vec3 x, vec3 p_dir, float g, bool p_cloud_beam, inout uint rng) {
	vec3 sum = vec3(0.0);
	uint light_count = uint(get_rt_param(RT_PARAM_LIGHT_COUNT));
	uint dir_count = lights_directional_count(light_count);
	[[dont_unroll]] for (uint li = 0u; li < dir_count; li++) {
		RTLightData light = lights_get(li);
		vec3 axis = -normalize(light.position);
		vec2 u = rand2(rng);
		float cos_max = cos(light.radius);
		float cos_t = 1.0 - u.x * (1.0 - cos_max);
		float sin_t = sqrt(max(0.0, 1.0 - cos_t * cos_t));
		float phi = 2.0 * PI * u.y;
		vec3 tangent = normalize(abs(axis.y) < 0.999 ? cross(axis, vec3(0.0, 1.0, 0.0)) : cross(axis, vec3(1.0, 0.0, 0.0)));
		vec3 bitangent = cross(axis, tangent);
		vec3 L = normalize(tangent * (sin_t * cos(phi)) + bitangent * (sin_t * sin(phi)) + axis * cos_t);
		vec3 beam = p_cloud_beam ? lights_directional_projector_beam(light, x) : vec3(1.0);
		if (max(beam.r, max(beam.g, beam.b)) <= 0.0) {
			continue;
		}
		sum += light.emission * beam * rt_volume_hg(dot(L, p_dir), g) * lights_shadow_transmittance(light, x, L, 10000.0, 0xFFu, rng);
	}
	uint g_start, g_count, c_start, c_count;
	lights_grid_lists(x, g_start, g_count, c_start, c_count);
	uint n = g_count + c_count;
	if (n > 0u) {
		uint m = min(n, PLIGHT_CANDIDATES);
		float w_sum = 0.0;
		float w_sel = 0.0;
		vec3 e_sel = vec3(0.0);
		vec3 l_sel = vec3(0.0, 1.0, 0.0);
		float d_sel = 0.0;
		uint li_sel = 0u;
		for (uint k = 0u; k < m; k++) {
			uint pick = (n <= PLIGHT_CANDIDATES) ? k : min(uint(rand(rng) * float(n)), n - 1u);
			uint li = lights_grid_light(pick, g_start, g_count, c_start);
			RTLightData light = lights_get(li);
			if (light.type == RT_LIGHT_TYPE_DIRECTIONAL) {
				continue;
			}
			vec3 to_l;
			float dist;
			vec3 e = plight_positional(light, x, to_l, dist) * rt_volume_hg(dot(to_l, p_dir), g) * light.volume_energy;
			float w = plight_luma(e) * (n <= PLIGHT_CANDIDATES ? 1.0 : float(n) / float(m));
			if (!(w > 0.0) || isinf(w)) {
				continue;
			}
			w_sum += w;
			if (rand(rng) * w_sum < w) {
				w_sel = plight_luma(e);
				e_sel = e;
				l_sel = to_l;
				d_sel = dist;
				li_sel = li;
			}
		}
		if (w_sel > 0.0) {
			sum += e_sel * (w_sum / w_sel) * lights_shadow_transmittance(lights_get(li_sel), x, l_sel, max(d_sel - 0.01, 0.002), 0xFFu, rng);
		}
	}
	return sum;
}

// The open air's aerosol haze (WeatherMap3D traced_haze): extinction sigma0 exp(-(y - base) / H). The optical
// depth along a ray from o is a (1 - exp(-k t)) / k, with a the extinction at o and k = d.y / H, which inverts in
// closed form, so distances are drawn exactly (no tracking).
void rt_haze_ray(vec3 p_o, vec3 p_d, out float r_a, out float r_k) {
	float h = get_rt_param(RT_PARAM_HAZE_HEIGHT);
	r_a = get_rt_param(RT_PARAM_HAZE_SIGMA) * exp(clamp((get_rt_param(RT_PARAM_HAZE_BASE) - p_o.y) / h, -80.0, 8.0));
	r_k = p_d.y / h;
}

float rt_haze_depth(float p_a, float p_k, float p_t) {
	return abs(p_k * p_t) < 1e-4 ? p_a * p_t : p_a * (1.0 - exp(-p_k * p_t)) / p_k;
}

// The distance at which the optical depth reaches p_tau; -1 when it never does (an upward ray's whole depth is less).
float rt_haze_distance(float p_a, float p_k, float p_tau, float p_t_max) {
	if (abs(p_k * p_t_max) < 1e-4) {
		return p_tau / p_a;
	}
	float x = p_tau * p_k / p_a;
	return x >= 1.0 ? -1.0 : -log(1.0 - x) / p_k;
}

// The medium of payload code p_code (MEDIUM_CHANGE_FLAG): 0 none, RT_MEDIUM_CODE_GEOMETRY plus the TLAS entry
// of an RT_MAT_FLAG_VOLUME surface, or an inline extinction code (rt_medium_absorption(); with
// RT_MEDIUM_CODE_SCATTER, the scattering the entering hit left in rt_medium_image).
// The scattering word a medium of code p_code reads from rt_medium_image, taken when the path enters it:
// a later hit (subsurface scattering) reuses the pixel's texel.
uint rt_medium_scatter_word(uint p_code) {
	return (p_code & (RT_MEDIUM_CODE_GEOMETRY | RT_MEDIUM_CODE_SCATTER)) == RT_MEDIUM_CODE_SCATTER ? imageLoad(rt_medium_image, ivec2(RT_LAUNCH_ID.xy)).r : 0u;
}

// Only the code and the scatter word stay live across the bounce's trace (2 words instead of 8), and the
// medium is decoded after it: a pure function of both, so the path is the same to the bit (live state
// across traceRay is kept on the stack; Indiana Jones RT, NVIDIA 2024).
void rt_medium_set(uint p_code, uint p_scatter_word, out bool r_in_medium, out vec3 r_sigma_t, out vec3 r_albedo, out float r_g) {
	r_in_medium = p_code != 0u;
	r_albedo = vec3(0.0);
	r_g = 0.0;
	if ((p_code & RT_MEDIUM_CODE_GEOMETRY) != 0u) {
		MaterialData mat = rt_entry_material(p_code & ~RT_MEDIUM_CODE_GEOMETRY);
		r_sigma_t = rt_medium_absorption(rt_material_medium(mat));
		vec4 scatter = rt_material_scatter(mat);
		r_albedo = scatter.rgb;
		r_g = scatter.w;
	} else {
		r_sigma_t = rt_medium_absorption(p_code);
		if ((p_code & RT_MEDIUM_CODE_SCATTER) != 0u) {
			vec4 scatter = rt_medium_scatter_unpack(p_scatter_word);
			r_albedo = scatter.rgb;
			r_g = scatter.w;
		}
	}
}

// One caustic photon segment: the payload's report of the first surface along the ray.
bool rt_caustics_trace(vec3 p_origin, vec3 p_dir, float p_width, uint p_rng, out PathState r_ps) {
	r_ps.bsdf_pdf = 0.0;
	r_ps.radiance = vec3(0.0);
	r_ps.throughput = vec3(1.0);
	// One bounce in, so the hit writes no primary depth, velocity or guides.
	r_ps.packed_bounces_flags = inc_total_bounce(0u) | CAUSTIC_PHOTON_FLAG;
	r_ps.rng_state = p_rng;
	r_ps.hit_t = -1.0;
	r_ps.offset_normal = vec3(0.0, 0.0, 1.0);
	r_ps.next_ray_dir = p_dir;
	path_pack(payload, r_ps);
	payload_set_cone_width(payload, p_width);
	traceRayEXT(tlas, RT_RAY_FLAGS, RT_BOUNCE_MASK, 0, 0, 0, p_origin, 0.001, p_dir, 10000.0, 0);
	r_ps = path_unpack(payload);
	return r_ps.hit_t >= 0.0;
}

// A path into or through a medium leaves each surface from offset_ray_origin()'s start, up to 256 ulps of the
// position off it: about 1.5 cm a kilometre from the origin. Water thinner than that (a pool's or a beach's
// shallow edge) fits inside the offset: a ray refracted in at the top started under the floor, and one off the
// floor started over the top, and both stayed "in the water" for good, every later segment absorbed (blue strips
// along the shores of rimstone pools far from the origin). Probe the gap from the start back to the point; a
// surface there (the floor under the water, the water over the floor) moves the start to halfway between the two.
// Only where the offset is wider than the trace's own 1 mm minimum distance, so scenes near the origin pay nothing.
vec3 rt_medium_gap_start(vec3 p_hit, vec3 p_start) {
	vec3 g = p_hit - p_start;
	float d = length(g);
	if (d < 0.002) {
		return p_start;
	}
	PathState ps;
	ps.bsdf_pdf = 0.0;
	ps.radiance = vec3(0.0);
	ps.throughput = vec3(1.0);
	ps.packed_bounces_flags = inc_total_bounce(0u) | CAUSTIC_PHOTON_FLAG;
	ps.rng_state = 0u;
	ps.hit_t = -1.0;
	ps.offset_normal = vec3(0.0, 0.0, 1.0);
	ps.next_ray_dir = g / d;
	path_pack(payload, ps);
	payload_set_cone_width(payload, 0.0);
	// From 2 mm past the start (a surface just beyond it is still skipped by the next trace's 1 mm minimum
	// distance), back faces too: from a start under the floor the floor is met from behind.
	const float past = 0.002;
	traceRayEXT(tlas, gl_RayFlagsNoneEXT, RT_BOUNCE_MASK, 0, 0, 0, p_start - g * (past / d), 0.0, g / d, d * 0.98 + past, 0);
	ps = path_unpack(payload);
	return ps.hit_t >= 0.0 ? p_hit - g * (0.5 * max(d + past - ps.hit_t, 0.0) / d) : p_start;
}

// ============================================================================
// SUBSURFACE SCATTERING (docs/ideas/subsurface-scattering.md)
// ============================================================================
// A random walk through the medium under a surface, as Cycles' Random Walk (Chiang, Kutz and Burley,
// SIGGRAPH 2016; src/kernel/integrator/subsurface_random_walk.h): free flights on one colour channel
// chosen by throughput times single-scatter albedo, combined over the three by the balance heuristic,
// isotropic scattering, until a probe ray leaves the mesh through a back face.

#define RT_SSS_MAX_STEPS 256u

// Traces a subsurface probe from p_origin along p_dir with no face culling: the nearest surface's
// distance (-1: none), whether the ray leaves through it (a back face), its normal and TLAS entry.
float rt_sss_probe(vec3 p_origin, vec3 p_dir, out bool r_back_face, out vec3 r_normal, out uint r_geometry) {
	payload.packed_rt[0] = 0u;
	payload.packed_rt[1] = 0u;
	payload.packed_rt[2] = 0u;
	payload.packed_bounces_flags = SSS_PROBE_FLAG;
	payload.rng_state = 0u;
	payload.hit_t = -1.0;
	payload.oct_offset_nrm = 0u;
	payload.oct_next_dir = 0u;
	payload.bsdf_pdf = 0.0;
	traceRayEXT(tlas, gl_RayFlagsNoneEXT, RT_BOUNCE_MASK, 0, 0, 0, p_origin, 0.0, p_dir, 10000.0, 0);
	r_back_face = (payload.packed_bounces_flags & SSS_PROBE_BACK_FACE_FLAG) != 0u;
	r_normal = oct_to_vec3(unpackUnorm2x16(payload.oct_offset_nrm) * 2.0 - 1.0);
	r_geometry = payload.oct_next_dir;
	return payload.hit_t;
}

#define RT_SSS_LOST 0u
#define RT_SSS_EXIT 1u
#define RT_SSS_THIN 2u

// Walks from p_origin (just inside the surface) along p_dir in a medium whose colour after many
// scattering events is p_color and whose extinction is 1 / radius (p_sigma_t). Returns RT_SSS_EXIT
// with the exit point, its outward normal, TLAS entry and the walk's weight; RT_SSS_THIN when the
// first probe does not leave the mesh through a back face (an open surface: plain diffuse at the
// entry); RT_SSS_LOST when the walk ends inside (absorbed, escaped or out of steps).
uint rt_sss_walk(vec3 p_origin, vec3 p_dir, vec3 p_color, vec3 p_sigma_t, inout uint rng,
		out vec3 r_weight, out vec3 r_pos, out vec3 r_normal, out uint r_geometry) {
	r_weight = vec3(0.0);
	r_pos = p_origin;
	r_normal = -p_dir;
	r_geometry = 0u;
	// Single-scatter albedo from the multi-scatter colour: van de Hulst's relation as in d'Eon's
	// "A Hitchhiker's Guide to Multiple Scattering", eq. 53.7 (Cycles), isotropic phase.
	vec3 c = clamp(p_color, vec3(0.0), vec3(0.999));
	vec3 s = 4.09712 + 4.20863 * c - sqrt(9.59217 + 41.6808 * c + 17.7126 * c * c);
	vec3 alpha = clamp(1.0 - s * s, vec3(0.0), vec3(0.999999));
	vec3 throughput = vec3(1.0);
	// Low albedos: a floor of 0.2 with the weight scaled to match (Cycles), against sharp edges.
	for (int i = 0; i < 3; i++) {
		if (alpha[i] < 0.2) {
			throughput[i] *= alpha[i] / 0.2;
			alpha[i] = 0.2;
		}
	}
	vec3 sigma_t = max(p_sigma_t, vec3(1e-6));
	vec3 sigma_s = sigma_t * alpha;
	vec3 x = p_origin;
	vec3 dir = p_dir;
	[[dont_unroll]] for (uint walk_step = 0u; walk_step < RT_SSS_MAX_STEPS; walk_step++) {
		vec3 channel_weight = throughput * alpha;
		float channel_sum = channel_weight.r + channel_weight.g + channel_weight.b;
		if (!(channel_sum > 0.0)) {
			return RT_SSS_LOST;
		}
		vec3 channel_pdf = channel_weight / channel_sum;
		float u = rand(rng);
		float sigma_c = u < channel_pdf.r ? sigma_t.r : (u < channel_pdf.r + channel_pdf.g ? sigma_t.g : sigma_t.b);
		float t = -log(max(1.0 - rand(rng), 1e-30)) / sigma_c;

		bool back_face;
		vec3 n;
		uint geometry;
		float hit_t = rt_sss_probe(x, dir, back_face, n, geometry);
		if (hit_t < 0.0 || !back_face) {
			// Not inside a closed mesh: an open surface on the first step, else lost.
			return walk_step == 0u ? RT_SSS_THIN : RT_SSS_LOST;
		}
		bool exits = hit_t <= t;
		float d = exits ? hit_t : t;
		vec3 tr = exp(-sigma_t * d);
		vec3 pdf = exits ? tr : sigma_t * tr;
		throughput *= (exits ? tr : sigma_s * tr) / max(dot(channel_pdf, pdf), 1e-30);
		if (exits) {
			r_pos = x + dir * hit_t;
			r_normal = dot(n, dir) < 0.0 ? -n : n;
			r_geometry = geometry;
			r_weight = throughput;
			return RT_SSS_EXIT;
		}
		x += dir * d;
		// Isotropic phase function.
		vec2 v = rand2(rng);
		float cos_t = 1.0 - 2.0 * v.x;
		float sin_t = sqrt(max(0.0, 1.0 - cos_t * cos_t));
		float phi = 2.0 * PI * v.y;
		dir = vec3(sin_t * cos(phi), sin_t * sin(phi), cos_t);
		// Russian roulette on faint walks.
		float m = max(throughput.r, max(throughput.g, throughput.b));
		if (m < 0.25) {
			if (rand(rng) * 0.25 >= m) {
				return RT_SSS_LOST;
			}
			throughput *= 0.25 / m;
		}
	}
	return RT_SSS_LOST;
}

// Light reaching the exit point of a walk: a white Lambertian surface on p_normal (Cycles' exit),
// sampled with the surfaces' light selection.
vec3 rt_sss_exit_direct(vec3 p_pos, vec3 p_normal, uint p_layers, uint p_shadow_mask, bool p_is_indirect, inout uint rng) {
	MaterialProperties lambert;
	lambert.baseColor = vec3(1.0);
	lambert.metalness = 0.0;
	lambert.emissive = vec3(0.0);
	lambert.roughness = 1.0;
	lambert.dielectricF0 = 0.0;
	lambert.transmissivness = 0.0;
	lambert.opacity = 1.0;
	lambert.energyCompensation = vec3(1.0);
	lambert.backlight = vec3(0.0);
	lambert.diffuseMode = GODOT_DIFFUSE_LAMBERT;
	lambert.specularMode = GODOT_SPECULAR_DISABLED;
	lambert.clearcoat = 0.0;
	lambert.clearcoatAlpha = 0.0;
	lambert.coatNormal = p_normal;
	lambert.subsurface = 0.0;
	lambert.anisotropy = 0.0;
	lambert.anisoTangent = vec3(1.0, 0.0, 0.0);
	uint light_count = uint(get_rt_param(RT_PARAM_LIGHT_COUNT));
	return lights_evaluate_direct_lighting(p_pos, p_normal, p_normal, lambert, rng, p_is_indirect, light_count, p_layers, p_shadow_mask);
}

// From a point just past a medium's surface on to the first other surface, through any further
// medium surfaces unbent.
bool rt_caustics_land(vec3 p_origin, vec3 p_dir, float p_width, uint p_rng, out vec3 r_p) {
	vec3 o = p_origin;
	r_p = o;
	for (uint i = 0u; i < 4u; i++) {
		PathState ps;
		if (!rt_caustics_trace(o, p_dir, p_width, p_rng, ps)) {
			return false;
		}
		vec3 hit = o + p_dir * ps.hit_t;
		if ((ps.packed_bounces_flags & MEDIUM_CHANGE_FLAG) == 0u) {
			r_p = hit;
			return true;
		}
		o = offset_ray_origin(hit, ps.offset_normal, ps.hit_t);
	}
	return false;
}

// The medium the camera ray of this pixel starts in (a camera under water), Cycles' way: probe straight
// up from the ray's start; the first medium surface met decides. Its back face: the start is inside
// that medium (its code, as the hit reports it; scattering left in rt_medium_image). A front face or
// no medium surface at all: air. Other surfaces (a pier, a hull) are passed through.
uint rt_camera_medium(vec3 p_origin) {
	const vec3 up = vec3(0.0, 1.0, 0.0);
	// From 1 cm below: a surface within the trace's 1 mm minimum distance of the point is still met
	// (a start just over the water then meets the top first, and reads as air).
	vec3 o = p_origin - up * 0.01;
	for (uint i = 0u; i < 4u; i++) {
		PathState ps;
		if (!rt_caustics_trace(o, up, 0.0, 0u, ps)) {
			return 0u;
		}
		if ((ps.packed_bounces_flags & MEDIUM_CHANGE_FLAG) != 0u) {
			return ps.bsdf_pdf < 0.0 ? (floatBitsToUint(ps.bsdf_pdf) & 0x7FFFFFFFu) : 0u;
		}
		// Into a solid thicker than a hull's plating (the rock over a cave): water beyond it is not the camera's.
		// The next surface up, faces either way, is that solid's far side when it is a back face; before, a stream
		// on the ground 60 m over a cave read as the water the cave's camera stood in, and the whole passage
		// filled with its scattering.
		vec3 hit_p = o + up * ps.hit_t;
		bool back;
		vec3 bn;
		uint bg;
		float t_out = rt_sss_probe(hit_p + up * 0.002, up, back, bn, bg);
		if (t_out > 1.0 && back) {
			return 0u;
		}
		// On past it: off the surface on its upper side.
		vec3 n = dot(ps.offset_normal, up) >= 0.0 ? ps.offset_normal : -ps.offset_normal;
		o = offset_ray_origin(hit_p, n, ps.hit_t);
	}
	return 0u;
}

// One photon from the sun into cascade p_cascade's square, and its straight twin.
// p_sub of p_count: this photon's strip of the pixel (the strips stratify a pixel's photons along x).
// p_block: the photon's cell is the p_block x p_block block of pixels the pixel lies in (2 for the outer
// cascades, which only one pixel of each 2 x 2 block feeds a frame). With the pixel's own cell there, a
// frame's photons covered every other column and row: the straight twins sampled that comb at one phase and
// the bent ones, shifted by refraction in proportion to the depth, at another, so the ratio swung with the
// depth into bands along the shore's contours under flat water.
void rt_caustics_photon_cascade(vec3 p_center, uint p_cascade, uint p_write, uvec2 p_pixel, uint p_block, uint p_sub, uint p_count, inout uint rng) {
	CausticsFrame f;
	if (!rt_caustics_frame(p_center, p_cascade, f)) {
		return;
	}
	vec2 j = rand2(rng);
	j.x = (float(p_sub) + j.x) / float(p_count);
	vec2 uv = (vec2((p_pixel / p_block) * p_block) + j * float(p_block)) / vec2(RT_LAUNCH_SIZE.xy);
	vec2 q = f.origin + uv * f.size;
	// Each photon from a point on the sun's disc (at least the real sun's, 0.27 degrees across the
	// radius, when the light is set sharper): the disc blurs the lines by its angle times the depth,
	// so centimetre capillaries a metre down leave a soft shimmer rather than grain, as in a real
	// stream, while the longer ripples still focus into lines. The ray passes the square's own point
	// at the reference depth, so the jitter does not shift the square.
	LightSample ls;
	ls.cone_axis = -f.L;
	ls.cos_theta_max = min(cos(max(lights_get(0u).radius, RT_CAUSTICS_MIN_SUN_RADIUS)), 0.999999);
	float cone_pdf;
	vec3 dir = lights_sample_cone(ls, rand2(rng), cone_pdf); // about -f.L, down from the sun
	vec3 start = q.x * f.r + q.y * f.u + f.ref_depth * f.L - dir * RT_CAUSTICS_SUN_HEIGHT;
	float width = f.size / float(f.res);
	PathState ps;
	if (!rt_caustics_trace(start, dir, width, rng, ps)) {
		return;
	}
	// A photon that meets dry ground first lands where it is, bent and straight alike, so the ratio is 1 there. Without
	// it a dry point beside the water read its texel's ratio from the water's photons alone: bent photons land short of
	// their straight twins (refraction steepens them), so along one bank the bent ones had no twins to match (a bright
	// line on the shore) and along the other the twins had no bent ones (a dark line).
	if ((ps.packed_bounces_flags & MEDIUM_CHANGE_FLAG) == 0u) {
		vec3 dry = start + dir * ps.hit_t;
		rt_caustics_deposit(f, p_write, dry, 0u);
		rt_caustics_deposit(f, p_write, dry, 1u);
		return;
	}
	// Only photons entering a medium through its front face count.
	if (ps.bsdf_pdf < 0.5) {
		return;
	}
	vec3 entry = start + dir * ps.hit_t;
	vec3 o = offset_ray_origin(entry, ps.offset_normal, ps.hit_t);
	vec3 p;
	if (rt_caustics_land(o, ps.next_ray_dir, width, rng, p)) {
		rt_caustics_deposit(f, p_write, p, 0u);
	}
	if (rt_caustics_land(o, dir, width, rng, p)) {
		rt_caustics_deposit(f, p_write, p, 1u);
	}
}

// This pixel's share of the caustics map (raytracing_caustics_inc.glsl): clear part of the slot
// the next frame writes, then trace one photon from the sun. The square is centred where the
// camera looked last frame, at least half its width ahead; the centre pixel measures that
// distance for the next frame.
void rt_caustics_photon(uvec2 p_pixel, uint p_frame, vec3 p_cam_pos, vec3 p_cam_forward) {
	uint write = rt_caustics_write_slot();
	uint clear = (write + 1u) % 3u;
	uint threads = RT_LAUNCH_SIZE.x * RT_LAUNCH_SIZE.y;
	uint tid = p_pixel.y * RT_LAUNCH_SIZE.x + p_pixel.x;
	uint cascades = rt_caustics_cascades();
	for (uint c = 0u; c < cascades; c++) {
		uint slot_uints = c == 0u ? RT_CAUSTICS_SLOT_UINTS : RT_CAUSTICS_SLOT_UINTS_OUTER;
		uint base = rt_caustics_slot_base(c, clear);
		uint share = (slot_uints + threads - 1u) / threads;
		for (uint k = 0u; k < share; k++) {
			uint i = tid * share + k;
			if (i < slot_uints) {
				rt_caustics.data[base + i] = 0u;
			}
		}
	}

	float half_size = 0.5 * max(get_rt_param(RT_PARAM_CAUSTICS_SIZE), 0.01);
	uint last = (write + 2u) % 3u;
	float look = rt_caustics_history() ? uintBitsToFloat(rt_caustics.data[RT_CAUSTICS_HEADER + last * 4u]) : 0.0;
	vec3 center = p_cam_pos + p_cam_forward * clamp(look, half_size, 5000.0);
	uint h = RT_CAUSTICS_HEADER + write * 4u;
	if (p_pixel == RT_LAUNCH_SIZE.xy / 2u) {
		PathState look_ps;
		rt_caustics_trace(p_cam_pos, p_cam_forward, 0.0, 0u, look_ps);
		rt_caustics.data[h] = floatBitsToUint(look_ps.hit_t);
	}
	if (all(equal(p_pixel, uvec2(0u)))) {
		rt_caustics.data[h + 1u] = floatBitsToUint(center.x);
		rt_caustics.data[h + 2u] = floatBitsToUint(center.y);
		rt_caustics.data[h + 3u] = floatBitsToUint(center.z);
	}

	uint rng = init_rng(p_pixel, p_frame, 7919u);
	// Every pixel's photons go to cascade 0; a quarter of the pixels (turning every frame) also send
	// to cascade 1, and another quarter to cascade 2. A bright line is a density estimate, so each
	// cascade gets at least RT_CAUSTICS_MIN_PHOTONS a texel a frame: at 1080p one photon a pixel is
	// enough, while a small frame (640x360: about one a texel, so the lines dissolved into grain under
	// rippled water) sends up to RT_CAUSTICS_MAX_PER_PIXEL each.
	uint n0 = clamp((RT_CAUSTICS_MIN_PHOTONS * RT_CAUSTICS_RES * RT_CAUSTICS_RES + threads - 1u) / threads, 1u, RT_CAUSTICS_MAX_PER_PIXEL);
	for (uint k = 0u; k < n0; k++) {
		rt_caustics_photon_cascade(center, 0u, write, p_pixel, 1u, k, n0, rng);
	}
	uint quarter = ((p_pixel.x & 1u) | ((p_pixel.y & 1u) << 1u)) ^ (p_frame & 3u);
	if (quarter >= 1u && quarter < cascades) {
		uint n1 = clamp((RT_CAUSTICS_MIN_PHOTONS * RT_CAUSTICS_RES_OUTER * RT_CAUSTICS_RES_OUTER * 4u + threads - 1u) / threads, 1u, RT_CAUSTICS_MAX_PER_PIXEL);
		for (uint k = 0u; k < n1; k++) {
			rt_caustics_photon_cascade(center, quarter, write, p_pixel, 2u, k, n1, rng);
		}
	}
}

// The camera cut-out round the player (material.cpp camera_cutout_weight(): the same lines). p_world and p_target in
// the trace's space (world minus rt_origin).
float rt_cutout_weight(vec3 p_world, vec3 p_target, vec3 p_axis, float p_radius, float p_floor) {
	vec3 q = p_world - p_target;
	float rel = dot(q, p_axis);
	float d = length(q - p_axis * rel);
	return (1.0 - smoothstep(0.7 * p_radius, p_radius, d)) * (1.0 - smoothstep(-1.5, -0.5, rel)) * smoothstep(p_floor, p_floor + 0.3, p_world.y);
}

// Where a camera ray leaves the cut. Along a camera ray the weight only falls (the ray leaves the axis, nears the
// target, sinks toward the floor), so the cut is one span from the camera: the ray starts where the weight drops
// under a random threshold, which over the samples is the raster's dithered edge.
float rt_cutout_tmin(vec3 p_origin, vec3 p_dir, inout uint r_rng) {
	float radius = get_rt_param(RT_PARAM_CUTOUT_RADIUS);
	if (radius <= 0.0) {
		return 0.001;
	}
	vec3 target = vec3(get_rt_param(RT_PARAM_CUTOUT_X), get_rt_param(RT_PARAM_CUTOUT_Y), get_rt_param(RT_PARAM_CUTOUT_Z)) - rt_origin.xyz;
	float floor_y = get_rt_param(RT_PARAM_CUTOUT_FLOOR) - rt_origin.y;
	bool ortho = scene_data_block.data.projection_matrix[3][3] == 1.0;
	vec3 axis = ortho ? p_dir : normalize(target - p_origin);
	float u = rand(r_rng);
	if (rt_cutout_weight(p_origin, target, axis, radius, floor_y) <= u) {
		return 0.001;
	}
	// Past the target (half a metre before it the weight is 0) the ray is out.
	float along = max(dot(p_dir, axis), 0.05);
	float lo = 0.0;
	float hi = (max(-dot(p_origin - target, axis), 0.0) + 1.0) / along;
	for (int i = 0; i < 20; i++) {
		float mid = 0.5 * (lo + hi);
		if (rt_cutout_weight(p_origin + p_dir * mid, target, axis, radius, floor_y) > u) {
			lo = mid;
		} else {
			hi = mid;
		}
	}
	return max(hi, 0.001);
}

void main() {
#ifdef RT_TIMING_ENABLED
	uvec2 start_clock = clockRealtime2x32EXT();
#endif
	uvec2 pixel = RT_LAUNCH_ID.xy;
	const vec2 pixel_center = vec2(pixel) + vec2(0.5);
	const vec2 in_uv = pixel_center / vec2(RT_LAUNCH_SIZE.xy);
	vec2 d = in_uv * 2.0 - 1.0;

	mat4 inv_view = transpose(mat4(scene_data_block.data.inv_view_matrix[0],
			scene_data_block.data.inv_view_matrix[1],
			scene_data_block.data.inv_view_matrix[2],
			vec4(0.0, 0.0, 0.0, 1.0)));

	// Orthogonal cameras shoot parallel rays from the near plane; perspective ones fan out from the eye.
	const bool orthogonal = scene_data_block.data.projection_matrix[3][3] != 0.0;
	vec4 target = scene_data_block.data.inv_projection_matrix * vec4(d.x, d.y, 1.0, 1.0);
	vec3 view_origin = orthogonal ? vec3(target.xy / target.w, 0.0) : vec3(0.0);
	vec3 view_direction = orthogonal ? vec3(0.0, 0.0, -1.0) : normalize(target.xyz);
	vec4 origin = inv_view * vec4(view_origin, 1.0);
	vec4 direction = inv_view * vec4(view_direction, 0.0);

	// Depth composite against a co-tenant viewport (reverse-Z clear is 0.0): primary rays
	// stop at its depth, and on a primary miss its color/velocity/guides win the pixel.
	// Out to the camera's far plane, as raster draws: a fixed 10 km left distant ground (a plain, the far side of
	// a lake) to the sky, and the media drawn over it (rain and snow veils) had no ground to stand on.
	float composite_t_max = max(10000.0, scene_data_block.data.z_far);
	float composite_prepass_depth = 0.0;
	// The co-tenant renders the whole shared target before us, so on a primary miss
	// its color is the correct background whatever its depth. Testing the depth here
	// would exclude its sky, which sits at the reverse-Z far value of 0.0.
	bool composite_enabled = (RT_FLAGS & RT_FLAG_DEPTH_COMPOSITE_ENABLED) != 0u;
	if (composite_enabled) {
		composite_prepass_depth = texelFetch(sampler2D(prepass_depth_texture, SAMPLER_NEAREST_CLAMP), ivec2(pixel), 0).r;
		if (composite_prepass_depth > 0.0) {
			vec4 prepass_pos = scene_data_block.data.inv_projection_matrix * vec4(d.x, d.y, composite_prepass_depth, 1.0);
			composite_t_max = length(prepass_pos.xyz / prepass_pos.w - view_origin);
		}
	}

	// Sample count from specialization constant (or uniform in dynamic mode), frame index from uniform
	const uint samples_per_pixel = RT_GET_SAMPLE_COUNT();
	uint frame_index = uint(get_rt_param(RT_PARAM_FRAME_INDEX));

	// Accumulate multiple samples per pixel
	vec3 total_radiance = vec3(0.0);
	bool composite_passthrough = false;

#ifdef RT_DEBUG_ENABLED
	uint debug_tlayers = 0u; // VIS 23: primary-segment transparency hits
#endif

	if (rt_caustics_enabled()) {
		rt_caustics_photon(pixel, frame_index, inv_view[3].xyz, normalize(-inv_view[2].xyz));
	}

	// A camera under water (or in any medium): each pixel starts on the near plane, as raster clips
	// there, in the medium the probe finds above that point, so the water line splits the lens where
	// raster splits it.
	vec3 camera_ray_origin = origin.xyz;
	float camera_t_far = composite_t_max;
	uint camera_medium = 0u;
	if (rt_material_feature(RT_MATERIAL_FEATURE_REFRACTION)) {
		if (!orthogonal) {
			vec3 near_view = target.xyz / target.w;
			float t_near = length(near_view);
			camera_ray_origin = origin.xyz + direction.xyz * t_near;
			camera_t_far = max(composite_t_max - t_near, 0.001);
		}
		// Only a material that bounds a medium can put the camera in one.
		camera_medium = rt_material_feature(RT_MATERIAL_FEATURE_MEDIA) ? rt_camera_medium(camera_ray_origin) : 0u;
	}

	const uint max_bounces = RT_GET_MAX_BOUNCES();
	if (rt_rc_enabled()) {
		rt_rc_resolve(pixel.y * RT_LAUNCH_SIZE.x + pixel.x, RT_LAUNCH_SIZE.x * RT_LAUNCH_SIZE.y);
	}

	// Indirect light reuse (raytracing_indirect_reuse_inc.glsl): off where the first bounce segment
	// can scatter, since a reconnection would skip what lies on it.
	const bool ir_allowed = rt_ir_enabled() && !rt_volume_enabled() && camera_medium == 0u && (RT_FLAGS & RT_FLAG_FOG_ENABLED) == 0u;
	// The samples' first bounces through the diffuse lobe are candidates for this frame's reservoir,
	// and every one of them is shaded with the reservoir's result once the pixel's samples are done.
	IRFresh ir_fresh = rt_ir_fresh_init();
	bool ir_any_surface = false;
	vec3 ir_px_x1 = vec3(0.0);
	uint ir_diffuse_samples = 0u;
	uint ir_rng = init_rng(pixel, frame_index, 104729u);

	// TODO: when we have a spp > 0 the first raycast is always identical,
	// we should move it out of the loop

	[[dont_unroll]] for (uint sample_idx = 0u; sample_idx < samples_per_pixel; sample_idx++) {
		// Every field must be defined: path_pack branches on the octahedral encodes,
		// and branching on an undefined value lets the driver drop the whole path.
		PathState ps;
		ps.bsdf_pdf = 0.0;
		ps.radiance = vec3(0.0);
		ps.throughput = vec3(1.0);
		ps.packed_bounces_flags = (sample_idx == 0u) ? set_sample_zero(0u) : 0u;
		ps.rng_state = init_rng(pixel, frame_index, sample_idx);
		ps.hit_t = -1.0;
		ps.offset_normal = vec3(0.0, 0.0, 1.0);
		ps.next_ray_dir = vec3(0.0, 0.0, 1.0);

		vec3 ray_origin = camera_ray_origin;
		vec3 ray_dir = direction.xyz;
		// Without Ray Reconstruction nothing antialiases the image (its jittered camera does that
		// over frames), so geometry finer than a pixel, such as hair strands, would alias to
		// sparkle: the samples after the first spread over the pixel (box filter, R2 sequence,
		// Roberts 2018, rotated per frame). The first stays at the centre for the guide buffers.
		if (sample_idx > 0u && (RT_FLAGS & RT_FLAG_DLSS_RR_ENABLED) == 0u) {
			vec2 jitter = fract(vec2(0.7548777, 0.5698403) * float(sample_idx) + vec2(0.7548777, 0.5698403) * float(frame_index % 4096u)) - 0.5;
			vec2 dj = (pixel_center + jitter) / vec2(RT_LAUNCH_SIZE.xy) * 2.0 - 1.0;
			vec4 target_j = scene_data_block.data.inv_projection_matrix * vec4(dj.x, dj.y, 1.0, 1.0);
			vec3 view_origin_j = orthogonal ? vec3(target_j.xy / target_j.w, 0.0) : vec3(0.0);
			vec3 view_direction_j = orthogonal ? vec3(0.0, 0.0, -1.0) : normalize(target_j.xyz);
			ray_dir = (inv_view * vec4(view_direction_j, 0.0)).xyz;
			ray_origin = (inv_view * vec4(view_origin_j, 1.0)).xyz;
			if (rt_material_feature(RT_MATERIAL_FEATURE_REFRACTION) && !orthogonal) {
				ray_origin += ray_dir * length(target_j.xyz / target_j.w);
			}
		}
		// The ray cone's width at the segment's origin (texture level of detail, rt_texture_cone):
		// 0 at a perspective eye, one pixel's footprint for an orthogonal camera, then grown by
		// the pixel's spread angle over each segment traced.
		float cone_width = rt_cone_eye_width();
		// The medium the ray travels in (MEDIUM_CHANGE_FLAG), starting in the camera's (rt_camera_medium()): its extinction
		// per metre, single-scatter albedo and phase anisotropy (rt_medium_set()).
		uint medium_code = camera_medium;
		uint medium_scatter_word = camera_medium != 0u ? rt_medium_scatter_word(camera_medium) : 0u;
#ifdef DLSS_RR_ENABLED
		// Primary surface replacement this sample still owes (only sample 0 starts one).
		bool replace_pending = false;
		float replace_hit_t = 0.0;
#endif

		// Radiance cache training (raytracing_radiance_cache_inc.glsl): the surface the first
		// bounce reached, with the light and throughput the path had before it.
		uint rc_count = 0u;
		vec3 rc_pos[1];
		vec3 rc_normal[1];
		vec3 rc_radiance[1];
		vec3 rc_throughput[1];

		// Indirect light reuse: the first hit (x1), and the first bounce's direction, the light and
		// throughput the path had after the first hit, and the point the bounce reached.
		const bool ir_on = ir_allowed;
		bool ir_surface = false;
		bool ir_active = false;
		bool ir_traced = false;
		bool ir_has_x2 = false;
		bool ir_rough = false;
		vec3 ir_x1 = vec3(0.0);
		vec3 ir_dir = vec3(0.0, 0.0, 1.0);
		vec3 ir_radiance = vec3(0.0);
		vec3 ir_throughput = vec3(1.0);
		vec3 ir_x2 = vec3(0.0);
		vec3 ir_n2 = vec3(0.0, 0.0, 1.0);

		// Transmission past the first bounce does not count toward max_bounces (see the transmission lobe in
		// sample_surface_continuation()); the extra segments it may take are capped here.
		const uint segment_limit = max_bounces + uint(get_rt_param(RT_PARAM_TRANSMISSION_BOUNCES));
		// Scattering in the simulated gas has its own budget (RT_VOLUME_MAX_SCATTERS), not max_bounces: a white
		// cloud (snow powder, steam) keeps most of its light through tens of scatters, and cutting the path after the
		// surface budget (3 by default) darkened its inside to grey. Russian roulette still ends the paths.
		uint volume_scatters = 0u;
		[[dont_unroll]] for (uint bounce = 0u; bounce <= segment_limit + volume_scatters; bounce++) {
			vec3 throughput_entry = ps.throughput;
			vec3 radiance_pre = ps.radiance;
			uint flags_entry = ps.packed_bounces_flags;
			if (bounce == 0u) {
				// Heat shimmer: the camera ray bends through the gas's hot air (raytracing_volume_inc.glsl).
				rt_volume_bend(ray_origin, ray_dir, camera_t_far, ps.rng_state);
			}
			float t_far = (bounce == 0u) ? camera_t_far : 10000.0;
			// The camera cut-out round the player (rt_cutout_tmin()): a camera ray passes the surfaces that open in
			// the cut short of where it ends, as raster discards them, and stops at the first that keeps out of it (a
			// character: BaseMaterial3D camera_cutout off). The closest hit reports such a surface unshaded
			// (payload hit_t below CUTOUT_REPORTED) and the ray goes on past it, a few layers at most.
			float t_min = 0.001;
			float t_cut = (bounce == 0u) ? rt_cutout_tmin(ray_origin, ray_dir, ps.rng_state) : 0.001;
			bool cut_probe = t_cut > 0.0011;
			// A miss must not retain the previous segment's hit distance; the probe passes how far the cut reaches.
			ps.hit_t = cut_probe ? -(t_cut + CUTOUT_PROBE_BASE) : -1.0;
			path_pack(payload, ps);
			payload_set_cone_width(payload, cone_width);
			// The path state goes across the trace in the payload only: a surface that opens in the cut answers with
			// hit_t alone (closest hit), so a further layer reuses the payload as it is, and no copy of the state stays
			// live across the trace (it was spilled to the ray tracing stack at every bounce).
			for (uint cut_layer = 0u;; cut_layer++) {
#ifdef USE_SER
				hitObjectEXT hitObject;
				hitObjectTraceRayEXT(hitObject, tlas, RT_RAY_FLAGS, bounce == 0u ? RT_VIS_MASK : RT_BOUNCE_MASK, 0, 0, 0, ray_origin, t_min, ray_dir, t_far, 0);
				// Coherence hint (NVIDIA SER whitepaper): reordering already groups threads by hit group, but most
				// surfaces share the default material's, whose shading then branches on the material. Two bits for
				// the costliest branches, light through the surface and hair fibres, sort those threads together.
				// Reordering moves work between threads, never changes it, so the image is the same.
				uint ser_hint = 0u;
				if (rt_material_feature(RT_MATERIAL_FEATURE_SER_HINTS) && hitObjectIsHitEXT(hitObject)) {
					uint hint_flags = rt_entry_material_flags(hitObjectGetInstanceCustomIndexEXT(hitObject) + hitObjectGetGeometryIndexEXT(hitObject));
					ser_hint = ((hint_flags & (RT_MAT_FLAG_TRANSMISSIVE | RT_MAT_FLAG_VOLUME)) != 0u ? 1u : 0u) | ((hint_flags & RT_MAT_FLAG_HAIR) != 0u ? 2u : 0u);
				}
				reorderThreadEXT(hitObject, ser_hint, 2u);
				hitObjectExecuteShaderEXT(hitObject, 0);
#else
				traceRayEXT(tlas, RT_RAY_FLAGS, bounce == 0u ? RT_VIS_MASK : RT_BOUNCE_MASK, 0, 0, 0, ray_origin, t_min, ray_dir, t_far, 0);
#endif
				if (!cut_probe || payload.hit_t > -CUTOUT_REPORTED) {
					break;
				}
				// Past the surface that opened; after a few, past the whole cut as before.
				t_min = (-payload.hit_t - CUTOUT_REPORTED) + 0.002;
				if (cut_layer >= 3u) {
					t_min = max(t_min, t_cut);
					cut_probe = false;
				}
				payload.hit_t = cut_probe ? -(t_cut + CUTOUT_PROBE_BASE) : -1.0;
			}

			ps = path_unpack(payload);
			bool in_medium = false;
			vec3 medium_sigma_t = vec3(0.0);
			vec3 medium_albedo = vec3(0.0);
			float medium_g = 0.0;
			if (medium_code != 0u) {
				rt_medium_set(medium_code, medium_scatter_word, in_medium, medium_sigma_t, medium_albedo, medium_g);
			}

			// Simulated gas on this segment (raytracing_volume_inc.glsl). Delta tracking up to the
			// surface the segment reached: a real collision before it replaces the surface (its
			// shading is dropped) with a scattering event in the gas.
			if (rt_volume_enabled()) {
				float seg_len = ps.hit_t > 0.0 ? min(ps.hit_t, t_far) : t_far;
				vec3 vol_emitted;
				float t_vol = rt_volume_free_flight(ray_origin, ray_dir, seg_len, ps.rng_state, vol_emitted);
				if (rt_volume.emission.z > 1.5 && bounce > 0u && (flags_entry & LAST_BOUNCE_DIFFUSE_FLAG) != 0u) {
					vol_emitted = vec3(0.0); // a proxy light (Flow's fire light) already lit this vertex by next event estimation
				}
				if (t_vol > 0.0) {
					vec3 x = ray_origin + ray_dir * t_vol;
					vec3 albedo = rt_volume.smoke_color.rgb * rt_volume.medium.y;
					ps.radiance = radiance_pre + throughput_entry * vol_emitted;
					ps.throughput = throughput_entry * albedo;
					ps.radiance += ps.throughput * rt_volume_direct(x, ray_dir, rt_volume.medium.z, false, ps.rng_state);
					// Free of the surface budget while within the volume's own: the total only leaves 0 (no longer the
					// primary ray), so the surfaces the path meets later still get their max_bounces.
					uint vol_flags = flags_entry & ~(PATH_TERMINATED_FLAG | PRIMARY_MISS_FLAG | DLSS_RR_REPLACE_PENDING_FLAG);
					bool vol_free = volume_scatters < RT_VOLUME_MAX_SCATTERS;
					if (vol_free) {
						volume_scatters++;
					}
					ps.packed_bounces_flags = ((vol_free && get_total_bounces(vol_flags) > 0u) ? vol_flags : inc_total_bounce(vol_flags)) | NON_SINGULAR_PATH_FLAG | LAST_BOUNCE_DIFFUSE_FLAG;
					ps.hit_t = t_vol;
					cone_width += rt_cone_spread_angle() * t_vol;
					if (bounce == 0u) {
						// The gas is the primary surface: Ray Reconstruction guides at the scattering point.
						imageStore(rt_depth_image, ivec2(pixel), vec4(rt_ndc_depth(x)));
						imageStore(rt_velocity_image, ivec2(pixel), vec4(rt_motion_vector(x, x), 0.0, 0.0));
#ifdef DLSS_RR_ENABLED
						replace_pending = false;
						dlss_rr_write_albedo_guides(albedo, vec3(0.0));
						imageStore(dlss_rr_normal_roughness, ivec2(pixel), vec4(-ray_dir, 1.0));
						dlss_rr_write_specular_motion_vector(rt_motion_vector(x, x));
#endif
					}
					if (!russian_roulette_survives(bounce + 1u, ps)) {
						break;
					}
					ray_origin = x;
					ray_dir = rt_volume_sample_hg(ray_dir, rt_volume.medium.z, rand2(ps.rng_state));
					continue;
				}
				ps.radiance += throughput_entry * vol_emitted;
			}

			// The open air's aerosol haze (WeatherMap3D traced_haze; docs/ideas/weather.md, godrays stage 3), in place of
			// its froxels: a free flight drawn exactly against its extinction, up to the surface, or to the sky's own
			// aerosol for a ray that reaches the sky. A collision replaces the surface with a scattering event lit by
			// the sun's beam through the clouds and through any shadow (pixel-sharp shafts, on every bounce), then the
			// path goes on along the phase function (the sky's and the ground's light on the haze, multiple scattering).
			// The sun's light scattered on the segment is estimated on every segment (next event at a distance drawn in
			// proportion to sigma T over the segment, weighted by its scattering probability 1 - T); the path's own
			// collision, drawn independently, only decides whether it scatters there and goes on, so that light is not
			// counted twice. Far less noise than lighting only the collisions, for the same expectation.
			// A bounce that leaves for the sky takes no haze here: the radiance map it reads holds the aerosol along the
			// whole ray from its start (atmo_scatter with aerosol_from 0, only the camera's own sky starts at the reach),
			// and with the ambient light off the sky lights nothing, as in raster. Before, its first reach was counted
			// twice under the weather sky, and lit the ground under any other sky (a sun-only scene 17% over at a 30
			// degree sun, more as the sun sank).
			bool haze_sky_ray = bounce > 0u && ps.hit_t <= 0.0;
			if (!in_medium && !haze_sky_ray && get_rt_param(RT_PARAM_HAZE_SIGMA) > 0.0) {
				float seg_len = ps.hit_t > 0.0 ? min(ps.hit_t, t_far) : min(t_far, get_rt_param(RT_PARAM_HAZE_REACH));
				if (seg_len > 0.0) {
					float h_a, h_k;
					rt_haze_ray(ray_origin, ray_dir, h_a, h_k);
					float p_scatter = 1.0 - exp(-rt_haze_depth(h_a, h_k, seg_len));
					vec3 haze_light = vec3(0.0);
					if (p_scatter > 1e-6) {
						float t_ne = rt_haze_distance(h_a, h_k, -log(max(1.0 - rand(ps.rng_state) * p_scatter, 1e-30)), seg_len);
						t_ne = t_ne < 0.0 ? seg_len : min(t_ne, seg_len);
						haze_light = throughput_entry * (get_rt_param(RT_PARAM_HAZE_ALBEDO) * p_scatter) *
								rt_volume_direct(ray_origin + ray_dir * t_ne, ray_dir, 0.6, true, ps.rng_state);
					}
					float t_haze = rt_haze_distance(h_a, h_k, -log(max(1.0 - rand(ps.rng_state), 1e-30)), seg_len);
					if (t_haze > 0.0 && t_haze < seg_len) {
						vec3 x = ray_origin + ray_dir * t_haze;
						ps.radiance = radiance_pre + haze_light;
						ps.throughput = throughput_entry * get_rt_param(RT_PARAM_HAZE_ALBEDO);
						ps.packed_bounces_flags = inc_total_bounce(flags_entry & ~(PATH_TERMINATED_FLAG | PRIMARY_MISS_FLAG | DLSS_RR_REPLACE_PENDING_FLAG | MEDIUM_CHANGE_FLAG)) |
								NON_SINGULAR_PATH_FLAG | LAST_BOUNCE_DIFFUSE_FLAG;
						ps.hit_t = t_haze;
						ps.bsdf_pdf = 0.0;
						cone_width += rt_cone_spread_angle() * t_haze;
						if (!russian_roulette_survives(bounce + 1u, ps)) {
							break;
						}
						ray_origin = x;
						ray_dir = rt_volume_sample_hg(ray_dir, 0.6, rand2(ps.rng_state));
						continue;
					}
					ps.radiance += haze_light;
				}
			}

			// A segment inside a medium (refraction into RT_MAT_FLAG_VOLUME surfaces). Without scattering,
			// Beer-Lambert absorption: the surface it reached, and all it sends back, is seen through it.
			// With scattering, a free flight is drawn on one colour channel picked at random and weighed
			// over all three (spectral MIS, Wilkie et al. 2014; Miller, Georgiev and Jarosz 2019): a
			// collision before the surface replaces it with a scattering event in the water.
			vec3 medium_transmittance = vec3(1.0);
			if (in_medium) {
				float seg_len = ps.hit_t > 0.0 ? min(ps.hit_t, t_far) : t_far;
				bool scatters = max(medium_albedo.r, max(medium_albedo.g, medium_albedo.b)) > 0.0;
				float t_coll = 1e30;
				if (scatters) {
					float u_channel = rand(ps.rng_state);
					float sigma_c = u_channel < 1.0 / 3.0 ? medium_sigma_t.r : (u_channel < 2.0 / 3.0 ? medium_sigma_t.g : medium_sigma_t.b);
					if (sigma_c > 0.0) {
						t_coll = -log(max(1.0 - rand(ps.rng_state), 1e-30)) / sigma_c;
					}
				}
				if (t_coll < seg_len) {
					vec3 tr = exp(-medium_sigma_t * t_coll);
					float pdf = dot(medium_sigma_t * tr, vec3(1.0 / 3.0));
					vec3 x = ray_origin + ray_dir * t_coll;
					ps.radiance = radiance_pre;
					ps.throughput = throughput_entry * medium_albedo * medium_sigma_t * tr / max(pdf, 1e-30);
					ps.radiance += ps.throughput * rt_volume_direct(x, ray_dir, medium_g, false, ps.rng_state);
					ps.packed_bounces_flags = inc_total_bounce(flags_entry & ~(PATH_TERMINATED_FLAG | PRIMARY_MISS_FLAG | DLSS_RR_REPLACE_PENDING_FLAG | MEDIUM_CHANGE_FLAG)) |
							NON_SINGULAR_PATH_FLAG | LAST_BOUNCE_DIFFUSE_FLAG;
					ps.hit_t = t_coll;
					ps.bsdf_pdf = 0.0;
					cone_width += rt_cone_spread_angle() * t_coll;
					if (!russian_roulette_survives(bounce + 1u, ps)) {
						break;
					}
					ray_origin = x;
					ray_dir = rt_volume_sample_hg(ray_dir, medium_g, rand2(ps.rng_state));
					continue;
				}
				// A miss in the water is absorbed over the whole far distance too (black in clear water).
				{
					medium_transmittance = exp(-medium_sigma_t * seg_len);
					// Probability of no collision over the segment, averaged over the channels it was drawn on.
					float p_pass = scatters ? dot(medium_transmittance, vec3(1.0 / 3.0)) : 1.0;
					medium_transmittance /= max(p_pass, 1e-30);
					ps.radiance = radiance_pre + (ps.radiance - radiance_pre) * medium_transmittance;
					ps.throughput *= medium_transmittance;
				}
			}
			// The next segment starts in the medium the hit's continuation crossed into.
			if ((ps.packed_bounces_flags & MEDIUM_CHANGE_FLAG) != 0u) {
				medium_code = floatBitsToUint(ps.bsdf_pdf);
				medium_scatter_word = rt_medium_scatter_word(medium_code);
				rt_medium_set(medium_code, medium_scatter_word, in_medium, medium_sigma_t, medium_albedo, medium_g);
				ps.bsdf_pdf = 0.0;
				ps.packed_bounces_flags &= ~MEDIUM_CHANGE_FLAG;
			}

			if (ps.hit_t > 0.0) {
				cone_width += rt_cone_spread_angle() * ps.hit_t;
			}
			if (ir_on && bounce == 0u) {
				uint f = ps.packed_bounces_flags;
				ps.packed_bounces_flags &= ~(RT_IR_SURFACE_FLAG | RT_IR_DIFFUSE_FLAG);
				ir_surface = (f & RT_IR_SURFACE_FLAG) != 0u && ps.hit_t > 0.0;
				if (ir_surface) {
					ir_x1 = ray_origin + ray_dir * ps.hit_t;
					ir_any_surface = true;
					ir_px_x1 = ir_x1;
				}
				ir_active = ir_surface && (f & RT_IR_DIFFUSE_FLAG) != 0u && !is_path_terminated(f);
				if (ir_active) {
					ir_radiance = ps.radiance;
					ir_throughput = ps.throughput;
					ir_dir = ps.next_ray_dir;
					// The first bounce's hit reports its normal and roughness under this flag.
					ps.packed_bounces_flags |= RT_IR_DIFFUSE_FLAG;
				}
			} else if (ir_active && bounce == 1u) {
				ir_traced = true;
				ir_has_x2 = ps.hit_t > 0.0;
				ir_rough = !ir_has_x2 || (ps.packed_bounces_flags & RT_IR_DIFFUSE_FLAG) != 0u;
				if (ir_has_x2) {
					ir_x2 = ray_origin + ray_dir * ps.hit_t;
					ir_n2 = ps.offset_normal;
					if (ir_rough && rt_ir_enhanced()) {
						ir_rough = rt_ir_reconnectable(origin.xyz, ir_x1, ir_x2, ir_n2);
					}
				}
				ps.packed_bounces_flags &= ~RT_IR_DIFFUSE_FLAG;
			}
			bool rc_recorded = false;
			bool peel_seen = false;
			if ((ps.packed_bounces_flags & RT_RC_ELIGIBLE_FLAG) != 0u) {
				ps.packed_bounces_flags &= ~RT_RC_ELIGIBLE_FLAG;
				if (rc_count < 1u && ps.hit_t > 0.0) {
					rc_pos[rc_count] = ray_origin + ray_dir * ps.hit_t;
					rc_normal[rc_count] = ps.offset_normal;
					rc_radiance[rc_count] = radiance_pre;
					// The light it sent back reached the path through the medium, so it is read before that.
					rc_throughput[rc_count] = throughput_entry * medium_transmittance;
					rc_count++;
					rc_recorded = true;
				}
			}
#ifdef DLSS_RR_ENABLED
			if (bounce == 0u && (ps.packed_bounces_flags & DLSS_RR_REPLACE_PENDING_FLAG) != 0u) {
				replace_pending = true;
				replace_hit_t = ps.hit_t;
			} else if (bounce == 1u && replace_pending) {
				// The hit clears the flag once it wrote the replacement; a singular miss reflects the sky.
				replace_pending = (ps.packed_bounces_flags & DLSS_RR_REPLACE_PENDING_FLAG) != 0u &&
						!(is_singular_path(flags_entry) && ps.hit_t < 0.0);
				ps.packed_bounces_flags &= ~DLSS_RR_REPLACE_PENDING_FLAG;
			}
#endif
			bool opaque_terminated = is_path_terminated(ps.packed_bounces_flags);

			// Transparency over this segment is bounded closest-hit peeling.
			// Non-scissored transparent instances are opaque to traversal, so
			// only the closest hit runs a shader on each layer.
			// Each segment's own budget gates it: with max_transparency_layers 0 the primary
			// segment's transparents are drawn by the raster pass over the denoised frame, while
			// reflections and refractions still peel them (the raster pass can't show them there).
			uint layer_param = bounce == 0u			? RT_PARAM_MAX_TRANSPARENCY_LAYERS
					: is_singular_path(flags_entry) ? RT_PARAM_MAX_TRANSPARENCY_LAYERS_REFLECTION
													: RT_PARAM_MAX_TRANSPARENCY_LAYERS_INDIRECT;
			bool transparency_enabled = get_rt_param(RT_PARAM_TRANSPARENT_COUNT) > 0.0 &&
					get_rt_param(layer_param) > 0.0 &&
					bounce <= uint(get_rt_param(RT_PARAM_TRANSPARENCY_MAX_BOUNCE));
#ifdef RT_DEBUG_ENABLED
			transparency_enabled = transparency_enabled && rt_debug_keeps_transparency(int(get_rt_param(RT_PARAM_VIS_MODE)));
#endif
			if (transparency_enabled) {
				// SCENE_DEPTH for peel hits: NDC depth of this segment's opaque
				// hit on the primary segment, far (0.0) on bounces.
				float scene_depth = 0.0;
				if (bounce == 0u) {
					if (!is_primary_miss(ps.packed_bounces_flags) && ps.hit_t > 0.0) {
						scene_depth = rt_ndc_depth(ray_origin + ray_dir * ps.hit_t);
					} else if (composite_prepass_depth > 0.0) {
						scene_depth = composite_prepass_depth;
					}
				}
				// Termination only means there is no next bounce. Unshaded surfaces
				// and hits at the bounce limit still occlude transparency behind them.
				float seg_end = ps.hit_t >= 0.0 ? min(ps.hit_t, t_far) : t_far;
				float indirect_range = get_rt_param(RT_PARAM_TRANSPARENCY_INDIRECT_RANGE);
				float indirect_fade = get_rt_param(RT_PARAM_TRANSPARENCY_INDIRECT_FADE);
				bool range_limited = bounce > 0u && !is_singular_path(flags_entry) && indirect_range > 0.0;
				if (range_limited) {
					seg_end = min(seg_end, indirect_range);
				}
				uint peel_hits = 0u;
				vec3 throughput_after_opaque = ps.throughput;
				if (ir_active && bounce <= 1u) {
					// Layers in front of x1 or x2: x1's light is not the opaque surface's alone, and x2
					// is seen through them, so the pixel keeps its own path this frame.
					peel_seen = true;
				}
				vec3 radiance_after_opaque = ps.radiance;
				PathState opaque_continuation = ps;
				ContinuationReservoir continuation = continuation_reservoir_initial();
				uint continuation_rng = ps.rng_state;

				float T_total = 1.0;
				uint layer_cap = min(uint(get_rt_param(layer_param)), 64u);
				float peel_tmin = 0.001;
				for (uint l = 0u; l < layer_cap && T_total > (1.0 / 256.0) && peel_tmin < seg_end; l++) {
					ps.hit_t = -1.0;
					ps.packed_bounces_flags = flags_entry | PEEL_RAY_FLAG;
					if (l == 0u) {
						ps.packed_bounces_flags |= PEEL_FIRST_LAYER_FLAG;
					}
					ps.rng_state = continuation_rng;
					ps.throughput = throughput_entry * T_total;
					vec3 radiance_before_layer = ps.radiance;
					path_pack_peel(payload, ps, scene_depth);
					traceRayEXT(tlas_transparent, RT_RAY_FLAGS, RT_TMASK_TRANSPARENT,
							0, 0, 0, ray_origin, peel_tmin, ray_dir, seg_end, 0);
					ps = path_unpack(payload);
					if (ps.hit_t < 0.0) {
						break;
					}
					float layer_alpha = payload_peel_alpha(payload);
					ps.packed_bounces_flags &= ~PEEL_SCENE_DEPTH_FLAG;
					// The hit returned its alpha in place of its random state: the next layer takes a fresh stream.
					continuation_rng = pcg_hash(continuation_rng ^ 0x9E3779B9u);
					float layer_weight = 1.0;
					if (range_limited && indirect_fade > 0.0) {
						layer_weight = 1.0 - smoothstep(indirect_range - indirect_fade, indirect_range, ps.hit_t);
						ps.radiance = radiance_before_layer + (ps.radiance - radiance_before_layer) * layer_weight;
					}
					if ((ps.packed_bounces_flags & PEEL_HAS_CONTINUATION_FLAG) != 0u) {
						PathState layer_continuation = ps;
						layer_continuation.packed_bounces_flags &=
								~(PEEL_RAY_FLAG | PEEL_FIRST_LAYER_FLAG | PEEL_HAS_CONTINUATION_FLAG | PEEL_DEPTH_BARRIER_HIT_FLAG);
						layer_continuation.throughput *= layer_weight;
						continuation_reservoir_add(continuation, layer_continuation, continuation_rng);
					}
					float a = layer_alpha * layer_weight;
					T_total *= (1.0 - a);
					peel_hits++;
					// Advance by one representable positive float to avoid
					// re-hitting this layer without an identity any-hit shader.
					peel_tmin = uintBitsToFloat(floatBitsToUint(ps.hit_t) + 1u);
					if ((ps.packed_bounces_flags & PEEL_DEPTH_BARRIER_HIT_FLAG) != 0u) {
						break;
					}
				}

				if (peel_hits > 0u && peel_seen) {
					if (bounce == 0u) {
						ir_active = false;
					} else {
						ir_rough = false;
					}
				}
				peel_seen = false;
				if (peel_hits > 0u && rc_recorded) {
					// Layers in front of the surface would count as its own bounce light.
					rc_count--;
				}
				if (peel_hits == 0u) {
					// Preserve primary-miss/composite state when the transparent
					// lane had no geometry. The peel payload starts from flags_entry.
					ps = opaque_continuation;
					opaque_terminated = is_path_terminated(ps.packed_bounces_flags);
				} else {
					vec3 peel_sum = ps.radiance - radiance_after_opaque;
					vec3 segment_radiance = radiance_after_opaque - radiance_pre;
					if (bounce == 0u && composite_enabled &&
							is_primary_miss(opaque_continuation.packed_bounces_flags)) {
						vec3 composite_color = texelFetch(
								sampler2D(prepass_color_texture, SAMPLER_NEAREST_CLAMP), ivec2(pixel), 0)
													   .rgb;
						segment_radiance = throughput_entry * composite_color;
					}
					vec3 resolved_radiance = radiance_pre + segment_radiance * T_total + peel_sum;

					opaque_continuation.throughput = throughput_after_opaque * T_total;
					opaque_continuation.rng_state = continuation_rng;
					continuation_reservoir_add(continuation, opaque_continuation, continuation_rng);
					if (continuation.has_selection) {
						ps = continuation.selected;
						ps.throughput *= continuation.total_weight / continuation.selected_weight;
						ps.radiance = resolved_radiance;
						ps.rng_state = continuation_rng;
						opaque_terminated = false;
					} else {
						ps.radiance = resolved_radiance;
						ps.throughput = vec3(0.0);
						ps.packed_bounces_flags = set_path_terminated(flags_entry);
						ps.rng_state = continuation_rng;
						opaque_terminated = true;
					}
#ifdef DLSS_RR_ENABLED
					// Glass in front already composited onto the mirror's guides; replacing would drop it.
					if (bounce == 0u) {
						replace_pending = false;
						ps.packed_bounces_flags &= ~DLSS_RR_REPLACE_PENDING_FLAG;
					}
#endif
				}
#ifdef RT_DEBUG_ENABLED
				if (bounce == 0u && sample_idx == 0u) {
					debug_tlayers = peel_hits;
				}
#endif
			}

			if (opaque_terminated || !russian_roulette_survives(bounce + 1u, ps)) {
				break;
			}
			// Reconstruct the next ray origin from the current ray + hit distance, apply bias
			vec3 hit_pos = ray_origin + ray_dir * ps.hit_t;
			if ((ps.packed_bounces_flags & SSS_ENTER_FLAG) != 0u) {
				// Subsurface scattering: walk the medium under the surface, then go on from where the
				// walk left it as from a white Lambertian surface (rt_sss_walk(), Cycles' Random Walk).
				ps.packed_bounces_flags &= ~SSS_ENTER_FLAG;
				vec3 sigma_t = rt_medium_absorption(floatBitsToUint(ps.bsdf_pdf));
				vec3 color = unpackUnorm4x8(imageLoad(rt_medium_image, ivec2(pixel)).r).rgb;
				ps.bsdf_pdf = 0.0;
				vec3 outward = -ps.offset_normal;
				vec3 weight;
				vec3 exit_pos;
				vec3 exit_normal;
				uint exit_geometry;
				uint walk = rt_sss_walk(offset_ray_origin(hit_pos, ps.offset_normal, ps.hit_t), ps.next_ray_dir, color, sigma_t,
						ps.rng_state, weight, exit_pos, exit_normal, exit_geometry);
				if (walk == RT_SSS_LOST) {
					break;
				}
				uint layers = 0xFFFFFFFFu;
				uint shadow_mask = ~uint(get_rt_param(RT_PARAM_SELF_SHADOW_MASK)) & 0xFFu;
				float exit_t = ps.hit_t;
				if (walk == RT_SSS_THIN) {
					exit_pos = hit_pos;
					exit_normal = outward;
					weight = color;
				} else {
					layers = geometries[exit_geometry].layers;
					if ((geometries[exit_geometry].flags & FLAG_SELF_SHADOWING) != 0u) {
						shadow_mask = 0xFFu;
					}
					exit_t = max(exit_t, distance(hit_pos, exit_pos));
				}
				ps.throughput *= weight;
				ray_origin = offset_ray_origin(exit_pos, exit_normal, exit_t);
				// The entry already counted a diffuse bounce, so lights are indirect after an earlier one.
				bool is_indirect = get_diffuse_bounces(ps.packed_bounces_flags) > 1u;
				ps.radiance += ps.throughput * rt_sss_exit_direct(ray_origin, exit_normal, layers, shadow_mask, is_indirect, ps.rng_state);
				float4 q = getRotationToZAxis(exit_normal);
				ray_dir = normalize(rotatePoint(invertRotation(q), sampleHemisphere(rand2(ps.rng_state))));
				continue;
			}
			ray_origin = offset_ray_origin(hit_pos, ps.offset_normal, ps.hit_t);
			ray_dir = ps.next_ray_dir;
			if (in_medium) {
				ray_origin = rt_medium_gap_start(hit_pos, ray_origin);
			}
		}
#ifdef DLSS_RR_ENABLED
		if (replace_pending) {
			dlss_rr_replace_primary_surface(camera_ray_origin, direction.xyz, replace_hit_t);
		}
#endif

		if (rc_count > 0u) {
			uint rc_rng = init_rng(pixel, frame_index, 7919u + sample_idx);
			for (uint i = 0u; i < rc_count; i++) {
				vec3 t = rc_throughput[i];
				vec3 past = ps.radiance - rc_radiance[i];
				vec3 l = vec3(t.r > 1e-6 ? past.r / t.r : 0.0, t.g > 1e-6 ? past.g / t.g : 0.0, t.b > 1e-6 ? past.b / t.b : 0.0);
				rt_rc_write(rc_pos[i], rc_normal[i], l, rc_rng);
			}
		}

		if (ir_on && ir_active) {
			// The diffuse lobe's light is added from the reservoir after the samples; this sample's
			// own first bounce becomes a candidate for it.
			vec3 past = ps.radiance - ir_radiance;
			vec3 t = ir_throughput;
			vec3 L = vec3(t.r > 1e-6 ? past.r / t.r : 0.0, t.g > 1e-6 ? past.g / t.g : 0.0, t.b > 1e-6 ? past.b / t.b : 0.0);
			L = clamp(L, vec3(0.0), vec3(60000.0));
			rt_ir_fresh_add(ir_fresh, ir_x1, ir_traced, ir_dir, ir_has_x2, ir_x2, ir_n2, ir_rough, L, ir_rng);
			ps.radiance = ir_radiance;
			ir_diffuse_samples++;
		}

		total_radiance += ps.radiance;
		composite_passthrough = composite_passthrough || (composite_enabled && is_primary_miss(ps.packed_bounces_flags));
	}

	if (ir_any_surface) {
		// Indirect light reuse: resample, store this frame's reservoir, shade the diffuse samples.
		vec3 reused = rt_ir_resolve(ir_diffuse_samples > 0u, ir_px_x1, -direction.xyz, max(distance(origin.xyz, ir_px_x1), 0.01), ir_fresh, ir_rng);
		total_radiance += float(ir_diffuse_samples) * reused;
	}

#ifdef RT_DEBUG_ENABLED
	if (int(get_rt_param(RT_PARAM_VIS_MODE)) == RT_VIS_TRANSPARENCY_LAYERS) {
		// Transparency cost heatmap: green 0 -> yellow 2 -> red 4+.
		float heat_x = clamp(float(debug_tlayers) * 0.25, 0.0, 1.0);
		vec3 heat = (heat_x < 0.5) ? mix(vec3(0.0, 1.0, 0.0), vec3(1.0, 1.0, 0.0), heat_x * 2.0)
								   : mix(vec3(1.0, 1.0, 0.0), vec3(1.0, 0.0, 0.0), heat_x * 2.0 - 1.0);
		imageStore(image, ivec2(pixel), vec4(heat, 1.0));
		return;
	}
#endif

	if (composite_passthrough) {
		// The co-tenant pixel is nearer than anything traced: keep its color and depth,
		// leave its velocity and DLSS-RR guides untouched.
		imageStore(image, ivec2(pixel), texelFetch(sampler2D(prepass_color_texture, SAMPLER_NEAREST_CLAMP), ivec2(pixel), 0));
		imageStore(rt_depth_image, ivec2(pixel), vec4(composite_prepass_depth));
#ifdef RT_TIMING_ENABLED
		store_path_timing(ivec2(pixel), start_clock);
#endif
		return;
	}

	vec3 final_radiance = total_radiance / float(samples_per_pixel);
	imageStore(image, ivec2(pixel), vec4(final_radiance, 1.0));

	if (get_rt_param(RT_PARAM_PLIGHT_ENABLED) > 0.5) {
		plight_update(pixel, inv_view, frame_index);
	}
#ifdef RT_TIMING_ENABLED
	store_path_timing(ivec2(pixel), start_clock);
#endif
}

#[miss]

#version 460

#VERSION_DEFINES

#pragma shader_stage(miss)
#extension GL_EXT_ray_tracing : enable
#extension GL_EXT_nonuniform_qualifier : require

#define GLSL 1
#define RT_STAGE_MISS 1
// clang-format off
#include "raytracing_inc.glsl"
#include "../scene_data_inc.glsl"
#include "raytracing_hair_inc.glsl"
#include "brdf_inc.glsl"
#include "raytracing_common_inc.glsl"
#include "raytracing_samplers_inc.glsl"
#include "raytracing_volumetric_fog_inc.glsl"
// clang-format on

layout(location = 0) rayPayloadInEXT PathPayload payload;

layout(set = 0, binding = 29) uniform texture2D prepass_depth_texture;
layout(set = 0, binding = 33) uniform texture2D sky_screen_texture;

layout(set = 1, binding = 0) uniform texture2D bindless_textures[];

// The share of the clear sky's diffuse light reaching p (relative to rt_origin) through the clouds, from WeatherMap3D's
// cloud sky map (a cloud ambient occlusion map: red its hemisphere visibility, blue the clouds' height), eased to 1
// from p_ground (the height it is for: the camera's, which the map follows) up to the clouds' (raster's
// cloud_sky_visibility()).
float rt_cloud_sky_visibility(uint p_map, vec3 p, float p_ground) {
	vec3 w = p + rt_origin.xyz;
	vec2 uv = (w.xz - vec2(get_rt_param(RT_PARAM_CLOUD_SKY_X), get_rt_param(RT_PARAM_CLOUD_SKY_Z))) / RT_CLOUD_SKY_EXTENT;
	if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
		return 1.0;
	}
	vec4 m = textureLod(sampler2D(bindless_textures[p_map], SAMPLER_LINEAR_CLAMP), uv, 0.0);
	return mix(1.0, m.r, clamp((m.b - w.y) / max(m.b - p_ground, 1.0), 0.0, 1.0));
}

#ifdef USE_RADIANCE_OCTMAP_ARRAY

layout(set = 0, binding = 7) uniform texture2DArray radiance_octmap;
layout(set = 0, binding = 8) uniform sampler radiance_sampler;

// Array-backed octahedral radiance: roughness selects the prefiltered array
// layer (with a blend between adjacent layers). Ray/hit stages have no
// screen-space derivatives, so the intra-layer mip is fixed at 0.0; roughness is
// represented entirely by the layer selection.
vec3 radiance_octmap_sample(vec2 p_oct_uv, float p_roughness) {
	float layer;
	float blend = modf(clamp(p_roughness, 0.0, 1.0) * MAX_ROUGHNESS_LOD, layer);
	vec3 a = textureLod(sampler2DArray(radiance_octmap, radiance_sampler), vec3(p_oct_uv, layer), 0.0).rgb;
	vec3 b = textureLod(sampler2DArray(radiance_octmap, radiance_sampler), vec3(p_oct_uv, layer + 1.0), 0.0).rgb;
	return mix(a, b, blend);
}

#else

layout(set = 0, binding = 7) uniform texture2D radiance_octmap;
layout(set = 0, binding = 8) uniform sampler radiance_sampler;

// Single-texture octahedral radiance: roughness maps to the mip LOD.
vec3 radiance_octmap_sample(vec2 p_oct_uv, float p_roughness) {
	return textureLod(sampler2D(radiance_octmap, radiance_sampler), p_oct_uv, clamp(p_roughness, 0.0, 1.0) * MAX_ROUGHNESS_LOD).rgb;
}

#endif // USE_RADIANCE_OCTMAP_ARRAY

void main() {
	if ((payload.packed_bounces_flags & SSS_PROBE_FLAG) != 0u) {
		payload.hit_t = -1.0;
		return;
	}
	// Transparency peel traces own their payload; the scene miss must not touch it.
	if ((payload.packed_bounces_flags & PEEL_RAY_FLAG) != 0u) {
		return;
	}

	PathState ps = path_unpack(payload);

#if !defined(USE_SER)
	// A shadow-ray miss returns its accumulated colored transmittance.
	if (is_shadow_ray(ps.packed_bounces_flags)) {
		ps.radiance = ps.throughput;
		path_pack(payload, ps);
		return;
	}
#endif

	// Miss always ends the path.
	ps.packed_bounces_flags = set_path_terminated(ps.packed_bounces_flags);

	bool composite_passthrough = false;
	if (get_total_bounces(ps.packed_bounces_flags) == 0u) {
		ps.packed_bounces_flags = set_primary_miss(ps.packed_bounces_flags);
		// Matches raygen: a primary miss always yields the pixel to the co-tenant,
		// including where the co-tenant left far depth (its own sky).
		composite_passthrough = (RT_FLAGS & RT_FLAG_DEPTH_COMPOSITE_ENABLED) != 0u;
	}

#ifdef RT_DEBUG_ENABLED
	{
		int VIS_MODE = int(get_rt_param(RT_PARAM_VIS_MODE));
		// Specular hit distance: the first-bounce closest_hit pre-seeds the
		// "no hit" color into radiance, so a missed reflection just keeps it.
		if (VIS_MODE == 13 && get_total_bounces(ps.packed_bounces_flags) > 0u) {
			path_pack(payload, ps);
			return;
		}
	}
#endif // RT_DEBUG_ENABLED

	// Primary ray miss: write depth, velocity, and DLSS RR defaults (sample 0 only).
	{
		uint total_bounces = get_total_bounces(ps.packed_bounces_flags);
		if (total_bounces == 0u && is_sample_zero(ps.packed_bounces_flags) && !composite_passthrough) {
			ivec2 pixel = ivec2(RT_LAUNCH_ID.xy);

			imageStore(rt_depth_image, pixel, vec4(0.0));

			// Sky velocity: reproject a far-plane point using unjittered VPs.
			{
				vec3 far_world = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * 10000.0;
				vec2 curr_uv = project_uv(far_world, curr_vp_unjittered);
				vec2 prev_uv = project_uv(far_world, prev_vp_unjittered);
				imageStore(rt_velocity_image, pixel, vec4(prev_uv - curr_uv, 0.0, 0.0));
#ifdef DLSS_RR_ENABLED
				dlss_rr_write_specular_motion_vector(prev_uv - curr_uv);
#endif
			}
		}
	}

	// A camera ray leaves along the pixel it was launched from, and blend-alpha
	// peels do not refract, so the launch ID still indexes the right texel even
	// behind transparency. The screen-space sky is the same shader the rasterizer
	// runs and comes out of sky.glsl already fogged and in render-buffer
	// luminance, so it takes neither the IBL normalization nor the fog mix below.
	vec3 sky_color;
	if (get_rt_param(RT_PARAM_SKY_SCREEN_ENABLED) > 0.5 && get_total_bounces(ps.packed_bounces_flags) == 0u) {
		sky_color = texelFetch(sampler2D(sky_screen_texture, radiance_sampler), ivec2(RT_LAUNCH_ID.xy), 0).rgb;
	} else {
		vec2 sky_uv = rt_sky_oct_uv(gl_WorldRayDirectionEXT);

		if (get_rt_param(RT_PARAM_BG_COLOR_ENABLED) > 0.5 && get_total_bounces(ps.packed_bounces_flags) == 0u) {
			// A color background, as the raster clear; bounces keep the radiance map.
			sky_color = vec3(get_rt_param(RT_PARAM_BG_COLOR_R), get_rt_param(RT_PARAM_BG_COLOR_G), get_rt_param(RT_PARAM_BG_COLOR_B));
		} else {
			sky_color = radiance_octmap_sample(sky_uv, 0.0);
			sky_color *= scene_data_block.data.IBL_exposure_normalization;
		}

		if ((RT_FLAGS & RT_FLAG_FOG_ENABLED) != 0u) {
			vec3 fog_color = scene_data_block.data.fog_light_color;

			if (scene_data_block.data.fog_aerial_perspective > 0.0) {
				vec3 sky_fog = radiance_octmap_sample(sky_uv, 1.0 / MAX_ROUGHNESS_LOD);
				sky_fog *= scene_data_block.data.IBL_exposure_normalization;
				fog_color = mix(fog_color, sky_fog, scene_data_block.data.fog_aerial_perspective);
			}

			sky_color = mix(sky_color, fog_color, scene_data_block.data.fog_sky_affect);
		}

		if (get_total_bounces(ps.packed_bounces_flags) == 0u && rt_volumetric_fog_enabled()) {
			vec3 far_world = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * 10000.0;
			sky_color = rt_volumetric_fog_apply_sky(sky_color, project_uv(far_world, curr_vp_unjittered));
		}

		// A bounce's sky through the clouds over its start: the radiance map is the sky the camera sees, clouds and
		// all, so the ray takes its start's visibility against the camera's (at most 1: a clear spot seen from under a
		// cloud keeps the map's light), as raster's cloud_sky_light(). Per direction (the cloud along the ray against
		// the camera's) the capped ratio biased the mean dark, and uncapped it lit single rays by the map's bright
		// cloud fifty times over.
		uint cloud_sky_map = uint(get_rt_param(RT_PARAM_CLOUD_SKY_MAP));
		if (get_total_bounces(ps.packed_bounces_flags) > 0u && cloud_sky_map != 0u) {
			vec3 cam = vec3(scene_data_block.data.inv_view_matrix[0].w, scene_data_block.data.inv_view_matrix[1].w, scene_data_block.data.inv_view_matrix[2].w);
			float ground = cam.y + rt_origin.y;
			sky_color *= min(rt_cloud_sky_visibility(cloud_sky_map, gl_WorldRayOriginEXT, ground) / max(rt_cloud_sky_visibility(cloud_sky_map, cam, ground), 0.02), 1.0);
		}

		// The environment's ambient light switched off (ambient_light_source DISABLED): raster's ambient term is
		// gone, so a diffuse bounce leaving the scene gathers nothing either, as in Unreal's path tracer without
		// a sky light (the sky only lights through it). The sky stays visible to the camera, in reflections and
		// through glass. A colour or sky ambient keeps the sky here: the trace's own, directional, sky light.
		if (get_total_bounces(ps.packed_bounces_flags) > 0u && rt_ray_is_diffuse(ps.packed_bounces_flags) &&
				(scene_data_block.data.flags & SCENE_DATA_FLAGS_USE_AMBIENT_LIGHT) == 0u) {
			sky_color = vec3(0.0);
		}
	}

#ifdef DLSS_RR_ENABLED
	{
		uint total_bounces = get_total_bounces(ps.packed_bounces_flags);
		if (total_bounces == 0u && is_sample_zero(ps.packed_bounces_flags) && !composite_passthrough) {
			ivec2 pixel = ivec2(RT_LAUNCH_ID.xy);
			imageStore(dlss_rr_diffuse_albedo, pixel, vec4(vec3(DLSSRR_SKY_DIFFUSE_ALBEDO), 1.0));
			imageStore(dlss_rr_specular_albedo, pixel, vec4(0.0));
			imageStore(dlss_rr_normal_roughness, pixel, vec4(0.0));
		}
	}
#endif

#ifdef RT_DEBUG_ENABLED
	{
		int VIS_MODE = int(get_rt_param(RT_PARAM_VIS_MODE));
		if (VIS_MODE == 20) {
			ps.radiance = vec3(1.0);
		} else if (VIS_MODE == RT_VIS_VOLUMETRIC_FOG || VIS_MODE == RT_VIS_VOLUMETRIC_FOG_TRANSMITTANCE) {
			vec3 far_world = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * 10000.0;
			ps.radiance = rt_volumetric_fog_debug(VIS_MODE, rt_volumetric_fog_sky_sample(project_uv(far_world, curr_vp_unjittered)));
		} else if (VIS_MODE == 0) {
			ps.radiance += ps.throughput * sky_color;
		} else {
			ps.radiance = sky_color;
		}
	}
#else
	ps.radiance += ps.throughput * sky_color;
#endif // RT_DEBUG_ENABLED

	path_pack(payload, ps);
}

#[closest_hit]

#version 460

#VERSION_DEFINES

#pragma shader_stage(closest_hit)
#extension GL_EXT_ray_tracing : enable
#extension GL_EXT_ray_query : enable
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_ARB_gpu_shader_int64 : require
#extension GL_EXT_nonuniform_qualifier : require
#ifdef USE_SER
#extension GL_EXT_shader_invocation_reorder : enable
#endif
#ifdef RT_LSS
#extension GL_NV_linear_swept_spheres : require
#endif

#define GLSL 1
#define RT_STAGE_CLOSEST_HIT 1
// clang-format off
#include "raytracing_inc.glsl"
#include "../scene_data_inc.glsl"
#include "raytracing_hair_inc.glsl"
#include "brdf_inc.glsl"
#include "raytracing_common_inc.glsl"
// clang-format on

#ifdef RT_LSS
// A swept sphere hit writes no barycentrics: rt_lss_resolve() sets these to its strip triangle's.
vec2 rt_hit_bary = vec2(0.0);
int rt_hit_primitive = 0;
bool rt_lss_hit = false;
vec3 rt_lss_offset = vec3(0.0); // From the tube's axis to the hit, world space.
#define attribs rt_hit_bary
#define RT_HIT_PRIMITIVE_ID rt_hit_primitive
#else
#define attribs hit_attribs.bary_or_uv
#endif
#define RT_HIT_ATTRIBS_DECLARED

#include "raytracing_hit_inc.glsl"

layout(set = 0, binding = 1) uniform accelerationStructureEXT tlas;
layout(location = 0) rayPayloadInEXT PathPayload payload;

layout(set = 1, binding = 0) uniform texture2D bindless_textures[];

#include "raytracing_samplers_inc.glsl"

// clang-format off
layout(set = 0, binding = 3, std430) readonly buffer GeometryBuffer {
	GeometryData geometries[];
};

layout(set = 0, binding = 4, std430) readonly buffer MotionIndexBuffer {
	int motion_indices[];
};

layout(set = 0, binding = 5, std430) readonly buffer MaterialBuffer {
	MaterialData materials[];
};
// clang-format on

#include "raytracing_entry_material_inc.glsl"
#include "raytracing_lights_inc.glsl"

// clang-format off
layout(set = 0, binding = 32, std430) readonly buffer MotionTransforms {
	InstanceMotionData motion_transforms[];
};
// clang-format on

#ifdef USE_RADIANCE_OCTMAP_ARRAY

layout(set = 0, binding = 7) uniform texture2DArray radiance_octmap;
layout(set = 0, binding = 8) uniform sampler radiance_sampler;

// Array-backed octahedral radiance: roughness selects the prefiltered array
// layer (with a blend between adjacent layers). Ray/hit stages have no
// screen-space derivatives, so the intra-layer mip is fixed at 0.0; roughness is
// represented entirely by the layer selection.
vec3 radiance_octmap_sample(vec2 p_oct_uv, float p_roughness) {
	float layer;
	float blend = modf(clamp(p_roughness, 0.0, 1.0) * MAX_ROUGHNESS_LOD, layer);
	vec3 a = textureLod(sampler2DArray(radiance_octmap, radiance_sampler), vec3(p_oct_uv, layer), 0.0).rgb;
	vec3 b = textureLod(sampler2DArray(radiance_octmap, radiance_sampler), vec3(p_oct_uv, layer + 1.0), 0.0).rgb;
	return mix(a, b, blend);
}

#else

layout(set = 0, binding = 7) uniform texture2D radiance_octmap;
layout(set = 0, binding = 8) uniform sampler radiance_sampler;

// Single-texture octahedral radiance: roughness maps to the mip LOD.
vec3 radiance_octmap_sample(vec2 p_oct_uv, float p_roughness) {
	return textureLod(sampler2D(radiance_octmap, radiance_sampler), p_oct_uv, clamp(p_roughness, 0.0, 1.0) * MAX_ROUGHNESS_LOD).rgb;
}

#endif // USE_RADIANCE_OCTMAP_ARRAY

layout(set = 0, binding = 39) uniform texture2D rt_dfg;

// Same lookup as raster prefiltered_dfg(); the roughness axis is inverted.
vec2 rt_prefiltered_dfg(float p_roughness, float p_NdotV) {
	return textureLod(sampler2D(rt_dfg, SAMPLER_LINEAR_CLAMP), vec2(p_NdotV, 1.0 - p_roughness), 0.0).xy;
}

// clang-format off
#include "raytracing_material_eval_inc.glsl"
#include "raytracing_closest_hit_common_inc.glsl"
#include "raytracing_decals_inc.glsl"
// clang-format on

// ============================================================================
// CUSTOM SHADER GLOBALS (injected by ShaderCompiler for HG1+)
// ============================================================================
#ifdef RT_CUSTOM_HIT_GROUP
#include "raytracing_custom_globals_inc.glsl"
#endif

// ============================================================================
// MAIN
// ============================================================================
// Exact sparkle (docs/ideas/snow.md, follow-up 5): the facet of the grain a ray hits. Every 2 mm grain is
// an ice facet, its normal GGX-distributed (alpha 0.4) about the snow's; a pixel's samples land on
// different grains, so the path tracer integrates the discrete facets of the footprint itself
// (the reference of Jakob et al. 2014 and Yan et al. 2014), at any distance and on any bounce.
vec3 snow_grain_facet(vec3 p_pos, vec3 p_n, float p_hoar) {
	// Over surface hoar (p_hoar, mm), crystal-sized cells, a share hf of them plates set every way (as snow_glint).
	float hf = smoothstep(0.5, 4.0, p_hoar);
	float grain = max(0.002, 0.0006 * p_hoar);
	ivec3 q = ivec3(floor(p_pos / grain));
	uint h = (uint(q.x) * 73856093u) ^ (uint(q.y) * 19349663u) ^ (uint(q.z) * 83492791u);
	h ^= h >> 16u;
	h *= 0x7feb352du;
	h ^= h >> 15u;
	h *= 0x846ca68bu;
	h ^= h >> 16u;
	float r1 = float(h & 0xffffu) / 65536.0;
	float r2 = float(h >> 16u) / 65536.0;
	uint h2 = h * 0x9e3779b9u;
	h2 ^= h2 >> 15u;
	float ct;
	if (float(h2 & 0xffffu) / 65536.0 < hf) {
		ct = mix(0.5, 1.0, r1);
	} else {
		float tan2 = 0.16 * r1 / max(1.0 - r1, 1e-4);
		ct = inversesqrt(1.0 + tan2);
	}
	float st = sqrt(max(1.0 - ct * ct, 0.0));
	vec3 t = normalize(cross(p_n, abs(p_n.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
	vec3 b = cross(p_n, t);
	float phi = 6.2831853 * r2;
	return normalize(p_n * ct + (t * cos(phi) + b * sin(phi)) * st);
}

// Snow sparkle (docs/ideas/snow.md, stage 5): the same function as BaseMaterial3D's raster shader, there
// with its comments. Returns a facet normal and its weight (0: none) for a footprint in metres.
// Desiccation cracks (docs/ideas/mud.md, stage 2b), the same lines in material.cpp and scene_raytracing_raygen.glsl.
// Drying clay cracks into plates whose size follows the drying layer's thickness (Groisman and Kaplan 1994). The
// cracks come in generations (Bohn, Pauchard and Couder 2005, "Hierarchical crack pattern as formed by successive
// domain divisions"): the first ones meet at about 120 degrees and part the layer into large domains; each domain
// then splits across its long axis, the new crack meeting the old ones at right angles (T junctions), and the
// halves split again, each generation narrower and shallower than the one before. Wetting and drying again moves
// the junctions towards 120 degrees and the plates towards hexagons (Goehring et al. 2010, "Evolving mud-crack
// patterns"). The plates' rims curl up as their top dries and shrinks first (Allen 1986).
// Here: the first generation is the Voronoi cells of a jittered hexagonal lattice (Quilez's two-pass border
// distance); the second a line through each cell's seed towards its nearest neighbour, so across the cell's long
// axis and square to the edge it shares with that neighbour; the third, in each half, a line square to the second
// that may end short of the far edge, tapering out. The pattern meanders (a smooth warp) and its width wanders.
vec2 ground_crack_hash(ivec2 p_c, uint p_salt) {
	uint h = (uint(p_c.x) * 73856093u) ^ (uint(p_c.y) * 19349663u) ^ (p_salt * 83492791u);
	h ^= h >> 16u;
	h *= 0x7feb352du;
	h ^= h >> 15u;
	h *= 0x846ca68bu;
	h ^= h >> 16u;
	return vec2(float(h & 0xffffu), float(h >> 16u)) / 65535.0;
}
vec2 ground_crack_seed(ivec2 p_c, float p_jitter) {
	// Lattice basis (1, 0), (0.5, 0.866): hexagonal plates at no jitter.
	return vec2(float(p_c.x) + 0.5 * float(p_c.y), 0.8660254 * float(p_c.y)) + p_jitter * (ground_crack_hash(p_c, 0u) - 0.5);
}
// A smooth value noise in 0..1 (two channels).
vec2 ground_crack_noise(vec2 p_p, uint p_salt) {
	ivec2 i = ivec2(floor(p_p));
	vec2 f = fract(p_p);
	f = f * f * (3.0 - 2.0 * f);
	vec2 a = ground_crack_hash(i, p_salt);
	vec2 b = ground_crack_hash(i + ivec2(1, 0), p_salt);
	vec2 c = ground_crack_hash(i + ivec2(0, 1), p_salt);
	vec2 d = ground_crack_hash(i + ivec2(1, 1), p_salt);
	return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
// The cracks near a point: per generation (x, y, z) the distance (m) to it, its width and depth (m), and the
// direction (xz) in which the distance grows; and the rims' curl (height m, reach m).
struct GroundCracks {
	vec3 dist;
	vec3 width;
	vec3 depth;
	vec2 dir1;
	vec2 dir2;
	vec2 dir3;
	float curl;
	float reach;
	float scale; // the first generation's domain size, m
};
// p_c: the crack state (opening 0..1, plate size m, regularity 0..1, crust 0..1 or negative while damp).
GroundCracks ground_cracks(vec2 p_xz, vec4 p_c) {
	GroundCracks g;
	float reg = clamp(p_c.z, 0.0, 1.0);
	float open = clamp(p_c.x, 0.0, 1.0);
	float thick = 0.25 * p_c.y;
	// A silt skin (mud round 15.1, its share sent as a negative regularity by the ground's crack state). Its plates
	// are some ten times the layer across, so thick here is 2.5 layers. It shrinks more (fresh slurry), cracks
	// through the layer only, and its plates curl up at their edges well into the plate (mud curls, Allen 1986).
	float skin = clamp(-p_c.z, 0.0, 1.0);
	// The first generation's domains: two to three plates across, one as the cycles make them hexagons.
	float s1 = p_c.y * mix(2.4, 1.1, reg);
	vec2 q = p_xz / s1;
	q += 0.22 * (ground_crack_noise(q * 0.8, 5u) - 0.5) + 0.025 * (ground_crack_noise(q * 6.0, 6u) - 0.5);
	float jit = mix(0.9, 0.25, reg);
	// The nearest seed, then the distance to the cell's border and the nearest neighbour's direction.
	vec2 l = vec2(q.x - q.y * 0.57735027, q.y * 1.1547005);
	ivec2 c = ivec2(floor(l));
	vec2 best = vec2(0.0);
	ivec2 id = c;
	float bd = 1e9;
	for (int j = -1; j <= 2; j++) {
		for (int i = -1; i <= 2; i++) {
			ivec2 k = c + ivec2(i, j);
			vec2 r = ground_crack_seed(k, jit) - q;
			float d = dot(r, r);
			if (d < bd) {
				bd = d;
				best = r;
				id = k;
			}
		}
	}
	float e1 = 1e9;
	vec2 n1 = vec2(1.0, 0.0);
	vec2 nn = vec2(1.0, 0.0);
	float nnd = 1e9;
	for (int j = -2; j <= 2; j++) {
		for (int i = -2; i <= 2; i++) {
			ivec2 k = id + ivec2(i, j);
			if (all(equal(k, id))) {
				continue;
			}
			vec2 dr = ground_crack_seed(k, jit) - q - best;
			float dl = dot(dr, dr);
			if (dl > 1e-8) {
				vec2 dn = dr * inversesqrt(dl);
				float e = dot(best + 0.5 * dr, dn);
				if (e < e1) {
					e1 = e;
					n1 = dn;
				}
				if (dl < nnd) {
					nnd = dl;
					nn = dn;
				}
			}
		}
	}
	vec2 x = -best; // from the seed, in domain units
	vec2 h2 = ground_crack_hash(id, 2u);
	vec2 h3 = ground_crack_hash(id, 3u);
	// Second generation: along the nearest neighbour's direction, turned and shifted a little.
	float a2 = (h2.y - 0.5) * 0.4;
	vec2 u2 = vec2(nn.x * cos(a2) - nn.y * sin(a2), nn.x * sin(a2) + nn.y * cos(a2));
	vec2 v2 = vec2(-u2.y, u2.x);
	float t2 = dot(x, v2) - (h2.x - 0.5) * 0.16;
	bool split2 = h2.x * 0.7 + h2.y * 0.3 < 0.92 - 0.75 * reg;
	// Third generation, in this half: square to the second, from it outwards, ending at a length of its own.
	float side = t2 >= 0.0 ? 1.0 : 0.0;
	vec2 h3s = side > 0.5 ? h3 : ground_crack_hash(id, 4u);
	float t3 = dot(x, u2) - (h3s.x - 0.5) * 0.5;
	float len3 = mix(0.25, 0.9, h3s.y);
	// A silt skin's plates are small and thin: they split across, rim to rim, and do not grow the short spurs.
	bool split3 = split2 && skin < 0.5 && h3s.x * 0.5 + h3s.y * 0.5 < 0.75 - 0.65 * reg;
	// Widths: the layer's shrinkage (8%, Peron et al. 2009 for clays) shared out over the generations, which open
	// in turn as the layer dries; each wanders along its length.
	float strain = mix(0.08, 0.12, skin) * s1;
	vec2 wander = ground_crack_noise(q * 3.0, 7u);
	g.width = vec3(0.5 * smoothstep(0.0, 0.5, open), 0.3 * smoothstep(0.3, 0.8, open), 0.17 * smoothstep(0.6, 1.0, open)) * strain;
	g.width *= vec3(0.55 + 0.9 * wander.x, 0.55 + 0.9 * wander.y, 0.5 + wander.x);
	g.width.z *= smoothstep(len3, 0.55 * len3, abs(t2)); // the tip tapers out
	g.depth = thick * mix(1.0, 0.4, skin) * vec3(1.0, 0.75, 0.45) * clamp(4.0 * open, 0.0, 1.0);
	g.dist = vec3(max(e1, 0.0), split2 ? abs(t2) : 1e3, split3 && abs(t2) < len3 ? abs(t3) : 1e3) * s1;
	g.dir1 = -n1;
	g.dir2 = t2 >= 0.0 ? v2 : -v2;
	g.dir3 = t3 >= 0.0 ? u2 : -u2;
	g.curl = max(p_c.w, 0.0) * open * mix(0.35, 0.5, skin) * thick;
	g.reach = mix(0.06, 0.18, skin) * s1;
	g.scale = s1;
	return g;
}
// The surface's offset at the cracks seen through a footprint p_fw (m) wide: (height m, its slope x, z, the share of
// the footprint that is crack). A crack narrower than the footprint spreads into a dip of the same volume as wide
// as the footprint (a box filter), and the curl likewise; so the mesh takes the cracks its spacing carries, the
// shading those it does not.
vec4 ground_crack_height(GroundCracks p_g, float p_fw) {
	float fw = max(p_fw, 1e-4);
	float h = 0.0;
	vec2 slope = vec2(0.0);
	float cover = 0.0;
	// The rims' curl: each crack lifts the plate's edge beside it over a short reach; joined as a smooth union
	// (1 - the product of what each leaves flat), so the plate's middle stays flat with no crease where the
	// nearest crack changes.
	float reach = max(p_g.reach, 0.5 * fw);
	float flat_all = 1.0;
	float fl[3];
	vec2 fg[3];
	for (int k = 0; k < 3; k++) {
		float d = k == 0 ? p_g.dist.x : (k == 1 ? p_g.dist.y : p_g.dist.z);
		float w = k == 0 ? p_g.width.x : (k == 1 ? p_g.width.y : p_g.width.z);
		float dp = k == 0 ? p_g.depth.x : (k == 1 ? p_g.depth.y : p_g.depth.z);
		vec2 dir = k == 0 ? p_g.dir1 : (k == 1 ? p_g.dir2 : p_g.dir3);
		fl[k] = 1.0;
		fg[k] = vec2(0.0);
		if (w <= 0.0) {
			continue;
		}
		// The rim beside it: e beyond the crack's edge.
		// A crack lifts its rims only once it has opened a few millimetres (a tapering tip lifts none).
		float e = max(d - 0.5 * w, 0.0);
		float x = exp(-e / reach) * smoothstep(0.0, 0.005, w);
		fl[k] = 1.0 - x;
		fg[k] = d > 0.5 * w ? dir * (x / reach) : vec2(0.0); // the slope of (1 - x)
		flat_all *= fl[k];
		float hw = 0.5 * max(w, fw);
		float deep = dp * min(w / fw, 1.0);
		if (d < hw) {
			// Steep walls over the outer third of the half width, a rounded floor.
			float t = 1.0 - d / hw;
			float s = smoothstep(0.0, 0.35, t);
			float hk = -deep * s;
			if (hk < h) {
				h = hk;
				float ds = t < 0.35 ? 6.0 * (t / 0.35) * (1.0 - t / 0.35) / 0.35 : 0.0;
				slope = dir * (deep * ds / hw);
			}
			cover = max(cover, min(w / fw, 1.0) * (t < 0.35 ? s : 1.0));
		}
	}
	// A footprint wider than a fraction of a domain spans whole plates: their mean, flat, with the cracks' mean
	// share. The curled rims are wider than the cracks: they go on into footprints up to a few reaches wide, so a
	// mesh that cannot carry the slots still carries the dished plates.
	// The slots and the curled rims add: the rim rises from the plate to the slot's lip and the slot cuts down from
	// there, so a coarse footprint sees a dip between two lifted lips (each plate a shallow dish) and no step where
	// the slot's half width ends.
	float fade = 1.0 - smoothstep(0.04 * p_g.scale, 0.12 * p_g.scale, fw);
	float curl_fade = 1.0 - smoothstep(p_g.reach, 3.0 * p_g.reach, fw);
	float keep = max(fade, curl_fade);
	h *= keep;
	slope *= keep;
	if (p_g.curl > 0.0) {
		float c = p_g.curl * (p_g.reach / reach) * keep;
		h += c * (1.0 - flat_all);
		vec2 g = fg[0] * fl[1] * fl[2] + fg[1] * fl[0] * fl[2] + fg[2] * fl[0] * fl[1];
		slope -= c * g;
	}
	float mean = clamp(dot(p_g.width, vec3(1.0)) * 2.0 / max(p_g.scale, 1e-4), 0.0, 1.0);
	return vec4(h, slope, mix(mean, cover, fade));
}
// Mud on a vehicle's body (docs/ideas/mud.md, stage 3b), the same lines in material.cpp and scene_raytracing_raygen.glsl.
// Off-road games keep an accumulation level per region and let it threshold a splat mask, so mud lands as flecks and
// grows into a cake. Here the regions are the vehicle's own (each tyre's tread, the spray each wheel throws up its arch
// and back along the body, the belly up to the deepest it sat in mud), in the vehicle's frame (x across, y up, z forward,
// m), and the mask is an object-space noise: no authored mask, no UVs, the same in both renderers.
float vmud_hash(vec3 p_c) {
	uvec3 q = uvec3(ivec3(p_c));
	uint h = (q.x * 73856093u) ^ (q.y * 19349663u) ^ (q.z * 83492791u);
	h ^= h >> 16u;
	h *= 0x7feb352du;
	h ^= h >> 15u;
	h *= 0x846ca68bu;
	h ^= h >> 16u;
	return float(h & 0xffffu) / 65535.0;
}
float vmud_noise(vec3 p_p) {
	vec3 i = floor(p_p);
	vec3 f = fract(p_p);
	f = f * f * (3.0 - 2.0 * f);
	float a = mix(vmud_hash(i), vmud_hash(i + vec3(1.0, 0.0, 0.0)), f.x);
	float b = mix(vmud_hash(i + vec3(0.0, 1.0, 0.0)), vmud_hash(i + vec3(1.0, 1.0, 0.0)), f.x);
	float c = mix(vmud_hash(i + vec3(0.0, 0.0, 1.0)), vmud_hash(i + vec3(1.0, 0.0, 1.0)), f.x);
	float d = mix(vmud_hash(i + vec3(0.0, 1.0, 1.0)), vmud_hash(i + vec3(1.0, 1.0, 1.0)), f.x);
	return mix(mix(a, b, f.y), mix(c, d, f.y), f.z);
}
// The splat field, 0..1: splashes of about 6 cm with 2 cm drops; the drops fade to their mean over a footprint p_fw (m)
// that cannot hold them.
float vmud_splat(vec3 p_p, float p_fw) {
	float fine = mix(vmud_noise(p_p * 48.0 + 17.0), 0.5, smoothstep(0.004, 0.015, p_fw));
	return 0.6 * vmud_noise(p_p * 16.0) + 0.4 * fine;
}
// One wheel (p_w: centre, radius; its tread fill and thrown spray, 0..1).
float vmud_wheel(vec3 p_p, vec4 p_w, float p_fill, float p_spray) {
	if (p_w.w <= 0.0) {
		return 0.0;
	}
	vec3 q = (p_p - p_w.xyz) / p_w.w;
	float rho = length(q.yz);
	// The tyre: its tread packed as full as the fill, the sidewall a little less.
	float tyre = p_fill * (1.0 - smoothstep(1.0, 1.06, rho)) * (1.0 - smoothstep(0.55, 0.7, abs(q.x))) * mix(0.55, 1.0, smoothstep(0.6, 0.9, rho));
	// The spray: thrown off the top and back of the tread, up the arch and back along the lower body, thinning outwards
	// (as far as about 1.5 m behind and 0.8 m above a 35 cm wheel), heaviest low down.
	float e = length((q - vec3(0.0, 0.4, -1.2)) / vec3(2.2, 2.4, 4.4));
	float spray = p_spray * (1.0 - smoothstep(0.25, 1.0, e)) * mix(1.0, 0.6, smoothstep(0.0, 2.0, q.y));
	return max(tyre, spray);
}
// The accumulation level at p_p (0 clean, 1 caked). p_body: chassis bottom (y), mud line above it (m).
// A leg (mud stage 4e), p_p in its shin's frame (x across, y up from the sole, z forward; see vmud_at()); p_leg: mud
// line above the sole (m), shin length (m), shin radius (m), flecks (0..1). The boot below the ankle reaches forward
// to the toe.
float vmud_leg(vec3 p_p, vec4 p_leg) {
	if (p_leg.x <= 0.0 && p_leg.w <= 0.0) {
		return 0.0;
	}
	float up = p_p.y;
	float boot = 1.0 - smoothstep(0.1, 0.16, up);
	// Within the shin (a little proud of it), or within the boot's footprint, reaching 22 cm forward to the toe.
	vec2 q = vec2(p_p.x, p_p.z - boot * clamp(p_p.z, 0.0, 0.14));
	float r = mix(1.3 * p_leg.z, 0.09, boot);
	float on = (1.0 - smoothstep(r, 1.4 * r, length(q))) * step(-0.06, up) * (1.0 - smoothstep(p_leg.y, p_leg.y + 0.04, up));
	if (on <= 0.0) {
		return 0.0;
	}
	// The line ragged round the leg and higher at the front (the toe throws it up the shin).
	float line = p_leg.x * (0.7 + 0.5 * vmud_noise(vec3(p_p.x * 30.0, 0.0, p_p.z * 30.0) + 61.0)) * (1.0 + 0.25 * smoothstep(-0.02, 0.05, p_p.z));
	float lvl = p_leg.x > 0.0 ? 1.0 - clamp((up - line) / (0.03 + 0.3 * p_leg.x), 0.0, 1.0) : 0.0;
	// Flecks thrown up the shin by the stride, thinning upwards.
	float fleck = p_leg.w * (1.0 - smoothstep(0.0, 0.4, up - line)) * smoothstep(0.72, 0.8, vmud_noise(p_p * 45.0 + 7.0));
	return on * max(lvl, fleck);
}
float vmud_seg(vec3 p_p, vec3 p_a, vec3 p_b) {
	vec3 ab = p_b - p_a;
	float t = clamp(dot(p_p - p_a, ab) / max(dot(ab, ab), 1e-8), 0.0, 1.0);
	return length(p_p - p_a - ab * t);
}
float vmud_level(vec3 p_p, vec4 p_w0, vec4 p_w1, vec4 p_w2, vec4 p_w3, vec4 p_fill, vec4 p_spray, vec4 p_body) {
	if (p_body.w > 0.5) {
		return vmud_leg(p_p, p_fill);
	}
	float lvl = 0.0;
	if (p_body.y > 0.0) {
		// Caked below the line, flecked above it, the line's edge ragged.
		float line = p_body.y * (0.7 + 0.6 * vmud_noise(vec3(p_p.x * 3.0, 0.0, p_p.z * 3.0) + 41.0));
		lvl = 1.0 - clamp((p_p.y - p_body.x - line) / (0.1 + 0.5 * p_body.y), 0.0, 1.0);
	}
	lvl = max(lvl, vmud_wheel(p_p, p_w0, p_fill.x, p_spray.x));
	lvl = max(lvl, vmud_wheel(p_p, p_w1, p_fill.y, p_spray.y));
	lvl = max(lvl, vmud_wheel(p_p, p_w2, p_fill.z, p_spray.z));
	lvl = max(lvl, vmud_wheel(p_p, p_w3, p_fill.w, p_spray.w));
	return lvl;
}
// Levels joined as an "or" of their shares of a full cake (1.25): a + b - a b / 1.25. Overlapping splats and runs
// merge into one cake with no seam or ridge where they meet (a plain or smooth maximum leaves one), and a cake already
// full stays full.
float vmud_join(float a, float b) {
	a = clamp(a, 0.0, 1.25);
	b = clamp(b, 0.0, 1.25);
	return a + b - a * b / 1.25;
}
// The level thresholds the splat field: a low level keeps only the splats' peaks, a full one covers all. The cake lies
// where the field f = splat + 1.2 level passes 1.075; its edge is a ramp p_w wide in f, at least 0.15 and wider where a
// footprint holds more of the field (a filtered step), so outlines stay smooth at any distance and in both renderers.
float vmud_cover_f(float p_f, float p_w) {
	return smoothstep(1.075 - 0.5 * p_w, 1.075 + 0.5 * p_w, p_f);
}
float vmud_cover(float p_level, float p_splat) {
	return p_level <= 0.0 ? 0.0 : vmud_cover_f(p_splat + 1.2 * p_level, 0.15);
}
// Desiccation cracks (mud round 11): drying clay shrinks against the panel it sticks to and splits into plates a few
// times as wide as the layer is thick (Groisman and Kaplan 1994); a layer under a critical thickness dries whole
// (Routh 2013), and the cracks open as it dries, the plates' edges curling up off the panel. The plates are cells of a
// cellular (Worley 1996) distance, F2 - F1, in the vehicle's frame, warped so they read as clay rather than foam.
vec2 vmud_cells(vec3 p_p) {
	vec3 i = floor(p_p);
	vec3 f = p_p - i;
	float d1 = 8.0;
	float d2 = 8.0;
	for (int z = -1; z <= 1; z++) {
		for (int y = -1; y <= 1; y++) {
			for (int x = -1; x <= 1; x++) {
				vec3 o = vec3(float(x), float(y), float(z));
				vec3 cc = i + o;
				vec3 jit = vec3(vmud_hash(cc + vec3(911.0, 0.0, 0.0)), vmud_hash(cc + vec3(0.0, 577.0, 0.0)), vmud_hash(cc + vec3(0.0, 0.0, 313.0)));
				float d = length(o + 0.1 + 0.8 * jit - f);
				if (d < d1) {
					d2 = d1;
					d1 = d;
				} else if (d < d2) {
					d2 = d;
				}
			}
		}
	}
	return vec2(d1, d2);
}
// At p_p (vehicle frame, m) for a cake p_h thick, p_dry dry (0..1) over a footprint p_fw: (the crack's share, the
// plate's curl, m).
vec2 vmud_crack(vec3 p_p, float p_h, float p_dry, float p_fw) {
	float on = smoothstep(0.0028, 0.0045, p_h) * smoothstep(0.45, 0.9, p_dry);
	if (on <= 0.0 || p_fw > 0.012) {
		return vec2(0.0);
	}
	const float CELL = 0.022;
	vec3 q = p_p / CELL + 0.35 * vec3(vmud_noise(p_p * 31.0 + 5.0), vmud_noise(p_p * 31.0 + 19.0), vmud_noise(p_p * 31.0 + 37.0));
	vec2 d = vmud_cells(q);
	// Half the distance between the two nearest plate centres' bisector, in m: how far to the plate's edge.
	float e = 0.5 * (d.y - d.x) * CELL;
	float w = 0.0006 * on;
	float fw = max(p_fw, 1e-4);
	// A filtered step over the footprint, faded where a pixel holds many plates.
	float open = (1.0 - smoothstep(w - 0.5 * fw, w + 0.5 * fw, e)) * (1.0 - smoothstep(0.004, 0.012, p_fw));
	float curl = 0.0007 * on * (1.0 - smoothstep(w, w + 0.005, e));
	return vec2(open, curl);
}
// The cake's relief (m): thicker where it lies deep (a splat's middle, below the mud line), lumpy, and with a lip a few
// mm wide just inside its edge where a splash's crown piles up; its edge a step.
float vmud_height(float p_f, float p_w, float p_level, float p_splat, vec3 p_p, float p_fw) {
	float c = vmud_cover_f(p_f, p_w);
	float lip = 1.0 - smoothstep(0.05, 0.35, p_f - 1.075);
	// Aggregate: grit of a few mm and the odd lump of a pebble or a root, fading over a footprint too wide to hold them.
	float grit = (vmud_noise(p_p * 260.0 + 11.0) - 0.5) * (1.0 - smoothstep(0.0015, 0.005, p_fw));
	// (two value noises on different, turned lattices: one alone thresholded this high leaves grid-aligned crosses)
	float ln = 0.5 * (vmud_noise(p_p * 70.0 + 29.0) + vmud_noise(vec3(0.8 * p_p.x + 0.6 * p_p.z, p_p.y, 0.8 * p_p.z - 0.6 * p_p.x) * 93.0 + 5.0));
	float lump = smoothstep(0.62, 0.78, ln) * (1.0 - smoothstep(0.004, 0.015, p_fw));
	return c * (0.002 + 0.0016 * clamp(p_level, 0.0, 1.25) + 0.002 * p_splat + 0.0015 * lip + 0.0005 * grit + 0.0012 * lump);
}
// A clod that stuck to the body (mud stage 4b; p_a: where, in the vehicle's frame, and its radius; p_b: its flight
// direction and amount): a splat spread along the flight, a short tail ahead of the hit and a shorter rim behind it.
float vmud_splat_one(vec3 p_p, vec4 p_a, vec4 p_b) {
	vec3 d = p_p - p_a.xyz;
	float along = dot(d, p_b.xyz);
	float e = length(d - along * p_b.xyz) / p_a.w + max(along, 0.0) / (2.5 * p_a.w) + max(-along, 0.0) / (0.7 * p_a.w);
	// A ragged rim (the splash's fingers) and a halo of droplets thrown past it, thresholded by the splat field. The
	// core is a dome, deepest in the middle.
	float rag = 0.75 + 0.5 * vmud_noise(p_p * 22.0 + 7.0);
	e *= rag;
	float core = 1.25 * (1.0 - smoothstep(0.3, 1.0, e));
	float halo = 0.5 * (1.0 - smoothstep(1.0, 2.2, e));
	float lvl = p_b.w * max(core, halo);
	// Drip runs: wet clay is thick, so it sags only a little way down the panel the clod hit, in a few short fat runs,
	// lumpy along their length, each ending in a bead. None from a clod that hit a roof or a floor.
	vec3 tn = cross(vec3(0.0, 1.0, 0.0), p_b.xyz);
	float tl = length(tn);
	if (d.y < 0.0 && tl > 0.3 && abs(dot(d, cross(tn, vec3(0.0, 1.0, 0.0)))) < 0.5 * p_a.w * tl) {
		float seed = dot(p_a.xyz, vec3(13.1, 7.7, 5.3));
		// The runs wander a little as they sag.
		float t = dot(d, tn) / tl + 0.006 * (vmud_noise(vec3(d.y * 35.0, seed, 7.5)) - 0.5);
		// Runs in lanes about 3 cm wide across the panel, about half of them with a run: each its own length and
		// width, so every splat drips a little whatever its seed.
		float u = t * 30.0;
		float lane = floor(u);
		float fu = fract(u) - 0.5;
		float has = step(0.45, vmud_hash(vec3(lane, seed, 1.0)));
		// From the splat's lower rim, 2 to 6 cm past it whatever the splat's size: clay is too stiff to run far.
		float run = 0.015 + 0.035 * vmud_hash(vec3(lane, seed, 5.0)) + 0.08 * p_a.w;
		// Measured from the ragged rim above it in its own column (where the dome thins out, e about 0.7 across the
		// splat's round), found in two steps since the rim's raggedness moves it; a lane past the splat's side has no
		// rim above it and no run.
		float rr = 0.7 * p_a.w / (0.75 + 0.5 * vmud_noise((p_p - vec3(0.0, d.y + 0.7 * p_a.w, 0.0)) * 22.0 + 7.0));
		float rim = sqrt(max(rr * rr - t * t, 0.0));
		rr = 0.68 * p_a.w / (0.75 + 0.5 * vmud_noise((p_p - vec3(0.0, d.y + rim, 0.0)) * 22.0 + 7.0));
		has *= step(abs(t), 0.9 * rr);
		rim = sqrt(max(rr * rr - t * t, 0.0));
		// A third of a run's length inside it, so the run grows out of the rim's thin edge with no gap.
		float down = max(-d.y - rim + 0.3 * run, 0.0) / run;
		float bead = exp(-pow((down - 1.15) / 0.1, 2.0));
		float lumpy = 0.14 * (vmud_noise(vec3(u * 2.0, d.y * 70.0, seed)) - 0.5);
		// Half its lane's width (0.2 to 0.33 of it), thinning as it runs, swelling at the bead.
		float half_w = (0.2 + 0.13 * vmud_hash(vec3(lane, seed, 9.0))) * (1.0 - 0.45 * down) + 0.14 * bead + lumpy;
		// Nothing inside the splat itself (it would show as ridges where the run meets the dome).
		float drip = has * (1.0 - smoothstep(half_w - 0.05, half_w, abs(fu))) * smoothstep(0.0, 0.3, down) * (1.0 - smoothstep(1.25, 1.31, down)) * (1.0 - smoothstep(0.35, 0.6, abs(t) / p_a.w));
		// Only past the cake's thick part (its halo of droplets aside): inside it a run would show its lane's edges
		// in the relief.
		lvl = vmud_join(lvl, (1.0 + 0.15 * bead) * p_b.w * drip * (1.0 - smoothstep(0.2, 0.6, p_b.w * core)));
	}
	return lvl;
}
// Spatter is drawn out along its flight (mud stage 4d): the direction out from the nearest wheel, the way its spray
// left the tread, with a faint vertical grain where it ran (stuck clods carry their own drip runs).
vec3 vmud_flow(vec3 p_p, vec4 p_w0, vec4 p_w1, vec4 p_w2, vec4 p_w3) {
	vec3 flow = vec3(0.0, -1.0, 0.0);
	float best = 1e9;
	vec4 w[4] = vec4[](p_w0, p_w1, p_w2, p_w3);
	for (int k = 0; k < 4; k++) {
		if (w[k].w > 0.0) {
			vec3 d = p_p - w[k].xyz;
			float l = length(d);
			if (l < best && l > 1e-4) {
				best = l;
				flow = d / l;
			}
		}
	}
	return flow;
}
// The splat field sampled with the flight squeezed out of it (features nearly twice as long along it) and the
// vertical grain.
float vmud_streaked(vec3 p_p, vec3 p_flow, float p_fw) {
	vec3 q = p_p - p_flow * (0.45 * dot(p_p, p_flow));
	return vmud_splat(q, p_fw) + 0.15 * (vmud_noise(vec3(p_p.x * 45.0, p_p.y * 5.0, p_p.z * 45.0) + 3.0) - 0.5);
}
// The clods stuck to this vehicle (mud stage 4b), as the raster shader's vmud_splats_level().
float vmud_splats_level(vec3 p_p, vec4 p_splat) {
	uint tex = uint(get_rt_param(RT_PARAM_VEHICLE_MUD_SPLATS));
	if (tex == 0u) {
		return 0.0;
	}
	float lvl = 0.0;
	int row = int(p_splat.x);
	int n = int(p_splat.y);
	for (int k = 0; k < n; k++) {
		vec4 a = texelFetch(sampler2D(bindless_textures[nonuniformEXT(tex)], SAMPLER_NEAREST_CLAMP), ivec2(2 * k, row), 0);
		vec4 b = texelFetch(sampler2D(bindless_textures[nonuniformEXT(tex)], SAMPLER_NEAREST_CLAMP), ivec2(2 * k + 1, row), 0);
		lvl = vmud_join(lvl, vmud_splat_one(p_p, a, b));
	}
	return lvl;
}
// Every region's level and the stuck clods'.
float vmud_all(vec3 p_p, vec4 p_w0, vec4 p_w1, vec4 p_w2, vec4 p_w3, vec4 p_fill, vec4 p_spray, vec4 p_body, vec4 p_splat) {
	float lvl = vmud_level(p_p, p_w0, p_w1, p_w2, p_w3, p_fill, p_spray, p_body);
	if (p_splat.y > 0.0) {
		lvl = max(lvl, vmud_splats_level(p_p, p_splat));
	}
	return lvl;
}
// The mud at world point p_world on a vehicle (p_frame0..2: world to vehicle rows; p_splat: its row of stuck clods
// and their count, see vmud_splats_level()): (cover, relief gradient in world space (x, y, z) per m). The cover's edge
// is filtered over the footprint p_fw; the gradient fades out over a footprint too wide to hold the lumps.
vec4 vmud_at(vec3 p_world, float p_fw, vec4 p_frame0, vec4 p_frame1, vec4 p_frame2, vec4 p_w0, vec4 p_w1, vec4 p_w2, vec4 p_w3, vec4 p_fill, vec4 p_spray, vec4 p_body, vec4 p_splat, out vec2 r_tone) {
	r_tone = vec2(1.0, 0.0);
	bool legs = p_body.w > 0.5;
	if (legs) {
		// Legs (mud stage 4e): p_w0/p_w1 the left sole and knee (w: its mud line, its flecks), p_w2/p_w3 the right,
		// p_frame0.xyz the way the character faces, p_fill.x the shin radius. The nearer leg's own frame: along the
		// shin, across it, towards the toe, so the mud moves with the bones through the stride.
		bool left = vmud_seg(p_world, p_w0.xyz, p_w1.xyz) <= vmud_seg(p_world, p_w2.xyz, p_w3.xyz);
		vec4 sole = left ? p_w0 : p_w2;
		vec4 knee = left ? p_w1 : p_w3;
		vec3 ax = knee.xyz - sole.xyz;
		float len = length(ax);
		ax = len > 1e-4 ? ax / len : vec3(0.0, 1.0, 0.0);
		vec3 across = cross(ax, p_frame0.xyz);
		across = dot(across, across) > 1e-6 ? normalize(across) : normalize(cross(ax, abs(ax.x) < 0.9 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 0.0, 1.0)));
		vec3 front = cross(across, ax);
		p_fill = vec4(sole.w, len, p_fill.x, knee.w);
		p_frame0 = vec4(across, -dot(across, sole.xyz));
		p_frame1 = vec4(ax, -dot(ax, sole.xyz));
		p_frame2 = vec4(front, -dot(front, sole.xyz));
		p_splat = vec4(0.0);
	}
	vec3 p = vec3(dot(p_frame0.xyz, p_world) + p_frame0.w, dot(p_frame1.xyz, p_world) + p_frame1.w, dot(p_frame2.xyz, p_world) + p_frame2.w);
	float lvl = vmud_all(p, p_w0, p_w1, p_w2, p_w3, p_fill, p_spray, p_body, p_splat);
	if (lvl <= 0.0) {
		return vec4(0.0);
	}
	float fw = max(p_fw, 1e-4);
	// The differences' step follows the footprint, fine enough near to hold the grit.
	float e = clamp(1.5 * fw, 0.0008, 0.003);
	vec3 flow = legs ? vec3(0.0, -1.0, 0.0) : vmud_flow(p, p_w0, p_w1, p_w2, p_w3);
	float s = vmud_streaked(p, flow, fw);
	float f = s + 1.2 * lvl;
	vec3 px = p + vec3(e, 0.0, 0.0);
	vec3 py = p + vec3(0.0, e, 0.0);
	vec3 pz = p + vec3(0.0, 0.0, e);
	vec3 ls = vec3(vmud_all(px, p_w0, p_w1, p_w2, p_w3, p_fill, p_spray, p_body, p_splat), vmud_all(py, p_w0, p_w1, p_w2, p_w3, p_fill, p_spray, p_body, p_splat),
			vmud_all(pz, p_w0, p_w1, p_w2, p_w3, p_fill, p_spray, p_body, p_splat));
	vec3 ss = vec3(vmud_streaked(px, flow, fw), vmud_streaked(py, flow, fw), vmud_streaked(pz, flow, fw));
	vec3 fs = ss + 1.2 * ls;
	// The edge's ramp: as wide as the field changes over one and a half footprints, and no softer: clay is there or
	// not, so where the field rises slowly its edge stays crisp rather than fading over centimetres.
	float w = max(0.03, 1.5 * length(fs - f) / e * fw);
	float c = vmud_cover_f(f, w);
	if (c <= 0.0) {
		return vec4(0.0);
	}
	float h0 = vmud_height(f, w, lvl, s, p, fw);
	vec3 hs = vec3(vmud_height(fs.x, w, ls.x, ss.x, px, fw), vmud_height(fs.y, w, ls.y, ss.y, py, fw), vmud_height(fs.z, w, ls.z, ss.z, pz, fw));
	// Drying cracks (round 11): the cake's dryness here, its thick middle drying last (as vmud_apply() tones it).
	float t0 = smoothstep(0.85, 1.2, lvl);
	float dry = 1.0 - clamp(p_body.z + 0.6 * t0 * (1.0 - p_body.z) + 0.25 * t0, 0.0, 1.0);
	if (dry > 0.45 && !legs) {
		vec3 ts = smoothstep(vec3(0.85), vec3(1.2), ls);
		vec3 drys = 1.0 - clamp(p_body.z + 0.6 * ts * (1.0 - p_body.z) + 0.25 * ts, 0.0, 1.0);
		vec2 k0 = vmud_crack(p, h0, dry, fw);
		vec2 kx = vmud_crack(px, hs.x, drys.x, fw);
		vec2 ky = vmud_crack(py, hs.y, drys.y, fw);
		vec2 kz = vmud_crack(pz, hs.z, drys.z, fw);
		h0 = (h0 + k0.y) * (1.0 - k0.x);
		hs = (hs + vec3(kx.y, ky.y, kz.y)) * (1.0 - vec3(kx.x, ky.x, kz.x));
		// The panel shows in the crack.
		c *= 1.0 - k0.x;
	}
	vec3 g = hs - h0;
	g *= (1.0 - smoothstep(0.004, 0.02, fw)) / e;
	// Back to world space: the frame's rows are a rotation.
	vec3 gw = g.x * p_frame0.xyz + g.y * p_frame1.xyz + g.z * p_frame2.xyz;
	// The tone: clay mottled over a few cm (more or less silt and organic matter) with a speckle of grit, and the
	// thick middle of a cake still wet (darker, glossier) after its thin edges have dried.
	float mottle = vmud_noise(p * 9.0 + 53.0);
	float speck = (vmud_noise(p * 260.0 + 11.0) - 0.5) * (1.0 - smoothstep(0.0015, 0.006, fw));
	r_tone = vec2(0.78 + 0.45 * mottle + 0.35 * speck, smoothstep(0.85, 1.2, lvl));
	return vec4(c, gw);
}
// Wet clay dark and glossy, drying to a pale matte crust (the crack crust's tone).
void vmud_apply(float p_cover, vec2 p_tone, vec3 p_color, float p_wet, inout vec3 r_albedo, inout float r_roughness, inout float r_metallic, inout float r_specular) {
	// The thick middle stays wet longer: it dries last.
	float wet = clamp(p_wet + 0.6 * p_tone.y * (1.0 - p_wet) + 0.25 * p_tone.y, 0.0, 1.0);
	vec3 dry = p_color * 1.6;
	dry = mix(dry, vec3(dot(dry, vec3(0.299, 0.587, 0.114))), 0.2);
	vec3 clay = mix(dry, p_color * 0.5, wet) * p_tone.x * mix(1.0, 0.62, p_tone.y * wet);
	// Patches richer in iron oxide read warmer, siltier ones greyer.
	clay *= mix(vec3(0.93, 0.97, 1.06), vec3(1.07, 0.99, 0.9), clamp((p_tone.x - 0.75) / 0.5, 0.0, 1.0));
	r_albedo = mix(r_albedo, clay, p_cover);
	// Wet clay glistens, but not as plastic: grit breaks its film up.
	r_roughness = mix(r_roughness, clamp(mix(0.95, 0.36, wet) + 0.35 * abs(1.0 - p_tone.x), 0.2, 1.0), p_cover);
	r_metallic = mix(r_metallic, 0.0, p_cover);
	r_specular = mix(r_specular, 0.5, p_cover);
}

// The ripples' variation (nothing repeating may show): the grown field tiles every few metres, so each point reads
// it through a three-octave world-space warp (crests turn and change spacing over tens of metres and bend over a
// few) and edge dislocations in pairs along the crests (a crest forks into two and rejoins: the Y-junctions of
// real ripple fields), with a height that wanders between 35 and 100%. p_k is the grown field's dominant wave
// vector (rad/m), so a dislocation shifts its crests by exactly one wavelength. The real ripples' window is filled
// through the same lines (GroundRipples::vary), so the two meet crest to crest.
float ground_ripple_hash(ivec2 p_c, uint p_s) {
	uint h = (uint(p_c.x) * 0x8da6b343u) ^ (uint(p_c.y) * 0xd8163841u) ^ (p_s * 0xcb1ab31fu);
	h = (h ^ (h >> 16u)) * 0x7feb352du;
	h = (h ^ (h >> 15u)) * 0x846ca68bu;
	h ^= h >> 16u;
	return float(h >> 8u) * (1.0 / 16777216.0);
}
float ground_ripple_vn(vec2 p_p, uint p_s) {
	vec2 i = floor(p_p);
	vec2 f = p_p - i;
	vec2 u = f * f * (3.0 - 2.0 * f);
	ivec2 c = ivec2(i);
	return mix(mix(ground_ripple_hash(c, p_s), ground_ripple_hash(c + ivec2(1, 0), p_s), u.x), mix(ground_ripple_hash(c + ivec2(0, 1), p_s), ground_ripple_hash(c + ivec2(1, 1), p_s), u.x), u.y);
}

// material.cpp ground_salt_cells(), the same lines.
vec2 ground_salt_cells(vec2 p_p) {
	vec2 ip = floor(p_p);
	vec2 fp = p_p - ip;
	float d1 = 8.0;
	float d2 = 8.0;
	vec2 id = ip;
	for (int j = -1; j <= 1; j++) {
		for (int i = -1; i <= 1; i++) {
			vec2 g = vec2(float(i), float(j));
			uvec2 q = uvec2(ivec2(ip + g) + ivec2(32768));
			uint h = (q.x * 1597334677u) ^ (q.y * 3812015801u);
			h = (h ^ (h >> 16u)) * 2246822519u;
			h ^= h >> 13u;
			vec2 o = vec2(float(h & 0xffffu), float(h >> 16u)) / 65535.0;
			vec2 r = g + 0.15 + 0.7 * o - fp;
			float d = dot(r, r);
			if (d < d1) {
				d2 = d1;
				d1 = d;
				id = ip + g;
			} else if (d < d2) {
				d2 = d;
			}
		}
	}
	uvec2 q = uvec2(ivec2(id) + ivec2(32768));
	uint h = (q.x * 2654435761u) ^ (q.y * 2246822519u);
	h = (h ^ (h >> 15u)) * 2654435761u;
	return vec2(0.5 * (sqrt(d2) - sqrt(d1)), float(h >> 8u) / 16777215.0);
}
// Grain sorting on wind ripples: dark heavy minerals in the troughs, coarse paler crests (material.cpp
// ground_ripple_sort_tint(), the same lines).
vec3 ground_ripple_sort_tint(float p_t, vec2 p_xz, float p_resolved, float p_amp) {
	float heavy = mix(0.35, 1.0, ground_ripple_vn(p_xz / 7.0, 31u));
	vec3 lag = mix(vec3(1.0), vec3(0.5, 0.48, 0.47), 0.6 * heavy);
	vec3 coarse = vec3(1.05, 1.0, 0.92);
	vec3 rt = mix(vec3(1.0), lag, smoothstep(0.2, 0.75, -p_t)) * mix(vec3(1.0), coarse, smoothstep(0.3, 0.8, p_t));
	vec3 mt = mix(vec3(1.0), mix(vec3(1.0), lag, 0.25) * mix(vec3(1.0), coarse, 0.25), clamp(p_amp, 0.0, 1.0));
	return mix(mt, rt, p_resolved);
}
// The coarse lag's share on an interdune flat (the dune tone's lag term): the floors between dunes, flat, patchy.
float ground_dune_lag(vec2 p_xz, float p_normal_y, float p_interdune) {
	if (p_interdune < 0.0) {
		return 0.0;
	}
	float thin = p_interdune;
	return thin * smoothstep(0.93, 0.98, p_normal_y) * mix(0.45, 1.0, ground_ripple_vn(p_xz / 23.0, 43u));
}
// Adhesion warts (GroundSurface3D adhesion_warts; docs/ideas/sand.md, "Adhesion warts"): dry grains the wind drives
// onto damp sand stick where they land and build irregular lumps, longer across the wind, about 1 to 2 cm across and
// a few millimetres high, their tops of dry, paler grains (Hunter 1973; Kocurek and Fielder 1982). p_grow: how far
// they have grown (0..1) times the damp share; p_wind: the wind that built them (unit, x and z); p_fp: the pixel's
// footprint (m), lumps finer than about three pixels fade to their mean. Returns the slope (dh/dx, dh/dz); p_cap: the
// share of dry grains on top (it dries the pixel's pore water).
vec2 ground_adhesion_warts(vec2 p_xz, float p_grow, vec2 p_wind, float p_fp, out float p_cap) {
	p_cap = 0.0;
	if (p_grow <= 0.0 || dot(p_wind, p_wind) < 0.5) {
		return vec2(0.0);
	}
	float res = 1.0 - smoothstep(0.002, 0.006, p_fp);
	if (res <= 0.0) {
		p_cap = 0.5 * p_grow;
		return vec2(0.0);
	}
	// Lumps a centimetre or two across, longer across the wind, that merge into a rough skin as they grow: the
	// tops of two octaves of value noise over a threshold that falls with the growth, warped and turned against
	// each other so no lattice shows.
	vec2 k = p_wind;
	vec2 t = vec2(-k.y, k.x);
	// Patchy over decimetres, as the supply of blown grains and the dampness are.
	float gl = p_grow * mix(0.45, 1.0, ground_ripple_vn(p_xz / 0.7, 79u));
	float th = 0.72 - 0.3 * gl;
	float H = 0.002 + 0.003 * gl;
	float e = 0.0015;
	vec3 h3;
	for (int i = 0; i < 3; i++) {
		vec2 q = p_xz + (i == 1 ? vec2(e, 0.0) : (i == 2 ? vec2(0.0, e) : vec2(0.0)));
		vec2 ab = vec2(dot(q, k), dot(q, t));
		ab += 0.005 * vec2(ground_ripple_vn(ab / 0.011, 77u), ground_ripple_vn(ab / 0.011 + 17.3, 78u));
		vec2 ab2 = mat2(vec2(0.8, 0.6), vec2(-0.6, 0.8)) * ab;
		float n = 0.65 * ground_ripple_vn(ab / vec2(0.008, 0.013), 75u) + 0.35 * ground_ripple_vn(ab2 / vec2(0.0035, 0.005), 76u);
		h3[i] = smoothstep(th, th + 0.22, n);
	}
	// The mean dry-topped share where the lumps are finer than the pixel.
	p_cap = mix(0.5 * p_grow, h3.x, res);
	return res * H * vec2(h3.y - h3.x, h3.z - h3.x) / e;
}
// Rain pits (GroundSurface3D rain_pits; docs/ideas/sand.md, "Rain pits and rain crust"): each drop digs a crater
// about 2.5 times its own size, some 0.12 of its width deep, ringed by a low rim of the sand it throws out
// (Katsuragi 2010; Zhao, van der Meer and Lohse 2015), 3 to 7 mm across here, with two chances in each 1 cm cell,
// filled by p_cover (the share the rain so far has pitted); p_seed changes the pattern each rain. p_fp: the pixel's
// footprint (m); craters finer than about three pixels fade to their mean, a slightly darker, rougher skin.
// Returns the slope (dh/dx, dh/dz); p_tone: the albedo factor.
vec2 ground_rain_pits(vec2 p_xz, float p_cover, float p_seed, float p_fp, out float p_tone) {
	float res = 1.0 - smoothstep(0.0007, 0.002, p_fp);
	p_tone = 1.0 - 0.05 * p_cover * (1.0 - res);
	if (p_cover <= 0.0 || res <= 0.0) {
		return vec2(0.0);
	}
	ivec2 off = ivec2(int(p_seed) * 7919, int(p_seed) * 104729);
	ivec2 c0 = ivec2(floor(p_xz / 0.01));
	vec2 g = vec2(0.0);
	for (int dz = -1; dz <= 1; dz++) {
		for (int dx = -1; dx <= 1; dx++) {
			ivec2 c = c0 + ivec2(dx, dz);
			for (int k = 0; k < 2; k++) {
				ivec2 ck = c + off + ivec2(k * 50331, 0);
				if (ground_ripple_hash(ck, 61u) >= 0.8 * p_cover) {
					continue;
				}
				vec2 m = (vec2(c) + vec2(ground_ripple_hash(ck, 62u), ground_ripple_hash(ck, 63u))) * 0.01;
				float R = 0.0015 + 0.002 * ground_ripple_hash(ck, 64u) * ground_ripple_hash(ck, 65u);
				vec2 d = p_xz - m;
				float r = length(d);
				float u = r / R;
				if (u >= 1.6 || r < 1e-6) {
					continue;
				}
				// h = -0.24 R (1 - u^2) inside, and a rim 0.072 R high round u = 1.1.
				float rim = (u - 1.1) / 0.25;
				float dhdu = (u < 1.0 ? 0.48 * R * u : 0.0) - 0.072 * R * 8.0 * rim * exp(-rim * rim);
				g += dhdu / R * (d / r);
			}
		}
	}
	return g * res;
}
// Granule ripples (megaripples) on that lag: the coarse grains the wind cannot lift creep up into long, low ripples
// 0.5 to 2 m apart and up to 5 cm high (ripple index about 25), broad crests armoured with coarse granules over
// troughs of finer sand (Sharp 1963; Ellwood, Evans and Wilson 1975; Yizhaq 2008; Lammel et al. 2018). In plan the
// crests are sinuous and fork in Y-junctions, and each fades out along its length over a few metres. Crests run
// across p_wind, the dune-building wind (unit, x and z). p_weight: the lag's share; p_fp: the pixel's footprint (m),
// crests closer than about four pixels fade to their mean tone. Returns the slope (dh/dx, dh/dz); p_tint: the albedo
// factor (the crests' coarse grains, paler than the troughs' fines).
// The phase (in wavelengths): the spacing wanders between about 0.5 and 2 m (a warp along the wind whose slope adds
// to or takes from the 1.1 m mean) and the crests bend over metres; they end by tapering out (no phase defects:
// a defect's closed contours read as small rimmed pits).
float ground_granule_phase(vec2 p_xz, vec2 p_k, vec2 p_t) {
	float along = dot(p_xz, p_k);
	float across = dot(p_xz, p_t);
	float s = along / 1.1 + 2.5 * ground_ripple_vn(vec2(along / 6.0, across / 5.0), 52u) + 1.2 * ground_ripple_vn(p_xz / 9.0, 51u);
	return s;
}
vec2 ground_granule_ripples(vec2 p_xz, float p_weight, vec2 p_wind, float p_fp, out vec3 p_tint) {
	p_tint = vec3(1.0);
	if (p_weight <= 0.0 || dot(p_wind, p_wind) < 0.5) {
		return vec2(0.0);
	}
	vec2 k = p_wind;
	vec2 t = vec2(-k.y, k.x);
	float along = dot(p_xz, k);
	float across = dot(p_xz, t);
	// Crests tens of wavelengths long that taper out at their ends (or merge at a fork).
	float amp = p_weight * smoothstep(0.15, 0.5, ground_ripple_vn(vec2(along / 3.0, across / 15.0), 53u));
	if (amp <= 0.0) {
		return vec2(0.0);
	}
	// The slope from the phase's own gradient (the warp turns the crests), by differences.
	float e = 0.03;
	float s = ground_granule_phase(p_xz, k, t);
	vec2 gs = vec2(ground_granule_phase(p_xz + vec2(e, 0.0), k, t) - s, ground_granule_phase(p_xz + vec2(0.0, e), k, t) - s);
	gs /= e;
	float c = 0.5 + 0.5 * cos(6.2831853 * s);
	// Resolved by the local spacing (1 / |grad s|).
	float lam = 1.0 / max(length(gs), 0.3);
	float resolved = 1.0 - smoothstep(0.15 * lam, 0.3 * lam, p_fp);
	vec3 crest = vec3(1.08, 1.05, 0.99);
	vec3 trough = vec3(0.95, 0.92, 0.87);
	// About a third of a ripple's length is crest: the unresolved pixel takes the mean.
	vec3 rt = mix(trough, crest, smoothstep(0.35, 0.8, c));
	vec3 mt = mix(trough, crest, 0.4);
	p_tint = mix(vec3(1.0), mix(mt, rt, resolved), amp);
	// Low and rounded (h = 0.04 amp c, slopes under 7 degrees, so a low sun leaves no hard-edged dark troughs):
	// dh/dp = 0.04 amp dc/ds grad s.
	float dhds = 0.04 * amp * (-3.14159265 * sin(6.2831853 * s));
	return resolved * dhds * gs;
}
// Dune-scale sand colour: rubefaction, coarse interdune flats, pale slip faces (material.cpp ground_dune_tone(),
// the same lines).
vec3 ground_dune_tone(vec2 p_xz, float p_normal_y, float p_interdune) {
	vec3 t = vec3(1.0);
	if (p_interdune >= 0.0) {
		float thin = p_interdune;
		float age = smoothstep(0.25, 0.7, 0.65 * ground_ripple_vn(p_xz / 420.0, 41u) + 0.35 * ground_ripple_vn(p_xz / 130.0, 42u));
		t *= mix(vec3(1.0), vec3(1.15, 0.93, 0.78), age * (0.7 + 0.3 * thin));
		float lag = ground_dune_lag(p_xz, p_normal_y, p_interdune);
		t *= mix(vec3(1.0), vec3(0.84, 0.74, 0.62), lag);
	}
	// Slopes of about 29 to 33 degrees: the slip face at the sand's repose.
	float slip = smoothstep(0.78, 0.84, p_normal_y) * (1.0 - smoothstep(0.88, 0.93, p_normal_y));
	t *= mix(vec3(1.0), vec3(1.08, 1.07, 1.05), slip);
	return t;
}
// The ripple field's patchy height (the amplitude ground_ripple_vary() returns), alone.
float ground_ripple_amp(vec2 p) {
	float amp = mix(0.2, 1.0, smoothstep(0.1, 0.9, 0.5 * ground_ripple_vn(p / 6.0, 7u) + 0.5 * ground_ripple_vn(p / 1.8, 8u)));
	amp *= smoothstep(0.36, 0.6, 0.7 * ground_ripple_vn(p / 16.0, 15u) + 0.3 * ground_ripple_vn(p / 5.0, 16u));
	return amp;
}
vec3 ground_ripple_vary(vec2 p_xz, vec2 p_k) {
	vec2 p = p_xz;
	vec2 o = 4.0 * (vec2(ground_ripple_vn(p / 24.0, 1u), ground_ripple_vn(p / 24.0, 2u)) - 0.5);
	o += 1.2 * (vec2(ground_ripple_vn(p / 6.0, 3u), ground_ripple_vn(p / 6.0, 4u)) - 0.5);
	o += 0.06 * (vec2(ground_ripple_vn(p / 1.7, 5u), ground_ripple_vn(p / 1.7, 6u)) - 0.5);
	// Patchy over tens of metres: smooth sheets between ripple fields (material.cpp ground_ripple_vary()).
	float amp = ground_ripple_amp(p);
	float kk = dot(p_k, p_k);
	if (kk > 0.0) {
		vec2 kn = p_k / sqrt(kk);
		vec2 t = vec2(-kn.y, kn.x);
		// Spacing: a stretch along the crests' normal only, so crests crowd and open by up to a fifth over a few metres.
		o += kn * (0.3 * (ground_ripple_vn(vec2(dot(p, kn) / 2.5, dot(p, t) / 6.0), 14u) - 0.5));
		ivec2 c0 = ivec2(floor(p / 2.0));
		float ph = 0.0;
		for (int dz = -1; dz <= 1; dz++) {
			for (int dx = -1; dx <= 1; dx++) {
				ivec2 c = c0 + ivec2(dx, dz);
				if (ground_ripple_hash(c, 9u) >= 0.5) {
					continue;
				}
				vec2 m = (vec2(c) + vec2(ground_ripple_hash(c, 10u), ground_ripple_hash(c, 11u))) * 2.0;
				float hl = 0.15 + 0.3 * ground_ripple_hash(c, 12u);
				vec2 da = p - (m - t * hl);
				vec2 db = p - (m + t * hl);
				float sgn = ground_ripple_hash(c, 13u) < 0.5 ? -1.0 : 1.0;
				ph += sgn * atan(da.x * db.y - da.y * db.x, dot(da, db)) * (1.0 - smoothstep(0.9, 1.9, length(p - m)));
			}
		}
		o += p_k * (ph / kk);
	}
	return vec3(o, amp);
}

// Standing water's depth between texels. The water's level (bed plus depth) is what varies smoothly, so the depth
// is taken as the level less the bed, interpolated, with dry texels given their wet neighbours' mean level (a
// level-set extension, as heightfield water renderers do at the wet/dry front): the waterline then runs across the
// texels where the level meets the bed. Interpolating the depth itself and thresholding it near zero (the film's
// gloss starts at 20 um) draws every dry texel as a square island with stepped edges. p_r and p_g: the four texels'
// net depression and depth (textureGather order), p_f the position between them.
// The deformation map at p_xz with the near field blended in inside its square (material.cpp ground_deform_sample()).
vec4 rt_ground_sample(vec2 p_xz) {
	uint gmap = uint(get_rt_param(RT_PARAM_GROUND_DEFORM_MAP));
	vec2 guv = (p_xz - vec2(get_rt_param(RT_PARAM_GROUND_DEFORM_X), get_rt_param(RT_PARAM_GROUND_DEFORM_Z))) / max(get_rt_param(RT_PARAM_GROUND_DEFORM_SIZE), 1e-3);
	if (gmap == 0u || any(lessThan(guv, vec2(0.0))) || any(greaterThan(guv, vec2(1.0)))) {
		return vec4(0.0, 0.0, 1.0, 1.0);
	}
	vec4 g = textureLod(sampler2D(bindless_textures[nonuniformEXT(gmap)], SAMPLER_LINEAR_CLAMP), guv, 0.0);
	uint nmap = uint(get_rt_param(RT_PARAM_GROUND_NEAR_MAP));
	float nsize = get_rt_param(RT_PARAM_GROUND_NEAR_SIZE);
	if (nmap != 0u && nsize > 0.0) {
		vec2 n0 = vec2(get_rt_param(RT_PARAM_GROUND_NEAR_X), get_rt_param(RT_PARAM_GROUND_NEAR_Z));
		vec2 ne = min(p_xz - n0, n0 + nsize - p_xz);
		float nw = smoothstep(0.0, 1.0, min(ne.x, ne.y));
		if (nw > 0.0) {
			g = mix(g, textureLod(sampler2D(bindless_textures[nonuniformEXT(nmap)], SAMPLER_LINEAR_CLAMP), (p_xz - n0) / nsize, 0.0), nw);
		}
	}
	return g;
}
// The deformation's own relief at p_xz (m, up; material.cpp ground_deform_relief()): what is laid less what is pressed.
float rt_ground_relief(vec2 p_xz) {
	vec4 g = rt_ground_sample(p_xz);
	return max(g.g, 0.0) - g.r;
}
// The net depression at p_xz (m; the deformation map's R).
float rt_ground_dent(vec2 p_xz) {
	return rt_ground_sample(p_xz).r;
}

float ground_water_between(vec4 p_r, vec4 p_g, vec2 p_f) {
	vec4 wet = step(vec4(1e-7), p_g);
	float n = dot(wet, vec4(1.0));
	if (n == 0.0) {
		return 0.0;
	}
	float fill = dot((p_g - p_r) * wet, vec4(1.0)) / n;
	vec4 dz = mix(vec4(fill) + p_r, p_g, wet);
	return max(mix(mix(dz.w, dz.z, p_f.x), mix(dz.x, dz.y, p_f.x), p_f.y), 0.0);
}
// Standing water over the deformed ground (docs/ideas/mud.md, stage 2), the same lines in material.cpp and
// scene_raytracing_raygen.glsl: (albedo factor, share that is water, water's roughness, 0) at a depth (m) over
// ground with pore water p_wet, the water carrying p_mud kg/m3 of clay (mud round 3: churned up by tyres and feet).
#include "raytracing_rain_inc.glsl"

// The rain a face catches, from the weather (material.cpp weather_surface_rain(): the same maps and lines). p_wp world
// position, p_n world normal, p_air the surface's own motion through the air, negated (world m/s).
vec4 pt_weather_surface_rain(vec3 p_wp, vec3 p_n, vec3 p_air) {
	vec4 wrect = vec4(get_rt_param(RT_PARAM_WETNESS_RECT_X), get_rt_param(RT_PARAM_WETNESS_RECT_Y),
			get_rt_param(RT_PARAM_WETNESS_RECT_W), get_rt_param(RT_PARAM_WETNESS_RECT_H));
	if (wrect.z <= 0.0) {
		return vec4(0.0);
	}
	uint smap = uint(get_rt_param(RT_PARAM_WETNESS_MAP));
	vec2 ht = 0.5 / max(vec2(textureSize(sampler2D(bindless_textures[nonuniformEXT(smap)], SAMPLER_LINEAR_CLAMP), 0)), vec2(1.0));
	vec2 uv = clamp((p_wp.xz - wrect.xy) / wrect.zw, ht, 1.0 - ht);
	vec4 st = textureLod(sampler2D(bindless_textures[nonuniformEXT(smap)], SAMPLER_LINEAR_CLAMP), uv, 0.0);
	float air = get_rt_param(RT_PARAM_PRECIP_TEMPERATURE) - get_rt_param(RT_PARAM_PRECIP_LAPSE) * (p_wp.y - get_rt_param(RT_PARAM_PRECIP_GROUND));
	float rain = max(st.r, 0.0) * rain_liquid_share_nose(air, get_rt_param(RT_PARAM_PRECIP_HUMIDITY), p_wp.y - get_rt_param(RT_PARAM_PRECIP_GROUND),
										 get_rt_param(RT_PARAM_PRECIP_NOSE), get_rt_param(RT_PARAM_PRECIP_NOSE_BASE));
	uint rmap = uint(get_rt_param(RT_PARAM_RAIN_MAP));
	float drip = rmap != 0u ? textureLod(sampler2D(bindless_textures[nonuniformEXT(rmap)], SAMPLER_LINEAR_CLAMP), uv, 0.0).b : 0.0;
	uint wmap = uint(get_rt_param(RT_PARAM_WIND_MAP));
	vec2 wind = (wmap != 0u ? textureLod(sampler2D(bindless_textures[nonuniformEXT(wmap)], SAMPLER_LINEAR_CLAMP), uv, 0.0).rg : vec2(0.0)) + p_air.xz;
	return rain_surface_catch(p_n, rain, drip, wind, st.g);
}

// The water standing on a face (mm over its mean), as material.cpp weather_surface_puddle().
float pt_weather_surface_puddle(vec3 p_wp, vec3 p_n) {
	vec4 wrect = vec4(get_rt_param(RT_PARAM_WETNESS_RECT_X), get_rt_param(RT_PARAM_WETNESS_RECT_Y),
			get_rt_param(RT_PARAM_WETNESS_RECT_W), get_rt_param(RT_PARAM_WETNESS_RECT_H));
	if (wrect.z <= 0.0) {
		return 0.0;
	}
	uint smap = uint(get_rt_param(RT_PARAM_WETNESS_MAP));
	vec2 ht = 0.5 / max(vec2(textureSize(sampler2D(bindless_textures[nonuniformEXT(smap)], SAMPLER_LINEAR_CLAMP), 0)), vec2(1.0));
	vec2 uv = clamp((p_wp.xz - wrect.xy) / wrect.zw, ht, 1.0 - ht);
	return rain_surface_puddle(p_n, textureLod(sampler2D(bindless_textures[nonuniformEXT(smap)], SAMPLER_LINEAR_CLAMP), uv, 0.0).b);
}

vec4 ground_water_look(float p_depth, float p_wet, float p_mud) {
	// A film reads as water once it covers the grains' relief (half a millimetre), whole by 2 mm.
	float cover = smoothstep(0.0003, 0.002, p_depth);
	// Thinner, it already glistens: a seepage sheet a few hundredths of a millimetre deep fills the pores between
	// the top grains and its surface is smooth (Lagarde 2013's accumulated water), though the grains still show.
	// Fully a mirror only from most of a millimetre: a sheet of a tenth of one between rills is wet sand with a
	// sheen, so the water in the channels, deepening across them, reads as the brightest and their banks round off
	// (from 0.3 mm the whole sheet was one mirror, the dry ridges cut out of it as flat-sided steps).
	float gloss = smoothstep(0.00004, 0.0008, p_depth);
	// The bed under it is soaked (Lagarde 2013, porosity 0.6: albedo 0.52), less what its pore water already took.
	float soak = mix(1.0, 0.52, cover * (1.0 - clamp(p_wet, 0.0, 1.0)));
	// Rut water carries clay: it hides the bed within a centimetre or so (Beer-Lambert down and back up, an
	// e-folding depth of 1 cm, water with a few grams of clay a litre) and scatters back a darker soil colour.
	// Stirred, it carries more: the attenuation grows with the clay (Beer-Lambert in the concentration), so at some
	// 40 kg/m3 it hides the bed under a millimetre.
	float murk = 1.0 - exp(-2.0 * p_depth * (3.0 + max(p_mud, 0.0)) / 0.03);
	// Thick with clay it no longer shows the soaked bed darker: it scatters back the clay's own colour (the dry
	// ground's, a little darker: the pore water's darkening, Lagarde's 0.52 with p_wet of it already applied, undone),
	// milky brown, as the slurry it is.
	float slurry = max(p_mud, 0.0) / (max(p_mud, 0.0) + 30.0);
	float bed = soak * mix(1.0, 0.75, murk * cover);
	float clay = 0.8 / mix(1.0, 0.52, clamp(p_wet, 0.0, 1.0));
	return vec4(mix(bed, clay, slurry * murk * cover), cover, 0.03, gloss);
}
// Flowing water (docs/ideas/sand.md, flowing rills): the sheet's surface carries small ripples that the flow
// sweeps downstream, drawn as a flow map does (Vlachos 2010, Water Flow in Portal 2): a ripple pattern advected by
// the water's velocity over a one-second cycle, two copies half a cycle apart cross-faded so neither's reset shows.
// The velocity is the shallow water's own, by Manning (n 0.025, as the GPU's water) from the depth and the surface's
// slope (its normal): downhill, d^(2/3) sqrt(S) / n. The same lines are in material.cpp and
// scene_raytracing_raygen.glsl.
vec2 ground_flow_grad(vec2 p_cell) {
	uvec2 q = uvec2(ivec2(floor(p_cell)) + ivec2(1 << 20));
	uint h = (q.x * 1597334677u) ^ (q.y * 3812015801u);
	h ^= h >> 16u;
	h *= 0x7feb352du;
	h ^= h >> 15u;
	h *= 0x846ca68bu;
	h ^= h >> 16u;
	float a = float(h) * (6.2831853 / 4294967296.0);
	return vec2(cos(a), sin(a));
}
// Gradient noise and its slope, p in cells (quintic fade).
vec3 ground_flow_noise(vec2 p) {
	vec2 i = floor(p);
	vec2 f = p - i;
	vec2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
	vec2 du = 30.0 * f * f * (f * (f - 2.0) + 1.0);
	vec2 ga = ground_flow_grad(i);
	vec2 gb = ground_flow_grad(i + vec2(1.0, 0.0));
	vec2 gc = ground_flow_grad(i + vec2(0.0, 1.0));
	vec2 gd = ground_flow_grad(i + vec2(1.0, 1.0));
	float va = dot(ga, f);
	float vb = dot(gb, f - vec2(1.0, 0.0));
	float vc = dot(gc, f - vec2(0.0, 1.0));
	float vd = dot(gd, f - vec2(1.0, 1.0));
	float k = va - vb - vc + vd;
	float v = va + u.x * (vb - va) + u.y * (vc - va) + u.x * u.y * k;
	vec2 d = ga + u.x * (gb - ga) + u.y * (gc - ga) + u.x * u.y * (ga - gb - gc + gd) + du * (u.yx * k + vec2(vb, vc) - va);
	return vec3(v, d);
}
// The ripples' slope (d/dx, d/dz) on water p_depth deep (m) whose surface normal is p_n (world), at time p_t (s);
// p_fp is the pixel's footprint (m): ripples finer than two pixels fade out rather than alias.
vec2 ground_water_flow(vec2 p_xz, vec3 p_n, float p_depth, float p_t, float p_fp) {
	float s = length(p_n.xz) / max(p_n.y, 0.2);
	float speed = min(pow(max(p_depth, 0.0), 2.0 / 3.0) * sqrt(s) / 0.025, 1.5);
	// Ripples stand up once the sheet runs (a few cm/s); still water stays a mirror.
	float run = smoothstep(0.03, 0.25, speed);
	if (run <= 0.0 || s < 1e-4) {
		return vec2(0.0);
	}
	vec2 dir = p_n.xz / length(p_n.xz);
	vec2 v = dir * speed;
	// Stretched across the flow, as the small transverse ripples on sheet flow are: cells twice as long across.
	vec2 side = vec2(-dir.y, dir.x);
	float ph0 = fract(p_t);
	float ph1 = fract(p_t + 0.5);
	float w0 = 1.0 - abs(2.0 * ph0 - 1.0);
	vec2 slope = vec2(0.0);
	for (int o = 0; o < 2; o++) {
		// Wavelengths 2 cm and 7 mm, heights a twentieth of them.
		float l = o == 0 ? 0.02 : 0.007;
		float fade = 1.0 - smoothstep(0.25 * l, 0.5 * l, p_fp);
		if (fade <= 0.0) {
			continue;
		}
		for (int k = 0; k < 2; k++) {
			float ph = k == 0 ? ph0 : ph1;
			float w = k == 0 ? w0 : 1.0 - w0;
			vec2 q = p_xz - v * ph;
			vec2 c = vec2(dot(q, dir), 0.5 * dot(q, side)) / l + vec2(17.0 * float(k) + 5.0 * float(o), 31.0 * float(k));
			vec3 n = ground_flow_noise(c);
			// Back from the stretched cells to world: d/d(along) = n.y / l, d/d(across) = 0.5 n.z / l.
			slope += w * fade * 0.05 * (dir * n.y + side * 0.5 * n.z);
		}
	}
	return slope * run;
}
// Puddle ice (docs/ideas/mud.md, round 7), the same lines in material.cpp and scene_raytracing_raygen.glsl.
// The Voronoi cells of p (in cells, Worley 1996): x about the distance to the nearest edge (half of F2 - F1), yz
// the nearest site's two hashes (0..1), w the distance to that site.
vec4 ground_ice_cells(vec2 p) {
	vec2 i = floor(p);
	vec2 f = p - i;
	float d1 = 8.0;
	float d2 = 8.0;
	vec2 h1 = vec2(0.0);
	for (int y = -1; y <= 1; y++) {
		for (int x = -1; x <= 1; x++) {
			vec2 c = vec2(float(x), float(y));
			vec2 g = ground_flow_grad(i + c);
			vec2 g2 = ground_flow_grad(i + c + vec2(311.0, 173.0));
			vec2 o = c + 0.5 + 0.4 * g * (0.5 + 0.5 * g2.x) - f;
			float d = dot(o, o);
			if (d < d1) {
				d2 = d1;
				d1 = d;
				h1 = 0.5 + 0.5 * g2;
			} else if (d < d2) {
				d2 = d;
			}
		}
	}
	return vec4(0.5 * (sqrt(d2) - sqrt(d1)), h1, sqrt(d1));
}
// Ice p_h thick (m) over a puddle p_depth deep, p_broken of it broken into plates, seen with a footprint of p_fp (m):
// (the share that is white, the share that is ice, its roughness, 0); r_slope the tilt of a broken plate. A puddle
// freezes from its rim in: there the ice reaches the bed (and the water under it drains or freezes out its air), so
// the rim is milky white with trapped bubbles, the middle clear and dark over the water (black ice). The surface is
// not a mirror: frozen ripples and rime frost it a little. Broken, the ice lies in tilted plates (larger the thicker
// it is) with white fractured edges and open water between them.
vec4 ground_ice_look(vec2 p_xz, float p_h, float p_depth, float p_broken, float p_fp, out vec2 r_slope) {
	r_slope = vec2(0.0);
	float ice = smoothstep(0.0003, 0.001, p_h);
	if (ice <= 0.0) {
		return vec4(0.0);
	}
	// Frozen to the bed (the rim), and a ragged edge to that zone.
	float n = ground_flow_noise(p_xz / 0.12 + vec2(41.0, 7.0)).x;
	float through = smoothstep(0.55, 0.95, p_h / max(p_depth, 1e-4) + 0.3 * n);
	// Bubbles: dots of 1 to 3 mm on a 1.5 cm grid, crowded where it froze through, sparse in the clear ice; past a
	// couple of millimetres a pixel they average out.
	vec4 bc = ground_ice_cells(p_xz / 0.015);
	float br = 0.0007 + 0.0015 * bc.z;
	float dense = mix(0.08, 0.9, through);
	float dot_ = step(bc.y, dense) * (1.0 - smoothstep(br, br + p_fp, bc.w * 0.015));
	float avg = dense * 3.14159 * br * br / (0.015 * 0.015);
	float bubbles = mix(dot_, avg, smoothstep(0.002, 0.006, p_fp));
	float milk = clamp(0.75 * through + 0.6 * bubbles * (1.0 - 0.5 * through) + 0.06, 0.0, 0.9);
	// Frozen ripples: a gentle relief that breaks the mirror.
	vec3 rip = ground_flow_noise(p_xz / 0.05 + vec2(3.0, 19.0));
	r_slope = 0.04 * rip.yz;
	float s = clamp(0.06 + 2.0 * p_h, 0.06, 0.25);
	vec4 pc = ground_ice_cells(p_xz / s);
	// Broken where the load went through (the share past 0.4): the lattice's smooth falloff leaves no stray plates.
	float brk = step(pc.y, (p_broken - 0.4) / 0.6);
	float pe = pc.x * s;
	float gap = 0.003 + 0.004 * pc.z;
	float open = brk * (1.0 - smoothstep(gap, gap + p_fp, pe));
	float rim = brk * (1.0 - smoothstep(gap, gap + 0.012 + p_fp, pe)) * (1.0 - open);
	// Broken plates tip: one edge sinks, the other rises from the water.
	r_slope += brk * (1.0 - open) * 0.35 * vec2(pc.z - 0.5, fract(pc.y * 7.31) - 0.5);
	float cover = ice * (1.0 - open);
	float white = cover * clamp(milk + 0.5 * rim, 0.0, 0.9);
	return vec4(white, cover, mix(0.12, 0.45, white / max(cover, 1e-3)), 0.0);
}
// The crust's tear field at p_xz (in units of 5 cm), warped by noise at two scales so the tears wander and nothing
// repeats.
vec2 ground_needle_q(vec2 p_xz) {
	vec2 w = vec2(ground_flow_noise(p_xz / 0.9 + vec2(13.0, 71.0)).x, ground_flow_noise(p_xz / 0.9 + vec2(97.0, 29.0)).x);
	vec2 w2 = vec2(ground_flow_noise(p_xz / 0.17 + vec2(3.0, 41.0)).x, ground_flow_noise(p_xz / 0.17 + vec2(59.0, 7.0)).x);
	// A finer jitter a centimetre or so across: torn soil breaks jagged, not in smooth curves.
	vec2 w3 = vec2(ground_flow_noise(p_xz / 0.012 + vec2(11.0, 83.0)).x, ground_flow_noise(p_xz / 0.012 + vec2(37.0, 19.0)).x);
	return p_xz / 0.05 + 1.6 * w + 0.5 * w2 + 0.12 * w3;
}
// Needle ice (docs/ideas/mud.md, round 9), the same lines in material.cpp and scene_raytracing_raygen.glsl. Ice
// needles p_h tall (m) stand upright under a crust of soil they lifted (Outcalt 1971; Lawler 1988). The crust stays
// whole over most of the ground, a lumpy skin; in patches it tears open along meandering lines (where it lifted most
// unevenly), and in the tears, lens-shaped and up to about a third of the needles' height wide, the needles show
// side-on: bright upright columns about a millimetre apart, with the crust's dark underside over them. After the
// thaw the crust lies as loose crumbs (p_loose) a few millimetres across. Returns (the share that is needle ice
// showing, the soil's albedo factor, the crumbs' weight for a rough matte surface, 0); r_slope the crust's lumps and
// the crumbs' tilt.
vec4 ground_needle_look(vec2 p_xz, float p_h, float p_loose, float p_fp, out vec2 r_slope) {
	r_slope = vec2(0.0);
	float lift = smoothstep(0.002, 0.015, p_h);
	// The tears: where a warped noise crosses zero (meandering, branching lines, no cells to repeat), its distance
	// from the line its value over its slope; open only in patches about half a metre across.
	vec2 q = ground_needle_q(p_xz);
	vec3 n = ground_flow_noise(q * 0.7);
	float e = abs(n.x) / max(length(n.yz) * 0.7 / 0.05, 1.0);
	float tear = lift * smoothstep(0.05, 0.45, ground_flow_noise(p_xz / 0.45 + vec2(31.0, 11.0)).x);
	// The opening swells and pinches along its line.
	float along = clamp(0.5 + 0.9 * ground_flow_noise(p_xz / 0.03 + vec2(7.0, 3.0)).x, 0.0, 1.0);
	float gw = tear * p_h * (0.05 + 0.35 * along);
	float far_ = smoothstep(0.003, 0.02, p_fp);
	float gap = gw > 1e-5 ? 1.0 - smoothstep(gw, gw + p_fp, e) : 0.0;
	// In the tear the needles stand side-on: columns about a millimetre apart (each its own brightness), averaging
	// out past a millimetre or two a pixel; the outer fifth of the opening is the crust's dark underside.
	vec4 col = ground_ice_cells(p_xz / 0.0012 + vec2(17.0, 5.0));
	float cols = mix(0.35 + 0.65 * smoothstep(0.25, 0.05, col.x) * (0.6 + 0.4 * col.y), 0.6, smoothstep(0.0008, 0.003, p_fp));
	float under = 1.0 - smoothstep(0.6, 0.85, e / max(gw, 1e-5));
	float white = gap * cols * under;
	// Past a few millimetres a pixel it averages out to its share of the area.
	white = mix(white, tear * clamp(0.3 * p_h / 0.05, 0.0, 0.25) * 0.6, far_);
	float shadow = gap * (1.0 - under);
	// The crust's lumps a few centimetres across; beside a tear its edges curl up away from the opening.
	vec3 lump = ground_flow_noise(p_xz / 0.035 + vec2(23.0, 2.0));
	vec2 away = sign(n.x) * normalize(n.yz + 1e-6);
	float edge = tear * (1.0 - gap) * (1.0 - smoothstep(gw, gw + 0.01, e));
	r_slope = lift * (1.0 - gap) * 0.1 * lump.yz - 0.3 * edge * away;
	vec4 k = ground_ice_cells(p_xz / 0.006);
	float fine = 1.0 - smoothstep(0.002, 0.008, p_fp);
	r_slope += p_loose * fine * 0.5 * vec2(k.y - 0.5, k.z - 0.5);
	float shade = mix(1.0, mix(1.0, 0.8 + 0.4 * k.y, fine), p_loose) * (1.0 - 0.7 * clamp(shadow, 0.0, 1.0));
	return vec4(white, shade, p_loose, 0.0);
}

// Sand grains (the functions of the same names in material.cpp; docs/ideas/sand.md, sand glints).
uint ground_grain_hash(ivec3 p_q, uint p_level) {
	uint h = (uint(p_q.x) * 73856093u) ^ (uint(p_q.y) * 19349663u) ^ (uint(p_q.z) * 83492791u) ^ (p_level * 2654435761u);
	h ^= h >> 16u;
	h *= 0x7feb352du;
	h ^= h >> 15u;
	h *= 0x846ca68bu;
	h ^= h >> 16u;
	return h;
}

float ground_grain_layer(vec3 p_pos, float p_footprint, float p_grain, uint p_salt) {
	float lf = max(log2(max(p_footprint, p_grain) / p_grain), 0.0);
	float l0 = floor(lf);
	float v = 0.0;
	for (int k = 0; k < 2; k++) {
		float level = l0 + float(k);
		float cell = p_grain * exp2(level);
		uint h = ground_grain_hash(ivec3(floor(p_pos / cell)), uint(level) + p_salt);
		// The sum of two uniforms: a rounded spread about 0, standard deviation 0.41.
		float s = (float(h & 0xffffu) + float(h >> 16u)) / 65535.0 - 1.0;
		v += (k == 0 ? 1.0 - (lf - l0) : lf - l0) * s / exp2(level);
	}
	return v;
}
// Medium sand (0.25 mm) and the coarse grains, shell bits and dark minerals among it (about 1.2 mm), which are
// what still read as single grains from half a metre.
float ground_grain_speckle(vec3 p_pos, float p_footprint) {
	return 1.0 + 0.8 * ground_grain_layer(p_pos, p_footprint, 0.00025, 0u) + 0.45 * ground_grain_layer(p_pos, p_footprint, 0.0012, 40u);
}

vec4 ground_glint(vec3 p_pos, vec3 p_n, float p_footprint) {
	const float grain = 0.00025;
	const float share = 0.12;
	float level = floor(max(log2(max(p_footprint, grain) / grain), 0.0));
	if (level > 7.0) {
		return vec4(p_n, 0.0);
	}
	float cell = grain * exp2(level);
	uint h = ground_grain_hash(ivec3(floor(p_pos / cell)), uint(level) + 17u);
	float r0 = float(h & 0x3ffu) / 1023.0;
	float r1 = float((h >> 10u) & 0x3ffu) / 1024.0;
	float r2 = float((h >> 20u) & 0x3ffu) / 1023.0;
	float grains = exp2(2.0 * level);
	float chance = min(share * grains, 1.0);
	if (r0 >= chance) {
		return vec4(p_n, 0.0);
	}
	float tan2 = 0.25 * r1 / max(1.0 - r1, 1e-3);
	float ct = inversesqrt(1.0 + tan2);
	float st = sqrt(max(1.0 - ct * ct, 0.0));
	vec3 t = normalize(cross(p_n, abs(p_n.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
	vec3 b = cross(p_n, t);
	float phi = 6.2831853 * r2;
	vec3 f = normalize(p_n * ct + (t * cos(phi) + b * sin(phi)) * st);
	return vec4(f, share / chance / sqrt(max(share * grains, 1.0)));
}

// A cover map read with a cubic B-spline in four bilinear taps, as snow_map_bspline() in the raster shader.
vec4 snow_map_bspline(uint p_map, vec2 p_uv) {
	vec2 size = max(vec2(textureSize(sampler2D(bindless_textures[nonuniformEXT(p_map)], SAMPLER_LINEAR_CLAMP), 0)), vec2(1.0));
	vec2 st = p_uv * size - 0.5;
	vec2 i = floor(st);
	vec2 f = st - i;
	vec2 f2 = f * f;
	vec2 f3 = f2 * f;
	vec2 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0;
	vec2 w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;
	vec2 w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0;
	vec2 w3 = f3 / 6.0;
	vec2 g0 = w0 + w1;
	vec2 g1 = w2 + w3;
	vec2 half_texel = 0.5 / size;
	vec2 t0 = clamp((i + 0.5 - 1.0 + w1 / g0) / size, half_texel, 1.0 - half_texel);
	vec2 t1 = clamp((i + 0.5 + 1.0 + w3 / g1) / size, half_texel, 1.0 - half_texel);
	return g0.y * (g0.x * textureLod(sampler2D(bindless_textures[nonuniformEXT(p_map)], SAMPLER_LINEAR_CLAMP), t0, 0.0) + g1.x * textureLod(sampler2D(bindless_textures[nonuniformEXT(p_map)], SAMPLER_LINEAR_CLAMP), vec2(t1.x, t0.y), 0.0)) +
			g1.y * (g0.x * textureLod(sampler2D(bindless_textures[nonuniformEXT(p_map)], SAMPLER_LINEAR_CLAMP), vec2(t0.x, t1.y), 0.0) + g1.x * textureLod(sampler2D(bindless_textures[nonuniformEXT(p_map)], SAMPLER_LINEAR_CLAMP), t1, 0.0));
}

// Value noise in 3D: the field lies in space, not on the map, so on a steep bank it is not drawn out downhill.
float snow_value_noise(vec3 p_p) {
	vec3 i = floor(p_p);
	vec3 f = fract(p_p);
	f = f * f * (3.0 - 2.0 * f);
	vec2 o = vec2(0.0, 1.0);
	float a = fract(sin(dot(i, vec3(127.1, 311.7, 74.7))) * 43758.5453);
	float b = fract(sin(dot(i + o.yxx, vec3(127.1, 311.7, 74.7))) * 43758.5453);
	float c = fract(sin(dot(i + o.xyx, vec3(127.1, 311.7, 74.7))) * 43758.5453);
	float d = fract(sin(dot(i + o.yyx, vec3(127.1, 311.7, 74.7))) * 43758.5453);
	float e = fract(sin(dot(i + o.xxy, vec3(127.1, 311.7, 74.7))) * 43758.5453);
	float g = fract(sin(dot(i + o.yxy, vec3(127.1, 311.7, 74.7))) * 43758.5453);
	float k = fract(sin(dot(i + o.xyy, vec3(127.1, 311.7, 74.7))) * 43758.5453);
	float l = fract(sin(dot(i + o.yyy, vec3(127.1, 311.7, 74.7))) * 43758.5453);
	return mix(mix(mix(a, b, f.x), mix(c, d, f.x), f.y), mix(mix(e, g, f.x), mix(k, l, f.x), f.y), f.z);
}

// Depth within a cell over the cell's mean, lognormal (CV 0.5, Liston 2004): as snow_subgrid() in raster.
float snow_subgrid(vec3 p_pos) {
	float n = 0.55 * snow_value_noise(p_pos / 9.0) + 0.3 * snow_value_noise(p_pos / 3.5 + 17.0) + 0.15 * snow_value_noise(p_pos / 1.3 + 5.0);
	float z = clamp((n - 0.5) / 0.15, -3.0, 3.0);
	// The thinning fringe: near the melt-out line the last centimetres break up at the scale of stones and
	// tussocks under them, so the edge goes over a metre or so of speckle, not at once.
	float m = 0.6 * snow_value_noise(p_pos / 1.1 + 3.0) + 0.4 * snow_value_noise(p_pos / 0.45 + 9.0);
	float zf = clamp((m - 0.5) / 0.17, -3.0, 3.0);
	return exp(0.47 * z + 0.5 * zf - 0.235);
}

vec4 snow_glint(vec3 p_pos, vec3 p_n, float p_footprint, float p_hoar) {
	// Surface hoar (p_hoar, mm): its crystals are flat ice plates and feathers a few mm to cm across, standing
	// on the snow (Colbeck 1988; Lang et al. 1984), each one a mirror. Over hoar, the cell is the crystal's size,
	// most crystals show a smooth face, and a share hf of the facets are plates set every way, their normals
	// spread evenly over the sky up to 60 degrees off the snow's (past that the coat's grazing Fresnel would only
	// darken the snow under it): far fewer, larger and brighter glints than the grains', from any sun.
	float hf = smoothstep(0.5, 4.0, p_hoar);
	float grain = max(0.002, 0.0006 * p_hoar);
	float share = mix(0.25, 0.35, hf);
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
	float ct;
	if (r0 < chance * hf) {
		ct = mix(0.5, 1.0, r1);
	} else {
		float tan2 = 0.16 * r1 / max(1.0 - r1, 1e-3);
		ct = inversesqrt(1.0 + tan2);
	}
	float st = sqrt(max(1.0 - ct * ct, 0.0));
	vec3 t = normalize(cross(p_n, abs(p_n.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
	vec3 b = cross(p_n, t);
	float phi = 6.2831853 * r2;
	vec3 f = normalize(p_n * ct + (t * cos(phi) + b * sin(phi)) * st);
	return vec4(f, share / chance / sqrt(max(share * grains, 1.0)));
}

#ifdef RT_LSS
// Swept sphere hits read their attributes on the strip triangle at the same place along the segment
// (rt_lss_triangle), and keep where the ray met the round fibre for the normal and the hair's fibre offset.
void rt_lss_resolve() {
	rt_lss_hit = gl_HitIsLSSNV;
	if (!rt_lss_hit) {
		rt_hit_bary = hit_attribs.bary_or_uv;
		rt_hit_primitive = gl_PrimitiveID;
		return;
	}
	vec3 p0 = gl_HitLSSPositionsNV[0];
	vec3 axis = gl_HitLSSPositionsNV[1] - p0;
	vec3 hit_obj = gl_ObjectRayOriginEXT + gl_ObjectRayDirectionEXT * gl_HitTEXT;
	float len2 = dot(axis, axis);
	float u = len2 > 0.0 ? clamp(dot(hit_obj - p0, axis) / len2, 0.0, 1.0) : 0.0;
	rt_hit_primitive = rt_lss_triangle(gl_PrimitiveID, u);
	rt_hit_bary = rt_lss_barycentrics(u);
	rt_lss_offset = mat3(gl_ObjectToWorldEXT) * (hit_obj - (p0 + axis * u));
}
#endif

void main() {
#ifdef RT_LSS
	rt_lss_resolve();
#endif
	if ((payload.packed_bounces_flags & SSS_PROBE_FLAG) != 0u) {
		rt_sss_probe_hit();
		return;
	}
	// The camera cut-out (raygen): a camera ray's surface that opens in the cut, short of where the cut ends, reports
	// its distance unshaded so the ray goes on past it, as raster discards it.
	if (payload.hit_t <= -CUTOUT_PROBE_BASE && payload.hit_t > -CUTOUT_REPORTED && gl_HitTEXT < -payload.hit_t - CUTOUT_PROBE_BASE &&
			rt_material_camera_cutout(rt_entry_material(RT_HIT_GEOMETRY_IDX))) {
		payload.hit_t = -(CUTOUT_REPORTED + gl_HitTEXT);
		return;
	}
	HitData h = compute_hit_data();
	write_primary_hit_depth(h.hit_pos);
	write_primary_hit_velocity(h.hit_pos);

#ifdef RT_CUSTOM_HIT_GROUP
	uint rt_geometry_idx = h.geometry_idx;
	vec3 rt_hit_pos = h.hit_pos;
	vec2 rt_uv = h.uv;
	vec2 rt_uv2 = h.uv2;
	vec4 rt_color = h.color;
	vec3 rt_normal = h.geometry_normal;
	vec3 rt_tangent = h.tangent;
	vec3 rt_bitangent = h.bitangent;
	bool rt_front_face = h.is_front_face;

#include "raytracing_custom_fragment_inc.glsl"

	// Relief drawn without faces shades itself from the sun (SUN_SHADOW with relief_sun_direction()).
	rt_relief_sun_visibility = clamp(sun_shadow, 0.0, 1.0);

	// Build MaterialResult from fragment outputs.
	MaterialResult m;
	m.albedo = albedo;
	m.alpha = alpha;
	m.roughness = roughness;
	m.metalness = metallic;
	m.specular = specular;
	m.emissive = emission * scene_data_block.data.emissive_exposure_normalization;
	m.emission_indirect = emission_indirect;
	m.backlight = backlight;
	m.clearcoat = rt_material_feature(RT_MATERIAL_FEATURE_CLEARCOAT) ? clamp(clearcoat, 0.0, 1.0) : 0.0;
	m.clearcoat_roughness = clamp(clearcoat_roughness, 0.0, 1.0);
	m.transmission = rt_material_feature(RT_MATERIAL_FEATURE_TRANSMISSION) ? clamp(transmission, 0.0, 1.0) * (1.0 - clamp(metallic, 0.0, 1.0)) : 0.0;
	// IOR, ABSORPTION and SCATTERING: the surface bounds a medium (water) that absorbs and scatters
	// per metre; the code carries the extinction, the pixel's rt_medium_image the scattering.
	m.ior = ior >= 1.0 ? ior : 0.0;
	vec3 sigma_s = max(scattering, vec3(0.0));
	vec3 sigma_t = max(absorption, vec3(0.0)) + sigma_s;
	m.medium = rt_medium_code(sigma_t);
	m.medium_scatter = 0u;
	if (max(sigma_s.r, max(sigma_s.g, sigma_s.b)) > 0.0) {
		m.medium |= RT_MEDIUM_CODE_SCATTER;
		m.medium_scatter = rt_medium_scatter_pack(sigma_s / max(sigma_t, vec3(1e-6)), scattering_anisotropy);
	}
	m.subsurface = rt_material_feature(RT_MATERIAL_FEATURE_SUBSURFACE) ? clamp(sss_strength, 0.0, 1.0) : 0.0;
#ifdef SSS_MODE_SKIN
	m.subsurface_skin = true;
#else
	m.subsurface_skin = false;
#endif
	material_result_no_hair(m);
	// view space -> world space
	mat3 view_to_world = mat3(inv_view_matrix);
	m.normal = normalize(view_to_world * normal);
	// The coat lies on NORMAL before the normal map, as raster's geo_normal (snow's glints write it).
	m.coat_normal = m.normal;
	// SPECULAR_NORMAL tilts the base specular alone (snow grain facets); diffuse keeps NORMAL.
	m.specular_normal = dot(specular_normal, specular_normal) > 0.25 ? normalize(view_to_world * specular_normal) : vec3(0.0);

	// The vertex stage may have rewritten TANGENT/BINORMAL (triplanar mapping
	// does), so the shading frame comes from the shader, not the mesh TBN.
	vec3 world_tangent = normalize(view_to_world * tangent);
	vec3 world_bitangent = normalize(view_to_world * binormal);
	// ANISOTROPY: anisotropic GGX on the tangent ANISOTROPY_FLOW turns, as raster's light_compute() (brdf_inc.glsl).
	if (anisotropy > 0.01) {
		vec3 at = normalize(anisotropy_flow.x * world_tangent + anisotropy_flow.y * world_bitangent);
		at -= dot(at, m.normal) * m.normal;
		if (dot(at, at) > 1e-12) {
			m.aniso = vec4(normalize(at), clamp(anisotropy, 0.0, 1.0));
		}
	}

	// Apply normal map if it was written.
	vec3 ts_normal = vec3(0.0, 0.0, 1.0);
	if (normal_map != vec3(0.5, 0.5, 1.0)) {
		ts_normal.xy = normal_map.xy * 2.0 - 1.0;
		ts_normal.z = sqrt(max(0.0, 1.0 - dot(ts_normal.xy, ts_normal.xy)));
		vec3 mapped = world_tangent * ts_normal.x + world_bitangent * ts_normal.y + m.normal * ts_normal.z;
		m.normal = normalize(mix(m.normal, mapped, normal_map_depth));
	}

	// HAIR: fibres along BINORMAL (a shell folds light back out); render_mode hair_cards lets it
	// through, hair_strands also takes the fibre offset across the strip from UV.x (HairMesh layout).
	if (hair > 0.0 && rt_material_feature(RT_MATERIAL_FEATURE_HAIR)) {
		vec2 hair_beta = clamp(hair_roughness, vec2(0.01), vec2(1.0));
		vec3 fibre_sigma_a = all(greaterThanEqual(hair_absorption, vec3(0.0))) ? hair_absorption : hair_sigma_a_from_color(m.albedo, hair_beta.y);
		m.hair = clamp(hair, 0.0, 1.0);
		m.hair_params = vec4(hair_beta, 2.0, 0.0);
		vec3 t = world_bitangent;
#ifdef HAIR_STRANDS
		m.hair_cards = true;
		m.hair_strand = true;
		m.hair_sigma_a = fibre_sigma_a;
		vec3 v_perp = -gl_WorldRayDirectionEXT;
		v_perp -= dot(v_perp, t) * t;
		vec3 across = world_tangent - dot(world_tangent, t) * t;
		if (dot(v_perp, v_perp) > 1e-12 && dot(across, across) > 1e-12) {
			m.hair_params.w = clamp((2.0 * clamp(rt_uv.x, 0.0, 1.0) - 1.0) * dot(normalize(across), cross(normalize(v_perp), t)), -0.999, 0.999);
		}
#ifdef RT_LSS
		// A swept sphere hit is on the round fibre itself: h is where it met it across the view.
		vec3 lss_offset = rt_lss_offset - dot(rt_lss_offset, t) * t;
		if (rt_lss_hit && dot(v_perp, v_perp) > 1e-12 && dot(lss_offset, lss_offset) > 0.0) {
			m.hair_params.w = clamp(dot(normalize(lss_offset), normalize(cross(normalize(v_perp), t))), -0.999, 0.999);
		}
#endif
#else
#ifdef HAIR_CARDS
		m.hair_cards = true;
#endif
		m.hair_sigma_a = hair_mass_sigma_a(hair_color_from_sigma_a(fibre_sigma_a, hair_beta.y));
		t -= dot(t, m.normal) * m.normal;
#endif
		m.hair_tangent = dot(t, t) > 1e-12 ? normalize(t) : world_tangent;
	}

#ifdef RT_DECALS_ENABLED
	if (rt_decals_process(h, m)) {
		return;
	}
#endif

#ifdef RT_DEBUG_ENABLED
	{
		int VIS_MODE = int(get_rt_param(RT_PARAM_VIS_MODE));
		// VIS 23 (transparency heatmap) shades normally; raygen overrides the image.
		if (VIS_MODE != 0 && VIS_MODE != 23) {
			debug_visualize(VIS_MODE, debug_material_inputs(h, m, ts_normal, world_tangent, world_bitangent, h.uv, ao), m.alpha);
			return;
		}
	}
#endif // RT_DEBUG_ENABLED
	shade_and_bounce(h, m);
#else
	// HG0: StandardMaterial3D evaluation.
	MaterialData mat = rt_entry_material(h.geometry_idx);
	MaterialUV muv = material_uv_from_world(mat, geometries[h.geometry_idx], h.uv, h.hit_pos, h.geometry_normal);

	// Triplanar mapping derives its own tangent frame from the blend normal.
	if (muv.triplanar) {
		material_uv_triplanar_tangents(muv, h.tangent, h.bitangent);
	}

	// Normal mapping.
	vec3 tangent_space_normal = vec3(0.0, 0.0, 1.0);
	vec3 final_normal = h.geometry_normal;
	if ((mat.flags & RT_MAT_FLAG_HAS_NORMAL_MAP) != 0u) {
		vec3 normal_sample = material_uv_sample(mat.normal_texture, muv, mat.flags).rgb;
		tangent_space_normal.xy = normal_sample.xy * 2.0 - 1.0;
		tangent_space_normal.z = sqrt(max(0.0, 1.0 - dot(tangent_space_normal.xy, tangent_space_normal.xy)));
		final_normal = apply_normal_map(h, tangent_space_normal, mat.normal_map_depth);
	}

	// Texture sampling.
	vec4 albedo_tex = material_uv_sample(mat.albedo_texture, muv, mat.flags);
	vec3 albedo = albedo_tex.rgb * mat.albedo_color.rgb;
	vec2 roughness_metallic = material_roughness_metallic(mat, muv);
	float roughness = roughness_metallic.x;
	float metalness = roughness_metallic.y;

	vec3 emissive = material_emission(mat, muv, h.uv2, h.hit_pos, h.geometry_normal) * scene_data_block.data.emissive_exposure_normalization;

	// Build MaterialResult.
	MaterialResult m;
	m.albedo = albedo;
	m.alpha = albedo_tex.a * mat.albedo_color.a;
	m.roughness = roughness;
	m.metalness = metalness;
	m.specular = rt_material_surface_params(mat).z;
	m.emissive = emissive;
	m.emission_indirect = rt_material_emission_indirect(mat);
	m.normal = final_normal;
	MaterialLayerTextures layers = material_layer_textures(mat, muv);
	m.backlight = rt_material_feature(RT_MATERIAL_FEATURE_BACK_LIGHTING) ? rt_material_backlight(mat) + layers.backlight : vec3(0.0);
	vec2 coat = rt_material_feature(RT_MATERIAL_FEATURE_CLEARCOAT) ? material_clearcoat(mat, muv) : vec2(0.0);
	m.clearcoat = coat.x;
	m.clearcoat_roughness = coat.y;
	m.transmission = 0.0;
	m.ior = (mat.flags & RT_MAT_FLAG_VOLUME) != 0u ? rt_material_ior(mat) : 0.0;
	m.medium = RT_MEDIUM_CODE_GEOMETRY | h.geometry_idx;
	m.medium_scatter = 0u;
	m.subsurface = rt_material_feature(RT_MATERIAL_FEATURE_SUBSURFACE) ? material_subsurface(mat, muv) : 0.0;
	m.subsurface_skin = rt_material_subsurface(mat).y > 0.5;
	if ((mat.flags & RT_MAT_FLAG_TRANSMISSIVE) != 0u) {
		// Refraction lets through 1 - alpha and draws the surface opaque, as raster.
		float transmission = clamp(rt_material_transmission(mat) * layers.transmission, 0.0, 1.0);
		if ((mat.flags & RT_MAT_FLAG_TRANSMISSION_FROM_ALPHA) != 0u) {
			transmission *= 1.0 - m.alpha;
			m.alpha = 1.0;
		}
		if (rt_material_feature(RT_MATERIAL_FEATURE_TRANSMISSION)) {
			m.transmission = transmission * (1.0 - m.metalness);
		}
	}

	// Hair (BaseMaterial3D): fibre scattering where the mask is set, strands along hair_direction.
	material_result_no_hair(m);
	if (!RT_SCENE_ELIDES(RT_FLAG_SCENE_NO_HAIR) && (mat.flags & RT_MAT_FLAG_HAIR) != 0u) {
		float hair_mask = clamp(material_texture_channel(material_uv_sample(mat.hair_mask_texture, muv, mat.flags), mat.hair_mask_texture), 0.0, 1.0);
		vec3 hp = rt_material_hair(mat);
		vec3 hair_color = m.albedo;
		vec3 fibre_sigma_a = hair_sigma_a_from_color(m.albedo, hp.y);
		if (rt_material_hair_melanin(mat)) {
			fibre_sigma_a = rt_medium_absorption(mat.hair_absorption);
			// The colour melanin gives a mass of hair; the raster and the hair-off path show it too.
			hair_color = hair_color_from_sigma_a(fibre_sigma_a, hp.y);
			m.albedo = mix(m.albedo, hair_color, hair_mask);
		}
		if (rt_material_feature(RT_MATERIAL_FEATURE_HAIR)) {
			m.hair = hair_mask;
		}
		m.hair_params = vec4(hp, 0.0); // h is drawn per path vertex in shade_and_bounce().
		m.hair_cards = rt_material_hair_cards(mat);
		// Strands are real fibres: the path tracer scatters light among them, so they keep their own
		// absorption. Shells and cards stand for the whole mass.
		m.hair_sigma_a = rt_material_hair_direction(mat) == 3u ? fibre_sigma_a : hair_mass_sigma_a(hair_color);
	}
	if (m.hair > 0.0) {
		uint direction = rt_material_hair_direction(mat);
		vec3 t;
		if (direction == 3u) {
			// Strand geometry (HairMesh): crossed strips around each fibre, UV.x across a strip
			// (its tangent), UV.y along the strand (the bitangent). The fibre offset h is the
			// hit's distance from the fibre's axis across the view, over the radius (pbrt's
			// convention: positive toward cross(view, fibre)).
			t = normalize(h.bitangent);
			vec3 v_perp = -gl_WorldRayDirectionEXT;
			v_perp -= dot(v_perp, t) * t;
			vec3 across = h.tangent - dot(h.tangent, t) * t;
			if (dot(v_perp, v_perp) > 1e-12 && dot(across, across) > 1e-12) {
				vec3 y = cross(normalize(v_perp), t);
				m.hair_params.w = clamp((2.0 * clamp(h.uv.x, 0.0, 1.0) - 1.0) * dot(normalize(across), y), -0.999, 0.999);
			}
#ifdef RT_LSS
			// A swept sphere hit is on the round fibre itself: h is where it met it across the view.
			vec3 lss_offset = rt_lss_offset - dot(rt_lss_offset, t) * t;
			if (rt_lss_hit && dot(v_perp, v_perp) > 1e-12 && dot(lss_offset, lss_offset) > 0.0) {
				m.hair_params.w = clamp(dot(normalize(lss_offset), normalize(cross(normalize(v_perp), t))), -0.999, 0.999);
			}
#endif
			m.hair_strand = true;
		} else if (direction == 0u) {
			// Away from the crown, in object space (skinned meshes trace their posed vertices).
			vec3 p_obj = (mat4(gl_WorldToObjectEXT) * vec4(h.hit_pos, 1.0)).xyz;
			t = mat3(gl_ObjectToWorldEXT) * (p_obj - rt_material_hair_crown(mat));
		} else {
			t = direction == 1u ? h.tangent : h.bitangent;
		}
		t -= dot(t, m.normal) * m.normal;
		if (dot(t, t) < 1e-12) {
			t = h.tangent - dot(h.tangent, m.normal) * m.normal;
		}
		m.hair_tangent = dot(t, t) > 1e-12 ? normalize(t) : vec3(1.0, 0.0, 0.0);
	}

	// Turned ground on GroundSurface3D's patch (docs/ideas/ground-deform.md): the raster shader's factors.
	float g_wet = 0.0; // the ground's pore water (sand stage 4), as wetness, and its standing-water sheen
	float g_sheen = 0.0;
	float g_grain = 0.0; // the share of loose dry sand showing its grains (sand_glints)
	float g_water = 0.0; // standing water over the deformed ground, m (mud stage 2)
	float g_mud = 0.0; // the clay it carries, kg/m3 (mud round 3)
	vec4 g_crack = vec4(0.0); // desiccation cracks: opening, plate size m, regularity, crust (mud stage 2b)
	vec4 g_dep = vec4(0.0); // a fresh deposit: cover, linear colour (mud round 13)
	vec4 g_ice = vec4(0.0); // puddle ice: thickness m, broken share (mud round 7); needle ice: height m, crumbs (round 9)
	float g_salt = 0.0; // the salt crust round still water in a dry climate, 0 to 1 (salt_crusts)
	// The terrain round the patch reads the pore water too (the far lattice), with no grains.
	if (rt_material_ground_deform(mat) || rt_material_ground_terrain(mat)) {
		uint wmap_g = uint(get_rt_param(RT_PARAM_GROUND_WET_MAP));
		if (wmap_g != 0u) {
			// The ground_wet_at() lines of material.cpp: the fine lattice's rows, the far lattice's rows, then a
			// row with their rects; the fine block fades into the far one by its weight (A).
			ivec2 gts = textureSize(sampler2D(bindless_textures[nonuniformEXT(wmap_g)], SAMPLER_NEAREST_CLAMP), 0);
			vec4 grect = texelFetch(sampler2D(bindless_textures[nonuniformEXT(wmap_g)], SAMPLER_NEAREST_CLAMP), ivec2(0, gts.y - 1), 0);
			vec4 frect = gts.x > 1 ? texelFetch(sampler2D(bindless_textures[nonuniformEXT(wmap_g)], SAMPLER_NEAREST_CLAMP), ivec2(1, gts.y - 1), 0) : vec4(0.0);
			bool gfar = abs(frect.w - 3.5) < 0.01;
			// The crack block (mud stage 2b) follows the far one: texel 2's tag 4.5.
			bool gcrk = gts.x > 2 && abs(texelFetch(sampler2D(bindless_textures[nonuniformEXT(wmap_g)], SAMPLER_NEAREST_CLAMP), ivec2(2, gts.y - 1), 0).w - 4.5) < 0.01;
			// The puddle ice (mud round 7) after them: texel 3's tag 5.5; then the mud on plants, texel 4's 6.5
			// (material.cpp ground_wet_blocks()).
			bool gice = gts.x > 3 && abs(texelFetch(sampler2D(bindless_textures[nonuniformEXT(wmap_g)], SAMPLER_NEAREST_CLAMP), ivec2(3, gts.y - 1), 0).w - 5.5) < 0.01;
			bool gpmud = gts.x > 4 && abs(texelFetch(sampler2D(bindless_textures[nonuniformEXT(wmap_g)], SAMPLER_NEAREST_CLAMP), ivec2(4, gts.y - 1), 0).w - 6.5) < 0.01;
			// Then a fresh deposit's block, texel 5's 7.5 (material.cpp ground_deposit_state()).
			bool gdepb = gts.x > 5 && abs(texelFetch(sampler2D(bindless_textures[nonuniformEXT(wmap_g)], SAMPLER_NEAREST_CLAMP), ivec2(5, gts.y - 1), 0).w - 7.5) < 0.01;
			// The salt crust last: texel 6's 8.5.
			bool gsalt = gts.x > 6 && abs(texelFetch(sampler2D(bindless_textures[nonuniformEXT(wmap_g)], SAMPLER_NEAREST_CLAMP), ivec2(6, gts.y - 1), 0).w - 8.5) < 0.01;
			int gblocks = 1 + (gfar ? 1 : 0) + (gcrk ? 1 : 0) + (gice ? 1 : 0) + (gpmud ? 1 : 0) + (gdepb ? 1 : 0) + (gsalt ? 1 : 0);
			float grows = float((gts.y - 1) / gblocks);
			vec2 gxz = (h.hit_pos + rt_origin.xyz).xz;
			if (abs(grect.w - 2.5) < 0.01) {
				vec4 gwn = vec4(0.0);
				vec4 gwf = vec4(0.0);
				for (int gb = 0; gb < (gfar ? 2 : 1); gb++) {
					vec4 rr = gb == 0 ? grect : frect;
					vec2 guv = (gxz - rr.xy) / max(rr.z, 1e-3);
					if (rr.z > 0.0 && all(greaterThanEqual(guv, vec2(0.0))) && all(lessThanEqual(guv, vec2(1.0)))) {
						float gv = clamp(guv.y * grows, 0.5, grows - 0.5) + float(gb) * grows;
						vec4 s = textureLod(sampler2D(bindless_textures[nonuniformEXT(wmap_g)], SAMPLER_LINEAR_CLAMP), vec2(guv.x, gv / float(gts.y)), 0.0);
						if (gb == 0) {
							gwn = s;
						} else {
							gwf = s;
						}
					}
				}
				vec3 gw = vec3(mix(gwf.xy, gwn.xy, gwn.w), gwn.z * gwn.w);
				if (gcrk && rt_material_ground_deform(mat)) {
					vec2 guv = (gxz - grect.xy) / max(grect.z, 1e-3);
					if (all(greaterThanEqual(guv, vec2(0.0))) && all(lessThanEqual(guv, vec2(1.0)))) {
						float gv = clamp(guv.y * grows, 0.5, grows - 0.5) + float(gfar ? 2 : 1) * grows;
						g_crack = textureLod(sampler2D(bindless_textures[nonuniformEXT(wmap_g)], SAMPLER_LINEAR_CLAMP), vec2(guv.x, gv / float(gts.y)), 0.0);
					}
				}
				if (gice && rt_material_ground_deform(mat)) {
					vec2 guv = (gxz - grect.xy) / max(grect.z, 1e-3);
					if (all(greaterThanEqual(guv, vec2(0.0))) && all(lessThanEqual(guv, vec2(1.0)))) {
						float gv = clamp(guv.y * grows, 0.5, grows - 0.5) + float(1 + (gfar ? 1 : 0) + (gcrk ? 1 : 0)) * grows;
						g_ice = textureLod(sampler2D(bindless_textures[nonuniformEXT(wmap_g)], SAMPLER_LINEAR_CLAMP), vec2(guv.x, gv / float(gts.y)), 0.0);
					}
				}
				if (gdepb) {
					vec2 guv = (gxz - grect.xy) / max(grect.z, 1e-3);
					if (all(greaterThanEqual(guv, vec2(0.0))) && all(lessThanEqual(guv, vec2(1.0)))) {
						float gv = clamp(guv.y * grows, 0.5, grows - 0.5) + float(gblocks - 1 - (gsalt ? 1 : 0)) * grows;
						g_dep = textureLod(sampler2D(bindless_textures[nonuniformEXT(wmap_g)], SAMPLER_LINEAR_CLAMP), vec2(guv.x, gv / float(gts.y)), 0.0);
					}
				}
				if (gsalt) {
					vec2 guv = (gxz - grect.xy) / max(grect.z, 1e-3);
					if (all(greaterThanEqual(guv, vec2(0.0))) && all(lessThanEqual(guv, vec2(1.0)))) {
						float gv = clamp(guv.y * grows, 0.5, grows - 0.5) + float(gblocks - 1) * grows;
						g_salt = textureLod(sampler2D(bindless_textures[nonuniformEXT(wmap_g)], SAMPLER_LINEAR_CLAMP), vec2(guv.x, gv / float(gts.y)), 0.0).x;
					}
				}
				g_grain = clamp(gw.z, 0.0, 1.0);
				float gup = smoothstep(-0.6, 0.2, h.geometry_normal.y);
				g_wet = clamp(gw.x, 0.0, 1.0) * gup;
				g_sheen = smoothstep(0.95, 1.0, gw.y) * smoothstep(0.35, 0.7, h.geometry_normal.y); // material.cpp ground_wet_at()
				if (!rt_material_ground_deform(mat)) {
					g_grain = 0.0;
				}
			}
		}
	}
	if (rt_material_ground_deform(mat)) {
		uint gmap = uint(get_rt_param(RT_PARAM_GROUND_DEFORM_MAP));
		if (gmap != 0u) {
			vec3 gpos = h.hit_pos + rt_origin.xyz;
			vec2 guv = (gpos.xz - vec2(get_rt_param(RT_PARAM_GROUND_DEFORM_X), get_rt_param(RT_PARAM_GROUND_DEFORM_Z))) / max(get_rt_param(RT_PARAM_GROUND_DEFORM_SIZE), 1e-3);
			if (all(greaterThanEqual(guv, vec2(0.0))) && all(lessThanEqual(guv, vec2(1.0)))) {
				vec4 g = textureLod(sampler2D(bindless_textures[nonuniformEXT(gmap)], SAMPLER_LINEAR_CLAMP), guv, 0.0);
				vec2 gwf2 = fract(guv * vec2(textureSize(sampler2D(bindless_textures[nonuniformEXT(gmap)], SAMPLER_LINEAR_CLAMP), 0)) - 0.5);
				float gwd = ground_water_between(textureGather(sampler2D(bindless_textures[nonuniformEXT(gmap)], SAMPLER_LINEAR_CLAMP), guv, 0),
						textureGather(sampler2D(bindless_textures[nonuniformEXT(gmap)], SAMPLER_LINEAR_CLAMP), guv, 1), gwf2);
				// The finer near field inside its square (material.cpp ground_deform_near_weight()).
				uint nmap = uint(get_rt_param(RT_PARAM_GROUND_NEAR_MAP));
				float nsize = get_rt_param(RT_PARAM_GROUND_NEAR_SIZE);
				if (nmap != 0u && nsize > 0.0) {
					vec2 n0 = vec2(get_rt_param(RT_PARAM_GROUND_NEAR_X), get_rt_param(RT_PARAM_GROUND_NEAR_Z));
					vec2 ne = min(gpos.xz - n0, n0 + nsize - gpos.xz);
					float nw = smoothstep(0.0, 1.0, min(ne.x, ne.y));
					if (nw > 0.0) {
						vec2 nuv = (gpos.xz - n0) / nsize;
						g = mix(g, textureLod(sampler2D(bindless_textures[nonuniformEXT(nmap)], SAMPLER_LINEAR_CLAMP), nuv, 0.0), nw);
						vec2 nwf = fract(nuv * vec2(textureSize(sampler2D(bindless_textures[nonuniformEXT(nmap)], SAMPLER_LINEAR_CLAMP), 0)) - 0.5);
						gwd = mix(gwd, ground_water_between(textureGather(sampler2D(bindless_textures[nonuniformEXT(nmap)], SAMPLER_LINEAR_CLAMP), nuv, 0),
								textureGather(sampler2D(bindless_textures[nonuniformEXT(nmap)], SAMPLER_LINEAR_CLAMP), nuv, 1), nwf), nw);
					}
				}
				g_water = gwd; // material.cpp ground_deform_water()
				// A < 0: muddy water, its clay in place of the turned ground's gloss (material.cpp ground_deform_tint()).
				g_mud = 400.0 * max(-g.a, 0.0);
				m.albedo *= g.b;
				// The field's millimetre relief, finer than the patch's vertices carry (the bed's runoff roughness, a
				// swash mark's lip): the material.cpp unsharp mask on the field's heights a texel and a half round, as
				// raster draws it. Traced, the vertices smoothed it away and grazing sand read flat beside raster.
				{
					float gtex = get_rt_param(RT_PARAM_GROUND_DEFORM_SIZE) / float(max(textureSize(sampler2D(bindless_textures[nonuniformEXT(gmap)], SAMPLER_LINEAR_CLAMP), 0).x, 1));
					if (nmap != 0u && nsize > 0.0) {
						vec2 n0r = vec2(get_rt_param(RT_PARAM_GROUND_NEAR_X), get_rt_param(RT_PARAM_GROUND_NEAR_Z));
						vec2 ner = min(gpos.xz - n0r, n0r + nsize - gpos.xz);
						float ntex = nsize / float(max(textureSize(sampler2D(bindless_textures[nonuniformEXT(nmap)], SAMPLER_LINEAR_CLAMP), 0).x, 1));
						gtex = mix(gtex, ntex, smoothstep(0.0, 1.0, min(ner.x, ner.y)));
					}
					float gde = 1.5 * max(gtex, 1e-3);
					float gfp = rt_cone_footprint_log2 > RT_TEXTURE_LOD_BASE ? exp2(rt_cone_footprint_log2) : 0.0;
					float gdk = 1.0 - smoothstep(0.5 * gde, 1.5 * gde, gfp);
					if (gdk > 0.0) {
						float gh0 = rt_ground_relief(gpos.xz);
						float gh1 = rt_ground_relief(gpos.xz + vec2(gde, 0.0));
						float gh2 = rt_ground_relief(gpos.xz - vec2(gde, 0.0));
						float gh3 = rt_ground_relief(gpos.xz + vec2(0.0, gde));
						float gh4 = rt_ground_relief(gpos.xz - vec2(0.0, gde));
						float ghm = 0.25 * (gh1 + gh2 + gh3 + gh4);
						float gspan = max(max(max(gh1, gh2), max(gh3, gh4)), gh0) - min(min(min(gh1, gh2), min(gh3, gh4)), gh0);
						vec2 gdr = min(guv, 1.0 - guv) * get_rt_param(RT_PARAM_GROUND_DEFORM_SIZE);
						float gcv = clamp((gh0 - ghm) / 0.0015, -1.0, 1.0) * gdk * (1.0 - smoothstep(0.004, 0.01, gspan)) * smoothstep(0.0, 0.2 * get_rt_param(RT_PARAM_GROUND_DEFORM_SIZE), min(gdr.x, gdr.y));
						m.albedo *= 1.0 + (gcv > 0.0 ? 0.35 : 0.4) * gcv;
					}
				}
				// The fresh sheen on floors only (material.cpp: torn walls keep their roughness).
				m.roughness = clamp(m.roughness * mix(1.0, g.a < 0.0 ? 1.0 : g.a, smoothstep(0.5, 0.85, h.geometry_normal.y)), 0.0, 1.0);
			}
		}
	}
	// Far ripples (sand stage 3): the material.cpp ground_ripple_far_slope() lines on the ripple map, with
	// texelFetch in place of the raster's mipmapped copy (the samples average what mips would); on the patch and,
	// past it, over the dune field (GroundSurface3D far_ripples) on the terrain round it.
	if (rt_material_ground_deform(mat) || rt_material_ground_terrain(mat)) {
		uint rmap = uint(get_rt_param(RT_PARAM_GROUND_RIPPLE_MAP));
		if (rmap != 0u) {
			ivec2 ts = textureSize(sampler2D(bindless_textures[nonuniformEXT(rmap)], SAMPLER_NEAREST_CLAMP), 0);
			vec4 m0 = texelFetch(sampler2D(bindless_textures[nonuniformEXT(rmap)], SAMPLER_NEAREST_CLAMP), ivec2(0, ts.y - 1), 0);
			vec4 m1 = texelFetch(sampler2D(bindless_textures[nonuniformEXT(rmap)], SAMPLER_NEAREST_CLAMP), ivec2(1, ts.y - 1), 0);
			vec4 m2 = texelFetch(sampler2D(bindless_textures[nonuniformEXT(rmap)], SAMPLER_NEAREST_CLAMP), ivec2(2, ts.y - 1), 0);
			vec4 m3 = texelFetch(sampler2D(bindless_textures[nonuniformEXT(rmap)], SAMPLER_NEAREST_CLAMP), ivec2(3, ts.y - 1), 0);
			vec2 p = (h.hit_pos + rt_origin.xyz).xz;
			int rn = int(m2.y + 0.5);
			int sn = int(m2.z + 0.5);
			// The dune field's colour at this scale (GroundSurface3D dune_tone; material.cpp ground_sand_at(), the same
			// lines): the generator's interdune share and the sand share from the sand block (G, B); past it, the depth rows
			// round the patch (R, faded over their outer 15%) and the ground's share.
			vec4 m4 = ts.x >= 5 ? texelFetch(sampler2D(bindless_textures[nonuniformEXT(rmap)], SAMPLER_NEAREST_CLAMP), ivec2(4, ts.y - 1), 0) : vec4(0.0);
			if (m0.z > 0.0 && m4.x > 0.5) {
				vec4 gsa = vec4(-1.0, m3.z, 0.0, 1.0);
				vec2 su = (p - m1.xy) / max(m1.z, 1e-4) * float(sn) - 0.5;
				int dn = ts.y - 1 - (rn + sn + rn);
				vec2 du = (p - m4.yz) / max(m4.w, 1e-4) * float(dn) - 0.5;
				if (sn > 0 && all(greaterThanEqual(su, vec2(-0.5))) && all(lessThanEqual(su, vec2(float(sn) - 0.5)))) {
					ivec2 s0 = clamp(ivec2(floor(su)), ivec2(0), ivec2(sn - 1));
					ivec2 s1 = clamp(s0 + 1, ivec2(0), ivec2(sn - 1));
					vec2 sf = clamp(su - floor(su), 0.0, 1.0);
					vec3 ga = texelFetch(sampler2D(bindless_textures[nonuniformEXT(rmap)], SAMPLER_NEAREST_CLAMP), ivec2(s0.x, rn + s0.y), 0).gba;
					vec3 gb = texelFetch(sampler2D(bindless_textures[nonuniformEXT(rmap)], SAMPLER_NEAREST_CLAMP), ivec2(s1.x, rn + s0.y), 0).gba;
					vec3 gc = texelFetch(sampler2D(bindless_textures[nonuniformEXT(rmap)], SAMPLER_NEAREST_CLAMP), ivec2(s0.x, rn + s1.y), 0).gba;
					vec3 gd = texelFetch(sampler2D(bindless_textures[nonuniformEXT(rmap)], SAMPLER_NEAREST_CLAMP), ivec2(s1.x, rn + s1.y), 0).gba;
					vec3 r = mix(mix(ga, gb, sf.x), mix(gc, gd, sf.x), sf.y);
					vec2 sbe = min(p - m1.xy, m1.xy + m1.z - p);
					r.z = mix(1.0, r.z, smoothstep(0.0, 4.0, min(sbe.x, sbe.y)));
					gsa = min(min(ga.x, gb.x), min(gc.x, gd.x)) < 0.0 ? vec4(-1.0, r.y, 0.0, r.z) : vec4(r.xy, 1.0, r.z);
				} else if (dn > 0 && all(greaterThanEqual(du, vec2(-0.5))) && all(lessThanEqual(du, vec2(float(dn) - 0.5)))) {
					int r0 = rn + sn + rn;
					ivec2 d0 = clamp(ivec2(floor(du)), ivec2(0), ivec2(dn - 1));
					ivec2 d1 = clamp(d0 + 1, ivec2(0), ivec2(dn - 1));
					vec2 df = clamp(du - floor(du), 0.0, 1.0);
					vec4 q = vec4(texelFetch(sampler2D(bindless_textures[nonuniformEXT(rmap)], SAMPLER_NEAREST_CLAMP), ivec2(d0.x, r0 + d0.y), 0).r,
							texelFetch(sampler2D(bindless_textures[nonuniformEXT(rmap)], SAMPLER_NEAREST_CLAMP), ivec2(d1.x, r0 + d0.y), 0).r,
							texelFetch(sampler2D(bindless_textures[nonuniformEXT(rmap)], SAMPLER_NEAREST_CLAMP), ivec2(d0.x, r0 + d1.y), 0).r,
							texelFetch(sampler2D(bindless_textures[nonuniformEXT(rmap)], SAMPLER_NEAREST_CLAMP), ivec2(d1.x, r0 + d1.y), 0).r);
					if (min(min(q.x, q.y), min(q.z, q.w)) >= 0.0) {
						vec2 de = min(p - m4.yz, m4.yz + m4.w - p);
						gsa = vec4(mix(mix(q.x, q.y, df.x), mix(q.z, q.w, df.x), df.y), m3.z, smoothstep(0.0, 0.15 * m4.w, min(de.x, de.y)), 1.0);
					}
				}
				if (gsa.y > 0.0) {
					float gny = h.geometry_normal.y;
					vec3 gdt = mix(ground_dune_tone(p, gny, -1.0), ground_dune_tone(p, gny, gsa.x), gsa.z);
					m.albedo *= mix(vec3(1.0), gdt, gsa.y);
					// Granule ripples on the interdune lag (material.cpp, the same lines).
					vec4 m5 = ts.x >= 6 ? texelFetch(sampler2D(bindless_textures[nonuniformEXT(rmap)], SAMPLER_NEAREST_CLAMP), ivec2(5, ts.y - 1), 0) : vec4(0.0);
					// Dust devil tracks (material.cpp ground_sand_scour_at() and ground_scour_tone(), the same lines): the
					// share swept, from the tile rows' G over the sand block's rect.
					if (m5.w > 0.5 && sn > 0 && rn >= sn && all(greaterThanEqual(su, vec2(-0.5))) && all(lessThanEqual(su, vec2(float(sn) - 0.5)))) {
						ivec2 c0 = clamp(ivec2(floor(su)), ivec2(0), ivec2(sn - 1));
						ivec2 c1 = clamp(c0 + 1, ivec2(0), ivec2(sn - 1));
						vec2 cf = clamp(su - floor(su), 0.0, 1.0);
						int cr = rn + sn;
						float sc = mix(mix(texelFetch(sampler2D(bindless_textures[nonuniformEXT(rmap)], SAMPLER_NEAREST_CLAMP), ivec2(c0.x, cr + c0.y), 0).g,
											   texelFetch(sampler2D(bindless_textures[nonuniformEXT(rmap)], SAMPLER_NEAREST_CLAMP), ivec2(c1.x, cr + c0.y), 0).g, cf.x),
								mix(texelFetch(sampler2D(bindless_textures[nonuniformEXT(rmap)], SAMPLER_NEAREST_CLAMP), ivec2(c0.x, cr + c1.y), 0).g,
										texelFetch(sampler2D(bindless_textures[nonuniformEXT(rmap)], SAMPLER_NEAREST_CLAMP), ivec2(c1.x, cr + c1.y), 0).g, cf.x),
								cf.y);
						m.albedo *= mix(vec3(1.0), mix(vec3(1.0), vec3(0.9, 0.87, 0.82), clamp(sc, 0.0, 1.0)), gsa.y);
					}
					if (m5.z > 0.5) {
						vec3 ggt;
						vec2 ggs = ground_granule_ripples(p, gsa.y * gsa.z * gsa.w * ground_dune_lag(p, gny, gsa.x), m5.xy, rt_cone_width, ggt);
						m.albedo *= ggt;
						if (ggs != vec2(0.0)) {
							m.normal = normalize(m.normal - vec3(ggs.x, 0.0, ggs.y));
						}
					}
					// Rain pits (material.cpp, the same lines).
					vec4 m6 = ts.x >= 7 ? texelFetch(sampler2D(bindless_textures[nonuniformEXT(rmap)], SAMPLER_NEAREST_CLAMP), ivec2(6, ts.y - 1), 0) : vec4(0.0);
					if (m6.x > 0.0) {
						float gpt;
						vec2 gps = ground_rain_pits(p, m6.x * gsa.y * (1.0 - smoothstep(0.003, 0.008, abs(rt_ground_relief(p)))), m6.z, rt_cone_width, gpt);
						m.albedo *= gpt;
						if (gps != vec2(0.0)) {
							m.normal = normalize(m.normal - vec3(gps.x, 0.0, gps.y));
						}
					}
					// Adhesion warts (material.cpp, the same lines).
					vec4 m7 = ts.x >= 8 ? texelFetch(sampler2D(bindless_textures[nonuniformEXT(rmap)], SAMPLER_NEAREST_CLAMP), ivec2(7, ts.y - 1), 0) : vec4(0.0);
					if (m7.z > 0.0) {
						float gdamp = smoothstep(0.15, 0.3, g_wet);
						float gcap;
						vec2 gas = ground_adhesion_warts(p, m7.z * gdamp * gsa.y, m7.xy, rt_cone_width, gcap);
						if (gas != vec2(0.0)) {
							m.normal = normalize(m.normal - vec3(gas.x, 0.0, gas.y));
						}
						g_wet *= 1.0 - 0.8 * gcap;
					}
				}
			}
			if (m2.x > 0.5 && rn > 0) {
				float far = m2.w > 0.5 ? m3.z : 0.0;
				float sand = far;
				vec2 su = (p - m1.xy) / max(m1.z, 1e-4) * float(sn) - 0.5;
				if (all(greaterThanEqual(su, vec2(-0.5))) && all(lessThanEqual(su, vec2(float(sn) - 0.5)))) {
					ivec2 s0 = clamp(ivec2(floor(su)), ivec2(0), ivec2(sn - 1));
					ivec2 s1 = clamp(s0 + 1, ivec2(0), ivec2(sn - 1));
					vec2 sf = clamp(su - floor(su), 0.0, 1.0);
					float block = mix(mix(texelFetch(sampler2D(bindless_textures[nonuniformEXT(rmap)], SAMPLER_NEAREST_CLAMP), ivec2(s0.x, rn + s0.y), 0).r,
											  texelFetch(sampler2D(bindless_textures[nonuniformEXT(rmap)], SAMPLER_NEAREST_CLAMP), ivec2(s1.x, rn + s0.y), 0).r, sf.x),
							mix(texelFetch(sampler2D(bindless_textures[nonuniformEXT(rmap)], SAMPLER_NEAREST_CLAMP), ivec2(s0.x, rn + s1.y), 0).r,
									texelFetch(sampler2D(bindless_textures[nonuniformEXT(rmap)], SAMPLER_NEAREST_CLAMP), ivec2(s1.x, rn + s1.y), 0).r, sf.x),
							sf.y);
					vec2 be = min(p - m1.xy, m1.xy + m1.z - p);
					sand = mix(far, block, m2.w > 0.5 ? smoothstep(0.0, 4.0, min(be.x, be.y)) : 1.0);
				}
				vec2 wuv = (p - m0.xy) / m0.z;
				vec2 e = min(wuv, 1.0 - wuv) * m0.z;
				float live = min(e.x, e.y) > 0.0 ? smoothstep(m0.w, 0.15 * m0.z, min(e.x, e.y)) : 0.0;
				// None on slip faces; faded where a footprint spans fewer than three crests (material.cpp).
				float lam = dot(m3.xy, m3.xy) > 0.0 ? 6.2831853 / length(m3.xy) : 0.11;
				float rfp = rt_cone_width;
				float w = sand * (1.0 - live) * smoothstep(0.85, 0.91, h.geometry_normal.y);
				float resolved = 1.0 - smoothstep(0.22 * lam, 0.4 * lam, rfp);
				vec3 vr = w > 0.0 ? (resolved > 0.0 ? ground_ripple_vary(p, m3.xy) : vec3(0.0, 0.0, ground_ripple_amp(p))) : vec3(0.0);
				// Crests finer than the pixel shade it as a patchy tone (material.cpp ground_ripple_far_slope()).
				m.albedo *= 1.0 - 0.15 * w * vr.z * (1.0 - resolved);
				// Grain sorting (GroundSurface3D ripple_sorting; m3.w the grown field's height spread, 0 when off):
				// the window's ripples (rows 0..n-1, height times share, faded as the raster's) and the far ones (rows
				// n+S..n+S+n-1, the grown field's height in spreads).
				if (m3.w > 0.0 && live > 0.0) {
					vec2 wt = wuv * float(rn) - 0.5;
					ivec2 w0 = clamp(ivec2(floor(wt)), ivec2(0), ivec2(rn - 1));
					ivec2 w1 = clamp(w0 + 1, ivec2(0), ivec2(rn - 1));
					vec2 wf = clamp(wt - floor(wt), 0.0, 1.0);
					vec2 r00 = texelFetch(sampler2D(bindless_textures[nonuniformEXT(rmap)], SAMPLER_NEAREST_CLAMP), w0, 0).rg;
					vec2 r10 = texelFetch(sampler2D(bindless_textures[nonuniformEXT(rmap)], SAMPLER_NEAREST_CLAMP), ivec2(w1.x, w0.y), 0).rg;
					vec2 r01 = texelFetch(sampler2D(bindless_textures[nonuniformEXT(rmap)], SAMPLER_NEAREST_CLAMP), ivec2(w0.x, w1.y), 0).rg;
					vec2 r11 = texelFetch(sampler2D(bindless_textures[nonuniformEXT(rmap)], SAMPLER_NEAREST_CLAMP), w1, 0).rg;
					float wh = mix(mix(r00.x * r00.y, r10.x * r10.y, wf.x), mix(r01.x * r01.y, r11.x * r11.y, wf.x), wf.y) * live;
					m.albedo *= ground_ripple_sort_tint(wh / m3.w, p, resolved, 0.0);
				}
				if (w > 0.0) {
					// The tile: cell c's centre at c * cell, its texel (c mod n), bilinear with wrap.
					vec2 tc = (p + vr.xy) / m0.w;
					ivec2 c0 = ivec2(floor(tc));
					vec2 cf = tc - vec2(c0);
					ivec2 a = ((c0 % rn) + rn) % rn;
					ivec2 b = (a + 1) % rn;
					if (m3.w > 0.0) {
						int hr = rn + sn;
						float th = mix(mix(texelFetch(sampler2D(bindless_textures[nonuniformEXT(rmap)], SAMPLER_NEAREST_CLAMP), ivec2(a.x, hr + a.y), 0).r,
												 texelFetch(sampler2D(bindless_textures[nonuniformEXT(rmap)], SAMPLER_NEAREST_CLAMP), ivec2(b.x, hr + a.y), 0).r, cf.x),
								mix(texelFetch(sampler2D(bindless_textures[nonuniformEXT(rmap)], SAMPLER_NEAREST_CLAMP), ivec2(a.x, hr + b.y), 0).r,
										texelFetch(sampler2D(bindless_textures[nonuniformEXT(rmap)], SAMPLER_NEAREST_CLAMP), ivec2(b.x, hr + b.y), 0).r, cf.x),
								cf.y);
						m.albedo *= mix(vec3(1.0), ground_ripple_sort_tint(vr.z * th, p, resolved, vr.z), w);
					}
				}
				w *= resolved;
				if (w > 0.0) {
					vec2 tc = (p + vr.xy) / m0.w;
					ivec2 c0 = ivec2(floor(tc));
					vec2 cf = tc - vec2(c0);
					ivec2 a = ((c0 % rn) + rn) % rn;
					ivec2 b = (a + 1) % rn;
					vec2 sl = mix(mix(texelFetch(sampler2D(bindless_textures[nonuniformEXT(rmap)], SAMPLER_NEAREST_CLAMP), ivec2(a.x, a.y), 0).ba,
										  texelFetch(sampler2D(bindless_textures[nonuniformEXT(rmap)], SAMPLER_NEAREST_CLAMP), ivec2(b.x, a.y), 0).ba, cf.x),
							mix(texelFetch(sampler2D(bindless_textures[nonuniformEXT(rmap)], SAMPLER_NEAREST_CLAMP), ivec2(a.x, b.y), 0).ba,
									texelFetch(sampler2D(bindless_textures[nonuniformEXT(rmap)], SAMPLER_NEAREST_CLAMP), ivec2(b.x, b.y), 0).ba, cf.x),
							cf.y) * w * vr.z;
					m.normal = normalize(m.normal - vec3(sl.x, 0.0, sl.y));
				}
			}
		}
	}

	// Wetness (BaseMaterial3D): Lagarde 2013, the same lines and inputs as the raster shader.
	vec4 wetness = rt_material_wetness(mat);
	if (wetness.z > 0.5) {
		float wet = wetness.y;
		if (wetness.w > 0.5) {
			vec4 wrect = vec4(get_rt_param(RT_PARAM_WETNESS_RECT_X), get_rt_param(RT_PARAM_WETNESS_RECT_Y),
					get_rt_param(RT_PARAM_WETNESS_RECT_W), get_rt_param(RT_PARAM_WETNESS_RECT_H));
			if (wrect.z > 0.0) {
				uint wmap = uint(get_rt_param(RT_PARAM_WETNESS_MAP));
				vec2 wsize = vec2(textureSize(sampler2D(bindless_textures[nonuniformEXT(wmap)], SAMPLER_LINEAR_CLAMP), 0));
				vec2 half_texel = 0.5 / max(wsize, vec2(1.0));
				vec3 wpos = h.hit_pos + rt_origin.xyz;
				vec2 wuv = clamp((wpos.xz - wrect.xy) / wrect.zw, half_texel, 1.0 - half_texel);
				float weather_wet = textureLod(sampler2D(bindless_textures[nonuniformEXT(wmap)], SAMPLER_LINEAR_CLAMP), wuv, 0.0).g;
				// A canopy stays wet while it holds the water it intercepted (the rain map's green), as the raster.
				uint rnmap = uint(get_rt_param(RT_PARAM_RAIN_MAP));
				if (rnmap != 0u && rt_material_wetness_canopy(mat)) {
					weather_wet = max(weather_wet, textureLod(sampler2D(bindless_textures[nonuniformEXT(rnmap)], SAMPLER_LINEAR_CLAMP), wuv, 0.0).g);
				}
				// Wet where water has been (contact wetness), easing out over its square's outer tenth.
				uint cmap = uint(get_rt_param(RT_PARAM_CONTACT_WET_MAP));
				if (cmap != 0u) {
					vec2 cuv = (wpos.xz - vec2(get_rt_param(RT_PARAM_CONTACT_WET_X), get_rt_param(RT_PARAM_CONTACT_WET_Z))) / max(get_rt_param(RT_PARAM_CONTACT_WET_SIZE), 1e-3);
					vec2 ce = min(cuv, 1.0 - cuv);
					float cedge = smoothstep(0.0, 0.1, min(ce.x, ce.y));
					if (cedge > 0.0) {
						// On printed ground, in its prints only (material.cpp ground_contact_shape()).
						float cshape = 1.0;
						float gsz = get_rt_param(RT_PARAM_GROUND_DEFORM_SIZE);
						if (rt_material_ground_deform(mat) && uint(get_rt_param(RT_PARAM_GROUND_DEFORM_MAP)) != 0u && gsz > 0.0) {
							vec2 g0 = vec2(get_rt_param(RT_PARAM_GROUND_DEFORM_X), get_rt_param(RT_PARAM_GROUND_DEFORM_Z));
							vec2 ge = min(wpos.xz - g0, g0 + gsz - wpos.xz);
							float gin = smoothstep(0.5, 1.5, min(ge.x, ge.y));
							if (gin > 0.0) {
								cshape = mix(1.0, smoothstep(0.0005, 0.002, rt_ground_dent(wpos.xz)), gin);
							}
						}
						// Catmull-Rom from five bilinear taps, as the raster read (material.cpp weather_contact_wetness_at).
						vec2 cts = vec2(textureSize(sampler2D(bindless_textures[nonuniformEXT(cmap)], SAMPLER_LINEAR_CLAMP), 0));
						vec2 csp = cuv * cts;
						vec2 ct1 = floor(csp - 0.5) + 0.5;
						vec2 cf = csp - ct1;
						vec2 cw0 = cf * (-0.5 + cf * (1.0 - 0.5 * cf));
						vec2 cw2 = cf * (0.5 + cf * (2.0 - 1.5 * cf));
						vec2 cw3 = cf * cf * (-0.5 + 0.5 * cf);
						vec2 cw12 = 1.0 + cf * cf * (-2.5 + 1.5 * cf) + cw2;
						vec2 ct0 = (ct1 - 1.0) / cts;
						vec2 ct3 = (ct1 + 2.0) / cts;
						vec2 ct12 = (ct1 + cw2 / cw12) / cts;
						float ca = cw12.x * cw0.y, cb = cw0.x * cw12.y, cc = cw12.x * cw12.y, cd = cw3.x * cw12.y, cg = cw12.x * cw3.y;
						float cr = textureLod(sampler2D(bindless_textures[nonuniformEXT(cmap)], SAMPLER_LINEAR_CLAMP), vec2(ct12.x, ct0.y), 0.0).r * ca + textureLod(sampler2D(bindless_textures[nonuniformEXT(cmap)], SAMPLER_LINEAR_CLAMP), vec2(ct0.x, ct12.y), 0.0).r * cb + textureLod(sampler2D(bindless_textures[nonuniformEXT(cmap)], SAMPLER_LINEAR_CLAMP), ct12, 0.0).r * cc + textureLod(sampler2D(bindless_textures[nonuniformEXT(cmap)], SAMPLER_LINEAR_CLAMP), vec2(ct3.x, ct12.y), 0.0).r * cd + textureLod(sampler2D(bindless_textures[nonuniformEXT(cmap)], SAMPLER_LINEAR_CLAMP), vec2(ct12.x, ct3.y), 0.0).r * cg;
						weather_wet = max(weather_wet, clamp(cr / (ca + cb + cc + cd + cg), 0.0, 1.0) * cedge * cshape);
					}
				}
				wet *= weather_wet;
				// Walls run with the rain they catch, in streaks at full detail (material.cpp: rain_surface_wetness()).
				if (!rt_material_wetness_canopy(mat)) {
					vec3 sr_air = vec3(0.0);
					float sr_run = wetness.y;
					uint sr_iuo = geometries[h.geometry_idx].instance_uniforms_ofs;
					if (rt_material_wetness_moving(mat) && sr_iuo != 0xFFFFFFFFu) {
						sr_air = -global_shader_uniforms.data[sr_iuo + 13u].xyz;
						sr_run *= 1.0 - clamp(global_shader_uniforms.data[sr_iuo + 14u].x, 0.0, 1.0);
					}
					vec4 sr = pt_weather_surface_rain(wpos, h.geometry_normal, sr_air);
					wet = rain_surface_wetness(wet, sr_run, h.geometry_normal, sr, rain_wall_streaks(wpos, h.geometry_normal, sr.zw, 0.0));
				}
			} else {
				wet = 0.0;
			}
		}
		wet = clamp(wet, 0.0, 1.0) * smoothstep(-0.6, 0.2, h.geometry_normal.y);
		wet = max(wet, g_wet); // the ground's own pore water: never wetted twice
		float wet_factor = mix(1.0, 0.2, (1.0 - m.metalness) * wetness.x);
		m.albedo *= mix(1.0, wet_factor, wet);
		m.roughness *= mix(1.0, wet_factor, 0.5 * wet);
		if (rt_material_wetness_canopy(mat) && wetness.w > 0.5) {
			// Water beaded on a waxy leaf, at full detail (footprint 0), as the raster's rain_surface_beads().
			vec4 bead = rain_surface_beads(h.hit_pos + rt_origin.xyz, m.normal, wet * smoothstep(-0.2, 0.6, m.normal.y), 0.0);
			if (bead.w > 0.0) {
				m.normal = normalize(m.normal - bead.xyz);
				m.roughness = mix(m.roughness, 0.05, bead.w);
				m.albedo *= 1.0 - 0.5 * bead.w;
			}
		}
	} else if (g_wet > 0.0) {
		// A ground material without wetness: sand's porosity (0.6).
		float wet_factor = mix(1.0, 0.2, (1.0 - m.metalness) * 0.6);
		m.albedo *= mix(1.0, wet_factor, g_wet);
		m.roughness *= mix(1.0, wet_factor, 0.5 * g_wet);
	}
	// Saturated ground under a film of water: the film's smooth surface hides the sand's relief (Lagarde 2013's
	// accumulated water), as material.cpp does.
	if (g_dep.x > 0.0) {
		// A fresh deposit in the flow's own colour (material.cpp ground_deposit_state()).
		m.albedo = mix(m.albedo, g_dep.yzw, clamp(g_dep.x, 0.0, 1.0));
		m.roughness = mix(m.roughness, 0.55, clamp(g_dep.x, 0.0, 1.0));
	}
	if (g_sheen > 0.0) {
		m.normal = normalize(mix(m.normal, h.geometry_normal, 0.85 * g_sheen));
		m.roughness = mix(m.roughness, 0.2, 0.6 * g_sheen);
	}
	// The salt crust over the wet margin (material.cpp ground_salt_apply()).
	if (g_salt > 0.0) {
		vec2 gsp = (h.hit_pos + rt_origin.xyz).xz;
		float gse = g_salt * mix(0.35, 1.0, ground_ripple_vn(gsp / 1.5, 23u));
		float gsn = 0.6 * ground_ripple_vn(gsp / 0.35, 21u) + 0.4 * ground_ripple_vn(gsp / 0.09, 22u);
		float gsu = smoothstep(0.3, 0.8, h.geometry_normal.y);
		vec2 gsv = ground_salt_cells(gsp / 0.1);
		float gsc = step(gsv.y, smoothstep(1.0 - gse, 1.35 - gse, gsn)) * (0.85 + 0.15 * (1.0 - smoothstep(0.0, 0.08, gsv.x))) * gsu;
		m.albedo = mix(m.albedo, vec3(0.56, 0.53, 0.48), max(0.75 * gsc, 0.15 * gse * gsu));
		m.roughness = mix(m.roughness, 0.92, min(gsc, 1.0));
		float gsg = 0.05 * ground_ripple_vn(gsp / 0.04, 25u);
		m.albedo *= 1.0 + step(gsv.y, smoothstep(1.0 - gse, 1.35 - gse, gsn)) * gsu * (0.12 * fract(gsv.y * 17.0) - 0.06 - 0.25 * (1.0 - smoothstep(gsg, gsg + 0.02, gsv.x)));
	}
	// Standing water in ruts and prints (material.cpp ground_water_look()): the patch already lies on the water's
	// level there; the surface is water's (F0 0.02, smooth, no relief), the bed under it soaked and, deeper, hidden
	// by the clay the water carries.
	if (g_water > 0.0) {
		vec4 gwl = ground_water_look(g_water, g_wet, g_mud);
		m.albedo *= gwl.x;
		m.roughness = mix(m.roughness, gwl.z, max(gwl.y, 0.85 * gwl.w));
		m.specular = mix(m.specular, 0.25, max(gwl.y, gwl.w));
		m.normal = normalize(mix(m.normal, h.geometry_normal, max(gwl.y, 0.6 * gwl.w)));
		g_grain *= 1.0 - gwl.y;
		// Running water's ripples, swept downstream (material.cpp ground_water_flow()); the footprint as the
		// cracks take it below.
		float gffp;
		vec3 gfp = h.hit_pos + rt_origin.xyz;
		if (get_total_bounces(payload.packed_bounces_flags) == 0u) {
			vec3 cam = vec3(scene_data_block.data.inv_view_matrix[0].w, scene_data_block.data.inv_view_matrix[1].w,
					scene_data_block.data.inv_view_matrix[2].w);
			float pix = 2.0 / (abs(scene_data_block.data.projection_matrix[1][1]) * scene_data_block.data.viewport_size.y);
			gffp = scene_data_block.data.projection_matrix[3][3] != 0.0 ? pix : distance(h.hit_pos, cam) * pix;
		} else {
			gffp = exp2(rt_cone_footprint_log2);
		}
		// Ice over it (material.cpp ground_ice_look()).
		vec2 gisl = vec2(0.0);
		vec4 gil = g_ice.x > 0.0 ? ground_ice_look(gfp.xz, min(g_ice.x, g_water), g_water, g_ice.y, gffp, gisl) : vec4(0.0);
		gil *= gwl.y;
		gisl *= gwl.y;
		m.albedo = mix(m.albedo, vec3(0.8, 0.83, 0.86), gil.x);
		m.roughness = mix(m.roughness, gil.z, gil.y);
		m.specular = mix(m.specular, 0.22, gil.y);
		vec2 gfs = ground_water_flow(gfp.xz, h.geometry_normal, g_water, scene_data_block.data.time, gffp) * max(gwl.y, gwl.w) * (1.0 - gil.y) + gisl;
		// Raindrop rings on the water (material.cpp: rain_ripples() at weather_rainfall_at()); the rain from the
		// weather state map (red, mm/h) times the rain share at this height.
		{
			float grs = max(gwl.y, gwl.w) * (1.0 - gil.y);
			float grate = 0.0;
			vec4 grect = vec4(get_rt_param(RT_PARAM_WETNESS_RECT_X), get_rt_param(RT_PARAM_WETNESS_RECT_Y),
					get_rt_param(RT_PARAM_WETNESS_RECT_W), get_rt_param(RT_PARAM_WETNESS_RECT_H));
			if (grs > 0.0 && grect.z > 0.0) {
				uint gwmap = uint(get_rt_param(RT_PARAM_WETNESS_MAP));
				vec2 gws = vec2(textureSize(sampler2D(bindless_textures[nonuniformEXT(gwmap)], SAMPLER_LINEAR_CLAMP), 0));
				vec2 ght = 0.5 / max(gws, vec2(1.0));
				vec2 gwuv = clamp((gfp.xz - grect.xy) / grect.zw, ght, 1.0 - ght);
				float gprec = textureLod(sampler2D(bindless_textures[nonuniformEXT(gwmap)], SAMPLER_LINEAR_CLAMP), gwuv, 0.0).r;
				float gair = get_rt_param(RT_PARAM_PRECIP_TEMPERATURE) - get_rt_param(RT_PARAM_PRECIP_LAPSE) * (gfp.y - get_rt_param(RT_PARAM_PRECIP_GROUND));
				grate = max(gprec, 0.0) * rain_liquid_share_nose(gair, get_rt_param(RT_PARAM_PRECIP_HUMIDITY), gfp.y - get_rt_param(RT_PARAM_PRECIP_GROUND),
								get_rt_param(RT_PARAM_PRECIP_NOSE), get_rt_param(RT_PARAM_PRECIP_NOSE_BASE));
			}
			vec3 grr = grate > 0.0 ? rain_ripples(gfp.xz, scene_data_block.data.time, grate, gffp) : vec3(0.0);
			gfs += grr.xy * grs;
			m.roughness = mix(m.roughness, rain_ripple_roughness(m.roughness, grr.z), grs);
		}
		if (gfs != vec2(0.0)) {
			m.normal = normalize(m.normal - vec3(gfs.x, 0.0, gfs.y));
		}
	}
	// Needle ice and its thaw's crumbs (material.cpp ground_needle_look()).
	if (g_ice.z > 0.0 || g_ice.w > 0.0) {
		vec3 gnp = h.hit_pos + rt_origin.xyz;
		float gnfp;
		if (get_total_bounces(payload.packed_bounces_flags) == 0u) {
			vec3 cam = vec3(scene_data_block.data.inv_view_matrix[0].w, scene_data_block.data.inv_view_matrix[1].w,
					scene_data_block.data.inv_view_matrix[2].w);
			float pix = 2.0 / (abs(scene_data_block.data.projection_matrix[1][1]) * scene_data_block.data.viewport_size.y);
			gnfp = scene_data_block.data.projection_matrix[3][3] != 0.0 ? pix : distance(h.hit_pos, cam) * pix;
			gnfp /= max(abs(dot(normalize(cam - h.hit_pos), h.geometry_normal)), 0.15);
		} else {
			gnfp = exp2(rt_cone_footprint_log2);
		}
		vec2 gnsl;
		vec4 gnl = ground_needle_look(gnp.xz, g_ice.z, clamp(g_ice.w, 0.0, 1.0), gnfp, gnsl);
		m.albedo = mix(m.albedo * gnl.y, vec3(0.75, 0.78, 0.81), gnl.x);
		m.roughness = mix(mix(m.roughness, 1.0, 0.6 * gnl.z), 0.3, gnl.x);
		m.specular = mix(m.specular, 0.22, gnl.x);
		m.normal = normalize(m.normal - vec3(gnsl.x, 0.0, gnsl.y));
	}
	// Desiccation cracks (material.cpp): the mesh carries those its spacing can; the shading the slopes and the slots'
	// darkness of the finer ones.
	// A pool's bed (material.cpp): its fine clay buries the soil's grain (its normal map), before the cracks' slopes.
	m.normal = normalize(mix(m.normal, h.geometry_normal, 0.8 * smoothstep(0.035, 0.05, g_crack.y) * step(1e-4, abs(g_crack.w) + g_crack.x)));
	if (g_crack.x > 0.0 && g_crack.y > 0.0) {
		vec3 gcp = h.hit_pos + rt_origin.xyz;
		float gcfp;
		if (get_total_bounces(payload.packed_bounces_flags) == 0u) {
			vec3 cam = vec3(scene_data_block.data.inv_view_matrix[0].w, scene_data_block.data.inv_view_matrix[1].w,
					scene_data_block.data.inv_view_matrix[2].w);
			float pix = 2.0 / (abs(scene_data_block.data.projection_matrix[1][1]) * scene_data_block.data.viewport_size.y);
			gcfp = scene_data_block.data.projection_matrix[3][3] != 0.0 ? pix : distance(h.hit_pos, cam) * pix;
			// Stretched along the ground at a grazing view (the pixel's long axis there), as material.cpp.
			gcfp /= max(abs(dot(normalize(cam - h.hit_pos), h.geometry_normal)), 0.15);
		} else {
			gcfp = exp2(rt_cone_footprint_log2);
		}
		// The mesh's spacing there: the main field's texel, the near field's inside its square.
		float gcv = 1e-3;
		uint gcm = uint(get_rt_param(RT_PARAM_GROUND_DEFORM_MAP));
		if (gcm != 0u) {
			gcv = get_rt_param(RT_PARAM_GROUND_DEFORM_SIZE) / float(textureSize(sampler2D(bindless_textures[nonuniformEXT(gcm)], SAMPLER_NEAREST_CLAMP), 0).x);
			uint gcn = uint(get_rt_param(RT_PARAM_GROUND_NEAR_MAP));
			float gns = get_rt_param(RT_PARAM_GROUND_NEAR_SIZE);
			if (gcn != 0u && gns > 0.0) {
				vec2 n0 = vec2(get_rt_param(RT_PARAM_GROUND_NEAR_X), get_rt_param(RT_PARAM_GROUND_NEAR_Z));
				vec2 ne = min(gcp.xz - n0, n0 + gns - gcp.xz);
				float nt = gns / float(textureSize(sampler2D(bindless_textures[nonuniformEXT(gcn)], SAMPLER_NEAREST_CLAMP), 0).x);
				gcv = mix(gcv, nt, smoothstep(0.0, 1.0, min(ne.x, ne.y)));
			}
		}
		GroundCracks gc = ground_cracks(gcp.xz, g_crack);
		vec4 gcf = ground_crack_height(gc, gcfp);
		vec4 gcmh = ground_crack_height(gc, 1.5 * max(gcv, 1e-3));
		m.normal = normalize(m.normal - vec3(gcf.y - gcmh.y, 0.0, gcf.z - gcmh.z));
		float gcd = max(gc.depth.x, 1e-4);
		float gcw = max(max(gc.width.x, gc.width.y), 1e-4);
		m.albedo *= mix(1.0, 0.7 * gcw / (gcw + 2.0 * gcd), max(gcf.w - gcmh.w, 0.0));
		m.albedo *= mix(1.0, 0.75, gcf.w * max(g_crack.w, 0.0));
	}
	{
		// The crust: the fine clay the water left, pale and matte; the layer still drying, dark with its water.
		float gcc = max(g_crack.w, 0.0);
		vec3 gcl = m.albedo * 1.9;
		m.albedo = mix(m.albedo, mix(gcl, vec3(dot(gcl, vec3(0.299, 0.587, 0.114))), 0.2), gcc);
		m.roughness = mix(m.roughness, 1.0, 0.5 * gcc);
		float gcdmp = max(-g_crack.w - g_wet, 0.0);
		m.albedo *= 1.0 - 0.5 * gcdmp;
		m.roughness = mix(m.roughness, 0.45, 0.6 * gcdmp);
	}
	// Loose dry sand: its grains' colours and the grains that mirror a light (sand_glints), sized by the
	// footprint as the snow's sparkle is (camera rays: pixel angle times distance; others: their ray cone).
	if (g_grain > 0.0) {
		float gfp;
		if (get_total_bounces(payload.packed_bounces_flags) == 0u) {
			vec3 cam = vec3(scene_data_block.data.inv_view_matrix[0].w, scene_data_block.data.inv_view_matrix[1].w,
					scene_data_block.data.inv_view_matrix[2].w);
			float pix = 2.0 / (abs(scene_data_block.data.projection_matrix[1][1]) * scene_data_block.data.viewport_size.y);
			gfp = scene_data_block.data.projection_matrix[3][3] != 0.0 ? pix : distance(h.hit_pos, cam) * pix;
		} else {
			gfp = exp2(rt_cone_footprint_log2);
		}
		vec3 gpos = h.hit_pos + rt_origin.xyz;
		m.albedo *= mix(1.0, ground_grain_speckle(gpos, gfp), g_grain);
		vec4 gl = ground_glint(gpos, h.geometry_normal, gfp);
		float glw = g_grain * gl.w * (1.0 - smoothstep(0.05, 0.3, g_wet));
		if (glw > 0.0 && rt_material_feature(RT_MATERIAL_FEATURE_CLEARCOAT)) {
			m.clearcoat = min(1.1 * glw, 1.0);
			m.clearcoat_roughness = 0.45;
			m.coat_normal = gl.xyz;
		}
	}

	// Glaze (BaseMaterial3D glaze): clear ice from freezing rain, the rain map's alpha (mm), as material.cpp's
	// weather_glaze_apply(), under any snow on it.
	if (rt_material_glaze(mat)) {
		uint gzmap = uint(get_rt_param(RT_PARAM_RAIN_MAP));
		vec4 gzrect = vec4(get_rt_param(RT_PARAM_WETNESS_RECT_X), get_rt_param(RT_PARAM_WETNESS_RECT_Y),
				get_rt_param(RT_PARAM_WETNESS_RECT_W), get_rt_param(RT_PARAM_WETNESS_RECT_H));
		if (gzmap != 0u && gzrect.z > 0.0) {
			vec2 gzsize = vec2(textureSize(sampler2D(bindless_textures[nonuniformEXT(gzmap)], SAMPLER_LINEAR_CLAMP), 0));
			vec2 gzhalf = 0.5 / max(gzsize, vec2(1.0));
			vec2 gzuv = clamp(((h.hit_pos + rt_origin.xyz).xz - gzrect.xy) / gzrect.zw, gzhalf, 1.0 - gzhalf);
			float gzmm = textureLod(sampler2D(bindless_textures[nonuniformEXT(gzmap)], SAMPLER_LINEAR_CLAMP), gzuv, 0.0).a;
			float gzcover = smoothstep(0.0, 1.0, gzmm);
			if (gzcover > 0.0) {
				vec4 gzwet = rt_material_wetness(mat);
				float gzpor = gzwet.z > 0.5 ? gzwet.x : 0.3; // Without Wetness: bark, a middling porosity (birch bark barely soaks).
				m.albedo *= mix(1.0, mix(1.0, 0.2, (1.0 - m.metalness) * gzpor), gzcover);
				m.roughness = mix(m.roughness, 0.06, gzcover);
				m.specular = mix(m.specular, 0.225, gzcover);
				m.normal = normalize(mix(m.normal, h.geometry_normal, gzcover * smoothstep(0.5, 6.0, gzmm)));
			}
		}
	}

	// Snow (BaseMaterial3D, docs/ideas/snow.md): the same lines and inputs as the raster shader.
	vec4 snow_p = rt_material_snow(mat);
	if (snow_p.z > 0.5) {
		float snow_cover = snow_p.x;
		float snow_albedo = 0.85;
		float snow_hoar = 0.0;
		float snow_wet = 0.0;
		if (snow_p.w > 0.5) {
			vec4 srect = vec4(get_rt_param(RT_PARAM_SNOW_RECT_X), get_rt_param(RT_PARAM_SNOW_RECT_Y),
					get_rt_param(RT_PARAM_SNOW_RECT_W), get_rt_param(RT_PARAM_SNOW_RECT_H));
			vec4 wrect = vec4(get_rt_param(RT_PARAM_SNOW_WORLD_RECT_X), get_rt_param(RT_PARAM_SNOW_WORLD_RECT_Y),
					get_rt_param(RT_PARAM_SNOW_WORLD_RECT_W), get_rt_param(RT_PARAM_SNOW_WORLD_RECT_H));
			vec3 spos = h.hit_pos + rt_origin.xyz;
			vec4 s = vec4(0.0);
			bool have = false;
			float snow_under = 0.0;
			float snow_crown = 0.0;
			float snow_crown_load = 0.0;
			// The world tier, then the near square faded in over its outer tenth (as the raster shader).
			if (wrect.z > 0.0) {
				vec2 wuv = (spos.xz - wrect.xy) / wrect.zw;
				if (all(greaterThanEqual(wuv, vec2(0.0))) && all(lessThanEqual(wuv, vec2(1.0)))) {
					uint wmap = uint(get_rt_param(RT_PARAM_SNOW_WORLD_MAP));
					s = snow_map_bspline(wmap, wuv);
					have = true;
				}
			}
			if (srect.z > 0.0) {
				vec2 nuv = (spos.xz - srect.xy) / srect.zw;
				vec2 ne = min(nuv, 1.0 - nuv);
				// With no world tier, no data outside the square (as the raster shader).
				float nw = wrect.z > 0.0 ? clamp(min(ne.x, ne.y) * 10.0, 0.0, 1.0) : step(0.0, min(ne.x, ne.y));
				if (nw > 0.0) {
					uint smap = uint(get_rt_param(RT_PARAM_SNOW_MAP));
					vec2 ssize = vec2(textureSize(sampler2D(bindless_textures[nonuniformEXT(smap)], SAMPLER_LINEAR_CLAMP), 0));
					vec2 half_texel = 0.5 / max(ssize, vec2(1.0));
					s = mix(s, snow_map_bspline(smap, nuv), nw);
					have = true;
					// Shelter and tree crowns from the catch map, as the raster shader.
					uint cmap = uint(get_rt_param(RT_PARAM_SNOW_CATCH_MAP));
					if (cmap != 0u) {
						vec4 c = textureLod(sampler2D(bindless_textures[nonuniformEXT(cmap)], SAMPLER_LINEAR_CLAMP), clamp(nuv, half_texel, 1.0 - half_texel), 0.0);
						if (c.a > 0.5) {
							float dy = spos.y - c.r;
							snow_under = (1.0 - smoothstep(-0.8, -0.4, dy)) * nw;
							snow_crown = smoothstep(1.5, 3.0, dy) * step(0.05, c.b) * nw;
							snow_crown_load = c.g;
							// Surface hoar on the pack's top (mm; alpha 1 + mm / 10), as snow_hoar_at() in raster.
							snow_hoar = max(c.a - 1.0, 0.0) * 10.0 * smoothstep(-0.8, -0.4, dy) * (1.0 - smoothstep(1.0, 1.5, dy));
						}
					}
				}
			}
			if (have) {
				// Niu and Yang (2007) snow-covered fraction, z0 the pixel's ground roughness (as snow_cover_at()).
				float snow_amt = snow_cover;
				snow_cover *= tanh(max(s.r, 0.0) * snow_subgrid(spos) / (0.01 * max(s.a * 10.0, 0.5))) * (1.0 - snow_under);
				snow_cover = mix(snow_cover, snow_amt * min(snow_crown_load * 2.0, 1.0), snow_crown);
				snow_albedo = s.g;
				snow_wet = clamp(s.b, 0.0, 1.0);
			} else {
				snow_cover = 0.0;
			}
		}
		// A moving body's own snow (BaseMaterial3D snow_on_body, SnowBodies): instance uniform slot 12 holds depth
		// (m), albedo, melt water and 1 + density / 1000, as the raster shader reads it.
		if (rt_material_snow_on_body(mat)) {
			uint sbo = geometries[h.geometry_idx].instance_uniforms_ofs;
			vec4 sb = sbo != 0xFFFFFFFFu ? global_shader_uniforms.data[sbo + 12u] : vec4(0.0);
			if (sb.w > 0.5) {
				snow_cover = snow_p.x * tanh(max(sb.x, 0.0) / (0.025 * max((sb.w - 1.0) * 10.0, 0.5)));
				snow_albedo = sb.y;
				snow_wet = clamp(sb.z, 0.0, 1.0);
			} else if (snow_p.w < 0.5) {
				snow_cover = 0.0;
			}
		}
		float snow_cos_max = min(cos(radians(snow_p.y)), 0.999);
		float snow_hold = smoothstep(snow_cos_max, cos(radians(max(snow_p.y - 20.0, 0.0))), h.geometry_normal.y);
		snow_cover = clamp(snow_cover, 0.0, 1.0);
		float snow_flat = clamp(1.0 - (1.0 - m.normal.y) / 0.3, 0.0, 1.0);
		float snow = snow_hold * max(smoothstep(1.0 - 1.2 * snow_cover, 1.2 - 1.2 * snow_cover, snow_flat), smoothstep(0.9, 1.0, snow_cover));
		m.albedo = mix(m.albedo, vec3(snow_albedo), snow);
		m.roughness = mix(m.roughness, mix(0.7, 0.35, snow_wet), snow);
		m.metalness = mix(m.metalness, 0.0, snow);
		m.specular = mix(m.specular, 0.335, snow);
		m.normal = normalize(mix(m.normal, h.geometry_normal, snow));
		// Sparkle on dry snow. Camera rays size the grid by the pixel's angle times the distance, as raster
		// does; reflected and bounced rays by their ray cone (Akenine-Moller et al. 2019, carried from the
		// camera through each bounce), so snow seen in water or a window, and the light it bounces, keeps
		// its glints at the grain size that footprint resolves.
		float glint_dry = 1.0 - smoothstep(0.05, 0.3, snow_wet);
		if (get_rt_param(RT_PARAM_SNOW_EXACT_GLINTS) > 0.5 && snow >= 0.5 && glint_dry > 0.0) {
			// Exact: the snow's ice specular (F0 0.018) lies on the facet of the grain hit, near-smooth
			// (roughness 0.22, alpha 0.05), the diffuse staying on the snow's normal. Over a pixel's
			// samples the facets sum to the rough lobe (alpha 0.4) they stand for, with the glints of the
			// grains that mirror a light. Melt water films the grains over: the rough lobe comes back.
			m.specular_normal = snow_grain_facet(h.hit_pos + rt_origin.xyz, h.geometry_normal, snow_hoar);
			m.roughness = mix(m.roughness, 0.22, glint_dry);
		} else if (snow >= 0.5 && (get_total_bounces(payload.packed_bounces_flags) == 0u || rt_cone_footprint_log2 > RT_TEXTURE_LOD_BASE)) {
			float fp;
			if (get_total_bounces(payload.packed_bounces_flags) == 0u) {
				vec3 cam = vec3(scene_data_block.data.inv_view_matrix[0].w, scene_data_block.data.inv_view_matrix[1].w,
						scene_data_block.data.inv_view_matrix[2].w);
				float pix = 2.0 / (abs(scene_data_block.data.projection_matrix[1][1]) * scene_data_block.data.viewport_size.y);
				fp = scene_data_block.data.projection_matrix[3][3] != 0.0 ? pix : distance(h.hit_pos, cam) * pix;
			} else {
				fp = exp2(rt_cone_footprint_log2);
			}
			vec4 g = snow_glint(h.hit_pos + rt_origin.xyz, h.geometry_normal, fp, snow_hoar);
			float gw = g.w * (1.0 - smoothstep(0.05, 0.3, snow_wet));
			if (gw > 0.0 && rt_material_feature(RT_MATERIAL_FEATURE_CLEARCOAT)) {
				// The facet is the coat lobe on its own normal, as in raster; the snow keeps its shading.
				m.clearcoat = 0.45 * gw;
				m.clearcoat_roughness = 0.5;
				m.coat_normal = g.xyz;
			}
		}
	}

	// Mud on a vehicle's body (BaseMaterial3D Mud, docs/ideas/mud.md, stage 3b): the same lines as the raster shader,
	// from the same instance uniforms (BaseMaterial3D::get_mud_functions_code(), slots 0-11).
	if (rt_material_mud(mat) && geometries[h.geometry_idx].instance_uniforms_ofs != 0xFFFFFFFFu) {
		uint vmo = geometries[h.geometry_idx].instance_uniforms_ofs;
		float vmfp;
		if (get_total_bounces(payload.packed_bounces_flags) == 0u) {
			vec3 cam = vec3(scene_data_block.data.inv_view_matrix[0].w, scene_data_block.data.inv_view_matrix[1].w,
					scene_data_block.data.inv_view_matrix[2].w);
			float pix = 2.0 / (abs(scene_data_block.data.projection_matrix[1][1]) * scene_data_block.data.viewport_size.y);
			vmfp = scene_data_block.data.projection_matrix[3][3] != 0.0 ? pix : distance(h.hit_pos, cam) * pix;
		} else {
			vmfp = exp2(rt_cone_footprint_log2);
		}
		vec4 vmb = global_shader_uniforms.data[vmo + 9u];
		vec2 vmtone;
		vec4 vm = vmud_at(h.hit_pos + rt_origin.xyz, vmfp, global_shader_uniforms.data[vmo], global_shader_uniforms.data[vmo + 1u],
				global_shader_uniforms.data[vmo + 2u], global_shader_uniforms.data[vmo + 3u], global_shader_uniforms.data[vmo + 4u],
				global_shader_uniforms.data[vmo + 5u], global_shader_uniforms.data[vmo + 6u], global_shader_uniforms.data[vmo + 7u],
				global_shader_uniforms.data[vmo + 8u], vmb, global_shader_uniforms.data[vmo + 11u], vmtone);
		if (vm.x > 0.0) {
			vmud_apply(vm.x, vmtone, global_shader_uniforms.data[vmo + 10u].rgb, vmb.z, m.albedo, m.roughness, m.metalness, m.specular);
			m.normal = normalize(mix(m.normal, h.geometry_normal, vm.x));
			m.normal = normalize(m.normal - (vm.yzw - dot(vm.yzw, m.normal) * m.normal));
		}
	}

	// Proximity/distance fade (BaseMaterial3D feature, mirrors raster).
	if ((mat.flags & (RT_MAT_FLAG_PROXIMITY_FADE | RT_MAT_FLAG_DISTANCE_FADE)) != 0u) {
		mat4 fade_view_mat = transpose(mat4(scene_data_block.data.view_matrix[0],
				scene_data_block.data.view_matrix[1],
				scene_data_block.data.view_matrix[2],
				vec4(0.0, 0.0, 0.0, 1.0)));
		vec3 fade_view_pos = (fade_view_mat * vec4(h.hit_pos, 1.0)).xyz;
		if ((mat.flags & RT_MAT_FLAG_DISTANCE_FADE) != 0u) {
			vec2 distance_fade = unpackHalf2x16(mat.distance_fade);
			m.alpha *= clamp(smoothstep(distance_fade.x, distance_fade.y, length(fade_view_pos)), 0.0, 1.0);
		}
		if ((mat.flags & RT_MAT_FLAG_PROXIMITY_FADE) != 0u && (payload.packed_bounces_flags & PEEL_RAY_FLAG) != 0u) {
			// Peel rays carry the segment's opaque NDC depth; 0.0 = far (no fade).
			float fade_scene_z = -1e19;
			float scene_depth = payload_peel_scene_depth(payload);
			if (scene_depth > 0.0) {
				vec4 fade_scene_pos = scene_data_block.data.inv_projection_matrix * vec4(0.0, 0.0, scene_depth, 1.0);
				fade_scene_z = fade_scene_pos.z / fade_scene_pos.w;
			}
			m.alpha *= clamp(1.0 - smoothstep(fade_scene_z + rt_material_proximity_fade_distance(mat), fade_scene_z, fade_view_pos.z), 0.0, 1.0);
		}
	}

#ifdef RT_DECALS_ENABLED
	if (rt_decals_process(h, m)) {
		return;
	}
#endif

#ifdef RT_DEBUG_ENABLED
	{
		int VIS_MODE = int(get_rt_param(RT_PARAM_VIS_MODE));
		// VIS 23 (transparency heatmap) shades normally; raygen overrides the image.
		if (VIS_MODE != 0 && VIS_MODE != 23) {
			vec2 debug_uv = muv.triplanar ? muv.triplanar_pos.xy : muv.uv;
			debug_visualize(VIS_MODE, debug_material_inputs(h, m, tangent_space_normal, h.tangent, h.bitangent, debug_uv, material_ao(mat, muv)), m.alpha);
			return;
		}
	}
#endif // RT_DEBUG_ENABLED
	shade_and_bounce(h, m);
#endif
}

#[any_hit]

#version 460

#VERSION_DEFINES

#pragma shader_stage(any_hit)
#extension GL_EXT_ray_tracing : enable
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_ARB_gpu_shader_int64 : require
#extension GL_EXT_nonuniform_qualifier : require
#ifdef RT_LSS
#extension GL_NV_linear_swept_spheres : require
#endif

#define GLSL 1
#define RT_STAGE_ANY_HIT 1
// clang-format off
#include "raytracing_inc.glsl"
#include "../scene_data_inc.glsl"
#include "raytracing_common_inc.glsl"
// clang-format on

#ifdef RT_LSS
// A swept sphere hit writes no barycentrics: rt_lss_resolve() sets these to its strip triangle's.
vec2 rt_hit_bary = vec2(0.0);
int rt_hit_primitive = 0;
bool rt_lss_hit = false;
vec3 rt_lss_offset = vec3(0.0); // From the tube's axis to the hit, world space.
#define attribs rt_hit_bary
#define RT_HIT_PRIMITIVE_ID rt_hit_primitive
#else
#define attribs hit_attribs.bary_or_uv
#endif
#define RT_HIT_ATTRIBS_DECLARED

#include "raytracing_hit_inc.glsl"

layout(location = 0) rayPayloadInEXT PathPayload payload;

// clang-format off
layout(set = 0, binding = 3, std430) readonly buffer GeometryBuffer {
	GeometryData geometries[];
};

layout(set = 0, binding = 4, std430) readonly buffer MotionIndexBuffer {
	int motion_indices[];
};

layout(set = 0, binding = 5, std430) readonly buffer MaterialBuffer {
	MaterialData materials[];
};
// clang-format on

#include "raytracing_entry_material_inc.glsl"

layout(set = 1, binding = 0) uniform texture2D bindless_textures[];

#include "raytracing_samplers_inc.glsl"

// clang-format off
#include "raytracing_material_eval_inc.glsl"

layout(set = 0, binding = 32, std430) readonly buffer MotionTransforms {
	InstanceMotionData motion_transforms[];
};
// clang-format on

// ============================================================================
// CUSTOM SHADER GLOBALS (injected for per-HG any-hit)
// ============================================================================
#ifdef RT_CUSTOM_HIT_GROUP
#include "raytracing_custom_globals_inc.glsl"
#endif

#ifdef RT_LSS
// Swept sphere hits read their attributes on the strip triangle at the same place along the segment
// (rt_lss_triangle), and keep where the ray met the round fibre for the normal and the hair's fibre offset.
void rt_lss_resolve() {
	rt_lss_hit = gl_HitIsLSSNV;
	if (!rt_lss_hit) {
		rt_hit_bary = hit_attribs.bary_or_uv;
		rt_hit_primitive = gl_PrimitiveID;
		return;
	}
	vec3 p0 = gl_HitLSSPositionsNV[0];
	vec3 axis = gl_HitLSSPositionsNV[1] - p0;
	vec3 hit_obj = gl_ObjectRayOriginEXT + gl_ObjectRayDirectionEXT * gl_HitTEXT;
	float len2 = dot(axis, axis);
	float u = len2 > 0.0 ? clamp(dot(hit_obj - p0, axis) / len2, 0.0, 1.0) : 0.0;
	rt_hit_primitive = rt_lss_triangle(gl_PrimitiveID, u);
	rt_hit_bary = rt_lss_barycentrics(u);
	rt_lss_offset = mat3(gl_ObjectToWorldEXT) * (hit_obj - (p0 + axis * u));
}
#endif

void main() {
#ifdef RT_LSS
	rt_lss_resolve();
#endif
	uint geometry_idx = RT_HIT_GEOMETRY_IDX;
	// Shadow rays run this for every candidate (gl_RayFlagsNoOpaqueEXT), and most are opaque: decide from the flags
	// word alone, before the geometry record and the whole material are read.
	uint entry_flags = rt_entry_material_flags(geometry_idx);
	bool transparent = (entry_flags & RT_MAT_FLAG_TRANSPARENT) != 0u;
	bool alpha_scissor = (entry_flags & RT_MAT_FLAG_ALPHA_SCISSOR) != 0u;
	// Thin glass lets shadow rays through; other rays hit it as an opaque surface.
	bool shadow_transmission = rt_material_feature(RT_MATERIAL_FEATURE_TRANSMISSION);
	bool shadow_back_lighting = rt_material_feature(RT_MATERIAL_FEATURE_BACK_LIGHTING);
	bool transmissive_shadow = !transparent && !alpha_scissor && is_shadow_ray(payload.packed_bounces_flags) &&
			(((entry_flags & RT_MAT_FLAG_TRANSMISSIVE) != 0u && shadow_transmission) || ((entry_flags & RT_MAT_FLAG_BACKLIT) != 0u && shadow_back_lighting) ||
					((entry_flags & RT_MAT_FLAG_CUSTOM_TRANSMITTANCE) != 0u && (shadow_transmission || shadow_back_lighting)));
	if (!transparent && !alpha_scissor && !transmissive_shadow) {
		return; // Ordinary opaque shadow candidate: accept immediately.
	}
	GeometryData geom = geometries[geometry_idx];
	MaterialData mat = rt_entry_material(geometry_idx);
	// A custom shader that bounds a medium (TRANSMISSION and IOR) and cuts its outline with ALPHA_SCISSOR (water's
	// shoreline): decided once its fragment has run (below), where it passes the light through its medium as an
	// unscissored one does. Taken as an opaque scissored caster, the sun reached a bed under water only past the
	// water's own TLAS mask (water casts no raster shadows), with none of the water's absorption on the way down.
	bool scissored_medium_candidate = alpha_scissor && !transparent && is_shadow_ray(payload.packed_bounces_flags) &&
			(mat.flags & RT_MAT_FLAG_CUSTOM_TRANSMITTANCE) != 0u && shadow_transmission;

	uint i0, i1, i2;
	get_triangle_indices(geom, i0, i1, i2);
	vec3 bary = vec3(1.0 - attribs.x - attribs.y, attribs.x, attribs.y);

	float hit_alpha = 1.0;
	vec3 hit_tint = vec3(1.0);
	float scissor_threshold = -1.0;
	MaterialLayerTextures hit_layers = material_layer_textures_none();

#ifdef RT_CUSTOM_HIT_GROUP
	// Compute hit data inline (cannot include closest_hit_common_inc here).
	uint rt_geometry_idx = geometry_idx;
	vec2 rt_uv = fetch_uv(geom, i0, i1, i2, bary);
	vec2 rt_uv2 = fetch_uv2(geom, i0, i1, i2, bary);
	TBNResult ah_tbn = fetch_tbn(geom, i0, i1, i2, bary);

	mat3 model_rotation = mat3(gl_ObjectToWorldEXT);
	mat3 normal_matrix = mat3(
			normalize(model_rotation[0]),
			normalize(model_rotation[1]),
			normalize(model_rotation[2]));

	vec3 rt_normal = normalize(normal_matrix * ah_tbn.normal);
	vec3 rt_tangent = normalize(normal_matrix * ah_tbn.tangent);
	vec3 rt_bitangent = cross(rt_normal, rt_tangent) * ah_tbn.bitangent_sign;

	bool rt_front_face = rt_triangle_front_facing(geom);
	rt_normal = rt_side_checked_normal(geom, rt_normal, rt_front_face);

	vec3 rt_hit_pos = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * gl_HitTEXT;
	vec4 rt_color = fetch_color(geom, i0, i1, i2, bary);

#include "raytracing_custom_fragment_inc.glsl"

	hit_alpha = alpha;
	hit_tint = clamp(albedo, vec3(0.0), vec3(1.0));
	scissor_threshold = alpha_scissor_threshold;
	if (scissored_medium_candidate && ior >= 1.0 && transmission > 0.0) {
		if (hit_alpha < (scissor_threshold >= 0.0 ? scissor_threshold : 0.5)) {
			ignoreIntersectionEXT; // cut out: no surface here
		}
		transmissive_shadow = true;
	}
	if (transmissive_shadow) {
		// TRANSMISSION and BACKLIGHT, as BaseMaterial3D's Transmission and Back Lighting.
		PathState shadow_ps = path_unpack(payload);
		shadow_ps.throughput *= rt_shadow_transmittance(hit_tint, shadow_transmission ? transmission : 0.0,
				shadow_back_lighting ? backlight : vec3(0.0), specular, metallic);
		if (ior >= 1.0 && shadow_transmission && rt_material_feature(RT_MATERIAL_FEATURE_REFRACTION) && !rt_front_face) {
			// Leaving the medium of IOR, ABSORPTION and SCATTERING: its extinction from the shaded point on.
			shadow_ps.throughput *= exp(-(max(absorption, vec3(0.0)) + max(scattering, vec3(0.0))) * gl_HitTEXT);
		}
		path_pack(payload, shadow_ps);
		if (max(shadow_ps.throughput.r, max(shadow_ps.throughput.g, shadow_ps.throughput.b)) > 1e-3) {
			ignoreIntersectionEXT;
		}
		return;
	}
#else
	// HG0: StandardMaterial3D alpha evaluation.
	{
		vec3 world_pos = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * gl_HitTEXT;
		vec3 world_normal = vec3(0.0, 1.0, 0.0);
		if ((mat.flags & RT_MAT_FLAG_TRIPLANAR) != 0u) {
			// Only triplanar needs a normal, and fetching the frame is not free.
			TBNResult ah_tbn = fetch_tbn(geom, i0, i1, i2, bary);
			world_normal = normalize(mat3(gl_ObjectToWorldEXT) * ah_tbn.normal);
		}

		MaterialUV muv = material_uv_from_world(mat, geom, fetch_uv(geom, i0, i1, i2, bary), world_pos, world_normal);
		vec4 albedo_tex = material_uv_sample(mat.albedo_texture, muv, mat.flags);
		hit_alpha = albedo_tex.a * mat.albedo_color.a;
		hit_tint = clamp(albedo_tex.rgb * mat.albedo_color.rgb, vec3(0.0), vec3(1.0));
		if (transmissive_shadow) {
			hit_layers = material_layer_textures(mat, muv);
		}
	}
#endif
#ifndef RT_CUSTOM_HIT_GROUP
	if (transmissive_shadow) {
		// Glass and back-lit cloth or leaves: the light they let through, tinted (an even shadow, as
		// transparent casters give).
		PathState shadow_ps = path_unpack(payload);
		shadow_ps.throughput *= rt_material_shadow_transmittance(mat, hit_tint, hit_alpha, hit_layers, shadow_transmission, shadow_back_lighting);
		if ((mat.flags & RT_MAT_FLAG_VOLUME) != 0u && rt_material_feature(RT_MATERIAL_FEATURE_REFRACTION) && !rt_triangle_front_facing(geom)) {
			// Leaving a medium: the light was absorbed all the way from the shaded point (Beer-Lambert),
			// taken straight, without the bend of refraction (transmit shadows).
			shadow_ps.throughput *= rt_medium_transmittance(rt_material_medium(mat), gl_HitTEXT);
		}
		path_pack(payload, shadow_ps);
		if (max(shadow_ps.throughput.r, max(shadow_ps.throughput.g, shadow_ps.throughput.b)) > 1e-3) {
			ignoreIntersectionEXT;
		}
		return;
	}
#endif
	hit_alpha = rt_entry_alpha(mat, hit_alpha, scissor_threshold >= 0.0 ? scissor_threshold : 0.5);

	uint blend_class = (mat.flags & RT_MAT_BLEND_CLASS_MASK) >> RT_MAT_BLEND_CLASS_SHIFT;
	if (blend_class == RT_BLEND_CLASS_PREMULT && hit_alpha > 1e-6) {
		hit_tint = clamp(hit_tint / hit_alpha, vec3(0.0), vec3(1.0));
	}

	if ((mat.flags & RT_MAT_FLAG_INSTANCE_FADE) != 0u) {
		// rt_entry_alpha() zeroes scissored texels; the fade alone never reaches 0 (fully faded entries are culled).
		if (hit_alpha <= 0.0) {
			ignoreIntersectionEXT;
		}
	} else if (scissor_threshold >= 0.0) {
		if (hit_alpha < scissor_threshold) {
			ignoreIntersectionEXT;
		}
	} else if (!transparent && hit_alpha < 0.5) {
		ignoreIntersectionEXT;
	}
	// Alpha-0 texels of Mix/OIT/Add/Sub layers contribute nothing; skip them so
	// they don't spend a peel layer or take the first-layer depth/guide slot
	// from what's behind (glyph hull in front of its outline). Premult and Mul
	// carry light at alpha 0; depth_draw_always keeps raster's occlude-at-zero.
	if (transparent && hit_alpha < (1.0 / 255.0) &&
			blend_class != RT_BLEND_CLASS_PREMULT && blend_class != RT_BLEND_CLASS_MUL &&
			(mat.flags & RT_MAT_FLAG_DEPTH_DRAW_ALWAYS) == 0u) {
		ignoreIntersectionEXT;
	}
	// Transparent shadow candidates filter the light by their coverage: the ray keeps
	// (1 - alpha) of it, tinted, in the throughput payload, the expected value of a
	// coverage coin flip without its noise. Clear glass casts an even, light shadow.
	// Geometries are built without duplicate any-hit calls, so each surface counts once.
	if (transparent && is_shadow_ray(payload.packed_bounces_flags)) {
		float shadow_alpha = clamp(hit_alpha, 0.0, 1.0);
		if (shadow_alpha < 0.999) {
			PathState shadow_ps = path_unpack(payload);
			shadow_ps.throughput *= (1.0 - shadow_alpha) * mix(vec3(1.0), hit_tint, shadow_alpha);
			path_pack(payload, shadow_ps);
			if (max(shadow_ps.throughput.r, max(shadow_ps.throughput.g, shadow_ps.throughput.b)) > 1e-3) {
				ignoreIntersectionEXT;
			}
		}
	}
}

#[intersection]

#version 460

#VERSION_DEFINES

#pragma shader_stage(intersection)
#extension GL_EXT_ray_tracing : enable
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_ARB_gpu_shader_int64 : require
#extension GL_EXT_nonuniform_qualifier : require

#define GLSL 1
#define RT_STAGE_INTERSECTION 1

// clang-format off
#include "raytracing_inc.glsl"
#include "../scene_data_inc.glsl"
#include "raytracing_data_inc.glsl"
#include "raytracing_common_inc.glsl"
// clang-format on

// Write all attributes and report the intersection. Transparently delta-compresses
// PREV_POSITION into spare .w bytes of packed_normal/tangent + prev_pos_delta_yz.
#define report_intersection(t_hit, kind)                                                      \
	{                                                                                         \
		vec3 _obj_hit = gl_ObjectRayOriginEXT + gl_ObjectRayDirectionEXT * (t_hit);           \
		vec3 _delta = any(isnan(m_PREV_POSITION)) ? vec3(0.0) : (m_PREV_POSITION - _obj_hit); \
		uint _n4 = packSnorm4x8(vec4(m_HIT_NORMAL, 0.0));                                     \
		uint _t4 = packSnorm4x8(vec4(m_HIT_TANGENT, 0.0));                                    \
		uint _dx = packHalf2x16(vec2(_delta.x, 0.0));                                         \
		hit_attribs.bary_or_uv = m_HIT_UV;                                                    \
		hit_attribs.packed_normal = (_n4 & 0x00FFFFFFu) | ((_dx & 0xFFu) << 24u);             \
		hit_attribs.packed_tangent = (_t4 & 0x00FFFFFFu) | (((_dx >> 8u) & 0xFFu) << 24u);    \
		hit_attribs.prev_pos_delta_yz = packHalf2x16(vec2(_delta.y, _delta.z));               \
		reportIntersectionEXT(t_hit, kind);                                                   \
	}

#ifdef RT_CUSTOM_HIT_GROUP

// clang-format off
layout(set = 0, binding = 3, std430) readonly buffer GeometryBuffer {
	GeometryData geometries[];
};

layout(set = 0, binding = 5, std430) readonly buffer MaterialBuffer {
	MaterialData materials[];
};
// clang-format on

#include "raytracing_entry_material_inc.glsl"

layout(set = 1, binding = 0) uniform texture2D bindless_textures[];

#include "raytracing_samplers_inc.glsl"

layout(buffer_reference, std140) readonly buffer CustomMaterialUniforms{
	/* RT_CUSTOM_UNIFORM_MEMBERS */
};

/* RT_CUSTOM_TEXTURE_DEFINES */

// File-scope built-ins accessible from user helper functions in globals.
float global_time = scene_data_block.data.time;
float global_prev_time = 0.0;
mat4 read_model_matrix = mat4(0.0);
mat3 model_normal_matrix = mat3(0.0);
mat4 m_INV_MODEL_MATRIX = mat4(0.0);
mat4 read_view_matrix = transpose(mat4(scene_data_block.data.view_matrix[0], scene_data_block.data.view_matrix[1], scene_data_block.data.view_matrix[2], vec4(0.0, 0.0, 0.0, 1.0)));
mat4 inv_view_matrix = transpose(mat4(scene_data_block.data.inv_view_matrix[0], scene_data_block.data.inv_view_matrix[1], scene_data_block.data.inv_view_matrix[2], vec4(0.0, 0.0, 0.0, 1.0)));
mat4 projection_matrix = scene_data_block.data.projection_matrix;
mat4 inv_projection_matrix = scene_data_block.data.inv_projection_matrix;
vec2 read_viewport_size = scene_data_block.data.viewport_size;
float m_Z_NEAR = scene_data_block.data.z_near;
float m_Z_FAR = scene_data_block.data.z_far;

uint64_t _rt_material_address;
#define material CustomMaterialUniforms(_rt_material_address)

/* RT_CUSTOM_INTERSECTION_GLOBALS */

#endif

void main() {
#ifdef RT_CUSTOM_HIT_GROUP
	// Writable outputs.
	vec2 m_HIT_UV = vec2(0.0);
	vec3 m_HIT_NORMAL = vec3(0.0, 1.0, 0.0);
	vec3 m_HIT_TANGENT = vec3(1.0, 0.0, 0.0);
	vec3 m_PREV_POSITION = vec3(uintBitsToFloat(0x7FC00000u)); // NaN sentinel = not set.

	// Per-invocation built-ins (require RT intrinsics, only available in main).
	vec3 m_ORIGIN = gl_ObjectRayOriginEXT;
	vec3 m_DIRECTION = gl_ObjectRayDirectionEXT;
	vec3 m_WORLD_ORIGIN = gl_WorldRayOriginEXT;
	vec3 m_WORLD_DIRECTION = gl_WorldRayDirectionEXT;
	float m_T_MIN = gl_RayTminEXT;
	float m_T_MAX = gl_RayTmaxEXT;
	global_prev_time = scene_data_block.prev_data.time;

	// Resolve custom material uniforms via BDA (assigns file-scope address).
	uint rt_geometry_idx = RT_HIT_GEOMETRY_IDX;
	MaterialData rt_mat = rt_entry_material(rt_geometry_idx);
	_rt_material_address = rt_mat.uniform_address;

	// Per-primitive AABB bounds (available when expose_aabb_bounds is enabled).
	GeometryData rt_geom = geometries[rt_geometry_idx];
	vec3 m_AABB_MIN = vec3(0.0);
	vec3 m_AABB_MAX = vec3(0.0);
	if (rt_geom.vertex_address != 0ul) {
		FloatBuffer aabb_buf = FloatBuffer(rt_geom.vertex_address);
		int base = int(gl_PrimitiveID) * 6;
		m_AABB_MIN = vec3(aabb_buf.v[base + 0], aabb_buf.v[base + 1], aabb_buf.v[base + 2]);
		m_AABB_MAX = vec3(aabb_buf.v[base + 3], aabb_buf.v[base + 4], aabb_buf.v[base + 5]);
	}

	read_model_matrix = mat4(gl_ObjectToWorldEXT);
	model_normal_matrix = mat3(read_model_matrix);
	m_INV_MODEL_MATRIX = mat4(gl_WorldToObjectEXT);
	// Absolute world for user code; the trace itself is relative to rt_origin.
	read_model_matrix[3].xyz += rt_origin.xyz;
	m_INV_MODEL_MATRIX[3].xyz -= mat3(m_INV_MODEL_MATRIX) * rt_origin.xyz;
	inv_view_matrix[3].xyz += rt_origin.xyz;
	read_view_matrix[3].xyz -= mat3(read_view_matrix) * rt_origin.xyz;
	m_WORLD_ORIGIN += rt_origin.xyz;

	/* RT_CUSTOM_INTERSECTION_CODE */

#else
	// Base-variant fallback: never executed at runtime (the intersection
	// stage is always rebuilt per-HG with RT_CUSTOM_HIT_GROUP defined).
	// Only touch unconditional HitAttribs fields so the base variant parses.
	hit_attribs.bary_or_uv = vec2(0.0);
	reportIntersectionEXT(gl_RayTminEXT, 0u);
#endif
}

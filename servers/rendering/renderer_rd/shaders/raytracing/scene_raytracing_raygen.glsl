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
	vec3 own_normal = imageLoad(dlss_rr_normal_roughness, ivec2(gl_LaunchIDEXT.xy)).xyz;
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
	uvec2 grid_xy = max(uvec2(gl_LaunchSizeEXT.xy) / tile, uvec2(1u));
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
// Henyey-Greenstein phase function of anisotropy g (the gas's, or a water medium's).
vec3 rt_volume_direct(vec3 x, vec3 p_dir, float g, inout uint rng) {
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
		sum += light.emission * rt_volume_hg(dot(L, p_dir), g) * lights_shadow_transmittance(light, x, L, 10000.0, 0xFFu, rng);
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
			vec3 e = plight_positional(light, x, to_l, dist) * rt_volume_hg(dot(to_l, p_dir), g);
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

// The medium of payload code p_code (MEDIUM_CHANGE_FLAG): 0 none, RT_MEDIUM_CODE_GEOMETRY plus the TLAS entry
// of an RT_MAT_FLAG_VOLUME surface, or an inline extinction code (rt_medium_absorption(); with
// RT_MEDIUM_CODE_SCATTER, the scattering the entering hit left in rt_medium_image).
void rt_medium_set(uint p_code, out bool r_in_medium, out vec3 r_sigma_t, out vec3 r_albedo, out float r_g) {
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
			vec4 scatter = rt_medium_scatter_unpack(imageLoad(rt_medium_image, ivec2(gl_LaunchIDEXT.xy)).r);
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
		// On past it: off the surface on its upper side.
		vec3 n = dot(ps.offset_normal, up) >= 0.0 ? ps.offset_normal : -ps.offset_normal;
		o = offset_ray_origin(o + up * ps.hit_t, n, ps.hit_t);
	}
	return 0u;
}

// This pixel's share of the caustics map (raytracing_caustics_inc.glsl): clear part of the slot
// the next frame writes, then trace one photon from the sun. The square is centred where the
// camera looked last frame, at least half its width ahead; the centre pixel measures that
// distance for the next frame.
void rt_caustics_photon(uvec2 p_pixel, uint p_frame, vec3 p_cam_pos, vec3 p_cam_forward) {
	uint write = rt_caustics_write_slot();
	uint clear = (write + 1u) % 3u;
	uint threads = gl_LaunchSizeEXT.x * gl_LaunchSizeEXT.y;
	uint tid = p_pixel.y * gl_LaunchSizeEXT.x + p_pixel.x;
	uint share = (RT_CAUSTICS_SLOT_UINTS + threads - 1u) / threads;
	for (uint k = 0u; k < share; k++) {
		uint i = tid * share + k;
		if (i < RT_CAUSTICS_SLOT_UINTS) {
			rt_caustics.data[clear * RT_CAUSTICS_SLOT_UINTS + i] = 0u;
		}
	}

	float half_size = 0.5 * max(get_rt_param(RT_PARAM_CAUSTICS_SIZE), 0.01);
	uint last = (write + 2u) % 3u;
	float look = rt_caustics_history() ? uintBitsToFloat(rt_caustics.data[RT_CAUSTICS_HEADER + last * 4u]) : 0.0;
	vec3 center = p_cam_pos + p_cam_forward * clamp(look, half_size, 5000.0);
	uint h = RT_CAUSTICS_HEADER + write * 4u;
	if (p_pixel == gl_LaunchSizeEXT.xy / 2u) {
		PathState look_ps;
		rt_caustics_trace(p_cam_pos, p_cam_forward, 0.0, 0u, look_ps);
		rt_caustics.data[h] = floatBitsToUint(look_ps.hit_t);
	}
	if (all(equal(p_pixel, uvec2(0u)))) {
		rt_caustics.data[h + 1u] = floatBitsToUint(center.x);
		rt_caustics.data[h + 2u] = floatBitsToUint(center.y);
		rt_caustics.data[h + 3u] = floatBitsToUint(center.z);
	}

	CausticsFrame f;
	if (!rt_caustics_frame(center, f)) {
		return;
	}
	uint rng = init_rng(p_pixel, p_frame, 7919u);
	vec2 uv = (vec2(p_pixel) + rand2(rng)) / vec2(gl_LaunchSizeEXT.xy);
	vec2 q = f.origin + uv * f.size;
	vec3 start = q.x * f.r + q.y * f.u + (f.ref_depth + RT_CAUSTICS_SUN_HEIGHT) * f.L;
	vec3 dir = -f.L;
	float width = f.size / float(RT_CAUSTICS_RES);
	PathState ps;
	// Only photons entering a medium through its front face count.
	if (!rt_caustics_trace(start, dir, width, rng, ps) || (ps.packed_bounces_flags & MEDIUM_CHANGE_FLAG) == 0u || ps.bsdf_pdf < 0.5) {
		return;
	}
	vec3 entry = start + dir * ps.hit_t;
	vec3 o = offset_ray_origin(entry, ps.offset_normal, ps.hit_t);
	vec3 p;
	if (rt_caustics_land(o, ps.next_ray_dir, width, rng, p)) {
		rt_caustics_deposit(f, write, p, 0u);
	}
	if (rt_caustics_land(o, dir, width, rng, p)) {
		rt_caustics_deposit(f, write, p, 1u);
	}
}

void main() {
#ifdef RT_TIMING_ENABLED
	uvec2 start_clock = clockRealtime2x32EXT();
#endif
	uvec2 pixel = gl_LaunchIDEXT.xy;
	const vec2 pixel_center = vec2(pixel) + vec2(0.5);
	const vec2 in_uv = pixel_center / vec2(gl_LaunchSizeEXT.xy);
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
	float composite_t_max = 10000.0;
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
		camera_medium = rt_camera_medium(camera_ray_origin);
	}

	const uint max_bounces = RT_GET_MAX_BOUNCES();
	if (rt_rc_enabled()) {
		rt_rc_resolve(pixel.y * gl_LaunchSizeEXT.x + pixel.x, gl_LaunchSizeEXT.x * gl_LaunchSizeEXT.y);
	}

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
		// The ray cone's width at the segment's origin (texture level of detail, rt_texture_cone):
		// 0 at a perspective eye, one pixel's footprint for an orthogonal camera, then grown by
		// the pixel's spread angle over each segment traced.
		float cone_width = rt_cone_eye_width();
		// The medium the ray travels in (MEDIUM_CHANGE_FLAG), starting in the camera's (rt_camera_medium()): its extinction
		// per metre, single-scatter albedo and phase anisotropy (rt_medium_set()).
		bool in_medium = false;
		vec3 medium_sigma_t = vec3(0.0);
		vec3 medium_albedo = vec3(0.0);
		float medium_g = 0.0;
		if (camera_medium != 0u) {
			rt_medium_set(camera_medium, in_medium, medium_sigma_t, medium_albedo, medium_g);
		}
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

		[[dont_unroll]] for (uint bounce = 0u; bounce <= max_bounces; bounce++) {
			vec3 throughput_entry = ps.throughput;
			vec3 radiance_pre = ps.radiance;
			uint flags_entry = ps.packed_bounces_flags;
			ps.hit_t = -1.0; // A miss must not retain the previous segment's hit distance.
			path_pack(payload, ps);
			payload_set_cone_width(payload, cone_width);
			float t_far = (bounce == 0u) ? camera_t_far : 10000.0;

#ifdef USE_SER
			hitObjectEXT hitObject;
			hitObjectTraceRayEXT(hitObject, tlas, RT_RAY_FLAGS, bounce == 0u ? RT_VIS_MASK : RT_BOUNCE_MASK, 0, 0, 0, ray_origin, 0.001, ray_dir, t_far, 0);
			reorderThreadEXT(hitObject);
			hitObjectExecuteShaderEXT(hitObject, 0);
#else
			traceRayEXT(tlas, RT_RAY_FLAGS, bounce == 0u ? RT_VIS_MASK : RT_BOUNCE_MASK, 0, 0, 0, ray_origin, 0.001, ray_dir, t_far, 0);
#endif

			ps = path_unpack(payload);

			// Simulated gas on this segment (raytracing_volume_inc.glsl). Delta tracking up to the
			// surface the segment reached: a real collision before it replaces the surface (its
			// shading is dropped) with a scattering event in the gas.
			if (rt_volume_enabled()) {
				float seg_len = ps.hit_t > 0.0 ? min(ps.hit_t, t_far) : t_far;
				vec3 vol_emitted;
				float t_vol = rt_volume_free_flight(ray_origin, ray_dir, seg_len, ps.rng_state, vol_emitted);
				if (t_vol > 0.0) {
					vec3 x = ray_origin + ray_dir * t_vol;
					vec3 albedo = rt_volume.smoke_color.rgb * rt_volume.medium.y;
					ps.radiance = radiance_pre + throughput_entry * vol_emitted;
					ps.throughput = throughput_entry * albedo;
					ps.radiance += ps.throughput * rt_volume_direct(x, ray_dir, rt_volume.medium.z, ps.rng_state);
					ps.packed_bounces_flags = inc_total_bounce(flags_entry & ~(PATH_TERMINATED_FLAG | PRIMARY_MISS_FLAG | DLSS_RR_REPLACE_PENDING_FLAG)) | NON_SINGULAR_PATH_FLAG | LAST_BOUNCE_DIFFUSE_FLAG;
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
					ps.radiance += ps.throughput * rt_volume_direct(x, ray_dir, medium_g, ps.rng_state);
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
				rt_medium_set(floatBitsToUint(ps.bsdf_pdf), in_medium, medium_sigma_t, medium_albedo, medium_g);
				ps.bsdf_pdf = 0.0;
				ps.packed_bounces_flags &= ~MEDIUM_CHANGE_FLAG;
			}

			if (ps.hit_t > 0.0) {
				cone_width += rt_cone_spread_angle() * ps.hit_t;
			}
			bool rc_recorded = false;
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
					continuation_rng = ps.rng_state;
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
					float a = get_peel_alpha(ps.packed_bounces_flags) * layer_weight;
					T_total *= (1.0 - a);
					peel_hits++;
					// Advance by one representable positive float to avoid
					// re-hitting this layer without an identity any-hit shader.
					peel_tmin = uintBitsToFloat(floatBitsToUint(ps.hit_t) + 1u);
					if ((ps.packed_bounces_flags & PEEL_DEPTH_BARRIER_HIT_FLAG) != 0u) {
						break;
					}
				}

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

		total_radiance += ps.radiance;
		composite_passthrough = composite_passthrough || (composite_enabled && is_primary_miss(ps.packed_bounces_flags));
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
			ivec2 pixel = ivec2(gl_LaunchIDEXT.xy);

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
		sky_color = texelFetch(sampler2D(sky_screen_texture, radiance_sampler), ivec2(gl_LaunchIDEXT.xy), 0).rgb;
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
	}

#ifdef DLSS_RR_ENABLED
	{
		uint total_bounces = get_total_bounces(ps.packed_bounces_flags);
		if (total_bounces == 0u && is_sample_zero(ps.packed_bounces_flags) && !composite_passthrough) {
			ivec2 pixel = ivec2(gl_LaunchIDEXT.xy);
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

#define GLSL 1
#define RT_STAGE_CLOSEST_HIT 1
// clang-format off
#include "raytracing_inc.glsl"
#include "../scene_data_inc.glsl"
#include "raytracing_hair_inc.glsl"
#include "brdf_inc.glsl"
#include "raytracing_common_inc.glsl"
// clang-format on

#define attribs hit_attribs.bary_or_uv
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
void main() {
	if ((payload.packed_bounces_flags & SSS_PROBE_FLAG) != 0u) {
		rt_sss_probe_hit();
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

	// The vertex stage may have rewritten TANGENT/BINORMAL (triplanar mapping
	// does), so the shading frame comes from the shader, not the mesh TBN.
	vec3 world_tangent = normalize(view_to_world * tangent);
	vec3 world_bitangent = normalize(view_to_world * binormal);

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
	if ((mat.flags & RT_MAT_FLAG_HAIR) != 0u) {
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
				wet *= textureLod(sampler2D(bindless_textures[nonuniformEXT(wmap)], SAMPLER_LINEAR_CLAMP), wuv, 0.0).g;
			} else {
				wet = 0.0;
			}
		}
		wet = clamp(wet, 0.0, 1.0) * smoothstep(-0.6, 0.2, h.geometry_normal.y);
		float wet_factor = mix(1.0, 0.2, (1.0 - m.metalness) * wetness.x);
		m.albedo *= mix(1.0, wet_factor, wet);
		m.roughness *= mix(1.0, wet_factor, 0.5 * wet);
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

#define GLSL 1
#define RT_STAGE_ANY_HIT 1
// clang-format off
#include "raytracing_inc.glsl"
#include "../scene_data_inc.glsl"
#include "raytracing_common_inc.glsl"
// clang-format on

#define attribs hit_attribs.bary_or_uv
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

void main() {
	uint geometry_idx = RT_HIT_GEOMETRY_IDX;
	GeometryData geom = geometries[geometry_idx];
	MaterialData mat = rt_entry_material(geometry_idx);
	bool transparent = (mat.flags & RT_MAT_FLAG_TRANSPARENT) != 0u;
	bool alpha_scissor = (mat.flags & RT_MAT_FLAG_ALPHA_SCISSOR) != 0u;
	// Thin glass lets shadow rays through; other rays hit it as an opaque surface.
	bool shadow_transmission = rt_material_feature(RT_MATERIAL_FEATURE_TRANSMISSION);
	bool shadow_back_lighting = rt_material_feature(RT_MATERIAL_FEATURE_BACK_LIGHTING);
	bool transmissive_shadow = !transparent && !alpha_scissor && is_shadow_ray(payload.packed_bounces_flags) &&
			(((mat.flags & RT_MAT_FLAG_TRANSMISSIVE) != 0u && shadow_transmission) || ((mat.flags & RT_MAT_FLAG_BACKLIT) != 0u && shadow_back_lighting) ||
					((mat.flags & RT_MAT_FLAG_CUSTOM_TRANSMITTANCE) != 0u && (shadow_transmission || shadow_back_lighting)));
	if (!transparent && !alpha_scissor && !transmissive_shadow) {
		return; // Ordinary opaque shadow candidate: accept immediately.
	}

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

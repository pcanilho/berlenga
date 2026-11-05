#[raygen]

#version 460

#extension GL_EXT_control_flow_attributes : enable

#VERSION_DEFINES

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

layout(set = 0, binding = 0, rgba16f) uniform image2D image; // RB_TEX_RAYTRACING is R16G16B16A16_SFLOAT.
layout(set = 0, binding = 1) uniform accelerationStructureEXT tlas;

layout(location = 0) rayPayloadEXT PathPayload payload;

// The volumetric fog lit through the traced scene (sun shafts): in-scatter in rgb,
// the transmittance to the primary hit in a. Its own image, filtered and laid over
// Ray Reconstruction's output; never part of the path's radiance, so RR never sees it.
layout(set = 0, binding = 29, rgba16f) uniform image2D rt_fog_image;

// As RTLightData (raytracing_lights_inc.glsl), the volumetric fog energy in _pad0.
struct FogLightData {
	vec3 position;
	uint type;
	vec3 emission;
	float radius;
	float attenuation;
	float inv_max_range;
	float max_range_squared;
	float specular_amount;
	float indirect_energy;
	float inv_spot_attenuation;
	float cos_spot_angle;
	float volumetric_energy;
	vec3 spot_direction;
	float _pad1;
};

layout(set = 0, binding = 13, std430) readonly buffer FogLightBuffer {
	FogLightData fog_lights[];
};

// The light grid (RenderRaytracing::build_light_grid, as raytracing_lights_inc.glsl
// reads it): the lights that can reach a point.
layout(set = 0, binding = 33, std430) readonly buffer FogLightGrid {
	uint fog_light_grid[];
};

// The fog volumes with a FogMaterial (RT_FogVolumeData).
struct FogVolumeData {
	vec4 inv_xform[3]; // World to local, rows of an affine.
	vec3 half_size;
	uint shape; // 0 ellipsoid, 1 cone, 2 cylinder, 3 box, 4 world.
	vec3 albedo;
	float density;
	vec3 emission;
	float height_falloff;
	vec3 origin;
	float edge_fade;
	vec3 aabb_min;
	float _pad0;
	vec3 aabb_max;
	float _pad1;
};

layout(set = 0, binding = 34, std430) readonly buffer FogVolumeBuffer {
	FogVolumeData fog_volumes[];
};

// From a point in the air towards a light: 1 clear, 0 blocked, between for a
// shadow-only caster's alpha (its any-hit keeps the transmittance in hit_t).
float fog_shadow(vec3 p_origin, vec3 p_dir, float p_max) {
	PathState shadow_ps;
	shadow_ps.radiance = vec3(0.0);
	shadow_ps.throughput = vec3(0.0);
	shadow_ps.packed_bounces_flags = set_shadow_ray(0u);
	shadow_ps.rng_state = 0u;
	shadow_ps.hit_t = 1.0;
	shadow_ps.offset_normal = vec3(0.0, 0.0, 1.0);
	shadow_ps.next_ray_dir = vec3(0.0, 0.0, 1.0);
	path_pack(payload, shadow_ps);
#ifdef USE_SER
	hitObjectEXT hitObject;
	hitObjectTraceRayEXT(hitObject, tlas, gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT,
			RT_MASK_SHADOW, 0, 0, 0, p_origin, 0.001, p_dir, p_max, 0);
	bool visible = !hitObjectIsHitEXT(hitObject);
#else
	traceRayEXT(tlas, gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT,
			RT_MASK_SHADOW, 0, 0, 0, p_origin, 0.001, p_dir, p_max, 0);
	bool visible = path_unpack(payload).radiance.x > 0.5;
#endif
	return visible ? clamp(payload.hit_t, 0.0, 1.0) : 0.0;
}

float fog_phase(float p_cos, float p_g) {
	float g2 = p_g * p_g;
	return (1.0 - g2) / (4.0 * PI * pow(max(1.0 + g2 - 2.0 * p_g * p_cos, 1e-4), 1.5));
}

// The medium at a point: extinction, scattering (per colour) and emission per metre.
// The environment's fog, plus each FogMaterial volume as the froxel fog's fog pass
// has it (volumetric_fog.glsl: the shapes' signed distances, the height falloff, the
// edge fade, the cull mask).
void fog_medium(vec3 p_x, uint p_volume_count, out float r_extinction, out vec3 r_scattering, out vec3 r_emission) {
	float env_density = get_rt_param(RT_PARAM_FOG_DENSITY);
	vec3 env_albedo = vec3(get_rt_param(RT_PARAM_FOG_ALBEDO_R), get_rt_param(RT_PARAM_FOG_ALBEDO_G), get_rt_param(RT_PARAM_FOG_ALBEDO_B));
	r_extinction = env_density;
	r_scattering = env_albedo * env_density;
	r_emission = vec3(0.0);
	for (uint i = 0u; i < p_volume_count; i++) {
		FogVolumeData v = fog_volumes[i];
		if (any(lessThan(p_x, v.aabb_min)) || any(greaterThan(p_x, v.aabb_max))) {
			continue;
		}
		vec4 xh = vec4(p_x, 1.0);
		vec3 lp = vec3(dot(v.inv_xform[0], xh), dot(v.inv_xform[1], xh), dot(v.inv_xform[2], xh));
		vec3 hs = v.half_size;
		float sdf = -1.0;
		if (v.shape == 0u) {
			float k0 = length(lp / hs);
			float k1 = length(lp / (hs * hs));
			sdf = k0 * (k0 - 1.0) / k1;
		} else if (v.shape == 1u) {
			float inv_height = 1.0 / max(0.001, hs.y);
			float radius = 1.0 / max(0.001, (min(hs.x, hs.z) * 0.5));
			float hypotenuse = sqrt(radius * radius + inv_height * inv_height);
			vec2 c = vec2(radius / hypotenuse, inv_height / hypotenuse);
			float q = length(lp.xz);
			sdf = max(dot(c, vec2(q, lp.y - hs.y)), -hs.y - lp.y);
		} else if (v.shape == 2u) {
			vec2 d = abs(vec2(length(lp.xz), lp.y)) - vec2(min(hs.x, hs.z), hs.y);
			sdf = min(max(d.x, d.y), 0.0) + length(max(d, 0.0));
		} else if (v.shape == 3u) {
			vec3 q = abs(lp) - hs;
			sdf = length(max(q, 0.0)) + min(max(q.x, max(q.y, q.z)), 0.0);
		}
		float density = v.density * clamp(exp2(-v.height_falloff * (p_x.y - v.origin.y)), 0.0, 1.0);
		if (v.shape <= 3u) {
			float cull_mask = 1.0 - smoothstep(-0.1, 0.0, sdf);
			if (cull_mask <= 0.0) {
				continue;
			}
			density *= cull_mask * pow(clamp(-2.0 * sdf / max(min(min(hs.x, hs.y), hs.z) * 2.0, 1e-4), 0.0, 1.0), v.edge_fade);
		}
		r_extinction += density;
		r_scattering += v.albedo * density;
		r_emission += v.emission * clamp(density, 0.0, 1.0);
	}
	r_extinction = max(r_extinction, 0.0);
	r_scattering = max(r_scattering, vec3(0.0));
}

// Where along [p_t0, p_t1] of the ray the light can reach: inside its range's sphere
// and its spot cone (a single nappe under 90 degrees is convex, so this is one span).
bool fog_light_span(vec3 p_ro, vec3 p_rd, FogLightData p_light, inout float r_t0, inout float r_t1) {
	vec3 co = p_ro - p_light.position;
	if (p_light.max_range_squared > 0.0) {
		float b = dot(p_rd, co);
		float c = dot(co, co) - p_light.max_range_squared;
		float disc = b * b - c;
		if (disc <= 0.0) {
			return false;
		}
		float q = sqrt(disc);
		r_t0 = max(r_t0, -b - q);
		r_t1 = min(r_t1, -b + q);
		if (r_t1 <= r_t0) {
			return false;
		}
	}
	if (p_light.type == 3u && p_light.cos_spot_angle > 0.0) {
		vec3 ax = p_light.spot_direction;
		float c2 = p_light.cos_spot_angle * p_light.cos_spot_angle;
		float da = dot(p_rd, ax);
		float ca = dot(co, ax);
		float A = da * da - c2;
		float B = 2.0 * (da * ca - dot(p_rd, co) * c2);
		float C = ca * ca - dot(co, co) * c2;
		float pts[4];
		int np = 0;
		pts[np++] = r_t0;
		if (abs(A) > 1e-7) {
			float disc = B * B - 4.0 * A * C;
			if (disc > 0.0) {
				float q = sqrt(disc);
				float ra = (-B - q) / (2.0 * A);
				float rb = (-B + q) / (2.0 * A);
				float lo = min(ra, rb);
				float hi = max(ra, rb);
				if (lo > r_t0 && lo < r_t1) {
					pts[np++] = lo;
				}
				if (hi > r_t0 && hi < r_t1) {
					pts[np++] = hi;
				}
			}
		} else if (abs(B) > 1e-7) {
			float r = -C / B;
			if (r > r_t0 && r < r_t1) {
				pts[np++] = r;
			}
		}
		pts[np++] = r_t1;
		float lo = 1e30;
		float hi = -1e30;
		for (int j = 0; j < np - 1; j++) {
			vec3 q = co + p_rd * (0.5 * (pts[j] + pts[j + 1]));
			float qa = dot(q, ax);
			if (qa >= 0.0 && qa * qa >= c2 * dot(q, q)) {
				lo = min(lo, pts[j]);
				hi = max(hi, pts[j + 1]);
			}
		}
		r_t0 = lo;
		r_t1 = hi;
	}
	return r_t1 > r_t0;
}

// What a positional light adds to the fog at a point, unshadowed and before the
// medium's scattering: its radiance through the range window, the distance and the
// cone, by the phase function.
vec3 fog_light_inscatter(FogLightData p_light, vec3 p_x, vec3 p_dir, float p_g, out vec3 r_to_light, out float r_dist) {
	vec3 to_l = p_light.position - p_x;
	float d2 = dot(to_l, to_l);
	r_dist = sqrt(d2);
	r_to_light = to_l / max(r_dist, 1e-4);
	if (p_light.max_range_squared > 0.0 && d2 > p_light.max_range_squared) {
		return vec3(0.0);
	}
	float atten = min(pow(max(r_dist, 0.0001), -p_light.attenuation), 1.0);
	if (p_light.inv_max_range >= 0.0) {
		float nd = r_dist * p_light.inv_max_range;
		nd *= nd;
		nd *= nd;
		nd = max(1.0 - nd, 0.0);
		atten *= nd * nd;
	}
	if (p_light.type == 3u) {
		float scos = dot(-r_to_light, p_light.spot_direction);
		if (scos <= p_light.cos_spot_angle) {
			return vec3(0.0);
		}
		float spot_rim = max(1e-4, (1.0 - scos) / (1.0 - p_light.cos_spot_angle));
		atten *= 1.0 - pow(spot_rim, p_light.inv_spot_attenuation);
	}
	return p_light.emission * p_light.volumetric_energy * atten * fog_phase(dot(p_dir, r_to_light), p_g);
}

#define FOG_LIGHT_CANDIDATES 8
// Per stretch, the positional-light estimates made without a shadow ray (FOG_CANDIDATES),
// and per ray, the shadow rays traced for the ones resampling keeps (FOG_SHADOW_RAYS):
// the candidate generation of Volumetric ReSTIR (Lin, Wyman & Yuksel, "Fast Volume
// Rendering with Spatiotemporal Reservoir Resampling", SIGGRAPH Asia 2021), without its
// spatial and temporal reuse.
#define FOG_CANDIDATES 8
#define FOG_SHADOW_RAYS 2

// The in-scatter from the positional lights over one stretch [p_a, p_b] of the ray,
// given a point p_t in it, uniform. Each light is integrated only where it can reach
// (fog_light_span), its distance drawn by equiangular sampling (Kulla & Fajardo,
// "Importance Sampling Techniques for Path Tracing in Participating Media", EGSR 2012),
// which puts the samples near the light, where its 1 / d^2 is: what makes a headlight's
// cone converge at a sample or two. The light is one of those that reach p_t (the light
// grid's list there), picked by resampled importance sampling (Talbot et al. 2005) by its
// unshadowed in-scatter over its span in closed form (the integral of 1 / (h^2 + s^2),
// Sun et al., "A Practical Analytic Single Scattering Model", SIGGRAPH 2005). A light
// whose span covers only part of the stretch is in the list at p_t only that part of the
// time, so its estimate is divided by that share: unbiased. Unshadowed: the point and the
// way to the light come back for fog_march, which resamples the candidates of all its
// stretches and traces shadow rays only for those it keeps.
vec3 fog_positional(vec3 p_ro, vec3 p_rd, float p_a, float p_b, float p_t, float p_tau_a, float p_extinction, uint p_volume_count, float p_g, inout uint rng, out vec3 r_x, out vec3 r_to_light, out float r_dist) {
	r_x = vec3(0.0);
	r_to_light = vec3(0.0, 1.0, 0.0);
	r_dist = 0.0;
	vec3 x = p_ro + p_rd * p_t;
	uint g_count = fog_light_grid[3];
	uint g_start = 8u;
	uint c_start = 0u;
	uint c_count = 0u;
	uvec3 dims = uvec3(fog_light_grid[0], fog_light_grid[1], fog_light_grid[2]);
	if (dims.x != 0u) {
		vec3 origin = uintBitsToFloat(uvec3(fog_light_grid[4], fog_light_grid[5], fog_light_grid[6]));
		vec3 c = floor((x - origin) * uintBitsToFloat(fog_light_grid[7]));
		if (all(greaterThanEqual(c, vec3(0.0))) && all(lessThan(c, vec3(dims)))) {
			uvec3 ci = uvec3(c);
			uint cell = ci.x + dims.x * (ci.y + dims.y * ci.z);
			uint starts = 8u + g_count;
			c_start = fog_light_grid[starts + cell];
			c_count = fog_light_grid[starts + cell + 1u] - c_start;
		}
	}
	uint n = g_count + c_count;
	if (n == 0u) {
		return vec3(0.0);
	}
	bool enumerate = n <= uint(FOG_LIGHT_CANDIDATES);
	uint m = enumerate ? n : uint(FOG_LIGHT_CANDIDATES);
	float w_scale = enumerate ? 1.0 : float(n) / float(m);
	float w_sum = 0.0;
	float sel_p_hat = 0.0;
	uint sel = 0u;
	float sel_t0 = 0.0;
	float sel_t1 = 0.0;
	for (uint i = 0u; i < m; i++) {
		uint j = enumerate ? i : min(uint(rand(rng) * float(n)), n - 1u);
		uint idx = j < g_count ? fog_light_grid[g_start + j] : fog_light_grid[c_start + (j - g_count)];
		FogLightData light = fog_lights[idx];
		if (light.type == 1u || light.volumetric_energy <= 0.0) {
			continue;
		}
		float t0 = p_a;
		float t1 = p_b;
		if (!fog_light_span(p_ro, p_rd, light, t0, t1) || p_t < t0 || p_t > t1) {
			continue;
		}
		float tc = dot(light.position - p_ro, p_rd);
		float h = max(length(p_ro + p_rd * tc - light.position), 1e-3);
		float line = (atan((t1 - tc) / h) - atan((t0 - tc) / h)) / h;
		vec3 mid = p_ro + p_rd * clamp(tc, t0, t1);
		float lum = dot(abs(light.emission), vec3(0.2126, 0.7152, 0.0722)) * light.volumetric_energy;
		float p_hat = lum * line * fog_phase(dot(p_rd, normalize(light.position - mid)), p_g);
		float w = p_hat * w_scale;
		if (!(w > 0.0)) {
			continue;
		}
		w_sum += w;
		if (rand(rng) * w_sum < w) {
			sel = idx;
			sel_p_hat = p_hat;
			sel_t0 = t0;
			sel_t1 = t1;
		}
	}
	if (w_sum <= 0.0) {
		return vec3(0.0);
	}

	FogLightData light = fog_lights[sel];
	// equiangular: t = tc + h tan(theta), theta uniform between the span's ends
	float tc = dot(light.position - p_ro, p_rd);
	float h = max(length(p_ro + p_rd * tc - light.position), 1e-3);
	float th0 = atan((sel_t0 - tc) / h);
	float th1 = atan((sel_t1 - tc) / h);
	float ts = clamp(tc + h * tan(mix(th0, th1, rand(rng))), sel_t0, sel_t1);
	float pdf = h / (max(th1 - th0, 1e-7) * (h * h + (ts - tc) * (ts - tc)));
	vec3 xs = p_ro + p_rd * ts;

	float ext_s;
	vec3 scat_s;
	vec3 emis_s;
	fog_medium(xs, p_volume_count, ext_s, scat_s, emis_s);
	if (all(lessThanEqual(scat_s, vec3(0.0)))) {
		return vec3(0.0);
	}
	vec3 to_light;
	float dist;
	vec3 inscatter = fog_light_inscatter(light, xs, p_rd, p_g, to_light, dist);
	if (all(lessThanEqual(abs(inscatter), vec3(0.0)))) {
		return vec3(0.0);
	}
	float transmittance = exp(-(p_tau_a + p_extinction * (ts - p_a)));
	// the share of the stretch this light is listed over
	float share = (sel_t1 - sel_t0) / max(p_b - p_a, 1e-6);
	r_x = xs;
	r_to_light = to_light;
	r_dist = dist;
	// unshadowed: fog_march resamples these and traces the shadow ray of the ones it keeps
	return scat_s * inscatter * transmittance / pdf / share * (w_sum / sel_p_hat);
}

// The fog along the primary ray, in-scatter in rgb and transmittance in a: K stretches
// over the part of the ray the medium can be in (the environment's fog, or the fog
// volumes' bounds), stratified with a per-pixel, per-frame offset. At each stretch's
// point: the medium, the directional lights by a shadow ray each, the ambient light and
// the emission; and the positional lights over the stretch (fog_positional). The
// transmittance is taken piecewise constant over the stretches (ray marching, Novák et
// al., "Monte Carlo Methods for Volumetric Light Transport Simulation", 2018).
vec4 fog_march(vec3 p_origin, vec3 p_dir, float p_primary_t, uint p_samples, inout uint rng) {
	float env_density = get_rt_param(RT_PARAM_FOG_DENSITY);
	uint volume_count = uint(get_rt_param(RT_PARAM_FOG_VOLUME_COUNT));
	float reach = get_rt_param(RT_PARAM_FOG_LENGTH);
	bool sky = p_primary_t < 0.0;
	float t_end = sky ? reach : min(p_primary_t, reach);
	float weight = sky ? get_rt_param(RT_PARAM_FOG_SKY_AFFECT) : 1.0;
	if (t_end <= 0.0 || (env_density <= 0.0 && volume_count == 0u)) {
		return vec4(0.0, 0.0, 0.0, 1.0);
	}
	// where the medium can be: all the way in the environment's fog, else the span of
	// the fog volumes' bounds, so the camera's own clear air takes no samples
	float t0 = 0.0;
	float t1 = t_end;
	if (env_density <= 0.0) {
		float lo = 1e30;
		float hi = -1e30;
		vec3 inv_dir = 1.0 / max(abs(p_dir), vec3(1e-8)) * sign(p_dir + vec3(1e-12));
		for (uint i = 0u; i < volume_count; i++) {
			vec3 ta = (fog_volumes[i].aabb_min - p_origin) * inv_dir;
			vec3 tb = (fog_volumes[i].aabb_max - p_origin) * inv_dir;
			vec3 tmin = min(ta, tb);
			vec3 tmax = max(ta, tb);
			float enter = max(max(tmin.x, tmin.y), tmin.z);
			float leave = min(min(tmax.x, tmax.y), tmax.z);
			if (leave > max(enter, 0.0)) {
				lo = min(lo, max(enter, 0.0));
				hi = max(hi, leave);
			}
		}
		t0 = max(lo, 0.0);
		t1 = min(hi, t_end);
	}
	if (t1 <= t0) {
		return vec4(0.0, 0.0, 0.0, 1.0);
	}

	float g = get_rt_param(RT_PARAM_FOG_ANISOTROPY);
	uint light_count = uint(get_rt_param(RT_PARAM_LIGHT_COUNT));
	vec3 ambient = vec3(get_rt_param(RT_PARAM_FOG_AMBIENT_R), get_rt_param(RT_PARAM_FOG_AMBIENT_G), get_rt_param(RT_PARAM_FOG_AMBIENT_B));
	float dx = (t1 - t0) / float(p_samples);
	float offset = rand(rng);
	vec3 scatter = vec3(0.0);
	float tau = 0.0; // optical depth to the stretch's start
	// one reservoir per shadow ray, over every stretch's positional-light candidates
	float res_w_sum[FOG_SHADOW_RAYS];
	float res_w[FOG_SHADOW_RAYS];
	vec3 res_est[FOG_SHADOW_RAYS];
	vec3 res_x[FOG_SHADOW_RAYS];
	vec3 res_l[FOG_SHADOW_RAYS];
	float res_d[FOG_SHADOW_RAYS];
	for (uint r = 0u; r < uint(FOG_SHADOW_RAYS); r++) {
		res_w_sum[r] = 0.0;
		res_w[r] = 0.0;
		res_est[r] = vec3(0.0);
		res_x[r] = vec3(0.0);
		res_l[r] = vec3(0.0, 1.0, 0.0);
		res_d[r] = 0.0;
	}
	[[dont_unroll]] for (uint i = 0u; i < p_samples; i++) {
		float a = t0 + float(i) * dx;
		float t = a + offset * dx;
		vec3 x = p_origin + p_dir * t;
		float extinction;
		vec3 scattering;
		vec3 emission;
		fog_medium(x, volume_count, extinction, scattering, emission);
		float transmittance = exp(-(tau + extinction * (t - a)));
		// the directional lights lead the light buffer
		[[dont_unroll]] for (uint li = 0u; li < light_count; li++) {
			FogLightData light = fog_lights[li];
			if (light.type != 1u) {
				break;
			}
			if (light.volumetric_energy <= 0.0 || all(lessThanEqual(scattering, vec3(0.0)))) {
				continue;
			}
			// a direction in the sun's disc, so the shafts' edges are as soft as the shadows
			vec3 axis = -normalize(light.position);
			vec2 u = rand2(rng);
			float cos_max = cos(light.radius);
			float cos_t = 1.0 - u.x * (1.0 - cos_max);
			float sin_t = sqrt(max(0.0, 1.0 - cos_t * cos_t));
			float phi = 2.0 * PI * u.y;
			vec3 tangent = normalize(abs(axis.y) < 0.999 ? cross(axis, vec3(0.0, 1.0, 0.0)) : cross(axis, vec3(1.0, 0.0, 0.0)));
			vec3 bitangent = cross(axis, tangent);
			vec3 L = normalize(tangent * (sin_t * cos(phi)) + bitangent * (sin_t * sin(phi)) + axis * cos_t);
			float v = fog_shadow(x, L, 10000.0);
			scatter += scattering * light.emission * light.volumetric_energy * fog_phase(dot(p_dir, L), g) * v * transmittance * dx;
		}
		// the sky's and the room's light, which no shadow ray stops, and the medium's own
		scatter += (scattering * ambient + emission) * transmittance * dx;
		// the positional lights: FOG_CANDIDATES unshadowed estimates of this stretch, offered to
		// the ray's reservoirs (the shadow rays come after the march, for the kept ones only)
		for (uint c = 0u; c < uint(FOG_CANDIDATES); c++) {
			float tc = c == 0u ? t : a + rand(rng) * dx;
			vec3 cx;
			vec3 cl;
			float cd;
			vec3 est = fog_positional(p_origin, p_dir, a, a + dx, tc, tau, extinction, volume_count, g, rng, cx, cl, cd) / float(FOG_CANDIDATES);
			float w = dot(max(est, vec3(0.0)), vec3(0.2126, 0.7152, 0.0722));
			if (!(w > 0.0) || isinf(w)) {
				continue;
			}
			for (uint r = 0u; r < uint(FOG_SHADOW_RAYS); r++) {
				res_w_sum[r] += w;
				if (rand(rng) * res_w_sum[r] < w) {
					res_est[r] = est;
					res_w[r] = w;
					res_x[r] = cx;
					res_l[r] = cl;
					res_d[r] = cd;
				}
			}
		}
		tau += extinction * dx;
	}
	// the kept candidates, each by its shadow ray: sum(w) / w * estimate * visibility is an
	// unbiased estimate of the whole ray's positional in-scatter (resampled importance
	// sampling of a sum, Talbot et al. 2005), averaged over the reservoirs
	for (uint r = 0u; r < uint(FOG_SHADOW_RAYS); r++) {
		if (res_w[r] > 0.0) {
			float v = fog_shadow(res_x[r], res_l[r], max(res_d[r] - 0.01, 0.002));
			scatter += res_est[r] * v * (res_w_sum[r] / res_w[r]) / float(FOG_SHADOW_RAYS);
		}
	}
	float through = exp(-tau);
	if (sky) {
		through = mix(1.0, through, weight);
	}
	return vec4(scatter * weight, through);
}

void main() {
	uvec2 pixel = gl_LaunchIDEXT.xy;
	const vec2 pixel_center = vec2(pixel) + vec2(0.5);
	const vec2 in_uv = pixel_center / vec2(gl_LaunchSizeEXT.xy);
	vec2 d = in_uv * 2.0 - 1.0;

	mat4 inv_view = transpose(mat4(scene_data_block.data.inv_view_matrix[0],
			scene_data_block.data.inv_view_matrix[1],
			scene_data_block.data.inv_view_matrix[2],
			vec4(0.0, 0.0, 0.0, 1.0)));

	vec4 origin;
	vec4 direction;
	// The ray cone's width at the origin (texture level of detail): 0 at a perspective
	// eye, one pixel's footprint for an orthogonal camera, whose rays never spread.
	float cone_width = 0.0;
	if (abs(scene_data_block.data.projection_matrix[2][3]) < 1e-6) {
		cone_width = 2.0 / (abs(scene_data_block.data.projection_matrix[1][1]) * float(gl_LaunchSizeEXT.y));
		// Orthogonal: parallel rays along the view axis, each from its own point on
		// the near plane (reverse Z: the near plane at NDC z = 1).
		vec4 near_point = scene_data_block.data.inv_projection_matrix * vec4(d.x, d.y, 1.0, 1.0);
		origin = inv_view * vec4(near_point.xyz / near_point.w, 1.0);
		direction = inv_view * vec4(0.0, 0.0, -1.0, 0.0);
	} else {
		// Perspective: every ray from the eye, through its pixel.
		vec4 target = scene_data_block.data.inv_projection_matrix * vec4(d.x, d.y, 1.0, 1.0);
		origin = inv_view * vec4(0.0, 0.0, 0.0, 1.0);
		direction = inv_view * vec4(normalize(target.xyz), 0);
	}

	// Sample count from specialization constant, frame index from uniform
	const uint samples_per_pixel = RT_GET_SAMPLE_COUNT();
	uint frame_index = uint(get_rt_param(RT_PARAM_FRAME_INDEX));

	// Accumulate multiple samples per pixel
	vec3 total_radiance = vec3(0.0);

	const uint max_bounces = RT_GET_MAX_BOUNCES();

	// TODO: when we have a spp > 0 the first raycast is always identical,
	// we should move it out of the loop

	// the primary ray's hit distance, sample 0 (-1: the sky)
	float primary_t = -1.0;

	[[dont_unroll]] for (uint sample_idx = 0u; sample_idx < samples_per_pixel; sample_idx++) {
		PathState ps;
		ps.radiance = vec3(0.0);
		ps.throughput = vec3(1.0);
		ps.packed_bounces_flags = set_cone_width((sample_idx == 0u) ? set_sample_zero(0u) : 0u, cone_width);
		ps.rng_state = init_rng(pixel, frame_index, sample_idx);

		vec3 ray_origin = origin.xyz;
		vec3 ray_dir = direction.xyz;

		[[dont_unroll]] for (uint bounce = 0u; bounce <= max_bounces; bounce++) {
			if (bounce == 0u) {
				ps.hit_t = -1.0;
			}
			path_pack(payload, ps);

#ifdef USE_SER
			hitObjectEXT hitObject;
			hitObjectTraceRayEXT(hitObject, tlas, RT_RAY_FLAGS, bounce == 0u ? RT_MASK_SCENE : RT_MASK_BOUNCE, 0, 0, 0, ray_origin, 0.001, ray_dir, 10000.0, 0);

			// Reorder with a coherence hint that has 8 bits
			uint hint = 0;
			if (hitObjectIsHitEXT(hitObject)) {
				// TODO: This hint barely does anything. There is a lot of untapped potential here.
				hint = hitObjectGetInstanceIdEXT(hitObject);
			}
			reorderThreadEXT(hitObject, hint, 8);

			hitObjectExecuteShaderEXT(hitObject, 0);
#else
			traceRayEXT(tlas, RT_RAY_FLAGS, bounce == 0u ? RT_MASK_SCENE : RT_MASK_BOUNCE, 0, 0, 0, ray_origin, 0.001, ray_dir, 10000.0, 0);
#endif

			ps = path_unpack(payload);
			if (bounce == 0u && sample_idx == 0u) {
				primary_t = ps.hit_t;
			}
			if (is_path_terminated(ps.packed_bounces_flags)) {
				break;
			}
			// Reconstruct the next ray origin from the current ray + hit distance, apply bias
			vec3 hit_pos = ray_origin + ray_dir * ps.hit_t;
			ray_origin = offset_ray_origin(hit_pos, ps.offset_normal);
			ray_dir = ps.next_ray_dir;
		}

		total_radiance += ps.radiance;
	}

	vec3 final_radiance = total_radiance / float(samples_per_pixel);

	uint fog_samples = uint(get_rt_param(RT_PARAM_FOG_SAMPLES));
	if (fog_samples > 0u) {
		uint fog_rng = init_rng(pixel, frame_index, 977u);
		vec4 fog = fog_march(origin.xyz, normalize(direction.xyz), primary_t, fog_samples, fog_rng);
#ifdef DLSS_RR_ENABLED
		// laid over Ray Reconstruction's output, filtered (_composite_transparency)
		imageStore(rt_fog_image, ivec2(pixel), fog);
#else
		// no layer to lay it in: into the frame, noisier but there
		final_radiance = final_radiance * fog.a + fog.rgb;
#endif
	}

	imageStore(image, ivec2(pixel), vec4(final_radiance, 1.0));
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
#include "brdf_inc.glsl"
#include "raytracing_common_inc.glsl"
// clang-format on

layout(location = 0) rayPayloadInEXT PathPayload payload;

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
	PathState ps = path_unpack(payload);

#if !defined(USE_SER)
	// Shadow rays that miss mean the light is visible (no occluder).
	if (is_shadow_ray(ps.packed_bounces_flags)) {
		ps.radiance = vec3(1.0);
		path_pack(payload, ps);
		return;
	}
#endif

	// Miss always ends the path.
	ps.packed_bounces_flags = set_path_terminated(ps.packed_bounces_flags);

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
		if (total_bounces == 0u && is_sample_zero(ps.packed_bounces_flags)) {
			ivec2 pixel = ivec2(gl_LaunchIDEXT.xy);

			imageStore(rt_depth_image, pixel, vec4(0.0));

			// Sky velocity: reproject a far-plane point using unjittered VPs.
			{
				vec3 far_world = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * 10000.0;
				vec2 curr_uv = project_uv(far_world, curr_vp_unjittered);
				vec2 prev_uv = project_uv(far_world, prev_vp_unjittered);
				imageStore(rt_velocity_image, pixel, vec4(prev_uv - curr_uv, 0.0, 0.0));
			}
		}
	}

	mat3 camera_basis = mat3(scene_data_block.data.inv_view_matrix);
	mat3 world_to_sky = scene_data_block.data.radiance_inverse_xform * camera_basis;
	vec3 sky_dir = world_to_sky * gl_WorldRayDirectionEXT;

	vec2 border = vec2(scene_data_block.data.radiance_border_size,
			1.0 - scene_data_block.data.radiance_border_size * 2.0);
	vec2 sky_uv = vec3_to_oct_with_border(sky_dir, border);

	vec3 sky_color = radiance_octmap_sample(sky_uv, 0.0);
	sky_color *= scene_data_block.data.IBL_exposure_normalization;

	if ((RT_FLAGS & RT_FLAG_FOG_ENABLED) != 0u) {
		vec3 fog_color = scene_data_block.data.fog_light_color;

		if (scene_data_block.data.fog_aerial_perspective > 0.0) {
			vec3 sky_fog = radiance_octmap_sample(sky_uv, 1.0 / MAX_ROUGHNESS_LOD);
			sky_fog *= scene_data_block.data.IBL_exposure_normalization;
			fog_color = mix(fog_color, sky_fog, scene_data_block.data.fog_aerial_perspective);
		}

		sky_color = mix(sky_color, fog_color, scene_data_block.data.fog_sky_affect);
	}

#ifdef DLSS_RR_ENABLED
	{
		uint total_bounces = get_total_bounces(ps.packed_bounces_flags);
		if (total_bounces == 0u && is_sample_zero(ps.packed_bounces_flags)) {
			ivec2 pixel = ivec2(gl_LaunchIDEXT.xy);
			imageStore(dlss_rr_diffuse_albedo, pixel, vec4(DLSSRR_encodeDiffuseAlbedo(sky_color), 1.0));
			imageStore(dlss_rr_specular_albedo, pixel, vec4(0.0));
			imageStore(dlss_rr_normal_roughness, pixel, vec4(-gl_WorldRayDirectionEXT, 0.0));
			imageStore(dlss_rr_specular_hit_dist, pixel, vec4(-1.0));
		}
	}
#endif

#ifdef RT_DEBUG_ENABLED
	{
		int VIS_MODE = int(get_rt_param(RT_PARAM_VIS_MODE));
		if (VIS_MODE == 20) {
			ps.radiance = vec3(1.0);
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

// clang-format off
#include "raytracing_material_eval_inc.glsl"
#include "raytracing_closest_hit_common_inc.glsl"
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
	HitData h = compute_hit_data();
	write_primary_hit_depth(h.hit_pos);
	write_primary_hit_velocity(h.hit_pos);

#ifdef RT_CUSTOM_HIT_GROUP
	uint rt_geometry_idx = h.geometry_idx;
	vec3 rt_hit_pos = h.hit_pos;
	vec2 rt_uv = h.uv;
	vec2 rt_uv2 = rt_uv;
	{
		GeometryData rt_geom = geometries[h.geometry_idx];
		if (rt_geom.uv2_byte_offset != OFFSET_NONE && rt_geom.attribute_address != 0ul && (rt_geom.flags & FLAG_PROCEDURAL) == 0u) {
			uint rt_i0, rt_i1, rt_i2;
			get_triangle_indices(rt_geom, rt_i0, rt_i1, rt_i2);
			rt_uv2 = fetch_uv2(rt_geom, rt_i0, rt_i1, rt_i2, vec3(1.0 - attribs.x - attribs.y, attribs.x, attribs.y));
		}
	}
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
	m.normal = normalize(mat3(inv_view_matrix) * normal); // view space -> world space

	// Apply normal map if it was written.
	if (normal_map != vec3(0.5, 0.5, 1.0)) {
		vec3 ts_normal;
		ts_normal.xy = normal_map.xy * 2.0 - 1.0;
		ts_normal.z = sqrt(max(0.0, 1.0 - dot(ts_normal.xy, ts_normal.xy)));
		m.normal = apply_normal_map(h, ts_normal, normal_map_depth);
	}

#ifdef RT_DEBUG_ENABLED
	{
		int VIS_MODE = int(get_rt_param(RT_PARAM_VIS_MODE));
		if (VIS_MODE != 0) {
			vec3 V = -gl_WorldRayDirectionEXT;
			float NdotV = max(dot(m.normal, V), 0.0001);
			vec3 orm = vec3(1.0, m.roughness, m.metalness);
			debug_visualize(VIS_MODE, h.geometry_normal, m.normal, normal_map,
					h.tangent, h.bitangent, h.uv, m.albedo, orm, m.metalness, m.roughness, m.specular, m.emissive, V, NdotV);
			return;
		}
	}
#endif // RT_DEBUG_ENABLED
	shade_and_bounce(h, m);
#else
	// HG0: StandardMaterial3D evaluation.
	MaterialData mat = materials[h.geometry_idx];
	vec2 uv = h.uv * mat.uv1_scale + mat.uv1_offset;
	// the ray cone's level follows the material's UV scale (triplanar sets its own)
	rt_texture_lod += 0.5 * log2(max(abs(mat.uv1_scale.x * mat.uv1_scale.y), 1e-12));
	rt_uv_dx *= mat.uv1_scale;
	rt_uv_dy *= mat.uv1_scale;
	rt_uv_aniso = (mat.flags & 64u) != 0u;

	// Triplanar materials ignore the mesh's UVs and blend three axis projections,
	// as raster's generated code does (from Puddlefunk/godot bc91da5).
	bool triplanar = (mat.flags & 16u) != 0u;
	TriplanarCoords tc;
	tc.weights = vec3(0.0);
	tc.pos = vec3(0.0);
	tc.tangent = vec3(1.0, 0.0, 0.0);
	tc.binormal = vec3(0.0, 0.0, 1.0);
	if (triplanar) {
		tc = compute_triplanar_coords(h, mat, geometries[h.geometry_idx]);
	}

	// Normal mapping.
	vec3 tangent_space_normal = vec3(0.0, 0.0, 1.0);
	vec3 final_normal = h.geometry_normal;
	if ((mat.flags & 1u) != 0u) {
		vec3 normal_sample = triplanar
				? sample_material_texture_triplanar(mat.normal_texture_idx, tc, mat.flags).rgb
				: sample_bindless_texture(mat.normal_texture_idx, uv).rgb;
		tangent_space_normal.xy = normal_sample.xy * 2.0 - 1.0;
		tangent_space_normal.z = sqrt(max(0.0, 1.0 - dot(tangent_space_normal.xy, tangent_space_normal.xy)));
		final_normal = triplanar
				? apply_normal_map_triplanar(h, tc, tangent_space_normal, mat.normal_map_depth)
				: apply_normal_map(h, tangent_space_normal, mat.normal_map_depth);
	}

	// Texture sampling.
	vec4 albedo_tex = triplanar
			? sample_material_texture_triplanar(mat.albedo_texture_idx, tc, mat.flags)
			: sample_material_texture(mat.albedo_texture_idx, uv, mat.flags);
	vec3 albedo = albedo_tex.rgb * mat.albedo_color.rgb;
	vec3 orm = (triplanar
			? sample_material_texture_triplanar(mat.orm_texture_idx, tc, mat.flags)
			: sample_material_texture(mat.orm_texture_idx, uv, mat.flags)).rgb;
	float roughness = saturate(orm.g * mat.roughness);
	float metalness = saturate(orm.b * mat.metallic);

	vec3 emissive = vec3(0.0);
	if ((mat.flags & 8u) != 0u) {
		// Emission on: its colour, times the texture if it has one.
		emissive = mat.emission_color * mat.emission_strength;
		if ((mat.flags & 2u) != 0u) {
			emissive *= (triplanar
					? sample_material_texture_triplanar(mat.emission_texture_idx, tc, mat.flags)
					: sample_material_texture(mat.emission_texture_idx, uv, mat.flags)).rgb;
		}
		emissive *= scene_data_block.data.emissive_exposure_normalization;
	}

	// Build MaterialResult.
	MaterialResult m;
	m.albedo = albedo;
	m.alpha = albedo_tex.a * mat.albedo_color.a;
	m.roughness = roughness;
	m.metalness = metalness;
	m.specular = mat.specular;
	m.emissive = emissive;
	m.normal = final_normal;

#ifdef RT_DEBUG_ENABLED
	{
		int VIS_MODE = int(get_rt_param(RT_PARAM_VIS_MODE));
		if (VIS_MODE != 0) {
			vec3 V = -gl_WorldRayDirectionEXT;
			float NdotV = max(dot(m.normal, V), 0.0001);
			debug_visualize(VIS_MODE, h.geometry_normal, final_normal, tangent_space_normal,
					h.tangent, h.bitangent, uv, albedo, orm, metalness, roughness, mat.specular, emissive, V, NdotV);
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

layout(set = 1, binding = 0) uniform texture2D bindless_textures[];

#include "raytracing_samplers_inc.glsl"

// clang-format off
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
	uint geometry_idx = gl_InstanceCustomIndexEXT;
	GeometryData geom = geometries[geometry_idx];

	uint i0, i1, i2;
	get_triangle_indices(geom, i0, i1, i2);
	vec3 bary = vec3(1.0 - attribs.x - attribs.y, attribs.x, attribs.y);

#ifdef RT_CUSTOM_HIT_GROUP
	// Compute hit data inline (cannot include closest_hit_common_inc here).
	uint rt_geometry_idx = geometry_idx;
	vec2 rt_uv = fetch_uv(geom, i0, i1, i2, bary);
	vec2 rt_uv2 = geom.uv2_byte_offset != OFFSET_NONE ? fetch_uv2(geom, i0, i1, i2, bary) : rt_uv;
	TBNResult ah_tbn = fetch_tbn(geom, i0, i1, i2, bary);

	mat3 model_rotation = mat3(gl_ObjectToWorldEXT);
	mat3 normal_matrix = mat3(
			normalize(model_rotation[0]),
			normalize(model_rotation[1]),
			normalize(model_rotation[2]));

	vec3 rt_normal = normalize(normal_matrix * ah_tbn.normal);
	vec3 rt_tangent = normalize(normal_matrix * ah_tbn.tangent);
	vec3 rt_bitangent = cross(rt_normal, rt_tangent) * ah_tbn.bitangent_sign;

	bool rt_front_face = (gl_HitKindEXT == gl_HitKindFrontFacingTriangleEXT);
	if (!rt_front_face) {
		rt_normal = -rt_normal;
	}

	vec3 rt_hit_pos = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * gl_HitTEXT;
	vec4 rt_color = fetch_color(geom, i0, i1, i2, bary);

#include "raytracing_custom_fragment_inc.glsl"

	if ((geom.flags & FLAG_SHADOW_CASTER) != 0u && is_shadow_ray(payload.packed_bounces_flags)) {
		// On a shadow ray (bounce rays meet casters too, and take them as any
		// surface below). Its alpha dims the
		// light, kept as the ray's transmittance in the payload's hit_t, and the
		// ray goes on: a soft alpha casts a soft shadow with no noise. Once next
		// to nothing is left, the hit stands and the light is blocked.
		float caster_alpha = alpha_scissor_threshold > 0.0 ? step(alpha_scissor_threshold, alpha) : clamp(alpha, 0.0, 1.0);
		payload.hit_t *= 1.0 - caster_alpha;
		if (payload.hit_t > RT_SHADOW_OPAQUE_TRANSMITTANCE) {
			ignoreIntersectionEXT;
		}
	} else {
		if (alpha_scissor_threshold > 0.0 && alpha < alpha_scissor_threshold) {
			ignoreIntersectionEXT;
		}
		// Hashed alpha: the hit passes with probability 1 - alpha, redrawn per ray and frame.
		if (alpha_scissor_threshold <= 0.0 && alpha < 1.0) {
			uint h = pcg_hash(gl_LaunchIDEXT.x + gl_LaunchIDEXT.y * 65536u);
			h = pcg_hash(h ^ floatBitsToUint(gl_HitTEXT));
			h = pcg_hash(h ^ floatBitsToUint(scene_data_block.data.time) ^ floatBitsToUint(scene_data_block.data.taa_frame_count));
			h = pcg_hash(h ^ floatBitsToUint(gl_WorldRayDirectionEXT.x + 3.0 * gl_WorldRayDirectionEXT.y));
			if (alpha < float(h) / 4294967296.0) {
				ignoreIntersectionEXT;
			}
		}
	}
#else
	// HG0: Standard material alpha test.
	vec2 uv = fetch_uv(geom, i0, i1, i2, bary);
	MaterialData mat = materials[geometry_idx];
	uv = uv * mat.uv1_scale + mat.uv1_offset;
	float alpha = texture(sampler2D(bindless_textures[nonuniformEXT(mat.albedo_texture_idx)], SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT), uv).a;
	alpha *= mat.albedo_color.a;

	if ((geom.flags & FLAG_SHADOW_CASTER) != 0u && is_shadow_ray(payload.packed_bounces_flags)) {
		// As a custom caster above, by the albedo's alpha.
		payload.hit_t *= 1.0 - clamp(alpha, 0.0, 1.0);
		if (payload.hit_t > RT_SHADOW_OPAQUE_TRANSMITTANCE) {
			ignoreIntersectionEXT;
		}
	} else if (alpha < 0.5) {
		ignoreIntersectionEXT;
	}
#endif
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
	uint rt_geometry_idx = gl_InstanceCustomIndexEXT;
	MaterialData rt_mat = materials[rt_geometry_idx];
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

	mat4 rt_aabb_xform;
	mat4 rt_inv_aabb_xform;
	get_aabb_compression_xforms(rt_geom, rt_aabb_xform, rt_inv_aabb_xform);
	read_model_matrix = mat4(gl_ObjectToWorldEXT) * rt_inv_aabb_xform;
	m_INV_MODEL_MATRIX = rt_aabb_xform * mat4(gl_WorldToObjectEXT);

	/* RT_CUSTOM_INTERSECTION_CODE */

#else
	// Base-variant fallback: never executed at runtime (the intersection
	// stage is always rebuilt per-HG with RT_CUSTOM_HIT_GROUP defined).
	// Only touch unconditional HitAttribs fields so the base variant parses.
	hit_attribs.bary_or_uv = vec2(0.0);
	reportIntersectionEXT(gl_RayTminEXT, 0u);
#endif
}

// Light sampling and Next Event Estimation (NEE) for raytracing.
// Requires: raytracing_inc.glsl, brdf_inc.glsl, tlas at binding 1, payload at location 0.
// Note: ray_query_alpha_test() still requires GL_EXT_ray_query (used by DLSS-RR path or when USE_RAY_QUERY_SHADOWS is defined).

// ============================================================================
// Light Types and Constants
// ============================================================================

#define RT_LIGHT_TYPE_OMNI 0 // Point light with radius (soft shadows)
#define RT_LIGHT_TYPE_DIRECTIONAL 1 // Sun/moon with angular size
#define RT_LIGHT_TYPE_SPOT 3 // Spot light with cone falloff

// Reservoir sampling batch size for stochastic light selection.
#ifndef RT_LIGHT_RESERVOIR_SIZE
#define RT_LIGHT_RESERVOIR_SIZE 16
#endif

// ============================================================================
// Light Data (matches C++ RT_LightData, 80 bytes, std430)
// ============================================================================

struct RTLightData {
	vec3 position; // World pos (omni/spot) or direction (directional).
	uint type; // RT_LIGHT_TYPE_*.
	vec3 emission; // HDR emission color (color * energy).
	float radius; // Light size (omni/spot) or angular radius (directional).
	float attenuation; // Attenuation exponent (2=inverse-square, 1=linear).
	float inv_max_range; // 1/range (-1 = infinite).
	float max_range_squared; // range^2 (0 = infinite).
	float specular_amount; // Godot specular multiplier [0..1].
	float indirect_energy; // Godot indirect energy multiplier.
	float inv_spot_attenuation; // Spot cone softness.
	float cos_spot_angle; // Cosine of spot cone half-angle.
	float _pad0;
	vec3 spot_direction; // Spot direction (normalized, world space).
	float _pad1;
};

// Light buffer SSBO (binding provided by the including shader via RT_LIGHT_BUFFER_BINDING).
#ifndef RT_LIGHT_BUFFER_BINDING
#define RT_LIGHT_BUFFER_BINDING 13
#endif

layout(set = 0, binding = RT_LIGHT_BUFFER_BINDING, std430) readonly buffer LightBuffer {
	RTLightData rt_lights[];
};

// The world grid of light lists (RenderRaytracing::build_light_grid): which lights
// can reach a point. In uints: [0..2] cells along x, y, z (0: none), [3] the global
// count G, [4..6] the origin and [7] 1 / cell size (floats' bits), [8, 8 + G) the
// lights in every list (directional, unbounded), then each cell's start, then the
// cells' lists.
#ifndef RT_LIGHT_GRID_BINDING
#define RT_LIGHT_GRID_BINDING 33
#endif

layout(set = 0, binding = RT_LIGHT_GRID_BINDING, std430) readonly buffer LightGridBuffer {
	uint rt_light_grid[];
};

// The lights that can reach p: the global ones at [r_global_start, + r_global_count)
// and the cell's at [r_cell_start, + r_cell_count), indices into rt_lights.
void lights_grid_lists(vec3 p, out uint r_global_start, out uint r_global_count, out uint r_cell_start, out uint r_cell_count) {
	r_global_start = 8u;
	r_global_count = rt_light_grid[3];
	r_cell_start = 0u;
	r_cell_count = 0u;
	uvec3 dims = uvec3(rt_light_grid[0], rt_light_grid[1], rt_light_grid[2]);
	if (dims.x == 0u) {
		return;
	}
	vec3 origin = uintBitsToFloat(uvec3(rt_light_grid[4], rt_light_grid[5], rt_light_grid[6]));
	float inv_cell = uintBitsToFloat(rt_light_grid[7]);
	vec3 c = floor((p - origin) * inv_cell);
	if (any(lessThan(c, vec3(0.0))) || any(greaterThanEqual(c, vec3(dims)))) {
		return;
	}
	uvec3 ci = uvec3(c);
	uint cell = ci.x + dims.x * (ci.y + dims.y * ci.z);
	uint starts = 8u + r_global_count;
	r_cell_start = rt_light_grid[starts + cell];
	r_cell_count = rt_light_grid[starts + cell + 1u] - r_cell_start;
}

// The i-th light of the union of a point's two lists.
uint lights_grid_light(uint i, uint global_start, uint global_count, uint cell_start) {
	return i < global_count ? rt_light_grid[global_start + i] : rt_light_grid[cell_start + (i - global_count)];
}

// ============================================================================
// Unified Cone Sampling (for sphere, directional, spot lights)
// ============================================================================

struct LightSample {
	vec3 cone_axis; // Direction to sample around (normalized).
	float cos_theta_max; // Cone half-angle cosine (1.0 = point, 0.0 = hemisphere).
	vec3 emission; // Light radiance.
	float distance_sq; // Squared distance to light (0 = directional).
	float max_distance; // Max shadow ray distance.
};

// Build orthonormal basis from a single direction.
void lights_build_basis(vec3 dir, out vec3 tangent, out vec3 bitangent) {
	vec3 up = abs(dir.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
	tangent = normalize(cross(up, dir));
	bitangent = cross(dir, tangent);
}

// Transform local direction to world space around axis.
vec3 lights_local_to_world(vec3 local_dir, vec3 axis) {
	vec3 tangent, bitangent;
	lights_build_basis(axis, tangent, bitangent);
	return local_dir.x * tangent + local_dir.y * bitangent + local_dir.z * axis;
}

// Prepare unified cone sample from any light type.
LightSample lights_prepare_sample(vec3 hit_pos, RTLightData light) {
	LightSample s;

	vec3 to_light = light.position - hit_pos;
	float dist_sq = dot(to_light, to_light);
	float inv_dist = inversesqrt(dist_sq + 1e-10);
	float dist = dist_sq * inv_dist;

	float is_directional = (light.type == RT_LIGHT_TYPE_DIRECTIONAL) ? 1.0 : 0.0;

	// Cone axis.
	vec3 sphere_axis = to_light * inv_dist;
	vec3 dir_axis = -normalize(light.position);
	s.cone_axis = mix(sphere_axis, dir_axis, is_directional);

	// Distance (0 for directional = no falloff).
	s.distance_sq = mix(dist_sq, 0.0, is_directional);

	// Cone angle from subtended solid angle.
	float sin_theta_sphere = clamp(light.radius * inv_dist, 0.0, 1.0);
	float cos_theta_sphere = sqrt(max(0.0, 1.0 - sin_theta_sphere * sin_theta_sphere));
	float cos_theta_dir = cos(light.radius);
	s.cos_theta_max = mix(cos_theta_sphere, cos_theta_dir, is_directional);
	s.cos_theta_max = min(s.cos_theta_max, 0.999999);

	s.emission = light.emission;

	// Max shadow ray distance.
	float sphere_max = dist + light.radius;
	float dir_max = 10000.0;
	s.max_distance = mix(sphere_max, dir_max, is_directional);

	return s;
}

// Sample a direction within the light's cone.
vec3 lights_sample_cone(LightSample ls, vec2 u, out float pdf) {
	float cos_theta = 1.0 - u.x * (1.0 - ls.cos_theta_max);
	float sin_theta = sqrt(max(0.0, 1.0 - cos_theta * cos_theta));
	float phi = 2.0 * PI * u.y;

	vec3 local_dir = vec3(sin_theta * cos(phi), sin_theta * sin(phi), cos_theta);
	vec3 L = lights_local_to_world(local_dir, ls.cone_axis);

	float solid_angle = 2.0 * PI * (1.0 - ls.cos_theta_max);
	pdf = 1.0 / max(solid_angle, 1e-10);

	return L;
}

// ============================================================================
// Attenuation
// ============================================================================

// Godot-style windowed distance attenuation.
// window = (1 - (d/range)^4)^2, combined with pow(d, -decay).
float lights_get_attenuation(LightSample ls, float inv_max_range, float decay) {
	if (ls.distance_sq <= 0.0) {
		return 1.0; // Directional: no distance attenuation.
	}

	float distance = sqrt(ls.distance_sq);
	float atten = min(pow(max(distance, 0.0001), -decay), 1.0);

	// Windowed falloff if range is finite (inv_max_range >= 0).
	if (inv_max_range >= 0.0) {
		float nd = distance * inv_max_range;
		nd *= nd;
		nd *= nd; // nd^4
		nd = max(1.0 - nd, 0.0);
		nd *= nd; // nd^2 window
		return atten * nd;
	}

	return atten;
}

// Per-light specular multiplier.
float lights_get_specular_multiplier(float specular_amount, float roughness) {
	if (specular_amount >= 0.0) {
		return specular_amount;
	} else {
		float r3 = roughness * roughness * roughness;
		return mix(0.0, r3, -specular_amount);
	}
}

// ============================================================================
// Inline Alpha Test (shared by all ray query proceed loops)
// ============================================================================

/// Inline alpha test for ray query candidates. Returns true if the hit is opaque (alpha >= 0.5).
/// Mirrors the any-hit shader logic for use with inline ray queries.
bool ray_query_alpha_test(uint geometry_idx, uint primitive_id, vec2 candidate_bary) {
	vec3 bary = vec3(1.0 - candidate_bary.x - candidate_bary.y, candidate_bary.x, candidate_bary.y);

	GeometryData geom = geometries[geometry_idx];
	uint i0, i1, i2;
	get_triangle_indices_ex(geom, primitive_id, i0, i1, i2);
	vec2 uv = fetch_uv(geom, i0, i1, i2, bary);

	MaterialData mat = materials[geometry_idx];
	uv = uv * mat.uv1_scale + mat.uv1_offset;
	float alpha = texture(sampler2D(bindless_textures[nonuniformEXT(mat.albedo_texture_idx)], SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT), uv).a;
	alpha *= mat.albedo_color.a;

	return alpha >= 0.5;
}

// ============================================================================
// Shadow Ray (traceRayEXT pipeline)
// ============================================================================

/// How much of the light arrives, 0 to 1: 0 behind an opaque hit, otherwise
/// the product of 1 - alpha over the shadow casters the ray passes, which
/// their any-hit keeps in the payload's hit_t.
/// Uses SkipClosestHitShader so only any_hit (alpha test) and miss are invoked.
/// TerminateOnFirstHit causes early exit on first confirmed opaque hit.
float lights_trace_shadow_ray(vec3 origin, vec3 direction, float max_dist, inout uint rng_state) {
#ifdef USE_RAY_QUERY_SHADOWS
	// Ray queries are significantly faster, but can not handle complex alpha materials.
	// They run no any-hit, so casters, whose alpha is their shader's, are left out.
	rayQueryEXT shadow_rq;
	rayQueryInitializeEXT(shadow_rq, tlas,
			gl_RayFlagsTerminateOnFirstHitEXT,
			RT_MASK_SCENE, origin, 0.001, direction, max_dist - 0.001);

	while (rayQueryProceedEXT(shadow_rq)) {
		if (rayQueryGetIntersectionTypeEXT(shadow_rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT) {
			// quick and dirty way to check transparency by sampling the alpha texture
			// this completely ignores the actual material, so might not be accurate
			if (ray_query_alpha_test(
						rayQueryGetIntersectionInstanceCustomIndexEXT(shadow_rq, false),
						rayQueryGetIntersectionPrimitiveIndexEXT(shadow_rq, false),
						rayQueryGetIntersectionBarycentricsEXT(shadow_rq, false))) {
				rayQueryConfirmIntersectionEXT(shadow_rq);
			}
		}
	}

	return rayQueryGetIntersectionTypeEXT(shadow_rq, true) == gl_RayQueryCommittedIntersectionNoneEXT ? 1.0 : 0.0;
#else
	// Save full payload, set up shadow ray, then restore after trace. The
	// transmittance starts at 1 in hit_t, which a shadow ray does not use.
	PathPayload saved_payload = payload;

	PathState shadow_ps;
	shadow_ps.radiance = vec3(0.0);
	shadow_ps.throughput = vec3(0.0);
	shadow_ps.packed_bounces_flags = set_shadow_ray(0u);
	shadow_ps.rng_state = 0u;
	shadow_ps.hit_t = 1.0;
	shadow_ps.offset_normal = vec3(0.0, 0.0, 1.0);
	shadow_ps.next_ray_dir = vec3(0.0, 0.0, 1.0);
	path_pack(payload, shadow_ps);

#if defined(USE_SER)
	hitObjectEXT hitObject;
	hitObjectTraceRayEXT(hitObject, tlas,
			gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT,
			RT_MASK_SHADOW, 0, 0, 0,
			origin, 0.001, direction, max_dist - 0.001, 0);
	bool visible = !hitObjectIsHitEXT(hitObject);
#else
	/// The miss shader writes radiance = vec3(1.0) for shadow rays (visible).
	/// If an opaque hit occurs, miss is never called and radiance stays vec3(0.0).
	traceRayEXT(tlas,
			gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT,
			RT_MASK_SHADOW, 0, 0, 0,
			origin, 0.001, direction, max_dist - 0.001, 0);
	bool visible = path_unpack(payload).radiance.x > 0.5;
#endif
	float transmittance = payload.hit_t;

	payload = saved_payload;

	return visible ? clamp(transmittance, 0.0, 1.0) : 0.0;
#endif
}

// ============================================================================
// Next Event Estimation (NEE) - Direct Light Sampling
// ============================================================================

// A light's unshadowed contribution at a point, roughly (luminance, distance and
// cone falloff, cosine): the target the light selection resamples by. Never 0
// where the light can add anything: sphere and disc lights get the slack of their
// size on the cosine and the cone, as a sampled direction may stray that far.
float lights_target(RTLightData light, vec3 p, vec3 N, bool is_indirect_bounce) {
	// abs: a negative light takes light away, and is picked by how much
	float lum = dot(abs(light.emission), vec3(0.2126, 0.7152, 0.0722));
	if (is_indirect_bounce) {
		lum *= max(light.indirect_energy, 0.0);
	}
	if (lum <= 0.0) {
		return 0.0;
	}
	if (light.type == RT_LIGHT_TYPE_DIRECTIONAL) {
		vec3 d = -normalize(light.position);
		return lum * clamp(dot(N, d) + sin(min(light.radius, 1.5)), 0.0, 1.0);
	}
	vec3 to_l = light.position - p;
	float d2 = dot(to_l, to_l);
	if (light.max_range_squared != 0.0 && d2 > light.max_range_squared) {
		return 0.0;
	}
	float d = sqrt(d2);
	vec3 dir = to_l / max(d, 1e-4);
	float slack = light.radius > 0.01 ? min(2.0 * light.radius / max(d, 1e-4), 1.0) : 0.0;
	float cos_n = clamp(dot(N, dir) + slack, 0.0, 1.0);
	if (cos_n <= 0.0) {
		return 0.0;
	}
	LightSample ls;
	ls.distance_sq = d2;
	float atten = lights_get_attenuation(ls, light.inv_max_range, light.attenuation);
	if (light.type == RT_LIGHT_TYPE_SPOT) {
		float scos = dot(-dir, light.spot_direction);
		if (scos <= light.cos_spot_angle - slack) {
			return 0.0;
		}
		float spot_rim = clamp((1.0 - scos) / max(1.0 - light.cos_spot_angle, 1e-4), 1e-4, 1.0);
		float spot = 1.0 - pow(spot_rim, light.inv_spot_attenuation);
		atten *= max(spot, slack > 0.0 ? 0.02 : 0.0);
	}
	// a floor so a light at its range's edge still gets picked now and then
	return lum * max(atten, 1e-6) * cos_n;
}

// Evaluate direct lighting using NEE with stochastic light selection.
// Picks the light by resampled importance sampling over the light grid.
vec3 lights_evaluate_direct_lighting(
		vec3 hit_pos,
		vec3 N,
		vec3 V,
		MaterialProperties material,
		inout uint rng_state,
		bool is_indirect_bounce,
		uint light_count) {
	if (light_count == 0u) {
		return vec3(0.0);
	}

	// Resampled importance sampling (Talbot, Cline & Egbert, "Importance Resampling
	// for Global Illumination", EGSR 2005; the candidate step of ReSTIR DI, Bitterli et
	// al. 2020): candidates drawn uniformly from the lights that can reach this point
	// (the light grid's list), each weighted by its unshadowed contribution p_hat over
	// its source pdf, one kept in proportion. The estimate f(y) / p_hat(y) * mean(w) is
	// unbiased as p_hat > 0 wherever the light can add anything. With no more lights
	// than candidates, each is taken once (w = p_hat), with less noise still.
	uint g_start, g_count, c_start, c_count;
	lights_grid_lists(hit_pos, g_start, g_count, c_start, c_count);
	uint n = g_count + c_count;
	if (n == 0u) {
		return vec3(0.0);
	}
	const bool enumerate = n <= uint(RT_LIGHT_RESERVOIR_SIZE);
	const uint m = enumerate ? n : uint(RT_LIGHT_RESERVOIR_SIZE);
	// 1 / (M * source pdf): n / M drawn, 1 enumerated
	const float w_scale = enumerate ? 1.0 : float(n) / float(m);
	float w_sum = 0.0;
	float selected_p_hat = 0.0;
	uint selected_idx = 0u;

	for (uint i = 0u; i < m; i++) {
		uint j = enumerate ? i : min(uint(rand(rng_state) * float(n)), n - 1u);
		uint idx = lights_grid_light(j, g_start, g_count, c_start);
		float p_hat = lights_target(rt_lights[idx], hit_pos, N, is_indirect_bounce);
		float w = p_hat * w_scale;
		if (w <= 0.0) {
			continue;
		}
		w_sum += w;
		if (rand(rng_state) * w_sum < w) {
			selected_idx = idx;
			selected_p_hat = p_hat;
		}
	}

	if (w_sum <= 0.0) {
		return vec3(0.0);
	}

	// The contribution's weight, as the 1 / pdf the shading below divides by.
	float light_select_pdf = selected_p_hat / w_sum;

	RTLightData light = rt_lights[selected_idx];
	vec2 u = rand2(rng_state);

	// === POSITIONAL LIGHT PATH (omni + spot) ===
	if (light.type == RT_LIGHT_TYPE_OMNI || light.type == RT_LIGHT_TYPE_SPOT) {
		vec3 to_light = light.position - hit_pos;
		float dist_sq = dot(to_light, to_light);

		// Early out: outside max range.
		if (light.max_range_squared != 0.0 && dist_sq > light.max_range_squared) {
			return vec3(0.0);
		}

		float dist = sqrt(dist_sq);
		vec3 L;
		float shadow_dist;

		if (light.radius <= 0.01) {
			// True point light: exact direction.
			L = to_light / max(dist, 0.0001);
			shadow_dist = dist;
		} else {
			// Sphere light: cone sampling for soft shadows.
			LightSample ls = lights_prepare_sample(hit_pos, light);
			float light_pdf;
			L = lights_sample_cone(ls, u, light_pdf);
			float t_center = dot(to_light, L);
			vec3 perp = to_light - t_center * L;
			float perp_sq = dot(perp, perp);
			float dt = sqrt(max(0.0, light.radius * light.radius - perp_sq));
			shadow_dist = max(0.0, t_center - dt);
		}

		// Spot cone early-out.
		float spot_atten = 1.0;
		if (light.type == RT_LIGHT_TYPE_SPOT) {
			float scos = dot(-L, light.spot_direction);
			if (scos <= light.cos_spot_angle) {
				return vec3(0.0);
			}
			float spot_rim = max(1e-4, (1.0 - scos) / (1.0 - light.cos_spot_angle));
			spot_atten = 1.0 - pow(spot_rim, light.inv_spot_attenuation);
		}

		float visibility = lights_trace_shadow_ray(hit_pos, L, shadow_dist, rng_state);
		if (visibility <= 0.0) {
			return vec3(0.0);
		}

		// Evaluate BRDF (diffuse + specular separately for specular_amount control).
		vec3 brdf_diffuse, brdf_specular;
		evalCombinedBRDFSeparate(N, L, V, material, brdf_diffuse, brdf_specular);

		// Distance attenuation.
		LightSample ls_atten;
		ls_atten.distance_sq = dist_sq;
		float atten = lights_get_attenuation(ls_atten, light.inv_max_range, light.attenuation) * spot_atten;

		float spec_mul = lights_get_specular_multiplier(light.specular_amount, material.roughness);
		vec3 brdf_value = brdf_diffuse + brdf_specular * spec_mul;

		float indirect_mul = is_indirect_bounce ? light.indirect_energy : 1.0;

		// NdotL is already included in brdf_value (evalLambertian/evalMicrofacet bake it in).
		vec3 contribution = brdf_value * light.emission * atten * indirect_mul * visibility;
		return contribution / max(light_select_pdf, 1e-10);
	}
	// === CONE LIGHT PATH (directional) ===
	else {
		LightSample ls = lights_prepare_sample(hit_pos, light);
		float light_pdf;
		vec3 L = lights_sample_cone(ls, u, light_pdf);

		float NdotL = dot(N, L);
		if (NdotL <= 0.0) {
			return vec3(0.0);
		}

		float visibility = lights_trace_shadow_ray(hit_pos, L, ls.max_distance, rng_state);
		if (visibility <= 0.0) {
			return vec3(0.0);
		}

		vec3 brdf_diffuse, brdf_specular;
		evalCombinedBRDFSeparate(N, L, V, material, brdf_diffuse, brdf_specular);

		float spec_mul = lights_get_specular_multiplier(light.specular_amount, material.roughness);
		vec3 brdf_value = brdf_diffuse + brdf_specular * spec_mul;

		float indirect_mul = is_indirect_bounce ? light.indirect_energy : 1.0;

		return brdf_value * light.emission * indirect_mul * visibility / max(light_select_pdf, 1e-10);
	}
}

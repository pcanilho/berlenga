// Requires raytracing_inc.glsl, raytracing_hit_inc.glsl and raytracing_entry_material_inc.glsl. The includer
// declares tlas, geometries[], materials[], bindless_textures[] and SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT.

// Whether thin transmissive casters let light through (rendering/pathtracing/transmission). Includers
// without the path tracer's params define it first.
#ifndef RT_SHADOW_QUERY_TRANSMISSION
#define RT_SHADOW_QUERY_TRANSMISSION rt_material_feature(RT_MATERIAL_FEATURE_TRANSMISSION)
#define RT_SHADOW_QUERY_BACK_LIGHTING rt_material_feature(RT_MATERIAL_FEATURE_BACK_LIGHTING)
#define RT_SHADOW_QUERY_REFRACTION rt_material_feature(RT_MATERIAL_FEATURE_REFRACTION)
#endif

// UV1 of a candidate hit with the material's scale and offset.
vec2 ray_query_uv(uint geometry_idx, uint primitive_id, vec2 candidate_bary, MaterialData mat) {
	vec3 bary = vec3(1.0 - candidate_bary.x - candidate_bary.y, candidate_bary.x, candidate_bary.y);

	GeometryData geom = geometries[geometry_idx];
	uint i0, i1, i2;
	get_triangle_indices_ex(geom, primitive_id, i0, i1, i2);
	return fetch_uv(geom, i0, i1, i2, bary) * mat.uv1_scale.xy + mat.uv1_offset.xy;
}

vec4 ray_query_texture(uint p_texture, vec2 uv, float lod) {
	return textureLod(sampler2D(bindless_textures[nonuniformEXT(rt_texture_index(p_texture))], SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT), uv, lod);
}

vec4 ray_query_surface(uint geometry_idx, uint primitive_id, vec2 candidate_bary, float lod) {
	MaterialData mat = rt_entry_material(geometry_idx);
	vec2 uv = ray_query_uv(geometry_idx, primitive_id, candidate_bary, mat);
	vec4 albedo = ray_query_texture(mat.albedo_texture, uv, lod);
	albedo *= mat.albedo_color;
	albedo.a = rt_entry_alpha(mat, albedo.a, 0.5);
	if ((mat.flags & RT_MAT_BLEND_CLASS_MASK) == RT_BLEND_CLASS_PREMULT && albedo.a > 1e-6) {
		albedo.rgb /= albedo.a;
	}

	return clamp(albedo, vec4(0.0), vec4(1.0));
}

float ray_query_alpha(uint geometry_idx, uint primitive_id, vec2 candidate_bary) {
	return ray_query_surface(geometry_idx, primitive_id, candidate_bary, 0.0).a;
}

bool ray_query_alpha_test(uint geometry_idx, uint primitive_id, vec2 candidate_bary) {
	return ray_query_alpha(geometry_idx, primitive_id, candidate_bary) >= 0.5;
}

// Colored transmittance along the ray, 0 when an opaque caster is hit. Transparent casters filter the light
// by their coverage, so all candidates must be visited (no opaque traversal flags).
vec3 ray_query_shadow_transmittance(vec3 origin, vec3 direction, float t_min, float t_max, uint mask, float lod, inout uint rng_state) {
	vec3 transmittance = vec3(1.0);
	rayQueryEXT shadow_rq;
	rayQueryInitializeEXT(shadow_rq, tlas, gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsNoOpaqueEXT, mask, origin, t_min, direction, t_max);

	while (rayQueryProceedEXT(shadow_rq)) {
		if (!RT_RQ_IS_TRIANGLE(shadow_rq)) {
			continue;
		}
		uint geometry_idx = RT_RQ_GEOMETRY_IDX(shadow_rq, false);
		uint mat_flags = rt_entry_material_flags(geometry_idx);
		if ((mat_flags & (RT_MAT_FLAG_TRANSPARENT | RT_MAT_FLAG_ALPHA_SCISSOR)) == 0u) {
			bool trans = (mat_flags & RT_MAT_FLAG_TRANSMISSIVE) != 0u && RT_SHADOW_QUERY_TRANSMISSION;
			bool back = (mat_flags & RT_MAT_FLAG_BACKLIT) != 0u && RT_SHADOW_QUERY_BACK_LIGHTING;
			if (trans || back) {
				// Glass and back-lit surfaces, as the any-hit shader filters them.
				uint primitive_id = RT_RQ_PRIMITIVE(shadow_rq, false);
				vec2 candidate_bary = RT_RQ_BARYCENTRICS(shadow_rq, false);
				vec4 surface = ray_query_surface(geometry_idx, primitive_id, candidate_bary, lod);
				MaterialData mat = rt_entry_material(geometry_idx);
				MaterialLayerTextures layers = material_layer_textures_none();
				if (rt_texture_index(mat.transmission_texture) != 0u || rt_texture_index(mat.backlight_texture) != 0u) {
					vec2 uv = ray_query_uv(geometry_idx, primitive_id, candidate_bary, mat);
					if (trans && rt_texture_index(mat.transmission_texture) != 0u) {
						layers.transmission = ray_query_texture(mat.transmission_texture, uv, lod).r;
					}
					if (back && rt_texture_index(mat.backlight_texture) != 0u) {
						layers.backlight = ray_query_texture(mat.backlight_texture, uv, lod).rgb;
					}
				}
				transmittance *= rt_material_shadow_transmittance(mat, surface.rgb, surface.a, layers, trans, back);
				bool front = rayQueryGetIntersectionFrontFaceEXT(shadow_rq, false) != ((geometries[geometry_idx].flags & FLAG_FACING_INVERTED) != 0u);
				if (trans && (mat_flags & RT_MAT_FLAG_VOLUME) != 0u && RT_SHADOW_QUERY_REFRACTION && !front) {
					// Leaving a medium: absorbed from the origin on (Beer-Lambert), as the any-hit shader does.
					transmittance *= rt_medium_transmittance(rt_material_medium(mat), rayQueryGetIntersectionTEXT(shadow_rq, false));
				}
				if (max(transmittance.r, max(transmittance.g, transmittance.b)) <= 1e-3) {
					rayQueryConfirmIntersectionEXT(shadow_rq);
				}
				continue;
			}
			rayQueryConfirmIntersectionEXT(shadow_rq);
			continue;
		}

		bool transparent_shadow = (mat_flags & RT_MAT_FLAG_TRANSPARENT) != 0u;
		// The albedo sample below would be meaningless, so like raster, cast no shadow. Faded opaque
		// materials only need the instance fade.
		if (transparent_shadow && (mat_flags & (RT_MAT_FLAG_NO_RAY_QUERY_ALPHA | RT_MAT_FLAG_INSTANCE_FADE)) == RT_MAT_FLAG_NO_RAY_QUERY_ALPHA) {
			continue;
		}

		vec4 surface = ray_query_surface(geometry_idx, RT_RQ_PRIMITIVE(shadow_rq, false), RT_RQ_BARYCENTRICS(shadow_rq, false), lod);
		if (transparent_shadow && surface.a < 0.999) {
			// Coverage filters the light by (1 - alpha), tinted, without a coin flip's noise.
			transmittance *= (1.0 - surface.a) * mix(vec3(1.0), surface.rgb, surface.a);
			if (max(transmittance.r, max(transmittance.g, transmittance.b)) <= 1e-3) {
				rayQueryConfirmIntersectionEXT(shadow_rq);
			}
		} else if (transparent_shadow || surface.a >= 0.5) {
			rayQueryConfirmIntersectionEXT(shadow_rq);
		}
	}

	return rayQueryGetIntersectionTypeEXT(shadow_rq, true) == gl_RayQueryCommittedIntersectionNoneEXT ? transmittance : vec3(0.0);
}

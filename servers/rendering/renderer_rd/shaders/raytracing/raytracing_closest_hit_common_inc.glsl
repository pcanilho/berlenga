// Common closest_hit utilities shared by all hit groups.
//
// Required includes (before this file):
//   raytracing_inc.glsl, brdf_inc.glsl, raytracing_hit_inc.glsl, raytracing_entry_material_inc.glsl,
//   raytracing_lights_inc.glsl, raytracing_material_eval_inc.glsl
//
// Required bindings (before this file):
//   tlas, payload, scene_data_block, geometries[], motion_indices[], materials[], motion_transforms[], bindless_textures[],
//   SAMPLER_* (12 material samplers), rt_params, rt_depth_image, rt_prefiltered_dfg() (rt_dfg),
//   DLSS-RR images (ifdef DLSS_RR_ENABLED)

// ============================================================================
// HIT DATA
// ============================================================================

struct HitData {
	vec3 hit_pos;
	vec3 geometry_normal; // World space, flipped on back faces of cull_front / cull_disabled materials (raster DO_SIDE_CHECK).
	vec3 tangent; // World space.
	vec3 bitangent; // World space.
	vec2 uv; // Raw UV (no material scale/offset applied).
	vec2 uv2; // Raw UV2 (no shader scale/offset applied).
	vec4 color; // Vertex color (white if not present).
	bool is_front_face;
	uint geometry_idx;
};

// A vertex's object-space position (as gl_ObjectToWorldEXT takes it): compressed surfaces
// store unorm16 fractions of their AABB (the BLAS geometry transform decodes them), others floats.
vec3 fetch_object_position(in GeometryData geom, uint idx) {
	Uint32Buffer vb = Uint32Buffer(geom.vertex_address);
	uint stride_words = max(geom.position_stride >> 2, 1u);
	if ((geom.flags & FLAG_COMPRESSED) != 0u) {
		uint w0 = vb.v[idx * stride_words];
		uint w1 = vb.v[idx * stride_words + 1u];
		vec3 f = vec3(float(w0 & 0xFFFFu), float(w0 >> 16), float(w1 & 0xFFFFu)) / 65535.0;
		return f * vec3(geom.aabb_size_x, geom.aabb_size_y, geom.aabb_size_z) + vec3(geom.aabb_pos_x, geom.aabb_pos_y, geom.aabb_pos_z);
	}
	return vec3(uintBitsToFloat(vb.v[idx * stride_words]), uintBitsToFloat(vb.v[idx * stride_words + 1u]), uintBitsToFloat(vb.v[idx * stride_words + 2u]));
}

// Ray cones (Akenine-Moller et al., "Texture Level of Detail Strategies for Real-Time Ray Tracing",
// Ray Tracing Gems 2019, ch. 20; "Improved Shader and Texture Level of Detail Using Ray Cones", JCGT 2021).
// Ray tracing stages have no screen-space derivatives, so without this every texture reads its base level
// and distant detail aliases into noise under the denoiser. The cone's width, carried in the payload from
// the eye, grows by the pixel's spread angle over each segment (bounces keep the eye's spread, which never
// blurs more than the paper's curvature term). Sets rt_cone_* for material_uv_compute():
// - rt_cone_footprint_log2: log2 of the footprint on the surface in world units (triplanar, scaled UVs).
// - rt_cone_uv_lod: the isotropic level in UV units, 0.5 * log2(uv area / world area) + footprint.
// - rt_cone_uv_dx/dy: the footprint ellipse's axes mapped into UV space, the derivatives raster would see.
void rt_texture_cone(in GeometryData geom, vec3 world_normal) {
	rt_cone_uv_lod = RT_TEXTURE_LOD_BASE;
	rt_cone_footprint_log2 = RT_TEXTURE_LOD_BASE;
	rt_cone_uv_grad = false;
	float width = payload_cone_width(payload, rt_cone_eye_width()) + rt_cone_spread_angle() * gl_HitTEXT;
	if ((payload.packed_bounces_flags & (CAUSTIC_PHOTON_FLAG | PEEL_RAY_FLAG)) == CAUSTIC_PHOTON_FLAG) {
		// Caustic photons start far up the sun's direction: their width is the map's texel, not a pixel's cone.
		width = payload_cone_width(payload, 0.0);
	}
	if (width <= 0.0) {
		return;
	}
	vec3 dir = gl_WorldRayDirectionEXT;
	float cos_n = max(abs(dot(world_normal, dir)), 0.05);
	rt_cone_footprint_log2 = log2(width / cos_n);

	if (geom.uv_byte_offset == OFFSET_NONE || geom.attribute_address == 0ul || geom.vertex_address == 0ul) {
		return;
	}
	uint i0, i1, i2;
	get_triangle_indices(geom, i0, i1, i2);
	mat4x3 o2w = gl_ObjectToWorldEXT;
	vec3 p0 = o2w * vec4(fetch_object_position(geom, i0), 1.0);
	vec3 p1 = o2w * vec4(fetch_object_position(geom, i1), 1.0);
	vec3 p2 = o2w * vec4(fetch_object_position(geom, i2), 1.0);
	vec2 t0 = fetch_uv(geom, i0, i1, i2, vec3(1.0, 0.0, 0.0));
	vec2 t1 = fetch_uv(geom, i0, i1, i2, vec3(0.0, 1.0, 0.0));
	vec2 t2 = fetch_uv(geom, i0, i1, i2, vec3(0.0, 0.0, 1.0));
	vec3 dp1 = p1 - p0;
	vec3 dp2 = p2 - p0;
	vec3 tri_n = cross(dp1, dp2);
	float nn = dot(tri_n, tri_n);
	vec2 e1 = t1 - t0;
	vec2 e2 = t2 - t0;
	float uv_area = abs(e1.x * e2.y - e1.y * e2.x);
	if (nn <= 1e-24 || uv_area <= 1e-14) {
		return;
	}
	rt_cone_uv_lod = 0.5 * log2(uv_area / sqrt(nn)) + rt_cone_footprint_log2;

	// Anisotropic footprint (JCGT 2021, sec. 4.3 / 5): the cone meets the triangle's plane in an ellipse,
	// its minor axis the width across the ray and its major axis width / cos along the ray's projection.
	// Mapped into UV space through the triangle's UV gradients, so textureGrad keeps detail across a grazing
	// floor instead of blurring the whole footprint to the major axis.
	vec3 c1 = cross(dp2, tri_n) / nn;
	vec3 c2 = cross(tri_n, dp1) / nn;
	vec3 grad_u = e1.x * c1 + e2.x * c2;
	vec3 grad_v = e1.y * c1 + e2.y * c2;
	vec3 plane_n = tri_n * inversesqrt(nn);
	vec3 minor_dir = cross(dir, plane_n);
	float minor_len = length(minor_dir);
	if (minor_len < 1e-4) {
		minor_dir = normalize(cross(plane_n, abs(plane_n.x) < 0.9 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0)));
	} else {
		minor_dir /= minor_len;
	}
	vec3 major_dir = cross(plane_n, minor_dir);
	float cos_plane = max(abs(dot(plane_n, dir)), 0.05);
	vec3 major_axis = major_dir * (width / cos_plane);
	vec3 minor_axis = minor_dir * width;
	rt_cone_uv_dx = vec2(dot(grad_u, major_axis), dot(grad_v, major_axis));
	rt_cone_uv_dy = vec2(dot(grad_u, minor_axis), dot(grad_v, minor_axis));
	vec4 grads = vec4(rt_cone_uv_dx, rt_cone_uv_dy);
	rt_cone_uv_grad = !any(isnan(grads)) && !any(isinf(grads));
}

// Fetch vertex attributes and transform to world space.
// Requires hitAttributeEXT HitAttribs and GeometryBuffer/MaterialBuffer bindings.
HitData compute_hit_data() {
	HitData h;
	h.geometry_idx = RT_HIT_GEOMETRY_IDX;
	GeometryData geom = geometries[h.geometry_idx];

	// Custom hit groups may reference vertex color in their fragment code, so
	// pull all attributes; default HGs can skip color for perf.
#ifdef RT_CUSTOM_HIT_GROUP
	VertexAttributes attrs = fetch_vertex_attributes(geom, attribs, FETCH_ALL);
#else
	uint uv2_fetch = (rt_entry_material_flags(h.geometry_idx) & RT_MAT_FLAG_EMISSION_UV2) != 0u ? FETCH_UV2 : 0u;
	VertexAttributes attrs = fetch_vertex_attributes(geom, attribs, FETCH_UV | FETCH_TBN | uv2_fetch);
#endif
	h.uv = attrs.uv;
	h.uv2 = attrs.uv2;
	h.color = attrs.color;

	mat3 model_rotation = mat3(gl_ObjectToWorldEXT);
	mat3 normal_matrix = mat3(
			normalize(model_rotation[0]),
			normalize(model_rotation[1]),
			normalize(model_rotation[2]));

#ifdef ENABLE_INTERSECTION_SHADERS
	if ((geom.flags & FLAG_PROCEDURAL) != 0u) {
		h.uv = hit_attribs.bary_or_uv;
		h.uv2 = h.uv;
		vec3 obj_normal = normalize(unpackSnorm4x8(hit_attribs.packed_normal).xyz);
		vec3 obj_tangent = normalize(unpackSnorm4x8(hit_attribs.packed_tangent).xyz);
		h.geometry_normal = normalize(normal_matrix * obj_normal);
		h.tangent = normalize(normal_matrix * obj_tangent);
		h.bitangent = cross(h.geometry_normal, h.tangent);

		h.is_front_face = (dot(h.geometry_normal, -gl_WorldRayDirectionEXT) > 0.0);
		if (!h.is_front_face) {
			h.geometry_normal = -h.geometry_normal;
		}
	} else
#endif
	{
		// Triangle hit: reuse `attrs` from the top-level fetch (already has
		// UV / TBN from FETCH_UV | FETCH_TBN or FETCH_ALL).
		h.geometry_normal = normalize(normal_matrix * attrs.normal);
		h.tangent = normalize(normal_matrix * attrs.tangent);
		h.bitangent = cross(h.geometry_normal, h.tangent) * attrs.bitangent_sign;

		h.is_front_face = rt_triangle_front_facing(geom);
		h.geometry_normal = rt_side_checked_normal(geom, h.geometry_normal, h.is_front_face);
	}

	h.hit_pos = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * gl_HitTEXT;

#ifdef ENABLE_INTERSECTION_SHADERS
	if ((geom.flags & FLAG_PROCEDURAL) == 0u)
#endif
	{
		rt_texture_cone(geom, h.geometry_normal);
	}

	return h;
}

// ============================================================================
// HELPERS
// ============================================================================

// Apply tangent-space normal map to geometry normal.
vec3 apply_normal_map(HitData h, vec3 tangent_space_normal, float normal_map_depth) {
	vec3 mapped = h.tangent * tangent_space_normal.x + h.bitangent * tangent_space_normal.y + h.geometry_normal * tangent_space_normal.z;
	return normalize(mix(h.geometry_normal, mapped, normal_map_depth));
}

// ============================================================================
// DEPTH WRITE (primary ray only)
// ============================================================================

void rt_store_ndc_depth(vec3 hit_pos) {
	imageStore(rt_depth_image, ivec2(gl_LaunchIDEXT.xy), vec4(rt_ndc_depth(hit_pos)));
}

// Write NDC depth for primary ray hits (bounce 0, sample 0 only).
// Peel rays defer to the dominant-layer logic in shade_and_bounce.
void write_primary_hit_depth(vec3 hit_pos) {
	if (get_total_bounces(payload.packed_bounces_flags) == 0u && is_sample_zero(payload.packed_bounces_flags) &&
			(payload.packed_bounces_flags & PEEL_RAY_FLAG) == 0u) {
		rt_store_ndc_depth(hit_pos);
	}
}

// ============================================================================
// VELOCITY WRITE (primary ray only, MV-gated)
// ============================================================================

#ifdef ENABLE_INTERSECTION_SHADERS
// Decode the FP16-compressed PREV_POSITION delta from HitAttribs.
vec3 decode_prev_pos_delta() {
	uint dx_low = (hit_attribs.packed_normal >> 24u) & 0xFFu;
	uint dx_high = (hit_attribs.packed_tangent >> 24u) & 0xFFu;
	float delta_x = unpackHalf2x16(dx_low | (dx_high << 8u)).x;
	vec2 delta_yz = unpackHalf2x16(hit_attribs.prev_pos_delta_yz);
	return vec3(delta_x, delta_yz.x, delta_yz.y);
}
#endif

// Reconstruct previous-frame mat4 from a compact motion transform entry.
mat4 decode_prev_object_to_world(int motion_idx) {
	InstanceMotionData m = motion_transforms[motion_idx];
	return transpose(mat4(
			vec4(m.prev_xform[0], m.prev_xform[1], m.prev_xform[2], m.prev_xform[3]),
			vec4(m.prev_xform[4], m.prev_xform[5], m.prev_xform[6], m.prev_xform[7]),
			vec4(m.prev_xform[8], m.prev_xform[9], m.prev_xform[10], m.prev_xform[11]),
			vec4(0.0, 0.0, 0.0, 1.0)));
}

// Previous-frame object-space position of a triangle point: deformed meshes read
// their previous vertex buffer, everything else is static in object space.
vec3 rt_prev_triangle_object_pos(GeometryData geom, uint p_primitive_id, vec2 p_bary, vec3 p_obj_pos) {
	uint64_t prev_addr = packUint2x32(uvec2(geom.prev_vertex_address_lo, geom.prev_vertex_address_hi));
	if ((geom.flags & FLAG_DEFORMED) == 0u || prev_addr == 0ul) {
		return p_obj_pos;
	}
	uint i0, i1, i2;
	get_triangle_indices_ex(geom, p_primitive_id, i0, i1, i2);
	vec3 bary = vec3(1.0 - p_bary.x - p_bary.y, p_bary.x, p_bary.y);
	FloatBuffer prev_vb = FloatBuffer(prev_addr);
	uint stride_floats = geom.position_stride >> 2;
	vec3 p0 = vec3(prev_vb.v[i0 * stride_floats + 0u],
			prev_vb.v[i0 * stride_floats + 1u],
			prev_vb.v[i0 * stride_floats + 2u]);
	vec3 p1 = vec3(prev_vb.v[i1 * stride_floats + 0u],
			prev_vb.v[i1 * stride_floats + 1u],
			prev_vb.v[i1 * stride_floats + 2u]);
	vec3 p2 = vec3(prev_vb.v[i2 * stride_floats + 0u],
			prev_vb.v[i2 * stride_floats + 1u],
			prev_vb.v[i2 * stride_floats + 2u]);
	return bary.x * p0 + bary.y * p1 + bary.z * p2;
}

// Previous model matrix: compact motion entry if the instance moved, current transform otherwise.
vec3 rt_prev_world_pos(uint p_geometry_idx, vec3 p_prev_obj_pos, mat4 p_object_to_world) {
	int mi = motion_indices[p_geometry_idx];
	mat4 prev_model = (mi >= 0) ? decode_prev_object_to_world(mi) : p_object_to_world;
	return (prev_model * vec4(p_prev_obj_pos, 1.0)).xyz;
}

// Previous-frame world position of the current closest-hit point.
vec3 rt_hit_prev_world_pos(vec3 hit_pos) {
	uint geom_idx = RT_HIT_GEOMETRY_IDX;
	GeometryData geom = geometries[geom_idx];
	vec3 prev_obj_pos = (mat4(gl_WorldToObjectEXT) * vec4(hit_pos, 1.0)).xyz;

#ifdef ENABLE_INTERSECTION_SHADERS
	if ((geom.flags & FLAG_PROCEDURAL) != 0u) {
		prev_obj_pos += decode_prev_pos_delta();
	}
#endif
#ifdef RT_HIT_ATTRIBS_DECLARED
	prev_obj_pos = rt_prev_triangle_object_pos(geom, gl_PrimitiveID, attribs, prev_obj_pos);
#endif

	return rt_prev_world_pos(geom_idx, prev_obj_pos, mat4(gl_ObjectToWorldEXT));
}

void rt_store_velocity(vec3 hit_pos) {
	imageStore(rt_velocity_image, ivec2(gl_LaunchIDEXT.xy), vec4(rt_motion_vector(hit_pos, rt_hit_prev_world_pos(hit_pos)), 0.0, 0.0));
}

// Write motion vectors for primary ray hits (bounce 0, sample 0 only).
// Peel rays defer to the dominant-layer logic in shade_and_bounce.
void write_primary_hit_velocity(vec3 hit_pos) {
	if (get_total_bounces(payload.packed_bounces_flags) != 0u || !is_sample_zero(payload.packed_bounces_flags) ||
			(payload.packed_bounces_flags & PEEL_RAY_FLAG) != 0u) {
		return;
	}
	rt_store_velocity(hit_pos);
}

// ============================================================================
// ENVIRONMENT FOG (per ray segment)
// ============================================================================

vec3 fog_get_directional_color(uint index) {
	return lights_get(index).emission;
}

vec3 fog_get_directional_direction(uint index) {
	vec3 world_dir = -normalize(rt_light_buffer[index].position);
	mat3 view_rot = transpose(mat3(
			scene_data_block.data.view_matrix[0].xyz,
			scene_data_block.data.view_matrix[1].xyz,
			scene_data_block.data.view_matrix[2].xyz));
	return view_rot * world_dir;
}

#define FOG_HAS_RADIANCE

vec3 fog_sample_radiance(vec3 vertex, float mip_level) {
	vec3 cube_view = scene_data_block.data.radiance_inverse_xform * vertex;
	vec2 border = vec2(scene_data_block.data.radiance_border_size,
			1.0 - scene_data_block.data.radiance_border_size * 2.0);
	vec2 cube_uv = vec3_to_oct_with_border(cube_view, border);
	// mip_level is a normalized roughness (0..1); radiance_octmap_sample maps it
	// onto the prefiltered roughness array layers.
	return radiance_octmap_sample(cube_uv, mip_level);
}

#include "../fog_inc.glsl"
#include "raytracing_volumetric_fog_inc.glsl"

// Analytic and, on camera segments, froxel fog at a hit, as premultiplied
// inscatter and transmittance.
vec4 rt_fog_at(vec3 world_pos, vec3 view_vertex, bool camera_segment) {
	bool use_fog = (RT_FLAGS & RT_FLAG_FOG_ENABLED) != 0u;
	vec4 fog = vec4(0.0, 0.0, 0.0, 1.0);
	if (use_fog) {
		fog = fog_process(scene_data_block.data, view_vertex);
		fog = vec4(fog.rgb * fog.a, 1.0 - fog.a);
	}
	if (camera_segment && rt_volumetric_fog_enabled()) {
		fog = rt_combine_fog(fog, use_fog, rt_volumetric_fog_at(world_pos, view_vertex));
	}
	return fog;
}

vec3 rt_world_to_view(vec3 p_world) {
	mat4 view_mat = transpose(mat4(
			scene_data_block.data.view_matrix[0],
			scene_data_block.data.view_matrix[1],
			scene_data_block.data.view_matrix[2],
			vec4(0.0, 0.0, 0.0, 1.0)));
	return (view_mat * vec4(p_world, 1.0)).xyz;
}

// Apply environment fog for the ray segment that was just traversed.
// Attenuates throughput and adds in-scattered fog color.
void apply_segment_fog(float segment_dist, uint mat_flags, bool camera_segment, inout vec3 radiance, inout vec3 throughput) {
	if ((mat_flags & RT_MAT_FLAG_FOG_DISABLED) != 0u) {
		return;
	}
	if ((RT_FLAGS & RT_FLAG_FOG_ENABLED) == 0u && !(camera_segment && rt_volumetric_fog_enabled())) {
		return;
	}

	vec3 world_hit = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * segment_dist;
	vec4 fog = rt_fog_at(world_hit, rt_world_to_view(world_hit), camera_segment);
	radiance += throughput * fog.rgb;
	throughput *= fog.a;
}

// ============================================================================
// BRDF MATERIAL SETUP
// ============================================================================

// BRDF inputs for a surface. 'p_mat_flags' supplies the Godot light model and
// 'p_NdotV' the rasterizer's DFG energy compensation. 'p_clearcoat' (0 = none) and
// 'p_clearcoat_roughness' describe a coat on 'p_coat_normal', the unbumped normal.
MaterialProperties material_properties_coated(vec3 p_albedo, float p_metallic, float p_roughness, float p_specular,
		vec3 p_backlight, uint p_mat_flags, float p_NdotV, float p_clearcoat, float p_clearcoat_roughness, vec3 p_coat_normal) {
	MaterialProperties material;
	material.baseColor = p_albedo;
	material.metalness = p_metallic;
	material.roughness = p_roughness;
	material.dielectricF0 = material_specular_to_f0(p_specular);
	material.emissive = vec3(0.0);
	material.transmissivness = 0.0;
	material.opacity = 1.0;
	material.clearcoat = p_clearcoat;
	material.clearcoatAlpha = clearcoatAlphaFromRoughness(p_clearcoat_roughness);
	material.coatNormal = p_coat_normal;
	vec3 f0 = materialSpecularF0(material);
	float dfg_y = rt_prefiltered_dfg(p_roughness, clamp(p_NdotV, 0.0001, 1.0)).y;
	material.energyCompensation = 1.0 + f0 * (1.0 / dfg_y - 1.0);
	material.backlight = p_backlight;
	material.diffuseMode = (p_mat_flags >> RT_MAT_DIFFUSE_MODE_SHIFT) & RT_MAT_SHADING_MODE_MASK;
	material.specularMode = (p_mat_flags >> RT_MAT_SPECULAR_MODE_SHIFT) & RT_MAT_SHADING_MODE_MASK;
	material.subsurface = 0.0;
	material.hair = 0.0;
	material.hairSigmaA = vec3(0.0);
	material.hairTangent = vec3(1.0, 0.0, 0.0);
	material.hairPlaneNormal = p_coat_normal;
	material.hairParams = vec4(0.3, 0.3, 2.0, 0.0);
	material.hairCards = false;
	return material;
}

// The same without a coat.
MaterialProperties material_properties(vec3 p_albedo, float p_metallic, float p_roughness, float p_specular,
		vec3 p_backlight, uint p_mat_flags, float p_NdotV) {
	return material_properties_coated(p_albedo, p_metallic, p_roughness, p_specular, p_backlight, p_mat_flags, p_NdotV,
			0.0, 0.0, vec3(0.0, 0.0, 1.0));
}

MaterialProperties material_properties_from_result(HitData h, MaterialResult m, vec3 N, vec3 V) {
	MaterialProperties material = material_properties_coated(m.albedo, m.metalness, m.roughness, m.specular, m.backlight,
			rt_entry_material_flags(h.geometry_idx), dot(N, V), m.clearcoat, m.clearcoat_roughness, h.geometry_normal);
	material.emissive = m.emissive;
	material.transmissivness = m.transmission;
	material.subsurface = m.subsurface > 0.0 ? 1.0 : 0.0;
	material.hair = m.hair;
	material.hairSigmaA = m.hair_sigma_a;
	material.hairTangent = m.hair_tangent;
	material.hairPlaneNormal = h.geometry_normal;
	material.hairParams = m.hair_params;
	material.hairCards = m.hair_cards;
	return material;
}

// Shadow-caster groups p_geometry_idx receives shadows from: only SELF_SHADOWING receivers keep the SELF_SHADOWING group.
uint rt_receiver_shadow_mask(uint p_geometry_idx) {
	if ((geometries[p_geometry_idx].flags & FLAG_SELF_SHADOWING) != 0u) {
		return 0xFFu;
	}
	return ~uint(get_rt_param(RT_PARAM_SELF_SHADOW_MASK)) & 0xFFu;
}

// ============================================================================
// DEBUG VISUALIZATION
// ============================================================================

#ifdef RT_DEBUG_ENABLED
#define RT_VIS_DIRECTIONAL_SHADOW_BLOCKER 27

bool debug_is_camera_visible(uint p_geometry_idx, vec3 p_origin, vec3 p_direction, float p_t) {
	rayQueryEXT rq;
	rayQueryInitializeEXT(rq, tlas, gl_RayFlagsNoOpaqueEXT, RT_VIS_MASK, p_origin, max(p_t - 0.01, 0.0), p_direction, p_t + 0.01);
	while (rayQueryProceedEXT(rq)) {
		if (RT_RQ_GEOMETRY_IDX(rq, false) == p_geometry_idx) {
			rayQueryConfirmIntersectionEXT(rq);
		}
	}
	return rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT;
}

// White: sun visible. Yellow: only transparent casters (scaled by transmittance).
// Red: opaque blocker the camera cannot see (raster ignores it). Blue: opaque blocker the camera sees.
// Darker means a farther blocker. Gray: facing away from the sun. Magenta: no directional light.
vec3 debug_directional_shadow_blocker(vec3 p_geometry_normal, uint p_receiver_shadow_mask) {
	uint light_count = uint(get_rt_param(RT_PARAM_LIGHT_COUNT));
	int sun = -1;
	for (uint i = 0u; i < light_count; i++) {
		if (lights_get_type(i) == RT_LIGHT_TYPE_DIRECTIONAL) {
			sun = int(i);
			break;
		}
	}
	if (sun < 0) {
		return vec3(1.0, 0.0, 1.0);
	}
	vec3 to_sun = -normalize(rt_light_buffer[sun].position);
	if (dot(p_geometry_normal, to_sun) <= 0.0) {
		return vec3(0.1);
	}

	vec3 origin = offset_ray_origin(gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * gl_HitTEXT, p_geometry_normal, gl_HitTEXT);
	vec3 transmittance = vec3(1.0);
	rayQueryEXT rq;
	rayQueryInitializeEXT(rq, tlas, gl_RayFlagsNoOpaqueEXT, RT_SHADOW_GROUPS_MASK & p_receiver_shadow_mask, origin, 0.001, to_sun, 10000.0);
	while (rayQueryProceedEXT(rq)) {
		uint geometry_idx = RT_RQ_GEOMETRY_IDX(rq, false);
		uint mat_flags = rt_entry_material_flags(geometry_idx);
		if ((mat_flags & (RT_MAT_FLAG_TRANSPARENT | RT_MAT_FLAG_ALPHA_SCISSOR)) == 0u) {
			rayQueryConfirmIntersectionEXT(rq);
			continue;
		}
		bool transparent = (mat_flags & RT_MAT_FLAG_TRANSPARENT) != 0u;
		if (transparent && (mat_flags & (RT_MAT_FLAG_NO_RAY_QUERY_ALPHA | RT_MAT_FLAG_INSTANCE_FADE)) == RT_MAT_FLAG_NO_RAY_QUERY_ALPHA) {
			continue;
		}
		vec4 surface = ray_query_surface(geometry_idx, rayQueryGetIntersectionPrimitiveIndexEXT(rq, false), rayQueryGetIntersectionBarycentricsEXT(rq, false), 0.0);
		if (transparent) {
			// Expected value of ray_query_shadow_transmittance's stochastic coverage.
			transmittance *= (1.0 - surface.a) * mix(vec3(1.0), surface.rgb, surface.a);
		} else if (surface.a >= 0.5) {
			rayQueryConfirmIntersectionEXT(rq);
		}
	}

	if (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT) {
		return transmittance == vec3(1.0) ? vec3(1.0) : vec3(1.0, 1.0, 0.0) * luminance(transmittance);
	}
	float t = rayQueryGetIntersectionTEXT(rq, true);
	uint blocker = RT_RQ_GEOMETRY_IDX(rq, true);
	float brightness = clamp(1.0 - log2(1.0 + t) / 8.0, 0.25, 1.0);
	vec3 color = debug_is_camera_visible(blocker, origin, to_sun, t) ? vec3(0.0, 0.3, 1.0) : vec3(1.0, 0.0, 0.0);
	return color * brightness;
}

// Must match RTDynamicGeometryDebug.
#define RT_DYNAMIC_GEOMETRY_CATEGORY_MAX 7u
#define RT_DYNAMIC_GEOMETRY_CATEGORY_MASK 15u
#define RT_DYNAMIC_GEOMETRY_REST_SHIFT 4u
#define RT_DYNAMIC_GEOMETRY_AGE_SHIFT 8u
#define RT_DYNAMIC_GEOMETRY_FADE_FRAMES 60.0

// Indexed by RTDynamicGeometryDebug::Category: static, moved, refit, build, captured refit, captured build,
// built idle, captured built idle.
const vec3 RT_DYNAMIC_GEOMETRY_COLORS[8] = vec3[](vec3(0.0), vec3(0.0, 1.0, 0.0), vec3(1.0, 1.0, 0.0), vec3(1.0, 0.0, 0.0), vec3(0.05, 0.15, 1.0), vec3(1.0, 0.0, 1.0),
		vec3(0.25, 0.015, 0.015), vec3(0.22, 0.015, 0.22));
const float RT_DYNAMIC_GEOMETRY_STRENGTH[8] = float[](0.0, 0.4, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0);

// Category color of the packed category at p_shift, over p_base.
vec3 debug_dynamic_geometry_tint(uint p_state, uint p_shift, vec3 p_base, float p_recency) {
	uint category = min((p_state >> p_shift) & RT_DYNAMIC_GEOMETRY_CATEGORY_MASK, RT_DYNAMIC_GEOMETRY_CATEGORY_MAX);
	return mix(p_base, RT_DYNAMIC_GEOMETRY_COLORS[category], RT_DYNAMIC_GEOMETRY_STRENGTH[category] * p_recency);
}

// Category color over the rest category's color, itself over dim grayscale shading. Work done this frame
// (age 0) is opaque; earlier work starts well below that and fades out over RT_DYNAMIC_GEOMETRY_FADE_FRAMES.
vec3 debug_dynamic_geometry(uint p_state, vec3 p_albedo, float p_NdotV) {
	uint age = p_state >> RT_DYNAMIC_GEOMETRY_AGE_SHIFT;
	vec3 gray = vec3((0.02 + 0.13 * luminance(p_albedo)) * (0.3 + 0.7 * p_NdotV));
	vec3 rest = debug_dynamic_geometry_tint(p_state, RT_DYNAMIC_GEOMETRY_REST_SHIFT, gray, 1.0);
	float recency = age == 0u ? 1.0 : mix(0.6, 0.1, float(age) / RT_DYNAMIC_GEOMETRY_FADE_FRAMES);
	return debug_dynamic_geometry_tint(p_state, 0u, rest, recency);
}

// Opacity range of transparent layers: faint particles stay visible, near-opaque ones still show what's behind.
#define RT_DYNAMIC_GEOMETRY_MIN_OPACITY 0.35
#define RT_DYNAMIC_GEOMETRY_MAX_OPACITY 0.75

// Reports a transparency peel layer of p_color, like shade_and_bounce does: raygen composites it over what's behind.
void debug_dynamic_geometry_peel(inout PathState r_ps, vec3 p_color, float p_alpha) {
	float opacity = clamp(p_alpha, RT_DYNAMIC_GEOMETRY_MIN_OPACITY, RT_DYNAMIC_GEOMETRY_MAX_OPACITY);
	r_ps.hit_t = gl_HitTEXT;
	r_ps.radiance += r_ps.throughput * opacity * p_color;
	r_ps.packed_bounces_flags = set_peel_alpha(r_ps.packed_bounces_flags, opacity) & ~PEEL_HAS_CONTINUATION_FLAG;
	path_pack(payload, r_ps);
}

// Debug inputs of a closest hit. p_tangent_space_normal is the unpacked normal map sample.
MaterialDebugInputs debug_material_inputs(HitData p_h, MaterialResult p_m, vec3 p_tangent_space_normal, vec3 p_tangent, vec3 p_bitangent, vec2 p_uv, float p_ao) {
	MaterialDebugInputs inputs;
	inputs.geometry_normal = p_h.geometry_normal;
	inputs.normal = p_m.normal;
	inputs.view_normal = normalize(mat3(scene_data_block.data.inv_view_matrix) * p_m.normal);
	inputs.tangent_space_normal = p_tangent_space_normal;
	inputs.tangent = p_tangent;
	inputs.bitangent = p_bitangent;
	inputs.uv = p_uv;
	inputs.albedo = p_m.albedo;
	inputs.ao = p_ao;
	inputs.roughness = p_m.roughness;
	inputs.metallic = p_m.metalness;
	inputs.specular = p_m.specular;
	inputs.emission = p_m.emissive;
	inputs.NdotV = max(dot(p_m.normal, -gl_WorldRayDirectionEXT), 0.0001);
	inputs.front_facing = p_h.is_front_face;
	inputs.depth = gl_HitTEXT / (scene_data_block.data.z_far - scene_data_block.data.z_near);
	return inputs;
}

void debug_visualize(int vis_mode, MaterialDebugInputs dbg, float alpha) {
	PathState ps = path_unpack(payload);
	vec3 V = -gl_WorldRayDirectionEXT;

	if (material_debug_is_material_mode(vis_mode)) {
		ps.radiance = material_debug_color(vis_mode, dbg);
	} else if (vis_mode == 1) {
		if (get_total_bounces(ps.packed_bounces_flags) == 0u) {
			ps.packed_bounces_flags = inc_total_bounce(ps.packed_bounces_flags);
			ps.hit_t = gl_HitTEXT;
			ps.offset_normal = dbg.geometry_normal;
			ps.next_ray_dir = reflect(gl_WorldRayDirectionEXT, dbg.geometry_normal);
			path_pack(payload, ps);
			return;
		} else {
			ps.radiance = dbg.geometry_normal * 0.5 + 0.5;
		}
	} else if (vis_mode == 13) {
		if (get_total_bounces(ps.packed_bounces_flags) == 0u) {
			if (dbg.roughness < MAX_DENOISER_SPECULAR_HIT_THRESHOLD) {
				ps.packed_bounces_flags = inc_total_bounce(ps.packed_bounces_flags);
				ps.radiance = vec3(0.1, 0.1, 0.4);
				ps.hit_t = gl_HitTEXT;
				ps.offset_normal = dbg.normal;
				ps.next_ray_dir = reflect(gl_WorldRayDirectionEXT, dbg.normal);
				path_pack(payload, ps);
				return;
			} else {
				ps.radiance = vec3(0.1, 0.1, 0.4);
			}
		} else {
			float spec_hit_t = gl_HitTEXT;
			float v = clamp(log(spec_hit_t + 1.0) / log(1000.0), 0.0, 1.0);
			vec3 color;
			if (v < 0.33) {
				color = mix(vec3(0.0, 0.0, 0.0), vec3(1.0, 0.0, 0.0), v * 3.0);
			} else if (v < 0.66) {
				color = mix(vec3(1.0, 0.0, 0.0), vec3(1.0, 1.0, 0.0), (v - 0.33) * 3.0);
			} else {
				color = mix(vec3(1.0, 1.0, 0.0), vec3(1.0, 1.0, 1.0), (v - 0.66) * 3.0);
			}
			ps.radiance = color;
		}
	} else if (vis_mode == RT_VIS_DIRECTIONAL_SHADOW_BLOCKER) {
		ps.radiance = debug_directional_shadow_blocker(dbg.geometry_normal, rt_receiver_shadow_mask(RT_HIT_GEOMETRY_IDX));
	} else if (vis_mode == RT_VIS_DYNAMIC_GEOMETRY) {
		vec3 color = debug_dynamic_geometry(geometries[RT_HIT_GEOMETRY_IDX].debug_state, dbg.albedo, dbg.NdotV);
		if ((ps.packed_bounces_flags & PEEL_RAY_FLAG) != 0u) {
			debug_dynamic_geometry_peel(ps, color, alpha);
			return;
		}
		ps.radiance = color;
	} else if (vis_mode == RT_VIS_VOLUMETRIC_FOG || vis_mode == RT_VIS_VOLUMETRIC_FOG_TRANSMITTANCE) {
		vec3 world_hit = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * gl_HitTEXT;
		ps.radiance = rt_volumetric_fog_debug(vis_mode, rt_volumetric_fog_at(world_hit, rt_world_to_view(world_hit)));
	} else if (vis_mode == 22) {
		// BRDF below-hemisphere fallback visualization. Reflects the two-layer
		// recovery applied in shade_and_bounce:
		//   Layer 1 - shading-normal clamp toward geometry (energy preserving
		//             but flattens detail at grazing angles)
		//   Layer 2 - mirror rejected directions across the geometry plane
		//             (biased; reuses the rejected sample's BRDF weight)
		//
		//
		// Color legend (ordered cleanest -> most biased):
		//   green   = no rejection in either pass (no fallback needed)
		//   blue    = clamp alone resolves rejection (energy preserving)
		//   yellow  = clamp + mirror (one lobe still mirrored after clamp)
		//   red     = clamp + mirror (both lobes mirrored - most biased pixel)
		//
		// Production output is never black anymore -- yellow/red just indicate
		// where the cheaper Layer-1 clamp could not catch the bump and the
		// biased Layer-2 mirror had to step in. Raise
		// RT_SHADING_NORMAL_CLAMP_THRESHOLD if yellow/red dominate.
		vec3 final_normal = dbg.normal;
		vec3 geometry_normal = dbg.geometry_normal;
		MaterialProperties dbg_mat = material_properties(dbg.albedo, dbg.metallic, dbg.roughness, dbg.specular,
				vec3(0.0), rt_entry_material_flags(RT_HIT_GEOMETRY_IDX), dbg.NdotV);

		vec3 dbg_dir;
		vec3 dbg_weight;
		// Draw both random pairs up front so each (specular/diffuse) test in
		// the un-clamped and clamped pass uses the exact same numbers.
		vec2 u_spec_rng = rand2(ps.rng_state);
		vec2 u_diff_rng = rand2(ps.rng_state);

		bool u_spec_ok = evalIndirectCombinedBRDF(u_spec_rng, final_normal, geometry_normal, V, dbg_mat, SPECULAR_TYPE, dbg_dir, dbg_weight, vec4(0.0));
		bool u_diff_ok = evalIndirectCombinedBRDF(u_diff_rng, final_normal, geometry_normal, V, dbg_mat, DIFFUSE_TYPE, dbg_dir, dbg_weight, vec4(0.0));

		// Uses the same threshold as shade_and_bounce so the visualization
		// stays in sync with production sampling.
		vec3 N_clamped = clampShadingNormal(final_normal, geometry_normal, V, RT_SHADING_NORMAL_CLAMP_THRESHOLD);
		bool c_spec_ok = evalIndirectCombinedBRDF(u_spec_rng, N_clamped, geometry_normal, V, dbg_mat, SPECULAR_TYPE, dbg_dir, dbg_weight, vec4(0.0));
		bool c_diff_ok = evalIndirectCombinedBRDF(u_diff_rng, N_clamped, geometry_normal, V, dbg_mat, DIFFUSE_TYPE, dbg_dir, dbg_weight, vec4(0.0));

		bool all_uncl_ok = u_spec_ok && u_diff_ok;
		bool all_cl_ok = c_spec_ok && c_diff_ok;
		bool any_cl_ok = c_spec_ok || c_diff_ok;

		if (all_uncl_ok) {
			ps.radiance = vec3(0.0, 1.0, 0.0);
		} else if (all_cl_ok) {
			ps.radiance = vec3(0.0, 0.4, 1.0);
		} else if (any_cl_ok) {
			ps.radiance = vec3(1.0, 1.0, 0.0);
		} else {
			ps.radiance = vec3(1.0, 0.0, 0.0);
		}
	}

	ps.packed_bounces_flags = set_path_terminated(ps.packed_bounces_flags);
	path_pack(payload, ps);
}
#endif // RT_DEBUG_ENABLED

// ============================================================================
// SHADE AND BOUNCE
// ============================================================================

bool surface_is_unshaded(uint geometry_idx) {
#ifdef MODE_UNSHADED
	return true;
#else
	return (rt_entry_material_flags(geometry_idx) & RT_MAT_FLAG_UNSHADED) != 0u;
#endif
}

// Ambient and reflection lighting for max_bounces == 0, which never traces the
// sky. Mirrors the rasterizer (scene_forward_clustered.glsl): ambient source,
// sky contribution, reflection source, specular occlusion and DFG weighting.
vec3 ambient_approx(vec3 N, vec3 V, MaterialResult m, MaterialProperties material) {
	uint flags = scene_data_block.data.flags;
	vec4 ambient_color_energy = scene_data_block.data.ambient_light_color_energy;
	float sky_scale = scene_data_block.data.IBL_exposure_normalization * ambient_color_energy.a;
	float roughness = material.roughness;

	vec3 ambient = vec3(0.0);
	if ((flags & SCENE_DATA_FLAGS_USE_AMBIENT_LIGHT) != 0u) {
		ambient = ambient_color_energy.rgb;
		if ((flags & SCENE_DATA_FLAGS_USE_AMBIENT_CUBEMAP) != 0u) {
			vec3 sky_ambient = radiance_octmap_sample(rt_sky_oct_uv(N), 1.0) * sky_scale;
			ambient = mix(ambient, sky_ambient, scene_data_block.data.ambient_color_sky_mix);
		}
	}

	vec3 specular = vec3(0.0);
	if ((flags & SCENE_DATA_FLAGS_USE_REFLECTION_CUBEMAP) != 0u) {
		vec3 ref_vec = mix(reflect(-V, N), N, roughness * roughness);
		float horizon = min(1.0 + dot(ref_vec, N), 1.0);
		specular = radiance_octmap_sample(rt_sky_oct_uv(ref_vec), sqrt(roughness)) * sky_scale * horizon * horizon;

		float specular_occlusion = min(dot(ambient, vec3(0.3, 0.59, 0.11)) * 8.0, 1.0);
		float reflective_f = (1.0 - roughness) * material.metalness;
		specular_occlusion = max(min(reflective_f * specular_occlusion * 10.0, 1.0), specular_occlusion);

		specular *= specular_occlusion;
		if (material.diffuseMode == GODOT_DIFFUSE_TOON) {
			specular *= m.specular * material.metalness * material.baseColor * 2.0;
		} else {
			vec3 f0 = materialSpecularF0(material);
			vec2 env_brdf = rt_prefiltered_dfg(roughness, clamp(dot(N, V), 0.0001, 1.0));
			float f90 = clamp(50.0 * f0.g, material.metalness, 1.0);
			specular *= material.energyCompensation * ((f90 - f0) * env_brdf.x + f0 * env_brdf.y);
		}
	}

	vec3 diffuse = materialDiffuseReflectance(material) * ambient;
	if (material.clearcoat > 0.0 && material.diffuseMode != GODOT_DIFFUSE_TOON) {
		// The coat's sky reflection on the vertex normal, weighted by its Fresnel (raster cc_specular_light).
		float coat_fresnel = clearcoatFresnel(material.clearcoat, dot(material.coatNormal, V));
		diffuse *= 1.0 - coat_fresnel;
		specular *= 1.0 - coat_fresnel;
		if ((flags & SCENE_DATA_FLAGS_USE_REFLECTION_CUBEMAP) != 0u) {
			vec3 cc_ref_vec = mix(reflect(-V, material.coatNormal), material.coatNormal, material.clearcoatAlpha);
			specular += radiance_octmap_sample(rt_sky_oct_uv(cc_ref_vec), sqrt(material.clearcoatAlpha)) * sky_scale * coat_fresnel;
		}
	}
	return diffuse + specular;
}

// ============================================================================
// EMISSIVE SURFACES AS LIGHTS (RenderRaytracing::append_emissive_lights)
// ============================================================================
// The emissive section of the light grid buffer: [9] its start E (0: none); at E the entry
// count N and the geometry map size G, then N entries of 16 uints, then G uints (entry + 1).
#define RT_EMISSIVE_ENTRY_UINTS 16u
#define RT_EMISSIVE_HEADER 4u

uint rt_emissive_count() {
	uint b = rt_light_grid[9];
	return b == 0u ? 0u : rt_light_grid[b];
}

// The entry sampling geometry p_geometry_idx, plus 1; 0 when it is not sampled as a light.
uint rt_emissive_entry_of(uint p_geometry_idx) {
	uint b = rt_light_grid[9];
	if (b == 0u) {
		return 0u;
	}
	uint n = rt_light_grid[b];
	if (p_geometry_idx >= rt_light_grid[b + 1u]) {
		return 0u;
	}
	return rt_light_grid[b + RT_EMISSIVE_HEADER + n * RT_EMISSIVE_ENTRY_UINTS + p_geometry_idx];
}

uint rt_emissive_entry_base(uint p_entry) {
	return rt_light_grid[9] + RT_EMISSIVE_HEADER + p_entry * RT_EMISSIVE_ENTRY_UINTS;
}

vec3 rt_emissive_to_world(uint p_base, vec3 p_obj) {
	vec4 p = vec4(p_obj, 1.0);
	return vec3(
			dot(uintBitsToFloat(uvec4(rt_light_grid[p_base + 0u], rt_light_grid[p_base + 1u], rt_light_grid[p_base + 2u], rt_light_grid[p_base + 3u])), p),
			dot(uintBitsToFloat(uvec4(rt_light_grid[p_base + 4u], rt_light_grid[p_base + 5u], rt_light_grid[p_base + 6u], rt_light_grid[p_base + 7u])), p),
			dot(uintBitsToFloat(uvec4(rt_light_grid[p_base + 8u], rt_light_grid[p_base + 9u], rt_light_grid[p_base + 10u], rt_light_grid[p_base + 11u])), p));
}

// A triangle's world-space corners.
void rt_emissive_triangle(uint p_base, GeometryData p_geom, uint p_prim, out vec3 r_p0, out vec3 r_p1, out vec3 r_p2, out uint r_i0, out uint r_i1, out uint r_i2) {
	get_triangle_indices_ex(p_geom, p_prim, r_i0, r_i1, r_i2);
	r_p0 = rt_emissive_to_world(p_base, fetch_object_position(p_geom, r_i0));
	r_p1 = rt_emissive_to_world(p_base, fetch_object_position(p_geom, r_i1));
	r_p2 = rt_emissive_to_world(p_base, fetch_object_position(p_geom, r_i2));
}

// Solid-angle density with which lights_emissive_sample() picks the point p_light on triangle
// (p0, p1, p2) of entry p_entry (index, not + 1), seen from p_from.
float rt_emissive_pdf(uint p_entry, uint p_triangles, vec3 p0, vec3 p1, vec3 p2, vec3 p_from, vec3 p_light) {
	vec3 c = cross(p1 - p0, p2 - p0);
	float twice_area = length(c);
	if (!(twice_area > 0.0)) {
		return 0.0;
	}
	vec3 n = c / twice_area;
	vec3 d = p_light - p_from;
	float d2 = dot(d, d);
	float cos_l = abs(dot(n, d)) * inversesqrt(max(d2, 1e-12));
	if (!(cos_l > 1e-6)) {
		return 0.0;
	}
	float prob = uintBitsToFloat(rt_light_grid[rt_emissive_entry_base(p_entry) + 15u]);
	float pdf_area = prob / (float(p_triangles) * 0.5 * twice_area);
	return pdf_area * d2 / cos_l;
}

// Solid-angle density with which sample_surface_continuation() draws L off the base, mirroring
// its lobe choice: the specular lobe by GGX VNDF reflection, the diffuse one cosine-weighted.
// Probability of a bounce ray passing through the surface (back lighting): the transmitted
// share of the base's reflected and transmitted light.
float backlight_lobe_probability(MaterialProperties material) {
	if (!rt_material_feature(RT_MATERIAL_FEATURE_BACK_LIGHTING)) {
		return 0.0;
	}
	float back_lum = luminance(backlightTransmittance(material));
	if (back_lum <= 0.0) {
		return 0.0;
	}
	float lum = back_lum + luminance(materialSpecularF0(material)) + luminance(materialDiffuseReflectance(material));
	return clamp(back_lum / lum, 0.01, 0.99);
}

// The density of the reflection lobes alone.
float reflection_continuation_pdf(vec3 N, vec3 V, vec3 L, MaterialProperties material) {
	float NdotL = dot(N, L);
	float NdotV = dot(N, V);
	if (NdotL <= 0.0) {
		return 0.0;
	}
	vec3 specular_f0 = materialSpecularF0(material);
	vec3 diffuse_reflectance = materialDiffuseReflectance(material);
	float specular_lum = luminance(specular_f0);
	float diffuse_lum = luminance(diffuse_reflectance);
	float p_spec;
	if (diffuse_lum < 0.0001) {
		p_spec = 1.0;
	} else if (specular_lum < 0.0001) {
		p_spec = 0.0;
	} else {
		p_spec = clamp(specular_lum / (specular_lum + diffuse_lum), 0.01, 0.99);
	}
	float pdf = (1.0 - p_spec) * NdotL / PI;
	if (p_spec > 0.0) {
		float alpha = material.roughness * material.roughness;
		if (alpha <= 0.0) {
			return 1e30; // a mirror: only the reflection ray finds the light
		}
		vec3 H = normalize(V + L);
		pdf += p_spec * sampleGGXVNDFReflectionPdf(alpha, alpha * alpha, max(dot(N, H), 0.0), max(NdotV, 0.0), max(dot(L, H), 0.0));
	}
	return pdf;
}

float opaque_continuation_pdf(vec3 N, vec3 V, vec3 L, MaterialProperties material) {
	float p_back = backlight_lobe_probability(material);
	if (p_back > 0.0) {
		return (1.0 - p_back) * reflection_continuation_pdf(N, V, L, material) + p_back * max(-dot(N, L), 0.0) / PI;
	}
	return reflection_continuation_pdf(N, V, L, material);
}

// Probability of a bounce ray passing through a thin transmissive surface: the transmitted
// share of its light.
float transmission_lobe_probability(MaterialProperties material) {
	if (material.transmissivness <= 0.0) {
		return 0.0;
	}
	float trans_lum = luminance(thinTransmittance(material));
	float lum = trans_lum + luminance(materialSpecularF0(material)) + luminance(materialDiffuseReflectance(material)) + luminance(backlightTransmittance(material));
	return lum > 0.0 ? clamp(trans_lum / lum, 0.01, 0.99) : 0.0;
}

// The reflection and back light lobes' density. The thin transmission lobe adds none: no light
// sample draws its directions (a ray through it weighs what it finds in full).
float base_continuation_pdf(vec3 N, vec3 V, vec3 L, MaterialProperties material) {
	float p_trans = transmission_lobe_probability(material);
	return (1.0 - p_trans) * opaque_continuation_pdf(N, V, L, material);
}

// The same with the coat lobe, taken with probability clearcoatLobeProbability().
float surface_continuation_pdf_unhaired(vec3 N, vec3 V, vec3 L, MaterialProperties material) {
	float p_coat = clearcoatLobeProbability(material, V);
	if (p_coat <= 0.0) {
		return base_continuation_pdf(N, V, L, material);
	}
	return (1.0 - p_coat) * base_continuation_pdf(N, V, L, material) + p_coat * clearcoatPdf(material, V, L);
}

// And the hair fibres, taken with the probability of the hair mask (sample_surface_continuation()).
float surface_continuation_pdf(vec3 N, vec3 V, vec3 L, MaterialProperties material) {
	if (material.hair <= 0.0) {
		return surface_continuation_pdf_unhaired(N, V, L, material);
	}
	float pdf = material.hair * hairPdf(material, L, V);
	if (material.hair < 1.0) {
		pdf += (1.0 - material.hair) * surface_continuation_pdf_unhaired(N, V, L, material);
	}
	return pdf;
}

// Power heuristic (Veach 1997) for one sample from each of two strategies.
float rt_mis_power(float p_this, float p_other) {
	float a = p_this * p_this;
	float b = p_other * p_other;
	return (a + b) > 0.0 ? a / (a + b) : 1.0;
}

// One next event estimation sample of the emissive surfaces at a shaded point: an entry by
// its probability, a triangle uniformly, a point on it uniformly, weighed against the bounce
// ray by multiple importance sampling (power heuristic). Returns the contribution, shadowed.
vec3 lights_emissive_sample(vec3 p_origin, vec3 N, vec3 V, MaterialProperties material, uint p_shadow_mask, inout uint rng_state) {
	uint b = rt_light_grid[9];
	uint n = b == 0u ? 0u : rt_light_grid[b];
	if (n == 0u) {
		return vec3(0.0);
	}
	// The entry: binary search of the CDF.
	float u = rand(rng_state);
	uint lo = 0u;
	uint hi = n - 1u;
	while (lo < hi) {
		uint mid = (lo + hi) >> 1u;
		if (u < uintBitsToFloat(rt_light_grid[rt_emissive_entry_base(mid) + 14u])) {
			hi = mid;
		} else {
			lo = mid + 1u;
		}
	}
	uint entry = lo;
	uint base = rt_emissive_entry_base(entry);
	uint geometry_idx = rt_light_grid[base + 12u];
	GeometryData geom = geometries[geometry_idx];
	uint triangles = geom.primitive_count;
	if (triangles == 0u) {
		return vec3(0.0);
	}
	uint prim = min(uint(rand(rng_state) * float(triangles)), triangles - 1u);
	vec3 p0, p1, p2;
	uint i0, i1, i2;
	rt_emissive_triangle(base, geom, prim, p0, p1, p2, i0, i1, i2);
	vec2 r = rand2(rng_state);
	float sr = sqrt(r.x);
	vec3 bary = vec3(1.0 - sr, sr * (1.0 - r.y), sr * r.y);
	vec3 p_light = p0 * bary.x + p1 * bary.y + p2 * bary.z;
	vec3 to_light = p_light - p_origin;
	float dist = length(to_light);
	if (!(dist > 1e-4)) {
		return vec3(0.0);
	}
	vec3 L = to_light / dist;
	if (dot(N, L) <= 0.0 && !godotShadesBackfacingLight(material)) {
		return vec3(0.0);
	}
	vec3 c = cross(p1 - p0, p2 - p0);
	// The surface's normal there, as a hit would shade it (mat3(gl_ObjectToWorldEXT), columns normalized).
	vec3 n_light = normalize(c);
	if (geom.normal_byte_offset != OFFSET_NONE) {
		vec3 n_obj = fetch_tbn(geom, i0, i1, i2, bary).normal;
		vec3 n_world = vec3(
				dot(uintBitsToFloat(uvec3(rt_light_grid[base + 0u], rt_light_grid[base + 1u], rt_light_grid[base + 2u])), n_obj),
				dot(uintBitsToFloat(uvec3(rt_light_grid[base + 4u], rt_light_grid[base + 5u], rt_light_grid[base + 6u])), n_obj),
				dot(uintBitsToFloat(uvec3(rt_light_grid[base + 8u], rt_light_grid[base + 9u], rt_light_grid[base + 10u])), n_obj));
		if (dot(n_world, n_world) > 0.0) {
			n_light = normalize(n_world);
		}
	}
	// Bounce rays cull back faces: only the side they can hit emits towards p_origin. The
	// front side is the one the vertex normals face (the back one for cull-front materials).
	uint facing = rt_light_grid[base + 13u];
	if ((facing & 1u) == 0u) {
		bool towards = dot(n_light, -L) > 0.0;
		if (towards == ((facing & 2u) != 0u)) {
			return vec3(0.0);
		}
	}
	float pdf_l = rt_emissive_pdf(entry, triangles, p0, p1, p2, p_origin, p_light);
	if (!(pdf_l > 0.0) || isinf(pdf_l)) {
		return vec3(0.0);
	}

	// The emitted radiance at the point, as a bounce ray hitting it would read it.
	MaterialData mat = rt_entry_material(geometry_idx);
	vec2 uv = fetch_uv(geom, i0, i1, i2, bary);
	vec2 uv2 = vec2(0.0);
	if ((mat.flags & RT_MAT_FLAG_EMISSION_UV2) != 0u) {
		uv2 = fetch_uv_at(geom, geom.uv2_byte_offset, geom.uv2_scale_packed, i0, i1, i2, bary);
	}
	vec3 obj_pos = fetch_object_position(geom, i0) * bary.x + fetch_object_position(geom, i1) * bary.y + fetch_object_position(geom, i2) * bary.z;
	MaterialUV muv = material_uv_compute(material_uv1_channel(mat), uv, obj_pos, n_light, p_light, n_light);
	muv.cone_grad = false;
	muv.cone_lod = RT_TEXTURE_LOD_BASE;
	vec3 Le = material_emission(mat, muv, uv2, p_light, n_light) * scene_data_block.data.emissive_exposure_normalization * rt_material_emission_indirect(mat);
	if (all(lessThanEqual(Le, vec3(0.0)))) {
		return vec3(0.0);
	}

	vec3 brdf_diffuse, brdf_specular;
	evalCombinedBRDFSeparate(N, L, V, material, brdf_diffuse, brdf_specular);
	vec3 f = brdf_diffuse + brdf_specular; // NdotL included
	if (all(lessThanEqual(f, vec3(0.0)))) {
		return vec3(0.0);
	}
	float w = rt_mis_power(pdf_l, surface_continuation_pdf(N, V, L, material));
	vec3 vis = lights_trace_shadow_ray(rt_shadow_origin(p_origin, L), L, max(dist * 0.999 - 0.002, 0.0), p_shadow_mask, rng_state);
	return f * Le * vis * (w / pdf_l);
}

// The multiple importance sampling weight of emission a bounce ray found on p_geometry_idx at
// p_hit, drawn from p_from with density p_bsdf_pdf (0: no light sample was taken, weight 1).
float rt_emissive_hit_weight(uint p_geometry_idx, uint p_prim, vec3 p_from, vec3 p_hit, float p_bsdf_pdf) {
	if (!(p_bsdf_pdf > 0.0)) {
		return 1.0;
	}
	uint e = rt_emissive_entry_of(p_geometry_idx);
	if (e == 0u) {
		return 1.0;
	}
	GeometryData geom = geometries[p_geometry_idx];
	vec3 p0, p1, p2;
	uint i0, i1, i2;
	rt_emissive_triangle(rt_emissive_entry_base(e - 1u), geom, p_prim, p0, p1, p2, i0, i1, i2);
	float pdf_l = rt_emissive_pdf(e - 1u, max(geom.primitive_count, 1u), p0, p1, p2, p_from, p_hit);
	return rt_mis_power(p_bsdf_pdf, pdf_l);
}

// p_is_indirect: the path made a diffuse bounce, scaling lights by indirect_energy and emission by emission_indirect.
// p_bsdf_pdf: PathState::bsdf_pdf of the ray that hit (0: the emission counts in full).
vec3 shade_local_surface(HitData h, MaterialResult m, vec3 N, vec3 V, bool p_is_indirect, float p_bsdf_pdf, inout uint rng_state) {
	float emission_weight = p_is_indirect ? m.emission_indirect : 1.0;
	if (surface_is_unshaded(h.geometry_idx)) {
		return (m.albedo + m.emissive) * emission_weight;
	}
	if (p_bsdf_pdf > 0.0 && any(notEqual(m.emissive, vec3(0.0)))) {
		// The emissive surfaces' light sample at the last vertex found this light too.
		emission_weight *= rt_emissive_hit_weight(h.geometry_idx, gl_PrimitiveID, gl_WorldRayOriginEXT, h.hit_pos, p_bsdf_pdf);
	}
	vec3 lit = m.emissive * emission_weight;
	MaterialProperties material = material_properties_from_result(h, m, N, V);
	uint light_count = uint(get_rt_param(RT_PARAM_LIGHT_COUNT));
	vec3 hit_pos_offset = offset_ray_origin(h.hit_pos, h.geometry_normal, gl_HitTEXT);
	rt_set_shadow_origins(hit_pos_offset, offset_ray_origin(h.hit_pos, -h.geometry_normal, gl_HitTEXT), h.geometry_normal);
	uint layers = geometries[h.geometry_idx].layers;
	uint shadow_mask = rt_receiver_shadow_mask(h.geometry_idx);
	if (RT_GET_MAX_BOUNCES() == 0u) {
		// Direct only: one ray per light group, plus the rasterizer's ambient.
		lit += lights_evaluate_direct_lighting_per_group(hit_pos_offset, N, V, material,
				rng_state, p_is_indirect, light_count, layers, shadow_mask);
		lit += ambient_approx(N, V, m, material);
	} else {
		lit += lights_evaluate_direct_lighting(hit_pos_offset, N, V, material,
				rng_state, p_is_indirect, light_count, layers, shadow_mask);
	}
	if (rt_emissive_count() > 0u) {
		lit += lights_emissive_sample(hit_pos_offset, N, V, material, shadow_mask, rng_state);
	}
	return lit;
}

// A bounce ray off the coat, taken with probability p_coat. Counts as a specular bounce.
bool sample_coat_continuation(HitData h, MaterialProperties material, vec3 N, vec3 V, float p_coat, inout PathState ps) {
	vec3 next_dir;
	vec3 weight;
	if (!sampleClearcoat(rand2(ps.rng_state), h.geometry_normal, V, material, next_dir, weight)) {
		vec3 recovered_dir;
		if (luminance(weight) == 0.0 || !recoverBelowHemisphereSample(next_dir, h.geometry_normal, recovered_dir)) {
			return false;
		}
		next_dir = recovered_dir;
	}
	ps.throughput *= weight / p_coat;
	ps.packed_bounces_flags = inc_total_bounce(ps.packed_bounces_flags) & ~LAST_BOUNCE_DIFFUSE_FLAG;
	if (sqrt(material.clearcoatAlpha) > SINGULAR_ROUGHNESS_MAX) {
		ps.packed_bounces_flags |= NON_SINGULAR_PATH_FLAG;
	}
	ps.offset_normal = h.geometry_normal;
	ps.next_ray_dir = next_dir;
	ps.bsdf_pdf = rt_emissive_count() > 0u ? surface_continuation_pdf(N, V, next_dir, material) : 0.0;
	return true;
}

// Subsurface scattering (docs/ideas/subsurface-scattering.md, Cycles' Random Walk): the diffuse lobe
// enters the surface along a cosine direction about -N and raygen walks the medium below it
// (SSS_ENTER_FLAG, rt_sss_walk()). The walk adds the albedo, so the weight is the diffuse lobe's
// without it. The medium's colour is the albedo; its radius the strength times
// rendering/pathtracing/subsurface_scattering_scale, per channel as measured skin in Skin Mode
// (Jensen et al. 2001, skin1: diffuse mean free paths 3.67, 1.37 and 0.68 mm).
bool sample_subsurface_entry(HitData h, MaterialResult m, MaterialProperties material, vec3 N, inout PathState ps) {
	float4 q = getRotationToZAxis(-N);
	vec3 dir = normalize(rotatePoint(invertRotation(q), sampleHemisphere(rand2(ps.rng_state))));
	if (dot(h.geometry_normal, dir) >= 0.0) {
		return false;
	}
	vec3 profile = m.subsurface_skin ? vec3(1.0, 0.373, 0.185) : vec3(1.0);
	vec3 radius = max(m.subsurface * get_rt_param(RT_PARAM_SSS_SCALE) * profile, vec3(1.0 / 4096.0));
	ps.throughput *= (1.0 - material.metalness) * (1.0 - material.transmissivness);
	ps.packed_bounces_flags = inc_diffuse_bounce(ps.packed_bounces_flags) | LAST_BOUNCE_DIFFUSE_FLAG | NON_SINGULAR_PATH_FLAG | SSS_ENTER_FLAG;
	ps.offset_normal = -h.geometry_normal;
	ps.next_ray_dir = dir;
	ps.bsdf_pdf = uintBitsToFloat(rt_medium_code(1.0 / radius));
	imageStore(rt_medium_image, ivec2(gl_LaunchIDEXT.xy), uvec4(packUnorm4x8(vec4(clamp(material.baseColor, vec3(0.0), vec3(1.0)), 0.0))));
	return true;
}

// A subsurface probe ray's hit (SSS_PROBE_FLAG): distance, facing, interpolated normal and TLAS entry,
// without evaluating the material.
void rt_sss_probe_hit() {
	uint geometry_idx = RT_HIT_GEOMETRY_IDX;
	GeometryData geom = geometries[geometry_idx];
	mat3 model_rotation = mat3(gl_ObjectToWorldEXT);
	mat3 normal_matrix = mat3(normalize(model_rotation[0]), normalize(model_rotation[1]), normalize(model_rotation[2]));
	vec3 n;
	bool front;
#ifdef ENABLE_INTERSECTION_SHADERS
	if ((geom.flags & FLAG_PROCEDURAL) != 0u) {
		n = normalize(normal_matrix * normalize(unpackSnorm4x8(hit_attribs.packed_normal).xyz));
		front = dot(n, gl_WorldRayDirectionEXT) < 0.0;
	} else
#endif
	{
		VertexAttributes attrs = fetch_vertex_attributes(geom, attribs, FETCH_TBN);
		n = normalize(normal_matrix * attrs.normal);
		front = rt_triangle_front_facing(geom);
	}
	payload.hit_t = gl_HitTEXT;
	payload.oct_offset_nrm = packUnorm2x16(vec3_to_oct(n));
	payload.oct_next_dir = geometry_idx;
	payload.packed_bounces_flags = SSS_PROBE_FLAG | (front ? 0u : SSS_PROBE_BACK_FACE_FLAG);
}

bool sample_surface_continuation(HitData h, MaterialResult m, vec3 N, vec3 V, inout PathState ps) {
	if (surface_is_unshaded(h.geometry_idx)) {
		return false;
	}
	uint total_bounces = get_total_bounces(ps.packed_bounces_flags);
	uint diffuse_bounces = get_diffuse_bounces(ps.packed_bounces_flags);
	if (total_bounces >= RT_GET_MAX_BOUNCES() || diffuse_bounces >= MAX_DIFFUSE_BOUNCES) {
		return false;
	}

	MaterialProperties material = material_properties_from_result(h, m, N, V);

	// Hair fibres, with the share of the surface the hair mask gives them. Either choice weighs
	// its own lobes in full: the mask's probability cancels the mask's share.
	if (material.hair > 0.0 && (material.hair >= 1.0 || rand(ps.rng_state) < material.hair)) {
		vec3 hair_dir;
		vec3 hair_weight;
		if (!sampleHair(vec4(rand2(ps.rng_state), rand2(ps.rng_state)), V, material, hair_dir, hair_weight)) {
			return false;
		}
		ps.throughput *= hair_weight;
		ps.packed_bounces_flags = (inc_total_bounce(ps.packed_bounces_flags) & ~LAST_BOUNCE_DIFFUSE_FLAG) | NON_SINGULAR_PATH_FLAG;
		ps.offset_normal = dot(hair_dir, h.geometry_normal) >= 0.0 ? h.geometry_normal : -h.geometry_normal;
		ps.next_ray_dir = hair_dir;
		ps.bsdf_pdf = rt_emissive_count() > 0u ? surface_continuation_pdf(N, V, hair_dir, material) : 0.0;
		return true;
	}

	vec3 specular_f0 = materialSpecularF0(material);
	vec3 diffuse_reflectance = materialDiffuseReflectance(material);
	float specular_lum = luminance(specular_f0);
	float diffuse_lum = luminance(diffuse_reflectance);

	// The coat first, with its share of the reflected light (clearcoatLobeProbability()). The base
	// lobes keep the rest, and their weight carries the coat's attenuation (1 - Fc), as raster's ambient.
	float p_coat = clearcoatLobeProbability(material, V);
	if (p_coat > 0.0) {
		if (rand(ps.rng_state) < p_coat) {
			return sample_coat_continuation(h, material, N, V, p_coat, ps);
		}
		float coat_fresnel = clearcoatFresnel(material.clearcoat, dot(material.coatNormal, V));
		ps.throughput *= (1.0 - coat_fresnel) / (1.0 - p_coat);
	}

	// Thin transmission: straight through the surface, with its share of the light.
	float p_trans = transmission_lobe_probability(material);
	if (p_trans > 0.0) {
		if (rand(ps.rng_state) < p_trans) {
			vec3 trans_dir;
			vec3 trans_weight;
			bool volume = m.ior >= 1.0 && rt_material_feature(RT_MATERIAL_FEATURE_REFRACTION) && (ps.packed_bounces_flags & PEEL_RAY_FLAG) == 0u;
			bool refracted = true;
			if (volume) {
				// Into the medium through a front face, out of it through a back face (raygen tracks it).
				float ior = m.ior;
				if (!sampleDielectricRefraction(rand2(ps.rng_state), N, h.geometry_normal, V, material, h.is_front_face ? 1.0 / ior : ior,
							trans_dir, trans_weight, refracted)) {
					return false;
				}
			} else if (!sampleThinTransmission(rand2(ps.rng_state), N, h.geometry_normal, V, material, trans_dir, trans_weight)) {
				return false;
			}
			ps.throughput *= trans_weight / p_trans;
			ps.packed_bounces_flags = inc_total_bounce(ps.packed_bounces_flags) & ~LAST_BOUNCE_DIFFUSE_FLAG;
			if (material.roughness > SINGULAR_ROUGHNESS_MAX) {
				ps.packed_bounces_flags |= NON_SINGULAR_PATH_FLAG;
			}
			ps.offset_normal = refracted ? -h.geometry_normal : h.geometry_normal;
			ps.next_ray_dir = trans_dir;
			ps.bsdf_pdf = 0.0;
			if (volume && refracted) {
				// The ray crossed the surface: raygen takes the medium it is in now from bsdf_pdf.
				ps.packed_bounces_flags |= MEDIUM_CHANGE_FLAG;
				ps.bsdf_pdf = uintBitsToFloat(h.is_front_face ? m.medium : 0u);
				if (h.is_front_face && (m.medium & RT_MEDIUM_CODE_SCATTER) != 0u) {
					imageStore(rt_medium_image, ivec2(gl_LaunchIDEXT.xy), uvec4(m.medium_scatter));
				}
			}
			return true;
		}
		ps.throughput /= 1.0 - p_trans;
	}

	// Back lighting: a diffuse bounce through the surface, with its share of the base's light.
	float p_back = backlight_lobe_probability(material);
	if (p_back > 0.0) {
		if (rand(ps.rng_state) < p_back) {
			vec3 back_dir;
			vec3 back_weight;
			if (!sampleBacklight(rand2(ps.rng_state), N, h.geometry_normal, material, back_dir, back_weight)) {
				return false;
			}
			ps.throughput *= back_weight / p_back;
			ps.packed_bounces_flags = inc_diffuse_bounce(ps.packed_bounces_flags) | LAST_BOUNCE_DIFFUSE_FLAG | NON_SINGULAR_PATH_FLAG;
			ps.offset_normal = -h.geometry_normal;
			ps.next_ray_dir = back_dir;
			ps.bsdf_pdf = rt_emissive_count() > 0u ? surface_continuation_pdf(N, V, back_dir, material) : 0.0;
			return true;
		}
		ps.throughput /= 1.0 - p_back;
	}

	int brdf_type;
	if (diffuse_lum < 0.0001) {
		brdf_type = SPECULAR_TYPE;
	} else if (specular_lum < 0.0001) {
		brdf_type = DIFFUSE_TYPE;
	} else {
		float brdf_probability = clamp(specular_lum / (specular_lum + diffuse_lum), 0.01, 0.99);
		if (rand(ps.rng_state) < brdf_probability) {
			brdf_type = SPECULAR_TYPE;
			ps.throughput /= brdf_probability;
		} else {
			brdf_type = DIFFUSE_TYPE;
			ps.throughput /= (1.0 - brdf_probability);
		}
	}

	if (brdf_type == DIFFUSE_TYPE && material.subsurface > 0.5) {
		return sample_subsurface_entry(h, m, material, N, ps);
	}

	vec3 next_dir;
	vec3 brdf_weight;
	if (!evalIndirectCombinedBRDF(rand2(ps.rng_state), N, h.geometry_normal, V, material, brdf_type, next_dir, brdf_weight, vec4(0.0))) {
		vec3 recovered_dir;
		if (luminance(brdf_weight) == 0.0 ||
				!recoverBelowHemisphereSample(next_dir, h.geometry_normal, recovered_dir)) {
			return false;
		}
		next_dir = recovered_dir;
	}

	ps.throughput *= brdf_weight;
	ps.packed_bounces_flags = (brdf_type == DIFFUSE_TYPE)
			? inc_diffuse_bounce(ps.packed_bounces_flags) | LAST_BOUNCE_DIFFUSE_FLAG
			: inc_total_bounce(ps.packed_bounces_flags) & ~LAST_BOUNCE_DIFFUSE_FLAG;
	if (brdf_type == DIFFUSE_TYPE || material.roughness > SINGULAR_ROUGHNESS_MAX) {
		ps.packed_bounces_flags |= NON_SINGULAR_PATH_FLAG;
	}
	ps.offset_normal = h.geometry_normal;
	ps.next_ray_dir = next_dir;
	// The density for weighing emission this ray finds against the emissive light sample
	// shade_local_surface() took here (rt_emissive_hit_weight).
	ps.bsdf_pdf = rt_emissive_count() > 0u ? surface_continuation_pdf(N, V, next_dir, material) : 0.0;
	return true;
}

#ifdef DLSS_RR_ENABLED
// Specular motion vector guide (rt_velocity_image convention). Treats the reflector as a plane:
// the mirror hit's virtual image sits on the view ray at the reflection distance, moved by the
// reflector's motion plus the reflected point's motion mirrored through the plane.
// Rough surfaces use the surface motion. Deterministic: RR guides must never carry sampling noise.
// p_plane_normal must be the unbumped geometry normal: normal-map detail scatters the virtual
// image per pixel and DLSS-RR warps highlights along that noise.
vec2 dlss_rr_specular_motion_vector(vec3 p_hit_pos, vec3 p_prev_hit_pos, vec3 p_plane_normal, vec3 V, float p_roughness, uint p_mask) {
	if (p_roughness >= MAX_DENOISER_SPECULAR_HIT_THRESHOLD) {
		return rt_motion_vector(p_hit_pos, p_prev_hit_pos);
	}
	vec3 N = dot(p_plane_normal, V) < 0.0 ? -p_plane_normal : p_plane_normal;
	vec3 spec_dir = reflect(-V, N);
	vec3 spec_origin = offset_ray_origin(p_hit_pos, spec_dir, gl_HitTEXT);
	rayQueryEXT spec_rq;
	// A/B: TerminateOnFirstHit stops at whichever hit traversal finds first, not the closest.
	rayQueryInitializeEXT(spec_rq, tlas, RT_RAY_FLAGS | gl_RayFlagsTerminateOnFirstHitEXT, p_mask, spec_origin, 0.001, spec_dir, 10000.0);
	while (rayQueryProceedEXT(spec_rq)) {
		if (rayQueryGetIntersectionTypeEXT(spec_rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT &&
				ray_query_alpha_test(
						RT_RQ_GEOMETRY_IDX(spec_rq, false),
						rayQueryGetIntersectionPrimitiveIndexEXT(spec_rq, false),
						rayQueryGetIntersectionBarycentricsEXT(spec_rq, false))) {
			rayQueryConfirmIntersectionEXT(spec_rq);
		}
	}
	if (rayQueryGetIntersectionTypeEXT(spec_rq, true) == gl_RayQueryCommittedIntersectionNoneEXT) {
		// Reflected sky is at infinity: only camera rotation moves it, like the primary sky velocity.
		vec3 far_world = p_hit_pos - V * 10000.0;
		return rt_motion_vector(far_world, far_world);
	}

	float spec_t = rayQueryGetIntersectionTEXT(spec_rq, true);
	vec3 reflected_pos = spec_origin + spec_dir * spec_t;
	uint reflected_geom_idx = RT_RQ_GEOMETRY_IDX(spec_rq, true);
	vec3 reflected_obj_pos = rt_prev_triangle_object_pos(geometries[reflected_geom_idx],
			uint(rayQueryGetIntersectionPrimitiveIndexEXT(spec_rq, true)),
			rayQueryGetIntersectionBarycentricsEXT(spec_rq, true),
			(mat4(rayQueryGetIntersectionWorldToObjectEXT(spec_rq, true)) * vec4(reflected_pos, 1.0)).xyz);
	vec3 reflected_prev_pos = rt_prev_world_pos(reflected_geom_idx, reflected_obj_pos,
			mat4(rayQueryGetIntersectionObjectToWorldEXT(spec_rq, true)));

	vec3 reflector_motion = p_prev_hit_pos - p_hit_pos;
	vec3 relative_motion = reflected_prev_pos - reflected_pos - reflector_motion;
	vec3 virtual_pos = p_hit_pos - V * spec_t;
	vec3 virtual_prev_pos = virtual_pos + reflector_motion + reflect(relative_motion, N);
	return rt_motion_vector(virtual_pos, virtual_prev_pos);
}

// The environment's specular albedo. A coat adds its Fresnel and attenuates the base by the rest
// (raster ambient), its base F0 taken under the coat.
vec3 dlss_rr_compute_specular_albedo(MaterialResult m, float NdotV) {
	if (m.clearcoat <= 0.0) {
		return DLSSRR_computeSpecularAlbedo(m.albedo, m.metalness, material_specular_to_f0(m.specular), m.roughness, NdotV);
	}
	vec3 f0 = baseColorToSpecularF0(m.albedo, m.metalness, material_specular_to_f0(m.specular));
	f0 = mix(f0, f0ClearCoatToSurface(f0), m.clearcoat);
	float coat_fresnel = clearcoatFresnel(m.clearcoat, NdotV);
	return material_specular_albedo(f0, m.roughness, NdotV) * (1.0 - coat_fresnel) + vec3(coat_fresnel);
}

// The roughness guide: a coat's sharp reflection over a rough base pulls it toward the coat's.
float dlss_rr_roughness(MaterialResult m) {
	return mix(m.roughness, min(m.roughness, sqrt(clearcoatAlphaFromRoughness(m.clearcoat_roughness))), m.clearcoat);
}

// The diffuse albedo guide. An emissive surface's light is its own, not reflected, so its
// albedo alone tells Ray Reconstruction nothing of the pattern it glows in (lettering on a
// sign, a lit screen): the denoiser takes the emission for noisy lighting and spreads it.
// As the miss shader bakes the sky's colour into this guide to keep its detail, the
// emission, mapped into [0, 1) by Reinhard, is taken where it is brighter than the albedo.
vec3 dlss_rr_diffuse_guide(MaterialResult m) {
	vec3 emissive_guide = max(m.emissive, vec3(0.0)) / (vec3(1.0) + max(m.emissive, vec3(0.0)));
	return max(DLSSRR_computeDiffuseAlbedo(m.albedo, m.metalness), emissive_guide);
}

void dlss_rr_store_albedo_guides(MaterialResult m, float NdotV) {
	dlss_rr_write_albedo_guides(dlss_rr_diffuse_guide(m), dlss_rr_compute_specular_albedo(m, NdotV));
}

// Primary hits at least this metallic take the reflection's motion as their own.
#define DLSS_RR_SPECULAR_ONLY_METALNESS 0.9
// Material-only test: an albedo/Fresnel based share flips per texel and with view angle, tearing
// replaced and own guides apart on textured dielectrics.
bool dlss_rr_is_replaceable_mirror(HitData h, MaterialResult m) {
	return m.roughness <= DLSS_RR_REPLACEMENT_ROUGHNESS_MAX && m.metalness >= DLSS_RR_SPECULAR_ONLY_METALNESS &&
			!surface_is_unshaded(h.geometry_idx);
}

// Guide ray hit, returned to raygen in the payload. Every hit: hit_t, diffuse albedo (packed_rt[2])
// and specular albedo (rng_state) as unorm8. A replaceable mirror adds its exact reflection
// (next_ray_dir); any other surface its world motion (packed_rt[0], packed_rt[1].x). Raygen blends
// albedos and maps motion through the chain.
void dlss_rr_guide_hit(HitData h, MaterialResult m, vec3 N, vec3 V, float NdotV, uint p_flags) {
	bool mirror = (p_flags & GUIDE_LAST_FLAG) == 0u && dlss_rr_is_replaceable_mirror(h, m);
	PathState gs;
	gs.bsdf_pdf = 0.0;
	gs.radiance = vec3(0.0);
	gs.throughput = vec3(0.0);
	gs.packed_bounces_flags = p_flags | GUIDE_RESULT_FLAG | (mirror ? GUIDE_MIRROR_FLAG : 0u);
	gs.rng_state = 0u;
	gs.hit_t = gl_HitTEXT;
	gs.offset_normal = h.geometry_normal;
	gs.next_ray_dir = mirror ? reflect(-V, N) : N;
	path_pack(payload, gs);
	vec3 motion = mirror ? vec3(0.0) : rt_hit_prev_world_pos(h.hit_pos) - h.hit_pos;
	payload.packed_rt[0] = packHalf2x16(motion.xy);
	payload.packed_rt[1] = packHalf2x16(vec2(motion.z, 0.0));
	payload.packed_rt[2] = packUnorm4x8(vec4(dlss_rr_diffuse_guide(m), 0.0));
	payload.rng_state = packUnorm4x8(vec4(dlss_rr_compute_specular_albedo(m, NdotV), 0.0));
}

// Composites a peel layer's RR guides over the ones stored for this pixel. p_cover is
// how much of what is behind the layer it hides. A premultiplied layer adds its albedos
// unscaled and takes over normal and roughness by its share of the
// specular albedo, so its guides stay continuous at any alpha.
void dlss_rr_composite_peel_guides(HitData h, MaterialResult m, vec3 N, vec3 V, float NdotV, float p_cover, bool p_premultiplied) {
	ivec2 pixel = ivec2(gl_LaunchIDEXT.xy);
	float source_weight = p_premultiplied ? 1.0 : p_cover;
	float behind = 1.0 - p_cover;

	vec3 diffuse = imageLoad(dlss_rr_diffuse_albedo, pixel).rgb * behind +
			dlss_rr_diffuse_guide(m) * source_weight;
	vec3 layer_specular = source_weight * dlss_rr_compute_specular_albedo(m, NdotV);
	vec3 specular = imageLoad(dlss_rr_specular_albedo, pixel).rgb * behind + layer_specular;
	dlss_rr_write_albedo_guides(diffuse, specular);

	float share = p_premultiplied ? luminance(layer_specular) / max(luminance(specular), 1e-4) : p_cover;
	vec4 prev_nr = imageLoad(dlss_rr_normal_roughness, pixel);
	// Sky stores a zero normal, so the blend can be zero when share is 0.
	vec3 blended_n = mix(prev_nr.xyz, N, share);
	blended_n = dot(blended_n, blended_n) > 0.0 ? normalize(blended_n) : blended_n;
	imageStore(dlss_rr_normal_roughness, pixel, vec4(blended_n, mix(prev_nr.w, dlss_rr_roughness(m), share)));

	// Blended motion vectors would point at neither surface, so the larger share owns them.
	if (share >= 0.5) {
		vec2 specular_mv = dlss_rr_specular_motion_vector(h.hit_pos, rt_hit_prev_world_pos(h.hit_pos), h.geometry_normal, V, dlss_rr_roughness(m), RT_VIS_MASK | RT_TMASK_TRANSPARENT);
		dlss_rr_write_specular_motion_vector(specular_mv);
	}
}
#endif // DLSS_RR_ENABLED

// Production shading: emissive + NEE direct lighting + BRDF importance sampling + next bounce.
// Also handles DLSS-RR G-buffer output on primary ray.
void shade_and_bounce(HitData h, MaterialResult m) {
	// Any-hit already rejected alpha-scissored texels.
	m.alpha = rt_entry_alpha(rt_entry_material(h.geometry_idx), m.alpha, 0.0);
#ifdef RT_DEBUG_ENABLED
	if (rt_peel_scene_depth_lost) {
		m.albedo = vec3(0.0);
		m.emissive = vec3(50.0, 0.0, 25.0); // Hot pink, bright enough to survive exposure and tonemapping.
		m.alpha = 1.0;
	}
#endif
	PathState ps = path_unpack(payload);
	// Every surface bounds this ray segment, even if it cannot continue the path.
	ps.hit_t = gl_HitTEXT;
	if (m.hair > 0.0 && !m.hair_strand) {
		// Where on a fibre the path meets it: shells and cards have no fibres, so every path vertex
		// draws its own offset, which averages over the fibre's width (the far-field view).
		m.hair_params.w = rand(ps.rng_state) * 2.0 - 1.0;
	}

	// Subsurface scattering's light comes through the walk after a bounce, so where the path ends
	// here (bounce limits, unshaded) or the ray is a transparency peel, the surface is plain diffuse.
	bool path_ends = get_total_bounces(ps.packed_bounces_flags) >= RT_GET_MAX_BOUNCES() ||
			get_diffuse_bounces(ps.packed_bounces_flags) >= MAX_DIFFUSE_BOUNCES;
	if ((ps.packed_bounces_flags & PEEL_RAY_FLAG) != 0u || surface_is_unshaded(h.geometry_idx) || path_ends) {
		m.subsurface = 0.0;
	}

	vec3 V = -gl_WorldRayDirectionEXT;

	// Clamp shading normal toward geometry at grazing view angles to keep BRDF above the geometry hemisphere.
	vec3 N = clampShadingNormal(m.normal, h.geometry_normal, V, RT_SHADING_NORMAL_CLAMP_THRESHOLD);
	float NdotV = max(dot(N, V), 0.0001);

	if ((ps.packed_bounces_flags & (CAUSTIC_PHOTON_FLAG | PEEL_RAY_FLAG)) == CAUSTIC_PHOTON_FLAG) {
		// A caustic photon (raytracing_caustics_inc.glsl): report the surface, shade nothing.
		ps.packed_bounces_flags &= ~MEDIUM_CHANGE_FLAG;
		ps.bsdf_pdf = 0.0;
		ps.offset_normal = h.geometry_normal;
		if (m.transmission > 0.0 && m.ior >= 1.0 && rt_material_feature(RT_MATERIAL_FEATURE_REFRACTION)) {
			ps.packed_bounces_flags |= MEDIUM_CHANGE_FLAG;
			ps.offset_normal = -h.geometry_normal;
			ps.next_ray_dir = gl_WorldRayDirectionEXT;
			if (h.is_front_face) {
				vec3 t = refract(gl_WorldRayDirectionEXT, N, 1.0 / m.ior);
				if (dot(t, t) > 0.0) {
					ps.next_ray_dir = normalize(t);
					ps.bsdf_pdf = 1.0;
				}
			} else {
				// A back face: the ray came from inside this medium. Its code, with the sign bit set
				// (a negative finite float, so photons still read it as "not entering"), for the
				// camera's medium probe (rt_camera_medium()).
				ps.bsdf_pdf = uintBitsToFloat(m.medium | 0x80000000u);
				if ((m.medium & RT_MEDIUM_CODE_SCATTER) != 0u) {
					imageStore(rt_medium_image, ivec2(gl_LaunchIDEXT.xy), uvec4(m.medium_scatter));
				}
			}
		}
		path_pack(payload, ps);
		return;
	}

#ifdef DLSS_RR_ENABLED
	if ((ps.packed_bounces_flags & GUIDE_RAY_FLAG) != 0u) {
		dlss_rr_guide_hit(h, m, N, V, NdotV, ps.packed_bounces_flags);
		return;
	}
#endif

	if ((ps.packed_bounces_flags & PEEL_RAY_FLAG) != 0u) {
		// Peel layer: shade like an opaque hit, apply its blend operation, no bounce.
		// Raygen pre-scales throughput by the running transmittance and owns
		// path continuation; this only reports (t, alpha) and its own light.
		uint mat_flags = rt_entry_material_flags(h.geometry_idx);
		uint bclass = (mat_flags & RT_MAT_BLEND_CLASS_MASK) >> RT_MAT_BLEND_CLASS_SHIFT;
		bool depth_barrier = (mat_flags & RT_MAT_FLAG_DEPTH_DRAW_ALWAYS) != 0u;
		float material_alpha = clamp(m.alpha, 0.0, 1.0);
		float peel_alpha;
		float continuation_alpha = 0.0;
		vec3 fogged_throughput = ps.throughput;
		vec3 branch_fog = vec3(0.0);
		apply_segment_fog(gl_HitTEXT, mat_flags, get_total_bounces(ps.packed_bounces_flags) == 0u, branch_fog, fogged_throughput);
		if (bclass == RT_BLEND_CLASS_MUL) {
			// Mul contributes no light; its color folds into the scalar T.
			peel_alpha = clamp(1.0 - luminance(m.albedo), 0.0, 1.0);
		} else {
			// Add/Sub scale their source by alpha but do not attenuate the
			// destination. Premultiplied alpha already includes alpha in RGB.
			peel_alpha = (bclass == RT_BLEND_CLASS_ADD || bclass == RT_BLEND_CLASS_SUB) ? 0.0 : material_alpha;
			ps.throughput = fogged_throughput;
			path_pack(payload, ps);
			vec3 lit = shade_local_surface(h, m, N, V,
					get_diffuse_bounces(ps.packed_bounces_flags) > 0u, ps.bsdf_pdf, ps.rng_state);
			float source_alpha = (bclass == RT_BLEND_CLASS_PREMULT) ? 1.0 : material_alpha;
			vec3 contribution = ps.throughput * source_alpha * lit;
			if (bclass == RT_BLEND_CLASS_SUB) {
				ps.radiance -= contribution;
			} else {
				ps.radiance += contribution;
			}
			// Add/Sub don't attenuate the destination (peel_alpha above), but they
			// still need a chance at their own indirect bounce so they can pick up
			// GI/sky ambient like every other blend class does. Premult adds its
			// direct light unscaled by alpha, so its bounce must be unscaled too.
			continuation_alpha = (bclass == RT_BLEND_CLASS_PREMULT) ? 1.0 : material_alpha;
			if (bclass != RT_BLEND_CLASS_ADD && bclass != RT_BLEND_CLASS_SUB) {
				ps.radiance += branch_fog * material_alpha;
			}

			// Dominant surface: the first peel layer on the primary segment
			// with alpha >= the dominant layer alpha owns depth/velocity and
			// blends into RR guides. depth_draw_always owns depth regardless of its alpha.
			// Premult reflects at full strength at any alpha, so it always
			// composites into the RR guides.
			float dominant_alpha = get_rt_param(RT_PARAM_TRANSPARENCY_DOMINANT_LAYER_ALPHA);
			bool primary_first_layer = get_total_bounces(ps.packed_bounces_flags) == 0u && is_sample_zero(ps.packed_bounces_flags) &&
					(ps.packed_bounces_flags & PEEL_FIRST_LAYER_FLAG) != 0u;
			bool dominant = depth_barrier || (dominant_alpha > 0.0 && peel_alpha >= dominant_alpha);
			if (primary_first_layer && dominant) {
				rt_store_ndc_depth(h.hit_pos);
				rt_store_velocity(h.hit_pos);
			}
#ifdef DLSS_RR_ENABLED
			bool premultiplied = bclass == RT_BLEND_CLASS_PREMULT;
			if (primary_first_layer && (dominant || premultiplied)) {
				dlss_rr_composite_peel_guides(h, m, N, V, NdotV, depth_barrier ? material_alpha : peel_alpha, premultiplied);
			}
#endif
		}
		ps.packed_bounces_flags = set_peel_alpha(ps.packed_bounces_flags, peel_alpha);
		if (depth_barrier) {
			ps.packed_bounces_flags |= PEEL_DEPTH_BARRIER_HIT_FLAG;
		}
		ps.packed_bounces_flags &= ~PEEL_HAS_CONTINUATION_FLAG;
		// Continue with probability continuation_alpha rather than scaling throughput by
		// it, so faint layers rarely spawn a full indirect path.
		if (rand(ps.rng_state) < continuation_alpha && sample_surface_continuation(h, m, N, V, ps)) {
			ps.packed_bounces_flags |= PEEL_HAS_CONTINUATION_FLAG;
		}
		path_pack(payload, ps);
		return;
	}

	uint total_bounces = get_total_bounces(ps.packed_bounces_flags);
	uint diffuse_bounces = get_diffuse_bounces(ps.packed_bounces_flags);

	// Environment fog for this ray segment (before surface contribution).
	apply_segment_fog(gl_HitTEXT, rt_entry_material_flags(h.geometry_idx), total_bounces == 0u, ps.radiance, ps.throughput);

	// Radiance cache: a rough surface a bounce reaches reads all it sends back, its own light
	// included, from the cell around it, and the path ends. Training pixels shade and trace
	// on, and raygen records what this surface sent back (raytracing_radiance_cache_inc.glsl).
	// Points under water caustics shade on, since a cell would average the lines away.
	bool rc_eligible = total_bounces == 1u && m.roughness >= RT_RC_MIN_ROUGHNESS && rt_rc_enabled();
	bool rc_training = rc_eligible && rt_rc_is_training_pixel(gl_LaunchIDEXT.xy);
	if (rc_eligible && !rc_training && !is_singular_path(ps.packed_bounces_flags) &&
			gl_HitTEXT >= 0.5 * rt_rc_cell_size(h.hit_pos) && !rt_caustics_covers(h.hit_pos)) {
		vec4 cached = rt_rc_read(h.hit_pos, h.geometry_normal);
		if (cached.w > 0.0) {
			ps.radiance += ps.throughput * cached.rgb;
			ps.packed_bounces_flags = set_path_terminated(ps.packed_bounces_flags);
			path_pack(payload, ps);
			return;
		}
	}

	path_pack(payload, ps);
	ps.radiance += ps.throughput * shade_local_surface(h, m, N, V, diffuse_bounces > 0u, ps.bsdf_pdf, ps.rng_state);

	// =================================================================
	// DLSS Ray Reconstruction output (primary ray, sample 0 only)
	// =================================================================
#ifdef DLSS_RR_ENABLED
	if (total_bounces == 0u && is_sample_zero(ps.packed_bounces_flags)) {
		ivec2 pixel = ivec2(gl_LaunchIDEXT.xy);
		dlss_rr_store_albedo_guides(m, NdotV);
		float rr_roughness = dlss_rr_roughness(m);
		imageStore(dlss_rr_normal_roughness, pixel, vec4(N, rr_roughness));
		vec2 specular_mv = dlss_rr_specular_motion_vector(h.hit_pos, rt_hit_prev_world_pos(h.hit_pos), h.geometry_normal, V, rr_roughness, RT_VIS_MASK);
		dlss_rr_write_specular_motion_vector(specular_mv);
		// Metals show nothing of their own, so primary motion follows the reflection (which is the
		// surface motion again past the spec MV roughness limit).
		if (m.metalness >= DLSS_RR_SPECULAR_ONLY_METALNESS) {
			imageStore(rt_velocity_image, pixel, vec4(specular_mv, 0.0, 0.0));
		}
		// The path's next hit (or raygen's fallback walk) replaces these guides with the surface seen through the mirror.
		if (dlss_rr_is_replaceable_mirror(h, m)) {
			ps.packed_bounces_flags |= DLSS_RR_REPLACE_PENDING_FLAG;
		}
	} else if (total_bounces == 1u && (ps.packed_bounces_flags & DLSS_RR_REPLACE_PENDING_FLAG) != 0u &&
			is_sample_zero(ps.packed_bounces_flags) && is_singular_path(ps.packed_bounces_flags) &&
			!dlss_rr_is_replaceable_mirror(h, m)) {
		// Singular bounce off the replaceable primary mirror: this hit's image across the mirror plane
		// stands in for the mirror. Exact even though the bounce direction was sampled. A second
		// mirror leaves the flag set for raygen's walk.
		vec4 mirror_normal_roughness = imageLoad(dlss_rr_normal_roughness, ivec2(gl_LaunchIDEXT.xy));
		mat3 mirror_xform = dlss_rr_householder(mirror_normal_roughness.xyz);
		vec3 mirror_pos = gl_WorldRayOriginEXT;
		dlss_rr_write_replaced_guides(mirror_pos + mirror_xform * (h.hit_pos - mirror_pos),
				mirror_xform * (rt_hit_prev_world_pos(h.hit_pos) - h.hit_pos), vec3(1.0),
				dlss_rr_diffuse_guide(m), dlss_rr_compute_specular_albedo(m, NdotV));
		ps.packed_bounces_flags &= ~DLSS_RR_REPLACE_PENDING_FLAG;
	}
#endif

	if (!sample_surface_continuation(h, m, N, V, ps)) {
		ps.packed_bounces_flags = set_path_terminated(ps.packed_bounces_flags);
		ps.offset_normal = h.geometry_normal;
	}
	if (rc_training) {
		ps.packed_bounces_flags |= RT_RC_ELIGIBLE_FLAG;
	}
	path_pack(payload, ps);
}

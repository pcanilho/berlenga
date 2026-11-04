// Common closest_hit utilities shared by all hit groups.
//
// Required includes (before this file):
//   raytracing_inc.glsl, brdf_inc.glsl, raytracing_hit_inc.glsl, raytracing_lights_inc.glsl,
//   raytracing_material_eval_inc.glsl
//
// Required bindings (before this file):
//   tlas, payload, scene_data_block, geometries[], motion_indices[], materials[], motion_transforms[], bindless_textures[],
//   SAMPLER_* (12 material samplers), rt_params, rt_depth_image,
//   DLSS-RR images (ifdef DLSS_RR_ENABLED)

// ============================================================================
// HIT DATA
// ============================================================================

struct HitData {
	vec3 hit_pos;
	vec3 geometry_normal; // World space, flipped for back-face hits.
	vec3 tangent; // World space.
	vec3 bitangent; // World space.
	vec2 uv; // Raw UV (no material scale/offset applied).
	vec4 color; // Vertex color (white if not present).
	bool is_front_face;
	uint geometry_idx;
};

// The hit's texture level of detail before each texture's own size (ray cones,
// rt_texture_cone): the material samplers add 0.5 * log2(width * height). Without a
// cone (no UVs, a zero width) it stays far negative, so every texture reads level 0.
const float RT_TEXTURE_LOD_BASE = -1e30;
float rt_texture_lod = RT_TEXTURE_LOD_BASE;
// log2 of the cone's footprint on the surface (world units), for materials that
// map textures by position (triplanar) or scale the mesh's UVs.
float rt_cone_footprint_log2 = RT_TEXTURE_LOD_BASE;
// The cone's footprint ellipse in the mesh's UV space (rt_texture_cone): its major
// and minor axes, as the screen-space derivatives raster texturing takes. When set,
// textures are sampled with these gradients through the anisotropic sampler, so a
// floor seen at a grazing angle keeps its detail across the ray as raster does.
vec2 rt_uv_dx = vec2(0.0);
vec2 rt_uv_dy = vec2(0.0);
bool rt_uv_grad = false;

/// A vertex's position in the BLAS's object space (as gl_ObjectToWorldEXT takes it):
/// the decoded copy a flat compressed surface's BLAS was built from, a compressed
/// surface's 16-bit fractions of its AABB (its BLAS is in that unit cube), or floats.
vec3 fetch_object_position(in GeometryData geom, uint idx) {
	uint64_t decoded = packUint2x32(uvec2(geom.blas_position_address_lo, geom.blas_position_address_hi));
	if (decoded != 0ul) {
		FloatBuffer pb = FloatBuffer(decoded);
		return vec3(pb.v[idx * 3u], pb.v[idx * 3u + 1u], pb.v[idx * 3u + 2u]);
	}
	Uint32Buffer vb = Uint32Buffer(geom.vertex_address);
	uint stride_words = max(geom.position_stride >> 2, 1u);
	if ((geom.flags & FLAG_COMPRESSED) != 0u) {
		uint w0 = vb.v[idx * stride_words];
		uint w1 = vb.v[idx * stride_words + 1u];
		return vec3(float(w0 & 0xFFFFu), float(w0 >> 16), float(w1 & 0xFFFFu)) / 65535.0;
	}
	return vec3(uintBitsToFloat(vb.v[idx * stride_words]), uintBitsToFloat(vb.v[idx * stride_words + 1u]), uintBitsToFloat(vb.v[idx * stride_words + 2u]));
}

/// Ray cones (Akenine-Moller, Nilsson, Andersson, Barre-Brisebois, Toth & Karras,
/// "Texture Level of Detail Strategies for Real-Time Ray Tracing", Ray Tracing Gems
/// 2019, ch. 20; and "Improved Shader and Texture Level of Detail Using Ray Cones",
/// JCGT 2021). The cone's width grows by the pixel's spread angle over each segment,
/// carried in the payload from the eye (or from an orthogonal camera's pixel
/// footprint), and the level is the triangle's texel density, 0.5 * log2(uv area /
/// world area), plus log2(width / |n . d|). Ray tracing stages have no screen-space
/// derivatives, so texture() would always read the base level and distant detail
/// would alias into fuzz under the denoiser. Bounces keep the eye's spread (no
/// curvature term), which never blurs more than the paper's.
void rt_texture_cone(in GeometryData geom, vec3 world_normal) {
	rt_texture_lod = RT_TEXTURE_LOD_BASE;
	rt_cone_footprint_log2 = RT_TEXTURE_LOD_BASE;
	rt_uv_grad = false;
	// the cone at this hit
	float p11 = scene_data_block.data.projection_matrix[1][1];
	bool ortho = abs(scene_data_block.data.projection_matrix[2][3]) < 1e-6;
	float spread = ortho ? 0.0 : atan(2.0 / (abs(p11) * float(gl_LaunchSizeEXT.y)));
	float width = get_cone_width(payload.packed_bounces_flags) + spread * gl_HitTEXT;
	payload.packed_bounces_flags = set_cone_width(payload.packed_bounces_flags, width);
	if (width <= 0.0) {
		return;
	}
	// its footprint on the surface, in world units
	float cos_n = max(abs(dot(world_normal, gl_WorldRayDirectionEXT)), 0.05);
	rt_cone_footprint_log2 = log2(width / cos_n);

	if (geom.uv_byte_offset == OFFSET_NONE || geom.attribute_address == 0ul || geom.vertex_address == 0ul) {
		return;
	}
	// the triangle's texel density
	uint i0, i1, i2;
	get_triangle_indices(geom, i0, i1, i2);
	mat4x3 o2w = gl_ObjectToWorldEXT;
	vec3 p0 = o2w * vec4(fetch_object_position(geom, i0), 1.0);
	vec3 p1 = o2w * vec4(fetch_object_position(geom, i1), 1.0);
	vec3 p2 = o2w * vec4(fetch_object_position(geom, i2), 1.0);
	vec2 t0 = fetch_uv(geom, i0, i1, i2, vec3(1.0, 0.0, 0.0));
	vec2 t1 = fetch_uv(geom, i0, i1, i2, vec3(0.0, 1.0, 0.0));
	vec2 t2 = fetch_uv(geom, i0, i1, i2, vec3(0.0, 0.0, 1.0));
	float world_area = length(cross(p1 - p0, p2 - p0));
	vec2 e1 = t1 - t0;
	vec2 e2 = t2 - t0;
	float uv_area = abs(e1.x * e2.y - e1.y * e2.x);
	if (world_area <= 1e-12 || uv_area <= 1e-14) {
		return;
	}
	rt_texture_lod = 0.5 * log2(uv_area / world_area) + rt_cone_footprint_log2;

	// Anisotropic footprint (JCGT 2021, sec. 4.3 / 5): the cone meets the triangle's
	// plane in an ellipse, its minor axis the cone's width across the ray and its major
	// axis that width over the cosine, along the ray's projection on the plane. Mapped
	// into UV space through the triangle's UV gradients, the two axes are the
	// derivatives raster would see, and textureGrad filters them anisotropically
	// instead of blurring the whole footprint to the major axis.
	vec3 dp1 = p1 - p0;
	vec3 dp2 = p2 - p0;
	vec3 tri_n = cross(dp1, dp2);
	float nn = dot(tri_n, tri_n);
	vec3 c1 = cross(dp2, tri_n) / nn;
	vec3 c2 = cross(tri_n, dp1) / nn;
	vec3 grad_u = e1.x * c1 + e2.x * c2; // world-space gradient of u on the plane
	vec3 grad_v = e1.y * c1 + e2.y * c2;
	vec3 plane_n = tri_n * inversesqrt(nn);
	vec3 dir = gl_WorldRayDirectionEXT;
	vec3 minor_dir = cross(dir, plane_n);
	float minor_len = length(minor_dir);
	if (minor_len < 1e-4) {
		// Head-on: a circle, any in-plane frame will do.
		minor_dir = normalize(cross(plane_n, abs(plane_n.x) < 0.9 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0)));
	} else {
		minor_dir /= minor_len;
	}
	vec3 major_dir = cross(plane_n, minor_dir);
	float cos_plane = max(abs(dot(plane_n, dir)), 0.05);
	vec3 major_axis = major_dir * (width / cos_plane);
	vec3 minor_axis = minor_dir * width;
	rt_uv_dx = vec2(dot(grad_u, major_axis), dot(grad_v, major_axis));
	rt_uv_dy = vec2(dot(grad_u, minor_axis), dot(grad_v, minor_axis));
	vec4 grads = vec4(rt_uv_dx, rt_uv_dy);
	rt_uv_grad = !any(isnan(grads)) && !any(isinf(grads));
}

/// Fetch vertex attributes and transform to world space.
/// Requires hitAttributeEXT HitAttribs and GeometryBuffer/MaterialBuffer bindings.
HitData compute_hit_data() {
	HitData h;
	h.geometry_idx = gl_InstanceCustomIndexEXT;
	GeometryData geom = geometries[h.geometry_idx];

	// Custom hit groups may reference vertex color in their fragment code, so
	// pull all attributes; default HGs can skip color for perf.
#ifdef RT_CUSTOM_HIT_GROUP
	VertexAttributes attrs = fetch_vertex_attributes(geom, attribs, FETCH_ALL);
#else
	VertexAttributes attrs = fetch_vertex_attributes(geom, attribs, FETCH_UV | FETCH_TBN);
#endif
	h.uv = attrs.uv;
	h.color = attrs.color;

	mat3 model_rotation = mat3(gl_ObjectToWorldEXT);
	mat3 normal_matrix = mat3(
			normalize(model_rotation[0]),
			normalize(model_rotation[1]),
			normalize(model_rotation[2]));

#ifdef ENABLE_INTERSECTION_SHADERS
	if ((geom.flags & FLAG_PROCEDURAL) != 0u) {
		h.uv = hit_attribs.bary_or_uv;
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

		h.is_front_face = (gl_HitKindEXT == gl_HitKindFrontFacingTriangleEXT);
		if (!h.is_front_face) {
			h.geometry_normal = -h.geometry_normal;
		}
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

vec4 sample_bindless_texture(uint tex_idx, vec2 uv) {
	// The samplers are the render buffers' own (binding 16-27), so their mipmap bias
	// (the upscaler's render-to-display ratio, TAA's and the project's) applies here
	// as it does in raster, to the gradients and to the explicit level alike.
	if (rt_uv_grad) {
		return textureGrad(sampler2D(bindless_textures[nonuniformEXT(tex_idx)], SAMPLER_LINEAR_WITH_MIPMAPS_ANISOTROPIC_REPEAT), uv, rt_uv_dx, rt_uv_dy);
	}
	// the hit's ray-cone level, plus this texture's own size
	ivec2 size = textureSize(sampler2D(bindless_textures[nonuniformEXT(tex_idx)], SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT), 0);
	float lod = max(rt_texture_lod + 0.5 * log2(float(max(size.x, 1) * max(size.y, 1))), 0.0);
	return textureLod(sampler2D(bindless_textures[nonuniformEXT(tex_idx)], SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT), uv, lod);
}

/// Sample with point/nearest filtering (for pixel art textures).
vec4 sample_bindless_texture_point(uint tex_idx, vec2 uv) {
	return texture(sampler2D(bindless_textures[nonuniformEXT(tex_idx)], SAMPLER_NEAREST_REPEAT), uv);
}

/// Sample with the appropriate filter based on material flags (bit 2 = point filtering).
vec4 sample_material_texture(uint tex_idx, vec2 uv, uint mat_flags) {
	if ((mat_flags & 4u) != 0u) {
		return sample_bindless_texture_point(tex_idx, uv);
	}
	return sample_bindless_texture(tex_idx, uv);
}

/// Apply tangent-space normal map to geometry normal.
vec3 apply_normal_map(HitData h, vec3 tangent_space_normal, float normal_map_depth) {
	vec3 mapped = h.tangent * tangent_space_normal.x + h.bitangent * tangent_space_normal.y + h.geometry_normal * tangent_space_normal.z;
	return normalize(mix(h.geometry_normal, mapped, normal_map_depth));
}

// ============================================================================
// UV1 TRIPLANAR (StandardMaterial3D parity under PT)
// ============================================================================
// Raster triplanar lives in BaseMaterial3D's GENERATED shader code, which the
// PT hit path never runs — so PT reimplements the same math from the record:
// weights = pow(abs(N), sharpness)/sum, pos = P * uv1_scale + uv1_offset then
// pos *= (1,-1,1), samples xy*w.z + xz*w.y + zy*(-1,1)*w.x (material.cpp
// ~1391-1473 is the reference). Object mode uses MODEL space: for compressed
// meshes BLAS object space is the unit-cube compression space, so positions
// go through aabb_xform and normals through the model matrix transpose
// (same construction as read_model_matrix in the custom-fragment path).

struct TriplanarCoords {
	vec3 weights;
	vec3 pos;
	vec3 tangent; // World-space axis-aligned frame for normal mapping,
	vec3 binormal; // matching raster's generated TANGENT/BINORMAL.
};

TriplanarCoords compute_triplanar_coords(HitData h, MaterialData mat, GeometryData geom) {
	TriplanarCoords tc;
	bool world_mode = (mat.flags & 32u) != 0u;
	vec3 n; // Blend-space normal (world or model).
	vec3 p; // Blend-space position.
	mat4 model = mat4(1.0);
	if (world_mode) {
		n = h.geometry_normal;
		p = h.hit_pos;
	} else {
		mat4 aabb_xform, inv_aabb_xform;
		get_aabb_compression_xforms(geom, aabb_xform, inv_aabb_xform);
		model = mat4(gl_ObjectToWorldEXT) * inv_aabb_xform;
		// n_model = transpose(M) * n_world (inverse of the inverse-transpose).
		n = normalize((vec4(h.geometry_normal, 0.0) * model).xyz);
		p = (aabb_xform * vec4(gl_ObjectRayOriginEXT + gl_HitTEXT * gl_ObjectRayDirectionEXT, 1.0)).xyz;
	}

	// Triplanar maps by position, not the mesh's UVs: the isotropic level below.
	rt_uv_grad = false;

	tc.weights = pow(abs(n), vec3(mat.triplanar_sharpness));
	tc.weights /= dot(tc.weights, vec3(1.0));

	p = p * vec3(mat.uv1_scale, mat.uv1_scale_z) + vec3(mat.uv1_offset, mat.uv1_offset_z);
	tc.pos = p * vec3(1.0, -1.0, 1.0);

	// ray cone level: texture units per world unit are uv1_scale (per model unit
	// in object mode, so divided by the model's own scale)
	float tex_per_world = (abs(mat.uv1_scale.x) + abs(mat.uv1_scale.y) + abs(mat.uv1_scale_z)) / 3.0;
	if (!world_mode) {
		tex_per_world /= max(pow(abs(determinant(mat3(model))), 1.0 / 3.0), 1e-6);
	}
	rt_texture_lod = rt_cone_footprint_log2 + log2(max(tex_per_world, 1e-6));

	// Raster's axis-aligned triplanar frame (material.cpp ~1369-1388),
	// brought to world space for object mode.
	vec3 t = vec3(0.0, 0.0, -1.0) * abs(n.x) + vec3(1.0, 0.0, 0.0) * abs(n.y) + vec3(1.0, 0.0, 0.0) * abs(n.z);
	vec3 b = vec3(0.0, 1.0, 0.0) * abs(n.x) + vec3(0.0, 0.0, -1.0) * abs(n.y) + vec3(0.0, 1.0, 0.0) * abs(n.z);
	if (!world_mode) {
		t = mat3(model) * t;
		b = mat3(model) * b;
	}
	tc.tangent = normalize(t);
	tc.binormal = normalize(b);
	return tc;
}

vec4 sample_material_texture_triplanar(uint tex_idx, TriplanarCoords tc, uint mat_flags) {
	vec4 samp = vec4(0.0);
	samp += sample_material_texture(tex_idx, tc.pos.xy, mat_flags) * tc.weights.z;
	samp += sample_material_texture(tex_idx, tc.pos.xz, mat_flags) * tc.weights.y;
	samp += sample_material_texture(tex_idx, tc.pos.zy * vec2(-1.0, 1.0), mat_flags) * tc.weights.x;
	return samp;
}

/// Normal mapping over the triplanar frame instead of mesh tangents.
vec3 apply_normal_map_triplanar(HitData h, TriplanarCoords tc, vec3 tangent_space_normal, float normal_map_depth) {
	vec3 mapped = tc.tangent * tangent_space_normal.x + tc.binormal * tangent_space_normal.y + h.geometry_normal * tangent_space_normal.z;
	return normalize(mix(h.geometry_normal, mapped, normal_map_depth));
}

// ============================================================================
// DEPTH WRITE (primary ray only)
// ============================================================================

/// Write NDC depth for primary ray hits (bounce 0, sample 0 only).
void write_primary_hit_depth(vec3 hit_pos) {
	if (get_total_bounces(payload.packed_bounces_flags) == 0u && is_sample_zero(payload.packed_bounces_flags)) {
		mat4 view_mat = transpose(mat4(scene_data_block.data.view_matrix[0],
				scene_data_block.data.view_matrix[1],
				scene_data_block.data.view_matrix[2],
				vec4(0.0, 0.0, 0.0, 1.0)));
		vec3 view_pos = (view_mat * vec4(hit_pos, 1.0)).xyz;
		vec4 clip_pos = scene_data_block.data.projection_matrix * vec4(view_pos, 1.0);
		float ndc_depth = clip_pos.z / clip_pos.w;
		imageStore(rt_depth_image, ivec2(gl_LaunchIDEXT.xy), vec4(ndc_depth));
	}
}

// ============================================================================
// VELOCITY WRITE (primary ray only, MV-gated)
// ============================================================================

#ifdef ENABLE_INTERSECTION_SHADERS
/// Decode the FP16-compressed PREV_POSITION delta from HitAttribs.
vec3 decode_prev_pos_delta() {
	uint dx_low = (hit_attribs.packed_normal >> 24u) & 0xFFu;
	uint dx_high = (hit_attribs.packed_tangent >> 24u) & 0xFFu;
	float delta_x = unpackHalf2x16(dx_low | (dx_high << 8u)).x;
	vec2 delta_yz = unpackHalf2x16(hit_attribs.prev_pos_delta_yz);
	return vec3(delta_x, delta_yz.x, delta_yz.y);
}
#endif

/// Reconstruct previous-frame mat4 from a compact motion transform entry.
mat4 decode_prev_object_to_world(int motion_idx) {
	InstanceMotionData m = motion_transforms[motion_idx];
	return transpose(mat4(
			vec4(m.prev_xform[0], m.prev_xform[1], m.prev_xform[2], m.prev_xform[3]),
			vec4(m.prev_xform[4], m.prev_xform[5], m.prev_xform[6], m.prev_xform[7]),
			vec4(m.prev_xform[8], m.prev_xform[9], m.prev_xform[10], m.prev_xform[11]),
			vec4(0.0, 0.0, 0.0, 1.0)));
}

/// Write motion vectors for primary ray hits (bounce 0, sample 0 only).
/// Uses unjittered VP matrices matching the raster motion_vectors_store convention.
void write_primary_hit_velocity(vec3 hit_pos) {
	if (get_total_bounces(payload.packed_bounces_flags) != 0u || !is_sample_zero(payload.packed_bounces_flags)) {
		return;
	}

	uint geom_idx = gl_InstanceCustomIndexEXT;
	int mi = motion_indices[geom_idx];

	// Resolve previous-frame model matrix: compact entry if moved, current transform otherwise.
	mat4 prev_model = (mi >= 0) ? decode_prev_object_to_world(mi) : mat4(gl_ObjectToWorldEXT);

	vec3 obj_pos = (mat4(gl_WorldToObjectEXT) * vec4(hit_pos, 1.0)).xyz;
	vec3 prev_obj_pos = obj_pos;

	GeometryData geom = geometries[geom_idx];

#ifdef ENABLE_INTERSECTION_SHADERS
	if ((geom.flags & FLAG_PROCEDURAL) != 0u) {
		prev_obj_pos += decode_prev_pos_delta();
	}
#endif

#ifdef RT_HIT_ATTRIBS_DECLARED
	if ((geom.flags & FLAG_DEFORMED) != 0u) {
		uint64_t prev_addr = packUint2x32(uvec2(geom.prev_vertex_address_lo, geom.prev_vertex_address_hi));
		if (prev_addr != 0ul) {
			uint i0, i1, i2;
			get_triangle_indices(geom, i0, i1, i2);
			vec3 bary = vec3(1.0 - attribs.x - attribs.y, attribs.x, attribs.y);
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
			prev_obj_pos = bary.x * p0 + bary.y * p1 + bary.z * p2;
		}
	}
#endif

	vec3 prev_world_pos = (prev_model * vec4(prev_obj_pos, 1.0)).xyz;

	vec2 curr_uv = project_uv(hit_pos, curr_vp_unjittered);
	vec2 prev_uv = project_uv(prev_world_pos, prev_vp_unjittered);

	imageStore(rt_velocity_image, ivec2(gl_LaunchIDEXT.xy), vec4(prev_uv - curr_uv, 0.0, 0.0));
}

// ============================================================================
// ENVIRONMENT FOG (per ray segment)
// ============================================================================

vec3 fog_get_directional_color(uint index) {
	return rt_lights[index].emission;
}

vec3 fog_get_directional_direction(uint index) {
	vec3 world_dir = -normalize(rt_lights[index].position);
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

/// Apply environment fog for the ray segment that was just traversed.
/// Attenuates throughput and adds in-scattered fog color.
void apply_segment_fog(float segment_dist, inout vec3 radiance, inout vec3 throughput) {
	if ((RT_FLAGS & RT_FLAG_FOG_ENABLED) == 0u) {
		return;
	}

	// Build a view-space vertex along the ray direction at the hit distance.
	// fog_process needs view-space position for distance and height calculations.
	mat4 view_mat = transpose(mat4(
			scene_data_block.data.view_matrix[0],
			scene_data_block.data.view_matrix[1],
			scene_data_block.data.view_matrix[2],
			vec4(0.0, 0.0, 0.0, 1.0)));
	vec3 world_hit = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * segment_dist;
	vec3 vertex = (view_mat * vec4(world_hit, 1.0)).xyz;

	vec4 fog = fog_process(scene_data_block.data, vertex);
	radiance += throughput * fog.rgb * fog.a;
	throughput *= (1.0 - fog.a);
}

/// Converts specular parameter [0..1] to dielectric F0.
float specular_to_f0(float specular) {
	return 0.16 * specular * specular;
}

// ============================================================================
// DEBUG VISUALIZATION
// ============================================================================

#ifdef RT_DEBUG_ENABLED
void debug_visualize(
		int vis_mode,
		vec3 geometry_normal,
		vec3 final_normal,
		vec3 tangent_space_normal,
		vec3 world_tangent,
		vec3 world_bitangent,
		vec2 uv,
		vec3 albedo,
		vec3 orm,
		float metalness,
		float roughness,
		float specular,
		vec3 emissive,
		vec3 V,
		float NdotV) {
	PathState ps = path_unpack(payload);

	if (vis_mode == 1) {
		if (get_total_bounces(ps.packed_bounces_flags) == 0u) {
			ps.packed_bounces_flags = inc_total_bounce(ps.packed_bounces_flags);
			ps.hit_t = gl_HitTEXT;
			ps.offset_normal = geometry_normal;
			ps.next_ray_dir = reflect(gl_WorldRayDirectionEXT, geometry_normal);
			path_pack(payload, ps);
			return;
		} else {
			ps.radiance = geometry_normal * 0.5 + 0.5;
		}
	} else if (vis_mode == 2) {
		ps.radiance = geometry_normal * 0.5 + 0.5;
	} else if (vis_mode == 3) {
		ps.radiance = final_normal * 0.5 + 0.5;
	} else if (vis_mode == 4) {
		ps.radiance = tangent_space_normal * 0.5 + 0.5;
	} else if (vis_mode == 5) {
		ps.radiance = world_tangent * 0.5 + 0.5;
	} else if (vis_mode == 6) {
		ps.radiance = world_bitangent * 0.5 + 0.5;
	} else if (vis_mode == 7) {
		ps.radiance = vec3(fract(uv), 0.0);
	} else if (vis_mode == 8) {
		ps.radiance = albedo;
	} else if (vis_mode == 9) {
		ps.radiance = orm;
	} else if (vis_mode == 10) {
		ps.radiance = DLSSRR_computeDiffuseAlbedo(albedo, metalness);
	} else if (vis_mode == 11) {
		ps.radiance = DLSSRR_computeSpecularAlbedo(albedo, metalness, specular_to_f0(specular), roughness, NdotV);
	} else if (vis_mode == 12) {
		ps.radiance = (final_normal * 0.5 + 0.5) * (1.0 - roughness * 0.5);
	} else if (vis_mode == 13) {
		if (get_total_bounces(ps.packed_bounces_flags) == 0u) {
			if (roughness < MAX_DENOISER_SPECULAR_HIT_THRESHOLD) {
				ps.packed_bounces_flags = inc_total_bounce(ps.packed_bounces_flags);
				ps.radiance = vec3(0.1, 0.1, 0.4);
				ps.hit_t = gl_HitTEXT;
				ps.offset_normal = final_normal;
				ps.next_ray_dir = reflect(gl_WorldRayDirectionEXT, final_normal);
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
	} else if (vis_mode == 14) {
		ps.radiance = vec3(metalness);
	} else if (vis_mode == 15) {
		ps.radiance = vec3(roughness);
	} else if (vis_mode == 16) {
		mat3 world_to_view = mat3(scene_data_block.data.inv_view_matrix);
		ps.radiance = normalize(world_to_view * final_normal) * 0.5 + 0.5;
	} else if (vis_mode == 17) {
		vec3 diffuse_albedo = DLSSRR_computeDiffuseAlbedo(albedo, metalness);
		vec3 specular_albedo = DLSSRR_computeSpecularAlbedo(albedo, metalness, specular_to_f0(specular), roughness, NdotV);
		ps.radiance = mix(diffuse_albedo, specular_albedo, metalness);
	} else if (vis_mode == 18) {
		ps.radiance = baseColorToSpecularF0(albedo, metalness, specular_to_f0(specular));
	} else if (vis_mode == 19) {
		bool is_front_face = (gl_HitKindEXT == gl_HitKindFrontFacingTriangleEXT);
		ps.radiance = is_front_face ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
	} else if (vis_mode == 20) {
		float depth_range = scene_data_block.data.z_far - scene_data_block.data.z_near;
		float d = clamp(gl_HitTEXT / depth_range, 0.0, 1.0);
		ps.radiance = vec3(d);
	} else if (vis_mode == 21) {
		ps.radiance = emissive;
	} else if (vis_mode == 22) {
		// BRDF below-hemisphere fallback visualization. Reflects the two-layer
		// recovery applied in shade_and_bounce:
		//   Layer 1 - shading-normal clamp toward geometry (energy preserving
		//             but flattens detail at grazing angles)
		//   Layer 2 - mirror rejected directions across the geometry plane
		//             (biased; reuses the rejected sample's BRDF weight)
		//
		// We sample both BRDF lobes once with the un-clamped shading normal
		// and once with the clamped shading normal, reusing the same random
		// pair so the two passes are directly comparable.
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
		MaterialProperties dbg_mat;
		dbg_mat.baseColor = albedo;
		dbg_mat.metalness = metalness;
		dbg_mat.roughness = roughness;
		dbg_mat.dielectricF0 = specular_to_f0(specular);
		dbg_mat.emissive = vec3(0.0);
		dbg_mat.transmissivness = 0.0;
		dbg_mat.opacity = 1.0;

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

/// Production shading: emissive + NEE direct lighting + BRDF importance sampling + next bounce.
/// Also handles DLSS-RR G-buffer output on primary ray.
void shade_and_bounce(HitData h, MaterialResult m) {
	PathState ps = path_unpack(payload);

	vec3 V = -gl_WorldRayDirectionEXT;

	// Clamp shading normal toward geometry at grazing view angles to keep BRDF above the geometry hemisphere.
	vec3 N = clampShadingNormal(m.normal, h.geometry_normal, V, RT_SHADING_NORMAL_CLAMP_THRESHOLD);
	float NdotV = max(dot(N, V), 0.0001);

	uint total_bounces = get_total_bounces(ps.packed_bounces_flags);
	uint diffuse_bounces = get_diffuse_bounces(ps.packed_bounces_flags);

	// Environment fog for this ray segment (before surface contribution).
	apply_segment_fog(gl_HitTEXT, ps.radiance, ps.throughput);

	// Emissive contribution.
	ps.radiance += ps.throughput * m.emissive;

	// Bounce limit check.
	if (total_bounces >= RT_GET_MAX_BOUNCES() || diffuse_bounces >= MAX_DIFFUSE_BOUNCES) {
		ps.packed_bounces_flags = set_path_terminated(ps.packed_bounces_flags);
		path_pack(payload, ps);
		return;
	}

	// BRDF material setup.
	MaterialProperties brdf_mat;
	brdf_mat.baseColor = m.albedo;
	brdf_mat.metalness = m.metalness;
	brdf_mat.roughness = m.roughness;
	brdf_mat.dielectricF0 = specular_to_f0(m.specular);
	brdf_mat.emissive = m.emissive;
	brdf_mat.transmissivness = 0.0;
	brdf_mat.opacity = 1.0;

	vec3 specularF0 = baseColorToSpecularF0(brdf_mat.baseColor, brdf_mat.metalness, brdf_mat.dielectricF0);
	vec3 diffuseReflectance = baseColorToDiffuseReflectance(brdf_mat.baseColor, brdf_mat.metalness);

	// =================================================================
	// DLSS Ray Reconstruction output (primary ray, sample 0 only)
	// =================================================================
#ifdef DLSS_RR_ENABLED
	if (total_bounces == 0u && is_sample_zero(ps.packed_bounces_flags)) {
		ivec2 pixel = ivec2(gl_LaunchIDEXT.xy);

		// An emissive surface's light is its own, not reflected, so its albedo alone tells Ray
		// Reconstruction nothing of the pattern it glows in (lettering on a sign, a lit screen):
		// the denoiser takes the emission for noisy lighting and spreads it. As the miss shader
		// bakes the sky's colour into this guide to keep its detail, the emission, mapped into
		// [0, 1) by Reinhard, is taken where it is brighter than the albedo.
		vec3 emissive_guide = m.emissive / (vec3(1.0) + m.emissive);
		vec3 diffuse_albedo = DLSSRR_encodeDiffuseAlbedo(max(DLSSRR_computeDiffuseAlbedo(m.albedo, m.metalness), emissive_guide));
		imageStore(dlss_rr_diffuse_albedo, pixel, vec4(diffuse_albedo, 1.0));

		vec3 specular_albedo = DLSSRR_computeSpecularAlbedo(m.albedo, m.metalness, brdf_mat.dielectricF0, m.roughness, NdotV);
		imageStore(dlss_rr_specular_albedo, pixel, vec4(clamp(specular_albedo, vec3(0.04), vec3(1.0)), 1.0)); // match UNORM8 like before - fixes some issues with garbling..

		imageStore(dlss_rr_normal_roughness, pixel, vec4(N, m.roughness));

		// Specular hit distance via inline ray query (only for smooth surfaces).
		float spec_hit_dist = -1.0;
		if (m.roughness < MAX_DENOISER_SPECULAR_HIT_THRESHOLD) {
			vec3 spec_dir = reflect(-V, N);
			vec3 spec_origin = offset_ray_origin(h.hit_pos, spec_dir);

			rayQueryEXT spec_rq;
			rayQueryInitializeEXT(spec_rq, tlas, RT_RAY_FLAGS | gl_RayFlagsTerminateOnFirstHitEXT,
					RT_MASK_SCENE, spec_origin, 0.001, spec_dir, 10000.0);
			while (rayQueryProceedEXT(spec_rq)) {
				if (rayQueryGetIntersectionTypeEXT(spec_rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT) {
					if (ray_query_alpha_test(
								rayQueryGetIntersectionInstanceCustomIndexEXT(spec_rq, false),
								rayQueryGetIntersectionPrimitiveIndexEXT(spec_rq, false),
								rayQueryGetIntersectionBarycentricsEXT(spec_rq, false))) {
						rayQueryConfirmIntersectionEXT(spec_rq);
					}
				}
			}
			if (rayQueryGetIntersectionTypeEXT(spec_rq, true) != gl_RayQueryCommittedIntersectionNoneEXT) {
				spec_hit_dist = rayQueryGetIntersectionTEXT(spec_rq, true);
			}
		}
		imageStore(dlss_rr_specular_hit_dist, pixel, vec4(spec_hit_dist));
	}
#endif

	// =================================================================
	// NEE: Next Event Estimation (direct light sampling)
	// =================================================================
	path_pack(payload, ps);

	uint rt_light_count = uint(get_rt_param(RT_PARAM_LIGHT_COUNT));
	if (rt_light_count > 0u) {
		vec3 hit_pos_offset = offset_ray_origin(h.hit_pos, h.geometry_normal);
		bool is_indirect = (diffuse_bounces > 0u);
		vec3 direct_light = lights_evaluate_direct_lighting(
				hit_pos_offset, N, V, brdf_mat, ps.rng_state, is_indirect, rt_light_count);
		ps.radiance += ps.throughput * direct_light;
	}

	// =================================================================
	// BRDF importance sampling for next bounce
	// =================================================================
	float specularLum = luminance(specularF0);
	float diffuseLum = luminance(diffuseReflectance);

	int brdfType;
	if (diffuseLum < 0.0001) {
		brdfType = SPECULAR_TYPE;
	} else if (specularLum < 0.0001) {
		brdfType = DIFFUSE_TYPE;
	} else {
		float brdfProbability = clamp(specularLum / (specularLum + diffuseLum), 0.01, 0.99);
		if (rand(ps.rng_state) < brdfProbability) {
			brdfType = SPECULAR_TYPE;
			ps.throughput /= brdfProbability;
		} else {
			brdfType = DIFFUSE_TYPE;
			ps.throughput /= (1.0 - brdfProbability);
		}
	}

	vec2 u = rand2(ps.rng_state);
	vec3 next_dir;
	vec3 brdf_weight;
	if (!evalIndirectCombinedBRDF(u, N, h.geometry_normal, V, brdf_mat, brdfType, next_dir, brdf_weight, vec4(0.0))) {
		// Two failure modes:
		//   1) Sample weight is zero (no contribution). Terminate.
		//   2) Sampled direction is below the geometry plane. Recover by
		//      mirroring across the geometry plane (see brdf_inc.glsl). The
		//      mirror is gated by RT_BELOW_HEMISPHERE_RECOVERY_ENABLED; when
		//      disabled, recovery returns false and we terminate the path.
		vec3 recovered_dir;
		if (luminance(brdf_weight) == 0.0 ||
				!recoverBelowHemisphereSample(next_dir, h.geometry_normal, recovered_dir)) {
			ps.packed_bounces_flags = set_path_terminated(ps.packed_bounces_flags);
			path_pack(payload, ps);
			return;
		}
		next_dir = recovered_dir;
	}

	ps.throughput *= brdf_weight;

	if (brdfType == DIFFUSE_TYPE) {
		ps.packed_bounces_flags = inc_diffuse_bounce(ps.packed_bounces_flags);
	} else {
		ps.packed_bounces_flags = inc_total_bounce(ps.packed_bounces_flags);
	}

	// Hand the next ray back to raygen. PATH_TERMINATED_FLAG stays clear so
	// the raygen loop continues with the reconstructed origin and next_ray_dir.
	ps.hit_t = gl_HitTEXT;
	ps.offset_normal = h.geometry_normal;
	ps.next_ray_dir = next_dir;
	path_pack(payload, ps);
}

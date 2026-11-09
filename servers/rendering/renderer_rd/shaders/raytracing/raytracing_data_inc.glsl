// Shared data structures for raytracing shaders.
// Requires: GL_EXT_buffer_reference, GL_ARB_gpu_shader_int64, raytracing_inc.glsl (for OFFSET_NONE/FLAG_*)

// ============================================================================
// BUFFER REFERENCES
// ============================================================================
layout(buffer_reference, std430) readonly buffer FloatBuffer {
	float v[];
};
layout(buffer_reference, std430) readonly buffer Uint32Buffer {
	uint v[];
};

// ============================================================================
// GEOMETRY DATA (matches C++ RT_GeometryData, 160 bytes, 8-byte alignment)
// ============================================================================
struct GeometryData {
	uint64_t vertex_address;
	uint64_t attribute_address;
	uint64_t index_address;

	uint vertex_count;
	uint position_stride;
	uint normal_byte_offset;
	uint normal_stride;
	uint tangent_byte_offset;
	uint tangent_stride;

	uint attribute_stride;
	uint uv_byte_offset;
	uint uv_scale_packed; // fp16 x, fp16 y
	uint index_format;
	uint primitive_count;
	uint flags;
	float aabb_size_x;
	float aabb_size_y;
	float aabb_size_z;

	// Byte offset of the vertex color attribute inside `attribute_stride`,
	// or OFFSET_NONE if the mesh has no vertex colors.
	uint color_byte_offset;

	float aabb_pos_x;
	float aabb_pos_y;
	float aabb_pos_z;

	// For deformed geometry: previous-frame position buffer used for motion vectors.
	uint prev_vertex_address_lo;
	uint prev_vertex_address_hi;

	uint layers;
	uint instance_uniforms_ofs;
	uint uv2_byte_offset;
	uint uv2_scale_packed; // fp16 x, fp16 y
	uint instance_data_primitive_divisor;
	uint64_t instance_data_address;
	uint instance_data_layout; // See rt_pack_instance_data_layout().
	uint debug_state; // See RTDynamicGeometryDebug::pack().
	uint material_index; // See rt_pack_material_index().
	uint material_flags;
	// With FLAG_REST: byte offset from vertex_address of the rest block (vertex() inputs, same layout as the
	// positions and TBN before it).
	uint rest_byte_offset;
	uint rest_pad;
};

// ============================================================================
// PER-INSTANCE MOTION DATA (matches C++ RT_InstanceMotionData, 48 bytes)
// ============================================================================
struct InstanceMotionData {
	float prev_xform[12]; // Previous object-to-world (mat3x4, transposed 3x4)
};

// ============================================================================
// MATERIAL DATA (matches C++ layout, 160 bytes)
// ============================================================================

// MaterialData::flags bits (must match RT_MAT_FLAG_* in render_raytracing.h).
#define RT_MAT_FLAG_HAS_NORMAL_MAP 1u
#define RT_MAT_FLAG_HAS_EMISSION_TEX 2u
#define RT_MAT_FLAG_POINT_FILTER 4u
#define RT_MAT_FLAG_TRIPLANAR 8u
#define RT_MAT_FLAG_TRIPLANAR_WORLD 16u
#define RT_MAT_FLAG_DEPTH_DRAW_ALWAYS 32u
#define RT_MAT_FLAG_UNSHADED 64u
#define RT_MAT_FLAG_TRANSPARENT 128u
#define RT_MAT_FLAG_ALPHA_SCISSOR 256u
#define RT_MAT_FLAG_PROXIMITY_FADE 512u
#define RT_MAT_FLAG_DISTANCE_FADE 1024u
#define RT_MAT_FLAG_FOG_DISABLED 2048u
#define RT_MAT_FLAG_NO_RAY_QUERY_ALPHA 4096u
#define RT_MAT_FLAG_EMISSION_ADD 8192u
#define RT_MAT_FLAG_EMISSION_UV2 16384u
#define RT_MAT_FLAG_UV2_TRIPLANAR 32768u
#define RT_MAT_FLAG_UV2_TRIPLANAR_WORLD 65536u
#define RT_MAT_FLAG_INSTANCE_FADE 131072u
#define RT_MAT_FLAG_ANISOTROPIC 262144u // texture_filter *_ANISOTROPIC: ray-cone gradients sampled anisotropically.
#define RT_MAT_FLAG_TRANSMISSION_FROM_ALPHA 524288u // Refraction: transmission is 1 - alpha and the surface is opaque.
#define RT_MAT_FLAG_TRANSMISSIVE 1048576u // Transmission or Refraction: light passes through, see rt_material_transmission().
#define RT_MAT_FLAG_BACKLIT 2097152u // BaseMaterial3D back lighting: shadow rays keep the light it passes through.
#define RT_MAT_FLAG_CUSTOM_TRANSMITTANCE 4194304u // Custom TRANSMISSION or BACKLIGHT: the any-hit evaluates the fragment for shadow rays.
#define RT_MAT_FLAG_HAIR 16777216u // BaseMaterial3D Hair: fibre scattering where the mask is set, see rt_material_hair().
#define RT_MAT_FLAG_VOLUME 8388608u // Transmission, thin_walled off: the surface bounds a medium, see rt_material_ior(), rt_material_medium().

// Multi-bit fields fill the top bits so new single-bit flags can grow upward.
// Blend class of transparent surfaces (bits 25-27). Opaque surfaces leave 0.
#define RT_MAT_BLEND_CLASS_SHIFT 25u
#define RT_MAT_BLEND_CLASS_MASK (7u << RT_MAT_BLEND_CLASS_SHIFT)
#define RT_BLEND_CLASS_MIX 0u
#define RT_BLEND_CLASS_ADD 1u
#define RT_BLEND_CLASS_SUB 2u
#define RT_BLEND_CLASS_OIT 3u
#define RT_BLEND_CLASS_MUL 4u
#define RT_BLEND_CLASS_PREMULT 5u
// Godot diffuse mode (bits 28-29) and specular mode (bits 30-31), values GODOT_DIFFUSE_* / GODOT_SPECULAR_*.
#define RT_MAT_DIFFUSE_MODE_SHIFT 28u
#define RT_MAT_SPECULAR_MODE_SHIFT 30u
#define RT_MAT_SHADING_MODE_MASK 3u

// Texture channel of the roughness, metallic and AO textures (must match RTTextureChannel in render_raytracing.h).
#define RT_TEXTURE_CHANNEL_GRAYSCALE 4u

// Bitfields of C++ RT_MaterialData, unpacked by the accessors below. *_texture: bindless index in bits 0-23
// (RT_MaterialData::TEXTURE_INDEX_BITS), channel or albedo instance transparency in bits 24-31.
#define RT_TEXTURE_INDEX_BITS 24u
#define RT_TEXTURE_INDEX_MASK 0xFFFFFFu
// GeometryData::material_index keeps the instance transparency where albedo_texture does (C++ RT_MATERIAL_INDEX_MASK).
#define RT_MATERIAL_INDEX_MASK RT_TEXTURE_INDEX_MASK

struct MaterialData {
	uint albedo_texture; // Bits 24-31: transparency of the TLAS entry's instance (set by rt_entry_material()), see rt_instance_alpha().
	uint normal_texture; // Bits 24-31: back light green, see rt_material_backlight().
	uint roughness_texture; // texture_roughness, or texture_orm for ORMMaterial3D.
	uint emission_texture; // Bits 24-31: back light blue.

	vec4 albedo_color;
	vec3 emission_color;
	float emission_strength;

	vec3 uv1_scale; // UV1 scale (default 1,1,1)
	float normal_map_depth; // Normal map strength (default 1.0)
	vec3 uv1_offset; // UV1 offset (default 0,0,0)
	uint surface_params; // unorm8x4: roughness, metallic, specular, back light red. See rt_material_surface_params().

	uint uv2_scale_xy; // fp16x2. UV2 transform: identity unless triplanar or applied by the vertex stage.
	uint flags; // See RT_MAT_FLAG_*.
	uint64_t uniform_address; // BDA for custom shader uniform buffer (0 = none)

	uint blend_sharpness; // fp16x2: UV1, UV2 triplanar per-axis blend exponents (default 1.0)
	uint distance_fade; // fp16x2: distance_fade_min, distance_fade_max.
	uint proximity_fade_clearcoat; // Bits 0-15 fp16 proximity_fade_distance, 16-23 clearcoat, 24-31 clearcoat_roughness (unorm8).
	uint uv2_offset_xy; // fp16x2, modulo 1.
	uint uv2_scale_offset_z; // fp16x2: scale.z, offset.z (modulo 1).
	uint metallic_texture; // texture_metallic, or texture_orm for ORMMaterial3D.
	uint ao_texture; // AO texture when AO is enabled; 0 (white) otherwise.
	uint emission_indirect_transmission; // Bits 0-15 fp16 emission_indirect_energy, 16-23 transmission (unorm8), 24-31 IOR.

	// BaseMaterial3D layer textures, 0 (white) when absent. See material_clearcoat(), material_backlight(),
	// material_transmission().
	uint clearcoat_texture; // Red scales clearcoat, green clearcoat_roughness.
	uint backlight_texture; // Added to the back light colour, as raster.
	uint transmission_texture; // Red scales transmission.
	uint medium; // With RT_MAT_FLAG_VOLUME: the extinction code of the volume it bounds, see rt_medium_absorption().
	uint scatter; // unorm8x4: single-scatter albedo of that volume (rgb) and (g + 1) / 2 of its phase. See rt_material_scatter().
	uint subsurface; // Bits 0-7 unorm8 subsurface scattering strength (0: none), bit 8 Skin Mode. See rt_material_subsurface().
	uint subsurface_texture; // texture_subsurface_scattering: red scales the strength.
	uint hair; // unorm8 beta_m, beta_n, (tilt + 10) / 20; bits 24-25 direction, 26 melanin, 27 cards. See rt_material_hair().
	uint hair_absorption; // Melanin absorption per fibre diameter (rt_medium_absorption() code).
	uint hair_mask_texture; // Where the surface is hair; 0 (white) = everywhere.
	uint hair_crown_xy; // fp16x2 hair_crown, object space.
	uint hair_crown_z; // fp16 in bits 0-15.
};

// Bindless index of a MaterialData *_texture.
uint rt_texture_index(uint p_texture) {
	return p_texture & RT_TEXTURE_INDEX_MASK;
}

// RT_TEXTURE_CHANNEL_* of a MaterialData roughness, metallic or AO texture.
uint rt_texture_channel(uint p_texture) {
	return p_texture >> RT_TEXTURE_INDEX_BITS;
}

// GeometryInstance3D transparency and visibility-parent fade of the TLAS entry, scaling surface alpha.
float rt_instance_alpha(MaterialData p_mat) {
	return 1.0 - float(p_mat.albedo_texture >> RT_TEXTURE_INDEX_BITS) / 255.0;
}

// Coverage of material alpha p_alpha on the TLAS entry. Opaque materials traced as transparent only for the
// instance fade (RT_MAT_FLAG_INSTANCE_FADE) ignore material alpha like raster, except for their alpha scissor.
float rt_entry_alpha(MaterialData p_mat, float p_alpha, float p_scissor_threshold) {
	if ((p_mat.flags & RT_MAT_FLAG_INSTANCE_FADE) != 0u) {
		p_alpha = (p_mat.flags & RT_MAT_FLAG_ALPHA_SCISSOR) != 0u && p_alpha < p_scissor_threshold ? 0.0 : 1.0;
	}
	return p_alpha * rt_instance_alpha(p_mat);
}

// BaseMaterial3D proximity_fade_distance.
float rt_material_proximity_fade_distance(MaterialData p_mat) {
	return unpackHalf2x16(p_mat.proximity_fade_clearcoat).x;
}

// BaseMaterial3D back light colour, linear (stored sRGB in the textures' and surface_params' top bytes).
vec3 rt_material_backlight(MaterialData p_mat) {
	vec3 c = vec3(p_mat.surface_params >> 24u, p_mat.normal_texture >> 24u, p_mat.emission_texture >> 24u) / 255.0;
	return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), greaterThan(c, vec3(0.04045)));
}

// BaseMaterial3D emission_indirect_energy, see MaterialResult::emission_indirect.
float rt_material_emission_indirect(MaterialData p_mat) {
	return unpackHalf2x16(p_mat.emission_indirect_transmission).x;
}

// BaseMaterial3D transmission (or 1 with RT_MAT_FLAG_TRANSMISSION_FROM_ALPHA).
float rt_material_transmission(MaterialData p_mat) {
	return unpackUnorm4x8(p_mat.emission_indirect_transmission).z;
}

// Single-scatter albedo (rgb) and Henyey-Greenstein g (w) of the volume an RT_MAT_FLAG_VOLUME surface bounds.
vec4 rt_material_scatter(MaterialData p_mat) {
	vec4 s = unpackUnorm4x8(p_mat.scatter);
	return vec4(s.rgb, s.a * 2.0 - 1.0);
}

// Index of refraction of the medium an RT_MAT_FLAG_VOLUME surface bounds (1 to 3).
float rt_material_ior(MaterialData p_mat) {
	return 1.0 + 2.0 * unpackUnorm4x8(p_mat.emission_indirect_transmission).w;
}

// Medium code of the volume an RT_MAT_FLAG_VOLUME surface bounds; never 0 (C++ rt_medium_code()).
uint rt_material_medium(MaterialData p_mat) {
	return p_mat.medium;
}

// BaseMaterial3D clearcoat and clearcoat_roughness (x, y); x is 0 without the coat.
// BaseMaterial3D subsurface scattering strength (x, 0: none) and Skin Mode (y, 0 or 1).
vec2 rt_material_subsurface(MaterialData p_mat) {
	return vec2(float(p_mat.subsurface & 0xFFu) / 255.0, float((p_mat.subsurface >> 8u) & 1u));
}

vec2 rt_material_clearcoat(MaterialData p_mat) {
	return unpackUnorm4x8(p_mat.proximity_fade_clearcoat).zw;
}

// BaseMaterial3D roughness, metallic, specular (x, y, z).
// BaseMaterial3D Hair: longitudinal and azimuthal roughness, cuticle tilt in degrees (x, y, z).
vec3 rt_material_hair(MaterialData p_mat) {
	vec3 v = unpackUnorm4x8(p_mat.hair).xyz;
	return vec3(v.xy, v.z * 20.0 - 10.0);
}

uint rt_material_hair_direction(MaterialData p_mat) {
	return (p_mat.hair >> 24u) & 3u;
}

bool rt_material_hair_melanin(MaterialData p_mat) {
	return ((p_mat.hair >> 26u) & 1u) != 0u;
}

bool rt_material_hair_cards(MaterialData p_mat) {
	return ((p_mat.hair >> 27u) & 1u) != 0u;
}

vec3 rt_material_hair_crown(MaterialData p_mat) {
	return vec3(unpackHalf2x16(p_mat.hair_crown_xy), unpackHalf2x16(p_mat.hair_crown_z).x);
}

vec3 rt_material_surface_params(MaterialData p_mat) {
	return unpackUnorm4x8(p_mat.surface_params).xyz;
}

// What BaseMaterial3D's layer textures hold at a hit (white transmission, black back light when absent).
struct MaterialLayerTextures {
	float transmission; // Scales rt_material_transmission().
	vec3 backlight; // Added to rt_material_backlight().
};

MaterialLayerTextures material_layer_textures_none() {
	MaterialLayerTextures l;
	l.transmission = 1.0;
	l.backlight = vec3(0.0);
	return l;
}

// What an opaque thin caster lets a shadow ray keep, given its albedo p_tint: transmission's light
// straight through, less one face's reflection head-on (raster F0 from p_specular), and back
// lighting's light through the surface (albedo times back light, all the light its diffuse
// transmission sends on, taken straight through). 0: blocks the ray.
vec3 rt_shadow_transmittance(vec3 p_tint, float p_transmission, vec3 p_backlight, float p_specular, float p_metallic) {
	vec3 t = p_tint * (clamp(p_transmission, 0.0, 1.0) * (1.0 - 0.16 * p_specular * p_specular) + max(p_backlight, vec3(0.0)));
	return clamp(t * (1.0 - clamp(p_metallic, 0.0, 1.0)), vec3(0.0), vec3(1.0));
}

// rt_shadow_transmittance() of a BaseMaterial3D with alpha p_alpha (Refraction lets 1 - alpha through).
vec3 rt_material_shadow_transmittance(MaterialData p_mat, vec3 p_tint, float p_alpha, MaterialLayerTextures p_layers, bool p_transmission, bool p_back_lighting) {
	vec3 params = rt_material_surface_params(p_mat);
	float transmission = 0.0;
	if (p_transmission && (p_mat.flags & RT_MAT_FLAG_TRANSMISSIVE) != 0u) {
		transmission = rt_material_transmission(p_mat) * p_layers.transmission;
		if ((p_mat.flags & RT_MAT_FLAG_TRANSMISSION_FROM_ALPHA) != 0u) {
			transmission *= 1.0 - clamp(p_alpha, 0.0, 1.0);
		}
	}
	vec3 backlight = vec3(0.0);
	if (p_back_lighting && (p_mat.flags & RT_MAT_FLAG_BACKLIT) != 0u) {
		backlight = rt_material_backlight(p_mat) + p_layers.backlight;
	}
	return rt_shadow_transmittance(p_tint, transmission, backlight, params.z, params.y);
}

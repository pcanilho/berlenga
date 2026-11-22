// Material evaluation result - common output format for all hit groups.
// Standard materials and custom ShaderMaterials both produce this struct.
//
// Requires (before this file): raytracing_data_inc.glsl, bindless_textures[],
// raytracing_samplers_inc.glsl.

struct MaterialResult {
	vec3 albedo;
	float alpha;
	float roughness;
	float metalness;
	float specular; // Dielectric specular reflectance control [0..1], default 0.5 -> F0 = 0.04.
	vec3 emissive;
	float emission_indirect; // EMISSION_INDIRECT: weight of emissive (and unshaded) radiance on indirect paths.
	vec3 normal; // Final shading normal (world space, after normal mapping).
	vec3 backlight; // Light "leaking through" thin/translucent surfaces (BACKLIGHT).
	float clearcoat; // CLEARCOAT, 0 = no coat (or rendering/pathtracing/clearcoat off).
	float clearcoat_roughness; // CLEARCOAT_ROUGHNESS.
	vec3 coat_normal; // The coat's normal, world space; zero: the geometry normal (snow's glints set it).
	vec3 specular_normal; // The base specular's own normal, world space; zero: the shading normal (snow's exact glints).
	float transmission; // BaseMaterial3D transmission times (1 - metallic): light let straight through, tinted by albedo.
	float ior; // With transmission: the IOR of the medium the surface bounds; 0 for a thin surface.
	uint medium; // With ior: the medium's payload code (MEDIUM_CHANGE_FLAG, RT_MEDIUM_CODE_GEOMETRY).
	uint medium_scatter; // With RT_MEDIUM_CODE_SCATTER in medium: rt_medium_scatter_pack().
	float subsurface; // SSS_STRENGTH: subsurface scattering radius over rendering/pathtracing/subsurface_scattering_scale; 0 = none.
	bool subsurface_skin; // Skin Mode (sss_mode_skin): measured skin's per-channel radii.
	float hair; // BaseMaterial3D Hair: the share of the surface that is hair (its mask), 0 = none.
	vec3 hair_sigma_a; // Absorption per fibre diameter.
	vec3 hair_tangent; // World space, along the strands.
	vec4 hair_params; // Longitudinal roughness, azimuthal roughness, cuticle tilt (degrees), fibre offset h.
	bool hair_cards; // Light passes through (cards); otherwise a shell folds it back out.
	bool hair_strand; // Strand geometry (HairMesh): hair_params.w is the true fibre offset at the hit.
};

void material_result_no_hair(inout MaterialResult r) {
	r.coat_normal = vec3(0.0);
	r.specular_normal = vec3(0.0);
	r.hair = 0.0;
	r.hair_sigma_a = vec3(0.0);
	r.hair_tangent = vec3(1.0, 0.0, 0.0);
	r.hair_params = vec4(0.3, 0.3, 2.0, 0.0);
	r.hair_cards = false;
	r.hair_strand = false;
}

// Sensible default for a mid-grey diffuse surface.
MaterialResult default_material_result(vec3 geometry_normal) {
	MaterialResult r;
	r.albedo = vec3(0.8);
	r.alpha = 1.0;
	r.roughness = 0.5;
	r.metalness = 0.0;
	r.specular = 0.5;
	r.emissive = vec3(0.0);
	r.emission_indirect = 1.0;
	r.normal = geometry_normal;
	r.backlight = vec3(0.0);
	r.clearcoat = 0.0;
	r.clearcoat_roughness = 0.0;
	r.transmission = 0.0;
	r.ior = 0.0;
	r.medium = 0u;
	r.medium_scatter = 0u;
	r.subsurface = 0.0;
	r.subsurface_skin = false;
	material_result_no_hair(r);
	return r;
}

// ============================================================================
// TEXTURE SAMPLING
// ============================================================================

// Texture level of detail by ray cones (Akenine-Moller, Nilsson, Andersson, Barre-Brisebois,
// Toth & Karras, "Texture Level of Detail Strategies for Real-Time Ray Tracing", Ray Tracing
// Gems 2019, ch. 20; "Improved Shader and Texture Level of Detail Using Ray Cones", JCGT 2021).
// Ray tracing stages have no screen-space derivatives, so texture() reads the base level and
// distant detail aliases into fuzz under the denoiser. rt_texture_cone() (closest hits) sets
// the hit's cone; material_uv_sample() turns it into the sampled texture's level:
// - rt_tex_grad: the footprint ellipse's axes in UV space, sampled with textureGrad.
// - otherwise rt_tex_lod: log2 of the footprint in texture units, before the texture's own size.
// Elsewhere (any-hit, shadow queries) they stay unset and the base level is read.
const float RT_TEXTURE_LOD_BASE = -1e30;
float rt_tex_lod = RT_TEXTURE_LOD_BASE;
vec2 rt_tex_dx = vec2(0.0);
vec2 rt_tex_dy = vec2(0.0);
bool rt_tex_grad = false;
bool rt_tex_aniso = false; // The material asks for anisotropic filtering (RT_MAT_FLAG_ANISOTROPIC).
// The hit's cone, in the mesh's own UV1 (rt_texture_cone): footprint axes, isotropic level, and
// log2 of the footprint in world units for position-mapped (triplanar) textures.
vec2 rt_cone_uv_dx = vec2(0.0);
vec2 rt_cone_uv_dy = vec2(0.0);
bool rt_cone_uv_grad = false;
float rt_cone_uv_lod = RT_TEXTURE_LOD_BASE;
float rt_cone_footprint_log2 = RT_TEXTURE_LOD_BASE;

vec4 sample_bindless_texture(uint tex_idx, vec2 uv) {
	// The samplers are the render buffers' own (bindings 16-27), so their mipmap bias (the
	// upscaler's render-to-display ratio, TAA's and the project's) applies here as in raster,
	// to the gradients and to the explicit level alike. Anisotropic only when the material
	// asks for it, as in raster.
	if (rt_tex_grad) {
		if (rt_tex_aniso) {
			return textureGrad(sampler2D(bindless_textures[nonuniformEXT(tex_idx)], SAMPLER_LINEAR_WITH_MIPMAPS_ANISOTROPIC_REPEAT), uv, rt_tex_dx, rt_tex_dy);
		}
		return textureGrad(sampler2D(bindless_textures[nonuniformEXT(tex_idx)], SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT), uv, rt_tex_dx, rt_tex_dy);
	}
	ivec2 size = textureSize(sampler2D(bindless_textures[nonuniformEXT(tex_idx)], SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT), 0);
	float lod = max(rt_tex_lod + 0.5 * log2(float(max(size.x, 1) * max(size.y, 1))), 0.0);
	return textureLod(sampler2D(bindless_textures[nonuniformEXT(tex_idx)], SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT), uv, lod);
}

// Sample with point/nearest filtering (for pixel art textures).
vec4 sample_bindless_texture_point(uint tex_idx, vec2 uv) {
	return texture(sampler2D(bindless_textures[nonuniformEXT(tex_idx)], SAMPLER_NEAREST_REPEAT), uv);
}

// Sample a MaterialData *_texture with the appropriate filter based on material flags.
vec4 sample_material_texture(uint p_texture, vec2 uv, uint mat_flags) {
	uint tex_idx = rt_texture_index(p_texture);
	if ((mat_flags & RT_MAT_FLAG_POINT_FILTER) != 0u) {
		return sample_bindless_texture_point(tex_idx, uv);
	}
	return sample_bindless_texture(tex_idx, uv);
}

// ============================================================================
// UV ADDRESSING (plain UV1/UV2 or triplanar)
// ============================================================================

// Resolved texture addressing for one hit. Mirrors the UV setup that
// BaseMaterial3D generates for the rasterizer, so both mapping modes can be
// sampled through a single entry point.
struct MaterialUV {
	vec2 uv; // Mesh UV with scale/offset applied (plain mapping).
	vec3 triplanar_pos; // Scaled/offset position, object or world space.
	vec3 triplanar_weight; // Per-axis blend weights, normalized to sum 1.
	vec3 blend_normal; // Normal the weights came from, in the mapping space.
	bool triplanar;
	bool object_space; // Triplanar mapping runs in model space, not world space.
	// Ray cone (see rt_tex_lod): UV footprint axes when cone_grad, else the isotropic level.
	vec2 cone_dx;
	vec2 cone_dy;
	bool cone_grad;
	float cone_lod;
};

// Addressing parameters of one BaseMaterial3D UV channel.
struct MaterialUVChannel {
	vec3 scale;
	vec3 offset;
	float blend_sharpness;
	bool triplanar;
	bool object_space;
	bool uv2; // The cone's UV gradients are UV1's; UV2 takes the base level.
};

MaterialUVChannel material_uv1_channel(MaterialData mat) {
	MaterialUVChannel ch;
	ch.scale = mat.uv1_scale;
	ch.offset = mat.uv1_offset;
	ch.blend_sharpness = unpackHalf2x16(mat.blend_sharpness).x;
	ch.triplanar = (mat.flags & RT_MAT_FLAG_TRIPLANAR) != 0u;
	ch.object_space = (mat.flags & RT_MAT_FLAG_TRIPLANAR_WORLD) == 0u;
	ch.uv2 = false;
	return ch;
}

MaterialUVChannel material_uv2_channel(MaterialData mat) {
	MaterialUVChannel ch;
	vec2 z = unpackHalf2x16(mat.uv2_scale_offset_z);
	ch.scale = vec3(unpackHalf2x16(mat.uv2_scale_xy), z.x);
	ch.offset = vec3(unpackHalf2x16(mat.uv2_offset_xy), z.y);
	ch.blend_sharpness = unpackHalf2x16(mat.blend_sharpness).y;
	ch.triplanar = (mat.flags & RT_MAT_FLAG_UV2_TRIPLANAR) != 0u;
	ch.object_space = (mat.flags & RT_MAT_FLAG_UV2_TRIPLANAR_WORLD) == 0u;
	ch.uv2 = true;
	return ch;
}

// Build the addressing for a hit. `object_pos`/`object_normal` must be in
// decompressed model space (what the rasterizer sees as VERTEX/NORMAL);
// `world_pos`/`world_normal` are only used by world triplanar mode.
MaterialUV material_uv_compute(MaterialUVChannel ch, vec2 mesh_uv,
		vec3 object_pos, vec3 object_normal, vec3 world_pos, vec3 world_normal) {
	MaterialUV muv;
	muv.uv = mesh_uv * ch.scale.xy + ch.offset.xy;
	muv.triplanar_pos = vec3(0.0);
	muv.triplanar_weight = vec3(0.0);
	muv.blend_normal = world_normal;
	muv.triplanar = ch.triplanar;
	muv.object_space = ch.object_space;
	// The cone's footprint through the channel's UV scale: the mesh UV1's gradients scaled for
	// plain mapping, the world footprint times the scale for triplanar (per model unit in
	// object space). UV2 has no gradients of its own here and reads the base level.
	muv.cone_grad = !ch.uv2 && !ch.triplanar && rt_cone_uv_grad;
	muv.cone_dx = rt_cone_uv_dx * ch.scale.xy;
	muv.cone_dy = rt_cone_uv_dy * ch.scale.xy;
	muv.cone_lod = ch.uv2 ? RT_TEXTURE_LOD_BASE : rt_cone_uv_lod + 0.5 * log2(max(abs(ch.scale.x * ch.scale.y), 1e-12));
	if (ch.triplanar && !ch.uv2) {
		float tex_per_world = (abs(ch.scale.x) + abs(ch.scale.y) + abs(ch.scale.z)) / 3.0;
		muv.cone_lod = rt_cone_footprint_log2 + log2(max(tex_per_world, 1e-6));
	}

	if (muv.triplanar) {
		vec3 pos = muv.object_space ? object_pos : world_pos + rt_origin.xyz; // Absolute world, as raster.
		muv.blend_normal = muv.object_space ? object_normal : world_normal;

		vec3 weight = pow(abs(muv.blend_normal), vec3(ch.blend_sharpness));
		muv.triplanar_weight = weight / max(dot(weight, vec3(1.0)), 0.0001);

		muv.triplanar_pos = (pos * ch.scale + ch.offset) * vec3(1.0, -1.0, 1.0);
	}

	return muv;
}

// Ray-tracing stages only carry world-space hit data, so recover the model
// space that local triplanar mapping needs.
MaterialUV material_uv_channel_from_world(MaterialUVChannel ch, vec2 mesh_uv,
		vec3 world_pos, vec3 world_normal) {
	vec3 object_pos = vec3(0.0);
	vec3 object_normal = vec3(0.0);

	if (ch.triplanar && ch.object_space) {
		mat4 world_to_model = mat4(gl_WorldToObjectEXT);
		object_pos = (world_to_model * vec4(world_pos, 1.0)).xyz;
		object_normal = normalize(mat3(world_to_model) * world_normal);
	}

	return material_uv_compute(ch, mesh_uv, object_pos, object_normal, world_pos, world_normal);
}

MaterialUV material_uv_from_world(MaterialData mat, GeometryData geom, vec2 mesh_uv,
		vec3 world_pos, vec3 world_normal) {
	return material_uv_channel_from_world(material_uv1_channel(mat), mesh_uv, world_pos, world_normal);
}

// Sample a material texture through the resolved addressing.
vec4 material_uv_sample(uint tex_idx, MaterialUV muv, uint mat_flags) {
	rt_tex_grad = muv.cone_grad;
	rt_tex_dx = muv.cone_dx;
	rt_tex_dy = muv.cone_dy;
	rt_tex_lod = muv.cone_lod;
	rt_tex_aniso = (mat_flags & RT_MAT_FLAG_ANISOTROPIC) != 0u;
	if (!muv.triplanar) {
		return sample_material_texture(tex_idx, muv.uv, mat_flags);
	}

	vec3 p = muv.triplanar_pos;
	vec3 w = muv.triplanar_weight;
	vec4 samp = sample_material_texture(tex_idx, p.xy, mat_flags) * w.z;
	samp += sample_material_texture(tex_idx, p.xz, mat_flags) * w.y;
	samp += sample_material_texture(tex_idx, p.zy * vec2(-1.0, 1.0), mat_flags) * w.x;
	return samp;
}

// World-space tangent frame implied by triplanar mapping. Normal maps on a
// triplanar material are authored against these axes rather than the mesh
// tangents, so they replace the fetched frame.
void material_uv_triplanar_tangents(MaterialUV muv, out vec3 tangent, out vec3 binormal) {
	vec3 n = abs(muv.blend_normal);

	tangent = vec3(0.0, 0.0, -1.0) * n.x;
	tangent += vec3(1.0, 0.0, 0.0) * n.y;
	tangent += vec3(1.0, 0.0, 0.0) * n.z;
	tangent = normalize(tangent);

	binormal = vec3(0.0, 1.0, 0.0) * n.x;
	binormal += vec3(0.0, 0.0, -1.0) * n.y;
	binormal += vec3(0.0, 1.0, 0.0) * n.z;
	binormal = normalize(binormal);

	if (muv.object_space) {
		// Match compute_hit_data: normalized columns keep uniformly scaled
		// instances from skewing the frame.
		mat3 model_rotation = mat3(gl_ObjectToWorldEXT);
		mat3 object_to_world = mat3(
				normalize(model_rotation[0]),
				normalize(model_rotation[1]),
				normalize(model_rotation[2]));
		tangent = normalize(object_to_world * tangent);
		binormal = normalize(object_to_world * binormal);
	}
}

// ============================================================================
// ROUGHNESS / METALLIC / AO
// ============================================================================

// dot(texel, mask) with the BaseMaterial3D texture channel mask of p_texture.
float material_texture_channel(vec4 p_texel, uint p_texture) {
	uint channel = rt_texture_channel(p_texture);
	return channel == RT_TEXTURE_CHANNEL_GRAYSCALE ? dot(p_texel.rgb, vec3(0.333333)) : p_texel[channel];
}

// ROUGHNESS and METALLIC (x, y) of BaseMaterial3D from already sampled textures.
vec2 material_roughness_metallic_from_texels(MaterialData mat, vec4 p_roughness_tex, vec4 p_metallic_tex) {
	vec3 params = rt_material_surface_params(mat);
	return clamp(vec2(
						 material_texture_channel(p_roughness_tex, mat.roughness_texture) * params.x,
						 material_texture_channel(p_metallic_tex, mat.metallic_texture) * params.y),
			0.0, 1.0);
}

// ROUGHNESS and METALLIC (x, y) of BaseMaterial3D. ORM materials share one texture fetch.
vec2 material_roughness_metallic(MaterialData mat, MaterialUV muv) {
	vec4 roughness_tex = material_uv_sample(mat.roughness_texture, muv, mat.flags);
	vec4 metallic_tex = rt_texture_index(mat.metallic_texture) == rt_texture_index(mat.roughness_texture)
			? roughness_tex
			: material_uv_sample(mat.metallic_texture, muv, mat.flags);
	return material_roughness_metallic_from_texels(mat, roughness_tex, metallic_tex);
}

// AO of BaseMaterial3D (1.0 when disabled). UV1 only: AO on UV2 is not mirrored, and the
// path tracer only shows AO in debug views since it traces occlusion itself.
float material_ao(MaterialData mat, MaterialUV muv) {
	return material_texture_channel(material_uv_sample(mat.ao_texture, muv, mat.flags), mat.ao_texture);
}

// ============================================================================
// LAYER TEXTURES (clearcoat, back light, transmission)
// ============================================================================

// BaseMaterial3D clearcoat and clearcoat_roughness (x, y), scaled by texture_clearcoat's red and green
// as raster. x is 0 without the coat.
vec2 material_clearcoat(MaterialData mat, MaterialUV muv) {
	vec2 coat = rt_material_clearcoat(mat);
	if (coat.x > 0.0 && rt_texture_index(mat.clearcoat_texture) != 0u) {
		coat *= material_uv_sample(mat.clearcoat_texture, muv, mat.flags).xy;
	}
	return coat;
}

// BaseMaterial3D subsurface scattering strength scaled by texture_subsurface_scattering's red, as raster.
float material_subsurface(MaterialData mat, MaterialUV muv) {
	float strength = rt_material_subsurface(mat).x;
	if (strength > 0.0 && rt_texture_index(mat.subsurface_texture) != 0u) {
		strength *= material_uv_sample(mat.subsurface_texture, muv, mat.flags).r;
	}
	return strength;
}

// texture_transmission's red and texture_backlight at the hit; the back light texture defaults to black.
MaterialLayerTextures material_layer_textures(MaterialData mat, MaterialUV muv) {
	MaterialLayerTextures l = material_layer_textures_none();
	if ((mat.flags & RT_MAT_FLAG_TRANSMISSIVE) != 0u && rt_texture_index(mat.transmission_texture) != 0u) {
		l.transmission = material_uv_sample(mat.transmission_texture, muv, mat.flags).r;
	}
	if ((mat.flags & RT_MAT_FLAG_BACKLIT) != 0u && rt_texture_index(mat.backlight_texture) != 0u) {
		l.backlight = material_uv_sample(mat.backlight_texture, muv, mat.flags).rgb;
	}
	return l;
}

// ============================================================================
// EMISSION
// ============================================================================

// Raw emission output of BaseMaterial3D for one hit (before exposure
// normalization). Mirrors the generated fragment code: the emission texture
// defaults to black, and the operator combines it with the emission color.
vec3 material_emission(MaterialData mat, MaterialUV uv1, vec2 mesh_uv2, vec3 world_pos, vec3 world_normal) {
	vec3 emission_tex = vec3(0.0);
	if ((mat.flags & RT_MAT_FLAG_HAS_EMISSION_TEX) != 0u) {
		MaterialUV muv = uv1;
		if ((mat.flags & RT_MAT_FLAG_EMISSION_UV2) != 0u) {
			muv = material_uv_channel_from_world(material_uv2_channel(mat), mesh_uv2, world_pos, world_normal);
		}
		emission_tex = material_uv_sample(mat.emission_texture, muv, mat.flags).rgb;
	}
	vec3 emission = (mat.flags & RT_MAT_FLAG_EMISSION_ADD) != 0u
			? mat.emission_color + emission_tex
			: mat.emission_color * emission_tex;
	return emission * mat.emission_strength;
}

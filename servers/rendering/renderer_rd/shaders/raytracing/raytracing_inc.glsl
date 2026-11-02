// Shared definitions for raytracing shaders (raygen, miss, closest_hit, any_hit).
// Must be included before scene_data_inc.glsl since it defines MAX_VIEWS.
#include "../oct_inc.glsl"

#include "../oct_inc.glsl"

#define MAX_VIEWS 2

#ifndef PI
#define PI 3.141592653589f
#endif

// ============================================================================
// RT_PARAMS INDICES - Must match RT_PARAM_* in scene_shader_raytracing.h
// ============================================================================
// rt_params is a vec4[5] uniform buffer (20 floats total)
// Access: rt_params[idx >> 2][idx & 3] or get_rt_param(idx)
#define RT_PARAM_VIS_MODE 0 // rt_params[0].x - Debug visualization mode (0 = disabled)
#define RT_PARAM_SAMPLE_COUNT 1 // rt_params[0].y - Samples per pixel
#define RT_PARAM_MAX_BOUNCES 2 // rt_params[0].z - Maximum ray bounces
#define RT_PARAM_DENOISER 3 // rt_params[0].w - Denoiser selection (0=none, 1=DLSS RR)
// 4-11: the volumetric fog the sun lights through the traced scene (sun shafts)
#define RT_PARAM_FOG_DENSITY 4 // extinction per metre
#define RT_PARAM_FOG_ALBEDO_R 5
#define RT_PARAM_FOG_ALBEDO_G 6
#define RT_PARAM_FOG_ALBEDO_B 7
#define RT_PARAM_FOG_ANISOTROPY 8 // Henyey-Greenstein g
#define RT_PARAM_FOG_LENGTH 9 // metres marched, at most
#define RT_PARAM_FOG_SKY_AFFECT 10
#define RT_PARAM_FOG_SAMPLES 11 // points (and shadow rays) a pixel; 0 off
#define RT_PARAM_FOG_VOLUME_COUNT 12 // fog volumes in the fog volume buffer (binding 34)
// Index 13 reserved for future use
#define RT_PARAM_LIGHT_COUNT 14 // rt_params[3].z - Number of active lights in light buffer
#define RT_PARAM_FRAME_INDEX 15 // rt_params[3].w - Frame counter for temporal variation
// 16-18: the light the fog takes from the sky and the room (the environment's ambient
// light times the fog's ambient inject, plus its emission), unshadowed
#define RT_PARAM_FOG_AMBIENT_R 16
#define RT_PARAM_FOG_AMBIENT_G 17
#define RT_PARAM_FOG_AMBIENT_B 18

// ============================================================================
// PATHTRACING PAYLOAD (32 bytes, fp16/unorm-packed)
// ============================================================================
// Radiance (r) and throughput (T) interleaved as fp16 into 3 uints:
//   [0]=rg, [1]=bR, [2]=GB  (packHalf2x16 pairs).
// The next-bounce origin is not stored: raygen reconstructs it from the
// current ray and hit_t as `offset_ray_origin(ray_origin + ray_dir * hit_t,
// offset_normal)`. The offset normal and next direction are stored as
// 16-bit unorm octahedral pairs (effective angular error <~0.001 deg,
// below the 8-bit normal quantization baked into offset_ray_origin).
// Use PathState + path_pack/path_unpack for fp32 working copies.

struct PathPayload {
	uint packed_rt[3]; // 12 bytes - radiance+throughput interleaved as fp16
	uint packed_bounces_flags; //  4 bytes - [flags:8][unused:8][diffuse:8][total:8]
	uint rng_state; //  4 bytes - RNG state for PCG
	float hit_t; //  4 bytes - ray distance, used by raygen to rebuild origin
	uint oct_offset_nrm; //  4 bytes - packUnorm2x16(vec3_to_oct(offset normal))
	uint oct_next_dir; //  4 bytes - packUnorm2x16(vec3_to_oct(next direction))
};

/// Unpacked fp32 working copy of the payload.
struct PathState {
	vec3 radiance;
	vec3 throughput;
	uint packed_bounces_flags;
	uint rng_state;
	float hit_t;
	vec3 offset_normal;
	vec3 next_ray_dir;
};

PathState path_unpack(PathPayload p) {
	PathState s;
	vec2 rg = unpackHalf2x16(p.packed_rt[0]);
	vec2 bR = unpackHalf2x16(p.packed_rt[1]);
	vec2 GB = unpackHalf2x16(p.packed_rt[2]);
	s.radiance = max(vec3(rg.x, rg.y, bR.x), vec3(0.0));
	s.throughput = max(vec3(bR.y, GB.x, GB.y), vec3(0.0));
	s.packed_bounces_flags = p.packed_bounces_flags;
	s.rng_state = p.rng_state;
	s.hit_t = p.hit_t;
	s.offset_normal = oct_to_vec3(unpackUnorm2x16(p.oct_offset_nrm) * 2.0 - 1.0);
	s.next_ray_dir = oct_to_vec3(unpackUnorm2x16(p.oct_next_dir) * 2.0 - 1.0);
	return s;
}

void path_pack(inout PathPayload p, PathState s) {
	const float FP16_MAX = 65504.0;
	vec3 r = clamp(s.radiance, vec3(0.0), vec3(FP16_MAX));
	vec3 t = clamp(s.throughput, vec3(0.0), vec3(FP16_MAX));
	p.packed_rt[0] = packHalf2x16(vec2(r.r, r.g));
	p.packed_rt[1] = packHalf2x16(vec2(r.b, t.r));
	p.packed_rt[2] = packHalf2x16(vec2(t.g, t.b));
	p.packed_bounces_flags = s.packed_bounces_flags;
	p.rng_state = s.rng_state;
	p.hit_t = s.hit_t;
	p.oct_offset_nrm = packUnorm2x16(vec3_to_oct(s.offset_normal));
	p.oct_next_dir = packUnorm2x16(vec3_to_oct(s.next_ray_dir));
}

// Bounce count helpers (bits 0-7: total, bits 8-15: diffuse)
uint get_total_bounces(uint packed) {
	return packed & 0xFFu;
}
uint get_diffuse_bounces(uint packed) {
	return (packed >> 8u) & 0xFFu;
}
uint pack_bounces(uint total, uint diffuse) {
	return total | (diffuse << 8u);
}
uint inc_total_bounce(uint packed) {
	return packed + 1u;
}
uint inc_diffuse_bounce(uint packed) {
	return packed + 0x101u;
} // +1 to both total and diffuse

// The ray cone's width at the ray's origin (bits 16-23), for texture level of detail
// (Akenine-Moller et al., "Texture Level of Detail Strategies for Real-Time Ray
// Tracing", Ray Tracing Gems 2019): log2(width) in eighths, offset by 128; 0 is a
// width of 0 (a perspective camera's eye).
float get_cone_width(uint packed) {
	uint q = (packed >> 16u) & 0xFFu;
	return q == 0u ? 0.0 : exp2((float(q) - 128.0) * 0.125);
}
uint set_cone_width(uint packed, float width) {
	uint q = width <= 0.0 ? 0u : uint(clamp(round(log2(width) * 8.0 + 128.0), 1.0, 255.0));
	return (packed & ~(0xFFu << 16u)) | (q << 16u);
}

// Sample 0 flag (bit 24) - only write DLSS RR outputs on first sample
const uint SAMPLE_ZERO_FLAG = (1u << 24);
uint set_sample_zero(uint packed) {
	return packed | SAMPLE_ZERO_FLAG;
}
bool is_sample_zero(uint packed) {
	return (packed & SAMPLE_ZERO_FLAG) != 0u;
}

// Shadow ray flag (bit 25) - indicates this ray is a shadow/occlusion test
const uint SHADOW_RAY_FLAG = (1u << 25);
uint set_shadow_ray(uint packed) {
	return packed | SHADOW_RAY_FLAG;
}
bool is_shadow_ray(uint packed) {
	return (packed & SHADOW_RAY_FLAG) != 0u;
}

const uint PATH_TERMINATED_FLAG = (1u << 26);
uint set_path_terminated(uint packed) {
	return packed | PATH_TERMINATED_FLAG;
}
uint clear_path_terminated(uint packed) {
	return packed & ~PATH_TERMINATED_FLAG;
}
bool is_path_terminated(uint packed) {
	return (packed & PATH_TERMINATED_FLAG) != 0u;
}

// Bounce limits
#define MAX_DIFFUSE_BOUNCES 2u
#define MAX_DENOISER_SPECULAR_HIT_THRESHOLD 0.25

// ============================================================================
// CONSTANTS
// ============================================================================
const uint OFFSET_NONE = 0xFFFFFFFFu;
const uint FLAG_COMPRESSED = 1u;
const uint FLAG_PROCEDURAL = 2u;
// Set when the BLAS uses a per-frame-deformed vertex buffer.
const uint FLAG_DEFORMED = 4u;
// A shadow caster (rendering/pathtracing/shadow_caster_layers): on a shadow
// ray its any-hit multiplies the ray's transmittance by 1 - alpha.
const uint FLAG_SHADOW_CASTER = 8u;

// Cull masks. Casters (Shadows Only) carry instance mask 0x02, instances whose
// Cast Shadow is Off 0x08, and every other instance 0xFF. Camera rays and the
// denoiser's specular query (RT_MASK_SCENE) never meet a caster; shadow rays
// (RT_MASK_SHADOW) never meet a Cast Shadow Off instance, as no shadow map
// draws one; bounce rays (RT_MASK_BOUNCE) meet all three: a hidden ceiling
// still holds the light in and bounces it back. The ray query shadow path runs
// no any-hit, so it leaves casters out too (RT_MASK_SHADOW_OPAQUE).
#define RT_MASK_SCENE 0x09u
#define RT_MASK_BOUNCE 0xFFu
#define RT_MASK_SHADOW 0xF7u
#define RT_MASK_SHADOW_OPAQUE 0x01u
// A shadow ray's transmittance under this is taken as blocked.
#define RT_SHADOW_OPAQUE_TRANSMITTANCE (1.0 / 256.0)

// ============================================================================
// RANDOM NUMBER GENERATION - PCG (Permuted Congruential Generator)
// ============================================================================

/// PCG random number generator - improved XSH-RR variant with better avalanche
uint pcg_hash(uint seed) {
	uint state = seed * 747796405u + 2891336453u;
	uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
	return (word >> 22u) ^ word;
}

/// Initialize RNG state with improved mixing to eliminate patterns
uint init_rng(uvec2 pixel, uint frame, uint sample_idx) {
	// Use Wang hash for better mixing - eliminates linear patterns
	uint seed = pixel.x + pixel.y * 65536u + frame * 1000000u + sample_idx * 100000000u;
	return pcg_hash(seed);
}

/// Random float in [0, 1)
float rand(inout uint state) {
	state = pcg_hash(state);
	return float(state) / 4294967296.0;
}

/// Random vec2 in [0, 1)
vec2 rand2(inout uint state) {
	return vec2(rand(state), rand(state));
}

// ============================================================================
// RAY ORIGIN OFFSET (prevents self-intersection)
// Wachter-Binder method: offsets in integer float representation so the
// displacement scales naturally with position magnitude.
// ============================================================================
float _offset_component(float p, float n_comp, int of_comp) {
	const float origin = 1.0 / 32.0;
	const float float_scale = 1.0 / 65536.0;
	int shifted = floatBitsToInt(p) + ((p >= 0.0) ? of_comp : -of_comp);
	float p_i = intBitsToFloat(shifted);
	return (abs(p) < origin) ? (p + float_scale * n_comp) : p_i;
}

vec3 offset_ray_origin(vec3 p, vec3 n) {
	const float int_scale = 256.0;
	ivec3 of = ivec3(int_scale * n);
	return vec3(
			_offset_component(p.x, n.x, of.x),
			_offset_component(p.y, n.y, of.y),
			_offset_component(p.z, n.z, of.z));
}

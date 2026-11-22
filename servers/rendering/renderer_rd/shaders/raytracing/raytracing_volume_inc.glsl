// Path traced sparse volumes: simulated gas (smoke, steam, fire) the path tracer scatters,
// absorbs and emits in, on every ray type. The grid is a sparse block atlas in the layout of
// NVIDIA Flow (a 3D density atlas plus a hash table of block locations), published each frame by
// whatever simulates it (RTVolumes in render_raytracing_volumes.h).
//
// Estimators (Hofmann, Hasselgren, Clarberg, Munkberg, "Interactive Path Tracing and Reconstruction
// of Sparse Volumes", NVIDIA, 2024):
// - free flight: delta tracking (Woodcock) against a majorant, emission collected at every real and
//   null collision;
// - transmittance on shadow rays: ratio tracking (Novak, Selle, Jarosz, "Residual Ratio Tracking for
//   Estimating Attenuation in Participating Media", 2014), Russian roulette below 0.1;
// - scattering: Henyey-Greenstein phase function.
// Empty space is skipped a block at a time through the sparse table.
//
// Units: the shader works in meters; the atlas and table are in the simulation's units
// (units_per_meter).

#ifndef RT_VOLUME_INC
#define RT_VOLUME_INC

struct RTVolumeParams {
	uvec4 level[10]; // NvFlowSparseLevelParams of the density level
	vec4 world_min; // xyz: simulation units; w: units per meter
	vec4 world_max; // xyz: simulation units; w: 1 when a volume is present
	vec4 block_size; // xyz: block size (simulation units); w: layer index
	vec4 medium; // x: extinction per meter at density 1; y: albedo; z: HG g; w: density majorant
	vec4 emission; // x: emission scale; y: 1 / temperature range; z: 1 when emissive; w: dense box count (0: the sparse atlas)
	vec4 heat_ramp[8]; // rgb: emitted color along the temperature range
	vec4 smoke_color; // rgb: scattering color
};

layout(set = 0, binding = 47) uniform texture3D rt_volume_density; // r: temperature, a: smoke
layout(set = 0, binding = 48, std430) restrict readonly buffer RTVolumeTable {
	uint rt_volume_table[];
};
layout(set = 0, binding = 49, std430) restrict readonly buffer RTVolumeParamsBuffer {
	RTVolumeParams rt_volume;
};

#define RTV_BLOCK_DIM_BITS (rt_volume.level[1].xyz)
#define RTV_TABLE_DIM_LESS_ONE (rt_volume.level[2].xyz)
#define RTV_TABLE_DIM_BITS_X (rt_volume.level[3].x)
#define RTV_TABLE_DIM_BITS_XY (rt_volume.level[3].y)
#define RTV_LOCATION_OFFSET (rt_volume.level[3].w)
#define RTV_BLOCK_LEVEL_OFFSET_GLOBAL (rt_volume.level[4].z)
#define RTV_DIM_INV (uintBitsToFloat(rt_volume.level[7].xyz))
#define RTV_NO_BLOCK 0xFFFFFFFFu

bool rt_volume_enabled() {
	return rt_volume.world_max.w > 0.5;
}

// The block at a location (NvFlowShader.hlsli's sparse lookup).
uint rt_volume_location_to_block(ivec4 p_loc) {
	uvec3 bucket = uvec3(p_loc.xyz) & RTV_TABLE_DIM_LESS_ONE;
	uint bucket1d = (bucket.z << RTV_TABLE_DIM_BITS_XY) | (bucket.y << RTV_TABLE_DIM_BITS_X) | bucket.x;
	uint range_begin = rt_volume_table[(bucket1d << 1u) + 0u];
	uint range_end = rt_volume_table[(bucket1d << 1u) + 1u];
	for (uint b = range_begin; b < range_end; b++) {
		uint i = (b << 2u) + RTV_LOCATION_OFFSET;
		if (int(rt_volume_table[i + 0u]) == p_loc.x && int(rt_volume_table[i + 1u]) == p_loc.y && int(rt_volume_table[i + 2u]) == p_loc.z && int(rt_volume_table[i + 3u]) == p_loc.w) {
			return b;
		}
	}
	return RTV_NO_BLOCK;
}

// One block lookup remembered: consecutive samples along a ray mostly share a block.
struct RTVolumeAccessor {
	ivec4 location;
	ivec3 offset; // virtual cell -> atlas texel
	bool valid;
};

RTVolumeAccessor rt_volume_accessor() {
	RTVolumeAccessor a;
	a.location = ivec4(0x40000000);
	a.offset = ivec3(0);
	a.valid = false;
	return a;
}

void rt_volume_accessor_update(inout RTVolumeAccessor a, ivec4 p_loc) {
	if (p_loc == a.location) {
		return;
	}
	a.location = p_loc;
	uint b = rt_volume_location_to_block(p_loc);
	a.valid = false;
	if (b != RTV_NO_BLOCK) {
		uint v = rt_volume_table[b + RTV_BLOCK_LEVEL_OFFSET_GLOBAL];
		a.valid = (v >> 31u) != 0u;
		ivec3 base = ivec3(int((v << 1u) | 1u) & 0xFFF, int((v >> 10u) | 1u) & 0x7FF, int((v >> 20u) | 1u) & 0x7FF);
		a.offset = base - (p_loc.xyz << ivec3(RTV_BLOCK_DIM_BITS));
	}
}

// Dense boxes (RTDenseVolumes: waterfall mist), instead of the sparse atlas when emission.w holds a
// box count: box i is level[2i] (min corner in meters, meters per texel) and level[2i + 1] (texels,
// first atlas slice); the boxes are stacked along the atlas's z with a zero texel between them, the
// density in r (no temperature: they never emit).
bool rt_volume_dense_fetch(vec3 p_pos, out vec2 r_temp_smoke) {
	r_temp_smoke = vec2(0.0);
	int count = int(rt_volume.emission.w + 0.5);
	for (int i = 0; i < count; i++) {
		vec4 o = uintBitsToFloat(rt_volume.level[2 * i]);
		uvec4 d = rt_volume.level[2 * i + 1];
		vec3 g = (p_pos - o.xyz) / o.w;
		if (any(lessThan(g, vec3(0.0))) || any(greaterThanEqual(g, vec3(d.xyz)))) {
			continue;
		}
		vec3 size = vec3(textureSize(sampler3D(rt_volume_density, SAMPLER_LINEAR_CLAMP), 0));
		vec3 uvw = (clamp(g, vec3(0.5), vec3(d.xyz) - 0.5) + vec3(0.0, 0.0, float(d.w))) / size;
		r_temp_smoke = vec2(0.0, textureLod(sampler3D(rt_volume_density, SAMPLER_LINEAR_CLAMP), uvw, 0.0).r);
		return true;
	}
	return false;
}

// Temperature (x) and smoke (y) at a point in simulation units; false where no block is allocated.
bool rt_volume_fetch(vec3 p_pos_sim, inout RTVolumeAccessor a, out vec2 r_temp_smoke) {
	if (rt_volume.emission.w > 0.5) {
		return rt_volume_dense_fetch(p_pos_sim, r_temp_smoke);
	}
	vec3 cell = rt_volume.block_size.xyz / vec3(rt_volume.level[0].xyz + 1u);
	vec3 vidxf = p_pos_sim / cell;
	ivec4 loc = ivec4(ivec3(floor(vidxf)) >> ivec3(RTV_BLOCK_DIM_BITS), int(rt_volume.block_size.w));
	rt_volume_accessor_update(a, loc);
	if (!a.valid) {
		r_temp_smoke = vec2(0.0);
		return false;
	}
	vec4 d = textureLod(sampler3D(rt_volume_density, SAMPLER_LINEAR_CLAMP), (vidxf + vec3(a.offset)) * RTV_DIM_INV, 0.0);
	r_temp_smoke = vec2(d.r, d.a);
	return true;
}

// The distance to the far side of the current block along the ray (meters), for skipping empty blocks.
float rt_volume_block_exit(vec3 p_pos_sim, vec3 p_dir, float p_upm) {
	vec3 bs = rt_volume.block_size.xyz;
	vec3 lo = floor(p_pos_sim / bs) * bs;
	vec3 hi = lo + bs;
	vec3 inv = 1.0 / p_dir;
	vec3 t = max((lo - p_pos_sim) * inv, (hi - p_pos_sim) * inv);
	return max(min(min(t.x, t.y), t.z), 0.0) / p_upm + 1e-4;
}

// The ray's span inside the volume's bounds (meters); false when it misses them.
bool rt_volume_span(vec3 p_origin, vec3 p_dir, float p_t_max, out float r_t0, out float r_t1) {
	float upm = rt_volume.world_min.w;
	vec3 o = (p_origin + rt_origin.xyz) * upm; // the trace is relative to rt_origin; Flow works in absolute world
	vec3 inv = 1.0 / p_dir;
	vec3 a = (rt_volume.world_min.xyz - o) * inv;
	vec3 b = (rt_volume.world_max.xyz - o) * inv;
	vec3 lo = min(a, b);
	vec3 hi = max(a, b);
	r_t0 = max(max(max(lo.x, lo.y), lo.z) / upm, 0.0);
	r_t1 = min(min(min(hi.x, hi.y), hi.z) / upm, p_t_max);
	return r_t1 > r_t0;
}

float rt_volume_sigma_t(float p_smoke) {
	return rt_volume.medium.x * clamp(p_smoke, 0.0, rt_volume.medium.w);
}

// The emission coefficient (radiance per meter) at a temperature.
vec3 rt_volume_emitted(float p_temperature) {
	if (rt_volume.emission.z < 0.5 || p_temperature <= 0.0) {
		return vec3(0.0);
	}
	float x = clamp(p_temperature * rt_volume.emission.y, 0.0, 1.0) * 7.0;
	int i = min(int(x), 6);
	vec3 c = mix(rt_volume.heat_ramp[i].rgb, rt_volume.heat_ramp[i + 1].rgb, x - float(i));
	return c * rt_volume.emission.x;
}

// Delta tracking from p_origin along p_dir up to p_t_max (meters). Returns the distance of the
// first real collision, or a negative value when the ray leaves the volume (or reaches p_t_max)
// without one. Emission met on the way is added to r_emitted (already weighted; multiply by the
// path throughput).
float rt_volume_free_flight(vec3 p_origin, vec3 p_dir, float p_t_max, inout uint rng_state, out vec3 r_emitted) {
	r_emitted = vec3(0.0);
	float t0;
	float t1;
	if (!rt_volume_enabled() || !rt_volume_span(p_origin, p_dir, p_t_max, t0, t1)) {
		return -1.0;
	}
	float upm = rt_volume.world_min.w;
	float majorant = rt_volume.medium.x * rt_volume.medium.w;
	if (majorant <= 0.0) {
		return -1.0;
	}
	RTVolumeAccessor acc = rt_volume_accessor();
	float t = t0;
	for (uint i = 0u; i < 512u; i++) {
		t -= log(max(1.0 - rand(rng_state), 1e-7)) / majorant;
		if (t >= t1) {
			return -1.0;
		}
		vec3 p = (p_origin + rt_origin.xyz + p_dir * t) * upm;
		vec2 ts;
		if (!rt_volume_fetch(p, acc, ts)) {
			// Unallocated block: no gas, jump to its far side.
			t += rt_volume_block_exit(p, p_dir, upm);
			continue;
		}
		float sigma_t = rt_volume_sigma_t(ts.y);
		// Emission at every collision, real or null: the emission coefficient / majorant
		// estimates its integral along the ray up to the collision. Fire emits by temperature,
		// whatever smoke it carries.
		r_emitted += rt_volume_emitted(ts.x) / majorant;
		if (rand(rng_state) * majorant < sigma_t) {
			return t;
		}
	}
	return -1.0;
}

// Ratio tracking transmittance from p_origin along p_dir over p_t_max meters.
float rt_volume_transmittance(vec3 p_origin, vec3 p_dir, float p_t_max, inout uint rng_state) {
	float t0;
	float t1;
	if (!rt_volume_enabled() || !rt_volume_span(p_origin, p_dir, p_t_max, t0, t1)) {
		return 1.0;
	}
	float upm = rt_volume.world_min.w;
	float majorant = rt_volume.medium.x * rt_volume.medium.w;
	if (majorant <= 0.0) {
		return 1.0;
	}
	RTVolumeAccessor acc = rt_volume_accessor();
	float tr = 1.0;
	float t = t0;
	for (uint i = 0u; i < 256u; i++) {
		t -= log(max(1.0 - rand(rng_state), 1e-7)) / majorant;
		if (t >= t1) {
			break;
		}
		vec3 p = (p_origin + rt_origin.xyz + p_dir * t) * upm;
		vec2 ts;
		if (!rt_volume_fetch(p, acc, ts)) {
			t += rt_volume_block_exit(p, p_dir, upm);
			continue;
		}
		tr *= 1.0 - rt_volume_sigma_t(ts.y) / majorant;
		if (tr < 0.1) {
			// Russian roulette: keep the estimate unbiased while cutting long dark tails.
			if (rand(rng_state) > tr) {
				return 0.0;
			}
			tr = 1.0;
		}
	}
	return tr;
}

float rt_volume_hg(float p_cos, float p_g) {
	float g2 = p_g * p_g;
	float denom = 1.0 + g2 - 2.0 * p_g * p_cos;
	return (1.0 - g2) / (4.0 * PI * denom * sqrt(max(denom, 1e-6)));
}

// Samples a Henyey-Greenstein direction around p_forward (the direction the light travels on).
vec3 rt_volume_sample_hg(vec3 p_forward, float p_g, vec2 p_u) {
	float cos_t;
	if (abs(p_g) < 1e-3) {
		cos_t = 1.0 - 2.0 * p_u.x;
	} else {
		float s = (1.0 - p_g * p_g) / (1.0 - p_g + 2.0 * p_g * p_u.x);
		cos_t = (1.0 + p_g * p_g - s * s) / (2.0 * p_g);
	}
	float sin_t = sqrt(max(0.0, 1.0 - cos_t * cos_t));
	float phi = 2.0 * PI * p_u.y;
	vec3 t = abs(p_forward.z) < 0.999 ? normalize(cross(vec3(0.0, 0.0, 1.0), p_forward)) : vec3(1.0, 0.0, 0.0);
	vec3 b = cross(p_forward, t);
	return normalize(t * (sin_t * cos(phi)) + b * (sin_t * sin(phi)) + p_forward * cos_t);
}

#endif // RT_VOLUME_INC

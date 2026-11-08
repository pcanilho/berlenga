// Radiance cache (binding 45): the light rough surfaces send back, learned in world space
// around the camera, so a path can stop at its second surface and read it there instead of
// sampling lights and tracing on. As NVIDIA's SHaRC.
//
// Cells are RT_RC_CELL wide near the camera and double every octave of distance past
// RT_RC_NEAR, split by the facing axis and side. One pixel in RT_RC_TRAIN_EVERY traces its
// paths in full and records what the surface its first bounce reached sent back, own light
// included (raygen). That depth keeps the cache's bounce count equal to the paths it ends.
// A cell keeps two accumulators: the frame's writers add to one while raygen folds last
// frame's into the cell's running mean (rt_rc_resolve), so no separate pass is needed. A
// bounce that reaches a rough surface, after a rough scatter and far enough for the cell to
// be small beside the ray, reads the mean once the cell has RT_RC_MIN_SAMPLES
// (shade_and_bounce). Off under environment fog, which lies on bounce segments and would be
// learned with the surface.
//
// Entry, 16 uints: key, frame last written, two accumulators of fixed-point RGB plus a
// count, then the mean RGB and its sample count as floats.

#define RT_RC_BINDING 45
layout(set = 0, binding = RT_RC_BINDING, std430) coherent buffer RadianceCacheBuffer {
	uint data[];
}
rt_rc;

#define RT_RC_ENTRIES 262144u // must match RT_RADIANCE_CACHE_ENTRIES (render_raytracing.h)
#define RT_RC_ENTRY_UINTS 16u
#define RT_RC_PROBES 8u
#define RT_RC_CELL 0.25 // cell size near the camera, metres
#define RT_RC_NEAR 8.0 // cells double in size every octave of distance past this
#define RT_RC_SCALE 1024.0 // fixed-point steps per unit of radiance
#define RT_RC_CLAMP 64.0 // largest radiance one sample may add
#define RT_RC_HISTORY 64.0 // samples the mean remembers
#define RT_RC_MIN_SAMPLES 4.0 // samples before a cell is read
#define RT_RC_STALE 32u // frames unwritten before a cell may be taken by another key
#define RT_RC_TRAIN_EVERY 8u // one pixel in this many trains the cache
#define RT_RC_MIN_ROUGHNESS 0.4 // shinier surfaces trace on: the cache holds no view direction

#define RT_RC_ELIGIBLE_FLAG (1u << 16) // hit output: raygen records this surface for training

bool rt_rc_enabled() {
	// Environment fog lies on bounce segments, and the cache would learn it with the surface.
	return get_rt_param(RT_PARAM_RADIANCE_CACHE) > 0.5 && (RT_FLAGS & RT_FLAG_FOG_ENABLED) == 0u &&
			uint(rt_rc.data.length()) >= RT_RC_ENTRIES * RT_RC_ENTRY_UINTS;
}

uint rt_rc_frame() {
	return uint(get_rt_param(RT_PARAM_FRAME_INDEX));
}

bool rt_rc_is_training_pixel(uvec2 p_pixel) {
	uint h = (p_pixel.x * 0x8da6b343u) ^ (p_pixel.y * 0xd8163841u) ^ (rt_rc_frame() * 0xcb1ab31fu);
	h ^= h >> 15u;
	h *= 0x2c1b3c6du;
	h ^= h >> 12u;
	return (h % RT_RC_TRAIN_EVERY) == 0u;
}

uint rt_rc_hash(uint x) {
	x ^= x >> 16u;
	x *= 0x7feb352du;
	x ^= x >> 15u;
	x *= 0x846ca68bu;
	x ^= x >> 16u;
	return x;
}

float rt_rc_cell_size(vec3 p) {
	vec3 cam = vec3(scene_data_block.data.inv_view_matrix[0].w, scene_data_block.data.inv_view_matrix[1].w, scene_data_block.data.inv_view_matrix[2].w);
	float level = clamp(floor(log2(max(distance(p, cam), RT_RC_NEAR) / RT_RC_NEAR)), 0.0, 15.0);
	return RT_RC_CELL * exp2(level);
}

uint rt_rc_key(vec3 p, vec3 N) {
	float cell = rt_rc_cell_size(p);
	ivec3 c = rt_world_cell(p, cell);
	vec3 a = abs(N);
	uint axis = a.x > a.y ? (a.x > a.z ? 0u : 2u) : (a.y > a.z ? 1u : 2u);
	uint side = N[axis] < 0.0 ? 1u : 0u;
	uint h = rt_rc_hash(uint(c.x) + 0x632be5abu);
	h = rt_rc_hash(h ^ uint(c.y));
	h = rt_rc_hash(h ^ uint(c.z));
	return max(rt_rc_hash(h ^ (floatBitsToUint(cell) | (axis << 1u) | side)), 1u);
}

uint rt_rc_home(uint p_key) {
	return rt_rc_hash(p_key ^ 0x27d4eb2fu) % RT_RC_ENTRIES;
}

// The cell's mean radiance, w = 0 when it has too few samples.
vec4 rt_rc_read(vec3 p, vec3 N) {
	uint key = rt_rc_key(p, N);
	uint s = rt_rc_home(key);
	for (uint i = 0u; i < RT_RC_PROBES; i++) {
		uint base = s * RT_RC_ENTRY_UINTS;
		uint k = rt_rc.data[base];
		if (k == key) {
			float n = uintBitsToFloat(rt_rc.data[base + 15u]);
			if (n < RT_RC_MIN_SAMPLES) {
				return vec4(0.0);
			}
			return vec4(uintBitsToFloat(rt_rc.data[base + 12u]), uintBitsToFloat(rt_rc.data[base + 13u]), uintBitsToFloat(rt_rc.data[base + 14u]), 1.0);
		}
		if (k == 0u) {
			break;
		}
		s = (s + 1u) % RT_RC_ENTRIES;
	}
	return vec4(0.0);
}

// Adds one sample of the radiance leaving p.
void rt_rc_write(vec3 p, vec3 N, vec3 p_radiance, inout uint rng_state) {
	if (any(isnan(p_radiance)) || any(isinf(p_radiance))) {
		return;
	}
	uint frame = rt_rc_frame();
	uint key = rt_rc_key(p, N);
	uint s = rt_rc_home(key);
	for (uint i = 0u; i < RT_RC_PROBES; i++) {
		uint base = s * RT_RC_ENTRY_UINTS;
		uint k = rt_rc.data[base];
		bool ours = k == key;
		if (!ours && k == 0u) {
			uint prev = atomicCompSwap(rt_rc.data[base], 0u, key);
			ours = prev == 0u || prev == key;
		} else if (!ours && frame - rt_rc.data[base + 1u] > RT_RC_STALE) {
			// Unwritten for a while: hand the slot to this cell, from an empty mean.
			if (atomicCompSwap(rt_rc.data[base], k, key) == k) {
				rt_rc.data[base + 15u] = floatBitsToUint(0.0);
				ours = true;
			}
		}
		if (ours) {
			rt_rc.data[base + 1u] = frame;
			uint acc = base + 2u + 4u * (frame & 1u);
			vec3 v = min(max(p_radiance, vec3(0.0)), vec3(RT_RC_CLAMP)) * RT_RC_SCALE;
			// Dithered rounding keeps dim light from rounding away.
			uvec3 q = uvec3(floor(v + vec3(rand(rng_state), rand(rng_state), rand(rng_state))));
			atomicAdd(rt_rc.data[acc], q.r);
			atomicAdd(rt_rc.data[acc + 1u], q.g);
			atomicAdd(rt_rc.data[acc + 2u], q.b);
			atomicAdd(rt_rc.data[acc + 3u], 1u);
			return;
		}
		s = (s + 1u) % RT_RC_ENTRIES;
	}
}

// Folds the accumulator last frame wrote into the means of the slots this thread owns.
void rt_rc_resolve(uint p_thread, uint p_threads) {
	uint frame = rt_rc_frame();
	uint parity = (frame + 1u) & 1u;
	for (uint s = p_thread; s < RT_RC_ENTRIES; s += p_threads) {
		uint base = s * RT_RC_ENTRY_UINTS;
		uint acc = base + 2u + 4u * parity;
		uint n = rt_rc.data[acc + 3u];
		if (n == 0u) {
			continue;
		}
		vec3 sum = vec3(uvec3(rt_rc.data[acc], rt_rc.data[acc + 1u], rt_rc.data[acc + 2u])) / RT_RC_SCALE;
		rt_rc.data[acc] = 0u;
		rt_rc.data[acc + 1u] = 0u;
		rt_rc.data[acc + 2u] = 0u;
		rt_rc.data[acc + 3u] = 0u;
		float old_n = min(uintBitsToFloat(rt_rc.data[base + 15u]), RT_RC_HISTORY);
		vec3 mean = vec3(uintBitsToFloat(rt_rc.data[base + 12u]), uintBitsToFloat(rt_rc.data[base + 13u]), uintBitsToFloat(rt_rc.data[base + 14u]));
		float total = old_n + float(n);
		mean = (mean * old_n + sum) / total;
		rt_rc.data[base + 12u] = floatBitsToUint(mean.r);
		rt_rc.data[base + 13u] = floatBitsToUint(mean.g);
		rt_rc.data[base + 14u] = floatBitsToUint(mean.b);
		rt_rc.data[base + 15u] = floatBitsToUint(total);
	}
}

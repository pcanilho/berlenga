// Light sampling and Next Event Estimation (NEE) for raytracing.
// Requires: raytracing_inc.glsl, brdf_inc.glsl, tlas at binding 1, payload at location 0, bindless_textures[].
// Note: raytracing_shadow_query_inc.glsl requires GL_EXT_ray_query.

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
// Light Data (matches C++ RT_LightData, 64 bytes, std430)
// ============================================================================

#define RT_LIGHT_NO_PROJECTOR 0u // Bindless default white texture.

// Bitfield words of C++ RT_LightData; see it for the layout. Positional-only fields are zero for directional lights.
struct RTLightPacked {
	vec3 position; // World pos (omni/spot) or normalized direction (directional).
	float range; // Omni/spot range, 0 = infinite.
	uint emission_rg; // fp16 r, g.
	uint emission_b_radius; // fp16 b, radius.
	uint energy; // fp16 specular, indirect.
	uint masks; // cull_mask : 20, type : 2, unused : 2, shadow_ray_mask : 8.
	uint attenuation_shadow; // fp16 attenuation, shadow opacity.
	uint spot; // fp16 1 - cos(spot angle), 1 / spot attenuation.
	uint orientation_xy; // snorm16 quaternion x, y.
	uint orientation_zw; // snorm16 quaternion z, w.
	uint projector_texture; // index : 24, unused : 8.
	uint reserved0; // Directional projector: float corner u, v (relative to rt_origin), fp16 size u, v.
	uint reserved1;
	uint reserved2;
};

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
	float shadow_opacity; // 0 skips the shadow ray; otherwise blends traced transmittance towards 1.
	float one_minus_cos_spot; // 1 - cosine of the spot cone half-angle.
	float inv_spot_attenuation; // Spot cone softness.
	uint light_masks; // Bits 0-19: light_cull_mask (lighting test). Bits 24-31: TLAS shadow-ray mask.
	uint projector_texture; // Bindless sRGB index, RT_LIGHT_NO_PROJECTOR when unset.
	vec4 orientation; // Unit rotation quaternion; the spot cone points along local -Z. Directional: only with a projector.
	vec2 projector_corner; // Directional projector rect in the light's plane (along +X and -Y), metres.
	vec2 projector_size;
};

// Light buffer SSBO (binding provided by the including shader via RT_LIGHT_BUFFER_BINDING).
#ifndef RT_LIGHT_BUFFER_BINDING
#define RT_LIGHT_BUFFER_BINDING 13
#endif

layout(set = 0, binding = RT_LIGHT_BUFFER_BINDING, std430) readonly buffer LightBuffer {
	RTLightPacked rt_light_buffer[];
};

layout(set = 0, binding = 40) uniform sampler rt_light_projector_sampler;

// The grid of positional light lists around the camera (RenderRaytracing::build_light_grid):
// which lights can reach a point. In uints: [0..2] cells along x, y, z (0: none), [3] the
// global count G, [4..6] the origin and [7] 1 / cell size (floats' bits), [8] the far
// count F; from RT_LIGHT_GRID_HEADER the G lights in every list (no range), the F lights
// reaching past the grid (the list of points outside it), each cell's start, the cells' lists.
#define RT_LIGHT_GRID_HEADER 12u
#ifndef RT_LIGHT_GRID_BINDING
#define RT_LIGHT_GRID_BINDING 42
#endif

layout(set = 0, binding = RT_LIGHT_GRID_BINDING, std430) readonly buffer LightGridBuffer {
	uint rt_light_grid[];
};

// The lights that can reach p: the global ones at [r_global_start, + r_global_count)
// and the cell's at [r_cell_start, + r_cell_count), indices into rt_light_buffer.
void lights_grid_lists(vec3 p, out uint r_global_start, out uint r_global_count, out uint r_cell_start, out uint r_cell_count) {
	r_global_start = RT_LIGHT_GRID_HEADER;
	r_global_count = rt_light_grid[3];
	// Outside the grid: the far list, the lights whose reach leaves the grid.
	r_cell_start = RT_LIGHT_GRID_HEADER + r_global_count;
	r_cell_count = rt_light_grid[8];
	uvec3 dims = uvec3(rt_light_grid[0], rt_light_grid[1], rt_light_grid[2]);
	if (dims.x == 0u) {
		r_cell_count = 0u;
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
	uint starts = RT_LIGHT_GRID_HEADER + r_global_count + rt_light_grid[8];
	r_cell_start = rt_light_grid[starts + cell];
	r_cell_count = rt_light_grid[starts + cell + 1u] - r_cell_start;
}

// The i-th light of the union of a point's two lists.
uint lights_grid_light(uint i, uint global_start, uint global_count, uint cell_start) {
	return i < global_count ? rt_light_grid[global_start + i] : rt_light_grid[cell_start + (i - global_count)];
}

// ============================================================================
// LIGHT VISIBILITY MEMORY (binding 44)
// ============================================================================
// For each small region of the scene and each light: how many of the last samples of
// it lit the region (shadow ray through, and light on the surface's lit side).
// Selection weights candidates by it, so lamps behind walls, lamps inside a building
// seen from its own walls (no self-shadow ray hit), or a sun under a roof stop eating
// samples where they never land. This is
// the visible-light memory of MegaLights (Narkowicz & Costa, "MegaLights: Stochastic
// Direct Lighting", SIGGRAPH Advances 2025), kept in world space so bounces use it
// too, in a spatial hash like SHaRC's (NVIDIA RTXGI). A light never seen keeps a
// floor share, and resampling divides by the weight it picked with, so the result
// stays unbiased: only where the samples go changes.
//
// Two tables in one buffer:
// - pairs, per (region, light): [2i] key (0 = empty), [2i + 1] tries (bits 0-11), hits
//   (12-23), frame (24-31). Counts fade a little each frame, so the memory follows
//   moving lights and doors. Selection weights candidates by it.
// - lists, per region: [9i] key, then 8 entries of light index + 1 (bits 0-15) and the
//   frame it was last seen lit (24-31). Lights that shadow rays reached from the region
//   lately: half the candidates are drawn from it, so in a place where a few of many
//   lights in range are visible (a street among lit buildings), selection finds them
//   rather than hoping a uniform draw does. As MegaLights' visible-light list.

#define RT_LIGHT_VIS_BINDING 44
layout(set = 0, binding = RT_LIGHT_VIS_BINDING, std430) coherent buffer LightVisibilityBuffer {
	uint data[];
}
rt_light_vis;

#define RT_LIGHT_VIS_PAIRS 262144u // must match RT_LIGHT_VISIBILITY_PAIRS (render_raytracing.h)
#define RT_LIGHT_VIS_LISTS 65536u // must match RT_LIGHT_VISIBILITY_LISTS
#define RT_LIGHT_VIS_LIST_SIZE 8u
#define RT_LIGHT_VIS_LIST_BASE (2u * RT_LIGHT_VIS_PAIRS)
#define RT_LIGHT_VIS_PROBES 4u
#define RT_LIGHT_VIS_DIRECTIONAL 0xFFFFu // the directional group's entry
#define RT_LIGHT_VIS_FLOOR 0.02 // share left to a light that looked hidden
#define RT_LIGHT_VIS_STALE 64u // frames before a slot may be taken by another key
#define RT_LIGHT_VIS_CELL 1.0 // region size near the camera, metres
#define RT_LIGHT_VIS_NEAR 16.0 // regions double in size every octave of distance past this
#define RT_LIGHT_VIS_FADE 3u // counts lose 1 / 2^FADE a frame
#define RT_LIGHT_VIS_RECORD_EVERY 4u // at most one pixel in this many records its shadow ray outcomes
#define RT_LIGHT_VIS_RECORD_PIXELS 65536u // pixels that record each frame, whatever the resolution

uint lights_vis_hash(uint x) {
	x ^= x >> 16u;
	x *= 0x7feb352du;
	x ^= x >> 15u;
	x *= 0x846ca68bu;
	x ^= x >> 16u;
	return x;
}

bool lights_vis_enabled() {
	return get_rt_param(RT_PARAM_LIGHT_VISIBILITY) > 0.5 && uint(rt_light_vis.data.length()) >= RT_LIGHT_VIS_LIST_BASE + 9u * RT_LIGHT_VIS_LISTS;
}

// The region a shading point falls in: RT_LIGHT_VIS_CELL cells near the camera,
// doubling every octave of distance past RT_LIGHT_VIS_NEAR (as SHaRC's levels), split
// by the facing axis so
// a wall's two sides and the floor beside it learn apart. 0 when the memory is off.
uint lights_vis_region(vec3 p, vec3 N) {
	if (!lights_vis_enabled()) {
		return 0u;
	}
	vec3 cam = vec3(scene_data_block.data.inv_view_matrix[0].w, scene_data_block.data.inv_view_matrix[1].w, scene_data_block.data.inv_view_matrix[2].w);
	float level = clamp(floor(log2(max(distance(p, cam), RT_LIGHT_VIS_NEAR) / RT_LIGHT_VIS_NEAR)), 0.0, 15.0);
	ivec3 c = rt_world_cell(p, RT_LIGHT_VIS_CELL * exp2(level));
	vec3 a = abs(N);
	uint axis = a.x > a.y ? (a.x > a.z ? 0u : 2u) : (a.y > a.z ? 1u : 2u);
	uint side = N[axis] < 0.0 ? 1u : 0u;
	uint h = lights_vis_hash(uint(c.x) + 0x9e3779b9u);
	h = lights_vis_hash(h ^ uint(c.y));
	h = lights_vis_hash(h ^ uint(c.z));
	return max(lights_vis_hash(h ^ (uint(level) | (axis << 4u) | (side << 6u))), 1u);
}

uint lights_vis_key(uint region, uint light) {
	return max(lights_vis_hash(region ^ (light * 0x9e3779b9u)), 1u);
}

uint lights_vis_slots() {
	return RT_LIGHT_VIS_PAIRS;
}

// Counts faded to this frame.
uvec2 lights_vis_counts(uint value, uint frame) {
	uint age = min((frame - (value >> 24u)) & 0xFFu, 16u);
	uint tries = value & 0xFFFu;
	uint hits = (value >> 12u) & 0xFFFu;
	for (uint i = 0u; i < age; i++) {
		tries -= tries >> RT_LIGHT_VIS_FADE;
		hits -= hits >> RT_LIGHT_VIS_FADE;
	}
	return uvec2(hits, tries);
}

// Fraction of recent shadow rays that got through, 0.5 when unknown.
float lights_vis_read(uint region, uint light) {
	if (region == 0u) {
		return 0.5;
	}
	uint key = lights_vis_key(region, light);
	uint slots = lights_vis_slots();
	uint s = lights_vis_hash(key ^ 0x85ebca6bu) % slots;
	for (uint i = 0u; i < RT_LIGHT_VIS_PROBES; i++) {
		uint k = rt_light_vis.data[2u * s];
		if (k == key) {
			uvec2 c = lights_vis_counts(rt_light_vis.data[2u * s + 1u], uint(get_rt_param(RT_PARAM_FRAME_INDEX)) & 0xFFu);
			return (float(c.x) + 0.5) / (float(c.y) + 1.0);
		}
		if (k == 0u) {
			break;
		}
		s = (s + 1u) % slots;
	}
	return 0.5;
}

// Selection weight from the memory: 1 where seen, the floor where hidden.
float lights_vis_weight(uint region, uint light) {
	return region == 0u ? 1.0 : max(lights_vis_read(region, light), RT_LIGHT_VIS_FLOOR);
}

void lights_vis_list_add(uint region, uint light);

// One sample's outcome for a light, from this region: p_lit when it added light.
void lights_vis_record(uint region, uint light, bool p_lit) {
	if (region == 0u) {
		return;
	}
	// About RT_LIGHT_VIS_RECORD_PIXELS pixels record each frame, a different set every frame
	// (at least one in RT_LIGHT_VIS_RECORD_EVERY): a hit rate is a ratio, so a subset learns it
	// as well, and the compare-and-swap loops on a hot region stop growing with resolution.
	uint every = max(RT_LIGHT_VIS_RECORD_EVERY, (gl_LaunchSizeEXT.x * gl_LaunchSizeEXT.y) / RT_LIGHT_VIS_RECORD_PIXELS);
	uint pick = (gl_LaunchIDEXT.x * 0x8da6b343u) ^ (gl_LaunchIDEXT.y * 0xd8163841u) ^ (uint(get_rt_param(RT_PARAM_FRAME_INDEX)) * 0x9e3779b9u);
	if (lights_vis_hash(pick) % every != 0u) {
		return;
	}
	if (p_lit && light != RT_LIGHT_VIS_DIRECTIONAL) {
		lights_vis_list_add(region, light);
	}
	uint frame = uint(get_rt_param(RT_PARAM_FRAME_INDEX)) & 0xFFu;
	uint key = lights_vis_key(region, light);
	uint slots = lights_vis_slots();
	uint s = lights_vis_hash(key ^ 0x85ebca6bu) % slots;
	for (uint i = 0u; i < RT_LIGHT_VIS_PROBES; i++) {
		uint k = rt_light_vis.data[2u * s];
		bool mine = k == key;
		if (!mine && k == 0u) {
			uint prev = atomicCompSwap(rt_light_vis.data[2u * s], 0u, key);
			mine = prev == 0u || prev == key;
			k = prev;
			if (prev == 0u) {
				atomicExchange(rt_light_vis.data[2u * s + 1u], frame << 24u);
			}
		}
		if (!mine && k != key) {
			// A slot nobody has fed for a while goes to the newcomer.
			uint old_value = rt_light_vis.data[2u * s + 1u];
			if (((frame - (old_value >> 24u)) & 0xFFu) >= RT_LIGHT_VIS_STALE &&
					atomicCompSwap(rt_light_vis.data[2u * s], k, key) == k) {
				atomicExchange(rt_light_vis.data[2u * s + 1u], frame << 24u);
				mine = true;
			}
		}
		if (mine) {
			for (uint attempt = 0u; attempt < 8u; attempt++) {
				uint old_value = rt_light_vis.data[2u * s + 1u];
				uvec2 c = lights_vis_counts(old_value, frame);
				uint hits = c.x + (p_lit ? 1u : 0u);
				uint tries = c.y + 1u;
				if (tries >= 0xFFFu) {
					hits >>= 1u;
					tries >>= 1u;
				}
				uint new_value = tries | (hits << 12u) | (frame << 24u);
				if (atomicCompSwap(rt_light_vis.data[2u * s + 1u], old_value, new_value) == old_value) {
					break;
				}
			}
			return;
		}
		s = (s + 1u) % slots;
	}
}

// The region's list slot (its first uint), claiming one when p_claim; ~0u if none.
uint lights_vis_list_slot(uint region, bool p_claim) {
	if (region == 0u) {
		return ~0u;
	}
	uint frame = uint(get_rt_param(RT_PARAM_FRAME_INDEX)) & 0xFFu;
	uint s = lights_vis_hash(region ^ 0x27d4eb2fu) % RT_LIGHT_VIS_LISTS;
	for (uint i = 0u; i < RT_LIGHT_VIS_PROBES; i++) {
		uint base = RT_LIGHT_VIS_LIST_BASE + 9u * s;
		uint k = rt_light_vis.data[base];
		if (k == region) {
			return base;
		}
		if (p_claim) {
			if (k == 0u) {
				uint prev = atomicCompSwap(rt_light_vis.data[base], 0u, region);
				if (prev == 0u || prev == region) {
					return base;
				}
				k = prev;
			}
			// A list none of whose entries was refreshed for a while goes to the newcomer.
			uint newest = 0u;
			for (uint e = 0u; e < RT_LIGHT_VIS_LIST_SIZE; e++) {
				uint v = rt_light_vis.data[base + 1u + e];
				if (v != 0u) {
					newest = max(newest, 256u - ((frame - (v >> 24u)) & 0xFFu));
				}
			}
			if (256u - newest >= RT_LIGHT_VIS_STALE && atomicCompSwap(rt_light_vis.data[base], k, region) == k) {
				for (uint e = 0u; e < RT_LIGHT_VIS_LIST_SIZE; e++) {
					atomicExchange(rt_light_vis.data[base + 1u + e], 0u);
				}
				return base;
			}
		} else if (k == 0u) {
			break;
		}
		s = (s + 1u) % RT_LIGHT_VIS_LISTS;
	}
	return ~0u;
}

// A light that lit the region joins its list, in place of the entry seen longest ago.
void lights_vis_list_add(uint region, uint light) {
	uint base = lights_vis_list_slot(region, true);
	if (base == ~0u) {
		return;
	}
	uint frame = uint(get_rt_param(RT_PARAM_FRAME_INDEX)) & 0xFFu;
	uint entry = (light + 1u) | (frame << 24u);
	uint oldest = 0u;
	uint oldest_age = 0u;
	for (uint e = 0u; e < RT_LIGHT_VIS_LIST_SIZE; e++) {
		uint v = rt_light_vis.data[base + 1u + e];
		if ((v & 0xFFFFu) == light + 1u) {
			if ((v >> 24u) != frame) {
				atomicCompSwap(rt_light_vis.data[base + 1u + e], v, entry);
			}
			return;
		}
		uint age = v == 0u ? 256u : (frame - (v >> 24u)) & 0xFFu;
		if (age >= oldest_age) {
			oldest_age = age;
			oldest = e;
		}
	}
	uint v = rt_light_vis.data[base + 1u + oldest];
	atomicCompSwap(rt_light_vis.data[base + 1u + oldest], v, entry);
}

uint lights_get_type(uint idx) {
	return bitfieldExtract(rt_light_buffer[idx].masks, 20, 2);
}

RTLightData lights_get(uint idx) {
	RTLightPacked p = rt_light_buffer[idx];
	RTLightData light;
	light.position = p.position;
	light.type = bitfieldExtract(p.masks, 20, 2);
	vec2 emission_b_radius = unpackHalf2x16(p.emission_b_radius);
	light.emission = vec3(unpackHalf2x16(p.emission_rg), emission_b_radius.x);
	light.radius = emission_b_radius.y;
	vec2 energy = unpackHalf2x16(p.energy);
	light.specular_amount = energy.x;
	light.indirect_energy = energy.y;
	vec2 attenuation_shadow = unpackHalf2x16(p.attenuation_shadow);
	light.attenuation = attenuation_shadow.x;
	light.shadow_opacity = attenuation_shadow.y;
	light.inv_max_range = p.range > 0.0 ? 1.0 / p.range : -1.0;
	light.max_range_squared = p.range * p.range;
	vec2 spot = unpackHalf2x16(p.spot);
	light.one_minus_cos_spot = spot.x;
	light.inv_spot_attenuation = spot.y;
	light.light_masks = p.masks;
	light.projector_texture = bitfieldExtract(p.projector_texture, 0, 24);
	vec4 q = vec4(unpackSnorm2x16(p.orientation_xy), unpackSnorm2x16(p.orientation_zw));
	light.orientation = q * inversesqrt(max(dot(q, q), 1e-8));
	light.projector_corner = vec2(uintBitsToFloat(p.reserved0), uintBitsToFloat(p.reserved1));
	light.projector_size = max(unpackHalf2x16(p.reserved2), vec2(0.01));
	return light;
}

// Rotates p_v by the unit quaternion p_q.
vec3 lights_rotate(vec4 p_q, vec3 p_v) {
	return p_v + 2.0 * cross(p_q.xyz, cross(p_q.xyz, p_v) + p_q.w * p_v);
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

// Godot-style windowed distance attenuation, identical to the rasterizer's get_omni_attenuation().
// window = (1 - (d/range)^4)^2, combined with pow(d, -decay). Not clamped: lights closer than
// 1 unit must brighten past 1.0, which small-range fixture lights rely on.
float lights_get_attenuation(LightSample ls, float inv_max_range, float decay) {
	if (ls.distance_sq <= 0.0) {
		return 1.0; // Directional: no distance attenuation.
	}

	float distance = sqrt(ls.distance_sq);
	float atten = pow(max(distance, 0.0001), -decay);

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

// Projector tint for light-to-surface direction p_dir, using the rasterizer's mapping. 1.0 without a projector.
vec3 lights_projector_color(RTLightData light, vec3 p_dir) {
	if (light.projector_texture == RT_LIGHT_NO_PROJECTOR) {
		return vec3(1.0);
	}
	vec3 local = lights_rotate(vec4(-light.orientation.xyz, light.orientation.w), p_dir);
	vec2 uv;
	if (light.type == RT_LIGHT_TYPE_SPOT) {
		// Perspective onto [-1, 1] along -Z with the rasterizer's fov = 2 * spot angle, clamped below 90 degrees.
		// The texture's V axis points down.
		float omc = light.one_minus_cos_spot;
		float cot_spot = max(1.0 - omc, 0.0017453) * inversesqrt(max(omc * (2.0 - omc), 1e-8));
		vec2 ndc = local.xy * cot_spot / max(-local.z, 1e-4);
		uv = vec2(ndc.x, -ndc.y) * 0.5 + 0.5;
	} else {
		// Equirectangular panorama, as the rasterizer's decal atlas converts it to dual paraboloid: -Z at u = 0, down at v = 0.
		uv = vec2(fract(atan(-local.x, -local.z) * (0.5 / PI)), acos(clamp(-local.y, -1.0, 1.0)) * (1.0 / PI));
	}
	vec4 projector = textureLod(sampler2D(bindless_textures[nonuniformEXT(light.projector_texture)], rt_light_projector_sampler), uv, 0.0);
	return projector.rgb * projector.a;
}

// A directional light's projector at p_pos, projected along the light over a rect in its plane
// (the rasterizer's directional_projector_color()). White without one or outside the rect.
vec3 lights_directional_projector_color(RTLightData light, vec3 p_pos) {
	if (light.projector_texture == RT_LIGHT_NO_PROJECTOR) {
		return vec3(1.0);
	}
	vec3 axis_u = lights_rotate(light.orientation, vec3(1.0, 0.0, 0.0));
	vec3 axis_v = -lights_rotate(light.orientation, vec3(0.0, 1.0, 0.0));
	vec2 uv = (vec2(dot(p_pos, axis_u), dot(p_pos, axis_v)) - light.projector_corner) / light.projector_size;
	if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
		return vec3(1.0);
	}
	vec4 projector = textureLod(sampler2D(bindless_textures[nonuniformEXT(light.projector_texture)], rt_light_projector_sampler), uv, 0.0);
	return projector.rgb * projector.a;
}

// Raygen (the transparent pass's light cache) has no geometry tables: it traces its shadow rays.
#ifndef RT_LIGHTS_TRACE_SHADOWS_ONLY
#include "raytracing_shadow_query_inc.glsl"
#endif

#include "raytracing_caustics_inc.glsl"
#include "raytracing_volume_inc.glsl"

// ============================================================================
// Shadow Ray (traceRayEXT pipeline)
// ============================================================================

// Returns colored light transmittance.
// Uses SkipClosestHitShader so only any_hit (alpha test) and miss are invoked.
// TerminateOnFirstHit exits on the first opaque or stochastic alpha blocker.
// p_shadow_mask is the light's 8-bit shadow-caster group mask.
vec3 lights_trace_shadow_ray(vec3 origin, vec3 direction, float max_dist, uint p_shadow_mask, inout uint rng_state) {
#if defined(USE_RAY_QUERY_SHADOWS) && !defined(RT_LIGHTS_TRACE_SHADOWS_ONLY)
	// Ray queries are faster but approximate custom or animated material outputs
	// with a direct albedo texture sample.
	return ray_query_shadow_transmittance(origin, direction, 0.001, max_dist - 0.001, p_shadow_mask, 0.0, rng_state);
#elif defined(USE_SER)
	PathPayload saved_payload = payload;
	PathState shadow_ps;
	shadow_ps.bsdf_pdf = 0.0;
	shadow_ps.radiance = vec3(0.0);
	shadow_ps.throughput = vec3(1.0);
	shadow_ps.packed_bounces_flags = set_shadow_ray(0u);
	shadow_ps.rng_state = rng_state;
	shadow_ps.hit_t = 0.0;
	shadow_ps.offset_normal = vec3(0.0, 0.0, 1.0);
	shadow_ps.next_ray_dir = vec3(0.0, 0.0, 1.0);
	path_pack(payload, shadow_ps);

	hitObjectEXT hitObject;
	hitObjectTraceRayEXT(hitObject, tlas,
			gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT | gl_RayFlagsNoOpaqueEXT,
			p_shadow_mask, 0, 0, 0,
			origin, 0.001, direction, max_dist - 0.001, 0);

	shadow_ps = path_unpack(payload);
	rng_state = shadow_ps.rng_state;
	payload = saved_payload;
	return hitObjectIsHitEXT(hitObject) ? vec3(0.0) : shadow_ps.throughput;
#else
	// The miss shader copies accumulated transmittance to radiance.
	// If an opaque hit occurs, miss is never called and radiance stays vec3(0.0).
	// Save full payload, set up shadow ray, then restore after trace.
	PathPayload saved_payload = payload;

	PathState shadow_ps;
	shadow_ps.bsdf_pdf = 0.0;
	shadow_ps.radiance = vec3(0.0);
	shadow_ps.throughput = vec3(1.0);
	shadow_ps.packed_bounces_flags = set_shadow_ray(0u);
	shadow_ps.rng_state = rng_state;
	shadow_ps.hit_t = 0.0;
	shadow_ps.offset_normal = vec3(0.0, 0.0, 1.0);
	shadow_ps.next_ray_dir = vec3(0.0, 0.0, 1.0);
	path_pack(payload, shadow_ps);

	traceRayEXT(tlas,
			gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT | gl_RayFlagsNoOpaqueEXT,
			p_shadow_mask, 0, 0, 0,
			origin, 0.001, direction, max_dist - 0.001, 0);

	// Unpack the colored visibility written by the miss shader.
	shadow_ps = path_unpack(payload);
	vec3 transmittance = shadow_ps.radiance;
	rng_state = shadow_ps.rng_state;

	payload = saved_payload;

	return transmittance;
#endif
}

// Shadow transmittance from origin towards the light, blended towards 1 by the light's shadow opacity.
// Skips the trace when the path tracer draws no shadow for this light. p_receiver_shadow_mask filters the light's caster groups.
// The shaded point's shadow ray origins on its two sides (rt_set_shadow_origins()). A light
// behind the surface (back lighting, wrap, toon) is traced from the back side: from the front,
// its shadow ray would start by hitting the surface itself.
vec3 rt_shadow_front_origin = vec3(0.0);
vec3 rt_shadow_back_origin = vec3(0.0);
vec3 rt_shadow_side_normal = vec3(0.0);

void rt_set_shadow_origins(vec3 p_front, vec3 p_back, vec3 p_geometry_normal) {
	rt_shadow_front_origin = p_front;
	rt_shadow_back_origin = p_back;
	rt_shadow_side_normal = p_geometry_normal;
}

// p_origin, or the back origin for a light behind the shaded point (p_origin its front origin).
vec3 rt_shadow_origin(vec3 p_origin, vec3 p_direction) {
	return p_origin == rt_shadow_front_origin && dot(p_direction, rt_shadow_side_normal) < 0.0 ? rt_shadow_back_origin : p_origin;
}

vec3 lights_shadow_transmittance(RTLightData light, vec3 origin, vec3 direction, float max_dist, uint p_receiver_shadow_mask, inout uint rng_state) {
	if (light.shadow_opacity <= 0.001) {
		return vec3(1.0);
	}
	origin = rt_shadow_origin(origin, direction);
	vec3 traced = lights_trace_shadow_ray(origin, direction, max_dist, (light.light_masks >> 24u) & p_receiver_shadow_mask, rng_state);
	if (max(traced.r, max(traced.g, traced.b)) > 0.0) {
		// Simulated gas between the point and the light (ratio tracking).
		traced *= rt_volume_transmittance(origin, direction, max_dist, rng_state);
	}
	return mix(vec3(1.0), traced, light.shadow_opacity);
}

// ============================================================================
// Next Event Estimation (NEE) - Direct Light Sampling
// ============================================================================

// gather_lights() packs every directional light before the positional ones.
uint lights_directional_count(uint light_count) {
	uint count = 0u;
	while (count < light_count && lights_get_type(count) == RT_LIGHT_TYPE_DIRECTIONAL) {
		count++;
	}
	return count;
}

// Mini-batch reservoir selection over rt_light_buffer[first, first + count): draws k
// candidates and keeps one valid light uniformly. Returns false if none is valid.
bool lights_select(vec3 hit_pos, uint first, uint count, uint instance_layers, inout uint rng_state,
		out uint r_idx, out float r_select_pdf) {
	r_idx = first;
	r_select_pdf = 1.0;
	if (count == 0u) {
		return false;
	}

	const uint k = min(uint(RT_LIGHT_RESERVOIR_SIZE), count);
	uint valid_found = 0u;

	for (uint i = 0u; i < k; i++) {
		uint idx = first + min(uint(rand(rng_state) * float(count)), count - 1u);
		RTLightData test_light = lights_get(idx);

		// light_cull_mask: the light only affects intersecting layers.
		bool is_valid = (test_light.light_masks & instance_layers & 0xFFFFFu) != 0u;

		// Range check for positional lights.
		bool is_positional = (test_light.type == RT_LIGHT_TYPE_OMNI || test_light.type == RT_LIGHT_TYPE_SPOT);
		if (is_valid && is_positional) {
			vec3 to_l = test_light.position - hit_pos;
			float d2 = dot(to_l, to_l);
			is_valid = (test_light.max_range_squared == 0.0 || d2 <= test_light.max_range_squared);
		}

		if (is_valid) {
			valid_found++;
			if (rand(rng_state) < 1.0 / float(valid_found)) {
				r_idx = idx;
			}
		}
	}

	if (valid_found == 0u) {
		return false;
	}

	// Estimate valid count from sample ratio, PDF = 1/validCount.
	float valid_count_estimate = float(count) * (float(valid_found) / float(k));
	r_select_pdf = 1.0 / max(valid_count_estimate, 1.0);
	return true;
}

// A positional light's unshadowed contribution at a point, roughly (luminance,
// distance and cone falloff, cosine): the target the selection resamples by. Never 0
// where the light can add anything: sphere lights get the slack of their size on the
// cosine and the cone, as a sampled direction may stray that far.
float lights_target(RTLightData light, vec3 p, vec3 N, bool is_indirect_bounce, uint instance_layers) {
	if ((light.light_masks & instance_layers & 0xFFFFFu) == 0u) {
		return 0.0;
	}
	// abs: a negative light takes light away, and is picked by how much.
	float lum = dot(abs(light.emission), vec3(0.2126, 0.7152, 0.0722));
	if (is_indirect_bounce) {
		lum *= max(light.indirect_energy, 0.0);
	}
	if (lum <= 0.0) {
		return 0.0;
	}
	vec3 to_l = light.position - p;
	float d2 = dot(to_l, to_l);
	if (light.max_range_squared != 0.0 && d2 > light.max_range_squared) {
		return 0.0;
	}
	float d = sqrt(d2);
	vec3 dir = to_l / max(d, 1e-4);
	float slack = light.radius > 0.01 ? min(2.0 * light.radius / max(d, 1e-4), 1.0) : 0.0;
	// Backlit and transmissive materials are lit from behind too.
	float cos_n = clamp(abs(dot(N, dir)) + slack, 0.0, 1.0);
	LightSample ls;
	ls.distance_sq = d2;
	float atten = lights_get_attenuation(ls, light.inv_max_range, light.attenuation);
	if (light.type == RT_LIGHT_TYPE_SPOT) {
		float one_minus_scos = 1.0 + dot(dir, lights_rotate(light.orientation, vec3(0.0, 0.0, -1.0)));
		if (one_minus_scos >= light.one_minus_cos_spot + slack) {
			return 0.0;
		}
		float spot_rim = clamp(one_minus_scos / max(light.one_minus_cos_spot, 1e-4), 1e-4, 1.0);
		float spot = 1.0 - pow(spot_rim, light.inv_spot_attenuation);
		atten *= max(spot, slack > 0.0 ? 0.02 : 0.0);
	}
	// A floor so a light at its range's edge still gets picked now and then.
	return lum * max(atten, 1e-6) * max(cos_n, 0.05);
}

// Picks a positional light by resampled importance sampling (Talbot, Cline & Egbert,
// "Importance Resampling for Global Illumination", EGSR 2005; the candidate step of
// ReSTIR DI, Bitterli et al. 2020): candidates drawn uniformly from the lights that can
// reach this point (the light grid's lists), each weighted by its unshadowed
// contribution over its source pdf, one kept in proportion. r_select_pdf is the
// estimator's p_hat(y) / mean(w), so contribution / r_select_pdf stays unbiased. With
// no more lights than candidates, each is taken once, with less noise still. The
// weight also carries what the visibility memory knows of each light from p_region.
bool lights_select_positional(vec3 hit_pos, vec3 N, bool is_indirect_bounce, uint instance_layers, uint p_region, inout uint rng_state,
		out uint r_idx, out float r_select_pdf) {
	r_idx = 0u;
	r_select_pdf = 1.0;
	uint g_start, g_count, c_start, c_count;
	lights_grid_lists(hit_pos, g_start, g_count, c_start, c_count);
	uint n = g_count + c_count;
	if (n == 0u) {
		return false;
	}
	const bool enumerate = n <= uint(RT_LIGHT_RESERVOIR_SIZE);
	const uint m = enumerate ? n : uint(RT_LIGHT_RESERVOIR_SIZE);

	// The region's visible-light list, when there is one: half the candidates come from
	// it. Each candidate is weighted by the balance heuristic over both sources
	// (Veach & Guibas 1995; multi-strategy RIS, Talbot et al. 2005), so a light found
	// either way counts once.
	uint list[RT_LIGHT_VIS_LIST_SIZE];
	uint list_count = 0u;
	if (!enumerate) {
		uint base = lights_vis_list_slot(p_region, false);
		if (base != ~0u) {
			for (uint e = 0u; e < RT_LIGHT_VIS_LIST_SIZE; e++) {
				uint v = rt_light_vis.data[base + 1u + e] & 0xFFFFu;
				if (v != 0u) {
					list[list_count++] = v - 1u;
				}
			}
		}
	}
	const uint m_list = list_count > 0u ? m / 2u : 0u;
	const uint m_grid = m - m_list;

	float w_sum = 0.0;
	float selected_p_hat = 0.0;
	for (uint i = 0u; i < m; i++) {
		uint idx;
		if (i < m_list) {
			idx = list[min(uint(rand(rng_state) * float(list_count)), list_count - 1u)];
		} else {
			uint j = enumerate ? i : min(uint(rand(rng_state) * float(n)), n - 1u);
			idx = lights_grid_light(j, g_start, g_count, c_start);
		}
		float p_hat = lights_target(lights_get(idx), hit_pos, N, is_indirect_bounce, instance_layers);
		if (p_hat <= 0.0) {
			continue;
		}
		p_hat *= lights_vis_weight(p_region, idx);
		float w;
		if (enumerate) {
			w = p_hat;
		} else {
			// Sum over sources of candidates drawn times the source's pdf. A listed light is
			// assumed to be in the grid's lists here: one it isn't in can't light the point.
			float in_list = 0.0;
			for (uint e = 0u; e < list_count; e++) {
				in_list += list[e] == idx ? 1.0 : 0.0;
			}
			float denom = float(m_grid) / float(n) + (list_count > 0u ? float(m_list) * in_list / float(list_count) : 0.0);
			w = p_hat / denom;
		}
		w_sum += w;
		if (rand(rng_state) * w_sum < w) {
			r_idx = idx;
			selected_p_hat = p_hat;
		}
	}
	if (w_sum <= 0.0 || selected_p_hat <= 0.0) {
		return false;
	}
	r_select_pdf = selected_p_hat / w_sum;
	return true;
}

// ============================================================================
// Light reuse across pixels and frames: ReSTIR DI (Bitterli et al. 2020), as RTXDI
// (Wyman and Panteleev 2021), with generalized RIS weights (Lin et al. 2022). See
// docs/ideas/light-reuse.md.
//
// At a camera ray's first hit, the positional light chosen is kept per pixel as a
// reservoir: the light, its contribution weight W and its confidence M, with the surface
// it was chosen for. The next frame the pixel resamples its fresh choice together with its
// own reservoir (found through the motion vector) and two neighbours', each weighted by the
// balance heuristic over the surfaces' targets, so a light one pixel found lights the
// pixels around it. Visibility is not folded in: the image stays unbiased.
//
// Binding 51: header (flags, last frame's light count, width, height), the light remap
// (last frame's index -> this frame's + 1, 0 when gone), then two frames of reservoirs of
// RT_LIGHT_REUSE_STRIDE uints: light + 1 (bits 0-15) and M (16-23), W, position (relative
// to the anchor), packed normal.
// ============================================================================

#define RT_LIGHT_REUSE_BINDING 51
layout(set = 0, binding = RT_LIGHT_REUSE_BINDING, std430) restrict buffer LightReuseBuffer {
	uint data[];
}
rt_light_reuse;

#define RT_LIGHT_REUSE_REMAP 4096u // must match RT_LIGHTS_MAX (render_raytracing.h)
#define RT_LIGHT_REUSE_HEADER (4u + RT_LIGHT_REUSE_REMAP)
#define RT_LIGHT_REUSE_STRIDE 6u
#define RT_LIGHT_REUSE_M_CAP 20u // frames of history (RTXDI's default)
#define RT_LIGHT_REUSE_NEIGHBOURS 2u
#define RT_LIGHT_REUSE_RADIUS 16.0 // pixels
#define RT_LIGHT_REUSE_NORMAL 0.9 // cosine a neighbour's normal must reach
#define RT_LIGHT_REUSE_PLANE 0.05 // distance off this surface's plane, over the distance to the camera

// Set by the closest hit before shading a camera ray's first hit (raytracing_closest_hit_common_inc.glsl).
bool light_reuse_here = false;
bool light_reuse_first_sample = false;
vec3 light_reuse_prev_pos = vec3(0.0); // where the surface was last frame
vec2 light_reuse_motion = vec2(0.0); // its motion vector (rt_motion_vector)
vec3 light_reuse_eye = vec3(0.0); // the camera ray's origin

struct LightReservoir {
	uint light; // ~0u: none
	float W;
	uint M;
	vec3 pos;
	vec3 N;
};

bool light_reuse_enabled() {
	if (get_rt_param(RT_PARAM_LIGHT_REUSE) < 0.5) {
		return false;
	}
	uint pixels = gl_LaunchSizeEXT.x * gl_LaunchSizeEXT.y;
	return rt_light_reuse.data[2] == gl_LaunchSizeEXT.x && rt_light_reuse.data[3] == gl_LaunchSizeEXT.y &&
			uint(rt_light_reuse.data.length()) >= RT_LIGHT_REUSE_HEADER + 2u * pixels * RT_LIGHT_REUSE_STRIDE;
}

uint light_reuse_state() {
	return uint(get_rt_param(RT_PARAM_LIGHT_REUSE)) - 1u; // bit 0: slot written, bit 1: last frame's valid
}

uint light_reuse_base(uint p_slot, ivec2 p_pixel) {
	uint pixels = gl_LaunchSizeEXT.x * gl_LaunchSizeEXT.y;
	return RT_LIGHT_REUSE_HEADER + (p_slot * pixels + uint(p_pixel.y) * gl_LaunchSizeEXT.x + uint(p_pixel.x)) * RT_LIGHT_REUSE_STRIDE;
}

LightReservoir light_reuse_load(uint p_slot, ivec2 p_pixel) {
	uint b = light_reuse_base(p_slot, p_pixel);
	uint head = rt_light_reuse.data[b];
	LightReservoir r;
	r.light = (head & 0xFFFFu) == 0u ? ~0u : (head & 0xFFFFu) - 1u;
	r.M = (head >> 16u) & 0xFFu;
	r.W = uintBitsToFloat(rt_light_reuse.data[b + 1u]);
	r.pos = uintBitsToFloat(uvec3(rt_light_reuse.data[b + 2u], rt_light_reuse.data[b + 3u], rt_light_reuse.data[b + 4u]));
	r.N = oct_to_vec3(unpackUnorm2x16(rt_light_reuse.data[b + 5u]) * 2.0 - 1.0);
	return r;
}

void light_reuse_store(uint p_slot, ivec2 p_pixel, LightReservoir r) {
	uint b = light_reuse_base(p_slot, p_pixel);
	bool has = r.light != ~0u && r.W > 0.0 && !isinf(r.W) && !isnan(r.W);
	rt_light_reuse.data[b] = (has ? r.light + 1u : 0u) | (min(r.M, 255u) << 16u);
	rt_light_reuse.data[b + 1u] = floatBitsToUint(has ? r.W : 0.0);
	rt_light_reuse.data[b + 2u] = floatBitsToUint(r.pos.x);
	rt_light_reuse.data[b + 3u] = floatBitsToUint(r.pos.y);
	rt_light_reuse.data[b + 4u] = floatBitsToUint(r.pos.z);
	rt_light_reuse.data[b + 5u] = packUnorm2x16(vec3_to_oct(r.N));
}

// Last frame's reservoir at p_pixel, if its surface matches this one (seen where it was last
// frame), with its light renumbered for this frame.
bool light_reuse_load_prev(ivec2 p_pixel, vec3 p_pos, vec3 N, float p_eye_dist, out LightReservoir r) {
	if (any(lessThan(p_pixel, ivec2(0))) || any(greaterThanEqual(p_pixel, ivec2(gl_LaunchSizeEXT.xy)))) {
		return false;
	}
	r = light_reuse_load(1u - (light_reuse_state() & 1u), p_pixel);
	if (r.M == 0u || dot(r.N, N) < RT_LIGHT_REUSE_NORMAL || abs(dot(N, r.pos - p_pos)) > RT_LIGHT_REUSE_PLANE * p_eye_dist) {
		return false;
	}
	if (r.light != ~0u) {
		uint mapped = r.light < min(rt_light_reuse.data[1], RT_LIGHT_REUSE_REMAP) ? rt_light_reuse.data[4u + r.light] : 0u;
		r.light = mapped == 0u ? ~0u : mapped - 1u;
	}
	return true;
}

// The positional light for a camera ray's first hit, through the reservoirs. r_W: the
// contribution weight (the light's contribution times r_W estimates the group's). False
// when no light is chosen.
bool lights_select_reused(vec3 hit_pos, vec3 N, uint instance_layers, uint p_region, inout uint rng_state,
		out uint r_idx, out float r_W) {
	r_idx = 0u;
	r_W = 0.0;
	uint state = light_reuse_state();
	ivec2 pixel = ivec2(gl_LaunchIDEXT.xy);

	LightReservoir res[1u + 1u + RT_LIGHT_REUSE_NEIGHBOURS];
	uint count = 0u;

	// This frame's candidates.
	uint fresh_idx;
	float fresh_pdf;
	res[0].light = lights_select_positional(hit_pos, N, false, instance_layers, p_region, rng_state, fresh_idx, fresh_pdf) ? fresh_idx : ~0u;
	res[0].W = res[0].light != ~0u ? 1.0 / max(fresh_pdf, 1e-20) : 0.0;
	res[0].M = 1u;
	res[0].pos = hit_pos;
	res[0].N = N;
	count = 1u;

	// Last frame's: this surface's own reservoir, then neighbours'.
	if ((state & 2u) != 0u) {
		float eye_dist = max(distance(light_reuse_eye, hit_pos), 0.01);
		vec2 prev_px = vec2(pixel) + 0.5 + light_reuse_motion * vec2(gl_LaunchSizeEXT.xy);
		LightReservoir r;
		if (light_reuse_load_prev(ivec2(floor(prev_px)), light_reuse_prev_pos, N, eye_dist, r)) {
			r.M = min(r.M, RT_LIGHT_REUSE_M_CAP);
			res[count++] = r;
		}
		for (uint k = 0u; k < RT_LIGHT_REUSE_NEIGHBOURS; k++) {
			vec2 u = rand2(rng_state);
			float rad = RT_LIGHT_REUSE_RADIUS * sqrt(u.x);
			vec2 off = rad * vec2(cos(6.2831853 * u.y), sin(6.2831853 * u.y));
			if (dot(off, off) < 1.0) {
				continue;
			}
			if (light_reuse_load_prev(ivec2(floor(prev_px + off)), light_reuse_prev_pos, N, eye_dist, r)) {
				r.M = min(r.M, RT_LIGHT_REUSE_M_CAP);
				res[count++] = r;
			}
		}
	}

	// Resample: each reservoir's light weighted by its target here, times its W, times the
	// balance heuristic over the surfaces' unshadowed targets (generalized RIS).
	float w_sum = 0.0;
	float chosen_p_hat = 0.0;
	uint chosen = ~0u;
	uint m_sum = 0u;
	for (uint i = 0u; i < count; i++) {
		m_sum += res[i].M;
		if (res[i].light == ~0u || res[i].W <= 0.0) {
			continue;
		}
		RTLightData light = lights_get(res[i].light);
		float t_here = lights_target(light, hit_pos, N, false, instance_layers);
		if (t_here <= 0.0) {
			continue;
		}
		float p_hat = t_here * lights_vis_weight(p_region, res[i].light);
		float own = 0.0;
		float total = 0.0;
		for (uint j = 0u; j < count; j++) {
			float t = j == 0u ? t_here : lights_target(light, res[j].pos, res[j].N, false, instance_layers);
			total += float(res[j].M) * t;
			if (j == i) {
				own = float(res[j].M) * t;
			}
		}
		float w = total > 0.0 ? own / total * p_hat * res[i].W : 0.0;
		w_sum += w;
		if (rand(rng_state) * w_sum < w) {
			chosen = res[i].light;
			chosen_p_hat = p_hat;
		}
	}

	LightReservoir out_r;
	out_r.light = chosen;
	out_r.W = chosen != ~0u ? w_sum / chosen_p_hat : 0.0;
	out_r.M = min(m_sum, RT_LIGHT_REUSE_M_CAP);
	out_r.pos = hit_pos;
	out_r.N = N;

	uint slot = state & 1u;
	if (!light_reuse_first_sample) {
		// Later samples of the pixel merge into what the earlier ones stored (same surface).
		LightReservoir prev = light_reuse_load(slot, pixel);
		float w_a = out_r.light != ~0u ? chosen_p_hat * out_r.W * float(out_r.M) : 0.0;
		float p_b = 0.0;
		if (prev.light != ~0u && prev.W > 0.0) {
			p_b = lights_target(lights_get(prev.light), hit_pos, N, false, instance_layers) * lights_vis_weight(p_region, prev.light);
		}
		float w_b = p_b * prev.W * float(prev.M);
		LightReservoir merged = out_r;
		merged.M = min(out_r.M + prev.M, RT_LIGHT_REUSE_M_CAP);
		if (w_a + w_b > 0.0 && float(out_r.M + prev.M) > 0.0) {
			bool take_b = rand(rng_state) * (w_a + w_b) < w_b;
			merged.light = take_b ? prev.light : out_r.light;
			merged.W = (w_a + w_b) / ((take_b ? p_b : chosen_p_hat) * float(out_r.M + prev.M));
		}
		light_reuse_store(slot, pixel, merged);
	} else {
		light_reuse_store(slot, pixel, out_r);
	}

	if (chosen == ~0u || w_sum <= 0.0) {
		return false;
	}
	r_idx = chosen;
	r_W = out_r.W;
	return true;
}

// Where a camera ray's first hit samples the directional group instead, its reservoir is
// carried over from last frame so the history survives.
void lights_reuse_carry(vec3 hit_pos, vec3 N) {
	if (!light_reuse_first_sample) {
		return;
	}
	uint state = light_reuse_state();
	ivec2 pixel = ivec2(gl_LaunchIDEXT.xy);
	LightReservoir r;
	r.light = ~0u;
	r.W = 0.0;
	r.M = 0u;
	if ((state & 2u) != 0u) {
		float eye_dist = max(distance(light_reuse_eye, hit_pos), 0.01);
		vec2 prev_px = vec2(pixel) + 0.5 + light_reuse_motion * vec2(gl_LaunchSizeEXT.xy);
		if (!light_reuse_load_prev(ivec2(floor(prev_px)), light_reuse_prev_pos, N, eye_dist, r)) {
			r.light = ~0u;
			r.W = 0.0;
			r.M = 0u;
		}
	}
	r.pos = hit_pos;
	r.N = N;
	light_reuse_store(state & 1u, pixel, r);
}

// Unweighted contribution of one omni or spot light, one shadow ray. r_visible: the
// shadow ray's outcome (1 through, 0 blocked), -1 when none was traced.
vec3 lights_evaluate_positional(RTLightData light, vec3 hit_pos, vec3 N, vec3 V, MaterialProperties material,
		inout uint rng_state, bool is_indirect_bounce, uint p_receiver_shadow_mask, out float r_visible) {
	r_visible = -1.0;
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
		L = lights_sample_cone(ls, rand2(rng_state), light_pdf);
		float t_center = dot(to_light, L);
		vec3 perp = to_light - t_center * L;
		float perp_sq = dot(perp, perp);
		float dt = sqrt(max(0.0, light.radius * light.radius - perp_sq));
		shadow_dist = max(0.0, t_center - dt);
	}

	// Spot cone early-out.
	float spot_atten = 1.0;
	if (light.type == RT_LIGHT_TYPE_SPOT) {
		float one_minus_scos = 1.0 + dot(L, lights_rotate(light.orientation, vec3(0.0, 0.0, -1.0)));
		if (one_minus_scos >= light.one_minus_cos_spot) {
			return vec3(0.0);
		}
		float spot_rim = max(1e-4, one_minus_scos / light.one_minus_cos_spot);
		spot_atten = 1.0 - pow(spot_rim, light.inv_spot_attenuation);
	}

	// From the light center, as in raster, so area lights keep a sharp projection.
	vec3 projector_color = lights_projector_color(light, -to_light / max(dist, 0.0001));
	if (all(lessThanEqual(projector_color, vec3(0.0)))) {
		return vec3(0.0);
	}

	vec3 shadow_transmittance = lights_shadow_transmittance(light, hit_pos, L, shadow_dist, p_receiver_shadow_mask, rng_state);
	r_visible = max(shadow_transmittance.r, max(shadow_transmittance.g, shadow_transmittance.b)) > 0.5 ? 1.0 : 0.0;

	// Evaluate BRDF (diffuse incl. backlight, specular separately for specular_amount control).
	vec3 brdf_diffuse, brdf_specular;
	evalCombinedBRDFSeparate(N, L, V, material, brdf_diffuse, brdf_specular);

	// Distance attenuation.
	LightSample ls_atten;
	ls_atten.distance_sq = dist_sq;
	float atten = lights_get_attenuation(ls_atten, light.inv_max_range, light.attenuation) * spot_atten;

	float spec_mul = lights_get_specular_multiplier(light.specular_amount, material.roughness);
	vec3 brdf_value = brdf_diffuse + brdf_specular * spec_mul;

	float indirect_mul = is_indirect_bounce ? light.indirect_energy : 1.0;

	// NdotL is already included in brdf_value.
	return brdf_value * light.emission * projector_color * shadow_transmittance * atten * indirect_mul;
}

// Unweighted contribution of one directional light, one shadow ray. r_visible as for
// lights_evaluate_positional.
vec3 lights_evaluate_directional(RTLightData light, vec3 hit_pos, vec3 N, vec3 V, MaterialProperties material,
		inout uint rng_state, bool is_indirect_bounce, uint p_receiver_shadow_mask, out float r_visible) {
	r_visible = -1.0;
	LightSample ls = lights_prepare_sample(hit_pos, light);
	float light_pdf;
	vec3 L = lights_sample_cone(ls, rand2(rng_state), light_pdf);

	if (dot(N, L) <= 0.0 && !godotShadesBackfacingLight(material)) {
		return vec3(0.0);
	}

	vec3 projector_color = lights_directional_projector_color(light, hit_pos);
	if (all(lessThanEqual(projector_color, vec3(0.0)))) {
		return vec3(0.0);
	}

	vec3 shadow_transmittance = lights_shadow_transmittance(light, hit_pos, L, ls.max_distance, p_receiver_shadow_mask, rng_state) * projector_color;
	r_visible = max(shadow_transmittance.r, max(shadow_transmittance.g, shadow_transmittance.b)) > 0.5 ? 1.0 : 0.0;
	if (max(shadow_transmittance.r, max(shadow_transmittance.g, shadow_transmittance.b)) > 0.0 && rt_caustics_enabled() && light.position == lights_get(0u).position) {
		// Under water, the sun's light gathered into lines by the surface (raytracing_caustics_inc.glsl).
		shadow_transmittance *= rt_caustics_ratio(hit_pos);
	}

	vec3 brdf_diffuse, brdf_specular;
	evalCombinedBRDFSeparate(N, L, V, material, brdf_diffuse, brdf_specular);

	float spec_mul = lights_get_specular_multiplier(light.specular_amount, material.roughness);
	vec3 brdf_value = brdf_diffuse + brdf_specular * spec_mul;

	float indirect_mul = is_indirect_bounce ? light.indirect_energy : 1.0;

	return brdf_value * light.emission * shadow_transmittance * indirect_mul;
}

// NEE with one shadow ray to a single light selected among all lights.
vec3 lights_evaluate_direct_lighting(
		vec3 hit_pos,
		vec3 N,
		vec3 V,
		MaterialProperties material,
		inout uint rng_state,
		bool is_indirect_bounce,
		uint light_count,
		uint instance_layers,
		uint p_receiver_shadow_mask) {
	// One group, then one light in it: directional lights uniformly, positional ones
	// through the light grid by resampled importance sampling. The directional group
	// gets half the rays, less where the visibility memory has seen it blocked (under
	// a roof): down to RT_LIGHT_VIS_FLOOR / (1 + RT_LIGHT_VIS_FLOOR).
	uint region = lights_vis_region(hit_pos, N);
	uint directional_count = lights_directional_count(light_count);
	bool has_positional = light_count > directional_count;
	float p_directional = 1.0;
	if (directional_count > 0u && has_positional) {
		float f = region == 0u ? 1.0 : clamp(2.0 * lights_vis_read(region, RT_LIGHT_VIS_DIRECTIONAL), RT_LIGHT_VIS_FLOOR, 1.0);
		p_directional = f / (f + 1.0);
	} else if (directional_count == 0u) {
		p_directional = 0.0;
	}
	bool pick_directional = directional_count > 0u && (!has_positional || rand(rng_state) < p_directional);
	bool reuse = light_reuse_here && !is_indirect_bounce && light_reuse_enabled();
	float group_pdf = pick_directional ? p_directional : 1.0 - p_directional;

	uint idx;
	float select_pdf;
	float visible;
	if (pick_directional) {
		if (!lights_select(hit_pos, 0u, directional_count, instance_layers, rng_state, idx, select_pdf)) {
			return vec3(0.0);
		}
		vec3 contribution = lights_evaluate_directional(lights_get(idx), hit_pos, N, V, material,
				rng_state, is_indirect_bounce, p_receiver_shadow_mask, visible);
		if (visible >= 0.0 && has_positional) {
			lights_vis_record(region, RT_LIGHT_VIS_DIRECTIONAL, visible > 0.5 && any(notEqual(contribution, vec3(0.0))));
		}
		if (reuse && has_positional) {
			lights_reuse_carry(hit_pos, N);
		}
		return contribution / max(select_pdf * group_pdf, 1e-10);
	}
	if (reuse) {
		float W;
		if (!lights_select_reused(hit_pos, N, instance_layers, region, rng_state, idx, W)) {
			return vec3(0.0);
		}
		select_pdf = 1.0 / W;
	} else if (!lights_select_positional(hit_pos, N, is_indirect_bounce, instance_layers, region, rng_state, idx, select_pdf)) {
		return vec3(0.0);
	}
	vec3 contribution = lights_evaluate_positional(lights_get(idx), hit_pos, N, V, material,
			rng_state, is_indirect_bounce, p_receiver_shadow_mask, visible);
	if (visible >= 0.0) {
		lights_vis_record(region, idx, visible > 0.5 && any(notEqual(contribution, vec3(0.0))));
	}
	return contribution / max(select_pdf * group_pdf, 1e-10);
}

// NEE with up to two shadow rays: one to a directional light and one to a
// positional light, each selected within its own group.
vec3 lights_evaluate_direct_lighting_per_group(
		vec3 hit_pos,
		vec3 N,
		vec3 V,
		MaterialProperties material,
		inout uint rng_state,
		bool is_indirect_bounce,
		uint light_count,
		uint instance_layers,
		uint p_receiver_shadow_mask) {
	vec3 result = vec3(0.0);
	uint region = lights_vis_region(hit_pos, N);
	uint directional_count = lights_directional_count(light_count);

	uint idx;
	float select_pdf;
	float visible;
	if (lights_select(hit_pos, 0u, directional_count, instance_layers, rng_state, idx, select_pdf)) {
		vec3 contribution = lights_evaluate_directional(lights_get(idx), hit_pos, N, V, material,
				rng_state, is_indirect_bounce, p_receiver_shadow_mask, visible);
		result += contribution / max(select_pdf, 1e-10);
	}
	bool reuse = light_reuse_here && !is_indirect_bounce && light_reuse_enabled();
	float reuse_W = 0.0;
	if (light_count > directional_count && (reuse ? lights_select_reused(hit_pos, N, instance_layers, region, rng_state, idx, reuse_W) : lights_select_positional(hit_pos, N, is_indirect_bounce, instance_layers, region, rng_state, idx, select_pdf))) {
		if (reuse) {
			select_pdf = 1.0 / reuse_W;
		}
		vec3 contribution = lights_evaluate_positional(lights_get(idx), hit_pos, N, V, material,
				rng_state, is_indirect_bounce, p_receiver_shadow_mask, visible);
		if (visible >= 0.0) {
			lights_vis_record(region, idx, visible > 0.5 && any(notEqual(contribution, vec3(0.0))));
		}
		result += contribution / max(select_pdf, 1e-10);
	}
	return result;
}

#include "raytracing_radiance_cache_inc.glsl"

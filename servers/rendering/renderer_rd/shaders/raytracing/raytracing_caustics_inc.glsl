// Water caustics (binding 46): a caustics map for the first directional light, as the ray-guided
// water caustics of Gruen (Ray Tracing Gems, 2019) and Yang and Ouyang (Ray Tracing Gems II,
// ch. 30, 2021), fed to the path tracer's sun light instead of drawn over the screen.
//
// Raygen shoots one photon a pixel from the sun across a square (rendering/pathtracing/caustics_area)
// around the point the camera looked at last frame (the centre pixel's first surface). Where a photon enters a medium's surface
// (RT_MAT_FLAG_VOLUME, or a shader writing IOR), the surface's own material bends it (normal maps
// and shader ripples included), and two photons go on from there: the bent one, and a straight
// twin that keeps the sun's direction. Each lands on the first surface past the medium, and is
// added to the light-space texel above its landing point (bilinear). A texel's bent count over its
// straight count is how much the surface focuses the sun there: 1 under flat water, above 1 in the
// bright lines, below 1 between them. The sun's shadow ray already crosses the water straight
// (Fresnel and absorption, raytracing_shadow_query_inc.glsl), so the sun light at a point under
// water is multiplied by that ratio (lights_evaluate_directional). Points whose texel saw no
// straight photon, or one landing at another depth, keep the plain sun.
//
// The map is three slots: the frame adds to one, reads the one the last frame wrote, and clears
// the one it read last frame (each pixel's thread clears its share), so no separate pass runs.
// After the slots, a header per slot: the distance the camera looked to when it was written,
// and the square's centre it used.

#define RT_CAUSTICS_BINDING 46
#define RT_CAUSTICS_RES 512u // texels across, must match RT_CAUSTICS_RES (render_raytracing.h)
#define RT_CAUSTICS_SLOT_UINTS (RT_CAUSTICS_RES * RT_CAUSTICS_RES * 3u)
#define RT_CAUSTICS_HEADER (3u * RT_CAUSTICS_SLOT_UINTS) // 4 uints a slot: look distance, centre xyz
#define RT_CAUSTICS_UINTS (RT_CAUSTICS_HEADER + 12u) // must match RT_CAUSTICS_UINTS (render_raytracing.h)
#define RT_CAUSTICS_WEIGHT 64.0 // fixed-point steps of one photon
#define RT_CAUSTICS_DEPTH_SCALE 100.0 // depth sum steps per metre
#define RT_CAUSTICS_DEPTH_BIAS 50000.0 // depths are stored from -500 m to 500 m around the area's centre
#define RT_CAUSTICS_DEPTH_TOLERANCE 0.3 // metres a point may lie off the depth its texel's photons landed at
#define RT_CAUSTICS_MAX_RATIO 32.0
#define RT_CAUSTICS_SUN_HEIGHT 2000.0 // metres above the area's centre the photons start

// Ray input on non-peel rays: a caustic photon. The hit reports the surface instead of shading
// it: MEDIUM_CHANGE_FLAG for a medium's surface, with next_ray_dir the photon's direction past it
// (bent at a front face; bsdf_pdf 1 when it was), and nothing for any other surface.
const uint CAUSTIC_PHOTON_FLAG = (1u << 18);

layout(set = 0, binding = RT_CAUSTICS_BINDING, std430) coherent buffer CausticsBuffer {
	uint data[];
}
rt_caustics;

// RT_PARAM_CAUSTICS_STATE: 0 off, else 1 + the slot written this frame + 4 when last frame's is valid.
bool rt_caustics_enabled() {
	return get_rt_param(RT_PARAM_CAUSTICS_STATE) > 0.5 && uint(rt_caustics.data.length()) >= RT_CAUSTICS_UINTS;
}

uint rt_caustics_write_slot() {
	return (uint(get_rt_param(RT_PARAM_CAUSTICS_STATE)) - 1u) & 3u;
}

bool rt_caustics_history() {
	return ((uint(get_rt_param(RT_PARAM_CAUSTICS_STATE)) - 1u) & 4u) != 0u;
}

struct CausticsFrame {
	vec3 L; // towards the light
	vec3 r;
	vec3 u;
	vec2 origin; // the area's corner along r and u, snapped to texels so the map holds still
	float size;
	float ref_depth; // the area's centre along L
};

// The first light, when it is directional (gather_lights() packs those first), over a square
// centred on p_center.
bool rt_caustics_frame(vec3 p_center, out CausticsFrame f) {
	f.L = vec3(0.0, 1.0, 0.0);
	f.r = vec3(1.0, 0.0, 0.0);
	f.u = vec3(0.0, 0.0, 1.0);
	f.origin = vec2(0.0);
	f.size = 1.0;
	f.ref_depth = 0.0;
	if (get_rt_param(RT_PARAM_LIGHT_COUNT) < 0.5 || lights_get_type(0u) != RT_LIGHT_TYPE_DIRECTIONAL) {
		return false;
	}
	f.L = -normalize(lights_get(0u).position);
	vec3 a = abs(f.L.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
	f.r = normalize(cross(a, f.L));
	f.u = cross(f.L, f.r);
	f.size = max(get_rt_param(RT_PARAM_CAUSTICS_SIZE), 0.01);
	vec3 c = p_center;
	float texel = f.size / float(RT_CAUSTICS_RES);
	f.origin = floor(vec2(dot(c, f.r), dot(c, f.u)) / texel) * texel - 0.5 * f.size;
	f.ref_depth = dot(c, f.L);
	return true;
}

vec3 rt_caustics_slot_center(uint p_slot) {
	uint h = RT_CAUSTICS_HEADER + p_slot * 4u;
	return vec3(uintBitsToFloat(rt_caustics.data[h + 1u]), uintBitsToFloat(rt_caustics.data[h + 2u]), uintBitsToFloat(rt_caustics.data[h + 3u]));
}

// The map position of p in texels (centres at .5) and its stored depth.
vec2 rt_caustics_texel(CausticsFrame f, vec3 p) {
	return (vec2(dot(p, f.r), dot(p, f.u)) - f.origin) / f.size * float(RT_CAUSTICS_RES);
}

float rt_caustics_depth(CausticsFrame f, vec3 p) {
	return clamp((dot(p, f.L) - f.ref_depth) * RT_CAUSTICS_DEPTH_SCALE + RT_CAUSTICS_DEPTH_BIAS, 0.0, 2.0 * RT_CAUSTICS_DEPTH_BIAS);
}

// Channel 0: bent photons. Channel 1: straight twins, with their depth sum in channel 2.
void rt_caustics_deposit(CausticsFrame f, uint p_slot, vec3 p, uint p_channel) {
	vec2 st = rt_caustics_texel(f, p) - 0.5;
	ivec2 i0 = ivec2(floor(st));
	vec2 fr = st - vec2(i0);
	uint depth = uint(rt_caustics_depth(f, p));
	for (int k = 0; k < 4; k++) {
		ivec2 o = ivec2(k & 1, k >> 1);
		ivec2 t = i0 + o;
		if (any(lessThan(t, ivec2(0))) || any(greaterThanEqual(t, ivec2(RT_CAUSTICS_RES)))) {
			continue;
		}
		vec2 w2 = mix(1.0 - fr, fr, vec2(o));
		uint w = uint(w2.x * w2.y * RT_CAUSTICS_WEIGHT + 0.5);
		if (w == 0u) {
			continue;
		}
		uint idx = p_slot * RT_CAUSTICS_SLOT_UINTS + (uint(t.y) * RT_CAUSTICS_RES + uint(t.x)) * 3u;
		atomicAdd(rt_caustics.data[idx + p_channel], w);
		if (p_channel == 1u) {
			atomicAdd(rt_caustics.data[idx + 2u], w * depth);
		}
	}
}

// How much the water above p focuses the sun there, or -1 where the map holds nothing for p.
float rt_caustics_lookup(vec3 p) {
	if (!rt_caustics_enabled() || !rt_caustics_history()) {
		return -1.0;
	}
	uint slot = (rt_caustics_write_slot() + 2u) % 3u;
	CausticsFrame f;
	if (!rt_caustics_frame(rt_caustics_slot_center(slot), f)) {
		return -1.0;
	}
	vec2 st = rt_caustics_texel(f, p) - 0.5;
	ivec2 i0 = ivec2(floor(st));
	vec2 fr = st - vec2(i0);
	float bent = 0.0;
	float straight = 0.0;
	float depth_sum = 0.0;
	for (int k = 0; k < 4; k++) {
		ivec2 o = ivec2(k & 1, k >> 1);
		ivec2 t = clamp(i0 + o, ivec2(0), ivec2(RT_CAUSTICS_RES - 1u));
		vec2 w2 = mix(1.0 - fr, fr, vec2(o));
		float w = w2.x * w2.y;
		uint idx = slot * RT_CAUSTICS_SLOT_UINTS + (uint(t.y) * RT_CAUSTICS_RES + uint(t.x)) * 3u;
		bent += w * float(rt_caustics.data[idx]);
		straight += w * float(rt_caustics.data[idx + 1u]);
		depth_sum += w * float(rt_caustics.data[idx + 2u]);
	}
	if (any(lessThan(st, vec2(-0.5))) || any(greaterThan(st, vec2(float(RT_CAUSTICS_RES) - 0.5))) || straight < 0.5 * RT_CAUSTICS_WEIGHT) {
		return -1.0;
	}
	float tolerance = max(RT_CAUSTICS_DEPTH_TOLERANCE, 2.0 * f.size / float(RT_CAUSTICS_RES)) * RT_CAUSTICS_DEPTH_SCALE;
	if (abs(depth_sum / straight - rt_caustics_depth(f, p)) > tolerance) {
		return -1.0;
	}
	return min(bent / straight, RT_CAUSTICS_MAX_RATIO);
}

// The sun's multiplier at p (1: as the straight shadow ray has it).
float rt_caustics_ratio(vec3 p) {
	float ratio = rt_caustics_lookup(p);
	return ratio < 0.0 ? 1.0 : ratio;
}

// Whether the map covers p: the radiance cache would average its lines away there.
bool rt_caustics_covers(vec3 p) {
	return rt_caustics_lookup(p) >= 0.0;
}

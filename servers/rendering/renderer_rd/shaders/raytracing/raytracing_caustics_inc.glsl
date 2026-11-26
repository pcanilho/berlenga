// Water caustics (binding 46): a caustics map for the first directional light, as the ray-guided
// water caustics of Gruen (Ray Tracing Gems, 2019) and Yang and Ouyang (Ray Tracing Gems II,
// ch. 30, 2021), fed to the path tracer's sun light instead of drawn over the screen.
//
// Raygen shoots photons from the sun (one a pixel, more on small frames: RT_CAUSTICS_MIN_PHOTONS) across a square (rendering/pathtracing/caustics_area)
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
// Cascades, as shadow cascades and as the chapter extends it to oceans: square c is 4^c times the
// area's width around the same centre. Cascade 0 (512 texels) takes one photon from every pixel, as
// before; the outer two (256 texels each) take one more from a quarter of the pixels each, which
// keeps about eight photons a texel in all three. A point reads the finest cascade that covers it,
// blended into the next one out over the finer one's border.
//
// Each cascade is three slots: the frame adds to one, reads the one the last frame wrote, and clears
// the one it read last frame (each pixel's thread clears its share), so no separate pass runs.
// After all the slots, a header per slot: the distance the camera looked to when it was written,
// and the squares' centre it used.

#define RT_CAUSTICS_BINDING 46
#define RT_CAUSTICS_RES 512u // cascade 0 texels across, must match RT_CAUSTICS_RES (render_raytracing.h)
#define RT_CAUSTICS_RES_OUTER 256u // the outer cascades, must match RT_CAUSTICS_RES_OUTER
#define RT_CAUSTICS_CASCADES 3u
#define RT_CAUSTICS_CASCADE_SCALE 4.0
#define RT_CAUSTICS_SLOT_UINTS (RT_CAUSTICS_RES * RT_CAUSTICS_RES * 3u)
#define RT_CAUSTICS_SLOT_UINTS_OUTER (RT_CAUSTICS_RES_OUTER * RT_CAUSTICS_RES_OUTER * 3u)
#define RT_CAUSTICS_HEADER (3u * RT_CAUSTICS_SLOT_UINTS + 3u * (RT_CAUSTICS_CASCADES - 1u) * RT_CAUSTICS_SLOT_UINTS_OUTER) // 4 uints a slot: look distance, centre xyz
// The lamp maps (below): three slots after the sun's header, each holding RT_CAUSTICS_LAMPS maps of
// RT_CAUSTICS_LAMP_RES texels, then a header per slot: each lamp's position and how many of its photons entered water.
#define RT_CAUSTICS_LAMPS 4u
#define RT_CAUSTICS_LAMP_RES 256u
#define RT_CAUSTICS_LAMP_MAP_UINTS (RT_CAUSTICS_LAMP_RES * RT_CAUSTICS_LAMP_RES * 3u)
#define RT_CAUSTICS_LAMP_SLOT_UINTS (RT_CAUSTICS_LAMPS * RT_CAUSTICS_LAMP_MAP_UINTS)
#define RT_CAUSTICS_LAMP_BASE (RT_CAUSTICS_HEADER + 12u)
#define RT_CAUSTICS_LAMP_HEADER (RT_CAUSTICS_LAMP_BASE + 3u * RT_CAUSTICS_LAMP_SLOT_UINTS)
#define RT_CAUSTICS_UINTS (RT_CAUSTICS_LAMP_HEADER + 3u * RT_CAUSTICS_LAMPS * 4u) // must match RT_CAUSTICS_UINTS (render_raytracing.h)
#define RT_CAUSTICS_WEIGHT 64.0 // fixed-point steps of one photon
#define RT_CAUSTICS_DEPTH_SCALE 100.0 // depth sum steps per metre
#define RT_CAUSTICS_DEPTH_BIAS 50000.0 // depths are stored from -500 m to 500 m around the area's centre
#define RT_CAUSTICS_DEPTH_TOLERANCE 0.3 // metres a point may lie off the depth its texel's photons landed at
#define RT_CAUSTICS_MAX_RATIO 32.0
#define RT_CAUSTICS_EDGE_FADE 0.08 // share of the map across, at each border, over which caustics fade out
#define RT_CAUSTICS_MIN_PHOTONS 4u // photons a texel a frame each cascade gets at least (raygen)
#define RT_CAUSTICS_MAX_PER_PIXEL 6u // the most photons a pixel sends to one cascade
#define RT_CAUSTICS_MIN_SUN_RADIUS 0.0046 // radians: the sun's angular radius, the least disc photons start from
#define RT_CAUSTICS_SUN_HEIGHT 2000.0 // metres above the area's centre the photons start
#define RT_CAUSTICS_LAMP_SCAN 256u // positional lights looked through for the lamp map
#define RT_CAUSTICS_LAMP_OMNI_TAN 1.7320508 // an omni light's map reaches 60 degrees off straight down
#define RT_CAUSTICS_LAMP_MAX_TAN 5.6712818 // a spot's map at most 80 degrees off its axis
#define RT_CAUSTICS_LAMP_FIXTURE 0.3 // metres round a lamp in which dry surfaces (its fixture) let photons through

// Ray input on non-peel rays: a caustic photon. The hit reports the surface instead of shading
// it: MEDIUM_CHANGE_FLAG for a medium's surface, with next_ray_dir the photon's direction past it
// (bent at a front face; bsdf_pdf 1 when it was), and nothing for any other surface.
const uint CAUSTIC_PHOTON_FLAG = (1u << 18);

layout(set = 0, binding = RT_CAUSTICS_BINDING, std430) coherent buffer CausticsBuffer {
	uint data[];
}
rt_caustics;

// RT_PARAM_CAUSTICS_STATE: 0 off, else 1 + the slot written this frame + 4 when last frame's is valid
// + 8 times (the cascades in use - 1).
bool rt_caustics_enabled() {
	return get_rt_param(RT_PARAM_CAUSTICS_STATE) > 0.5 && uint(rt_caustics.data.length()) >= RT_CAUSTICS_UINTS;
}

uint rt_caustics_write_slot() {
	return (uint(get_rt_param(RT_PARAM_CAUSTICS_STATE)) - 1u) & 3u;
}

bool rt_caustics_history() {
	return ((uint(get_rt_param(RT_PARAM_CAUSTICS_STATE)) - 1u) & 4u) != 0u;
}

uint rt_caustics_cascades() {
	return min(((uint(get_rt_param(RT_PARAM_CAUSTICS_STATE)) - 1u) >> 3u) + 1u, RT_CAUSTICS_CASCADES);
}

// Where slot p_slot of cascade p_cascade starts.
uint rt_caustics_slot_base(uint p_cascade, uint p_slot) {
	if (p_cascade == 0u) {
		return p_slot * RT_CAUSTICS_SLOT_UINTS;
	}
	return 3u * RT_CAUSTICS_SLOT_UINTS + (3u * (p_cascade - 1u) + p_slot) * RT_CAUSTICS_SLOT_UINTS_OUTER;
}

struct CausticsFrame {
	vec3 L; // towards the light
	vec3 r;
	vec3 u;
	vec2 origin; // the area's corner along r and u, snapped to texels so the map holds still
	float size;
	float ref_depth; // the area's centre along L
	uint res; // texels across
	uint cascade;
};

// The first light, when it is directional (gather_lights() packs those first), over cascade
// p_cascade's square centred on p_center.
bool rt_caustics_frame(vec3 p_center, uint p_cascade, out CausticsFrame f) {
	f.res = p_cascade == 0u ? RT_CAUSTICS_RES : RT_CAUSTICS_RES_OUTER;
	f.cascade = p_cascade;
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
	f.size = max(get_rt_param(RT_PARAM_CAUSTICS_SIZE), 0.01) * pow(RT_CAUSTICS_CASCADE_SCALE, float(p_cascade));
	vec3 c = p_center;
	float texel = f.size / float(f.res);
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
	return (vec2(dot(p, f.r), dot(p, f.u)) - f.origin) / f.size * float(f.res);
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
		if (any(lessThan(t, ivec2(0))) || any(greaterThanEqual(t, ivec2(f.res)))) {
			continue;
		}
		vec2 w2 = mix(1.0 - fr, fr, vec2(o));
		uint w = uint(w2.x * w2.y * RT_CAUSTICS_WEIGHT + 0.5);
		if (w == 0u) {
			continue;
		}
		uint idx = rt_caustics_slot_base(f.cascade, p_slot) + (uint(t.y) * f.res + uint(t.x)) * 3u;
		atomicAdd(rt_caustics.data[idx + p_channel], w);
		if (p_channel == 1u) {
			atomicAdd(rt_caustics.data[idx + 2u], w * depth);
		}
	}
}

// How much the water above p focuses the sun there in one cascade, or -1 where it holds nothing for
// p; r_fade is 1 inside the square and falls to 0 over its border.
float rt_caustics_lookup_cascade(vec3 p_center, uint p_cascade, uint p_slot, vec3 p, out float r_fade) {
	r_fade = 0.0;
	CausticsFrame f;
	if (!rt_caustics_frame(p_center, p_cascade, f)) {
		return -1.0;
	}
	vec2 st = rt_caustics_texel(f, p) - 0.5;
	float res = float(f.res);
	if (any(lessThan(st, vec2(-0.5))) || any(greaterThan(st, vec2(res - 0.5)))) {
		return -1.0;
	}
	ivec2 i0 = ivec2(floor(st));
	vec2 fr = st - vec2(i0);
	float bent = 0.0;
	float straight = 0.0;
	float depth_sum = 0.0;
	uint base = rt_caustics_slot_base(p_cascade, p_slot);
	for (int k = 0; k < 4; k++) {
		ivec2 o = ivec2(k & 1, k >> 1);
		ivec2 t = clamp(i0 + o, ivec2(0), ivec2(f.res - 1u));
		vec2 w2 = mix(1.0 - fr, fr, vec2(o));
		float w = w2.x * w2.y;
		uint idx = base + (uint(t.y) * f.res + uint(t.x)) * 3u;
		bent += w * float(rt_caustics.data[idx]);
		straight += w * float(rt_caustics.data[idx + 1u]);
		depth_sum += w * float(rt_caustics.data[idx + 2u]);
	}
	if (straight < 0.5 * RT_CAUSTICS_WEIGHT) {
		return -1.0;
	}
	float tolerance = max(RT_CAUSTICS_DEPTH_TOLERANCE, 2.0 * f.size / res) * RT_CAUSTICS_DEPTH_SCALE;
	if (abs(depth_sum / straight - rt_caustics_depth(f, p)) > tolerance) {
		return -1.0;
	}
	// Near the border photons bent out of the square are lost while none bend in from past it, so
	// the ratio dips into a dark line: hand over to the next cascade (or the plain sun) there.
	vec2 edge = min(st + 0.5, vec2(res - 0.5) - st);
	r_fade = smoothstep(0.0, RT_CAUSTICS_EDGE_FADE * res, min(edge.x, edge.y));
	return min(bent / straight, RT_CAUSTICS_MAX_RATIO);
}

// How much the water above p focuses the sun there, or -1 where the map holds nothing for p.
float rt_caustics_lookup(vec3 p) {
	if (!rt_caustics_enabled() || !rt_caustics_history()) {
		return -1.0;
	}
	uint slot = (rt_caustics_write_slot() + 2u) % 3u;
	vec3 center = rt_caustics_slot_center(slot);
	uint cascades = rt_caustics_cascades();
	// Finest first; a cascade fully inside its square settles it, otherwise its border blends over
	// what the coarser ones give.
	float ratio[RT_CAUSTICS_CASCADES];
	float fade[RT_CAUSTICS_CASCADES];
	uint n = 0u;
	for (uint c = 0u; c < cascades; c++) {
		ratio[n] = rt_caustics_lookup_cascade(center, c, slot, p, fade[n]);
		n++;
		if (ratio[n - 1u] >= 0.0 && fade[n - 1u] >= 1.0) {
			break;
		}
	}
	float result = -1.0;
	for (uint k = n; k > 0u; k--) {
		float r = ratio[k - 1u];
		if (r < 0.0) {
			continue;
		}
		result = mix(result < 0.0 ? 1.0 : result, r, fade[k - 1u]);
	}
	return result;
}

// The sun's multiplier at p (1: as the straight shadow ray has it).
float rt_caustics_ratio(vec3 p) {
	float ratio = rt_caustics_lookup(p);
	return ratio < 0.0 ? 1.0 : ratio;
}

// Whether the map puts lines at p: the radiance cache would average them away there. Dry ground gets
// as many bent photons as straight ones (ratio exactly 1), so the cache serves it as anywhere else.
bool rt_caustics_covers(vec3 p) {
	float ratio = rt_caustics_lookup(p);
	return ratio >= 0.0 && ratio != 1.0;
}


// Lamp caustics (docs/ideas/water.md "Lamps above the water", path tracer): the same bent-over-straight ratio as the
// sun's, for the four omni or spot lights that reach brightest round the area's centre, so a torch or a spot shone into water
// lights the bed through the bent surface: gathered toward the vertical under flat water (a lamp at height a over a
// bed b deep lights it as one at a + b/n would), and into moving lines under waves. The map is the lamp's perspective
// view (texel = the direction from the lamp, over the tangent plane of its axis), depth = the distance from the lamp.
// Photons go out evenly over that plane, each weighted by the lamp's intensity in its direction (the spot's cone) and
// the solid angle of its texel (cos^3 off the axis), so a bent photon landing in another texel brings its own power.
// The shadow ray already crosses the surface straight (Fresnel and absorption): lights_evaluate_positional multiplies
// by the ratio at the point. Four lamps, the brightest by the inverse square of their distance to the area's centre,
// each its own map of RT_CAUSTICS_LAMP_RES texels.

struct CausticsLampFrame {
	vec3 apex;
	vec3 axis;
	vec3 r;
	vec3 u;
	float ext; // tangent of the map's half-angle
};

bool rt_caustics_lamp_frame(RTLightData p_light, out CausticsLampFrame f) {
	f.apex = p_light.position;
	f.axis = vec3(0.0, -1.0, 0.0);
	f.ext = RT_CAUSTICS_LAMP_OMNI_TAN;
	if (p_light.type == RT_LIGHT_TYPE_SPOT) {
		f.axis = normalize(lights_rotate(p_light.orientation, vec3(0.0, 0.0, -1.0)));
		float c = clamp(1.0 - p_light.one_minus_cos_spot, -1.0, 1.0);
		f.ext = c > 0.17364818 ? min(sqrt(max(1.0 - c * c, 0.0)) / c, RT_CAUSTICS_LAMP_MAX_TAN) : RT_CAUSTICS_LAMP_MAX_TAN;
	} else if (p_light.type != RT_LIGHT_TYPE_OMNI) {
		return false;
	}
	vec3 a = abs(f.axis.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
	f.r = normalize(cross(a, f.axis));
	f.u = cross(f.axis, f.r);
	return true;
}

// The lamps chosen for the maps (r_count of them, brightest first): the same for every thread (it reads only the light
// buffer).
uint rt_caustics_pick_lamps(vec3 p_center, out uint r_idx[RT_CAUSTICS_LAMPS]) {
	uint count = uint(get_rt_param(RT_PARAM_LIGHT_COUNT));
	uint first = 0u;
	while (first < count && lights_get_type(first) == RT_LIGHT_TYPE_DIRECTIONAL) {
		first++;
	}
	float half_size = 0.5 * max(get_rt_param(RT_PARAM_CAUSTICS_SIZE), 0.01);
	float score[RT_CAUSTICS_LAMPS];
	for (uint k = 0u; k < RT_CAUSTICS_LAMPS; k++) {
		score[k] = 0.0;
		r_idx[k] = 0xFFFFFFFFu;
	}
	uint n = 0u;
	for (uint i = first; i < min(count, first + RT_CAUSTICS_LAMP_SCAN); i++) {
		RTLightData l = lights_get(i);
		float range = l.max_range_squared > 0.0 ? sqrt(l.max_range_squared) : 1e4;
		float d = distance(l.position, p_center);
		if (d - range > half_size || l.shadow_opacity <= 0.001 || (l.type != RT_LIGHT_TYPE_OMNI && l.type != RT_LIGHT_TYPE_SPOT)) {
			continue;
		}
		float sc = max(l.emission.r, max(l.emission.g, l.emission.b)) / max(d * d, 1.0);
		if (!(sc > score[RT_CAUSTICS_LAMPS - 1u])) {
			continue;
		}
		// Insert, keeping the list sorted brightest first.
		uint k = RT_CAUSTICS_LAMPS - 1u;
		while (k > 0u && sc > score[k - 1u]) {
			score[k] = score[k - 1u];
			r_idx[k] = r_idx[k - 1u];
			k--;
		}
		score[k] = sc;
		r_idx[k] = i;
		n = min(n + 1u, RT_CAUSTICS_LAMPS);
	}
	return n;
}

// Texel position (centres at .5) of p in the lamp's map; false behind the lamp or off the map.
bool rt_caustics_lamp_texel(CausticsLampFrame f, vec3 p, out vec2 r_st) {
	vec3 q = p - f.apex;
	float z = dot(q, f.axis);
	r_st = vec2(-1.0);
	if (z <= 1e-4) {
		return false;
	}
	vec2 t = vec2(dot(q, f.r), dot(q, f.u)) / (z * f.ext);
	r_st = (t * 0.5 + 0.5) * float(RT_CAUSTICS_LAMP_RES);
	return all(greaterThanEqual(t, vec2(-1.0))) && all(lessThanEqual(t, vec2(1.0)));
}

float rt_caustics_lamp_depth(CausticsLampFrame f, vec3 p) {
	return clamp(distance(p, f.apex) * RT_CAUSTICS_DEPTH_SCALE, 0.0, 2.0 * RT_CAUSTICS_DEPTH_BIAS);
}

// Channel 0 bent, 1 straight with its depth sum in 2, as the sun's; p_weight in photons (0..1).
void rt_caustics_lamp_deposit(CausticsLampFrame f, uint p_slot, uint p_lamp, vec3 p, uint p_channel, float p_weight) {
	vec2 st;
	if (!rt_caustics_lamp_texel(f, p, st)) {
		return;
	}
	st -= 0.5;
	ivec2 i0 = ivec2(floor(st));
	vec2 fr = st - vec2(i0);
	uint depth = uint(rt_caustics_lamp_depth(f, p));
	uint base = RT_CAUSTICS_LAMP_BASE + p_slot * RT_CAUSTICS_LAMP_SLOT_UINTS + p_lamp * RT_CAUSTICS_LAMP_MAP_UINTS;
	for (int k = 0; k < 4; k++) {
		ivec2 o = ivec2(k & 1, k >> 1);
		ivec2 t = i0 + o;
		if (any(lessThan(t, ivec2(0))) || any(greaterThanEqual(t, ivec2(RT_CAUSTICS_LAMP_RES)))) {
			continue;
		}
		vec2 w2 = mix(1.0 - fr, fr, vec2(o));
		uint w = uint(w2.x * w2.y * p_weight * RT_CAUSTICS_WEIGHT + 0.5);
		if (w == 0u) {
			continue;
		}
		uint idx = base + (uint(t.y) * RT_CAUSTICS_LAMP_RES + uint(t.x)) * 3u;
		atomicAdd(rt_caustics.data[idx + p_channel], w);
		if (p_channel == 1u) {
			atomicAdd(rt_caustics.data[idx + 2u], w * depth);
		}
	}
}

// Lamp p_light's multiplier at p (1: as the straight shadow ray has it).
float rt_caustics_lamp_ratio(RTLightData p_light, vec3 p) {
	if (!rt_caustics_enabled() || !rt_caustics_history()) {
		return 1.0;
	}
	uint slot = (rt_caustics_write_slot() + 2u) % 3u;
	uint lamp = RT_CAUSTICS_LAMPS;
	for (uint k = 0u; k < RT_CAUSTICS_LAMPS; k++) {
		uint h = RT_CAUSTICS_LAMP_HEADER + (slot * RT_CAUSTICS_LAMPS + k) * 4u;
		vec3 lp = vec3(uintBitsToFloat(rt_caustics.data[h]), uintBitsToFloat(rt_caustics.data[h + 1u]), uintBitsToFloat(rt_caustics.data[h + 2u]));
		if (all(equal(lp, p_light.position))) {
			// None of its photons entered water: its light is as the straight shadow ray has it.
			lamp = rt_caustics.data[h + 3u] == 0u ? RT_CAUSTICS_LAMPS : k;
			break;
		}
	}
	if (lamp == RT_CAUSTICS_LAMPS) {
		return 1.0;
	}
	CausticsLampFrame f;
	vec2 st;
	if (!rt_caustics_lamp_frame(p_light, f) || !rt_caustics_lamp_texel(f, p, st)) {
		return 1.0;
	}
	st -= 0.5;
	ivec2 i0 = ivec2(floor(st));
	vec2 fr = st - vec2(i0);
	float bent = 0.0;
	float straight = 0.0;
	float depth_sum = 0.0;
	uint base = RT_CAUSTICS_LAMP_BASE + slot * RT_CAUSTICS_LAMP_SLOT_UINTS + lamp * RT_CAUSTICS_LAMP_MAP_UINTS;
	for (int k = 0; k < 4; k++) {
		ivec2 o = ivec2(k & 1, k >> 1);
		ivec2 t = clamp(i0 + o, ivec2(0), ivec2(RT_CAUSTICS_LAMP_RES - 1u));
		vec2 w2 = mix(1.0 - fr, fr, vec2(o));
		float w = w2.x * w2.y;
		uint idx = base + (uint(t.y) * RT_CAUSTICS_LAMP_RES + uint(t.x)) * 3u;
		bent += w * float(rt_caustics.data[idx]);
		straight += w * float(rt_caustics.data[idx + 1u]);
		depth_sum += w * float(rt_caustics.data[idx + 2u]);
	}
	if (straight < 0.5 * RT_CAUSTICS_WEIGHT) {
		return 1.0;
	}
	float dist = distance(p, f.apex);
	// A texel's width at that distance, as the sun's tolerance.
	float texel = 2.0 * f.ext * dist / float(RT_CAUSTICS_LAMP_RES);
	float tolerance = max(RT_CAUSTICS_DEPTH_TOLERANCE, 2.0 * texel) * RT_CAUSTICS_DEPTH_SCALE;
	if (abs(depth_sum / straight - rt_caustics_lamp_depth(f, p)) > tolerance) {
		return 1.0;
	}
	float res = float(RT_CAUSTICS_LAMP_RES);
	vec2 edge = min(st + 0.5, vec2(res - 0.5) - st);
	float fade = smoothstep(0.0, RT_CAUSTICS_EDGE_FADE * res, min(edge.x, edge.y));
	return mix(1.0, min(bent / straight, RT_CAUSTICS_MAX_RATIO), fade);
}

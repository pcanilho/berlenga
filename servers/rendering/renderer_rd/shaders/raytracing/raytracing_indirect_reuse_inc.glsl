// Indirect light reuse across pixels and frames: ReSTIR GI (Ouyang et al. 2021), with
// generalized RIS weights (Lin et al. 2022). See docs/ideas/indirect-reuse.md.
//
// Where a pixel's camera ray first hits a surface and takes the diffuse lobe, the first bounce
// is kept as a sample: the point it reached (x2), that point's normal and the radiance that
// came back. Raygen resamples the pixel's samples' first bounces with last frame's reservoir
// for the same surface and two neighbours', reconnecting this surface to their x2, and shades
// the diffuse lobe of every sample that took it with the sample chosen. Weights are taken in
// area measure at x2, where the reconnection shift needs no Jacobian, with the balance heuristic
// over the surfaces' targets. A sample that reached nothing (the sky) is a direction, in solid
// angle.
//
// Binding 52: header (flags, unused, width, height), two frames of reservoirs of
// RT_IR_STRIDE uints, then this frame's surface records of RT_IR_RECORD uints.
//   Reservoir: x1 (relative to the anchor), x1's normal, direction to x2, distance to x2
//   (0: sky), x2's normal, radiance (half rgb), M and flags, W.
//   Record (closest hit -> raygen): the diffuse lobe's weight before its cosine and Burley
//   factor (half rgb) and roughness, shading normal, geometric normal, where the surface
//   was last frame (half offset) and the diffuse model.

#define RT_IR_BINDING 52
layout(set = 0, binding = RT_IR_BINDING, std430) restrict buffer IndirectReuseBuffer {
	uint data[];
}
rt_ir;

#define RT_IR_HEADER 4u // must match RT_INDIRECT_REUSE_HEADER (render_raytracing.h)
#define RT_IR_STRIDE 10u // must match RT_INDIRECT_REUSE_STRIDE
#define RT_IR_RECORD 6u // must match RT_INDIRECT_REUSE_RECORD
#define RT_IR_M_CAP 20u // frames of history
#define RT_IR_NEIGHBOURS 2u
#define RT_IR_RADIUS 16.0 // pixels
#define RT_IR_NORMAL 0.9 // cosine a neighbour's normal must reach
#define RT_IR_PLANE 0.05 // distance off this surface's plane, over the distance to the camera
#define RT_IR_MIN_ROUGHNESS 0.4 // an x2 shinier than this is reused only by its own surface

// Hit outputs on the main path (bits 22-23 are free outside peel rays).
#define RT_IR_SURFACE_FLAG (1u << 22) // bounce 0: the surface record was written
#define RT_IR_DIFFUSE_FLAG (1u << 23) // bounce 0: the diffuse lobe was taken; bounce 1: x2 is rough

#define RT_IR_ROUGH_BIT (1u << 24) // reservoir flags word

bool rt_ir_enabled() {
	if (get_rt_param(RT_PARAM_INDIRECT_REUSE) < 0.5) {
		return false;
	}
	uint pixels = gl_LaunchSizeEXT.x * gl_LaunchSizeEXT.y;
	return rt_ir.data[2] == gl_LaunchSizeEXT.x && rt_ir.data[3] == gl_LaunchSizeEXT.y &&
			uint(rt_ir.data.length()) >= RT_IR_HEADER + pixels * (2u * RT_IR_STRIDE + RT_IR_RECORD);
}

uint rt_ir_state() {
	return uint(get_rt_param(RT_PARAM_INDIRECT_REUSE)) - 1u; // bit 0: slot written, bit 1: last frame's valid
}

uint rt_ir_record_base(ivec2 p_pixel) {
	uint pixels = gl_LaunchSizeEXT.x * gl_LaunchSizeEXT.y;
	return RT_IR_HEADER + 2u * pixels * RT_IR_STRIDE + (uint(p_pixel.y) * gl_LaunchSizeEXT.x + uint(p_pixel.x)) * RT_IR_RECORD;
}

uint rt_ir_pack_dir(vec3 v) {
	return packUnorm2x16(vec3_to_oct(normalize(v))); // vec3_to_oct() gives 0..1
}

vec3 rt_ir_unpack_dir(uint p) {
	return oct_to_vec3(unpackUnorm2x16(p) * 2.0 - 1.0);
}

// The cosine the path tracer's diffuse sampling weighs a direction by (pi times its density):
// cosine-weighted about the shading normal, with samples below the geometric plane mirrored
// across it (recoverBelowHemisphereSample()), so a direction is reached two ways.
float rt_ir_cosine(vec3 N, vec3 Ng, vec3 L) {
	float g = dot(Ng, L);
	if (g <= 0.0) {
		return 0.0;
	}
	float c = max(dot(N, L), 0.0);
#if RT_BELOW_HEMISPHERE_RECOVERY_ENABLED
	c += max(dot(N, L - 2.0 * g * Ng), 0.0);
#endif
	return c;
}

// Burley's retro-reflection factor, as godotDiffuseSampleWeight() (1 for Lambert).
float rt_ir_burley(vec3 N, vec3 V, vec3 L, float p_roughness) {
	vec3 H = normalize(V + L);
	float LdotH = clamp(dot(L, H), 0.0, 1.0);
	float NdotL = clamp(dot(N, L), 0.00001, 1.0);
	float NdotV = clamp(dot(N, V), 0.00001, 1.0);
	float fd90_minus_one = 2.0 * p_roughness * LdotH * LdotH - 0.5;
	return (1.0 + fd90_minus_one * pow(1.0 - NdotL, 5.0)) * (1.0 + fd90_minus_one * pow(1.0 - NdotV, 5.0));
}

#if defined(RT_STAGE_CLOSEST_HIT)
// Set by sample_surface_continuation() when it takes the diffuse lobe of a plain surface.
bool rt_ir_diffuse_taken = false;
vec3 rt_ir_weight = vec3(0.0); // the throughput before the diffuse weight, times the diffuse reflectance

// The camera ray's first hit (the same for every sample): the surface record raygen reuses with.
void rt_ir_write_record(vec3 p_hit_pos, vec3 p_prev_pos, vec3 N, vec3 Ng, float p_roughness, bool p_burley) {
	uint b = rt_ir_record_base(ivec2(gl_LaunchIDEXT.xy));
	vec3 w = rt_ir_diffuse_taken ? rt_ir_weight : vec3(0.0);
	vec3 d = p_prev_pos - p_hit_pos;
	rt_ir.data[b] = packHalf2x16(w.rg);
	rt_ir.data[b + 1u] = packHalf2x16(vec2(w.b, p_roughness));
	rt_ir.data[b + 2u] = rt_ir_pack_dir(N);
	rt_ir.data[b + 3u] = rt_ir_pack_dir(Ng);
	rt_ir.data[b + 4u] = packHalf2x16(d.xy);
	rt_ir.data[b + 5u] = packHalf2x16(vec2(d.z, p_burley ? 1.0 : 0.0));
}
#endif

#if defined(RT_STAGE_RAYGEN)
struct IRReservoir {
	vec3 x1; // the surface it belongs to
	vec3 n1;
	vec3 dir; // from x1 to x2
	float dist; // to x2; 0: the sky (dir is the sample)
	vec3 n2;
	vec3 L; // radiance from x2 towards x1
	uint M;
	bool rough; // x2 may be reconnected to other surfaces
	float W; // contribution weight, area measure at x2 (solid angle for the sky)
};

uint rt_ir_base(uint p_slot, ivec2 p_pixel) {
	uint pixels = gl_LaunchSizeEXT.x * gl_LaunchSizeEXT.y;
	return RT_IR_HEADER + (p_slot * pixels + uint(p_pixel.y) * gl_LaunchSizeEXT.x + uint(p_pixel.x)) * RT_IR_STRIDE;
}

IRReservoir rt_ir_load(uint p_slot, ivec2 p_pixel) {
	uint b = rt_ir_base(p_slot, p_pixel);
	IRReservoir r;
	r.x1 = uintBitsToFloat(uvec3(rt_ir.data[b], rt_ir.data[b + 1u], rt_ir.data[b + 2u]));
	r.n1 = rt_ir_unpack_dir(rt_ir.data[b + 3u]);
	r.dir = rt_ir_unpack_dir(rt_ir.data[b + 4u]);
	r.dist = uintBitsToFloat(rt_ir.data[b + 5u]);
	r.n2 = rt_ir_unpack_dir(rt_ir.data[b + 6u]);
	uint w8 = rt_ir.data[b + 8u];
	r.L = vec3(unpackHalf2x16(rt_ir.data[b + 7u]), unpackHalf2x16(w8 & 0xFFFFu).x);
	r.M = (w8 >> 16u) & 0xFFu;
	r.rough = (w8 & RT_IR_ROUGH_BIT) != 0u;
	r.W = uintBitsToFloat(rt_ir.data[b + 9u]);
	return r;
}

void rt_ir_store(uint p_slot, ivec2 p_pixel, IRReservoir r) {
	uint b = rt_ir_base(p_slot, p_pixel);
	bool has = r.W > 0.0 && !isinf(r.W) && !isnan(r.W) && all(lessThan(r.L, vec3(65000.0)));
	rt_ir.data[b] = floatBitsToUint(r.x1.x);
	rt_ir.data[b + 1u] = floatBitsToUint(r.x1.y);
	rt_ir.data[b + 2u] = floatBitsToUint(r.x1.z);
	rt_ir.data[b + 3u] = rt_ir_pack_dir(r.n1);
	rt_ir.data[b + 4u] = rt_ir_pack_dir(r.dir);
	rt_ir.data[b + 5u] = floatBitsToUint(r.dist);
	rt_ir.data[b + 6u] = rt_ir_pack_dir(r.n2);
	vec3 L = has ? r.L : vec3(0.0);
	rt_ir.data[b + 7u] = packHalf2x16(L.rg);
	rt_ir.data[b + 8u] = (packHalf2x16(vec2(L.b, 0.0)) & 0xFFFFu) | (min(r.M, 255u) << 16u) | (r.rough ? RT_IR_ROUGH_BIT : 0u);
	rt_ir.data[b + 9u] = floatBitsToUint(has ? r.W : 0.0);
}

// Last frame's reservoir at p_pixel, if its surface matches this one (seen where it was last frame).
bool rt_ir_load_prev(ivec2 p_pixel, vec3 p_prev_pos, vec3 N, float p_eye_dist, out IRReservoir r) {
	if (any(lessThan(p_pixel, ivec2(0))) || any(greaterThanEqual(p_pixel, ivec2(gl_LaunchSizeEXT.xy)))) {
		return false;
	}
	r = rt_ir_load(1u - (rt_ir_state() & 1u), p_pixel);
	return r.M > 0u && dot(r.n1, N) >= RT_IR_NORMAL && abs(dot(N, r.x1 - p_prev_pos)) <= RT_IR_PLANE * p_eye_dist;
}

// The target of sample r at a surface (x, n, ng), in area measure at x2 (solid angle for the sky):
// the radiance's luminance times the diffuse sampling cosine. r_dir and r_dist: x to x2.
float rt_ir_target(IRReservoir r, vec3 x, vec3 n, vec3 ng, out vec3 r_dir, out float r_dist) {
	float lum = luminance(r.L);
	if (r.dist <= 0.0) {
		r_dir = r.dir;
		r_dist = 0.0;
		return lum * rt_ir_cosine(n, ng, r.dir);
	}
	vec3 x2 = r.x1 + r.dir * r.dist;
	vec3 d = x2 - x;
	float d2 = dot(d, d);
	if (d2 < 1e-8) {
		r_dir = r.dir;
		r_dist = 0.0;
		return 0.0;
	}
	r_dist = sqrt(d2);
	r_dir = d / r_dist;
	return lum * rt_ir_cosine(n, ng, r_dir) * abs(dot(r.n2, r_dir)) / d2;
}

// The first bounce of each sample whose camera ray's first hit took the diffuse lobe, as
// candidates for this frame's reservoir (resampled importance sampling, Talbot et al. 2005).
struct IRFresh {
	IRReservoir r; // the candidate chosen
	float w_sum;
	float t; // its target
	uint k; // candidates
};

IRFresh rt_ir_fresh_init() {
	IRFresh f;
	f.r.x1 = vec3(0.0);
	f.r.n1 = vec3(0.0, 0.0, 1.0);
	f.r.dir = vec3(0.0, 0.0, 1.0);
	f.r.dist = 0.0;
	f.r.n2 = vec3(0.0, 0.0, 1.0);
	f.r.L = vec3(0.0);
	f.r.M = 1u;
	f.r.rough = false;
	f.r.W = 0.0;
	f.w_sum = 0.0;
	f.t = 0.0;
	f.k = 0u;
	return f;
}

// One candidate: the bounce along p_dir from x1, which reached x2 (p_has_x2) or the sky, and the
// radiance p_L that came back. p_traced false: the path ended before the bounce (L = 0).
void rt_ir_fresh_add(inout IRFresh f, vec3 x1, bool p_traced, vec3 p_dir, bool p_has_x2, vec3 x2, vec3 n2, bool p_rough, vec3 p_L, inout uint rng) {
	f.k++;
	if (!p_traced || luminance(p_L) <= 0.0) {
		return;
	}
	uint b = rt_ir_record_base(ivec2(gl_LaunchIDEXT.xy));
	vec3 N = rt_ir_unpack_dir(rt_ir.data[b + 2u]);
	vec3 Ng = rt_ir_unpack_dir(rt_ir.data[b + 3u]);
	IRReservoir c;
	c.x1 = x1;
	c.n1 = N;
	c.M = 1u;
	c.rough = p_rough;
	c.L = p_L;
	c.n2 = p_has_x2 ? n2 : N;
	c.dir = p_dir;
	c.dist = 0.0;
	c.W = 0.0;
	// The path's density for the direction, in area measure at x2 when it reached a surface.
	float pdf = rt_ir_cosine(N, Ng, p_dir) / PI;
	if (!(pdf > 0.0)) {
		return;
	}
	float inv_pdf = 1.0 / pdf;
	if (p_has_x2) {
		vec3 d = x2 - x1;
		float d2 = dot(d, d);
		if (d2 <= 1e-8) {
			return;
		}
		c.dist = sqrt(d2);
		c.dir = d / c.dist;
		float c2 = abs(dot(n2, c.dir));
		if (c2 <= 1e-4) {
			return;
		}
		inv_pdf = d2 / (pdf * c2);
	}
	vec3 dir_q;
	float dist_q;
	float t = rt_ir_target(c, x1, N, Ng, dir_q, dist_q);
	float w = t * inv_pdf;
	if (!(w > 0.0) || isinf(w)) {
		return;
	}
	f.w_sum += w;
	if (rand(rng) * f.w_sum < w) {
		f.r = c;
		f.t = t;
	}
}

// After the pixel's samples: resample this frame's candidates with last frame's reservoirs, store
// this frame's, and return the diffuse lobe's light to use in place of each diffuse sample's own.
// p_active: some sample took the diffuse lobe (else only the history is carried).
vec3 rt_ir_resolve(bool p_active, vec3 x1, vec3 V, float p_eye_dist, IRFresh p_fresh, inout uint rng) {
	ivec2 pixel = ivec2(gl_LaunchIDEXT.xy);
	uint state = rt_ir_state();
	uint b = rt_ir_record_base(pixel);
	vec2 w_rg = unpackHalf2x16(rt_ir.data[b]);
	vec2 w_b_rough = unpackHalf2x16(rt_ir.data[b + 1u]);
	vec3 weight = vec3(w_rg, w_b_rough.x);
	float roughness = w_b_rough.y;
	vec3 N = rt_ir_unpack_dir(rt_ir.data[b + 2u]);
	vec3 Ng = rt_ir_unpack_dir(rt_ir.data[b + 3u]);
	vec2 d_xy = unpackHalf2x16(rt_ir.data[b + 4u]);
	vec2 d_z_burley = unpackHalf2x16(rt_ir.data[b + 5u]);
	vec3 prev_pos = x1 + vec3(d_xy, d_z_burley.x);
	bool burley = d_z_burley.y > 0.5;
	vec2 prev_px = vec2(pixel) + 0.5 + rt_motion_vector(x1, prev_pos) * vec2(gl_LaunchSizeEXT.xy);
	bool history = (state & 2u) != 0u;

	if (!p_active) {
		// Not shaded through the reservoirs this frame: carry last frame's forward unchanged.
		IRReservoir r;
		if (!history || !rt_ir_load_prev(ivec2(floor(prev_px)), prev_pos, N, p_eye_dist, r)) {
			r.x1 = x1;
			r.n1 = N;
			r.dir = N;
			r.dist = 0.0;
			r.n2 = N;
			r.L = vec3(0.0);
			r.M = 0u;
			r.rough = false;
			r.W = 0.0;
		}
		rt_ir_store(state & 1u, pixel, r);
		return vec3(0.0);
	}

	IRReservoir res[2u + RT_IR_NEIGHBOURS];
	uint count = 0u;

	// This frame's sample: the path's own first bounce, one candidate drawn with the path's density.
	IRReservoir fresh = p_fresh.r;
	fresh.x1 = x1;
	fresh.n1 = N;
	fresh.M = 1u; // however many candidates built it (RTXDI)
	fresh.W = p_fresh.w_sum > 0.0 ? p_fresh.w_sum / (float(p_fresh.k) * p_fresh.t) : 0.0;
	res[count++] = fresh;

	// Last frame's: this surface's own reservoir, then neighbours' whose x2 is rough.
	if (history) {
		IRReservoir r;
		if (rt_ir_load_prev(ivec2(floor(prev_px)), prev_pos, N, p_eye_dist, r)) {
			r.M = min(r.M, RT_IR_M_CAP);
			res[count++] = r;
		}
		for (uint k = 0u; k < RT_IR_NEIGHBOURS; k++) {
			vec2 u = vec2(rand(rng), rand(rng));
			float rad = RT_IR_RADIUS * sqrt(u.x);
			vec2 off = rad * vec2(cos(6.2831853 * u.y), sin(6.2831853 * u.y));
			if (dot(off, off) < 1.0) {
				continue;
			}
			if (rt_ir_load_prev(ivec2(floor(prev_px + off)), prev_pos, N, p_eye_dist, r) && (r.rough || r.dist <= 0.0)) {
				r.M = min(r.M, RT_IR_M_CAP);
				res[count++] = r;
			}
		}
	}

	// Resample: each sample weighted by its target here times its W, times the balance heuristic
	// over every surface's target for it (generalized RIS), all in area measure at x2.
	float w_sum = 0.0;
	uint m_sum = 0u;
	uint chosen = ~0u;
	float chosen_target = 0.0;
	vec3 chosen_dir = vec3(0.0);
	float chosen_dist = 0.0;
	for (uint i = 0u; i < count; i++) {
		m_sum += res[i].M;
		if (!(res[i].W > 0.0) || luminance(res[i].L) <= 0.0) {
			continue;
		}
		vec3 dir_q;
		float dist_q;
		float t_here = rt_ir_target(res[i], x1, N, Ng, dir_q, dist_q);
		if (!(t_here > 0.0)) {
			continue;
		}
		float own = 0.0;
		float total = 0.0;
		for (uint j = 0u; j < count; j++) {
			vec3 dj;
			float sj;
			float t = j == 0u ? t_here : rt_ir_target(res[i], res[j].x1, res[j].n1, res[j].n1, dj, sj);
			total += float(res[j].M) * t;
			if (j == i) {
				own = float(res[j].M) * t;
			}
		}
		float w = total > 0.0 ? own / total * t_here * res[i].W : 0.0;
		w_sum += w;
		if (rand(rng) * w_sum < w) {
			chosen = i;
			chosen_target = t_here;
			chosen_dir = dir_q;
			chosen_dist = dist_q;
		}
	}

	IRReservoir out_r = fresh;
	out_r.M = min(m_sum, RT_IR_M_CAP);
	out_r.W = 0.0;
	vec3 result = vec3(0.0);
	if (chosen != ~0u && w_sum > 0.0) {
		IRReservoir c = res[chosen];
		float W = w_sum / chosen_target;
		vec3 visible = vec3(1.0);
		if (chosen != 0u) {
			// Another surface's sample: check that this one sees its point.
			vec3 o = offset_ray_origin(x1, Ng, p_eye_dist);
			float reach = c.dist > 0.0 ? max(chosen_dist * 0.999 - 0.01, 0.001) : 10000.0;
			visible = lights_trace_shadow_ray(o, chosen_dir, reach, RT_BOUNCE_MASK, rng);
		}
		float lum_vis = luminance(visible);
		// The stored sample keeps x2 and its light; x1 becomes this surface.
		out_r.L = c.L;
		out_r.n2 = c.n2;
		out_r.rough = c.rough;
		out_r.dir = chosen_dir;
		out_r.dist = c.dist > 0.0 ? chosen_dist : 0.0;
		out_r.W = lum_vis > 0.0 ? W : 0.0; // a blocked sample is dropped (RTXDI's visibility bias correction)
		if (lum_vis > 0.0) {
			float g = burley ? rt_ir_burley(N, V, chosen_dir, roughness) : 1.0;
			float geo = c.dist > 0.0 ? abs(dot(c.n2, chosen_dir)) / (chosen_dist * chosen_dist) : 1.0;
			result = weight * (g / PI) * rt_ir_cosine(N, Ng, chosen_dir) * geo * c.L * W * visible;
		}
	}
	out_r.x1 = x1;
	out_r.n1 = N;
	rt_ir_store(state & 1u, pixel, out_r);
	return result;
}
#endif

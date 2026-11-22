#[compute]
#version 450

#include "water_inc.glsl"

// Local water layer: the shallow-water equations on a staggered grid, after Chentanez and Müller,
// "Real-time Simulation of Large Bodies of Water with Small Scale Details" (SCA 2010), carrying
// the baked current (docs/ideas/water.md, step 3c). It used to be an iWave-style damped wave
// equation; for small waves on still water the two are the same water (the shallow-water
// equations linearise to the wave equation with c^2 = g * depth), but this one also moves water:
// the current runs through it, rocks shed wakes and eddies, and floaters read its velocity.
//
// Per cell: x = surface height above still water (eta), y = u' on the cell's +x face, z = foam
// (carried), w = w' on its +z face. u' and w' are the live part of the velocity: the total is the
// baked flow (base_flux_tex, exactly divergence-free face fluxes from the stream function) plus
// them, so a cleared grid starts as the baked flow, and with nothing disturbing it the height
// stays flat. The faces on the grid's edge keep u' = 0: the baked flow goes in and out there.
//
// Two passes a tick (res_count.w), ping-ponged by the caller (A -> B -> A):
//   0: height from the face fluxes (their eq. 4, upwind face depth, depth capped for stability),
//      bodies' soft targets, and the cell-centre velocity image;
//   1: face velocities: semi-Lagrangian self-advection with clamped Catmull-Rom interpolation
//      (low diffusion, so eddies survive; the paper uses modified MacCormack, which needs one
//      more pass), the gravity term from the new heights (their eq. 8), wall friction along dry
//      ground, damping of the live part, and the |v| <= 0.5 dx / dt clamp.
//
// Written flat with small helpers on purpose (see this module's history with a heavily-inlined
// helper hanging one driver's compiler).

layout(local_size_x = 8, local_size_y = 8) in;

// Baked flow as face fluxes (m^2/s, depth times velocity): x +x face, y +z face, z -x face,
// w -z face (only the grid's own edge faces need the last two). Zero = still water.
layout(set = 0, binding = 7) uniform sampler2D base_flux_tex;
// Total velocity at cell centres (m/s, xy), for materials and debugging.
layout(set = 0, binding = 8, rgba16f) uniform restrict writeonly image2D velocity_image;

#define PASS (res_count.w)
#define WALL_FRICTION (ripple_control.y)
#define FLOW_FOAM (ripple_control.z)
#define FLOW_FOAM_PERSISTENCE (ripple_control.w)
#define MIN_FLOW_DEPTH 0.25 // m: the depth face fluxes divide by, at least (as the bake)

float still_depth(ivec2 c) {
	return max(cell_depth(c), 0.0);
}

// Still-water depth a face's flux divides by.
float face_depth(ivec2 a, ivec2 b) {
	return max(0.5 * (still_depth(a) + still_depth(b)), MIN_FLOW_DEPTH);
}

// Total velocity on the +x face of cell f (0 where either side is dry). f clamped into the grid.
float face_u(ivec2 f) {
	f = clamp(f, ivec2(0), ivec2(GRID_N - 1));
	ivec2 b = ivec2(min(f.x + 1, GRID_N - 1), f.y);
	if (!is_wet(f) || !is_wet(b)) {
		return 0.0;
	}
	return texelFetch(base_flux_tex, f, 0).x / face_depth(f, b) + state_in[grid_index(f)].y;
}

// Total velocity on the +z face of cell f.
float face_w(ivec2 f) {
	f = clamp(f, ivec2(0), ivec2(GRID_N - 1));
	ivec2 b = ivec2(f.x, min(f.y + 1, GRID_N - 1));
	if (!is_wet(f) || !is_wet(b)) {
		return 0.0;
	}
	return texelFetch(base_flux_tex, f, 0).y / face_depth(f, b) + state_in[grid_index(f)].w;
}

float catmull_rom(float p0, float p1, float p2, float p3, float t) {
	float t2 = t * t;
	float t3 = t2 * t;
	return 0.5 * (2.0 * p1 + (p2 - p0) * t + (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t2 + (3.0 * p1 - p0 - 3.0 * p2 + p3) * t3);
}

// The x-face velocity field at p (grid units, cell (i,j) spans [i, i+1] x [j, j+1]; its +x face
// sits at (i + 1, j + 0.5)), Catmull-Rom clamped to the four nearest values so it adds no new
// extremes (Fedkiw, Stam and Jensen 2001's monotone cubic, simplified).
float sample_u(vec2 p) {
	vec2 g = p - vec2(1.0, 0.5);
	ivec2 i0 = ivec2(floor(g));
	vec2 t = g - vec2(i0);
	float rows[4];
	float lo = 1e30;
	float hi = -1e30;
	for (int r = 0; r < 4; r++) {
		int y = i0.y - 1 + r;
		float v0 = face_u(ivec2(i0.x - 1, y));
		float v1 = face_u(ivec2(i0.x, y));
		float v2 = face_u(ivec2(i0.x + 1, y));
		float v3 = face_u(ivec2(i0.x + 2, y));
		if (r == 1 || r == 2) {
			lo = min(lo, min(v1, v2));
			hi = max(hi, max(v1, v2));
		}
		rows[r] = catmull_rom(v0, v1, v2, v3, t.x);
	}
	return clamp(catmull_rom(rows[0], rows[1], rows[2], rows[3], t.y), lo, hi);
}

float sample_w(vec2 p) {
	vec2 g = p - vec2(0.5, 1.0);
	ivec2 i0 = ivec2(floor(g));
	vec2 t = g - vec2(i0);
	float rows[4];
	float lo = 1e30;
	float hi = -1e30;
	for (int r = 0; r < 4; r++) {
		int y = i0.y - 1 + r;
		float v0 = face_w(ivec2(i0.x - 1, y));
		float v1 = face_w(ivec2(i0.x, y));
		float v2 = face_w(ivec2(i0.x + 1, y));
		float v3 = face_w(ivec2(i0.x + 2, y));
		if (r == 1 || r == 2) {
			lo = min(lo, min(v1, v2));
			hi = max(hi, max(v1, v2));
		}
		rows[r] = catmull_rom(v0, v1, v2, v3, t.x);
	}
	return clamp(catmull_rom(rows[0], rows[1], rows[2], rows[3], t.y), lo, hi);
}

float eta_at(ivec2 c);

// Total velocity at a cell's centre (faces averaged; the grid's own edge reads the nearest face).
vec2 centre_velocity(ivec2 c) {
	c = clamp(c, ivec2(0), ivec2(GRID_N - 1));
	if (!is_wet(c)) {
		return vec2(0.0);
	}
	float r = face_u(c);
	float t = face_w(c);
	float l = c.x > 0 ? face_u(c - ivec2(1, 0)) : r;
	float b = c.y > 0 ? face_w(c - ivec2(0, 1)) : t;
	return 0.5 * vec2(r + l, t + b);
}

// Foam carried on the water (state z), bilinear between cell centres; dry cells hold none.
float foam_sample(vec2 p) {
	vec2 g = p - 0.5;
	ivec2 i0 = ivec2(floor(g));
	vec2 t = g - vec2(i0);
	float f00 = state_in[grid_index(clamp(i0, ivec2(0), ivec2(GRID_N - 1)))].z;
	float f10 = state_in[grid_index(clamp(i0 + ivec2(1, 0), ivec2(0), ivec2(GRID_N - 1)))].z;
	float f01 = state_in[grid_index(clamp(i0 + ivec2(0, 1), ivec2(0), ivec2(GRID_N - 1)))].z;
	float f11 = state_in[grid_index(clamp(i0 + ivec2(1, 1), ivec2(0), ivec2(GRID_N - 1)))].z;
	return mix(mix(f00, f10, t.x), mix(f01, f11, t.x), t.y);
}

// White water the flow makes this tick, after Ihmsen et al. 2012's whitewater potentials, on the
// grid: air is trapped where neighbouring water moves past each other (shear: wakes, the edges of
// a jet, eddies), and where the flow is supercritical (Froude number past about 1: chutes,
// rapids, hydraulic jumps), gated by the water's own speed so slow water never foams.
float flow_foam_source(ivec2 c, vec2 v) {
	float shear = 0.0;
	shear += length(v - centre_velocity(c + ivec2(1, 0)));
	shear += length(v - centre_velocity(c - ivec2(1, 0)));
	shear += length(v - centre_velocity(c + ivec2(0, 1)));
	shear += length(v - centre_velocity(c - ivec2(0, 1)));
	shear *= 0.25 * 0.25 / CELL; // mean m/s change over 25 cm, so the cell size doesn't matter
	float speed = length(v);
	float froude = speed / sqrt(GRAV * max(still_depth(c) + eta_at(c), 0.02));
	float trapped_air = smoothstep(0.15, 0.6, shear);
	float supercritical = smoothstep(0.8, 1.6, froude);
	float energy = smoothstep(0.5, 1.5, speed);
	return energy * max(trapped_air, supercritical);
}

float eta_at(ivec2 c) {
	return state_in[grid_index(c)].x;
}

// Depth the live part of a face's flux may carry: the explicit gravity-wave scheme is stable
// while sqrt(g * h) * dt / dx stays under about 0.67 (2D, staggered); deeper water is treated
// as this deep (its waves run slower than real ones, as the old wave equation's clamp did).
// Chentanez and Müller cap the face depth in the same place (their h_avgmax).
float depth_cap() {
	return 0.45 * CELL * CELL / (GRAV * DT * DT);
}

// Flux (m^2/s) through the face between wet cells a (upstream side for positive velocity) and b,
// with base flux q and live velocity v: q + eta_upwind * q / d + min(depth_upwind, cap) * v.
float face_flux(ivec2 a, ivec2 b, float q, float v) {
	float d = face_depth(a, b);
	float total = q / d + v;
	ivec2 up = total >= 0.0 ? a : b;
	float eta_up = eta_at(up);
	float depth_up = max(still_depth(up) + eta_up, 0.0);
	return q + eta_up * (q / d) + min(depth_up, depth_cap()) * v;
}

void height_pass(ivec2 c, int idx) {
	vec4 st = state_in[idx];
	float eta = st.x;
	vec4 q = texelFetch(base_flux_tex, c, 0);
	// Fluxes through the four faces. A dry neighbour is a wall (no flux); the grid's edge passes
	// the baked flux, carrying still water (eta 0) in and this cell's eta out.
	float f_right = 0.0;
	float f_left = 0.0;
	float f_top = 0.0;
	float f_bottom = 0.0;
	if (c.x < GRID_N - 1) {
		ivec2 r = c + ivec2(1, 0);
		if (is_wet(r)) {
			f_right = face_flux(c, r, q.x, st.y);
		}
	} else {
		f_right = q.x * (1.0 + (q.x > 0.0 ? eta / max(still_depth(c), MIN_FLOW_DEPTH) : 0.0));
	}
	if (c.x > 0) {
		ivec2 l = c - ivec2(1, 0);
		if (is_wet(l)) {
			f_left = face_flux(l, c, texelFetch(base_flux_tex, l, 0).x, state_in[grid_index(l)].y);
		}
	} else {
		f_left = q.z * (1.0 + (q.z < 0.0 ? eta / max(still_depth(c), MIN_FLOW_DEPTH) : 0.0));
	}
	if (c.y < GRID_N - 1) {
		ivec2 t = c + ivec2(0, 1);
		if (is_wet(t)) {
			f_top = face_flux(c, t, q.y, st.w);
		}
	} else {
		f_top = q.y * (1.0 + (q.y > 0.0 ? eta / max(still_depth(c), MIN_FLOW_DEPTH) : 0.0));
	}
	if (c.y > 0) {
		ivec2 b = c - ivec2(0, 1);
		if (is_wet(b)) {
			f_bottom = face_flux(b, c, texelFetch(base_flux_tex, b, 0).y, state_in[grid_index(b)].w);
		}
	} else {
		f_bottom = q.w * (1.0 + (q.w < 0.0 ? eta / max(still_depth(c), MIN_FLOW_DEPTH) : 0.0));
	}
	float hn = eta - DT / CELL * (f_right - f_left + f_top - f_bottom);

	// Body-disturbance coupling: pull the surface toward a soft target set by nearby
	// spheres/impulses, gated by proximity (an ungated blend damped the whole field every step).
	vec2 xz = (vec2(c) + 0.5) / float(GRID_N) * DOMAIN * 2.0 - DOMAIN;
	float target = 0.0;
	float weight = 0.0;
	for (int i = 0; i < NUM_SPHERES; i++) {
		vec4 s = spheres[i];
		float t = length(xz - s.xy) / max(s.z, 1e-4);
		if (t < 1.6) {
			float w = exp(-pow(t * 1.1, 6.0)) * s.w;
			target -= s.z * w;
			weight = max(weight, w);
		}
	}
	for (int i = 0; i < NUM_IMPULSES; i++) {
		vec4 imp = impulses[i];
		float t = length(xz - imp.xy) / max(imp.z, 1e-4);
		if (t < 1.0) {
			float w = (1.0 - t) * imp.w;
			target += imp.z * w;
			weight = max(weight, w);
		}
	}
	hn += (target * RIPPLE_AMPLITUDE - hn) * weight * clamp(6.0 * DT, 0.0, 1.0);
	hn = max(hn, 0.02 - still_depth(c)); // (no wet/dry fronts: the water never empties a cell)
	if (!(abs(hn) < 1e4)) {
		hn = 0.0; // NaN / runaway guard: one bad cell would spread over the grid
	}

	// Flow foam: carried with the water, made where it runs fast and shears, fading over its
	// persistence.
	vec2 v_old = centre_velocity(c);
	float foam = foam_sample(vec2(c) + 0.5 - clamp(v_old, vec2(-0.5 * CELL / DT), vec2(0.5 * CELL / DT)) * DT / CELL);
	if (FLOW_FOAM > 0.0) {
		foam += FLOW_FOAM * flow_foam_source(c, v_old) * DT * 2.0; // (full in about half a second)
	}
	foam = clamp(foam * exp(-DT / max(FLOW_FOAM_PERSISTENCE, 0.05)), 0.0, 1.0);

	state_out[idx] = vec4(hn, st.y, foam, st.w);
	height_out[idx] = WATER_LEVEL + hn;
	vec2 v = vec2(0.5 * (face_u(c) + face_u(c - ivec2(1, 0))), 0.5 * (face_w(c) + face_w(c - ivec2(0, 1))));
	if (c.x == 0) {
		v.x = 0.5 * (face_u(c) + q.z / max(still_depth(c), MIN_FLOW_DEPTH));
	}
	if (c.y == 0) {
		v.y = 0.5 * (face_w(c) + q.w / max(still_depth(c), MIN_FLOW_DEPTH));
	}
	imageStore(velocity_image, c, vec4(v, foam, 0.0)); // (b: flow foam, for materials)
}

// One face's new live velocity. a, b: its two cells; along_wall: dry ground beside the face
// (its tangential flow is slowed there, which is what sheds eddies off rocks and banks).
float face_velocity(ivec2 a, ivec2 b, vec2 pos, float total, float other, float base, bool along_wall, bool x_face) {
	float limit = 0.5 * CELL / DT;
	vec2 vel = x_face ? vec2(total, other) : vec2(other, total);
	vec2 back = pos - clamp(vel, vec2(-limit), vec2(limit)) * DT / CELL;
	float adv = x_face ? sample_u(back) : sample_w(back);
	float v = adv - GRAV * DT / CELL * (eta_at(b) - eta_at(a));
	if (along_wall) {
		v *= 1.0 - clamp(WALL_FRICTION * DT, 0.0, 1.0);
	}
	v = clamp(v, -limit, limit);
	float live = (v - base) * (1.0 - clamp(ALPHA * DT, 0.0, 1.0));
	return abs(live) < 1e3 ? live : 0.0;
}

void velocity_pass(ivec2 c, int idx) {
	vec4 st = state_in[idx];
	vec4 q = texelFetch(base_flux_tex, c, 0);
	float u_new = 0.0;
	float w_new = 0.0;
	if (c.x < GRID_N - 1) {
		ivec2 r = c + ivec2(1, 0);
		if (is_wet(r)) {
			float base = q.x / face_depth(c, r);
			float other = 0.25 * (face_w(c) + face_w(r) + face_w(c - ivec2(0, 1)) + face_w(r - ivec2(0, 1)));
			bool wall = !is_wet(clamp(c + ivec2(0, 1), ivec2(0), ivec2(GRID_N - 1))) || !is_wet(clamp(c - ivec2(0, 1), ivec2(0), ivec2(GRID_N - 1))) || !is_wet(clamp(r + ivec2(0, 1), ivec2(0), ivec2(GRID_N - 1))) || !is_wet(clamp(r - ivec2(0, 1), ivec2(0), ivec2(GRID_N - 1)));
			u_new = face_velocity(c, r, vec2(c) + vec2(1.0, 0.5), base + st.y, other, base, wall, true);
		}
	}
	if (c.y < GRID_N - 1) {
		ivec2 t = c + ivec2(0, 1);
		if (is_wet(t)) {
			float base = q.y / face_depth(c, t);
			float other = 0.25 * (face_u(c) + face_u(t) + face_u(c - ivec2(1, 0)) + face_u(t - ivec2(1, 0)));
			bool wall = !is_wet(clamp(c + ivec2(1, 0), ivec2(0), ivec2(GRID_N - 1))) || !is_wet(clamp(c - ivec2(1, 0), ivec2(0), ivec2(GRID_N - 1))) || !is_wet(clamp(t + ivec2(1, 0), ivec2(0), ivec2(GRID_N - 1))) || !is_wet(clamp(t - ivec2(1, 0), ivec2(0), ivec2(GRID_N - 1)));
			w_new = face_velocity(c, t, vec2(c) + vec2(0.5, 1.0), base + st.w, other, base, wall, false);
		}
	}
	state_out[idx] = vec4(st.x, u_new, st.z, w_new);
}

void main() {
	ivec2 c = ivec2(gl_GlobalInvocationID.xy);
	if (c.x >= GRID_N || c.y >= GRID_N) {
		return;
	}
	int idx = grid_index(c);
	if (!is_wet(c)) {
		// Dry (seabed above the water, or outside the surface mesh's footprint): never simulated.
		// Its rendered height continues the adjacent water (mean of its wet 3x3 neighbours) instead
		// of pinning to WATER_LEVEL: the surface mesh's rim lies between the last wet and first dry
		// cell centres, and a flat dry texel there turns every crest at the staircase shoreline
		// into a one-cell sawtooth.
		state_out[idx] = vec4(0.0);
		if (PASS == 0) {
			float sum = 0.0;
			float count = 0.0;
			for (int dy = -1; dy <= 1; dy++) {
				for (int dx = -1; dx <= 1; dx++) {
					ivec2 nc = clamp(c + ivec2(dx, dy), ivec2(0), ivec2(GRID_N - 1));
					if (is_wet(nc)) {
						sum += state_in[grid_index(nc)].x;
						count += 1.0;
					}
				}
			}
			height_out[idx] = WATER_LEVEL + (count > 0.0 ? sum / count : 0.0);
			imageStore(velocity_image, c, vec4(0.0));
		}
		return;
	}
	if (PASS == 0) {
		height_pass(c, idx);
	} else {
		velocity_pass(c, idx);
	}
}

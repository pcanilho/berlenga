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
// Wet and dry fronts (Chentanez and Müller 2010, section 5): ground up to RUNUP_BAND above the
// still water is simulated too, holding water depth max(cell_depth + eta, 0), so a wake or a
// splash runs up a bank or beach and drains back. A dry cell's eta is its ground (cell_depth
// negative over land); faces carry water only out of cells that have some (upwind depth), at
// most a quarter of it per face and tick so a cell never empties below zero, and a face between
// a dry cell and water lower than its ground carries no velocity.
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
#define RUNUP_BAND 1.0 // m: ground this far above the still water is simulated (wet/dry fronts)
#define DRY_DEPTH 0.002 // m: a cell holding less water than this is dry

// Simulated: water at rest, or ground low enough for water to run up onto (not a wall).
bool in_band(ivec2 c) {
	return cell_depth(c) > -RUNUP_BAND;
}

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
	if (!in_band(f) || !in_band(b)) {
		return 0.0;
	}
	return texelFetch(base_flux_tex, f, 0).x / face_depth(f, b) + state_in[grid_index(f)].y;
}

// Total velocity on the +z face of cell f.
float face_w(ivec2 f) {
	f = clamp(f, ivec2(0), ivec2(GRID_N - 1));
	ivec2 b = ivec2(f.x, min(f.y + 1, GRID_N - 1));
	if (!in_band(f) || !in_band(b)) {
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

// Water depth held by a simulated cell now (m).
float water_depth(ivec2 c) {
	return max(cell_depth(c) + eta_at(c), 0.0);
}

bool has_water(ivec2 c) {
	return in_band(c) && water_depth(c) > DRY_DEPTH;
}

float dry_height(ivec2 c);

// Total velocity at a cell's centre (faces averaged; the grid's own edge reads the nearest face).
vec2 centre_velocity(ivec2 c) {
	c = clamp(c, ivec2(0), ivec2(GRID_N - 1));
	if (!has_water(c)) {
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
//
// Impact foam (IMPACT_FOAM, docs/ideas/stream-demo-assessment.md): two more sources Ihmsen et al.
// also name, where water runs into something rather than past it.
// - Convergence: the velocity's divergence well below zero, water piling into itself (a jet
//   meeting slack water, the toe of a jump, two currents colliding). Measured as the speed lost
//   over 25 cm, like the shear, so the cell size doesn't matter. In slow water the same
//   convergence gathers faint scum lines instead (half strength).
// - Stagnation: current running straight at dry ground standing out of it, the "pillow" of
//   white water on a boulder's upstream face. The wall is the nearest dry cell along each axis
//   within about 0.75 m; the approach speed is read a little upstream of this cell, where the
//   flow has not yet turned aside.
#define IMPACT_FOAM (flow_extra.x > 0.5)

float impact_foam_source(ivec2 c, vec2 v) {
	// Convergence.
	float u_r = face_u(c);
	float u_l = c.x > 0 ? face_u(c - ivec2(1, 0)) : u_r;
	float w_t = face_w(c);
	float w_b = c.y > 0 ? face_w(c - ivec2(0, 1)) : w_t;
	float div = (u_r - u_l + w_t - w_b) / CELL;
	float approach = max(max(abs(u_l), abs(u_r)), max(abs(w_b), abs(w_t))); // the faster side: water arriving
	float squeeze = smoothstep(0.15, 0.6, max(-div, 0.0) * 0.25);
	float converge = squeeze * smoothstep(0.5, 1.5, approach);
	// Scum lines: slow water converging (eddy lines, the edge of a backwater, a pool's seam) sinks and leaves
	// its bubbles and scum on the surface in faint lines without making white water (Langmuir's windrows are
	// the same at sea), so half strength from 0.15 m/s.
	converge = max(converge, 0.5 * squeeze * smoothstep(0.15, 0.5, length(v)));

	// Stagnation.
	int reach = clamp(int(round(0.75 / CELL)), 1, 3);
	vec2 to_wall = vec2(0.0);
	const ivec2 dirs[4] = ivec2[4](ivec2(1, 0), ivec2(-1, 0), ivec2(0, 1), ivec2(0, -1));
	for (int d = 0; d < 4; d++) {
		for (int k = 1; k <= 3; k++) {
			if (k > reach) {
				break;
			}
			ivec2 nc = c + dirs[d] * k;
			if (nc.x < 0 || nc.y < 0 || nc.x >= GRID_N || nc.y >= GRID_N) {
				break; // (the grid's edge is open water, not a wall)
			}
			if (!has_water(nc)) {
				to_wall += vec2(dirs[d]) * (1.0 - float(k - 1) / float(reach + 1));
				break;
			}
		}
	}
	float pillow = 0.0;
	float wall = length(to_wall);
	if (wall > 1e-3) {
		vec2 n = to_wall / wall;
		vec2 v_up = centre_velocity(c - ivec2(round(n * float(reach))));
		if (dot(v_up, v_up) < 1e-8) {
			v_up = v;
		}
		pillow = smoothstep(0.3, 1.2, dot(v_up, n)) * clamp(wall, 0.0, 1.0);
	}
	return max(converge, pillow);
}

float flow_foam_source(ivec2 c, vec2 v) {
	float shear = 0.0;
	shear += length(v - centre_velocity(c + ivec2(1, 0)));
	shear += length(v - centre_velocity(c - ivec2(1, 0)));
	shear += length(v - centre_velocity(c + ivec2(0, 1)));
	shear += length(v - centre_velocity(c - ivec2(0, 1)));
	shear *= 0.25 * 0.25 / CELL; // mean m/s change over 25 cm, so the cell size doesn't matter
	float speed = length(v);
	float froude = speed / sqrt(GRAV * max(water_depth(c), 0.02));
	float trapped_air = smoothstep(0.15, 0.6, shear);
	float supercritical = smoothstep(0.8, 1.6, froude);
	float energy = smoothstep(0.5, 1.5, speed);
	float foam = energy * max(trapped_air, supercritical);
	if (IMPACT_FOAM) {
		foam = max(foam, impact_foam_source(c, v));
	}
	return foam;
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
	float depth_up = water_depth(up);
	float f = q + eta_up * (q / d) + min(depth_up, depth_cap()) * v;
	// At most a quarter of the upwind cell's water through one face a tick, so the four together
	// never take more than it holds (only bites in the thin water at a front).
	float most = 0.25 * depth_up * CELL / DT;
	return clamp(f, -most, most);
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
		if (in_band(r)) {
			f_right = face_flux(c, r, q.x, st.y);
		}
	} else {
		f_right = q.x * (1.0 + (q.x > 0.0 ? eta / max(still_depth(c), MIN_FLOW_DEPTH) : 0.0));
	}
	if (c.x > 0) {
		ivec2 l = c - ivec2(1, 0);
		if (in_band(l)) {
			f_left = face_flux(l, c, texelFetch(base_flux_tex, l, 0).x, state_in[grid_index(l)].y);
		}
	} else {
		f_left = q.z * (1.0 + (q.z < 0.0 ? eta / max(still_depth(c), MIN_FLOW_DEPTH) : 0.0));
	}
	if (c.y < GRID_N - 1) {
		ivec2 t = c + ivec2(0, 1);
		if (in_band(t)) {
			f_top = face_flux(c, t, q.y, st.w);
		}
	} else {
		f_top = q.y * (1.0 + (q.y > 0.0 ? eta / max(still_depth(c), MIN_FLOW_DEPTH) : 0.0));
	}
	if (c.y > 0) {
		ivec2 b = c - ivec2(0, 1);
		if (in_band(b)) {
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
	hn = max(hn, -cell_depth(c)); // (never below the ground: a front's last water drains to dry)
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

	bool dry = cell_depth(c) + hn <= DRY_DEPTH;
	if (dry) {
		foam = 0.0;
	}
	state_out[idx] = vec4(hn, st.y, foam, st.w);
	height_out[idx] = dry ? dry_height(c) : WATER_LEVEL + hn;
	vec2 v = vec2(0.5 * (face_u(c) + face_u(c - ivec2(1, 0))), 0.5 * (face_w(c) + face_w(c - ivec2(0, 1))));
	if (c.x == 0) {
		v.x = 0.5 * (face_u(c) + q.z / max(still_depth(c), MIN_FLOW_DEPTH));
	}
	if (c.y == 0) {
		v.y = 0.5 * (face_w(c) + q.w / max(still_depth(c), MIN_FLOW_DEPTH));
	}
	imageStore(velocity_image, c, dry ? vec4(0.0) : vec4(v, foam, 0.0)); // (b: flow foam, for materials)
}

// Rendered height of a dry cell: it continues the adjacent water (mean of its wet 3x3
// neighbours) instead of pinning to WATER_LEVEL: the surface mesh's rim lies between the last wet
// and first dry cell centres, and a flat dry texel there turns every crest at the shoreline into
// a one-cell sawtooth. The ground (higher than that water, or it would be wet) hides it. With no
// water beside it, the still level, or just under the ground where that lies below it (a hollow
// the water has drained from).
float dry_height(ivec2 c) {
	float sum = 0.0;
	float count = 0.0;
	for (int dy = -1; dy <= 1; dy++) {
		for (int dx = -1; dx <= 1; dx++) {
			ivec2 nc = clamp(c + ivec2(dx, dy), ivec2(0), ivec2(GRID_N - 1));
			if (has_water(nc)) {
				sum += state_in[grid_index(nc)].x;
				count += 1.0;
			}
		}
	}
	return WATER_LEVEL + (count > 0.0 ? sum / count : min(0.0, -cell_depth(c) - 0.05));
}

// One face's new live velocity. a, b: its two cells; along_wall: dry ground beside the face
// (its tangential flow is slowed there, which is what sheds eddies off rocks and banks).
float face_velocity(ivec2 a, ivec2 b, vec2 pos, float total, float other, float base, bool along_wall, bool x_face) {
	float limit = 0.5 * CELL / DT;
	// Fronts: no water either side, or a dry cell whose ground stands above the other side's
	// surface (water can't climb it), carries nothing (Chentanez and Müller's dry-face rule).
	bool wet_a = has_water(a);
	bool wet_b = has_water(b);
	if ((!wet_a && !wet_b) || (!wet_a && eta_at(a) >= eta_at(b)) || (!wet_b && eta_at(b) >= eta_at(a))) {
		return -base; // (live part cancelling the baked flow, which is 0 over dry ground)
	}
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
		if (in_band(r)) {
			float base = q.x / face_depth(c, r);
			float other = 0.25 * (face_w(c) + face_w(r) + face_w(c - ivec2(0, 1)) + face_w(r - ivec2(0, 1)));
			bool wall = !has_water(clamp(c + ivec2(0, 1), ivec2(0), ivec2(GRID_N - 1))) || !has_water(clamp(c - ivec2(0, 1), ivec2(0), ivec2(GRID_N - 1))) || !has_water(clamp(r + ivec2(0, 1), ivec2(0), ivec2(GRID_N - 1))) || !has_water(clamp(r - ivec2(0, 1), ivec2(0), ivec2(GRID_N - 1)));
			u_new = face_velocity(c, r, vec2(c) + vec2(1.0, 0.5), base + st.y, other, base, wall, true);
		}
	}
	if (c.y < GRID_N - 1) {
		ivec2 t = c + ivec2(0, 1);
		if (in_band(t)) {
			float base = q.y / face_depth(c, t);
			float other = 0.25 * (face_u(c) + face_u(t) + face_u(c - ivec2(1, 0)) + face_u(t - ivec2(1, 0)));
			bool wall = !has_water(clamp(c + ivec2(1, 0), ivec2(0), ivec2(GRID_N - 1))) || !has_water(clamp(c - ivec2(1, 0), ivec2(0), ivec2(GRID_N - 1))) || !has_water(clamp(t + ivec2(1, 0), ivec2(0), ivec2(GRID_N - 1))) || !has_water(clamp(t - ivec2(1, 0), ivec2(0), ivec2(GRID_N - 1)));
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
	if (!in_band(c)) {
		// Wall (ground well above the water, or outside the surface mesh's footprint): never
		// simulated.
		state_out[idx] = vec4(0.0);
		if (PASS == 0) {
			height_out[idx] = dry_height(c);
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

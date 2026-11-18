#[compute]
#version 450

// Advected ripple coordinates for the live water (docs/ideas/stream-demo-assessment.md).
//
// Materials carry their ripples and foam along the current with two-phase flow mapping (Vlachos
// 2010): each of two copies is offset by the LOCAL velocity times its phase's age, o = f(x) *
// (p - 0.5) * period, and restarts while the other shows. That is exact in a uniform current
// and wrong where the flow turns: the offset is a straight line along today's velocity, so in an
// eddy behind a rock the pattern shears and smears instead of going round.
//
// Advected texture coordinates (Max and Becker 1995; Neyret 2003, "Advected Textures") fix that:
// the offset is itself carried by the flow, do/dt + v . grad(o) = v, so each texel of the pattern
// rides the real path the water took since its copy restarted. Solved semi-Lagrangian on the
// ripple grid, one dispatch per solver step, from the velocity image the shallow-water pass wrote.
//
// Stored as the difference from the materials' flow-map offset, d = o - f(x) (p - 0.5) period,
// for both phases (rg phase A, ba phase B). Zero is plain flow mapping, so a cleared grid, the
// window's edge, a uniform current and any material that does not read this texture all agree,
// and each phase restarts at d = 0 when its own phase wraps (the same phase as the materials:
// the clock they use plus the same low-frequency noise).
//
//   d'(x) = d(xu) + f(xu) (p(xu, t) - 0.5) P + f(x) dt - f(x) (p(x, t + dt) - 0.5) P,
//   xu = x - f(x) dt.
//
// Where the flow shears hard (an eddy's edge), carrying the coordinates for a whole period stretches the
// pattern into parallel combed arcs. Neyret 2003 regenerates a layer early where its local deformation grows;
// here each phase's correction relaxes back toward plain flow mapping (d = 0, which cannot comb) at a rate set by
// how far the correction's own gradient |grad d| (dimensionless strain) has passed DEFORM_START, so a copy is
// carried round an eddy only as far as it stays undistorted.
//
// A second dispatch in copy mode (res.z == 1) copies dst back to src, as the shore foam does.

layout(local_size_x = 8, local_size_y = 8) in;

#define DEFORM_START 0.25 // strain where the relaxation begins
#define DEFORM_FULL 0.75 // strain where it runs at its full rate
#define RELAX_RATE 8.0 // 1/s at full strain: back to plain flow mapping in about an eighth of a second

layout(set = 0, binding = 0, std140) uniform Params {
	vec4 clock; // x flow period P (s), y material clock at the last step, z clock now, w dt (s)
	vec4 grid; // xy grid centre (world xz), zw domain size (m)
	ivec4 res; // x grid n, y 1 = on (0 writes zeros), z 1 = copy mode, w 1 = restart every cell
};
layout(set = 0, binding = 1) uniform sampler2D velocity_tex; // live total velocity, rg m/s (water_ripple.glsl)
layout(set = 0, binding = 2, rgba32f) uniform restrict readonly image2D src_tex;
layout(set = 0, binding = 3, rgba32f) uniform restrict writeonly image2D dst_tex;

// The materials' phase noise (flow_value_noise / r_noise, the same hash).
float flow_hash(vec2 p) {
	vec3 p3 = fract(vec3(p.xyx) * 0.1031);
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.x + p3.y) * p3.z);
}

float flow_value_noise(vec2 p) {
	vec2 i = floor(p);
	vec2 f = fract(p);
	vec2 u = f * f * (3.0 - 2.0 * f);
	return mix(mix(flow_hash(i), flow_hash(i + vec2(1.0, 0.0)), u.x),
			mix(flow_hash(i + vec2(0.0, 1.0)), flow_hash(i + vec2(1.0, 1.0)), u.x), u.y);
}

// Phase A at world_xz and time t (phase B is half a period on).
float phase_a(vec2 world_xz, float t) {
	return fract(t / clock.x + flow_value_noise(world_xz * 0.05));
}

vec4 src_bilinear(vec2 g, int n) {
	g = clamp(g - 0.5, vec2(0.0), vec2(float(n - 1)));
	ivec2 i0 = ivec2(floor(g));
	ivec2 i1 = min(i0 + 1, ivec2(n - 1));
	vec2 f = g - vec2(i0);
	vec4 a = imageLoad(src_tex, i0);
	vec4 b = imageLoad(src_tex, ivec2(i1.x, i0.y));
	vec4 c = imageLoad(src_tex, ivec2(i0.x, i1.y));
	vec4 d = imageLoad(src_tex, i1);
	return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

void main() {
	ivec2 c = ivec2(gl_GlobalInvocationID.xy);
	int n = res.x;
	if (c.x >= n || c.y >= n) {
		return;
	}
	if (res.z == 1) {
		imageStore(dst_tex, c, imageLoad(src_tex, c));
		return;
	}
	vec2 f = texelFetch(velocity_tex, c, 0).rg;
	if (res.y == 0 || res.w == 1 || dot(f, f) < 1e-8) {
		imageStore(dst_tex, c, vec4(0.0)); // (dry, still or restarting: plain flow mapping)
		return;
	}
	float period = max(clock.x, 0.05);
	float t0 = clock.y;
	float t1 = clock.z;
	float dt = clock.w;
	float cell = grid.z / float(n);
	vec2 g = vec2(c) + 0.5;
	vec2 x = grid.xy + (g / float(n) - 0.5) * grid.zw;
	vec2 step_m = clamp(f * dt, vec2(-2.0 * cell), vec2(2.0 * cell));
	vec2 xu = x - step_m;
	vec2 gu = g - step_m / cell;
	vec4 d_up = src_bilinear(gu, n);
	vec2 f_up = texture(velocity_tex, gu / float(n)).rg;

	float pa_up = phase_a(xu, t0);
	float pa_was = phase_a(x, t0);
	float pa_now = phase_a(x, t1);
	float pb_up = fract(pa_up + 0.5);
	float pb_was = fract(pa_was + 0.5);
	float pb_now = fract(pa_now + 0.5);

	vec2 da = d_up.xy + f_up * (pa_up - 0.5) * period + step_m - f * (pa_now - 0.5) * period;
	vec2 db = d_up.zw + f_up * (pb_up - 0.5) * period + step_m - f * (pb_now - 0.5) * period;
	// A phase that wrapped this step restarts (its weight is zero there, so the jump is unseen).
	if (pa_now < pa_was) {
		da = vec2(0.0);
	}
	if (pb_now < pb_was) {
		db = vec2(0.0);
	}
	// Strain of each phase's correction (central differences, m per m), relaxed where it combs.
	ivec2 lo = max(c - 1, ivec2(0));
	ivec2 hi = min(c + 1, ivec2(n - 1));
	vec4 ddx = (imageLoad(src_tex, ivec2(hi.x, c.y)) - imageLoad(src_tex, ivec2(lo.x, c.y))) / (float(max(hi.x - lo.x, 1)) * cell);
	vec4 ddz = (imageLoad(src_tex, ivec2(c.x, hi.y)) - imageLoad(src_tex, ivec2(c.x, lo.y))) / (float(max(hi.y - lo.y, 1)) * cell);
	float strain_a = max(length(vec2(ddx.x, ddz.x)), length(vec2(ddx.y, ddz.y)));
	float strain_b = max(length(vec2(ddx.z, ddz.z)), length(vec2(ddx.w, ddz.w)));
	da *= exp(-RELAX_RATE * dt * smoothstep(DEFORM_START, DEFORM_FULL, strain_a));
	db *= exp(-RELAX_RATE * dt * smoothstep(DEFORM_START, DEFORM_FULL, strain_b));
	// Bounded: a copy lives one period, so it cannot have strayed further than the fastest water
	// carries it in that time.
	float most = 3.0 * period;
	vec4 d = clamp(vec4(da, db), vec4(-most), vec4(most));
	if (!all(lessThan(abs(d), vec4(1e4)))) {
		d = vec4(0.0);
	}
	imageStore(dst_tex, c, d);
}

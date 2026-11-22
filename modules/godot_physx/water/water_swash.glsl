#[compute]
#version 450

// Swash and wet sand, on the ripple grid (fixed to the ground). Per cell:
//   r  run-up level (m above still water). Set at the waterline by the
//      arriving waves, then carried up the slope from the seaward neighbor
//      at a limited uprush speed, and drained back down at a steady speed --
//      so each big wave rushes up the sand in tongues and slides back down
//      more slowly. Materials add it to the still-water depth: sand lower
//      than r above the water is covered by a thin film.
//   g  sand wetness 0..1: 1 wherever water (still or swash) covers the
//      cell, decaying once it's uncovered, so the sand stays dark and shiny
//      for a while after the backwash.
//   b  running mean square of the local surface rise at the waterline, to
//      tell the bigger crests (the ones that run up) from the everyday chop,
//      whatever the sea state.
// Reads src, writes dst; a second dispatch in copy mode (res.y == 1)
// copies dst back to src.

layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0, std140) uniform Params {
	vec4 swash; // x dt, y max run-up (m), z drain speed (m/s, vertical), w wetness decay factor this step
	vec4 domains; // xy ripple domain size, zw ocean domain size
	vec4 misc; // x water_level, y time (s), z uprush speed (m/s, vertical), w rise mean-square blend this step
	ivec4 res; // x = ripple grid n, y = 1 for copy mode
	vec4 shore; // x, y the shore wave trains' angular frequencies (rad/s), z the surface's clock (s), w 1 when shore waves are on
	vec4 shore_b; // xy grid centre (world XZ), z deep-water wave height H0 (m), w deep-water wavelength L0 (m)
};
layout(set = 0, binding = 1) uniform sampler2D cell_depth_tex;
layout(set = 0, binding = 2) uniform sampler2D ripple_height_tex;
layout(set = 0, binding = 3) uniform sampler2D ocean_height_tex;
layout(set = 0, binding = 4, rgba32f) uniform restrict readonly image2D src_tex;
layout(set = 0, binding = 5, rgba32f) uniform restrict writeonly image2D dst_tex;
// PhysXWaterSurface3D's shore waves on the ripple grid: phase 1, height 1,
// phase 2, height 2 (see shore_wave() in its material shader).
layout(set = 0, binding = 6) uniform sampler2D shore_wave_tex;

const float BREAKER = 0.78;

// Still water shallower than this (m) is where waves set the run-up.
const float SOURCE_DEPTH = 0.6;

float hash(vec2 p) {
	vec3 p3 = fract(vec3(p.xyx) * 0.1031);
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.x + p3.y) * p3.z);
}

float value_noise(vec2 p) {
	vec2 i = floor(p);
	vec2 f = fract(p);
	vec2 u = f * f * (3.0 - 2.0 * f);
	return mix(mix(hash(i), hash(i + vec2(1.0, 0.0)), u.x), mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), u.x), u.y);
}

float src_r_bilinear(vec2 p, int n) {
	p = clamp(p, vec2(0.0), vec2(float(n - 1)));
	ivec2 i0 = ivec2(floor(p));
	ivec2 i1 = min(i0 + 1, ivec2(n - 1));
	vec2 f = p - vec2(i0);
	float a = imageLoad(src_tex, i0).r;
	float b = imageLoad(src_tex, ivec2(i1.x, i0.y)).r;
	float c = imageLoad(src_tex, ivec2(i0.x, i1.y)).r;
	float d = imageLoad(src_tex, i1).r;
	return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// Surface rise (m) of the breaking shore waves at a cell, as the water
// material's shore_wave() draws it (height only).
float shore_eta(ivec2 c, vec2 rel, float depth, int n) {
	if (shore.w < 0.5 || textureSize(shore_wave_tex, 0).x != n) {
		return 0.0;
	}
	vec4 a = texelFetch(shore_wave_tex, c, 0);
	vec2 world = rel + shore_b.xy;
	float t = shore.z;
	float vary = 0.55 + 0.45 * value_noise(world * 0.04 + vec2(t * 0.03, 0.0));
	float eta = 0.0;
	for (int i = 0; i < 2; i++) {
		float phi = i == 0 ? a.x : a.z;
		float height = i == 0 ? a.y : a.w;
		if (height <= 1e-4) {
			continue;
		}
		float brk = smoothstep(0.8, 0.98, height / max(BREAKER * max(depth, 0.0), 1e-3));
		float th = phi - (i == 0 ? shore.x : shore.y) * t;
		float qs = 1.0 + 1.5 * brk;
		float base = max(0.5 + 0.5 * cos(th - 0.6 * brk * cos(th)), 0.0);
		eta += height * vary * (pow(base, qs) - inversesqrt(3.14159265359 * (qs + 0.25)));
	}
	return eta;
}

// Depth for the slope estimate; a wall neighbor counts as this cell's own
// depth so the sentinel can't blow the gradient up.
float slope_depth(ivec2 p, float fallback, int n) {
	float d = texelFetch(cell_depth_tex, clamp(p, ivec2(0), ivec2(n - 1)), 0).r;
	return d < -5000.0 ? fallback : d;
}

void main() {
	ivec2 c = ivec2(gl_GlobalInvocationID.xy);
	int n = res.x;
	if (c.x >= n || c.y >= n) {
		return;
	}
	if (res.y == 1) {
		imageStore(dst_tex, c, imageLoad(src_tex, c));
		return;
	}
	float depth = texelFetch(cell_depth_tex, c, 0).r;
	if (depth < -5000.0 || depth >= SOURCE_DEPTH) {
		// A wall (WaterSolver::WALL_DEPTH), or water too deep to matter.
		imageStore(dst_tex, c, vec4(0.0, depth > 0.0 ? 1.0 : 0.0, 0.0, 0.0));
		return;
	}
	vec4 s = imageLoad(src_tex, c);
	vec2 rel_c = ((vec2(c) + 0.5) / float(n) - 0.5) * domains.xy;
	float target;
	float ms = s.b;
	if (depth > 0.0) {
		// Waterline: surface rise here -- ripples plus the (unfaded) ocean
		// waves arriving. Only crests well above the usual ones run up (wave
		// groups), by an amount that grows with the sea state, capped at the
		// max run-up; slow noise varies it along the beach so the swash comes
		// up in tongues.
		vec2 rel = ((vec2(c) + 0.5) / float(n) - 0.5) * domains.xy;
		vec2 uv_o = clamp(rel / domains.zw + 0.5, vec2(0.0), vec2(1.0));
		float rise = texelFetch(ripple_height_tex, c, 0).r - misc.x + texture(ocean_height_tex, uv_o).r + shore_eta(c, rel, depth, n);
		ms = mix(ms, rise * rise, misc.w);
		float rms = sqrt(max(ms, 1e-6));
		float nz = value_noise(rel * 0.3 + vec2(misc.y * 0.06, misc.y * 0.025));
		// How far this stretch of beach can run up: a slow pattern 5-15 m
		// across (like beach cusps), so the highest reach -- and the wet line
		// it leaves -- scallops along the shore instead of tracing one
		// height contour.
		float cusp = 0.65 * value_noise(rel * 0.07 + vec2(11.0, misc.y * 0.004)) + 0.35 * value_noise(rel * 0.19 - vec2(4.0, 0.0));
		float cap_max = swash.y;
		if (shore.w > 0.5 && shore_b.z > 0.0) {
			// With shore waves, the reach follows the empirical run-up of
			// Stockdon et al. (2006): the 2% exceedance run-up from the
			// deep-water height H0, wavelength L0 and the beach slope here.
			float cell = domains.x / float(n);
			vec2 g = vec2(slope_depth(c + ivec2(1, 0), depth, n) - slope_depth(c - ivec2(1, 0), depth, n),
					slope_depth(c + ivec2(0, 1), depth, n) - slope_depth(c - ivec2(0, 1), depth, n));
			float beta = clamp(length(g) / (2.0 * cell), 0.005, 0.3);
			float hl = sqrt(shore_b.z * shore_b.w);
			float r2 = 1.1 * (0.35 * beta * hl + 0.5 * sqrt(shore_b.z * shore_b.w * (0.563 * beta * beta + 0.004)));
			cap_max = max(cap_max, r2);
		}
		float cap = cap_max * (0.25 + 0.75 * cusp);
		// Each wave reaches in proportion to how big it is, so successive
		// waves stop at different places.
		float strength = clamp((rise / rms - 0.5) / 1.5, 0.0, 1.0);
		target = min(min(cap, 2.0 * rms) * strength * (0.6 + 0.8 * nz), cap);
	} else {
		// Dry sand: the run-up climbs from the seaward neighbor (up the
		// depth gradient), so a tongue keeps its shape as it runs up.
		vec2 grad = vec2(slope_depth(c + ivec2(1, 0), depth, n) - slope_depth(c - ivec2(1, 0), depth, n),
				slope_depth(c + ivec2(0, 1), depth, n) - slope_depth(c - ivec2(0, 1), depth, n));
		target = length(grad) > 1e-5 ? src_r_bilinear(vec2(c) + normalize(grad), n) : 0.0;
	}
	float r = target > s.r ? min(target, s.r + misc.z * swash.x) : max(target, s.r - swash.z * swash.x);

	// Partial coverage across the edge (r and the depth vary smoothly), so the
	// wet line falls between cells instead of stepping cell by cell.
	// A fixed pattern on the sand jitters the line by a few cm of height, so
	// it's ragged rather than following the run-up contour exactly. Only
	// scales well above the cell size (finer noise aliased into per-cell
	// blocks); the materials add the fine detail per pixel.
	float wn = 0.6 * value_noise(rel_c * 0.9) + 0.4 * value_noise(rel_c * 1.9 + 5.0);
	float wet = max(smoothstep(-0.01, 0.02, depth + r + (wn - 0.5) * 0.12), s.g * swash.w);
	imageStore(dst_tex, c, vec4(r, wet, ms, 0.0));
}

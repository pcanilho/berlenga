#[compute]
#version 450

// Dispersive surface waves over the live shallow water (docs/ideas/water.md "Ripples that bounce off boulders").
//
// The shallow-water grid moves every wave at sqrt(g d), whatever its length, and its cells (25 cm on a river's
// live window) cannot hold the short waves round a stone at all, so the ripples a material draws are noise that
// only rides the current. Jeschke and Wojtan, "Generalizing Shallow Water Simulations with Dispersive Surface
// Waves" (ACM ToG 2023), keep the bulk flow in the shallow-water solver and step the surface waves exactly in
// Fourier space with Airy's dispersion, omega^2 = g k tanh(k d), at a few depths blended per cell, carried by the
// bulk velocity. Here that surface layer runs on its own finer grid over the same window (one-way: the bulk is
// the shallow-water solver's), so a wave 40 cm long travels at its own speed, wraps round a boulder, reflects
// off the banks (dry cells hold it at zero) and trails behind as a wake in the current.
//
// Sources: water meeting a stone. A surface-piercing obstacle in a current is the classic moving pressure
// patch of ship-wake theory (Kelvin; Havelock): the stagnation head 0.5 |v|^2 / g pushes on the water along
// the obstacle's upstream side, phi -= dt g p, and the dispersion turns that into the standing wave train and
// the wake.
//
// Three modes a step (res.y), between the shared-memory FFTs (water_fft_rows.glsl, the +i transform only):
//   0 pack: advect h and phi (semi-Lagrangian, bilinear) by the live velocity, add the sources, hold dry cells
//     at zero and damp the edges, write conj(h + i phi) so the +i FFT yields the conjugate of the forward one
//     (the state image is only read here: the unpack writes it, so no cell reads a neighbour already moved);
//   1 spectral: split the packed spectrum Z into H and Phi (Z(-k) for the conjugate halves), step each depth
//     sample exactly (cos and sin of omega dt) and write Z' / N^2 for that sample;
//   2 unpack: after the inverse transforms, blend the depth samples by this cell's still depth (in log depth)
//     and store h, phi.

layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0, std140) uniform Params {
	vec4 step_p; // x dt (s), y fine cell (m), z gravity, w time (s)
	vec4 domain_p; // xy domain size (m), z source strength, w damping (1/s)
	vec4 depths; // the four depth samples (m), ascending
	ivec4 res; // x fine grid N, y mode, z 1 = on (0 clears), w unused
};
layout(set = 0, binding = 1) uniform sampler2D velocity_tex; // live total velocity, rg m/s (water_ripple.glsl)
layout(set = 0, binding = 2) uniform sampler2D cell_depth_tex; // still depth per live cell (m, < 0 dry)
layout(set = 0, binding = 3, rg32f) uniform restrict image2D state_img; // r h (m), g phi (m^2/s)
layout(set = 0, binding = 4, std430) restrict buffer Packed {
	vec2 packed_buf[];
};
layout(set = 0, binding = 5, std430) restrict buffer D0 {
	vec2 d0[];
};
layout(set = 0, binding = 6, std430) restrict buffer D1 {
	vec2 d1[];
};
layout(set = 0, binding = 7, std430) restrict buffer D2 {
	vec2 d2[];
};
layout(set = 0, binding = 8, std430) restrict buffer D3 {
	vec2 d3[];
};

#define EDGE_CELLS 24.0 // the sponge along the window's edge, in fine cells (the transform wraps)
#define DRY_DEPTH 0.01 // m: shallower than this holds no surface waves

vec2 state_bilinear(vec2 p, int n) {
	p = clamp(p, vec2(0.0), vec2(float(n - 1)));
	ivec2 i0 = ivec2(floor(p));
	ivec2 i1 = min(i0 + 1, ivec2(n - 1));
	vec2 f = p - vec2(i0);
	vec2 a = imageLoad(state_img, i0).rg;
	vec2 b = imageLoad(state_img, ivec2(i1.x, i0.y)).rg;
	vec2 c = imageLoad(state_img, ivec2(i0.x, i1.y)).rg;
	vec2 d = imageLoad(state_img, i1).rg;
	return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

float depth_at(vec2 uv) {
	return texture(cell_depth_tex, uv).r;
}

// Hash for the eddies' unsteadiness (the same as the materials').
float sw_hash(vec2 p) {
	vec3 p3 = fract(vec3(p.xyx) * 0.1031);
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.x + p3.y) * p3.z);
}

void main() {
	ivec2 c = ivec2(gl_GlobalInvocationID.xy);
	int n = res.x;
	if (c.x >= n || c.y >= n) {
		return;
	}
	int idx = c.y * n + c.x;
	vec2 uv = (vec2(c) + 0.5) / float(n);
	float dt = step_p.x;
	float cell = step_p.y;
	float g = step_p.z;

	if (res.y == 0) {
		float d = depth_at(uv);
		vec2 hp = vec2(0.0);
		if (res.z != 0 && d > DRY_DEPTH) {
			vec2 v = texture(velocity_tex, uv).rg;
			hp = state_bilinear(vec2(c) - v * dt / cell, n);
			// The stagnation head where the current runs into dry ground (a stone, a bank's point): the obstacle
			// mask's gradient points into it, so the water on its upstream face pushes, the lee does not.
			vec2 du = vec2(1.0) / vec2(textureSize(cell_depth_tex, 0));
			float mx0 = 1.0 - smoothstep(-0.02, 0.05, depth_at(uv - vec2(du.x, 0.0)));
			float mx1 = 1.0 - smoothstep(-0.02, 0.05, depth_at(uv + vec2(du.x, 0.0)));
			float mz0 = 1.0 - smoothstep(-0.02, 0.05, depth_at(uv - vec2(0.0, du.y)));
			float mz1 = 1.0 - smoothstep(-0.02, 0.05, depth_at(uv + vec2(0.0, du.y)));
			vec2 grad = 0.5 * vec2(mx1 - mx0, mz1 - mz0);
			float edge = length(grad);
			// The speed the water arrives with, two cells out from the stone (on its face the water has already
			// slowed into the stagnation point it is pushing up).
			vec2 nrm = edge > 1e-3 ? grad / edge : vec2(0.0);
			vec2 va = texture(velocity_tex, uv - nrm * 2.0 * du).rg;
			float speed = length(va);
			if (edge > 1e-3 && speed > 0.05) {
				float meet = smoothstep(-0.2, 0.6, dot(va / speed, nrm));
				// Unsteady as real water round a stone is: the push breathes over a second or so, patch by patch.
				float breathe = 0.7 + 0.3 * sin(step_p.w * (2.0 + 2.0 * sw_hash(floor(uv * 64.0))) + 6.28 * sw_hash(floor(uv * 64.0) + 7.0));
				float p = domain_p.z * 0.5 * speed * speed / g * min(edge * 2.0, 1.0) * meet * breathe;
				hp.y -= dt * g * p;
			}
			// Turbulence: the eddies the current sheds off stones and banks (the shallow-water grid resolves them)
			// press on the surface with their velocity fluctuations, p' ~ u'^2 / g, u' = l |grad v| with the mixing
			// length l one live cell (Prandtl). That pressure jitters over a few fine cells and a fraction of a second,
			// and the dispersion spreads it into the ripple patches that cover real water downstream of a stone.
			vec2 vx0 = texture(velocity_tex, uv - vec2(du.x, 0.0)).rg;
			vec2 vx1 = texture(velocity_tex, uv + vec2(du.x, 0.0)).rg;
			vec2 vz0 = texture(velocity_tex, uv - vec2(0.0, du.y)).rg;
			vec2 vz1 = texture(velocity_tex, uv + vec2(0.0, du.y)).rg;
			float l = domain_p.x * du.x;
			float shear = length(vec4(vx1 - vx0, vz1 - vz0)) * 0.5 / l;
			float up = l * shear;
			float p_t = domain_p.z * up * up / g * (1.0 - min(edge * 2.0, 1.0));
			if (p_t > 1e-6) {
				// Value noise over 3 fine cells, re-drawn every 0.15 s and blended between draws.
				vec2 q = vec2(c) / 3.0;
				float slot = step_p.w / 0.15;
				float s0 = floor(slot);
				vec2 qi = floor(q);
				vec2 qf = q - qi;
				vec2 qu = qf * qf * (3.0 - 2.0 * qf);
				float n0 = mix(mix(sw_hash(qi + s0 * 17.0), sw_hash(qi + vec2(1.0, 0.0) + s0 * 17.0), qu.x),
						mix(sw_hash(qi + vec2(0.0, 1.0) + s0 * 17.0), sw_hash(qi + vec2(1.0) + s0 * 17.0), qu.x), qu.y);
				float n1 = mix(mix(sw_hash(qi + (s0 + 1.0) * 17.0), sw_hash(qi + vec2(1.0, 0.0) + (s0 + 1.0) * 17.0), qu.x),
						mix(sw_hash(qi + vec2(0.0, 1.0) + (s0 + 1.0) * 17.0), sw_hash(qi + vec2(1.0) + (s0 + 1.0) * 17.0), qu.x), qu.y);
				float jitter = 2.0 * mix(n0, n1, smoothstep(0.0, 1.0, slot - s0)) - 1.0;
				hp.y -= dt * g * p_t * jitter * 4.0;
			}
			// The sponge along the window's edge, where the transform would wrap a wave round to the far side.
			vec2 e = min(vec2(c), vec2(n - 1) - vec2(c));
			float sponge = smoothstep(0.0, EDGE_CELLS, min(e.x, e.y));
			hp *= mix(exp(-8.0 * dt), 1.0, sponge);
		}
		packed_buf[idx] = vec2(hp.x, -hp.y); // conj(h + i phi); the state is written back by the unpack
		return;
	}

	if (res.y == 1) {
		ivec2 cm = (ivec2(n) - c) % n;
		vec2 zk = packed_buf[idx];
		zk = vec2(zk.x, -zk.y); // the forward transform's Z(k)
		vec2 zm = packed_buf[cm.y * n + cm.x];
		zm = vec2(zm.x, -zm.y);
		vec2 zmc = vec2(zm.x, -zm.y); // conj(Z(-k))
		vec2 hk = 0.5 * (zk + zmc);
		vec2 dz = 0.5 * (zk - zmc); // = i Phi
		vec2 pk = vec2(dz.y, -dz.x); // Phi = dz / i
		vec2 kv = vec2(c.x < n / 2 ? c.x : c.x - n, c.y < n / 2 ? c.y : c.y - n) * 6.28318530718 / domain_p.xy;
		float k = length(kv);
		float damp = exp(-domain_p.w * dt) / float(n * n);
		for (int j = 0; j < 4; j++) {
			float dj = depths[j];
			float w2 = g * k * tanh(k * dj);
			float w = sqrt(w2);
			vec2 h2;
			vec2 p2;
			if (w < 1e-5) {
				h2 = hk + pk * (k * tanh(k * dj)) * dt;
				p2 = pk - hk * g * dt;
			} else {
				float cw = cos(w * dt);
				float sw = sin(w * dt);
				float a = w / g;
				h2 = hk * cw + pk * (a * sw);
				p2 = pk * cw - hk * (sw / a);
			}
			vec2 z2 = (h2 + vec2(-p2.y, p2.x)) * damp; // H' + i Phi'
			if (j == 0) {
				d0[idx] = z2;
			} else if (j == 1) {
				d1[idx] = z2;
			} else if (j == 2) {
				d2[idx] = z2;
			} else {
				d3[idx] = z2;
			}
		}
		return;
	}

	// Unpack: the four depths' fields blended by this cell's depth (in log depth, clamped to the samples).
	float d = depth_at(uv);
	vec2 hp = vec2(0.0);
	if (res.z != 0 && d > DRY_DEPTH) {
		float ld = log2(clamp(d, depths.x, depths.w));
		vec4 l = log2(depths);
		vec2 z0 = d0[idx];
		vec2 z1 = d1[idx];
		vec2 z2 = d2[idx];
		vec2 z3 = d3[idx];
		if (ld <= l.y) {
			hp = mix(z0, z1, (ld - l.x) / max(l.y - l.x, 1e-4));
		} else if (ld <= l.z) {
			hp = mix(z1, z2, (ld - l.y) / max(l.z - l.y, 1e-4));
		} else {
			hp = mix(z2, z3, (ld - l.z) / max(l.w - l.z, 1e-4));
		}
		// The shore: the waves die out over the last few centimetres instead of standing at the line.
		hp *= smoothstep(DRY_DEPTH, 0.06, d);
	}
	imageStore(state_img, c, vec4(hp, 0.0, 0.0));
}

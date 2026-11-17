#[compute]
#version 450

#include "mpm_block_inc.glsl"
#include "mpm_bs_diffuse_inc.glsl"

layout(local_size_x = GROUP) in;

// One thread per fluid particle: Ihmsen 2012 Sec. 4, spawn potentials and
// sampling, read off the grid of the step's last substep.
void main() {
	uint id = gl_GlobalInvocationID.x;
	if (id >= uint(PCOUNT) || DCAP == 0u) {
		return;
	}
	if (particle_dead(id)) {
		return;
	}
	vec3 x = particles[id].x_d.xyz;
	vec3 v = particles[id].v.xyz;
	float speed = length(v);

	// Kinetic energy per unit mass (Eq. 5, mass dropped: every particle weighs the same).
	float ik = potential(0.5 * speed * speed, KE_MIN, KE_MAX);
	if (ik <= 0.0) {
		return;
	}

	ivec3 base;
	vec3 fx;
	grid_local(x, base, fx);
	vec3 wx = bspline(fx.x), wy = bspline(fx.y), wz = bspline(fx.z);
	ivec3 base_bc = base >> 2;
	int bs[8];
	resolve_blocks(base_bc, (base + 2) >> 2, bs);

	// The particle's own velocity field from its last G2P (APIC): v + C (x_node - x), about the position it had
	// then. Trapped air compares each node with that field, not with v alone, so only motion the smooth field
	// cannot hold counts -- liquid striking liquid, a break in the velocity -- and a stream's own shear, stretch
	// and necking (which a single velocity reads as ~1 m/s across the stencil: a smooth pour full of bubbles) do not.
	mat3 C = mat3(particles[id].c0.xyz, particles[id].c1.xyz, particles[id].c2.xyz);
	vec3 x_then = x - DT * v;

	const float m_rest = RHO0 * DX * DX * DX;
	float ta = 0.0; // trapped air, Eq. 2
	float m = 0.0;
	vec3 mc = vec3(0.0);
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			for (int k = 0; k < 3; k++) {
				ivec3 node = base + ivec3(i, j, k);
				int idx = cell_slot(node, base_bc, bs);
				if (idx < 0) {
					continue;
				}
				vec4 g = grid_v[idx];
				if (g.w <= 0.0) {
					continue;
				}
				float w = wx[i] * wy[j] * wz[k];
				vec3 xn = ORIGIN + vec3(node) * DX;
				vec3 vij = v + C * (xn - x_then) - g.xyz;
				vec3 xij = x - xn;
				float lv = length(vij);
				float lx = length(xij);
				// A node holding little liquid is mostly air: weight it by its liquid fraction.
				float wl = w * clamp(g.w / m_rest, 0.0, 1.0);
				if (lv > 1e-6 && lx > 1e-6) {
					ta += wl * lv * (1.0 - dot(vij / lv, xij / lx));
				}
				m += w * g.w;
				mc += w * g.w * xn;
			}
		}
	}
	if (m <= 0.0) {
		return;
	}

	// Wave crest, Eq. 3-4: a convex surface (the liquid's centre of mass sits
	// well inside) moving out along its normal. The centroid offset in cells is
	// the curvature proxy on a grid.
	vec3 off = x - mc / m;
	float lo = length(off);
	float iwc = 0.0;
	if (lo > 1e-6 && speed > 1e-6 && dot(v / speed, off / lo) >= 0.6) {
		iwc = potential(lo / DX, 0.45, 1.0);
	}

	float ita = potential(ta, TA_MIN, TA_MAX);
	// Eq. 6. Crests spawn at half the trapped-air rate.
	float n = ik * DRATE * (ita + 0.5 * iwc) * FDT;
	if (n <= 0.0) {
		return;
	}

	uint s = pcg(id * 9781u + DSEED * 6271u);
	int count = int(n);
	if (rand01(s) < fract(n)) {
		count++;
	}
	count = min(count, 4);

	// Eq. 7-9: in the cylinder the particle sweeps this frame, around its velocity.
	vec3 dir = speed > 1e-6 ? v / speed : vec3(0.0, 1.0, 0.0);
	vec3 helper = abs(dir.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
	vec3 e1 = normalize(cross(dir, helper));
	vec3 e2 = cross(dir, e1);
	const float rv = DX * 0.5; // the particle's own radius (spacing is DX / 2)
	for (int c = 0; c < count; c++) {
		float r = rv * sqrt(rand01(s));
		float th = 6.2831853 * rand01(s);
		float h = rand01(s) * speed * FDT;
		vec3 lat = r * cos(th) * e1 + r * sin(th) * e2;
		vec3 xd = x + lat + h * dir;
		vec3 vd = v + lat;

		// Bubble radius from Q(r) ~ 1/r^3 on [DRMIN, DRMAX] (inverse CDF).
		float a = 1.0 / (DRMIN * DRMIN);
		float b = 1.0 / (DRMAX * DRMAX);
		float rad = 1.0 / sqrt(a - rand01(s) * (a - b));

		// Normally distributed lifetime (Box-Muller), sd 0.4 of the mean.
		float u1 = max(rand01(s), 1e-6);
		float u2 = rand01(s);
		float z = sqrt(-2.0 * log(u1)) * cos(6.2831853 * u2);
		float life = max(DLIFE * (1.0 + 0.4 * z), 0.1 * DLIFE);

		uint slot = atomicAdd(dhead[0], 1u) % DCAP;
		diffuse[slot * 2u] = vec4(xd, life);
		diffuse[slot * 2u + 1u] = vec4(vd, rad);
	}
}

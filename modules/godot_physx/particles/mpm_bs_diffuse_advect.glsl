#[compute]
#version 450

#include "mpm_block_inc.glsl"
#include "mpm_bs_diffuse_inc.glsl"

layout(local_size_x = GROUP) in;

// One thread per diffuse slot: sort it into spray, foam or a bubble by the
// liquid around it, then move it by that kind's rule (Ihmsen 2012 Sec. 5, with
// the bubble drag and the foam surface hold of Wretborn 2022; see the include).
// Also packs the slot for drawing.

vec3 bspline_d(float fx) {
	return vec3(-(1.5 - fx), -2.0 * (fx - 1.0), fx - 0.5);
}

void main() {
	uint id = gl_GlobalInvocationID.x;
	if (id >= DCAP) {
		return;
	}
	vec4 a = diffuse[id * 2u];
	vec4 b = diffuse[id * 2u + 1u];
	if (a.w <= 0.0) {
		dout[id] = vec4(0.0);
		return;
	}
	vec3 x = a.xyz;
	vec3 v = b.xyz;
	float life = a.w;
	float rad = b.w;

	ivec3 base;
	vec3 fx;
	grid_local(x, base, fx);
	vec3 wx = bspline(fx.x), wy = bspline(fx.y), wz = bspline(fx.z);
	vec3 dwx = bspline_d(fx.x), dwy = bspline_d(fx.y), dwz = bspline_d(fx.z);
	ivec3 base_bc = base >> 2;
	int bs[8];
	resolve_blocks(base_bc, (base + 2) >> 2, bs);

	vec3 uf = vec3(0.0);
	float m = 0.0;
	vec3 gm = vec3(0.0); // gradient of the grid mass field
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			for (int k = 0; k < 3; k++) {
				int idx = cell_slot(base + ivec3(i, j, k), base_bc, bs);
				if (idx < 0) {
					continue;
				}
				vec4 g = grid_v[idx];
				if (g.w <= 0.0) {
					continue;
				}
				float w = wx[i] * wy[j] * wz[k];
				uf += w * g.w * g.xyz;
				m += w * g.w;
				gm += g.w * vec3(dwx[i] * wy[j] * wz[k], wx[i] * dwy[j] * wz[k], wx[i] * wy[j] * dwz[k]) / DX;
			}
		}
	}
	const float m_rest = RHO0 * DX * DX * DX;
	float frac = m / m_rest;
	if (m > 1e-12) {
		uf /= m;
	}

	float age = 0.5; // spray and bubbles age slowly (Ihmsen: not at all) so none outlive the scene
	if (frac < SPRAY_BELOW) {
		// Spray: ballistic.
		v += GRAV * FDT;
		x += v * FDT;
	} else if (frac > BUBBLE_ABOVE) {
		// Bubble: carried by the water at the slip speed where buoyancy meets the
		// drag of Wretborn 2022 Eq. 5, f = mu r (6 pi + pi/2 rho r |dv| / mu) dv.
		// A bubble's own inertia is negligible, so it sits at that balance.
		const float mu = 1.0e-3;
		float g = length(GRAV);
		float qa = 1.5707963 * RHO0 * rad * rad;
		float qb = 18.849556 * mu * rad;
		float qc = 4.1887902 * rad * rad * rad * max(RHO0 - 1.2, 0.0) * g * DBUOY;
		float slip = (-qb + sqrt(qb * qb + 4.0 * qa * qc)) / (2.0 * max(qa, 1e-12));
		vec3 up = g > 1e-6 ? -GRAV / g : vec3(0.0, 1.0, 0.0);
		v = uf + up * slip;
		x += v * FDT;
	} else {
		// Foam: carried by the surface, held on it (Wretborn 2022 manifold
		// constraint: project onto the level, each move capped).
		v = uf;
		x += v * FDT;
		vec3 gf = gm / m_rest;
		float gl2 = dot(gf, gf);
		if (gl2 > 1e-8) {
			vec3 d = -(frac - FOAM_LEVEL) * gf / gl2;
			float dl = length(d);
			float cap = 0.5 * DX;
			if (dl > cap) {
				d *= cap / dl;
			}
			x += d;
		}
		age = 1.0;
	}

	bool touched = false;
	for (int ci = 0; ci < NCOL; ci++) {
		float sd = collider_sdf(ci, x);
		if (sd < 0.0) {
			vec3 nh = collider_normal(ci, x);
			x += nh * (-sd + 1e-4);
			float vn = dot(v - colliders[ci].c2.xyz, nh);
			if (vn < 0.0) {
				v -= vn * nh;
			}
			touched = true;
		}
	}
	float floor_y = ORIGIN.y + DX * 1.5;
	if (x.y < floor_y) {
		x.y = floor_y;
		v.y = max(v.y, 0.0);
		touched = true;
	}
	// Spray that lands on something dry breaks up fast.
	if (touched && frac < SPRAY_BELOW) {
		age = 4.0;
	}
	life -= age * FDT;

	vec3 far = vec3(1950.0) * DX;
	if (!all(equal(x, x)) || any(greaterThan(abs(x - ORIGIN), far))) {
		life = 0.0;
	}

	diffuse[id * 2u] = vec4(x, life);
	diffuse[id * 2u + 1u] = vec4(v, rad);
	// Foam shrinks over the last half of its mean life.
	float shrink = clamp(life / (0.5 * DLIFE), 0.0, 1.0);
	dout[id] = life > 0.0 ? vec4(x, rad * (0.35 + 0.65 * shrink)) : vec4(0.0);
}

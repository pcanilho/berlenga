#[compute]
#version 450

#include "mpm_block_inc.glsl"

// GPU marching tetrahedra over the block-sparse density field (surf_i). Indirect
// dispatch: one workgroup per active block, one thread per cell. Each cell is
// split into six tets sharing the body diagonal; triangles are appended
// non-indexed via an atomic counter, normals from the density gradient.
//
// Normals use cell_gradient() below -- the closed-form gradient of the cell's
// own trilinear density field, evaluated from the 8 corner densities already
// cached once per cell -- rather than independently resampling the world-space
// field via dsample_w()/block_lookup() at every vertex (the dense mpm_march.glsl
// still does that; it has no equivalent bug). That approach compiled fine on
// Turing but hangs compute_pipeline_create() outright on at least one confirmed
// RTX 5070/Blackwell + driver combination: block_lookup() contains a probe loop
// (up to PROBE_MAX=48 iters) plus a spin-wait (up to 1024 iters), and the old
// per-vertex dnormal() -> dsample_w() -> dsample() chain inlined it roughly
// 3000+ times per shader invocation (dnormal calls dsample_w 6x, each doing 8
// dsample() calls, times ~4 dnormal calls per triangle vertex/face-normal, times
// up to 18 unrolled write_tri call sites) -- enough to exceed the driver's PSO
// compile budget. cell_gradient() needs zero additional block_lookup() calls
// per vertex (pure arithmetic on the already-cached cv[8]), cutting total
// block_lookup() call sites from ~3000+ down to the unavoidable 8 (the initial
// per-cell corner fetch). A GL_EXT_control_flow_attributes [[dont_unroll]] hint
// on block_lookup()'s own loops was tried first and did NOT fix the hang --
// confirmed empirically, matching external reports that this NVIDIA driver
// compiler stage doesn't reliably respect such hints -- so this call-count
// reduction is the real fix, not a hint.
//
// Numerically this is not an approximation of the old approach -- it's exact:
// the analytic gradient of the same trilinear field the density values already
// define within this cell, vs. the old code's finite-difference estimate of it
// (which also incidentally blended slightly across cell boundaries via its
// e=DX*0.5 sample offset). Standard technique for GPU marching cubes normals.
//
// Smooth shading: a cell's own trilinear gradient jumps from cell to cell, so the
// surface shaded as facets. Normals now come from central-difference gradients at
// the grid nodes, interpolated trilinearly across the cell, as GPU marching cubes
// does (GPU Gems 3, ch. 1): neighbouring cells share their nodes' gradients, so the
// normal is continuous over the surface. The block's densities plus a two-node halo
// (9^3) are fetched once into shared memory by a loop with a single dsample() call
// site, so block_lookup() is still not inlined per vertex (see above), then smoothed
// once (see main()).

layout(set = 1, binding = 0, std430) restrict buffer MeshVerts {
	vec4 mverts[];
};
layout(set = 1, binding = 1, std430) restrict buffer MeshNorms {
	vec4 mnorms[];
};
layout(set = 1, binding = 2, std430) restrict buffer MeshCount {
	uint tri_count;
	uint tri_budget;
};

layout(local_size_x = BCELLS) in;

#define ISO (extra.z)

const ivec3 CORNER[8] = ivec3[8](
		ivec3(0, 0, 0), ivec3(1, 0, 0), ivec3(1, 1, 0), ivec3(0, 1, 0),
		ivec3(0, 0, 1), ivec3(1, 0, 1), ivec3(1, 1, 1), ivec3(0, 1, 1));
const int TET[24] = int[24](0, 1, 2, 6, 0, 2, 3, 6, 0, 3, 7, 6, 0, 7, 4, 6, 0, 4, 5, 6, 0, 5, 1, 6);
const int EA[6] = int[6](0, 0, 0, 1, 1, 2);
const int EB[6] = int[6](1, 2, 3, 2, 3, 3);
const int INC[12] = int[12](0, 1, 2, 0, 3, 4, 1, 3, 5, 2, 4, 5);

shared float sh_raw[729]; // this block's nodes -2..6 on each axis, as scattered
shared float sh_density[343]; // nodes -1..5, smoothed

float sh_at(ivec3 n) { // n: node relative to the block's first, -1..5
	ivec3 i = n + 1;
	return sh_density[(i.z * 7 + i.y) * 7 + i.x];
}

// Outward unit normal at local position fx in the cell, from the 8 corner gradients cg.
vec3 smooth_normal(vec3 fx, vec3 cg[8]) {
	vec3 g = mix(mix(mix(cg[0], cg[1], fx.x), mix(cg[3], cg[2], fx.x), fx.y),
			mix(mix(cg[4], cg[5], fx.x), mix(cg[7], cg[6], fx.x), fx.y), fx.z);
	float len = length(g);
	return len > 1e-6 ? -g / len : vec3(0.0, 1.0, 0.0); // outward = down-gradient
}

float dsample(ivec3 c) {
	int slot = block_lookup(c >> 2);
	if (slot < 0) {
		return 0.0;
	}
	ivec3 lc = c & 3;
	return float(surf_i[slot * BCELLS + (lc.z * 4 + lc.y) * 4 + lc.x]) / SURF_FIXED;
}

// Analytic gradient of this cell's own trilinear density field at local
// coordinates fx (each in [0,1]), given its 8 corner densities in CORNER
// order. No block_lookup() calls -- see the file header.
vec3 cell_gradient(vec3 fx, float cv[8]) {
	float gx = (cv[1] - cv[0]) * (1.0 - fx.y) * (1.0 - fx.z) + (cv[2] - cv[3]) * fx.y * (1.0 - fx.z) + (cv[5] - cv[4]) * (1.0 - fx.y) * fx.z + (cv[6] - cv[7]) * fx.y * fx.z;
	float gy = (cv[3] - cv[0]) * (1.0 - fx.x) * (1.0 - fx.z) + (cv[2] - cv[1]) * fx.x * (1.0 - fx.z) + (cv[7] - cv[4]) * (1.0 - fx.x) * fx.z + (cv[6] - cv[5]) * fx.x * fx.z;
	float gz = (cv[4] - cv[0]) * (1.0 - fx.x) * (1.0 - fx.y) + (cv[5] - cv[1]) * fx.x * (1.0 - fx.y) + (cv[7] - cv[3]) * (1.0 - fx.x) * fx.y + (cv[6] - cv[2]) * fx.x * fx.y;
	vec3 g = vec3(gx, gy, gz);
	float len = length(g);
	return len > 1e-6 ? -g / len : vec3(0.0, 1.0, 0.0); // outward = down-gradient
}

void write_tri(vec3 p0, vec3 p1, vec3 p2, vec3 f0, vec3 f1, vec3 f2, vec3 cg[8]) {
	float mx = DX * 3.0;
	if (distance(p0, p1) > mx || distance(p1, p2) > mx || distance(p2, p0) > mx) {
		return;
	}
	vec3 fn = cross(p1 - p0, p2 - p0);
	vec3 outn = smooth_normal((f0 + f1 + f2) * (1.0 / 3.0), cg);
	// Godot's front faces wind CLOCKWISE seen from outside: cross(p1 - p0, p2 - p0) must point INTO the
	// fluid, so a back-face-culled material shows the outside and DO_SIDE_CHECK keeps the outward normal.
	if (dot(fn, outn) > 0.0) {
		vec3 tp = p1;
		p1 = p2;
		p2 = tp;
		vec3 tf = f1;
		f1 = f2;
		f2 = tf;
	}
	uint slot = atomicAdd(tri_count, 1u);
	if (slot >= tri_budget) {
		return;
	}
	uint o = slot * 3u;
	mverts[o + 0] = vec4(p0, 0.0);
	mverts[o + 1] = vec4(p1, 0.0);
	mverts[o + 2] = vec4(p2, 0.0);
	mnorms[o + 0] = vec4(smooth_normal(f0, cg), 0.0);
	mnorms[o + 1] = vec4(smooth_normal(f1, cg), 0.0);
	mnorms[o + 2] = vec4(smooth_normal(f2, cg), 0.0);
}

void main() {
	uint bslot = gl_WorkGroupID.x;
	if (bslot >= bcounts[0]) {
		return;
	}
	uint lin = gl_LocalInvocationID.x;
	ivec3 bc = unpack_block(bkey[bslot]);
	ivec3 lc = ivec3(int(lin) & 3, (int(lin) >> 2) & 3, int(lin) >> 4);
	ivec3 cell = bc * BLK + lc;
	for (int i = int(lin); i < 729; i += BCELLS) {
		ivec3 n = ivec3(i % 9, (i / 9) % 9, i / 81) - 2;
		sh_raw[i] = dsample(bc * BLK + n);
	}
	barrier();
	// One binomial (1 2 1) pass each way over the scattered density before marching. The field
	// sampled from a sparse stream dips below the iso level between particles, which marched into
	// small closed bubbles inside the liquid: hidden on an opaque surface, but through clear water
	// they showed as bright cell-sized outlines, like cracked glass. The blur closes them and
	// smooths the surface; a feature thinner than a cell thins slightly with it.
	for (int i = int(lin); i < 343; i += BCELLS) {
		ivec3 n = ivec3(i % 7, (i / 7) % 7, i / 49) + 1; // into sh_raw's -2..6 indexing
		float sum = 0.0;
		for (int dz = -1; dz <= 1; dz++) {
			for (int dy = -1; dy <= 1; dy++) {
				for (int dx = -1; dx <= 1; dx++) {
					ivec3 m = n + ivec3(dx, dy, dz);
					float w = float((2 - abs(dx)) * (2 - abs(dy)) * (2 - abs(dz)));
					sum += w * sh_raw[(m.z * 9 + m.y) * 9 + m.x];
				}
			}
		}
		sh_density[i] = sum * (1.0 / 64.0);
	}
	barrier();

	float cv[8];
	vec3 cp[8];
	vec3 cg[8];
	for (int k = 0; k < 8; k++) {
		ivec3 n = lc + CORNER[k];
		cv[k] = sh_at(n);
		cp[k] = ORIGIN + vec3(cell + CORNER[k]) * DX;
		cg[k] = vec3(sh_at(n + ivec3(1, 0, 0)) - sh_at(n - ivec3(1, 0, 0)),
				sh_at(n + ivec3(0, 1, 0)) - sh_at(n - ivec3(0, 1, 0)),
				sh_at(n + ivec3(0, 0, 1)) - sh_at(n - ivec3(0, 0, 1)));
	}

	for (int t = 0; t < 6; t++) {
		int lcn[4] = int[4](TET[t * 4 + 0], TET[t * 4 + 1], TET[t * 4 + 2], TET[t * 4 + 3]);
		int mask = 0;
		for (int k = 0; k < 4; k++) {
			if (cv[lcn[k]] >= ISO) {
				mask |= 1 << k;
			}
		}
		if (mask == 0 || mask == 15) {
			continue;
		}

		vec3 ev[6];
		vec3 ef[6]; // local fx per edge, parallel to ev[] -- for smooth_normal()
		for (int e = 0; e < 6; e++) {
			int i = EA[e], j = EB[e];
			if (((mask >> i) & 1) != ((mask >> j) & 1)) {
				float vi = cv[lcn[i]], vj = cv[lcn[j]];
				float tt = clamp((ISO - vi) / (vj - vi + 1e-8), 0.0, 1.0);
				ev[e] = mix(cp[lcn[i]], cp[lcn[j]], tt);
				ef[e] = mix(vec3(CORNER[lcn[i]]), vec3(CORNER[lcn[j]]), tt);
			}
		}

		int inside = bitCount(mask);
		if (inside == 1 || inside == 3) {
			int lone = 0;
			for (int k = 0; k < 4; k++) {
				bool bit = ((mask >> k) & 1) != 0;
				if ((inside == 1) == bit) {
					lone = k;
				}
			}
			write_tri(ev[INC[lone * 3 + 0]], ev[INC[lone * 3 + 1]], ev[INC[lone * 3 + 2]],
					ef[INC[lone * 3 + 0]], ef[INC[lone * 3 + 1]], ef[INC[lone * 3 + 2]], cg);
		} else {
			int in0 = -1, in1 = -1, out0 = -1, out1 = -1;
			for (int k = 0; k < 4; k++) {
				if (((mask >> k) & 1) != 0) {
					if (in0 < 0) {
						in0 = k;
					} else {
						in1 = k;
					}
				} else {
					if (out0 < 0) {
						out0 = k;
					} else {
						out1 = k;
					}
				}
			}
			int e_ac = -1, e_ad = -1, e_bc = -1, e_bd = -1;
			for (int e = 0; e < 6; e++) {
				int a = EA[e], b = EB[e];
				if ((a == in0 && b == out0) || (a == out0 && b == in0)) {
					e_ac = e;
				}
				if ((a == in0 && b == out1) || (a == out1 && b == in0)) {
					e_ad = e;
				}
				if ((a == in1 && b == out0) || (a == out0 && b == in1)) {
					e_bc = e;
				}
				if ((a == in1 && b == out1) || (a == out1 && b == in1)) {
					e_bd = e;
				}
			}
			write_tri(ev[e_ac], ev[e_ad], ev[e_bd], ef[e_ac], ef[e_ad], ef[e_bd], cg);
			write_tri(ev[e_ac], ev[e_bd], ev[e_bc], ef[e_ac], ef[e_bd], ef[e_bc], cg);
		}
	}
}

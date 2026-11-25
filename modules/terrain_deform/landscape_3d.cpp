/**************************************************************************/
/*  landscape_3d.cpp                                                      */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2026-present Paulo Canilho and Berlenga contributors.    */
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/




#include "landscape_3d.h"

#include "terrain_generator_3d.h"

#include "modules/world/world_contracts.h"

#include "core/math/geometry_2d.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/templates/hash_set.h"
#include "core/templates/sort_array.h"
#include "scene/3d/camera_3d.h"
#include "scene/main/viewport.h"
#include "scene/resources/3d/world_3d.h"
#include "scene/scene_string_names.h"
#include "servers/physics_3d/physics_server_3d.h"
#include "servers/rendering/rendering_server.h"

#ifdef TOOLS_ENABLED
#include "editor/editor_interface.h"
#endif

// Frames an unused mesh (and so its BLAS) stays, in case the camera comes back.
static const uint64_t MESH_TTL = 600;
// Error of the levels below COARSE in a chunk not read in full: never picked.
static const float UNREAD = 1e30f;
// rebuild() reads before the first frame the chunks whose 4 m grid would be off by more than this many times
// lod_error there; the rest the view needs come in over the next frames, nearest first.
static const real_t FIRST_VIEW_SLACK = 4.0;

float Landscape3D::_h(int p_x, int p_z) const {
	return heights[CLAMP(p_z, 0, gh - 1) * gw + CLAMP(p_x, 0, gw - 1)];
}

// Central differences at 1 m. Where a chunk is not read yet its points between the 4 m grid are interpolated, and
// there this gives exactly the 4 m central difference at the 4 m points (the only ones its meshes use).
// At a crease (a dune's brink, a terrace edge, a cliff top: the slope changes by more than about 10 degrees between
// one side of the point and the other) the central difference averages the two faces, and the light rounds the
// edge over two metres. There each axis takes the one-sided difference from the smoother side instead (the
// essentially non-oscillatory choice, Harten et al. 1987: the stencil with the smaller second difference), so the
// faces next to an edge keep their own slope and the edge stays sharp in the shading as in the geometry.
static _FORCE_INLINE_ real_t _eno_slope(float a2, float a1, float c, float b1, float b2, real_t p_spacing) {
	const real_t back = c - a1, fwd = b1 - c;
	const real_t central = 0.5f * (back + fwd);
	const real_t kink = Math::abs(fwd - back) / p_spacing;
	if (kink < 0.18f) {
		return central;
	}
	const real_t eno = Math::abs(c - 2.0f * a1 + a2) <= Math::abs(b2 - 2.0f * b1 + c) ? back : fwd;
	const real_t t = CLAMP((kink - 0.18f) / (0.35f - 0.18f), real_t(0.0), real_t(1.0));
	return Math::lerp(central, eno, t * t * (3.0f - 2.0f * t));
}

Vector3 Landscape3D::_normal(int p_x, int p_z) const {
	const float c = _h(p_x, p_z);
	const real_t sx = _eno_slope(_h(p_x - 2, p_z), _h(p_x - 1, p_z), c, _h(p_x + 1, p_z), _h(p_x + 2, p_z), spacing);
	const real_t sz = _eno_slope(_h(p_x, p_z - 2), _h(p_x, p_z - 1), c, _h(p_x, p_z + 1), _h(p_x, p_z + 2), spacing);
	return Vector3(-sx, spacing, -sz).normalized();
}

// Indices of a chunk at p_level, in vertex rows of (n + 1), n = chunk_quads >> p_level. The grid is cut into 2 x 2
// cell blocks, each a fan of 8 triangles around its centre vertex; where p_mask flags a side whose neighbour is one
// level coarser (bit 0 -z, 1 +x, 2 +z, 3 -x), the blocks on that side skip its odd vertex and fan to the corners,
// which lie on the neighbour's coarser edge. Corner blocks with two flagged sides work the same way. All are made
// in rebuild(), so worker threads only read them.
const LocalVector<int> &Landscape3D::_indices(int p_level, int p_mask) {
	LocalVector<int> &r = indices[p_level][p_mask];
	if (!r.is_empty()) {
		return r;
	}
	const int n = chunk_quads >> p_level;
	const int row = n + 1;
	const int nb = n / 2;
	r.reserve(n * n * 6);
	for (int b = 0; b < nb; b++) {
		for (int a = 0; a < nb; a++) {
			const int x = 2 * a;
			const int z = 2 * b;
			const int c = (z + 1) * row + x + 1;
			int ring[8];
			int k = 0;
			ring[k++] = z * row + x;
			if (!(b == 0 && (p_mask & 1))) {
				ring[k++] = z * row + x + 1;
			}
			ring[k++] = z * row + x + 2;
			if (!(a == nb - 1 && (p_mask & 2))) {
				ring[k++] = (z + 1) * row + x + 2;
			}
			ring[k++] = (z + 2) * row + x + 2;
			if (!(b == nb - 1 && (p_mask & 4))) {
				ring[k++] = (z + 2) * row + x + 1;
			}
			ring[k++] = (z + 2) * row + x;
			if (!(a == 0 && (p_mask & 8))) {
				ring[k++] = (z + 1) * row + x;
			}
			for (int i = 0; i < k; i++) {
				r.push_back(ring[i]);
				r.push_back(ring[(i + 1) % k]);
				r.push_back(c);
			}
		}
	}
	return r;
}

// The drawn chunk's indices: _indices()' blocks, but where a block's sides all meet neighbours of its own level, its
// four quads are each cut along the diagonal whose ends differ least in height (the shorter diagonal in height,
// as a TIN follows breaklines; Garland and Heckbert 1995): a sharp crest or gully running across the grid followed
// the fan's fixed diagonals half the time and drew a sawtooth of 1 m teeth along every arete's skyline. Interior
// diagonals never cross a block's sides, so the edges shared with neighbours, and the cliff refinement's splits of
// them, are unchanged.
void Landscape3D::_crease_indices(int p_cx, int p_cz, int p_level, int p_mask, LocalVector<int> &r) const {
	const int n = chunk_quads >> p_level;
	const int s = 1 << p_level;
	const int row = n + 1;
	const int nb = n / 2;
	const int gx = p_cx * chunk_quads, gz = p_cz * chunk_quads;
	r.clear();
	r.reserve(n * n * 6);
	auto hv = [&](int x, int z) { return _h(gx + x * s, gz + z * s); };
	for (int b = 0; b < nb; b++) {
		for (int a = 0; a < nb; a++) {
			const int x = 2 * a;
			const int z = 2 * b;
			const bool edge = (b == 0 && (p_mask & 1)) || (a == nb - 1 && (p_mask & 2)) || (b == nb - 1 && (p_mask & 4)) || (a == 0 && (p_mask & 8));
			if (!edge) {
				for (int qz = z; qz < z + 2; qz++) {
					for (int qx = x; qx < x + 2; qx++) {
						const int v00 = qz * row + qx, v10 = v00 + 1, v01 = v00 + row, v11 = v01 + 1;
						if (Math::abs(hv(qx, qz) - hv(qx + 1, qz + 1)) <= Math::abs(hv(qx + 1, qz) - hv(qx, qz + 1))) {
							r.push_back(v00);
							r.push_back(v10);
							r.push_back(v11);
							r.push_back(v01);
							r.push_back(v00);
							r.push_back(v11);
						} else {
							r.push_back(v00);
							r.push_back(v10);
							r.push_back(v01);
							r.push_back(v10);
							r.push_back(v11);
							r.push_back(v01);
						}
					}
				}
				continue;
			}
			const int c = (z + 1) * row + x + 1;
			int ring[8];
			int k = 0;
			ring[k++] = z * row + x;
			if (!(b == 0 && (p_mask & 1))) {
				ring[k++] = z * row + x + 1;
			}
			ring[k++] = z * row + x + 2;
			if (!(a == nb - 1 && (p_mask & 2))) {
				ring[k++] = (z + 1) * row + x + 2;
			}
			ring[k++] = (z + 2) * row + x + 2;
			if (!(b == nb - 1 && (p_mask & 4))) {
				ring[k++] = (z + 2) * row + x + 1;
			}
			ring[k++] = (z + 2) * row + x;
			if (!(a == 0 && (p_mask & 8))) {
				ring[k++] = (z + 1) * row + x;
			}
			for (int i = 0; i < k; i++) {
				r.push_back(ring[i]);
				r.push_back(ring[(i + 1) % k]);
				r.push_back(c);
			}
		}
	}
}

// Height range and, per level, the largest gap between that level's triangles and the level 0 heights under them,
// for the (chunk_quads + 1)^2 heights at p_h, rows p_stride apart. Safe on worker threads.
void Landscape3D::_measure(const float *p_h, int p_stride, float *r_error, float &r_min, float &r_max) const {
	float lo = 1e30f, hi = -1e30f;
	for (int z = 0; z <= chunk_quads; z++) {
		for (int x = 0; x <= chunk_quads; x++) {
			const float h = p_h[z * p_stride + x];
			lo = MIN(lo, h);
			hi = MAX(hi, h);
		}
	}
	r_min = lo;
	r_max = hi;
	r_error[0] = 0.0f;
	for (int l = 1; l < levels; l++) {
		const int s = 1 << l;
		const int row = (chunk_quads >> l) + 1;
		const LocalVector<int> &idx = indices[l][0];
		float err = r_error[l - 1];
		for (uint32_t t = 0; t + 2 < idx.size(); t += 3) {
			int px[3], pz[3];
			float ph[3];
			for (int v = 0; v < 3; v++) {
				px[v] = (idx[t + v] % row) * s;
				pz[v] = (idx[t + v] / row) * s;
				ph[v] = p_h[pz[v] * p_stride + px[v]];
			}
			const float den = float((pz[1] - pz[2]) * (px[0] - px[2]) + (px[2] - px[1]) * (pz[0] - pz[2]));
			if (den == 0.0f) {
				continue;
			}
			const int bx0 = MIN(px[0], MIN(px[1], px[2])), bx1 = MAX(px[0], MAX(px[1], px[2]));
			const int bz0 = MIN(pz[0], MIN(pz[1], pz[2])), bz1 = MAX(pz[0], MAX(pz[1], pz[2]));
			for (int z = bz0; z <= bz1; z++) {
				for (int x = bx0; x <= bx1; x++) {
					const float wa = float((pz[1] - pz[2]) * (x - px[2]) + (px[2] - px[1]) * (z - pz[2])) / den;
					const float wb = float((pz[2] - pz[0]) * (x - px[2]) + (px[0] - px[2]) * (z - pz[2])) / den;
					const float wc = 1.0f - wa - wb;
					if (wa < -1e-5f || wb < -1e-5f || wc < -1e-5f) {
						continue;
					}
					err = MAX(err, Math::abs(wa * ph[0] + wb * ph[1] + wc * ph[2] - p_h[z * p_stride + x]));
				}
			}
		}
		r_error[l] = err;
	}
}

void Landscape3D::_measure_chunk(int p_cx, int p_cz) {
	Chunk &c = chunks[p_cz * cw + p_cx];
	_measure(heights.ptr() + size_t(p_cz) * chunk_quads * gw + size_t(p_cx) * chunk_quads, gw, c.error, c.min_h, c.max_h);
}

void Landscape3D::_measure_task(uint32_t p_index, void *p_userdata) {
	_measure_chunk(p_index % cw, p_index / cw);
}

// Mesh cache keys: the chunk's height generation, chunk index, level (3 bits), edge mask (9 bits: 4 coarser sides,
// cliff relief on, 4 sides whose neighbour has none).
uint64_t Landscape3D::_mesh_key(int p_chunk, int p_level, int p_mask) const {
	return (uint64_t(chunks[p_chunk].gen) << 36) | (uint64_t(p_chunk) << 12) | (uint64_t(p_level) << 9) | uint64_t(p_mask);
}

// Vertex data of one chunk mesh, ready for the renderer. Reads only the grid and the indices, so it runs on worker
// threads while the main thread leaves both alone.
// Cliff relief (cliff_relief). A heightfield has no vertices on a face steeper than its spacing: a 30 m wall is one
// strip of stretched triangles, whatever the shader draws on it. So at the finest level the triangles of steep faces
// are refined where an edge spans more height than the spacing (red-green refinement: an edge is split by its own
// end points only, so the two triangles on it, in this chunk or the next, always agree and nothing cracks), and every
// vertex is pushed out along the face's horizontal normal by a rock relief: beds 3 m thick that stand proud or sit
// back (differential erosion of strata; ledges), staggered joint blocks along them, and roughness. Displacement along
// the normal is how REDengine 3 (Gollent 2014) and Frostbite (Widmark 2012) break up terrain cliffs. Sides whose
// neighbour is coarser keep their vertices and edges as they are, so the stitching there is untouched.
static inline float _lc_hash(int32_t a, int32_t b, int32_t c) {
	uint32_t h = uint32_t(a) * 0x8DA6B343u ^ uint32_t(b) * 0xD8163841u ^ uint32_t(c) * 0xCB1AB31Fu;
	h ^= h >> 13;
	h *= 0x5BD1E995u;
	h ^= h >> 15;
	return float(h & 0xFFFFFFu) / float(0xFFFFFF);
}

static inline float _lc_noise(const Vector3 &p) {
	const Vector3 i = p.floor();
	Vector3 f = p - i;
	f = f * f * (Vector3(3, 3, 3) - 2.0f * f);
	const int32_t x = int32_t(i.x), y = int32_t(i.y), z = int32_t(i.z);
	auto hh = [&](int dx, int dy, int dz) { return _lc_hash(x + dx, y + dy, z + dz + 7919); };
	const float a = Math::lerp(Math::lerp(hh(0, 0, 0), hh(1, 0, 0), float(f.x)), Math::lerp(hh(0, 1, 0), hh(1, 1, 0), float(f.x)), float(f.y));
	const float b = Math::lerp(Math::lerp(hh(0, 0, 1), hh(1, 0, 1), float(f.x)), Math::lerp(hh(0, 1, 1), hh(1, 1, 1), float(f.x)), float(f.y));
	return Math::lerp(a, b, float(f.z));
}

Vector3 Landscape3D::_normal_central(int p_x, int p_z) const {
	const real_t sx = 0.5f * (_h(p_x + 1, p_z) - _h(p_x - 1, p_z));
	const real_t sz = 0.5f * (_h(p_x, p_z + 1) - _h(p_x, p_z - 1));
	return Vector3(-sx, spacing, -sz).normalized();
}

Vector3 Landscape3D::_normal_central_at(real_t p_x, real_t p_z) const {
	const int x = int(Math::floor(p_x)), z = int(Math::floor(p_z));
	const real_t u = p_x - x, v = p_z - z;
	return (_normal_central(x, z) * (1 - u) * (1 - v) + _normal_central(x + 1, z) * u * (1 - v) + _normal_central(x, z + 1) * (1 - u) * v + _normal_central(x + 1, z + 1) * u * v).normalized();
}

Vector3 Landscape3D::_normal_at(real_t p_x, real_t p_z) const {
	const int x = int(Math::floor(p_x)), z = int(Math::floor(p_z));
	const real_t u = p_x - x, v = p_z - z;
	return (_normal(x, z) * (1 - u) * (1 - v) + _normal(x + 1, z) * u * (1 - v) + _normal(x, z + 1) * (1 - u) * v + _normal(x + 1, z + 1) * u * v).normalized();
}

// Metres out of the face (negative: back into it) at a point of the landscape's own space, before cliff_relief.
// Joint steps (the face's fracture as relief, not paint). A rock face breaks along regional joint sets, planes of one
// orientation over a whole outcrop at spacings of a metre or two that scatter about their mean (Priest 1993; Hencher
// 2015): a gently dipping set cuts the face into blocks stacked down it, a steep set strikes across them. Each block's
// top stands out as a lip and its face leans back under it to the joint below, so the sun lights the lips and the
// faces under them fall into shade; the blocks' steps differ from one to the next across the steep set (their joint
// traces). Going down a face the steps only ever stand out, so they never overhang; the lean back is held to 0.75 of
// what would (p_steep: the relief scale times the face's tan(slope)). In offset units, as _cliff_offset.
static inline float _lc_joint_amp(int32_t p_blk, int32_t p_col) {
	const float h = _lc_hash(p_blk, p_col, 71);
	const float on = _lc_hash(p_blk, p_col, 73) > 0.3f ? 1.0f : 0.25f; // some blocks barely step
	return (0.25f + 0.6f * h * h) * on;
}
float Landscape3D::_joint_offset(const Vector3 &p, real_t p_steep) {
	const float SD = 2.4f, SJ = 2.6f; // mean spacings, metres
	// The dipping set: 7 degrees, toward a fixed regional direction; the planes wander a little.
	const float sd = float(p.y + 0.1f * p.x - 0.07f * p.z) / SD + 0.35f * (_lc_noise(p * 0.15f + Vector3(4, 4, 4)) - 0.5f);
	// The steep set, struck at 37 degrees.
	const float sj = float(0.799f * p.x + 0.602f * p.z) / SJ + 0.3f * (_lc_noise(p * 0.2f + Vector3(9, 2, 6)) - 0.5f);
	const int32_t cd = int32_t(Math::floor(sd));
	const float fr = sd - float(cd);
	// Uneven spacing: about half the gaps hold one more joint at a place of their own.
	const float b = 0.3f + 0.4f * _lc_hash(cd, 61, 5);
	int32_t blk = cd * 2;
	float f = fr, T = SD;
	if (_lc_hash(cd, 67, 5) > 0.45f) {
		if (fr < b) {
			f = fr / b;
			T = b * SD;
		} else {
			f = (fr - b) / (1.0f - b);
			T = (1.0f - b) * SD;
			blk += 1;
		}
	}
	const int32_t cj = int32_t(Math::floor(sj));
	const float gj = sj - float(cj);
	const float dist = MIN(gj, 1.0f - gj) * SJ;
	const float seam = 0.5f * (1.0f - Math::smoothstep(0.0f, 0.6f, dist));
	const float steep = MAX(float(p_steep), 0.05f);
	auto amp = [&](int32_t p_b, float p_t) {
		return MIN(Math::lerp(_lc_joint_amp(p_b, cj), _lc_joint_amp(p_b, gj < 0.5f ? cj - 1 : cj + 1), seam), 0.75f * p_t / steep);
	};
	const float A = amp(blk, T);
	// The block above and its step. The step at a joint is drawn over a lip at least 0.6 m tall (two of the finest
	// vertex rows, as the beds' lips): a step cut sharp was sampled by the vertex grid where the dipping planes cross
	// its rows, so the mesh stood as a comb of teeth along a face (aliasing of a discontinuous displacement), dark
	// spikes under the path tracer's exact shadows.
	int32_t blk_up;
	float T_up;
	if (blk == cd * 2 && _lc_hash(cd, 67, 5) > 0.45f) {
		blk_up = blk + 1;
		T_up = (1.0f - b) * SD;
	} else {
		const float b_up = 0.3f + 0.4f * _lc_hash(cd + 1, 61, 5);
		blk_up = (cd + 1) * 2;
		T_up = _lc_hash(cd + 1, 67, 5) > 0.45f ? b_up * SD : SD;
	}
	const float A_up = amp(blk_up, T_up);
	const float r = MIN(MAX(0.6f, 1.2f * 0.5f * (A + A_up)), 0.45f * T);
	const float u = f * T;
	if (u <= T - r) {
		return A * (f - 0.5f);
	}
	const float t = (u - (T - r)) / r;
	return Math::lerp(A * ((T - r) / T - 0.5f), -0.5f * A_up, t * t * (3.0f - 2.0f * t));
}

float Landscape3D::_cliff_offset(const Vector3 &p, real_t p_steep, real_t p_relief, bool p_joints) const {
	// Beds as a real sequence draws them (bed thickness in stratified rock is near log-normal; resistant beds stand out
	// as ledges, weak ones weather back: differential erosion). Each 12 m unit holds 1 to 5 beds of uneven
	// thickness; most beds sit near flush, about one in four is a hard one standing proud, and each bed's ledge fades
	// and breaks along the face (every 10-30 m) so no stripe runs on unbroken. The step to the next bed is rounded over the
	// top of the lower bed (a lit lip, not a saw cut; at least two vertex rows so it does not alias).
	const real_t U = 12.0;
	const real_t yb = p.y + (_lc_noise(p * 0.05f) - 0.5f) * 4.0f + (_lc_noise(p * 0.013f + Vector3(5, 0, 2)) - 0.5f) * 10.0f;
	const int32_t unit = int32_t(Math::floor(yb / U));
	const real_t fu0 = yb - unit * U;
	const int nb = 1 + int(_lc_hash(unit, 21, 4) * 4.999f);
	float wsum = 0.0f, wb[5];
	for (int i = 0; i < nb; i++) {
		// Log-normal weights: a few thick beds and several thin ones.
		wb[i] = Math::exp(1.1f * (_lc_hash(unit, i, 17) + _lc_hash(unit, i, 19) - 1.0f) * 1.7f);
		wsum += wb[i];
	}
	int bi = 0;
	real_t lo = 0.0, T = U;
	for (int i = 0; i < nb; i++) {
		const real_t th = U * wb[i] / wsum;
		if (fu0 < lo + th || i == nb - 1) {
			bi = i;
			T = th;
			break;
		}
		lo += th;
	}
	const int32_t bed = unit * 8 + bi;
	const int32_t bed_up = bi + 1 < nb ? bed + 1 : (unit + 1) * 8;
	// The bed under this one and its thickness (the last bed of the unit below for the first).
	int32_t bed_dn = bed - 1;
	real_t T_dn = 0.0;
	if (bi > 0) {
		T_dn = U * wb[bi - 1] / wsum;
	} else {
		const int nb_dn = 1 + int(_lc_hash(unit - 1, 21, 4) * 4.999f);
		float ws_dn = 0.0f, wl = 0.0f;
		for (int i = 0; i < nb_dn; i++) {
			wl = Math::exp(1.1f * (_lc_hash(unit - 1, i, 17) + _lc_hash(unit - 1, i, 19) - 1.0f) * 1.7f);
			ws_dn += wl;
		}
		bed_dn = (unit - 1) * 8 + nb_dn - 1;
		T_dn = U * wl / ws_dn;
	}
	const real_t fy = fu0 - lo;
	const auto proud = [&](int32_t p_bed) {
		const float hard = _lc_hash(p_bed, 11, 3);
		const float base = hard > 0.75f ? 0.6f + 1.4f * (hard - 0.75f) * 4.0f : (hard - 0.4f) * 0.9f;
		// Along the face the ledge swells, thins and breaks off.
		const float run = _lc_noise(Vector3(p.x * 0.06f, float(p_bed) * 1.37f, p.z * 0.06f));
		return base * Math::smoothstep(0.4f, 0.7f, run) * (0.6f + 0.8f * run);
	};
	// Joints across the beds, 4.5 m apart and staggered bed to bed, each block a little forward or back. Blended
	// into the next bed's blocks over the same lip as the beds: switched at the bed's top, the blocks' 1.2 m steps
	// cut a slit along every bed boundary.
	const auto joint = [&](int32_t p_bed) {
		const real_t u = p.x * 0.8f + p.z * 0.6f + _lc_hash(p_bed, 5, 1) * 4.5f;
		const int32_t jb = int32_t(Math::floor(u / 4.5f));
		const real_t fu = u - jb * 4.5f;
		const float j0 = _lc_hash(p_bed, jb, 9) - 0.5f, j1 = _lc_hash(p_bed, jb + 1, 9) - 0.5f;
		const real_t tu = CLAMP((fu - 3.0f) / 1.5f, real_t(0.0), real_t(1.0));
		return 1.2f * Math::lerp(j0, j1, float(tu * tu * (3.0f - 2.0f * tu)));
	};
	const float a_dn = proud(bed_dn) + joint(bed_dn), a0 = proud(bed) + joint(bed), a1 = proud(bed_up) + joint(bed_up);
	// The step between two beds is rounded over a lip at least 1.2 times as tall as the step (in offset units), and
	// on a face of slope tan t with the relief scaled by k (p_steep = k tan t) tall enough that the step's steepest
	// part leans out no further than the face leans back (1.5 k d tan t / lip <= 1.1, the smoothstep's peak slope
	// 1.5 times its mean): steeper, the bed above overhangs the one below and its underside, facing the ground, is a
	// black slit under every ledge in raster (and the grid's columns fold through it). The lip sits in the top of the
	// lower bed; where that bed is too thin for it, the rest of the step carries on into the bottom of the bed above
	// (up to most of it), so the step keeps its slope instead of steepening into an overhang.
	const real_t steep = MIN(p_steep, real_t(6.0) * MAX(p_relief, real_t(0.0)));
	const auto lip_of = [&](real_t p_t, float p_d, real_t &r_low, real_t &r_up) {
		const real_t req = MAX(MIN(real_t(1.5), MAX(real_t(0.6), p_t * 0.5f)), MAX(real_t(1.2f * Math::abs(p_d)), real_t(2.6f) * steep * Math::abs(p_d)));
		r_low = MIN(p_t, req);
		r_up = req - r_low;
		return req;
	};
	float o = a0;
	{
		// From the bed below: the part of its step that did not fit in its top.
		real_t lo_dn, up_dn;
		const real_t req = lip_of(T_dn, a0 - a_dn, lo_dn, up_dn);
		if (up_dn > 0.0) {
			const float carry = a_dn + (a0 - a_dn) * float(lo_dn / req);
			const real_t lip_up = MIN(up_dn, T * 0.9f);
			const real_t tb = CLAMP(fy / lip_up, real_t(0.0), real_t(1.0));
			o = Math::lerp(carry, a0, float(tb * tb * (3.0f - 2.0f * tb)));
		}
	}
	{
		// Toward the bed above, over the top of this one.
		real_t lip, up;
		const real_t req = lip_of(T, a1 - a0, lip, up);
		const float target = up > 0.0 ? a0 + (a1 - a0) * float(lip / req) : a1;
		const real_t ty = CLAMP((fy - (T - lip)) / lip, real_t(0.0), real_t(1.0));
		o = Math::lerp(o, target, float(ty * ty * (3.0f - 2.0f * ty)));
	}
	// Roughness.
	// On the steepest faces its bumps are drawn out 4 times down the fall line (flutes, as rain and frost groove a
	// steep wall): at full height their upper sides leaned out past the face and showed undersides as black slits.
	const Vector3 pf(p.x, p.y * 0.25f, p.z);
	const float fl = Math::smoothstep(0.25f, 0.7f, float(steep));
	o += Math::lerp(0.5f * (_lc_noise(p * 0.7f) - 0.5f) + 0.2f * (_lc_noise(p * 2.1f + Vector3(3, 1, 7)) - 0.5f),
			0.5f * (_lc_noise(pf * 0.7f) - 0.5f) + 0.2f * (_lc_noise(pf * 2.1f + Vector3(3, 1, 7)) - 0.5f), fl);
	if (p_joints) {
		o += _joint_offset(p, p_steep);
	}
	return o;
}

// Bank relief (bank_relief). Soil banks of 40 to 55 degrees do not keep beds and joints: they slump. Shallow slumps
// and translational slides leave a head scarp (a hollow under a step) and a toe lobe (a bulge) below it, bodies 5 to
// 20 m wide (Varnes 1978; Cruden and Varnes 1996; Highland and Bobrowsky 2008), with hummocks over them. Slump units 6 m
// tall run along the contour (height warped so no line stays level), each present over a stretch and broken off
// along the bank; the profile is zero at each unit's ends, so the field is continuous. Metres out of the bank.
float Landscape3D::_bank_offset(const Vector3 &p) const {
	const real_t U = 6.0;
	const real_t yb = p.y + (_lc_noise(p * 0.07f + Vector3(11, 3, 5)) - 0.5f) * 6.0f;
	const int32_t unit = int32_t(Math::floor(yb / U));
	const float t = float(yb / U - unit);
	const float strength = 0.4f + 0.6f * _lc_hash(unit, 31, 7);
	const float run = _lc_noise(Vector3(p.x * 0.09f, float(unit) * 1.71f, p.z * 0.09f));
	const float amp = strength * Math::smoothstep(0.35f, 0.65f, run);
	const float tl = (t - 0.25f) / 0.17f, ts = (t - 0.78f) / 0.12f;
	const float lobe = Math::exp(-tl * tl);
	const float scarp = Math::exp(-ts * ts);
	const float ends = Math::smoothstep(0.0f, 0.1f, t) * (1.0f - Math::smoothstep(0.9f, 1.0f, t));
	float o = amp * (0.9f * lobe - scarp) * ends;
	// Hummocks.
	o += 0.35f * (_lc_noise(p * 0.35f + Vector3(2, 9, 4)) - 0.5f) + 0.15f * (_lc_noise(p * 1.1f + Vector3(7, 2, 8)) - 0.5f);
	return o;
}

Vector2 Landscape3D::_relief_weights(const Vector3 &n) {
	// Cliffs past about 50 degrees, full past about 62; banks from about 40 (full by 50), handing over to the cliff.
	const real_t wc = 1.0f - CLAMP((n.y - 0.47f) / (0.64f - 0.47f), real_t(0.0), real_t(1.0));
	const real_t wb = 1.0f - CLAMP((n.y - 0.643f) / (0.766f - 0.643f), real_t(0.0), real_t(1.0));
	const real_t cs = wc * wc * (3.0f - 2.0f * wc);
	const real_t bs = wb * wb * (3.0f - 2.0f * wb);
	return Vector2(cs, bs * (1.0f - cs));
}

// The relief's weights at a grid point, tent-filtered over 1.5 times the relief's reach either way. Pushed out by w d,
// the surface folds where d dw/ds < -1 across the face, and the weights went from cliff to none within one cell at a
// sharp cliff foot (the slope eases from 62 to 50 degrees in a metre there): a fold along the foot drawn as black
// dashes. Spread over some 10 m (relief 1 m), the fade's slope stays under a sixth of the reach's.
Vector2 Landscape3D::_relief_weights_at(real_t p_fx, real_t p_fz) const {
	const real_t r = 1.5f * 3.5f * MAX(cliff_relief, bank_relief) / spacing * 0.5f; // cells between taps
	if (r < 0.25f) {
		return _relief_weights(_normal_at(p_fx, p_fz));
	}
	// 3 x 3 taps 2 r apart (a [1 2 1] tent): the same reach as 5 x 5 r apart for a third of the reads (relief_at is
	// read per plant and stone).
	static const real_t tent[3] = { 1.0f, 2.0f, 1.0f };
	Vector2 w;
	for (int j = 0; j < 3; j++) {
		for (int i = 0; i < 3; i++) {
			const real_t fx = CLAMP(p_fx + (i - 1) * 2.0f * r, real_t(0.0), real_t(gw - 1));
			const real_t fz = CLAMP(p_fz + (j - 1) * 2.0f * r, real_t(0.0), real_t(gh - 1));
			w += _relief_weights(_normal_at(fx, fz)) * (tent[i] * tent[j]);
		}
	}
	return w / 16.0f;
}

// The relief's scale where the slope's horizontal normal converges or spreads in plan (a gully's inside, a spur's
// nose). Pushed out along that normal by d, a contour's length scales by 1 + d k, k the plan divergence of the unit
// horizontal normal (1 / the contour's radius); past -1 the surface folds through itself and shows its back faces as
// black slits and pockets in a tight gully or behind a spur. Relief here reaches about 3.5 times cliff_relief either
// way, so it is scaled to keep 1 + d k above 0.3 (displacement mapping's self-intersection bound; the same limit an
// offset curve keeps to stay simple: d under the radius of curvature).
real_t Landscape3D::_relief_fold_scale(real_t p_x, real_t p_z) const {
	const real_t reach = 3.5f * MAX(cliff_relief, bank_relief);
	if (reach <= 0.0) {
		return 1.0;
	}
	const auto unit_m = [&](real_t x, real_t z) {
		const Vector3 n = _normal_at(x, z);
		const Vector2 m(n.x, n.z);
		const real_t l = m.length();
		return l < 1e-4f ? Vector2() : m / l;
	};
	const real_t k = (unit_m(p_x + 1.0f, p_z).x - unit_m(p_x - 1.0f, p_z).x + unit_m(p_x, p_z + 1.0f).y - unit_m(p_x, p_z - 1.0f).y) / (2.0f * spacing);
	return MIN(real_t(1.0), real_t(0.7f) / (reach * Math::abs(k) + real_t(1e-6f)));
}

// The cliff relief's scale at a point, k, eased where the beds' offset changes too fast with height for the face:
// pushed out by k o(y) on a face of slope tan t, the surface overhangs where k |do/dy| tan t > 1, and the underside
// of every such step (most often a thin bed between two others, too thin for the lip rule in _cliff_offset to round
// the step over) drew as a black slit, or on a gully's steep banks as tall shards. The steepest change of o over
// 2 m above and below (sampled every 0.4 m, tapered with distance so the scale varies slowly) sets k no higher than 0.8 / (tan t max |o'|).
// A function of the position only, so chunks agree on shared vertices; the meshes hand it to the material in CUSTOM0.
real_t Landscape3D::_cliff_scale(const Vector3 &p, real_t p_k, real_t p_tan) const {
	if (p_k <= 0.0) {
		return 0.0;
	}
	return MIN(p_k, _cliff_scale_limit(p, p_tan));
}

real_t Landscape3D::_cliff_scale_limit(const Vector3 &p, real_t p_tan) const {
	const real_t tn = MIN(p_tan, real_t(6.0));
	// Down the surface's own fall line: outward along the slope's horizontal normal m by 0.4 / tn while 0.4 m down, so
	// the offset's change across the face (its noise and joints) counts with its change in height: the surface folds
	// where 1 + k do/ds < 0, s the distance across the face in plan.
	const Vector3 nh = _normal_at((p.x - origin.x) / spacing, (p.z - origin.y) / spacing);
	const Vector2 mh = Vector2(nh.x, nh.z).normalized();
	const Vector3 step(mh.x * 0.4f / MAX(tn, real_t(0.1)), -0.4f, mh.y * 0.4f / MAX(tn, real_t(0.1)));
	// Over 2 m either way, each step's slope weighed down with its distance (a tent): with a plain window the scale
	// dropped by two thirds within 10 cm where a bed's step came into reach, itself a fold (black dashes along a
	// cliff's foot); tapered, it eases in as the step nears.
	real_t prev = 0.0, g = 0.0;
	for (int i = -5; i <= 5; i++) {
		const Vector3 q = p + step * real_t(i);
		// Without the joint steps: their lean back keeps within its own share by itself (_joint_offset).
		const real_t o = _cliff_offset(q, cliff_relief * tn, cliff_relief, false);
		if (i > -5) {
			// Only a fall in the offset going down and out folds: a step out (a joint's lip) is safe however sharp.
			g = MAX(g, MAX(prev - o, real_t(0.0)) / 0.4f * (1.0f - Math::abs(real_t(i) - 0.5f) / 5.5f));
		}
		prev = o;
	}
	return real_t(0.8) / MAX(tn * g, real_t(1e-4));
}

// The bank relief's scale at a point, as _cliff_scale for the cliffs: a slump's head scarp falls 0.4 to 1 of its
// amplitude over some 0.7 m of height, and on a bank of 50 degrees or past (the hand-over to a cliff, a gully's
// side) the scarp's foot then overhangs: black dashes along a cliff's foot and shards on a gully's banks, with the
// cliff relief off too. The steepest change of the offset over 1.2 m above and below sets the scale no higher than
// 0.6 / (tan t max |o'|) (less than the cliff's 0.8: where both weigh in, their slopes add).
real_t Landscape3D::_bank_scale(const Vector3 &p, real_t p_k, real_t p_tan) const {
	if (p_k <= 0.0) {
		return 0.0;
	}
	return MIN(p_k, _bank_scale_limit(p, p_tan));
}

real_t Landscape3D::_bank_scale_limit(const Vector3 &p, real_t p_tan) const {
	const real_t tn = MIN(p_tan, real_t(6.0));
	// Down the surface's own fall line: outward along the slope's horizontal normal m by 0.4 / tn while 0.4 m down, so
	// the offset's change across the face (its noise and joints) counts with its change in height: the surface folds
	// where 1 + k do/ds < 0, s the distance across the face in plan.
	const Vector3 nh = _normal_at((p.x - origin.x) / spacing, (p.z - origin.y) / spacing);
	const Vector2 mh = Vector2(nh.x, nh.z).normalized();
	const Vector3 step(mh.x * 0.4f / MAX(tn, real_t(0.1)), -0.4f, mh.y * 0.4f / MAX(tn, real_t(0.1)));
	// Over 2 m either way, each step's slope weighed down with its distance (a tent): with a plain window the scale
	// dropped by two thirds within 10 cm where a bed's step came into reach, itself a fold (black dashes along a
	// cliff's foot); tapered, it eases in as the step nears.
	real_t prev = 0.0, g = 0.0;
	for (int i = -5; i <= 5; i++) {
		const Vector3 q = p + step * real_t(i);
		const real_t o = _bank_offset(q);
		if (i > -5) {
			// Only a fall in the offset going down and out folds: a step out (a joint's lip) is safe however sharp.
			g = MAX(g, MAX(prev - o, real_t(0.0)) / 0.4f * (1.0f - Math::abs(real_t(i) - 0.5f) / 5.5f));
		}
		prev = o;
	}
	return real_t(0.6) / MAX(tn * g, real_t(1e-4));
}

// The displaced point for a point on the heightfield surface (landscape space).
Vector3 Landscape3D::_cliff_point(const Vector3 &p, real_t p_weight_scale) const {
	ReliefFrame frame;
	return _cliff_point_framed(p, p_weight_scale, frame);
}

void Landscape3D::_relief_frame_weights(real_t p_fx, real_t p_fz, ReliefFrame &r_frame) const {
	if (!r_frame.has_w) {
		r_frame.w = _relief_weights_at(p_fx, p_fz);
		r_frame.fold = _relief_fold_scale(p_fx, p_fz);
		r_frame.has_w = true;
	}
}

// As _cliff_point, keeping what it read at the point in r_frame (and reading only what r_frame lacks), so the mesh
// build's other reads at a vertex (its CUSTOM channels) come from the same values instead of a second evaluation.
Vector3 Landscape3D::_cliff_point_framed(const Vector3 &p, real_t p_weight_scale, ReliefFrame &r_frame) const {
	const real_t fx = (p.x - origin.x) / spacing, fz = (p.z - origin.y) / spacing;
	if (!r_frame.has_n) {
		r_frame.n = _normal_at(fx, fz);
		r_frame.ml = Vector2(r_frame.n.x, r_frame.n.z).length();
		r_frame.has_n = true;
	}
	const Vector3 n = r_frame.n;
	const Vector2 m(n.x, n.z);
	const real_t ml = r_frame.ml;
	if (ml < 1e-4f || p_weight_scale <= 0.0) {
		return p;
	}
	_relief_frame_weights(fx, fz, r_frame);
	const Vector2 w = r_frame.w * (p_weight_scale * r_frame.fold);
	real_t d = 0.0;
	if (w.x > 0.0 && cliff_relief > 0.0) {
		const real_t tn = ml / MAX(n.y, real_t(1e-3));
		if (r_frame.cliff_limit < 0.0) {
			r_frame.cliff_limit = _cliff_scale_limit(p, tn);
		}
		const real_t k = MIN(cliff_relief * w.x, r_frame.cliff_limit); // _cliff_scale (its p_k is over 0 here)
		d += k * _cliff_offset(p, cliff_relief * tn, cliff_relief, false); // the lip rule at the full scale, as _cliff_scale measured it
		// The joint steps at the face's own scale: they never overhang, so the easing for the rest leaves them be.
		d += cliff_relief * w.x * _joint_offset(p, cliff_relief * tn);
	}
	if (w.y > 0.0 && bank_relief > 0.0) {
		if (r_frame.bank_limit < 0.0) {
			r_frame.bank_limit = _bank_scale_limit(p, ml / MAX(n.y, real_t(1e-3)));
		}
		d += MIN(bank_relief * w.y, r_frame.bank_limit) * _bank_offset(p); // _bank_scale
	}
	return p + Vector3(m.x / ml, 0.0, m.y / ml) * d;
}

Vector3 Landscape3D::get_cliff_point(const Vector3 &p_global_position) const {
	// Where the drawn surface is over a point of the heightfield surface (global space): the point itself unless
	// cliff_relief pushes it out of or into a steep face. For water laid on a face (falls, sheets).
	RWLockRead hl(heights_lock);
	if ((cliff_relief <= 0.0 && bank_relief <= 0.0) || heights.is_empty()) {
		return p_global_position;
	}
	const Transform3D xf = is_inside_tree() ? get_global_transform() : Transform3D();
	return xf.xform(_cliff_point(xf.affine_inverse().xform(p_global_position), 1.0));
}

void Landscape3D::_build_cliff_surface(MeshJob &r_job) const {
	const int cx = r_job.chunk % cw;
	const int cz = r_job.chunk / cw;
	const int n = chunk_quads;
	const int row = n + 1;
	const Vector3 corner(origin.x + cx * chunk_quads * spacing, 0.0, origin.y + cz * chunk_quads * spacing);
	LocalVector<Vector3> base; // landscape space
	LocalVector<real_t> keep; // 1 = free to move, 0 = on a side stitched to a coarser neighbour
	base.resize(row * row);
	keep.resize(row * row);
	for (int j = 0; j <= n; j++) {
		for (int i = 0; i <= n; i++) {
			const int x = cx * chunk_quads + i, z = cz * chunk_quads + j;
			base[j * row + i] = Vector3(origin.x + x * spacing, _h(x, z), origin.y + z * spacing);
			keep[j * row + i] = 0.0; // set below from the position
		}
	}
	// 1 = free to move, 0 on a side stitched to a coarser neighbour (or one without relief), easing in over the
	// three cells in from it: a step from no relief on the side's row to full relief on the next drew a strip
	// along the chunk's side that leaned out or in by the whole offset in one cell, its underside a long black slit.
	const int keep_fm = r_job.mask | (r_job.mask >> 5);
	auto keep_at = [&](const Vector3 &q) -> real_t {
		const real_t u = (q.x - corner.x) / spacing, v = (q.z - corner.z) / spacing;
		real_t d = 1e9;
		if (keep_fm & 1) {
			d = MIN(d, v);
		}
		if (keep_fm & 2) {
			d = MIN(d, real_t(n) - u);
		}
		if (keep_fm & 4) {
			d = MIN(d, real_t(n) - v);
		}
		if (keep_fm & 8) {
			d = MIN(d, u);
		}
		const real_t t = CLAMP(d / 3.0f, real_t(0.0), real_t(1.0));
		return t * t * (3.0f - 2.0f * t);
	};
	for (uint32_t i = 0; i < base.size(); i++) {
		keep[i] = keep_at(base[i]);
	}
	auto on_fixed_side = [&](const Vector3 &a, const Vector3 &b) {
		const real_t x0 = corner.x, x1 = corner.x + n * spacing, z0 = corner.z, z1 = corner.z + n * spacing;
		const real_t e = spacing * 1e-3f;
		const int fm = r_job.mask | (r_job.mask >> 5);
		return ((fm & 1) && Math::abs(a.z - z0) < e && Math::abs(b.z - z0) < e) || ((fm & 2) && Math::abs(a.x - x1) < e && Math::abs(b.x - x1) < e) || ((fm & 4) && Math::abs(a.z - z1) < e && Math::abs(b.z - z1) < e) || ((fm & 8) && Math::abs(a.x - x0) < e && Math::abs(b.x - x0) < e);
	};
	// Displacement of each vertex (from its position only, so both chunks on an edge agree), read once.
	LocalVector<Vector3> disp;
	LocalVector<uint8_t> has_disp;
	disp.resize(base.size());
	has_disp.resize(base.size());
	memset(has_disp.ptr(), 0, has_disp.size());
	auto disp_of = [&](int a) -> Vector3 {
		if (!has_disp[a]) {
			disp[a] = _cliff_point(base[a], 1.0) - base[a];
			has_disp[a] = 1;
		}
		return disp[a];
	};
	// Along a face the relief steps at each ledge's lip and each joint: an edge is split too where the relief at its
	// middle strays from the straight line between its ends (error-driven tessellation of a displacement, as adaptive
	// displacement mapping does, Moreton 2001; Nießner et al. 2016), so a lip and the shadow it casts follow vertices
	// placed along it instead of zig-zagging across the triangles.
	// An edge is shared by two triangles (and tested again at each depth it stays whole): the test runs once per edge,
	// and the displacement of its middle is kept for the vertex a split puts there (the same point).
	HashMap<uint64_t, uint8_t> split_done;
	HashMap<uint64_t, Vector3> mid_disp;
	auto edge_key = [](int a, int b) -> uint64_t {
		return a < b ? (uint64_t(a) << 32 | uint32_t(b)) : (uint64_t(b) << 32 | uint32_t(a));
	};
	auto split_test = [&](int a, int b) -> bool {
		if (on_fixed_side(base[a], base[b])) {
			return false;
		}
		const real_t dy = Math::abs(base[a].y - base[b].y);
		if (dy > 1.0f * spacing) {
			return true;
		}
		if (Vector2(base[a].x - base[b].x, base[a].z - base[b].z).length() < 0.3f * spacing && dy < 0.3f * spacing) {
			return false;
		}
		const Vector3 da = disp_of(a), db = disp_of(b);
		if (da.is_zero_approx() && db.is_zero_approx()) {
			return false;
		}
		const Vector3 pm = (base[a] + base[b]) * 0.5f;
		const Vector3 dm = _cliff_point(pm, 1.0) - pm;
		mid_disp.insert(edge_key(a, b), dm);
		return (dm - (da + db) * 0.5f).length() > 0.08f;
	};
	auto split = [&](int a, int b) -> bool {
		const uint64_t k = edge_key(a, b);
		if (const uint8_t *f = split_done.getptr(k)) {
			return *f != 0;
		}
		const bool s = split_test(a, b);
		split_done.insert(k, s ? 1 : 0);
		return s;
	};
	HashMap<uint64_t, int> mids;
	auto mid = [&](int a, int b) -> int {
		const uint64_t k = edge_key(a, b);
		if (const int *f = mids.getptr(k)) {
			return *f;
		}
		const int id = int(base.size());
		base.push_back((base[a] + base[b]) * 0.5f);
		keep.push_back(keep_at((base[a] + base[b]) * 0.5f));
		const Vector3 *dm = mid_disp.getptr(k);
		disp.push_back(dm ? *dm : Vector3());
		has_disp.push_back(dm ? 1 : 0);
		mids.insert(k, id);
		return id;
	};
	LocalVector<int> tris;
	struct T {
		int a, b, c, depth;
	};
	LocalVector<T> stack;
	LocalVector<int> idx;
	_crease_indices(cx, cz, 0, r_job.mask & 15, idx);
	const LocalVector<uint8_t> *hl = chunk_holes.getptr(r_job.chunk);
	for (uint32_t t = 0; t < idx.size(); t += 3) {
		if (hl && _hole_tri(hl, 0, idx[t], idx[t + 1], idx[t + 2])) {
			continue; // a hole: refinement splits edges by their end points only, so the neighbours are unaffected
		}
		stack.push_back({ idx[t], idx[t + 1], idx[t + 2], 0 });
		while (!stack.is_empty()) {
			const T tr = stack[stack.size() - 1];
			stack.remove_at(stack.size() - 1);
			int v[3] = { tr.a, tr.b, tr.c };
			bool s[3] = { false, false, false };
			int cnt = 0;
			if (tr.depth < 6) {
				for (int e = 0; e < 3; e++) {
					s[e] = split(v[e], v[(e + 1) % 3]);
					cnt += s[e] ? 1 : 0;
				}
			}
			if (cnt == 0) {
				tris.push_back(v[0]);
				tris.push_back(v[1]);
				tris.push_back(v[2]);
				continue;
			}
			const int d = tr.depth + 1;
			if (cnt == 3) {
				const int ab = mid(v[0], v[1]), bc = mid(v[1], v[2]), ca = mid(v[2], v[0]);
				stack.push_back({ v[0], ab, ca, d });
				stack.push_back({ ab, v[1], bc, d });
				stack.push_back({ ca, bc, v[2], d });
				stack.push_back({ ab, bc, ca, d });
				continue;
			}
			// Rotate so the split edges start at the first: one split edge is v0-v1, two are v0-v1 and v1-v2.
			int r = 0;
			if (cnt == 1) {
				r = s[0] ? 0 : (s[1] ? 1 : 2);
			} else {
				r = !s[2] ? 0 : (!s[0] ? 1 : 2);
			}
			const int a = v[r], b = v[(r + 1) % 3], c = v[(r + 2) % 3];
			if (cnt == 1) {
				const int m = mid(a, b);
				stack.push_back({ a, m, c, d });
				stack.push_back({ m, b, c, d });
			} else {
				const int ab = mid(a, b), bc = mid(b, c);
				stack.push_back({ ab, b, bc, d });
				stack.push_back({ a, ab, bc, d });
				stack.push_back({ a, bc, c, d });
			}
		}
	}
	// Close the T junctions: a triangle at the depth cap keeps whole an edge its shallower neighbour split, and the
	// split's middle vertex, displaced, opened a crack in the face there (a black slit at the foot of a wall). Every
	// triangle is cut at each midpoint its edges carry, down to the finest (the mids are final now), so the mesh is
	// conforming.
	{
		LocalVector<int> conf;
		conf.reserve(tris.size());
		LocalVector<int> todo;
		for (uint32_t t = 0; t < tris.size(); t += 3) {
			todo.push_back(tris[t]);
			todo.push_back(tris[t + 1]);
			todo.push_back(tris[t + 2]);
			while (!todo.is_empty()) {
				const int c2 = todo[todo.size() - 1], b2 = todo[todo.size() - 2], a2 = todo[todo.size() - 3];
				todo.resize(todo.size() - 3);
				const int v[3] = { a2, b2, c2 };
				bool cut = false;
				for (int e = 0; e < 3 && !cut; e++) {
					const int a = v[e], b = v[(e + 1) % 3], c = v[(e + 2) % 3];
					const uint64_t k = a < b ? (uint64_t(a) << 32 | uint32_t(b)) : (uint64_t(b) << 32 | uint32_t(a));
					if (const int *m = mids.getptr(k)) {
						todo.push_back(a);
						todo.push_back(*m);
						todo.push_back(c);
						todo.push_back(*m);
						todo.push_back(b);
						todo.push_back(c);
						cut = true;
					}
				}
				if (!cut) {
					conf.push_back(a2);
					conf.push_back(b2);
					conf.push_back(c2);
				}
			}
		}
		tris = conf;
	}
	// Displace, and the normal from the displaced surface around each point (seamless: it reads positions only).
	PackedVector3Array verts;
	PackedVector3Array norms;
	// For the material's per-pixel relief normal (TerrainMaterial3D, cliff relief): CUSTOM0 the undisplaced point in
	// landscape space and the displacement scale (cliff_relief times the face weight), CUSTOM1 the heightfield normal.
	// Both are linear over each triangle of the undisplaced surface, so the pixel gets them exactly; the vertex
	// normals below only light what the triangles can carry, and the ledges' edges came out saw-toothed.
	PackedFloat32Array cus0, cus1;
	verts.resize(base.size());
	norms.resize(base.size());
	cus0.resize(base.size() * 4);
	cus1.resize(base.size() * 4);
	// CUSTOM2.x: the joint steps' scale (_joint_offset); .yzw: the heightfield's central-difference normal, for the
	// material's texture projection only. The ENO normals (_normal) take the flatter side at a kink, so on a wall one
	// cell across both its rows get flat normals and the material projected its textures from above down the whole
	// face (long vertical smears); the central difference sees the wall.
	PackedFloat32Array cus2;
	cus2.resize(base.size() * 4);
	Vector3 *vp = verts.ptrw();
	Vector3 *nw = norms.ptrw();
	float *c0 = cus0.ptrw();
	float *c1 = cus1.ptrw();
	float *c2 = cus2.ptrw();
	const real_t e = 0.3f;
	for (uint32_t i = 0; i < base.size(); i++) {
		const Vector3 p = base[i];
		ReliefFrame frame; // the reads below at p, each once
		vp[i] = _cliff_point_framed(p, keep[i], frame) - corner;
		_relief_frame_weights((p.x - origin.x) / spacing, (p.z - origin.y) / spacing, frame);
		const Vector3 nb = frame.n;
		const Vector2 w = frame.w * frame.fold;
		{
			const real_t ml = Vector2(nb.x, nb.z).length();
			const real_t tn = ml / MAX(nb.y, real_t(1e-3));
			c0[i * 4 + 0] = p.x;
			c0[i * 4 + 1] = p.y;
			c0[i * 4 + 2] = p.z;
			// _cliff_scale and _bank_scale, their bounds from the frame.
			const real_t kc = cliff_relief * w.x * keep[i];
			if (ml < 1e-4f || kc <= 0.0) {
				c0[i * 4 + 3] = 0.0f;
			} else {
				if (frame.cliff_limit < 0.0) {
					frame.cliff_limit = _cliff_scale_limit(p, tn);
				}
				c0[i * 4 + 3] = float(MIN(kc, frame.cliff_limit));
			}
			c1[i * 4 + 0] = nb.x;
			c1[i * 4 + 1] = nb.y;
			c1[i * 4 + 2] = nb.z;
			const real_t kb = bank_relief * w.y * keep[i];
			if (ml < 1e-4f || kb <= 0.0) {
				c1[i * 4 + 3] = 0.0f;
			} else {
				if (frame.bank_limit < 0.0) {
					frame.bank_limit = _bank_scale_limit(p, tn);
				}
				c1[i * 4 + 3] = float(MIN(kb, frame.bank_limit));
			}
			c2[i * 4 + 0] = ml < 1e-4f ? 0.0f : float(cliff_relief * w.x * keep[i]);
			const Vector3 np = _normal_central_at((p.x - origin.x) / spacing, (p.z - origin.y) / spacing);
			c2[i * 4 + 1] = np.x;
			c2[i * 4 + 2] = np.y;
			c2[i * 4 + 3] = np.z;
		}
		if ((w.x <= 0.0 && w.y <= 0.0) || keep[i] <= 0.0) { // the weights reach past 40 degrees now (_relief_weights_at)
			nw[i] = nb;
			continue;
		}
		Vector3 t1 = nb.cross(Vector3(0, 1, 0));
		t1 = t1.length_squared() > 1e-8f ? t1.normalized() : Vector3(1, 0, 0);
		const Vector3 t2 = t1.cross(nb);
		const Vector3 du = _cliff_point(p + t1 * e, keep[i]) - _cliff_point(p - t1 * e, keep[i]);
		const Vector3 dv = _cliff_point(p + t2 * e, keep[i]) - _cliff_point(p - t2 * e, keep[i]);
		Vector3 nn = dv.cross(du);
		if (nn.dot(nb) < 0.0) {
			nn = -nn;
		}
		nw[i] = nn.length_squared() > 1e-12f ? nn.normalized() : nb;
	}
	// Folds left over. The scale k (_cliff_scale) eases the relief where its offset changes fast, but it changes fast
	// itself where a bed's step comes into its reach, and k o then falls by more than the distance across the face in
	// a few centimetres: the displaced triangle turns over in plan (the surface folds through itself, black dashes
	// along a cliff's foot). Such a triangle's free vertices are moved until none turns over (the relief is
	// horizontal, so a triangle folds exactly when its plan projection changes orientation). The chunk's sides are
	// left alone, so neighbours still agree on them.
	{
		const real_t x0 = corner.x, x1 = corner.x + n * spacing, z0 = corner.z, z1 = corner.z + n * spacing;
		const real_t eb = spacing * 1e-3f;
		auto on_side = [&](const Vector3 &q) {
			return Math::abs(q.x - x0) < eb || Math::abs(q.x - x1) < eb || Math::abs(q.z - z0) < eb || Math::abs(q.z - z1) < eb;
		};
		// First relaxed: each turned triangle's free vertices take their neighbours' mean displacement (Laplacian
		// smoothing of the offset, as displacement-mapping pipelines repair foldovers), which spreads a too-steep
		// fall over the cells around it. Drawn back toward the heightfield instead, as a last resort, they left a pit
		// in the relief up to its full depth across, whose walls caught the sun's shadow: small dark nicks on lit faces.
		LocalVector<uint8_t> moved;
		moved.resize(base.size());
		memset(moved.ptr(), 0, moved.size());
		LocalVector<uint8_t> mark;
		mark.resize(base.size());
		LocalVector<Vector3> sum_d;
		LocalVector<Vector4> sum_c;
		LocalVector<real_t> cnt;
		const int RELAX = 12;
		for (int it = 0; it < RELAX + 8; it++) {
			memset(mark.ptr(), 0, mark.size());
			bool any = false;
			for (uint32_t t = 0; t + 2 < tris.size(); t += 3) {
				const int a = tris[t], b = tris[t + 1], c = tris[t + 2];
				const Vector3 &ba = base[a], &bb = base[b], &bc = base[c];
				const real_t area0 = (bb.x - ba.x) * (bc.z - ba.z) - (bb.z - ba.z) * (bc.x - ba.x);
				if (Math::abs(area0) < 1e-6f * spacing * spacing) {
					continue;
				}
				const Vector3 pa = vp[a] + corner, pb = vp[b] + corner, pc = vp[c] + corner;
				const real_t area1 = (pb.x - pa.x) * (pc.z - pa.z) - (pb.z - pa.z) * (pc.x - pa.x);
				if (area1 / area0 < 0.05f) {
					for (int v : { a, b, c }) {
						if (keep[v] > 0.0 && !on_side(base[v])) {
							mark[v] = 1;
							any = true;
						}
					}
				}
			}
			if (!any) {
				break;
			}
			if (it < RELAX) {
				if (sum_d.is_empty()) {
					sum_d.resize(base.size());
					sum_c.resize(base.size());
					cnt.resize(base.size());
				}
				for (uint32_t i = 0; i < base.size(); i++) {
					if (mark[i]) {
						sum_d[i] = Vector3();
						sum_c[i] = Vector4();
						cnt[i] = 0.0;
					}
				}
				for (uint32_t t = 0; t + 2 < tris.size(); t += 3) {
					for (int e = 0; e < 3; e++) {
						const int v = tris[t + e];
						if (!mark[v]) {
							continue;
						}
						for (int f = 1; f < 3; f++) {
							const int u = tris[t + (e + f) % 3];
							sum_d[v] += vp[u] - (base[u] - corner);
							sum_c[v] += Vector4(c0[u * 4 + 3], c1[u * 4 + 3], c2[u * 4], 0.0f);
							cnt[v] += 1.0;
						}
					}
				}
				for (uint32_t i = 0; i < base.size(); i++) {
					if (mark[i] && cnt[i] > 0.0) {
						vp[i] = (base[i] - corner) + sum_d[i] / cnt[i];
						const Vector4 cc = sum_c[i] / cnt[i];
						c0[i * 4 + 3] = cc.x;
						c1[i * 4 + 3] = cc.y;
						c2[i * 4] = cc.z;
						moved[i] = 1;
					}
				}
			} else {
				for (uint32_t i = 0; i < base.size(); i++) {
					if (mark[i]) {
						vp[i] = (base[i] - corner) + (vp[i] - (base[i] - corner)) * 0.6f;
						c0[i * 4 + 3] *= 0.6f;
						c1[i * 4 + 3] *= 0.6f;
						c2[i * 4] *= 0.6f;
						moved[i] = 1;
					}
				}
			}
		}
		// The moved vertices' normals from the triangles round them as they now stand (area weighted).
		bool any_moved = false;
		for (uint32_t i = 0; i < base.size() && !any_moved; i++) {
			any_moved = moved[i];
		}
		if (any_moved) {
			LocalVector<Vector3> acc;
			acc.resize(base.size());
			for (uint32_t t = 0; t + 2 < tris.size(); t += 3) {
				const int a = tris[t], b = tris[t + 1], c = tris[t + 2];
				if (!moved[a] && !moved[b] && !moved[c]) {
					continue;
				}
				Vector3 fn = (vp[b] - vp[a]).cross(vp[c] - vp[a]);
				const Vector3 nb = _normal_at((base[a].x - origin.x) / spacing, (base[a].z - origin.y) / spacing);
				if (fn.dot(nb) < 0.0) {
					fn = -fn;
				}
				for (int v : { a, b, c }) {
					if (moved[v]) {
						acc[v] += fn;
					}
				}
			}
			for (uint32_t i = 0; i < base.size(); i++) {
				if (moved[i] && acc[i].length_squared() > 1e-12f) {
					nw[i] = acc[i].normalized();
				}
			}
		}
	}
	PackedInt32Array ia;
	ia.resize(tris.size());
	memcpy(ia.ptrw(), tris.ptr(), tris.size() * sizeof(int));
	Array arrays;
	arrays.resize(RSE::ARRAY_MAX);
	arrays[RSE::ARRAY_VERTEX] = verts;
	arrays[RSE::ARRAY_NORMAL] = norms;
	arrays[RSE::ARRAY_CUSTOM0] = cus0;
	arrays[RSE::ARRAY_CUSTOM1] = cus1;
	arrays[RSE::ARRAY_CUSTOM2] = cus2;
	arrays[RSE::ARRAY_INDEX] = ia;
	const uint64_t cflags = (uint64_t(RSE::ARRAY_CUSTOM_RGBA_FLOAT) << RSE::ARRAY_FORMAT_CUSTOM0_SHIFT) | (uint64_t(RSE::ARRAY_CUSTOM_RGBA_FLOAT) << RSE::ARRAY_FORMAT_CUSTOM1_SHIFT) | (uint64_t(RSE::ARRAY_CUSTOM_RGBA_FLOAT) << RSE::ARRAY_FORMAT_CUSTOM2_SHIFT);
	RS::get_singleton()->mesh_create_surface_data_from_arrays(&r_job.surface, RSE::PRIMITIVE_TRIANGLES, arrays, Array(), Dictionary(), cflags);
	r_job.triangles = int(tris.size() / 3);
}

void Landscape3D::_build_surface(MeshJob &r_job) const {
	if (r_job.mask & 16) {
		_build_cliff_surface(r_job);
		return;
	}
	const int cx = r_job.chunk % cw;
	const int cz = r_job.chunk / cw;
	const int s = 1 << r_job.level;
	const int n = chunk_quads >> r_job.level;
	PackedVector3Array verts;
	PackedVector3Array norms;
	verts.resize((n + 1) * (n + 1));
	norms.resize((n + 1) * (n + 1));
	Vector3 *vw = verts.ptrw();
	Vector3 *nw = norms.ptrw();
	for (int j = 0; j <= n; j++) {
		for (int i = 0; i <= n; i++) {
			const int x = cx * chunk_quads + i * s;
			const int z = cz * chunk_quads + j * s;
			vw[j * (n + 1) + i] = Vector3(i * s * spacing, _h(x, z), j * s * spacing);
			nw[j * (n + 1) + i] = _normal(x, z);
		}
	}
	LocalVector<int> idx;
	_crease_indices(cx, cz, r_job.level, r_job.mask & 15, idx);
	PackedInt32Array ia;
	const LocalVector<uint8_t> *hl = chunk_holes.getptr(r_job.chunk);
	if (hl) {
		// Triangles touching a hole sample are left out (a cave mouth's passage; its rock hides the edge).
		for (uint32_t t = 0; t < idx.size(); t += 3) {
			if (!_hole_tri(hl, r_job.level, idx[t], idx[t + 1], idx[t + 2])) {
				ia.push_back(idx[t]);
				ia.push_back(idx[t + 1]);
				ia.push_back(idx[t + 2]);
			}
		}
	} else {
		ia.resize(idx.size());
		memcpy(ia.ptrw(), idx.ptr(), idx.size() * sizeof(int));
	}
	Array arrays;
	arrays.resize(RSE::ARRAY_MAX);
	arrays[RSE::ARRAY_VERTEX] = verts;
	arrays[RSE::ARRAY_NORMAL] = norms;
	arrays[RSE::ARRAY_INDEX] = ia;
	RS::get_singleton()->mesh_create_surface_data_from_arrays(&r_job.surface, RSE::PRIMITIVE_TRIANGLES, arrays);
	r_job.triangles = int(ia.size() / 3);
}

void Landscape3D::_mesh_task(uint32_t p_index, void *p_userdata) {
	_build_surface(mesh_jobs[p_index]);
}

void Landscape3D::_set_chunk_mesh(int p_chunk, int p_level, int p_mask) {
	Chunk &c = chunks[p_chunk];
	const uint64_t key = _mesh_key(p_chunk, p_level, p_mask);
	if (key == c.mesh_key) {
		return;
	}
	MeshEntry *e = meshes.getptr(key);
	if (!e) {
		// Not built ahead (only when called outside a pick): build it here.
		MeshJob job;
		job.key = key;
		job.chunk = p_chunk;
		job.level = p_level;
		job.mask = p_mask;
		_build_surface(job);
		MeshEntry entry;
		entry.mesh = RS::get_singleton()->mesh_create();
		RS::get_singleton()->mesh_add_surface(entry.mesh, job.surface);
		entry.triangles = job.triangles;
		meshes.insert(key, entry);
		mesh_builds++;
		e = meshes.getptr(key);
	}
	if (c.mesh_key != UINT64_MAX) {
		MeshEntry *old = meshes.getptr(c.mesh_key);
		if (old) {
			old->users--;
			old->last_used = frame;
		}
		if (c.lod != p_level) {
			lod_switches++;
		}
	}
	e->users++;
	e->last_used = frame;
	c.mesh_key = key;
	c.lod = p_level;
	c.mask = p_mask;
	if (c.instance.is_valid()) {
		RS::get_singleton()->instance_set_base(c.instance, e->mesh);
	}
}

void Landscape3D::_evict(bool p_all) {
	LocalVector<uint64_t> gone;
	for (KeyValue<uint64_t, MeshEntry> &kv : meshes) {
		if (p_all || (kv.value.users <= 0 && frame - kv.value.last_used > MESH_TTL)) {
			gone.push_back(kv.key);
		}
	}
	for (uint64_t key : gone) {
		RS::get_singleton()->free_rid(meshes[key].mesh);
		meshes.erase(key);
	}
}

/* Collision */

// What the collision follows: collision_target, else the camera.
Node3D *Landscape3D::_collision_node() const {
	if (!is_inside_tree()) {
		return nullptr;
	}
	if (!collision_target.is_empty()) {
		return Object::cast_to<Node3D>(get_node_or_null(collision_target));
	}
	return _camera();
}

// One static heightfield per chunk within collision_radius of the target, at full detail (level 0, whatever level
// the chunk is drawn at), split like the drawn level 0 mesh. Tiles are freed past 1.25 x the radius. A chunk not read
// in full yet gets its tile once read; the target's own chunk and its neighbours are read at once.
void Landscape3D::_update_collision() {
	const bool on = collision_enabled && !chunks.is_empty() && is_inside_tree() && get_world_3d().is_valid() && !Engine::get_singleton()->is_editor_hint();
	Node3D *tn = on ? _collision_node() : nullptr;
	if (!tn) {
		if (collision_tiles > 0) {
			_free_all_collision();
		}
		return;
	}
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	const Vector3 t = get_global_transform().affine_inverse().xform(tn->get_global_position());
	const real_t size = chunk_quads * spacing;
	const real_t keep = collision_radius * 1.25f;
	auto dist = [&](int p_cx, int p_cz) {
		const real_t x0 = origin.x + p_cx * size, z0 = origin.y + p_cz * size;
		const Vector2 q(CLAMP(real_t(t.x), x0, x0 + size), CLAMP(real_t(t.z), z0, z0 + size));
		return q.distance_to(Vector2(t.x, t.z));
	};
	// Tiles out of reach go.
	for (uint32_t i = 0; i < chunks.size() && collision_tiles > 0; i++) {
		if (chunks[i].body.is_valid() && dist(i % cw, i / cw) > keep) {
			_free_collision(int(i));
		}
	}
	const int tx = int(Math::floor((t.x - origin.x) / size));
	const int tz = int(Math::floor((t.z - origin.y) / size));
	const int reach = int(Math::ceil(collision_radius / size)) + 1;
	// The ground right under the target cannot wait for the stream.
	LocalVector<int> urgent;
	for (int z = MAX(0, tz - 1); z <= MIN(ch - 1, tz + 1); z++) {
		for (int x = MAX(0, tx - 1); x <= MIN(cw - 1, tx + 1); x++) {
			const int i = z * cw + x;
			if (!chunks[i].fine && dist(x, z) <= collision_radius) {
				urgent.push_back(i);
			}
		}
	}
	if (!urgent.is_empty() && _stream_generator()) {
		_finish_meshes(true);
		_finish_heights(true);
		LocalVector<int> left;
		for (int i : urgent) {
			if (!chunks[i].fine) {
				left.push_back(i);
			}
		}
		_read_chunks(left);
	}
	for (int z = MAX(0, tz - reach); z <= MIN(ch - 1, tz + reach); z++) {
		for (int x = MAX(0, tx - reach); x <= MIN(cw - 1, tx + reach); x++) {
			const int i = z * cw + x;
			if (!chunks[i].body.is_valid() && chunks[i].fine && dist(x, z) <= collision_radius) {
				_collision_tile(i);
			}
		}
	}
	last_collision_usec = double(OS::get_singleton()->get_ticks_usec() - t0);
}

void Landscape3D::_collision_tile(int p_chunk) {
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	Chunk &c = chunks[p_chunk];
	const int cx = p_chunk % cw, cz = p_chunk / cw;
	const int row = chunk_quads + 1;
	PackedFloat32Array h;
	h.resize(row * row);
	float *w = h.ptrw();
	for (int j = 0; j < row; j++) {
		memcpy(w + j * row, heights.ptr() + size_t(cz * chunk_quads + j) * gw + cx * chunk_quads, row * sizeof(float));
	}
	if (const LocalVector<uint8_t> *hl = chunk_holes.getptr(p_chunk)) {
		// A NaN height is a hole for every physics backend here (PhysX eHOLE, Jolt FLT_MAX, Godot Physics): it takes
		// every triangle touching the sample, so only samples whose quads are all holes go (the collision hole sits
		// inside the drawn one; the cave block's own collision covers its edge).
		const int n = chunk_quads;
		for (int j = 0; j < row; j++) {
			for (int i = 0; i < row; i++) {
				bool all = true;
				for (int k = 0; k < 4 && all; k++) {
					const int qi = i - 1 + (k & 1), qj = j - 1 + (k >> 1);
					all = qi >= 0 && qj >= 0 && qi < n && qj < n && (*hl)[qj * n + qi];
				}
				if (all) {
					w[j * row + i] = Math::NaN;
				}
			}
		}
	}
	Dictionary d;
	d["width"] = row;
	d["depth"] = row;
	d["heights"] = h;
	d["min_height"] = c.min_h;
	d["max_height"] = MAX(c.max_h, c.min_h + 0.001f);
	d["fan_blocks"] = true;
	c.shape = ps->heightmap_shape_create();
	ps->shape_set_data(c.shape, d);
	c.body = ps->body_create();
	ps->body_set_mode(c.body, PhysicsServer3D::BODY_MODE_STATIC);
	// The shape is centred on its middle sample and one unit per sample: scale it to the spacing.
	const real_t half = chunk_quads * spacing * 0.5f;
	const Vector3 centre(origin.x + cx * chunk_quads * spacing + half, 0.0, origin.y + cz * chunk_quads * spacing + half);
	ps->body_add_shape(c.body, c.shape, Transform3D(Basis().scaled(Vector3(spacing, 1.0, spacing)), centre));
	ps->body_set_collision_layer(c.body, collision_layer);
	ps->body_set_collision_mask(c.body, collision_mask);
	if (physics_material.is_valid()) {
		ps->body_set_param(c.body, PhysicsServer3D::BODY_PARAM_FRICTION, physics_material->computed_friction());
		ps->body_set_param(c.body, PhysicsServer3D::BODY_PARAM_BOUNCE, physics_material->computed_bounce());
	}
	ps->body_attach_object_instance_id(c.body, get_instance_id());
	ps->body_set_state(c.body, PhysicsServer3D::BODY_STATE_TRANSFORM, get_global_transform());
	ps->body_set_space(c.body, get_world_3d()->get_space());
	collision_tiles++;
}

void Landscape3D::_free_collision(int p_chunk) {
	Chunk &c = chunks[p_chunk];
	if (c.body.is_valid()) {
		PhysicsServer3D::get_singleton()->free_rid(c.body);
		c.body = RID();
		collision_tiles--;
	}
	if (c.shape.is_valid()) {
		PhysicsServer3D::get_singleton()->free_rid(c.shape);
		c.shape = RID();
	}
}

void Landscape3D::_free_all_collision() {
	for (uint32_t i = 0; i < chunks.size(); i++) {
		_free_collision(int(i));
	}
	collision_tiles = 0;
}

bool Landscape3D::has_chunk_collision(const Vector2i &p_chunk) const {
	ERR_FAIL_COND_V(p_chunk.x < 0 || p_chunk.y < 0 || p_chunk.x >= cw || p_chunk.y >= ch, false);
	return chunks[p_chunk.y * cw + p_chunk.x].body.is_valid();
}

void Landscape3D::set_collision_enabled(bool p_enable) {
	collision_enabled = p_enable;
	if (!p_enable) {
		_free_all_collision();
	}
}

void Landscape3D::set_collision_layer(uint32_t p_layer) {
	collision_layer = p_layer;
	for (Chunk &c : chunks) {
		if (c.body.is_valid()) {
			PhysicsServer3D::get_singleton()->body_set_collision_layer(c.body, p_layer);
		}
	}
}

void Landscape3D::set_collision_mask(uint32_t p_mask) {
	collision_mask = p_mask;
	for (Chunk &c : chunks) {
		if (c.body.is_valid()) {
			PhysicsServer3D::get_singleton()->body_set_collision_mask(c.body, p_mask);
		}
	}
}

void Landscape3D::set_physics_material(const Ref<PhysicsMaterial> &p_material) {
	physics_material = p_material;
	_free_all_collision(); // rebuilt with the new values on the next update
}

void Landscape3D::_free_instances() {
	for (Chunk &c : chunks) {
		if (c.instance.is_valid()) {
			RS::get_singleton()->free_rid(c.instance);
			c.instance = RID();
		}
	}
}

void Landscape3D::_create_instances() {
	if (!is_inside_tree() || get_world_3d().is_null()) {
		return;
	}
	RS *rs = RS::get_singleton();
	const RID scenario = get_world_3d()->get_scenario();
	for (uint32_t i = 0; i < chunks.size(); i++) {
		Chunk &c = chunks[i];
		if (c.instance.is_valid()) {
			continue;
		}
		c.instance = rs->instance_create();
		rs->instance_set_scenario(c.instance, scenario);
		rs->instance_set_visible(c.instance, is_visible_in_tree());
		if (c.mesh_key != UINT64_MAX) {
			rs->instance_set_base(c.instance, meshes[c.mesh_key].mesh);
		}
	}
	_update_transforms();
	_apply_material();
}

void Landscape3D::_update_transforms() {
	const Transform3D xf = is_inside_tree() ? get_global_transform() : Transform3D();
	for (uint32_t i = 0; i < chunks.size(); i++) {
		if (chunks[i].instance.is_valid()) {
			const Vector3 corner(origin.x + (i % cw) * chunk_quads * spacing, 0.0, origin.y + (i / cw) * chunk_quads * spacing);
			RS::get_singleton()->instance_set_transform(chunks[i].instance, xf * Transform3D(Basis(), corner));
		}
	}
}

void Landscape3D::_apply_material() {
	RS *rs = RS::get_singleton();
	const RID mat = material.is_valid() ? material->get_rid() : RID();
	for (Chunk &c : chunks) {
		if (c.instance.is_valid()) {
			rs->instance_geometry_set_material_override(c.instance, mat);
			rs->instance_geometry_set_cast_shadows_setting(c.instance, cast_shadows ? RSE::SHADOW_CASTING_SETTING_ON : RSE::SHADOW_CASTING_SETTING_OFF);
		}
	}
}

Camera3D *Landscape3D::_camera() const {
	if (!is_inside_tree()) {
		return nullptr;
	}
	if (!camera_path.is_empty()) {
		return Object::cast_to<Camera3D>(get_node_or_null(camera_path));
	}
#ifdef TOOLS_ENABLED
	if (Engine::get_singleton()->is_editor_hint() && EditorInterface::get_singleton()) {
		SubViewport *vp = EditorInterface::get_singleton()->get_editor_viewport_3d(0);
		return vp ? vp->get_camera_3d() : nullptr;
	}
#endif
	return get_viewport() ? get_viewport()->get_camera_3d() : nullptr;
}

// Picks a level and edge mask per chunk into target_lod and target_mask, and marks in need which unread chunks the
// view needs read in full. Returns true when every target mesh is already built.
bool Landscape3D::_pick_lods(const Transform3D &p_camera, const Projection &p_projection, real_t p_viewport_height) {
	const Transform3D inv = is_inside_tree() ? get_global_transform().affine_inverse() : Transform3D();
	const Vector3 eye = inv.xform(p_camera.origin);
	const bool ortho = p_projection.is_orthogonal();
	// Pixels per metre at distance 1 (perspective) or anywhere (orthogonal).
	const real_t k = Math::abs(p_projection.columns[1][1]) * p_viewport_height * 0.5;
	const real_t size = chunk_quads * spacing;
	const int n = int(chunks.size());
	LocalVector<int> &want = target_lod;
	want.resize(n);
	LocalVector<int> raw;
	raw.resize(n);
	LocalVector<int> floor_l;
	floor_l.resize(n);
	LocalVector<real_t> scale_of;
	scale_of.resize(n);
	for (int i = 0; i < n; i++) {
		const Chunk &c = chunks[i];
		const Vector3 lo(origin.x + (i % cw) * size, c.min_h, origin.y + (i / cw) * size);
		const Vector3 hi(lo.x + size, c.max_h, lo.z + size);
		const Vector3 d = eye.clamp(lo, hi) - eye;
		const real_t scale = ortho ? k : k / MAX(d.length(), real_t(0.001));
		scale_of[i] = scale;
		int loose = 0, strict = 0;
		for (int l = 1; l < levels; l++) {
			const real_t px = c.error[l] * scale;
			if (px <= lod_error) {
				loose = l;
			}
			if (px <= lod_error * 0.7) {
				strict = l;
			}
		}
		raw[i] = loose;
		floor_l[i] = c.fine ? 0 : COARSE;
		const int cur = c.mesh_key == UINT64_MAX ? loose : c.lod;
		want[i] = MAX(loose < cur ? loose : (strict > cur ? strict : cur), floor_l[i]);
	}
	// Chunks not read in full hold their neighbours at most one level finer.
	bool changed = true;
	while (changed) {
		changed = false;
		for (int z = 0; z < ch; z++) {
			for (int x = 0; x < cw; x++) {
				int &f = floor_l[z * cw + x];
				const int nb[4] = { z > 0 ? floor_l[(z - 1) * cw + x] : 0, x < cw - 1 ? floor_l[z * cw + x + 1] : 0, z < ch - 1 ? floor_l[(z + 1) * cw + x] : 0, x > 0 ? floor_l[z * cw + x - 1] : 0 };
				for (int s = 0; s < 4; s++) {
					if (f < nb[s] - 1) {
						f = nb[s] - 1;
						changed = true;
					}
				}
			}
		}
	}
	for (int i = 0; i < n; i++) {
		want[i] = MAX(want[i], floor_l[i]);
		if (chunks[i].fine && chunk_holes.has(i)) {
			want[i] = 0; // a hole keeps its shape at any distance
		}
	}
	// Neighbours at most one level apart; the finer side wins (never below the floor: a neighbour sits at or above
	// its own floor, which is at most one under this one's).
	changed = true;
	while (changed) {
		changed = false;
		for (int z = 0; z < ch; z++) {
			for (int x = 0; x < cw; x++) {
				int &l = want[z * cw + x];
				const int nb[4] = { z > 0 ? want[(z - 1) * cw + x] : INT_MAX, x < cw - 1 ? want[z * cw + x + 1] : INT_MAX, z < ch - 1 ? want[(z + 1) * cw + x] : INT_MAX, x > 0 ? want[z * cw + x - 1] : INT_MAX };
				for (int s = 0; s < 4; s++) {
					if (nb[s] != INT_MAX && l > nb[s] + 1) {
						l = nb[s] + 1;
						changed = true;
					}
				}
			}
		}
	}
	// What the view needs read (2): an unread chunk it wants finer than COARSE, and one next to a chunk it wants at
	// level 0 (which could not sit next to it otherwise); 3 where the 4 m grid would be off by FIRST_VIEW_SLACK
	// times lod_error. The estimates are upper bounds, so reading never asks for more.
	need.resize(n);
	for (int i = 0; i < n; i++) {
		need[i] = chunks[i].fine ? 0 : (raw[i] < COARSE ? (chunks[i].error[COARSE] * scale_of[i] > lod_error * FIRST_VIEW_SLACK ? 3 : 2) : 1);
		if (!chunks[i].fine && chunk_holes.has(i)) {
			need[i] = 3;
		}
	}
	for (int z = 0; z < ch; z++) {
		for (int x = 0; x < cw; x++) {
			if (raw[z * cw + x] >= 1) {
				continue;
			}
			const int nb[4] = { z > 0 ? (z - 1) * cw + x : -1, x < cw - 1 ? z * cw + x + 1 : -1, z < ch - 1 ? (z + 1) * cw + x : -1, x > 0 ? z * cw + x - 1 : -1 };
			for (int s = 0; s < 4; s++) {
				if (nb[s] >= 0 && need[nb[s]] == 1) {
					need[nb[s]] = 2;
				}
			}
		}
	}
	target_mask.resize(n);
	LocalVector<uint8_t> rel;
	rel.resize(n);
	for (int i = 0; i < n; i++) {
		rel[i] = MAX(cliff_relief, bank_relief) * scale_of[i] >= lod_error * 0.5 ? 1 : 0;
	}
	bool ready = true;
	for (int z = 0; z < ch; z++) {
		for (int x = 0; x < cw; x++) {
			const int l = want[z * cw + x];
			int mask = 0;
			if (z > 0 && want[(z - 1) * cw + x] > l) {
				mask |= 1;
			}
			if (x < cw - 1 && want[z * cw + x + 1] > l) {
				mask |= 2;
			}
			if (z < ch - 1 && want[(z + 1) * cw + x] > l) {
				mask |= 4;
			}
			if (x > 0 && want[z * cw + x - 1] > l) {
				mask |= 8;
			}
			// Cliff relief on the finest chunks where it spans half the pixel error or more; sides next to a chunk
			// without it keep their edges (bits 5 to 8), like sides next to a coarser one.
			if ((cliff_relief > 0.0 || bank_relief > 0.0) && l == 0 && rel[z * cw + x]) {
				mask |= 16;
				const int nbi[4] = { z > 0 ? (z - 1) * cw + x : -1, x < cw - 1 ? z * cw + x + 1 : -1, z < ch - 1 ? (z + 1) * cw + x : -1, x > 0 ? z * cw + x - 1 : -1 };
				for (int sd = 0; sd < 4; sd++) {
					if (nbi[sd] >= 0 && !(want[nbi[sd]] == 0 && rel[nbi[sd]])) {
						mask |= 32 << sd;
					}
				}
			}
			target_mask[z * cw + x] = mask;
			if (ready && !meshes.has(_mesh_key(z * cw + x, l, mask))) {
				ready = false;
			}
		}
	}
	return ready;
}

// Builds the meshes the target lacks on worker threads; with p_wait, waits and shows the target.
void Landscape3D::_launch_meshes(bool p_wait) {
	HashSet<uint64_t> queued;
	mesh_jobs.clear();
	for (uint32_t i = 0; i < chunks.size(); i++) {
		const uint64_t key = _mesh_key(i, target_lod[i], target_mask[i]);
		if (meshes.has(key) || queued.has(key)) {
			continue;
		}
		queued.insert(key);
		MeshJob job;
		job.key = key;
		job.chunk = int(i);
		job.level = target_lod[i];
		job.mask = target_mask[i];
		mesh_jobs.push_back(job);
	}
	if (mesh_jobs.is_empty()) {
		for (uint32_t i = 0; i < chunks.size(); i++) {
			_set_chunk_mesh(i, target_lod[i], target_mask[i]);
		}
		return;
	}
	mesh_group = WorkerThreadPool::get_singleton()->add_template_group_task(this, &Landscape3D::_mesh_task, (void *)nullptr, int(mesh_jobs.size()), -1, p_wait, SNAME("Landscape3D meshes"));
	if (p_wait) {
		_finish_meshes(true);
	}
}

// Hands the built meshes to the renderer and shows the whole target at once, so no frame has a level next to one
// it does not fit.
void Landscape3D::_finish_meshes(bool p_wait) {
	if (mesh_group < 0) {
		return;
	}
	if (!p_wait && !WorkerThreadPool::get_singleton()->is_group_task_completed(mesh_group)) {
		return;
	}
	WorkerThreadPool::get_singleton()->wait_for_group_task_completion(mesh_group);
	mesh_group = -1;
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	RS *rs = RS::get_singleton();
	for (MeshJob &job : mesh_jobs) {
		MeshEntry entry;
		entry.mesh = rs->mesh_create();
		rs->mesh_add_surface(entry.mesh, job.surface);
		entry.triangles = job.triangles;
		entry.last_used = frame;
		meshes.insert(job.key, entry);
		mesh_builds++;
	}
	mesh_jobs.clear();
	for (uint32_t i = 0; i < chunks.size(); i++) {
		_set_chunk_mesh(i, target_lod[i], target_mask[i]);
	}
	last_apply_usec = double(OS::get_singleton()->get_ticks_usec() - t0);
}

/* Streaming heights (docs/ideas/terrain.md, "Landscape3D: streaming and off-thread builds") */

const TerrainGenerator3D *Landscape3D::_stream_generator() const {
	return stream_source.is_valid() ? Object::cast_to<TerrainGenerator3D>(ObjectDB::get_instance(stream_source)) : nullptr;
}

// One row of the 4 m grid (every 4th point), past the valid points repeating the last row and column as the full
// read does.
void Landscape3D::_coarse_task(uint32_t p_row, void *p_userdata) {
	const TerrainGenerator3D *gen = static_cast<const TerrainGenerator3D *>(p_userdata);
	const int z = MIN(int(p_row) * 4, gh - 1);
	const real_t wz = origin.y + MIN(z, vh - 1) * spacing;
	for (int x = 0; x < gw; x += 4) {
		const float h = gen->get_height(Vector3(origin.x + MIN(x, vw - 1) * spacing, 0.0, wz));
		heights[size_t(z) * gw + x] = Math::is_nan(h) ? 0.0f : h;
	}
}

// One row of the grid between the 4 m points, bilinear from them.
void Landscape3D::_fill_task(uint32_t p_row, void *p_userdata) {
	const int z = int(p_row);
	const int z0 = z & ~3;
	const int z1 = MIN(z0 + 4, gh - 1);
	const float fz = float(z - z0) * 0.25f;
	float *row = heights.ptr() + size_t(z) * gw;
	const float *r0 = heights.ptr() + size_t(z0) * gw;
	const float *r1 = heights.ptr() + size_t(z1) * gw;
	for (int x = 0; x < gw; x++) {
		if ((x & 3) == 0 && z == z0) {
			continue;
		}
		const int x0 = x & ~3;
		const int x1 = MIN(x0 + 4, gw - 1);
		const float fx = float(x - x0) * 0.25f;
		const float a = Math::lerp(r0[x0], r0[x1], fx);
		const float b = Math::lerp(r1[x0], r1[x1], fx);
		row[x] = Math::lerp(a, b, fz);
	}
}

// One chunk in full into its own buffer, then its level errors; the main thread copies it in.
void Landscape3D::_height_task(uint32_t p_index, void *p_userdata) {
	const TerrainGenerator3D *gen = static_cast<const TerrainGenerator3D *>(p_userdata);
	HeightJob &job = height_jobs[p_index];
	const int row = chunk_quads + 1;
	const int x0 = (job.chunk % cw) * chunk_quads;
	const int z0 = (job.chunk / cw) * chunk_quads;
	job.h.resize(row * row);
	for (int j = 0; j < row; j++) {
		const real_t wz = origin.y + MIN(z0 + j, vh - 1) * spacing;
		for (int i = 0; i < row; i++) {
			const float h = gen->get_height(Vector3(origin.x + MIN(x0 + i, vw - 1) * spacing, 0.0, wz));
			job.h[j * row + i] = Math::is_nan(h) ? 0.0f : h;
		}
	}
	_measure(job.h.ptr(), row, job.error, job.min_h, job.max_h);
}

void Landscape3D::_start_heights(const LocalVector<int> &p_chunks, bool p_wait) {
	const TerrainGenerator3D *gen = _stream_generator();
	if (!gen || p_chunks.is_empty() || height_group >= 0) {
		return;
	}
	height_jobs.resize(p_chunks.size());
	for (uint32_t i = 0; i < p_chunks.size(); i++) {
		height_jobs[i].chunk = p_chunks[i];
		chunks[p_chunks[i]].loading = true;
	}
	height_group = WorkerThreadPool::get_singleton()->add_template_group_task(this, &Landscape3D::_height_task, (void *)const_cast<TerrainGenerator3D *>(gen), int(height_jobs.size()), -1, p_wait, SNAME("Landscape3D heights"));
}

// The next chunks to read: those the view needs first, then the rest, nearest the camera first.
void Landscape3D::_launch_heights() {
	if (coarse_chunks <= 0 || height_group >= 0 || !_stream_generator()) {
		return;
	}
	const Vector3 eye = (is_inside_tree() ? get_global_transform().affine_inverse() : Transform3D()).xform(last_camera.origin);
	const real_t size = chunk_quads * spacing;
	struct Candidate {
		float order = 0.0f;
		int chunk = 0;
		bool operator<(const Candidate &p_other) const { return order < p_other.order; }
	};
	LocalVector<Candidate> list;
	for (uint32_t i = 0; i < chunks.size(); i++) {
		const Chunk &c = chunks[i];
		if (c.fine || c.loading) {
			continue;
		}
		const Vector2 centre(origin.x + ((i % cw) + 0.5f) * size, origin.y + ((i / cw) + 0.5f) * size);
		Candidate cand;
		cand.chunk = int(i);
		cand.order = (have_camera ? centre.distance_to(Vector2(eye.x, eye.z)) : float(i)) + (i < need.size() ? float(3 - MIN(int(need[i]), 3)) * 1e9f : 3e9f);
		list.push_back(cand);
	}
	if (list.is_empty()) {
		return;
	}
	// Ground the collision target will stand on comes first of all.
	if (Node3D *tn = collision_enabled ? _collision_node() : nullptr) {
		const Vector3 t = get_global_transform().affine_inverse().xform(tn->get_global_position());
		for (Candidate &cand : list) {
			const int cx = cand.chunk % cw, cz = cand.chunk / cw;
			const Rect2 r(origin.x + cx * size, origin.y + cz * size, size, size);
			const Vector2 q(CLAMP(real_t(t.x), r.position.x, r.position.x + size), CLAMP(real_t(t.z), r.position.y, r.position.y + size));
			if (q.distance_to(Vector2(t.x, t.z)) <= collision_radius) {
				cand.order -= 4e9f;
			}
		}
	}
	list.sort();
	const int batch = MIN(int(list.size()), MAX(4, WorkerThreadPool::get_singleton()->get_thread_count() * 2));
	LocalVector<int> pick;
	for (int i = 0; i < batch; i++) {
		pick.push_back(list[i].chunk);
	}
	_start_heights(pick, false);
}

// Copies read chunks into the grid. Waits for any mesh batch first: those read the grid.
void Landscape3D::_finish_heights(bool p_wait) {
	if (height_group < 0) {
		return;
	}
	if (!p_wait && (mesh_group >= 0 || !WorkerThreadPool::get_singleton()->is_group_task_completed(height_group))) {
		return;
	}
	_finish_meshes(true);
	WorkerThreadPool::get_singleton()->wait_for_group_task_completion(height_group);
	height_group = -1;
	const int row = chunk_quads + 1;
	for (HeightJob &job : height_jobs) {
		Chunk &c = chunks[job.chunk];
		const int cx = job.chunk % cw;
		const int cz = job.chunk / cw;
		for (int j = 0; j < row; j++) {
			memcpy(heights.ptr() + size_t(cz * chunk_quads + j) * gw + cx * chunk_quads, job.h.ptr() + j * row, row * sizeof(float));
		}
		memcpy(c.error, job.error, sizeof(c.error));
		c.min_h = job.min_h;
		c.max_h = job.max_h;
		c.loading = false;
		if (!c.fine) {
			c.fine = true;
			coarse_chunks--;
			chunks_streamed++;
		}
		// Its own vertices and the normals along its neighbours' sides (and corners) change.
		for (int dz = -1; dz <= 1; dz++) {
			for (int dx = -1; dx <= 1; dx++) {
				if (cx + dx >= 0 && cx + dx < cw && cz + dz >= 0 && cz + dz < ch) {
					chunks[(cz + dz) * cw + cx + dx].gen++;
				}
			}
		}
		// The relief follows the full-resolution normals: placers on it lay that ground again.
		if (cliff_relief > 0.0 || bank_relief > 0.0) {
			const real_t size = chunk_quads * spacing;
			_relief_changed(Rect2(origin.x + cx * size, origin.y + cz * size, size, size).grow(spacing));
		}
		if (uint32_t(job.chunk) < need.size()) {
			need[job.chunk] = 0;
		}
	}
	height_jobs.clear();
	lods_dirty = true;
}

void Landscape3D::_read_chunks(const LocalVector<int> &p_chunks) {
	_finish_heights(true);
	_start_heights(p_chunks, true);
	_finish_heights(true);
}

void Landscape3D::_wait_all() {
	if (mesh_group >= 0) {
		WorkerThreadPool::get_singleton()->wait_for_group_task_completion(mesh_group);
		mesh_group = -1;
		mesh_jobs.clear();
		lods_dirty = true;
	}
	if (height_group >= 0) {
		WorkerThreadPool::get_singleton()->wait_for_group_task_completion(height_group);
		height_group = -1;
		for (HeightJob &job : height_jobs) {
			chunks[job.chunk].loading = false;
		}
		height_jobs.clear();
	}
}

// The generator is about to replace its result (or leave the tree): finish reading it and stop. Unread chunks
// keep their 4 m heights until the next rebuild().
void Landscape3D::_on_source_changing() {
	_wait_all();
	TerrainGenerator3D *gen = Object::cast_to<TerrainGenerator3D>(ObjectDB::get_instance(stream_source));
	if (gen) {
		const Callable cb = callable_mp(this, &Landscape3D::_on_source_changing);
		if (gen->is_connected(SNAME("changing"), cb)) {
			gen->disconnect(SNAME("changing"), cb);
		}
		if (gen->is_connected(SceneStringName(tree_exiting), cb)) {
			gen->disconnect(SceneStringName(tree_exiting), cb);
		}
	}
	stream_source = ObjectID();
}

void Landscape3D::load_all_heights() {
	if (!_stream_generator()) {
		return;
	}
	LocalVector<int> all;
	for (uint32_t i = 0; i < chunks.size(); i++) {
		if (!chunks[i].fine) {
			all.push_back(int(i));
		}
	}
	if (all.is_empty()) {
		return;
	}
	_read_chunks(all);
	if (have_camera) {
		update_lods(last_camera, last_projection, last_viewport_height);
	}
}

/* Levels */

// Synchronous: reads the chunks this view needs in full first (so the frame is exact), builds the missing meshes on
// worker threads and waits for them. The node itself does the same each frame without waiting.
void Landscape3D::update_lods(const Transform3D &p_camera, const Projection &p_projection, real_t p_viewport_height) {
	_update_lods(p_camera, p_projection, p_viewport_height, 2);
}

void Landscape3D::_update_lods(const Transform3D &p_camera, const Projection &p_projection, real_t p_viewport_height, int p_read) {
	if (chunks.is_empty()) {
		return;
	}
	_finish_meshes(true);
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	_pick_lods(p_camera, p_projection, p_viewport_height);
	for (int pass = 0; pass < 4 && coarse_chunks > 0 && _stream_generator(); pass++) {
		LocalVector<int> needed;
		for (uint32_t i = 0; i < need.size(); i++) {
			if (need[i] >= p_read) {
				needed.push_back(int(i));
			}
		}
		if (needed.is_empty()) {
			break;
		}
		_read_chunks(needed);
		_pick_lods(p_camera, p_projection, p_viewport_height);
	}
	last_update_usec = double(OS::get_singleton()->get_ticks_usec() - t0);
	last_camera = p_camera;
	last_projection = p_projection;
	last_viewport_height = p_viewport_height;
	have_camera = true;
	lods_dirty = false;
	_launch_meshes(true);
}

void Landscape3D::update_lods_from_camera() {
	Camera3D *cam = _camera();
	if (!cam || !cam->get_viewport()) {
		return;
	}
	update_lods(cam->get_global_transform(), cam->get_camera_projection(), cam->get_viewport()->get_visible_rect().size.y);
}

void Landscape3D::set_chunk_quads(int p_quads) {
	int q = 8;
	while (q < p_quads && q < 256) {
		q *= 2;
	}
	chunk_quads = q;
}

void Landscape3D::set_lod_error(real_t p_pixels) {
	lod_error = MAX(p_pixels, real_t(0.05));
	lods_dirty = true;
}

void Landscape3D::set_cliff_relief(real_t p_metres) {
	if (Math::is_equal_approx(cliff_relief, MAX(p_metres, real_t(0.0)))) {
		return;
	}
	cliff_relief = MAX(p_metres, real_t(0.0));
	// New meshes for every chunk (the cache keys carry the height generation).
	for (uint32_t i = 0; i < chunks.size(); i++) {
		chunks[i].gen++;
	}
	lods_dirty = true;
	_relief_changed(Rect2());
}

void Landscape3D::set_bank_relief(real_t p_metres) {
	if (Math::is_equal_approx(bank_relief, MAX(p_metres, real_t(0.0)))) {
		return;
	}
	bank_relief = MAX(p_metres, real_t(0.0));
	for (uint32_t i = 0; i < chunks.size(); i++) {
		chunks[i].gen++;
	}
	lods_dirty = true;
	_relief_changed(Rect2());
}

void Landscape3D::set_terracette_relief(real_t p_share) {
	p_share = CLAMP(p_share, real_t(0.0), real_t(1.0));
	if (Math::is_equal_approx(terracette_relief, p_share)) {
		return;
	}
	terracette_relief = p_share;
	_publish_cliff_relief(!is_inside_tree());
	_relief_changed(Rect2());
}

// The relief over a rect (landscape space; empty: all of it) changed: WorldGround.relief_changed in world space.
void Landscape3D::_relief_changed(const Rect2 &p_area) {
	if (!is_inside_tree() || heights.is_empty()) {
		return;
	}
	Rect2 r = p_area.has_area() ? p_area : Rect2(origin, Vector2((gw - 1) * spacing, (gh - 1) * spacing));
	const Vector3 o = get_global_transform().origin;
	r.position += Vector2(o.x, o.z);
	WorldContracts::ground()->notify_relief_changed(r);
}

void Landscape3D::set_material(const Ref<Material> &p_material) {
	material = p_material;
	_apply_material();
}

void Landscape3D::set_cast_shadows(bool p_enable) {
	cast_shadows = p_enable;
	_apply_material();
}

void Landscape3D::clear() {
	_on_source_changing();
	chunk_holes.clear();
	if (TerrainGenerator3D *hg = Object::cast_to<TerrainGenerator3D>(ObjectDB::get_instance(hole_source))) {
		const Callable hcb = callable_mp(this, &Landscape3D::_on_holes_changed);
		if (hg->is_connected(SNAME("holes_changed"), hcb)) {
			hg->disconnect(SNAME("holes_changed"), hcb);
		}
	}
	hole_source = ObjectID();
	_free_instances();
	_free_all_collision();
	_evict(true);
	chunks.clear();
	{
		RWLockWrite hl(heights_lock);
		heights.clear();
		gw = gh = 0;
	}
	need.clear();
	target_lod.clear();
	target_mask.clear();
	for (int l = 0; l < 8; l++) {
		for (int m = 0; m < 16; m++) {
			indices[l][m].clear();
		}
	}
	gw = gh = vw = vh = cw = ch = 0;
	coarse_chunks = 0;
	detail_estimate = 0.0f;
	have_camera = false;
	lods_dirty = true;
}

/* Holes */

// A triangle is in a hole when the quad holding its centroid is (each triangle of a level's 2x2 fan lies in one
// quad of that level): the hole follows its box to within half a quad, not a whole quad around every hole sample.
bool Landscape3D::_hole_tri(const LocalVector<uint8_t> *p_holes, int p_level, int p_a, int p_b, int p_c) const {
	// Holes are cut at level 0 only (a hole's chunk is pinned there once its heights are read): a coarse triangle
	// spans many quads, and dropping it for its centre quad opened the ground far past the hole.
	if (p_level > 0) {
		return false;
	}
	const int s = 1 << p_level;
	const int rl = (chunk_quads >> p_level) + 1;
	const real_t x = real_t((p_a % rl) + (p_b % rl) + (p_c % rl)) * s / 3.0f;
	const real_t z = real_t((p_a / rl) + (p_b / rl) + (p_c / rl)) * s / 3.0f;
	const int qx = CLAMP(int(x), 0, chunk_quads - 1), qz = CLAMP(int(z), 0, chunk_quads - 1);
	return (*p_holes)[qz * chunk_quads + qx] != 0;
}

int Landscape3D::get_hole_quad_count() const {
	int n = 0;
	for (const KeyValue<int, LocalVector<uint8_t>> &kv : chunk_holes) {
		for (uint8_t f : kv.value) {
			n += f;
		}
	}
	return n;
}

void Landscape3D::_on_holes_changed(const Rect2 &p_rect) {
	_refresh_holes(p_rect);
}

// Reads the source's holes over the chunks touching p_rect (global x, z); a chunk whose holes changed gets new
// meshes (its key changes) and its collision tile again.
void Landscape3D::_refresh_holes(const Rect2 &p_rect) {
	const TerrainGenerator3D *gen = Object::cast_to<TerrainGenerator3D>(ObjectDB::get_instance(hole_source));
	if (!gen || chunks.is_empty()) {
		return;
	}
	_wait_all(); // mesh jobs read the flags
	const Transform3D gt = is_inside_tree() ? get_global_transform() : Transform3D();
	const Transform3D inv = gt.affine_inverse();
	Vector2 lo(1e30f, 1e30f), hi(-1e30f, -1e30f);
	for (int k = 0; k < 4; k++) {
		const Vector3 c = inv.xform(Vector3((k & 1) ? p_rect.get_end().x : p_rect.position.x, 0, (k & 2) ? p_rect.get_end().y : p_rect.position.y));
		lo = lo.min(Vector2(c.x, c.z));
		hi = hi.max(Vector2(c.x, c.z));
	}
	const real_t size = chunk_quads * spacing;
	const int x0 = CLAMP(int(Math::floor((lo.x - origin.x) / size)) - 1, 0, cw - 1), x1 = CLAMP(int(Math::floor((hi.x - origin.x) / size)) + 1, 0, cw - 1);
	const int z0 = CLAMP(int(Math::floor((lo.y - origin.y) / size)) - 1, 0, ch - 1), z1 = CLAMP(int(Math::floor((hi.y - origin.y) / size)) + 1, 0, ch - 1);
	for (int cz = z0; cz <= z1; cz++) {
		for (int cx = x0; cx <= x1; cx++) {
			const int ci = cz * cw + cx;
			LocalVector<uint8_t> f;
			bool any = false;
			Vector2 wlo(1e30f, 1e30f), whi(-1e30f, -1e30f);
			for (int k = 0; k < 4; k++) {
				const Vector3 c = gt.xform(Vector3(origin.x + (cx + (k & 1)) * size, 0, origin.y + (cz + ((k >> 1) & 1)) * size));
				wlo = wlo.min(Vector2(c.x, c.z));
				whi = whi.max(Vector2(c.x, c.z));
			}
			if (gen->has_holes_in(Rect2(wlo, whi - wlo).grow(spacing))) {
				// One flag per quad: a hole where the ground across the quad (from its lowest corner to its highest, at
				// its centre) passes through a hole box. On a steep wall the centre's height alone can lie above or
				// below a passage the wall crosses.
				f.resize(chunk_quads * chunk_quads);
				for (int j = 0; j < chunk_quads; j++) {
					for (int i = 0; i < chunk_quads; i++) {
						const Vector3 w = gt.xform(Vector3(origin.x + (cx * chunk_quads + i + 0.5f) * spacing, 0, origin.y + (cz * chunk_quads + j + 0.5f) * spacing));
						bool h = false;
						if (gen->has_holes_in(Rect2(w.x, w.z, 0, 0).grow(spacing))) {
							real_t ylo = 1e30, yhi = -1e30;
							for (int k = 0; k < 4; k++) {
								const Vector3 cw3 = gt.xform(Vector3(origin.x + (cx * chunk_quads + i + (k & 1)) * spacing, 0, origin.y + (cz * chunk_quads + j + ((k >> 1) & 1)) * spacing));
								const real_t hy = gen->get_height(cw3);
								ylo = MIN(ylo, hy);
								yhi = MAX(yhi, hy);
							}
							h = gen->is_hole_span(w.x, w.z, ylo, yhi);
						}
						f[j * chunk_quads + i] = h ? 1 : 0;
						any = any || h;
					}
				}
			}
			const LocalVector<uint8_t> *old = chunk_holes.getptr(ci);
			bool same = (old == nullptr) == !any;
			if (same && any) {
				for (int k = 0; k < chunk_quads * chunk_quads && same; k++) {
					same = (*old)[k] == f[k];
				}
			}
			if (same) {
				continue;
			}
			if (any) {
				chunk_holes[ci] = f;
			} else {
				chunk_holes.erase(ci);
			}
			chunks[ci].gen++;
			if (chunks[ci].body.is_valid()) {
				_free_collision(ci);
			}
			lods_dirty = true;
		}
	}
	_update_collision();
}

void Landscape3D::set_source(const NodePath &p_path) {
	if (source == p_path) {
		return;
	}
	source = p_path;
	if (is_inside_tree()) {
		_auto_connect();
	}
}

void Landscape3D::set_auto_rebuild(bool p_enable) {
	if (auto_rebuild == p_enable) {
		return;
	}
	auto_rebuild = p_enable;
	if (is_inside_tree()) {
		_auto_connect();
	}
}

// Follows the source generator's "generated", and queues a rebuild when it already has a result and nothing is drawn.
void Landscape3D::_auto_connect() {
	_auto_disconnect();
	if (!auto_rebuild || !is_inside_tree()) {
		return;
	}
	TerrainGenerator3D *gen = Object::cast_to<TerrainGenerator3D>(source.is_empty() ? nullptr : get_node_or_null(source));
	if (!gen) {
		return;
	}
	gen->connect(SNAME("generated"), callable_mp(this, &Landscape3D::_on_source_generated));
	auto_source = gen->get_instance_id();
	if (gen->has_result() && chunks.is_empty()) {
		_on_source_generated();
	}
}

void Landscape3D::_auto_disconnect() {
	if (TerrainGenerator3D *gen = Object::cast_to<TerrainGenerator3D>(ObjectDB::get_instance(auto_source))) {
		const Callable cb = callable_mp(this, &Landscape3D::_on_source_generated);
		if (gen->is_connected(SNAME("generated"), cb)) {
			gen->disconnect(SNAME("generated"), cb);
		}
	}
	auto_source = ObjectID();
}

void Landscape3D::_on_source_generated() {
	if (rebuild_pending) {
		return;
	}
	rebuild_pending = true;
	callable_mp(this, &Landscape3D::_auto_rebuild).call_deferred();
}

void Landscape3D::_auto_rebuild() {
	if (!rebuild_pending || !is_inside_tree()) {
		rebuild_pending = false;
		return;
	}
	rebuild();
}

// A TerrainMaterial3D drawn on a generator's ground reads that generator's geology and erosion detail.
void Landscape3D::_auto_material(Node *p_source) {
	TerrainGenerator3D *gen = Object::cast_to<TerrainGenerator3D>(p_source);
	if (!gen || !gen->has_result() || material.is_null() || !material->has_method(SNAME("use_generator"))) {
		return;
	}
	if (Object::cast_to<Object>(material->call(SNAME("get_generator"))) == gen) {
		return;
	}
	material->call(SNAME("use_generator"), gen);
	material->call(SNAME("update_shader"));
}

Callable Landscape3D::get_rebuild_button() {
	return callable_mp(this, &Landscape3D::rebuild);
}

bool Landscape3D::rebuild() {
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	rebuild_pending = false;
	clear();
	// Valid points from the source (vw x vh), then the grid rounded up to whole chunks, clamped at the far edges.
	Ref<Image> img;
	Node *src = source.is_empty() ? nullptr : get_node_or_null(source);
	_auto_material(src);
	if (src) {
		ERR_FAIL_COND_V_MSG(!src->has_method("sample_heights"), false, "Landscape3D: the source needs sample_heights(origin, spacing, size), as TerrainGenerator3D has.");
		if (src->has_method("get_world_rect")) {
			const Rect2 rect = src->call("get_world_rect");
			origin = rect.position;
			vw = int(Math::floor(rect.size.x / spacing)) + 1;
			vh = int(Math::floor(rect.size.y / spacing)) + 1;
		}
	} else if (height_image.is_valid() && !height_image->is_empty()) {
		img = height_image->duplicate();
		if (img->is_compressed()) {
			img->decompress();
		}
		img->convert(Image::FORMAT_RF);
		vw = img->get_width();
		vh = img->get_height();
	}
	ERR_FAIL_COND_V_MSG(vw < 2 || vh < 2, false, "Landscape3D: no heights (set source to a TerrainGenerator3D, or height_image).");
	cw = MAX(1, (vw - 2) / chunk_quads + 1);
	ch = MAX(1, (vh - 2) / chunk_quads + 1);
	const int new_gw = cw * chunk_quads + 1, new_gh = ch * chunk_quads + 1;
	ERR_FAIL_COND_V_MSG(int64_t(new_gw) * new_gh > 256 * 1024 * 1024, false, "Landscape3D: grid too large; raise spacing.");
	{
		RWLockWrite hl(heights_lock);
		gw = new_gw;
		gh = new_gh;
		heights.resize(gw * gh);
	}
	int max_levels = 1;
	while ((chunk_quads >> max_levels) >= 2 && max_levels < 8) {
		max_levels++;
	}
	levels = MIN(lod_levels, max_levels);
	for (int l = 0; l < levels; l++) {
		for (int m = 0; m < 16; m++) {
			_indices(l, m);
		}
	}
	chunks.resize(cw * ch);
	TerrainGenerator3D *gen = Object::cast_to<TerrainGenerator3D>(src);
	const bool streaming = stream_heights && gen && levels > COARSE + 1;
	WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
	if (streaming) {
		// The 4 m grid first, the rest interpolated until read.
		const int rows = (gh - 1) / 4 + 1;
		WorkerThreadPool::GroupID g = pool->add_template_group_task(this, &Landscape3D::_coarse_task, (void *)gen, rows, -1, true, SNAME("Landscape3D"));
		pool->wait_for_group_task_completion(g);
		g = pool->add_template_group_task(this, &Landscape3D::_fill_task, (void *)nullptr, gh, -1, true, SNAME("Landscape3D"));
		pool->wait_for_group_task_completion(g);
	} else {
		if (src) {
			const Ref<Image> got = src->call("sample_heights", origin, spacing, Vector2i(vw, vh));
			ERR_FAIL_COND_V(got.is_null() || got->get_width() != vw || got->get_height() != vh, false);
			img = got;
		}
		const float *in = reinterpret_cast<const float *>(img->get_data().ptr());
		for (int z = 0; z < gh; z++) {
			const int zz = MIN(z, vh - 1);
			for (int x = 0; x < gw; x++) {
				const float h = in[zz * vw + MIN(x, vw - 1)];
				heights[z * gw + x] = Math::is_nan(h) ? 0.0f : h;
			}
		}
	}
	const uint64_t t_src = OS::get_singleton()->get_ticks_usec();
	WorkerThreadPool::GroupID g = pool->add_template_group_task(this, &Landscape3D::_measure_task, (void *)nullptr, cw * ch, -1, true, SNAME("Landscape3D"));
	pool->wait_for_group_task_completion(g);
	if (streaming) {
		stream_source = gen->get_instance_id();
		const Callable cb = callable_mp(this, &Landscape3D::_on_source_changing);
		gen->connect(SNAME("changing"), cb);
		gen->connect(SceneStringName(tree_exiting), cb);
		for (Chunk &c : chunks) {
			c.fine = false;
			for (int l = 0; l < COARSE; l++) {
				c.error[l] = UNREAD;
			}
		}
		coarse_chunks = int(chunks.size());
		// How much the 4 m grid misses: read a sparse set of chunks in full and compare their errors.
		LocalVector<int> samples;
		for (int z = MIN(3, ch / 2); z < ch; z += 8) {
			for (int x = MIN(3, cw / 2); x < cw; x += 8) {
				samples.push_back(z * cw + x);
			}
		}
		LocalVector<float> before;
		for (int i : samples) {
			for (int l = COARSE; l < levels; l++) {
				before.push_back(chunks[i].error[l]);
			}
		}
		_read_chunks(samples);
		// Per level: the most a read chunk's error grew over its 4 m estimate, with a margin.
		float miss[8] = {};
		for (uint32_t s = 0; s < samples.size(); s++) {
			for (int l = COARSE; l < levels; l++) {
				miss[l] = MAX(miss[l], chunks[samples[s]].error[l] - before[s * (levels - COARSE) + l - COARSE]);
			}
		}
		detail_estimate = 0.0f;
		for (int l = COARSE; l < levels; l++) {
			miss[l] *= 1.25f;
			detail_estimate = MAX(detail_estimate, miss[l]);
		}
		for (Chunk &c : chunks) {
			if (!c.fine) {
				for (int l = COARSE; l < levels; l++) {
					c.error[l] += miss[l];
				}
				for (int l = COARSE + 1; l < levels; l++) {
					c.error[l] = MAX(c.error[l], c.error[l - 1]);
				}
			}
		}
	}
	const uint64_t t_measure = OS::get_singleton()->get_ticks_usec();
	_create_instances();
	Camera3D *cam = _camera();
	if (cam && cam->get_viewport()) {
		// Reads what the first view needs before showing it, so the first frame is exact.
		const Transform3D xf = cam->get_global_transform();
		const Projection pr = cam->get_camera_projection();
		const real_t vp_h = cam->get_viewport()->get_visible_rect().size.y;
		_update_lods(xf, pr, vp_h, 3);
	} else {
		// No camera yet: the coarsest level everywhere (cheap); the first camera refines it.
		for (uint32_t i = 0; i < chunks.size(); i++) {
			_set_chunk_mesh(i, levels - 1, 0);
		}
	}
	if (TerrainGenerator3D *hg = Object::cast_to<TerrainGenerator3D>(src)) {
		hole_source = hg->get_instance_id();
		const Callable hcb = callable_mp(this, &Landscape3D::_on_holes_changed);
		if (!hg->is_connected(SNAME("holes_changed"), hcb)) {
			hg->connect(SNAME("holes_changed"), hcb);
		}
		if (hg->get_hole_count() > 0) {
			_refresh_holes(Rect2(-1e9, -1e9, 2e9, 2e9));
		}
	}
	_update_collision();
	update_gizmos();
	const uint64_t t_end = OS::get_singleton()->get_ticks_usec();
	last_build_msec = double(t_end - t0) / 1000.0;
	last_source_msec = double(t_src - t0) / 1000.0;
	last_measure_msec = double(t_measure - t_src) / 1000.0;
	return true;
}

int Landscape3D::get_chunk_lod(const Vector2i &p_chunk) const {
	ERR_FAIL_COND_V(p_chunk.x < 0 || p_chunk.y < 0 || p_chunk.x >= cw || p_chunk.y >= ch, -1);
	return chunks[p_chunk.y * cw + p_chunk.x].lod;
}

int Landscape3D::get_chunk_edge_mask(const Vector2i &p_chunk) const {
	ERR_FAIL_COND_V(p_chunk.x < 0 || p_chunk.y < 0 || p_chunk.x >= cw || p_chunk.y >= ch, -1);
	return chunks[p_chunk.y * cw + p_chunk.x].mask & 15;
}

bool Landscape3D::is_chunk_loaded(const Vector2i &p_chunk) const {
	ERR_FAIL_COND_V(p_chunk.x < 0 || p_chunk.y < 0 || p_chunk.x >= cw || p_chunk.y >= ch, false);
	return chunks[p_chunk.y * cw + p_chunk.x].fine;
}

real_t Landscape3D::get_chunk_lod_error(const Vector2i &p_chunk, int p_level) const {
	ERR_FAIL_COND_V(p_chunk.x < 0 || p_chunk.y < 0 || p_chunk.x >= cw || p_chunk.y >= ch, -1.0);
	ERR_FAIL_INDEX_V(p_level, levels, -1.0);
	return chunks[p_chunk.y * cw + p_chunk.x].error[p_level];
}

// The drawn triangles of a chunk in global space, three points each.
PackedVector3Array Landscape3D::get_chunk_triangles(const Vector2i &p_chunk) const {
	PackedVector3Array out;
	ERR_FAIL_COND_V(p_chunk.x < 0 || p_chunk.y < 0 || p_chunk.x >= cw || p_chunk.y >= ch, out);
	const Chunk &c = chunks[p_chunk.y * cw + p_chunk.x];
	const LocalVector<int> &idx = indices[c.lod][c.mask & 15];
	ERR_FAIL_COND_V(idx.is_empty(), out);
	const Transform3D xf = is_inside_tree() ? get_global_transform() : Transform3D();
	const int s = 1 << c.lod;
	const int row = (chunk_quads >> c.lod) + 1;
	out.resize(idx.size());
	Vector3 *w = out.ptrw();
	for (uint32_t i = 0; i < idx.size(); i++) {
		const int x = p_chunk.x * chunk_quads + (idx[i] % row) * s;
		const int z = p_chunk.y * chunk_quads + (idx[i] / row) * s;
		w[i] = xf.xform(Vector3(origin.x + x * spacing, _h(x, z), origin.y + z * spacing));
	}
	return out;
}

// Level 0 ground under a global point, bilinear between the grid heights.
real_t Landscape3D::get_height(const Vector3 &p_position) const {
	RWLockRead hl(heights_lock);
	if (heights.is_empty()) {
		return 0.0;
	}
	const Vector3 p = (is_inside_tree() ? get_global_transform().affine_inverse() : Transform3D()).xform(p_position);
	const real_t fx = (p.x - origin.x) / spacing;
	const real_t fz = (p.z - origin.y) / spacing;
	const int x = int(Math::floor(fx));
	const int z = int(Math::floor(fz));
	const real_t u = fx - x, v = fz - z;
	return Math::lerp(Math::lerp(_h(x, z), _h(x + 1, z), float(u)), Math::lerp(_h(x, z + 1), _h(x + 1, z + 1), float(u)), float(v));
}

AABB Landscape3D::get_aabb() const {
	if (chunks.is_empty()) {
		return AABB();
	}
	float lo = 1e30f, hi = -1e30f;
	for (const Chunk &c : chunks) {
		lo = MIN(lo, c.min_h);
		hi = MAX(hi, c.max_h);
	}
	return AABB(Vector3(origin.x, lo, origin.y), Vector3((gw - 1) * spacing, hi - lo, (gh - 1) * spacing));
}

Dictionary Landscape3D::get_stats() const {
	Dictionary d;
	int tris = 0;
	PackedInt32Array per_level;
	per_level.resize(levels);
	per_level.fill(0);
	for (const Chunk &c : chunks) {
		if (c.mesh_key != UINT64_MAX) {
			tris += meshes[c.mesh_key].triangles;
			per_level.set(c.lod, per_level[c.lod] + 1);
		}
	}
	d["chunks"] = int(chunks.size());
	d["triangles"] = tris;
	d["chunks_per_level"] = per_level;
	d["meshes"] = int(meshes.size());
	d["mesh_builds"] = int64_t(mesh_builds);
	d["lod_switches"] = int64_t(lod_switches);
	d["build_msec"] = last_build_msec;
	d["source_msec"] = last_source_msec;
	d["measure_msec"] = last_measure_msec;
	d["update_usec"] = last_update_usec;
	d["apply_usec"] = last_apply_usec;
	d["unread_chunks"] = coarse_chunks;
	d["chunks_streamed"] = int64_t(chunks_streamed);
	d["detail_estimate"] = detail_estimate;
	d["busy"] = mesh_group >= 0 || height_group >= 0;
	d["collision_tiles"] = collision_tiles;
	d["collision_usec"] = last_collision_usec;
	return d;
}


// The stand's ground map (docs/ideas/terrain-material.md, "Trails and rooting"): WorldStand's worn trails and boar
// rooting, rasterised round the camera for TerrainMaterial3D. Trails: the tread share (trail_at's: flat over the
// middle half, easing to the edge) times the wear their passes give. Trampling takes the field layer's cover off
// roughly as 1 - exp(-passes / P) (Cole 1995, Liddle 1997: a few hundred passes a year keep a line bare in most
// field layers), P = 300 here. Rooting: the freshness of turned soil, 1 up to a season after the boar, gone by two
// years as litter and the field layer close over it. Burn scars: years since the ground last burnt (WorldFire's
// burn_age_at, a fire in play or an authored WorldBurnScar3D), for the material's char overlay. Rebuilt when the
// camera crosses a 64 m step or the stand changes; the burn channel also when the fire reports burnt ground changed
// (only the area it names).
void Landscape3D::_update_stand_map(const Vector3 &p_camera) {
	WorldStand *st = WorldContracts::stand();
	WorldFire *fire = WorldContracts::fire();
	const bool has_fire = fire && fire->has_provider();
	if (!st && !has_fire) {
		_clear_stand_map();
		return;
	}
	const real_t step = 64.0;
	const Vector2 origin = Vector2(Math::round(p_camera.x / step) * step, Math::round(p_camera.z / step) * step) - Vector2(STAND_SIZE, STAND_SIZE) * 0.5;
	const uint64_t gen = st ? st->get_stand_generation() : 0;
	const ObjectID sid = st ? st->get_instance_id() : ObjectID();
	const uint64_t fgen = has_fire ? fire->get_burn_generation() : 0;
	const bool moved = origin != stand_origin;
	const bool stand_changed = moved || gen != stand_generation || sid != stand_id;
	const bool fire_changed = moved || has_fire != stand_fire || fgen != stand_burn_generation;
	if (!stand_changed && !fire_changed) {
		return;
	}
	const int R = STAND_RES;
	const real_t texel = STAND_SIZE / real_t(R);
	const int G = int(STAND_SIZE / 2.0) + 1;
	if (stand_changed) {
		stand_origin = origin;
		stand_generation = gen;
		stand_id = sid;
		stand_trail.resize(R * R);
		memset(stand_trail.ptr(), 0, stand_trail.size());
		stand_rooting.resize(G * G);
		for (float &v : stand_rooting) {
			v = 0.0f;
		}
		stand_any = false;
		if (st) {
			stand_any = _raster_stand(st, origin, texel);
		}
	}
	if (fire_changed) {
		// Burn scars: years since the ground last burnt (WorldFire.burn_age_at), sampled every 2 m like the rooting.
		// Only the area the fire's change log names is read again when the map has not moved (a fire in play reports
		// a few times a second); everything after a move, or once the log no longer reaches back.
		Rect2i cells(0, 0, G, G);
		if (!moved && has_fire && stand_fire && stand_burn.size() == uint32_t(G * G)) {
			Rect2 area;
			if (fire->burn_changes_since(stand_burn_generation, area)) {
				const Rect2 g = Rect2(area.position - origin, area.size).grow(2.0);
				const Vector2i c0 = Vector2i(CLAMP(int(Math::floor(g.position.x / 2.0)), 0, G), CLAMP(int(Math::floor(g.position.y / 2.0)), 0, G));
				const Vector2i c1 = Vector2i(CLAMP(int(Math::ceil(g.get_end().x / 2.0)) + 1, 0, G), CLAMP(int(Math::ceil(g.get_end().y / 2.0)) + 1, 0, G));
				cells = Rect2i(c0, (c1 - c0).maxi(0));
			}
		}
		stand_burn.resize(G * G);
		if (cells.size.x == G && cells.size.y == G) {
			for (float &v : stand_burn) {
				v = -1.0f;
			}
		}
		if (has_fire) {
			for (int z = cells.position.y; z < cells.get_end().y; z++) {
				for (int x = cells.position.x; x < cells.get_end().x; x++) {
					stand_burn[z * G + x] = fire->burn_age_at(Vector3(origin.x + x * 2.0, 0.0, origin.y + z * 2.0));
				}
			}
		}
		stand_fire = has_fire;
		stand_burn_generation = fgen;
		stand_any_burn = false;
		for (const float b : stand_burn) {
			stand_any_burn = stand_any_burn || b >= 0.0f;
		}
	}
	if (!stand_any && !stand_any_burn) {
		_unpublish_stand_map();
		return;
	}
	// RGBA8: r trail wear, g fresh rooting, b burn: 1 - years / BURN_YEARS (0 never burnt, so a scar as old as that
	// still reads as burnt; TerrainMaterial3D fades the char over those years), a unused.
	Vector<uint8_t> data;
	data.resize(R * R * 4);
	uint8_t *w = data.ptrw();
	auto bilerp = [&](const LocalVector<float> &p_grid, real_t p_gx, real_t p_gz, float &r_v) {
		const int ix = MIN(int(p_gx), G - 2);
		const int iz = MIN(int(p_gz), G - 2);
		const float fx = float(p_gx - ix), fz = float(p_gz - iz);
		const float r0 = Math::lerp(p_grid[iz * G + ix], p_grid[iz * G + ix + 1], fx);
		const float r1 = Math::lerp(p_grid[(iz + 1) * G + ix], p_grid[(iz + 1) * G + ix + 1], fx);
		r_v = Math::lerp(r0, r1, fz);
	};
	auto burn_v = [](float p_age) { return p_age < 0.0f ? 0.0f : 1.0f - MIN(p_age, BURN_YEARS) / (BURN_YEARS * 1.02f); };
	for (int z = 0; z < R; z++) {
		const real_t gz = ((z + 0.5) * texel) / 2.0;
		for (int x = 0; x < R; x++) {
			const real_t gx = ((x + 0.5) * texel) / 2.0;
			float g = 0.0f;
			if (stand_any) {
				bilerp(stand_rooting, gx, gz, g);
			}
			float b = 0.0f;
			if (stand_any_burn) {
				// The four corners' burn values, not their ages (an unburnt corner is 0, not an age of -1).
				const int ix = MIN(int(gx), G - 2);
				const int iz = MIN(int(gz), G - 2);
				const float fx = float(gx - ix), fz = float(gz - iz);
				const float b0 = Math::lerp(burn_v(stand_burn[iz * G + ix]), burn_v(stand_burn[iz * G + ix + 1]), fx);
				const float b1 = Math::lerp(burn_v(stand_burn[(iz + 1) * G + ix]), burn_v(stand_burn[(iz + 1) * G + ix + 1]), fx);
				b = Math::lerp(b0, b1, fz);
			}
			const int i = (z * R + x) * 4;
			w[i] = stand_any ? stand_trail[z * R + x] : 0;
			w[i + 1] = uint8_t(CLAMP(g * 255.0f, 0.0f, 255.0f));
			w[i + 2] = uint8_t(CLAMP(b * 255.0f + 0.5f, 0.0f, 255.0f));
			w[i + 3] = 0;
		}
	}
	Ref<Image> img = Image::create_from_data(R, R, false, Image::FORMAT_RGBA8, data);
	if (stand_texture.is_valid() && stand_texture->get_width() == R && stand_texture->get_format() == Image::FORMAT_RGBA8) {
		stand_texture->update(img);
	} else {
		stand_texture = ImageTexture::create_from_image(img);
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	rs->global_shader_parameter_set("terrain_stand_map", stand_texture->get_rid());
	rs->global_shader_parameter_set("terrain_stand_rect", Vector4(origin.x, origin.y, STAND_SIZE, STAND_SIZE));
	stand_published = true;
}

// The stand's trails and rooting into stand_trail (bytes, STAND_RES square) and stand_rooting (2 m grid); true if any.
bool Landscape3D::_raster_stand(WorldStand *st, const Vector2 &origin, real_t texel) {
	const int R = STAND_RES;
	const bool scripted = st->get_script_instance() != nullptr;
	LocalVector<uint8_t> &trail = stand_trail;
	bool any = false;
	auto wear = [](float p_passes) { return 1.0f - Math::exp(-MAX(p_passes, 0.0f) / 300.0f); };
	if (scripted) {
		// A script stand answers per point only: sampled at 1 m, spread over the texels.
		const int C = int(STAND_SIZE);
		for (int z = 0; z < C; z++) {
			for (int x = 0; x < C; x++) {
				const Vector2 t = st->trail_at(Vector3(origin.x + x + 0.5, 0.0, origin.y + z + 0.5));
				const float v = t.y * wear(t.x);
				if (v <= 0.0f) {
					continue;
				}
				any = true;
				const uint8_t b = uint8_t(CLAMP(v * 255.0f, 0.0f, 255.0f));
				for (int dz = 0; dz < 2; dz++) {
					for (int dx = 0; dx < 2; dx++) {
						trail[(z * 2 + dz) * R + x * 2 + dx] = b;
					}
				}
			}
		}
	} else {
		const Array trails = st->get_trails();
		for (int t = 0; t < trails.size(); t++) {
			const Dictionary d = trails[t];
			LocalVector<Vector2> pts;
			const Variant pv = d.get("points", Variant());
			if (pv.get_type() == Variant::PACKED_VECTOR3_ARRAY) {
				for (const Vector3 &q : PackedVector3Array(pv)) {
					pts.push_back(Vector2(q.x, q.z));
				}
			} else if (pv.get_type() == Variant::PACKED_VECTOR2_ARRAY) {
				for (const Vector2 &q : PackedVector2Array(pv)) {
					pts.push_back(q);
				}
			}
			const float hw = 0.5f * MAX(float(d.get("width", 0.6f)), 0.05f);
			const float w = wear(float(d.has("passes") ? d["passes"] : d.get("passes_per_year", 0.0f)));
			if (w <= 0.0f) {
				continue;
			}
			for (uint32_t i = 0; i + 1 < pts.size(); i++) {
				const Vector2 a = pts[i], b = pts[i + 1];
				const int x0 = MAX(0, int(Math::floor((MIN(a.x, b.x) - hw - origin.x) / texel))), x1 = MIN(R - 1, int(Math::ceil((MAX(a.x, b.x) + hw - origin.x) / texel)));
				const int z0 = MAX(0, int(Math::floor((MIN(a.y, b.y) - hw - origin.y) / texel))), z1 = MIN(R - 1, int(Math::ceil((MAX(a.y, b.y) + hw - origin.y) / texel)));
				for (int z = z0; z <= z1; z++) {
					for (int x = x0; x <= x1; x++) {
						const Vector2 p(origin.x + (x + 0.5) * texel, origin.y + (z + 0.5) * texel);
						const float dist = p.distance_to(Geometry2D::get_closest_point_to_segment(p, a, b));
						if (dist > hw) {
							continue;
						}
						const float v = (1.0f - Math::smoothstep(0.5f * hw, hw, dist)) * w;
						const uint8_t bv = uint8_t(CLAMP(v * 255.0f, 0.0f, 255.0f));
						if (bv > trail[z * R + x]) {
							trail[z * R + x] = bv;
							any = true;
						}
					}
				}
			}
		}
	}
	// Rooting: sampled every 2 m (the discs are metres across), bilinear over the texels.
	const int G = int(STAND_SIZE / 2.0) + 1;
	LocalVector<float> &fresh = stand_rooting;
	bool any_rooting = false;
	for (int z = 0; z < G; z++) {
		for (int x = 0; x < G; x++) {
			const float age = st->rooting_age_at(Vector3(origin.x + x * 2.0, 0.0, origin.y + z * 2.0));
			const float f = age < 0.0f ? 0.0f : 1.0f - Math::smoothstep(0.25f, 2.0f, age);
			fresh[z * G + x] = f;
			any_rooting = any_rooting || f > 0.0f;
		}
	}
	return any || any_rooting;
}

void Landscape3D::_unpublish_stand_map() {
	if (stand_published) {
		RenderingServer::get_singleton()->global_shader_parameter_set("terrain_stand_rect", Vector4());
		stand_published = false;
	}
}

void Landscape3D::_clear_stand_map() {
	_unpublish_stand_map();
	stand_origin = Vector2(1e30, 1e30);
	stand_generation = UINT64_MAX;
	stand_id = ObjectID();
	stand_fire = false;
	stand_burn_generation = UINT64_MAX;
	stand_burn.clear();
	stand_any = false;
	stand_any_burn = false;
}

// The drawn ground over the heightfield at a point's xz (landscape space): x metres up, y the relief's share. Cliff and
// bank relief push a surface point out along the slope's horizontal normal m by d, so at a fixed xz the surface stands
// d tan(slope) higher (outward is downhill). The full field: the stitch weight on sides next to coarser chunks and the
// finest-chunk-only drawing are left out (both are under a pixel by design).
Vector2 Landscape3D::_relief_at_local(const Vector3 &p, bool p_steps) const {
	if (heights.is_empty() || (cliff_relief <= 0.0 && bank_relief <= 0.0 && terracette_relief <= 0.0)) {
		return Vector2(0.0, 0.0);
	}
	const real_t fx = (p.x - origin.x) / spacing, fz = (p.z - origin.y) / spacing;
	if (fx < 0.0 || fz < 0.0 || fx > real_t(gw - 1) || fz > real_t(gh - 1)) {
		return Vector2(NAN, 0.0);
	}
	const Vector3 n = _normal_at(fx, fz);
	const real_t ml = Vector2(n.x, n.z).length();
	if (ml < 1e-4f || n.y < 1e-3f) {
		return Vector2(0.0, 0.0);
	}
	const int ix = int(Math::floor(fx)), iz = int(Math::floor(fz));
	const real_t u = fx - ix, v = fz - iz;
	const real_t h = Math::lerp(Math::lerp(real_t(_h(ix, iz)), real_t(_h(ix + 1, iz)), u), Math::lerp(real_t(_h(ix, iz + 1)), real_t(_h(ix + 1, iz + 1)), u), v);
	// The terracettes' steps as the near ground draws them (half a metre's filter, twice the near patch's grid).
	real_t tc_share = p_steps ? terracette_relief : real_t(0.0);
	if (tc_share > 0.0) {
		// On open soil only, as TerrainMaterial3D's (WorldContracts::terracette_soil_share).
		const TerrainGenerator3D *gen = _stream_generator();
		if (gen) {
			tc_share *= WorldContracts::terracette_soil_share(gen->get_material_geology_at(Vector3(p.x, h, p.z)));
		}
	}
	const real_t tc = p_steps ? real_t(WorldContracts::terracette_step(Vector3(p.x, h, p.z), Vector2(-n.x, -n.z) / n.y, 0.5f, float(tc_share)).x) : real_t(0.0);
	const Vector2 w = _relief_weights_at(fx, fz);
	if (w.x <= 0.0 && w.y <= 0.0) {
		return Vector2(tc, 0.0);
	}
	const real_t fold = _relief_fold_scale(fx, fz);
	const Vector3 q(p.x, h, p.z);
	real_t d = 0.0;
	if (w.x > 0.0 && cliff_relief > 0.0) {
		const real_t tn = ml / MAX(n.y, real_t(1e-3));
		const real_t k = _cliff_scale(q, cliff_relief * w.x * fold, tn);
		d += k * _cliff_offset(q, cliff_relief * tn, cliff_relief, false);
		d += cliff_relief * w.x * fold * _joint_offset(q, cliff_relief * tn);
	}
	if (w.y > 0.0 && bank_relief > 0.0) {
		d += _bank_scale(q, bank_relief * w.y * fold, ml / MAX(n.y, real_t(1e-3))) * _bank_offset(q);
	}
	return Vector2(d * ml / n.y + tc, MIN(w.x + w.y, real_t(1.0)));
}

Vector2 Landscape3D::contract_relief_at(const Vector3 &p_position, bool p_steps) const {
	RWLockRead hl(heights_lock);
	const Transform3D xf = is_inside_tree() ? get_global_transform() : Transform3D();
	return _relief_at_local(xf.affine_inverse().xform(p_position), p_steps);
}

// What contract_relief_at reads besides the stored properties: the placement, the grid and its heights as read so far.
void Landscape3D::contract_relief_state(void (*p_add)(void *p_user, const void *p_data, size_t p_size), void *p_user) const {
	RWLockRead hl(heights_lock);
	const Transform3D xf = is_inside_tree() ? get_global_transform() : Transform3D();
	p_add(p_user, &xf, sizeof(xf));
	const real_t grid[] = { origin.x, origin.y, spacing, cliff_relief, bank_relief, terracette_relief };
	p_add(p_user, grid, sizeof(grid));
	const int32_t dims[] = { gw, gh };
	p_add(p_user, dims, sizeof(dims));
	p_add(p_user, heights.ptr(), sizeof(float) * heights.size());
}

void Landscape3D::_update_relief_map(const Vector3 &p_camera) {
	if (heights.is_empty() || (cliff_relief <= 0.0 && bank_relief <= 0.0)) {
		_clear_relief_map();
		_clear_cliff_map();
		return;
	}
	// Landscape space is world space for the map (as for the stand map).
	const real_t step = 16.0;
	const Vector2 org = Vector2(Math::round(p_camera.x / step) * step, Math::round(p_camera.z / step) * step) - Vector2(RELIEF_SIZE, RELIEF_SIZE) * 0.5;
	// Rebuilt when the camera moves a step, the relief settings change or any chunk's heights change (their gen).
	uint64_t gens = 0;
	for (uint32_t i = 0; i < chunks.size(); i++) {
		gens += uint64_t(chunks[i].gen) * (i + 1);
	}
	const uint64_t key = hash_murmur3_one_64(uint64_t(gw) * 73856093u ^ uint64_t(gh), hash_murmur3_one_double(cliff_relief, hash_murmur3_one_double(bank_relief, hash_murmur3_one_64(gens))));
	if (org == relief_origin && key == relief_key) {
		return;
	}
	relief_origin = org;
	relief_key = key;
	_update_cliff_map(org + Vector2(RELIEF_SIZE, RELIEF_SIZE) * 0.5);
	const int R = RELIEF_RES;
	const real_t texel = RELIEF_SIZE / real_t(R);
	Vector<uint8_t> data;
	data.resize(R * R * 4);
	uint16_t *w = reinterpret_cast<uint16_t *>(data.ptrw());
	bool any = false;
	for (int z = 0; z < R; z++) {
		for (int x = 0; x < R; x++) {
			// Without the terracettes' steps: a 3 m step pattern sampled at texel centres aliases, and placers on the
			// GPU add the steps at their own roots (wc_terracette_step).
			Vector2 r = _relief_at_local(Vector3(org.x + (x + 0.5) * texel, 0.0, org.y + (z + 0.5) * texel), false);
			if (Math::is_nan(r.x)) {
				r = Vector2(0.0, 0.0);
			}
			any = any || r.y > 0.0;
			w[(z * R + x) * 2] = Math::make_half_float(float(r.x));
			w[(z * R + x) * 2 + 1] = Math::make_half_float(float(r.y));
		}
	}
	if (!any) {
		_clear_relief_map();
		relief_origin = org;
		relief_key = key;
		return;
	}
	Ref<Image> img = Image::create_from_data(R, R, false, Image::FORMAT_RGH, data);
	if (relief_texture.is_valid() && relief_texture->get_width() == R) {
		relief_texture->update(img);
	} else {
		relief_texture = ImageTexture::create_from_image(img);
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	rs->global_shader_parameter_set("terrain_relief_map", relief_texture->get_rid());
	rs->global_shader_parameter_set("terrain_relief_rect", Vector4(org.x, org.y, RELIEF_SIZE, RELIEF_SIZE));
	relief_published = true;
}

// The cliff relief's share (Landscape3D::_relief_weights' x, as the meshes weight it) at CLIFF_MAP_SIZE / CLIFF_MAP_RES
// texels: only the heightfield's normal per texel, no relief offsets, so a square four times the relief map's width
// costs about a quarter of it.
void Landscape3D::_update_cliff_map(const Vector2 &p_center) {
	if (cliff_relief <= 0.0) {
		_clear_cliff_map();
		return;
	}
	const int R = CLIFF_MAP_RES;
	const real_t texel = CLIFF_MAP_SIZE / real_t(R);
	const Vector2 org = p_center - Vector2(CLIFF_MAP_SIZE, CLIFF_MAP_SIZE) * 0.5;
	Vector<uint8_t> data;
	data.resize(R * R);
	uint8_t *w = data.ptrw();
	bool any = false;
	for (int z = 0; z < R; z++) {
		for (int x = 0; x < R; x++) {
			const real_t fx = (org.x + (x + 0.5) * texel - origin.x) / spacing, fz = (org.y + (z + 0.5) * texel - origin.y) / spacing;
			float share = 0.0f;
			if (fx >= 0.0 && fz >= 0.0 && fx <= real_t(gw - 1) && fz <= real_t(gh - 1)) {
				share = CLAMP(float(_relief_weights_at(fx, fz).x), 0.0f, 1.0f);
			}
			any = any || share > 0.0f;
			w[z * R + x] = uint8_t(Math::round(share * 255.0f));
		}
	}
	if (!any) {
		_clear_cliff_map();
		return;
	}
	Ref<Image> img = Image::create_from_data(R, R, false, Image::FORMAT_R8, data);
	if (cliff_map_texture.is_valid() && cliff_map_texture->get_width() == R) {
		cliff_map_texture->update(img);
	} else {
		cliff_map_texture = ImageTexture::create_from_image(img);
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	rs->global_shader_parameter_set("terrain_cliff_map", cliff_map_texture->get_rid());
	rs->global_shader_parameter_set("terrain_cliff_rect", Vector4(org.x, org.y, CLIFF_MAP_SIZE, CLIFF_MAP_SIZE));
	cliff_map_published = true;
}

void Landscape3D::_clear_cliff_map() {
	if (cliff_map_published) {
		RenderingServer::get_singleton()->global_shader_parameter_set("terrain_cliff_rect", Vector4());
		cliff_map_published = false;
	}
}

void Landscape3D::_clear_relief_map() {
	if (relief_published) {
		RenderingServer::get_singleton()->global_shader_parameter_set("terrain_relief_rect", Vector4());
		relief_published = false;
	}
	relief_origin = Vector2(1e30, 1e30);
	relief_key = 0;
}

void Landscape3D::_publish_cliff_relief(bool p_clear) {
	const Vector4 v = p_clear || heights.is_empty() ? Vector4() : Vector4(cliff_relief, spacing, origin.x, origin.y);
	const Vector2 b = p_clear || heights.is_empty() ? Vector2() : Vector2(bank_relief, terracette_relief);
	if (b != bank_published) {
		bank_published = b;
		RenderingServer::get_singleton()->global_shader_parameter_set("terrain_bank_relief", Vector4(b.x, b.y, 0, 0));
	}
	if (v == cliff_published) {
		return;
	}
	cliff_published = v;
	RenderingServer::get_singleton()->global_shader_parameter_set("terrain_cliff_relief", v);
}

void Landscape3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_WORLD: {
			_create_instances();
			_auto_connect();
			if (!relief_registered) {
				WorldContracts::register_relief(this);
				relief_registered = true;
				_relief_changed(Rect2());
			}
			set_process_internal(true);
		} break;
		case NOTIFICATION_EXIT_WORLD: {
			_auto_disconnect();
			_wait_all();
			_free_instances();
			_free_all_collision();
			_clear_stand_map();
			_clear_relief_map();
			_clear_cliff_map();
			if (relief_registered) {
				WorldContracts::unregister_relief(this);
				relief_registered = false;
			}
			_publish_cliff_relief(true);
			set_process_internal(false);
		} break;
		case NOTIFICATION_TRANSFORM_CHANGED: {
			_update_transforms();
			if (is_inside_tree()) {
				for (Chunk &c : chunks) {
					if (c.body.is_valid()) {
						PhysicsServer3D::get_singleton()->body_set_state(c.body, PhysicsServer3D::BODY_STATE_TRANSFORM, get_global_transform());
					}
				}
			}
			lods_dirty = true;
		} break;
		case NOTIFICATION_VISIBILITY_CHANGED: {
			for (Chunk &c : chunks) {
				if (c.instance.is_valid()) {
					RS::get_singleton()->instance_set_visible(c.instance, is_visible_in_tree());
				}
			}
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			frame++;
			if (chunks.is_empty()) {
				break;
			}
			// Built meshes in, read chunks in (both on the main thread, between worker batches), then a new pick
			// when the view or the heights changed, then the next chunks to read.
			_finish_meshes(false);
			_finish_heights(false);
			Camera3D *cam = _camera();
			if (mesh_group < 0 && cam && cam->get_viewport()) {
				const Transform3D xf = cam->get_global_transform();
				const Projection pr = cam->get_camera_projection();
				const real_t vp_h = cam->get_viewport()->get_visible_rect().size.y;
				if (lods_dirty || !have_camera || xf != last_camera || pr != last_projection || vp_h != last_viewport_height) {
					const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
					_pick_lods(xf, pr, vp_h);
					last_update_usec = double(OS::get_singleton()->get_ticks_usec() - t0);
					last_camera = xf;
					last_projection = pr;
					last_viewport_height = vp_h;
					have_camera = true;
					lods_dirty = false;
					_launch_meshes(false);
				}
			}
			if (cam) {
				_update_stand_map(cam->get_global_transform().origin);
				_update_relief_map(cam->get_global_transform().origin);
			}
			_publish_cliff_relief();
			_update_collision();
			_launch_heights();
			if (frame % 60 == 0) {
				_evict(false);
			}
		} break;
	}
}

Landscape3D::Landscape3D() {
	set_notify_transform(true);
}

Landscape3D::~Landscape3D() {
	clear();
}

void Landscape3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_source", "path"), &Landscape3D::set_source);
	ClassDB::bind_method(D_METHOD("get_source"), &Landscape3D::get_source);
	ClassDB::bind_method(D_METHOD("set_height_image", "image"), &Landscape3D::set_height_image);
	ClassDB::bind_method(D_METHOD("get_height_image"), &Landscape3D::get_height_image);
	ClassDB::bind_method(D_METHOD("set_origin", "origin"), &Landscape3D::set_origin);
	ClassDB::bind_method(D_METHOD("get_origin"), &Landscape3D::get_origin);
	ClassDB::bind_method(D_METHOD("set_spacing", "spacing"), &Landscape3D::set_spacing);
	ClassDB::bind_method(D_METHOD("get_spacing"), &Landscape3D::get_spacing);
	ClassDB::bind_method(D_METHOD("set_chunk_quads", "quads"), &Landscape3D::set_chunk_quads);
	ClassDB::bind_method(D_METHOD("get_chunk_quads"), &Landscape3D::get_chunk_quads);
	ClassDB::bind_method(D_METHOD("set_lod_levels", "levels"), &Landscape3D::set_lod_levels);
	ClassDB::bind_method(D_METHOD("get_lod_levels"), &Landscape3D::get_lod_levels);
	ClassDB::bind_method(D_METHOD("set_lod_error", "pixels"), &Landscape3D::set_lod_error);
	ClassDB::bind_method(D_METHOD("get_lod_error"), &Landscape3D::get_lod_error);
	ClassDB::bind_method(D_METHOD("set_material", "material"), &Landscape3D::set_material);
	ClassDB::bind_method(D_METHOD("get_material"), &Landscape3D::get_material);
	ClassDB::bind_method(D_METHOD("set_cliff_relief", "metres"), &Landscape3D::set_cliff_relief);
	ClassDB::bind_method(D_METHOD("get_cliff_relief"), &Landscape3D::get_cliff_relief);
	ClassDB::bind_method(D_METHOD("set_bank_relief", "metres"), &Landscape3D::set_bank_relief);
	ClassDB::bind_method(D_METHOD("set_terracette_relief", "share"), &Landscape3D::set_terracette_relief);
	ClassDB::bind_method(D_METHOD("get_terracette_relief"), &Landscape3D::get_terracette_relief);
	ClassDB::bind_method(D_METHOD("get_bank_relief"), &Landscape3D::get_bank_relief);
	ClassDB::bind_method(D_METHOD("get_cliff_point", "global_position"), &Landscape3D::get_cliff_point);
	ClassDB::bind_method(D_METHOD("set_cast_shadows", "enable"), &Landscape3D::set_cast_shadows);
	ClassDB::bind_method(D_METHOD("is_casting_shadows"), &Landscape3D::is_casting_shadows);
	ClassDB::bind_method(D_METHOD("set_camera", "path"), &Landscape3D::set_camera);
	ClassDB::bind_method(D_METHOD("get_camera"), &Landscape3D::get_camera);
	ClassDB::bind_method(D_METHOD("set_stream_heights", "enable"), &Landscape3D::set_stream_heights);
	ClassDB::bind_method(D_METHOD("is_streaming_heights"), &Landscape3D::is_streaming_heights);

	ClassDB::bind_method(D_METHOD("set_collision_enabled", "enable"), &Landscape3D::set_collision_enabled);
	ClassDB::bind_method(D_METHOD("is_collision_enabled"), &Landscape3D::is_collision_enabled);
	ClassDB::bind_method(D_METHOD("set_collision_radius", "radius"), &Landscape3D::set_collision_radius);
	ClassDB::bind_method(D_METHOD("get_collision_radius"), &Landscape3D::get_collision_radius);
	ClassDB::bind_method(D_METHOD("set_collision_target", "path"), &Landscape3D::set_collision_target);
	ClassDB::bind_method(D_METHOD("get_collision_target"), &Landscape3D::get_collision_target);
	ClassDB::bind_method(D_METHOD("set_collision_layer", "layer"), &Landscape3D::set_collision_layer);
	ClassDB::bind_method(D_METHOD("get_collision_layer"), &Landscape3D::get_collision_layer);
	ClassDB::bind_method(D_METHOD("set_collision_mask", "mask"), &Landscape3D::set_collision_mask);
	ClassDB::bind_method(D_METHOD("get_collision_mask"), &Landscape3D::get_collision_mask);
	ClassDB::bind_method(D_METHOD("set_physics_material", "material"), &Landscape3D::set_physics_material);
	ClassDB::bind_method(D_METHOD("get_physics_material"), &Landscape3D::get_physics_material);
	ClassDB::bind_method(D_METHOD("update_collision"), &Landscape3D::update_collision);
	ClassDB::bind_method(D_METHOD("has_chunk_collision", "chunk"), &Landscape3D::has_chunk_collision);
	ClassDB::bind_method(D_METHOD("rebuild"), &Landscape3D::rebuild);
	ClassDB::bind_method(D_METHOD("set_auto_rebuild", "enable"), &Landscape3D::set_auto_rebuild);
	ClassDB::bind_method(D_METHOD("is_auto_rebuild"), &Landscape3D::is_auto_rebuild);
	ClassDB::bind_method(D_METHOD("get_rebuild_button"), &Landscape3D::get_rebuild_button);
	ClassDB::bind_method(D_METHOD("clear"), &Landscape3D::clear);
	ClassDB::bind_method(D_METHOD("update_lods", "camera_transform", "projection", "viewport_height"), &Landscape3D::update_lods);
	ClassDB::bind_method(D_METHOD("update_lods_from_camera"), &Landscape3D::update_lods_from_camera);
	ClassDB::bind_method(D_METHOD("load_all_heights"), &Landscape3D::load_all_heights);
	ClassDB::bind_method(D_METHOD("is_fully_loaded"), &Landscape3D::is_fully_loaded);
	ClassDB::bind_method(D_METHOD("is_chunk_loaded", "chunk"), &Landscape3D::is_chunk_loaded);
	ClassDB::bind_method(D_METHOD("get_chunk_grid"), &Landscape3D::get_chunk_grid);
	ClassDB::bind_method(D_METHOD("get_lod_count"), &Landscape3D::get_lod_count);
	ClassDB::bind_method(D_METHOD("get_hole_chunk_count"), &Landscape3D::get_hole_chunk_count);
	ClassDB::bind_method(D_METHOD("get_hole_quad_count"), &Landscape3D::get_hole_quad_count);
	ClassDB::bind_method(D_METHOD("get_chunk_lod", "chunk"), &Landscape3D::get_chunk_lod);
	ClassDB::bind_method(D_METHOD("get_chunk_edge_mask", "chunk"), &Landscape3D::get_chunk_edge_mask);
	ClassDB::bind_method(D_METHOD("get_chunk_lod_error", "chunk", "level"), &Landscape3D::get_chunk_lod_error);
	ClassDB::bind_method(D_METHOD("get_chunk_triangles", "chunk"), &Landscape3D::get_chunk_triangles);
	ClassDB::bind_method(D_METHOD("get_height", "global_position"), &Landscape3D::get_height);
	ClassDB::bind_method(D_METHOD("get_aabb"), &Landscape3D::get_aabb);
	ClassDB::bind_method(D_METHOD("get_stats"), &Landscape3D::get_stats);

	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "source", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "TerrainGenerator3D"), "set_source", "get_source");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "auto_rebuild"), "set_auto_rebuild", "is_auto_rebuild");
	ADD_PROPERTY(PropertyInfo(Variant::CALLABLE, "rebuild_button", PROPERTY_HINT_TOOL_BUTTON, "Rebuild,Reload", PROPERTY_USAGE_EDITOR), "", "get_rebuild_button");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "height_image", PROPERTY_HINT_RESOURCE_TYPE, "Image"), "set_height_image", "get_height_image");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "origin", PROPERTY_HINT_NONE, "suffix:m"), "set_origin", "get_origin");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "spacing", PROPERTY_HINT_RANGE, "0.05,16,0.05,or_greater,suffix:m"), "set_spacing", "get_spacing");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "chunk_quads", PROPERTY_HINT_ENUM, "8:8,16:16,32:32,64:64,128:128,256:256"), "set_chunk_quads", "get_chunk_quads");
	ADD_GROUP("LOD", "lod_");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "lod_levels", PROPERTY_HINT_RANGE, "1,8,1"), "set_lod_levels", "get_lod_levels");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lod_error", PROPERTY_HINT_RANGE, "0.05,16,0.05,suffix:px"), "set_lod_error", "get_lod_error");
	ADD_GROUP("", "");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "material", PROPERTY_HINT_RESOURCE_TYPE, "BaseMaterial3D,ShaderMaterial"), "set_material", "get_material");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cliff_relief", PROPERTY_HINT_RANGE, "0,2,0.05,or_greater,suffix:m"), "set_cliff_relief", "get_cliff_relief");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "bank_relief", PROPERTY_HINT_RANGE, "0,2,0.05,or_greater,suffix:m"), "set_bank_relief", "get_bank_relief");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "terracette_relief", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_terracette_relief", "get_terracette_relief");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "cast_shadows"), "set_cast_shadows", "is_casting_shadows");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "stream_heights"), "set_stream_heights", "is_streaming_heights");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "camera", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Camera3D"), "set_camera", "get_camera");
	ADD_GROUP("Collision", "collision_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "collision_enabled"), "set_collision_enabled", "is_collision_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "collision_radius", PROPERTY_HINT_RANGE, "0,2048,1,or_greater,suffix:m"), "set_collision_radius", "get_collision_radius");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "collision_target", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D"), "set_collision_target", "get_collision_target");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_layer", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_collision_layer", "get_collision_layer");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_mask", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_collision_mask", "get_collision_mask");
	ADD_GROUP("", "");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "physics_material", PROPERTY_HINT_RESOURCE_TYPE, "PhysicsMaterial"), "set_physics_material", "get_physics_material");
}

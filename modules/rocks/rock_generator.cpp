/**************************************************************************/
/*  rock_generator.cpp                                                    */
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

#include "rock_generator.h"

#include "core/math/random_pcg.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "scene/resources/3d/importer_mesh.h"
#include "scene/resources/surface_tool.h"

#ifndef ROCK_SIMPLIFY_ERROR
#define ROCK_SIMPLIFY_ERROR 0.03f
#endif

HashMap<int, Ref<ShaderMaterial>> RockGenerator::material_cache;
Ref<Shader> RockGenerator::shared_shader;

/* Rock types (docs/ideas/rocks.md, "Science") */

// Block proportions from joint spacing: granite and basalt break into near-equant blocks (three joint sets, sheeting),
// limestone and sandstone into slabs along their beds, slate into thin plates along its cleavage. Abrasion lengths
// (the river distance that takes a stone about two thirds of the way to round; the rounding phase is quick, the
// shrinking slow) from tumbling and field rounding rates: soft sandstone and limestone round within a few
// kilometres, granite and basalt over about ten (Kuenen 1956; Attal and Lavé 2009; Domokos et al. 2014).
// Colours are linear albedo of dry rock.
static const RockGenerator::Lithology lithologies[RockGenerator::ROCK_MAX] = {
	// name, a, c, jitter, columnar, cuts, abrasion m, spheroidal, pits, laminae, cleavage, grain,
	// base, speck, speck share, speck size, band, band contrast, rind, roughness, porosity
	{ "granite", 1.3f, 0.72f, 8.0f, false, 0.8f, 10000.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.7f,
			Color(0.30f, 0.28f, 0.26f), Color(0.04f, 0.04f, 0.04f), 0.18f, 0.004f, Color(0.42f, 0.34f, 0.30f), 0.15f, Color(0.24f, 0.21f, 0.16f), 0.82f, 0.25f },
	{ "limestone", 1.45f, 0.55f, 6.0f, false, 0.6f, 3000.0f, 0.2f, 1.0f, 0.3f, 0.0f, 0.4f,
			Color(0.37f, 0.35f, 0.31f), Color(0.30f, 0.29f, 0.26f), 0.05f, 0.01f, Color(0.38f, 0.35f, 0.30f), 0.35f, Color(0.30f, 0.30f, 0.28f), 0.78f, 0.55f },
	{ "sandstone", 1.55f, 0.5f, 5.0f, false, 0.5f, 2000.0f, 0.3f, 0.2f, 1.0f, 0.0f, 0.9f,
			Color(0.36f, 0.24f, 0.15f), Color(0.44f, 0.35f, 0.24f), 0.1f, 0.002f, Color(0.27f, 0.15f, 0.09f), 0.5f, Color(0.22f, 0.17f, 0.12f), 0.9f, 0.7f },
	{ "basalt", 1.25f, 0.85f, 4.0f, true, 0.6f, 8000.0f, 0.7f, 0.6f, 0.0f, 0.0f, 0.5f,
			Color(0.065f, 0.065f, 0.065f), Color(0.12f, 0.11f, 0.10f), 0.12f, 0.006f, Color(0.09f, 0.085f, 0.08f), 0.1f, Color(0.12f, 0.10f, 0.08f), 0.75f, 0.2f },
	{ "slate", 1.5f, 0.24f, 10.0f, false, 0.9f, 5000.0f, 0.0f, 0.0f, 0.2f, 1.0f, 0.3f,
			Color(0.085f, 0.09f, 0.10f), Color(0.14f, 0.14f, 0.15f), 0.08f, 0.003f, Color(0.11f, 0.12f, 0.13f), 0.25f, Color(0.15f, 0.13f, 0.11f), 0.6f, 0.15f },
};

const RockGenerator::Lithology &RockGenerator::get_lithology(RockType p_type) {
	return lithologies[CLAMP(int(p_type), 0, int(ROCK_MAX) - 1)];
}

String RockGenerator::get_rock_type_name(RockType p_type) {
	return get_lithology(p_type).name;
}

String RockGenerator::get_family_name(Family p_family) {
	static const char *names[FAMILY_MAX] = { "rockfall", "scree", "glacial", "cobble", "pebble", "cliff_block" };
	return names[CLAMP(int(p_family), 0, int(FAMILY_MAX) - 1)];
}

void RockGenerator::set_rock_type(RockType p_type) {
	rock_type = RockType(CLAMP(int(p_type), 0, int(ROCK_MAX) - 1));
}

void RockGenerator::set_family(Family p_family) {
	family = Family(CLAMP(int(p_family), 0, int(FAMILY_MAX) - 1));
}

int RockGenerator::get_effective_resolution() const {
	if (resolution > 0) {
		return resolution;
	}
	static const int res[FAMILY_MAX] = { 112, 88, 96, 56, 48, 112 };
	return res[family];
}

real_t RockGenerator::get_effective_transport_distance() const {
	if (transport_distance >= 0.0) {
		return transport_distance;
	}
	const real_t l = get_lithology(rock_type).abrasion_length;
	switch (family) {
		case FAMILY_ROCKFALL:
		case FAMILY_CLIFF_BLOCK:
			return 0.0;
		case FAMILY_SCREE:
			return 30.0; // the talus run: chipped, not rounded
		case FAMILY_GLACIAL:
			return l * 0.45; // subangular to subrounded: ice wears the bed face, carries the rest
		case FAMILY_COBBLE:
			return l * 1.2;
		case FAMILY_PEBBLE:
			return l * 3.0;
		default:
			return 0.0;
	}
}

real_t RockGenerator::get_effective_weathering() const {
	if (weathering >= 0.0) {
		return weathering;
	}
	static const float w[FAMILY_MAX] = { 0.45f, 0.2f, 0.6f, 0.15f, 0.05f, 0.7f };
	return w[family];
}

real_t RockGenerator::get_roundness() const {
	const real_t x = MAX(get_effective_transport_distance(), real_t(0.0));
	return 1.0 - Math::exp(-x / MAX(real_t(get_lithology(rock_type).abrasion_length), real_t(1.0)));
}

namespace {

inline uint32_t rk_hash(uint32_t p_x) {
	p_x ^= p_x >> 16;
	p_x *= 0x7FEB352Du;
	p_x ^= p_x >> 15;
	p_x *= 0x846CA68Bu;
	p_x ^= p_x >> 16;
	return p_x;
}

inline float rk_lattice(int p_x, int p_y, int p_z, uint32_t p_seed) {
	return float(rk_hash(uint32_t(p_x) * 73856093u ^ uint32_t(p_y) * 19349663u ^ uint32_t(p_z) * 83492791u ^ p_seed) >> 8) / 16777216.0f;
}

// Smooth value noise, 0..1.
float rk_noise(const Vector3 &p_p, uint32_t p_seed) {
	const int x = int(Math::floor(p_p.x)), y = int(Math::floor(p_p.y)), z = int(Math::floor(p_p.z));
	float fx = p_p.x - x, fy = p_p.y - y, fz = p_p.z - z;
	fx = fx * fx * (3.0f - 2.0f * fx);
	fy = fy * fy * (3.0f - 2.0f * fy);
	fz = fz * fz * (3.0f - 2.0f * fz);
	float c[2][2][2];
	for (int k = 0; k < 2; k++) {
		for (int j = 0; j < 2; j++) {
			for (int i = 0; i < 2; i++) {
				c[k][j][i] = rk_lattice(x + i, y + j, z + k, p_seed);
			}
		}
	}
	const float x00 = Math::lerp(c[0][0][0], c[0][0][1], fx), x10 = Math::lerp(c[0][1][0], c[0][1][1], fx);
	const float x01 = Math::lerp(c[1][0][0], c[1][0][1], fx), x11 = Math::lerp(c[1][1][0], c[1][1][1], fx);
	return Math::lerp(Math::lerp(x00, x10, fy), Math::lerp(x01, x11, fy), fz);
}

float rk_fbm(const Vector3 &p_p, uint32_t p_seed, int p_octaves) {
	float sum = 0.0f, amp = 0.5f, norm = 0.0f;
	Vector3 p = p_p;
	for (int o = 0; o < p_octaves; o++) {
		sum += amp * (rk_noise(p, p_seed + uint32_t(o) * 1013u) * 2.0f - 1.0f);
		norm += amp;
		amp *= 0.5f;
		p *= 2.03f;
	}
	return sum / norm;
}

// Distance to the nearest feature point of a jittered grid (Worley 1996), in cells.
float rk_worley(const Vector3 &p_p, uint32_t p_seed) {
	const int x = int(Math::floor(p_p.x)), y = int(Math::floor(p_p.y)), z = int(Math::floor(p_p.z));
	float best = 9.0f;
	for (int k = -1; k <= 1; k++) {
		for (int j = -1; j <= 1; j++) {
			for (int i = -1; i <= 1; i++) {
				const Vector3 f(x + i + rk_lattice(x + i, y + j, z + k, p_seed), y + j + rk_lattice(x + i, y + j, z + k, p_seed + 7u), z + k + rk_lattice(x + i, y + j, z + k, p_seed + 13u));
				best = MIN(best, (f - p_p).length_squared());
			}
		}
	}
	return Math::sqrt(best);
}

struct RockPlane {
	Vector3 n;
	float d = 0.0f;
	bool fresh = false;
};

struct RockScoop {
	Vector3 c;
	float r = 0.0f;
};

struct RockCrack {
	Vector3 n, t;
	float d = 0.0f, t0 = 0.0f, half_length = 0.0f, width = 0.0f, depth = 0.0f;
};

// A scalar field on a regular grid (signed distance, inside negative).
struct RockGrid {
	int nx = 0, ny = 0, nz = 0;
	float h = 1.0f;
	Vector3 origin;
	LocalVector<float> v;

	void init(const Vector3 &p_half, float p_h) {
		h = p_h;
		nx = int(Math::ceil(2.0f * p_half.x / h)) + 1;
		ny = int(Math::ceil(2.0f * p_half.y / h)) + 1;
		nz = int(Math::ceil(2.0f * p_half.z / h)) + 1;
		origin = -Vector3((nx - 1) * h, (ny - 1) * h, (nz - 1) * h) * 0.5f;
		v.resize(nx * ny * nz);
	}
	_FORCE_INLINE_ int idx(int p_x, int p_y, int p_z) const { return (p_z * ny + p_y) * nx + p_x; }
	_FORCE_INLINE_ Vector3 pos(int p_x, int p_y, int p_z) const { return origin + Vector3(p_x, p_y, p_z) * h; }
	float sample(const Vector3 &p_p) const {
		const Vector3 g = (p_p - origin) / h;
		const float gx = CLAMP(g.x, 0.0f, float(nx - 1) - 1e-3f), gy = CLAMP(g.y, 0.0f, float(ny - 1) - 1e-3f), gz = CLAMP(g.z, 0.0f, float(nz - 1) - 1e-3f);
		const int x = int(gx), y = int(gy), z = int(gz);
		const float fx = gx - x, fy = gy - y, fz = gz - z;
		const int x1 = MIN(x + 1, nx - 1), y1 = MIN(y + 1, ny - 1), z1 = MIN(z + 1, nz - 1);
		const float c00 = Math::lerp(v[idx(x, y, z)], v[idx(x1, y, z)], fx), c10 = Math::lerp(v[idx(x, y1, z)], v[idx(x1, y1, z)], fx);
		const float c01 = Math::lerp(v[idx(x, y, z1)], v[idx(x1, y, z1)], fx), c11 = Math::lerp(v[idx(x, y1, z1)], v[idx(x1, y1, z1)], fx);
		// Outside the grid the field keeps growing, so samples off the edge still read as outside.
		const float out = (Vector3(gx, gy, gz) - g).length() * h;
		return Math::lerp(Math::lerp(c00, c10, fy), Math::lerp(c01, c11, fy), fz) + out;
	}
	Vector3 gradient(const Vector3 &p_p) const {
		const float e = h * 0.5f;
		const Vector3 g(sample(p_p + Vector3(e, 0, 0)) - sample(p_p - Vector3(e, 0, 0)), sample(p_p + Vector3(0, e, 0)) - sample(p_p - Vector3(0, e, 0)), sample(p_p + Vector3(0, 0, e)) - sample(p_p - Vector3(0, 0, e)));
		const float l = g.length();
		return l > 1e-12f ? g / l : Vector3(0, 1, 0);
	}
	// Central-difference gradient at a node (unit when the field is a distance).
	_FORCE_INLINE_ Vector3 node_gradient(int p_x, int p_y, int p_z) const {
		const int xm = MAX(p_x - 1, 0), xp = MIN(p_x + 1, nx - 1), ym = MAX(p_y - 1, 0), yp = MIN(p_y + 1, ny - 1), zm = MAX(p_z - 1, 0), zp = MIN(p_z + 1, nz - 1);
		return Vector3((v[idx(xp, p_y, p_z)] - v[idx(xm, p_y, p_z)]) / (float(xp - xm) * h), (v[idx(p_x, yp, p_z)] - v[idx(p_x, ym, p_z)]) / (float(yp - ym) * h), (v[idx(p_x, p_y, zp)] - v[idx(p_x, p_y, zm)]) / (float(zp - zm) * h));
	}
};

// Keeps a level set a signed distance near its surface: Sussman, Smereka and Osher (1994), phi_t = S(phi0)(1 - |grad
// phi|), with Godunov upwinding (Rouy and Tourin 1992), only in the band where the surface is.
void rk_reinitialize(RockGrid &r_g, const LocalVector<int> &p_band, int p_iterations) {
	LocalVector<float> phi0;
	phi0.resize(p_band.size());
	for (uint32_t k = 0; k < p_band.size(); k++) {
		phi0[k] = r_g.v[p_band[k]];
	}
	const float h = r_g.h;
	const float dt = 0.4f * h;
	LocalVector<float> next;
	next.resize(p_band.size());
	for (int it = 0; it < p_iterations; it++) {
		for (uint32_t k = 0; k < p_band.size(); k++) {
			const int i = p_band[k];
			const int x = i % r_g.nx, y = (i / r_g.nx) % r_g.ny, z = i / (r_g.nx * r_g.ny);
			if (x == 0 || y == 0 || z == 0 || x == r_g.nx - 1 || y == r_g.ny - 1 || z == r_g.nz - 1) {
				next[k] = r_g.v[i];
				continue;
			}
			const float p = r_g.v[i];
			const float s = phi0[k] / Math::sqrt(phi0[k] * phi0[k] + h * h);
			float gsum = 0.0f;
			const int strides[3] = { 1, r_g.nx, r_g.nx * r_g.ny };
			for (int a = 0; a < 3; a++) {
				const float dm = (p - r_g.v[i - strides[a]]) / h;
				const float dp = (r_g.v[i + strides[a]] - p) / h;
				float g;
				if (s > 0.0f) {
					g = MAX(MAX(dm, 0.0f) * MAX(dm, 0.0f), MIN(dp, 0.0f) * MIN(dp, 0.0f));
				} else {
					g = MAX(MIN(dm, 0.0f) * MIN(dm, 0.0f), MAX(dp, 0.0f) * MAX(dp, 0.0f));
				}
				gsum += g;
			}
			next[k] = p - dt * s * (Math::sqrt(gsum) - 1.0f);
		}
		for (uint32_t k = 0; k < p_band.size(); k++) {
			r_g.v[p_band[k]] = next[k];
		}
	}
}

void rk_band(const RockGrid &p_g, float p_width, LocalVector<int> &r_band) {
	r_band.clear();
	for (uint32_t i = 0; i < p_g.v.size(); i++) {
		if (Math::abs(p_g.v[i]) < p_width) {
			r_band.push_back(int(i));
		}
	}
}

// Bloore's (1977) abrasion: the surface moves in at a speed 1 + c kappa (kappa the mean curvature, largest at corners
// and edges, so they go first), here as phi_t = w(n) (a + b kappa) |grad phi| with kappa from the Laplacian of the
// distance field. b carries the rounding phase, a the slow shrinking that follows it (Domokos et al. 2014). w(n)
// favours one direction: beach pebbles slide on their broad faces in the swash and wear them flatter still.
void rk_abrade(RockGrid &r_g, float p_time, float p_uniform, float p_flatten) {
	if (p_time <= 0.0f) {
		return;
	}
	const float h = r_g.h;
	const float wmax = 1.0f + p_flatten;
	const float dt = 0.15f * h * h / wmax;
	const int steps = MIN(int(Math::ceil(p_time / dt)), 900);
	const float step_dt = p_time / MAX(steps, 1);
	const float a = p_uniform / p_time; // retreat over the whole time
	LocalVector<int> band;
	rk_band(r_g, 4.0f * h, band);
	LocalVector<float> next;
	const int sy = r_g.nx, sz = r_g.nx * r_g.ny;
	for (int s = 0; s < steps; s++) {
		if (s % 24 == 23) {
			rk_band(r_g, 4.0f * h, band);
		}
		next.resize(band.size());
		for (uint32_t k = 0; k < band.size(); k++) {
			const int i = band[k];
			const int x = i % r_g.nx, y = (i / r_g.nx) % r_g.ny, z = i / sz;
			if (x == 0 || y == 0 || z == 0 || x == r_g.nx - 1 || y == r_g.ny - 1 || z == r_g.nz - 1) {
				next[k] = r_g.v[i];
				continue;
			}
			const float p = r_g.v[i];
			const float lap = (r_g.v[i + 1] + r_g.v[i - 1] + r_g.v[i + sy] + r_g.v[i - sy] + r_g.v[i + sz] + r_g.v[i - sz] - 6.0f * p) / (h * h);
			float w = 1.0f;
			if (p_flatten > 0.0f) {
				const float gy = (r_g.v[i + sy] - r_g.v[i - sy]) / (2.0f * h);
				w += p_flatten * gy * gy;
			}
			next[k] = p + step_dt * w * (a + CLAMP(lap, -3.0f / h, 3.0f / h));
		}
		for (uint32_t k = 0; k < band.size(); k++) {
			r_g.v[band[k]] = next[k];
		}
		if (s % 6 == 5) {
			rk_reinitialize(r_g, band, 2);
		}
	}
	rk_band(r_g, 4.0f * h, band);
	rk_reinitialize(r_g, band, 4);
}

// The coarse grid's field on a finer grid of the same box, trilinear.
void rk_resample(const RockGrid &p_src, RockGrid &r_dst) {
	for (int z = 0; z < r_dst.nz; z++) {
		for (int y = 0; y < r_dst.ny; y++) {
			for (int x = 0; x < r_dst.nx; x++) {
				r_dst.v[r_dst.idx(x, y, z)] = p_src.sample(r_dst.pos(x, y, z));
			}
		}
	}
}

} // namespace

/* Generation */

Ref<ImporterMesh> RockGenerator::build_importer_mesh(Dictionary &r_meta) {
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	const Lithology &lith = get_lithology(rock_type);
	RandomPCG rng(uint64_t(uint32_t(seed)) * 2654435761ull + uint64_t(rock_type) * 97ull + uint64_t(family) * 7919ull + 1ull);
	const uint32_t nseed = rk_hash(uint32_t(seed) * 31u + uint32_t(rock_type) * 1009u + uint32_t(family) * 7u);
	const float roundness = float(get_roundness());
	const float weather = float(get_effective_weathering());

	// 1. Fracture: the block between the joint planes (x the long axis a, y the short axis c, z the intermediate b = 1;
	// y is the bedding or cleavage normal of layered rock). Plato's cube: most natural fragments are near-cuboid, so
	// six joint planes, wandering off square by the rock's joint jitter, then a few secondary fractures.
	float a = lith.a_ratio * (0.85f + 0.3f * rng.randf());
	float c = lith.c_ratio * (0.8f + 0.4f * rng.randf());
	if (family == FAMILY_CLIFF_BLOCK) {
		a *= 1.15f; // blocks in a wall run along the beds
	}
	if (family == FAMILY_PEBBLE) {
		c *= 0.8f; // shingle starts platy as well as wearing so
	}
	a = MAX(a, 1.0f);
	c = MIN(c, 1.0f);
	LocalVector<RockPlane> planes;
	auto jitter_dir = [&](const Vector3 &p_axis, float p_degrees) {
		const Vector3 r(rng.randf() * 2.0f - 1.0f, rng.randf() * 2.0f - 1.0f, rng.randf() * 2.0f - 1.0f);
		return (p_axis + r * Math::tan(Math::deg_to_rad(p_degrees))).normalized();
	};
	if (lith.columnar && family != FAMILY_PEBBLE) {
		// Columnar basalt: a 5 to 7-sided prism (contraction joints, mean six; Goehring 2009) along x, cut across.
		const int sides = CLAMP(int(Math::round(6.0f + rng.randfn(0.0f, 0.8f))), 5, 7);
		const float phase = rng.randf() * float(Math::TAU);
		for (int k = 0; k < sides; k++) {
			const float th = phase + float(Math::TAU) * (k + (rng.randf() - 0.5f) * 0.25f) / sides;
			Vector3 n(0.0f, Math::cos(th) / c, Math::sin(th));
			const float l = n.length();
			planes.push_back({ n / l, (0.92f + 0.12f * rng.randf()) / l, false });
		}
		planes.push_back({ jitter_dir(Vector3(1, 0, 0), 5.0f), a * (0.9f + 0.2f * rng.randf()), false });
		planes.push_back({ jitter_dir(Vector3(-1, 0, 0), 5.0f), a * (0.9f + 0.2f * rng.randf()), false });
	} else {
		const Vector3 half(a, c, 1.0f);
		for (int axis = 0; axis < 3; axis++) {
			for (int sgn = -1; sgn <= 1; sgn += 2) {
				Vector3 n;
				n[axis] = float(sgn);
				// Bedding (y) stays flatter than the cross joints.
				const float j = axis == 1 ? lith.joint_jitter * 0.4f : lith.joint_jitter;
				planes.push_back({ jitter_dir(n, j), half[axis] * (0.88f + 0.24f * rng.randf()), false });
			}
		}
	}
	// Secondary fractures: corner and edge cuts, fresh faces where the block broke when it fell or was struck.
	const int cuts = int(lith.extra_cuts * (family == FAMILY_ROCKFALL || family == FAMILY_SCREE ? 2.0f : 1.0f) * (0.5f + rng.randf()) + 0.5f);
	for (int k = 0; k < cuts; k++) {
		const Vector3 corner(a * (rng.randf() < 0.5f ? -1.0f : 1.0f), c * (rng.randf() < 0.5f ? -1.0f : 1.0f), rng.randf() < 0.5f ? -1.0f : 1.0f);
		const Vector3 n = jitter_dir(corner.normalized(), 25.0f);
		planes.push_back({ n, n.dot(corner) * (0.6f + 0.25f * rng.randf()), true });
	}
	// Fresh fragments are not trimmed boxes: a block that fell broke along new fractures right through it, at any
	// angle to the joints (Domokos et al. 2020: fragments average about six faces and eight vertices, but each one
	// is an irregular polyhedron). Two to four planes, each taking 15 to 45% off one side.
	if (family == FAMILY_ROCKFALL || family == FAMILY_SCREE || family == FAMILY_CLIFF_BLOCK) {
		const int through = (family == FAMILY_CLIFF_BLOCK ? 1 : 2) + int(rng.randf() * 2.99f);
		for (int k = 0; k < through; k++) {
			Vector3 n(rng.randfn(0.0f, 1.0f), rng.randfn(0.0f, 1.0f) * 0.6f, rng.randfn(0.0f, 1.0f));
			n = n.length_squared() > 1e-6f ? n.normalized() : Vector3(1, 0, 0);
			// Support of the block along n, then cut back from it.
			const float support = Math::abs(n.x) * a + Math::abs(n.y) * c + Math::abs(n.z);
			planes.push_back({ n, support * (0.55f + 0.3f * rng.randf()), true });
		}
	}
	// Impact chips: conchoidal scoops off corners and edges (bed-load chipping, Novák-Szabó et al. 2018), fresh.
	static const int chips_per_family[FAMILY_MAX] = { 3, 5, 3, 3, 1, 2 };
	LocalVector<RockScoop> scoops;
	const int chips = chips_per_family[family] + int(rng.randf() * 2.0f);
	for (int k = 0; k < chips; k++) {
		Vector3 sp(a * (rng.randf() < 0.5f ? -1.0f : 1.0f), c * (rng.randf() < 0.5f ? -1.0f : 1.0f), rng.randf() < 0.5f ? -1.0f : 1.0f);
		sp[rng.rand() % 3] *= rng.randf() * 2.0f - 1.0f; // an edge as often as a corner
		const Vector3 out = jitter_dir(sp.normalized(), 20.0f);
		const float r = (0.3f + 0.5f * rng.randf()) * c;
		const float depth = (0.06f + 0.1f * rng.randf()) * c;
		scoops.push_back({ sp * 0.9f + out * (r - depth), r });
	}
	// Partial cracks along the joint directions, open only near the surface (fresh rockfall and cliff blocks).
	LocalVector<RockCrack> cracks;
	const int crack_count = (family == FAMILY_ROCKFALL || family == FAMILY_CLIFF_BLOCK || family == FAMILY_SCREE) ? int(rng.randf() * 2.6f) : (family == FAMILY_GLACIAL ? int(rng.randf() * 1.5f) : 0);

	// 2. The field. A coarse grid carries the abrasion (the flow is smooth and needs many small steps); the fine grid
	// carries the detail.
	const int res = get_effective_resolution();
	const Vector3 half_box = Vector3(a, c, 1.0f) * 1.12f + Vector3(1, 1, 1) * 0.08f;
	const float h = 2.0f * half_box.x / float(res - 1);
	for (int k = 0; k < crack_count; k++) {
		RockCrack cr;
		const int axis = rng.rand() % 3;
		Vector3 n;
		n[axis] = 1.0f;
		cr.n = jitter_dir(n, lith.joint_jitter);
		cr.t = cr.n.cross(jitter_dir(Vector3(0, 1, 0).cross(cr.n).length_squared() > 0.1f ? Vector3(0, 1, 0) : Vector3(1, 0, 0), 20.0f)).normalized();
		const float extent = axis == 0 ? a : (axis == 1 ? c : 1.0f);
		cr.d = (rng.randf() * 1.2f - 0.6f) * extent;
		cr.t0 = (rng.randf() - 0.5f) * 0.8f;
		cr.half_length = 0.35f + 0.5f * rng.randf();
		cr.width = MAX(1.6f * h, 0.012f) * (1.0f + 0.5f * rng.randf());
		cr.depth = MIN((0.12f + 0.2f * rng.randf()) * c, 0.3f * c);
		cracks.push_back(cr);
	}
	auto shape = [&](const Vector3 &p, bool *r_fresh) {
		float phi = -1e9f;
		bool fresh = false;
		for (const RockPlane &pl : planes) {
			const float d = pl.n.dot(p) - pl.d;
			if (d > phi) {
				phi = d;
				fresh = pl.fresh;
			}
		}
		for (const RockScoop &s : scoops) {
			const float d = s.r - (p - s.c).length();
			if (d > phi) {
				phi = d;
				fresh = true;
			}
		}
		if (r_fresh) {
			*r_fresh = fresh;
		}
		return phi;
	};
	// Rounding time: corners round to a radius growing as the square root of the curvature-flow time, so the time
	// for a corner radius r is about r^2 / 2. A fully rounded stone has corners as round as half its thickness.
	// Spheroidal weathering of granite and basalt rounds blocks in place along the joints in the same way (corners
	// attacked from three sides), so it adds to the rounding of blocks left lying (tors, corestones).
	static const float spheroidal_share[FAMILY_MAX] = { 0.05f, 0.0f, 0.5f, 0.15f, 0.15f, 0.25f };
	const float spheroidal = lith.spheroidal * weather * spheroidal_share[family];
	const float round_eff = MIN(1.0f, roundness + spheroidal * (1.0f - roundness));
	// Corners round to at most 0.7 of the half thickness: a curvature flow run longer would shrink a thin stone to
	// nothing (a sphere of radius r vanishes at t = r^2 / 4).
	const float corner = round_eff * 0.7f * c;
	float flow_time = 0.5f * corner * corner;
	const float shrink = roundness > 0.6f ? (roundness - 0.6f) * 0.25f * c : 0.0f;
	const float flatten = family == FAMILY_PEBBLE ? 1.0f : 0.0f;

	RockGrid fine;
	fine.init(half_box, h);
	if (flow_time > 0.0f) {
		RockGrid coarse;
		coarse.init(half_box, h * 2.0f);
		for (int z = 0; z < coarse.nz; z++) {
			for (int y = 0; y < coarse.ny; y++) {
				for (int x = 0; x < coarse.nx; x++) {
					coarse.v[coarse.idx(x, y, z)] = shape(coarse.pos(x, y, z), nullptr);
				}
			}
		}
		LocalVector<int> band;
		rk_band(coarse, 4.0f * coarse.h, band);
		rk_reinitialize(coarse, band, 6);
		LocalVector<float> start;
		start.resize(coarse.v.size());
		memcpy(start.ptr(), coarse.v.ptr(), sizeof(float) * start.size());
		// A stone must keep a body: if the flow left less than two cells of it, run it again for half the time.
		for (int attempt = 0; attempt < 4; attempt++) {
			rk_abrade(coarse, flow_time, shrink * (attempt == 0 ? 1.0f : 0.0f), flatten);
			float deepest = 0.0f;
			for (float v : coarse.v) {
				deepest = MIN(deepest, v);
			}
			if (deepest < -2.0f * coarse.h) {
				break;
			}
			memcpy(coarse.v.ptr(), start.ptr(), sizeof(float) * start.size());
			flow_time *= 0.5f;
		}
		rk_resample(coarse, fine);
	} else {
		for (int z = 0; z < fine.nz; z++) {
			for (int y = 0; y < fine.ny; y++) {
				for (int x = 0; x < fine.nx; x++) {
					fine.v[fine.idx(x, y, z)] = shape(fine.pos(x, y, z), nullptr);
				}
			}
		}
	}
	// Glacial facets: a boulder dragged in the base of the ice is ground flat on the face against the bed (soled
	// and faceted clasts, Benn and Evans 2010), so a rounded block keeps one or two planar facets.
	if (family == FAMILY_GLACIAL) {
		const int facets = 1 + int(rng.randf() * 1.99f);
		for (int k = 0; k < facets; k++) {
			Vector3 n = k == 0 ? jitter_dir(Vector3(0, -1, 0), 12.0f) : jitter_dir(Vector3(rng.randfn(0.0f, 1.0f), rng.randfn(0.0f, 0.4f), rng.randfn(0.0f, 1.0f)).normalized(), 5.0f);
			float support = -1e9f;
			for (int z = 0; z < fine.nz; z++) {
				for (int y = 0; y < fine.ny; y++) {
					for (int x = 0; x < fine.nx; x++) {
						if (fine.v[fine.idx(x, y, z)] < 0.0f) {
							support = MAX(support, n.dot(fine.pos(x, y, z)));
						}
					}
				}
			}
			const float d = support - (0.1f + 0.1f * rng.randf()) * c;
			for (int z = 0; z < fine.nz; z++) {
				for (int y = 0; y < fine.ny; y++) {
					for (int x = 0; x < fine.nx; x++) {
						float &v = fine.v[fine.idx(x, y, z)];
						v = MAX(v, n.dot(fine.pos(x, y, z)) - d);
					}
				}
			}
		}
	}
	// Where the surface is a fresh break (lighter, no rind, no lichen): from the fracture, worn away by the transport.
	RockGrid fresh_grid;
	fresh_grid.init(half_box, h * 2.0f);
	for (int z = 0; z < fresh_grid.nz; z++) {
		for (int y = 0; y < fresh_grid.ny; y++) {
			for (int x = 0; x < fresh_grid.nx; x++) {
				bool fr = false;
				shape(fresh_grid.pos(x, y, z), &fr);
				fresh_grid.v[fresh_grid.idx(x, y, z)] = fr ? (1.0f - roundness) : 0.0f;
			}
		}
	}

	// 3. Weathering and the rock's own marks, carved into the fine field near its surface.
	LocalVector<int> band;
	rk_band(fine, 5.0f * h, band);
	const float worn = 1.0f - 0.75f * roundness; // transport wears the marks away
	const bool fresh_family = family == FAMILY_ROCKFALL || family == FAMILY_SCREE || family == FAMILY_CLIFF_BLOCK;
	// Periodic marks finer than about five grid cells alias into combs and notches: no period below that.
	const float min_period = 5.0f * h;
	const Vector3 striae_dir = Vector3(Math::cos(rng.randf() * float(Math::TAU)), 0.0f, Math::sin(rng.randf() * float(Math::TAU))).normalized();
	const Vector3 striae_perp(-striae_dir.z, 0.0f, striae_dir.x);
	const float bed_tilt = (rng.randf() - 0.5f) * 0.35f;
	for (uint32_t k = 0; k < band.size(); k++) {
		const int i = band[k];
		const int x = i % fine.nx, y = (i / fine.nx) % fine.ny, z = i / (fine.nx * fine.ny);
		const Vector3 p = fine.pos(x, y, z);
		Vector3 n = fine.node_gradient(x, y, z);
		const float nl = n.length();
		n = nl > 1e-6f ? n / nl : Vector3(0, 1, 0);
		float phi = fine.v[i];
		// Cracks: a slab along a joint, open only near the surface and over part of the face.
		for (const RockCrack &cr : cracks) {
			// A real crack wanders and closes towards its tips: the plane wobbles, the gap tapers to nothing at the
			// ends (a machined slot with square ends is what this replaced).
			const float u = (cr.t.dot(p) - cr.t0) / cr.half_length;
			const float open = Math::sqrt(MAX(0.0f, 1.0f - u * u));
			const float wobble = 1.5f * h * rk_fbm(p * 7.0f, nseed + 91u, 2);
			const float slab = cr.width * 0.5f * open - Math::abs(cr.n.dot(p) - cr.d - wobble);
			const float along = cr.half_length - Math::abs(cr.t.dot(p) - cr.t0);
			const float removed = MIN(MIN(slab, along), phi + cr.depth * open);
			phi = MAX(phi, removed);
		}
		// Grain-scale roughness (grus on granite, sand grains on sandstone).
		phi += lith.grain * float(detail) * 0.45f * h * rk_fbm(p / (2.2f * h), nseed, 2) * worn;
		// Lumps a few voxels wide: no rock face is a plane.
		phi += float(detail) * (fresh_family ? 0.45f : 1.1f) * h * rk_fbm(p * 2.6f, nseed + 5u, 3) * (0.5f + 0.5f * worn);
		// Solution pits on the upper faces (karren on limestone; Ford and Williams 2007) or gas vesicles (basalt).
		if (lith.pits > 0.0f) {
			const bool karst = !lith.columnar;
			const float cell = karst ? 0.16f : 0.07f;
			const float wd = rk_worley(p / cell, nseed + 21u);
			const float pit = MAX(0.0f, 0.55f - wd) / 0.55f;
			const float mask = karst ? Math::smoothstep(0.2f, 0.7f, n.y) * weather : 0.6f;
			phi += lith.pits * float(detail) * mask * pit * pit * MAX(1.6f * h, cell * 0.22f) * (karst ? worn : 1.0f);
		}
		// Bedding laminae etched out by weathering (differential erosion of the softer laminae), slightly cross-bedded.
		if (lith.laminae > 0.0f) {
			const float lam = MAX(0.07f, min_period);
			const float s = (p.y + bed_tilt * p.x + 0.02f * rk_fbm(p * 3.0f, nseed + 31u, 2)) / lam;
			const float groove = Math::pow(0.5f + 0.5f * Math::cos(s * float(Math::TAU)), 6.0f);
			// Laminae differ: most barely etched, a few soft ones weathered back deep.
			const float lamina = rk_hash(uint32_t(int(Math::floor(s + 0.5f)) + 4096) ^ nseed) / 4294967295.0f;
			const float strength = 0.25f + 0.75f * lamina * lamina;
			phi += lith.laminae * float(detail) * (0.3f + 0.7f * weather) * groove * strength * 1.1f * h * (1.0f - Math::abs(n.y)) * worn;
		}
		// Slaty cleavage: plates stepping back at the edges.
		if (lith.cleavage > 0.0f) {
			const float lam = MAX(0.05f, min_period);
			const float s = (p.y + 0.01f * rk_fbm(p * 4.0f, nseed + 41u, 2)) / lam;
			const float f = s - Math::floor(s);
			phi += lith.cleavage * float(detail) * f * 1.4f * h * (1.0f - Math::abs(n.y)) * worn;
		}
		// Tafoni: honeycomb hollows in the sides of weathered sandstone and granite blocks.
		if ((rock_type == ROCK_SANDSTONE || rock_type == ROCK_GRANITE) && weather > 0.4f && family != FAMILY_PEBBLE && family != FAMILY_COBBLE) {
			const float wd = rk_worley(p / 0.3f, nseed + 51u);
			const float hollow = MAX(0.0f, 0.5f - wd) / 0.5f;
			phi += (weather - 0.4f) * float(detail) * hollow * hollow * 0.09f * (1.0f - Math::abs(n.y)) * (rock_type == ROCK_SANDSTONE ? 1.0f : 0.4f);
		}
		// Glacial striations: fine grooves on the faces the ice dragged over the bed, all one way.
		if (family == FAMILY_GLACIAL) {
			const float s = (striae_perp.dot(p) + 0.01f * rk_fbm(p * 5.0f, nseed + 61u, 2)) / MAX(2.6f * h, 0.6f * min_period);
			const float g = Math::pow(0.5f + 0.5f * Math::cos(s * float(Math::TAU)), 10.0f) * Math::smoothstep(0.55f, 0.9f, Math::abs(n.y));
			phi += float(detail) * g * 0.9f * h * (0.5f + 0.5f * rk_noise(p * 6.0f, nseed + 71u));
		}
		fine.v[i] = phi;
	}
	rk_reinitialize(fine, band, 3);

	// 4. Mesh: surface nets (Gibson 1998): one vertex per cell the surface crosses, at the mean of its edge
	// crossings; one quad per crossed grid edge.
	const int cx = fine.nx - 1, cy = fine.ny - 1, cz = fine.nz - 1;
	LocalVector<int> cell_vertex;
	cell_vertex.resize(cx * cy * cz);
	LocalVector<Vector3> verts;
	static const int corner_offsets[8][3] = { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 }, { 1, 1, 0 }, { 0, 0, 1 }, { 1, 0, 1 }, { 0, 1, 1 }, { 1, 1, 1 } };
	static const int edges[12][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
	for (int z = 0; z < cz; z++) {
		for (int y = 0; y < cy; y++) {
			for (int x = 0; x < cx; x++) {
				float vals[8];
				int mask = 0;
				for (int k = 0; k < 8; k++) {
					vals[k] = fine.v[fine.idx(x + corner_offsets[k][0], y + corner_offsets[k][1], z + corner_offsets[k][2])];
					if (vals[k] < 0.0f) {
						mask |= 1 << k;
					}
				}
				const int ci = (z * cy + y) * cx + x;
				if (mask == 0 || mask == 255) {
					cell_vertex[ci] = -1;
					continue;
				}
				Vector3 sum;
				int cnt = 0;
				for (int e = 0; e < 12; e++) {
					const float v0 = vals[edges[e][0]], v1 = vals[edges[e][1]];
					if ((v0 < 0.0f) == (v1 < 0.0f)) {
						continue;
					}
					const float t = v0 / (v0 - v1);
					const Vector3 p0(corner_offsets[edges[e][0]][0], corner_offsets[edges[e][0]][1], corner_offsets[edges[e][0]][2]);
					const Vector3 p1(corner_offsets[edges[e][1]][0], corner_offsets[edges[e][1]][1], corner_offsets[edges[e][1]][2]);
					sum += p0.lerp(p1, t);
					cnt++;
				}
				cell_vertex[ci] = int(verts.size());
				verts.push_back(fine.pos(x, y, z) + sum / float(cnt) * fine.h);
			}
		}
	}
	LocalVector<int> indices;
	auto quad = [&](int p_a, int p_b, int p_c, int p_d, bool p_flip) {
		if (p_a < 0 || p_b < 0 || p_c < 0 || p_d < 0) {
			return;
		}
		if (p_flip) {
			SWAP(p_b, p_d);
		}
		indices.push_back(p_a);
		indices.push_back(p_b);
		indices.push_back(p_c);
		indices.push_back(p_a);
		indices.push_back(p_c);
		indices.push_back(p_d);
	};
	auto cv = [&](int p_x, int p_y, int p_z) { return (p_x < 0 || p_y < 0 || p_z < 0 || p_x >= cx || p_y >= cy || p_z >= cz) ? -1 : cell_vertex[(p_z * cy + p_y) * cx + p_x]; };
	for (int z = 1; z < fine.nz - 1; z++) {
		for (int y = 1; y < fine.ny - 1; y++) {
			for (int x = 1; x < fine.nx - 1; x++) {
				const bool in0 = fine.v[fine.idx(x, y, z)] < 0.0f;
				if (x < fine.nx - 1 && in0 != (fine.v[fine.idx(x + 1, y, z)] < 0.0f)) {
					quad(cv(x, y - 1, z - 1), cv(x, y, z - 1), cv(x, y, z), cv(x, y - 1, z), !in0);
				}
				if (y < fine.ny - 1 && in0 != (fine.v[fine.idx(x, y + 1, z)] < 0.0f)) {
					quad(cv(x - 1, y, z - 1), cv(x - 1, y, z), cv(x, y, z), cv(x, y, z - 1), !in0);
				}
				if (z < fine.nz - 1 && in0 != (fine.v[fine.idx(x, y, z + 1)] < 0.0f)) {
					quad(cv(x - 1, y - 1, z), cv(x, y - 1, z), cv(x, y, z), cv(x - 1, y, z), !in0);
				}
			}
		}
	}
	// Winding: Godot's front faces are clockwise seen from outside; check the first triangle against the field's
	// outward gradient and flip all if needed (the quads above are wound consistently).
	if (indices.size() >= 3) {
		int votes = 0;
		for (uint32_t t = 0; t < MIN(indices.size(), 3000u); t += 3) {
			const Vector3 &v0 = verts[indices[t]], &v1 = verts[indices[t + 1]], &v2 = verts[indices[t + 2]];
			const Vector3 fn = (v1 - v0).cross(v2 - v0);
			votes += fn.dot(fine.gradient((v0 + v1 + v2) / 3.0f)) < 0.0f ? 1 : -1;
		}
		if (votes < 0) {
			for (uint32_t t = 0; t < indices.size(); t += 3) {
				SWAP(indices[t + 1], indices[t + 2]);
			}
		}
	}
	ERR_FAIL_COND_V_MSG(verts.size() < 4 || indices.size() < 12, Ref<ImporterMesh>(), "RockGenerator: the rock came out empty.");

	// 5. Normals (area-weighted from the faces), cavity (how much the field closes in above a point: open faces 1,
	// cracks and pits darker), freshness.
	LocalVector<Vector3> normals;
	normals.resize(verts.size());
	for (uint32_t t = 0; t < indices.size(); t += 3) {
		const Vector3 &v0 = verts[indices[t]], &v1 = verts[indices[t + 1]], &v2 = verts[indices[t + 2]];
		const Vector3 fn = (v2 - v0).cross(v1 - v0); // clockwise front: outward
		normals[indices[t]] += fn;
		normals[indices[t + 1]] += fn;
		normals[indices[t + 2]] += fn;
	}
	// LOD0 without the flat-face excess: surface nets put a vertex in every grid cell, so a flat break carries as
	// many triangles as a pitted one. Collapse edges only where neither the surface nor its normal moves visibly
	// (meshoptimizer with normals as attributes, absolute error of 3% of a grid cell); the close-ups at 100% crop
	// match the full mesh. Normals keep the full mesh's smoothing, taken above before the collapse.
	if (simplify && indices.size() > 3000) {
		LocalVector<uint32_t> simplified;
		simplified.resize(indices.size());
		// Normals as attributes, so a shallow groove or pit that turns the surface but barely moves it still holds
		// its vertices: a normal turned by one radian costs as much as moving a cell.
		LocalVector<float> attrib;
		attrib.resize(verts.size() * 3);
		for (uint32_t k = 0; k < verts.size(); k++) {
			const Vector3 nn = normals[k].length_squared() > 1e-20f ? normals[k].normalized() : Vector3(0, 1, 0);
			attrib[k * 3 + 0] = nn.x;
			attrib[k * 3 + 1] = nn.y;
			attrib[k * 3 + 2] = nn.z;
		}
		const float w = 1.0f * fine.h;
		const float weights[3] = { w, w, w };
		float err = 0.0f;
		const size_t n = SurfaceTool::simplify_with_attrib_func ? SurfaceTool::simplify_with_attrib_func(simplified.ptr(), (const unsigned int *)indices.ptr(), indices.size(), (const float *)verts.ptr(), verts.size(), sizeof(Vector3), attrib.ptr(), sizeof(float) * 3, weights, 3, nullptr, 0, ROCK_SIMPLIFY_ERROR * fine.h, SurfaceTool::SIMPLIFY_ERROR_ABSOLUTE, &err) : 0;
		if (n >= 12) {
			// Keep only the vertices the triangles still use, in first-use order.
			LocalVector<int> remap;
			remap.resize(verts.size());
			for (int &r : remap) {
				r = -1;
			}
			LocalVector<Vector3> kv, kn;
			LocalVector<int> ki;
			ki.resize(n);
			for (size_t k = 0; k < n; k++) {
				const uint32_t v = simplified[k];
				if (remap[v] < 0) {
					remap[v] = int(kv.size());
					kv.push_back(verts[v]);
					kn.push_back(normals[v]);
				}
				ki[k] = remap[v];
			}
			verts = kv;
			normals = kn;
			indices = ki;
		}
	}
	// Normalise the rock: b axis (z) 2 units long, centred.
	AABB box(verts[0], Vector3());
	for (const Vector3 &v : verts) {
		box.expand_to(v);
	}
	const Vector3 centre = box.get_center();
	// Rest pose: the longest axis on x, the shortest on y (the stone lies on its flattest side), b on z.
	int ord[3] = { 0, 1, 2 };
	for (int i = 0; i < 3; i++) {
		for (int j = i + 1; j < 3; j++) {
			if (box.size[ord[j]] > box.size[ord[i]]) {
				SWAP(ord[i], ord[j]);
			}
		}
	}
	const int ax_long = ord[0], ax_b = ord[1], ax_short = ord[2];
	auto permute = [&](const Vector3 &p_v) { return Vector3(p_v[ax_long], p_v[ax_short], p_v[ax_b]); };
	// An odd permutation mirrors the stone: flip z then, so the winding stays outward.
	const bool odd = !((ax_long == 0 && ax_short == 1) || (ax_long == 1 && ax_short == 2) || (ax_long == 2 && ax_short == 0));
	const float scale = 2.0f / MAX(box.size[ax_b], 1e-4f);
	PackedVector3Array out_verts, out_normals;
	PackedColorArray out_colors;
	PackedVector2Array out_uv, out_uv2;
	PackedFloat32Array out_tangents;
	out_verts.resize(verts.size());
	out_normals.resize(verts.size());
	out_colors.resize(verts.size());
	out_uv.resize(verts.size());
	out_uv2.resize(verts.size());
	out_tangents.resize(verts.size() * 4);
	for (uint32_t k = 0; k < verts.size(); k++) {
		const Vector3 p = verts[k];
		Vector3 n = normals[k];
		n = n.length_squared() > 1e-20f ? n.normalized() : fine.gradient(p);
		float occ = 0.0f;
		for (int s = 1; s <= 3; s++) {
			const float dist = fine.h * float(1 << s);
			occ += MAX(0.0f, dist - fine.sample(p + n * dist)) / dist / float(1 << (s - 1));
		}
		const float cavity = CLAMP(1.0f - occ * 0.9f, 0.0f, 1.0f);
		const float fr = CLAMP(fresh_grid.sample(p), 0.0f, 1.0f);
		Vector3 q = permute((p - centre) * scale);
		n = permute(n);
		if (odd) {
			q.z = -q.z;
			n.z = -n.z;
		}
		out_verts.write[k] = q;
		out_normals.write[k] = n;
		out_colors.write[k] = Color(cavity, fr, 0.0f, 1.0f);
		out_uv.write[k] = Vector2(q.x, q.z);
		out_uv2.write[k] = Vector2(q.y, 0.0f);
		const Vector3 t = (Math::abs(n.y) < 0.95f ? Vector3(0, 1, 0) : Vector3(1, 0, 0)).cross(n).normalized();
		out_tangents.write[k * 4 + 0] = t.x;
		out_tangents.write[k * 4 + 1] = t.y;
		out_tangents.write[k * 4 + 2] = t.z;
		out_tangents.write[k * 4 + 3] = 1.0f;
	}
	PackedInt32Array out_indices;
	out_indices.resize(indices.size());
	for (uint32_t k = 0; k < indices.size(); k++) {
		out_indices.write[k] = indices[k];
	}
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = out_verts;
	arrays[Mesh::ARRAY_NORMAL] = out_normals;
	arrays[Mesh::ARRAY_TANGENT] = out_tangents;
	arrays[Mesh::ARRAY_COLOR] = out_colors;
	arrays[Mesh::ARRAY_TEX_UV] = out_uv;
	arrays[Mesh::ARRAY_TEX_UV2] = out_uv2;
	arrays[Mesh::ARRAY_INDEX] = out_indices;

	Ref<ImporterMesh> im;
	im.instantiate();
	im->add_surface(Mesh::PRIMITIVE_TRIANGLES, arrays, TypedArray<Array>(), Dictionary(), get_material(rock_type), "rock");
	if (lods) {
		im->generate_lods(25.0f, Array());
	}

	// Collision hull points: farthest-point sampling of the surface, 48 points.
	PackedVector3Array hull;
	{
		const int want = MIN(48, int(out_verts.size()));
		LocalVector<float> dmin;
		dmin.resize(out_verts.size());
		int pick = 0;
		float best = -1.0f;
		for (int k = 0; k < out_verts.size(); k++) {
			const float l = out_verts[k].length_squared();
			if (l > best) {
				best = l;
				pick = k;
			}
			dmin[k] = 1e30f;
		}
		for (int s = 0; s < want; s++) {
			hull.push_back(out_verts[pick]);
			int next = 0;
			float far = -1.0f;
			for (int k = 0; k < out_verts.size(); k++) {
				dmin[k] = MIN(dmin[k], out_verts[k].distance_squared_to(out_verts[pick]));
				if (dmin[k] > far) {
					far = dmin[k];
					next = k;
				}
			}
			pick = next;
		}
	}
	const Vector3 axes = Vector3(box.size[ax_long], box.size[ax_short], box.size[ax_b]) * scale * 0.5f;
	r_meta["rock_axes"] = axes;
	r_meta["rock_bottom"] = -axes.y;
	r_meta["rock_hull"] = hull;
	r_meta["rock_faces"] = int(planes.size());
	r_meta["rock_roundness"] = roundness;
	r_meta["rock_type"] = int(rock_type);
	r_meta["rock_family"] = int(family);
	last_triangles = int(out_indices.size() / 3);
	last_faces = int(planes.size());
	last_build_msec = double(OS::get_singleton()->get_ticks_usec() - t0) / 1000.0;
	return im;
}

Ref<ArrayMesh> RockGenerator::generate() {
	Dictionary meta;
	Ref<ImporterMesh> im = build_importer_mesh(meta);
	ERR_FAIL_COND_V(im.is_null(), Ref<ArrayMesh>());
	Ref<ArrayMesh> mesh = im->get_mesh();
	for (const KeyValue<Variant, Variant> &kv : meta) {
		mesh->set_meta(kv.key, kv.value);
	}
	return mesh;
}

Array RockGenerator::generate_library(int p_count) {
	Array out;
	const int base = seed;
	for (int k = 0; k < MAX(p_count, 0); k++) {
		seed = base + k;
		out.push_back(generate());
	}
	seed = base;
	return out;
}

/* Material */

String RockGenerator::get_shader_code() {
	String c = R"(shader_type spatial;
render_mode blend_mix, depth_draw_opaque, cull_back, diffuse_burley, specular_schlick_ggx;

// Rocks (docs/ideas/rocks.md). Mineral colour, bands and specks in the rock's own space (UV = local x, z; UV2.x =
// local y; one unit = half the b axis), so they stay on the stone; the weathered rind and fresh breaks (COLOR.g),
// cavities (COLOR.r); lichen on the open, lit faces, moss on the tops and in the hollows; wetness and snow from the
// engine's weather lines; a contact band where it meets the ground. INSTANCE_CUSTOM: x ground height (world),
// y water film (river bed, swash zone), z metres per unit, w a random 0..1 per stone.
)";
	c += BaseMaterial3D::get_wetness_shader_code();
	c += BaseMaterial3D::get_snow_shader_code();
	c += R"(
// Colours are linear albedo.
uniform vec3 base_color = vec3(0.30, 0.28, 0.26);
uniform vec3 speck_color = vec3(0.04);
uniform float speck_share = 0.18;
uniform float speck_size = 0.004;
uniform vec3 band_color = vec3(0.42, 0.34, 0.30);
uniform float band_contrast = 0.15;
uniform float band_spacing = 0.12;
uniform vec3 rind_color = vec3(0.24, 0.21, 0.16);
uniform float rock_roughness = 0.8;
uniform float porosity = 0.3;
uniform float grain_bump = 0.2;
uniform float lichen_cover = 0.5;
uniform float moss_cover = 0.4;
uniform vec3 moss_color = vec3(0.035, 0.06, 0.012);
uniform vec3 contact_color = vec3(0.07, 0.055, 0.04);
uniform float contact_height = 0.08;
uniform float wetness = 1.0;
uniform float snow = 1.0;
uniform float snow_max_slope = 55.0;

varying vec3 v_world;
varying vec4 v_custom;
varying float v_scale;

float rk_hash3(vec3 p) {
	p = fract(p * 0.3183099 + vec3(0.71, 0.113, 0.419));
	p *= 17.0;
	return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float rk_noise3(vec3 p) {
	vec3 i = floor(p);
	vec3 f = fract(p);
	f = f * f * (3.0 - 2.0 * f);
	return mix(mix(mix(rk_hash3(i), rk_hash3(i + vec3(1, 0, 0)), f.x), mix(rk_hash3(i + vec3(0, 1, 0)), rk_hash3(i + vec3(1, 1, 0)), f.x), f.y),
			mix(mix(rk_hash3(i + vec3(0, 0, 1)), rk_hash3(i + vec3(1, 0, 1)), f.x), mix(rk_hash3(i + vec3(0, 1, 1)), rk_hash3(i + vec3(1, 1, 1)), f.x), f.y), f.z);
}

float rk_fbm3(vec3 p) {
	return rk_noise3(p) * 0.5 + rk_noise3(p * 2.03 + 11.0) * 0.25 + rk_noise3(p * 4.11 + 23.0) * 0.125 + rk_noise3(p * 8.3 + 37.0) * 0.0625;
}

void vertex() {
	v_world = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	v_custom = INSTANCE_CUSTOM;
	v_scale = INSTANCE_CUSTOM.z > 0.0 ? INSTANCE_CUSTOM.z : length(MODEL_MATRIX[0].xyz);
}

void fragment() {
	vec3 lp = vec3(UV.x, UV2.x, UV.y);
	float seed = v_custom.w * 61.7;
	vec3 mp = lp * v_scale + seed; // metres along the stone
	float cavity = COLOR.r;
	float fresh = COLOR.g;
	vec3 wn = normalize((INV_VIEW_MATRIX * vec4(NORMAL, 0.0)).xyz);

	// Mineral colour: broad mottling, bands along the beds, specks of the dark (or light) mineral.
	vec3 col = base_color * (0.82 + 0.36 * rk_fbm3(mp * 1.7));
	col *= 0.9 + 0.2 * rk_noise3(vec3(seed));
	float bands = 0.5 + 0.5 * sin(6.2831853 * (lp.y * v_scale / band_spacing + 0.6 * rk_fbm3(mp * 0.8)));
	col = mix(col, band_color, band_contrast * smoothstep(0.3, 0.9, bands));
	// Pixel footprint in metres from the projection (no screen derivatives, so the path tracer's hits agree).
	float pix = 2.0 / (abs(PROJECTION_MATRIX[1][1]) * VIEWPORT_SIZE.y);
	float footprint = PROJECTION_MATRIX[3][3] != 0.0 ? pix : length(VERTEX) * pix;
	// Specks: point-sampled once a pixel covers several they alias into salt and pepper in raster, while the path
	// tracer's samples average them; both go to the mean share as the footprint passes the speck size.
	float sp = rk_hash3(floor(mp / speck_size));
	float speck_mix = mix(step(1.0 - speck_share, sp), speck_share, smoothstep(0.5 * speck_size, 2.0 * speck_size, footprint));
	col = mix(col, speck_color, speck_mix * 0.85);
	// The rind: weathered faces stained and duller than a fresh break.
	vec3 weathered = mix(col, rind_color, 0.45 + 0.25 * rk_fbm3(mp * 3.1));
	col = mix(weathered, col * 1.08, fresh);
	float rough = rock_roughness;

	// Grain-scale relief as a normal (the stone's geometry carries everything from about a centimetre up).
	float grain_fade = 1.0 - smoothstep(0.004, 0.03, footprint);
	if (grain_bump > 0.0 && grain_fade > 0.0) {
		float e = 0.002;
		vec3 g = vec3(rk_noise3((mp + vec3(e, 0, 0)) * 180.0) - rk_noise3((mp - vec3(e, 0, 0)) * 180.0),
				rk_noise3((mp + vec3(0, e, 0)) * 180.0) - rk_noise3((mp - vec3(0, e, 0)) * 180.0),
				rk_noise3((mp + vec3(0, 0, e)) * 180.0) - rk_noise3((mp - vec3(0, 0, e)) * 180.0));
		g -= wn * dot(g, wn);
		wn = normalize(wn - g * grain_bump * grain_fade);
	}

	// Ground contact: soil splashed up and damp where the stone sits in it.
	float contact = 0.0;
	if (v_custom.z > 0.0) {
		float above = v_world.y - v_custom.x;
		contact = 1.0 - smoothstep(0.0, contact_height * max(v_scale, 0.25), above);
		col = mix(col, contact_color, 0.6 * contact);
	}
	col *= mix(0.55, 1.0, cavity);

	// Lichen: crusts on open, lit, old faces (not on fresh breaks or under water), patches of grey-green with a few
	// orange and black ones (Rhizocarpon, Xanthoria, Lecidea are the common rock crusts).
	float open_face = (1.0 - fresh) * smoothstep(0.4, 0.9, cavity) * (1.0 - v_custom.y) * smoothstep(-0.3, 0.5, wn.y);
	float lic_n = rk_fbm3(mp * 11.0 + 3.0);
	float lic = open_face * smoothstep(0.7 - 0.2 * lichen_cover, 0.73 - 0.2 * lichen_cover, lic_n) * step(0.01, lichen_cover);
	float kind = rk_noise3(mp * 1.3 + 7.0);
	vec3 lic_col = kind < 0.55 ? vec3(0.24, 0.26, 0.19) : (kind < 0.8 ? vec3(0.28, 0.30, 0.07) : (kind < 0.92 ? vec3(0.45, 0.17, 0.03) : vec3(0.03, 0.03, 0.025)));
	col = mix(col, lic_col * (0.8 + 0.4 * rk_noise3(mp * 60.0)), lic);
	rough = mix(rough, 0.9, lic);

	// Moss: on the tops and in the hollows, more near the ground and where it stays damp.
	float moss_place = smoothstep(0.35, 0.85, wn.y + 0.35 * (1.0 - cavity) + 0.3 * contact) * (1.0 - fresh);
	float moss = moss_cover * moss_place * smoothstep(0.42, 0.58, rk_fbm3(mp * 2.2 + 17.0) + 0.25 * moss_cover - 0.12);
	col = mix(col, moss_color * (0.7 + 0.6 * rk_fbm3(mp * 25.0)), moss);
	rough = mix(rough, 0.95, moss);

	// A film of water and its biofilm: river beds and the swash zone stay wet and darker.
	float film = v_custom.y;
	col = mix(col, col * vec3(0.75, 0.72, 0.55), 0.5 * film);
	float wet = max(wetness * weather_wetness_at(v_world), max(film, contact * 0.4 * weather_wetness_at(v_world)));
	float metallic = 0.0;
	float specular = 0.5;
	weather_wetness_apply(wet, wn.y, metallic, porosity, col, rough);

	vec4 snow_s = snow_cover_at(v_world);
	float sw = snow_cover_weight(snow_s.x * snow, snow_max_slope, wn, wn);
	snow_cover_apply(sw, snow_s.y, snow_s.z, col, rough, metallic, specular);

	ALBEDO = col;
	ROUGHNESS = rough;
	METALLIC = metallic;
	SPECULAR = specular;
	NORMAL = normalize((VIEW_MATRIX * vec4(wn, 0.0)).xyz);
}
)";
	return c;
}

Ref<ShaderMaterial> RockGenerator::get_material(RockType p_type) {
	const int key = CLAMP(int(p_type), 0, int(ROCK_MAX) - 1);
	if (Ref<ShaderMaterial> *m = material_cache.getptr(key)) {
		return *m;
	}
	if (shared_shader.is_null()) {
		shared_shader.instantiate();
		shared_shader->set_code(get_shader_code());
	}
	const Lithology &l = get_lithology(RockType(key));
	Ref<ShaderMaterial> m;
	m.instantiate();
	m->set_shader(shared_shader);
	auto srgb = [](const Color &p_c) { return Vector3(p_c.r, p_c.g, p_c.b); }; // linear albedo, as the uniforms take it
	m->set_shader_parameter("base_color", srgb(l.base));
	m->set_shader_parameter("speck_color", srgb(l.speck));
	m->set_shader_parameter("speck_share", l.speck_share);
	m->set_shader_parameter("speck_size", l.speck_size);
	m->set_shader_parameter("band_color", srgb(l.band));
	m->set_shader_parameter("band_contrast", l.band_contrast);
	m->set_shader_parameter("rind_color", srgb(l.rind));
	m->set_shader_parameter("rock_roughness", l.roughness);
	m->set_shader_parameter("porosity", l.porosity);
	m->set_shader_parameter("grain_bump", 0.1f + 0.2f * l.grain);
	material_cache.insert(key, m);
	return m;
}

void RockGenerator::finish() {
	material_cache.clear();
	shared_shader.unref();
}

void RockGenerator::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_rock_type", "rock_type"), &RockGenerator::set_rock_type);
	ClassDB::bind_method(D_METHOD("get_rock_type"), &RockGenerator::get_rock_type);
	ClassDB::bind_method(D_METHOD("set_family", "family"), &RockGenerator::set_family);
	ClassDB::bind_method(D_METHOD("get_family"), &RockGenerator::get_family);
	ClassDB::bind_method(D_METHOD("set_seed", "seed"), &RockGenerator::set_seed);
	ClassDB::bind_method(D_METHOD("get_seed"), &RockGenerator::get_seed);
	ClassDB::bind_method(D_METHOD("set_resolution", "resolution"), &RockGenerator::set_resolution);
	ClassDB::bind_method(D_METHOD("get_resolution"), &RockGenerator::get_resolution);
	ClassDB::bind_method(D_METHOD("set_transport_distance", "metres"), &RockGenerator::set_transport_distance);
	ClassDB::bind_method(D_METHOD("get_transport_distance"), &RockGenerator::get_transport_distance);
	ClassDB::bind_method(D_METHOD("set_weathering", "weathering"), &RockGenerator::set_weathering);
	ClassDB::bind_method(D_METHOD("get_weathering"), &RockGenerator::get_weathering);
	ClassDB::bind_method(D_METHOD("set_detail", "detail"), &RockGenerator::set_detail);
	ClassDB::bind_method(D_METHOD("get_detail"), &RockGenerator::get_detail);
	ClassDB::bind_method(D_METHOD("set_lods", "enabled"), &RockGenerator::set_lods);
	ClassDB::bind_method(D_METHOD("get_lods"), &RockGenerator::get_lods);
	ClassDB::bind_method(D_METHOD("set_simplify", "enabled"), &RockGenerator::set_simplify);
	ClassDB::bind_method(D_METHOD("get_simplify"), &RockGenerator::get_simplify);
	ClassDB::bind_method(D_METHOD("get_roundness"), &RockGenerator::get_roundness);
	ClassDB::bind_method(D_METHOD("generate"), &RockGenerator::generate);
	ClassDB::bind_method(D_METHOD("generate_library", "count"), &RockGenerator::generate_library);
	ClassDB::bind_method(D_METHOD("get_last_build_msec"), &RockGenerator::get_last_build_msec);
	ClassDB::bind_method(D_METHOD("get_last_triangle_count"), &RockGenerator::get_last_triangle_count);
	ClassDB::bind_static_method("RockGenerator", D_METHOD("get_shader_code"), &RockGenerator::get_shader_code);
	ClassDB::bind_static_method("RockGenerator", D_METHOD("get_material", "rock_type"), &RockGenerator::get_material);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "rock_type", PROPERTY_HINT_ENUM, "Granite,Limestone,Sandstone,Basalt,Slate"), "set_rock_type", "get_rock_type");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "family", PROPERTY_HINT_ENUM, "Rockfall,Scree,Glacial,Cobble,Pebble,Cliff Block"), "set_family", "get_family");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "seed"), "set_seed", "get_seed");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "resolution", PROPERTY_HINT_RANGE, "0,160,1"), "set_resolution", "get_resolution");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "transport_distance", PROPERTY_HINT_RANGE, "-1,200000,1,or_greater,suffix:m"), "set_transport_distance", "get_transport_distance");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "weathering", PROPERTY_HINT_RANGE, "-1,1,0.01"), "set_weathering", "get_weathering");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "detail", PROPERTY_HINT_RANGE, "0,2,0.01"), "set_detail", "get_detail");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "lods"), "set_lods", "get_lods");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "simplify"), "set_simplify", "get_simplify");

	BIND_ENUM_CONSTANT(ROCK_GRANITE);
	BIND_ENUM_CONSTANT(ROCK_LIMESTONE);
	BIND_ENUM_CONSTANT(ROCK_SANDSTONE);
	BIND_ENUM_CONSTANT(ROCK_BASALT);
	BIND_ENUM_CONSTANT(ROCK_SLATE);
	BIND_ENUM_CONSTANT(ROCK_MAX);
	BIND_ENUM_CONSTANT(FAMILY_ROCKFALL);
	BIND_ENUM_CONSTANT(FAMILY_SCREE);
	BIND_ENUM_CONSTANT(FAMILY_GLACIAL);
	BIND_ENUM_CONSTANT(FAMILY_COBBLE);
	BIND_ENUM_CONSTANT(FAMILY_PEBBLE);
	BIND_ENUM_CONSTANT(FAMILY_CLIFF_BLOCK);
	BIND_ENUM_CONSTANT(FAMILY_MAX);
}

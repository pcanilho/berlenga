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

#include "modules/terrain_deform/terrain_generator_3d.h"

#include "core/math/random_pcg.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/templates/sort_array.h"
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
	static const char *names[FAMILY_MAX] = { "rockfall", "scree", "glacial", "cobble", "pebble", "cliff_block", "corestone", "sea_stack", "sea_arch", "sea_platform", "hoodoo", "pavement", "fin", "pothole", "goblin", "cave_mouth" };
	return names[CLAMP(int(p_family), 0, int(FAMILY_MAX) - 1)];
}

void RockGenerator::set_rock_type(RockType p_type) {
	rock_type = RockType(CLAMP(int(p_type), 0, int(ROCK_MAX) - 1));
}

void RockGenerator::set_family(Family p_family) {
	family = Family(CLAMP(int(p_family), 0, int(FAMILY_MAX) - 1));
}

int RockGenerator::get_base_resolution() const {
	static const int res[FAMILY_MAX] = { 112, 88, 96, 56, 48, 112, 104, 256, 224, 220, 200, 240, 224, 220, 128, 176 };
	return resolution > 0 ? resolution : res[family];
}

int RockGenerator::get_effective_resolution() const {
	const int r = get_base_resolution();
	// Close-up: cells of about 3 mm at the family's typical size, fine enough to carry the rock's millimetre fabric
	// as geometry (5 mm on a metre-scale block, where the grid caps at 300), never coarser than the base shape.
	if (!close_up) {
		return r;
	}
	const int want = int(2.4f * float(get_marks_metres_per_unit()) / 0.003f);
	return CLAMP(want, r, 300);
}

// The size the marks are laid out at: marks_scale, else the family's typical size, snapped to the scale steps the
// shader uses (octaves from the family's size), so a stone's crystals are within 41% of their real size (granites
// differ more than that) and the carved relief and the shader's mineral colours stay on one map.
real_t RockGenerator::get_marks_metres_per_unit() const {
	const real_t fam = get_family_metres_per_unit(family);
	if (marks_scale <= 0.0) {
		return fam;
	}
	return fam * Math::pow(real_t(2.0), Math::round(Math::log2(marks_scale / fam)));
}

real_t RockGenerator::get_family_metres_per_unit(Family p_family) {
	static const real_t mpu[FAMILY_MAX] = { 0.65, 0.16, 0.85, 0.11, 0.035, 0.65, 1.0, 3.5, 5.0, 5.0, 2.5, 4.0, 5.0, 2.5, 1.2, 5.0 };
	return mpu[CLAMP(int(p_family), 0, int(FAMILY_MAX) - 1)];
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
	static const float w[FAMILY_MAX] = { 0.45f, 0.2f, 0.6f, 0.15f, 0.05f, 0.7f, 0.95f, 0.6f, 0.6f, 0.45f, 0.7f, 0.6f, 0.75f, 0.1f, 0.85f, 0.6f };
	return w[family];
}

// Sea-spray salt: the sea stacks, arches, platforms and cave mouths stand in the spray; inland stones none.
real_t RockGenerator::get_effective_salt() const {
	if (salt >= 0.0) {
		return salt;
	}
	return (family == FAMILY_SEA_STACK || family == FAMILY_SEA_ARCH || family == FAMILY_SEA_PLATFORM) ? 1.0 : (family == FAMILY_CAVE_MOUTH ? 0.4 : 0.0);
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

/* Close-up detail */


// Worley F1, F2 (in cells) and the hash of the nearest cell.
void rk_worley2(const Vector3 &p_p, uint32_t p_seed, float &r_f1, float &r_f2, uint32_t &r_id) {
	const int x = int(Math::floor(p_p.x)), y = int(Math::floor(p_p.y)), z = int(Math::floor(p_p.z));
	float f1 = 9.0f, f2 = 9.0f;
	uint32_t id = 0;
	for (int k = -1; k <= 1; k++) {
		for (int j = -1; j <= 1; j++) {
			for (int i = -1; i <= 1; i++) {
				const Vector3 f(x + i + rk_lattice(x + i, y + j, z + k, p_seed), y + j + rk_lattice(x + i, y + j, z + k, p_seed + 7u), z + k + rk_lattice(x + i, y + j, z + k, p_seed + 13u));
				const float d = (f - p_p).length_squared();
				if (d < f1) {
					f2 = f1;
					f1 = d;
					id = rk_hash(uint32_t(x + i) * 73856093u ^ uint32_t(y + j) * 19349663u ^ uint32_t(z + k) * 83492791u ^ p_seed);
				} else if (d < f2) {
					f2 = d;
				}
			}
		}
	}
	r_f1 = Math::sqrt(f1);
	r_f2 = Math::sqrt(f2);
	r_id = id;
}

// Worley cell search that also gives the nearest feature point (in cells).
void rk_worley_point(const Vector3 &p_p, uint32_t p_seed, float &r_f1, float &r_f2, uint32_t &r_id, Vector3 &r_point) {
	const int x = int(Math::floor(p_p.x)), y = int(Math::floor(p_p.y)), z = int(Math::floor(p_p.z));
	float f1 = 9.0f, f2 = 9.0f;
	uint32_t id = 0;
	Vector3 best;
	for (int k = -1; k <= 1; k++) {
		for (int j = -1; j <= 1; j++) {
			for (int i = -1; i <= 1; i++) {
				const int cx = x + i, cy = y + j, cz = z + k;
				const Vector3 f(cx + rk_lattice(cx, cy, cz, p_seed), cy + rk_lattice(cx, cy, cz, p_seed + 7u), cz + rk_lattice(cx, cy, cz, p_seed + 13u));
				const float d = (f - p_p).length_squared();
				if (d < f1) {
					f2 = f1;
					f1 = d;
					id = rk_hash(uint32_t(cx) * 73856093u ^ uint32_t(cy) * 19349663u ^ uint32_t(cz) * 83492791u ^ (p_seed + 29u));
					best = f;
				} else if (d < f2) {
					f2 = d;
				}
			}
		}
	}
	r_f1 = Math::sqrt(f1);
	r_f2 = Math::sqrt(f2);
	r_id = id;
	r_point = best;
}

// The granite crystal map, shared with the shader (rk_crystal there, same hash and lattice): crystal cells of
// 1.8 cm, quartz (30%) or feldspar by the cell's id; biotite clots on a 1.3 cm map of their own (7% of its cells).
static const float RK_CRYSTAL_CELL = 0.018f;
static const uint32_t RK_CRYSTAL_SEED = 0x51A7u;
static const float RK_BIOTITE_CELL = 0.013f;
// Sandstone laminae: 1.9 cm, each with its own hardness (shared with the shader's lamina colour).
static const float RK_LAMINA = 0.019f;
static const uint32_t RK_LAMINA_SEED = 0x5A5Du;
static const uint32_t RK_BIOTITE_SEED = 0xB10Bu;
static const float RK_BIOTITE_SHARE = 0.07f; // biotite is 5 to 10% of a granite

// Millimetre to decimetre relief of a rock surface, in metres, outward positive. p in metres in the stone's own
// frame (the frame the shader sees), n the outward normal in that frame, fresh 1 on a fresh break, p_cell the
// grid cell in metres. Each scale is geometry, so the silhouette breaks and the path tracer sees it:
// - granite: crystals as planar facets at their own heights from the crystal map (quartz proud, feldspar a little
//   lower, biotite weathered out into pits; Migoń 2006), etched boundaries, grus pits where crystals were
//   plucked, flakes spalled off the rind, hairline cracks;
// - sandstone: laminae as ledges, harder ones standing out (differential weathering of beds);
// - limestone: rillenkarren as cut runnels, sharp crests between round channels, running down the upper faces
//   (Ford and Williams 2007), and micropits;
// - basalt: vesicles as real spherical cavities;
// - slate: cleavage steps;
// - fresh breaks: stepped fracture faces (hackle steps) and conchoidal ripples (Kulander et al. 1990).
// A feature needs about three grid cells across to be meshed rather than aliased: finer ones fade out.
float rk_relief(RockGenerator::RockType p_type, const Vector3 &p, const Vector3 &n, float p_fresh, float p_cell, float p_weather) {
	auto fit = [&](float p_size) { return Math::smoothstep(1.5f * p_cell, 3.0f * p_cell, p_size); };
	const float old = 1.0f - p_fresh;
	float d = 0.0f;
	float f1, f2;
	uint32_t id;
	Vector3 fp;
	// Flakes spalled off a weathered face: plates of 4 to 8 cm, each its own plane at its own depth, so the face is
	// terraced and the outline steps (all rock types but the soft ones get this on old faces).
	if (p_type == RockGenerator::ROCK_GRANITE || p_type == RockGenerator::ROCK_BASALT || p_type == RockGenerator::ROCK_SANDSTONE) {
		rk_worley_point(p / 0.06f, 0x3F1Au, f1, f2, id, fp);
		const float u = float(id >> 8) / 16777216.0f;
		const Vector3 tilt = Vector3(float((id >> 4) & 15u) - 7.5f, float((id >> 12) & 15u) - 7.5f, float((id >> 20) & 15u) - 7.5f) * 0.016f;
		// Deep enough to step the outline at a metre's distance (a 3 to 7 mm scar where a flake came away).
		const float plate = -0.007f * u + tilt.dot(p - fp * 0.06f);
		d += fit(0.06f) * old * MIN(plate, 0.002f) * (0.5f + 0.5f * p_weather);
	}
	switch (p_type) {
		case RockGenerator::ROCK_GRANITE: {
			rk_worley_point(p / RK_CRYSTAL_CELL, RK_CRYSTAL_SEED, f1, f2, id, fp);
			const float u = float(id >> 8) / 16777216.0f;
			// Each crystal a planar facet: its class height plus a small tilt about its centre (cleavage faces).
			const Vector3 tilt = Vector3(float((id >> 2) & 15u) - 7.5f, float((id >> 10) & 15u) - 7.5f, float((id >> 18) & 15u) - 7.5f) * 0.03f;
			float facet = u < 0.3f ? 0.0016f : 0.0003f; // quartz proud, feldspar lower (it kaolinises)
			facet += tilt.dot(p - fp * RK_CRYSTAL_CELL);
			d += fit(RK_CRYSTAL_CELL) * facet;
			// Etched boundaries: a V groove along each grain edge.
			const float border = (f2 - f1) * RK_CRYSTAL_CELL; // metres to the boundary, about twice
			d -= fit(RK_CRYSTAL_CELL) * 0.0009f * old * (1.0f - Math::smoothstep(0.0f, 1.2f * p_cell, border));
			// Biotite: clots of dark mica on their own finer map, weathered out into pits (the shader's dark grains).
			{
				float b1, b2;
				uint32_t bid;
				Vector3 bp;
				rk_worley_point(p / RK_BIOTITE_CELL, RK_BIOTITE_SEED, b1, b2, bid, bp);
				if (float(bid >> 8) / 16777216.0f < RK_BIOTITE_SHARE) {
					d -= Math::smoothstep(1.1f * p_cell, 2.2f * p_cell, RK_BIOTITE_CELL) * 0.0012f * (0.4f + 0.6f * old) * Math::smoothstep(0.0f, 0.2f, b2 - b1);
				}
			}
			// Grus pits: crystals plucked out of the rind, flat-bottomed, a few per decimetre.
			rk_worley_point(p / 0.035f, 0x6A11u, f1, f2, id, fp);
			if ((id & 255u) < 30u) {
				d -= fit(0.035f) * 0.003f * old * p_weather * Math::smoothstep(0.42f, 0.3f, f1);
			}
		} break;
		case RockGenerator::ROCK_SANDSTONE: {
			// Laminae 1.4 to 2.6 cm, each with its own hardness: the hard ones stand out as ledges.
			const float q = p.y / RK_LAMINA; // planar, so the shader's lamina colour lines up
			const float lam = Math::floor(q);
			const float in_lam = q - lam;
			const float hard = rk_lattice(int(lam), 0, 0, RK_LAMINA_SEED);
			const float ledge = 0.005f * hard * Math::smoothstep(0.0f, 0.1f, in_lam) * Math::smoothstep(1.0f, 0.9f, in_lam);
			d += fit(RK_LAMINA) * (ledge - 0.0025f) * (1.0f - Math::abs(n.y)) * (0.5f + 0.5f * old);
			d += fit(0.012f) * 0.0006f * rk_fbm(p / 0.012f, 0x21u, 2);
		} break;
		case RockGenerator::ROCK_LIMESTONE: {
			// Runnels down the slope of an upper face: round channels, knife-edge crests between them.
			const Vector3 across = n.cross(Vector3(0, 1, 0));
			if (across.length_squared() > 1e-4f && n.y > 0.15f) {
				const float q = (across.normalized().dot(p) + 0.005f * rk_fbm(p / 0.03f, 0x31u, 2)) / 0.022f;
				const float w = 2.0f * Math::abs(q - Math::floor(q) - 0.5f); // 0 mid-channel, 1 on the crest
				const float channel = 1.0f - w * w;
				d -= fit(0.022f) * 0.005f * channel * Math::smoothstep(0.15f, 0.6f, n.y) * (0.3f + 0.7f * old);
			}
			rk_worley_point(p / 0.016f, 0x33u, f1, f2, id, fp);
			if ((id & 255u) < 90u) {
				d -= fit(0.016f) * 0.0015f * Math::pow(MAX(0.0f, 1.0f - f1 / 0.4f), 2.0f);
			}
		} break;
		case RockGenerator::ROCK_BASALT: {
			// Vesicles: gas bubbles frozen in the lava, cut open at the surface as spherical cavities of 3 to 7 mm.
			rk_worley_point(p / 0.024f, 0x41u, f1, f2, id, fp);
			if ((id & 255u) < 120u) {
				const float r = (0.22f + 0.2f * float((id >> 8) & 255u) / 255.0f) * 0.024f;
				const float f = f1 * 0.024f;
				// A cavity is a dip one or two cells across even when it cannot be meshed round: fade later.
				d -= Math::smoothstep(1.0f * p_cell, 2.0f * p_cell, 2.0f * r) * Math::sqrt(MAX(r * r - f * f, 0.0f)) * 1.3f;
			}
			d += fit(0.015f) * 0.0004f * rk_fbm(p / 0.015f, 0x42u, 2);
		} break;
		case RockGenerator::ROCK_SLATE: {
			const float q = p.y / 0.015f + 0.4f * rk_fbm(p / 0.03f, 0x51u, 2);
			d += fit(0.015f) * 0.0016f * (q - Math::floor(q)) * (1.0f - Math::abs(n.y));
		} break;
		default:
			break;
	}
	// Hairline cracks: along cell boundaries of a 9 cm map, about one boundary in three, a V a few mm deep.
	if (p_type != RockGenerator::ROCK_SANDSTONE) {
		rk_worley_point(p / 0.09f, 0x7C3u, f1, f2, id, fp);
		const float edge = (f2 - f1) * 0.09f;
		const float on = Math::smoothstep(0.55f, 0.7f, rk_noise(p / 0.05f, 0x7C4u));
		d -= fit(0.03f) * 0.003f * on * (1.0f - Math::smoothstep(0.0f, 1.5f * p_cell, edge));
	}
	// Fresh breaks: hackle steps (a stepped face, risers of 2 to 4 mm every 4 to 7 cm across the break) and
	// conchoidal ripples round points of origin.
	if (p_fresh > 0.05f) {
		const Vector3 dir = Vector3(0.6f, 0.3f, 0.74f);
		const float q = dir.dot(p) / 0.055f + 0.35f * rk_fbm(p / 0.1f, 0x61u, 2);
		const float st = q - Math::floor(q);
		// Steps are short: each riser runs a few centimetres and dies out (they branch from the fracture front).
		const float run = Math::smoothstep(0.45f, 0.65f, rk_noise(p / 0.07f + Vector3(Math::floor(q) * 3.1f, 0, 0), 0x64u));
		const float riser = (0.003f + 0.003f * rk_lattice(int(Math::floor(q)), 1, 0, 0x62u)) * run;
		d += fit(0.055f) * p_fresh * riser * (Math::smoothstep(0.0f, 0.08f, st) - st);
		rk_worley2(p / 0.12f, 0x63u, f1, f2, id);
		const float r = f1 * 0.12f;
		const float ripple = Math::sin(r / 0.02f * float(Math::TAU)) * Math::exp(-r / 0.1f);
		d += fit(0.02f) * p_fresh * 0.0012f * ripple;
	}
	return d;
}


struct RockPlane {
	Vector3 n;
	float d = 0.0f;
	bool fresh = false;
	// A stepped face: the plane drops back by `rise` across a line (t . p = t0) on the face, the step a fracture
	// took from one joint to the next (en echelon steps).
	Vector3 t;
	float t0 = 0.0f;
	float rise = 0.0f;
	float width = 0.0f;
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

// Surface nets (Gibson 1998) over a field: one vertex per cell the surface crosses, at the mean of its edge crossings;
// one quad per crossed grid edge; area-weighted normals; optionally simplified (see below). False if it came out empty.
bool rk_surface_nets(const RockGrid &fine, bool p_simplify, LocalVector<Vector3> &verts, LocalVector<Vector3> &normals, LocalVector<int> &indices) {
	verts.clear();
	normals.clear();
	indices.clear();
	const int cx = fine.nx - 1, cy = fine.ny - 1, cz = fine.nz - 1;
	LocalVector<int> cell_vertex;
	cell_vertex.resize(cx * cy * cz);
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
	if (verts.size() < 4 || indices.size() < 12) {
		return false;
	}

	// 5. Normals (area-weighted from the faces), cavity (how much the field closes in above a point: open faces 1,
	// cracks and pits darker), freshness.
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
	if (p_simplify && indices.size() > 3000) {
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
	return true;
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
	if (family == FAMILY_CORESTONE) {
		// Tor blocks lie between sheeting joints, which run near the ground surface and close up towards it:
		// pillow-shaped, wider than thick (Twidale 1982).
		a = MAX(a, 1.15f);
		c *= 0.8f;
	}
	if (family == FAMILY_PEBBLE) {
		c *= 0.8f; // shingle starts platy as well as wearing so
	}
	// Coast forms (Trenhaile 1987; Sunamura 1992): one mass of bedrock each, cut by the joints and the beds.
	const bool coast_form = family == FAMILY_SEA_STACK || family == FAMILY_SEA_ARCH || family == FAMILY_SEA_PLATFORM;
	if (family == FAMILY_SEA_STACK) {
		a = 2.6f + 0.8f * rng.randf(); // x: the column's height when stood on end, several times its width
		c = 0.75f + 0.2f * rng.randf();
	} else if (family == FAMILY_SEA_ARCH) {
		a = 1.5f + 0.4f * rng.randf(); // x: the span along the coast; y: its height, as much as its thickness
		c = 1.0f;
	} else if (family == FAMILY_SEA_PLATFORM) {
		a = 3.0f; // x: along the cliff foot; z: out to sea; y: a thin bench
		c = 0.12f + 0.04f * rng.randf();
	} else if (family == FAMILY_HOODOO) {
		a = 2.6f + 1.4f * rng.randf(); // x: the column's height when stood on end, 3 to 6 times its width
		c = 0.7f + 0.2f * rng.randf();
	} else if (family == FAMILY_PAVEMENT) {
		a = 2.0f; // a square-ish patch of pavement; y: the slab's depth (the grykes' depth)
		c = 0.16f + 0.06f * rng.randf();
	} else if (family == FAMILY_FIN) {
		a = 2.0f + 1.4f * rng.randf(); // x: along the joint; z: the fin's height; y: its thickness, between two joints
		c = 0.22f + 0.13f * rng.randf();
	} else if (family == FAMILY_CAVE_MOUTH) {
		a = 1.3f + 0.5f * rng.randf(); // x: along the cliff; z: up; y: into it (the front face +y)
		c = 0.62f + 0.1f * rng.randf(); // well under the height after rounding, so y stays the short axis
	} else if (family == FAMILY_GOBLIN) {
		a = 1.3f + 0.6f * rng.randf(); // x: up when stood; a knob a little taller than wide
		c = 0.8f + 0.15f * rng.randf();
	} else if (family == FAMILY_POTHOLE) {
		a = 1.8f + 0.6f * rng.randf(); // x: down the channel; z: across; y: the slab's depth below the bed
		c = 0.26f + 0.08f * rng.randf();
	}
	const bool landform = family == FAMILY_HOODOO || family == FAMILY_PAVEMENT || family == FAMILY_FIN || family == FAMILY_POTHOLE || family == FAMILY_GOBLIN || family == FAMILY_CAVE_MOUTH;
	const bool upright = family == FAMILY_SEA_STACK || family == FAMILY_HOODOO || family == FAMILY_GOBLIN; // stood on end in the field: beds cross x
	// Fresh fragments spread over the Zingg classes rather than sitting near the cube: talus and rockfall hold many
	// slabs and blades, rods and wedges as well as blocks (Domokos et al. 2020 give the mean, not the spread).
	int fragment_form = 0; // 0 block, 1 slab, 2 rod, 3 wedge
	if (family == FAMILY_ROCKFALL || family == FAMILY_SCREE) {
		const float f = rng.randf();
		fragment_form = f < 0.32f ? 1 : (f < 0.55f ? 2 : (f < 0.75f ? 3 : 0));
		if (fragment_form == 1) {
			c *= 0.6f;
		} else if (fragment_form == 2) {
			a *= 1.4f;
			c *= 0.85f;
		}
	}
	a = MAX(a, 1.0f);
	c = MIN(c, 1.0f);
	LocalVector<RockPlane> planes;
	auto jitter_dir = [&](const Vector3 &p_axis, float p_degrees) {
		const Vector3 r(rng.randf() * 2.0f - 1.0f, rng.randf() * 2.0f - 1.0f, rng.randf() * 2.0f - 1.0f);
		return (p_axis + r * Math::tan(Math::deg_to_rad(p_degrees))).normalized();
	};
	if (family == FAMILY_SEA_STACK || family == FAMILY_HOODOO || family == FAMILY_GOBLIN) {
		// A stack is what is left of a headland between joints at all angles (Old Harry, the Twelve Apostles): in plan
		// an irregular polygon of 6 to 9 joint faces at uneven azimuths and distances, each a few degrees off upright,
		// the whole leaning a little one way and narrowing upward; the foot square across, the top two or three
		// oblique breaks, ragged rather than flat.
		const bool hoodoo = family == FAMILY_HOODOO || family == FAMILY_GOBLIN; // nearly upright, the neck carved later under its cap
		// A hoodoo is rounded by rain wash rather than cut by joints: more, shorter faces, all at much the same reach.
		const int sides = hoodoo ? 11 + int(rng.randf() * 4.99f) : 6 + int(rng.randf() * 3.99f);
		float th[16];
		float acc = rng.randf() * float(Math::TAU);
		for (int k = 0; k < sides; k++) {
			th[k] = acc;
			acc += float(Math::TAU) / sides * (0.45f + 1.1f * rng.randf());
		}
		const float lean = (hoodoo ? 0.01f + 0.03f * rng.randf() : 0.04f + 0.08f * rng.randf()), lean_az = rng.randf() * float(Math::TAU);
		const float taper = hoodoo ? 0.02f + 0.03f * rng.randf() : 0.05f + 0.08f * rng.randf();
		for (int k = 0; k < sides; k++) {
			const float cy = Math::cos(th[k]), sz = Math::sin(th[k]);
			// Leaning toward lean_az tilts the faces on that side out, the far side in; all of them in for the taper.
			const float tilt = taper - lean * Math::cos(th[k] - lean_az) + (rng.randf() - 0.5f) * 0.12f;
			Vector3 n(tilt, cy, sz);
			n.normalize();
			const float radius = Math::sqrt(c * c * cy * cy + sz * sz);
			planes.push_back({ n, radius * (hoodoo ? 0.88f + 0.12f * rng.randf() : 0.72f + 0.28f * rng.randf()) * n.dot(Vector3(0, cy, sz)), false });
		}
		planes.push_back({ jitter_dir(Vector3(-1, 0, 0), 3.0f), a, false });
		const int tops = 2 + int(rng.randf() * 1.99f);
		for (int k = 0; k < tops; k++) {
			const float az = rng.randf() * float(Math::TAU);
			const float slope = hoodoo ? 0.05f + 0.15f * rng.randf() : 0.15f + 0.6f * rng.randf(); // a caprock stays nearly flat
			const Vector3 n = Vector3(1.0f, slope * Math::cos(az), slope * Math::sin(az)).normalized();
			planes.push_back({ n, n.x * a * (0.82f + 0.16f * rng.randf()), k > 0 && rng.randf() < 0.5f });
		}
	} else if (lith.columnar && family != FAMILY_PEBBLE && family != FAMILY_SEA_ARCH && family != FAMILY_SEA_PLATFORM && family != FAMILY_PAVEMENT && family != FAMILY_FIN && family != FAMILY_POTHOLE && family != FAMILY_CAVE_MOUTH) {
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
		// Joint sets are rarely square to each other: the two cross sets meet at 60 to 90 degrees, and a fresh
		// fragment's faces wander more than a block still in the wall.
		const bool fresh_block = family == FAMILY_ROCKFALL || family == FAMILY_SCREE;
		const float set_a = rng.randfn(0.0f, 0.12f);
		const float set_b = set_a + (rng.randf() - 0.5f) * (fresh_block ? 0.9f : 0.45f);
		for (int axis = 0; axis < 3; axis++) {
			for (int sgn = -1; sgn <= 1; sgn += 2) {
				Vector3 n;
				n[axis] = float(sgn);
				if (axis != 1) {
					n = Basis(Vector3(0, 1, 0), axis == 0 ? set_a : set_b).xform(n);
				}
				// Bedding (y) stays flatter than the cross joints.
				const float j = (axis == 1 ? lith.joint_jitter * 0.4f : lith.joint_jitter) * (fresh_block ? 2.0f : 1.0f);
				RockPlane pl = { jitter_dir(n, j), half[axis] * (0.88f + 0.24f * rng.randf()), false };
				// Steps on some faces of fresh fragments and wall blocks.
				if ((fresh_block || family == FAMILY_CLIFF_BLOCK) && rng.randf() < 0.35f) {
					const Vector3 any = Math::abs(pl.n.y) < 0.9f ? Vector3(0, 1, 0) : Vector3(1, 0, 0);
					pl.t = pl.n.cross(any).normalized();
					pl.t = Basis(pl.n, (rng.randf() - 0.5f) * 1.2f).xform(pl.t);
					pl.t0 = (rng.randf() - 0.5f) * 0.5f; // mid-face: a step near an edge leaves a thin fin
					pl.rise = (0.05f + 0.1f * rng.randf()) * c;
					pl.width = 0.02f + 0.03f * rng.randf();
				}
				planes.push_back(pl);
			}
		}
	}
	// A stack's or arch's sides are old joint faces and falls, at all angles: big cuts off its edges and corners, so
	// its plan and profile are irregular polygons, not a box.
	if (family == FAMILY_SEA_STACK || family == FAMILY_SEA_ARCH) {
		// A stack's: one to three dominant joint faces, steep, at any azimuth, cutting deep into one side.
		const int count = family == FAMILY_SEA_STACK ? 1 + int(rng.randf() * 2.99f) : 4 + int(rng.randf() * 3.0f);
		for (int k = 0; k < count; k++) {
			Vector3 n(family == FAMILY_SEA_STACK ? Math::abs(rng.randfn(0.0f, 0.15f)) : rng.randfn(0.0f, 1.0f), rng.randfn(0.0f, 1.0f) * (family == FAMILY_SEA_ARCH ? 0.5f : 1.0f), rng.randfn(0.0f, 1.0f));
			n = n.length_squared() > 1e-6f ? n.normalized() : Vector3(0, 0, 1);
			// (A stack's face leans in toward the top: it takes more off up there and leaves the foot.)
			const float support = family == FAMILY_SEA_STACK ? Math::abs(n.y) * c + Math::abs(n.z) + n.x * a * 0.3f : Math::abs(n.x) * a + Math::abs(n.y) * c + Math::abs(n.z);
			// The last one or two are recent falls: a fresh scar face, paler and without the rind.
			planes.push_back({ n, support * (0.62f + 0.2f * rng.randf()), k >= count - 1 - int(rng.randf() * 1.5f) });
		}
	}
	// A platform's edge follows the joints and the sea's reach, not a rectangle: a few vertical cuts across corners.
	if (family == FAMILY_SEA_PLATFORM || family == FAMILY_PAVEMENT || family == FAMILY_POTHOLE) {
		for (int k = 0; k < 5; k++) {
			const float th = rng.randf() * float(Math::TAU);
			const Vector3 n(Math::cos(th), 0.0f, Math::sin(th));
			const float support = Math::abs(n.x) * a + Math::abs(n.z);
			planes.push_back({ n, support * (0.7f + 0.2f * rng.randf()), false });
		}
	}
	// Secondary fractures: corner and edge cuts, fresh faces where the block broke when it fell or was struck.
	const int cuts = (family == FAMILY_SEA_PLATFORM || landform) ? 0 : int(lith.extra_cuts * (family == FAMILY_ROCKFALL || family == FAMILY_SCREE ? 3.0f : 1.0f) * (0.5f + rng.randf()) + 0.5f);
	for (int k = 0; k < cuts; k++) {
		const Vector3 corner(a * (rng.randf() < 0.5f ? -1.0f : 1.0f), c * (rng.randf() < 0.5f ? -1.0f : 1.0f), rng.randf() < 0.5f ? -1.0f : 1.0f);
		const Vector3 n = jitter_dir(corner.normalized(), 25.0f);
		planes.push_back({ n, n.dot(corner) * ((family == FAMILY_ROCKFALL || family == FAMILY_SCREE) ? 0.45f + 0.3f * rng.randf() : 0.6f + 0.25f * rng.randf()), true });
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
	if (fragment_form == 3) {
		// A wedge: one face cut steeply across the top, from near full height on one side to low on the other.
		const float side = rng.randf() < 0.5f ? -1.0f : 1.0f;
		const Vector3 n = Vector3(side * (0.8f + 0.5f * rng.randf()) * c / a, 1.0f, (rng.randf() - 0.5f) * 0.4f).normalized();
		planes.push_back({ n, n.y * c * (0.15f + 0.25f * rng.randf()) + Math::abs(n.x) * a * 0.3f, true });
	}
	// Impact chips: conchoidal scoops off corners and edges (bed-load chipping, Novák-Szabó et al. 2018), fresh.
	static const int chips_per_family[FAMILY_MAX] = { 3, 5, 3, 3, 1, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
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
	// Weathering pans (gnammas, rock basins): water standing on the flat top of a tor block rots a round,
	// flat-floored pan into it, a few centimetres to decimetres deep (Twidale and Corbin 1963). One on most tops.
	if (family == FAMILY_CORESTONE && rng.randf() < 0.6f) {
		const float r = (0.25f + 0.25f * rng.randf()) * MIN(a, 1.0f);
		const float depth = (0.08f + 0.1f * rng.randf()) * c;
		const float big = r * 2.2f; // a large, shallow sphere: a pan with a flat floor and a sloping rim
		const Vector3 at((rng.randf() - 0.5f) * a * 0.6f, c * 0.95f - depth + big, (rng.randf() - 0.5f) * 0.6f);
		scoops.push_back({ at, big });
	}
	// Partial cracks along the joint directions, open only near the surface (fresh rockfall and cliff blocks).
	LocalVector<RockCrack> cracks;
	// A loose fragment broke along its cracks; open partial cracks stay on blocks still in the wall.
	const int crack_count = family == FAMILY_CLIFF_BLOCK ? int(rng.randf() * 1.6f) : (family == FAMILY_GLACIAL ? int(rng.randf() * 1.3f) : 0);

	// 2. The field. A coarse grid carries the abrasion (the flow is smooth and needs many small steps); the fine grid
	// carries the detail.
	const int res = get_effective_resolution();
	// The grid holds the whole solid: joint sets at an angle to each other (and stepped faces) reach past the a, c, 1
	// box at their acute corners, and a grid cut there leaves the mesh open. Probe the planes' intersection.
	Vector3 extent(a, c, 1.0f);
	{
		const int pn = 40;
		const Vector3 probe = Vector3(a, c, 1.0f) * 2.5f;
		for (int z = 0; z < pn; z++) {
			for (int y = 0; y < pn; y++) {
				for (int x = 0; x < pn; x++) {
					const Vector3 q = (Vector3(x, y, z) / float(pn - 1) * 2.0f - Vector3(1, 1, 1)) * probe;
					bool inside = true;
					for (const RockPlane &pl : planes) {
						if (pl.n.dot(q) - pl.d > 0.0f) {
							inside = false;
							break;
						}
					}
					if (inside) {
						extent = Vector3(MAX(extent.x, Math::abs(q.x)), MAX(extent.y, Math::abs(q.y)), MAX(extent.z, Math::abs(q.z)));
					}
				}
			}
		}
		extent += probe * (2.0f / float(pn - 1)); // one probe cell
	}
	const Vector3 half_box = extent * 1.04f + Vector3(1, 1, 1) * 0.08f;
	const float h = 2.0f * half_box.x / float(res - 1);
	// The abrasion runs on the base shape's coarse grid whatever the fine grid, so a close-up shape is the same stone
	// (the flow's step count is capped, and a finer coarse grid would round it less).
	const float hb = 2.0f * half_box.x / float(get_base_resolution() - 1); // the base shape's cell
	const float h_flow = 2.0f * hb;
	// The rock's centimetre marks (lumps, grain, laminae, pits, striae) are sized by the base cell too: the close-up
	// shape adds its millimetre relief on top of the same stone.
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
		cr.width = MAX(1.6f * hb, 0.012f) * (1.0f + 0.5f * rng.randf());
		cr.depth = MIN((0.12f + 0.2f * rng.randf()) * c, 0.3f * c);
		cracks.push_back(cr);
	}
	auto shape = [&](const Vector3 &p, bool *r_fresh) {
		float phi = -1e9f;
		bool fresh = false;
		for (const RockPlane &pl : planes) {
			float d = pl.n.dot(p) - pl.d;
			if (pl.rise != 0.0f) {
				d += pl.rise * Math::smoothstep(-pl.width, pl.width, pl.t.dot(p) - pl.t0);
			}
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
	static const float spheroidal_share[FAMILY_MAX] = { 0.05f, 0.0f, 0.5f, 0.15f, 0.15f, 0.25f, 0.75f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }; // coast and landforms: their own shaping
	const float spheroidal = lith.spheroidal * weather * spheroidal_share[family];
	// Tor blocks rotted in place for a long time: woolsack edges whatever the rock (at least half rounded).
	const float round_eff = MIN(1.0f, MAX(roundness + spheroidal * (1.0f - roundness), family == FAMILY_CORESTONE ? 0.55f : 0.0f));
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
		coarse.init(half_box, h_flow);
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
	// Coast forms, carved into the rounded mass (Trenhaile 1987; Sunamura 1992; Paris et al. 2020 build cliffs,
	// arches and stacks the same way, from a jointed volume and implicit cuts):
	// - beds: alternate harder and softer beds; the softer ones weather back, so a stack or arch is banded in ledges
	//   and recesses (strong in sedimentary rock, faint in granite and basalt, whose sheeting joints do the same);
	// - stack: the wave-cut notch where the sea works the foot (about a fifth up the column, at the waterline when the
	//   field stands it), a taper upward, and one or two open vertical joints down its sides;
	// - arch: the opening the waves cut through a headland along a joint, from the bed up through most of its height;
	// - platform: the bench cut flush at the cliff foot, crossed by two joint sets opened into grooves, with pools.
	if (coast_form) {
		const float layered = (rock_type == ROCK_GRANITE || rock_type == ROCK_BASALT) ? 0.45f : 1.0f;
		const int bed_axis = family == FAMILY_SEA_STACK ? 0 : 1;
		// Beds of uneven thickness (Paris et al. 2020; Trenhaile 1987): boundaries at random intervals, each bed hard
		// (it stands out, its underside an overhang) or soft (weathered back into a recess). The step between two beds
		// is one cell wide, so a ledge has an arris; a field offset smoothed over a bed read as stacked cushions.
		LocalVector<float> bed_at, bed_rec;
		if (family != FAMILY_SEA_PLATFORM) {
			const float mean = (family == FAMILY_SEA_STACK ? 0.42f : 0.3f) * (layered < 1.0f ? 1.5f : 1.0f);
			const float lo = -extent[bed_axis] - 0.2f, hi = extent[bed_axis] + 0.2f;
			float b = lo - rng.randf() * mean;
			bed_at.push_back(b);
			while (b < hi) {
				const float u = rng.randf();
				b += mean * (0.2f + 2.4f * u * u * u); // many thin beds, a few very thick ones
				bed_at.push_back(b);
			}
			bool prev_soft = false;
			for (uint32_t k = 0; k + 1 < bed_at.size(); k++) {
				// Softer beds rarely follow each other: a hard bed between keeps a ledge.
				const bool soft = rng.randf() < (prev_soft ? 0.2f : 0.5f);
				bed_rec.push_back((soft ? 0.12f + 0.12f * rng.randf() : 0.02f * rng.randf()) * c * layered);
				prev_soft = soft;
			}
		}
		const Vector3 dip = Vector3(0.0f, 0.0f, (rng.randf() - 0.5f) * 0.12f) + (bed_axis == 0 ? Vector3(0, (rng.randf() - 0.5f) * 0.12f, 0) : Vector3((rng.randf() - 0.5f) * 0.12f, 0, 0));
		const float step_w = 0.75f * h;
		// The recess at p: its bed's, eased into the neighbours' over a cell, varying along the bed, and hard beds
		// broken back here and there where a block fell out of the lip.
		auto bed_recess = [&](const Vector3 &p_p) -> float {
			const float q = p_p[bed_axis] + dip.dot(p_p) + 0.06f * (rk_noise(p_p * 0.9f, nseed + 92u) - 0.5f);
			int i = 0;
			while (i + 2 < int(bed_at.size()) && bed_at[i + 1] <= q) {
				i++;
			}
			const float r_lo = i > 0 ? bed_rec[i - 1] : bed_rec[i];
			const float r_hi = i + 1 < int(bed_rec.size()) ? bed_rec[i + 1] : bed_rec[i];
			float r = bed_rec[i];
			r += (r_lo - r) * (1.0f - Math::smoothstep(-step_w, step_w, q - bed_at[i]));
			r += (r_hi - r) * Math::smoothstep(-step_w, step_w, q - bed_at[i + 1]);
			const float along = rk_noise(p_p * 0.8f + Vector3(0, 0, float(i) * 7.3f), nseed + 93u);
			const float fallen = Math::smoothstep(0.62f, 0.65f, rk_noise(p_p * 0.55f + Vector3(float(i) * 3.1f, 0, 0), nseed + 94u));
			return r * (0.3f + 1.4f * along) + 0.14f * c * layered * fallen;
		};
		// Stack: the notch a fifth of the way up the solid as it stands after the cuts (not the grid's box).
		float x_lo = 1e9f, x_hi = -1e9f;
		for (int z = 0; z < fine.nz; z++) {
			for (int y = 0; y < fine.ny; y++) {
				for (int x = 0; x < fine.nx; x++) {
					if (fine.v[fine.idx(x, y, z)] < 0.0f) {
						const float px = fine.pos(x, y, z).x;
						x_lo = MIN(x_lo, px);
						x_hi = MAX(x_hi, px);
					}
				}
			}
		}
		const float notch_x = x_lo + 0.22f * (x_hi - x_lo);
		const float notch_w = 0.08f * a, notch_d = 0.18f * c;
		// Open joints: planar slots down the stack from its top (unloading opens them there first), a dark gap with
		// flat walls, not a groove; they close at the tip.
		struct Joint {
			Vector3 n;
			float d, w, depth, x0;
		};
		LocalVector<Joint> joints;
		if (family == FAMILY_SEA_STACK) {
			// Not every stack has one open, and it is rarely through the middle or quite upright.
			const int count = rng.randf() < 0.35f ? 0 : 1;
			for (int k = 0; k < count; k++) {
				const float th = rng.randf() * float(Math::TAU);
				const Vector3 n = Vector3((rng.randf() - 0.5f) * 0.3f, Math::cos(th), Math::sin(th)).normalized();
				const float off = (0.3f + 0.35f * rng.randf()) * (rng.randf() < 0.5f ? -1.0f : 1.0f) * Math::sqrt(c * c * n.y * n.y + n.z * n.z);
				joints.push_back({ n, off, MAX(0.06f * c, 2.5f * h), 0.5f * c, x_lo + (0.4f + 0.35f * rng.randf()) * (x_hi - x_lo) });
			}
		}
		// Arch: the opening follows the rock's planes: walls along a joint, nearly upright; the roof a bed's underside,
		// stepping down a bed toward the walls where the roof's blocks have not fallen yet.
		const float tx0 = (rng.randf() - 0.5f) * 0.3f * a, trx = (0.3f + 0.1f * rng.randf()) * a; // legs about as wide as the opening
		float roof = -extent.y + (1.5f + 0.2f * rng.randf()) * c;
		if (family == FAMILY_SEA_ARCH && bed_at.size() > 2) {
			uint32_t best = 1;
			for (uint32_t k = 1; k < bed_at.size(); k++) {
				if (Math::abs(bed_at[k] - roof) < Math::abs(bed_at[best] - roof)) {
					best = k;
				}
			}
			roof = bed_at[best];
		}
		const float arch_flare = 0.45f + 0.4f * rng.randf(), arch_skew = (rng.randf() - 0.5f) * 0.5f;
		const float arch_sag = (0.25f + 0.25f * rng.randf()) * MAX(extent.y - roof, 0.0f), arch_dip = (rng.randf() - 0.5f) * 0.2f;
		// Platform: joint sets at 60 to 90 degrees, a few pools.
		const float js = 0.5f + 0.25f * rng.randf(); // 3 to 5 m apart on a platform of 30 to 40 m
		const float th1 = rng.randf() * float(Math::PI), th2 = th1 + float(Math::PI) * (0.33f + 0.17f * rng.randf());
		const Vector3 j1(Math::cos(th1), 0, Math::sin(th1)), j2(Math::cos(th2), 0, Math::sin(th2));
		LocalVector<RockScoop> pools;
		if (family == FAMILY_SEA_PLATFORM) {
			const int count = 3 + int(rng.randf() * 4.0f);
			for (int k = 0; k < count; k++) {
				const float r = 0.25f + 0.35f * rng.randf();
				const float depth = (0.25f + 0.25f * rng.randf()) * c;
				pools.push_back({ Vector3((rng.randf() - 0.5f) * 1.6f * a, extent.y + r - depth, (rng.randf() - 0.5f) * 1.4f), r });
			}
		}
		const float near = 0.5f * c;
		for (int z = 0; z < fine.nz; z++) {
			for (int y = 0; y < fine.ny; y++) {
				for (int x = 0; x < fine.nx; x++) {
					const Vector3 p = fine.pos(x, y, z);
					float &v = fine.v[fine.idx(x, y, z)];
					if (v > near) {
						continue; // far outside: nothing here reaches the surface
					}
					const float v0 = v;
					if (!bed_rec.is_empty()) {
						// A recent fall's face keeps its beds flush: they have not had time to weather back.
						bool fr = false;
						if (v0 > -near) {
							shape(p, &fr);
						}
						float rec = bed_recess(p) * (fr ? 0.25f : 1.0f);
						if (family == FAMILY_SEA_STACK) {
							// Below the notch's roof the waves plane the beds back alike: no lips there to read as a plinth.
							rec = Math::lerp(0.2f * c * layered, rec, Math::smoothstep(notch_x + 0.6f * notch_w, notch_x + notch_w, p.x));
						}
						v += rec;
					}
					if (family != FAMILY_SEA_PLATFORM) {
						// No joint face is a plane at this size: blocks a metre or so across stand proud or sit back
						// by a hand's breadth (quantised noise, so the facets meet in edges), with a lumpy face on them.
						const float blk = rk_noise(p * 1.6f, nseed + 101u) * 4.0f;
						const float bi = Math::floor(blk);
						const float bq = (bi + Math::smoothstep(0.85f, 1.0f, blk - bi)) / 4.0f;
						v += 0.07f * c * (bq - 0.5f) + 0.05f * c * (rk_fbm(p * 2.4f, nseed + 102u, 3) - 0.5f);
					}
					if (family == FAMILY_SEA_STACK) {
						// The notch: a sloping floor up to a sharp roof (the visor), and below it the foot stays worn
						// back, so the column does not widen into a plinth under the water.
						const float t = (p.x - notch_x) / notch_w;
						const float prof = t > 1.0f ? 0.0f : (t > 0.75f ? 1.0f - (t - 0.75f) / 0.25f : (t > -1.5f ? 0.55f + 0.45f * (t + 1.5f) / 2.25f : 0.55f));
						v += notch_d * prof * (0.85f + 0.3f * rk_noise(p * 2.0f, nseed + 95u));
						for (const Joint &jt : joints) {
							const float off = jt.n.dot(p) - jt.d - 0.03f * c * (rk_noise(p * 1.7f, nseed + 97u) - 0.5f);
							const float tip = CLAMP((p.x - jt.x0) / (0.25f * (x_hi - x_lo)), 0.0f, 1.0f);
							const float slot = 0.5f * jt.w * Math::sqrt(tip) - Math::abs(off);
							v = MAX(v, MIN(MIN(slot, p.x - jt.x0), v0 + jt.depth));
						}
					} else if (family == FAMILY_SEA_ARCH) {
						// The opening (Durdle Door, Old Harry's arch): widest at the waterline where the waves work, closing
						// to a rounded, leaning head; above the springing its outline steps where roof blocks fell out
						// (a fifth of the rock's thickness each), its edges ragged.
						const float span_y = MAX(roof + extent.y, 0.01f);
						const float yb = (p.y + extent.y) / span_y;
						const float stq = 0.2f * c / span_y;
						const float yq = yb > 0.45f ? (Math::floor(yb / stq) + Math::smoothstep(0.88f, 1.0f, yb / stq - Math::floor(yb / stq))) * stq : yb;
						const float lo = 1.0f - CLAMP(yq, 0.0f, 1.0f);
						float half_w = trx * (1.0f + arch_flare * lo * lo) * Math::sqrt(MAX(1.0f - yq * yq * yq, 0.0f));
						half_w += 0.12f * trx * (rk_noise(p * 1.3f, nseed + 99u) - 0.5f) + 0.05f * trx * (rk_noise(p * 4.0f, nseed + 104u) - 0.5f);
						const float tun = Math::abs(p.x - tx0 - arch_skew * CLAMP(yb, 0.0f, 1.0f) * trx) - half_w;
						if (yq < 1.0f) {
							v = MAX(v, -tun);
						}
						// The deck thins and sags over the span, and dips along the ridge.
						const float ud = (p.x - tx0) / (1.7f * trx);
						const float deck = MAX(extent.y * 0.98f - arch_sag * MAX(1.0f - ud * ud, 0.0f) - arch_dip * p.x, roof + 0.4f * c); // never thinner than 0.4c
						v = MAX(v, p.y - deck);
					} else {
						// Joints: grooves cut down from the top a little way, their lines wandering.
						const float top = Math::smoothstep(-0.4f * c, 0.6f * c, p.y);
						const float wob = 0.12f * (rk_noise(p * 1.5f, nseed + 97u) - 0.5f);
						int set = 0;
						for (const Vector3 &jn : { j1, j2 }) {
							const float q = jn.dot(p) / js + wob;
							const float d = Math::abs(q - Math::round(q)) * js;
							const float g = MAX(0.0f, 1.0f - d / 0.07f); // at least two cells wide, or the groove beads
							// Not every joint runs through: the cross set stops and starts.
							const float run = set == 0 ? 1.0f : Math::smoothstep(0.4f, 0.6f, rk_noise(Vector3(Math::round(q) * 3.1f, jn.cross(Vector3(0, 1, 0)).dot(p) * 1.2f, 0.0f), nseed + 98u));
							v += 0.5f * c * g * g * top * run;
							set++;
						}
						// Low ledges stepping down seaward (+z), each a bed's edge, wandering.
						const float sq = (p.z + 0.25f * (rk_noise(p * 0.8f, nseed + 96u) - 0.5f)) / 0.45f;
						const float si = Math::floor(sq);
						v += 0.35f * c * (si + Math::smoothstep(0.85f, 1.0f, sq - si)) * Math::smoothstep(0.0f, 1.0f, p.y / c + 1.0f) * 0.5f;
						for (const RockScoop &pl : pools) {
							v = MAX(v, pl.r - (p - pl.c).length());
						}
					}
				}
			}
		}
	}
	// Landforms (docs/ideas/rocks.md, round 3):
	// - hoodoo: soft bedded rock beaded by its softer beds (deep, uneven recesses), narrowing to a neck under a harder
	//   caprock that stands wider and overhangs (Bryce Canyon; frost wedging opens the joints that free the column);
	// - pavement: a limestone slab dissolved along two joint sets into clints between deep, narrow grykes, the clint
	//   edges rounded by solution, shallow pans (kamenitza) on their tops (Williams 1966; Goldie 2005).
	struct Pothole {
		float x, z, r, depth;
	};
	LocalVector<Pothole> potholes; // a pothole slab's, in the grid's frame (kept for the mesh meta)
	float y_top_mesh = 0.0f;
	AABB cave_box; // a cave mouth's passage, in the grid's frame (kept for the mesh meta: the terrain hole)
	if (landform) {
		float x_lo = 1e9f, x_hi = -1e9f, y_top = -1e9f, z_lo = 1e9f, z_hi = -1e9f;
		for (int z = 0; z < fine.nz; z++) {
			for (int y = 0; y < fine.ny; y++) {
				for (int x = 0; x < fine.nx; x++) {
					if (fine.v[fine.idx(x, y, z)] < 0.0f) {
						const Vector3 q = fine.pos(x, y, z);
						x_lo = MIN(x_lo, q.x);
						x_hi = MAX(x_hi, q.x);
						y_top = MAX(y_top, q.y);
						z_lo = MIN(z_lo, q.z);
						z_hi = MAX(z_hi, q.z);
					}
				}
			}
		}
		const float span = MAX(x_hi - x_lo, 0.01f);
		// Hoodoo profile (Bryce, Drumheller): a few bulges and necks up the column where harder and softer beds alternate,
		// each a smooth swell rather than a step, since rain wash rounds them; fine bedding over it only as faint lines.
		LocalVector<float> knot_at, knot_rec;
		const float cap_t = 0.87f + 0.07f * rng.randf(); // where the caprock starts, as a share of the height: a thin cap
		if (family == FAMILY_HOODOO) {
			float b = x_lo - 0.3f;
			bool soft = rng.randf() < 0.5f;
			while (b < x_hi + 0.6f) {
				knot_at.push_back(b);
				knot_rec.push_back((soft ? 0.12f + 0.14f * rng.randf() : 0.03f * rng.randf()) * c);
				soft = rng.randf() < 0.75f ? !soft : soft;
				b += 0.45f + 0.5f * rng.randf();
			}
		}
		// Fin (Arches, Entrada and Navajo sandstone): a wall between two joints, its top and ends rounded by sheet wash,
		// beds etched across it; a weak bed near the foot hollows into an alcove, and in about half of them the alcove
		// has broken through into an arch whose roof follows the stress arch (Ostanin et al. 2017): a smooth
		// catenary-like head, the opening flaring where it meets the faces.
		const float fh = MAX(z_hi - z_lo, 0.01f);
		const bool fin_arch = family == FAMILY_FIN && rng.randf() < 0.55f;
		const float arch_w = (0.25f + 0.2f * rng.randf()) * a; // half width at the floor
		const float arch_x = (rng.randf() - 0.5f) * 2.0f * MAX(0.0f, 0.75f * a - arch_w);
		const float arch_floor = z_lo + (0.04f + 0.2f * rng.randf()) * fh;
		const float arch_h = MIN((0.35f + 0.25f * rng.randf()) * fh, z_hi - 0.22f * fh - arch_floor);
		const float arch_k = 1.2f + 1.5f * rng.randf(); // how pointed the head: low k a round arch, high k a tall one
		const float alcove_z = arch_floor + 0.3f * arch_h, alcove_depth = (0.25f + 0.3f * rng.randf()) * c;
		const float alcove_side = rng.randf() < 0.5f ? -1.0f : 1.0f;
		const float fin_drop = 0.15f + 0.35f * rng.randf(); // how far the crest falls toward the ends
		const float arch_lean = (rng.randf() - 0.5f) * 0.5f;
		LocalVector<float> fin_bed_at, fin_bed_rec;
		if (family == FAMILY_FIN) {
			for (float b = z_lo - 0.1f; b < z_hi + 0.3f; b += 0.12f + 0.25f * rng.randf()) {
				fin_bed_at.push_back(b);
				fin_bed_rec.push_back((rng.randf() < 0.4f ? 0.05f + 0.1f * rng.randf() : 0.015f * rng.randf()) * c);
			}
		}
		// Potholes (Richardson and Carling 2005): round, steep-walled hollows drilled into a bedrock bed by grinders whirled
		// in the flow, a little undercut, round-floored, in a line down the thalweg and sometimes breached into each
		// other; the bed between them polished and fluted along the flow, its upstream edge rounded.
		y_top_mesh = y_top;
		const float goblin_head = 0.72f + 0.14f * rng.randf(), goblin_neck = goblin_head - 0.26f - 0.08f * rng.randf();
		const float goblin_pinch = 0.22f + 0.2f * rng.randf();
		// Cave mouth (Ford and Williams 2007): a passage opened along a bedding plane or a joint where the cliff foot
		// cuts it; the roof bedding-flat or arched, the passage narrowing and turning a little as it goes in, so the
		// back is out of sight; a hood of the wall over the entrance.
		float cave_w = (0.32f + 0.18f * rng.randf()) * a, cave_x = (rng.randf() - 0.5f) * 0.4f * a;
		const float cave_floor = z_lo + 0.06f * (z_hi - z_lo), cave_h = (0.38f + 0.2f * rng.randf()) * (z_hi - z_lo);
		const bool cave_flat = rng.randf() < 0.5f; // a bedding-plane roof, else an arched one
		float cave_turn = (rng.randf() - 0.5f) * 0.8f; // how far the passage swings sideways per unit in
		const float cave_skew = (rng.randf() - 0.5f) * 0.6f; // the opening leaning along a joint
		if (family == FAMILY_CAVE_MOUTH) {
			// The passage stays inside the block's full-thickness middle (the sides round back past 0.55 of the half
			// span), so both walls and the floor's edges are rock, never a floor plate with a side open to the hill.
			const float xm = 0.5f * (x_lo + x_hi), room = 0.3f * span;
			// The widest the passage gets from its centre: 1.075 w with its wobble, plus 0.3 w of skew.
			if (1.4f * cave_w > 0.8f * room) {
				cave_w = 0.8f * room / 1.4f;
			}
			const float spare = MAX(room - 1.4f * cave_w, 0.0f);
			cave_x = xm + CLAMP(cave_x, -0.5f * spare, 0.5f * spare);
			const float d_end = 1.6f * c;
			const float swing_max = MAX(spare - Math::abs(cave_x - xm), 0.0f);
			cave_turn = CLAMP(cave_turn, -swing_max / (d_end * d_end), swing_max / (d_end * d_end));
		}
		float y_front = -1e9f;
		if (family == FAMILY_CAVE_MOUTH) {
			for (int z = 0; z < fine.nz; z++) {
				for (int y = 0; y < fine.ny; y++) {
					for (int x = 0; x < fine.nx; x++) {
						if (fine.v[fine.idx(x, y, z)] < 0.0f) {
							y_front = MAX(y_front, fine.pos(x, y, z).y);
						}
					}
				}
			}
		}
		if (family == FAMILY_CAVE_MOUTH) {
			// The passage's bounds: its centre swings by cave_turn d^2 over the 1.6c it goes in, it is at most 1.08 of
			// cave_w wide, the roof at most cave_h (plus its roughness) over the floor, from the front in.
			const float d_end = 1.6f * c;
			const float swing = cave_turn * d_end * d_end;
			const float w = cave_w * 1.05f;
			const float lean = 0.5f * Math::abs(cave_skew) * w;
			const float x0 = cave_x + MIN(0.0f, swing) - w - lean, x1 = cave_x + MAX(0.0f, swing) + w + lean;
			const float below = 0.04f * (z_hi - z_lo); // down into the floor's rock, so no ground shows through the floor
			cave_box = AABB(Vector3(x0, y_front - d_end, cave_floor - below), Vector3(x1 - x0, d_end, cave_h * 1.05f + 0.03f * (z_hi - z_lo) + below));
		}
		if (family == FAMILY_POTHOLE) {
			const int count = 1 + int(rng.randf() * 2.99f);
			float px = -0.7f * a + 0.4f * a * rng.randf();
			const float pz = (rng.randf() - 0.5f) * 0.6f;
			for (int k = 0; k < count && px < 0.8f * a; k++) {
				const float r = 0.14f + 0.36f * rng.randf() * rng.randf() + 0.08f * rng.randf();
				const float depth = MIN(r * (0.8f + 1.4f * rng.randf()), 1.7f * c);
				potholes.push_back({ px + r, pz + (rng.randf() - 0.5f) * 0.25f, r, depth });
				px += 2.0f * r * (0.75f + 0.8f * rng.randf()); // under 1: breached into the next
			}
		}
		// Pavement: two joint sets 60 to 90 degrees apart, clints 0.5 to 3 m (at 4 m a unit, 0.12 to 0.75), grykes
		// 0.1 to 0.5 m wide (at least two and a half cells) and most of the slab deep; pans on the tops.
		const float js1 = 0.2f + 0.25f * rng.randf(), js2 = js1 * (1.4f + 1.4f * rng.randf()); // the cross set wider apart
		const float th1 = rng.randf() * float(Math::PI), th2 = th1 + float(Math::PI) * (0.35f + 0.15f * rng.randf());
		const Vector3 j1(Math::cos(th1), 0, Math::sin(th1)), j2(Math::cos(th2), 0, Math::sin(th2));
		const float gw = MAX(0.025f, 2.5f * h);
		LocalVector<RockScoop> pans;
		if (family == FAMILY_PAVEMENT) {
			const int count = 6 + int(rng.randf() * 8.0f);
			for (int k = 0; k < count; k++) {
				const float r = 0.08f + 0.12f * rng.randf();
				const float depth = (0.15f + 0.2f * rng.randf()) * c;
				const float big = r * 2.5f; // flat-floored: a large shallow sphere
				pans.push_back({ Vector3((rng.randf() - 0.5f) * 1.8f * a, y_top + big - depth, (rng.randf() - 0.5f) * 1.8f), big });
			}
		}
		for (int z = 0; z < fine.nz; z++) {
			for (int y = 0; y < fine.ny; y++) {
				for (int x = 0; x < fine.nx; x++) {
					const Vector3 p = fine.pos(x, y, z);
					float &v = fine.v[fine.idx(x, y, z)];
					if (v > 0.5f * c) {
						continue;
					}
					const float v0 = v;
					if (family == FAMILY_HOODOO) {
						const float t = (p.x - x_lo) / span;
						const float q = p.x + 0.12f * (rk_noise(p * 0.9f, nseed + 111u) - 0.5f); // beds not quite level
						int i = 0;
						while (i + 2 < int(knot_at.size()) && knot_at[i + 1] <= q) {
							i++;
						}
						const float f = Math::smoothstep(knot_at[i], knot_at[i + 1], q);
						const float r = knot_rec[i] + (knot_rec[i + 1] - knot_rec[i]) * f;
						const float cap = Math::smoothstep(cap_t - 0.02f, cap_t + 0.02f, t);
						// Swells below, none in the cap; the neck just under the cap thinnest; the cap a little wider.
						v += r * (0.8f + 0.4f * rk_noise(p * 1.2f, nseed + 112u)) * (1.0f - cap);
						v += 0.1f * c * Math::smoothstep(cap_t - 0.3f, cap_t - 0.03f, t) * (1.0f - cap);
						v += 0.14f * c * t * (1.0f - cap); // wider at the foot, where the debris apron feeds it
						// The cap barely wider than the neck's foot, its rim and top rounded off and uneven.
						v -= 0.02f * c * cap;
						const float dome = Math::smoothstep(cap_t + 0.01f, 1.02f, t);
						v += 0.18f * c * dome * dome;
						v += 0.08f * c * cap * (rk_fbm(p * 1.6f, nseed + 125u, 3) - 0.5f);
						// Faint bedding: thin hard ribs 0.06 to 0.1 apart standing out a little.
						const float bed = q / (0.07f + 0.02f * rk_noise(Vector3(p.x * 0.3f, 0.0f, 0.0f), nseed + 119u));
						v += 0.012f * c * (Math::abs(bed - Math::round(bed)) * 2.0f - 0.5f) * (1.0f - cap);
						// Rills: rain runs down the sides and fluted them, grooves along the column.
						v += 0.07f * c * (rk_noise(Vector3(p.x * 0.35f, p.y * 4.0f, p.z * 4.0f), nseed + 120u) - 0.5f);
						v += 0.05f * c * (rk_fbm(p * 2.2f, nseed + 113u, 3) - 0.5f);
					} else if (family == FAMILY_CAVE_MOUTH) {
						const float fh = MAX(z_hi - z_lo, 0.01f);
						const float zt = (p.z - z_lo) / fh;
						// The front leans back above the entrance (the cliff above), the hood overhanging the opening.
						const float into = y_front - p.y; // depth behind the front face
						v += 0.25f * c * Math::smoothstep(0.55f, 1.0f, zt) * Math::smoothstep(0.0f, 0.6f * c, -into + 0.6f * c);
						// The block's outline lost in the cliff: sides and top rounded back, the front bulging and hollowed
						// at the scale of metres, a few thicker beds standing out.
						const float xt = Math::abs(p.x - 0.5f * (x_lo + x_hi)) / MAX(0.5f * span, 0.01f);
						v += 0.6f * c * Math::pow(Math::smoothstep(0.55f, 1.0f, xt), 2.0f);
						v += 0.35f * c * Math::pow(Math::smoothstep(0.75f, 1.0f, zt), 2.0f);
						v += 0.22f * c * (rk_fbm(p * 0.7f, nseed + 160u, 3) - 0.5f);
						const float thick = p.z / 0.35f + 0.6f * rk_noise(p * 0.5f, nseed + 164u);
						v -= 0.02f * c * Math::smoothstep(0.7f, 0.95f, 0.5f + 0.5f * Math::sin(thick * float(Math::TAU)));
						// The passage: its centre swinging sideways as it goes in, narrowing, its roof flat or a cosh arch,
						// the floor rising a little to a choke of fallen blocks at the back.
						const float d_in = MAX(into, 0.0f);
						const float cx = cave_x + cave_turn * d_in * d_in;
						const float narrow = 1.0f - 0.35f * Math::smoothstep(0.0f, 1.5f * c, d_in);
						const float w = cave_w * narrow * (1.0f + 0.15f * (rk_noise(p * 1.2f, nseed + 161u) - 0.5f));
						const float u = (p.x - cx) / MAX(w, 0.01f) - cave_skew * (p.z - cave_floor) / MAX(cave_h, 0.01f);
						float roof;
						if (cave_flat) {
							roof = cave_floor + cave_h * narrow * (1.0f - 0.25f * Math::pow(MIN(Math::abs(u), 1.0f), 6.0f));
						} else {
							roof = cave_floor + cave_h * narrow * (Math::cosh(1.6f) - Math::cosh(1.6f * MIN(Math::abs(u), 1.0f))) / (Math::cosh(1.6f) - 1.0f);
						}
						roof += 0.05f * fh * (rk_fbm(p * 1.5f, nseed + 162u, 2) - 0.5f);
						const float floor_z = cave_floor + 0.15f * cave_h * Math::smoothstep(0.6f * c, 1.6f * c, d_in);
						float d = MAX(MAX(Math::abs(p.x - cx) - w, floor_z - p.z), p.z - roof);
						d = MAX(d, into - 1.6f * c); // a blind end, round
						// The lip rounded back where the passage meets the front.
						d -= 0.12f * c * Math::smoothstep(0.25f * c, 0.0f, into);
						v = MAX(v, -d);
						v += 0.06f * c * (rk_fbm(p * 1.8f, nseed + 163u, 3) - 0.5f);
						// Inside, the walls are not smooth: the roof spalled along joints and beds into angular scars
						// (breakdown, Ford and Williams 2007) and the walls dissolved into shallow hollows, rougher the
						// further in, where no weather has rounded them.
						const float inside = Math::smoothstep(0.0f, 0.35f * c, into) * Math::smoothstep(0.25f * c, 0.0f, Math::abs(d));
						if (inside > 0.0f) {
							const float scar = Math::abs(rk_fbm(p * 2.6f, nseed + 165u, 3) - 0.5f) * 2.0f; // creased: sharp edges
							v += inside * c * (0.07f * (scar - 0.5f) + 0.05f * (rk_fbm(p * 5.0f, nseed + 166u, 2) - 0.5f));
						}
					} else if (family == FAMILY_GOBLIN) {
						// Goblin (Entrada sandstone, Goblin Valley): a knob rounded by spheroidal weathering and sand blast,
						// a bulbous head on a narrower neck over a flared foot, faint beds; no joint face left flat.
						const float t = (p.x - x_lo) / span;
						const float head = Math::exp(-Math::pow((t - goblin_head) / 0.14f, 2.0f));
						const float neck = Math::exp(-Math::pow((t - goblin_neck) / 0.12f, 2.0f));
						v -= 0.2f * c * head;
						v += goblin_pinch * c * neck;
						v += 0.3f * c * Math::smoothstep(0.75f, 1.0f, t) * Math::smoothstep(0.75f, 1.0f, t); // the crown rounded
						v -= 0.08f * c * (1.0f - Math::smoothstep(0.0f, 0.2f, t)); // the foot flared into the floor
						// The joint faces rounded off: pulled toward a round section of the same reach.
						const float ell = (Vector2(p.y / c, p.z).length() - 0.95f) * 0.6f;
						v = v + (ell - v) * 0.3f;
						const float bed = p.x / 0.06f;
						v += 0.01f * c * (Math::abs(bed - Math::round(bed)) * 2.0f - 0.5f);
						v += 0.07f * c * (rk_fbm(p * 2.0f, nseed + 150u, 3) - 0.5f);
					} else if (family == FAMILY_POTHOLE) {
						const float top_w = Math::smoothstep(y_top - 0.5f * c, y_top, p.y);
						// Polished, fluted along the flow, the upstream edge rounded over.
						v += 0.04f * c * top_w * (rk_noise(Vector3(p.x * 0.5f, 0.0f, p.z * 7.0f), nseed + 140u) - 0.5f);
						v += 0.1f * c * top_w * (rk_fbm(p * 0.9f, nseed + 141u, 2) - 0.5f);
						v += 0.8f * c * Math::pow(Math::smoothstep(0.55f, 1.0f, -p.x / MAX(0.5f * span, 0.01f)), 2.0f) * top_w;
						// All round, the top rolls down into the bed: the exposure meets gravel and banks without a cut edge.
						const float edge = MAX(Math::abs(p.x - 0.5f * (x_lo + x_hi)) / MAX(0.5f * span, 0.01f), Math::abs(p.z - 0.5f * (z_lo + z_hi)) / MAX(0.5f * (z_hi - z_lo), 0.01f));
						v += 1.6f * c * Math::pow(Math::smoothstep(0.45f, 1.0f, edge), 2.0f) * Math::smoothstep(y_top - 1.6f * c, y_top, p.y);
						for (const Pothole &ph : potholes) {
							const float yb = y_top - ph.depth + ph.r * 0.7f; // the centre of the round floor
							const float t = CLAMP((y_top - p.y) / MAX(ph.depth, 0.01f), 0.0f, 1.0f);
							const float wall_r = ph.r * (1.0f + 0.12f * Math::sin(t * float(Math::PI))); // undercut
							const float dxz = Math::sqrt((p.x - ph.x) * (p.x - ph.x) + (p.z - ph.z) * (p.z - ph.z));
							float d = MIN(MAX(dxz - wall_r, yb - p.y), Vector3(p.x - ph.x, p.y - yb, p.z - ph.z).length() - wall_r);
							// The rim rounded, and the grinder's spiral grooves round the wall.
							d -= 0.3f * ph.r * Math::smoothstep(y_top - 0.35f * ph.r, y_top + 0.05f * ph.r, p.y);
							const float ang = Math::atan2(p.z - ph.z, p.x - ph.x);
							d -= 0.025f * ph.r * Math::sin(ang * 3.0f + p.y / ph.r * 9.0f) * Math::exp(-Math::pow((dxz - wall_r) / (0.12f * ph.r), 2.0f));
							v = MAX(v, -d);
						}
					} else if (family == FAMILY_FIN) {
						const float xt = Math::abs(p.x - 0.5f * (x_lo + x_hi)) / MAX(0.5f * span, 0.01f);
						// The top a whaleback: rounded across, falling away toward the ends, its crest line wandering up and
						// down along the fin; the ends rounded in plan.
						const float crest = z_hi - fh * (fin_drop * Math::pow(Math::smoothstep(0.25f, 1.0f, xt), 1.6f) + 0.12f * rk_fbm(Vector3(p.x * 0.6f, 0.0f, 0.0f), nseed + 134u, 3));
						const float round_top = Math::smoothstep(crest - 0.5f * fh, crest, p.z);
						v = MAX(v, (p.z - crest) + 0.25f * fh * round_top * round_top * Math::smoothstep(0.0f, 1.0f, Math::abs(p.y) / c));
						v += 0.6f * c * round_top * round_top;
						v += 0.6f * c * Math::pow(Math::smoothstep(0.6f, 1.0f, xt), 2.0f);
						// Beds etched across, smooth swells rather than steps.
						const float q = p.z + 0.06f * (rk_noise(p * 0.8f, nseed + 130u) - 0.5f);
						int i = 0;
						while (i + 2 < int(fin_bed_at.size()) && fin_bed_at[i + 1] <= q) {
							i++;
						}
						const float f = Math::smoothstep(fin_bed_at[i], fin_bed_at[i + 1], q);
						v += fin_bed_rec[i] + (fin_bed_rec[i + 1] - fin_bed_rec[i]) * f;
						// The weak bed near the foot: an alcove under an overhang on one face, along part of the fin.
						const float along = Math::abs(p.x - arch_x) / (arch_w * 1.6f);
						v += alcove_depth * MAX(0.0f, 1.0f - along * along) * Math::exp(-Math::pow((p.z - alcove_z) / (0.15f * fh), 2.0f)) * Math::smoothstep(-0.2f * c, 0.6f * c, p.y * alcove_side);
						if (fin_arch) {
							// The opening: its walls at +-w(z), the head a cosh curve from the floor's width to the crown.
							const float u = (p.x - arch_x) / arch_w;
							const float ch = (Math::cosh(arch_k) - Math::cosh(arch_k * MIN(Math::abs(u), 1.0f))) / (Math::cosh(arch_k) - 1.0f);
							const float head = arch_floor + arch_h * ch + 0.1f * fh * (rk_fbm(p * 1.1f, nseed + 131u, 2) - 0.5f);
							// One wall leans; both wander where blocks have fallen out of them.
							const float zl = (p.z - arch_floor) / MAX(arch_h, 0.01f);
							const float wall = arch_w * (1.0f + arch_lean * zl * SIGN(p.x - arch_x)) + 0.12f * arch_w * (rk_fbm(p * 1.4f, nseed + 135u, 2) - 0.5f);
							float d = MAX(MAX(Math::abs(p.x - arch_x) - wall, arch_floor - p.z), p.z - head);
							// Wider where it meets the faces: the rim rounded back, not a drilled hole.
							d -= 0.6f * c * Math::smoothstep(0.1f * c, 1.1f * c, Math::abs(p.y));
							v = MAX(v, -d);
						}
						// Rills down the sides and sheet-wash pitting.
						v += 0.05f * c * (rk_noise(Vector3(p.x * 4.0f, p.y * 4.0f, p.z * 0.4f), nseed + 132u) - 0.5f);
						v += 0.06f * c * (rk_fbm(p * 1.8f, nseed + 133u, 3) - 0.5f);
					} else {
						const float top = p.y - (y_top - 0.9f * 2.0f * c); // grykes most of the way down
						const float wob = 0.06f * (rk_noise(p * 1.7f, nseed + 114u) - 0.5f) + 0.25f * (rk_noise(p * 0.6f, nseed + 118u) - 0.5f) * js1; // joints wander and their spacing varies
						int set = 0;
						for (const Vector3 &jn : { j1, j2 }) {
							const float js = set == 0 ? js1 : js2;
							// Each joint wanders on its own and the spacing swings by half either way, so clints differ in width.
							const float qq = (jn.dot(p) + wob + 0.45f * js * (rk_noise(p * (0.5f / js), nseed + 121u + set) - 0.5f) * 2.0f) / js;
							const float d = Math::abs(qq - Math::round(qq)) * js;
							// The cross set stops and starts: clints longer one way.
							const float run = set == 0 ? 1.0f : Math::smoothstep(0.35f, 0.45f, rk_noise(Vector3(Math::round(qq) * 3.7f, jn.cross(Vector3(0, 1, 0)).dot(p) * 1.5f, 0.0f), nseed + 115u)) * (rk_noise(p * 0.8f, nseed + 122u) > 0.4f ? 1.0f : 0.0f);
							const float w = gw * (0.8f + 0.6f * rk_noise(p * 2.3f, nseed + 116u + set));
							if (run > 0.5f) {
								v = MAX(v, MIN(MIN(0.5f * w - d, top), v0 + 2.0f * c));
							}
							// Solution rounds the clint's edge into the gryke.
							v += 0.12f * c * MAX(0.0f, 1.0f - d / (2.0f * w)) * run * Math::smoothstep(y_top - 0.5f * c, y_top, p.y);
							set++;
						}
						// Rillenkarren and runnels: shallow grooves down the clint tops, and the tops a little domed.
						const float top_w = Math::smoothstep(y_top - 0.4f * c, y_top, p.y);
						v += 0.05f * c * top_w * (rk_noise(Vector3(p.x * 6.0f, 0.0f, p.z * 0.8f), nseed + 123u) - 0.5f);
						v += 0.08f * c * top_w * (rk_fbm(p * 0.9f, nseed + 124u, 2) - 0.5f);
						for (const RockScoop &pn : pans) {
							v = MAX(v, pn.r - (p - pn.c).length());
						}
						v += 0.04f * c * (rk_fbm(p * 3.0f, nseed + 117u, 3) - 0.5f);
					}
				}
			}
		}
	}
	// Where the surface is a fresh break (lighter, no rind, no lichen): from the fracture, worn away by the transport.
	RockGrid fresh_grid;
	fresh_grid.init(half_box, h_flow);
	for (int z = 0; z < fresh_grid.nz; z++) {
		for (int y = 0; y < fresh_grid.ny; y++) {
			for (int x = 0; x < fresh_grid.nx; x++) {
				bool fr = false;
				shape(fresh_grid.pos(x, y, z), &fr);
				fresh_grid.v[fresh_grid.idx(x, y, z)] = fr ? (1.0f - roundness) : 0.0f;
			}
		}
	}

	// The stone's own frame (rest pose: longest axis x, shortest y, b axis z, 2 units long), fixed here from the
	// field's zero crossings before the marks are carved, so a base shape and its close-up version share it exactly
	// and the marks can be laid out in the frame the shader sees (UV, UV2.x).
	AABB box;
	{
		bool any = false;
		const int strides[3] = { 1, fine.nx, fine.nx * fine.ny };
		for (int z = 0; z < fine.nz; z++) {
			for (int y = 0; y < fine.ny; y++) {
				for (int x = 0; x < fine.nx; x++) {
					const int i = fine.idx(x, y, z);
					const float a = fine.v[i];
					const int lim[3] = { x < fine.nx - 1, y < fine.ny - 1, z < fine.nz - 1 };
					for (int ax = 0; ax < 3; ax++) {
						if (!lim[ax]) {
							continue;
						}
						const float b = fine.v[i + strides[ax]];
						if ((a < 0.0f) != (b < 0.0f)) {
							Vector3 q = fine.pos(x, y, z);
							q[ax] += fine.h * a / (a - b);
							if (!any) {
								box = AABB(q, Vector3());
								any = true;
							} else {
								box.expand_to(q);
							}
						}
					}
				}
			}
		}
	}
	const Vector3 centre = box.get_center();
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
	auto to_local = [&](const Vector3 &p_p) {
		Vector3 q = permute((p_p - centre) * scale);
		if (odd) {
			q.z = -q.z;
		}
		return q;
	};
	auto dir_local = [&](const Vector3 &p_n) {
		Vector3 q = permute(p_n);
		if (odd) {
			q.z = -q.z;
		}
		return q;
	};

	// Tafoni (docs/ideas/rocks.md, "Next options" 1): cavernous hollows in the sides of sandstone, granite and
	// limestone, decimetres to metres across, where salt crystallises in the drying zone just inside the surface and
	// the grains fall out; a hollow keeps a hood (visor) over its opening because the case-hardened rind holds there
	// while the softer core behind it goes (Mustoe 1982; Huinink et al. 2004; Geosciences 13(6) 161, 2023). They are
	// largest and most crowded where sea spray salts the rock (salt) and on old, dry inland blocks (tors, hoodoos).
	// Carved here over the whole field (they are deeper than the weathering band): each a squashed sphere sunk into
	// the stone along a side direction, lower and shaded faces first, with smaller hollows nested in its back wall.
	float tafoni = 0.0f;
	{
		const float lith_t = rock_type == ROCK_SANDSTONE ? 1.0f : (rock_type == ROCK_GRANITE ? 0.8f : (rock_type == ROCK_LIMESTONE ? 0.5f : 0.0f));
		const bool old_inland = family == FAMILY_CORESTONE || family == FAMILY_HOODOO || family == FAMILY_GOBLIN || family == FAMILY_FIN;
		const float inland = old_inland ? 0.6f : 0.5f * Math::smoothstep(0.4f, 0.9f, weather);
		tafoni = lith_t * MAX(float(get_effective_salt()), inland) * float(detail);
		if (family == FAMILY_PEBBLE || family == FAMILY_COBBLE || family == FAMILY_ROCKFALL || family == FAMILY_SCREE || family == FAMILY_CLIFF_BLOCK) {
			tafoni = 0.0f;
		}
	}
	// The stone's up as the field stands it: a fin (and a cave-mouth block) on its z, a column (stack, hoodoo,
	// goblin) on its x, everything else as built.
	const bool z_up = family == FAMILY_FIN || family == FAMILY_CAVE_MOUTH;
	const bool x_up = family == FAMILY_SEA_STACK || family == FAMILY_HOODOO || family == FAMILY_GOBLIN;
	// In the grid: the axis that becomes the local x is ax_long, y ax_short, z ax_b (to_local above).
	const int up_axis = z_up ? ax_b : (x_up ? ax_long : ax_short);
	Vector3 t_up, t_a1;
	t_up[up_axis] = (z_up && odd) ? -1.0f : 1.0f; // an odd permutation flips the local z
	t_a1[(up_axis + 1) % 3] = 1.0f;
	const Vector3 t_a2 = t_up.cross(t_a1);
	if (tafoni > 0.05f) {
		RandomPCG trng(uint64_t(nseed) * 6364136223846793005ull + 1442695040888963407ull);
		const Vector3 c0 = centre;
		const float extent = 0.5f * MIN(box.size.x, MIN(box.size.y, box.size.z)) + 0.5f * 0.5f * MAX(box.size.x, MAX(box.size.y, box.size.z));
		const float up_len = box.size[up_axis];
		// Tafoni crowd: a face that dries in the wind (or the spray) carries a cluster of them, many small and a few
		// large (their sizes run roughly log-normal; Mustoe 1982), packed so that only thin walls and visors stand
		// between neighbours, and behind them they open into each other (cavernous weathering).
		const int target = CLAMP(int(Math::round(tafoni * (12.0f + 12.0f * trng.randf()))), 4, 26);
		const int faces = 1 + (trng.randf() < 0.55f ? 1 : 0);
		float face_az[2], face_hy[2];
		for (int f = 0; f < 2; f++) {
			face_az[f] = trng.randf() * float(Math::TAU);
			face_hy[f] = (-0.3f + 0.4f * trng.randf()) * up_len; // the lower half and the middle
		}
		LocalVector<RockScoop> hollows;
		LocalVector<float> squash;
		LocalVector<uint8_t> main_hollow;
		int placed = 0;
		for (int attempt = 0; attempt < target * 8 && placed < target; attempt++) {
			const int f = attempt % faces;
			// Round the cluster's middle: spread across the face and up and down it (the column's length).
			const float az = face_az[f] + (trng.randf() + trng.randf() + trng.randf() - 1.5f) * 1.2f;
			const float hy = CLAMP(face_hy[f] + (trng.randf() + trng.randf() - 1.0f) * 0.45f * up_len, -0.45f * up_len, 0.38f * up_len);
			const Vector3 o0 = c0 + t_up * hy;
			const Vector3 d = t_a1 * Math::cos(az) + t_a2 * Math::sin(az);
			float t = 0.0f;
			const float step = fine.h * 0.5f;
			while (t < 4.0f * extent && fine.sample(o0 + d * t) < 0.0f) {
				t += step;
			}
			if (t <= step || t >= 4.0f * extent) {
				continue;
			}
			const Vector3 s0 = o0 + d * t;
			const float u = trng.randf();
			const float R = ((x_up || z_up) ? MIN(t, 0.5f * up_len) : 1.7f * MIN(t, up_len)) * (0.07f + 0.25f * u * u) * (0.6f + 0.4f * MIN(tafoni, 1.0f));
			if (R < 2.5f * fine.h) {
				continue;
			}
			// Sunk past the surface by 0.55 R and raised a little: the opening narrower than the hollow, the hood on top.
			const Vector3 c = s0 - d * (0.55f * R) + t_up * (0.22f * R);
			// Thin walls: the openings (0.83 R across each) nearly touch, but no hollow swallows a neighbour.
			bool clash = false;
			for (uint32_t j = 0; j < hollows.size() && !clash; j++) {
				if (main_hollow[j]) {
					clash = c.distance_to(hollows[j].c) < 1.02f * (R + hollows[j].r); // a wall at least a few cells thick: thinner tears in the mesh
				}
			}
			if (clash) {
				continue;
			}
			placed++;
			hollows.push_back({ c, R });
			squash.push_back(0.72f + 0.15f * trng.randf());
			main_hollow.push_back(1);
			// Nested hollows in the back wall and roof of the bigger ones.
			const int nested = R > 5.0f * fine.h ? 1 + int(trng.randf() * 3.0f) : 0;
			for (int m = 0; m < nested; m++) {
				const Vector3 o = (t_a1 * (trng.randf() - 0.5f) + t_a2 * (trng.randf() - 0.5f) + t_up * (0.3f + 0.5f * trng.randf())) * R;
				const float r2 = R * (0.3f + 0.15f * trng.randf());
				if (r2 >= 2.0f * fine.h) {
					hollows.push_back({ c - d * (0.55f * R) + o * 0.8f, r2 });
					squash.push_back(0.8f);
					main_hollow.push_back(0);
				}
			}
		}
		// Each hollow over its own box of the grid only (scanning the whole grid per hollow costs seconds a stone).
		for (uint32_t k = 0; k < hollows.size(); k++) {
			const RockScoop &hw = hollows[k];
			const float reach = 1.27f * hw.r / squash[k] + fine.h;
			const Vector3 lo = (hw.c - Vector3(reach, reach, reach) - fine.origin) / fine.h;
			const Vector3 hi = (hw.c + Vector3(reach, reach, reach) - fine.origin) / fine.h;
			const int x0 = MAX(0, int(Math::floor(lo.x))), y0 = MAX(0, int(Math::floor(lo.y))), z0 = MAX(0, int(Math::floor(lo.z)));
			const int x1 = MIN(fine.nx - 1, int(Math::ceil(hi.x))), y1 = MIN(fine.ny - 1, int(Math::ceil(hi.y))), z1 = MIN(fine.nz - 1, int(Math::ceil(hi.z)));
			for (int z = z0; z <= z1; z++) {
				for (int y = y0; y <= y1; y++) {
					for (int x = x0; x <= x1; x++) {
						const Vector3 p = fine.pos(x, y, z);
						float &v = fine.v[fine.idx(x, y, z)];
						const Vector3 q0 = p - hw.c;
						const float qy = q0.dot(t_up);
						Vector3 q = q0 + t_up * (qy * (1.0f / squash[k] - 1.0f));
						if (q.length_squared() > 1.6f * hw.r * hw.r) {
							continue;
						}
						float r = hw.r * (1.0f + 0.12f * (rk_fbm(p * (3.0f / MAX(hw.r, 0.05f)), nseed + 151u + k, 2) - 0.5f) * 2.0f);
						if (qy > 0.0f) {
							// The roof flakes off in scales (granular disintegration and flaking; Mustoe 1982): shallow
							// dished scars with their rims hanging down between them.
							float f1, f2;
							uint32_t fid;
							rk_worley2(p / MAX(0.28f * hw.r, 1.5f * fine.h), nseed + 171u + k, f1, f2, fid);
							const float rim = 1.0f - Math::smoothstep(0.0f, 0.18f, f2 - f1);
							r -= 0.09f * hw.r * rim * Math::smoothstep(0.0f, 0.4f * hw.r, qy);
						}
						float carve = (r - q.length()) * squash[k];
						if (main_hollow[k]) {
							// The grains that fall out lie on its floor: a level, sandy bottom.
							carve = MIN(carve, qy + 0.42f * hw.r * squash[k]);
						}
						// A smooth union: the lips and visors come out rounded, a few cells thick, not knife edges that
						// the mesher tears into flaps.
						const float kb = MAX(0.25f * hw.r, 2.0f * fine.h);
						const float hk = MAX(kb - Math::abs(v - carve), 0.0f) / kb;
						v = MAX(v, carve) + hk * hk * kb * 0.25f;
					}
				}
			}
		}
	}

	// 3. Weathering and the rock's own marks, carved into the fine field near its surface.
	LocalVector<int> band;
	rk_band(fine, MAX(5.0f * h, 3.0f * hb), band);
	const float worn = 1.0f - 0.75f * roundness; // transport wears the marks away
	const bool fresh_family = family == FAMILY_ROCKFALL || family == FAMILY_SCREE || family == FAMILY_CLIFF_BLOCK;
	// Periodic marks finer than about five grid cells alias into combs and notches: no period below that.
	const float min_period = 5.0f * hb;
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
			const float wobble = 1.5f * hb * rk_fbm(p * 7.0f, nseed + 91u, 2);
			const float slab = cr.width * 0.5f * open - Math::abs(cr.n.dot(p) - cr.d - wobble);
			const float along = cr.half_length - Math::abs(cr.t.dot(p) - cr.t0);
			const float removed = MIN(MIN(slab, along), phi + cr.depth * open);
			phi = MAX(phi, removed);
		}
		// Grain-scale roughness (grus on granite, sand grains on sandstone).
		phi += lith.grain * float(detail) * 0.45f * hb * rk_fbm(p / (2.2f * hb), nseed, 2) * worn;
		// Lumps a few voxels wide: no rock face is a plane.
		phi += float(detail) * ((fresh_family || coast_form || landform) ? 0.45f : 1.1f) * hb * rk_fbm(p * 2.6f, nseed + 5u, 3) * (0.5f + 0.5f * worn);
		// Solution pits on the upper faces (karren on limestone; Ford and Williams 2007) or gas vesicles (basalt).
		if (lith.pits > 0.0f) {
			const bool karst = !lith.columnar;
			const float cell = karst ? 0.16f : 0.07f;
			const float wd = rk_worley(p / cell, nseed + 21u);
			const float pit = MAX(0.0f, 0.55f - wd) / 0.55f;
			const float mask = karst ? Math::smoothstep(0.2f, 0.7f, n.y) * weather : 0.6f;
			phi += lith.pits * float(detail) * mask * pit * pit * MAX(1.6f * hb, cell * 0.22f) * (karst ? worn : 1.0f);
		}
		// Bedding laminae etched out by weathering (differential erosion of the softer laminae), slightly cross-bedded.
		if (lith.laminae > 0.0f) {
			const float lam = MAX(0.07f, min_period);
			const float s = ((upright ? p.x + bed_tilt * p.z : ((family == FAMILY_FIN || family == FAMILY_CAVE_MOUTH) ? p.z + bed_tilt * p.x : p.y + bed_tilt * p.x)) + 0.02f * rk_fbm(p * 3.0f, nseed + 31u, 2)) / lam;
			const float groove = Math::pow(0.5f + 0.5f * Math::cos(s * float(Math::TAU)), 6.0f);
			// Laminae differ: most barely etched, a few soft ones weathered back deep.
			const float lamina = rk_hash(uint32_t(int(Math::floor(s + 0.5f)) + 4096) ^ nseed) / 4294967295.0f;
			const float strength = 0.25f + 0.75f * lamina * lamina;
			phi += lith.laminae * float(detail) * (0.3f + 0.7f * weather) * groove * strength * 1.1f * hb * (1.0f - Math::abs(upright ? n.x : ((family == FAMILY_FIN || family == FAMILY_CAVE_MOUTH) ? n.z : n.y))) * worn;
		}
		// Slaty cleavage: plates stepping back at the edges.
		if (lith.cleavage > 0.0f) {
			const float lam = MAX(0.05f, min_period);
			const float s = (p.y + 0.01f * rk_fbm(p * 4.0f, nseed + 41u, 2)) / lam;
			const float f = s - Math::floor(s);
			phi += lith.cleavage * float(detail) * f * 1.4f * hb * (1.0f - Math::abs(n.y)) * worn;
		}
		// Honeycomb weathering (alveoli): centimetre cells with thin walls between them, crowded on the faces of salted
		// and tafoni-hollowed rock, inside the hollows too (the cell walls are the harder, salt-cemented rims; Huinink
		// et al. 2004). Worley cells, carved inside and left standing along the cell boundaries (F2 - F1 small).
		if (tafoni > 0.05f) {
			const float mpu = float(get_family_metres_per_unit(family));
			const float cell = MAX(0.06f / MAX(mpu, 1e-3f), 1.3f * min_period);
			float f1, f2;
			uint32_t cid;
			rk_worley2(p / cell, nseed + 161u, f1, f2, cid);
			const float inner = Math::smoothstep(0.05f, 0.3f, f2 - f1);
			const float patch = Math::smoothstep(0.3f, 0.5f, rk_fbm(p * (0.6f / cell), nseed + 162u, 2) + 0.3f * MIN(tafoni, 1.0f));
			const float side = 1.0f - Math::smoothstep(0.55f, 0.85f, n.dot(t_up)); // not on tops that rain washes
			const float cdepth = 0.45f * cell * (0.7f + 0.6f * float(cid & 255u) / 255.0f);
			phi += MIN(tafoni, 1.0f) * inner * patch * side * cdepth * worn;
		}
		// Close-up: the rock's own millimetre fabric as geometry (crystals, grains, vesicles, karren, cleavage steps,
		// conchoidal ripples on fresh breaks), so the path tracer and raster see the same relief.
		if (close_up) {
			// In the stone's frame, in metres at the family's typical size: the same map the shader colours the
			// minerals from (UV2.y carries the metres per unit).
			const float mpu = float(get_marks_metres_per_unit());
			const float fr = CLAMP(fresh_grid.sample(p), 0.0f, 1.0f);
			const float to_units = 1.0f / (mpu * scale); // metres to field units
			phi -= rk_relief(rock_type, to_local(p) * mpu, dir_local(n), fr, h * scale * mpu, weather) * to_units * float(detail) * (0.5f + 0.5f * worn);
		}
		// Glacial striations: fine grooves on the faces the ice dragged over the bed, all one way.
		if (family == FAMILY_GLACIAL) {
			if (close_up) {
				// Up close, scratches about a millimetre deep, spaced unevenly, each running a few centimetres and
				// fading (striae are discontinuous; Benn and Evans 2010).
				const float mpu = float(get_marks_metres_per_unit());
				// Lanes at least eight cells apart so each groove is meshed, not aliased into a comb.
				const float spacing = MAX(0.007f, 8.0f * h * mpu);
				const float q = (striae_perp.dot(p) * mpu + 0.004f * rk_fbm(p * mpu / 0.05f, nseed + 61u, 2)) / spacing;
				const float lane = Math::floor(q);
				const float in_lane = q - lane;
				const float run = rk_noise(Vector3(striae_dir.dot(p) * mpu / 0.04f, lane * 1.7f, 0.0f), nseed + 72u);
				const float g = Math::pow(MAX(0.0f, 1.0f - Math::abs(in_lane - 0.5f) / 0.18f), 2.0f) * Math::smoothstep(0.55f, 0.75f, run) * Math::smoothstep(0.55f, 0.9f, Math::abs(n.y));
				phi += float(detail) * g * 0.0012f / mpu;
			} else {
				const float s = (striae_perp.dot(p) + 0.01f * rk_fbm(p * 5.0f, nseed + 61u, 2)) / MAX(2.6f * hb, 0.6f * min_period);
				const float g = Math::pow(0.5f + 0.5f * Math::cos(s * float(Math::TAU)), 10.0f) * Math::smoothstep(0.55f, 0.9f, Math::abs(n.y));
				phi += float(detail) * g * 0.9f * hb * (0.5f + 0.5f * rk_noise(p * 6.0f, nseed + 71u));
			}
		}
		fine.v[i] = phi;
	}
	rk_reinitialize(fine, band, 3);

	// 4. Mesh: surface nets (rk_surface_nets). 5. Normals (area-weighted from the faces); cavity and freshness below.
	LocalVector<Vector3> verts, normals;
	LocalVector<int> indices;
	ERR_FAIL_COND_V_MSG(!rk_surface_nets(fine, simplify, verts, normals, indices), Ref<ImporterMesh>(), "RockGenerator: the rock came out empty.");
	// Normalise the rock in the frame fixed above. p_solid: for a fracture piece, the whole stone's field; the faces
	// inside it are fresh breaks.
	auto make_arrays = [&](const RockGrid &p_field, const LocalVector<Vector3> &p_verts, const LocalVector<Vector3> &p_normals, const LocalVector<int> &p_indices, const RockGrid *p_solid) {
		PackedVector3Array out_verts, out_normals;
		PackedColorArray out_colors;
		PackedVector2Array out_uv, out_uv2;
		PackedFloat32Array out_tangents;
		out_verts.resize(p_verts.size());
		out_normals.resize(p_verts.size());
		out_colors.resize(p_verts.size());
		out_uv.resize(p_verts.size());
		out_uv2.resize(p_verts.size());
		out_tangents.resize(p_verts.size() * 4);
		// Where rain runs (docs/ideas/rocks.md, "Rain-wet rock"): the area of stone draining through each vertex, by
		// multiple-flow-direction routing down the mesh in the stone's stood-up frame (Freeman 1991; Quinn et al. 1991:
		// each vertex hands its water to every lower neighbour in proportion to the slope to it, to the power 1.1).
		// Sheet flow on the open faces gathers into rivulets along the converging lines, the grooves and the lips.
		// Stored as COLOR.a = 1 - log2(1 + upslope cells) / 12, so 1 (any mesh without it) means no flow.
		LocalVector<float> flow_acc;
		{
			const uint32_t nv = p_verts.size();
			flow_acc.resize(nv);
			LocalVector<int> deg, start, adj;
			deg.resize(nv + 1);
			for (uint32_t k = 0; k <= nv; k++) {
				deg[k] = 0;
			}
			for (uint32_t k = 0; k < nv; k++) {
				flow_acc[k] = 0.0f;
			}
			for (uint32_t t = 0; t + 2 < p_indices.size(); t += 3) {
				const int a = p_indices[t], b = p_indices[t + 1], c = p_indices[t + 2];
				const float area = 0.5f * (p_verts[b] - p_verts[a]).cross(p_verts[c] - p_verts[a]).length() / 3.0f;
				flow_acc[a] += area;
				flow_acc[b] += area;
				flow_acc[c] += area;
				deg[a] += 2;
				deg[b] += 2;
				deg[c] += 2;
			}
			start.resize(nv + 1);
			start[0] = 0;
			for (uint32_t k = 0; k < nv; k++) {
				start[k + 1] = start[k] + deg[k];
				deg[k] = start[k];
			}
			adj.resize(start[nv]);
			for (uint32_t t = 0; t + 2 < p_indices.size(); t += 3) {
				const int tri[3] = { p_indices[t], p_indices[t + 1], p_indices[t + 2] };
				for (int e = 0; e < 3; e++) {
					adj[deg[tri[e]]++] = tri[(e + 1) % 3];
					adj[deg[tri[e]]++] = tri[(e + 2) % 3];
				}
			}
			struct ByHeight {
				const float *hgt = nullptr;
				bool operator()(int p_a, int p_b) const { return hgt[p_a] > hgt[p_b]; }
			};
			LocalVector<float> hgt;
			LocalVector<int> order;
			hgt.resize(nv);
			order.resize(nv);
			for (uint32_t k = 0; k < nv; k++) {
				hgt[k] = p_verts[k].dot(t_up);
				order[k] = int(k);
			}
			SortArray<int, ByHeight> sorter;
			sorter.compare.hgt = hgt.ptr();
			sorter.sort(order.ptr(), nv);
			for (uint32_t o = 0; o < nv; o++) {
				const int v = order[o];
				float wsum = 0.0f;
				for (int j = start[v]; j < start[v + 1]; j++) {
					const float drop = hgt[v] - hgt[adj[j]];
					if (drop > 0.0f) {
						wsum += Math::pow(drop / MAX((p_verts[adj[j]] - p_verts[v]).length(), 1e-6f), 1.1f);
					}
				}
				if (wsum <= 0.0f) {
					continue; // a hollow's floor: the water stands there
				}
				const float share = flow_acc[v] / wsum;
				for (int j = start[v]; j < start[v + 1]; j++) {
					const float drop = hgt[v] - hgt[adj[j]];
					if (drop > 0.0f) {
						flow_acc[adj[j]] += share * Math::pow(drop / MAX((p_verts[adj[j]] - p_verts[v]).length(), 1e-6f), 1.1f);
					}
				}
			}
			const float cell = p_field.h * p_field.h;
			for (uint32_t k = 0; k < nv; k++) {
				flow_acc[k] = 1.0f - CLAMP(Math::log2(1.0f + flow_acc[k] / cell) / 12.0f, 0.0f, 1.0f);
			}
		}
		for (uint32_t k = 0; k < p_verts.size(); k++) {
			const Vector3 p = p_verts[k];
			Vector3 n = p_normals[k];
			n = n.length_squared() > 1e-20f ? n.normalized() : p_field.gradient(p);
			float occ = 0.0f;
			for (int s = 1; s <= 3; s++) {
				const float dist = p_field.h * float(1 << s);
				occ += MAX(0.0f, dist - p_field.sample(p + n * dist)) / dist / float(1 << (s - 1));
			}
			const float cavity = CLAMP(1.0f - occ * 0.9f, 0.0f, 1.0f);
			float fr = CLAMP(fresh_grid.sample(p), 0.0f, 1.0f);
			if (p_solid) {
				// A new break: fresh from a cell inside the old surface.
				fr = MAX(fr, CLAMP(-p_solid->sample(p) / p_solid->h - 0.5f, 0.0f, 1.0f));
			}
			const Vector3 q = to_local(p);
			n = dir_local(n);
			out_verts.write[k] = q;
			out_normals.write[k] = n;
			out_colors.write[k] = Color(cavity, fr, upright ? 1.0f : ((family == FAMILY_FIN || family == FAMILY_CAVE_MOUTH) ? 0.5f : 0.0f), flow_acc[k]); // b: the beds cross x (1), z (0.5) or y (0)
			out_uv.write[k] = Vector2(q.x, q.z);
			out_uv2.write[k] = Vector2(q.y, float(get_family_metres_per_unit(family)));
			const Vector3 t = (Math::abs(n.y) < 0.95f ? Vector3(0, 1, 0) : Vector3(1, 0, 0)).cross(n).normalized();
			out_tangents.write[k * 4 + 0] = t.x;
			out_tangents.write[k * 4 + 1] = t.y;
			out_tangents.write[k * 4 + 2] = t.z;
			out_tangents.write[k * 4 + 3] = 1.0f;
		}
		PackedInt32Array out_indices;
		out_indices.resize(p_indices.size());
		for (uint32_t k = 0; k < p_indices.size(); k++) {
			out_indices.write[k] = p_indices[k];
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
		return arrays;
	};
	const Array arrays = make_arrays(fine, verts, normals, indices, nullptr);
	const PackedVector3Array out_verts = arrays[Mesh::ARRAY_VERTEX];
	const PackedInt32Array out_indices = arrays[Mesh::ARRAY_INDEX];

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
	float bottom = 0.0f;
	for (int k = 0; k < out_verts.size(); k++) {
		bottom = MIN(bottom, out_verts[k].y);
	}
	r_meta["rock_bottom"] = bottom;
	r_meta["rock_hull"] = hull;
	r_meta["rock_faces"] = int(planes.size());
	r_meta["rock_roundness"] = roundness;
	r_meta["rock_type"] = int(rock_type);
	r_meta["rock_family"] = int(family);
	if (cave_box.has_volume()) {
		// The passage as a box in the stone's frame: the unit cube [-1, 1]^3 through this transform.
		const Vector3 o = to_local(cave_box.get_center());
		const Vector3 h = cave_box.size * 0.5f;
		const Basis b(to_local(cave_box.get_center() + Vector3(h.x, 0, 0)) - o, to_local(cave_box.get_center() + Vector3(0, h.y, 0)) - o, to_local(cave_box.get_center() + Vector3(0, 0, h.z)) - o);
		r_meta["rock_cave"] = Transform3D(b, o);
	}
	if (!potholes.is_empty()) {
		// The potholes' floors in the stone's frame (x, y, z, radius) for the field's grinder stones.
		PackedFloat32Array holes;
		for (const Pothole &ph : potholes) {
			const Vector3 q = to_local(Vector3(ph.x, y_top_mesh - ph.depth + 0.25f * ph.r, ph.z));
			holes.push_back(q.x);
			holes.push_back(q.y);
			holes.push_back(q.z);
			holes.push_back(ph.r * scale);
		}
		r_meta["rock_potholes"] = holes;
	}
	// 6. Fracture pieces for breaking the stone (Blast, through RockField3D.break_stone): the solid split into cells
	// whose walls are rough rock breaks, not Voronoi planes. The partition is a Voronoi diagram of sites spread through
	// the solid by best-candidate sampling (Mitchell 1991), taken in domain-warped space (the cell walls wander like a
	// real fracture surface; the same noisy-interior idea as Houdini's RBD Material Fracture and Chaos' noise on cuts),
	// stretched across the bedding of layered rock so they break into slabs along it. Each piece is the stone's field
	// cut by its cell, meshed like the stone, so the old surface keeps its marks and the walls are fresh breaks.
	if (fracture_pieces > 1) {
		const float h = fine.h;
		const bool bedded = rock_type == ROCK_SANDSTONE || rock_type == ROCK_SLATE || rock_type == ROCK_LIMESTONE;
		const Vector3 metric = bedded ? Vector3(1.0f, rock_type == ROCK_SLATE ? 2.6f : 1.8f, 1.0f) : Vector3(1, 1, 1);
		int solid_nodes = 0;
		for (uint32_t i = 0; i < fine.v.size(); i++) {
			solid_nodes += fine.v[i] < 0.0f ? 1 : 0;
		}
		const float spacing = Math::pow(float(solid_nodes) * h * h * h / float(fracture_pieces), 1.0f / 3.0f);
		// Gentle wander only: a fresh break is near planar at the scale of the piece (it runs along the stress), rough at
		// the scale of the grains; the roughness comes from rk_relief below, not from bulging walls.
		const float warp_amp = 0.09f * spacing, warp_freq = 1.3f / spacing;
		const uint32_t fseed = rk_hash(uint32_t(seed) * 7919u + 0xF2AC);
		auto warped = [&](const Vector3 &p_p) {
			const Vector3 q = p_p * warp_freq;
			const Vector3 w(rk_fbm(q, fseed + 1u, 3), rk_fbm(q, fseed + 2u, 3), rk_fbm(q, fseed + 3u, 3));
			return (p_p + w * warp_amp) * metric;
		};
		// Sites inside the solid, at least two cells in.
		RandomPCG frng(fseed);
		LocalVector<Vector3> sites;
		const Vector3 lo = fine.origin, span = Vector3(fine.nx - 1, fine.ny - 1, fine.nz - 1) * h;
		for (int s_i = 0; s_i < fracture_pieces; s_i++) {
			Vector3 best;
			float best_d = -1.0f;
			for (int c = 0, found = 0; c < 400 && found < 6; c++) {
				const Vector3 p = lo + Vector3(frng.randf(), frng.randf(), frng.randf()) * span;
				if (fine.sample(p) > -2.0f * h) {
					continue;
				}
				found++;
				const Vector3 wp = warped(p);
				float d = 1e30f;
				for (const Vector3 &o : sites) {
					d = MIN(d, wp.distance_squared_to(o));
				}
				if (d > best_d) {
					best_d = d;
					best = wp;
				}
			}
			if (best_d >= 0.0f) {
				sites.push_back(best);
			}
		}
		// Per node: nearest and second-nearest site, and the distance to their bisector (both cells read the same
		// wall: the field of one is the negative of the other's there).
		const uint32_t node_count = fine.v.size();
		LocalVector<uint8_t> n1, n2;
		LocalVector<float> wall;
		n1.resize(node_count);
		n2.resize(node_count);
		wall.resize(node_count);
		auto label = [&]() {
			for (int z = 0; z < fine.nz; z++) {
				for (int y = 0; y < fine.ny; y++) {
					for (int x = 0; x < fine.nx; x++) {
						const int i = fine.idx(x, y, z);
						if (fine.v[i] > 2.0f * h || sites.size() < 2) {
							n1[i] = n2[i] = 255;
							wall[i] = 0.0f;
							continue;
						}
						const Vector3 wp = warped(fine.pos(x, y, z));
						float d1 = 1e30f, d2 = 1e30f;
						int a = 0, b = 0;
						for (uint32_t k = 0; k < sites.size(); k++) {
							const float d = wp.distance_squared_to(sites[k]);
							if (d < d1) {
								d2 = d1;
								b = a;
								d1 = d;
								a = int(k);
							} else if (d < d2) {
								d2 = d;
								b = int(k);
							}
						}
						n1[i] = uint8_t(a);
						n2[i] = uint8_t(b);
						// Back to grid units roughly: the metric stretches distances across the bedding.
						wall[i] = (d2 - d1) / (2.0f * MAX(sites[a].distance_to(sites[b]), 1e-6f));
					}
				}
			}
		};
		label();
		// Pieces smaller than a fifth of the mean are merged into their neighbours (a sliver would fly off as dust).
		for (int pass = 0; pass < 3 && sites.size() > 2; pass++) {
			LocalVector<int> count;
			count.resize(sites.size());
			for (int &c : count) {
				c = 0;
			}
			for (uint32_t i = 0; i < node_count; i++) {
				if (fine.v[i] < 0.0f && n1[i] != 255) {
					count[n1[i]]++;
				}
			}
			const int min_nodes = solid_nodes / int(sites.size()) / 5;
			bool removed = false;
			for (int k = int(sites.size()) - 1; k >= 0; k--) {
				if (count[k] < min_nodes) {
					sites.remove_at(k);
					removed = true;
				}
			}
			if (!removed) {
				break;
			}
			label();
		}
		const int pieces = int(sites.size());
		const float to_m = scale; // field units to local units
		Array chunks;
		PackedFloat32Array volumes;
		for (int c = 0; c < pieces && pieces > 1; c++) {
			// The piece's nodes, with two nodes of margin.
			Vector3i bmin(fine.nx, fine.ny, fine.nz), bmax(-1, -1, -1);
			int inside = 0;
			for (int z = 0; z < fine.nz; z++) {
				for (int y = 0; y < fine.ny; y++) {
					for (int x = 0; x < fine.nx; x++) {
						const int i = fine.idx(x, y, z);
						if (n1[i] == c && fine.v[i] < h) {
							bmin = bmin.min(Vector3i(x, y, z));
							bmax = bmax.max(Vector3i(x, y, z));
							inside += fine.v[i] < 0.0f ? 1 : 0;
						}
					}
				}
			}
			if (bmax.x < 0) {
				volumes.push_back(0.0f);
				chunks.push_back(Array());
				continue;
			}
			bmin = (bmin - Vector3i(2, 2, 2)).max(Vector3i(0, 0, 0));
			bmax = (bmax + Vector3i(2, 2, 2)).min(Vector3i(fine.nx - 1, fine.ny - 1, fine.nz - 1));
			// The piece on its own grid, finer than the stone's (cells of about 7 mm at the marks' size, at most 1.6
			// times finer), so the break's grain-scale relief is meshed: granite breaks round and through its
			// crystals, with hackle steps and conchoidal ripples (rk_relief with a fresh face). Both pieces of a wall
			// read the same displaced wall, so they still fit.
			const float mm = float(get_marks_metres_per_unit());
			const float sub = CLAMP(h * scale * mm / 0.007f, 1.0f, 1.6f);
			const float hp = h / sub;
			RockGrid piece;
			piece.h = hp;
			piece.origin = fine.pos(bmin.x, bmin.y, bmin.z);
			piece.nx = int(Math::ceil(float(bmax.x - bmin.x) * sub)) + 1;
			piece.ny = int(Math::ceil(float(bmax.y - bmin.y) * sub)) + 1;
			piece.nz = int(Math::ceil(float(bmax.z - bmin.z) * sub)) + 1;
			piece.v.resize(piece.nx * piece.ny * piece.nz);
			for (int z = 0; z < piece.nz; z++) {
				for (int y = 0; y < piece.ny; y++) {
					for (int x = 0; x < piece.nx; x++) {
						const Vector3 p = piece.pos(x, y, z);
						float f = fine.sample(p);
						if (f < 2.0f * h) {
							const Vector3 wp = warped(p);
							float d1 = 1e30f, d2 = 1e30f;
							int a = 0, b = 0;
							for (int k = 0; k < pieces; k++) {
								const float d = wp.distance_squared_to(sites[k]);
								if (d < d1) {
									d2 = d1;
									b = a;
									d1 = d;
									a = k;
								} else if (d < d2) {
									d2 = d;
									b = k;
								}
							}
							float cell = 3.0f * h; // another piece's
							if (c == a || c == b) {
								const float wall_d = (d2 - d1) / (2.0f * MAX(sites[a].distance_to(sites[b]), 1e-6f));
								cell = c == a ? -wall_d : MAX(wall_d, 1e-4f * hp);
								if (wall_d < 4.0f * h) {
									const int other = c == a ? b : a;
									const int lo_c = MIN(c, other), hi_c = MAX(c, other);
									Vector3 nab = (sites[hi_c] - sites[lo_c]) / metric;
									nab = nab.length_squared() > 1e-20f ? nab.normalized() : Vector3(0, 1, 0);
									const float r = rk_relief(rock_type, to_local(p) * mm, dir_local(nab), 1.0f, hp * scale * mm, 0.0f) / (mm * scale);
									cell -= c == lo_c ? r : -r;
								}
							}
							f = MAX(f, cell);
						}
						// The margin closes the piece.
						if (x == 0 || y == 0 || z == 0 || x == piece.nx - 1 || y == piece.ny - 1 || z == piece.nz - 1) {
							f = MAX(f, 0.5f * hp);
						}
						piece.v[piece.idx(x, y, z)] = f;
					}
				}
			}
			LocalVector<Vector3> pv, pn;
			LocalVector<int> pi;
			if (!rk_surface_nets(piece, simplify, pv, pn, pi)) {
				volumes.push_back(0.0f);
				chunks.push_back(Array());
				continue;
			}
			chunks.push_back(make_arrays(piece, pv, pn, pi, &fine));
			volumes.push_back(float(inside) * h * h * h * to_m * to_m * to_m);
		}
		// Bonds: the wall between two pieces inside the solid, from the grid edges that cross it (area, centroid,
		// normal from the lower-numbered piece to the other, in the stone's frame).
		struct BondAcc {
			float area = 0.0f;
			Vector3 centroid, normal;
		};
		HashMap<uint32_t, BondAcc> bonds;
		for (int z = 0; z < fine.nz; z++) {
			for (int y = 0; y < fine.ny; y++) {
				for (int x = 0; x < fine.nx; x++) {
					const int i = fine.idx(x, y, z);
					if (fine.v[i] >= 0.0f || n1[i] == 255) {
						continue;
					}
					const int nb[3] = { x + 1 < fine.nx ? fine.idx(x + 1, y, z) : -1, y + 1 < fine.ny ? fine.idx(x, y + 1, z) : -1, z + 1 < fine.nz ? fine.idx(x, y, z + 1) : -1 };
					for (int a = 0; a < 3; a++) {
						const int j = nb[a];
						if (j < 0 || fine.v[j] >= 0.0f || n1[j] == 255 || n1[j] == n1[i] || n1[i] >= pieces || n1[j] >= pieces) {
							continue;
						}
						const int lo_c = MIN(n1[i], n1[j]), hi_c = MAX(n1[i], n1[j]);
						BondAcc &b = bonds[uint32_t(lo_c) << 8 | uint32_t(hi_c)];
						Vector3 axis;
						axis[a] = n1[i] == lo_c ? 1.0f : -1.0f;
						b.area += h * h;
						b.centroid += fine.pos(x, y, z) + Vector3(a == 0, a == 1, a == 2) * (0.5f * h);
						b.normal += axis;
					}
				}
			}
		}
		Array bond_list;
		for (const KeyValue<uint32_t, BondAcc> &kv : bonds) {
			const int ca = int(kv.key >> 8), cb = int(kv.key & 0xFF);
			if (kv.value.area < 3.0f * h * h || volumes[ca] <= 0.0f || volumes[cb] <= 0.0f) {
				continue;
			}
			const float faces = kv.value.area / (h * h);
			Dictionary b;
			b["chunks"] = Vector2i(ca, cb);
			b["centroid"] = to_local(kv.value.centroid / faces);
			b["normal"] = dir_local(kv.value.normal.normalized());
			// Voxel faces overcount a slanted wall by about 1.5 (the mean of |nx| + |ny| + |nz| on a sphere).
			b["area"] = kv.value.area / 1.5f * to_m * to_m;
			bond_list.push_back(b);
		}
		r_meta["rock_chunks"] = chunks;
		r_meta["rock_chunk_volumes"] = volumes;
		r_meta["rock_bonds"] = bond_list;
		r_meta["rock_volume"] = float(solid_nodes) * h * h * h * to_m * to_m * to_m;
	}
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
// y water film (river bed, swash zone), z metres per unit, w a random 0..63 plus 64 x the ground's slope under the
// stone (two 8-bit components, -2..2).
)";
	c += TerrainGenerator3D::get_geology_shader_code();
	c += BaseMaterial3D::get_wetness_shader_code();
	c += BaseMaterial3D::get_snow_shader_code();
	c += R"(
// Colours are linear albedo.
uniform vec3 base_color = vec3(0.30, 0.28, 0.26);
uniform vec3 speck_color = vec3(0.04);
uniform float speck_share = 0.18;
// Granite: the crystals come from the same map as the close-up geometry (rk_relief), so quartz and feldspar stand
// proud and biotite sits in its pits in colour as in relief.
uniform bool crystal_map = false;
uniform bool lamina_map = false;
uniform vec3 quartz_color = vec3(0.15, 0.15, 0.148);
uniform float speck_size = 0.004;
uniform vec3 band_color = vec3(0.42, 0.34, 0.30);
uniform float band_contrast = 0.15;
uniform float band_spacing = 0.12;
uniform vec3 rind_color = vec3(0.24, 0.21, 0.16);
uniform float rock_roughness = 0.8;
uniform float porosity = 0.3;
uniform float grain_bump = 0.2;
uniform float lichen_cover = 0.5;
// Limestone and other lime-rich rock take their own lichens (white-grey and dark crusts that etch it, orange
// Caloplaca); siliceous rock (granite, sandstone, basalt, slate) the yellow-green map lichen and grey foliose ones.
uniform bool calcareous = false;
uniform float moss_cover = 0.4;
uniform vec3 moss_color = vec3(0.035, 0.06, 0.012);
// The side away from the noon sun (north in the northern hemisphere): moss holds on the shaded, damp faces, the
// crust lichens on the lit ones.
uniform vec3 pole_direction = vec3(0.0, 0.0, -1.0);
uniform float aspect_strength = 1.0;
uniform vec3 contact_color = vec3(0.07, 0.055, 0.04);
uniform float contact_height = 0.08;
// Ground blending (docs/ideas/rocks.md): the foot eased into the ground's own surface over a band this high (metres,
// grown with the stone's size), the ground's layer from the generator's geology, coloured as the landscape's layers.
uniform bool ground_blend = false;
uniform float ground_blend_height = 0.45;
// Forms set into a terrain cliff (cave mouths): the rock keeps its own mottling, beds and marks but takes the
// cliff's colour (ground_rock, the landscape's rock layer), so the block reads as part of the wall around it.
uniform bool cliff_match = false;
uniform vec3 ground_rock = vec3(0.107, 0.1, 0.089);
uniform vec3 ground_scree = vec3(0.141, 0.133, 0.114);
uniform vec3 ground_soil = vec3(0.033, 0.04, 0.01);
uniform vec3 ground_alluvium = vec3(0.064, 0.06, 0.022);
uniform vec3 ground_sand = vec3(0.448, 0.342, 0.181);
uniform float ground_alluvium_area = 5.5;
// Mineral glints (docs/ideas/rocks.md): the glossy crystals past the distance where they are drawn one by one.
uniform bool mineral_glints = false;
// The sea's mean level (none below -1e4) and its tidal range in metres: stones reaching it carry the shore zones.
uniform float sea_level = -100000.0;
uniform float tide_range = 2.0;
uniform float wetness = 1.0;
uniform bool rain_rivulets = true;
uniform float snow = 1.0;
uniform float snow_max_slope = 55.0;
// Instance data for a stone drawn outside the MultiMesh (a breakable copy): as INSTANCE_CUSTOM.
uniform vec4 rock_instance = vec4(0.0);

varying vec3 v_world;
varying vec4 v_custom;
varying float v_scale;
varying vec3 v_origin;
varying vec2 v_slope;
varying float v_random;

// Glints of crystals smaller than the pixel, counted per cell of the pixel's footprint (Zirr and Kaplanyan 2016;
// Deliot and Belcour 2023, reduced to one facet per cell, as snow_glint() does for ice): a cell holding n crystals
// has a glossy one with chance min(share n, 1), covering share / chance of it, its normal GGX-tilted (alpha 0.6:
// cleavage planes up to about 45 degrees off the face). Returns the facet (world) and its weight (0: none).
vec4 rk_glint(vec3 p_pos, vec3 p_n, float p_footprint, float p_grain, float p_share) {
	float level = floor(max(log2(max(p_footprint, p_grain) / p_grain), 0.0));
	if (level > 5.0) {
		return vec4(p_n, 0.0);
	}
	float cell = p_grain * exp2(level);
	ivec3 q = ivec3(floor(p_pos / cell));
	uint h = (uint(q.x) * 73856093u) ^ (uint(q.y) * 19349663u) ^ (uint(q.z) * 83492791u) ^ (uint(level) * 2654435761u) ^ 0x5bd1e995u;
	h ^= h >> 16u;
	h *= 0x7feb352du;
	h ^= h >> 15u;
	h *= 0x846ca68bu;
	h ^= h >> 16u;
	float r0 = float(h & 0x3ffu) / 1023.0;
	float r1 = float((h >> 10u) & 0x3ffu) / 1024.0;
	float r2 = float((h >> 20u) & 0x3ffu) / 1023.0;
	float grains = exp2(2.0 * level);
	float chance = min(p_share * grains, 1.0);
	if (r0 >= chance) {
		return vec4(p_n, 0.0);
	}
	float tan2 = 0.36 * r1 / max(1.0 - r1, 1e-3);
	float ct = inversesqrt(1.0 + tan2);
	float st = sqrt(max(1.0 - ct * ct, 0.0));
	vec3 t = normalize(cross(p_n, abs(p_n.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
	vec3 b = cross(p_n, t);
	float phi = 6.2831853 * r2;
	return vec4(normalize(p_n * ct + (t * cos(phi) + b * sin(phi)) * st), p_share / chance / sqrt(max(p_share * grains, 1.0)));
}

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

// Cellular grains (Worley 1996): the hash of the nearest jittered point's cell and the distance to the next one's
// boundary (F2 - F1), so each grain is an irregular polygon as crystals in a rock are.
vec2 rk_grain(vec3 p) {
	vec3 i = floor(p);
	float f1 = 9.0, f2 = 9.0, id = 0.0;
	for (int z = -1; z <= 1; z++) {
		for (int y = -1; y <= 1; y++) {
			for (int x = -1; x <= 1; x++) {
				vec3 c = i + vec3(float(x), float(y), float(z));
				vec3 f = c + vec3(rk_hash3(c), rk_hash3(c + 19.19), rk_hash3(c + 47.47));
				float d = length(f - p);
				if (d < f1) {
					f2 = f1;
					f1 = d;
					id = rk_hash3(c + 91.3);
				} else if (d < f2) {
					f2 = d;
				}
			}
		}
	}
	return vec2(id, f2 - f1);
}

// Lichen colonies (docs/ideas/rocks.md, "Lichen on rock"): each cell may hold one thallus grown out from its point
// as a rosette, its radius (in cells) from its age. The colony holding p is the one with the smallest distance over
// radius (a multiplicatively weighted Voronoi diagram, so a big old thallus overgrows a young one); returns that ratio,
// the next colony's, and the holder's id. Where two meet, the ratios match: the line between them.
// Colonisation is patchy (spores land and survive in some stretches of a face more than others): each colony's
// size is scaled by a patch field read at its own point (p_cell metres a cell, p_bias raising it), so colonies near a
// patch's edge are small and whole, never cut.
vec3 rk_lichen_cells(vec3 p, float p_grow, float p_share, float p_cell, float p_bias) {
	vec3 i = floor(p);
	float b1 = 9.0, b2 = 9.0, id = 0.0;
	for (int z = -1; z <= 1; z++) {
		for (int y = -1; y <= 1; y++) {
			for (int x = -1; x <= 1; x++) {
				vec3 c = i + vec3(float(x), float(y), float(z));
				if (rk_hash3(c + 5.3) > p_share) {
					continue;
				}
				vec3 f = c + vec3(rk_hash3(c), rk_hash3(c + 19.19), rk_hash3(c + 47.47));
				float patch_c = smoothstep(0.3, 0.6, rk_noise3(f * p_cell * 1.4 + 3.0) + p_bias);
				if (patch_c < 0.05) {
					continue;
				}
				float r = length(f - p) / max(p_grow * patch_c * (0.35 + 0.9 * rk_hash3(c + 71.1)), 1e-3);
				if (r < b1) {
					b2 = b1;
					b1 = r;
					id = rk_hash3(c + 91.3);
				} else if (r < b2) {
					b2 = r;
				}
			}
		}
	}
	return vec3(b1, b2, id);
}

uint rk_hash_u(uint x) {
	x ^= x >> 16u;
	x *= 2146121005u;
	x ^= x >> 15u;
	x *= 2221713035u;
	x ^= x >> 16u;
	return x;
}

float rk_lattice_u(ivec3 c, uint s) {
	return float(rk_hash_u(uint(c.x) * 73856093u ^ uint(c.y) * 19349663u ^ uint(c.z) * 83492791u ^ s) >> 8u) / 16777216.0;
}

// The crystal map of rk_relief (rk_worley_point with RK_CRYSTAL_SEED): x the cell's class value, y the distance
// to its boundary in cells (about twice).
vec2 rk_crystal(vec3 p, uint s) {
	ivec3 i = ivec3(floor(p));
	float f1 = 9.0, f2 = 9.0;
	uint id = 0u;
	for (int z = -1; z <= 1; z++) {
		for (int y = -1; y <= 1; y++) {
			for (int x = -1; x <= 1; x++) {
				ivec3 c = i + ivec3(x, y, z);
				vec3 f = vec3(c) + vec3(rk_lattice_u(c, s), rk_lattice_u(c, s + 7u), rk_lattice_u(c, s + 13u));
				vec3 dv = f - p;
				float d = dot(dv, dv);
				if (d < f1) {
					f2 = f1;
					f1 = d;
					id = rk_hash_u(uint(c.x) * 73856093u ^ uint(c.y) * 19349663u ^ uint(c.z) * 83492791u ^ (s + 29u));
				} else if (d < f2) {
					f2 = d;
				}
			}
		}
	}
	return vec2(float(id >> 8u) / 16777216.0, sqrt(f2) - sqrt(f1));
}

void vertex() {
	v_world = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	v_custom = INSTANCE_CUSTOM.z > 0.0 ? INSTANCE_CUSTOM : rock_instance;
	v_scale = v_custom.z > 0.0 ? v_custom.z : length(MODEL_MATRIX[0].xyz);
	v_origin = MODEL_MATRIX[3].xyz;
	float packed = v_custom.w;
	v_random = packed < 1.0 ? packed : mod(packed, 64.0) / 64.0;
	float q = floor(packed / 64.0);
	v_slope = packed >= 64.0 ? vec2(mod(q, 256.0), floor(q / 256.0)) / 255.0 * 4.0 - 2.0 : vec2(0.0);
}

void fragment() {
	vec3 lp = vec3(UV.x, UV2.x, UV.y);
	float seed = v_random * 61.7;
	vec3 mp = lp * v_scale + seed; // metres along the stone
	float cavity = COLOR.r;
	float fresh = COLOR.g;
	vec3 wn = normalize((INV_VIEW_MATRIX * vec4(NORMAL, 0.0)).xyz);

	// Mineral colour: broad mottling, bands along the beds, specks of the dark (or light) mineral.
	vec3 col = base_color * (0.82 + 0.36 * rk_fbm3(mp * 1.7));
	col *= 0.9 + 0.2 * rk_noise3(vec3(seed));
	// Beds lie across the short axis of a lying stone, the long one of an upright form, the b axis of a fin (COLOR.b).
	float bed_q = COLOR.b > 0.75 ? lp.x : (COLOR.b > 0.25 ? lp.z : lp.y);
	float bands = 0.5 + 0.5 * sin(6.2831853 * (bed_q * v_scale / band_spacing + 0.6 * rk_fbm3(mp * 0.8)));
	col = mix(col, band_color, band_contrast * smoothstep(0.3, 0.9, bands));
	if (cliff_match) {
		col *= ground_rock / max(base_color, vec3(1e-3));
	}
	// Mineral maps at the stone's real size, in octave steps from its family's typical size (UV2.y), the steps
	// the close-up shapes are carved at (RockGenerator.get_marks_metres_per_unit), so relief and colour share one map.
	float mpu_marks = UV2.y > 0.0 ? UV2.y * exp2(round(log2(max(v_scale, 1e-4) / UV2.y))) : v_scale;
	if (lamina_map) {
		// The laminae the geometry carved: harder ones (standing out) paler and better cemented.
		vec3 mg = lp * mpu_marks;
		float hard = rk_lattice_u(ivec3(int(floor(mg.y / 0.019)), 0, 0), 23133u);
		col *= 0.88 + 0.24 * hard;
	}
	// Pixel footprint in metres from the projection (no screen derivatives, so the path tracer's hits agree).
	float pix = 2.0 / (abs(PROJECTION_MATRIX[1][1]) * VIEWPORT_SIZE.y);
	float footprint = PROJECTION_MATRIX[3][3] != 0.0 ? pix : length(VERTEX) * pix;
	// Specks: point-sampled once a pixel covers several they alias into salt and pepper in raster, while the path
	// tracer's samples average them; both go to the mean share as the footprint passes the speck size.
	// Grains, not voxels: cellular grains, irregular polygons, only evaluated while a pixel is finer than a grain.
	float near_grain = 1.0 - smoothstep(0.5 * speck_size, 2.0 * speck_size, footprint);
	float speck_mix = speck_share;
	float crystal_gloss = 0.0;
	float resolved = 0.0; // how far the crystals are drawn one by one (1: all)
	vec3 facet = vec3(0.0); // crystal facet tilt, in the stone's frame
	if (crystal_map) {
		// In the stone's frame at its own size (see mpu_marks), as the geometry was carved.
		vec3 mg = lp * mpu_marks;
		float near_crystal = 1.0 - smoothstep(0.006, 0.03, footprint);
		resolved = near_crystal;
		float quartz = 0.3;
		speck_mix = 0.07;
		if (near_crystal > 0.0) {
			// Grain boundaries are not straight: crystals grew into each other, so the map is looked up through a
			// small warp (a fifth of a crystal) and its borders come out ragged instead of a polygon mosaic.
			vec3 warp = vec3(rk_noise3(mg * 160.0), rk_noise3(mg * 160.0 + 17.3), rk_noise3(mg * 160.0 + 41.9)) - 0.5;
			vec3 mgw = mg + warp * 0.0045;
			vec2 c = rk_crystal(mgw / 0.018, 20903u);
			vec2 b = rk_crystal(mgw / 0.013, 45323u);
			float edge = smoothstep(0.0, 0.1, c.y);
			float bio = step(b.x, 0.07) * smoothstep(0.0, 0.1, b.y);
			// Soft at the border: a crystal's class fades into its neighbour over the last tenth of a cell.
			float qz = step(c.x, 0.3) * mix(0.6, 1.0, smoothstep(0.0, 0.12, c.y));
			speck_mix = mix(0.07, bio, near_crystal);
			quartz = mix(quartz, qz, near_crystal);
			// Cleavage faces of feldspar and glassy quartz catch the light: smoother than the rind round them.
			crystal_gloss = near_crystal * (qz * 0.7 + (1.0 - qz) * step(0.65, fract(c.x * 13.7)));
			// Each crystal a facet at its own tilt (cleavage planes of feldspar, quartz grains), crisp at the cell
			// borders: a broken face reads as grains, not a smooth surface. Stronger on fresh breaks, where the
			// rind has not dulled them.
			float fa = fract(c.x * 91.7) * 6.2831853, fb = fract(c.x * 37.3) * 2.0 - 1.0;
			// Cleavage planes cut the crystal at any angle to the broken face, so the glossy ones tilt much further
			// (up to about 45 degrees): spread over the hemisphere, a few of them face the half vector of any sun.
			float tilt = mix(0.25 + 0.25 * fresh, 0.35 + 0.65 * fract(c.x * 5.13), step(0.5, crystal_gloss));
			facet = vec3(cos(fa) * sqrt(1.0 - fb * fb), fb, sin(fa) * sqrt(1.0 - fb * fb)) * near_crystal * tilt * (1.0 - bio);
			// Feldspar: each crystal its own shade, the boundaries etched darker.
			col *= mix(1.0, 0.9 + 0.2 * fract(c.x * 7.31), near_crystal);
			// Inside each crystal: feldspar clouded by its alteration (sericite, kaolin) in soft blotches, quartz
			// glassy and grey with faint inclusions. Tone noise per crystal, so no two cells are flat copies.
			float cloud = rk_fbm3(mg * 240.0 + fract(c.x * 53.1) * 31.0);
			col *= mix(1.0, mix(0.86 + 0.26 * cloud, 0.94 + 0.08 * cloud, qz), near_crystal);
			col *= mix(1.0, 0.9 + 0.1 * edge, near_crystal);
		}
		col = mix(col, quartz_color, quartz * 0.8);

		near_grain = 0.0;
	}
	if (near_grain > 0.0) {
		vec2 g = rk_grain(mp / speck_size);
		float in_speck = step(1.0 - speck_share, g.x) * smoothstep(0.0, 0.08, g.y);
		speck_mix = mix(speck_share, in_speck, near_grain);
		// The middle third of the grains a shade darker and glassier (quartz among feldspar).
		float glassy = step(0.35, g.x) * step(g.x, 0.65) * near_grain;
		col *= 1.0 - 0.18 * glassy;
	}
	col = mix(col, speck_color, speck_mix * 0.85);
	// The rind: weathered faces stained and duller than a fresh break.
	vec3 weathered = mix(col, rind_color, 0.45 + 0.25 * rk_fbm3(mp * 3.1));
	col = mix(weathered, col * 1.08, fresh);
	// Cleavage faces (a third of the feldspars) and glassy quartz are near mirrors, each at its own facet tilt: under a
	// low sun a few of them flash (cleavage glints, the crystal-scale case of a discrete microfacet glint model).
	float rough = mix(rock_roughness, 0.12, crystal_gloss * (0.6 + 0.4 * fresh));

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

	if (dot(facet, facet) > 0.0) {
		vec3 fw = normalize((MODEL_MATRIX * vec4(facet, 0.0)).xyz) * length(facet);
		fw -= wn * dot(fw, wn);
		wn = normalize(wn + fw);
	}

	// Ground contact: soil splashed up and damp where the stone sits in it.
	float contact = 0.0;
	float ground_cover = 0.0;
	if (v_custom.z > 0.0) {
		// The ground under this point: the plane through the stone's foot along the slope there.
		float above = v_world.y - (v_custom.x + dot(v_slope, v_world.xz - v_origin.xz));
		contact = 1.0 - smoothstep(0.0, contact_height * max(v_scale, 0.25), above);
		col = mix(col, contact_color, 0.6 * contact);
		// Ground blending (CryEngine's terrain layer blend, UE runtime virtual texture blending, Far Cry 5): over a
		// band above the ground the stone takes the ground's surface, the hollows first and the proud parts last
		// (height blend on the cavity map, Mishkinis 2013), its edge broken by noise, more on faces turned up (soil
		// and sand settle there), and the shading normal eased to the ground's so the seam lights alike.
		if (ground_blend && above < ground_blend_height * 2.5 * sqrt(max(v_scale, 0.25))) {
			float band = ground_blend_height * sqrt(max(v_scale, 0.25));
			// Debris banks up on the uphill side (sediment trapped against the stone).
			float sl = length(v_slope);
			float up = sl > 1e-3 ? dot(v_slope / sl, v_world.xz - v_origin.xz) / max(v_scale, 0.25) : 0.0;
			band *= 1.0 + 1.2 * clamp(up, 0.0, 1.0) * clamp(sl * 2.0, 0.0, 1.0);
			float edge = above / band + 0.9 * (cavity - 0.6) + 0.9 * (rk_fbm3(mp * 2.3 + 11.0) - 0.5) - 0.35 * clamp(wn.y, 0.0, 1.0);
			// A filled foot with a ragged top, and above it a thinning dust of the ground splashed and blown on.
			float gb = max(1.0 - smoothstep(0.45, 0.7, edge), 0.4 * (1.0 - smoothstep(0.7, 1.8, edge)) * smoothstep(0.3, 0.6, rk_fbm3(mp * 7.0 + 5.0)));
			vec3 gcol = ground_soil;
			if (terrain_geology_rect.z > 0.0) {
				vec4 g = terrain_geology(v_world);
				float rock_w = clamp(g.y, 0.0, 1.0);
				float scree_w = clamp(g.z, 0.0, 1.0) * (1.0 - rock_w) + 0.45 * clamp(-g.z, 0.0, 1.0) * (1.0 - rock_w);
				float rest = max(1.0 - rock_w - scree_w, 0.0);
				float alluv = smoothstep(ground_alluvium_area, ground_alluvium_area + 1.0, g.w);
				gcol = ground_rock * rock_w + ground_scree * scree_w + mix(ground_soil, ground_alluvium, alluv) * rest;
				gcol = mix(gcol, ground_sand, clamp(-g.x, 0.0, 1.0));
			}
			gcol *= 0.75 + 0.5 * rk_fbm3(mp * 9.0 + 4.0);
			// The ground's grain, not a flat colour: fines with grit (pale and dark chips, 1 to 3 cm).
			float grit = rk_noise3(v_world * 45.0);
			gcol *= grit > 0.72 ? 1.45 : (grit < 0.2 ? 0.6 : 1.0);
			// A damp foot: soil wicks water up the lowest part, darker and a little glossier.
			float damp = 1.0 - smoothstep(0.0, 0.45, edge);
			gcol *= mix(1.0, 0.55, damp);
			col = mix(col, gcol, gb);
			rough = mix(rough, mix(0.95, 0.7, damp), gb);
			vec3 gn = normalize(vec3(-v_slope.x, 1.0, -v_slope.y));
			wn = normalize(mix(wn, gn, 0.6 * gb * smoothstep(-0.3, 0.4, dot(wn, gn))));
			fresh *= 1.0 - gb;
			ground_cover = gb;
		}
	}
	col *= mix(0.55, 1.0, cavity);

	// Lichen: crusts on open, lit, old faces (not on fresh breaks or under water), patches of grey-green with a few
	// orange and black ones (Rhizocarpon, Xanthoria, Lecidea are the common rock crusts).
	// Aspect: how far the face turns to the pole (+1) or the sun (-1), on its horizontal part.
	// Instance film, plus 2 on stones the sea reaches (RockField3D flags them for the shore zones below).
	float at_sea = step(1.5, v_custom.y);
	float film_in = v_custom.y - 2.0 * at_sea;
	vec3 wh = vec3(wn.x, 0.0, wn.z);
	float shade_side = aspect_strength * dot(wh, normalize(vec3(pole_direction.x, 0.0, pole_direction.z) + vec3(1e-5, 0.0, 0.0)));
	float open_face = (1.0 - fresh) * smoothstep(0.4, 0.9, cavity) * (1.0 - film_in) * smoothstep(-0.3, 0.5, wn.y) * clamp(0.75 - 0.45 * shade_side, 0.2, 1.2);
	// Lichen thalli (docs/ideas/rocks.md, "Lichen on rock"): rosettes grown out from points, a few millimetres to a
	// decimetre across by age (map lichen gains well under a millimetre a year; Armstrong 2011, Bradwell 2009), lobed
	// at the margin, meeting their neighbours in black lines (the prothallus: the "map"), the crust cracked into areolae
	// with black fruiting discs (apothecia). Species by substrate and exposure (Fletcher 1973; Purvis et al. 1992): on
	// siliceous rock yellow-green Rhizocarpon, grey Lecanora and foliose Parmelia, black Lecidea, orange Xanthoria on
	// tops where birds perch; on limestone white-grey Aspicilia and dark Verrucaria crusts, orange Caloplaca. Colonised
	// patchily; fresh breaks bare. Below a pixel the detail fades to its average.
	float lic = 0.0;
	float px = footprint; // metres per pixel (no screen derivatives: the path tracer's hit shaders have none)
	// Not on stones under a stream or in the swash (a water film): lichens need to dry between wettings.
	if (lichen_cover > 0.01 && open_face > 0.01 && film_in < 0.3) {
		const float cell_m = 0.09;
		vec3 lc = rk_lichen_cells(mp / cell_m, 0.7 + 0.6 * lichen_cover, 0.85, cell_m, 0.5 * (lichen_cover - 0.5));
		// Lobed margins: lobes of about a centimetre, finer crenulation on them.
		float rim = 0.3 * (rk_noise3(mp * 90.0 + lc.z * 37.0) - 0.5) + 0.1 * (rk_noise3(mp * 380.0 + 5.0) - 0.5);
		float r_eff = lc.x + rim;
		float soft = clamp(px / (cell_m * 0.35), 0.03, 0.5);
		float inside = (1.0 - smoothstep(1.0 - soft, 1.0 + soft, r_eff)) * smoothstep(0.02, 0.3, open_face);
		if (inside > 0.0) {
			float top = smoothstep(0.65, 0.95, wn.y);
			float k = fract(lc.z * 7.31);
			vec3 sp_col;
			float prothallus; // how black the margin and the meeting lines are
			float foliose = 0.0;
			float fruit = 0.02; // apothecia colour
			if (calcareous) {
				if (k < 0.04 + 0.12 * top) {
					sp_col = vec3(0.62, 0.21, 0.02); // Caloplaca
					prothallus = 0.0;
					fruit = 0.35;
				} else if (k < 0.66) {
					sp_col = vec3(0.56, 0.56, 0.5); // Aspicilia calcarea
					prothallus = 0.5;
				} else if (k < 0.86) {
					sp_col = vec3(0.04, 0.037, 0.032); // Verrucaria nigrescens
					prothallus = 0.2;
				} else {
					sp_col = vec3(0.36, 0.37, 0.33); // Lecanora
					prothallus = 0.3;
					fruit = 0.2;
				}
			} else {
				if (k < 0.02 + 0.1 * top) {
					sp_col = vec3(0.6, 0.22, 0.02); // Xanthoria (where birds perch: a few tops)
					prothallus = 0.0;
					foliose = 1.0;
					fruit = 0.3;
				} else if (k < 0.5) {
					sp_col = vec3(0.34, 0.38, 0.08); // Rhizocarpon geographicum
					prothallus = 1.0;
				} else if (k < 0.82) {
					sp_col = vec3(0.34, 0.37, 0.3); // Parmelia, Lecanora
					prothallus = 0.3;
					foliose = step(0.66, k);
					fruit = 0.15;
				} else {
					sp_col = vec3(0.022, 0.021, 0.02); // Lecidea
					prothallus = 0.6;
				}
			}
			sp_col *= 0.85 + 0.3 * rk_noise3(mp * 40.0 + lc.z * 13.0);
			// Areolae and apothecia, a few millimetres; faded to their share past a pixel.
			float fine = 1.0 - smoothstep(0.0006, 0.0025, px);
			vec2 ar = rk_grain(mp / 0.0028);
			float crack = (1.0 - foliose) * (1.0 - smoothstep(0.04, 0.12, ar.y));
			float disc = step(0.82, ar.x) * smoothstep(0.18, 0.3, ar.y) * smoothstep(0.3, 0.7, 1.0 - lc.x);
			vec3 thallus = mix(sp_col, sp_col * 0.25, mix(0.15, crack, fine) * (1.0 - foliose));
			thallus = mix(thallus, vec3(fruit) * (sp_col + 0.1), mix(0.05, disc, fine));
			// The prothallus: a black rim and the lines where colonies meet, a millimetre or two wide.
			float edge_w = max(0.06, px / (cell_m * 0.5));
			float line = max(smoothstep(1.0 - 1.2 * edge_w, 1.0 - 0.3 * edge_w, r_eff), (1.0 - smoothstep(0.0, edge_w, lc.y - lc.x)) * step(lc.y, 1.1));
			thallus = mix(thallus, vec3(0.015), prothallus * line);
			lic = inside;
			col = mix(col, thallus, inside);
			rough = mix(rough, mix(0.92, 0.8, foliose), inside);
		}
	}

	// Moss: where the stone stays damp and shaded (Bates 2000; Glime, Bryophyte Ecology): the faces turned from the
	// sun, cracks and the joints between blocks, and the foot where it meets the soil. Open, sunlit tops dry out
	// between rains and carry lichen instead; overhangs get no rain.
	float moss_term = 0.9 * shade_side + 0.45 * (1.0 - cavity) + 0.8 * contact * (1.0 - smoothstep(0.4, 0.85, wn.y)) + 0.25 * max(wn.y, 0.0) - 0.4 * max(-wn.y - 0.3, 0.0);
	float moss_place = smoothstep(0.2, 0.7, moss_term) * (1.0 - fresh);
	float moss = moss_cover * moss_place * smoothstep(0.42, 0.58, rk_fbm3(mp * 2.2 + 17.0) + 0.25 * moss_cover - 0.12);
	col = mix(col, moss_color * (0.7 + 0.6 * rk_fbm3(mp * 25.0)), moss);
	rough = mix(rough, 0.95, moss);

	// A film of water and its biofilm: river beds and the swash zone stay wet and darker.
	float film = film_in;
	// The shore zones of rock by the sea (Stephenson and Stephenson 1949; Lewis 1964): below high water a crust of
	// barnacles over the stone, weed toward low water, all of it wet; above it the splash zone's black Verrucaria
	// lichen in a tar-like band; the grey and orange lichens only above that. Ragged edges, a wave's reach not a line.
	float above_sea = v_world.y - sea_level;
	if (at_sea > 0.5 && above_sea < tide_range * 4.0) {
		float zn = rk_fbm3(mp * 2.3 + 5.0);
		float edge = above_sea + (zn - 0.5) * 0.7 * tide_range * 0.5;
		float tidal = 1.0 - smoothstep(tide_range * 0.4, tide_range * 0.6, edge);
		// On an exposed stack the swash throws the splash zone several metres up.
		float splash = (1.0 - tidal) * (1.0 - smoothstep(tide_range * 1.6, tide_range * 3.0, edge + 0.6 * tide_range * (rk_fbm3(mp * 0.9 + 2.0) - 0.5)));
		// Barnacles: a dull grey-brown crust, specked by the shells' pale plates, not a clean pale band.
		float shells = smoothstep(0.62, 0.8, rk_noise3(mp * 140.0));
		vec3 barnacle = mix(vec3(0.13, 0.12, 0.10), vec3(0.42, 0.40, 0.35), shells) * (0.8 + 0.4 * rk_noise3(mp * 23.0));
		vec3 weed = vec3(0.05, 0.045, 0.02) * (0.8 + 0.4 * rk_noise3(mp * 9.0));
		vec3 shore = mix(barnacle, weed, 1.0 - smoothstep(-0.25 * tide_range, 0.1 * tide_range, edge + 0.3 * tide_range * (rk_fbm3(mp * 4.0) - 0.5)));
		col = mix(col, shore, 0.9 * tidal);
		col = mix(col, vec3(0.025, 0.025, 0.022), 0.92 * splash * (0.65 + 0.35 * smoothstep(0.35, 0.55, rk_fbm3(mp * 6.0 + 9.0) + 0.15)));
		rough = mix(rough, 0.85, tidal);
		film = max(film, tidal);
	}
	col = mix(col, col * vec3(0.75, 0.72, 0.55), 0.5 * film);
	// Grus on the floors of hollows (tafoni, pits): the grains that fell out of the walls, paler than the rind and
	// dull, settled where the face turns up deep in a cavity.
	float grus = smoothstep(0.55, 0.85, wn.y) * (1.0 - smoothstep(0.3, 0.65, cavity)) * (1.0 - film_in);
	if (grus > 0.0) {
		vec3 sand = col * (1.15 + 0.25 * rk_noise3(mp * 180.0)) + vec3(0.03, 0.025, 0.015);
		col = mix(col, sand, grus);
		rough = mix(rough, 0.97, grus);
	}
	// Rain on the stone (docs/ideas/rocks.md, "Rain-wet rock"): it wets the whole face while it rains, then dries
	// patchily, the open tops and the sunny side first, the hollows, the shaded side and the lines where the water
	// ran last (Lagarde 2013, "Water drop 2"; Tatarchuk 2006). While it rains, the water on the steep faces gathers
	// into rivulets along the converging lines the mesh's flow routing found (COLOR.a), each a thin wavy line of
	// standing water with beads running down it; under the pixel they fade to their share of the face.
	float ground_wet = weather_wetness_at(v_world);
	float flow = 1.0 - COLOR.a;
	float dry_n = rk_fbm3(mp * 3.1 + 41.0);
	float hold = clamp(0.38 + 0.32 * max(wn.y, 0.0) * cavity - 0.22 * shade_side - 0.45 * smoothstep(0.15, 0.6, flow) - 0.35 * (1.0 - cavity) + 0.6 * (dry_n - 0.5), 0.03, 0.88);
	float local_wet = mix(0.35, 1.0, ground_wet) * smoothstep(hold - 0.05, hold + 0.05, ground_wet);
	float wet = max(wetness * local_wet, max(film, contact * 0.4 * ground_wet));
	float rivulet = 0.0;
	if (rain_rivulets && weather_map_rect.z > 0.0) {
		vec2 st_half = 0.5 / vec2(textureSize(weather_state_map, 0));
		float rain_mm = textureLod(weather_state_map, clamp((v_world.xz - weather_map_rect.xy) / weather_map_rect.zw, st_half, 1.0 - st_half), 0.0).r;
		float running = max(smoothstep(0.2, 3.0, rain_mm), 0.6 * smoothstep(0.92, 1.0, ground_wet) * step(0.05, rain_mm));
		float fm = smoothstep(0.28, 0.7, flow);
		float steep = 1.0 - smoothstep(0.75, 0.95, abs(wn.y));
		if (running * fm * steep > 0.0) {
			// Lines across the face by the angle round the stone's vertical axis (a whole number of them round it,
			// so no seam), not by a projection on the bumpy normal, which would scatter them into specks.
			vec2 rel = v_world.xz - v_origin.xz;
			float n_lines = max(1.0, floor(float(TAU) * 0.85 * max(v_scale, 0.05) / 0.13));
			float sl = (atan(rel.y, rel.x) / float(TAU) + 0.5) * n_lines;
			float cw = float(TAU) * 0.85 * max(v_scale, 0.05) / n_lines; // m between rivulet lines across the face
			float along = v_world.y;
			float cid = floor(sl);
			float h1 = fract(sin(cid * 127.1 + v_random * 311.7) * 43758.5453);
			float h2 = fract(sin(cid * 269.5 + v_random * 183.3) * 43758.5453);
			float present = step(h1, (0.2 + 0.7 * fm) * running * smoothstep(0.1, 0.3, flow));
			float x0 = 0.35 + 0.3 * h2 + 0.14 * sin(along * 3.1 + h1 * 40.0) + 0.06 * sin(along * 8.3 + h2 * 13.0) + 0.03 * sin(along * 21.0 + h1 * 5.0);
			float dx = (fract(sl) - x0) * cw;
			// A band of standing water a centimetre or two wide where it starts, widening down the face as it gathers.
			float width = 0.007 + 0.022 * fm;
			// A rivulet breaks where the film thins (a dry rib, a grain) and starts again below.
			float runs = smoothstep(0.32, 0.45, fract(sin(floor(along * 4.0 + h2 * 3.0) * 91.7 + cid * 17.3 + v_random * 7.1) * 43758.5453) + 0.3 * fm);
			float core = runs * (1.0 - smoothstep(0.35 * width, width, abs(dx)));
			float bead = 0.85 + 0.15 * smoothstep(0.75, 1.0, fract(along * 2.5 - TIME * (0.6 + 0.5 * h2) + h1 * 7.0));
			float resolved_r = 1.0 - smoothstep(0.5 * width, 2.0 * width, footprint);
			rivulet = present * steep * mix(1.4 * width / cw, core * bead, resolved_r);
			wet = max(wet, running * steep * mix(0.6, 1.0, fm));
		}
	}
	float metallic = 0.0;
	float specular = 0.5;
	// Rivulets and overhang lips: the water clings under the overhangs too (the shared wet code leaves undersides dry).
	// Stone soaks: pores filling with water darken it far more than the shared default porosity gives (wet
	// sandstone and limestone go about half as bright, granite a third darker; Lekner and Dorf 1988).
	// Satin, not glazed: the soaked face keeps most of its roughness (the grains stand proud of the film); the gloss
	// is on the rivulets.
	float rough_dry = rough;
	weather_wetness_apply(wet, max(wn.y, rivulet > 0.0 ? 0.2 : -1.0), metallic, clamp(porosity + 0.4, 0.0, 1.0), col, rough);
	rough = mix(rough_dry, rough, 0.35);
	col *= 1.0 - 0.4 * rivulet;
	rough = mix(rough, 0.07, rivulet);

	vec4 snow_s = snow_cover_at(v_world);
	float sw = snow_cover_weight(snow_s.x * snow, snow_max_slope, wn, wn);
	snow_cover_apply(sw, snow_s.y, snow_s.z, col, rough, metallic, specular);

	vec3 n_view = normalize((VIEW_MATRIX * vec4(wn, 0.0)).xyz);
	NORMAL = n_view;
	// Mineral glints (Chermain et al. 2020; Deliot and Belcour 2023): past the distance where the crystals are drawn
	// one by one, the glossy ones (quartz, feldspar cleavage; on other rocks quartz and mica grains) still flash.
	// Crusts, moss, a water film, snow and the ground's dust cover them; the weathered rind dulls them.
	float glint_ok = (1.0 - lic) * (1.0 - moss) * (1.0 - sw) * (1.0 - smoothstep(0.05, 0.3, wet)) * (1.0 - ground_cover) * mix(0.35, 1.0, fresh) * (1.0 - resolved);
	if (mineral_glints && glint_ok > 0.0) {
		// Crystal size in metres on this stone (the marks' map is laid out at mpu_marks, the stone drawn at v_scale).
		float gd = crystal_map ? 0.018 * v_scale / max(mpu_marks, 1e-4) : 2.0 * speck_size;
		float gshare = crystal_map ? 0.45 : 0.12;
		if (IN_PATH_TRACER) {
			// Exact: the facet of the crystal each sample lands on (the path tracer's samples count the footprint's
			// crystals themselves, at any distance and bounce), in the base specular only (the diffuse keeps NORMAL).
			ivec3 q = ivec3(floor(v_world / gd));
			uint h = (uint(q.x) * 73856093u) ^ (uint(q.y) * 19349663u) ^ (uint(q.z) * 83492791u) ^ 0x2545f491u;
			h ^= h >> 16u;
			h *= 0x7feb352du;
			h ^= h >> 15u;
			h *= 0x846ca68bu;
			h ^= h >> 16u;
			if (float(h & 0x3ffu) / 1023.0 < gshare) {
				float r1 = float((h >> 10u) & 0x3ffu) / 1024.0;
				float r2 = float((h >> 20u) & 0x3ffu) / 1023.0;
				float tan2 = 0.36 * r1 / max(1.0 - r1, 1e-3);
				float ct = inversesqrt(1.0 + tan2);
				float st = sqrt(max(1.0 - ct * ct, 0.0));
				vec3 t = normalize(cross(wn, abs(wn.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
				vec3 b = cross(wn, t);
				float phi = 6.2831853 * r2;
				vec3 f = normalize(wn * ct + (t * cos(phi) + b * sin(phi)) * st);
				SPECULAR_NORMAL = normalize((VIEW_MATRIX * vec4(normalize(mix(wn, f, glint_ok)), 0.0)).xyz);
				rough = mix(rough, 0.12, glint_ok);
			}
		} else {
			// Raster: the counted facet as a second, near-mirror lobe (the coat's, which takes NORMAL before NORMAL_MAP
			// as its normal), weighted by the share of the pixel it covers; NORMAL_MAP is solved to land back on the
			// stone's own normal, so its shading is unchanged. Quartz and feldspar F0 about 0.045: the coat's 0.04.
			vec4 g = rk_glint(v_world, wn, footprint, gd, gshare);
			float gw = g.w * glint_ok;
			if (gw > 0.0) {
				vec3 f = normalize((VIEW_MATRIX * vec4(g.xyz, 0.0)).xyz);
				vec3 t = normalize(cross(f, abs(f.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
				vec3 b = cross(f, t);
				vec3 k = vec3(dot(n_view, t), dot(n_view, b), dot(n_view, f));
				if (k.z > 0.05) {
					NORMAL = f;
					TANGENT = t;
					BINORMAL = b;
					NORMAL_MAP = vec3(k.xy * 0.5 + 0.5, 1.0);
					NORMAL_MAP_DEPTH = 1.0;
					CLEARCOAT = 0.6 * min(gw, 1.0); // (its ambient term sees no per-facet occlusion: held under the full coat)
					CLEARCOAT_ROUGHNESS = 0.3;
				}
			}
		}
	}
	ALBEDO = col;
	ROUGHNESS = rough;
	METALLIC = metallic;
	SPECULAR = specular;
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
	m->set_shader_parameter("crystal_map", key == ROCK_GRANITE);
	m->set_shader_parameter("lamina_map", key == ROCK_SANDSTONE);
	m->set_shader_parameter("calcareous", key == ROCK_LIMESTONE);
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
	ClassDB::bind_method(D_METHOD("set_salt", "salt"), &RockGenerator::set_salt);
	ClassDB::bind_method(D_METHOD("get_salt"), &RockGenerator::get_salt);
	ClassDB::bind_method(D_METHOD("get_effective_salt"), &RockGenerator::get_effective_salt);
	ClassDB::bind_method(D_METHOD("get_weathering"), &RockGenerator::get_weathering);
	ClassDB::bind_method(D_METHOD("set_detail", "detail"), &RockGenerator::set_detail);
	ClassDB::bind_method(D_METHOD("get_detail"), &RockGenerator::get_detail);
	ClassDB::bind_method(D_METHOD("set_lods", "enabled"), &RockGenerator::set_lods);
	ClassDB::bind_method(D_METHOD("get_lods"), &RockGenerator::get_lods);
	ClassDB::bind_method(D_METHOD("set_simplify", "enabled"), &RockGenerator::set_simplify);
	ClassDB::bind_method(D_METHOD("get_simplify"), &RockGenerator::get_simplify);
	ClassDB::bind_method(D_METHOD("set_close_up", "enabled"), &RockGenerator::set_close_up);
	ClassDB::bind_method(D_METHOD("get_close_up"), &RockGenerator::get_close_up);
	ClassDB::bind_method(D_METHOD("set_fracture_pieces", "pieces"), &RockGenerator::set_fracture_pieces);
	ClassDB::bind_method(D_METHOD("get_fracture_pieces"), &RockGenerator::get_fracture_pieces);
	ClassDB::bind_method(D_METHOD("set_marks_scale", "metres_per_unit"), &RockGenerator::set_marks_scale);
	ClassDB::bind_method(D_METHOD("get_marks_scale"), &RockGenerator::get_marks_scale);
	ClassDB::bind_method(D_METHOD("get_roundness"), &RockGenerator::get_roundness);
	ClassDB::bind_method(D_METHOD("generate"), &RockGenerator::generate);
	ClassDB::bind_method(D_METHOD("generate_library", "count"), &RockGenerator::generate_library);
	ClassDB::bind_method(D_METHOD("get_last_build_msec"), &RockGenerator::get_last_build_msec);
	ClassDB::bind_method(D_METHOD("get_last_triangle_count"), &RockGenerator::get_last_triangle_count);
	ClassDB::bind_static_method("RockGenerator", D_METHOD("get_shader_code"), &RockGenerator::get_shader_code);
	ClassDB::bind_static_method("RockGenerator", D_METHOD("get_material", "rock_type"), &RockGenerator::get_material);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "rock_type", PROPERTY_HINT_ENUM, "Granite,Limestone,Sandstone,Basalt,Slate"), "set_rock_type", "get_rock_type");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "family", PROPERTY_HINT_ENUM, "Rockfall,Scree,Glacial,Cobble,Pebble,Cliff Block,Corestone"), "set_family", "get_family");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "seed"), "set_seed", "get_seed");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "resolution", PROPERTY_HINT_RANGE, "0,160,1"), "set_resolution", "get_resolution");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "transport_distance", PROPERTY_HINT_RANGE, "-1,200000,1,or_greater,suffix:m"), "set_transport_distance", "get_transport_distance");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "weathering", PROPERTY_HINT_RANGE, "-1,1,0.01"), "set_weathering", "get_weathering");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "salt", PROPERTY_HINT_RANGE, "-1,1,0.01"), "set_salt", "get_salt");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "detail", PROPERTY_HINT_RANGE, "0,2,0.01"), "set_detail", "get_detail");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "lods"), "set_lods", "get_lods");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "simplify"), "set_simplify", "get_simplify");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "close_up"), "set_close_up", "get_close_up");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "marks_scale", PROPERTY_HINT_RANGE, "0,10,0.001,or_greater,suffix:m"), "set_marks_scale", "get_marks_scale");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "fracture_pieces", PROPERTY_HINT_RANGE, "0,64,1"), "set_fracture_pieces", "get_fracture_pieces");

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
	BIND_ENUM_CONSTANT(FAMILY_CORESTONE);
	BIND_ENUM_CONSTANT(FAMILY_SEA_STACK);
	BIND_ENUM_CONSTANT(FAMILY_SEA_ARCH);
	BIND_ENUM_CONSTANT(FAMILY_SEA_PLATFORM);
	BIND_ENUM_CONSTANT(FAMILY_HOODOO);
	BIND_ENUM_CONSTANT(FAMILY_PAVEMENT);
	BIND_ENUM_CONSTANT(FAMILY_FIN);
	BIND_ENUM_CONSTANT(FAMILY_POTHOLE);
	BIND_ENUM_CONSTANT(FAMILY_GOBLIN);
	BIND_ENUM_CONSTANT(FAMILY_CAVE_MOUTH);
	BIND_ENUM_CONSTANT(FAMILY_MAX);
}

/**************************************************************************/
/*  plant_generator.cpp                                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
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

#include "plant_generator.h"
#include "vegetation_light.h"

#include "scene/resources/surface_tool.h"
#include "plant_species.h"

#include "modules/weather/weather_map_3d.h"

#include "core/object/class_db.h"
#include "scene/resources/3d/importer_mesh.h"

static const float GOLDEN_ANGLE = 2.39996323f; // 137.5 degrees, Vogel (1979).

// Frame for an organ growing along p_forward: z forward, y its upper face, x across.
static Basis organ_frame(const Vector3 &p_forward, const Vector3 &p_up_hint) {
	const Vector3 z = p_forward.normalized();
	Vector3 x = p_up_hint.cross(z);
	if (x.length_squared() < 1e-8f) {
		x = Vector3(1, 0, 0).cross(z);
		if (x.length_squared() < 1e-8f) {
			x = Vector3(0, 0, 1).cross(z);
		}
	}
	x.normalize();
	const Vector3 y = z.cross(x).normalized();
	return Basis(x, y, z);
}

static Vector3 polar_dir(float p_azimuth, float p_elevation) {
	return Vector3(Math::cos(p_azimuth) * Math::cos(p_elevation), Math::sin(p_elevation), Math::sin(p_azimuth) * Math::cos(p_elevation));
}

float PlantGenerator::_leaf_width(const Leaf &p_leaf, float p_t) {
	float t = p_t;
	if (p_leaf.petiole > 0.0f) {
		if (t < p_leaf.petiole) {
			return 0.07f; // The stalk.
		}
		t = (t - p_leaf.petiole) / (1.0f - p_leaf.petiole);
	}
	float w = 0.0f;
	switch (p_leaf.shape) {
		case LEAF_LANCEOLATE:
			w = Math::pow(Math::sin(Math::PI * Math::pow(t, 0.8f)), 0.8f);
			break;
		case LEAF_OVATE:
			w = Math::sin(Math::PI * Math::pow(t, 0.7f));
			break;
		case LEAF_ROUND:
			w = Math::sqrt(MAX(0.0f, Math::sin(Math::PI * Math::pow(t, 1.3f))));
			break;
		case LEAF_LINEAR:
			w = Math::pow(MAX(0.0f, 1.0f - t), 0.55f);
			break;
		case LEAF_SPATULATE:
			w = Math::pow(MAX(0.0f, Math::sin(Math::PI * Math::pow(t, 1.8f))), 0.7f);
			break;
		case LEAF_PALMATE:
			w = Math::sqrt(MAX(0.0f, Math::sin(Math::PI * t)));
			break;
	}
	if (p_leaf.lobes > 0 && p_leaf.lobe_depth > 0.0f) {
		// Teeth pointing back towards the base (dandelion's "lion's teeth"), or round lobes.
		const float f = Math::fmod(t * p_leaf.lobes, 1.0f);
		const float tooth = (p_leaf.shape == LEAF_PALMATE || p_leaf.round_lobes) ? 0.5f - 0.5f * Math::cos(f * Math::TAU) : (1.0f - f) * (1.0f - f);
		w *= 1.0f - p_leaf.lobe_depth * (1.0f - tooth);
	}
	return w;
}

void PlantGenerator::_begin_organ(const Vector3 &p_origin) {
	if (p_origin.y < 0.03f) {
		// Whatever was built so far (some parts push their vertices directly) is the old organ's.
		for (Builder &b : builders) {
			_tag_organ(b);
		}
		organ_bases.push_back(p_origin);
		organ = organ_bases.size(); // 0 stays the plant as a whole.
	}
}

void PlantGenerator::_tag_organ(Builder &p_builder) const {
	const int from = p_builder.organs.size();
	p_builder.organs.resize(p_builder.verts.size());
	int32_t *w = p_builder.organs.ptrw();
	for (int i = from; i < p_builder.verts.size(); i++) {
		w[i] = organ;
	}
}

void PlantGenerator::_add_leaf(Surface p_surface, const Transform3D &p_base, const Leaf &p_leaf, float p_wind_base, float p_ao, const Color &p_tint) {
	Builder &b = builders[p_surface];
	_begin_organ(p_base.origin);
	// Wear (docs/ideas/plant-generator.md, "Aged and irregular leaves"): uneven halves, insect
	// bites out of the margin, dead tips and the yellowing of the older, lower
	// leaves. Drawn from the leaf's own hash, so the plant's shape does not change with it.
	const bool worn = p_surface == SURFACE_LEAF && leaf_wear > 0.0f;
	uint32_t wh = hash_murmur3_one_32(leaf_serial++, hash_murmur3_one_32(uint32_t(seed) * 31u + uint32_t(preset)));
	auto wr = [&wh]() {
		wh = hash_murmur3_one_32(wh);
		return float(wh >> 8) / float(1 << 24);
	};
	const bool narrow = p_leaf.width < p_leaf.length * 0.15f; // Grass-like: tips die back, no bites.
	float asym = 0.0f;
	float age = 0.0f; // Senescence of the whole blade.
	float dieback = 0.0f; // Fraction of the length dead from the tip.
	struct Bite {
		float t = 0.0f;
		float r = 0.0f; // Radius over the leaf's length.
		int side = 1;
	} bites[2];
	int bite_count = 0;
	if (worn) {
		asym = (wr() - 0.5f) * 0.24f * leaf_wear;
		// Older leaves sit low on the shoot and yellow first (sequential senescence); a few anywhere
		// are past their best.
		const float low = 1.0f - CLAMP(p_wind_base, 0.0f, 1.0f);
		age = wr() < leaf_wear * 0.3f * (0.3f + 0.7f * low) ? Math::lerp(0.2f, 0.9f, wr()) : 0.0f;
		if (narrow) {
			dieback = wr() < leaf_wear * 0.6f ? Math::lerp(0.05f, 0.35f, wr() * wr()) : 0.0f;
		} else if (wr() < leaf_wear * 0.55f && p_leaf.length > 0.04f && p_leaf.width > 0.015f) {
			// Only on leaves big enough to show a bite: the extra rows cost triangles.
			// About 7% of the leaf area of temperate plants goes to insects (Kozlov et al. 2015),
			// most of it as bites from the margin.
			bite_count = wr() < 0.35f ? 2 : 1;
			for (int k = 0; k < bite_count; k++) {
				bites[k].t = Math::lerp(0.3f, 0.9f, wr());
				bites[k].r = Math::lerp(0.05f, 0.12f, wr());
				bites[k].side = wr() < 0.5f ? -1 : 1;
			}
		}
	}
	// Bites need rows close enough to draw the notch.
	const int n = MAX(bite_count > 0 ? p_leaf.segments * 2 : p_leaf.segments, 2);
	const int first = b.verts.size();
	Basis f = p_base.basis.orthonormalized();
	Vector3 p = p_base.origin;
	const float step = p_leaf.length / n;
	for (int i = 0; i <= n; i++) {
		const float t = float(i) / n;
		const float lw = _leaf_width(p_leaf, t);
		const float hw = 0.5f * p_leaf.width * lw;
		float side_hw[2] = { hw * (1.0f - asym), hw * (1.0f + asym) }; // Left (c = 0), right (c = 2).
		float bitten[2] = { 0.0f, 0.0f };
		for (int k = 0; k < bite_count; k++) {
			// A round bite whose centre sits a little outside the margin.
			const float r = bites[k].r * p_leaf.length;
			const float dt = (t - bites[k].t) * p_leaf.length;
			if (Math::abs(dt) < r) {
				const int si = bites[k].side < 0 ? 0 : 1;
				const float cut = Math::sqrt(r * r - dt * dt) - r * 0.3f;
				if (cut > 0.0f) {
					side_hw[si] = MAX(side_hw[si] - cut, side_hw[si] * 0.15f);
					bitten[si] = 1.0f;
				}
			}
		}
		const Vector3 x = f.get_column(0);
		const Vector3 y = f.get_column(1);
		const float wind = MIN(1.0f, p_wind_base + (1.0f - p_wind_base) * t);
		const float ao = p_ao * (0.75f + 0.25f * t);
		const Vector3 row[3] = { p - x * side_hw[0] + y * (p_leaf.fold * side_hw[0]), p, p + x * side_hw[1] + y * (p_leaf.fold * side_hw[1]) };
		const float dead_tip = dieback > 0.0f ? Math::smoothstep(1.0f - dieback, 1.0f - dieback * 0.6f, t) : 0.0f;
		for (int c = 0; c < 3; c++) {
			b.verts.push_back(row[c]);
			// Planar across the blade (u = 0.5 at the midrib, 0 and 1 at the widest point), so the
			// texture's veins keep their true shape whatever the outline does.
			const float off = c == 1 ? 0.0f : (p_leaf.width > 0.0f ? side_hw[c / 2] / (0.5f * p_leaf.width) : lw);
			b.uvs.push_back(Vector2(0.5f + (c - 1) * 0.5f * off, t));
			b.uv2s.push_back(Vector2(wind, 0.0f));
			// Alpha carries the browning (0 none, 1 dead): the shader mixes the dead colour in.
			float brown = MAX(age, dead_tip);
			if (c != 1) {
				brown = MAX(brown, bitten[c / 2] * 0.35f); // The bite's rim dries.
			}
			b.colors.push_back(Color(ao * p_tint.r, ao * p_tint.g, ao * p_tint.b, 1.0f - CLAMP(brown, 0.0f, 1.0f)));
		}
		if (i < n) {
			// Curl down along the length, twist about the midrib (parallel transport otherwise).
			f = Basis(f.get_column(2), p_leaf.twist / n) * Basis(x, p_leaf.bend / n) * f;
			f.orthonormalize();
			p += f.get_column(2) * step;
		}
	}
	for (int i = 0; i < n; i++) {
		const int r0 = first + i * 3;
		const int r1 = r0 + 3;
		for (int c = 0; c < 2; c++) {
			b.indices.append_array(PackedInt32Array({ r0 + c, r1 + c, r0 + c + 1, r0 + c + 1, r1 + c, r1 + c + 1 }));
		}
	}
	_tag_organ(b);
}

Transform3D PlantGenerator::_add_stem(const Transform3D &p_base, float p_length, float p_r0, float p_r1, float p_bend, int p_segments, float p_wind_base, Surface p_surface) {
	Builder &b = builders[p_surface];
	_begin_organ(p_base.origin);
	const int sides = 5;
	const int n = MAX(p_segments, 1);
	const int first = b.verts.size();
	Basis f = p_base.basis.orthonormalized();
	Vector3 p = p_base.origin;
	for (int i = 0; i <= n; i++) {
		const float t = float(i) / n;
		const float r = Math::lerp(p_r0, p_r1, t);
		const float wind = MIN(1.0f, p_wind_base + (1.0f - p_wind_base) * t);
		for (int s = 0; s < sides; s++) {
			const float a = s * Math::TAU / sides;
			b.verts.push_back(p + (f.get_column(0) * Math::cos(a) + f.get_column(1) * Math::sin(a)) * r);
			b.uvs.push_back(Vector2(float(s) / sides, t));
			b.uv2s.push_back(Vector2(wind, 0.0f));
			const float ao = 0.7f + 0.3f * t;
			b.colors.push_back(Color(ao, ao, ao));
		}
		if (i < n) {
			f = Basis(f.get_column(0), p_bend / n) * f;
			f.orthonormalize();
			p += f.get_column(2) * (p_length / n);
		}
	}
	for (int i = 0; i < n; i++) {
		for (int s = 0; s < sides; s++) {
			const int a = first + i * sides + s;
			const int c = first + i * sides + (s + 1) % sides;
			b.indices.append_array(PackedInt32Array({ a, c, a + sides, c, c + sides, a + sides }));
		}
	}
	_tag_organ(b);
	plant_height = MAX(plant_height, p.y);
	return Transform3D(f, p);
}

void PlantGenerator::_add_head(Surface p_surface, const Transform3D &p_base, float p_radius, float p_height) {
	// A dome along the base's z: flower centres, clover heads, plantain spikes.
	Builder &b = builders[p_surface];
	const int rings = 4;
	const int sectors = 10;
	const int first = b.verts.size();
	const Basis f = p_base.basis.orthonormalized();
	for (int r = 0; r <= rings; r++) {
		const float u = float(r) / rings; // 0 at the rim, 1 at the top.
		const float rad = p_radius * Math::sqrt(MAX(0.0f, 1.0f - u * u));
		const float h = p_height * u;
		for (int s = 0; s < sectors; s++) {
			const float a = s * Math::TAU / sectors;
			b.verts.push_back(p_base.origin + (f.get_column(0) * Math::cos(a) + f.get_column(1) * Math::sin(a)) * rad + f.get_column(2) * h);
			b.uvs.push_back(Vector2(float(s) / sectors, u));
			b.uv2s.push_back(Vector2(1.0f, 0.0f));
			b.colors.push_back(Color(1, 1, 1));
		}
	}
	for (int r = 0; r < rings; r++) {
		for (int s = 0; s < sectors; s++) {
			const int a = first + r * sectors + s;
			const int c = first + r * sectors + (s + 1) % sectors;
			b.indices.append_array(PackedInt32Array({ a, c, a + sectors, c, c + sectors, a + sectors }));
		}
	}
	plant_height = MAX(plant_height, (p_base.origin + f.get_column(2) * p_height).y);
	_tag_organ(b);
}

void PlantGenerator::_add_ray_flower(const Transform3D &p_top, float p_radius, int p_rays, const Leaf &p_petal) {
	const Basis f = p_top.basis.orthonormalized();
	const float spin = _rand(0.0f, Math::TAU);
	for (int k = 0; k < p_rays; k++) {
		const float a = spin + k * Math::TAU / p_rays + _rand(-0.08f, 0.08f);
		const Vector3 radial = f.get_column(0) * Math::cos(a) + f.get_column(1) * Math::sin(a);
		const float rise = _rand(0.05f, 0.25f);
		const Vector3 fwd = (radial * Math::cos(rise) + f.get_column(2) * Math::sin(rise)).normalized();
		Leaf petal = p_petal;
		petal.length *= _rand(0.85f, 1.1f);
		_add_leaf(SURFACE_PETAL, Transform3D(organ_frame(fwd, f.get_column(2)), p_top.origin + radial * p_radius * 0.8f), petal, 1.0f);
	}
	_add_head(SURFACE_CENTRE, Transform3D(f, p_top.origin - f.get_column(2) * p_radius * 0.15f), p_radius, p_radius * 0.55f);
}

void PlantGenerator::_add_vogel_head(const Transform3D &p_top, float p_radius, int p_florets, const Leaf &p_floret) {
	// Florets packed by Vogel's model: the n-th at angle n * 137.5 degrees, radius c * sqrt(n).
	// Outer florets are longer and lie flatter, inner ones short and upright (dandelion head).
	const Basis f = p_top.basis.orthonormalized();
	for (int n = 1; n <= p_florets; n++) {
		const float u = Math::sqrt(float(n) / p_florets);
		const float a = n * GOLDEN_ANGLE;
		const Vector3 radial = f.get_column(0) * Math::cos(a) + f.get_column(1) * Math::sin(a);
		const float rise = Math::lerp(1.3f, 0.15f, u);
		const Vector3 fwd = (radial * Math::cos(rise) + f.get_column(2) * Math::sin(rise)).normalized();
		Leaf fl = p_floret;
		fl.length *= Math::lerp(0.45f, 1.0f, u);
		_add_leaf(SURFACE_PETAL, Transform3D(organ_frame(fwd, f.get_column(2)), p_top.origin + radial * p_radius * u * 0.35f), fl, 1.0f);
	}
}

void PlantGenerator::_add_cup_flower(const Transform3D &p_top, float p_radius) {
	const Basis f = p_top.basis.orthonormalized();
	Leaf petal;
	petal.shape = LEAF_ROUND;
	petal.length = p_radius;
	petal.width = p_radius * 1.05f;
	petal.fold = 0.35f;
	petal.bend = -0.35f; // Curls up: a cup.
	petal.segments = 6;
	const float spin = _rand(0.0f, Math::TAU);
	for (int k = 0; k < 5; k++) {
		const float a = spin + k * Math::TAU / 5.0f;
		const Vector3 radial = f.get_column(0) * Math::cos(a) + f.get_column(1) * Math::sin(a);
		const float rise = _rand(0.45f, 0.7f);
		const Vector3 fwd = (radial * Math::cos(rise) + f.get_column(2) * Math::sin(rise)).normalized();
		_add_leaf(SURFACE_PETAL, Transform3D(organ_frame(fwd, f.get_column(2)), p_top.origin + radial * p_radius * 0.15f), petal, 1.0f);
	}
	_add_head(SURFACE_CENTRE, p_top, p_radius * 0.3f, p_radius * 0.25f);
}

static float smoothstep_f(float p_a, float p_b, float p_x) {
	return Math::smoothstep(p_a, p_b, p_x);
}

static Transform3D stem_base(float p_azimuth, float p_elevation) {
	return Transform3D(organ_frame(polar_dir(p_azimuth, p_elevation), Vector3(0, 1, 0)), Vector3());
}

// Rosette: leaves at the golden angle around the base; outer (older) leaves longer and flatter,
// inner ones shorter and more upright: size and angle as functions of position (Prusinkiewicz 2001).
void PlantGenerator::_build_dandelion() {
	const int leaves = int(_rand(9.0f, 16.0f));
	const float spin = _rand(0.0f, Math::TAU);
	const float big = _rand(0.13f, 0.22f);
	for (int i = 0; i < leaves; i++) {
		const float u = float(i) / leaves;
		Leaf l;
		l.shape = LEAF_LANCEOLATE;
		l.length = big * (1.0f - 0.45f * u) * _rand(0.85f, 1.1f);
		l.width = l.length * _rand(0.18f, 0.24f);
		l.petiole = 0.12f;
		l.fold = 0.25f;
		l.bend = _rand(0.4f, 0.9f);
		l.twist = _rand(-0.3f, 0.3f);
		l.lobes = int(_rand(4.0f, 7.0f));
		l.lobe_depth = _rand(0.35f, 0.65f);
		l.segments = 9;
		const float elev = Math::lerp(0.2f, 0.75f, u) + _rand(-0.08f, 0.08f);
		_add_leaf(SURFACE_LEAF, Transform3D(organ_frame(polar_dir(spin + i * GOLDEN_ANGLE, elev), Vector3(0, 1, 0)), Vector3(0, 0.004f, 0)), l, 0.0f, Math::lerp(0.75f, 1.0f, 1.0f - u));
	}
	const int stalks = rng.randf() < 0.6f ? 1 : 2;
	for (int s = 0; s < stalks; s++) {
		const Transform3D top = _add_stem(stem_base(_rand(0.0f, Math::TAU), _rand(1.3f, 1.5f)), _rand(0.16f, 0.3f), 0.0025f, 0.002f, _rand(-0.25f, 0.25f), 6, 0.0f);
		Leaf fl;
		fl.shape = LEAF_LINEAR;
		fl.length = 0.017f;
		fl.width = 0.0028f;
		fl.fold = 0.1f;
		fl.bend = -0.2f;
		fl.segments = 2;
		_add_vogel_head(top, 0.02f, 48, fl);
	}
}

void PlantGenerator::_build_plantain() {
	const int leaves = int(_rand(6.0f, 11.0f));
	const float spin = _rand(0.0f, Math::TAU);
	for (int i = 0; i < leaves; i++) {
		const float u = float(i) / leaves;
		Leaf l;
		l.shape = LEAF_LANCEOLATE;
		l.length = _rand(0.14f, 0.24f) * (1.0f - 0.3f * u);
		l.width = l.length * _rand(0.1f, 0.15f);
		l.fold = 0.35f;
		l.bend = _rand(0.5f, 1.0f);
		l.twist = _rand(-0.5f, 0.5f);
		l.segments = 10;
		_add_leaf(SURFACE_LEAF, Transform3D(organ_frame(polar_dir(spin + i * GOLDEN_ANGLE, Math::lerp(0.5f, 1.0f, u)), Vector3(0, 1, 0)), Vector3(0, 0.004f, 0)), l, 0.0f);
	}
	const int stalks = int(_rand(2.0f, 5.0f));
	for (int s = 0; s < stalks; s++) {
		const Transform3D top = _add_stem(stem_base(_rand(0.0f, Math::TAU), _rand(1.25f, 1.5f)), _rand(0.22f, 0.4f), 0.0018f, 0.0014f, _rand(-0.2f, 0.2f), 6, 0.0f);
		// The flower spike: a short dark cylinder with a ring of pale stamens.
		const Transform3D spike = _add_stem(top, 0.025f, 0.0045f, 0.0035f, 0.0f, 2, 1.0f, SURFACE_CENTRE);
		Leaf st;
		st.shape = LEAF_LINEAR;
		st.length = 0.006f;
		st.width = 0.0012f;
		st.segments = 2;
		const Transform3D ring(spike.basis, (top.origin + spike.origin) * 0.5f);
		for (int k = 0; k < 10; k++) {
			const float a = k * Math::TAU / 10.0f;
			const Vector3 radial = ring.basis.get_column(0) * Math::cos(a) + ring.basis.get_column(1) * Math::sin(a);
			_add_leaf(SURFACE_PETAL, Transform3D(organ_frame(radial, ring.basis.get_column(2)), ring.origin + radial * 0.004f), st, 1.0f);
		}
	}
}

void PlantGenerator::_build_clover() {
	// White clover: long petioles from creeping stems, each ending in a whorl of three round
	// leaflets; sometimes a globe flower head (a "phi-ball": florets on a Fibonacci sphere).
	const int petioles = int(_rand(4.0f, 9.0f));
	for (int i = 0; i < petioles; i++) {
		const Vector3 at(_rand(-0.04f, 0.04f), 0.0f, _rand(-0.04f, 0.04f));
		Transform3D base = stem_base(_rand(0.0f, Math::TAU), _rand(1.0f, 1.4f));
		base.origin = at;
		const Transform3D top = _add_stem(base, _rand(0.04f, 0.11f), 0.0012f, 0.001f, _rand(0.2f, 0.7f), 4, 0.0f);
		const float spin = _rand(0.0f, Math::TAU);
		Leaf l;
		l.shape = LEAF_ROUND;
		l.width = _rand(0.016f, 0.022f);
		l.length = l.width * _rand(1.0f, 1.2f);
		l.fold = 0.2f;
		l.bend = 0.2f;
		l.segments = 6;
		for (int k = 0; k < 3; k++) {
			const Vector3 radial = polar_dir(spin + k * Math::TAU / 3.0f, _rand(-0.1f, 0.25f));
			_add_leaf(SURFACE_LEAF, Transform3D(organ_frame(radial, Vector3(0, 1, 0)), top.origin), l, 1.0f);
		}
	}
	if (rng.randf() < 0.65f) {
		const Transform3D top = _add_stem(stem_base(_rand(0.0f, Math::TAU), _rand(1.3f, 1.5f)), _rand(0.09f, 0.15f), 0.0012f, 0.001f, 0.0f, 4, 0.0f);
		Leaf fl;
		fl.shape = LEAF_LINEAR;
		fl.length = 0.007f;
		fl.width = 0.0025f;
		fl.segments = 2;
		fl.bend = 0.4f;
		const int florets = 36;
		for (int n = 0; n < florets; n++) {
			// Fibonacci sphere, upper 85%; lower florets droop (older, as on real clover).
			const float y = 1.0f - 1.7f * (n + 0.5f) / florets;
			const float r = Math::sqrt(MAX(0.0f, 1.0f - y * y));
			const float a = n * GOLDEN_ANGLE;
			const Vector3 d = top.basis.xform(Vector3(Math::cos(a) * r, Math::sin(a) * r, y)).normalized();
			_add_leaf(SURFACE_PETAL, Transform3D(organ_frame(d, top.basis.get_column(2)), top.origin + d * 0.003f), fl, 1.0f);
		}
	}
}

void PlantGenerator::_build_fern() {
	// Fronds from the crown at the golden angle; pinnae alternate along the arching rachis,
	// longest in the middle of the frond (a positional function), lobed.
	const int fronds = int(_rand(4.0f, 8.0f));
	const float spin = _rand(0.0f, Math::TAU);
	const float big = _rand(0.3f, 0.55f);
	for (int i = 0; i < fronds; i++) {
		const float u = float(i) / fronds;
		const float length = big * (1.0f - 0.3f * u) * _rand(0.85f, 1.1f);
		const int segs = 12;
		Basis f = organ_frame(polar_dir(spin + i * GOLDEN_ANGLE, Math::lerp(0.95f, 1.25f, u)), Vector3(0, 1, 0));
		Vector3 p(0, 0.01f, 0);
		const float arch = _rand(1.0f, 1.6f);
		LocalVector<Transform3D> frames;
		// Rachis, stepped here so the pinnae can sit on it.
		const Transform3D start(f, p);
		for (int k = 0; k <= segs; k++) {
			frames.push_back(Transform3D(f, p));
			if (k < segs) {
				f = Basis(f.get_column(0), arch / segs) * f;
				f.orthonormalize();
				p += f.get_column(2) * (length / segs);
			}
		}
		_add_stem(start, length, 0.002f, 0.0008f, arch, segs, 0.0f);
		const int pinnae = int(_rand(14.0f, 22.0f));
		for (int k = 0; k < pinnae; k++) {
			const float s = Math::lerp(0.18f, 0.97f, float(k) / (pinnae - 1));
			const float fi = s * segs;
			const int fa = MIN(int(fi), segs - 1);
			const Transform3D fr = frames[fa].interpolate_with(frames[fa + 1], fi - fa);
			const float side = (k % 2 == 0) ? 1.0f : -1.0f;
			const float plen = length * 0.32f * Math::pow(MAX(0.0f, Math::sin(Math::PI * (s - 0.1f) / 0.92f)), 0.6f) * (1.0f - 0.25f * s);
			if (plen < 0.004f) {
				continue;
			}
			const Vector3 fwd = (fr.basis.get_column(0) * side * 0.8f + fr.basis.get_column(2) * 0.6f).normalized();
			Leaf l;
			l.shape = LEAF_LANCEOLATE;
			l.length = plen;
			l.width = plen * 0.28f;
			l.fold = 0.15f;
			l.bend = 0.35f;
			l.lobes = 7;
			l.lobe_depth = 0.45f;
			l.segments = 8;
			_add_leaf(SURFACE_LEAF, Transform3D(organ_frame(fwd, fr.basis.get_column(1)), fr.origin), l, s);
		}
	}
}

void PlantGenerator::_build_sedge() {
	const int blades = int(_rand(14.0f, 28.0f));
	for (int i = 0; i < blades; i++) {
		Leaf l;
		l.shape = LEAF_LINEAR;
		l.length = _rand(0.2f, 0.45f);
		l.width = _rand(0.004f, 0.007f);
		l.fold = 0.6f; // V-shaped in section, as sedges are.
		l.bend = _rand(0.6f, 1.8f);
		l.twist = _rand(-0.8f, 0.8f);
		l.segments = 8;
		const Vector3 at(_rand(-0.015f, 0.015f), 0.0f, _rand(-0.015f, 0.015f));
		_add_leaf(SURFACE_LEAF, Transform3D(organ_frame(polar_dir(_rand(0.0f, Math::TAU), _rand(1.0f, 1.45f)), Vector3(0, 1, 0)), at), l, 0.0f, 0.85f);
	}
	const int culms = int(_rand(0.0f, 3.0f));
	for (int c = 0; c < culms; c++) {
		const Transform3D top = _add_stem(stem_base(_rand(0.0f, Math::TAU), _rand(1.35f, 1.55f)), _rand(0.28f, 0.45f), 0.0015f, 0.001f, _rand(-0.3f, 0.3f), 6, 0.0f);
		for (int k = 0; k < 3; k++) {
			const Vector3 d = top.basis.xform(Vector3(Math::cos(k * 2.1f) * 0.5f, Math::sin(k * 2.1f) * 0.5f, 1.0f)).normalized();
			_add_head(SURFACE_CENTRE, Transform3D(organ_frame(d, Vector3(0, 1, 0)), top.origin + d * 0.004f * k), 0.0025f, 0.012f);
		}
	}
}

void PlantGenerator::_build_daisy() {
	const int basal = int(_rand(4.0f, 9.0f));
	const float spin = _rand(0.0f, Math::TAU);
	for (int i = 0; i < basal; i++) {
		Leaf l;
		l.shape = LEAF_SPATULATE;
		l.length = _rand(0.05f, 0.09f);
		l.width = l.length * 0.35f;
		l.petiole = 0.3f;
		l.lobes = 3;
		l.lobe_depth = 0.2f;
		l.fold = 0.15f;
		l.bend = _rand(0.3f, 0.7f);
		l.segments = 8;
		_add_leaf(SURFACE_LEAF, Transform3D(organ_frame(polar_dir(spin + i * GOLDEN_ANGLE, _rand(0.2f, 0.55f)), Vector3(0, 1, 0)), Vector3(0, 0.003f, 0)), l, 0.0f);
	}
	const int stems = int(_rand(1.0f, 4.0f));
	for (int s = 0; s < stems; s++) {
		const Transform3D base = stem_base(_rand(0.0f, Math::TAU), _rand(1.35f, 1.52f));
		const float len = _rand(0.25f, 0.45f);
		const float bend = _rand(-0.2f, 0.2f);
		const Transform3D top = _add_stem(base, len, 0.0018f, 0.0014f, bend, 8, 0.0f);
		// Two small stem leaves.
		for (int k = 0; k < 2; k++) {
			const Transform3D at = base.interpolate_with(top, 0.3f + 0.25f * k);
			const float side = k == 0 ? 1.0f : -1.0f;
			Leaf sl;
			sl.shape = LEAF_LANCEOLATE;
			sl.length = 0.03f;
			sl.width = 0.006f;
			sl.lobes = 3;
			sl.lobe_depth = 0.3f;
			sl.bend = 0.3f;
			sl.segments = 5;
			_add_leaf(SURFACE_LEAF, Transform3D(organ_frame(at.basis.get_column(0) * side + at.basis.get_column(2) * 0.6f, at.basis.get_column(1)), at.origin), sl, 0.4f + 0.2f * k);
		}
		Leaf petal;
		petal.shape = LEAF_SPATULATE;
		petal.length = _rand(0.02f, 0.026f);
		petal.width = 0.0055f;
		petal.fold = 0.05f;
		petal.bend = 0.15f;
		petal.segments = 4;
		_add_ray_flower(top, 0.008f, int(_rand(20.0f, 30.0f)), petal);
	}
}

void PlantGenerator::_build_buttercup() {
	const int basal = int(_rand(3.0f, 7.0f));
	const float spin = _rand(0.0f, Math::TAU);
	for (int i = 0; i < basal; i++) {
		Leaf l;
		l.shape = LEAF_PALMATE;
		l.length = _rand(0.08f, 0.13f);
		l.width = l.length * 0.6f;
		l.petiole = 0.5f;
		l.lobes = 3;
		l.lobe_depth = 0.55f;
		l.fold = 0.2f;
		l.bend = _rand(0.4f, 0.9f);
		l.segments = 12;
		_add_leaf(SURFACE_LEAF, Transform3D(organ_frame(polar_dir(spin + i * GOLDEN_ANGLE, _rand(0.6f, 1.0f)), Vector3(0, 1, 0)), Vector3(0, 0.003f, 0)), l, 0.0f);
	}
	const int stems = int(_rand(1.0f, 3.0f));
	for (int s = 0; s < stems; s++) {
		const Transform3D base = stem_base(_rand(0.0f, Math::TAU), _rand(1.35f, 1.52f));
		const Transform3D top = _add_stem(base, _rand(0.28f, 0.48f), 0.0018f, 0.0013f, _rand(-0.2f, 0.2f), 8, 0.0f);
		_add_cup_flower(top, _rand(0.012f, 0.016f));
		// A branch with a second flower, from two thirds up.
		if (rng.randf() < 0.7f) {
			Transform3D fork = base.interpolate_with(top, 0.65f);
			fork.basis = organ_frame(fork.basis.get_column(2) + fork.basis.get_column(0) * _rand(-0.8f, 0.8f), Vector3(0, 1, 0));
			const Transform3D top2 = _add_stem(fork, _rand(0.08f, 0.14f), 0.0012f, 0.001f, 0.2f, 4, 0.6f);
			_add_cup_flower(top2, _rand(0.011f, 0.014f));
		}
	}
}

// The frames along a stem, matching _add_stem's path, for organs placed on it.
LocalVector<Transform3D> PlantGenerator::_stem_frames(const Transform3D &p_base, float p_length, float p_bend, int p_segments) const {
	LocalVector<Transform3D> frames;
	const int n = MAX(p_segments, 1);
	Basis f = p_base.basis.orthonormalized();
	Vector3 p = p_base.origin;
	for (int i = 0; i <= n; i++) {
		frames.push_back(Transform3D(f, p));
		if (i < n) {
			f = Basis(f.get_column(0), p_bend / n) * f;
			f.orthonormalize();
			p += f.get_column(2) * (p_length / n);
		}
	}
	return frames;
}

Transform3D PlantGenerator::_frame_at(const LocalVector<Transform3D> &p_frames, float p_s) const {
	const int segs = int(p_frames.size()) - 1;
	const float fi = CLAMP(p_s, 0.0f, 1.0f) * segs;
	const int a = MIN(int(fi), segs - 1);
	return p_frames[a].interpolate_with(p_frames[a + 1], fi - a);
}

// Grass blades from one crown, leaning out and arching over (Poaceae tussock).
void PlantGenerator::_add_grass_tuft(int p_blades, float p_length_min, float p_length_max, float p_width_min, float p_width_max, float p_spread) {
	for (int i = 0; i < p_blades; i++) {
		Leaf l;
		l.shape = LEAF_LINEAR;
		l.length = _rand(p_length_min, p_length_max);
		l.width = _rand(p_width_min, p_width_max);
		l.fold = 0.35f;
		l.bend = _rand(0.5f, 1.6f);
		l.twist = _rand(-0.6f, 0.6f);
		l.segments = 7;
		const Vector3 at(_rand(-p_spread, p_spread), 0.0f, _rand(-p_spread, p_spread));
		const float shade = _rand(0.85f, 1.1f);
		_add_leaf(SURFACE_LEAF, Transform3D(organ_frame(polar_dir(_rand(0.0f, Math::TAU), _rand(1.05f, 1.45f)), Vector3(0, 1, 0)), at), l, 0.0f, 0.85f, Color(shade, shade, shade * 0.95f));
	}
}

// Stinging nettle (Urtica dioica): a clonal patch of erect, unbranched square stems; opposite,
// decussate leaves, heart-shaped and coarsely toothed with a drawn-out tip, largest low on the
// stem; drooping strings of tiny green flowers from the upper leaf axils.
void PlantGenerator::_build_nettle() {
	const int stems = int(_rand(3.0f, 7.0f));
	for (int s = 0; s < stems; s++) {
		Transform3D base = stem_base(_rand(0.0f, Math::TAU), _rand(1.38f, 1.55f));
		base.origin = Vector3(_rand(-0.08f, 0.08f), 0.0f, _rand(-0.08f, 0.08f));
		const float len = _rand(0.4f, 0.85f);
		const float bend = _rand(-0.2f, 0.2f);
		const LocalVector<Transform3D> frames = _stem_frames(base, len, bend, 10);
		_add_stem(base, len, 0.0032f, 0.0016f, bend, 10, 0.0f);
		const int nodes = int(_rand(6.0f, 10.0f));
		float spin = _rand(0.0f, Math::TAU);
		for (int k = 0; k < nodes; k++) {
			const float u = (k + 0.6f) / nodes;
			const Transform3D at = _frame_at(frames, u);
			const float size_u = Math::lerp(1.0f, 0.45f, u * u);
			for (int side = 0; side < 2; side++) {
				const float a = spin + side * Math::PI;
				const Vector3 radial = at.basis.get_column(0) * Math::cos(a) + at.basis.get_column(1) * Math::sin(a);
				const Vector3 fwd = (radial + at.basis.get_column(2) * _rand(0.1f, 0.4f)).normalized();
				Leaf l;
				l.shape = LEAF_OVATE;
				l.length = _rand(0.07f, 0.1f) * size_u;
				l.width = l.length * _rand(0.55f, 0.65f);
				l.petiole = 0.22f;
				l.lobes = 9;
				l.lobe_depth = 0.12f;
				l.fold = 0.2f;
				l.bend = _rand(0.6f, 1.1f);
				l.segments = 10;
				_add_leaf(SURFACE_LEAF, Transform3D(organ_frame(fwd, at.basis.get_column(2)), at.origin), l, u * 0.8f, Math::lerp(0.8f, 1.0f, u));
				if (u > 0.45f && rng.randf() < 0.75f) {
					// Flower string hanging from the axil.
					Transform3D fb(organ_frame(radial * 0.6f - at.basis.get_column(2) * 0.4f + Vector3(0, -0.3f, 0), Vector3(0, 1, 0)), at.origin + radial * 0.004f);
					const Transform3D tip = _add_stem(fb, _rand(0.03f, 0.06f), 0.0012f, 0.0012f, 0.3f, 3, u, SURFACE_CENTRE);
					(void)tip;
				}
			}
			spin += Math::PI * 0.5f; // Decussate: each pair at right angles to the last.
		}
	}
}

// Spear thistle (Cirsium vulgare): a rosette of deeply lobed, spine-tipped leaves, then a
// branched flowering stem with clasping spiny leaves and egg-shaped heads (green, spiny
// involucre) topped by a purple brush of florets.
void PlantGenerator::_build_thistle() {
	const int basal = int(_rand(6.0f, 11.0f));
	const float spin = _rand(0.0f, Math::TAU);
	for (int i = 0; i < basal; i++) {
		const float u = float(i) / basal;
		Leaf l;
		l.shape = LEAF_LANCEOLATE;
		l.length = _rand(0.14f, 0.24f) * (1.0f - 0.35f * u);
		l.width = l.length * _rand(0.3f, 0.38f);
		l.lobes = int(_rand(5.0f, 8.0f));
		l.lobe_depth = _rand(0.55f, 0.75f);
		l.fold = 0.3f;
		l.bend = _rand(0.3f, 0.7f);
		l.twist = _rand(-0.4f, 0.4f);
		l.segments = 12;
		_add_leaf(SURFACE_LEAF, Transform3D(organ_frame(polar_dir(spin + i * GOLDEN_ANGLE, Math::lerp(0.15f, 0.6f, u)), Vector3(0, 1, 0)), Vector3(0, 0.004f, 0)), l, 0.0f, Math::lerp(0.8f, 1.0f, 1.0f - u));
	}
	if (rng.randf() < 0.2f) {
		return; // A first-year rosette: no stem yet.
	}
	const Transform3D base = stem_base(_rand(0.0f, Math::TAU), _rand(1.42f, 1.56f));
	const float len = _rand(0.4f, 0.75f);
	const LocalVector<Transform3D> frames = _stem_frames(base, len, 0.1f, 10);
	const Transform3D top = _add_stem(base, len, 0.004f, 0.0022f, 0.1f, 10, 0.0f);
	const int leaves = int(_rand(5.0f, 9.0f));
	for (int k = 0; k < leaves; k++) {
		const float u = 0.12f + 0.7f * float(k) / leaves;
		const Transform3D at = _frame_at(frames, u);
		const float a = spin + k * GOLDEN_ANGLE;
		const Vector3 radial = at.basis.get_column(0) * Math::cos(a) + at.basis.get_column(1) * Math::sin(a);
		Leaf l;
		l.shape = LEAF_LANCEOLATE;
		l.length = _rand(0.07f, 0.12f) * (1.0f - 0.5f * u);
		l.width = l.length * 0.35f;
		l.lobes = 5;
		l.lobe_depth = 0.65f;
		l.fold = 0.3f;
		l.bend = 0.5f;
		l.segments = 8;
		_add_leaf(SURFACE_LEAF, Transform3D(organ_frame((radial + at.basis.get_column(2) * 0.5f).normalized(), at.basis.get_column(2)), at.origin), l, u);
	}
	auto add_head = [&](const Transform3D &p_top, float p_r) {
		// Involucre: a dome of the stem colour; the florets: a brush of thin purple strands.
		_add_head(SURFACE_STEM, Transform3D(p_top.basis, p_top.origin - p_top.basis.get_column(2) * p_r * 0.6f), p_r, p_r * 1.8f);
		Leaf fl;
		fl.shape = LEAF_LINEAR;
		fl.length = p_r * 1.6f;
		fl.width = 0.0016f;
		fl.fold = 0.0f;
		fl.bend = -0.1f;
		fl.segments = 2;
		const Basis f = p_top.basis;
		for (int n = 0; n < 22; n++) {
			const float uu = Math::sqrt((n + 0.5f) / 22.0f);
			const float a = n * GOLDEN_ANGLE;
			const Vector3 radial = f.get_column(0) * Math::cos(a) + f.get_column(1) * Math::sin(a);
			const Vector3 d = (f.get_column(2) + radial * uu * 0.7f).normalized();
			_add_leaf(SURFACE_PETAL, Transform3D(organ_frame(d, f.get_column(2)), p_top.origin + f.get_column(2) * p_r * 0.9f + radial * p_r * uu * 0.6f), fl, 1.0f);
		}
	};
	add_head(top, _rand(0.011f, 0.015f));
	const int branches = int(_rand(1.0f, 4.0f));
	for (int b = 0; b < branches; b++) {
		Transform3D at = _frame_at(frames, _rand(0.65f, 0.85f));
		const float a = _rand(0.0f, Math::TAU);
		const Vector3 radial = at.basis.get_column(0) * Math::cos(a) + at.basis.get_column(1) * Math::sin(a);
		at.basis = organ_frame(at.basis.get_column(2) + radial * 0.8f, Vector3(0, 1, 0));
		const Transform3D t2 = _add_stem(at, _rand(0.06f, 0.14f), 0.0018f, 0.0014f, -0.4f, 4, 0.7f);
		add_head(t2, _rand(0.009f, 0.013f));
	}
}

// Yarrow (Achillea millefolium): feathery, finely divided leaves ("millefolium", a thousand
// leaves) as a rachis with many thread-like segments; a stiff stem ending in a flat-topped
// corymb of small white heads, each a few short rays around a disc.
void PlantGenerator::_build_yarrow() {
	auto feather = [&](const Transform3D &p_base, float p_length, float p_wind, float p_droop) {
		const int segs = 6;
		const LocalVector<Transform3D> fr = _stem_frames(p_base, p_length, p_droop, segs);
		_add_stem(p_base, p_length, 0.0009f, 0.0004f, p_droop, segs, p_wind);
		const int pinnae = int(p_length / 0.005f);
		for (int k = 0; k < pinnae; k++) {
			const float s = 0.1f + 0.88f * float(k) / MAX(pinnae - 1, 1);
			const Transform3D at = _frame_at(fr, s);
			const float side = (k % 2 == 0) ? 1.0f : -1.0f;
			Leaf l;
			l.shape = LEAF_LINEAR;
			l.length = p_length * 0.16f * Math::sin(Math::PI * MIN(s * 1.1f, 1.0f)) + 0.003f;
			l.width = 0.0042f;
			l.fold = 0.1f;
			l.bend = 0.3f;
			l.lobes = 3;
			l.lobe_depth = 0.6f;
			l.segments = 2;
			_add_leaf(SURFACE_LEAF, Transform3D(organ_frame((at.basis.get_column(0) * side + at.basis.get_column(2) * 0.5f).normalized(), at.basis.get_column(1)), at.origin), l, Math::lerp(p_wind, 1.0f, s));
		}
	};
	const int basal = int(_rand(5.0f, 10.0f));
	const float spin = _rand(0.0f, Math::TAU);
	for (int i = 0; i < basal; i++) {
		const float u = float(i) / basal;
		feather(Transform3D(organ_frame(polar_dir(spin + i * GOLDEN_ANGLE, Math::lerp(0.4f, 1.0f, u)), Vector3(0, 1, 0)), Vector3(0, 0.003f, 0)), _rand(0.07f, 0.14f), 0.0f, _rand(0.4f, 0.9f));
	}
	const int stems = int(_rand(1.0f, 3.0f));
	for (int s = 0; s < stems; s++) {
		const Transform3D base = stem_base(_rand(0.0f, Math::TAU), _rand(1.42f, 1.56f));
		const float len = _rand(0.3f, 0.55f);
		const LocalVector<Transform3D> frames = _stem_frames(base, len, 0.0f, 8);
		const Transform3D top = _add_stem(base, len, 0.0022f, 0.0015f, 0.0f, 8, 0.0f);
		for (int k = 0; k < 4; k++) {
			const Transform3D at = _frame_at(frames, 0.2f + 0.15f * k);
			const float a = k * GOLDEN_ANGLE;
			const Vector3 radial = at.basis.get_column(0) * Math::cos(a) + at.basis.get_column(1) * Math::sin(a);
			feather(Transform3D(organ_frame((radial + at.basis.get_column(2) * 0.6f).normalized(), at.basis.get_column(2)), at.origin), _rand(0.03f, 0.06f), 0.3f + 0.15f * k, 0.3f);
		}
		// Corymb: pedicels from just below the top, their heads level in a flat dome.
		const int heads = int(_rand(12.0f, 22.0f));
		const float span = _rand(0.025f, 0.04f);
		for (int h = 0; h < heads; h++) {
			const float r = span * Math::sqrt((h + 0.5f) / heads);
			const float a = h * GOLDEN_ANGLE;
			const Vector3 tip_pos = top.origin + Vector3(Math::cos(a) * r, 0.012f - 40.0f * r * r * 0.15f, Math::sin(a) * r);
			const Vector3 from = top.origin - Vector3(0, 0.01f, 0);
			const Vector3 d = tip_pos - from;
			const Transform3D ped(organ_frame(d, Vector3(1, 0, 0)), from);
			const Transform3D ht = _add_stem(ped, d.length(), 0.0005f, 0.0004f, 0.0f, 1, 0.9f);
			const Transform3D up(organ_frame(Vector3(0, 1, 0), Vector3(1, 0, 0)), ht.origin);
			Leaf ray;
			ray.shape = LEAF_ROUND;
			ray.length = 0.0018f;
			ray.width = 0.002f;
			ray.fold = 0.1f;
			ray.bend = 0.2f;
			ray.segments = 2;
			const float rs = _rand(0.0f, Math::TAU);
			for (int k = 0; k < 5; k++) {
				const Vector3 radial = polar_dir(rs + k * Math::TAU / 5.0f, 0.15f);
				_add_leaf(SURFACE_PETAL, Transform3D(organ_frame(radial, Vector3(0, 1, 0)), ht.origin + radial * 0.0012f), ray, 1.0f);
			}
		}
	}
}

// Broad-leaved dock (Rumex obtusifolius): big oblong basal leaves on long stalks with wavy
// margins and reddish veins, and a tall ribbed stem that turns rust-brown as its whorls of
// winged seeds ripen along the upper branches.
void PlantGenerator::_build_dock() {
	const int basal = int(_rand(4.0f, 8.0f));
	const float spin = _rand(0.0f, Math::TAU);
	for (int i = 0; i < basal; i++) {
		const float u = float(i) / basal;
		Leaf l;
		l.shape = LEAF_OVATE;
		l.length = _rand(0.2f, 0.32f) * (1.0f - 0.35f * u);
		l.width = l.length * _rand(0.38f, 0.48f);
		l.petiole = 0.28f;
		l.lobes = 7;
		l.lobe_depth = 0.07f;
		l.fold = 0.25f;
		l.bend = _rand(0.5f, 1.0f);
		l.twist = _rand(-0.4f, 0.4f);
		l.segments = 12;
		_add_leaf(SURFACE_LEAF, Transform3D(organ_frame(polar_dir(spin + i * GOLDEN_ANGLE, Math::lerp(0.45f, 0.95f, u)), Vector3(0, 1, 0)), Vector3(0, 0.004f, 0)), l, 0.0f, Math::lerp(0.8f, 1.0f, 1.0f - u));
	}
	if (rng.randf() < 0.3f) {
		return;
	}
	const Transform3D base = stem_base(_rand(0.0f, Math::TAU), _rand(1.45f, 1.56f));
	const float len = _rand(0.5f, 0.8f);
	const LocalVector<Transform3D> frames = _stem_frames(base, len, 0.0f, 10);
	_add_stem(base, len, 0.0045f, 0.002f, 0.0f, 10, 0.0f, SURFACE_CENTRE);
	Leaf seed;
	seed.shape = LEAF_ROUND;
	seed.length = 0.004f;
	seed.width = 0.004f;
	seed.fold = 0.3f;
	seed.bend = 0.0f;
	seed.segments = 2;
	auto whorls = [&](const LocalVector<Transform3D> &p_fr, float p_from, float p_to, float p_wind) {
		const int n = int((p_to - p_from) * 26.0f) + 2;
		for (int k = 0; k < n; k++) {
			const float s = Math::lerp(p_from, p_to, float(k) / (n - 1));
			const Transform3D at = _frame_at(p_fr, s);
			const float a0 = _rand(0.0f, Math::TAU);
			for (int j = 0; j < 4; j++) {
				const float a = a0 + j * Math::TAU / 4.0f;
				const Vector3 radial = at.basis.get_column(0) * Math::cos(a) + at.basis.get_column(1) * Math::sin(a);
				_add_leaf(SURFACE_PETAL, Transform3D(organ_frame((radial - at.basis.get_column(2) * 0.6f).normalized(), at.basis.get_column(2)), at.origin + radial * 0.002f), seed, p_wind);
			}
		}
	};
	whorls(frames, 0.55f, 1.0f, 0.6f);
	const int branches = int(_rand(2.0f, 5.0f));
	for (int b = 0; b < branches; b++) {
		Transform3D at = _frame_at(frames, _rand(0.5f, 0.8f));
		const float a = _rand(0.0f, Math::TAU);
		const Vector3 radial = at.basis.get_column(0) * Math::cos(a) + at.basis.get_column(1) * Math::sin(a);
		at.basis = organ_frame(at.basis.get_column(2) + radial * 0.5f, Vector3(0, 1, 0));
		const float bl = _rand(0.1f, 0.2f);
		const LocalVector<Transform3D> bf = _stem_frames(at, bl, -0.2f, 4);
		_add_stem(at, bl, 0.0018f, 0.0012f, -0.2f, 4, 0.6f, SURFACE_CENTRE);
		whorls(bf, 0.15f, 1.0f, 0.8f);
	}
}

// Cock's-foot (Dactylis glomerata): a coarse, bluish tussock of broad folded blades; culms with
// a one-sided panicle of dense spikelet clusters on stiff spreading branches.
void PlantGenerator::_build_cocksfoot() {
	_add_grass_tuft(int(_rand(16.0f, 28.0f)), 0.18f, 0.38f, 0.006f, 0.01f, 0.025f);
	const int culms = int(_rand(2.0f, 6.0f));
	Leaf spk;
	spk.shape = LEAF_LANCEOLATE;
	spk.length = 0.008f;
	spk.width = 0.003f;
	spk.fold = 0.5f;
	spk.bend = 0.1f;
	spk.segments = 2;
	for (int c = 0; c < culms; c++) {
		const Transform3D base = stem_base(_rand(0.0f, Math::TAU), _rand(1.3f, 1.5f));
		const float len = _rand(0.45f, 0.8f);
		const float bend = _rand(0.1f, 0.4f);
		const LocalVector<Transform3D> fr = _stem_frames(base, len, bend, 9);
		const Transform3D top = _add_stem(base, len, 0.0018f, 0.0011f, bend, 9, 0.0f);
		auto cluster = [&](const Transform3D &p_at, int p_n, float p_wind) {
			for (int k = 0; k < p_n; k++) {
				const Vector3 d = p_at.basis.xform(Vector3(_rand(-0.6f, 0.6f), _rand(-0.6f, 0.6f), 1.0f)).normalized();
				_add_leaf(SURFACE_PETAL, Transform3D(organ_frame(d, Vector3(0, 1, 0)), p_at.origin + d * 0.002f), spk, p_wind);
			}
		};
		cluster(top, 10, 1.0f);
		const int branches = int(_rand(2.0f, 4.0f));
		for (int b = 0; b < branches; b++) {
			Transform3D at = _frame_at(fr, 0.82f + 0.05f * b);
			const float side = (b % 2 == 0) ? 1.0f : -1.0f;
			at.basis = organ_frame(at.basis.get_column(2) + at.basis.get_column(0) * side * 1.1f, Vector3(0, 1, 0));
			const Transform3D bt = _add_stem(at, _rand(0.02f, 0.05f), 0.0008f, 0.0006f, 0.0f, 2, 0.9f);
			cluster(bt, int(_rand(6.0f, 10.0f)), 1.0f);
		}
	}
}

// Timothy (Phleum pratense): a looser tuft of narrower blades; tall straight culms ending in a
// dense cylindrical spike.
void PlantGenerator::_build_timothy() {
	_add_grass_tuft(int(_rand(12.0f, 22.0f)), 0.15f, 0.32f, 0.004f, 0.007f, 0.02f);
	const int culms = int(_rand(2.0f, 6.0f));
	for (int c = 0; c < culms; c++) {
		const Transform3D base = stem_base(_rand(0.0f, Math::TAU), _rand(1.38f, 1.54f));
		const Transform3D top = _add_stem(base, _rand(0.45f, 0.8f), 0.0016f, 0.0012f, _rand(-0.15f, 0.15f), 9, 0.0f);
		_add_stem(top, _rand(0.05f, 0.1f), 0.0032f, 0.0026f, 0.0f, 3, 1.0f, SURFACE_CENTRE);
	}
}

// Dead grass: last year's straw. Blades bent, broken and lodged flat, a few still green at the
// base; this is what keeps a real meadow from looking freshly mown.
void PlantGenerator::_build_dry_grass() {
	const int blades = int(_rand(18.0f, 34.0f));
	for (int i = 0; i < blades; i++) {
		Leaf l;
		l.shape = LEAF_LINEAR;
		const bool broken = rng.randf() < 0.3f;
		const bool lodged = rng.randf() < 0.45f;
		l.length = _rand(0.1f, 0.35f) * (broken ? 0.45f : 1.0f);
		l.width = _rand(0.003f, 0.006f);
		l.fold = 0.5f;
		l.bend = lodged ? _rand(0.05f, 0.4f) : _rand(0.3f, 1.0f);
		l.twist = _rand(-1.2f, 1.2f);
		l.segments = 6;
		const Vector3 at(_rand(-0.04f, 0.04f), 0.0f, _rand(-0.04f, 0.04f));
		const float elev = lodged ? _rand(0.1f, 0.35f) : _rand(0.9f, 1.4f);
		const float k = _rand(0.75f, 1.2f);
		const Color tint = rng.randf() < 0.15f ? Color(0.75f, 1.05f, 0.7f) : Color(k, k * _rand(0.92f, 1.02f), k * _rand(0.85f, 1.0f));
		_add_leaf(SURFACE_LEAF, Transform3D(organ_frame(polar_dir(_rand(0.0f, Math::TAU), elev), Vector3(0, 1, 0)), at), l, 0.0f, 0.8f, tint);
	}
}

// Leaf litter: fallen leaves of several shapes lying on the ground, edges curled as they dry,
// in browns, ochres and the odd late yellow; a couple of twigs and dry blades among them.
void PlantGenerator::_build_leaf_litter() {
	if (!litter_leaves.is_empty()) {
		_build_stand_litter();
	} else {
		const int leaves = int(_rand(14.0f, 26.0f));
		for (int i = 0; i < leaves; i++) {
			Leaf l;
			const float pick = rng.randf();
			l.shape = pick < 0.4f ? LEAF_OVATE : (pick < 0.7f ? LEAF_LANCEOLATE : LEAF_ROUND);
			l.length = _rand(0.035f, 0.08f);
			l.width = l.length * (l.shape == LEAF_LANCEOLATE ? 0.35f : 0.65f);
			l.petiole = 0.1f;
			if (pick > 0.85f) {
				l.lobes = 3; // Oak-like.
				l.lobe_depth = 0.35f;
			}
			l.fold = _rand(-0.4f, 0.5f); // Curled up or cupped down as it dried.
			l.bend = _rand(-0.6f, 0.6f);
			l.twist = _rand(-0.5f, 0.5f);
			l.segments = 6;
			const float r = 0.13f * Math::sqrt(rng.randf());
			const float a = _rand(0.0f, Math::TAU);
			const Vector3 at(Math::cos(a) * r, 0.002f + 0.004f * rng.randf(), Math::sin(a) * r);
			const float k = _rand(0.6f, 1.25f);
			const float hue = rng.randf();
			const Color tint = hue < 0.15f ? Color(1.5f, 1.3f, 0.5f) : (hue < 0.35f ? Color(1.3f, 0.8f, 0.5f) : Color(k, k * 0.95f, k * 0.9f));
			// Lying nearly flat, tilted a little; upper or under side up.
			const Vector3 fwd = polar_dir(_rand(0.0f, Math::TAU), _rand(-0.12f, 0.12f));
			const Vector3 up = rng.randf() < 0.7f ? Vector3(0, 1, 0) : Vector3(0, -1, 0);
			_add_leaf(SURFACE_LEAF, Transform3D(organ_frame(fwd, up), at), l, 0.15f, 0.85f, tint);
		}
	}
	const int twigs = int(_rand(0.0f, 4.0f));
	for (int t = 0; t < twigs; t++) {
		Transform3D b(organ_frame(polar_dir(_rand(0.0f, Math::TAU), _rand(-0.05f, 0.05f)), Vector3(0, 1, 0)), Vector3(_rand(-0.1f, 0.1f), 0.003f, _rand(-0.1f, 0.1f)));
		const Transform3D e = _add_stem(b, _rand(0.06f, 0.18f), 0.0025f, 0.0012f, _rand(-0.3f, 0.3f), 3, 0.05f);
		if (rng.randf() < 0.6f) {
			Transform3D side(organ_frame(e.basis.get_column(2) + e.basis.get_column(0) * 0.7f, Vector3(0, 1, 0)), b.origin.lerp(e.origin, 0.6f));
			_add_stem(side, _rand(0.03f, 0.06f), 0.0012f, 0.0007f, 0.0f, 2, 0.05f);
		}
	}
}

// The stand's own fallen leaves (docs/ideas/forest-floor.md, step 6): each entry of litter_leaves
// is a leaf form as TreeGenerator.get_leaf_form() gives it (kind, count, depth, width as half width
// over length, size in metres) with the stand's share of the litter ("share"), its fallen colour
// ("color", the autumn colour) and the share of this year's leaves still fresh ("fresh"). Fresh
// leaves keep that colour; older ones fade to a dull tan brown and curl. A card
// carries about 1.3 layers of leaves over its disc, so big leaves come few and small ones many.
void PlantGenerator::_build_stand_litter() {
	float total = 0.0f;
	for (int i = 0; i < litter_leaves.size(); i++) {
		total += float(Dictionary(litter_leaves[i]).get("share", 1.0f));
	}
	if (total <= 0.0f) {
		return;
	}
	const float disc = 0.13f;
	float area_left = 1.3f * Math::PI * disc * disc;
	int guard = 0;
	while (area_left > 0.0f && guard++ < 60) {
		float pick = rng.randf() * total;
		Dictionary f = litter_leaves[0];
		for (int i = 0; i < litter_leaves.size(); i++) {
			f = litter_leaves[i];
			pick -= float(f.get("share", 1.0f));
			if (pick <= 0.0f) {
				break;
			}
		}
		const int kind = int(f.get("kind", 0));
		const int count = int(f.get("count", 0));
		const float depth = float(f.get("depth", 0.0f));
		const float half_w = CLAMP(float(f.get("width", 0.3f)), 0.05f, 0.7f);
		const float size = CLAMP(float(f.get("size", 0.08f)), 0.02f, 0.4f) * _rand(0.75f, 1.15f);
		const Color fallen = f.get("color", Color(0.35f, 0.2f, 0.07f));
		const float fresh = CLAMP(float(f.get("fresh", 0.3f)), 0.0f, 1.0f);
		// Weathered: a duller, paler tan-brown as the pigments break down and the tissue thins.
		const bool is_fresh = rng.randf() < fresh;
		const float k = _rand(0.8f, 1.15f);
		Color c = is_fresh ? fallen : fallen.lerp(Color(0.36f, 0.25f, 0.15f), _rand(0.4f, 0.8f)); // Bleached tan-brown.
		c = c * k;
		const Color tint(c.r / MAX(leaf_color.r, 0.01f), c.g / MAX(leaf_color.g, 0.01f), c.b / MAX(leaf_color.b, 0.01f));
		const float r = disc * Math::sqrt(rng.randf());
		const float a = _rand(0.0f, Math::TAU);
		const Vector3 at(Math::cos(a) * r, 0.002f + 0.004f * rng.randf(), Math::sin(a) * r);
		const Vector3 fwd = polar_dir(_rand(0.0f, Math::TAU), _rand(-0.12f, 0.12f));
		const Vector3 up = rng.randf() < 0.7f ? Vector3(0, 1, 0) : Vector3(0, -1, 0);
		const Basis frame = organ_frame(fwd, up);
		const float curl = is_fresh ? 0.6f : 1.0f; // Older leaves dry and curl more.
		Leaf l;
		l.shape = LEAF_OVATE;
		l.length = size;
		l.width = size * 2.0f * half_w;
		l.petiole = 0.08f;
		l.fold = _rand(-0.4f, 0.5f) * curl;
		l.bend = _rand(-0.6f, 0.6f) * curl;
		l.twist = _rand(-0.5f, 0.5f) * curl;
		l.segments = 6;
		float area = 0.6f * l.length * l.width;
		switch (kind) {
			case 1: // Lobed (oaks, hawthorn): rounded lobes along the sides.
				l.lobes = MAX(count, 1);
				l.lobe_depth = CLAMP(depth, 0.0f, 0.8f);
				l.round_lobes = true;
				l.segments = MAX(6, l.lobes * 3);
				area *= 1.0f - 0.5f * l.lobe_depth;
				_add_leaf(SURFACE_LEAF, Transform3D(frame, at), l, 0.15f, 0.85f, tint);
				break;
			case 2: // Palmate (maples, sycamore): broad, deep round lobes.
				l.shape = LEAF_PALMATE;
				l.width = size * 1.05f;
				l.lobes = MAX((count - 1) / 2, 1);
				l.lobe_depth = CLAMP(depth, 0.0f, 0.8f);
				l.segments = 8;
				area = 0.5f * l.length * l.width;
				_add_leaf(SURFACE_LEAF, Transform3D(frame, at), l, 0.15f, 0.85f, tint);
				break;
			case 3: { // Pinnate (ash, rowan, elder): mostly loose leaflets, now and then a whole leaf.
				const int n = MAX(count, 3);
				Leaf lf = l;
				lf.shape = LEAF_LANCEOLATE;
				lf.length = size * 2.4f / (n + 1);
				lf.width = lf.length * 0.36f;
				lf.petiole = 0.03f;
				if (rng.randf() < 0.3f) {
					Leaf rachis;
					rachis.shape = LEAF_LINEAR;
					rachis.length = size;
					rachis.width = 0.0025f;
					rachis.fold = 0.0f;
					rachis.bend = l.bend * 0.3f;
					rachis.segments = 3;
					_add_leaf(SURFACE_LEAF, Transform3D(frame, at), rachis, 0.15f, 0.85f, tint);
					for (int q = 0; q < n; q++) {
						const float u = (q / 2 + 1.0f) / (n / 2 + 1.0f);
						const float side = (q % 2 == 0) ? 1.0f : -1.0f;
						const Vector3 d = (frame.get_column(2) * 0.6f + frame.get_column(0) * side).normalized();
						const Vector3 p = at + frame.get_column(2) * (q == n - 1 ? size : u * size * 0.9f);
						_add_leaf(SURFACE_LEAF, Transform3D(organ_frame(q == n - 1 ? frame.get_column(2) : d, frame.get_column(1)), p), lf, 0.15f, 0.85f, tint);
					}
					area = 0.6f * lf.length * lf.width * n;
				} else {
					_add_leaf(SURFACE_LEAF, Transform3D(frame, at), lf, 0.15f, 0.85f, tint);
					area = 0.6f * lf.length * lf.width;
				}
			} break;
			case 4: { // Digitate (horse chestnut): leaflets spread from one point.
				const int n = MAX(count, 5);
				Leaf lf = l;
				lf.shape = LEAF_SPATULATE;
				lf.length = size * 0.6f;
				lf.width = lf.length * 0.42f;
				lf.petiole = 0.0f;
				const float spread = 2.6f;
				for (int q = 0; q < n; q++) {
					const float ang = (float(q) / MAX(n - 1, 1) - 0.5f) * spread;
					const Vector3 d = (frame.get_column(2) * Math::cos(ang) + frame.get_column(0) * Math::sin(ang)).normalized();
					Leaf lq = lf;
					lq.length *= 1.0f - 0.35f * Math::abs(ang) / spread * 2.0f;
					_add_leaf(SURFACE_LEAF, Transform3D(organ_frame(d, frame.get_column(1)), at), lq, 0.15f, 0.85f, tint);
				}
				area = 0.5f * lf.length * lf.width * n;
			} break;
			case 5: // Needles: the needle litter preset lies under conifers.
				area = 0.0f;
				break;
			default: // Simple (beech, birch, lime, alder, willow, hazel).
				_add_leaf(SURFACE_LEAF, Transform3D(frame, at), l, 0.15f, 0.85f, tint);
				break;
		}
		area_left -= MAX(area, 0.0004f);
	}
}

// Moss cushion (Polytrichum-like hair moss): a low dark mound packed with upright shoots, each a
// star of narrow leaves; densest and tallest in the middle.
void PlantGenerator::_build_moss() {
	// The cushion's surface is the packed shoot tips; at this size geometry per shoot would cost
	// thousands of triangles, so the mound is a lumpy dome carrying a generated shoot-tip texture
	// (albedo, normal, translucency; _bake_moss_texture), with loose shoots only along the rim
	// and a few standing proud, where the silhouette shows them.
	// Feather mosses (Hylocomium, Pleurozium) under conifers: wide, flat, loose carpets.
	const bool carpet = preset == PRESET_FEATHER_MOSS;
	const float radius = carpet ? _rand(0.18f, 0.32f) : _rand(0.06f, 0.12f);
	const float height = radius * (carpet ? _rand(0.08f, 0.13f) : _rand(0.22f, 0.35f));
	Builder &b = builders[SURFACE_CENTRE];
	const int rings = 8;
	const int sectors = 28;
	const int first = b.verts.size();
	float ph[6];
	for (float &p : ph) {
		p = _rand(0.0f, Math::TAU);
	}
	auto rim = [&](float p_a) {
		// An irregular outline: a few lobes.
		return radius * (1.0f + 0.12f * Math::sin(3.0f * p_a + ph[0]) + 0.07f * Math::sin(5.0f * p_a + ph[1]));
	};
	for (int r = 0; r <= rings; r++) {
		const float u = float(r) / rings; // 0 at the centre, 1 at the rim.
		for (int k = 0; k < sectors; k++) {
			const float a = k * Math::TAU / sectors;
			const float rr = rim(a) * u;
			const float x = Math::cos(a) * rr;
			const float z = Math::sin(a) * rr;
			const float lump = 1.0f + 0.18f * Math::sin(x * 70.0f + ph[2]) * Math::sin(z * 63.0f + ph[3]) + 0.1f * Math::sin(x * 150.0f + ph[4] + z * 90.0f);
			const float y = height * Math::pow(MAX(0.0f, 1.0f - u * u), 0.9f) * lump - 0.004f;
			b.verts.push_back(Vector3(x, y, z));
			b.uvs.push_back(Vector2(x, z) / 0.05f);
			b.uv2s.push_back(Vector2(0.05f, 0.0f));
			const float ao = Math::lerp(1.0f, 0.75f, u * u);
			b.colors.push_back(Color(ao, ao, ao));
		}
	}
	for (int r = 0; r < rings; r++) {
		for (int k = 0; k < sectors; k++) {
			const int a = first + r * sectors + k;
			const int c = first + r * sectors + (k + 1) % sectors;
			// The centre ring is degenerate (all at the middle): its triangles collapse harmlessly.
			b.indices.append_array(PackedInt32Array({ a, a + sectors, c, c, a + sectors, c + sectors }));
		}
	}
	plant_height = MAX(plant_height, height * 1.3f);
	Leaf l;
	l.shape = LEAF_LINEAR;
	l.width = 0.002f;
	l.fold = 0.3f;
	l.segments = 2;
	const int shoots = int(_rand(40.0f, 60.0f));
	for (int s = 0; s < shoots; s++) {
		const bool edge = s < shoots * 3 / 4;
		const float a = _rand(0.0f, Math::TAU);
		const float u = edge ? _rand(0.85f, 1.0f) : _rand(0.0f, 0.7f);
		const float rr = rim(a) * u;
		const float y = height * Math::pow(MAX(0.0f, 1.0f - u * u), 0.9f) - 0.004f;
		const Vector3 at(Math::cos(a) * rr, y, Math::sin(a) * rr);
		const Vector3 normal = Vector3(Math::cos(a) * u, 1.2f, Math::sin(a) * u).normalized();
		const float k = _rand(0.85f, 1.2f);
		l.length = _rand(0.004f, 0.007f);
		l.bend = 0.5f;
		const float sp = _rand(0.0f, Math::TAU);
		const Basis nb = organ_frame(normal, Vector3(1, 0, 0));
		for (int j = 0; j < 3; j++) {
			const float bb = sp + j * Math::TAU / 3.0f;
			const Vector3 d = (nb.get_column(2) * 1.4f + (nb.get_column(0) * Math::cos(bb) + nb.get_column(1) * Math::sin(bb))).normalized();
			_add_leaf(SURFACE_LEAF, Transform3D(organ_frame(d, normal), at), l, 0.1f, 1.0f, Color(1.5f * k, 1.45f * k, 1.1f * k));
		}
	}
}

// Bilberry (Vaccinium myrtillus): a dwarf shrub of green, angled, much-branched stems with small
// alternate, finely toothed oval leaves, and a few blue-black berries under them.
void PlantGenerator::_build_bilberry() {
	const int stems = int(_rand(3.0f, 7.0f));
	Leaf l;
	l.shape = LEAF_OVATE;
	l.petiole = 0.06f;
	l.lobes = 6;
	l.lobe_depth = 0.06f;
	l.fold = 0.25f;
	l.bend = 0.3f;
	l.segments = 5;
	auto branch = [&](const Transform3D &p_base, float p_len, float p_r, int p_depth, float p_wind) -> void {
		const float bend = _rand(-0.4f, 0.4f);
		const LocalVector<Transform3D> fr = _stem_frames(p_base, p_len, bend, 4);
		_add_stem(p_base, p_len, p_r, p_r * 0.6f, bend, 4, p_wind);
		const int leaves = int(p_len / 0.007f);
		float a = _rand(0.0f, Math::TAU);
		for (int k = 0; k < leaves; k++) {
			const float s = 0.15f + 0.85f * float(k) / MAX(leaves - 1, 1);
			const Transform3D at = _frame_at(fr, s);
			a += GOLDEN_ANGLE;
			const Vector3 radial = at.basis.get_column(0) * Math::cos(a) + at.basis.get_column(1) * Math::sin(a);
			l.length = _rand(0.016f, 0.03f);
			l.width = l.length * 0.6f;
			_add_leaf(SURFACE_LEAF, Transform3D(organ_frame((radial + at.basis.get_column(2) * 0.5f + Vector3(0, 0.3f, 0)).normalized(), Vector3(0, 1, 0)), at.origin), l, Math::lerp(p_wind, 1.0f, s));
			if (p_depth == 0 && rng.randf() < 0.08f) {
				_add_head(SURFACE_PETAL, Transform3D(organ_frame(Vector3(0, -1, 0), Vector3(1, 0, 0)), at.origin + radial * 0.004f - Vector3(0, 0.006f, 0)), 0.0035f, 0.006f);
			}
		}
	};
	for (int s = 0; s < stems; s++) {
		Transform3D base = stem_base(_rand(0.0f, Math::TAU), _rand(1.1f, 1.45f));
		base.origin = Vector3(_rand(-0.06f, 0.06f), 0.0f, _rand(-0.06f, 0.06f));
		const float len = _rand(0.12f, 0.28f);
		const LocalVector<Transform3D> fr = _stem_frames(base, len, 0.0f, 4);
		branch(base, len, 0.0018f, 1, 0.0f);
		const int subs = int(_rand(2.0f, 5.0f));
		for (int k = 0; k < subs; k++) {
			Transform3D at = _frame_at(fr, _rand(0.35f, 0.9f));
			const float a = _rand(0.0f, Math::TAU);
			at.basis = organ_frame(at.basis.get_column(2) + Vector3(Math::cos(a), 0.3f, Math::sin(a)) * 0.9f, Vector3(0, 1, 0));
			branch(at, _rand(0.05f, 0.12f), 0.0011f, 0, 0.5f);
		}
	}
}

// Bracken (Pteridium aquilinum), the fern of woodland edges and clearings: few fronds from a
// creeping rhizome, each a tall bare stipe whose blade turns nearly flat, triangular and
// two to three times pinnate: pinnae longest at the base, pinnules lobed (Page 1976).
void PlantGenerator::_build_bracken() {
	const int fronds = int(_rand(2.0f, 5.0f));
	for (int i = 0; i < fronds; i++) {
		Transform3D base = stem_base(_rand(0.0f, Math::TAU), _rand(1.35f, 1.52f));
		base.origin = Vector3(_rand(-0.25f, 0.25f), 0.0f, _rand(-0.25f, 0.25f));
		const float stipe = _rand(0.35f, 0.75f);
		const LocalVector<Transform3D> sf = _stem_frames(base, stipe, _rand(0.05f, 0.2f), 4);
		_add_stem(base, stipe, 0.0045f, 0.0035f, sf.size() > 0 ? 0.1f : 0.0f, 4, 0.0f);
		// The blade: a knee at the top of the stipe, then a nearly flat blade drooping a little.
		Transform3D top = sf[sf.size() - 1];
		top.basis = Basis(top.basis.get_column(0), _rand(0.85f, 1.1f)) * top.basis;
		top.basis.orthonormalize();
		const float blade = _rand(0.45f, 0.85f);
		const float turn = _rand(0.25f, 0.5f);
		const LocalVector<Transform3D> rf = _stem_frames(top, blade, turn, 10);
		_add_stem(top, blade, 0.0035f, 0.001f, turn, 10, 0.3f);
		const int pairs = int(_rand(8.0f, 12.0f));
		for (int k = 0; k < pairs; k++) {
			const float s = Math::lerp(0.08f, 0.95f, float(k) / (pairs - 1));
			const Transform3D at = _frame_at(rf, s);
			// Triangular blade: pinnae shorten from the base to the tip.
			const float plen = blade * 0.5f * Math::pow(1.0f - s, 0.85f) + 0.02f;
			for (int side = -1; side <= 1; side += 2) {
				const Vector3 out = (at.basis.get_column(0) * float(side) + at.basis.get_column(2) * 0.3f).normalized();
				// The blade is held flat: every lamina faces the sky.
				Transform3D pb(organ_frame(out, Vector3(0, 1, 0)), at.origin);
				if (plen < 0.07f) {
					// Upper pinnae: simple lobed leaves.
					Leaf l;
					l.shape = LEAF_LANCEOLATE;
					l.length = plen;
					l.width = plen * 0.28f;
					l.lobes = 6;
					l.lobe_depth = 0.4f;
					l.fold = 0.1f;
					l.bend = 0.3f;
					l.segments = 6;
					_add_leaf(SURFACE_LEAF, pb, l, Math::lerp(0.4f, 1.0f, s));
					continue;
				}
				const LocalVector<Transform3D> pf = _stem_frames(pb, plen, 0.25f, 4);
				_add_stem(pb, plen, 0.0012f, 0.0005f, 0.25f, 4, Math::lerp(0.4f, 1.0f, s));
				// Pinnules crowded along the pinna, nearly touching, so the blade reads as one lamina.
				const int pinnules = int(Math::lerp(8.0f, 15.0f, plen / (blade * 0.52f)));
				for (int q = 0; q < pinnules; q++) {
					const float u = Math::lerp(0.12f, 0.95f, float(q) / MAX(pinnules - 1, 1));
					const Transform3D pa = _frame_at(pf, u);
					const float side2 = (q % 2 == 0) ? 1.0f : -1.0f;
					Leaf l;
					l.shape = LEAF_LANCEOLATE;
					l.length = plen * 0.32f * (1.0f - 0.6f * u);
					l.width = l.length * 0.36f;
					l.lobes = 5;
					l.lobe_depth = 0.38f;
					l.fold = 0.12f;
					l.bend = 0.25f;
					l.segments = 5;
					const Vector3 fwd = (pa.basis.get_column(0) * side2 + pa.basis.get_column(2) * 0.45f).normalized();
					_add_leaf(SURFACE_LEAF, Transform3D(organ_frame(fwd, Vector3(0, 1, 0)), pa.origin), l, Math::lerp(0.5f, 1.0f, s));
				}
			}
		}
	}
}

// Bramble (Rubus fruticosus agg.): arching canes that root where their tips touch the ground,
// alternate leaves of three or five toothed leaflets on a stalk, and clusters of berries near the
// tips, red ripening to black.
void PlantGenerator::_build_bramble() {
	const int canes = int(_rand(5.0f, 9.0f));
	auto compound = [&](const Transform3D &p_at, const Vector3 &p_radial, float p_s, float p_size) -> void {
		// Leaf stalk, held out and up; leaflets fan from its tip, facing the light.
		const Vector3 out = (p_radial + Vector3(0, 0.7f, 0) + p_at.basis.get_column(2) * 0.3f).normalized();
		Transform3D pb(organ_frame(out, Vector3(0, 1, 0)), p_at.origin);
		const Transform3D tip = _add_stem(pb, _rand(0.035f, 0.07f), 0.0015f, 0.0012f, 0.3f, 2, p_s * 0.6f);
		const int leaflets = rng.randf() < 0.4f ? 5 : 3;
		for (int q = 0; q < leaflets; q++) {
			const float spread = (float(q) - (leaflets - 1) * 0.5f) * (leaflets == 5 ? 0.75f : 1.0f);
			const Vector3 f = (Basis(Vector3(0, 1, 0), spread).xform(tip.basis.get_column(2) * Vector3(1, 0, 1)) + Vector3(0, 0.15f, 0)).normalized();
			Leaf l;
			l.shape = LEAF_OVATE;
			l.length = _rand(0.06f, 0.1f) * p_size * (q == (leaflets - 1) / 2 ? 1.15f : 0.95f);
			l.width = l.length * 0.62f;
			l.petiole = 0.08f;
			l.lobes = 10;
			l.lobe_depth = 0.07f;
			l.fold = 0.25f;
			l.bend = _rand(0.4f, 0.8f);
			l.segments = 7;
			_add_leaf(SURFACE_LEAF, Transform3D(organ_frame(f, Vector3(0, 1, 0)), tip.origin), l, Math::lerp(0.3f, 1.0f, p_s), Math::lerp(0.75f, 1.0f, p_s));
		}
	};
	for (int c = 0; c < canes; c++) {
		Transform3D base = stem_base(_rand(0.0f, Math::TAU), _rand(1.25f, 1.48f));
		base.origin = Vector3(_rand(-0.1f, 0.1f), 0.0f, _rand(-0.1f, 0.1f));
		float len = _rand(0.9f, 1.9f);
		float arch = _rand(1.7f, 2.3f);
		// The cane ends where its tip comes back down to the ground (where it would root).
		{
			const LocalVector<Transform3D> probe = _stem_frames(base, len, arch, 28);
			for (uint32_t k = 8; k < probe.size(); k++) {
				if (probe[k].origin.y < 0.02f) {
					// Same curvature, shorter cane.
					len *= float(k) / 28.0f;
					arch *= float(k) / 28.0f;
					break;
				}
			}
		}
		const LocalVector<Transform3D> fr = _stem_frames(base, len, arch, 14);
		_add_stem(base, len, 0.0045f, 0.002f, arch, 14, 0.0f);
		const int nodes = int(len / 0.075f);
		float a = _rand(0.0f, Math::TAU);
		for (int k = 0; k < nodes; k++) {
			const float s = (k + 0.7f) / nodes;
			const Transform3D at = _frame_at(fr, s);
			if (at.origin.y < 0.03f) {
				continue;
			}
			a += GOLDEN_ANGLE * 0.8f;
			const Vector3 radial = at.basis.get_column(0) * Math::cos(a) + at.basis.get_column(1) * Math::sin(a);
			compound(at, radial, s, Math::lerp(1.0f, 0.65f, s));
			// Flowering laterals from the arch: short shoots with their own leaves.
			if (s > 0.25f && s < 0.85f && rng.randf() < 0.4f) {
				Transform3D lb(organ_frame((radial + Vector3(0, 0.9f, 0)).normalized(), Vector3(0, 1, 0)), at.origin);
				const float llen = _rand(0.12f, 0.3f);
				const LocalVector<Transform3D> lf = _stem_frames(lb, llen, 0.4f, 4);
				_add_stem(lb, llen, 0.002f, 0.001f, 0.4f, 4, s);
				const int ln = int(_rand(2.0f, 5.0f));
				for (int q = 0; q < ln; q++) {
					const float u = (q + 0.8f) / ln;
					const Transform3D la = _frame_at(lf, u);
					const float b = a + q * GOLDEN_ANGLE;
					compound(la, la.basis.get_column(0) * Math::cos(b) + la.basis.get_column(1) * Math::sin(b), Math::lerp(s, 1.0f, u), 0.7f);
				}
			}
			// Berry clusters on the upper side of the arch.
			if (s > 0.35f && s < 0.75f && rng.randf() < 0.35f) {
				const int berries = int(_rand(3.0f, 8.0f));
				for (int q = 0; q < berries; q++) {
					const Vector3 o = at.origin + radial * 0.02f + Vector3(_rand(-0.02f, 0.02f), _rand(-0.03f, 0.0f), _rand(-0.02f, 0.02f));
					// A drupelet cluster: a small dome (berry colour on the petal surface).
					_add_head(SURFACE_PETAL, Transform3D(organ_frame(Vector3(0, -1, 0), Vector3(1, 0, 0)), o), _rand(0.006f, 0.009f), _rand(0.009f, 0.013f));
				}
			}
		}
	}
}

// Foxglove (Digitalis purpurea), a biennial of woodland edges and clearings: a rosette of soft,
// downy, toothed leaves, then a tall spike of tubular purple flowers hanging to one side, opening
// from the bottom up.
void PlantGenerator::_build_foxglove() {
	const int basal = int(_rand(8.0f, 14.0f));
	const float spin = _rand(0.0f, Math::TAU);
	for (int i = 0; i < basal; i++) {
		const float u = float(i) / basal;
		Leaf l;
		l.shape = LEAF_LANCEOLATE;
		l.length = _rand(0.15f, 0.26f) * (1.0f - 0.35f * u);
		l.width = l.length * _rand(0.3f, 0.38f);
		l.petiole = 0.18f;
		l.lobes = 10;
		l.lobe_depth = 0.05f;
		l.fold = 0.25f;
		l.bend = _rand(0.8f, 1.3f);
		l.segments = 9;
		_add_leaf(SURFACE_LEAF, stem_base(spin + i * GOLDEN_ANGLE, Math::lerp(0.35f, 0.9f, u)), l, 0.0f, Math::lerp(0.75f, 1.0f, u));
	}
	Transform3D base = stem_base(_rand(0.0f, Math::TAU), _rand(1.48f, 1.57f));
	const float len = _rand(0.6f, 1.2f);
	const float bend = _rand(-0.12f, 0.12f);
	const LocalVector<Transform3D> fr = _stem_frames(base, len, bend, 12);
	_add_stem(base, len, 0.006f, 0.0025f, bend, 12, 0.0f);
	// Stem leaves, smaller up the stem.
	for (int k = 0; k < 7; k++) {
		const float s = 0.08f + 0.06f * k;
		const Transform3D at = _frame_at(fr, s);
		const float a = spin + k * GOLDEN_ANGLE;
		Leaf l;
		l.shape = LEAF_LANCEOLATE;
		l.length = Math::lerp(0.12f, 0.05f, float(k) / 6.0f);
		l.width = l.length * 0.32f;
		l.lobes = 8;
		l.lobe_depth = 0.05f;
		l.fold = 0.2f;
		l.bend = 0.7f;
		l.segments = 6;
		const Vector3 radial = at.basis.get_column(0) * Math::cos(a) + at.basis.get_column(1) * Math::sin(a);
		_add_leaf(SURFACE_LEAF, Transform3D(organ_frame((radial + Vector3(0, 0.5f, 0)).normalized(), Vector3(0, 1, 0)), at.origin), l, s);
	}
	// The flower spike: secund (all to one side), bells hanging, buds at the top.
	const int flowers = int(_rand(14.0f, 28.0f));
	const float face = _rand(0.0f, Math::TAU);
	for (int k = 0; k < flowers; k++) {
		const float u = float(k) / (flowers - 1);
		const float s = Math::lerp(0.5f, 0.98f, u);
		const Transform3D at = _frame_at(fr, s);
		const float a = face + _rand(-0.6f, 0.6f);
		const Vector3 side(Math::cos(a), 0.0f, Math::sin(a));
		const float open = 1.0f - smoothstep_f(0.6f, 1.0f, u); // Buds at the top.
		const float tube = Math::lerp(0.012f, 0.045f, open);
		// Short pedicel, then the bell hangs out and down.
		Transform3D fb(organ_frame((side * 0.8f + Vector3(0, -0.6f, 0)).normalized(), Vector3(0, 1, 0)), at.origin + side * 0.008f);
		_add_stem(fb, tube, 0.003f, Math::lerp(0.004f, 0.012f, open), -0.25f, 3, s, SURFACE_PETAL);
	}
}

// Leaf textures (docs/ideas/plant-generator.md, "Leaf textures"). Veins grow by Runions et al.,
// "Modeling and visualization of leaf venation patterns" (SIGGRAPH 2005): auxin sources scattered
// over the blade attract the nearest vein node, nodes step towards their sources, sources die
// when a vein reaches them; vein width then follows Murray's law (r^3 of a vein = sum of its
// branches). From the veins: the albedo (lighter veins, colour drift, yellowed margins, spots),
// a normal map (veins sunk into the blade, the blade puckered between them) and the back light
// (translucency) map, where veins block light as in Sousa's Crysis vegetation (GPU Gems 3, 16).

static float tex_hash(int p_x, int p_y, uint32_t p_seed) {
	uint32_t h = uint32_t(p_x) * 0x8DA6B343u ^ uint32_t(p_y) * 0xD8163841u ^ p_seed * 0xCB1AB31Fu;
	h ^= h >> 15;
	h *= 0x2C1B3C6Du;
	h ^= h >> 12;
	h *= 0x297A2D39u;
	h ^= h >> 15;
	return (h & 0xFFFFFF) / float(0xFFFFFF);
}

static float tex_noise(float p_x, float p_y, uint32_t p_seed) {
	const int ix = int(Math::floor(p_x));
	const int iy = int(Math::floor(p_y));
	float fx = p_x - ix;
	float fy = p_y - iy;
	fx = fx * fx * (3.0f - 2.0f * fx);
	fy = fy * fy * (3.0f - 2.0f * fy);
	const float a = tex_hash(ix, iy, p_seed);
	const float b = tex_hash(ix + 1, iy, p_seed);
	const float c = tex_hash(ix, iy + 1, p_seed);
	const float d = tex_hash(ix + 1, iy + 1, p_seed);
	return Math::lerp(Math::lerp(a, b, fx), Math::lerp(c, d, fx), fy);
}

static float tex_fbm(float p_x, float p_y, uint32_t p_seed) {
	return 0.5f * tex_noise(p_x, p_y, p_seed) + 0.3f * tex_noise(p_x * 2.3f, p_y * 2.3f, p_seed + 7) + 0.2f * tex_noise(p_x * 5.1f, p_y * 5.1f, p_seed + 13);
}



// Forest floor (docs/ideas/forest-floor.md).

// Needle litter under pines and spruces: a low, rusty mat with loose needles lying on it, a few
// cones, a twig.
void PlantGenerator::_build_needle_litter() {
	// The ground's own shader draws the needle carpet (VegetationField3D.get_floor_shader_code());
	// this adds its relief near the camera: loose needles lying across each other, and cones.
	const float radius = _rand(0.12f, 0.2f);
	// Loose needles on top, crossing every way.
	Leaf n;
	n.shape = LEAF_LINEAR;
	n.width = 0.0012f;
	n.fold = 0.0f;
	n.segments = 2;
	const int needles = int(_rand(70.0f, 120.0f));
	for (int i = 0; i < needles; i++) {
		n.length = _rand(0.012f, 0.045f); // Spruce short, pine long.
		n.bend = _rand(-0.3f, 0.3f);
		const float r = radius * 1.1f * Math::sqrt(rng.randf());
		const float a = _rand(0.0f, Math::TAU);
		const float lift = 0.0f;
		const Vector3 at(Math::cos(a) * r, lift + 0.001f + 0.003f * rng.randf(), Math::sin(a) * r);
		const float k = _rand(0.7f, 1.3f);
		_add_leaf(SURFACE_LEAF, Transform3D(organ_frame(polar_dir(_rand(0.0f, Math::TAU), _rand(-0.15f, 0.15f)), Vector3(0, 1, 0)), at), n, 0.0f, 0.9f, Color(k, k * 0.9f, k * 0.8f));
	}
	const int cones = int(_rand(0.0f, 3.0f));
	for (int i = 0; i < cones; i++) {
		const float len = _rand(0.04f, 0.12f);
		Transform3D cb(organ_frame(polar_dir(_rand(0.0f, Math::TAU), _rand(-0.1f, 0.1f)), Vector3(0, 1, 0)), Vector3(_rand(-radius, radius) * 0.7f, len * 0.16f, _rand(-radius, radius) * 0.7f));
		const Transform3D mid = _add_stem(cb, len * 0.6f, len * 0.12f, len * 0.16f, 0.0f, 3, 0.0f, SURFACE_PETAL);
		_add_stem(mid, len * 0.4f, len * 0.16f, len * 0.03f, 0.0f, 3, 0.0f, SURFACE_PETAL);
	}
}

// Fallen twigs and small branches: grey-brown sticks lying on the litter, forked, a few side
// shoots, the thicker ones cracked short.
void PlantGenerator::_build_twigs() {
	// Fine woody debris under 1 cm (docs/ideas/forest-floor.md, step 6; Brown 1974's classes):
	// short broken pieces, a few forked, most of them thin; the trees' generator lays the sticks
	// and branches from 1 cm up. A card scatters several over half a metre.
	const int sticks = int(_rand(4.0f, 9.0f));
	for (int i = 0; i < sticks; i++) {
		const float u = rng.randf();
		const float len = Math::lerp(0.05f, 0.35f, u * u);
		const float r0 = Math::lerp(0.001f, 0.0045f, rng.randf() * (0.4f + 0.6f * u)); // 2 to 9 mm thick.
		Transform3D b(organ_frame(polar_dir(_rand(0.0f, Math::TAU), _rand(-0.03f, 0.05f)), Vector3(0, 1, 0)), Vector3(_rand(-0.25f, 0.25f), r0 * 0.6f, _rand(-0.25f, 0.25f)));
		const float bend = _rand(-0.35f, 0.35f);
		const LocalVector<Transform3D> fr = _stem_frames(b, len, bend, 4);
		_add_stem(b, len, r0, r0 * _rand(0.45f, 0.8f), bend, 4, 0.0f);
		// Longer pieces keep a side twig or two; short ones are broken bits.
		const int sides = len > 0.15f ? int(_rand(0.0f, 3.0f)) : 0;
		for (int k = 0; k < sides; k++) {
			const Transform3D at = _frame_at(fr, _rand(0.25f, 0.85f));
			const float sgn = rng.randf() < 0.5f ? -1.0f : 1.0f;
			const Vector3 d = (at.basis.get_column(2) + at.basis.get_column(0) * sgn * _rand(0.6f, 1.2f) + Vector3(0, _rand(-0.05f, 0.2f), 0)).normalized();
			_add_stem(Transform3D(organ_frame(d, Vector3(0, 1, 0)), at.origin), len * _rand(0.15f, 0.4f), r0 * 0.5f, r0 * 0.25f, _rand(-0.4f, 0.4f), 2, 0.0f);
		}
	}
}

// Wood sorrel (Oxalis acetosella): carpets of three folded, heart-shaped leaflets on slender
// reddish stalks; a few white flowers veined with lilac.
void PlantGenerator::_build_wood_sorrel() {
	// Leaves rise singly from a creeping rhizome, so they spread as a patch, held level 5 to 9 cm up.
	const int leaves = int(_rand(10.0f, 20.0f));
	for (int i = 0; i < leaves; i++) {
		Transform3D base = stem_base(_rand(0.0f, Math::TAU), _rand(1.35f, 1.55f));
		base.origin = Vector3(_rand(-0.09f, 0.09f), 0.0f, _rand(-0.09f, 0.09f));
		const Transform3D top = _add_stem(base, _rand(0.05f, 0.09f), 0.0009f, 0.0007f, _rand(0.05f, 0.25f), 4, 0.0f);
		const float spin = _rand(0.0f, Math::TAU);
		Leaf l;
		l.shape = LEAF_SPATULATE; // Obcordate: widest at the notched tip.
		l.lobes = 1;
		l.lobe_depth = 0.25f;
		l.width = _rand(0.016f, 0.024f);
		l.length = l.width * 0.85f;
		l.fold = _rand(0.15f, 0.4f); // Folded along the midrib; open in the shade, shut in sun.
		l.bend = 0.3f;
		l.segments = 5;
		for (int k = 0; k < 3; k++) {
			const Vector3 radial = polar_dir(spin + k * Math::TAU / 3.0f, _rand(-0.25f, 0.1f));
			_add_leaf(SURFACE_LEAF, Transform3D(organ_frame(radial, Vector3(0, 1, 0)), top.origin), l, 1.0f);
		}
	}
	const int flowers = int(_rand(0.0f, 4.0f));
	for (int f = 0; f < flowers; f++) {
		const Transform3D top = _add_stem(stem_base(_rand(0.0f, Math::TAU), _rand(1.3f, 1.5f)), _rand(0.06f, 0.1f), 0.0008f, 0.0006f, _rand(0.3f, 0.8f), 4, 0.0f);
		Leaf p;
		p.shape = LEAF_SPATULATE;
		p.length = 0.009f;
		p.width = 0.006f;
		p.fold = 0.2f;
		p.bend = -0.3f;
		p.segments = 3;
		for (int k = 0; k < 5; k++) {
			const Vector3 d = (top.basis.get_column(2) * 0.8f + top.basis.get_column(0) * Math::cos(k * Math::TAU / 5.0f) + top.basis.get_column(1) * Math::sin(k * Math::TAU / 5.0f)).normalized();
			_add_leaf(SURFACE_PETAL, Transform3D(organ_frame(d, Vector3(0, 1, 0)), top.origin), p, 1.0f);
		}
	}
}

// Tree seedling (beech-like): a thin woody stem, zigzag at the buds, two-ranked oval leaves with
// wavy margins held flat. First-year plants still carry the broad fleshy seed leaves; older ones
// (advance regeneration, waiting under the canopy for a gap) grow flat horizontal sprays from the
// upper nodes, the shade form that catches the 1 to 5% of light that reaches the floor.
void PlantGenerator::_build_seedling() {
	const float len = _rand(0.08f, 0.75f);
	const float age = CLAMP((len - 0.15f) / 0.5f, 0.0f, 1.0f);
	const Transform3D base = stem_base(_rand(0.0f, Math::TAU), _rand(1.4f, 1.55f));
	const int nodes = MAX(3, int(2.0f + len * 15.0f));
	const float r_base = 0.0015f + 0.004f * len;
	Leaf l;
	l.shape = LEAF_OVATE;
	l.lobes = 6;
	l.lobe_depth = 0.04f;
	l.petiole = 0.06f;
	l.fold = 0.15f;
	l.bend = 0.1f;
	l.segments = 6;
	const Vector3 up(0, 1, 0);
	Transform3D at = base;
	float az = _rand(0.0f, Math::TAU);
	for (int k = 0; k < nodes; k++) {
		const float t0 = float(k) / nodes, t1 = float(k + 1) / nodes;
		const float seg = len / nodes;
		// Zigzag: the stem turns a little at every bud.
		const Vector3 d = (at.basis.get_column(2) + Vector3(Math::cos(az), 0.0f, Math::sin(az)) * 0.16f).normalized();
		at = _add_stem(Transform3D(organ_frame(d, up), at.origin), seg, r_base * (1.0f - 0.6f * t0), r_base * (1.0f - 0.6f * t1), 0.0f, 1, t0);
		az += Math::PI;
		const Vector3 side(Math::cos(az), 0.0f, Math::sin(az));
		if (k == 0 && age < 0.3f) {
			// Seed leaves: a fleshy pair, fan-shaped, wider than long.
			Leaf c = l;
			c.lobes = 0;
			c.length = _rand(0.015f, 0.02f);
			c.width = c.length * 1.8f;
			c.petiole = 0.1f;
			c.fold = 0.05f;
			for (int s = -1; s <= 1; s += 2) {
				const Vector3 cd = (Vector3(-side.z, 0.0f, side.x) * float(s) + Vector3(0, 0.25f, 0)).normalized();
				_add_leaf(SURFACE_LEAF, Transform3D(organ_frame(cd, up), at.origin), c, t1);
			}
			continue;
		}
		// Shade leaves: large and thin, the lowest the oldest.
		l.length = _rand(0.045f, 0.075f) * (0.75f + 0.25f * age);
		l.width = l.length * _rand(0.55f, 0.65f);
		_add_leaf(SURFACE_LEAF, Transform3D(organ_frame((side + Vector3(0, 0.12f, 0)).normalized(), up), at.origin), l, t1);
		// Side shoots from the upper nodes, longest low down: a layered, flat crown.
		if (age > 0.25f && t1 > 0.3f && t1 < 0.92f && rng.randf() < 0.3f + 0.35f * age) {
			const Vector3 sd = (side + Vector3(0, _rand(0.0f, 0.15f), 0)).normalized();
			const float sl = len * (0.1f + 0.3f * (1.0f - t1)) * _rand(0.7f, 1.1f);
			const int sn = MAX(2, int(sl * 32.0f));
			Transform3D s_at(organ_frame(sd, up), at.origin);
			float s_side = rng.randf() < 0.5f ? -1.0f : 1.0f;
			for (int j = 0; j < sn; j++) {
				const Vector3 fwd = s_at.basis.get_column(2);
				s_at = _add_stem(Transform3D(organ_frame((fwd + Vector3(0, -0.04f, 0)).normalized(), up), s_at.origin), sl / sn, r_base * 0.45f * (1.0f - 0.5f * j / sn), r_base * 0.45f * (1.0f - 0.5f * (j + 1) / sn), 0.0f, 1, t1);
				// Two-ranked: leaves alternate left and right in the shoot's flat plane.
				const Vector3 across = fwd.cross(up).normalized() * s_side;
				s_side = -s_side;
				Leaf sl_leaf = l;
				sl_leaf.length = _rand(0.035f, 0.065f) * (1.0f - 0.25f * j / sn);
				sl_leaf.width = sl_leaf.length * _rand(0.55f, 0.65f);
				_add_leaf(SURFACE_LEAF, Transform3D(organ_frame((across + fwd * 0.45f + Vector3(0, 0.05f, 0)).normalized(), up), s_at.origin), sl_leaf, MIN(1.0f, t1 + 0.1f * (j + 1)));
			}
		}
	}
}

// Woodruff (Galium odoratum): a clonal patch of upright, square, unbranched stems from creeping
// rhizomes, 15 to 30 cm, with whorls of six to nine lanceolate leaves held level at each node
// and a loose cluster of small white four-petalled flowers at the top (beech woods on mull).
void PlantGenerator::_build_woodruff() {
	const int stems = int(_rand(5.0f, 12.0f));
	Leaf l;
	l.shape = LEAF_LANCEOLATE;
	l.fold = 0.15f;
	l.bend = 0.25f;
	l.segments = 4;
	Leaf p;
	p.shape = LEAF_OVATE;
	p.length = 0.0028f;
	p.width = 0.002f;
	p.fold = 0.1f;
	p.bend = 0.0f;
	p.segments = 2;
	for (int i = 0; i < stems; i++) {
		Transform3D base = stem_base(_rand(0.0f, Math::TAU), _rand(1.38f, 1.55f));
		base.origin = Vector3(_rand(-0.1f, 0.1f), 0.0f, _rand(-0.1f, 0.1f));
		const float len = _rand(0.12f, 0.28f);
		const float bend = _rand(-0.2f, 0.2f);
		const LocalVector<Transform3D> fr = _stem_frames(base, len, bend, 5);
		_add_stem(base, len, 0.0011f, 0.0008f, bend, 5, 0.0f);
		const int whorls = MAX(2, int(len / 0.04f));
		float spin = _rand(0.0f, Math::TAU);
		for (int k = 0; k < whorls; k++) {
			const float s = 0.25f + 0.7f * float(k) / MAX(whorls - 1, 1);
			const Transform3D at = _frame_at(fr, s);
			const int n = int(_rand(6.0f, 9.0f));
			spin += 0.4f;
			for (int j = 0; j < n; j++) {
				l.length = _rand(0.022f, 0.038f) * (k == whorls - 1 ? 0.7f : 1.0f);
				l.width = l.length * 0.28f;
				const Vector3 d = polar_dir(spin + j * Math::TAU / n, _rand(-0.05f, 0.2f));
				_add_leaf(SURFACE_LEAF, Transform3D(organ_frame(d, Vector3(0, 1, 0)), at.origin), l, Math::lerp(0.3f, 1.0f, s));
			}
		}
		if (rng.randf() < 0.6f) {
			const Transform3D top = fr[fr.size() - 1];
			const int flowers = int(_rand(4.0f, 9.0f));
			for (int f = 0; f < flowers; f++) {
				const Vector3 off = polar_dir(_rand(0.0f, Math::TAU), _rand(0.6f, 1.2f)) * _rand(0.006f, 0.016f);
				const Vector3 c = top.origin + off;
				const Vector3 face = (off.normalized() + Vector3(0, 1.5f, 0)).normalized();
				const Basis fb = organ_frame(face, Vector3(1, 0, 0));
				for (int k = 0; k < 4; k++) {
					const float a = k * Math::PI * 0.5f;
					const Vector3 d = (fb.get_column(0) * Math::cos(a) + fb.get_column(1) * Math::sin(a) + face * 0.25f).normalized();
					_add_leaf(SURFACE_PETAL, Transform3D(organ_frame(d, face), c), p, 1.0f);
				}
			}
		}
	}
}

// Wood anemone (Anemone nemorosa): a spring carpet of single stems 10 to 25 cm, each with a whorl
// of three stalked, deeply three-lobed leaves two thirds up and one white, often pink-backed
// flower of six to seven tepals, nodding a little.
void PlantGenerator::_build_wood_anemone() {
	const int stems = int(_rand(4.0f, 10.0f));
	Leaf l;
	l.shape = LEAF_PALMATE;
	l.lobes = 3;
	l.lobe_depth = 0.65f;
	l.petiole = 0.3f;
	l.fold = 0.15f;
	l.bend = 0.35f;
	l.segments = 8;
	Leaf t;
	t.shape = LEAF_OVATE;
	t.fold = 0.3f;
	t.bend = -0.4f;
	t.segments = 3;
	for (int i = 0; i < stems; i++) {
		Transform3D base = stem_base(_rand(0.0f, Math::TAU), _rand(1.35f, 1.55f));
		base.origin = Vector3(_rand(-0.08f, 0.08f), 0.0f, _rand(-0.08f, 0.08f));
		const float len = _rand(0.1f, 0.22f);
		const float bend = _rand(0.1f, 0.35f);
		const LocalVector<Transform3D> fr = _stem_frames(base, len, bend, 5);
		const Transform3D top = _add_stem(base, len, 0.0012f, 0.0009f, bend, 5, 0.0f);
		const Transform3D at = _frame_at(fr, 0.62f);
		const float spin = _rand(0.0f, Math::TAU);
		for (int k = 0; k < 3; k++) {
			l.length = _rand(0.04f, 0.065f);
			l.width = l.length * 0.95f;
			const Vector3 d = polar_dir(spin + k * Math::TAU / 3.0f, _rand(0.05f, 0.3f));
			_add_leaf(SURFACE_LEAF, Transform3D(organ_frame(d, Vector3(0, 1, 0)), at.origin), l, 0.7f);
		}
		if (rng.randf() < 0.85f) {
			const Vector3 face = (top.basis.get_column(2) + Vector3(0, -0.4f, 0)).normalized();
			const Basis fb = organ_frame(face, Vector3(0, 1, 0));
			const int tepals = rng.randf() < 0.5f ? 6 : 7;
			t.length = _rand(0.011f, 0.016f);
			t.width = t.length * 0.5f;
			for (int k = 0; k < tepals; k++) {
				const float a = k * Math::TAU / tepals;
				const Vector3 d = (fb.get_column(0) * Math::cos(a) + fb.get_column(1) * Math::sin(a) + face * 0.6f).normalized();
				_add_leaf(SURFACE_PETAL, Transform3D(organ_frame(d, face), top.origin), t, 1.0f);
			}
			_add_head(SURFACE_CENTRE, Transform3D(fb, top.origin), 0.0025f, 0.003f);
		}
	}
}

// Ivy (Hedera helix) as ground cover: creeping stems along the floor, branching, rooting as they
// go, with glossy, dark, three- to five-lobed leaves on long stalks, the blades turned up to the
// light and overlapping into a carpet 5 to 15 cm deep (oak and ash woods, shaded banks).
void PlantGenerator::_build_ivy() {
	const int runners = int(_rand(3.0f, 6.0f));
	Leaf l;
	l.shape = LEAF_PALMATE;
	l.lobes = 2;
	l.lobe_depth = 0.28f;
	l.fold = 0.1f;
	l.bend = 0.15f;
	l.segments = 7;
	Leaf petiole_leaf = l;
	for (int r = 0; r < runners; r++) {
		const float heading = _rand(0.0f, Math::TAU);
		Vector3 at(_rand(-0.05f, 0.05f), 0.006f, _rand(-0.05f, 0.05f));
		float dir = heading;
		const float len = _rand(0.35f, 0.7f);
		const int nodes = int(len / 0.035f);
		for (int k = 0; k < nodes; k++) {
			dir += _rand(-0.35f, 0.35f);
			const Vector3 fwd(Math::cos(dir), 0.0f, Math::sin(dir));
			const float seg = len / nodes;
			// Runners hug the ground: each segment starts low, so the pivot packing sees them as organs.
			_add_stem(Transform3D(organ_frame(fwd, Vector3(0, 1, 0)), at), seg, 0.0018f, 0.0016f, 0.0f, 1, 0.0f);
			at += fwd * seg;
			// A leaf at each node on a stalk rising to the light, alternately left and right.
			const float side = (k % 2 == 0) ? 1.0f : -1.0f;
			const Vector3 out = (fwd.cross(Vector3(0, 1, 0)) * side).normalized();
			const float stalk = _rand(0.03f, 0.08f);
			const Transform3D st(organ_frame((out * 0.5f + fwd * 0.3f + Vector3(0, 1.0f, 0)).normalized(), Vector3(0, 1, 0)), at);
			const Transform3D top = _add_stem(st, stalk, 0.0009f, 0.0007f, _rand(0.6f, 1.1f), 3, 0.2f);
			petiole_leaf.length = _rand(0.045f, 0.08f);
			petiole_leaf.width = petiole_leaf.length * _rand(1.0f, 1.2f);
			const Vector3 ld = (top.basis.get_column(2) * 0.3f + out * 0.6f + fwd * 0.3f + Vector3(0, 0.1f, 0)).normalized();
			_add_leaf(SURFACE_LEAF, Transform3D(organ_frame(ld, Vector3(0, 1, 0)), top.origin), petiole_leaf, 1.0f, Math::lerp(0.7f, 1.0f, rng.randf()));
			// Branches now and then.
			if (k > 1 && rng.randf() < 0.12f) {
				dir += side * 0.8f;
			}
		}
	}
}

// Heather (Calluna vulgaris): a dwarf shrub 20 to 50 cm of wiry, much-branched woody stems; the
// shoots carry tiny scale leaves pressed in four ranks and end in spikes of small pink-mauve
// bells (pine heath, acid open ground).
void PlantGenerator::_build_heather() {
	const int stems = int(_rand(8.0f, 13.0f));
	Leaf sc;
	sc.shape = LEAF_LINEAR;
	sc.width = 0.0035f; // Each card is a short leafy side shoot, its scale leaves in the texture's stripes.
	sc.fold = 0.0f;
	sc.bend = 0.0f;
	sc.segments = 1;
	Leaf bell = sc;
	bell.shape = LEAF_OVATE;
	bell.length = 0.005f;
	bell.width = 0.004f;
	bell.fold = 0.5f;
	const float flowering = rng.randf();
	auto shoot = [&](const Transform3D &p_base, float p_len, float p_r, float p_wind) -> void {
		const float bend = _rand(-0.3f, 0.3f);
		const LocalVector<Transform3D> fr = _stem_frames(p_base, p_len, bend, 3);
		_add_stem(p_base, p_len, p_r, p_r * 0.5f, bend, 3, p_wind);
		const int scales = int(p_len / 0.0055f);
		const float spike = flowering < 0.6f ? 0.35f : 0.0f; // Top share of the shoot in flower.
		for (int k = 0; k < scales; k++) {
			const float s = 0.1f + 0.9f * float(k) / MAX(scales - 1, 1);
			const Transform3D at = _frame_at(fr, s);
			const float a = k * Math::PI * 0.5f + (k / 4) * 0.4f;
			const Vector3 radial = at.basis.get_column(0) * Math::cos(a) + at.basis.get_column(1) * Math::sin(a);
			if (s > 1.0f - spike) {
				const Vector3 d = (radial - at.basis.get_column(2) * 0.3f).normalized();
				_add_leaf(SURFACE_PETAL, Transform3D(organ_frame(d, at.basis.get_column(2)), at.origin), bell, Math::lerp(p_wind, 1.0f, s));
			} else {
				sc.length = _rand(0.008f, 0.014f);
				_add_leaf(SURFACE_LEAF, Transform3D(organ_frame((radial * 0.7f + at.basis.get_column(2)).normalized(), Vector3(0, 1, 0)), at.origin), sc, Math::lerp(p_wind, 1.0f, s));
			}
		}
	};
	for (int i = 0; i < stems; i++) {
		Transform3D base = stem_base(_rand(0.0f, Math::TAU), _rand(0.8f, 1.35f));
		base.origin = Vector3(_rand(-0.05f, 0.05f), 0.0f, _rand(-0.05f, 0.05f));
		const float len = _rand(0.1f, 0.24f);
		const float bend = _rand(-0.5f, 0.0f);
		const LocalVector<Transform3D> fr = _stem_frames(base, len, bend, 4);
		_add_stem(base, len, 0.002f, 0.0012f, bend, 4, 0.0f);
		const int subs = int(_rand(7.0f, 12.0f));
		for (int k = 0; k < subs; k++) {
			Transform3D at = _frame_at(fr, _rand(0.4f, 1.0f));
			const float a = _rand(0.0f, Math::TAU);
			at.basis = organ_frame(at.basis.get_column(2) * 0.8f + Vector3(Math::cos(a), 0.7f, Math::sin(a)) * 0.6f, Vector3(0, 1, 0));
			shoot(at, _rand(0.04f, 0.09f), 0.0009f, 0.4f);
		}
	}
}

// Wetland species (docs/ideas/plant-generator.md, "Wetland species").

// Common reed (Phragmites australis): a clonal stand of tall hollow culms from creeping rhizomes;
// flat grey-green leaves set alternately in two ranks up the culm, leaning out and drooping at
// the tip; a large soft plume of purplish-brown spikelets nodding to one side.
void PlantGenerator::_build_reed() {
	const int culms = int(_rand(6.0f, 13.0f));
	const float nod = _rand(0.0f, Math::TAU); // The plumes lean one way, as in a stand in wind.
	Leaf spk;
	spk.shape = LEAF_LANCEOLATE;
	spk.length = 0.012f;
	spk.width = 0.0025f;
	spk.fold = 0.4f;
	spk.bend = 0.2f;
	spk.segments = 2;
	for (int c = 0; c < culms; c++) {
		Transform3D base = stem_base(_rand(0.0f, Math::TAU), _rand(1.42f, 1.55f));
		base.origin = Vector3(_rand(-0.22f, 0.22f), 0.0f, _rand(-0.22f, 0.22f));
		const float len = _rand(1.3f, 2.2f);
		const float bend = _rand(0.05f, 0.25f);
		const LocalVector<Transform3D> fr = _stem_frames(base, len, bend, 14);
		const Transform3D top = _add_stem(base, len, 0.0045f, 0.0025f, bend, 14, 0.0f);
		// Leaves from 15% to 85% of the culm, alternating sides.
		const int nodes = int(_rand(6.0f, 10.0f));
		const float side0 = _rand(0.0f, Math::TAU);
		for (int k = 0; k < nodes; k++) {
			const float u = Math::lerp(0.15f, 0.85f, (k + _rand(0.0f, 0.6f)) / nodes);
			const Transform3D at = _frame_at(fr, u);
			const float az = side0 + (k % 2) * Math::PI + _rand(-0.3f, 0.3f);
			const Vector3 out(Math::cos(az), 0.0f, Math::sin(az));
			const Vector3 d = (at.basis.get_column(2) * _rand(0.9f, 1.4f) + out).normalized();
			Leaf l;
			l.shape = LEAF_LINEAR;
			l.length = _rand(0.22f, 0.42f) * (1.0f - 0.35f * u);
			l.width = _rand(0.013f, 0.022f);
			l.fold = 0.15f;
			l.bend = _rand(0.8f, 1.5f);
			l.twist = _rand(-0.5f, 0.5f);
			l.segments = 8;
			_add_leaf(SURFACE_LEAF, Transform3D(organ_frame(d, Vector3(0, 1, 0)), at.origin), l, u * 0.9f, 0.9f);
		}
		// The plume: a nodding panicle of branches carrying spikelets.
		if (_rand(0.0f, 1.0f) < 0.8f) {
			const Vector3 lean(Math::cos(nod), 0.0f, Math::sin(nod));
			Transform3D pb(organ_frame((top.basis.get_column(2) + lean * 0.5f).normalized(), Vector3(0, 1, 0)), top.origin);
			const float plen = _rand(0.18f, 0.32f);
			const LocalVector<Transform3D> pf = _stem_frames(pb, plen, 0.5f, 6);
			_add_stem(pb, plen, 0.0018f, 0.0006f, 0.5f, 6, 1.0f);
			const int branches = int(_rand(10.0f, 16.0f));
			for (int b = 0; b < branches; b++) {
				const float u = float(b) / branches;
				const Transform3D at = _frame_at(pf, u * 0.9f);
				const float az = b * GOLDEN_ANGLE;
				const Vector3 side = at.basis.get_column(0) * Math::cos(az) + at.basis.get_column(1) * Math::sin(az);
				const Vector3 d = (at.basis.get_column(2) * 1.2f + side * 0.6f + lean * 0.4f - Vector3(0, 0.3f, 0)).normalized();
				const float blen = plen * (0.55f - 0.35f * u);
				const Transform3D bt(organ_frame(d, Vector3(0, 1, 0)), at.origin);
				const LocalVector<Transform3D> bf = _stem_frames(bt, blen, 0.4f, 3);
				_add_stem(bt, blen, 0.0006f, 0.0003f, 0.4f, 3, 1.0f);
				for (int k = 0; k < 5; k++) {
					const Transform3D sa = _frame_at(bf, 0.2f + 0.18f * k);
					const Vector3 sd = (sa.basis.get_column(2) + sa.basis.get_column(0) * _rand(-0.5f, 0.5f)).normalized();
					_add_leaf(SURFACE_PETAL, Transform3D(organ_frame(sd, Vector3(0, 1, 0)), sa.origin), spk, 1.0f);
				}
			}
		}
	}
}

// Bulrush or cattail (Typha latifolia): fans of long, flat strap leaves sheathing from the base,
// erect and twisting a little, the tips arching; a stiff flowering stem carries the dark brown
// "cigar" of female flowers with the paler male spike directly above it.
void PlantGenerator::_build_cattail() {
	const int shoots = int(_rand(3.0f, 6.0f));
	for (int s = 0; s < shoots; s++) {
		const Vector3 at(_rand(-0.18f, 0.18f), 0.0f, _rand(-0.18f, 0.18f));
		const float fan = _rand(0.0f, Math::PI);
		const int leaves = int(_rand(6.0f, 10.0f));
		for (int i = 0; i < leaves; i++) {
			// A flat fan: the leaves of one shoot lie in one plane, alternating sides.
			const float side = (i % 2 == 0 ? 1.0f : -1.0f) * _rand(0.05f, 0.35f);
			const Vector3 d = (Vector3(0, 1, 0) + Vector3(Math::cos(fan), 0.0f, Math::sin(fan)) * side).normalized();
			Leaf l;
			l.shape = LEAF_LINEAR;
			l.length = _rand(1.0f, 1.8f);
			l.width = _rand(0.012f, 0.02f);
			l.fold = 0.2f;
			l.bend = _rand(0.3f, 0.9f);
			l.twist = _rand(-1.2f, 1.2f);
			l.segments = 12;
			_add_leaf(SURFACE_LEAF, Transform3D(organ_frame(d, Vector3(Math::cos(fan), 0.0f, Math::sin(fan))), at + Vector3(0, 0.01f * i, 0)), l, 0.0f, 0.85f);
		}
		if (_rand(0.0f, 1.0f) < 0.6f) {
			Transform3D base = stem_base(_rand(0.0f, Math::TAU), _rand(1.5f, 1.56f));
			base.origin = at;
			const float len = _rand(1.2f, 1.7f);
			const Transform3D top = _add_stem(base, len, 0.004f, 0.003f, _rand(-0.05f, 0.05f), 10, 0.0f);
			const float spike = _rand(0.14f, 0.22f);
			const Transform3D female = _add_stem(top, spike, 0.013f, 0.012f, 0.0f, 4, 1.0f, SURFACE_CENTRE);
			_add_head(SURFACE_CENTRE, female, 0.012f, 0.008f);
			_add_stem(female, _rand(0.1f, 0.16f), 0.004f, 0.002f, 0.0f, 3, 1.0f, SURFACE_PETAL);
		}
	}
}

// Soft rush (Juncus effusus): a dense tussock of smooth, glossy, leafless green stems, the outer
// ones leaning out; a loose cluster of small pale-brown flowers bursts from the side of many
// stems about two thirds of the way up.
void PlantGenerator::_build_rush() {
	const int stems = int(_rand(45.0f, 90.0f));
	Leaf fl;
	fl.shape = LEAF_LANCEOLATE;
	fl.length = 0.006f;
	fl.width = 0.0025f;
	fl.fold = 0.5f;
	fl.bend = 0.2f;
	fl.segments = 2;
	for (int i = 0; i < stems; i++) {
		const float r = Math::sqrt(_rand(0.0f, 1.0f));
		const float az = _rand(0.0f, Math::TAU);
		Transform3D base = stem_base(az, Math::lerp(1.55f, 1.15f, r));
		base.origin = Vector3(Math::cos(az), 0.0f, Math::sin(az)) * r * 0.07f;
		const float len = _rand(0.45f, 1.0f) * (1.0f - 0.3f * r);
		const float bend = _rand(0.1f, 0.5f) * (0.5f + r);
		const LocalVector<Transform3D> fr = _stem_frames(base, len, bend, 6);
		_add_stem(base, len, 0.0016f, 0.0004f, bend, 6, 0.0f);
		if (_rand(0.0f, 1.0f) < 0.4f) {
			const Transform3D at = _frame_at(fr, _rand(0.6f, 0.75f));
			const int n = int(_rand(8.0f, 16.0f));
			for (int k = 0; k < n; k++) {
				const Vector3 d = (at.basis.get_column(0) * _rand(0.3f, 1.0f) + at.basis.get_column(2) * _rand(-0.3f, 0.8f) + at.basis.get_column(1) * _rand(-0.6f, 0.6f)).normalized();
				const float reach = _rand(0.005f, 0.03f);
				_add_leaf(SURFACE_PETAL, Transform3D(organ_frame(d, Vector3(0, 1, 0)), at.origin + d * reach), fl, 1.0f);
			}
		}
	}
}

// One floating pad: a disc with a narrow cleft from the rim to the centre, where the stalk
// joins. UVs map it onto a round leaf whose base is the cleft, so the texture's palmate veins
// spread over it.
void PlantGenerator::_add_pad(const Vector3 &p_centre, float p_radius, float p_heading) {
	Builder &b = builders[SURFACE_LEAF];
	_begin_organ(p_centre);
	const int rings = 5;
	const int sectors = 24;
	const float cleft = 0.14f; // Half-angle of the cleft, radians.
	const int first = b.verts.size();
	const float lip = _rand(0.0f, 0.012f); // An old pad's rim curls up.
	for (int r = 0; r <= rings; r++) {
		const float u = float(r) / rings;
		for (int k = 0; k <= sectors; k++) {
			const float a = p_heading + cleft + (Math::TAU - 2.0f * cleft) * k / sectors;
			const float rr = (r == 0 ? 0.0f : p_radius * u * (1.0f + 0.03f * Math::sin(a * 5.0f + p_heading)));
			const Vector3 local(Math::cos(a) * rr, 0.004f + lip * u * u * u, Math::sin(a) * rr);
			b.verts.push_back(p_centre + local);
			// Leaf frame: y along the leaf from the cleft (the base, v = 0) to the far rim.
			const Vector2 d(local.x, local.z);
			const Vector2 ax(Math::cos(p_heading + Math::PI), Math::sin(p_heading + Math::PI));
			const float along = d.dot(ax);
			const float across = d.x * -ax.y + d.y * ax.x;
			b.uvs.push_back(Vector2(0.5f + across / (2.0f * p_radius), 0.5f + along / (2.0f * p_radius)));
			b.uv2s.push_back(Vector2(0.15f * u, 0.0f));
			const float ao = 0.9f + 0.1f * u;
			b.colors.push_back(Color(ao, ao, ao));
		}
	}
	for (int r = 0; r < rings; r++) {
		for (int k = 0; k < sectors; k++) {
			const int a = first + r * (sectors + 1) + k;
			const int c = a + sectors + 1;
			b.indices.append_array(PackedInt32Array({ a, c, a + 1, a + 1, c, c + 1 }));
		}
	}
	_tag_organ(b);
}

// White water lily (Nymphaea alba): round floating pads with a cleft to the stalk, spread over
// the water from a rhizome in the mud below; a few many-petalled white flowers with a yellow
// heart of stamens, opening on the surface. The plant's origin is the water surface.
void PlantGenerator::_build_water_lily() {
	const int pads = int(_rand(4.0f, 10.0f));
	const float spread = _rand(0.25f, 0.6f);
	for (int i = 0; i < pads; i++) {
		const float a = i * GOLDEN_ANGLE + _rand(-0.3f, 0.3f);
		const float r = spread * Math::sqrt((i + 0.5f) / pads);
		const Vector3 c(Math::cos(a) * r, 0.0f, Math::sin(a) * r);
		_add_pad(c, _rand(0.09f, 0.15f), _rand(0.0f, Math::TAU));
	}
	const int flowers = int(_rand(0.0f, 3.0f));
	for (int f = 0; f < flowers; f++) {
		const float a = _rand(0.0f, Math::TAU);
		const Vector3 c = Vector3(Math::cos(a), 0.0f, Math::sin(a)) * spread * _rand(0.2f, 0.8f) + Vector3(0, 0.01f, 0);
		const int petals = int(_rand(16.0f, 24.0f));
		for (int p = 0; p < petals; p++) {
			const float pa = p * GOLDEN_ANGLE;
			const float inner = float(p) / petals; // Inner petals stand more upright.
			Leaf l;
			l.shape = LEAF_OVATE;
			l.length = _rand(0.045f, 0.06f) * (1.0f - 0.3f * inner);
			l.width = l.length * 0.42f;
			l.fold = 0.35f;
			l.bend = -0.3f;
			l.segments = 5;
			const Vector3 d = polar_dir(pa, Math::lerp(0.25f, 1.05f, inner));
			_add_leaf(SURFACE_PETAL, Transform3D(organ_frame(d, Vector3(0, 1, 0)), c), l, 0.2f);
		}
		_add_head(SURFACE_CENTRE, Transform3D(organ_frame(Vector3(0, 1, 0), Vector3(1, 0, 0)), c), 0.014f, 0.012f);
	}
}

PlantGenerator::LeafTexture PlantGenerator::_leaf_texture_spec(bool p_petal) const {
	LeafTexture t;
	t.lamina = leaf_color;
	t.vein = leaf_color.lerp(Color(0.75f, 0.85f, 0.45f), 0.45f);
	Leaf &l = t.leaf;
	if (p_petal) {
		t.venation = VENATION_PARALLEL;
		t.parallel_veins = 9;
		t.lamina = petal_color;
		t.vein = petal_color.lerp(Color(1, 1, 1), 0.25f);
		t.yellowing = 0.0f;
		t.spots = 0.0f;
		l.shape = LEAF_SPATULATE;
		t.aspect = 0.3f;
		if (preset == PRESET_DAISY) {
			t.base_tint = Color(0.95f, 0.9f, 0.55f, 0.5f);
		} else if (preset == PRESET_BUTTERCUP) {
			l.shape = LEAF_ROUND;
			t.aspect = 1.0f;
			t.base_tint = Color(0.75f, 0.6f, 0.05f, 0.6f);
		} else if (preset == PRESET_CLOVER) {
			t.base_tint = Color(0.85f, 0.55f, 0.6f, 0.5f);
		} else {
			t.base_tint = Color(0.9f, 0.55f, 0.02f, 0.4f);
		}
		return t;
	}
	switch (preset) {
		case PRESET_DANDELION:
			l.shape = LEAF_LANCEOLATE;
			l.lobes = 5;
			l.lobe_depth = 0.5f;
			t.aspect = 0.21f;
			t.vein = Color(0.55f, 0.5f, 0.3f); // Pale, often reddish midribs.
			break;
		case PRESET_PLANTAIN:
			l.shape = LEAF_LANCEOLATE;
			t.aspect = 0.12f;
			t.venation = VENATION_PARALLEL;
			t.parallel_veins = 5;
			break;
		case PRESET_CLOVER:
			l.shape = LEAF_ROUND;
			t.aspect = 0.9f;
			t.chevron = 0.55f;
			t.yellowing = 0.15f;
			break;
		case PRESET_FERN:
			l.shape = LEAF_LANCEOLATE;
			l.lobes = 7;
			l.lobe_depth = 0.45f;
			t.aspect = 0.28f;
			t.spots = 0.15f;
			break;
		case PRESET_SEDGE:
			l.shape = LEAF_LINEAR;
			t.aspect = 0.04f;
			t.venation = VENATION_PARALLEL;
			t.parallel_veins = 7;
			t.yellowing = 0.6f;
			break;
		case PRESET_DAISY:
			l.shape = LEAF_SPATULATE;
			l.lobes = 3;
			l.lobe_depth = 0.2f;
			t.aspect = 0.35f;
			break;
		case PRESET_BUTTERCUP:
			l.shape = LEAF_PALMATE;
			l.lobes = 3;
			l.lobe_depth = 0.55f;
			t.aspect = 0.6f;
			t.venation = VENATION_PALMATE;
			t.spots = 0.5f;
			break;
		case PRESET_NETTLE:
			l.shape = LEAF_OVATE;
			l.lobes = 9;
			l.lobe_depth = 0.12f;
			t.aspect = 0.6f;
			t.vein = leaf_color.lerp(Color(0.3f, 0.4f, 0.15f), 0.4f);
			t.yellowing = 0.15f;
			t.spots = 0.4f;
			break;
		case PRESET_THISTLE:
			l.shape = LEAF_LANCEOLATE;
			l.lobes = 6;
			l.lobe_depth = 0.65f;
			t.aspect = 0.34f;
			t.vein = Color(0.62f, 0.66f, 0.5f); // Pale, prominent midribs.
			t.yellowing = 0.15f;
			t.spots = 0.1f;
			break;
		case PRESET_YARROW:
			l.shape = LEAF_LINEAR;
			t.aspect = 0.1f;
			t.venation = VENATION_PARALLEL;
			t.parallel_veins = 1;
			break;
		case PRESET_DOCK:
			l.shape = LEAF_OVATE;
			l.lobes = 7;
			l.lobe_depth = 0.07f;
			t.aspect = 0.43f;
			t.vein = Color(0.45f, 0.2f, 0.12f); // Red veins.
			t.yellowing = 0.25f;
			t.spots = 0.8f; // Docks are rarely unblemished.
			break;
		case PRESET_COCKSFOOT:
		case PRESET_TIMOTHY:
			l.shape = LEAF_LINEAR;
			t.aspect = 0.03f;
			t.venation = VENATION_PARALLEL;
			t.parallel_veins = 7;
			t.yellowing = 0.5f;
			break;
		case PRESET_DRY_GRASS:
			l.shape = LEAF_LINEAR;
			t.aspect = 0.025f;
			t.venation = VENATION_PARALLEL;
			t.parallel_veins = 7;
			t.vein = leaf_color.lerp(Color(0.75f, 0.7f, 0.55f), 0.4f);
			t.yellowing = 0.0f;
			t.spots = 0.6f;
			break;
		case PRESET_LEAF_LITTER:
			l.shape = LEAF_OVATE;
			t.aspect = 0.6f;
			t.vein = leaf_color.lerp(Color(0.6f, 0.45f, 0.25f), 0.5f);
			t.yellowing = 0.0f;
			t.spots = 1.0f;
			break;
		case PRESET_MOSS:
			l.shape = LEAF_LINEAR;
			t.aspect = 0.15f;
			t.venation = VENATION_PARALLEL;
			t.parallel_veins = 1;
			t.yellowing = 0.0f;
			t.spots = 0.0f;
			break;
		case PRESET_BILBERRY:
			l.shape = LEAF_OVATE;
			l.lobes = 6;
			l.lobe_depth = 0.06f;
			t.aspect = 0.6f;
			t.yellowing = 0.2f;
			t.spots = 0.5f;
			break;
		case PRESET_BRACKEN:
			l.shape = LEAF_LANCEOLATE;
			l.lobes = 5;
			l.lobe_depth = 0.38f;
			t.aspect = 0.3f;
			t.yellowing = 0.35f;
			t.spots = 0.25f;
			break;
		case PRESET_BRAMBLE:
			l.shape = LEAF_OVATE;
			l.lobes = 10;
			l.lobe_depth = 0.07f;
			t.aspect = 0.62f;
			t.vein = Color(0.22f, 0.32f, 0.12f); // Sunken, darker veins above.
			t.yellowing = 0.25f;
			t.spots = 0.6f;
			break;
		case PRESET_FOXGLOVE:
			l.shape = LEAF_LANCEOLATE;
			l.lobes = 10;
			l.lobe_depth = 0.05f;
			t.aspect = 0.34f;
			t.vein = Color(0.32f, 0.42f, 0.2f); // Downy, pale net of veins.
			t.yellowing = 0.2f;
			t.spots = 0.2f;
			break;
		case PRESET_REED:
		case PRESET_CATTAIL:
			l.shape = LEAF_LINEAR;
			t.aspect = 0.04f;
			t.venation = VENATION_PARALLEL;
			t.parallel_veins = 9;
			t.vein = leaf_color.lerp(Color(0.6f, 0.65f, 0.45f), 0.3f);
			t.yellowing = 0.5f;
			t.spots = 0.15f;
			break;
		case PRESET_RUSH:
			l.shape = LEAF_LINEAR;
			t.aspect = 0.04f;
			t.venation = VENATION_PARALLEL;
			break;
		case PRESET_NEEDLE_LITTER:
			l.shape = LEAF_LINEAR;
			t.aspect = 0.05f;
			t.venation = VENATION_PARALLEL;
			t.parallel_veins = 2;
			t.yellowing = 0.0f;
			break;
		case PRESET_WOOD_SORREL:
			l.shape = LEAF_SPATULATE;
			l.lobes = 1;
			l.lobe_depth = 0.25f;
			t.aspect = 1.0f;
			t.yellowing = 0.1f;
			break;
		case PRESET_SEEDLING:
			l.shape = LEAF_OVATE;
			l.lobes = 6;
			l.lobe_depth = 0.04f;
			t.aspect = 0.6f;
			t.spots = 0.3f;
			break;
		case PRESET_TWIGS:
		case PRESET_FEATHER_MOSS:
			break;
		case PRESET_WOODRUFF:
			l.shape = LEAF_LANCEOLATE;
			t.aspect = 0.28f;
			t.yellowing = 0.15f;
			break;
		case PRESET_WOOD_ANEMONE:
			l.shape = LEAF_PALMATE;
			l.lobes = 3;
			l.lobe_depth = 0.65f;
			t.aspect = 0.95f;
			t.venation = VENATION_PALMATE;
			t.yellowing = 0.15f;
			break;
		case PRESET_IVY:
			l.shape = LEAF_PALMATE;
			l.lobes = 2;
			l.lobe_depth = 0.28f;
			t.aspect = 1.1f;
			t.venation = VENATION_PALMATE;
			t.vein = Color(0.38f, 0.45f, 0.3f); // Ivy's pale, raised veins.
			t.yellowing = 0.1f;
			t.spots = 0.15f;
			break;
		case PRESET_HEATHER:
			l.shape = LEAF_LINEAR;
			t.aspect = 0.3f;
			t.venation = VENATION_PARALLEL;
			t.parallel_veins = 1;
			t.yellowing = 0.2f;
			break;
		case PRESET_WATER_LILY:
			l.shape = LEAF_ROUND;
			t.aspect = 1.0f;
			t.venation = VENATION_PALMATE;
			t.vein = leaf_color.lerp(Color(0.35f, 0.45f, 0.2f), 0.12f); // Faint: a waxy, smooth pad.
			t.yellowing = 0.3f;
			t.spots = 0.5f;
			break;
		case PRESET_MAX:
			break;
	}
	return t;
}

PlantGenerator::TextureSet PlantGenerator::_bake_leaf_texture(const LeafTexture &p_spec, uint32_t p_seed) const {
	const int res = texture_size;
	RandomPCG r;
	r.seed(p_seed * 0x9E3779B97F4A7C15ull + 17);
	const Leaf &leaf = p_spec.leaf;
	// Physical blade: length 1 along y, half width hw(y); the texture spans the widest point
	// (u = 0.5 + x / aspect), matching the planar UVs of _add_leaf. Veins fill the outline
	// without its teeth (env), so they run on into every lobe the geometry cuts.
	Leaf env_leaf = leaf;
	env_leaf.lobes = 0;
	auto hw = [&](float p_y) { return 0.5f * p_spec.aspect * _leaf_width(env_leaf, CLAMP(p_y, 0.0f, 1.0f)); };
	auto hw_lobed = [&](float p_y) { return 0.5f * p_spec.aspect * _leaf_width(leaf, CLAMP(p_y, 0.0f, 1.0f)); };
	const float aspect = MAX(p_spec.aspect, 1e-3f);

	// Veins as node chains: positions, parents, radii.
	LocalVector<Vector2> pos;
	LocalVector<int> parent;
	const float step = 0.012f;
	auto add_chain = [&](Vector2 p_from, Vector2 p_to, int p_parent) {
		const int n = MAX(int(p_from.distance_to(p_to) / step), 1);
		int prev = p_parent;
		for (int i = (p_parent < 0 ? 0 : 1); i <= n; i++) {
			pos.push_back(p_from.lerp(p_to, float(i) / n));
			parent.push_back(prev);
			prev = pos.size() - 1;
		}
		return prev;
	};
	const float base = 0.02f;
	if (p_spec.venation == VENATION_PARALLEL) {
		// Monocot blades and petals: veins side by side along the length, converging at the tip.
		const int n = MAX(p_spec.parallel_veins, 1);
		for (int k = 0; k < n; k++) {
			const float f = n == 1 ? 0.0f : (float(k) / (n - 1)) * 2.0f - 1.0f;
			const int root = pos.size();
			pos.push_back(Vector2(0.0f, base));
			parent.push_back(-1);
			int prev = root;
			for (int i = 1; i <= 60; i++) {
				const float y = Math::lerp(base, 0.985f, float(i) / 60);
				pos.push_back(Vector2(f * 0.82f * hw(y), y));
				parent.push_back(prev);
				prev = pos.size() - 1;
			}
		}
	} else if (p_spec.venation == VENATION_PALMATE) {
		const int primaries = 5;
		const int root = add_chain(Vector2(0, 0), Vector2(0, base), -1);
		for (int k = 0; k < primaries; k++) {
			const float a = Math::lerp(-1.1f, 1.1f, float(k) / (primaries - 1));
			const float len = 0.9f - 0.25f * Math::abs(a);
			Vector2 end(Math::sin(a) * len, base + Math::cos(a) * len);
			end.x = CLAMP(end.x, -hw(end.y) * 0.85f, hw(end.y) * 0.85f);
			add_chain(Vector2(0, base), end, root);
		}
	} else {
		add_chain(Vector2(0, 0), Vector2(0, 0.97f), -1); // The midrib.
	}
	if (p_spec.venation != VENATION_PARALLEL) {
		// Runions open venation: sources inside the blade, each pulling its nearest vein node.
		LocalVector<Vector2> sources;
		for (int tries = 0; tries < 20000 && sources.size() < 600; tries++) {
			const Vector2 s((r.randf() - 0.5f) * aspect, r.randf());
			if (Math::abs(s.x) < hw(s.y) * 0.95f && s.y > base) {
				sources.push_back(s);
			}
		}
		const float kill = 0.03f;
		const float cell = 0.04f;
		HashMap<Vector2i, LocalVector<int>> grid;
		auto key = [&](const Vector2 &p_p) { return Vector2i(int(Math::floor(p_p.x / cell)), int(Math::floor(p_p.y / cell))); };
		for (uint32_t i = 0; i < pos.size(); i++) {
			grid[key(pos[i])].push_back(i);
		}
		for (int iter = 0; iter < 160 && !sources.is_empty(); iter++) {
			HashMap<int, Vector2> pull;
			HashMap<int, int> pull_count;
			LocalVector<Vector2> alive;
			for (const Vector2 &s : sources) {
				// Nearest node, searching rings of cells outwards.
				const Vector2i c = key(s);
				int best = -1;
				float best_d = 1e9f;
				for (int ring = 0; ring < 30 && best < 0; ring++) {
					for (int dy = -ring; dy <= ring; dy++) {
						for (int dx = -ring; dx <= ring; dx++) {
							if (MAX(Math::abs(dx), Math::abs(dy)) != ring) {
								continue;
							}
							const LocalVector<int> *cellv = grid.getptr(c + Vector2i(dx, dy));
							if (!cellv) {
								continue;
							}
							for (int ni : *cellv) {
								const float d = pos[ni].distance_squared_to(s);
								if (d < best_d) {
									best_d = d;
									best = ni;
								}
							}
						}
					}
					if (best >= 0) {
						// One more ring can hold a nearer node.
						const int ring2 = ring + 1;
						for (int dy = -ring2; dy <= ring2; dy++) {
							for (int dx = -ring2; dx <= ring2; dx++) {
								if (MAX(Math::abs(dx), Math::abs(dy)) != ring2) {
									continue;
								}
								const LocalVector<int> *cellv = grid.getptr(c + Vector2i(dx, dy));
								if (!cellv) {
									continue;
								}
								for (int ni : *cellv) {
									const float d = pos[ni].distance_squared_to(s);
									if (d < best_d) {
										best_d = d;
										best = ni;
									}
								}
							}
						}
					}
				}
				if (best < 0) {
					continue;
				}
				if (best_d < kill * kill) {
					continue; // Reached: the source dies.
				}
				alive.push_back(s);
				pull[best] = (pull.has(best) ? pull[best] : Vector2()) + (s - pos[best]).normalized();
				pull_count[best] = (pull_count.has(best) ? pull_count[best] : 0) + 1;
			}
			sources = alive;
			if (pull.is_empty()) {
				break;
			}
			for (const KeyValue<int, Vector2> &kv : pull) {
				if (kv.value.length_squared() < 1e-8f) {
					continue;
				}
				// Veins lean towards the tip, as the blade grows from its base (Runions et al.'s
				// marginal growth, approximated by a bias instead of a growing blade).
				const Vector2 np = pos[kv.key] + (kv.value.normalized() + Vector2(0.0f, 0.55f)).normalized() * step;
				pos.push_back(np);
				parent.push_back(kv.key);
				grid[key(np)].push_back(pos.size() - 1);
			}
		}
	}
	// Murray's law, from the tips back: r^3 = r_tip^3 + sum of the children's r^3.
	const int nn = pos.size();
	LocalVector<float> r3;
	r3.resize(nn);
	const float tip = 0.0011f;
	for (int i = 0; i < nn; i++) {
		r3[i] = tip * tip * tip;
	}
	for (int i = nn - 1; i >= 0; i--) {
		if (parent[i] >= 0) {
			r3[parent[i]] += r3[i];
		}
	}
	// Raster the veins into the leaf's UV space: u across the row's own width, v along the length.
	LocalVector<float> vein;
	vein.resize(res * res);
	for (float &v : vein) {
		v = 0.0f;
	}
	const float max_r = 0.0065f;
	for (int i = 0; i < nn; i++) {
		if (parent[i] < 0) {
			continue;
		}
		const float rad = MIN(Math::pow(r3[i], 1.0f / 3.0f), max_r);
		const Vector2 a = pos[parent[i]];
		const Vector2 b = pos[i];
		const int stamps = MAX(int(a.distance_to(b) / (0.5f / res)) + 1, 2);
		for (int k = 0; k < stamps; k++) {
			const Vector2 p = a.lerp(b, float(k) / (stamps - 1));
			const float u = 0.5f + p.x / aspect;
			const float ru = rad / aspect * res + 0.6f;
			const float rv = rad * res + 0.6f;
			const int cx = int(u * res);
			const int cy = int(p.y * res);
			for (int y = int(cy - rv - 1); y <= int(cy + rv + 1); y++) {
				for (int x = int(cx - ru - 1); x <= int(cx + ru + 1); x++) {
					if (x < 0 || y < 0 || x >= res || y >= res) {
						continue;
					}
					const float dx = (x + 0.5f - u * res) / ru;
					const float dy = (y + 0.5f - p.y * res) / rv;
					const float d = Math::sqrt(dx * dx + dy * dy);
					const float cov = CLAMP(1.2f - d, 0.0f, 1.0f) * MIN(1.0f, rad / 0.004f + 0.35f);
					float &vv = vein[y * res + x];
					vv = MAX(vv, cov);
				}
			}
		}
	}
	// A blurred copy: the blade rises between veins (puckering) and shades the areoles.
	LocalVector<float> blur;
	blur.resize(res * res);
	{
		LocalVector<float> tmp;
		tmp.resize(res * res);
		const int k = MAX(res / 64, 2);
		for (int y = 0; y < res; y++) {
			for (int x = 0; x < res; x++) {
				float s = 0.0f;
				for (int i = -k; i <= k; i++) {
					s += vein[y * res + CLAMP(x + i, 0, res - 1)];
				}
				tmp[y * res + x] = s / (2 * k + 1);
			}
		}
		for (int y = 0; y < res; y++) {
			for (int x = 0; x < res; x++) {
				float s = 0.0f;
				for (int i = -k; i <= k; i++) {
					s += tmp[CLAMP(y + i, 0, res - 1) * res + x];
				}
				blur[y * res + x] = s / (2 * k + 1);
			}
		}
	}
	// Blemishes: a few small spots.
	LocalVector<Vector3> spots;
	const int nspots = int(p_spec.spots * 12.0f * r.randf());
	for (int i = 0; i < nspots; i++) {
		const float sy = 0.15f + 0.8f * r.randf();
		const float sx = 0.5f + (r.randf() * 2.0f - 1.0f) * 0.8f * hw(sy) / aspect;
		spots.push_back(Vector3(sx, sy, 0.006f + 0.014f * r.randf()));
	}
	Vector<uint8_t> alb, nor, bl;
	alb.resize(res * res * 3);
	nor.resize(res * res * 3);
	bl.resize(res * res * 3);
	uint8_t *aw = alb.ptrw();
	uint8_t *nw = nor.ptrw();
	uint8_t *bw = bl.ptrw();
	LocalVector<float> height;
	height.resize(res * res);
	const uint32_t ns = p_seed * 31u + 5u;
	for (int y = 0; y < res; y++) {
		const float v = (y + 0.5f) / res;
		const float w = hw_lobed(v);
		for (int x = 0; x < res; x++) {
			const float u = (x + 0.5f) / res;
			const int i = y * res + x;
			const float vn = vein[i];
			const float bv = blur[i];
			const float px = (u - 0.5f) * aspect; // Blade units, like the veins.
			const float n1 = tex_fbm(px * 14.0f + 3.1f, v * 14.0f, ns);
			const float n2 = tex_noise(px * 60.0f, v * 60.0f, ns + 3);
			// Distance to the margin, in blade units (negative outside the outline).
			const float margin = w - Math::abs(px);
			Color c = p_spec.lamina;
			// Colour drift over the blade: lighter, yellower patches and darker ones.
			c = c.lerp(c * 1.15f + Color(0.02f, 0.02f, 0.0f), CLAMP((n1 - 0.45f) * 1.2f, 0.0f, 1.0f));
			c *= 0.88f + 0.12f * n2;
			// Areoles (the blade between veins) a touch darker than where veins run.
			c *= 0.9f + 0.1f * MIN(bv * 3.0f, 1.0f);
			c = c.lerp(p_spec.vein, vn * (p_spec.venation == VENATION_PARALLEL ? 0.3f : 0.6f));
			if (p_spec.base_tint.a > 0.0f) {
				c = c.lerp(Color(p_spec.base_tint.r, p_spec.base_tint.g, p_spec.base_tint.b), p_spec.base_tint.a * (1.0f - Math::smoothstep(0.0f, 0.35f, v)));
			}
			if (p_spec.chevron > 0.0f) {
				// White clover's pale V: a band a little past the middle, pointing to the base.
				const float cv = 0.5f + Math::abs(px) / aspect * 0.5f;
				const float band = 1.0f - Math::smoothstep(0.03f, 0.07f, Math::abs(v - cv));
				c = c.lerp(Color(0.6f, 0.7f, 0.5f), band * p_spec.chevron * (0.7f + 0.3f * n2));
			}
			// Dry, yellowed margins and tip, patchy.
			const float dry = p_spec.yellowing * (1.0f - Math::smoothstep(0.0f, 0.006f + 0.012f * n1, margin)) * Math::smoothstep(0.35f, 0.7f, n1 + 0.25f * v);
			c = c.lerp(Color(0.55f, 0.48f, 0.18f), CLAMP(dry, 0.0f, 0.85f));
			for (const Vector3 &sp : spots) {
				const float d = Vector2((u - sp.x) * aspect, v - sp.y).length();
				if (d < sp.z) {
					const float f = 1.0f - Math::smoothstep(sp.z * 0.5f, sp.z, d);
					c = c.lerp(Color(0.32f, 0.25f, 0.1f), f * 0.8f);
				}
			}
			aw[i * 3 + 0] = uint8_t(CLAMP(c.get_r8(), 0, 255));
			aw[i * 3 + 1] = uint8_t(CLAMP(c.get_g8(), 0, 255));
			aw[i * 3 + 2] = uint8_t(CLAMP(c.get_b8(), 0, 255));
			// Translucency: thin blade glows, veins block, as through a real leaf against the sun.
			const Color t = Color(p_spec.lamina.r * 1.1f + 0.05f, p_spec.lamina.g * 1.15f + 0.06f, p_spec.lamina.b * 0.6f) * (1.0f - 0.65f * vn) * (0.8f + 0.2f * n1);
			const Color tb = p_spec.base_tint.a > 0.0f || p_spec.yellowing == 0.0f ? c * (1.0f - 0.4f * vn) * 0.8f : t;
			bw[i * 3 + 0] = uint8_t(CLAMP(int(tb.r * 255.0f), 0, 255));
			bw[i * 3 + 1] = uint8_t(CLAMP(int(tb.g * 255.0f), 0, 255));
			bw[i * 3 + 2] = uint8_t(CLAMP(int(tb.b * 255.0f), 0, 255));
			height[i] = -0.9f * vn + 0.35f * (1.0f - MIN(bv * 3.0f, 1.0f)) + 0.15f * n2;
		}
	}
	// Normal map from the height (tangent space, Godot's +Y up convention).
	const float strength = 2.2f;
	for (int y = 0; y < res; y++) {
		for (int x = 0; x < res; x++) {
			const float hl = height[y * res + MAX(x - 1, 0)];
			const float hr = height[y * res + MIN(x + 1, res - 1)];
			const float hd = height[MAX(y - 1, 0) * res + x];
			const float hu = height[MIN(y + 1, res - 1) * res + x];
			const Vector3 n = Vector3((hl - hr) * strength, (hd - hu) * strength, 1.0f).normalized();
			const int i = y * res + x;
			nw[i * 3 + 0] = uint8_t(CLAMP(int((n.x * 0.5f + 0.5f) * 255.0f), 0, 255));
			nw[i * 3 + 1] = uint8_t(CLAMP(int((n.y * 0.5f + 0.5f) * 255.0f), 0, 255));
			nw[i * 3 + 2] = uint8_t(CLAMP(int((n.z * 0.5f + 0.5f) * 255.0f), 0, 255));
		}
	}
	TextureSet set;
	Ref<Image> ai = Image::create_from_data(res, res, false, Image::FORMAT_RGB8, alb);
	Ref<Image> ni = Image::create_from_data(res, res, false, Image::FORMAT_RGB8, nor);
	Ref<Image> bi = Image::create_from_data(res, res, false, Image::FORMAT_RGB8, bl);
	ai->generate_mipmaps();
	ni->generate_mipmaps(true);
	bi->generate_mipmaps();
	set.albedo = ImageTexture::create_from_image(ai);
	set.normal = ImageTexture::create_from_image(ni);
	set.backlight = ImageTexture::create_from_image(bi);
	return set;
}

// Moss cushion surface: packed shoot tips seen from above, each a small star of leaves, bright
// at the tips and dark in the gaps between shoots. Tiles seamlessly (all drawing wraps).
PlantGenerator::TextureSet PlantGenerator::_bake_moss_texture(uint32_t p_seed) const {
	const int res = texture_size;
	RandomPCG r;
	r.seed(p_seed * 0x9E3779B97F4A7C15ull + 101);
	LocalVector<float> hgt;
	hgt.resize(res * res);
	for (float &v : hgt) {
		v = 0.0f;
	}
	LocalVector<float> tone;
	tone.resize(res * res);
	for (float &v : tone) {
		v = 0.5f;
	}
	const int shoots = res * res / 90;
	for (int i = 0; i < shoots; i++) {
		const float cx = r.randf() * res;
		const float cy = r.randf() * res;
		const float tip = 0.6f + 0.4f * r.randf();
		const float tn = r.randf();
		const int arms = 5 + int(r.randf() * 3.0f);
		const float a0 = r.randf() * Math::TAU;
		const float len = res / 64.0f * (1.4f + r.randf());
		for (int k = 0; k < arms; k++) {
			const float a = a0 + k * Math::TAU / arms + (r.randf() - 0.5f) * 0.4f;
			const int steps = int(len * 2.0f) + 1;
			for (int st = 0; st <= steps; st++) {
				const float t = float(st) / steps;
				const int x = int(Math::floor(cx + Math::cos(a) * len * t));
				const int y = int(Math::floor(cy + Math::sin(a) * len * t));
				const int ix = ((x % res) + res) % res;
				const int iy = ((y % res) + res) % res;
				const float hv = tip * (1.0f - 0.6f * t);
				if (hv > hgt[iy * res + ix]) {
					hgt[iy * res + ix] = hv;
					tone[iy * res + ix] = tn;
				}
			}
		}
	}
	Vector<uint8_t> alb, nor, bl;
	alb.resize(res * res * 3);
	nor.resize(res * res * 3);
	bl.resize(res * res * 3);
	uint8_t *aw = alb.ptrw();
	uint8_t *nw = nor.ptrw();
	uint8_t *bw = bl.ptrw();
	const Color base = leaf_color;
	for (int i = 0; i < res * res; i++) {
		const float h = hgt[i];
		const float t = tone[i];
		Color c = base * (0.25f + 0.95f * h);
		c = c.lerp(Color(base.r * 1.6f + 0.04f, base.g * 1.3f + 0.03f, base.b * 0.8f), h * t * 0.6f); // Fresh, yellower tips.
		aw[i * 3 + 0] = uint8_t(CLAMP(c.get_r8(), 0, 255));
		aw[i * 3 + 1] = uint8_t(CLAMP(c.get_g8(), 0, 255));
		aw[i * 3 + 2] = uint8_t(CLAMP(c.get_b8(), 0, 255));
		const Color tb = c * h * 0.8f;
		bw[i * 3 + 0] = uint8_t(CLAMP(tb.get_r8(), 0, 255));
		bw[i * 3 + 1] = uint8_t(CLAMP(tb.get_g8(), 0, 255));
		bw[i * 3 + 2] = uint8_t(CLAMP(tb.get_b8(), 0, 255));
	}
	const float strength = 3.0f;
	for (int y = 0; y < res; y++) {
		for (int x = 0; x < res; x++) {
			const float hl = hgt[y * res + (x + res - 1) % res];
			const float hr = hgt[y * res + (x + 1) % res];
			const float hd = hgt[((y + res - 1) % res) * res + x];
			const float hu = hgt[((y + 1) % res) * res + x];
			const Vector3 n = Vector3((hl - hr) * strength, (hd - hu) * strength, 1.0f).normalized();
			const int i = y * res + x;
			nw[i * 3 + 0] = uint8_t(CLAMP(int((n.x * 0.5f + 0.5f) * 255.0f), 0, 255));
			nw[i * 3 + 1] = uint8_t(CLAMP(int((n.y * 0.5f + 0.5f) * 255.0f), 0, 255));
			nw[i * 3 + 2] = uint8_t(CLAMP(int((n.z * 0.5f + 0.5f) * 255.0f), 0, 255));
		}
	}
	TextureSet set;
	Ref<Image> ai = Image::create_from_data(res, res, false, Image::FORMAT_RGB8, alb);
	Ref<Image> ni = Image::create_from_data(res, res, false, Image::FORMAT_RGB8, nor);
	Ref<Image> bi = Image::create_from_data(res, res, false, Image::FORMAT_RGB8, bl);
	ai->generate_mipmaps();
	ni->generate_mipmaps(true);
	bi->generate_mipmaps();
	set.albedo = ImageTexture::create_from_image(ai);
	set.normal = ImageTexture::create_from_image(ni);
	set.backlight = ImageTexture::create_from_image(bi);
	return set;
}

// One shader for every generated plant (docs/ideas/plant-generator.md, "Bending and wind").
// Vertex: the plant turns about its root by a bend that grows towards the tips (UV2.x, the wind
// weight baked per vertex), length-preserving like the grass blades: the wind's lean and gusts
// (WeatherMap3D, else GrassField3D's fallback wind), and bodies pressing it down through the grass
// trample field, which wins where it is set. Leaves add a small flutter (Sousa, "Vegetation
// Procedural Animation and Shading in Crysis", GPU Gems 3, 16: main bending plus detail bending).
// Fragment: leaf textures, back light, and the same wetness and snow lines as BaseMaterial3D.
// Card reduction (docs/ideas/vegetation-lod-streaming.md, "Cheaper plants"): each island of a
// leaf or petal surface is a card. Every vertex gets its card's pivot (the card's first vertex,
// where it is built from) and, packed in w, the card's length in millimetres plus a stable rank
// in [0, 1). The LODs keep the cards ranked below a half, a quarter and an eighth, at an error
// twice the size the plant shader has already shrunk the others away at (_plant_shader()).
bool PlantGenerator::_card_lods(const Builder &p_builder, CardSurface &r_cards) const {
	const int nv = p_builder.verts.size();
	const int ni = p_builder.indices.size();
	LocalVector<int> parent;
	parent.resize(nv);
	for (int i = 0; i < nv; i++) {
		parent[i] = i;
	}
	auto find = [&](int x) {
		while (parent[x] != x) {
			parent[x] = parent[parent[x]];
			x = parent[x];
		}
		return x;
	};
	const int *ix = p_builder.indices.ptr();
	for (int i = 0; i < ni; i += 3) {
		const int a = find(ix[i]);
		const int b = find(ix[i + 1]);
		const int c = find(ix[i + 2]);
		parent[b] = a;
		parent[find(c)] = a;
	}
	// Cards in the order of their first vertex: the root of each island is its smallest index.
	HashMap<int, int> card_of_root;
	LocalVector<int> card_pivot;
	LocalVector<float> card_len;
	LocalVector<int> card_of;
	card_of.resize(nv);
	const Vector3 *vr = p_builder.verts.ptr();
	for (int i = 0; i < nv; i++) {
		const int r = find(i);
		int *c = card_of_root.getptr(r);
		if (!c) {
			card_of_root.insert(r, card_pivot.size());
			card_pivot.push_back(i);
			card_len.push_back(0.0f);
			c = card_of_root.getptr(r);
		}
		card_of[i] = *c;
		card_len[*c] = MAX(card_len[*c], vr[i].distance_to(vr[card_pivot[*c]]));
	}
	const int cards = card_pivot.size();
	if (cards < 24) {
		return false; // A few big leaves: meshoptimizer's chain is fine.
	}
	LocalVector<float> rank;
	rank.resize(cards);
	float mean_len = 0.0f;
	for (int c = 0; c < cards; c++) {
		// A Weyl sequence: even shares at every keep, unlike a hash.
		rank[c] = Math::fmod(0.5f + float(c) * 0.6180339887f, 1.0f);
		mean_len += card_len[c];
	}
	mean_len /= float(cards);
	PackedFloat32Array custom;
	custom.resize(nv * 4);
	float *cw = custom.ptrw();
	for (int i = 0; i < nv; i++) {
		const int c = card_of[i];
		const Vector3 &p = vr[card_pivot[c]];
		cw[i * 4 + 0] = p.x;
		cw[i * 4 + 1] = p.y;
		cw[i * 4 + 2] = p.z;
		cw[i * 4 + 3] = Math::floor(CLAMP(card_len[c] * 1000.0f, 1.0f, 100000.0f)) + MIN(rank[c], 0.999f);
	}
	r_cards.custom = custom;
	// Levels at errors from an eighth of a card up: each keeps the cards the shader still draws
	// there (a half of them at an error of one card length, an eighth at four), then lets
	// meshoptimizer simplify what is left within the same error, as the stock chain does.
	const bool simplify = SurfaceTool::simplify_func && SurfaceTool::simplify_scale_func;
	const float extent = simplify ? SurfaceTool::simplify_scale_func((const float *)vr, nv, sizeof(Vector3)) : 1.0f;
	LocalVector<unsigned int> src;
	LocalVector<unsigned int> dst;
	int prev = ni;
	float e = mean_len * 0.125f;
	for (int level = 0; level < 10; level++, e *= 2.0f) {
		const float keep = CLAMP(mean_len / (2.0f * e), 0.125f, 1.0f);
		src.clear();
		for (int i = 0; i < ni; i += 3) {
			if (keep >= 1.0f || rank[card_of[ix[i]]] < keep) {
				src.push_back(ix[i]);
				src.push_back(ix[i + 1]);
				src.push_back(ix[i + 2]);
			}
		}
		size_t n = src.size();
		if (simplify && n > 0) {
			dst.resize(n);
			float err = 0.0f;
			n = SurfaceTool::simplify_func(dst.ptr(), src.ptr(), src.size(), (const float *)vr, nv, sizeof(Vector3), 0, e / MAX(extent, 1e-6f), 0, &err);
		} else {
			dst = src;
		}
		if (n < 3) {
			break;
		}
		if (int(n) > prev * 4 / 5) {
			continue; // Not worth a level.
		}
		PackedInt32Array lod;
		lod.resize(n);
		for (size_t k = 0; k < n; k++) {
			lod.set(k, dst[k]);
		}
		r_cards.lods[e] = lod;
		prev = n;
		if (keep <= 0.125f && n < 36) {
			break;
		}
	}
	r_cards.last_card_error = mean_len * 4.0f;
	r_cards.last_card_indices = prev;
	return true;
}

// Shared by the plant shader and the impostor shader (plant_impostor.cpp): wind, wetness, snow.
String PlantGenerator::shader_common_code() {
	String code = WeatherMap3D::get_wind_shader_code();
	code += VegetationLight::SHADER_CODE;
	// VegetationField3D packs the light a plant grew in into the fraction of its range (custom y).
	code += "float vegetation_light_of(float y) { float f = fract(y); return f > 0.5 / 101.0 ? f * (101.0 / 100.0) * 1.5 : 1.0; }\n";
	// The wind code already declares weather_map_rect: the wetness lines without their globals.
	code += BaseMaterial3D::get_wetness_functions_code(false);
	code += BaseMaterial3D::get_snow_shader_code();
	code += R"(
global uniform sampler2D weather_state_map;

float weather_wetness_at(vec3 p_world_pos) {
	if (weather_map_rect.z <= 0.0) {
		return 0.0;
	}
	vec2 half_texel = 0.5 / vec2(textureSize(weather_state_map, 0));
	vec2 uv = clamp((p_world_pos.xz - weather_map_rect.xy) / weather_map_rect.zw, half_texel, 1.0 - half_texel);
	return textureLod(weather_state_map, uv, 0.0).g;
}

global uniform vec4 grass_wind; // xy: fallback wind (m/s, x and z), z: the grass's response (rad per m/s).

vec3 plant_wind(vec3 wp, float h) {
	if (weather_map_rect.z > 0.0) {
		return weather_wind(wp, h);
	}
	vec2 w = grass_wind.xy;
	float s = length(w);
	if (s < 1e-4) {
		return vec3(0.0);
	}
	vec2 d = w / s;
	float phase = dot(wp.xz, d) * 0.12 - TIME * s * 0.12;
	float g = 1.0 + 0.45 * sin(phase) + 0.2 * sin(phase * 2.7 + dot(wp.xz, vec2(-d.y, d.x)) * 0.2);
	return vec3(w.x, 0.0, w.y) * g;
}
)";
	return code;
}

Ref<Shader> PlantGenerator::shaders[2];

Ref<Shader> PlantGenerator::_plant_shader(bool p_textured) {
	Ref<Shader> &sh = shaders[p_textured ? 1 : 0];
	if (sh.is_valid()) {
		return sh;
	}
	String code = "shader_type spatial;\nrender_mode cull_disabled;\n\n";
	if (p_textured) {
		code += "#define PLANT_TEXTURED\n";
	}
	code += shader_common_code();
	code += R"(
global uniform sampler2D grass_trample_map;
global uniform vec4 grass_trample_rect;

uniform vec3 base_color : source_color = vec3(1.0);
uniform vec3 backlight_color : source_color = vec3(0.0);
uniform float roughness = 0.7;
uniform vec3 dead_color : source_color = vec3(0.36, 0.24, 0.11);
uniform float stiffness = 1.0; // Over the grass: 1 bends as much, 2 half as much.
uniform float height = 0.3; // The plant's height, for the wind profile and the flutter.
#ifdef PLANT_TEXTURED
uniform sampler2D albedo_tex : source_color, filter_linear_mipmap_anisotropic;
uniform sampler2D normal_tex : hint_normal, filter_linear_mipmap_anisotropic;
uniform sampler2D backlight_tex : hint_default_black, filter_linear_mipmap_anisotropic; // Linear, as BaseMaterial3D reads it.
#endif

varying vec3 v_world;
varying vec3 v_geo_n;
varying float v_light;
uniform float light_response = 1.0;


vec3 rotate_about(vec3 v, vec3 axis, float a) {
	return v * cos(a) + cross(axis, v) * sin(a) + axis * dot(axis, v) * (1.0 - cos(a));
}

void vertex() {
	// Card reduction (docs/ideas/vegetation-lod-streaming.md): as a leaf card shrinks below a few
	// pixels, cards drop out in rank order, shrinking to their pivot, and the survivors grow to
	// keep the covered area (SpeedTree leaf reduction, Nanite Preserve Area). The mesh's LODs drop
	// a card's triangles only once it is gone here. Projected size at a 1080p, 60 degree view from
	// the main camera, so shadows match.
	if (CUSTOM0.w >= 1.0) {
		vec3 pivot = CUSTOM0.xyz;
		float card_len = floor(CUSTOM0.w) * 0.001 * length(MODEL_MATRIX[0].xyz);
		float rank = fract(CUSTOM0.w);
		float d = max(length((MODEL_MATRIX * vec4(pivot, 1.0)).xyz - MAIN_CAM_INV_VIEW_MATRIX[3].xyz), 0.01);
		float keep = clamp(card_len / d * 935.0 / 4.0, 0.125, 1.0);
		float sc = smoothstep(rank, rank + 0.1, keep * 1.1) * inversesqrt(keep);
		VERTEX = pivot + (VERTEX - pivot) * sc;
	}
	float w = clamp(UV2.x, 0.0, 1.0);
	vec3 root = MODEL_MATRIX[3].xyz;
	mat3 to_local = inverse(mat3(MODEL_MATRIX));
	vec3 rest_world = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	if (w > 0.0) {
		float resp = (grass_wind.z > 0.0 ? grass_wind.z : 0.12) / max(stiffness, 0.05);
		vec3 wind = to_local * plant_wind(root, max(height * 0.66, 0.05));
		vec2 wl = wind.xz;
		float ws = length(wl);
		vec2 off = ws > 1e-4 ? wl / ws * min(ws * resp, 1.1) : vec2(0.0);
		// Bodies: the trample field lays the plant down along the push, replacing the wind where
		// it is set. Each basal organ (a frond, a cane, a tiller) turns about its own base, whose
		// place UV2.y carries; the push's direction comes from the field at that base and at this
		// part, its strength from whichever is pressed harder.
		float turns = floor(UV2.y);
		float dist = (UV2.y - turns) * 2.0;
		float ha = (turns + 0.5) / 1024.0 * 6.2831853;
		vec3 pivot = vec3(cos(ha) * dist, 0.0, sin(ha) * dist);
		vec3 rel = VERTEX - pivot;
		vec2 hd = rel.xz; // The way this part reaches out from its base.
		float hl = length(hd);
		hd = hl > 1e-4 ? hd / hl : vec2(0.0);
		float upright = max(rel.y, 0.0) / max(max(rel.y, 0.0) + hl, 1e-4);
		float press = 0.0;
		vec2 push = vec2(0.0);
		if (grass_trample_rect.z > 0.0) {
			vec2 pw = (MODEL_MATRIX * vec4(pivot, 1.0)).xz;
			vec2 ub = (pw - grass_trample_rect.xy) / grass_trample_rect.zw;
			vec2 uv = (rest_world.xz - grass_trample_rect.xy) / grass_trample_rect.zw;
			vec2 tb = textureLod(grass_trample_map, ub, 0.0).rg * float(all(greaterThan(ub, vec2(0.0))) && all(lessThan(ub, vec2(1.0))));
			vec2 tv = textureLod(grass_trample_map, uv, 0.0).rg * float(all(greaterThan(uv, vec2(0.0))) && all(lessThan(uv, vec2(1.0))));
			press = smoothstep(0.0, 1.0, min(max(length(tb), length(tv)), 1.0));
			vec2 b = tb * 2.0 + tv;
			if (press > 1e-3 && length(b) > 1e-4) {
				push = normalize((to_local * vec3(b.x, 0.0, b.y)).xz) * 1.2 / max(sqrt(stiffness), 0.3);
			}
		}
		// Sway: on top of the mean lean, each organ rocks at its own natural frequency, driven by
		// the turbulence (de Langre 2008: herbs and grasses 0.5 to 4 Hz, lower for taller and
		// softer stems). An ellipse, wider along the wind than across it; the phase is the organ's.
		if (ws > 1e-4) {
			vec2 wd = wl / ws;
			vec2 org = (MODEL_MATRIX * vec4(pivot, 1.0)).xz;
			float ph = fract(sin(dot(org, vec2(12.9898, 78.233))) * 43758.5453) * 6.2831853;
			float f = clamp(0.9 * sqrt(stiffness) / pow(max(height, 0.05), 0.6), 0.5, 4.0);
			float amp = min(ws * resp, 1.1) * 0.22 * clamp(ws * 0.25, 0.3, 1.0);
			float t = TIME * 6.2831853 * f + ph;
			off += (wd * sin(t) + vec2(-wd.y, wd.x) * 0.4 * sin(t * 0.83 + 1.7)) * amp;
		}
		// Upright parts lean about their base along the push (or the wind). Parts that spread out
		// (fronds, arching canes, rosette leaves) cannot be leaned sideways without rolling them
		// over: they are laid down by the push's outward share and swung about their base by its
		// sideways share.
		float out_share = dot(push, hd);
		vec2 side = push - out_share * hd;
		out_share = abs(out_share); // Pressed down and outwards, never folded back over its base.
		vec2 lean = mix(off, mix(hd * out_share, push, upright), press);
		float ang = min(length(lean), 1.3) * w;
		float rest_y = VERTEX.y;
		if (ang > 1e-4) {
			vec3 axis = normalize(cross(vec3(0.0, 1.0, 0.0), vec3(lean.x, 0.0, lean.y)));
			rel = rotate_about(rel, axis, ang);
			NORMAL = rotate_about(NORMAL, axis, ang);
		}
		float swing = press * (1.0 - upright) * dot(side, vec2(-hd.y, hd.x)) * 0.6 * w;
		if (abs(swing) > 1e-4) {
			rel = rotate_about(rel, vec3(0.0, -1.0, 0.0), swing);
			NORMAL = rotate_about(NORMAL, vec3(0.0, -1.0, 0.0), swing);
		}
		VERTEX = pivot + rel;
		// Spreading parts laid down past flat would go under the ground.
		VERTEX.y = max(VERTEX.y, min(rest_y, 0.01 * w));
		// Leaf flutter: a few millimetres along the normal, out of phase across the plant.
		float ph = dot(rest_world, vec3(7.1, 3.3, 5.7));
		VERTEX += NORMAL * sin(TIME * (6.0 + 3.0 * fract(ph)) + ph) * 0.004 * w * clamp(ws * 0.2, 0.0, 1.0) * clamp(height * 4.0, 0.3, 1.5);
	}
	// Far distance (docs/ideas/plant-generator.md, stage 6): VegetationField3D gives each instance
	// its blue-noise rank among the plants kept there (x) and its species' range (y). The share
	// kept falls from 1 at 35% of the range to 0 at the range; a plant shrinks to nothing as the
	// share passes its rank, and the survivors grow a little to hold the cover (Horizon Zero
	// Dawn's thinning by rank, Ghost of Tsushima's widening). Measured from the main camera, so
	// shadow and other passes thin the same plants; the ground's colour takes over (vegetation_far()).
	v_light = vegetation_light_of(INSTANCE_CUSTOM.y);
	if (INSTANCE_CUSTOM.y > 0.0) {
		float range = floor(INSTANCE_CUSTOM.y);
		float d = length(MODEL_MATRIX[3].xyz - MAIN_CAM_INV_VIEW_MATRIX[3].xyz);
		float keep = clamp((range - d) / (range * 0.65), 0.0, 1.0);
		float s = smoothstep(INSTANCE_CUSTOM.x, INSTANCE_CUSTOM.x + 0.12, keep * 1.12);
		VERTEX *= s * (1.0 + 0.25 * (1.0 - keep));
	}
	v_world = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	v_geo_n = normalize(mat3(MODEL_MATRIX) * NORMAL);
}

void fragment() {
	vec3 albedo = base_color * COLOR.rgb;
	vec3 back = backlight_color;
#ifdef PLANT_TEXTURED
	albedo *= texture(albedo_tex, UV).rgb;
	back = texture(backlight_tex, UV).rgb;
	NORMAL_MAP = texture(normal_tex, UV).rgb;
#endif
	// Wear (vertex alpha): the blade yellows, then browns; dead tissue keeps the veins' pattern
	// (the texture's brightness), lets less light through and turns matte.
	float dead = 1.0 - COLOR.a;
	if (dead > 0.0) {
		float luma = dot(albedo, vec3(0.2126, 0.7152, 0.0722));
		vec3 yellow = vec3(0.30, 0.20, 0.025) * clamp(luma * 8.0, 0.6, 1.4);
		vec3 brown = dead_color * clamp(luma * 6.0, 0.55, 1.5);
		albedo = mix(albedo, mix(yellow, brown, smoothstep(0.35, 0.85, dead)), smoothstep(0.0, 0.5, dead));
		back *= 1.0 - 0.7 * dead;
	}
	// Living tissue acclimates to its light (sun and shade leaves); litter and dead tissue do not.
	albedo = mix(albedo, vegetation_light_tint(albedo, v_light), light_response * (1.0 - dead));
	float rough = roughness;
	float metallic = 0.0;
	float specular = 0.35; // Leaf cuticle.
	vec3 n_world = normalize((INV_VIEW_MATRIX * vec4(NORMAL, 0.0)).xyz);
	// Rain: leaves barely soak (low porosity), they darken a little and turn glossy.
	weather_wetness_apply(weather_wetness_at(v_world), n_world.y, metallic, 0.15, albedo, rough);
	// Snow lies on the faces that look up.
	vec4 snow_s = snow_cover_at(v_world);
	float snow = snow_cover_weight(snow_s.x, 60.0, n_world, n_world);
	snow_cover_apply(snow, snow_s.y, snow_s.z, albedo, rough, metallic, specular);
	ALBEDO = albedo;
	ROUGHNESS = rough;
	METALLIC = metallic;
	SPECULAR = specular;
	BACKLIGHT = back * (1.0 - snow);
}
)";
	sh.instantiate();
	sh->set_code(code);
	return sh;
}

void PlantGenerator::free_shaders() {
	shaders[0].unref();
	shaders[1].unref();
	impostor_shader.unref();
}

// Climber leaves for the trees' vines, one leaf per mesh (docs/ideas/plant-generator.md,
// "Climber leaves"). Ivy (Hedera helix): palmate, 3 to 5 shallow lobes, dark and glossy with pale
// veins. Old man's beard (Clematis vitalba): pinnate, five ovate leaflets on a rachis that twines.
// Honeysuckle (Lonicera periclymenum): ovate to elliptic, entire, short-stalked, dull green.
Array PlantGenerator::make_climber_leaves(int p_climber, int p_variants) {
	Array out;
	const int climber = p_climber == PRESET_IVY ? CLIMBER_IVY : CLAMP(p_climber, 0, int(CLIMBER_HONEYSUCKLE));
	Ref<PlantGenerator> g;
	g.instantiate();
	g->preset = PRESET_IVY; // Ivy's texture spec, adjusted below for the others.
	g->leaf_wear = 0.25f; // A few bites and the odd yellowing leaf among the four.
	Leaf l;
	l.length = 1.0f;
	l.segments = 8;
	float roughness = 0.6f;
	LeafTexture t = g->_leaf_texture_spec(false);
	switch (climber) {
		case CLIMBER_IVY:
			g->leaf_color = Color(0.05f, 0.15f, 0.04f);
			l.shape = LEAF_PALMATE;
			l.lobes = 2;
			l.lobe_depth = 0.28f;
			l.width = 1.1f;
			l.fold = 0.1f;
			l.bend = 0.15f;
			break;
		case CLIMBER_CLEMATIS:
			g->leaf_color = Color(0.1f, 0.24f, 0.06f);
			l.shape = LEAF_OVATE;
			l.width = 0.55f;
			l.fold = 0.15f;
			l.bend = 0.3f;
			roughness = 0.72f;
			break;
		default: // CLIMBER_HONEYSUCKLE
			g->leaf_color = Color(0.11f, 0.25f, 0.08f);
			l.shape = LEAF_OVATE;
			l.width = 0.55f;
			l.fold = 0.2f;
			l.bend = 0.25f;
			l.petiole = 0.06f;
			roughness = 0.75f;
			break;
	}
	t = g->_leaf_texture_spec(false);
	t.leaf = l;
	t.aspect = l.width;
	if (climber == CLIMBER_IVY) {
		t.venation = VENATION_PALMATE;
		t.vein = Color(0.38f, 0.45f, 0.3f);
		t.yellowing = 0.1f;
		t.spots = 0.15f;
	} else {
		t.venation = VENATION_PINNATE;
		t.yellowing = 0.2f;
		t.spots = 0.2f;
	}
	const TextureSet ts = g->_bake_leaf_texture(t, 9001u + uint32_t(climber) * 101u);
	Ref<ShaderMaterial> mat = g->_textured_material(ts, roughness);
	mat->set_shader_parameter("height", 1.0f);
	mat->set_shader_parameter("stiffness", 1.5f);
	mat->set_shader_parameter("light_response", 1.0f);
	const Basis flat = organ_frame(Vector3(0, 0, 1), Vector3(0, 1, 0));
	for (int v = 0; v < MAX(p_variants, 1); v++) {
		g->seed = v;
		g->rng.seed(uint64_t(9001 + climber * 31 + v));
		for (Builder &b : g->builders) {
			b = Builder();
		}
		g->organ = 0;
		g->organ_bases.clear();
		Leaf lv = l;
		lv.width = l.width * g->_rand(0.9f, 1.1f);
		lv.bend = l.bend * g->_rand(0.7f, 1.3f);
		lv.twist = g->_rand(-0.15f, 0.15f);
		if (climber == CLIMBER_CLEMATIS) {
			// A rachis with two opposite pairs of stalked leaflets and one at the tip.
			Leaf rachis;
			rachis.shape = LEAF_LINEAR;
			rachis.length = 0.62f;
			rachis.width = 0.025f;
			rachis.fold = 0.0f;
			rachis.bend = 0.2f;
			rachis.segments = 4;
			g->_add_leaf(SURFACE_LEAF, Transform3D(flat, Vector3()), rachis, 0.0f);
			const float at[2] = { 0.3f, 0.58f };
			for (int k = 0; k < 2; k++) {
				for (int side = -1; side <= 1; side += 2) {
					Leaf lf = lv;
					lf.length = g->_rand(0.36f, 0.44f) * (k == 0 ? 1.0f : 0.92f);
					lf.width = lf.length * lv.width;
					const Vector3 d = Vector3(float(side), 0.0f, 0.55f).normalized();
					g->_add_leaf(SURFACE_LEAF, Transform3D(organ_frame(d, Vector3(0, 1, 0)), Vector3(0.012f * side, 0.002f, at[k] * 0.62f)), lf, 0.3f + 0.3f * k);
				}
			}
			Leaf tip = lv;
			tip.length = 0.38f;
			tip.width = tip.length * lv.width;
			g->_add_leaf(SURFACE_LEAF, Transform3D(flat, Vector3(0.0f, 0.003f, 0.6f)), tip, 0.8f);
		} else {
			lv.width = lv.width * lv.length;
			g->_add_leaf(SURFACE_LEAF, Transform3D(flat, Vector3()), lv, 0.0f);
		}
		Builder &b = g->builders[SURFACE_LEAF];
		b.normals.resize(b.verts.size());
		b.normals.fill(Vector3());
		Vector3 *nw = b.normals.ptrw();
		const Vector3 *vr = b.verts.ptr();
		int *iw = b.indices.ptrw();
		for (int i = 0; i < b.indices.size(); i += 3) {
			SWAP(iw[i + 1], iw[i + 2]);
		}
		for (int i = 0; i < b.indices.size(); i += 3) {
			const int i0 = b.indices[i];
			const int i1 = b.indices[i + 2];
			const int i2 = b.indices[i + 1];
			const Vector3 fn = (vr[i1] - vr[i0]).cross(vr[i2] - vr[i0]);
			nw[i0] += fn;
			nw[i1] += fn;
			nw[i2] += fn;
		}
		for (int i = 0; i < b.verts.size(); i++) {
			nw[i] = nw[i].length_squared() > 0.0f ? nw[i].normalized() : Vector3(0, 1, 0);
		}
		Array arrays;
		arrays.resize(Mesh::ARRAY_MAX);
		arrays[Mesh::ARRAY_VERTEX] = b.verts;
		arrays[Mesh::ARRAY_NORMAL] = b.normals;
		arrays[Mesh::ARRAY_TEX_UV] = b.uvs;
		arrays[Mesh::ARRAY_TEX_UV2] = b.uv2s;
		arrays[Mesh::ARRAY_COLOR] = b.colors;
		arrays[Mesh::ARRAY_INDEX] = b.indices;
		Ref<ArrayMesh> m;
		m.instantiate();
		m->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
		m->surface_set_material(0, mat);
		m->set_name(vformat("climber_leaf_%d_%d", climber, v));
		out.push_back(m);
	}
	return out;
}

Ref<ShaderMaterial> PlantGenerator::_textured_material(const TextureSet &p_set, float p_roughness) const {
	Ref<ShaderMaterial> m;
	m.instantiate();
	m->set_shader(_plant_shader(true));
	m->set_shader_parameter("albedo_tex", p_set.albedo);
	m->set_shader_parameter("normal_tex", p_set.normal);
	m->set_shader_parameter("backlight_tex", p_set.backlight);
	m->set_shader_parameter("roughness", p_roughness);
	return m;
}

Ref<ShaderMaterial> PlantGenerator::_material(const Color &p_color, float p_roughness, bool p_backlight) const {
	Ref<ShaderMaterial> m;
	m.instantiate();
	m->set_shader(_plant_shader(false));
	m->set_shader_parameter("base_color", p_color);
	m->set_shader_parameter("roughness", p_roughness);
	// Thin leaves and petals pass light through (the engine's back lighting, raster and traced).
	m->set_shader_parameter("backlight_color", p_backlight ? p_color * 0.6f : Color(0, 0, 0));
	return m;
}

void PlantGenerator::apply_preset_colors() {
	stem_color = Color(0.2f, 0.36f, 0.1f);
	leaf_color = Color(0.12f, 0.3f, 0.07f);
	switch (preset) {
		case PRESET_DANDELION:
			petal_color = Color(1.0f, 0.75f, 0.03f);
			centre_color = Color(0.95f, 0.65f, 0.05f);
			break;
		case PRESET_PLANTAIN:
			leaf_color = Color(0.13f, 0.27f, 0.07f);
			petal_color = Color(0.9f, 0.88f, 0.75f);
			centre_color = Color(0.22f, 0.16f, 0.08f);
			break;
		case PRESET_CLOVER:
			leaf_color = Color(0.1f, 0.27f, 0.07f);
			petal_color = Color(0.95f, 0.9f, 0.88f);
			centre_color = Color(0.9f, 0.9f, 0.85f);
			break;
		case PRESET_FERN:
			leaf_color = Color(0.14f, 0.32f, 0.06f);
			stem_color = Color(0.25f, 0.3f, 0.1f);
			break;
		case PRESET_SEDGE:
			leaf_color = Color(0.2f, 0.3f, 0.1f);
			centre_color = Color(0.3f, 0.22f, 0.1f);
			break;
		case PRESET_DAISY:
			petal_color = Color(0.95f, 0.95f, 0.92f);
			centre_color = Color(1.0f, 0.75f, 0.05f);
			break;
		case PRESET_BUTTERCUP:
			petal_color = Color(1.0f, 0.82f, 0.02f);
			centre_color = Color(0.7f, 0.75f, 0.1f);
			break;
		case PRESET_NETTLE:
			leaf_color = Color(0.09f, 0.22f, 0.05f);
			stem_color = Color(0.16f, 0.26f, 0.08f);
			centre_color = Color(0.32f, 0.38f, 0.16f); // Flower strings.
			break;
		case PRESET_THISTLE:
			leaf_color = Color(0.13f, 0.25f, 0.09f);
			stem_color = Color(0.2f, 0.3f, 0.12f);
			petal_color = Color(0.55f, 0.12f, 0.45f);
			break;
		case PRESET_YARROW:
			leaf_color = Color(0.13f, 0.27f, 0.08f);
			stem_color = Color(0.25f, 0.33f, 0.14f);
			petal_color = Color(0.92f, 0.9f, 0.84f);
			break;
		case PRESET_DOCK:
			leaf_color = Color(0.12f, 0.26f, 0.06f);
			centre_color = Color(0.36f, 0.16f, 0.08f); // Ripening stem and branches.
			petal_color = Color(0.45f, 0.2f, 0.08f); // Winged seeds.
			break;
		case PRESET_COCKSFOOT:
			leaf_color = Color(0.14f, 0.27f, 0.12f); // Bluish green.
			stem_color = Color(0.3f, 0.36f, 0.16f);
			petal_color = Color(0.36f, 0.38f, 0.2f);
			break;
		case PRESET_TIMOTHY:
			leaf_color = Color(0.15f, 0.3f, 0.08f);
			stem_color = Color(0.3f, 0.37f, 0.15f);
			centre_color = Color(0.38f, 0.42f, 0.25f);
			break;
		case PRESET_DRY_GRASS:
			leaf_color = Color(0.5f, 0.44f, 0.27f);
			break;
		case PRESET_LEAF_LITTER:
			leaf_color = Color(0.33f, 0.22f, 0.11f);
			stem_color = Color(0.22f, 0.16f, 0.1f);
			break;
		case PRESET_MOSS:
			leaf_color = Color(0.17f, 0.3f, 0.05f);
			centre_color = Color(0.12f, 0.21f, 0.045f); // The packed shoots below the tips.
			break;
		case PRESET_BILBERRY:
			leaf_color = Color(0.17f, 0.36f, 0.08f);
			stem_color = Color(0.2f, 0.4f, 0.12f); // Green, angled stems.
			petal_color = Color(0.08f, 0.1f, 0.22f); // Berries.
			break;
		case PRESET_BRACKEN:
			leaf_color = Color(0.2f, 0.36f, 0.07f);
			stem_color = Color(0.32f, 0.3f, 0.12f);
			break;
		case PRESET_BRAMBLE:
			leaf_color = Color(0.09f, 0.22f, 0.05f); // Dark, glossy above.
			stem_color = Color(0.3f, 0.12f, 0.1f); // Red-purple canes.
			petal_color = Color(0.06f, 0.03f, 0.05f); // Ripe berries.
			break;
		case PRESET_FOXGLOVE:
			leaf_color = Color(0.16f, 0.3f, 0.1f);
			stem_color = Color(0.2f, 0.3f, 0.12f);
			petal_color = Color(0.55f, 0.16f, 0.42f);
			break;
		case PRESET_REED:
			leaf_color = Color(0.2f, 0.32f, 0.14f); // Grey-green.
			stem_color = Color(0.3f, 0.36f, 0.16f);
			petal_color = Color(0.3f, 0.2f, 0.2f); // Purplish-brown plume.
			break;
		case PRESET_CATTAIL:
			leaf_color = Color(0.2f, 0.34f, 0.12f);
			stem_color = Color(0.24f, 0.34f, 0.14f);
			centre_color = Color(0.36f, 0.21f, 0.1f); // The brown cigar.
			petal_color = Color(0.55f, 0.48f, 0.25f); // The spent male spike.
			break;
		case PRESET_RUSH:
			stem_color = Color(0.18f, 0.36f, 0.09f); // Glossy green stems.
			leaf_color = Color(0.18f, 0.36f, 0.09f);
			petal_color = Color(0.45f, 0.36f, 0.18f); // Pale brown flowers.
			break;
		case PRESET_WATER_LILY:
			leaf_color = Color(0.1f, 0.24f, 0.07f);
			petal_color = Color(0.92f, 0.92f, 0.88f);
			centre_color = Color(0.95f, 0.75f, 0.1f);
			break;
		case PRESET_NEEDLE_LITTER:
			leaf_color = Color(0.42f, 0.24f, 0.1f); // Rusty dead needles.
			centre_color = Color(0.3f, 0.19f, 0.09f);
			petal_color = Color(0.36f, 0.23f, 0.12f); // Cones.
			stem_color = Color(0.3f, 0.24f, 0.17f);
			break;
		case PRESET_TWIGS:
			stem_color = Color(0.33f, 0.28f, 0.22f); // Weathered grey-brown bark.
			break;
		case PRESET_WOOD_SORREL:
			leaf_color = Color(0.2f, 0.42f, 0.1f); // Bright, thin, shade leaves.
			stem_color = Color(0.4f, 0.22f, 0.15f);
			petal_color = Color(0.95f, 0.92f, 0.95f);
			break;
		case PRESET_SEEDLING:
			leaf_color = Color(0.2f, 0.38f, 0.1f);
			stem_color = Color(0.32f, 0.25f, 0.18f);
			break;
		case PRESET_FEATHER_MOSS:
			leaf_color = Color(0.32f, 0.4f, 0.1f); // Yellow-green.
			centre_color = Color(0.3f, 0.38f, 0.1f);
			break;
		case PRESET_WOODRUFF:
			leaf_color = Color(0.13f, 0.33f, 0.07f);
			stem_color = Color(0.2f, 0.34f, 0.1f);
			petal_color = Color(0.95f, 0.95f, 0.92f);
			break;
		case PRESET_WOOD_ANEMONE:
			leaf_color = Color(0.13f, 0.3f, 0.07f);
			stem_color = Color(0.32f, 0.2f, 0.16f); // Reddish stems.
			petal_color = Color(0.95f, 0.93f, 0.93f);
			centre_color = Color(0.85f, 0.7f, 0.15f);
			break;
		case PRESET_IVY:
			leaf_color = Color(0.05f, 0.15f, 0.04f); // Dark, glossy.
			stem_color = Color(0.22f, 0.2f, 0.12f);
			break;
		case PRESET_HEATHER:
			leaf_color = Color(0.13f, 0.22f, 0.07f); // Dark, dull green.
			stem_color = Color(0.25f, 0.18f, 0.12f);
			petal_color = Color(0.55f, 0.3f, 0.5f); // Pink-mauve.
			break;
		case PRESET_MAX:
			break;
	}
	emit_changed();
}

Ref<ArrayMesh> PlantGenerator::generate() {
	for (Builder &b : builders) {
		b = Builder();
	}
	plant_height = 0.0f;
	organ = 0;
	organ_bases.clear();
	leaf_serial = 0;
	rng.seed(uint64_t(seed) * 0x9E3779B97F4A7C15ull + uint64_t(preset) + 1);
	switch (preset) {
		case PRESET_DANDELION:
			_build_dandelion();
			break;
		case PRESET_PLANTAIN:
			_build_plantain();
			break;
		case PRESET_CLOVER:
			_build_clover();
			break;
		case PRESET_FERN:
			_build_fern();
			break;
		case PRESET_SEDGE:
			_build_sedge();
			break;
		case PRESET_DAISY:
			_build_daisy();
			break;
		case PRESET_BUTTERCUP:
			_build_buttercup();
			break;
		case PRESET_NETTLE:
			_build_nettle();
			break;
		case PRESET_THISTLE:
			_build_thistle();
			break;
		case PRESET_YARROW:
			_build_yarrow();
			break;
		case PRESET_DOCK:
			_build_dock();
			break;
		case PRESET_COCKSFOOT:
			_build_cocksfoot();
			break;
		case PRESET_TIMOTHY:
			_build_timothy();
			break;
		case PRESET_DRY_GRASS:
			_build_dry_grass();
			break;
		case PRESET_LEAF_LITTER:
			_build_leaf_litter();
			break;
		case PRESET_MOSS:
			_build_moss();
			break;
		case PRESET_BILBERRY:
			_build_bilberry();
			break;
		case PRESET_BRACKEN:
			_build_bracken();
			break;
		case PRESET_BRAMBLE:
			_build_bramble();
			break;
		case PRESET_FOXGLOVE:
			_build_foxglove();
			break;
		case PRESET_REED:
			_build_reed();
			break;
		case PRESET_CATTAIL:
			_build_cattail();
			break;
		case PRESET_RUSH:
			_build_rush();
			break;
		case PRESET_WATER_LILY:
			_build_water_lily();
			break;
		case PRESET_NEEDLE_LITTER:
			_build_needle_litter();
			break;
		case PRESET_TWIGS:
			_build_twigs();
			break;
		case PRESET_WOOD_SORREL:
			_build_wood_sorrel();
			break;
		case PRESET_SEEDLING:
			_build_seedling();
			break;
		case PRESET_FEATHER_MOSS:
			_build_moss();
			break;
		case PRESET_WOODRUFF:
			_build_woodruff();
			break;
		case PRESET_WOOD_ANEMONE:
			_build_wood_anemone();
			break;
		case PRESET_IVY:
			_build_ivy();
			break;
		case PRESET_HEATHER:
			_build_heather();
			break;
		case PRESET_MAX:
			break;
	}
	// Each vertex carries its organ's base in UV2.y: whole steps the heading from the plant's
	// origin (1024 to a turn), the fraction the distance over 2 m. 0 is the origin itself.
	for (Builder &b : builders) {
		_tag_organ(b);
		Vector2 *uw = b.uv2s.ptrw();
		for (int i = 0; i < b.verts.size(); i++) {
			const int o = b.organs[i];
			if (o <= 0) {
				continue;
			}
			const Vector3 &base = organ_bases[o - 1];
			const Vector2 a = Vector2(base.x, base.z) * size;
			if (a.length() < 1e-3f) {
				continue;
			}
			const float turn = Math::fposmod(Math::atan2(a.y, a.x) / float(Math::TAU), 1.0f);
			uw[i].y = Math::floor(MIN(turn * 1024.0f, 1023.0f)) + MIN(a.length() * 0.5f, 0.999f);
		}
	}
	Ref<ImporterMesh> im;
	im.instantiate();
	LocalVector<CardSurface> card_surfaces;
	const Color colors[SURFACE_MAX] = { leaf_color, stem_color, petal_color, centre_color };
	const float roughness[SURFACE_MAX] = { preset == PRESET_WATER_LILY ? 0.35f : (preset == PRESET_IVY ? 0.6f : 0.72f), 0.75f, preset == PRESET_BUTTERCUP ? 0.2f : 0.5f, 0.8f }; // Lily pads: waxy, shed water.
	for (int s = 0; s < SURFACE_MAX; s++) {
		Builder &b = builders[s];
		if (b.indices.is_empty()) {
			continue;
		}
		// Smooth normals from the faces, then the plant's size.
		b.normals.resize(b.verts.size());
		b.normals.fill(Vector3());
		Vector3 *nw = b.normals.ptrw();
		const Vector3 *vr = b.verts.ptr();
		// Godot's front faces wind clockwise: flip the builders' counter-clockwise triangles, so
		// the upper side of a leaf is its front face (a double-sided material flips the normal on
		// the back face).
		int *iw = b.indices.ptrw();
		for (int i = 0; i < b.indices.size(); i += 3) {
			SWAP(iw[i + 1], iw[i + 2]);
		}
		for (int i = 0; i < b.indices.size(); i += 3) {
			const int i0 = b.indices[i];
			const int i1 = b.indices[i + 2];
			const int i2 = b.indices[i + 1];
			const Vector3 fn = (vr[i1] - vr[i0]).cross(vr[i2] - vr[i0]);
			nw[i0] += fn;
			nw[i1] += fn;
			nw[i2] += fn;
		}
		Vector3 *vw = b.verts.ptrw();
		for (int i = 0; i < b.verts.size(); i++) {
			nw[i] = nw[i].length_squared() > 0.0f ? nw[i].normalized() : Vector3(0, 1, 0);
			vw[i] *= size;
		}
		Array arrays;
		arrays.resize(Mesh::ARRAY_MAX);
		arrays[Mesh::ARRAY_VERTEX] = b.verts;
		arrays[Mesh::ARRAY_NORMAL] = b.normals;
		arrays[Mesh::ARRAY_TEX_UV] = b.uvs;
		arrays[Mesh::ARRAY_TEX_UV2] = b.uv2s;
		arrays[Mesh::ARRAY_COLOR] = b.colors;
		arrays[Mesh::ARRAY_INDEX] = b.indices;
		static const char *names[SURFACE_MAX] = { "leaf", "stem", "petal", "centre" };
		// Leaves as separate cards: their own LOD chain by card rank (_card_lods()),
		// since edge-collapse simplification stalls on many small islands.
		// Not petals: a flower's few petals are its colour accent far off, and dropping some of
		// them dims the drift even with the area kept (seen in the heath shots).
		if (lods && s == SURFACE_LEAF) {
			CardSurface cs;
			if (_card_lods(b, cs)) {
				arrays[Mesh::ARRAY_CUSTOM0] = cs.custom;
				cs.surface = im->get_surface_count();
				cs.arrays = arrays;
				card_surfaces.push_back(cs);
			}
		}
		Ref<ShaderMaterial> mat;
		if (textures && (preset == PRESET_MOSS || preset == PRESET_FEATHER_MOSS) && s == SURFACE_CENTRE) {
			const uint64_t key = (uint64_t(preset) << 40) ^ (uint64_t(s) << 36) ^ (uint64_t(uint32_t(seed % 3)) << 32) ^ uint64_t(Math::abs(int(leaf_color.to_rgba32()) % 1000003)) ^ (uint64_t(texture_size) << 44);
			TextureSet *ts = texture_cache.getptr(key);
			if (!ts) {
				texture_cache.insert(key, _bake_moss_texture(uint32_t(seed % 3)));
				ts = texture_cache.getptr(key);
			}
			mat = _textured_material(*ts, 0.9f);
		} else if (textures && (s == SURFACE_LEAF || s == SURFACE_PETAL)) {
			// One texture set per preset, colour and variant (three per preset): shared by the
			// plants of a meadow, so a thousand dandelions cost three textures.
			const uint64_t key = (uint64_t(preset) << 40) ^ (uint64_t(s) << 36) ^ (uint64_t(uint32_t(seed % 3)) << 32) ^ uint64_t(Math::abs(int((s == SURFACE_LEAF ? leaf_color : petal_color).to_rgba32()) % 1000003)) ^ (uint64_t(texture_size) << 44);
			TextureSet *ts = texture_cache.getptr(key);
			if (!ts) {
				texture_cache.insert(key, _bake_leaf_texture(_leaf_texture_spec(s == SURFACE_PETAL), uint32_t(seed % 3) + uint32_t(preset) * 101u));
				ts = texture_cache.getptr(key);
			}
			mat = _textured_material(*ts, roughness[s]);
		} else {
			mat = _material(colors[s], roughness[s], s != SURFACE_CENTRE);
		}
		mat->set_shader_parameter("height", MAX(plant_height * size, 0.05f));
		// Sun and shade leaves: leaves and stems change colour with their light, petals and litter do not.
		mat->set_shader_parameter("light_response", s >= SURFACE_PETAL || preset == PRESET_LEAF_LITTER || preset == PRESET_NEEDLE_LITTER || preset == PRESET_TWIGS ? 0.0f : 1.0f);
		// Stiffer the bigger and woodier: bramble canes and bracken stipes barely lean in the wind.
		mat->set_shader_parameter("stiffness", preset == PRESET_BRAMBLE ? 4.0f : (preset == PRESET_BRACKEN || preset == PRESET_CATTAIL ? 2.5f : (preset == PRESET_MOSS || preset == PRESET_FEATHER_MOSS || preset == PRESET_LEAF_LITTER || preset == PRESET_NEEDLE_LITTER || preset == PRESET_TWIGS || preset == PRESET_WATER_LILY ? 50.0f : 1.5f)));
		im->add_surface(Mesh::PRIMITIVE_TRIANGLES, arrays, TypedArray<Array>(), Dictionary(), mat, names[s]);
	}
	// Distance LODs (meshoptimizer simplification, as the scene importer does): a meadow holds
	// thousands of these, and from a few metres a plant needs a fraction of its triangles.
	if (lods && im->get_surface_count() > 0) {
		im->generate_lods(60.0f, Array());
	}
	if (!card_surfaces.is_empty()) {
		// Rebuild with the card surfaces' own chains in place of meshoptimizer's (which drop
		// cards in no fixed order, so the shader could not have shrunk them first).
		Ref<ImporterMesh> out;
		out.instantiate();
		for (int i = 0; i < im->get_surface_count(); i++) {
			const CardSurface *cs = nullptr;
			for (const CardSurface &c : card_surfaces) {
				if (c.surface == i) {
					cs = &c;
				}
			}
			if (cs) {
				const uint64_t flags = uint64_t(Mesh::ARRAY_CUSTOM_RGBA_FLOAT) << Mesh::ARRAY_FORMAT_CUSTOM0_SHIFT;
				out->add_surface(Mesh::PRIMITIVE_TRIANGLES, cs->arrays, TypedArray<Array>(), cs->lods, im->get_surface_material(i), im->get_surface_name(i), flags);
			} else {
				Dictionary lod_map;
				for (int l = 0; l < im->get_surface_lod_count(i); l++) {
					lod_map[im->get_surface_lod_size(i, l)] = im->get_surface_lod_indices(i, l);
				}
				Array arr = im->get_surface_arrays(i);
				arr[Mesh::ARRAY_CUSTOM0] = Variant(); // A card surface that kept meshoptimizer's chain.
				out->add_surface(Mesh::PRIMITIVE_TRIANGLES, arr, TypedArray<Array>(), lod_map, im->get_surface_material(i), im->get_surface_name(i));
			}
		}
		im = out;
	}
	Ref<ArrayMesh> mesh = im->get_mesh();
	for (Builder &b : builders) {
		b = Builder();
	}
	return mesh;
}

TypedArray<ArrayMesh> PlantGenerator::generate_variants(int p_count) {
	TypedArray<ArrayMesh> res;
	const int base = seed;
	for (int i = 0; i < p_count; i++) {
		seed = base + i;
		res.push_back(generate());
	}
	seed = base;
	return res;
}

// Forest floor sets per stand type (docs/ideas/forest-floor.md; Ellenberg and Leuschner 2010):
// every species placed by light under the crowns (RULE_SHADE from the placed canopy) and, where it
// matters, by the stand overhead (RULE_CONIFER), so a mixed stand sorts its own floor. Gap and
// edge plants take the half-light; the deep-shade herbs, mosses and litter the closed floor.
// Order: bigger plants first, so they claim their space before the herbs.
namespace {
struct FloorSpec {
	PlantGenerator::Preset preset;
	float spacing, footprint, density, scale_min, scale_max;
	float patch_scale, patch_cover; // 0 scale: no patches.
	float shade_min, shade_max, shade_fade;
	float conifer_min, conifer_max; // 0..1: any stand.
	float visibility;
	float water_max = 0.0f; // > 0: within this distance of water (riparian).
};
} // namespace

// The leaves a stand drops (docs/ideas/forest-floor.md, step 6): its broadleaf species by their
// share of the litter, with the form and fallen colour TreeGenerator gives each one when that
// class is present (get_leaf_form(), get_leaf_season() for the season), else the same values kept
// here. Indices are TreeGenerator's presets.
namespace {
struct LitterTree {
	int preset; // TreeGenerator::Preset.
	int kind, count;
	float depth, width, size;
	Color fallen;
};
// kind: 0 simple, 1 lobed, 2 palmate, 3 pinnate, 4 digitate (TreeGenerator::LeafKind).
const LitterTree LITTER_OAK = { 0, 1, 4, 0.55f, 0.3f, 0.1f, Color(0.3f, 0.18f, 0.05f) };
const LitterTree LITTER_BEECH = { 1, 0, 0, 0.0f, 0.32f, 0.08f, Color(0.38f, 0.17f, 0.04f) };
const LitterTree LITTER_BIRCH = { 2, 0, 0, 0.0f, 0.35f, 0.05f, Color(0.45f, 0.33f, 0.04f) };
const LitterTree LITTER_HAZEL = { 5, 0, 0, 0.0f, 0.42f, 0.09f, Color(0.4f, 0.31f, 0.05f) };
const LitterTree LITTER_HAWTHORN = { 6, 1, 2, 0.7f, 0.4f, 0.045f, Color(0.32f, 0.11f, 0.04f) };
const LitterTree LITTER_LIME = { 7, 0, 0, 0.0f, 0.45f, 0.08f, Color(0.42f, 0.32f, 0.05f) };
const LitterTree LITTER_SYCAMORE = { 8, 2, 5, 0.45f, 0.5f, 0.13f, Color(0.38f, 0.28f, 0.05f) };
const LitterTree LITTER_ASH = { 9, 3, 9, 0.0f, 0.18f, 0.25f, Color(0.32f, 0.32f, 0.09f) };
const LitterTree LITTER_ALDER = { 11, 0, 0, 0.0f, 0.45f, 0.08f, Color(0.17f, 0.21f, 0.06f) };
const LitterTree LITTER_WILLOW = { 12, 0, 0, 0.0f, 0.12f, 0.09f, Color(0.38f, 0.33f, 0.07f) };
const LitterTree LITTER_ELDER = { 16, 3, 5, 0.0f, 0.2f, 0.2f, Color(0.3f, 0.22f, 0.08f) };
const LitterTree LITTER_SUGAR_MAPLE = { 20, 2, 5, 0.6f, 0.5f, 0.12f, Color(0.55f, 0.2f, 0.02f) };
const LitterTree LITTER_RED_MAPLE = { 21, 2, 3, 0.35f, 0.5f, 0.09f, Color(0.5f, 0.05f, 0.03f) };
const LitterTree LITTER_RED_OAK = { 22, 1, 4, 0.65f, 0.35f, 0.16f, Color(0.35f, 0.07f, 0.03f) };
const LitterTree LITTER_WHITE_OAK = { 23, 1, 4, 0.6f, 0.32f, 0.14f, Color(0.28f, 0.1f, 0.06f) };
const LitterTree LITTER_AMERICAN_BEECH = { 26, 0, 0, 0.0f, 0.3f, 0.1f, Color(0.42f, 0.28f, 0.06f) };
const LitterTree LITTER_YELLOW_BIRCH = { 28, 0, 0, 0.0f, 0.3f, 0.09f, Color(0.48f, 0.36f, 0.04f) };
} // namespace

Array PlantGenerator::stand_litter_leaves(FloorStand p_stand, const Dictionary &p_season) {
	struct Share {
		LitterTree tree;
		float share;
	};
	LocalVector<Share> mix;
	switch (p_stand) {
		case FLOOR_BEECH:
			mix = LocalVector<Share>({ { LITTER_BEECH, 0.8f }, { LITTER_SYCAMORE, 0.1f }, { LITTER_ASH, 0.1f } });
			break;
		case FLOOR_OAK_MIXED:
			mix = LocalVector<Share>({ { LITTER_OAK, 0.55f }, { LITTER_LIME, 0.2f }, { LITTER_HAZEL, 0.15f }, { LITTER_ASH, 0.1f } });
			break;
		case FLOOR_MONTANE_CONIFER:
			mix = LocalVector<Share>({ { LITTER_BEECH, 0.8f }, { LITTER_SYCAMORE, 0.2f } });
			break;
		case FLOOR_RIPARIAN:
			mix = LocalVector<Share>({ { LITTER_ALDER, 0.6f }, { LITTER_WILLOW, 0.3f }, { LITTER_ELDER, 0.1f } });
			break;
		case FLOOR_HEDGEROW:
			mix = LocalVector<Share>({ { LITTER_HAWTHORN, 0.4f }, { LITTER_HAZEL, 0.3f }, { LITTER_OAK, 0.15f }, { LITTER_ELDER, 0.15f } });
			break;
		case FLOOR_NORTHERN_HARDWOOD:
			mix = LocalVector<Share>({ { LITTER_SUGAR_MAPLE, 0.5f }, { LITTER_AMERICAN_BEECH, 0.3f }, { LITTER_YELLOW_BIRCH, 0.2f } });
			break;
		case FLOOR_EASTERN_OAK:
			mix = LocalVector<Share>({ { LITTER_RED_OAK, 0.45f }, { LITTER_WHITE_OAK, 0.35f }, { LITTER_RED_MAPLE, 0.2f } });
			break;
		case FLOOR_PINE_HEATH:
		case FLOOR_PINE_HEMLOCK:
			mix = LocalVector<Share>({ { LITTER_BIRCH, 1.0f } });
			break;
		default:
			break;
	}
	const bool trees = ClassDB::class_exists("TreeGenerator") && ClassDB::has_method("TreeGenerator", "get_leaf_form");
	const bool seasons = trees && !p_season.is_empty() && ClassDB::has_method("TreeGenerator", "get_leaf_season");
	Array out;
	for (const Share &m : mix) {
		Dictionary d;
		d["kind"] = m.tree.kind;
		d["count"] = m.tree.count;
		d["depth"] = m.tree.depth;
		d["width"] = m.tree.width;
		d["size"] = m.tree.size;
		d["color"] = m.tree.fallen;
		d["fresh"] = 0.3f;
		if (trees) {
			MethodBind *form = ClassDB::get_method("TreeGenerator", "get_leaf_form");
			const Variant pv = m.tree.preset;
			const Variant *args[1] = { &pv };
			Callable::CallError err;
			const Variant r = form ? form->call(nullptr, args, 1, err) : Variant();
			if (err.error == Callable::CallError::CALL_OK && r.get_type() == Variant::DICTIONARY) {
				const Dictionary f = r;
				static const char *keys[5] = { "kind", "count", "depth", "width", "size" };
				for (const char *key : keys) {
					if (f.has(key)) {
						d[key] = f[key];
					}
				}
			}
		}
		if (seasons) {
			MethodBind *season = ClassDB::get_method("TreeGenerator", "get_leaf_season");
			const Variant pv = m.tree.preset;
			const Variant sv = p_season;
			const Variant *args[2] = { &pv, &sv };
			Callable::CallError err;
			const Variant r = season ? season->call(nullptr, args, 2, err) : Variant();
			if (err.error == Callable::CallError::CALL_OK && r.get_type() == Variant::DICTIONARY) {
				const Dictionary ls = r;
				if (ls.has("autumn_color")) {
					d["color"] = ls["autumn_color"];
				}
				// This year's fallen leaves are the fresh ones among the older litter.
				d["fresh"] = CLAMP(float(ls.get("fallen_fraction", 0.0f)) * 0.7f, 0.05f, 0.7f);
			}
		}
		d["share"] = m.share;
		out.push_back(d);
	}
	return out;
}

TypedArray<Resource> PlantGenerator::make_floor(FloorStand p_stand, int p_variants, bool p_textures, float p_impostor_range, const Dictionary &p_season) {
	using P = PlantGenerator;
	// Shared by every stand.
	const FloorSpec gap_bramble = { P::PRESET_BRAMBLE, 1.4f, 0.6f, 0.8f, 0.8f, 1.2f, 7.0f, 0.4f, 0.05f, 0.7f, 0.15f, 0.0f, 1.0f, 45.0f };
	const FloorSpec gap_foxglove = { P::PRESET_FOXGLOVE, 0.8f, 0.2f, 0.5f, 0.8f, 1.1f, 6.0f, 0.3f, 0.1f, 0.7f, 0.1f, 0.0f, 0.6f, 40.0f };
	const FloorSpec gap_bracken = { P::PRESET_BRACKEN, 0.7f, 0.3f, 0.9f, 0.9f, 1.3f, 9.0f, 0.55f, 0.1f, 0.8f, 0.15f, 0.0f, 1.0f, 45.0f };
	const FloorSpec fern = { P::PRESET_FERN, 0.9f, 0.3f, 0.7f, 1.3f, 2.0f, 6.0f, 0.5f, 0.35f, 1.0f, 0.15f, 0.0f, 1.0f, 35.0f };
	const FloorSpec seedling = { P::PRESET_SEEDLING, 0.8f, 0.15f, 0.5f, 0.8f, 1.2f, 5.0f, 0.3f, 0.4f, 0.97f, 0.1f, 0.0f, 1.0f, 30.0f };
	const FloorSpec sorrel = { P::PRESET_WOOD_SORREL, 0.25f, 0.1f, 0.9f, 0.9f, 1.2f, 3.0f, 0.35f, 0.55f, 1.0f, 0.15f, 0.0f, 1.0f, 18.0f };
	const FloorSpec moss = { P::PRESET_MOSS, 0.3f, 0.0f, 0.6f, 0.8f, 1.6f, 4.0f, 0.3f, 0.6f, 1.0f, 0.15f, 0.0f, 1.0f, 15.0f };
	const FloorSpec twigs = { P::PRESET_TWIGS, 0.55f, 0.0f, 0.7f, 0.8f, 1.2f, 0.0f, 0.0f, 0.3f, 1.0f, 0.2f, 0.0f, 1.0f, 18.0f };
	const FloorSpec leaves = { P::PRESET_LEAF_LITTER, 0.2f, 0.0f, 0.6f, 1.2f, 1.8f, 0.0f, 0.0f, 0.3f, 1.0f, 0.2f, 0.0f, 0.5f, 15.0f };
	const FloorSpec needles = { P::PRESET_NEEDLE_LITTER, 0.35f, 0.0f, 0.8f, 0.8f, 1.3f, 0.0f, 0.0f, 0.3f, 1.0f, 0.2f, 0.5f, 1.0f, 15.0f };
	const FloorSpec bilberry = { P::PRESET_BILBERRY, 0.35f, 0.12f, 0.9f, 1.0f, 1.4f, 7.0f, 0.6f, 0.3f, 1.0f, 0.15f, 0.4f, 1.0f, 25.0f };
	LocalVector<FloorSpec> set;
	switch (p_stand) {
		case FLOOR_BEECH:
			// Deep shade, deep leaf litter, a sparse herb layer: woodruff, wood sorrel, anemone.
			set = LocalVector<FloorSpec>({ gap_bramble, gap_foxglove, fern, seedling,
				{ P::PRESET_WOODRUFF, 0.3f, 0.1f, 0.9f, 0.9f, 1.2f, 4.0f, 0.3f, 0.5f, 1.0f, 0.15f, 0.0f, 0.5f, 20.0f },
				{ P::PRESET_WOOD_ANEMONE, 0.25f, 0.08f, 0.9f, 0.9f, 1.2f, 5.0f, 0.3f, 0.4f, 1.0f, 0.15f, 0.0f, 0.6f, 20.0f },
				sorrel, moss, twigs, leaves, needles });
			set[2].density = 0.35f; // Few ferns under beech.
			set[3].density = 0.8f; // Beech seedlings carpet the floor.
			break;
		case FLOOR_OAK_MIXED:
			// More light: a rich herb layer, bramble and ivy carpets, anemone, male fern.
			set = LocalVector<FloorSpec>({ gap_bramble, gap_bracken, gap_foxglove, fern, seedling,
				{ P::PRESET_IVY, 0.6f, 0.0f, 0.9f, 0.9f, 1.3f, 6.0f, 0.45f, 0.3f, 1.0f, 0.15f, 0.0f, 0.6f, 25.0f },
				{ P::PRESET_WOOD_ANEMONE, 0.25f, 0.08f, 0.9f, 0.9f, 1.2f, 6.0f, 0.4f, 0.3f, 1.0f, 0.15f, 0.0f, 0.6f, 20.0f },
				{ P::PRESET_WOODRUFF, 0.3f, 0.1f, 0.7f, 0.9f, 1.2f, 4.0f, 0.2f, 0.5f, 1.0f, 0.15f, 0.0f, 0.5f, 20.0f },
				moss, twigs, leaves, needles });
			set[0].shade_max = 0.9f; // Bramble runs under oak, not only in gaps.
			set[0].patch_cover = 0.5f;
			break;
		case FLOOR_MONTANE_CONIFER:
			// Spruce, fir and beech: wood sorrel, ferns and bilberry on mixed litter.
			set = LocalVector<FloorSpec>({ gap_bramble, gap_foxglove, fern, seedling, bilberry, sorrel, moss, twigs, leaves, needles });
			set[2].density = 0.9f;
			set[2].patch_cover = 0.6f;
			break;
		case FLOOR_SUBALPINE:
			// Larch and spruce near the tree line: bilberry and heather, few herbs.
			set = LocalVector<FloorSpec>({ fern, seedling, bilberry,
				{ P::PRESET_HEATHER, 0.45f, 0.15f, 0.9f, 0.9f, 1.3f, 8.0f, 0.5f, 0.0f, 0.6f, 0.15f, 0.0f, 1.0f, 30.0f },
				sorrel, moss, twigs, needles });
			set[0].density = 0.3f;
			set[2].density = 1.0f;
			set[2].patch_cover = 0.8f;
			set[2].conifer_min = 0.0f; // Bilberry under larch (needle litter, open crowns) too.
			break;
		case FLOOR_PINE_HEATH:
			// Open pine crowns: bilberry, heather and wavy hair-grass, bracken in the gaps.
			set = LocalVector<FloorSpec>({ gap_bracken, seedling, bilberry,
				{ P::PRESET_HEATHER, 0.45f, 0.15f, 0.9f, 0.9f, 1.3f, 8.0f, 0.55f, 0.0f, 0.7f, 0.15f, 0.0f, 1.0f, 30.0f },
				{ P::PRESET_DRY_GRASS, 0.3f, 0.0f, 0.7f, 0.7f, 1.1f, 6.0f, 0.45f, 0.0f, 0.8f, 0.15f, 0.0f, 1.0f, 15.0f },
				moss, twigs, needles });
			set[1].density = 0.25f;
			set[2].density = 1.0f;
			set[2].patch_cover = 0.7f;
			set[2].conifer_min = 0.0f;
			break;
		case FLOOR_RIPARIAN:
			// Alder and willow on wet ground: nettle, sedge, rush, ferns.
			set = LocalVector<FloorSpec>({ { P::PRESET_NETTLE, 0.5f, 0.15f, 0.9f, 0.9f, 1.3f, 6.0f, 0.55f, 0.0f, 0.9f, 0.15f, 0.0f, 1.0f, 35.0f },
				fern,
				{ P::PRESET_SEDGE, 0.35f, 0.1f, 0.9f, 0.9f, 1.4f, 5.0f, 0.5f, 0.0f, 1.0f, 0.15f, 0.0f, 1.0f, 25.0f, 15.0f },
				{ P::PRESET_RUSH, 0.6f, 0.2f, 0.7f, 0.9f, 1.2f, 5.0f, 0.4f, 0.0f, 0.8f, 0.15f, 0.0f, 1.0f, 30.0f, 8.0f },
				moss, twigs, leaves });
			break;
		case FLOOR_HEDGEROW:
			// Hedge bottoms: bramble, nettle, ivy, foxglove, fern.
			set = LocalVector<FloorSpec>({ gap_bramble,
				{ P::PRESET_NETTLE, 0.5f, 0.15f, 0.8f, 0.9f, 1.3f, 5.0f, 0.45f, 0.0f, 0.9f, 0.15f, 0.0f, 1.0f, 35.0f },
				gap_foxglove, fern,
				{ P::PRESET_IVY, 0.6f, 0.0f, 0.9f, 0.9f, 1.3f, 5.0f, 0.5f, 0.2f, 1.0f, 0.15f, 0.0f, 1.0f, 25.0f },
				twigs, leaves });
			set[0].shade_max = 0.95f;
			set[0].density = 1.0f;
			set[0].patch_cover = 0.7f;
			break;
		case FLOOR_NORTHERN_HARDWOOD:
			// Sugar maple, beech, birch (North America): deep leaf litter, a spring carpet of
			// ephemerals (trout lily, spring beauty: anemone-like, here wood anemone), ferns,
			// wood sorrel, maple seedlings by the thousand.
			set = LocalVector<FloorSpec>({ gap_bramble, fern, seedling,
				{ P::PRESET_WOOD_ANEMONE, 0.25f, 0.08f, 0.9f, 0.9f, 1.2f, 6.0f, 0.45f, 0.3f, 1.0f, 0.15f, 0.0f, 0.6f, 20.0f },
				sorrel, moss, twigs, leaves, needles });
			set[1].density = 0.8f;
			set[1].patch_cover = 0.55f;
			set[2].density = 0.9f;
			break;
		case FLOOR_EASTERN_OAK:
			// Oak and hickory on drier ground: oak litter with lowbush blueberry and huckleberry
			// (the bilberry preset's form), bracken in gaps, few herbs.
			set = LocalVector<FloorSpec>({ gap_bracken, fern, seedling, bilberry, moss, twigs, leaves, needles });
			set[1].density = 0.35f;
			set[3].conifer_min = 0.0f; // Ericaceous shrubs under oak here, not only conifers.
			set[3].density = 1.0f;
			set[3].patch_cover = 0.7f;
			break;
		case FLOOR_PINE_HEMLOCK:
			// White pine and hemlock: deep needle duff in the hemlock's dark, nearly bare; a little
			// moss, wood sorrel and seedlings where light reaches.
			set = LocalVector<FloorSpec>({ fern, seedling, sorrel, moss, twigs, needles });
			set[0].density = 0.2f;
			set[1].density = 0.25f;
			set[2].density = 0.4f;
			set[3].density = 0.4f;
			break;
		case FLOOR_MAX:
			break;
	}
	// How much leaf litter lies on the ground (docs/ideas/forest-floor.md, step 6): a stand drops
	// about its leaf area each autumn (F layers) and it decays at k per year (Hobbie et al. 2006),
	// so t years after the fall the layers are F exp(-k t) / (1 - exp(-k)) and they hide
	// 1 - exp(-layers) of the ground. Leaf fall taken as 1 November; without a season, midsummer.
	struct Decay {
		float layers, k;
	};
	Decay decay = { 5.0f, 0.3f };
	Color twig_bark(0.3f, 0.26f, 0.21f);
	switch (p_stand) {
		case FLOOR_BEECH:
			decay = { 5.0f, 0.3f };
			twig_bark = Color(0.36f, 0.34f, 0.31f); // Smooth grey.
			break;
		case FLOOR_OAK_MIXED:
			decay = { 5.0f, 0.45f }; // Oak slow, lime, hazel and ash fast.
			twig_bark = Color(0.27f, 0.22f, 0.17f);
			break;
		case FLOOR_MONTANE_CONIFER:
			decay = { 2.5f, 0.3f };
			twig_bark = Color(0.3f, 0.24f, 0.18f);
			break;
		case FLOOR_RIPARIAN:
			decay = { 4.0f, 1.5f }; // Alder litter is gone within the year.
			twig_bark = Color(0.24f, 0.2f, 0.17f);
			break;
		case FLOOR_HEDGEROW:
			decay = { 3.0f, 0.8f };
			twig_bark = Color(0.26f, 0.21f, 0.16f);
			break;
		case FLOOR_NORTHERN_HARDWOOD:
			decay = { 5.0f, 0.4f };
			twig_bark = Color(0.33f, 0.3f, 0.26f);
			break;
		case FLOOR_EASTERN_OAK:
			decay = { 5.0f, 0.35f };
			twig_bark = Color(0.26f, 0.21f, 0.17f);
			break;
		case FLOOR_PINE_HEATH:
		case FLOOR_SUBALPINE:
		case FLOOR_PINE_HEMLOCK:
			twig_bark = Color(0.33f, 0.24f, 0.17f); // Pine and spruce twigs, reddish.
			break;
		default:
			break;
	}
	const float day = p_season.has("day_of_year") ? float(p_season["day_of_year"]) : 196.0f;
	const float since_fall = Math::fposmod(day - 305.0f, 365.0f) / 365.0f;
	const float layers = decay.layers * Math::exp(-decay.k * since_fall) / (1.0f - Math::exp(-decay.k));
	const float leaf_cover = 1.0f - Math::exp(-layers);

	TypedArray<Resource> out;
	int seed_base = 1000 + int(p_stand) * 100;
	for (FloorSpec f : set) {
		if (f.preset == P::PRESET_LEAF_LITTER) {
			f.density = CLAMP(0.9f * leaf_cover, 0.1f, 0.9f);
		}
		Ref<PlantGenerator> gen;
		gen.instantiate();
		gen->set_textures(p_textures);
		gen->set_preset(f.preset);
		gen->apply_preset_colors();
		gen->set_seed(seed_base);
		if (f.preset == P::PRESET_LEAF_LITTER) {
			gen->set_litter_leaves(stand_litter_leaves(p_stand, p_season));
		}
		if (f.preset == P::PRESET_TWIGS) {
			gen->stem_color = twig_bark;
		}
		Ref<PlantSpecies> sp;
		sp.instantiate();
		TypedArray<Mesh> meshes;
		const TypedArray<ArrayMesh> v = gen->generate_variants(MAX(p_variants, 1));
		for (int i = 0; i < v.size(); i++) {
			meshes.push_back(v[i]);
		}
		sp->set_meshes(meshes);
		sp->set_spacing(f.spacing);
		sp->set_footprint(f.footprint);
		sp->set_density(f.density);
		sp->set_scale_range(Vector2(f.scale_min, f.scale_max));
		if (f.patch_scale > 0.0f) {
			sp->set_patch_scale(f.patch_scale);
			sp->set_patch_cover(f.patch_cover);
			sp->set_patch_seed(seed_base + 7);
		}
		sp->set_rule_range(PlantSpecies::RULE_SHADE, Vector2(f.shade_min, f.shade_max));
		sp->set_rule_fade(PlantSpecies::RULE_SHADE, f.shade_fade);
		if (f.conifer_min > 0.0f || f.conifer_max < 1.0f) {
			sp->set_rule_range(PlantSpecies::RULE_CONIFER, Vector2(f.conifer_min, f.conifer_max));
			sp->set_rule_fade(PlantSpecies::RULE_CONIFER, 0.15f);
		}
		if (f.water_max > 0.0f) {
			sp->set_rule_range(PlantSpecies::RULE_WATER_DISTANCE, Vector2(0.0f, f.water_max));
			sp->set_rule_fade(PlantSpecies::RULE_WATER_DISTANCE, f.water_max * 0.3f);
		}
		sp->set_visibility_range(f.visibility);
		if (f.preset == P::PRESET_LEAF_LITTER || f.preset == P::PRESET_TWIGS) {
			// Litter lies where the crowns drop it, drifting past their edges into the light
			// (PlantSpecies.RULE_LITTER), not only in their shade. Twigs fall nearer the trunk.
			sp->set_rule_range(PlantSpecies::RULE_SHADE, Vector2(0.0f, 1.0f));
			sp->set_rule_range(PlantSpecies::RULE_LITTER, Vector2(f.preset == P::PRESET_TWIGS ? 0.35f : 0.12f, 1.0f));
			sp->set_rule_fade(PlantSpecies::RULE_LITTER, 0.3f);
		}
		if (f.preset == P::PRESET_MOSS || f.preset == P::PRESET_FEATHER_MOSS) {
			sp->set_rule_range(PlantSpecies::RULE_SPRAY, Vector2(0.0f, 1.0f)); // Mosses take a fall's spray zone.
		}
		// Far away the ground takes the plant's colour (VegetationField3D's vegetation_far_map):
		// the leaves, a little darker for the shade inside the plant, hiding more ground the
		// taller the plant. Litter and twigs are the ground's own (forest_floor()).
		if (f.preset != P::PRESET_LEAF_LITTER && f.preset != P::PRESET_NEEDLE_LITTER && f.preset != P::PRESET_TWIGS && v.size() > 0) {
			const Ref<ArrayMesh> m0 = v[0];
			const float h = m0.is_valid() ? m0->get_aabb().get_end().y : 0.2f;
			const Color lc = gen->get_leaf_color() * 0.8f;
			sp->set_far_color(Color(lc.r, lc.g, lc.b, CLAMP(h / 0.6f, 0.2f, 0.9f)));
		}
		// The larger shrubs, dense with thousands of triangles each, turn into impostors past the
		// range (raster only; docs/ideas/vegetation-lod-streaming.md).
		if (p_impostor_range > 0.0f && (f.preset == P::PRESET_BRAMBLE || f.preset == P::PRESET_BRACKEN)) {
			add_impostors(sp, p_impostor_range);
		}
		sp->set_meta("floor", true);
		out.push_back(sp);
		seed_base += 11;
	}
	return out;
}

void PlantGenerator::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_preset", "preset"), &PlantGenerator::set_preset);
	ClassDB::bind_method(D_METHOD("get_preset"), &PlantGenerator::get_preset);
	ClassDB::bind_method(D_METHOD("set_seed", "seed"), &PlantGenerator::set_seed);
	ClassDB::bind_method(D_METHOD("get_seed"), &PlantGenerator::get_seed);
	ClassDB::bind_method(D_METHOD("set_size", "size"), &PlantGenerator::set_size);
	ClassDB::bind_method(D_METHOD("get_size"), &PlantGenerator::get_size);
	ClassDB::bind_method(D_METHOD("set_leaf_color", "color"), &PlantGenerator::set_leaf_color);
	ClassDB::bind_method(D_METHOD("get_leaf_color"), &PlantGenerator::get_leaf_color);
	ClassDB::bind_method(D_METHOD("set_petal_color", "color"), &PlantGenerator::set_petal_color);
	ClassDB::bind_method(D_METHOD("get_petal_color"), &PlantGenerator::get_petal_color);
	ClassDB::bind_method(D_METHOD("set_centre_color", "color"), &PlantGenerator::set_centre_color);
	ClassDB::bind_method(D_METHOD("get_centre_color"), &PlantGenerator::get_centre_color);
	ClassDB::bind_method(D_METHOD("set_stem_color", "color"), &PlantGenerator::set_stem_color);
	ClassDB::bind_method(D_METHOD("get_stem_color"), &PlantGenerator::get_stem_color);
	ClassDB::bind_method(D_METHOD("set_lods", "enabled"), &PlantGenerator::set_lods);
	ClassDB::bind_method(D_METHOD("get_lods"), &PlantGenerator::get_lods);
	ClassDB::bind_method(D_METHOD("set_leaf_wear", "wear"), &PlantGenerator::set_leaf_wear);
	ClassDB::bind_method(D_METHOD("get_leaf_wear"), &PlantGenerator::get_leaf_wear);
	ClassDB::bind_method(D_METHOD("set_textures", "enabled"), &PlantGenerator::set_textures);
	ClassDB::bind_method(D_METHOD("get_textures"), &PlantGenerator::get_textures);
	ClassDB::bind_method(D_METHOD("set_texture_size", "size"), &PlantGenerator::set_texture_size);
	ClassDB::bind_method(D_METHOD("get_texture_size"), &PlantGenerator::get_texture_size);
	ClassDB::bind_method(D_METHOD("generate"), &PlantGenerator::generate);
	ClassDB::bind_method(D_METHOD("generate_variants", "count"), &PlantGenerator::generate_variants);
	ClassDB::bind_method(D_METHOD("apply_preset_colors"), &PlantGenerator::apply_preset_colors);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "preset", PROPERTY_HINT_ENUM, "Dandelion,Plantain,Clover,Fern,Sedge,Daisy,Buttercup,Nettle,Thistle,Yarrow,Dock,Cocksfoot,Timothy,Dry Grass,Leaf Litter,Moss,Bilberry,Bracken,Bramble,Foxglove,Reed,Cattail,Rush,Water Lily,Needle Litter,Twigs,Wood Sorrel,Seedling,Feather Moss,Woodruff,Wood Anemone,Ivy,Heather"), "set_preset", "get_preset");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "seed"), "set_seed", "get_seed");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "size", PROPERTY_HINT_RANGE, "0.1,4,0.01"), "set_size", "get_size");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "lods"), "set_lods", "get_lods");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "leaf_wear", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_leaf_wear", "get_leaf_wear");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "litter_leaves"), "set_litter_leaves", "get_litter_leaves");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "textures"), "set_textures", "get_textures");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "texture_size", PROPERTY_HINT_RANGE, "32,2048,1"), "set_texture_size", "get_texture_size");
	ADD_GROUP("Colors", "");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "leaf_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_leaf_color", "get_leaf_color");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "stem_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_stem_color", "get_stem_color");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "petal_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_petal_color", "get_petal_color");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "centre_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_centre_color", "get_centre_color");

	BIND_ENUM_CONSTANT(PRESET_DANDELION);
	BIND_ENUM_CONSTANT(PRESET_PLANTAIN);
	BIND_ENUM_CONSTANT(PRESET_CLOVER);
	BIND_ENUM_CONSTANT(PRESET_FERN);
	BIND_ENUM_CONSTANT(PRESET_SEDGE);
	BIND_ENUM_CONSTANT(PRESET_DAISY);
	BIND_ENUM_CONSTANT(PRESET_BUTTERCUP);
	BIND_ENUM_CONSTANT(PRESET_NETTLE);
	BIND_ENUM_CONSTANT(PRESET_THISTLE);
	BIND_ENUM_CONSTANT(PRESET_YARROW);
	BIND_ENUM_CONSTANT(PRESET_DOCK);
	BIND_ENUM_CONSTANT(PRESET_COCKSFOOT);
	BIND_ENUM_CONSTANT(PRESET_TIMOTHY);
	BIND_ENUM_CONSTANT(PRESET_DRY_GRASS);
	BIND_ENUM_CONSTANT(PRESET_LEAF_LITTER);
	BIND_ENUM_CONSTANT(PRESET_MOSS);
	BIND_ENUM_CONSTANT(PRESET_BILBERRY);
	BIND_ENUM_CONSTANT(PRESET_BRACKEN);
	BIND_ENUM_CONSTANT(PRESET_BRAMBLE);
	BIND_ENUM_CONSTANT(PRESET_FOXGLOVE);
	BIND_ENUM_CONSTANT(PRESET_REED);
	BIND_ENUM_CONSTANT(PRESET_CATTAIL);
	BIND_ENUM_CONSTANT(PRESET_RUSH);
	BIND_ENUM_CONSTANT(PRESET_WATER_LILY);
	BIND_ENUM_CONSTANT(PRESET_NEEDLE_LITTER);
	BIND_ENUM_CONSTANT(PRESET_TWIGS);
	BIND_ENUM_CONSTANT(PRESET_WOOD_SORREL);
	BIND_ENUM_CONSTANT(PRESET_SEEDLING);
	BIND_ENUM_CONSTANT(PRESET_FEATHER_MOSS);
	BIND_ENUM_CONSTANT(PRESET_WOODRUFF);
	BIND_ENUM_CONSTANT(PRESET_WOOD_ANEMONE);
	BIND_ENUM_CONSTANT(PRESET_IVY);
	BIND_ENUM_CONSTANT(PRESET_HEATHER);
	ClassDB::bind_static_method("PlantGenerator", D_METHOD("make_floor", "stand", "variants", "textures", "impostor_range", "season"), &PlantGenerator::make_floor, DEFVAL(3), DEFVAL(true), DEFVAL(15.0f), DEFVAL(Dictionary()));
	ClassDB::bind_static_method("PlantGenerator", D_METHOD("stand_litter_leaves", "stand", "season"), &PlantGenerator::stand_litter_leaves, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("set_litter_leaves", "leaves"), &PlantGenerator::set_litter_leaves);
	ClassDB::bind_method(D_METHOD("get_litter_leaves"), &PlantGenerator::get_litter_leaves);
	ClassDB::bind_static_method("PlantGenerator", D_METHOD("make_climber_leaves", "climber", "variants"), &PlantGenerator::make_climber_leaves, DEFVAL(4));
	BIND_ENUM_CONSTANT(CLIMBER_IVY);
	BIND_ENUM_CONSTANT(CLIMBER_CLEMATIS);
	BIND_ENUM_CONSTANT(CLIMBER_HONEYSUCKLE);
	ClassDB::bind_static_method("PlantGenerator", D_METHOD("make_impostor", "mesh", "frames", "frame_size"), &PlantGenerator::make_impostor, DEFVAL(8), DEFVAL(64));
	ClassDB::bind_static_method("PlantGenerator", D_METHOD("add_impostors", "species", "range", "frames", "frame_size"), &PlantGenerator::add_impostors, DEFVAL(8), DEFVAL(64));
	BIND_ENUM_CONSTANT(FLOOR_BEECH);
	BIND_ENUM_CONSTANT(FLOOR_OAK_MIXED);
	BIND_ENUM_CONSTANT(FLOOR_MONTANE_CONIFER);
	BIND_ENUM_CONSTANT(FLOOR_SUBALPINE);
	BIND_ENUM_CONSTANT(FLOOR_PINE_HEATH);
	BIND_ENUM_CONSTANT(FLOOR_RIPARIAN);
	BIND_ENUM_CONSTANT(FLOOR_HEDGEROW);
	BIND_ENUM_CONSTANT(FLOOR_NORTHERN_HARDWOOD);
	BIND_ENUM_CONSTANT(FLOOR_EASTERN_OAK);
	BIND_ENUM_CONSTANT(FLOOR_PINE_HEMLOCK);
	BIND_ENUM_CONSTANT(FLOOR_MAX);
	BIND_ENUM_CONSTANT(PRESET_MAX);
}

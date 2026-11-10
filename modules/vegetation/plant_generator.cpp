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
		const float tooth = p_leaf.shape == LEAF_PALMATE ? 0.5f - 0.5f * Math::cos(f * Math::TAU) : (1.0f - f) * (1.0f - f);
		w *= 1.0f - p_leaf.lobe_depth * (1.0f - tooth);
	}
	return w;
}

void PlantGenerator::_add_leaf(Surface p_surface, const Transform3D &p_base, const Leaf &p_leaf, float p_wind_base, float p_ao) {
	Builder &b = builders[p_surface];
	const int n = MAX(p_leaf.segments, 2);
	const int first = b.verts.size();
	Basis f = p_base.basis.orthonormalized();
	Vector3 p = p_base.origin;
	const float step = p_leaf.length / n;
	for (int i = 0; i <= n; i++) {
		const float t = float(i) / n;
		const float hw = 0.5f * p_leaf.width * _leaf_width(p_leaf, t);
		const Vector3 x = f.get_column(0);
		const Vector3 y = f.get_column(1);
		const float wind = MIN(1.0f, p_wind_base + (1.0f - p_wind_base) * t);
		const float ao = p_ao * (0.75f + 0.25f * t);
		const Vector3 row[3] = { p - x * hw + y * (p_leaf.fold * hw), p, p + x * hw + y * (p_leaf.fold * hw) };
		for (int c = 0; c < 3; c++) {
			b.verts.push_back(row[c]);
			// Planar across the blade (u = 0.5 at the midrib, 0 and 1 at the widest point), so the
			// texture's veins keep their true shape whatever the outline does.
			b.uvs.push_back(Vector2(0.5f + (c - 1) * 0.5f * _leaf_width(p_leaf, t), t));
			b.uv2s.push_back(Vector2(wind, 0.0f));
			b.colors.push_back(Color(ao, ao, ao));
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
}

Transform3D PlantGenerator::_add_stem(const Transform3D &p_base, float p_length, float p_r0, float p_r1, float p_bend, int p_segments, float p_wind_base, Surface p_surface) {
	Builder &b = builders[p_surface];
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

Ref<StandardMaterial3D> PlantGenerator::_textured_material(const TextureSet &p_set, float p_roughness) const {
	Ref<StandardMaterial3D> m;
	m.instantiate();
	m->set_albedo(Color(1, 1, 1));
	m->set_texture(BaseMaterial3D::TEXTURE_ALBEDO, p_set.albedo);
	m->set_roughness(p_roughness);
	m->set_specular(0.35f);
	m->set_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
	m->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
	m->set_feature(BaseMaterial3D::FEATURE_NORMAL_MAPPING, true);
	m->set_texture(BaseMaterial3D::TEXTURE_NORMAL, p_set.normal);
	m->set_normal_scale(1.0f);
	m->set_feature(BaseMaterial3D::FEATURE_BACKLIGHT, true);
	m->set_backlight(Color(0, 0, 0));
	m->set_texture(BaseMaterial3D::TEXTURE_BACKLIGHT, p_set.backlight);
	m->set_texture_filter(BaseMaterial3D::TEXTURE_FILTER_LINEAR_WITH_MIPMAPS_ANISOTROPIC);
	return m;
}

Ref<StandardMaterial3D> PlantGenerator::_material(const Color &p_color, float p_roughness, bool p_backlight) const {
	Ref<StandardMaterial3D> m;
	m.instantiate();
	m->set_albedo(p_color);
	m->set_roughness(p_roughness);
	m->set_specular(0.35f); // Leaf cuticle: a little less than the default dielectric.
	m->set_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
	m->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
	if (p_backlight) {
		// Thin leaves and petals pass light through (the engine's back lighting, raster and traced).
		m->set_feature(BaseMaterial3D::FEATURE_BACKLIGHT, true);
		m->set_backlight(p_color * 0.6f);
	}
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
		case PRESET_MAX:
			break;
	}
	Ref<ImporterMesh> im;
	im.instantiate();
	const Color colors[SURFACE_MAX] = { leaf_color, stem_color, petal_color, centre_color };
	const float roughness[SURFACE_MAX] = { 0.72f, 0.75f, preset == PRESET_BUTTERCUP ? 0.2f : 0.5f, 0.8f };
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
		Ref<StandardMaterial3D> mat;
		if (textures && (s == SURFACE_LEAF || s == SURFACE_PETAL)) {
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
		im->add_surface(Mesh::PRIMITIVE_TRIANGLES, arrays, TypedArray<Array>(), Dictionary(), mat, names[s]);
	}
	// Distance LODs (meshoptimizer simplification, as the scene importer does): a meadow holds
	// thousands of these, and from a few metres a plant needs a fraction of its triangles.
	if (lods && im->get_surface_count() > 0) {
		im->generate_lods(60.0f, Array());
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
	ClassDB::bind_method(D_METHOD("set_textures", "enabled"), &PlantGenerator::set_textures);
	ClassDB::bind_method(D_METHOD("get_textures"), &PlantGenerator::get_textures);
	ClassDB::bind_method(D_METHOD("set_texture_size", "size"), &PlantGenerator::set_texture_size);
	ClassDB::bind_method(D_METHOD("get_texture_size"), &PlantGenerator::get_texture_size);
	ClassDB::bind_method(D_METHOD("generate"), &PlantGenerator::generate);
	ClassDB::bind_method(D_METHOD("generate_variants", "count"), &PlantGenerator::generate_variants);
	ClassDB::bind_method(D_METHOD("apply_preset_colors"), &PlantGenerator::apply_preset_colors);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "preset", PROPERTY_HINT_ENUM, "Dandelion,Plantain,Clover,Fern,Sedge,Daisy,Buttercup"), "set_preset", "get_preset");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "seed"), "set_seed", "get_seed");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "size", PROPERTY_HINT_RANGE, "0.1,4,0.01"), "set_size", "get_size");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "lods"), "set_lods", "get_lods");
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
	BIND_ENUM_CONSTANT(PRESET_MAX);
}

/**************************************************************************/
/*  plant_mounds.cpp                                                      */
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

// Boar rooting (docs/ideas/vegetation.md, "Boar rooting"): wild boar dig for roots, bulbs and soil animals, turning the
// Anthills and molehills (docs/ideas/vegetation.md, "Anthills and molehills").
// - Yellow meadow ant (Lasius flavus) hills: domes of fine soil 0.3 to 1 m across and up to 0.5 m high, built up over
//   decades in old, unploughed, grazed pasture (a field of them dates the grassland), from tens to several hundred a
//   hectare, often in groups; the crest lies to the east and the long axis east-west, the face that warms first in the
//   morning built steepest (King 1977, J. Ecol.; Ant-hill heterogeneity and grassland management, Oxford). Their turf
//   is finer and greyer than the pasture's: sheep's fescue and wild thyme, with bare crumbs where the ants add soil.
// - Red wood ants (Formica rufa group): thatch domes of conifer needles, twig bits and resin, 0.5 to 2 m across and up to
//   1 m high, on an earth bank, in sunny gaps and edges of conifer and mixed woods; nests built under the forest fail
//   when it is clear felled (Sorvari and Hakkarainen; Formica aquilonia in clear fells).
// - Moles (Talpa europaea): heaps of loose soil pushed up from the runs, 25 to 50 cm across and 10 to 20 cm high, in
//   lines and arcs along a run, in moist, deep, worm-rich soil of pasture and broadleaf woods, scarce on acid sand,
//   in waterlogged ground and under conifers; a fresh heap is dark and crumbly, crusts grey in days, slumps over
//   weeks and is grassed over within months.

#include "plant_generator.h"
#include "plant_species.h"

namespace {
Basis mound_frame(const Vector3 &p_forward, const Vector3 &p_up_hint) {
	const Vector3 z = p_forward.normalized();
	Vector3 x = p_up_hint.cross(z);
	if (x.length_squared() < 1e-8f) {
		x = Vector3(1, 0, 0).cross(z);
		if (x.length_squared() < 1e-8f) {
			x = Vector3(0, 0, 1).cross(z);
		}
	}
	x.normalize();
	return Basis(x, z.cross(x).normalized(), z);
}

Vector3 mound_dir(float p_azimuth, float p_elevation) {
	return Vector3(Math::cos(p_azimuth) * Math::cos(p_elevation), Math::sin(p_elevation), Math::sin(p_azimuth) * Math::cos(p_elevation));
}

// The dome's surface: the height over a point (x, z) of an ellipse a by b with its crest moved east by p_shift (as a
// share of a). Zero outside; a smooth cap inside.
float dome_height(float p_x, float p_z, float p_a, float p_b, float p_h, float p_shift) {
	const float u = p_x / p_a;
	const float v = p_z / p_b;
	const float r2 = u * u + v * v;
	if (r2 >= 1.0f) {
		return 0.0f;
	}
	// Crest east of the centre: the east face steeper, the west one long.
	const float east = u - p_shift;
	const float k = east > 0.0f ? 1.0f / MAX(1.0f - p_shift, 0.1f) : 1.0f / (1.0f + p_shift);
	const float s = MIN(east * k * east * k + v * v, 1.0f);
	const float cap = 1.0f - s;
	return p_h * Math::sqrt(MAX(cap, 0.0f)) * Math::smoothstep(0.0f, 0.25f, 1.0f - r2);
}
} // namespace

// Mounds do not sway: the soil and thatch surfaces (centre) get no wind weight (uv2.x), whatever _add_head gave them.
// Their colours' alpha back to 1: the plant shader reads 1 - alpha as dead tissue (yellowing), and a scaled tint
// (Color * float) scales the alpha too.
static void mound_still(PackedVector2Array &p_uv2s, int p_from, PackedColorArray *p_colors = nullptr) {
	for (int i = p_from; i < p_uv2s.size(); i++) {
		p_uv2s.write[i] = Vector2(0.0f, p_uv2s[i].y);
	}
	if (p_colors) {
		for (int i = p_from; i < p_colors->size(); i++) {
			Color c = (*p_colors)[i];
			c.a = 1.0f;
			p_colors->write[i] = c;
		}
	}
}

float PlantGenerator::_mound_age() const {
	return season.has("mound_age") ? MAX(float(season["mound_age"]), 0.0f) : 0.04f;
}

// A yellow meadow ant hill: a grassed dome, 0.35 m to the rim at scale 1, crest to the east (+x), long axis east-west.
// The dome (centre) is the turf's base, tinted green; fine fescue blades and thyme cushions cover it, and on the crest a
// few bare patches of fresh fine soil.
void PlantGenerator::_build_anthill() {
	const int still_from = builders[SURFACE_CENTRE].uv2s.size();
	const float a = 0.35f * _rand(1.1f, 1.3f); // East-west.
	const float b = 0.35f * _rand(0.85f, 1.0f);
	const float h = _rand(0.16f, 0.3f);
	const float shift = _rand(0.15f, 0.3f);
	Builder &cb = builders[SURFACE_CENTRE];
	// The dome as a grid of rings, lifted by dome_height().
	const int rings = 10;
	const int sectors = 24;
	const int first = cb.verts.size();
	for (int r = 0; r <= rings; r++) {
		const float t = float(r) / rings;
		for (int sct = 0; sct < sectors; sct++) {
			const float ang = sct * Math::TAU / sectors;
			const float x = Math::cos(ang) * a * t * 1.04f;
			const float z = Math::sin(ang) * b * t * 1.04f;
			const float y = dome_height(x, z, a, b, h, shift) - 0.01f;
			cb.verts.push_back(Vector3(x, y, z));
			cb.uvs.push_back(Vector2(float(sct) / sectors, t));
			cb.uv2s.push_back(Vector2(1.0f, 0.0f));
			// Turf base: green-brown, darker in the hollows between the tussocks (noise).
			const float n = 0.85f + 0.3f * Math::sin(x * 37.0f + z * 23.0f) * Math::sin(z * 41.0f - x * 13.0f);
			cb.colors.push_back(Color(1, 1, 1) * n); // The turf's shaded base between the blades (centre_color).
		}
	}
	for (int r = 0; r < rings; r++) {
		for (int sct = 0; sct < sectors; sct++) {
			const int i0 = first + r * sectors + sct;
			const int i1 = first + r * sectors + (sct + 1) % sectors;
			cb.indices.append_array(PackedInt32Array({ i0, i1, i0 + sectors, i1, i1 + sectors, i0 + sectors }));
		}
	}
	_tag_organ(cb);
	plant_height = MAX(plant_height, h);
	auto ground = [&](float p_x, float p_z) { return Vector3(p_x, dome_height(p_x, p_z, a, b, h, shift) - 0.01f, p_z); };
	// The dome's normal by central differences.
	auto normal = [&](float p_x, float p_z) {
		const float e = 0.01f;
		const float dx = dome_height(p_x + e, p_z, a, b, h, shift) - dome_height(p_x - e, p_z, a, b, h, shift);
		const float dz = dome_height(p_x, p_z + e, a, b, h, shift) - dome_height(p_x, p_z - e, a, b, h, shift);
		return Vector3(-dx, 2.0f * e, -dz).normalized();
	};
	// Fine fescue and bent: grey-green blades as dense as the pasture's (a hill seen from a few metres is a grassy hump,
	// not a bare dome), a little longer down the flanks where they meet the sward.
	const int blades = int(_rand(2000.0f, 2400.0f));
	for (int k = 0; k < blades; k++) {
		const float ang = _rand(0.0f, Math::TAU);
		const float rr = Math::sqrt(_rand(0.0f, 1.0f)) * 0.98f;
		const float x = Math::cos(ang) * a * rr;
		const float z = Math::sin(ang) * b * rr;
		const Vector3 n = normal(x, z);
		const Vector3 d = (n * _rand(0.6f, 1.0f) + mound_dir(_rand(0.0f, Math::TAU), 0.0f) * _rand(0.2f, 0.6f)).normalized();
		Leaf l;
		l.shape = LEAF_LINEAR;
		l.length = _rand(0.04f, 0.1f) * (1.0f + 0.6f * rr);
		l.width = _rand(0.0015f, 0.0028f); // Fescue's rolled leaves and bent's flat ones.
		l.fold = 0.6f;
		l.bend = _rand(0.2f, 0.7f);
		l.twist = _rand(-0.3f, 0.3f);
		l.segments = 3;
		const float grey = _rand(0.0f, 1.0f);
		const Color tint = Color(1, 1, 1).lerp(Color(1.15f, 1.1f, 1.2f), grey * 0.6f) * _rand(0.85f, 1.12f);
		_add_leaf(SURFACE_LEAF, Transform3D(mound_frame(d, Vector3(0, 1, 0)), ground(x, z)), l, 0.0f, 0.8f, tint);
	}
	// Wild thyme: low cushions of tiny leaves on the sunny south-east flank (it likes the warm, dry face).
	const int cushions = int(_rand(2.0f, 5.0f));
	for (int c = 0; c < cushions; c++) {
		const float ang = _rand(-1.6f, 0.2f); // From south (+z is south) round to east.
		const float rr = _rand(0.3f, 0.75f);
		const Vector2 cc(Math::cos(ang) * a * rr, Math::sin(-ang) * b * rr);
		const float cr = _rand(0.04f, 0.08f);
		const int leaves = int(_rand(30.0f, 55.0f));
		for (int k = 0; k < leaves; k++) {
			const float la = _rand(0.0f, Math::TAU);
			const float lr = cr * Math::sqrt(_rand(0.0f, 1.0f));
			const float x = cc.x + Math::cos(la) * lr;
			const float z = cc.y + Math::sin(la) * lr;
			Leaf l;
			l.shape = LEAF_OVATE;
			l.length = _rand(0.004f, 0.007f);
			l.width = l.length * 0.6f;
			l.fold = 0.2f;
			l.bend = 0.2f;
			l.segments = 2;
			const Vector3 n = normal(x, z);
			const Vector3 d = (n * 0.5f + mound_dir(la, 0.0f)).normalized();
			_add_leaf(SURFACE_LEAF, Transform3D(mound_frame(d, n), ground(x, z) + n * 0.006f), l, 0.0f, 0.7f, Color(0.85f, 0.95f, 0.8f) * _rand(0.85f, 1.1f));
		}
	}
	// Fresh workings: fine pale soil the ants bring up through the turf near the crest, a few small bare patches of
	// crumbs (most in spring; the season's "ant_workings", 0 to 1, scales them; a few always).
	const float work = season.has("ant_workings") ? CLAMP(float(season["ant_workings"]), 0.0f, 1.0f) : 0.5f;
	const int patches = int(_rand(1.0f, 3.0f) + 3.0f * work);
	for (int p = 0; p < patches; p++) {
		const float ang = _rand(0.0f, Math::TAU);
		const float rr = _rand(0.0f, 0.45f);
		const Vector2 pc(Math::cos(ang) * a * rr + shift * a, Math::sin(ang) * b * rr);
		const int crumbs = int(_rand(8.0f, 18.0f));
		for (int k = 0; k < crumbs; k++) {
			const float ca = _rand(0.0f, Math::TAU);
			const float cr = _rand(0.0f, 0.045f);
			const float x = pc.x + Math::cos(ca) * cr;
			const float z = pc.y + Math::sin(ca) * cr;
			const float sz = _rand(0.004f, 0.01f);
			const Vector3 at = ground(x, z) + normal(x, z) * 0.006f;
			const int from = cb.verts.size();
			// Pale fine loam on the dark green turf base (centre_color): the tint lifts it to the soil's colour.
			_add_head(SURFACE_CENTRE, Transform3D(mound_frame(normal(x, z), Vector3(1, 0, 0)), at - normal(x, z) * sz * 0.3f), sz, sz * _rand(0.5f, 0.9f), Color(2.25f, 1.15f, 2.1f) * _rand(0.9f, 1.1f), 3, 6);
			_lump_since(from, at, sz, 0.3f);
		}
	}
	mound_still(builders[SURFACE_CENTRE].uv2s, still_from, &builders[SURFACE_CENTRE].colors);
	for (int sf = 0; sf < SURFACE_MAX; sf++) {
		PackedColorArray &cl = builders[sf].colors; // Alpha only: nothing on a mound wilts.
		for (int k = 0; k < cl.size(); k++) {
			Color c = cl[k];
			c.a = 1.0f;
			cl.write[k] = c;
		}
	}
}

// A red wood ant nest: an earth bank round the foot, and over it a dome of thatch (needles, twig bits) 0.6 m to the rim
// at scale 1, the surface a loose layer of needles lying every way, a few dark entrances round the lower flanks.
void PlantGenerator::_build_wood_ant_nest() {
	const int still_from = builders[SURFACE_CENTRE].uv2s.size();
	const float r = 0.6f * _rand(0.9f, 1.1f);
	const float h = r * _rand(0.6f, 0.95f);
	// The bank: low, mossy earth round the base.
	const int bank_from = builders[SURFACE_CENTRE].verts.size();
	_add_head(SURFACE_CENTRE, Transform3D(mound_frame(Vector3(0, 1, 0), Vector3(1, 0, 0)), Vector3(0, -0.02f, 0)), r * 1.18f, h * 0.22f, Color(0.75f, 0.9f, 0.6f), 4, 28);
	_lump_since(bank_from, Vector3(0, 0, 0), r * 1.18f, 0.08f);
	// The dome: thatch, a little south-leaning (the nest's sunny side is built up first; Kilpelainen et al.).
	const int dome_from = builders[SURFACE_CENTRE].verts.size();
	const Vector3 lean(0.0f, 1.0f, _rand(0.03f, 0.1f));
	_add_head(SURFACE_CENTRE, Transform3D(mound_frame(lean, Vector3(1, 0, 0)), Vector3(0, -0.01f, 0)), r, h, Color(1, 1, 1), 10, 32);
	_lump_since(dome_from, Vector3(0, 0, 0), r, 0.025f);
	plant_height = MAX(plant_height, h);
	const Basis db = mound_frame(lean, Vector3(1, 0, 0));
	// A point on the dome at height share u (0 rim, 1 top) and angle ang, and its normal.
	auto on_dome = [&](float p_u, float p_ang, Vector3 &r_n) {
		const float rad = r * Math::sqrt(MAX(0.0f, 1.0f - p_u * p_u));
		const Vector3 local(Math::cos(p_ang) * rad, Math::sin(p_ang) * rad, h * p_u);
		r_n = db.xform(Vector3(local.x / (r * r), local.y / (r * r), local.z / (h * h))).normalized();
		return db.xform(local) + Vector3(0, -0.01f, 0);
	};
	// Needles: 1 to 2 cm, lying on the surface every which way, fresh rusty ones on top of greyer old ones.
	const int needles = int(_rand(2600.0f, 3200.0f) * r / 0.6f); // A closed layer: the dome under them hardly shows.
	for (int k = 0; k < needles; k++) {
		const float u = Math::sqrt(_rand(0.0f, 0.98f));
		const float ang = _rand(0.0f, Math::TAU);
		Vector3 n;
		const Vector3 p = on_dome(u, ang, n);
		// A direction in the tangent plane, tipped a little off it.
		Vector3 t = n.cross(mound_dir(_rand(0.0f, Math::TAU), _rand(-0.3f, 0.3f)));
		if (t.length_squared() < 1e-6f) {
			continue;
		}
		t = (t.normalized() + n * _rand(-0.1f, 0.25f)).normalized();
		const int from = builders[SURFACE_STEM].colors.size();
		const float rad = _rand(0.0006f, 0.0009f);
		_add_stem(Transform3D(mound_frame(t, n), p), _rand(0.01f, 0.022f), rad, rad * 0.6f, 0.0f, 1, 0.0f);
		const float age = _rand(0.0f, 1.0f);
		_tint_since(SURFACE_STEM, from, Color(1, 1, 1).lerp(Color(0.62f, 0.6f, 0.62f), age * age) * _rand(0.8f, 1.15f));
	}
	// Twig bits, 3 to 7 cm, and bud scales: fewer, coarser.
	const int twigs = int(_rand(60.0f, 110.0f) * r / 0.6f);
	for (int k = 0; k < twigs; k++) {
		const float u = Math::sqrt(_rand(0.0f, 0.95f));
		Vector3 n;
		const Vector3 p = on_dome(u, _rand(0.0f, Math::TAU), n);
		Vector3 t = n.cross(mound_dir(_rand(0.0f, Math::TAU), 0.0f));
		if (t.length_squared() < 1e-6f) {
			continue;
		}
		const int from = builders[SURFACE_STEM].colors.size();
		const float rad = _rand(0.0012f, 0.0025f);
		_add_stem(Transform3D(mound_frame(t.normalized(), n), p), _rand(0.03f, 0.07f), rad, rad * 0.7f, _rand(0.0f, 0.3f), 2, 0.0f);
		_tint_since(SURFACE_STEM, from, Color(0.7f, 0.62f, 0.55f) * _rand(0.8f, 1.2f));
	}
	// Entrances: dark openings round the lower half, more on the sunny south side.
	const int holes = int(_rand(6.0f, 12.0f));
	for (int k = 0; k < holes; k++) {
		const float ang = _rand(0.0f, 1.0f) < 0.6f ? _rand(0.6f, 2.5f) : _rand(0.0f, Math::TAU);
		Vector3 n;
		const Vector3 p = on_dome(_rand(0.1f, 0.55f), ang, n);
		const float hr = _rand(0.008f, 0.016f);
		_add_head(SURFACE_CENTRE, Transform3D(mound_frame(n, Vector3(0, 1, 0)), p - n * 0.002f), hr, hr * 0.15f, Color(0.18f, 0.16f, 0.15f), 2, 8);
	}
	mound_still(builders[SURFACE_CENTRE].uv2s, still_from, &builders[SURFACE_CENTRE].colors);
	for (int sf = 0; sf < SURFACE_MAX; sf++) {
		PackedColorArray &cl = builders[sf].colors; // Alpha only: nothing on a mound wilts.
		for (int k = 0; k < cl.size(); k++) {
			Color c = cl[k];
			c.a = 1.0f;
			cl.write[k] = c;
		}
	}
}

// A molehill: loose soil pushed up from a run, 0.2 m to the rim at scale 1 and 0.1 to 0.16 m high; a fringe of spilt
// crumbs. By the season's "mound_age": fresh dark and crumbly; crusting greyer in days; slumping lower and rounder over
// weeks; grass growing in from the rim and over the top within months.
void PlantGenerator::_build_molehill() {
	const int still_from = builders[SURFACE_CENTRE].uv2s.size();
	const float age = _mound_age();
	const float crust = Math::smoothstep(0.0f, 0.1f, age);
	const float slump = Math::smoothstep(0.03f, 0.6f, age);
	const float green = Math::smoothstep(0.15f, 1.0f, age);
	const float r = 0.2f * _rand(0.85f, 1.2f);
	const float h = _rand(0.1f, 0.16f) * (1.0f - 0.55f * slump);
	const Color soil = Color(1, 1, 1).lerp(Color(1.55f, 1.5f, 1.4f), crust * 0.8f);
	// The heap: a lumpy cone of soil (a dome sharpened toward a top a little off centre, where the mole pushed up).
	const int from = builders[SURFACE_CENTRE].verts.size();
	_add_head(SURFACE_CENTRE, Transform3D(mound_frame(Vector3(_rand(-0.08f, 0.08f), 1.0f, _rand(-0.08f, 0.08f)), Vector3(1, 0, 0)), Vector3(0, -0.01f, 0)), r, h, soil, 7, 18);
	{
		PackedVector3Array &v = builders[SURFACE_CENTRE].verts;
		for (int i = from; i < v.size(); i++) {
			// Sharpen the top while fresh (loose soil stands at its angle of repose), round it as it slumps.
			const float y = v[i].y;
			const float u = CLAMP(y / MAX(h, 1e-3f), 0.0f, 1.0f);
			const float pinch = (1.0f - 0.45f * u) * (1.0f - slump) + slump;
			v.write[i] = Vector3(v[i].x * pinch, y, v[i].z * pinch);
		}
	}
	_lump_since(from, Vector3(0, h * 0.3f, 0), r, 0.22f * (1.0f - 0.6f * slump));
	plant_height = MAX(plant_height, h);
	// Crumbs: clods on the heap's surface and a fringe of spilt soil, fewer and flatter as rain settles them.
	const int crumbs = int(_rand(40.0f, 70.0f) * (1.0f - 0.75f * slump));
	for (int k = 0; k < crumbs; k++) {
		const float ang = _rand(0.0f, Math::TAU);
		const bool fringe = rng.randf() < 0.35f;
		const float rr = fringe ? r * _rand(0.9f, 1.35f) : r * Math::sqrt(_rand(0.0f, 0.85f));
		const float u = rr / r;
		const float y = fringe ? 0.0f : h * Math::sqrt(MAX(0.0f, 1.0f - u * u)) * ((1.0f - 0.45f * u) * (1.0f - slump) + slump);
		const float cr = _rand(0.006f, 0.02f) * (fringe ? 0.7f : 1.0f);
		const Vector3 at(Math::cos(ang) * rr, y - cr * 0.35f - 0.005f, Math::sin(ang) * rr);
		const int cf = builders[SURFACE_CENTRE].verts.size();
		_add_head(SURFACE_CENTRE, Transform3D(mound_frame(mound_dir(_rand(0.0f, Math::TAU), _rand(0.9f, 1.57f)), Vector3(1, 0, 0)), at), cr, cr * _rand(0.5f, 0.9f) * (1.0f - 0.4f * slump), soil * _rand(0.8f, 1.15f), 3, 6);
		_lump_since(cf, at, cr, 0.3f);
	}
	// The pasture's grass round the rim, flattened outward under the spill, and growing back in over the heap.
	const int blades = int(_rand(30.0f, 50.0f) + green * _rand(150.0f, 260.0f));
	for (int k = 0; k < blades; k++) {
		const float ang = _rand(0.0f, Math::TAU);
		const float reach = 1.0f - green; // How far in from the rim the bare soil still is (0: the top grassed too).
		const float rr = r * _rand(MIN(reach, 0.98f) * 0.95f, 1.1f);
		const float u = MIN(rr / r, 1.0f);
		const float y = rr >= r ? 0.0f : h * Math::sqrt(MAX(0.0f, 1.0f - u * u)) * ((1.0f - 0.45f * u) * (1.0f - slump) + slump);
		const Vector3 out_dir = mound_dir(ang, 0.0f);
		const bool flattened = rr > r * 0.95f && green < 0.5f;
		const Vector3 d = flattened ? (out_dir + Vector3(0, _rand(0.15f, 0.4f), 0)).normalized() : (Vector3(0, 1, 0) + out_dir * _rand(0.1f, 0.5f)).normalized();
		Leaf l;
		l.shape = LEAF_LINEAR;
		l.length = flattened ? _rand(0.04f, 0.1f) : _rand(0.02f, 0.07f) * (0.5f + 0.5f * green);
		l.width = _rand(0.002f, 0.0035f);
		l.fold = 0.4f;
		l.bend = _rand(0.2f, 0.6f);
		l.twist = _rand(-0.3f, 0.3f);
		l.segments = 3;
		_add_leaf(SURFACE_LEAF, Transform3D(mound_frame(d, Vector3(0, 1, 0)), Vector3(Math::cos(ang) * rr, y - 0.004f, Math::sin(ang) * rr)), l, 0.0f, 0.85f, Color(1, 1, 1) * _rand(0.85f, 1.1f));
	}
	mound_still(builders[SURFACE_CENTRE].uv2s, still_from, &builders[SURFACE_CENTRE].colors);
	for (int sf = 0; sf < SURFACE_MAX; sf++) {
		PackedColorArray &cl = builders[sf].colors; // Alpha only: nothing on a mound wilts.
		for (int k = 0; k < cl.size(); k++) {
			Color c = cl[k];
			c.a = 1.0f;
			cl.write[k] = c;
		}
	}
}

// The mound sets: each species carries "sward_mound" (its radius at scale 1) so GrassField3D keeps blades from under it.
TypedArray<Resource> PlantGenerator::make_mounds(MoundSet p_set, int p_variants, bool p_textures, const Dictionary &p_season) {
	using P = PlantGenerator;
	struct Spec {
		Preset preset;
		float age; // A molehill's "mound_age" (years).
		float radius; // At scale 1, m.
		float spacing, density, scale_min, scale_max;
		float patch_scale, patch_cover, patch_ring;
		Vector2 shade; // RULE_SHADE.
		Vector2 conifer; // RULE_CONIFER.
		float visibility;
	};
	LocalVector<Spec> set;
	switch (p_set) {
		case MOUNDS_MEADOW:
			// Ant hills in groups over the old pasture, out of the shade; molehills in lines and arcs along the runs
			// (the patches' rings), fresh ones among older, crusted and grassing ones.
			set = LocalVector<Spec>({ { P::PRESET_ANTHILL, 0.0f, 0.4f, 2.6f, 0.55f, 0.75f, 1.35f, 14.0f, 0.45f, 0.0f, Vector2(0.0f, 0.3f), Vector2(0.0f, 1.0f), 70.0f },
					{ P::PRESET_MOLEHILL, 0.02f, 0.22f, 1.3f, 0.5f, 0.85f, 1.25f, 9.0f, 0.35f, 0.5f, Vector2(0.0f, 0.6f), Vector2(0.0f, 0.5f), 60.0f },
					{ P::PRESET_MOLEHILL, 0.12f, 0.22f, 1.5f, 0.4f, 0.85f, 1.2f, 9.0f, 0.35f, 0.5f, Vector2(0.0f, 0.6f), Vector2(0.0f, 0.5f), 60.0f },
					{ P::PRESET_MOLEHILL, 0.6f, 0.22f, 1.6f, 0.35f, 0.8f, 1.15f, 9.0f, 0.35f, 0.5f, Vector2(0.0f, 0.6f), Vector2(0.0f, 0.5f), 45.0f } });
			break;
		case MOUNDS_WOOD:
			// Wood ant nests at the sunny gaps and edges of conifer and mixed stands (some crown shade, not deep
			// shade, not the open); a few molehills under broadleaves.
			set = LocalVector<Spec>({ { P::PRESET_WOOD_ANT_NEST, 0.0f, 0.72f, 14.0f, 0.55f, 0.6f, 1.6f, 45.0f, 0.4f, 0.0f, Vector2(0.15f, 0.7f), Vector2(0.3f, 1.0f), 90.0f },
					{ P::PRESET_MOLEHILL, 0.05f, 0.22f, 1.8f, 0.3f, 0.8f, 1.15f, 9.0f, 0.25f, 0.5f, Vector2(0.0f, 0.9f), Vector2(0.0f, 0.4f), 45.0f },
					{ P::PRESET_MOLEHILL, 0.4f, 0.22f, 2.0f, 0.25f, 0.8f, 1.1f, 9.0f, 0.25f, 0.5f, Vector2(0.0f, 0.9f), Vector2(0.0f, 0.4f), 40.0f } });
			break;
		case MOUNDS_MAX:
			break;
	}
	TypedArray<Resource> out;
	int seed_base = 10500 + int(p_set) * 100;
	for (const Spec &f : set) {
		Dictionary season = p_season.duplicate();
		if (f.preset == P::PRESET_MOLEHILL) {
			season["mound_age"] = f.age;
		}
		Ref<PlantGenerator> gen;
		gen.instantiate();
		gen->set_textures(p_textures);
		gen->set_preset(f.preset);
		gen->apply_preset_colors();
		gen->set_seed(seed_base);
		gen->set_season(season);
		Ref<PlantSpecies> sp;
		sp.instantiate();
		TypedArray<Mesh> meshes;
		const TypedArray<ArrayMesh> v = gen->generate_variants(MAX(p_variants, 1));
		for (int i = 0; i < v.size(); i++) {
			meshes.push_back(v[i]);
		}
		sp->set_meshes(meshes);
		sp->set_spacing(f.spacing);
		sp->set_footprint(f.radius);
		sp->set_density(f.density);
		sp->set_scale_range(Vector2(f.scale_min, f.scale_max));
		sp->set_align_to_ground(1.0f);
		// Ant hills keep their east crest: no random turn.
		sp->set_random_yaw(f.preset != P::PRESET_ANTHILL);
		sp->set_patch_scale(f.patch_scale);
		sp->set_patch_cover(f.patch_cover);
		sp->set_patch_seed(seed_base);
		if (f.patch_ring > 0.0f) {
			sp->set_patch_ring(f.patch_ring); // Lines and arcs along the patches' edges: a run's heaps.
		}
		sp->set_rule_range(PlantSpecies::RULE_SHADE, f.shade);
		sp->set_rule_fade(PlantSpecies::RULE_SHADE, 0.1f);
		sp->set_rule_range(PlantSpecies::RULE_CONIFER, f.conifer);
		sp->set_rule_fade(PlantSpecies::RULE_CONIFER, 0.15f);
		sp->set_rule_range(PlantSpecies::RULE_SLOPE, Vector2(0.0f, f.preset == P::PRESET_ANTHILL ? 25.0f : 30.0f));
		sp->set_rule_fade(PlantSpecies::RULE_SLOPE, 5.0f);
		// Not in waterlogged ground: moles and ants both need drained soil.
		sp->set_rule_range(PlantSpecies::RULE_WATER_DISTANCE, Vector2(3.0f, 1000000.0f));
		sp->set_rule_fade(PlantSpecies::RULE_WATER_DISTANCE, 2.0f);
		if (f.preset == P::PRESET_WOOD_ANT_NEST) {
			// Inside the wood near its edge or a gap: not out in the open, not deep in.
			sp->set_rule_range(PlantSpecies::RULE_FOREST_EDGE, Vector2(-2.0f, 40.0f));
			sp->set_rule_fade(PlantSpecies::RULE_FOREST_EDGE, 6.0f);
		}
		sp->set_visibility_range(f.visibility);
		sp->set_meta("mound", true);
		// The field's own blades stop a little inside a grassed ant hill's foot, so its turf and the sward overlap.
		sp->set_meta("sward_mound", f.preset == P::PRESET_ANTHILL ? f.radius * 0.75f : f.radius);
		if (f.preset == P::PRESET_MOLEHILL) {
			sp->set_meta("mound_age", f.age);
			Color far = Color(0.09f, 0.065f, 0.045f).lerp(Color(0.12f, 0.13f, 0.06f), Math::smoothstep(0.1f, 1.0f, f.age));
			far.a = 0.5f;
			sp->set_far_color(far);
		} else if (f.preset == P::PRESET_ANTHILL) {
			sp->set_far_color(Color(0.13f, 0.16f, 0.08f, 0.5f));
		} else {
			sp->set_far_color(Color(0.11f, 0.075f, 0.045f, 0.7f));
		}
		tag_species(sp, f.preset);
		out.push_back(sp);
		seed_base += 13;
	}
	return out;
}

// Leaf drifts (docs/ideas/vegetation.md, "Leaf drifts"): where the wind piles fallen leaves, in hollows and against
// trunks, logs and stones (VegetationField3D's drift, PlantSpecies.RULE_LITTER_DRIFT), they lie in loose banks a hand
// to two hands deep, the stand's own leaves on top and a dark, matted, half-rotted layer under them (the Oi over the
// Oe; Orndorff and Lang 1981). The heap: an ellipse a by b, h high, its crest nearer one end (the side the wind piled
// against), lumpy.
namespace {
float leaf_heap_height(float p_x, float p_z, const Vector3 &p_heap) {
	const float u = p_x / p_heap.x;
	const float v = p_z / p_heap.z;
	const float r2 = u * u + v * v;
	if (r2 >= 1.0f || p_heap.y <= 0.0f) {
		return 0.0f;
	}
	const float lean = 1.0f - 0.25f * u; // Higher towards -x, the side it was piled against.
	const float lumps = 1.0f + 0.12f * Math::sin(p_x * 23.0f + 1.3f) * Math::sin(p_z * 19.0f - 0.7f);
	return p_heap.y * Math::pow(1.0f - r2, 1.3f) * lean * lumps;
}

Vector3 leaf_heap_normal(float p_x, float p_z, const Vector3 &p_heap) {
	const float e = 0.01f;
	const float dx = leaf_heap_height(p_x + e, p_z, p_heap) - leaf_heap_height(p_x - e, p_z, p_heap);
	const float dz = leaf_heap_height(p_x, p_z + e, p_heap) - leaf_heap_height(p_x, p_z - e, p_heap);
	return Vector3(-dx, 2.0f * e, -dz).normalized();
}
} // namespace

Vector3 PlantGenerator::_litter_spot(float p_lift, Vector3 &r_up) {
	r_up = Vector3(0, 1, 0);
	const float u = rng.randf();
	const float ang = _rand(0.0f, Math::TAU);
	const float lift = p_lift + 0.002f + 0.004f * rng.randf();
	if (litter_heap.y <= 0.0f) {
		const float r = 0.13f * Math::sqrt(u);
		return Vector3(Math::cos(ang) * r, lift, Math::sin(ang) * r);
	}
	// Over the heap and a little past its foot, where the last leaves slid off.
	const float r = Math::sqrt(u) * 1.05f;
	const float x = Math::cos(ang) * litter_heap.x * r;
	const float z = Math::sin(ang) * litter_heap.z * r;
	r_up = leaf_heap_normal(x, z, litter_heap);
	return Vector3(x, leaf_heap_height(x, z, litter_heap) + lift, z);
}

void PlantGenerator::_build_leaf_bank() {
	const int still_from = builders[SURFACE_CENTRE].uv2s.size();
	const float a = _rand(0.45f, 0.8f);
	const Vector3 heap(a, _rand(0.08f, 0.18f), a * _rand(0.45f, 0.7f));
	// The matted, darker layer under the loose leaves, so no gap between them shows the ground: a ring grid under the
	// heap's surface (centre_color).
	Builder &cb = builders[SURFACE_CENTRE];
	const int rings = 8;
	const int sectors = 20;
	const int first = cb.verts.size();
	for (int r = 0; r <= rings; r++) {
		const float t = float(r) / rings;
		for (int sct = 0; sct < sectors; sct++) {
			const float ang = sct * Math::TAU / sectors;
			const float x = Math::cos(ang) * heap.x * t;
			const float z = Math::sin(ang) * heap.z * t;
			cb.verts.push_back(Vector3(x, MAX(leaf_heap_height(x, z, heap) - 0.006f, 0.0f), z));
			cb.uvs.push_back(Vector2(float(sct) / sectors, t));
			cb.uv2s.push_back(Vector2(0.0f, 0.0f));
			const float n = 0.85f + 0.3f * Math::sin(x * 31.0f + z * 17.0f) * Math::sin(z * 29.0f - x * 11.0f);
			cb.colors.push_back(Color(n, n, n));
		}
	}
	for (int r = 0; r < rings; r++) {
		for (int sct = 0; sct < sectors; sct++) {
			const int i0 = first + r * sectors + sct;
			const int i1 = first + r * sectors + (sct + 1) % sectors;
			cb.indices.append_array(PackedInt32Array({ i0, i1, i0 + sectors, i1, i1 + sectors, i0 + sectors }));
		}
	}
	_tag_organ(cb);
	// The stand's leaves over it, layer on layer (or generic leaves with none given).
	litter_heap = heap;
	_build_leaf_litter();
	litter_heap = Vector3();
	plant_height = MAX(plant_height, heap.y);
	mound_still(builders[SURFACE_CENTRE].uv2s, still_from, &builders[SURFACE_CENTRE].colors);
}

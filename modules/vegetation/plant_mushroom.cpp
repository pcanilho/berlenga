/**************************************************************************/
/*  plant_mushroom.cpp                                                    */
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

// Woodland-floor mushrooms (docs/ideas/mushrooms.md). Each part is a surface of revolution of a
// profile round a bent axis (Desbenoit, Vanderhaghe, Galin and Grosjean, "Interactive Modeling of
// Mushrooms", Eurographics 2004): the stalk along its frames, the cap's top and underside round the
// stalk's top frame. Gills are thin fins under the cap, long ones with ranks of short ones between
// as in real agarics, so their depth shows in path tracing; ceps have a tube layer instead. The
// cap is on the petal surface, the gills, ring and warts on the centre surface, the stalk on the
// stem surface.

#include "plant_generator.h"

namespace {
Basis mushroom_frame(const Vector3 &p_up) {
	const Vector3 z = p_up.normalized();
	Vector3 x = Vector3(0, 0, 1).cross(z);
	if (x.length_squared() < 1e-8f) {
		x = Vector3(1, 0, 0);
	}
	x.normalize();
	return Basis(x, z.cross(x).normalized(), z);
}
// A wavy margin's offset round the cap: two harmonics, so the lobes are uneven, not a star.
float mushroom_wave(float p_a, int p_k, float p_phase) {
	return 0.65f * Math::sin(p_k * p_a + p_phase) + 0.35f * Math::sin((2 * p_k + 1) * p_a + 1.7f * p_phase);
}
} // namespace

void PlantGenerator::_add_lathe(Surface p_surface, const LocalVector<Transform3D> &p_frames, const LocalVector<Vector2> &p_rz, const LocalVector<Color> &p_tint, int p_sectors, float p_wave, int p_wave_k, float p_wave_phase, float p_wave_from) {
	const int n = p_rz.size();
	if (n < 2 || p_frames.is_empty()) {
		return;
	}
	Builder &b = builders[p_surface];
	const int first = b.verts.size();
	float rmax = 1e-6f;
	for (const Vector2 &q : p_rz) {
		rmax = MAX(rmax, q.x);
	}
	for (int i = 0; i < n; i++) {
		const Transform3D &f = p_frames[MIN(i, int(p_frames.size()) - 1)];
		const Basis fb = f.basis.orthonormalized();
		const float w = p_wave * Math::smoothstep(p_wave_from, 1.0f, float(p_rz[i].x / rmax));
		for (int s = 0; s < p_sectors; s++) {
			const float a = s * Math::TAU / p_sectors;
			const float r = p_rz[i].x * (1.0f + w * mushroom_wave(a, p_wave_k, p_wave_phase));
			const Vector3 p = f.origin + (fb.get_column(0) * Math::cos(a) + fb.get_column(1) * Math::sin(a)) * r + fb.get_column(2) * p_rz[i].y;
			b.verts.push_back(p);
			b.uvs.push_back(Vector2(float(s) / p_sectors, float(i) / (n - 1)));
			b.uv2s.push_back(Vector2(0.0f, 0.0f));
			b.colors.push_back(i < int(p_tint.size()) ? p_tint[i] : Color(1, 1, 1));
			plant_height = MAX(plant_height, p.y);
		}
	}
	for (int i = 0; i < n - 1; i++) {
		for (int s = 0; s < p_sectors; s++) {
			const int a = first + i * p_sectors + s;
			const int c = first + i * p_sectors + (s + 1) % p_sectors;
			b.indices.append_array(PackedInt32Array({ a, c, a + p_sectors, c, c + p_sectors, a + p_sectors }));
		}
	}
	_tag_organ(b);
}

// The species' forms. p_age 0 is a young button, 1 an old, opened cap: caps open from a dome to
// flat (and the chanterelle's to a funnel) as they age; old ones are darker and drier.
PlantGenerator::MushroomForm PlantGenerator::_mushroom_form(float p_age) {
	MushroomForm m;
	const float open = Math::smoothstep(0.0f, 1.0f, p_age);
	switch (preset) {
		case PRESET_FLY_AGARIC:
			// Cap 8 to 20 cm, scarlet with white warts (the remains of the universal veil); white
			// gills; stalk 10 to 20 cm with a hanging ring and a bulb ringed by the volva.
			m.cap_r = _rand(0.045f, 0.09f) * Math::lerp(0.55f, 1.0f, open);
			m.cap_h = m.cap_r * Math::lerp(0.8f, 0.25f, open);
			m.cap_thick = m.cap_r * 0.22f;
			m.dome = Math::lerp(0.9f, 0.45f, open);
			m.curl = m.cap_r * Math::lerp(0.25f, 0.04f, open);
			m.gills = 44;
			m.gill_depth = m.cap_r * 0.16f;
			m.stalk_h = _rand(0.1f, 0.18f) * Math::lerp(0.5f, 1.0f, open);
			m.stalk_r = m.cap_r * 0.17f;
			m.stalk_base = 1.15f;
			m.bulb = 0.9f;
			m.volva = 3;
			m.ring = 0.82f;
			m.ring_w = m.stalk_r * 0.9f;
			m.warts = int(Math::lerp(35.0f, 60.0f, open));
			m.wart_r = m.cap_r * 0.07f;
			m.cap_centre = Color(0.85f, 0.75f, 0.85f); // Deeper red at the centre, orange to the margin.
			m.cap_margin = Color(1.05f, 1.25f, 1.0f);
			break;
		case PRESET_CEP:
			// Cap 8 to 25 cm, bun-shaped, brown, paler at the margin; a tube layer (white, then
			// olive) for gills; a stout, swollen stalk with a pale net.
			m.cap_r = _rand(0.05f, 0.11f) * Math::lerp(0.5f, 1.0f, open);
			m.cap_h = m.cap_r * Math::lerp(0.75f, 0.45f, open);
			m.cap_thick = m.cap_r * 0.35f;
			m.dome = Math::lerp(0.8f, 0.55f, open);
			m.curl = m.cap_r * 0.08f;
			m.gills = 0;
			m.tube_depth = m.cap_r * Math::lerp(0.12f, 0.28f, open);
			m.stalk_h = m.cap_r * _rand(1.0f, 1.5f);
			m.stalk_r = m.cap_r * 0.36f;
			m.stalk_base = 1.3f;
			m.bulb = 0.75f;
			m.cap_centre = Color(0.85f, 0.85f, 0.85f);
			m.cap_margin = Color(1.2f, 1.25f, 1.3f);
			break;
		case PRESET_CHANTERELLE:
			// Cap 3 to 8 cm opening to a funnel with a wavy, in-rolled margin; blunt forking
			// ridges running down the stalk; egg-yellow all over.
			m.cap_r = _rand(0.02f, 0.04f) * Math::lerp(0.6f, 1.0f, open);
			m.cap_h = m.cap_r * 0.35f;
			m.cap_thick = m.cap_r * 0.25f;
			m.dome = 0.5f;
			m.funnel = Math::lerp(0.2f, 1.2f, open);
			m.curl = m.cap_r * Math::lerp(0.2f, 0.08f, open);
			m.wave = Math::lerp(0.04f, 0.12f, open);
			m.wave_k = int(_rand(4.0f, 7.0f));
			m.gills = 26;
			m.gill_depth = m.cap_r * 0.06f;
			m.decurrent = m.cap_r * 1.1f;
			m.stalk_h = m.cap_r * _rand(1.1f, 1.6f);
			m.stalk_r = m.cap_r * 0.24f;
			m.stalk_base = 0.65f;
			m.cap_centre = Color(0.95f, 0.92f, 0.85f);
			m.cap_margin = Color(1.05f, 1.05f, 1.05f);
			break;
		case PRESET_BRITTLEGILL:
			// Cap 4 to 9 cm, convex then flat with a shallow dip, red; white, brittle, crowded
			// gills; a white stalk without a ring.
			m.cap_r = _rand(0.025f, 0.045f) * Math::lerp(0.6f, 1.0f, open);
			m.cap_h = m.cap_r * Math::lerp(0.55f, 0.12f, open);
			m.cap_thick = m.cap_r * 0.22f;
			m.dome = Math::lerp(0.7f, 0.35f, open);
			m.funnel = Math::lerp(0.0f, 0.35f, open);
			m.curl = m.cap_r * 0.05f;
			m.gills = 52;
			m.gill_depth = m.cap_r * 0.13f;
			m.stalk_h = m.cap_r * _rand(1.3f, 1.8f);
			m.stalk_r = m.cap_r * 0.24f;
			m.stalk_base = 1.05f;
			m.cap_centre = Color(0.8f, 0.8f, 0.85f);
			m.cap_margin = Color(1.1f, 1.15f, 1.1f);
			break;
		case PRESET_FAIRY_RING_MUSHROOM:
			// Marasmius oreades (Phillips 2006): cap 2 to 5 cm, bell-shaped then flat with a broad boss, buff to tan,
			// paler as it dries; distant, cream gills; stalk 4 to 8 cm, slender, tough and pale. In rings or arcs,
			// several together.
			m.cap_r = _rand(0.011f, 0.024f) * Math::lerp(0.6f, 1.0f, open);
			m.cap_h = m.cap_r * Math::lerp(0.7f, 0.25f, open);
			m.cap_thick = m.cap_r * 0.18f;
			m.dome = Math::lerp(0.75f, 0.4f, open);
			m.umbo = Math::lerp(0.1f, 0.35f, open);
			m.curl = m.cap_r * Math::lerp(0.12f, 0.02f, open);
			m.wave = Math::lerp(0.005f, 0.03f, open);
			m.gills = 22;
			m.gill_depth = m.cap_r * 0.16f;
			m.stalk_h = _rand(0.035f, 0.07f) * Math::lerp(0.6f, 1.0f, open);
			m.stalk_r = m.cap_r * 0.13f;
			m.stalk_base = 1.1f;
			m.cap_centre = Color(0.85f, 0.82f, 0.8f); // Darker tan over the boss.
			m.cap_margin = Color(1.12f, 1.12f, 1.1f);
			m.cap_fade = 0.4f;
			break;
		case PRESET_CLOUDED_AGARIC:
			// Cap 6 to 15 cm, grey to grey-brown with a bloom, convex then flat, the margin long
			// in-rolled; crowded cream gills running a little down the stalk; a stout stalk.
			m.cap_r = _rand(0.035f, 0.07f) * Math::lerp(0.55f, 1.0f, open);
			m.cap_h = m.cap_r * Math::lerp(0.6f, 0.18f, open);
			m.cap_thick = m.cap_r * 0.25f;
			m.dome = Math::lerp(0.7f, 0.4f, open);
			m.umbo = Math::lerp(0.0f, 0.15f, open);
			m.curl = m.cap_r * Math::lerp(0.2f, 0.06f, open);
			m.wave = 0.02f;
			m.gills = 60;
			m.gill_depth = m.cap_r * 0.1f;
			m.decurrent = m.cap_r * 0.2f;
			m.stalk_h = m.cap_r * _rand(1.0f, 1.4f);
			m.stalk_r = m.cap_r * 0.22f;
			m.stalk_base = 1.35f;
			m.cap_centre = Color(0.8f, 0.8f, 0.8f);
			m.cap_margin = Color(1.15f, 1.15f, 1.15f);
			break;
		case PRESET_HONEY_FUNGUS:
			// Armillaria mellea (Phillips 2006; Kibby 2017): caps 3 to 15 cm, convex then flat with
			// a low boss, tawny honey, darker to the centre where small, erect, dark brown fibrous
			// scales crowd; the margin paler and finely lined. Stalks 6 to 15 cm, tough and fibrous,
			// curved, often tapering to the foot, fused with their neighbours at the base in a dense
			// clump on wood; pale yellow above a thick cottony ring, darkening to olive brown at the
			// foot.
			// Thin-fleshed: convex with an in-rolled rim when young, flattening with age to a low
			// umbo and a thin, wavy, finely lined margin (not the thick, pillowy rolls of a bolete).
			m.cap_r = _rand(0.02f, 0.05f) * Math::lerp(0.55f, 1.0f, open);
			m.cap_h = m.cap_r * Math::lerp(0.38f, 0.08f, open);
			m.cap_thick = m.cap_r * Math::lerp(0.08f, 0.05f, open);
			m.dome = Math::lerp(0.6f, 0.25f, open); // A flatter shoulder: high domes read as bread rolls.
			m.umbo = Math::lerp(0.02f, 0.22f, open);
			m.curl = m.cap_r * Math::lerp(0.07f, -0.03f, open);
			m.wave = Math::lerp(0.002f, 0.022f, open);
			m.wave_k = int(_rand(6.0f, 9.99f));
			m.gills = 40;
			m.gill_depth = m.cap_r * 0.12f;
			m.decurrent = m.cap_r * 0.15f;
			m.stalk_h = m.cap_r * _rand(2.2f, 3.6f);
			m.stalk_r = m.cap_r * 0.15f;
			m.stalk_base = 0.75f;
			m.stalk_wave = _rand(0.3f, 0.6f);
			m.ring = 0.8f;
			m.ring_w = m.stalk_r * 1.2f;
			m.scales = 1.0f;
			m.stalk_foot = Color(0.36f, 0.27f, 0.18f);
			m.ring_tint = Color(1.0f, 0.9f, 0.62f); // Cottony, cream to yellowish (a pure white read as a blob in sun).
			m.cap_centre = Color(0.32f, 0.22f, 0.16f); // Dark tawny brown over the boss.
			m.cap_margin = Color(1.22f, 1.25f, 1.3f); // Pale honey yellow at the rim.
			m.cap_fade = 0.6f; // The brown reaches well out: about half the cap.
			m.fibrils = 0.3f; // Matt, finely fibrillose: the streaks break up the sheen.
			m.moist_band = rng.randf() < 0.5f ? _rand(0.1f, 0.25f) : 0.0f; // Some caps still damp at the rim.
			m.bulb = 0.55f; // The foot swells where it fuses with its neighbours.
			{
				// Each cap its own shade, honey yellow to tawny olive (the species varies with age,
				// moisture and host; Kibby 2017).
				const float v = _rand(0.0f, 1.0f);
				const Color shade = Color(1.06f, 1.04f, 0.92f).lerp(Color(0.86f, 0.84f, 0.8f), v);
				m.cap_centre *= shade;
				m.cap_margin *= shade;
			}
			break;
		default:
			break;
	}
	// Old caps darken and dry a little.
	const float dull = Math::lerp(1.0f, 0.82f, Math::smoothstep(0.6f, 1.0f, p_age));
	m.cap_centre *= dull;
	m.cap_margin *= dull;
	m.cap_centre.a = m.cap_margin.a = 1.0f;
	return m;
}

void PlantGenerator::_add_mushroom(const Transform3D &p_base, const MushroomForm &p_m, float p_tilt, float p_tilt_dir) {
	_begin_organ(p_base.origin);
	// The stalk: a slightly bent axis, leaning p_tilt towards p_tilt_dir, its radius by a profile
	// (taper, basal bulb with the volva's rims), sunk 1 cm in the ground.
	const Vector3 lean_axis = Vector3(Math::cos(p_tilt_dir + Math::PI * 0.5f), 0.0f, Math::sin(p_tilt_dir + Math::PI * 0.5f));
	Basis f0 = Basis(lean_axis, -p_tilt) * mushroom_frame(Vector3(0, 1, 0));
	const int segs = 10;
	const float sink = 0.01f;
	const float bend = _rand(-0.25f, 0.25f);
	const float sway_phase = _rand(0.0f, Math::TAU);
	// A stalk pushed out of a tuft curves back up towards the light: it leaves the base leaning and
	// rises to its cap, giving back most of the lean over its length.
	const float upright = p_tilt > 0.15f ? p_tilt * 0.75f : 0.0f;
	LocalVector<Transform3D> frames;
	LocalVector<Vector2> rz;
	LocalVector<Color> tint;
	{
		Basis f = f0;
		Vector3 p = p_base.origin - f.get_column(2) * sink;
		const float len = p_m.stalk_h + sink;
		for (int i = 0; i <= segs; i++) {
			const float t = float(i) / segs;
			frames.push_back(Transform3D(f, p));
			float r = p_m.stalk_r * Math::lerp(p_m.stalk_base, 1.0f, Math::sqrt(t));
			if (p_m.bulb > 0.0f) {
				r += p_m.stalk_r * p_m.bulb * Math::exp(-Math::pow(t / 0.16f, 2.0f));
			}
			if (p_m.volva > 0 && t < 0.3f) {
				// Concentric rims of the torn volva round the bulb's upper half.
				r += p_m.stalk_r * 0.12f * MAX(Math::sin(t / 0.3f * Math::PI * p_m.volva), 0.0f) * Math::smoothstep(0.05f, 0.12f, t);
			}
			rz.push_back(Vector2(r, 0.0f));
			const float ao = Math::lerp(0.55f, 1.0f, Math::smoothstep(0.0f, 0.35f, t)); // Soil-stained foot.
			// The foot's own colour, up to below the ring.
			const Color foot = p_m.stalk_foot.lerp(Color(1, 1, 1), Math::smoothstep(0.05f, 0.75f, t));
			tint.push_back(Color(ao * foot.r, ao * 0.97f * foot.g, ao * 0.92f * foot.b));
			if (i < segs) {
				// A fused tuft's stalks sway a little as they push past each other.
				const float sway = p_m.stalk_wave * Math::cos(t * Math::TAU + sway_phase) / segs;
				f = Basis(lean_axis, upright / segs) * Basis(f.get_column(0), bend / segs + sway) * f;
				f.orthonormalize();
				p += f.get_column(2) * (len / segs);
			}
		}
	}
	const int stalk_sectors = 12;
	_add_lathe(SURFACE_STEM, frames, rz, tint, stalk_sectors, 0.0f, 1, 0.0f, 1.0f);
	const Transform3D top = frames[frames.size() - 1];
	// The cap's frame: the stalk's top, the cap a little more level than the stalk.
	const Basis cb = top.basis.slerp(mushroom_frame(Vector3(0, 1, 0)), 0.35f).orthonormalized();
	const float gill_top = 0.0f; // The underside meets the stalk at the top frame.
	const Transform3D cap(cb, top.origin);
	const float R = p_m.cap_r;
	const float rs = p_m.stalk_r * 0.95f;
	const float phase = _rand(0.0f, Math::TAU);
	// Top surface height over the margin at x = r / R: a dome, a funnel sunk at the centre, an umbo.
	auto top_z = [&](float x) {
		const float dome = Math::pow(MAX(1.0f - x * x, 0.0f), p_m.dome);
		const float funnel = -p_m.funnel * Math::exp(-Math::pow(x / 0.55f, 2.0f));
		const float umbo = p_m.umbo * Math::exp(-Math::pow(x / 0.22f, 2.0f));
		return p_m.cap_h * (dome + funnel + umbo);
	};
	// Flesh thickness, thinning to the margin.
	auto thick = [&](float x) { return 0.0025f + p_m.cap_thick * Math::pow(MAX(1.0f - x, 0.0f), 0.8f); };
	// The cap's base level: the underside at the stalk sits at the stalk's top.
	const float z0 = gill_top - (top_z(rs / R) - thick(rs / R));
	// Margin curl: the outer rim turns down (and in).
	auto curl_z = [&](float x) { return -p_m.curl * Math::pow(Math::smoothstep(0.6f, 1.0f, x), 2.0f); };
	// Fewer sectors on small caps: about 6 mm between them at the margin, 16 to 32.
	const int prof = 10;
	// Fibrillose caps get more sectors: their streaks are drawn by the vertex colours.
	const int cap_sectors = p_m.fibrils > 0.0f ? CLAMP(int(R * Math::TAU / 0.003f), 32, 64) : CLAMP(int(R * Math::TAU / 0.006f), 16, 32);
	// Top and margin (petal surface): from the margin's underside edge, up round the rim, over the
	// top to the centre.
	{
		LocalVector<Transform3D> cf;
		cf.push_back(cap);
		LocalVector<Vector2> p;
		LocalVector<Color> c;
		const float edge = 0.0025f;
		const float xm = 1.0f;
		p.push_back(Vector2(R * 0.985f, z0 + top_z(xm) + curl_z(xm) - edge));
		c.push_back(p_m.cap_margin * 0.9f);
		for (int i = 0; i <= prof; i++) {
			const float x = 1.0f - float(i) / prof;
			p.push_back(Vector2(R * x, z0 + top_z(x) + curl_z(x)));
			Color t = p_m.cap_centre.lerp(p_m.cap_margin, Math::smoothstep(0.0f, 0.9f, Math::pow(x, 1.0f + p_m.cap_fade)));
			c.push_back(t);
		}
		Builder &pb = builders[SURFACE_PETAL];
		const int from = pb.verts.size();
		_add_lathe(SURFACE_PETAL, cf, p, c, cap_sectors, p_m.wave, p_m.wave_k, phase, 0.3f);
		if (p_m.fibrils > 0.0f || p_m.moist_band > 0.0f) {
			// Radial fibrils (a fine streak per sector, each its own depth; wider ones read as pleats) and the hygrophanous
			// rim band, on the cap's vertex colours by angle and distance from the axis.
			const Transform3D inv = cap.affine_inverse();
			const float seed_k = _rand(0.0f, 100.0f);
			Color *cols = pb.colors.ptrw();
			const Vector3 *vs = pb.verts.ptr();
			for (int v = from; v < pb.verts.size(); v++) {
				const Vector3 l = inv.xform(vs[v]);
				const float rr = Vector2(l.x, l.y).length() / MAX(R, 1e-4f);
				const float ang = Math::atan2(l.y, l.x);
				const float k = Math::floor((ang / Math::TAU + 0.5f) * float(cap_sectors));
				const float h = Math::fract(Math::sin(k * 12.9898f + seed_k) * 43758.5453f);
				float f = 1.0f - p_m.fibrils * h * Math::smoothstep(0.15f, 0.6f, rr);
				f *= 1.0f - p_m.moist_band * Math::smoothstep(0.62f, 0.78f, rr) * (1.0f - Math::smoothstep(0.9f, 1.0f, rr));
				cols[v] = Color(cols[v].r * f, cols[v].g * f, cols[v].b * f, cols[v].a);
			}
		}
	}
	// Underside (centre surface): from the stalk out to the margin's lower edge. With a tube
	// layer, a thick cushion whose lower face is the pores.
	{
		LocalVector<Transform3D> cf;
		cf.push_back(cap);
		LocalVector<Vector2> p;
		LocalVector<Color> c;
		for (int i = 0; i <= prof; i++) {
			const float x = Math::lerp(rs / R, 0.985f, float(i) / prof);
			float z = z0 + top_z(x) + curl_z(x) - thick(x);
			if (p_m.tube_depth > 0.0f) {
				z -= p_m.tube_depth * Math::pow(float(Math::sin(Math::PI * MIN((x - rs / R) / (0.95f - rs / R), 1.0f))), 0.45f);
			}
			if (i == prof) {
				z = MIN(z, z0 + top_z(0.985f) + curl_z(0.985f) - 0.0025f);
			}
			p.push_back(Vector2(R * x, z));
			const float shade = Math::lerp(0.55f, 0.9f, float(i) / prof); // Dark into the gills.
			c.push_back(Color(shade, shade, shade));
		}
		_add_lathe(SURFACE_CENTRE, cf, p, c, cap_sectors, p_m.wave, p_m.wave_k, phase, 0.3f);
	}
	const Basis cbo = cap.basis;
	auto cap_point = [&](float x, float a, float z) {
		const float w = 1.0f + p_m.wave * Math::smoothstep(0.3f, 1.0f, x) * mushroom_wave(a, p_m.wave_k, phase);
		return cap.origin + (cbo.get_column(0) * Math::cos(a) + cbo.get_column(1) * Math::sin(a)) * (R * x * w) + cbo.get_column(2) * z;
	};
	// Gills: long ones from the stalk to the margin, a short one (half the length, from the margin)
	// between each two. Each a fin bowing down from the underside,
	// deepest a third of the way out; decurrent ones run down the stalk.
	if (p_m.gills > 0 && p_m.tube_depth <= 0.0f) {
		Builder &b = builders[SURFACE_CENTRE];
		const int total = p_m.gills * 2;
		const int fs = 5;
		for (int k = 0; k < total; k++) {
			const float a = k * Math::TAU / total;
			const float x0 = k % 2 == 0 ? rs / R : Math::lerp(rs / R, 1.0f, 0.5f);
			const float x1 = 0.975f;
			const int first = b.verts.size();
			for (int i = 0; i <= fs; i++) {
				const float s = float(i) / fs;
				const float x = Math::lerp(x0, x1, s);
				const float under = z0 + top_z(x) + curl_z(x) - thick(x);
				// Deepest a third of the way out, nothing at the margin.
				float depth = p_m.gill_depth * Math::pow(float(MAX(Math::sin(Math::PI * Math::pow(s, 0.75f)), 0.0f)), 0.6f);
				Vector3 top_p = cap_point(x, a, under + 0.0015f);
				Vector3 bot_p = cap_point(x, a, under - depth);
				if (k % 2 == 0 && p_m.decurrent > 0.0f && i == 0) {
					// Down the stalk.
					bot_p = cap_point(x * 1.02f, a, under - p_m.decurrent);
				}
				b.verts.push_back(top_p);
				b.verts.push_back(bot_p);
				b.uvs.push_back(Vector2(s, 0.0f));
				b.uvs.push_back(Vector2(s, 1.0f));
				b.uv2s.push_back(Vector2());
				b.uv2s.push_back(Vector2());
				b.colors.push_back(Color(0.6f, 0.6f, 0.6f)); // In the cap's shadow.
				b.colors.push_back(Color(0.9f, 0.9f, 0.9f));
			}
			for (int i = 0; i < fs; i++) {
				const int q = first + i * 2;
				b.indices.append_array(PackedInt32Array({ q, q + 2, q + 1, q + 2, q + 3, q + 1 }));
			}
		}
		_tag_organ(b);
	}
	// The ring: a skirt hanging from the upper stalk.
	if (p_m.ring > 0.0f) {
		const Transform3D at = frames[int(Math::round(p_m.ring * segs))];
		LocalVector<Transform3D> rf;
		rf.push_back(at);
		LocalVector<Vector2> p;
		LocalVector<Color> c;
		const float r0 = p_m.stalk_r * 1.02f;
		for (int i = 0; i <= 4; i++) {
			const float s = float(i) / 4;
			p.push_back(Vector2(r0 + p_m.ring_w * Math::sin(s * Math::PI * 0.5f), -p_m.ring_w * 1.4f * s * s));
			c.push_back(p_m.ring_tint.lerp(p_m.ring_tint * Color(0.8f, 0.8f, 0.78f), s));
		}
		_add_lathe(SURFACE_CENTRE, rf, p, c, 16, 0.08f, 7, phase, 0.0f);
	}
	// Warts: low, flat-topped patches of the veil over the cap, smaller and denser to the centre.
	for (int w = 0; w < p_m.warts; w++) {
		const float x = Math::sqrt(_rand(0.0f, 0.85f));
		const float a = _rand(0.0f, Math::TAU);
		const float z = z0 + top_z(x) + curl_z(x);
		const Vector3 p = cap_point(x, a, z);
		const Vector3 pn = cap_point(MIN(x + 0.02f, 1.0f), a, z0 + top_z(MIN(x + 0.02f, 1.0f)) + curl_z(MIN(x + 0.02f, 1.0f)));
		const Vector3 pt = cap_point(x, a + 0.02f, z);
		Vector3 n = (pn - p).cross(pt - p);
		if (n.dot(cbo.get_column(2)) < 0.0f) {
			n = -n;
		}
		if (n.length_squared() < 1e-12f) {
			n = cbo.get_column(2);
		}
		const float r = p_m.wart_r * _rand(0.5f, 1.2f) * Math::lerp(0.7f, 1.1f, x);
		// A low dome: six sides, a flat top (16 triangles).
		const Basis wf = mushroom_frame(n);
		const Vector3 o = p - n.normalized() * r * 0.3f;
		LocalVector<Transform3D> one;
		one.push_back(Transform3D(wf, o));
		LocalVector<Vector2> wp;
		wp.push_back(Vector2(r, 0.0f));
		wp.push_back(Vector2(r * 0.75f, r * 0.55f));
		wp.push_back(Vector2(0.0f, r * 0.65f));
		LocalVector<Color> wc;
		wc.push_back(Color(0.85f, 0.85f, 0.82f));
		wc.push_back(Color(0.95f, 0.95f, 0.92f));
		wc.push_back(Color(0.95f, 0.95f, 0.92f));
		_add_lathe(SURFACE_CENTRE, one, wp, wc, 6, 0.0f, 1, 0.0f, 1.0f);
	}
	// Scales (honey fungus): small, dark brown fibrillose tufts crowded at the centre and thinning
	// out to the margin: each a fine hair-like filament lying out from the centre, its tip lifted
	// (erect at the centre, flatter outward), not a loose dot.
	const int scale_n = int(p_m.scales * Math::lerp(90.0f, 200.0f, MIN(R / 0.05f, 1.0f)));
	for (int w = 0; w < scale_n; w++) {
		const float x = 0.65f * Math::pow(_rand(0.0f, 1.0f), 2.4f);
		const float a = _rand(0.0f, Math::TAU);
		const float z = z0 + top_z(x) + curl_z(x);
		const Vector3 p = cap_point(x, a, z);
		const float x2 = MIN(x + 0.03f, 1.0f);
		const Vector3 pn = cap_point(x2, a, z0 + top_z(x2) + curl_z(x2));
		const Vector3 pt = cap_point(x, a + 0.03f, z);
		Vector3 n = (pn - p).cross(pt - p);
		if (n.dot(cbo.get_column(2)) < 0.0f) {
			n = -n;
		}
		if (n.length_squared() < 1e-12f) {
			n = cbo.get_column(2);
		}
		n.normalize();
		const Vector3 out = (pn - p).normalized();
		const float r = R * _rand(0.006f, 0.011f) * Math::lerp(1.2f, 0.7f, x);
		const float len = R * _rand(0.04f, 0.07f) * Math::lerp(1.0f, 0.75f, x);
		// Lying out from the centre, the tip lifting more near the centre; a little sideways jitter.
		const Vector3 side = n.cross(out).normalized();
		const float lift = Math::lerp(0.6f, 0.15f, MIN(x / 0.65f, 1.0f)); // Pressed to the cap but for the centre.
		const Basis sf = mushroom_frame((out + n * lift + side * _rand(-0.35f, 0.35f)).normalized());
		LocalVector<Transform3D> one;
		one.push_back(Transform3D(sf, p - n * r * 0.5f));
		LocalVector<Vector2> sp;
		sp.push_back(Vector2(r, 0.0f));
		sp.push_back(Vector2(r * 0.6f, len * 0.55f));
		sp.push_back(Vector2(0.0f, len));
		LocalVector<Color> sc;
		// Dark brown fibrils (the centre surface is pale, so these factors stay low: lit from above, lighter ones read as pale grit).
		// Brown, not black: on the brown centre they read as a felt, not pepper (docs, 16 Nov).
		const float dark = _rand(0.3f, 0.45f);
		sc.push_back(Color(dark * 1.3f, dark * 0.95f, dark * 0.6f));
		sc.push_back(Color(dark * 1.25f, dark * 0.9f, dark * 0.55f));
		sc.push_back(Color(dark * 1.1f, dark * 0.78f, dark * 0.48f));
		_add_lathe(SURFACE_CENTRE, one, sp, sc, 4, 0.0f, 1, 0.0f, 1.0f);
	}
}

// A group: one to a few fruit bodies of mixed ages (a flush spans a week or two), or the honey
// fungus's tuft fused at the base, the outer ones leaning out.
void PlantGenerator::_build_mushrooms() {
	int count = 1;
	float spread = 0.06f;
	bool tuft = false;
	switch (preset) {
		case PRESET_FLY_AGARIC:
			count = rng.randf() < 0.6f ? 1 : int(_rand(2.0f, 3.99f));
			spread = 0.25f;
			break;
		case PRESET_CEP:
			count = rng.randf() < 0.7f ? 1 : 2;
			spread = 0.2f;
			break;
		case PRESET_CHANTERELLE:
			count = int(_rand(3.0f, 8.99f));
			spread = 0.15f;
			break;
		case PRESET_BRITTLEGILL:
			count = rng.randf() < 0.65f ? 1 : 2;
			spread = 0.2f;
			break;
		case PRESET_CLOUDED_AGARIC:
			count = int(_rand(2.0f, 4.99f));
			spread = 0.3f;
			break;
		case PRESET_FAIRY_RING_MUSHROOM:
			count = int(_rand(3.0f, 8.99f));
			spread = 0.12f;
			break;
		case PRESET_HONEY_FUNGUS:
			count = int(_rand(8.0f, 18.99f));
			spread = 0.035f;
			tuft = true;
			break;
		default:
			break;
	}
	for (int i = 0; i < count; i++) {
		// A clump is mostly open caps, a few still convex (young convex caps crowding it read as rolls).
		const float u = _rand(0.0f, 1.0f);
		const float age = tuft ? 1.0f - 0.9f * (1.0f - u) * (1.0f - u) : Math::lerp(0.1f, 1.0f, u);
		const MushroomForm m = _mushroom_form(age);
		float d = i == 0 ? 0.0f : spread * Math::sqrt(_rand(0.15f, 1.0f));
		float a = _rand(0.0f, Math::TAU);
		float tilt = _rand(0.0f, 0.12f);
		float y = 0.0f;
		if (tuft) {
			// The fused base: the stalks all come from one point in the wood, mostly out of its
			// side (+X), the outer ones leaning out low and curving back up to their caps; their
			// swollen feet (the form's bulb) merge into one tough, dark mass where they meet.
			a = _rand(-0.6f, 0.6f) * Math::PI;
			const float reach = Math::sqrt(_rand(0.0f, 1.0f)) * Math::lerp(0.5f, 1.0f, 0.5f + 0.5f * Math::cos(a));
			d = spread * 0.22f * reach;
			tilt = 0.35f + 0.75f * reach + _rand(0.0f, 0.15f);
			y = 0.004f;
		}
		_add_mushroom(Transform3D(Basis(), Vector3(Math::cos(a) * d, y, Math::sin(a) * d)), m, tilt, a);
	}
	if (tuft) {
		// Aborted primordia: a few small knobs crowded in the fused base, as in real clumps.
		const int buttons = int(_rand(2.0f, 5.99f));
		for (int i = 0; i < buttons; i++) {
			const MushroomForm m = _mushroom_form(_rand(0.0f, 0.06f));
			const float a = _rand(-0.5f, 0.5f) * Math::PI;
			_add_mushroom(Transform3D(Basis(), Vector3(Math::cos(a) * spread * 0.2f, 0.002f, Math::sin(a) * spread * 0.2f)), m, _rand(0.4f, 0.9f), a);
		}
	}
}

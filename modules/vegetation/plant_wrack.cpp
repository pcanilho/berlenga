/**************************************************************************/
/*  plant_grazing.cpp                                                     */
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

// Flood wrack lines (docs/ideas/vegetation.md, "Flood wrack lines").
// - A falling flood leaves its floating load stranded along the highest water line: trails of twigs, grasses and
//   other small debris on grassy overbanks and floodplains, thin lines of seeds, needles and grass in tranquil water
//   (USGS, Koenig et al. 2016, Identifying and preserving high-water mark data, TM 3-A24: debris lines, seed lines).
//   The landward edge of a debris line is the flood's crest; debris sags as the water falls, so lower, thinner
//   lines lie below it.
// - Below the mark the grass is folded flat downstream and yellows (a "folded-grass wash line", TM 3-A24 fig. 19-20):
//   GrassField3D's side of it.
// - Seed lines go within hours or days; mud lines last weeks; a debris line of straw and sticks dries grey and is
//   broken up, grown through and gone over a season or so (the year's span is an estimate).
// The mesh: a strand line 1.2 to 2.6 m long along +Z (placement turns +X downstream, down the fall line, so +Z lies
// along the contour), its landward edge (-X) sharp and its foot frayed; a silted mat under straw, reed stem pieces,
// twigs and leaves laid along the line. The season's "flood_age" (years since the peak) ages it.

#include "plant_generator.h"
#include "plant_species.h"

namespace {
Basis wrack_frame(const Vector3 &p_forward, const Vector3 &p_up_hint) {
	const Vector3 z = p_forward.normalized();
	Vector3 x = p_up_hint.cross(z);
	if (x.length_squared() < 1e-8f) {
		x = Vector3(1, 0, 0).cross(z);
	}
	x.normalize();
	return Basis(x, z.cross(x).normalized(), z);
}
Vector3 wrack_dir(float p_azimuth, float p_elevation) {
	return Vector3(Math::cos(p_azimuth) * Math::cos(p_elevation), Math::sin(p_elevation), Math::sin(p_azimuth) * Math::cos(p_elevation));
}
// Wrack does not sway: no wind weight, colour alpha 1 (no withering).
void wrack_still(PackedVector2Array &p_uv2s, int p_from, PackedColorArray &p_colors) {
	for (int i = p_from; i < p_uv2s.size(); i++) {
		p_uv2s.write[i] = Vector2(0.0f, p_uv2s[i].y);
		Color c = p_colors[i];
		c.a = 1.0f;
		p_colors.write[i] = c;
	}
}
} // namespace

float PlantGenerator::_flood_age() const {
	return season.has("flood_age") ? MAX(float(season["flood_age"]), 0.0f) : 0.03f;
}

void PlantGenerator::_build_flood_wrack() {
	const float age = _flood_age();
	const float dry = Math::smoothstep(0.005f, 0.06f, age); // Wet, dark and silty for days; grey and dry in weeks.
	const float gone = Math::smoothstep(0.25f, 1.0f, age); // Broken up and grown through over a season.
	int from[SURFACE_MAX];
	for (int s = 0; s < SURFACE_MAX; s++) {
		from[s] = int(builders[s].uv2s.size());
	}
	const float len = _rand(1.2f, 2.6f);
	const float half_w = _rand(0.1f, 0.24f) * (1.0f - 0.35f * gone);
	const float thick = _rand(0.015f, 0.04f) * (1.0f - 0.7f * gone);
	const float wave_a = _rand(0.03f, 0.12f);
	const float wave_f = _rand(1.5f, 3.5f);
	const float wave_p = _rand(0.0f, Math::TAU);
	// The line's middle across (x) at z, and its half width: ends taper, gaps open where it thins.
	auto mid_x = [&](float z) { return wave_a * Math::sin(z * wave_f + wave_p) + 0.4f * wave_a * Math::sin(z * wave_f * 2.7f - wave_p); };
	auto width_at = [&](float z) {
		const float t = z / (0.5f * len);
		const float taper = Math::sqrt(MAX(1.0f - t * t * t * t, 0.0f));
		return half_w * taper * (0.62f + 0.22f * Math::sin(z * 7.3f + wave_p) + 0.16f * Math::sin(z * 23.0f - 2.0f * wave_p));
	};
	// Height over the ground at (x, z): a steep landward edge (-X, the crest), a long frayed foot downhill (+X).
	auto height_at = [&](float x, float z) {
		const float w = width_at(z);
		if (w <= 1e-4f) {
			return 0.0f;
		}
		const float u = (x - mid_x(z)) / w; // -1 landward edge, +1 the foot.
		if (u < -1.0f || u > 1.0f) {
			return 0.0f;
		}
		const float rise = Math::smoothstep(-1.0f, -0.6f, u);
		const float fall = 1.0f - Math::smoothstep(-0.2f, 1.0f, u);
		const float lump = 0.75f + 0.25f * Math::sin(z * 13.0f + x * 9.0f) * Math::sin(z * 5.0f - wave_p);
		// Lying on the combed-flat sward (a few centimetres of laid blades), down to the ground at its rims.
		const float bed = 0.05f * Math::smoothstep(-1.0f, -0.75f, u) * (1.0f - Math::smoothstep(0.7f, 1.0f, u));
		return bed + thick * rise * fall * lump;
	};
	// The mat: silted, matted grass and fines under the pieces (centre colour), so no ground shows through.
	Builder &cb = builders[SURFACE_CENTRE];
	const int across = 6;
	const int along = int(Math::ceil(len / 0.08f));
	const int first = int(cb.verts.size());
	for (int j = 0; j <= along; j++) {
		const float z = (float(j) / along - 0.5f) * len;
		const float w = width_at(z);
		for (int i = 0; i <= across; i++) {
			const float u = float(i) / across * 2.0f - 1.0f;
			const float x = mid_x(z) + u * w * 0.8f;
			const float y = MAX(height_at(x, z) - 0.006f, 0.0f) + 0.002f;
			cb.verts.push_back(Vector3(x, y, z));
			cb.uvs.push_back(Vector2(float(i) / across, float(j) / along));
			cb.uv2s.push_back(Vector2(0.0f, 0.0f));
			const float n = 0.85f + 0.3f * Math::sin(x * 37.0f + z * 19.0f) * Math::sin(z * 23.0f - x * 13.0f);
			cb.colors.push_back(Color(n, n, n));
		}
	}
	for (int j = 0; j < along; j++) {
		for (int i = 0; i < across; i++) {
			const int i0 = first + j * (across + 1) + i;
			const int i1 = i0 + across + 1;
			cb.indices.append_array(PackedInt32Array({ i0, i1, i0 + 1, i0 + 1, i1, i1 + 1 }));
		}
	}
	_tag_organ(cb);
	// A spot on the line: z along it, u across (-1 landward, +1 foot), lying on the heap.
	auto spot = [&](float &r_z, float p_lift) {
		r_z = _rand(-0.5f, 0.5f) * len;
		// More on the landward half, where the crest stranded the most.
		const float u = Math::lerp(-0.95f, 1.0f, Math::pow(rng.randf(), 1.4f));
		const float x = mid_x(r_z) + u * width_at(r_z);
		return Vector3(x, height_at(x, r_z) + p_lift, r_z);
	};
	// Along the line (+Z), with the line's own wander and some scatter: the water laid them along its edge.
	auto along_dir = [&](float z, float p_scatter, float p_tilt) {
		const float slope = (mid_x(z + 0.01f) - mid_x(z - 0.01f)) / 0.02f;
		const float a = Math::PI * 0.5f - Math::atan(slope) + _rand(-p_scatter, p_scatter) + (rng.randf() < 0.5f ? Math::PI : 0.0f);
		return Vector3(Math::cos(a), _rand(-p_tilt, p_tilt), Math::sin(a)).normalized();
	};
	const float k = 1.0f - 0.6f * gone;
	// Straw and dead grass blades: the bulk of a meadow flood's wrack.
	const int straws = int(len * _rand(220.0f, 300.0f) * k);
	for (int i = 0; i < straws; i++) {
		float z;
		const Vector3 at = spot(z, 0.001f);
		Leaf l;
		l.shape = LEAF_LINEAR;
		l.length = _rand(0.05f, 0.28f);
		l.width = _rand(0.0025f, 0.007f);
		l.fold = 0.3f;
		l.bend = _rand(-0.15f, 0.15f);
		l.twist = _rand(-0.8f, 0.8f);
		l.segments = 3;
		const float s = _rand(0.8f, 1.15f);
		_add_leaf(SURFACE_LEAF, Transform3D(wrack_frame(along_dir(z, 0.5f, 0.06f), Vector3(0, 1, 0)), at), l, 0.0f, 0.75f, Color(s, s, s * _rand(0.9f, 1.0f)));
	}
	// Reed and stem pieces, hollow culms 3 to 8 mm thick, broken at the nodes.
	const int reeds = int(len * _rand(5.0f, 9.0f) * k);
	for (int i = 0; i < reeds; i++) {
		float z;
		const float r0 = _rand(0.0015f, 0.004f);
		const Vector3 at = spot(z, r0 * 0.8f);
		_add_stem(Transform3D(wrack_frame(along_dir(z, 0.3f, 0.05f), Vector3(0, 1, 0)), at), _rand(0.1f, 0.4f), r0, r0 * _rand(0.8f, 1.0f), _rand(-0.08f, 0.08f), 3, 0.0f);
	}
	// Twigs, some forked: the woody share, more of it under trees and hedges upstream.
	const int twigs = int(len * _rand(3.0f, 6.0f) * k);
	for (int i = 0; i < twigs; i++) {
		float z;
		const float r0 = _rand(0.001f, 0.0045f);
		const Vector3 at = spot(z, r0 * 0.6f);
		const Vector3 d = along_dir(z, 0.9f, 0.08f);
		const float tl = _rand(0.06f, 0.3f);
		const Transform3D e = _add_stem(Transform3D(wrack_frame(d, Vector3(0, 1, 0)), at), tl, r0, r0 * 0.5f, _rand(-0.3f, 0.3f), 3, 0.0f);
		if (tl > 0.15f && rng.randf() < 0.5f) {
			Transform3D side(wrack_frame(e.basis.get_column(2) + e.basis.get_column(0) * _rand(-0.9f, 0.9f), Vector3(0, 1, 0)), at.lerp(e.origin, 0.6f));
			_add_stem(side, tl * _rand(0.2f, 0.4f), r0 * 0.5f, r0 * 0.25f, 0.0f, 2, 0.0f);
		}
	}
	// Fallen leaves, flat and silted, the odd one curled.
	const int leaves = int(len * _rand(12.0f, 22.0f) * k);
	for (int i = 0; i < leaves; i++) {
		float z;
		const Vector3 at = spot(z, 0.002f);
		Leaf l;
		const float pick = rng.randf();
		l.shape = pick < 0.5f ? LEAF_OVATE : (pick < 0.8f ? LEAF_LANCEOLATE : LEAF_ROUND);
		l.length = _rand(0.035f, 0.08f);
		l.width = l.length * (l.shape == LEAF_LANCEOLATE ? 0.35f : 0.6f);
		l.fold = _rand(-0.2f, 0.3f);
		l.bend = _rand(-0.3f, 0.3f);
		l.segments = 4;
		const float s = _rand(0.7f, 1.2f);
		_add_leaf(SURFACE_LEAF, Transform3D(wrack_frame(wrack_dir(_rand(0.0f, Math::TAU), _rand(-0.08f, 0.08f)), Vector3(0, 1, 0)), at), l, 0.0f, 0.8f, Color(0.62f * s, 0.5f * s, 0.36f * s)); // Brown against the straw.
	}
	plant_height = MAX(plant_height, thick + 0.06f);
	// Wet and silted fresh (darker, browner), drying to a pale grey crust; long-gone wrack bleaches.
	const Color wet(0.62f, 0.6f, 0.56f);
	const Color old(1.2f, 1.18f, 1.12f);
	const Color tint = wet.lerp(Color(1, 1, 1), dry).lerp(old, gone);
	for (int s = 0; s < SURFACE_MAX; s++) {
		PackedColorArray &c = builders[s].colors;
		for (int i = from[s]; i < c.size(); i++) {
			Color v = c[i];
			v.r *= tint.r;
			v.g *= tint.g;
			v.b *= tint.b;
			c.write[i] = v;
		}
		wrack_still(builders[s].uv2s, from[s], builders[s].colors);
	}
}

// The strand line along the last flood's mark: a near-continuous line of wrack pieces with its landward edge at
// the crest (RULE_HIGH_WATER from -8 cm to the mark), and thinner lines the falling water left lower down.
// All laid along the contour (follow_flow: mesh +X down the fall line). Gone a year or so after the flood.
TypedArray<Resource> PlantGenerator::make_flood_wrack(int p_variants, bool p_textures, const Dictionary &p_season) {
	TypedArray<Resource> out;
	struct Spec {
		Vector2 band; // RULE_HIGH_WATER, m.
		float spacing;
		float density;
		float scale;
	};
	const Spec set[2] = { { Vector2(-0.08f, 0.0f), 1.0f, 0.95f, 1.0f }, { Vector2(-0.9f, -0.35f), 1.6f, 0.3f, 0.7f } };
	for (int k = 0; k < 2; k++) {
		const Spec &f = set[k];
		Ref<PlantGenerator> gen;
		gen.instantiate();
		gen->set_textures(p_textures);
		gen->set_preset(PRESET_FLOOD_WRACK);
		gen->apply_preset_colors();
		gen->set_seed(12400 + k * 37);
		gen->set_season(p_season);
		Ref<PlantSpecies> sp;
		sp.instantiate();
		TypedArray<Mesh> meshes;
		const TypedArray<ArrayMesh> v = gen->generate_variants(MAX(p_variants, 1));
		for (int i = 0; i < v.size(); i++) {
			meshes.push_back(v[i]);
		}
		sp->set_meshes(meshes);
		sp->set_spacing(f.spacing);
		sp->set_footprint(0.2f);
		sp->set_density(f.density);
		sp->set_scale_range(Vector2(0.8f, 1.15f) * f.scale);
		sp->set_align_to_ground(1.0f);
		sp->set_follow_flow(true);
		sp->set_rule_range(PlantSpecies::RULE_HIGH_WATER, f.band);
		sp->set_rule_fade(PlantSpecies::RULE_HIGH_WATER, k == 0 ? 0.02f : 0.08f);
		sp->set_rule_range(PlantSpecies::RULE_FLOOD_AGE, Vector2(0.0f, 1.0f));
		sp->set_rule_fade(PlantSpecies::RULE_FLOOD_AGE, 0.3f);
		sp->set_visibility_range(35.0f);
		tag_species(sp, PRESET_FLOOD_WRACK);
		out.push_back(sp);
	}
	return out;
}

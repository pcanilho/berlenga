/**************************************************************************/

#pragma once

// White water's structure, shared by the sea (PhysXWaterSurface3D) and rivers (PhysXRiver3D) so the two cannot drift
// apart: a sheet where fresh, draining into holes torn at three scales with strands along their borders, rafts and
// scum, bubble grain and a crisp, ragged outline (docs/ideas/water.md, crisp foam). Shader text for a shader's global
// scope; it has no uniforms (the caller passes its own detail scale and crispness).
//   float cover = wf_structure(p, amount, footprint, streak_dir, stretch, detail_scale, crisp);
// p in world metres, amount 0..1 (how much foam is left), footprint the pixel's size on the water (m; 0 under the
// path tracer: full detail), drawn out along streak_dir by 1 + stretch.
inline constexpr const char *WATER_FOAM_SHARED_GLSL = R"(
float wf_hash(vec2 p) {
	vec3 p3 = fract(vec3(p.xyx) * 0.1031);
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.x + p3.y) * p3.z);
}

float wf_noise(vec2 p) {
	vec2 i = floor(p);
	vec2 f = fract(p);
	vec2 u = f * f * (3.0 - 2.0 * f);
	return mix(mix(wf_hash(i), wf_hash(i + vec2(1.0, 0.0)), u.x),
			mix(wf_hash(i + vec2(0.0, 1.0)), wf_hash(i + vec2(1.0, 1.0)), u.x), u.y);
}

// Turned 36.87 degrees (a 3-4-5 triangle): each octave on its own lattice, so their sum has no axis to line up on.
// Value noise on one lattice per octave lined every crisp threshold of it up with the world's axes, short bars
// across and along the surf like a barcode.
vec2 wf_rot(vec2 p) {
	return vec2(0.8 * p.x - 0.6 * p.y, 0.6 * p.x + 0.8 * p.y);
}

float wf_fbm(vec2 p) {
	return 0.5 * wf_noise(p) + 0.3 * wf_noise(wf_rot(p) * 2.3 + 7.1) + 0.2 * wf_noise(wf_rot(wf_rot(p)) * 5.1 - 3.3);
}

// Holes torn in a foam sheet, one per feature of a jittered grid (Worley 1996), each its own size: the cover left
// (1 where no hole), the nearest and second-nearest feature distances (for the strands along cell borders), in
// cell units, and how far inside a hole the point lies (0 at its edge, 1 at its middle). Every feature in the 3x3 neighbourhood carves its own hole, so a hole wider than its cell runs
// on into the next one instead of being cut along the straight cell border (a nearest-cell-only radius would).
// jit is the ragged tear added to the distance. Each layer tears at its own scale: the cell space is warped by noise a
// few times finer than the cells (film rupture stretched by the surface's strain, so the walls between holes bend and
// no border stays a straight segment), and each hole's edge wanders in lobes in proportion to its size.
vec4 wf_holes(vec2 p, float open, float scale, float bias, float soft, float jit) {
	p += 0.22 * (vec2(wf_fbm(p * 2.3 + 1.3), wf_fbm(p * 2.3 - 4.1)) - 0.5);
	float lobe = wf_fbm(p * 3.7 + 7.3) - 0.5;
	float tear = wf_fbm(p * 9.1 - 2.6) - 0.5;
	vec2 i = floor(p);
	vec2 f = fract(p);
	float d1 = 8.0;
	float d2 = 8.0;
	float keep = 1.0;
	float inside = 0.0;
	for (int y = -1; y <= 1; y++) {
		for (int x = -1; x <= 1; x++) {
			vec2 g = vec2(float(x), float(y));
			vec2 o = vec2(wf_hash(i + g), wf_hash(i + g + vec2(17.3, 5.1)));
			// Each feature anywhere in its cell: kept near the middle (0.15 to 0.85) the cells stayed nearly a square
			// grid, and the strands along their borders ran as rows of short bars across and along the foam.
			vec2 r = g + o - f;
			float d = length(r);
			if (d < d1) {
				d2 = d1;
				d1 = d;
			} else if (d < d2) {
				d2 = d;
			}
			// No feature outside the 3x3 neighbourhood is nearer than one cell, and a hole reaches its radius
			// plus the tear and lobes (up to 0.16 + 0.43 of the radius) past it: at most 0.56 cells keeps every
			// hole whole (a larger one was cut along a straight line where the search ended).
			float rad = min(open * max(wf_hash(i + g + vec2(41.7, 23.9)) * scale - bias, 0.0), 0.56);
			if (rad > 0.0) {
				float de = d + jit + rad * (0.6 * lobe + 0.25 * tear);
				keep = min(keep, smoothstep(rad - soft, rad + soft, de));
				inside = max(inside, (rad - de) / rad);
			}
		}
	}
	return vec4(keep, d1, d2, inside);
}

// How much of the surface foam covers, from its amount (docs/ideas/water.md "Ocean detail"). Fresh foam is a
// solid sheet. As it drains, films rupture: holes open, each its own size (some cells hold, some open wide),
// and grow until only torn strands of the borders between them are left (Plateau borders: Weaire and Hutzler
// 1999), drawn out a little along the wind's surface drift: the marbled lace an old whitecap leaves. Holes at
// three scales (about 2, 0.7 and 0.2 m) with ragged edges; clumps of bubbles (centimetres to a decimetre) give
// the sheet its grain. Each layer fades to its mean cover where it is finer than the pixel (footprint, m).
float wf_structure(vec2 p, float amount, float footprint, vec2 p_streak, float p_stretch, float p_detail_scale, float p_crisp) {
	vec2 sd = dot(p_streak, p_streak) > 1e-6 ? normalize(p_streak) : vec2(1.0, 0.0);
	vec2 q0 = p * p_detail_scale;
	// Drawn out along the drift by 1 + p_stretch.
	vec2 q = vec2(dot(q0, sd) / (1.0 + p_stretch), dot(q0, vec2(-sd.y, sd.x)));
	vec2 warp = vec2(wf_noise(q * 0.9 + 3.7), wf_noise(q * 0.9 - 8.1)) - 0.5;
	float a = clamp(amount, 0.0, 1.0);
	float open = 1.0 - pow(a, 0.6); // how far the foam has drained
	// Older foam has been sheared and drawn out longer by the surface currents: a stronger warp.
	float age_warp = 1.0 + 1.2 * open;
	// Ragged, torn edges: the distances jittered by finer noise.
	float rag = wf_fbm(q * 9.0) - 0.5;
	float soft1 = 0.015 + footprint * p_detail_scale * 1.0;
	float soft2 = 0.015 + footprint * p_detail_scale * 2.8;
	float soft3 = 0.02 + footprint * p_detail_scale * 9.2;
	// Hole radius per cell: about a fifth of the cells hold, the rest open to sizes spread wide; each layer's holes
	// torn at its own scale (wf_holes).
	vec4 h1 = wf_holes(q * 0.5 + warp * 0.8 * age_warp, open, 1.5, 0.3, soft1, 0.22 * rag);
	// Each layer's cells turned against the last, so no two share an axis.
	vec4 h2 = wf_holes(wf_rot(q) * 1.4 + warp * 1.4 * age_warp + 11.0, open, 1.4, 0.25, soft2, 0.28 * rag);
	// The smallest holes open in clusters (a metre or two across) with stretches of unbroken sheet between: films
	// drain unevenly, and one even sprinkle of them read as a pocked stipple over all the near foam.
	float cluster3 = smoothstep(0.38, 0.68, wf_fbm(q * 0.6 + vec2(-9.3, 4.4)));
	vec4 h3 = wf_holes(wf_rot(wf_rot(q)) * 4.6 + warp * 2.0 * age_warp - 5.0, open * mix(0.25, 1.35, cluster3), 1.2, 0.15, soft3, 0.32 * rag);
	float keep1 = h1.x;
	float keep2 = h2.x;
	float keep3 = h3.x;
	float cover = keep1 * keep2 * mix(1.0, keep3, 0.75);
	// Torn strands along the larger cells' borders, breaking along their length.
	float brk = smoothstep(0.4, 0.62, wf_fbm(q * 2.7 + 1.7));
	float w = (0.04 + 0.07 * a) * brk;
	float strands = max(1.0 - smoothstep(w - soft1, w + soft1, h1.z - h1.y + 0.2 * rag), 1.0 - smoothstep(w - soft2, w + soft2, h2.z - h2.y + 0.25 * rag));
	// Only where the sheet still holds enough foam (thin foam leaves no web over open water).
	cover = max(cover, strands * smoothstep(0.15, 0.45, a));
	// Islands and bridges: rafts of film that have not ruptured yet, left in the holes (fade where under a pixel).
	// Only well inside the larger holes, so they float free of the hole's rim (no hairline gap round a raft).
	float raft = smoothstep(0.62, 0.68, wf_fbm(q * 6.3 + 3.1)) * smoothstep(0.2, 0.5, a) * smoothstep(0.35, 0.5, max(h1.w, h2.w));
	// Gone well before a raft is a pixel or two across (single-pixel specks in the open water otherwise).
	cover = max(cover, raft * (1.0 - clamp(footprint * p_detail_scale * 20.0 - 0.2, 0.0, 1.0)));
	// Thin scum of loose bubbles left in the holes.
	cover = max(cover, 0.22 * smoothstep(0.05, 0.4, a) * smoothstep(0.35, 0.75, wf_fbm(q * 5.3 - 4.1)));
	// Past where the holes are under a pixel, their mean cover: the largest holes (about a metre and a half) stay
	// a few pixels across out to a few hundred metres, so only there (fading from a quarter of a cell a pixel; it
	// began at a fortieth, which smoothed the surf's bores 30 m away into even white bands).
	float coarse = clamp(footprint * p_detail_scale * 1.2 - 0.25, 0.0, 1.0);
	cover = mix(cover, clamp(1.0 - 1.4 * open * open, 0.05, 1.0), coarse);
	// Bubble clumps (about 7 cm cells, each its own brightness) and their walls (finer), to a few cm a pixel;
	// clusters of clumps (about 25 cm) carry on further out.
	float grain = 1.0 - clamp((footprint - 0.015) / 0.04, 0.0, 1.0);
	float cluster = 1.0 - clamp((footprint - 0.04) / 0.12, 0.0, 1.0);
	// Smooth, warped noise rather than cells, so the grain has no tile edges: soft clumps, the dark gaps
	// between them, and the small bright domes of single large bubbles.
	vec2 pw = p + 0.08 * warp;
	// The grain varies over the sheet (patches of fine, even froth beside coarse, pocked foam and smooth slick
	// films, about a metre across), not one even stipple of dots over all of it: real swash foam is patchy.
	float grain_patch = wf_fbm(p * 0.9 + vec2(13.1, -7.7));
	float coarse_grain = smoothstep(0.35, 0.7, grain_patch);
	// Two fixed scales blended, not one scale that varies (its rate times the distance from the origin sheared it).
	float clumps = mix(wf_fbm(pw * 20.0 + 4.7), wf_fbm(pw * 11.0 - 6.2), coarse_grain);
	// The dark gaps between clumps only in the coarse patches: sprinkled over the whole sheet they read as an even pocked
	// stipple from a few metres.
	float gaps = smoothstep(0.3, 0.12, mix(wf_fbm(pw * 36.0 - 2.9), wf_fbm(pw * 22.0 + 8.4), coarse_grain)) * smoothstep(0.5, 0.8, grain_patch);
	float domes = smoothstep(0.8, 0.95, wf_fbm(pw * 38.0 + 9.3)) * smoothstep(0.45, 0.8, grain_patch);
	// Past a pixel each fades to its own mean (wf_fbm averages 0.5: clumps 0.62 + 0.45 * 0.5, gaps and domes rare),
	// not to 1: fading to 1 drew far foam about a fifth brighter than the same foam near, or under the path tracer
	// (no derivatives, full detail, the same mean), so distant patches read as flat white sheets.
	float bubbles = mix(0.84, (0.62 + 0.45 * clumps) * (1.0 - 0.3 * gaps) + 0.12 * domes, grain);
	bubbles *= mix(0.925, 0.7 + 0.45 * wf_fbm(pw * 3.5 + 2.3), cluster);
	// Thicker and thinner stretches of the sheet (half a metre to a couple of metres): a fresh sheet is mottled, its
	// thin places greyer, not one even white.
	bubbles *= mix(0.93, 0.78 + 0.3 * wf_fbm(pw * 1.1 - 7.7), cluster);
	// Thin foam is translucent, thick foam opaque.
	float density = smoothstep(0.02, 0.35, a);
	if (p_crisp > 0.0) {
		// Crisp: a patch's edge is a narrow step of its density against a ragged threshold (fine noise), not
		// the soft blob of the amount texture; the thin haze an old patch leaves stays, translucent, so no foam
		// is lost.
		float rough = 0.6 * wf_fbm(q * 3.1 + 5.3) + 0.4 * wf_fbm(q * 11.0 - 2.1);
		float t_edge = 0.32 + 0.5 * (rough - 0.5);
		float w_edge = clamp(0.02 + footprint * p_detail_scale * 2.0, 0.02, 0.3);
		// A thin patch's step rises only part way: a small, thin whitecap stays translucent with a soft rim, not a
		// paper-white chip (only a thick sheet reads as opaque).
		float outline = max(smoothstep(t_edge - w_edge, t_edge + w_edge, density) * mix(0.55, 1.0, smoothstep(0.35, 0.7, a)), 0.8 * density);
		density = mix(density, outline, p_crisp);
		bubbles = mix(1.0, bubbles, 1.0 - 0.55 * p_crisp);
	}
	return clamp(cover * density * bubbles, 0.0, 1.0);
}

// The bubbles' domes (the clumps' smooth height, as wf_structure's grain), their slope for the light to catch the
// foam's grain; none past a few centimetres a pixel.
vec2 wf_bubble_slope(vec2 p, float footprint) {
	float grain = 1.0 - clamp((footprint - 0.015) / 0.04, 0.0, 1.0);
	if (grain <= 0.0) {
		return vec2(0.0);
	}
	const float e = 0.01;
	float h0 = wf_fbm(p * 16.0 + 4.7);
	float hx = wf_fbm((p + vec2(e, 0.0)) * 16.0 + 4.7);
	float hz = wf_fbm((p + vec2(0.0, e)) * 16.0 + 4.7);
	return vec2(hx - h0, hz - h0) / e * 0.03 * grain;
}
)";

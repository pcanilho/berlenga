/**************************************************************************/
/*  water_current_bend.h                                                  */
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

#pragma once

#include "modules/world/world_contracts.h"
#include "core/math/math_funcs.h"
#include "core/math/vector2.h"
#include "core/math/vector2i.h"
#include "core/templates/local_vector.h"

// A river's current laying the ground cover over (docs/ideas/world-links.md, flowing water → reeds, grass): a coarse
// square of the drag the water puts on a submerged stem, from WorldWater's surface and flow, which the trample fields
// (GrassField3D's own, VegetationField3D's ContactTrample) take wherever it bends harder than the bodies do, so the
// grass and plants that read the field (grass_trample_map) lean downstream and lie flat in a fast current.
//
// How far: the Cauchy number Ca = rho Cd b U^2 l^3 / (E I), drag against stiffness over the wetted length l (the
// depth up to the stem's own height); a flexible blade reconfigures and its effective length falls as Ca^-1/3 (Luhar
// and Nepf 2011, "Flow-induced reconfiguration of buoyant and flexible aquatic vegetation"), so the share laid down
// is 1 - (1 + Ca)^-1/3. A grass blade (b 4 mm, E I 5e-5 N m2, Cd 1.2) lies half over at 5 cm/s in 30 cm of water and
// flat past 30 cm/s, as grass in a flooded meadow does. The shaders soften it for stiff stems (reeds, rushes) by their
// own stiffness. Above the surface nothing changes: the wind keeps the rest.
class WaterCurrentBend {
	static constexpr int STRIDE = 4; // texels of the trample field per sample
	static constexpr float PERIOD = 0.25f; // s between resamples (the current changes slowly)
	static constexpr float RHO = 1000.0f, CD = 1.2f, WIDTH = 0.004f, EI = 5e-5f, STEM = 0.4f;

	LocalVector<Vector2> coarse; // bend per sample, on the trample field's texels / STRIDE
	Vector2i origin; // the trample field's origin when sampled
	int res = 0; // coarse samples a side
	float texel = 0.0f;
	double wait = 0.0;
	bool any = false;

public:
	static Vector2 bend_for(float p_depth, const Vector3 &p_flow) {
		const Vector2 u(p_flow.x, p_flow.z);
		const float speed = u.length();
		if (p_depth <= 0.01f || speed < 0.01f) {
			return Vector2();
		}
		const float l = MIN(p_depth, STEM);
		const float ca = RHO * CD * WIDTH * speed * speed * l * l * l / EI;
		const float share = 1.0f - Math::pow(1.0f + ca, -1.0f / 3.0f);
		return u / speed * share;
	}

	// Resamples when due or when the trample field moved; p_ground(pos) gives the ground height.
	template <typename G>
	void update(const Vector2i &p_origin, int p_res, float p_texel, double p_delta, const G &p_ground) {
		wait -= p_delta;
		const int n = (p_res + STRIDE - 1) / STRIDE + 1;
		if (wait > 0.0 && p_origin == origin && n == res && p_texel == texel) {
			return;
		}
		wait = PERIOD;
		origin = p_origin;
		res = n;
		texel = p_texel;
		coarse.resize(n * n);
		any = false;
		WorldWater *water = WorldContracts::water();
		if (!water || !water->has_provider()) {
			for (Vector2 &c : coarse) {
				c = Vector2();
			}
			return;
		}
		for (int z = 0; z < n; z++) {
			for (int x = 0; x < n; x++) {
				Vector3 p((origin.x + x * STRIDE) * texel, 0.0f, (origin.y + z * STRIDE) * texel);
				p.y = p_ground(p);
				const float surface = water->height_at(p);
				Vector2 b;
				if (Math::is_finite(surface) && surface > p.y) {
					b = bend_for(surface - p.y, water->flow_at(p));
				}
				coarse[z * n + x] = b;
				any = any || b.length_squared() > 1e-6f;
			}
		}
	}

	// Sets each texel of p_bend (p_res a side, at the origin given to update) that the current bends harder than it
	// already is; true if any was set.
	bool apply(LocalVector<Vector2> &p_bend, int p_res) const {
		if (!any || (int)p_bend.size() != p_res * p_res) {
			return false;
		}
		bool set = false;
		for (int z = 0; z < p_res; z++) {
			const float fz = float(z) / STRIDE;
			const int z0 = MIN(int(fz), res - 2);
			const float tz = fz - z0;
			for (int x = 0; x < p_res; x++) {
				const float fx = float(x) / STRIDE;
				const int x0 = MIN(int(fx), res - 2);
				const float tx = fx - x0;
				const Vector2 a = coarse[z0 * res + x0].lerp(coarse[z0 * res + x0 + 1], tx);
				const Vector2 c = coarse[(z0 + 1) * res + x0].lerp(coarse[(z0 + 1) * res + x0 + 1], tx);
				const Vector2 t = a.lerp(c, tz);
				Vector2 &b = p_bend[z * p_res + x];
				if (t.length_squared() > b.length_squared() + 1e-6f) {
					b = t;
					set = true;
				}
			}
		}
		return set;
	}
	bool is_any() const { return any; }
};

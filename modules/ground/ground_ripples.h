/**************************************************************************/
/*  ground_ripples.h                                                      */
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

#include "core/math/math_funcs.h"
#include "core/math/vector2.h"
#include "core/math/vector2i.h"
#include "core/math/vector3.h"
#include "core/templates/local_vector.h"

// Wind ripples on sand (docs/ideas/sand.md, stage 3): a height layer of a few millimetres on a fine grid
// (1.25 cm over the middle 4 m of the ground field), grown by Nishimori and Ouchi's model of saltation and
// creep (1993, "Formation of ripple patterns and dunes by wind-blown sand", PRL 71): each step every cell
// sends q of sand a hop L = L0 + b h downwind, then creep evens it with its neighbours. Ripples emerge across
// the wind at a wavelength set by the hop. The hop's growth with height saturates (tanh, at 3 mm), which holds
// them at a ripple index of about 16 (11 cm long, 7 mm high), as measured on dry sand.
class GroundRipples {
public:
	// Saltation over flat dry sand, shared with the streamers: u* from the wind at 10 m by the log law over
	// rippled sand (z0 0.1 mm), Bagnold's threshold for fine sand (u*t 0.2 m/s, about 5.7 m/s at 10 m), and
	// Lettau and Lettau's (1978) flux Q = C (rho_air / g) u*^2 (u* - u*t), kg/m/s.
	static constexpr float SAND_Z0 = 1e-4f;
	static constexpr float SAND_USTAR_T = 0.2f;
	static float friction_speed(float p_wind10) { return 0.4f * MAX(p_wind10, 0.0f) / Math::log(10.0f / SAND_Z0); }
	static float saltation_flux(float p_ustar) { return p_ustar > SAND_USTAR_T ? 2.6f * (1.2f / 9.81f) * p_ustar * p_ustar * (p_ustar - SAND_USTAR_T) : 0.0f; }

	float cell = 0.0125f; // m
	int n = 320; // cells across
	float hop = 0.08f; // L0, m
	float hop_gain = 3.0f; // b
	float hop_saturation = 0.005f; // m
	float q_ref = 6e-4f; // m a step at which creep is d_ref (the creep share scales with q)
	float d_ref = 0.45f;
	float tan_repose = 0.65f;

	Vector2i origin; // world cell of cell 0 (its centre is at origin * cell)
	bool valid = false;
	LocalVector<float> h; // m about the mean
	LocalVector<float> mask; // 0..1: sand that can move (dry sand share)
	LocalVector<float> tmpl; // a grown periodic field (n x n) the cells coming in take
	Vector2 tmpl_wind; // the wind it grew in
	Vector2 tmpl_k; // its dominant wave vector (rad/m; 0 = none found)
	bool tmpl_ready = false;

	// The template's dominant wave vector (rad/m): the strongest periodic component with a wavelength of 3 to 40 cm.
	static Vector2 dominant_k(const LocalVector<float> &p_h, int p_n, float p_cell);
	// Where a world point reads the periodic template, and how high (no repeat may show): a three-octave world-space
	// warp (crests turn and change spacing over tens of metres, bend over a few) and edge dislocations in pairs
	// (Y-junctions: a crest forks and rejoins) with a height that wanders; x, y the offset (m), z the height factor.
	// The ripple shaders carry the same lines (material.cpp, scene_raytracing_raygen.glsl ground_ripple_vary()).
	static Vector3 vary(Vector2 p_xz, Vector2 p_k);
	float tmpl_at(int p_wx, int p_wz) const; // the template as a cell coming in at world cell (p_wx, p_wz) takes it

	// Grows a periodic field from noise: p_steps steps of q_ref in direction p_wind (unit).
	static void grow(LocalVector<float> &r_h, int p_n, const GroundRipples &p_params, Vector2 p_wind, int p_steps, uint32_t p_seed);
	// One step: every cell sends p_q * mask a hop along p_wind (unit), then creep and avalanching.
	void step(float p_q, Vector2 p_wind);
	static void step_field(LocalVector<float> &r_h, LocalVector<float> &r_tmp, const LocalVector<float> *p_mask, int p_n, const GroundRipples &p_params, float p_q, Vector2 p_wind);
	// Moves the window so cell 0 is at p_origin: what overlaps is kept, the rest comes from the template.
	void move_to(Vector2i p_origin);
	float sample(float p_x, float p_z) const; // m at a world position (0 outside)
	float mean() const;

private:
	LocalVector<float> tmp;
};

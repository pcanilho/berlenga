/**************************************************************************/
/*  ground_streamers_3d.h                                                 */
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

#include "scene/3d/node_3d.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/material.h"

class GPUParticles3D;
class GroundSurface3D;

// Blowing sand (docs/ideas/sand.md, stage 3): the saltating grains over dry sand when the wind is past
// Bagnold's threshold, as GPU particles around the camera, traced by the path tracer like any particles.
//
// The amount follows the saltation flux the ripples use (GroundRipples::saltation_flux): the sand in the air
// over a square metre is Q / u, the flux over the grains' mean speed, and each particle stands for as many
// 0.25 mm grains as that takes, drawn with their summed cross-section, so the sheet is as dense to the eye as
// the real one. Grains hop: exponential hop heights with a 4 cm mean, about the same at any wind (Namikas
// 2003; Kok et al. 2012, sec. 2.1, grain speeds set by the threshold rather than the wind), a speed of 80% of
// the threshold wind at the hop's top, so a hop runs 10 to 15 times its height. Near the threshold the sand
// moves in streamers: ribbons about 20 cm wide and a few metres long that run downwind with the air's gusts,
// merging into a continuous sheet as the wind rises (Baas and Sherman 2005; Stout and Zobeck 1997).
class GroundStreamers3D : public Node3D {
	GDCLASS(GroundStreamers3D, Node3D);

	NodePath ground_surface;
	NodePath follow_target;
	float extent = 10.0f; // m, the square around the follow point
	int max_grains = 40000;
	float strength = 1.0f;
	float grain_size = 0.25f; // mm
	float streak_time = 1.0f / 60.0f;
	Color grain_color = Color(0.78, 0.68, 0.5);

	GPUParticles3D *particles = nullptr;
	Ref<ShaderMaterial> process;
	Ref<Shader> shader;
	Ref<StandardMaterial3D> grain_material;

	static constexpr int GROUND_N = 64;
	Ref<Image> ground_image;
	Ref<ImageTexture> ground_texture;
	Vector3 ground_center;
	bool ground_valid = false;
	double ground_accum = 0.0;
	double clock = 0.0;

	float cur_wind10 = 0.0f;
	float cur_peak_wind = 0.0f; // m/s at 10 m, with the shared wind's vortices
	float cur_ustar = 0.0f;
	float cur_flux = 0.0f;
	float cur_coverage = 0.0f;
	float cur_ratio = 0.0f;
	float cur_per_particle = 0.0f;

	GroundSurface3D *_surface() const;
	Vector3 _follow_position() const;
	void _ensure_children();
	void _update_ground(GroundSurface3D *p_surface, Vector3 p_center);
	void _update(double p_delta);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	// The mean horizontal speed of saltating grains, m/s, and the grains' mass in the air over a square metre
	// for a flux Q (kg/m/s).
	static float grain_speed();
	static float airborne_mass(float p_flux) { return p_flux / grain_speed(); }
	// The share of the ground under streamers at a friction speed: from 15% at the threshold to a continuous
	// sheet at twice it.
	static float streamer_coverage(float p_ustar);

	void set_ground_surface(const NodePath &p_path) { ground_surface = p_path; }
	NodePath get_ground_surface() const { return ground_surface; }
	void set_follow_target(const NodePath &p_path) { follow_target = p_path; }
	NodePath get_follow_target() const { return follow_target; }
	void set_extent(float p_extent);
	float get_extent() const { return extent; }
	void set_max_grains(int p_count);
	int get_max_grains() const { return max_grains; }
	void set_strength(float p_strength) { strength = MAX(p_strength, 0.0f); }
	float get_strength() const { return strength; }
	void set_grain_size(float p_mm) { grain_size = CLAMP(p_mm, 0.05f, 2.0f); }
	float get_grain_size() const { return grain_size; }
	void set_streak_time(float p_seconds) { streak_time = MAX(p_seconds, 0.0f); }
	float get_streak_time() const { return streak_time; }
	void set_grain_color(const Color &p_color);
	Color get_grain_color() const { return grain_color; }

	float get_wind() const { return cur_wind10; }
	float get_flux() const { return cur_flux; }
	float get_peak_wind() const { return cur_peak_wind; }
	float get_coverage() const { return cur_coverage; }
	float get_active_ratio() const { return cur_ratio; }
	float get_grains_per_particle() const { return cur_per_particle; }

	GroundStreamers3D();
};
